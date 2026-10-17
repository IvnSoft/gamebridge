#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
A small MCS-51 (8051) disassembler, with EZ-USB FX2 register annotation.

Written because nothing suitable was installed on my rig, and I was bored.
sdcc ships no disassembler, and radare2's 8051 support would be a large install
for a 7.6 KB image. This is ~250 lines, has no dependencies, and knows the FX2
SFR and XDATA register names, which a generic tool would not.

Usage:
    dis8051.py <image.bin> range <start> <end>     disassemble an address range
    dis8051.py <image.bin> at <addr> [count]       disassemble N instructions
    dis8051.py <image.bin> xrefs <addr16>          find MOV DPTR,#addr sites
"""

import sys

# ---------------------------------------------------------------- SFR names
# On the FX2 the low ports are IOA/IOB/IOC/IOD rather than P0..P3.
SFR = {
    0x80: "IOA", 0x81: "SP", 0x82: "DPL", 0x83: "DPH", 0x84: "DPL1",
    0x85: "DPH1", 0x86: "DPS", 0x87: "PCON", 0x88: "TCON", 0x89: "TMOD",
    0x8A: "TL0", 0x8B: "TL1", 0x8C: "TH0", 0x8D: "TH1", 0x8E: "CKCON",
    0x90: "IOB", 0x91: "EXIF", 0x92: "MPAGE", 0x98: "SCON0", 0x99: "SBUF0",
    0x9A: "AUTOPTRH1", 0x9B: "AUTOPTRL1", 0x9D: "AUTOPTRH2", 0x9E: "AUTOPTRL2",
    0xA0: "IOC", 0xA1: "INT2CLR", 0xA2: "INT4CLR", 0xA8: "IE",
    0xAA: "EP2468STAT", 0xAB: "EP24FIFOFLGS", 0xAC: "EP68FIFOFLGS",
    0xAF: "AUTOPTRSETUP", 0xB0: "IOD", 0xB1: "IOE", 0xB2: "OEA", 0xB3: "OEB",
    0xB4: "OEC", 0xB5: "OED", 0xB6: "OEE", 0xB8: "IP", 0xBA: "EP01STAT",
    0xBB: "GPIFTRIG", 0xBD: "GPIFSGLDATH", 0xBE: "GPIFSGLDATLX",
    0xBF: "GPIFSGLDATLNOX", 0xC0: "SCON1", 0xC1: "SBUF1", 0xC8: "T2CON",
    0xCA: "RCAP2L", 0xCB: "RCAP2H", 0xCC: "TL2", 0xCD: "TH2", 0xD0: "PSW",
    0xD8: "EICON", 0xE0: "ACC", 0xE8: "EIE", 0xF0: "B", 0xF8: "EIP",
}


def sfr(a):
    return SFR.get(a, f"0x{a:02x}")


def bitname(b):
    """Bit-addressable space: 0x00-0x7f is RAM 0x20-0x2f, 0x80+ is SFR bits."""
    if b >= 0x80:
        base = b & 0xF8
        return f"{sfr(base)}.{b & 7}"
    return f"0x{0x20 + (b >> 3):02x}.{b & 7}"


# ------------------------------------------------------------ opcode table
# (mnemonic, operands, length). Operand codes:
#   d=direct  i=#imm8  I=#imm16  b=bit  r=rel8  a=addr11  A=addr16
R = "R"


def build_table():
    t = [None] * 256

    def put(op, m, o="", n=1):
        t[op] = (m, o, n)

    simple = {
        0x00: ("NOP", "", 1), 0x02: ("LJMP", "A", 3), 0x03: ("RR", "A", 1),
        0x04: ("INC", "A", 1), 0x05: ("INC", "d", 2), 0x12: ("LCALL", "A", 3),
        0x13: ("RRC", "A", 1), 0x14: ("DEC", "A", 1), 0x15: ("DEC", "d", 2),
        0x10: ("JBC", "br", 3), 0x20: ("JB", "br", 3), 0x30: ("JNB", "br", 3),
        0x22: ("RET", "", 1), 0x23: ("RL", "A", 1), 0x24: ("ADD", "Ai", 2),
        0x25: ("ADD", "Ad", 2), 0x32: ("RETI", "", 1), 0x33: ("RLC", "A", 1),
        0x34: ("ADDC", "Ai", 2), 0x35: ("ADDC", "Ad", 2), 0x40: ("JC", "r", 2),
        0x42: ("ORL", "dA", 2), 0x43: ("ORL", "di", 3), 0x44: ("ORL", "Ai", 2),
        0x45: ("ORL", "Ad", 2), 0x50: ("JNC", "r", 2), 0x52: ("ANL", "dA", 2),
        0x53: ("ANL", "di", 3), 0x54: ("ANL", "Ai", 2), 0x55: ("ANL", "Ad", 2),
        0x60: ("JZ", "r", 2), 0x62: ("XRL", "dA", 2), 0x63: ("XRL", "di", 3),
        0x64: ("XRL", "Ai", 2), 0x65: ("XRL", "Ad", 2), 0x70: ("JNZ", "r", 2),
        0x72: ("ORL", "Cb", 2), 0x73: ("JMP", "@A+DPTR", 1),
        0x74: ("MOV", "Ai", 2), 0x75: ("MOV", "di", 3), 0x80: ("SJMP", "r", 2),
        0x82: ("ANL", "Cb", 2), 0x83: ("MOVC", "A,@A+PC", 1),
        0x84: ("DIV", "AB", 1), 0x85: ("MOV", "dd", 3),
        0x90: ("MOV", "DPTR,I", 3), 0x92: ("MOV", "bC", 2),
        0x93: ("MOVC", "A,@A+DPTR", 1), 0x94: ("SUBB", "Ai", 2),
        0x95: ("SUBB", "Ad", 2), 0xA0: ("ORL", "C/b", 2),
        0xA2: ("MOV", "Cb", 2), 0xA3: ("INC", "DPTR", 1),
        0xA4: ("MUL", "AB", 1), 0xA5: ("?", "", 1), 0xB0: ("ANL", "C/b", 2),
        0xB2: ("CPL", "b", 2), 0xB3: ("CPL", "C", 1),
        0xB4: ("CJNE", "Air", 3), 0xB5: ("CJNE", "Adr", 3),
        0xC0: ("PUSH", "d", 2), 0xC2: ("CLR", "b", 2), 0xC3: ("CLR", "C", 1),
        0xC4: ("SWAP", "A", 1), 0xC5: ("XCH", "Ad", 2), 0xD0: ("POP", "d", 2),
        0xD2: ("SETB", "b", 2), 0xD3: ("SETB", "C", 1), 0xD4: ("DA", "A", 1),
        0xD5: ("DJNZ", "dr", 3), 0xE0: ("MOVX", "A,@DPTR", 1),
        0xE4: ("CLR", "A", 1), 0xE5: ("MOV", "Ad", 2),
        0xF0: ("MOVX", "@DPTR,A", 1), 0xF4: ("CPL", "A", 1),
        0xF5: ("MOV", "dA", 2),
    }
    for op, (m, o, n) in simple.items():
        put(op, m, o, n)

    # AJMP/ACALL occupy op = (page<<5) | 0x01 / 0x11
    for p in range(8):
        put(0x01 | (p << 5), "AJMP", "a", 2)
        put(0x11 | (p << 5), "ACALL", "a", 2)

    # Register- and indirect-form families.
    fam = [
        (0x06, "INC", "@R", 1), (0x08, "INC", "R", 1),
        (0x16, "DEC", "@R", 1), (0x18, "DEC", "R", 1),
        (0x26, "ADD", "A,@R", 1), (0x28, "ADD", "A,R", 1),
        (0x36, "ADDC", "A,@R", 1), (0x38, "ADDC", "A,R", 1),
        (0x46, "ORL", "A,@R", 1), (0x48, "ORL", "A,R", 1),
        (0x56, "ANL", "A,@R", 1), (0x58, "ANL", "A,R", 1),
        (0x66, "XRL", "A,@R", 1), (0x68, "XRL", "A,R", 1),
        (0x76, "MOV", "@R,i", 2), (0x78, "MOV", "R,i", 2),
        (0x86, "MOV", "d,@R", 2), (0x88, "MOV", "d,R", 2),
        (0x96, "SUBB", "A,@R", 1), (0x98, "SUBB", "A,R", 1),
        (0xA6, "MOV", "@R,d", 2), (0xA8, "MOV", "R,d", 2),
        (0xB6, "CJNE", "@R,i,r", 3), (0xB8, "CJNE", "R,i,r", 3),
        (0xC6, "XCH", "A,@R", 1), (0xC8, "XCH", "A,R", 1),
        (0xD6, "XCHD", "A,@R", 1), (0xD8, "DJNZ", "R,r", 2),
        (0xE2, "MOVX", "A,@R", 1), (0xE6, "MOV", "A,@R", 1),
        (0xE8, "MOV", "A,R", 1), (0xF2, "MOVX", "@R,A", 1),
        (0xF6, "MOV", "@R,A", 1), (0xF8, "MOV", "R,A", 1),
    ]
    for base, m, o, n in fam:
        count = 2 if "@R" in o or base in (0xE2, 0xF2) else 8
        if base in (0x06, 0x16, 0x26, 0x36, 0x46, 0x56, 0x66, 0x76, 0x86,
                    0x96, 0xA6, 0xB6, 0xC6, 0xD6, 0xE2, 0xE6, 0xF2, 0xF6):
            count = 2
        for k in range(count):
            put(base + k, m, o.replace(R, f"R{k}"), n)

    for i, e in enumerate(t):
        if e is None:
            t[i] = (".db", f"0x{i:02x}", 1)
    return t


TABLE = build_table()

# FX2 XDATA register names, for annotating MOV DPTR,#imm16.
XREG = {
    0xE600: "CPUCS", 0xE601: "IFCONFIG", 0xE604: "FIFORESET",
    0xE609: "FIFOPINPOLAR", 0xE60A: "REVID", 0xE60B: "REVCTL",
    0xE610: "EP1OUTCFG", 0xE611: "EP1INCFG", 0xE612: "EP2CFG",
    0xE613: "EP4CFG", 0xE614: "EP6CFG", 0xE615: "EP8CFG",
    0xE618: "EP2FIFOCFG", 0xE61A: "EP6FIFOCFG",
    0xE640: "EP2ISOINPKTS", 0xE642: "EP6ISOINPKTS",
    0xE648: "INPKTEND", 0xE65C: "GPIFIE", 0xE65D: "GPIFIRQ",
    0xE670: "PORTACFG", 0xE678: "I2CS", 0xE679: "I2DAT", 0xE67A: "I2CTL",
    0xE680: "USBCS", 0xE681: "SUSPEND", 0xE682: "WAKEUPCS", 0xE683: "TOGCTL",
    0xE68A: "EP0BCH", 0xE68B: "EP0BCL", 0xE68D: "EP1OUTBC", 0xE68F: "EP1INBC",
    0xE6A0: "EP24FIFOFLGS", 0xE6B8: "SUDPTRH", 0xE6B9: "SUDPTRL",
    0xE6BA: "SUDPTRCTL", 0xE740: "EP0BUF", 0xE780: "EP1OUTBUF",
    0xE7C0: "EP1INBUF",
}


def decode(fw, pc):
    op = fw[pc]
    m, o, n = TABLE[op]
    b = fw[pc:pc + n]
    if len(b) < n:
        return pc + 1, f".db 0x{op:02x}", n

    def d(k):   return sfr(b[k])
    def i(k):   return f"#0x{b[k]:02x}"
    def rel(k): return f"0x{(pc + n + ((b[k] ^ 0x80) - 0x80)) & 0xFFFF:04x}"

    txt = None
    if o == "A" and m in ("LJMP", "LCALL"):
        txt = f"0x{(b[1] << 8) | b[2]:04x}"
    elif o == "a":
        tgt = ((pc + 2) & 0xF800) | ((op >> 5) << 8) | b[1]
        txt = f"0x{tgt:04x}"
    elif o == "DPTR,I":
        a16 = (b[1] << 8) | b[2]
        name = XREG.get(a16)
        txt = f"DPTR,#0x{a16:04x}" + (f"   ; {name}" if name else "")
    elif o == "":       txt = ""
    elif o == "A":      txt = "A"
    elif o == "C":      txt = "C"
    elif o == "AB":     txt = "AB"
    elif o == "DPTR":   txt = "DPTR"
    elif o == "d":      txt = d(1)
    elif o == "r":      txt = rel(1)
    elif o == "b":      txt = bitname(b[1])
    elif o == "br":     txt = f"{bitname(b[1])},{rel(2)}"
    elif o == "Ai":     txt = f"A,{i(1)}"
    elif o == "Ad":     txt = f"A,{d(1)}"
    elif o == "dA":     txt = f"{d(1)},A"
    elif o == "di":     txt = f"{d(1)},{i(2)}"
    elif o == "dd":     txt = f"{d(1)},{d(2)}"
    elif o == "dr":     txt = f"{d(1)},{rel(2)}"
    elif o == "Cb":     txt = f"C,{bitname(b[1])}"
    elif o == "C/b":    txt = f"C,/{bitname(b[1])}"
    elif o == "bC":     txt = f"{bitname(b[1])},C"
    elif o == "Air":    txt = f"A,{i(1)},{rel(2)}"
    elif o == "Adr":    txt = f"A,{d(1)},{rel(2)}"
    elif o.startswith("@A"):  txt = o
    elif o == "A,@A+PC":      txt = o
    else:
        t = o
        if ",i,r" in t:                       # CJNE Rn,#imm,rel
            t = t.replace(",i,r", f",{i(1)},{rel(2)}")
        elif t.endswith(",r"):                # DJNZ Rn,rel
            t = t[:-2] + f",{rel(1)}"
        elif t.endswith(",i"):
            t = t[:-2] + f",{i(1)}"
        elif t.endswith(",d"):
            t = t[:-2] + f",{d(1)}"
        elif t.startswith("d,"):
            t = d(1) + t[1:]
        txt = t
    return pc + n, f"{m} {txt}".strip(), n


def dump(fw, start, end):
    pc = start
    while pc < min(end, len(fw)):
        nxt, txt, n = decode(fw, pc)
        raw = " ".join(f"{x:02x}" for x in fw[pc:pc + n])
        print(f"  {pc:04x}:  {raw:<9}  {txt}")
        pc = nxt


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    fw = open(sys.argv[1], "rb").read()
    cmd = sys.argv[2]

    if cmd == "range":
        dump(fw, int(sys.argv[3], 0), int(sys.argv[4], 0))
    elif cmd == "at":
        start = int(sys.argv[3], 0)
        count = int(sys.argv[4], 0) if len(sys.argv) > 4 else 20
        pc = start
        for _ in range(count):
            if pc >= len(fw):
                break
            nxt, txt, n = decode(fw, pc)
            raw = " ".join(f"{x:02x}" for x in fw[pc:pc + n])
            print(f"  {pc:04x}:  {raw:<9}  {txt}")
            pc = nxt
    elif cmd == "switch":
        # SDCC inline switch table, as used by the EP1 OUT command dispatcher.
        # The helper at 0x0ef0 pops the return address to find this table, then
        # walks 3-byte entries [addr_hi, addr_lo, key], comparing key against
        # the switch value. A zero address terminates, and the two bytes after
        # the terminator are the default handler.
        p = int(sys.argv[3], 0)
        print(f"switch table at 0x{p:04x}:")
        n = 0
        while p + 2 < len(fw):
            addr = (fw[p] << 8) | fw[p + 1]
            if addr == 0:
                dflt = (fw[p + 2] << 8) | fw[p + 3]
                print(f"  default        -> 0x{dflt:04x}")
                break
            print(f"  case 0x{fw[p + 2]:02x}      -> 0x{addr:04x}")
            p += 3
            n += 1
        print(f"  ({n} cases)")
    elif cmd == "xrefs":
        want = int(sys.argv[3], 0)
        hi, lo = (want >> 8) & 0xFF, want & 0xFF
        print(f"MOV DPTR,#0x{want:04x} sites:")
        for k in range(len(fw) - 2):
            if fw[k] == 0x90 and fw[k + 1] == hi and fw[k + 2] == lo:
                print(f"  0x{k:04x}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
