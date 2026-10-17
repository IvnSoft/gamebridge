#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Decode the GameBridge's isochronous stream into UYVY frames.

The CX25840 emits ITU-R BT.656 and the FX2 forwards it verbatim -- there is no
vendor framing layer at all, which is why no per-packet header was ever found.
Each video line is bracketed by 4-byte timing reference codes:

    FF 00 00 XY

where XY is 1 F V H PPPP:

    F = field   (0 = field 1, 1 = field 2)
    V = 1 during vertical blanking
    H = 0 for SAV (start of active video), 1 for EAV (end)

This device zeroes the four protection bits, so the codes seen are 0x80 (SAV
field 1), 0x90 (EAV field 1), 0xc0 (SAV field 2), 0xd0 (EAV field 2) and the
0xa0/0xb0/0xe0/0xf0 variants during blanking. Between a SAV and the next EAV sit
1440 bytes: 720 pixels of 4:2:2, two bytes each.

Fields are interlaced, so field 1 supplies the even output lines and field 2 the
odd ones. A field ends when the F bit changes: the V bit sits only on blanking
lines, which are exactly the ones this device tends to drop.

Most EAVs are followed by an ancillary-data packet filling the horizontal
blanking interval (a line may carry none, so check the header):

    00 FF FF  DID SDID DC  <4 * (DC & 0x0f) bytes>  <checksum>

and that payload is 16-bit big-endian PCM audio, 48 kHz stereo. Silence sits at
0x0000/0xFFFF, so the payload routinely contains FF FF 00 00 -- which spells a
timing code. Scanning ancillary data instead of skipping it invents lines with
whatever F and V bits the audio happens to carry, and is what made both bits
look unreliable. Skip the block by its declared length.

Usage:
    bt656-decode.py <raw.bin> [--frames N] [--ppm out%d.ppm] [--uyvy out.uyvy]
"""

import sys

WIDTH = 720
LINE_BYTES = WIDTH * 2
FIELD_LINES = 240
HEIGHT = FIELD_LINES * 2


def find_codes(buf):
    """Yield (offset_of_payload, field, vblank, is_eav) for each timing code."""
    i, n = 0, len(buf)
    while True:
        i = buf.find(b"\xff\x00\x00", i)
        if i < 0 or i + 3 >= n:
            return
        xy = buf[i + 3]
        # Protection bits are zeroed by this device, so a real code has a zero
        # low nibble. 0xFF passes a bare bit-7 test but is nearly always the
        # first byte of the next preamble; consuming it loses sync.
        if (xy & 0x80) and (xy & 0x0f) == 0:
            yield (i + 4, (xy >> 6) & 1, (xy >> 5) & 1, (xy >> 4) & 1)
            i += 4
            if (xy >> 4) & 1:
                # An EAV is followed by an ancillary packet carrying audio.
                # Step over it by its declared length rather than searching
                # through it -- see the module docstring.
                if i + 6 <= n and buf[i] == 0x00 and buf[i+1] == 0xff \
                   and buf[i+2] == 0xff:
                    i += 6 + 4 * (buf[i+5] & 0x0f) + 1
        else:
            i += 1


def find_audio(buf):
    """Yield the ancillary payload of each line, in stream order."""
    i, n = 0, len(buf)
    while True:
        i = buf.find(b"\xff\x00\x00", i)
        if i < 0 or i + 3 >= n:
            return
        xy = buf[i + 3]
        if not ((xy & 0x80) and (xy & 0x0f) == 0):
            i += 1
            continue
        i += 4
        if not ((xy >> 4) & 1):
            continue
        if i + 6 > n or buf[i] != 0x00 or buf[i+1] != 0xff or buf[i+2] != 0xff \
           or buf[i+3] != 0xbf:          # DID is 0xbf on every real block
            continue
        ln = 4 * (buf[i+5] & 0x0f)
        yield buf[i+6:i+6+ln]
        i += 6 + ln + 1


def decode(buf, max_frames):
    """Assemble frames. Mirrors bt656_feed() in tools/gbcap.c deliberately.

    One anchor per field, and it is the F transition: this device omits about
    two thirds of its vertical-blanking intervals, so anchoring on the V edge
    where it exists and on F only as a fallback gives fields two origins two
    lines apart. Blanking lines consume no row index, which is what makes the
    two cases agree.

    row = 2 * line - F, because vactive is 487 -- an odd number -- so field 2's
    line k is the same picture row as field 1's line k+1.
    """
    frames, cur = [], None
    line_of = 0
    cur_field = 0
    started = False
    in_vblank = False
    run, prev_run = 0, 0          # lengths of the two fields building this frame

    for off, field, vblank, is_eav in find_codes(buf):
        if is_eav:
            continue                      # payload follows SAV, not EAV
        if vblank:
            in_vblank = True
            continue

        boundary = False
        if not started:
            boundary = True
        elif field != cur_field:
            boundary = True
        elif in_vblank and line_of >= FIELD_LINES // 2:
            boundary, field = True, 1 - cur_field    # F transition was lost
        elif line_of >= FIELD_LINES + 8:
            boundary, field = True, 1 - cur_field    # both lost; split anyway
        in_vblank = False

        if boundary and started and line_of < FIELD_LINES // 2:
            boundary = False              # implausibly short field

        if boundary:
            started = True
            cur_field = field
            if cur_field == 0:
                # A frame is only as good as the two fields that built it, and
                # a field that is not field-length is not one. Too short
                # rejects the start of a capture, which begins wherever the
                # device happens to be in the raster; too long means the split
                # rule fired, so a field boundary was missed and the frame is a
                # guess.
                if (cur is not None
                        and FIELD_LINES - 8 <= run < FIELD_LINES + 8
                        and FIELD_LINES - 8 <= prev_run < FIELD_LINES + 8):
                    frames.append(cur)
                    if len(frames) >= max_frames:
                        return frames
                cur = [None] * HEIGHT
            prev_run, run = run, 0
            line_of = 0

        if cur is None:
            cur = [None] * HEIGHT
        if off + LINE_BYTES > len(buf):
            break
        row = 2 * line_of - cur_field
        line_of += 1
        run += 1
        if 0 <= row < HEIGHT:
            cur[row] = buf[off:off + LINE_BYTES]

    return frames


def uyvy_to_ppm(frame, path):
    """Write a frame as binary PPM. Manual colour conversion, no dependencies."""
    out = bytearray()
    for row in frame:
        if row is None:
            out += b"\x00" * (WIDTH * 3)
            continue
        for x in range(0, len(row) - 3, 4):
            u, y0, v, y1 = row[x], row[x + 1], row[x + 2], row[x + 3]
            for y in (y0, y1):
                c = y - 16
                d = u - 128
                e = v - 128
                r = (298 * c + 409 * e + 128) >> 8
                g = (298 * c - 100 * d - 208 * e + 128) >> 8
                b = (298 * c + 516 * d + 128) >> 8
                out += bytes((max(0, min(255, r)),
                              max(0, min(255, g)),
                              max(0, min(255, b))))
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (WIDTH, HEIGHT))
        f.write(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    nmax = 4
    ppm = uyvy = wav = None
    for i, a in enumerate(sys.argv):
        if a == "--frames" and i + 1 < len(sys.argv):
            nmax = int(sys.argv[i + 1])
        if a == "--ppm" and i + 1 < len(sys.argv):
            ppm = sys.argv[i + 1]
        if a == "--uyvy" and i + 1 < len(sys.argv):
            uyvy = sys.argv[i + 1]
        if a == "--wav" and i + 1 < len(sys.argv):
            wav = sys.argv[i + 1]

    buf = open(src, "rb").read()
    print(f"{src}: {len(buf)} bytes")

    if wav:
        import struct, wave
        # An ancillary block is NOT stored interleaved: a 16-byte payload is
        # four consecutive samples of one channel then four of the other --
        # AAAA BBBB, not ABABABAB. Concatenating the payloads verbatim hands a
        # player the channels shuffled, and makes genuinely different channels
        # measure as identical. De-interleave each block on its own.
        vals = []
        for pay in find_audio(buf):
            k = len(pay) // 4                 # samples per channel in this block
            if k == 0 or len(pay) % 4:
                continue
            w = struct.unpack(f">{2*k}h", pay)
            for i in range(k):
                vals.append(w[i])             # LEFT
                vals.append(w[k + i])         # RIGHT, same instant
        with wave.open(wav, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(struct.pack(f"<{len(vals)}h", *vals))
        peak = max((abs(v) for v in vals), default=0)
        print(f"wrote {wav}: {len(vals)//2} stereo frames "
              f"({len(vals)/2/48000:.3f} s), peak {peak}")

    frames = decode(buf, nmax)
    print(f"decoded {len(frames)} frame(s) of {WIDTH}x{HEIGHT}")

    for n, fr in enumerate(frames):
        got = sum(1 for r in fr if r)
        ys = [r[1] for r in fr if r]
        print(f"  frame {n}: {got}/{HEIGHT} lines, "
              f"luma min {min(ys) if ys else 0} max {max(ys) if ys else 0}")
        if ppm:
            p = ppm % n if "%" in ppm else f"{ppm}.{n}"
            uyvy_to_ppm(fr, p)
            print(f"    wrote {p}")
        if uyvy:
            u = uyvy % n if "%" in uyvy else f"{uyvy}.{n}"
            with open(u, "wb") as f:
                for r in fr:
                    f.write(r if r else b"\x00" * LINE_BYTES)
            print(f"    wrote {u}")


if __name__ == "__main__":
    main()
