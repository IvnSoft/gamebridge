#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Extract the embedded FX2 firmware from Adaptec's avcgbfl.sys (GameBridge loader).

The loader driver carries the 8051 image inside its PE .data section as a table of
Cypress INTEL_HEX_RECORD structs -- the layout used by Cypress's own EZ-USB driver
samples and produced by their hex2c utility:

    typedef struct {
        WORD Length;        /* bytes in Data, 0..16                */
        WORD Address;       /* target address in FX2 memory        */
        BYTE Type;          /* 0 = data, 1 = end of file           */
        BYTE Data[16];
    } INTEL_HEX_RECORD;     /* sizeof == 22: 21 bytes + 1 pad      */

Note the struct is 21 bytes of content padded to 22 by WORD alignment. Getting that
stride wrong makes the table look like garbage after the first record, which is the
trap this parser exists to document.

Usage: extract-fw.py <avcgbfl.sys> <out.bin>
"""

import hashlib
import os
import struct
import sys

# SHA-256 of the 7624-byte image extracted from the avcgbfl.sys that
# fetch-firmware.sh pins.
IMAGE_SHA256 = "39845e82eb7691dddcea1e0ec55c53881dba200437fb0e369e53439d5260b22f"

RECORD_SIZE = 22
TYPE_DATA = 0
TYPE_EOF = 1


def pe_section(blob, want):
    """Return (file_offset, size) of a named PE section."""
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsections = struct.unpack_from("<H", blob, pe + 6)[0]
    opt_size = struct.unpack_from("<H", blob, pe + 20)[0]
    table = pe + 24 + opt_size
    for i in range(nsections):
        ent = table + i * 40
        name = blob[ent:ent + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, _vaddr, rawsize, rawptr = struct.unpack_from("<IIII", blob, ent + 8)
        if name == want:
            return rawptr, min(vsize or rawsize, rawsize)
    raise KeyError(want)


def parse_records(data):
    """Walk the hex-record table, returning {address: byte} and stopping at EOF."""
    mem, off, count = {}, 0, 0
    while off + RECORD_SIZE <= len(data):
        length, addr = struct.unpack_from("<HH", data, off)
        rtype = data[off + 4]
        if rtype == TYPE_EOF:
            count += 1
            break
        if rtype != TYPE_DATA or length > 16:
            # Table has ended and we have run into unrelated .data content.
            break
        for i in range(length):
            mem[addr + i] = data[off + 5 + i]
        off += RECORD_SIZE
        count += 1
    return mem, count


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    blob = open(sys.argv[1], "rb").read()
    off, size = pe_section(blob, ".data")
    print(f".data at file offset 0x{off:x}, {size} bytes")

    mem, count = parse_records(blob[off:off + size])
    if not mem:
        sys.exit("no hex records found -- wrong file or wrong record layout")

    lo, hi = min(mem), max(mem)
    print(f"{count} records -> {len(mem)} bytes spanning 0x{lo:04x}..0x{hi:04x}")

    # Materialise a flat image from 0. Gaps are left as 0x00; the FX2 does not care
    # because the 8051 only executes what the records actually covered.
    image = bytearray(hi + 1)
    for a, b in mem.items():
        image[a] = b

    gaps = (hi + 1) - len(mem)
    print(f"image {len(image)} bytes ({gaps} bytes not covered by any record)")
    print(f"reset vector: {image[0]:02x} {image[1]:02x} {image[2]:02x}"
          f"{'  (LJMP - looks like valid 8051)' if image[0] == 0x02 else '  (NOT an LJMP!)'}")

    # Pinned. This image runs on the 8051 and decides every descriptor and byte
    # the driver then trusts, and the kernel loader checks only its size and
    # first byte. fetch-firmware.sh pins the .sys it comes from; this pins the
    # result, so a tampered or mis-extracted image is refused here rather than
    # installed (second whole-repo review, 2026-10-16).
    got = hashlib.sha256(image).hexdigest()
    if got != IMAGE_SHA256:
        sys.exit(f"extracted image sha256 {got}\n"
                 f"            expected {IMAGE_SHA256}\n"
                 "refusing to write it; is the .sys the one fetch-firmware.sh pins?")
    # Written whole or not at all: a temporary file beside the target, synced,
    # then renamed over it. A bare open().write() let a full disk or a quota
    # truncate the file while this still printed "verified" -- the hash was of
    # the image in memory, not of what reached the disk (fifth review,
    # 2026-10-17; reproduced there with ulimit -f).
    out = sys.argv[2]
    tmp = f"{out}.tmp{os.getpid()}"
    try:
        with open(tmp, "wb") as f:
            f.write(image)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, out)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    print(f"wrote {out} (sha256 verified)")


if __name__ == "__main__":
    main()
