#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
r"""
Turn a usbmon capture of the Windows driver into a decoder bring-up sequence.

While Windows XP drives the GameBridge (passed through to a VM), every command
it sends appears on the host's usbmon interface. Command 0x09 is the firmware's
I2C passthrough, so each such packet is literally a register write to one of the
three chips on the board -- in order, for this exact hardware, with no inference.

usbmon text format (Documentation/usb/usbmon.rst):

    <tag> <timestamp> <S|C> <type>:<bus>:<dev>:<ep> <status> <len> = <data words>

where type is Ci/Co control, Bi/Bo bulk, Ii/Io interrupt, Zi/Zo isochronous, and
the data words are 8 hex characters (4 bytes) each.

NOTE the status field is not always a bare integer: interrupt transfers render it
as "-115:1" and isochronous as "0:1:7368:0". Matching it as \d+ silently drops
every interrupt packet -- which on this device means every command sent after
SET_INTERFACE selects a non-zero alt, since EP 0x01 is bulk at alt 0 and
interrupt at alts 1..5. That mistake hid 97% of the driver's traffic.

Capture with:
    sudo modprobe usbmon
    sudo cat /sys/kernel/debug/usb/usbmon/1u > capture.txt

Then:
    parse-usbmon.py capture.txt
    parse-usbmon.py capture.txt --c-array      emit a C register table
"""

import re
import sys

# I2C 7-bit addresses, confirmed against the parts read off the board.
CHIPS = {
    0x44: ("CX25840", "reg16"),   # Conexant video decoder, 16-bit big-endian regs
    0x50: ("24C02N", "reg8"),     # Atmel EEPROM, 8-bit sub-address
    0x61: ("XC2028", "raw"),      # Xceive silicon tuner
}

CMD_NAMES = {
    0x08: "I2C-WRITE",
    0x09: "I2C-XFER",
    0x36: "START-STREAM",
    0x37: "STOP-STREAM",
    0x46: "GPIO/STATUS-DUMP",
    0x83: "DECODER-RESET",
    0x84: "DECODER-RESET",
}

LINE = re.compile(
    r"^(\S+)\s+(\d+)\s+([SCE])\s+([A-Za-z]{2}):(\d+):(\d+):(\d+)\s+(\S+)\s+(\d+)(?:\s+=\s+(.*))?$"
)


def data_bytes(words):
    if not words:
        return b""
    out = bytearray()
    for w in words.split():
        if not re.fullmatch(r"[0-9a-fA-F]+", w):
            break
        for i in range(0, len(w) - 1, 2):
            out.append(int(w[i:i + 2], 16))
    return bytes(out)


def decode_i2c_write(p):
    """Command 0x08: 08 <addr7> <len> <reg_hi> <reg_lo> <data...>

    len counts the payload after itself, so len 3 is a 16-bit register address
    plus one data byte (cx25840_write) and len 6 is the address plus four data
    bytes little-endian (cx25840_write4)."""
    if len(p) < 5:
        return None
    addr, ln = p[1], p[2]
    payload = p[3:3 + ln]
    if len(payload) < 3:
        return None
    name, style = CHIPS.get(addr, (f"unknown@0x{addr:02x}", "raw"))
    if style == "reg16":
        reg = (payload[0] << 8) | payload[1]
        return (name, "write", reg, bytes(payload[2:]), 0)
    return (name, "raw", None, bytes(payload), 0)


def decode_i2c(p):
    """p is the command payload: 09 wcount rcount addr7 data..."""
    if len(p) < 4:
        return None
    wcount, rcount, addr = p[1], p[2], p[3]
    data = p[4:4 + wcount]
    name, style = CHIPS.get(addr, (f"unknown@0x{addr:02x}", "raw"))

    if style == "reg16" and wcount >= 2:
        reg = (data[0] << 8) | data[1]
        if wcount >= 3:
            return (name, "write", reg, data[2:], rcount)
        return (name, "setptr", reg, b"", rcount)
    if style == "reg8" and wcount >= 1:
        reg = data[0]
        if wcount >= 2:
            return (name, "write", reg, data[1:], rcount)
        return (name, "setptr", reg, b"", rcount)
    return (name, "raw", None, data, rcount)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    as_c = "--c-array" in sys.argv

    cmds, others, seen_dev = [], [], set()

    for line in open(path, errors="replace"):
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        _tag, ts, ev, typ, bus, dev, ep, _st, _ln, words = m.groups()
        if ev != "S":                      # submissions carry the outgoing data
            continue
        if typ not in ("Bo", "Io"):        # command endpoint is bulk or interrupt OUT
            continue
        if int(ep) != 1:
            continue
        p = data_bytes(words)
        if not p:
            continue
        seen_dev.add((int(bus), int(dev)))
        if p[0] == 0x08:
            d = decode_i2c_write(p)
            if d:
                cmds.append((int(ts), d, p))
        elif p[0] == 0x09:
            d = decode_i2c(p)
            if d:
                cmds.append((int(ts), d, p))
        else:
            others.append((int(ts), p))

    if not cmds and not others:
        print("no command traffic found on EP 0x01 OUT.")
        print("check that the device was passed through and the bus is right.")
        return

    print(f"devices seen: {sorted(seen_dev)}")
    print(f"{len(cmds)} I2C command(s), {len(others)} other command(s)\n")

    if others:
        print("=== non-I2C commands, in order ===")
        for ts, p in others:
            nm = CMD_NAMES.get(p[0], "?")
            hexs = " ".join(f"{b:02x}" for b in p)
            print(f"  {ts:>12}  {nm:<18} [{hexs}]")
        print()

    print("=== I2C traffic, in order ===")
    per_chip = {}
    for ts, (name, kind, reg, val, rcount), _raw in cmds:
        per_chip.setdefault(name, []).append((kind, reg, val))
        if kind == "write":
            v = " ".join(f"{b:02x}" for b in val)
            print(f"  {ts:>12}  {name:<9} reg 0x{reg:04x} = {v}")
        elif kind == "setptr":
            print(f"  {ts:>12}  {name:<9} set pointer 0x{reg:04x}"
                  f"{f', read {rcount}' if rcount else ''}")
        else:
            v = " ".join(f"{b:02x}" for b in val)
            print(f"  {ts:>12}  {name:<9} raw [{v}]"
                  f"{f', read {rcount}' if rcount else ''}")

    print("\n=== summary ===")
    for name, items in per_chip.items():
        w = sum(1 for k, _, _ in items if k == "write")
        print(f"  {name}: {len(items)} transaction(s), {w} register write(s)")

    if "--replay" in sys.argv:
        # Machine-readable replay file: one register write per line, as
        # "<addr7> <reg16> <byte...>", suitable for feeding straight back to the
        # device through command 0x08.
        for ts, (name, kind, reg, val, _rc), _raw in cmds:
            if kind != "write":
                continue
            a7 = next((k for k, (n, _s) in CHIPS.items() if n == name), None)
            if a7 is None:
                continue
            v = " ".join(f"{b:02x}" for b in val)
            print(f"{a7:02x} {reg:04x} {v}")
        return

    if as_c:
        for name, items in per_chip.items():
            writes = [(r, v) for k, r, v in items if k == "write"]
            if not writes:
                continue
            slug = name.lower().replace("-", "_")
            print(f"\n/* {name} bring-up, captured from the Windows driver */")
            print(f"static const struct {{ u16 reg; u8 val; }} {slug}_init[] = {{")
            for reg, val in writes:
                if len(val) == 1:
                    print(f"\t{{ 0x{reg:04x}, 0x{val[0]:02x} }},")
                else:
                    v = ", ".join(f"0x{b:02x}" for b in val)
                    print(f"\t/* {len(val)}-byte */ {{ 0x{reg:04x}, {v} }},")
            print("};")


if __name__ == "__main__":
    main()
