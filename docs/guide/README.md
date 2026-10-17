# Guide

For someone who wants to run the tools, capture from the device, or work on the
protocol. Assumes you are comfortable with USB terminology.

**To simply use the device, read [`driver.md`](driver.md)** - the kernel driver
makes it an ordinary camera, microphone and remote. This page documents the
userland tools that came before it: they need no module, and they remain the
way to diagnose the device and to work on the protocol.

## What state the project is in

Video **and audio** work end to end from Linux: firmware upload → replay the
captured Windows bring-up session → `SET_INTERFACE` alt 1 → decode raw BT.656
off the isochronous endpoint → UYVY frames into a player, with 48 kHz stereo
lifted out of the same stream's horizontal blanking.

| | |
|---|---|
| Geometry | 720×480 NTSC, 720×576 PAL |
| Rate | 21.4 MB/s, ~30 / ~25 fps |
| Frame completeness | every row of every frame |
| Vertical stability | 0.041 rows NTSC, 0.086 PAL (blocked-sensor measurement) |
| Colour | correct, no register changes needed |
| Audio | 48 kHz 16-bit stereo, channel order verified |

The same works through the **kernel driver** (`driver.md`), which shares the
frame assembler with `gbcap` - `driver/gb-bt656.c` is compiled into both.

## Prerequisites

Fedora 42+ (others also work); all present by default except where noted.

```sh
gcc, make, python3          # build and extraction
libusb-1.0 + headers        # pkg-config --modversion libusb-1.0  => 1.0.29
curl                        # firmware fetch
ffmpeg or mpv or vlc        # preview
sdcc                        # only for rebuilding 8051 analysis, optional
```

`fxload` is packaged in Fedora and does the same job as `tools/gbfw load`, but
`gbfw` is used here because it also dumps descriptors, needs no Intel-HEX
conversion of the extracted binary, and is what the driver's own loader
(`driver/gb-fw.c`) was ported from.

## Waking the device

```sh
./tools/fetch-firmware.sh
make -C tools
python3 tools/extract-fw.py firmware/avcgbfl.sys firmware/gb-loader-fw.bin
sudo ./tools/gbfw load firmware/gb-loader-fw.bin
```

Expected:

```
firmware firmware/gb-loader-fw.bin: 7624 bytes, reset vector 02 18 4d
halting 8051 (CPUCS 0xe600 <- 0x01)
uploading 7624 bytes to internal RAM........ done
releasing 8051 (CPUCS 0xe600 <- 0x00)
```

Then `lsusb` shows `03f3:009c` where `03f3:009b` used to be. If it still says
`009b`, the firmware did not start.

`sudo ./tools/gbfw info` prints the full endpoint map of whichever state the
device is in.

**This does not survive a replug.** The firmware goes to volatile internal RAM,
never to the EEPROM. Re-run the load each time the device is
reconnected - or load the kernel driver, which does the upload itself on every
plug-in.

## The hardware you are talking to

Read off the board:

| Part | Role | I2C (7-bit) |
|---|---|---|
| Cypress CY7C68013A (EZ-USB FX2LP) | USB bridge, 8051 + PHY | - |
| Conexant CX25840 | analogue video decoder | `0x44` |
| Xceive XC2028 | silicon TV tuner | `0x61` |
| Atmel 24C02N | FX2 boot EEPROM (C0) | `0x50` |
| AKM AK5357 | stereo audio ADC | none (pin-strapped) |

The FX2 is a bridge only. It runs in slave-FIFO mode - no GPIF waveforms - so
the CX25840 clocks pixels into the FX2's FIFO directly and the 8051 never
touches the data path. Everything interesting is CX25840 register state, reached
over I2C, which the 8051 performs on the host's behalf.

## The USB interface

Interface 0 is vendor-specific (class 255/1/1), three endpoints, six alternate
settings:

| Endpoint | Direction | Purpose | Transfer type |
|---|---|---|---|
| `0x01` | OUT | commands | **bulk at alt 0, interrupt at alts 1–5** |
| `0x81` | IN | command replies, probably audio | same split |
| `0x82` | IN | **video** | bulk at alt 0, isochronous at 1–5 |

That bulk/interrupt split on the command endpoint is not cosmetic - it is what
broke the first capture parse, and any host code must follow the currently
selected alt when sending commands.

| Alt | EP 0x82 | Per microframe | Throughput |
|---|---|---|---|
| 0 | bulk 512 | - | idle / control |
| 1 | isoc 1010 ×3 | 3030 B | **24.24 MB/s** |
| 2 | isoc 726 ×3 | 2178 B | 17.42 MB/s |
| 3 | isoc 686 ×3 | 2058 B | 16.46 MB/s |
| 4 | isoc 970 ×1 | 970 B | 7.76 MB/s |
| 5 | isoc 684 ×1 | 684 B | 5.47 MB/s |

Use alt 1. 720×480 UYVY at 30 fps needs 20.74 MB/s, so it is the only setting
with headroom for full-rate NTSC; the rest are the conventional fallback ladder.

Before streaming is armed, the ×3 alts return `LIBUSB_TRANSFER_ERROR` on every
packet. This is expected and is not a bandwidth problem: `EP2ISOINPKTS` defaults
to ×1 and the firmware raises it inside its `SET_INTERFACE` handler.

Return to alt 0 when idle - a high-bandwidth isochronous alt reserves bus
bandwidth from every other device on the controller.

## The command protocol

Commands go to EP `0x01` OUT, replies come back on EP `0x81` IN. The firmware
dispatches 19 opcodes; these are the ones in use:

```
08 <addr7> <len> <reg_hi> <reg_lo> <data...>   I2C write  (len counts from reg_hi)
09 <wcount> <rcount> <addr7> <data...>         I2C read / combined transfer
36 / 37                                        start / stop streaming
83 / 84                                        decoder hardware reset pulse (GPIO)
85                                             status poll
46                                             GPIO state dump (OED/IOD)
```

`0x08` is the fast write path and is what the Windows driver actually uses:
`len 3` is a 16-bit register address plus one byte, `len 6` is address plus four
bytes - exactly `cx25840_write()` and `cx25840_write4()`.

Command `0x09` replies with `0x08` for ACK and `0x07` for NACK in byte 0, which
is what makes `gbcap i2cscan` possible.

**The alt switch does the arming, and `0x36` follows it.**
The driver issues `SET_INTERFACE` alt 1 and *then* sends `36 03 00`; it stops
with `37` and re-arms with another `SETIF 1` + `36 03 00`. The firmware's
`EP2CFG`/`EP2ISOINPKTS` writes live in the `SET_INTERFACE` handler, so the ×3
geometry is armed there rather than by the command.

### Which I2C reads work

Reads are reliable in one transaction shape and wedge the 8051 in the others:

| `wcount` | `rcount` | result |
|---|---|---|
| 0 | 0 | fine, and resyncs a wedged read state machine |
| 1 | 1 | wedges, on `0x44` and `0x50` alike |
| **2** | **1** | **100 of 100 correct, no resync needed** |
| 2 | 2 or 4 | wedges |

A wedge is a firmware bug: the routine that starts a read refuses and says so
in the carry flag, and its caller never tests the carry, then busy-waits with no
timeout - every later transfer times out until a zero-count transaction resyncs
it. `wcount=2, rcount=1` is exactly what the kernel's `cx25840_read()` issues,
so the decoder works; a multi-byte read is done as single-byte reads at
incrementing register addresses. Writes (command `0x08`) are a separate firmware
path and are unaffected.

If reads start timing out, suspect the wedge, not the hardware - `gbcap
i2cscan` afterwards still returns the identical 3-ACK/109-NACK result.

## Getting a picture

The working path is a faithful replay of the captured Windows session:

```sh
sudo tools/gbcap session captures/full-session-replay.txt 10 --frames ./out.uyvy
```

or straight into a player:

```sh
tools/gbplay.sh 30 ffplay          # or: tools/gbplay.sh 30 mpv
```

`gbplay.sh` wraps the same call and supplies the geometry the player cannot
infer from raw UYVY. `GB_SESSION` overrides the script; `GB_INTERLACED=1`
switches from the 240p default to 480-line interlaced assembly.

`session` flags:

| Flag | Effect |
|---|---|
| `--frames <path>` | write assembled UYVY frames; `-` means stdout |
| `-o <path>` | dump the raw isochronous stream instead |
| `--pal` | 288-line fields / 576-line frames (geometry only) |
| `--progressive`, `--240p` | treat each field as a frame |
| `GB_DEBUG=1` | per-boundary diagnostics and `0x09` read replies |

The replay script format is three verbs, one per line:

```
CMD <hex bytes>    send on EP 0x01 OUT, drain the reply on EP 0x81 IN
SETIF <n>          SET_INTERFACE interface 0, alternate setting n
CLRHALT <ep>       CLEAR_FEATURE(ENDPOINT_HALT)
```

Order matters more than it looks. The Windows driver switches to alt 1 part-way
through and performs the **majority** of its 488 register writes afterwards, so
the script cannot be reordered into "configure, then stream".

## The wire format

There is no vendor framing. The CX25840 emits ITU-R BT.656 and the FX2 forwards
it verbatim - which is why there was no per-packet header. Lines are
bracketed by `FF 00 00 XY` timing reference codes, `XY = 1 F V H PPPP`:

| XY | meaning |
|---|---|
| `0x80` / `0x90` | SAV / EAV, field 1, active |
| `0xc0` / `0xd0` | SAV / EAV, field 2, active |
| `0xa0` `0xb0` `0xe0` `0xf0` | vertical blanking |

The device zeroes the four protection bits, so a valid code has a zero low
nibble - test for that, or `0xFF` bytes will be mistaken for codes and sync is
lost. Between a SAV and the next EAV sit 1440 bytes: 720 pixels of 4:2:2.

### Ancillary data

**Most EAVs are followed by an ancillary-data packet** filling the horizontal
blanking:

```
00 FF FF  DID SDID DC  <4 * (DC & 0x0f) bytes>  <1 checksum byte>
```

`DID` is always `0xbf`. Where a block is present it lands exactly on the next
SAV, so `7 + 4 * (DC & 0x0f)` is exact - but a line may carry **no** block
(24% of them in one capture), so test the `00 FF FF` header rather than assuming
it.

**Skip that block by its length.** Its payload is audio.

### Five rules, implemented in `gbcap.c`

- **Skip ancillary data**, as above. Everything else depends on it.
- **One field anchor, and it is the F transition.** The F bit is on every line;
  a blanking interval is under 600 bytes and fits inside a single lost packet.
  Using V where it exists and F as a fallback gives fields two origins two lines
  apart.
- **`row = 2*line − F` on 525/60, `+ F` on 625/50.** The sign follows the parity
  of `vactive`: 487 is odd so the fields do not pair evenly, 576 is even so they
  do. Getting it wrong doubles every horizontal edge.
- **Keep the data from partially-completed isochronous packets.** A short
  high-bandwidth microframe returns `-EXDEV`, which libusb reports as an error;
  the bytes that arrived are good. Discarding them loses whole blanking
  intervals and with them the field anchor.
- **rescan for the preamble mid-line.** `FF 00 00` inside what you think is a
line means sync was lost - resync there rather than trusting the byte count.

### About the "7% error rate"

It is mostly idle microframes and is not a problem. The device is allocated
24.24 MB/s and produces 21.36, so roughly one microframe in eight has nothing to
carry. Of 2358 errored packets in a five-second capture, **2122 had
`actual_length` 0**. The other 236 carried a full 1010-byte transaction and
must be kept, not discarded. Queue depth changes nothing: 16×32, 32×32, 32×64
and 8×64 all give the same rate.

`gbcap --pktlog` records every packet's offset, status and length.

## Standards and geometry

- Standard lives in `0x0400` bits 3:0 - 0 NTSC-M, 1 NTSC-J, 2 NTSC 4.43,
  4 PAL-BDGHI, 5 PAL-M, 6 PAL-N, 8 SECAM. The captured session leaves it at
  `0x0004e021`, i.e. NTSC-M.
- Changing it properly takes four writes, not one: `cx25840-core.c`'s
  `set_v4lstd()` forces NTSC-M first, clears LCOMB in `0x047b`, selects the
  format, then sets the `pal_m` bit in `0x0403`. The Windows session never
  writes `0x0403` at all. `captures/session-palm.txt` is a worked example.
- Geometry must follow the standard: NTSC 240 lines/field, PAL 288. `--pal`
  switches the assembler; it does **not** write the standard register.
- Active-window timing (`0x0470`–`0x047c`) still holds the captured NTSC values
  in the replay, which leaves some noise outside the picture area on PAL. The
  kernel driver does not have this problem: the in-kernel `cx25840` driver
  computes the window from the standard.

**Ask the chip before guessing.** `0x040d` bits 3:0 report the *detected*
standard.

240p sources - Atari 8-bits and most home computers of the era - never
interlace: every field has the same timing, so the second half of an interlaced
frame never arrives. They need `--progressive`, or every other row stays black
and no frame ever completes. The kernel driver has no such mode
yet; it is on the roadmap.

## Tools

| Tool | Use |
|---|---|
| `tools/gbfw load\|info` | upload firmware, dump descriptors |
| `tools/extract-fw.py` | pull the 8051 image out of `avcgbfl.sys` |
| `tools/gbcap session` | replay a script and capture/decode video |
| `tools/gbcap probe\|dump\|bulk` | endpoint behaviour without any init |
| `tools/gbcap i2cscan\|i2c\|cxreg\|regdump\|cxid` | bus and register diagnosis |
| `tools/gbcap cmd <bytes…>` | send one raw command, show the reply |
| `tools/gbplay.sh [secs] [ffplay\|mpv]` | live preview, picture **and sound** (prefer ffplay) |
| `tools/gb-up.sh [secs]` | the device as a system camera and microphone via v4l2loopback, without the driver |
| `tools/gbtest stream\|cycle\|sstd\|read` | exercise the kernel driver's node with a scriptable verdict; built for a debug-kernel test rig |
| `tools/gbcxreg /dev/i2c-N REG…` | read CX25840 registers through the bound driver's I2C adapter |
| `tools/gbstop /dev/bus/usb/BBB/DDD` | send one stop command to an idle, unbound device; used to find the lost-stop bug |
| `tools/gbcap decode <raw>` | replay a saved capture through the live assembler, **no hardware** |
| `tools/parse-usbmon.py <log> [--c-array\|--replay]` | decode a usbmon capture |
| `tools/bt656-decode.py <raw> [--frames N] [--ppm f%d.ppm] [--uyvy out]` | offline decode |
| `tools/dis8051.py`, `analyse-fw.py`, `find-regtables.py` | firmware analysis |

### Working without the device

`gbcap decode` runs a saved capture through **exactly** the live frame
assembler, so every change can be measured before anyone is asked to plug
anything in:

```sh
tools/gbcap decode captures/samples/bt656-osd-slice.raw --frames /tmp/f.uyvy --framelog /tmp/f.log
```

`captures/samples/` holds two: `bt656-slice.raw` is a black frame (useful only
for structure) and `bt656-osd-slice.raw` has real picture content. Decoding
the same capture before and after a change means nothing but the code differs.

`bt656-decode.py` is a second, independent implementation in Python.
It mirrors `bt656_feed()` and currently produces **byte-identical** frames on the sample;
if the two ever diverge, one of them is wrong. It also extracts audio:

```sh
tools/bt656-decode.py captures/samples/bt656-osd-slice.raw --frames 3 --ppm /tmp/f%d.ppm --wav /tmp/a.wav
```

### Environment variables

| | |
|---|---|
| `GB_PAL=1` | 625/50 instead of 525/60 |
| `GB_PROGRESSIVE=1` | one field per frame: half the latency, half the lines |
| `GB_NOAUDIO=1` | picture only |
| `GB_AUDIO_DEV=` | ALSA device for sound (default `plug:default`) |
| `GB_SESSION=` | a different bring-up script |
| `GB_XFERS=`, `GB_PKTS=` | URB depth (sweeping these changes nothing measurable) |

## Capturing the Windows driver 

If you are curious this is the KVM setup i used to get the details from 
the windows driver :
- A Windows XP in a VM with the device passed through, `usbmon` recording on the
  host. Only EP `0x01` OUT matters and it is bulk/interrupt, so it passes
  through cleanly.
- The shipped INFs install nothing: every filename in their CopyFiles sections
  is commented out, because `InstallGB.exe` was meant to copy the files. Windows
  reports Code 28, which alerts a missing driver.
- I will update this if anything comes up when working the tuner part.
