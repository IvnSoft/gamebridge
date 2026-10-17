#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Map which FX2 subsystems the GameBridge firmware actually drives.

FX2/FX2LP control registers are memory-mapped in XDATA at 0xE600-0xE7FF and are
reached with MOV DPTR,#imm16 -- opcode 0x90, high byte, low byte. So every
register the firmware touches appears literally in the image as `90 E6 xx` or
`90 E7 xx`. Scanning for that needs no disassembler and answers the questions
that matter first:

  * does it use the hardware I2C engine (I2CS 0xE678 / I2DAT 0xE679)?
    -> yes means there is an external chip on the I2C bus, and the byte it
       writes first is that chip's slave address
  * does it use GPIF (0xE6C0+) or slave FIFO?
    -> tells us how pixels get from the decoder into the endpoint FIFO
  * which endpoints does it configure, and as what?

Usage: analyse-fw.py <firmware.bin>
"""

import collections
import sys

# FX2LP register map, TRM chapter 15. Only the ones worth naming.
REGS = {
    0xE600: "CPUCS", 0xE601: "IFCONFIG", 0xE602: "PINFLAGSAB",
    0xE603: "PINFLAGSCD", 0xE604: "FIFORESET", 0xE605: "BREAKPT",
    0xE608: "UART230", 0xE609: "FIFOPINPOLAR", 0xE60A: "REVID",
    0xE60B: "REVCTL",
    0xE610: "EP1OUTCFG", 0xE611: "EP1INCFG", 0xE612: "EP2CFG",
    0xE613: "EP4CFG", 0xE614: "EP6CFG", 0xE615: "EP8CFG",
    0xE618: "EP2FIFOCFG", 0xE619: "EP4FIFOCFG", 0xE61A: "EP6FIFOCFG",
    0xE61B: "EP8FIFOCFG",
    0xE620: "EP2AUTOINLENH", 0xE621: "EP2AUTOINLENL",
    0xE624: "EP6AUTOINLENH", 0xE625: "EP6AUTOINLENL",
    0xE630: "EP2FIFOPFH", 0xE631: "EP2FIFOPFL",
    0xE634: "EP6FIFOPFH", 0xE635: "EP6FIFOPFL",
    0xE640: "EP2ISOINPKTS", 0xE641: "EP4ISOINPKTS",
    0xE642: "EP6ISOINPKTS", 0xE643: "EP8ISOINPKTS",
    0xE648: "INPKTEND/OUTPKTEND",
    0xE650: "EP2FIFOIE", 0xE651: "EP2FIFOIRQ",
    0xE654: "IBNIE", 0xE655: "IBNIRQ", 0xE656: "NAKIE", 0xE657: "NAKIRQ",
    0xE658: "USBIE", 0xE659: "USBIRQ", 0xE65A: "EPIE", 0xE65B: "EPIRQ",
    0xE65C: "GPIFIE", 0xE65D: "GPIFIRQ", 0xE65E: "USBERRIE",
    0xE65F: "USBERRIRQ",
    0xE670: "PORTACFG", 0xE671: "PORTCCFG", 0xE672: "PORTECFG",
    0xE678: "I2CS", 0xE679: "I2DAT", 0xE67A: "I2CTL",
    0xE67B: "XAUTODAT1", 0xE67C: "XAUTODAT2",
    0xE680: "USBCS", 0xE681: "SUSPEND", 0xE682: "WAKEUPCS",
    0xE683: "TOGCTL", 0xE684: "USBFRAMEH", 0xE685: "USBFRAMEL",
    0xE686: "MICROFRAME", 0xE687: "FNADDR",
    0xE68A: "EP0BCH", 0xE68B: "EP0BCL", 0xE68D: "EP1OUTBC", 0xE68F: "EP1INBC",
    0xE690: "EP2BCH", 0xE691: "EP2BCL", 0xE694: "EP6BCH", 0xE695: "EP6BCL",
    0xE6A0: "EP24FIFOFLGS", 0xE6A1: "EP68FIFOFLGS",
    0xE6B8: "SUDPTRH", 0xE6B9: "SUDPTRL", 0xE6BA: "SUDPTRCTL",
    0xE6C0: "GPIFWFSELECT", 0xE6C1: "GPIFIDLECS", 0xE6C2: "GPIFIDLECTL",
    0xE6C3: "GPIFCTLCFG", 0xE6C4: "GPIFADRH", 0xE6C5: "GPIFADRL",
    0xE6CE: "GPIFTCB3", 0xE6CF: "GPIFTCB2", 0xE6D0: "GPIFTCB1",
    0xE6D1: "GPIFTCB0", 0xE6D2: "EP2GPIFFLGSEL", 0xE6D3: "EP2GPIFPFSTOP",
    0xE6D4: "EP2GPIFTRIG", 0xE6DA: "EP6GPIFFLGSEL", 0xE6DB: "EP6GPIFPFSTOP",
    0xE6DC: "EP6GPIFTRIG", 0xE6E0: "XGPIFSGLDATH", 0xE6E1: "XGPIFSGLDATLX",
    0xE6E3: "GPIFREADYCFG", 0xE6E4: "GPIFREADYSTAT", 0xE6E5: "GPIFABORT",
    0xE6F0: "UDMACRCH", 0xE6F1: "UDMACRCL",
    0xE740: "EP0BUF", 0xE780: "EP1OUTBUF", 0xE7C0: "EP1INBUF",
}

SUBSYSTEMS = [
    (0xE610, 0xE61F, "endpoint configuration"),
    (0xE620, 0xE64F, "endpoint FIFO / autolen / isoc packets"),
    (0xE650, 0xE65F, "interrupts"),
    (0xE670, 0xE677, "port configuration"),
    (0xE678, 0xE67A, "I2C ENGINE"),
    (0xE680, 0xE68F, "USB core / setup"),
    (0xE6C0, 0xE6EF, "GPIF"),
    (0xE740, 0xE7FF, "endpoint 0/1 buffers"),
]

# Known 8-bit I2C slave addresses for analogue video parts of this era.
I2C_SLAVES = {
    0x4A: "SAA7113/7114/7115 video decoder (write)",
    0x4B: "SAA7113/7114/7115 video decoder (read)",
    0x40: "SAA7111 video decoder (write)",
    0x42: "SAA7111A / SAA7110 (write)",
    0xB8: "TVP5150 video decoder (write)",
    0xB9: "TVP5150 video decoder (read)",
    0x88: "CX25840/25841 video decoder (write)",
    0x89: "CX25840/25841 video decoder (read)",
    0x8A: "TW9910 video decoder (write)",
    0xA0: "24Cxx EEPROM (write) -- FX2 boot EEPROM",
    0xA1: "24Cxx EEPROM (read)",
    0xC0: "TV tuner PLL (write)",
    0xC2: "TV tuner PLL (write, alt address)",
    0xC4: "TV tuner PLL (write, alt address)",
    0xC6: "TV tuner PLL (write, alt address)",
}


def subsystem(addr):
    for lo, hi, name in SUBSYSTEMS:
        if lo <= addr <= hi:
            return name
    return "other"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    fw = open(sys.argv[1], "rb").read()
    print(f"{sys.argv[1]}: {len(fw)} bytes\n")

    # Every MOV DPTR,#imm16 in the image.
    hits = collections.defaultdict(list)
    for i in range(len(fw) - 2):
        if fw[i] == 0x90:
            addr = (fw[i + 1] << 8) | fw[i + 2]
            if 0xE600 <= addr <= 0xE7FF:
                hits[addr].append(i)

    if not hits:
        print("no FX2 register access found -- is this really FX2 firmware?")
        return

    print("=== FX2 registers touched ===")
    by_sub = collections.defaultdict(list)
    for addr in sorted(hits):
        by_sub[subsystem(addr)].append(addr)

    for _, _, name in SUBSYSTEMS + [(0, 0, "other")]:
        if name not in by_sub:
            continue
        print(f"\n  [{name}]")
        for addr in by_sub[name]:
            label = REGS.get(addr, "")
            sites = hits[addr]
            where = ", ".join(f"0x{s:04x}" for s in sites[:6])
            if len(sites) > 6:
                where += f", +{len(sites) - 6} more"
            print(f"    0x{addr:04x} {label:<20} x{len(sites):<3} at {where}")

    # The I2C engine is the interesting one: find what gets written to I2DAT.
    print("\n=== I2C analysis ===")
    if 0xE679 not in hits:
        print("  I2DAT (0xE679) is never written -- no hardware-I2C traffic.")
        print("  The decoder may be driven by bit-banged GPIO instead, or the")
        print("  firmware may configure nothing until told to.")
    else:
        print(f"  I2DAT written from {len(hits[0xE679])} site(s)")

    # Slave addresses appear as MOV A,#imm (0x74 imm) shortly before an I2DAT
    # write. Scan a window backwards from each I2DAT reference.
    print("\n=== candidate I2C slave addresses (MOV A,#imm near I2DAT) ===")
    found = collections.Counter()
    for site in hits.get(0xE679, []):
        lo = max(0, site - 24)
        for j in range(lo, min(site + 24, len(fw) - 1)):
            if fw[j] == 0x74:
                found[fw[j + 1]] += 1
    if not found:
        print("  none seen in +/-24 bytes of an I2DAT access")
    for val, n in found.most_common(20):
        note = I2C_SLAVES.get(val, "")
        flag = "  <== " + note if note else ""
        print(f"    0x{val:02x}  x{n}{flag}")

    # Any known slave address anywhere in the image, as a wider net.
    print("\n=== known slave-address bytes present anywhere in image ===")
    for val, note in sorted(I2C_SLAVES.items()):
        n = fw.count(bytes([val]))
        if n:
            print(f"    0x{val:02x}  x{n:<4} {note}")


if __name__ == "__main__":
    main()
