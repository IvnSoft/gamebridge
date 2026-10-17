#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Recover decoder register writes from the Windows capture driver's x86 code.

avcgbdr.sys has no static register table (see tools/find-regtables.py), and it is
a C++ driver whose I2C helpers are reached through vtables, so the call graph
does not lead anywhere statically. What it must still do is build each small I2C
payload on the stack with immediate byte stores:

    mov BYTE PTR [ebp-0x20],0x88     <- or whatever the operands are
    mov BYTE PTR [ebp-0x1f],0x01
    mov BYTE PTR [ebp-0x1e],0x40

So scan an objdump listing for runs of byte stores to *consecutive* stack slots
with immediate values, and reconstruct the buffer each run assembles. For a
16-bit-addressed decoder the interesting runs are length 3 (reg_hi, reg_lo, val).

Usage:
    objdump -d -M intel avcgbdr.sys > drv.asm
    find-i2c-writes.py drv.asm [minrun]
"""

import re
import sys
from collections import Counter

# e.g.  1d00e:  c6 45 fd 00   mov    BYTE PTR [ebp-0x3],0x0
STORE = re.compile(
    r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*mov\s+BYTE PTR \[(e[bs]p)([+-])0x([0-9a-f]+)\],0x([0-9a-f]+)\s*$"
)


def parse(path):
    out = []
    for line in open(path, errors="replace"):
        m = STORE.match(line.rstrip("\n"))
        if not m:
            out.append(None)          # a gap breaks a run
            continue
        addr, base, sign, off, val = m.groups()
        slot = int(off, 16) * (-1 if sign == "-" else 1)
        out.append((int(addr, 16), base, slot, int(val, 16)))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    minrun = int(sys.argv[2]) if len(sys.argv) > 2 else 3

    items = parse(sys.argv[1])
    runs, cur = [], []

    def flush():
        if len(cur) >= minrun:
            runs.append(list(cur))
        cur.clear()

    for it in items:
        if it is None:
            flush()
            continue
        if cur and it[1] == cur[-1][1] and it[2] == cur[-1][2] + 1:
            cur.append(it)
        else:
            flush()
            cur.append(it)
    flush()

    if not runs:
        print("no consecutive immediate byte-store runs found")
        return

    print(f"{len(runs)} run(s) of >= {minrun} consecutive immediate byte stores\n")

    # A CX2584x write is reg_hi, reg_lo, value with reg < 0x1000, so the first
    # byte of a 3-run should be small. Report every run, but flag those.
    lens = Counter(len(r) for r in runs)
    print("run lengths:", dict(sorted(lens.items())), "\n")

    for r in runs:
        addr = r[0][0]
        data = [x[3] for x in r]
        hexs = " ".join(f"{b:02x}" for b in data)
        note = ""
        if len(data) == 3 and data[0] <= 0x0F:
            note = f"   <== reg 0x{(data[0] << 8) | data[1]:04x} = 0x{data[2]:02x}"
        print(f"  0x{addr:05x}  [{hexs}]{note}")


if __name__ == "__main__":
    main()
