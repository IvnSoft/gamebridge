#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Hunt for decoder register-initialisation tables inside a Windows driver binary.

The GameBridge's Windows capture driver configures the video decoder through the
firmware's command 0x09 I2C passthrough, so the register sequences for this exact
board must be compiled into avcgbdr.sys. They are almost certainly a static array
of {u16 register, u8 value} entries -- which, padded for alignment, is a 4-byte
stride with a zero pad byte, or a packed 3-byte stride.

Rather than guess at offsets, score every window in the PE's data sections by how
well it matches that shape: a plausible register address range and a long run of
consistent entries.

Usage: find-regtables.py <driver.sys> [maxreg]
"""

import struct
import sys


def pe_sections(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    n = struct.unpack_from("<H", blob, pe + 6)[0]
    opt = struct.unpack_from("<H", blob, pe + 20)[0]
    tab = pe + 24 + opt
    out = []
    for i in range(n):
        e = tab + i * 40
        name = blob[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", blob, e + 8)
        out.append((name, rawptr, min(vsize or rawsize, rawsize)))
    return out


def runs(data, base, stride, maxreg, minlen=8):
    """Find maximal runs of entries that look like {u16 reg; u8 val; [pad]}."""
    found, i = [], 0
    while i + stride <= len(data):
        j, regs = i, []
        while j + stride <= len(data):
            reg = data[j] | (data[j + 1] << 8)
            pad_ok = (stride == 3) or (data[j + 3] == 0)
            if reg == 0 or reg > maxreg or not pad_ok:
                break
            regs.append((reg, data[j + 2]))
            j += stride
        if len(regs) >= minlen:
            # Reject runs whose addresses never move -- that is padding, not a table.
            # Also reject constant values: a u32 array whose high half is zero
            # matches the {u16 reg; u8 val; pad} shape perfectly but always has a
            # zero "value" byte, and that is the dominant false positive here.
            addrs_vary = len({r for r, _ in regs}) >= max(4, len(regs) // 3)
            vals_vary = len({v for _, v in regs}) >= 4
            if addrs_vary and vals_vary:
                found.append((base + i, regs))
            i = j
        else:
            i += 1
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    blob = open(sys.argv[1], "rb").read()
    maxreg = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x0FFF

    print(f"{sys.argv[1]}: {len(blob)} bytes, register ceiling 0x{maxreg:04x}\n")

    total = 0
    for name, off, size in pe_sections(blob):
        if name not in (".data", ".rdata"):
            continue
        data = blob[off:off + size]
        for stride in (4, 3):
            for at, regs in runs(data, off, stride, maxreg):
                total += 1
                lo = min(r for r, _ in regs)
                hi = max(r for r, _ in regs)
                print(f"[{name}] file 0x{at:05x}  stride {stride}  "
                      f"{len(regs)} entries  regs 0x{lo:03x}..0x{hi:03x}")
                for reg, val in regs[:14]:
                    print(f"      reg 0x{reg:04x} = 0x{val:02x}")
                if len(regs) > 14:
                    print(f"      ... {len(regs) - 14} more")
                print()

    if not total:
        print("no candidate tables found at this register ceiling")


if __name__ == "__main__":
    main()
