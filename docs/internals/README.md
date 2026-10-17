# Internals

How the device is built, how its firmware works, how the current code drives it,
and where the remaining unknowns are.

## Hardware architecture

Every part below was confirmed off the board with the device disassembled.

```
   composite / S-Video ──┐
   XC2028 silicon tuner ─┤  (I2C 0x61, needs an xc3028-*.fw blob at runtime)
                         v
              ┌──────────────────────┐
              │ Conexant CX25840     │   I2C 0x44, device ID 0x8404
              │ analogue decoder     │   drivers/media/i2c/cx25840/
              └──────────┬───────────┘
                         │ ITU-R BT.656, 8-bit parallel, 27 MHz
                         v
              ┌──────────────────────┐
              │ Cypress CY7C68013A   │   FX2LP: 8051 + USB 2.0 PHY,
              │ slave FIFO (no GPIF) │   16 KB internal RAM, no flash
              └──────────┬───────────┘
                         │  EP 0x82 IN, isochronous, 3030 B/microframe
                         v
                       USB 2.0 host

   Atmel 24C02N  (I2C 0x50)  - C0 boot EEPROM: magic, VID, PID, DID only
   AKM AK5357               - stereo audio ADC, pin-strapped, no I2C
```

Every measured I2C address matches its part's datasheet default, and the AK5357
having no I2C interface explains why the bus scan found three devices and not
four. FX2LP date code `0543` puts manufacture at week 43 of 2005 for my device.

The FX2 carries no video logic. `tools/analyse-fw.py` finds no GPIF waveform
setup anywhere in the firmware - only `IFCONFIG` - so the decoder drives the
FIFO pins directly in slave-FIFO mode and the 8051 never sees a pixel. The
8051's entire job is configuration: bring the decoder up over I2C, service the
command endpoint, arm the endpoint geometry on `SET_INTERFACE`.

The consequence for a driver is the good kind: nearly all device state is
CX25840 register state, and Linux has driven the CX25840 as a `v4l2_subdev`
since 2005, with `pvrusb2` and `cx231xx` as worked examples of a USB bridge
composing with it. The bridge glue is the only genuinely new work.

## Boot and firmware upload

The FX2LP boots by reading the first byte of its I2C EEPROM:

| First byte | Meaning |
|---|---|
| no/invalid EEPROM | enumerate as `04b4:8613`, 8051 in reset |
| `0xC0` | EEPROM supplies VID/PID/DID only; 8051 still in reset |
| `0xC2` | EEPROM holds the whole firmware; 8051 runs it immediately |

The GameBridge is **C0** - the EEPROM was dumped byte by byte over
the device's own I2C path:

```
C0 F3 03 9B 00 00 01 08
 |   \___/  \___/
 |    VID    PID  (little-endian: 03F3, 009B)
 C0 magic
```

Upload sequence, all via `bmRequestType 0x40`, `bRequest 0xA0`, `wValue = target
address` (EZ-USB TRM ch. 3):

1. `CPUCS (0xE600) <- 0x01` - assert `8051RES`, holding the core in reset
2. write the image to `0x0000…` in chunks (1024 bytes here; anything ≤ 4096 works)
3. `CPUCS (0xE600) <- 0x00` - release the core

The firmware sets `RENUM` and re-enumerates as `03f3:009c` with real strings,
real descriptors and a 500 mA request.

Because this is internal RAM it is lost on any power cycle. Making it permanent
would mean trying to write a `0xC2` image to the EEPROM - a one-way operation on
an irreplaceable device, and it probably would not work anyway.

## Firmware container format

`avcgbfl.sys` is a 19712-byte WDM driver whose section table gives it away:

| Section | Size | |
|---|---|---|
| `.text` | 0x0b8a (2954) | far too small to need 13.6 KB of data |
| `.rdata` | 0x00ad | |
| `.data` | **0x3520 (13600)** | the firmware |
| `INIT` | 0x01ac | |

`.data` is an array of Cypress `INTEL_HEX_RECORD`:

```c
typedef struct {
    WORD Length;      /* 0..16                */
    WORD Address;     /* FX2 target address   */
    BYTE Type;        /* 0 = data, 1 = EOF    */
    BYTE Data[16];
} INTEL_HEX_RECORD;   /* sizeof == 22 (21 + 1 pad byte, WORD alignment) */
```

558 records decode to 7624 bytes covering `0x0000`–`0x1dc7` with no gaps, reset
vector `02 18 4d`. Records are *not* address-ordered - the first two target
`0x0036` and `0x1a85` - so `tools/extract-fw.py` scatters into a sparse map and
materialises a flat image afterwards rather than assuming sequential layout. It
walks the PE section table rather than hard-coding offsets, so it should also
handle the AVC-1400 driver.

## Inside the loader firmware

`tools/analyse-fw.py` maps FX2 register usage without a disassembler: control
registers live in XDATA at `0xE6xx`/`0xE7xx` and are reached with
`MOV DPTR,#imm16` = `90 E6 xx`, so every register the firmware touches appears
literally in the image.

| Subsystem | Verdict |
|---|---|
| I2C engine (`I2CS 0xE678`, `I2DAT 0xE679`, `I2CTL 0xE67A`) | in use, 23/8/1 sites |
| EP1 buffers (`0xE780` OUT, `0xE7C0` IN) | heavily used - the command channel |
| `EP2CFG 0xE612`, `EP2ISOINPKTS 0xE640` | 8 and 12 sites - video endpoint geometry |
| GPIF (`0xE6C0`–`0xE6EF`) | **not used** - slave FIFO, as above |

### Command dispatch

At `0x0056` the firmware reads `EP1OUTBUF`, saves byte 0 as the command code,
and at `0x0074` calls the dispatcher at `0x0ef0`. That is SDCC's inline-switch
helper: it pops its own return address to find a table compiled immediately
after the call site, then walks 3-byte entries `[addr_hi, addr_lo, key]`. A zero
address terminates the table and the two bytes after it are the default handler.
`tools/dis8051.py switch <addr>` decodes this.

The table at `0x0077` is the device's complete command set - 19 opcodes:

| Cmd | Handler | | Cmd | Handler |
|---|---|---|---|---|
| `0x08` | `0x01e1` | | `0x83` | `0x0410` |
| `0x09` | `0x02c5` | | `0x84` | `0x0410` |
| `0x0b` | `0x00b4` | | `0x85` | `0x03cc` |
| `0x0c` | `0x0254` | | `0x86` | `0x03bd` |
| `0x0d` | `0x033b` | | `0x87` | `0x0109` |
| `0x0e` | `0x0379` | | `0xdc` | `0x00f1` |
| `0x36` | `0x017f` | | `0xde` | `0x00fa` |
| `0x37` | `0x01ba` | | `0xe8` | `0x01c7` |
| `0x46` | `0x0436` | | `0xe9` | `0x01d6` |
| | | | `0xeb` | `0x00d6` |
| | | | default | `0x0574` |

Replies go back through `EP1INBUF` (`0xE7C0`) with the length in `EP1INBC`
(`0xE68F`). Characterised: `0x08` (I2C write), `0x09` (I2C read/xfer),
`0x36`/`0x37` (start/stop), `0x83`/`0x84` (decoder reset), `0x85` (status poll),
`0x46` (GPIO dump). The Windows session uses only those plus `0x0b`, `0x86` and
`0xde`, once each, whose semantics are unknown - the replay sends them because
the original did. The remaining nine opcodes in the table never appear in the
capture at all; the audio and tuner paths are the obvious candidates for what
they are for.

Command frequency across the session is worth knowing before optimising
anything: 2563 × `0x85`, 488 × `0x08`, 103 × `0x09`, 2 × `0x36`, one each of
`0x0b`, `0x37`, `0x86`, `0xde`. The session is dominated by status polling.

### The I2C path, and why reads are fragile

The routine at `0x13a0` is a generic interrupt-driven state machine: state in
`0x1b`, buffer pointer `0x17`–`0x19`, index `0x1a`, count `0x16`. The slave
address arrives from the caller in a buffer rather than as an immediate, which
is why the decoder could never be identified by scanning the image for constants
- any such scan is noise.

Command `0x09` (`0x02c5`) is the passthrough:

```
byte[0]   0x09
byte[1]   number of data bytes to write   -> count at 0x53
byte[2]   reply length - 1                -> EP1INBC = byte[2] + 1
byte[3]   7-bit slave address             -> R7 -> R4, doubled at 0x17dc
byte[4..] data
```

The helper at `0x17b8` asserts START, writes `addr << 1` to `I2DAT`, then polls
`I2CS` for BERR (bit 2) and ACK (bit 1). The ISR records the outcome in `0x1b`
and byte 0 of the reply carries it: `0x08` ACK, `0x07` NACK. That is the whole
basis of `gbcap i2cscan`.

One firmware defect constrains everything built on top. When the ISR state is
not 0, the routine at `0x1af1` refuses to start a read and reports it by
clearing the carry flag; its caller at `0x1cb5` never tests the carry and
busy-waits, with **no timeout**, for a transfer that was never started. The
8051 wedges and the command endpoint stops answering. Which transactions leave
the state non-zero was measured, not derived: `wcount=2, rcount=1` reads are
reliable indefinitely, while `wcount=1` and any `rcount > 1` wedge. One
zero-count transaction resyncs it (it skips the write phase, so it is a read,
not a write). `driver/gb-i2c.c` refuses the shapes that wedge.

Command `0x08` (`0x01e1`) is the fast write path and has none of these problems.
`08 <addr7> <len> <reg_hi> <reg_lo> <data...>` with `len` counting from `reg_hi`
maps exactly onto the Linux kernel's `cx25840_write()` (len 3) and
`cx25840_write4()` (len 6), which is what lets the in-kernel `cx25840` driver
run over this bridge unchanged.

### Streaming is armed by SET_INTERFACE

The `EP2CFG` and `EP2ISOINPKTS` writes live at `0x1112`–`0x1200`, inside the
`SET_INTERFACE` handler. `EP2ISOINPKTS` selects ×1/×2/×3 packets per microframe
and **defaults to ×1**, which is the complete explanation for a puzzle that
looked like a bandwidth problem: before streaming is armed, every ×3 alt errors
100% of its packets because two of the three transactions per microframe have
nothing to carry, while the ×1 alts return clean zero-length packets. Moving the
device to a root port changed nothing, which refuted the hub hypothesis.

The Windows driver does also send `36 03 00` - `<isoinpkts> <mode>` - but only
*after* the alt switch, and it stops with `37` before re-arming with another
`SETIF 1` and `36 03 00`. An earlier reading of the capture concluded `0x36` was
never sent; that came from the truncated parse described below, and the full
session contains it twice. Whether `0x36` is redundant given the handler above
has not been tested - the replay sends both because the original does.

### The decoder reset line

`0x83`/`0x84` share a handler at `0x0410`:

```
0410:  ANL IOD,#0xfd     ; drive port D bit 1 LOW
0413:  R7 = 0x14         ; delay 20
0417:  LCALL 0x195d
041a:  ORL IOD,#0x02     ; release port D bit 1 HIGH
041d:  R7 = 0x0a         ; delay 10
0421:  LCALL 0x195d
```

A low-high pulse on an FX2 GPIO - the decoder's reset. Command `0x46` reports
`OED` and `IOD` back to the host, i.e. it is a GPIO state dump for exactly this
line.

## Where the bring-up sequence came from

`tools/find-regtables.py` scored every window of `avcgbdr.sys`'s `.data` and
`.rdata` against the shape a decoder init table would have - `{u16 reg; u8 val}`
at 3- or 4-byte stride, addresses varying, values varying - with no luck.

The cheap alternative was to watch it run: Windows XP in a VM, device passed
through, `usbmon` on the host, VirtualDub previewing. Only EP `0x01` OUT matters
and it is bulk/interrupt, so the isochronous-passthrough problem that usually
makes VM capture painful didn't apply. Each packet is a register write.

## The userland data path

`tools/gbcap session` came first and is still the reference the driver is checked
against. The kernel driver (`driver/gb-video.c`) does the same in a URB
completion handler and feeds the same assembler, `driver/gb-bt656.c`:

1. Claim interface 0, execute the session script (`CMD` / `SETIF` / `CLRHALT`),
   sending each command as bulk or interrupt according to the alt in force.
2. Confirm EP `0x82` is isochronous at the resulting alt, then submit async
   transfers - 16 in flight × 32 packets each by default, overridable with
   `GB_XFERS` and `GB_PKTS`.
3. Feed every packet's payload to an incremental BT.656 scanner.
4. Write completed frames as raw UYVY.

Measured: 21.36 MB/s, 25311 data packets in 3.4 s, packet lengths 3030 and 2020.

### The BT.656 assembler

The scanner must be incremental: a 1440-byte line never aligns with 3030-byte
packets, so lines straddle packet boundaries and state carries across
completions. Three design points, each of which was a bug first:

- **Validate the timing code's low nibble.** The device zeroes the four
  protection bits, so a real `XY` has a zero low nibble. A bare bit-7 test
  accepts `0xFF`, which is nearly always the first byte of the *next* preamble;
  consuming it loses sync.
- **Run the preamble matcher mid-line, not only between lines.** BT.656 reserves
  `0x00` and `0xFF`, so `FF 00 00` inside a supposed line means sync was lost -
  almost always because a packet was dropped and the copier ran through the gap,
  swallowing the next preamble. Resyncing there rather than trusting the byte
  count took complete frames from about a third to nearly all.
- **Take frame boundaries from the F bit.** The V bit appears only on blanking
  lines, and the device drops ~7% of microframes precisely during blanking, so
  vblank markers go missing and two fields merge into one 486-line run -
  measured at 6 of 77 runs. The F bit is in every line's code. This change took
  complete frames from 55/60 to 33/34 interlaced and 94/102 progressive.

Interlaced assembly puts field 1 in even rows and field 2 in odd. A 240p source
never sends the second field, so `--progressive` treats each field as a frame
with consecutive lines; without it every other row stays black and no frame ever
completes.

The 7% packet loss appears to be device behaviour rather than host buffering:
queue depths from 8×64 through 64×8 give the same rate, and 21.36 of a possible
24.24 MB/s is about 88%. It has not been investigated further because the
resync logic absorbs it.

`tools/bt656-decode.py` is a second, independent implementation in Python.
It follows the same F-bit rule and produces byte-identical frames on the sample
slices in `captures/samples/`.

## Standards handling

`0x0400` bits 3:0 select the standard (0 NTSC-M, 1 NTSC-J, 2 NTSC 4.43,
4 PAL-BDGHI, 5 PAL-M, 6 PAL-N, 8 SECAM); the captured session leaves
`0x0400 = 0x0004e021`. `0x040d` bits 3:0 report what the decoder has *detected*,
which is the register to consult before poking around.

A full change is four writes, per `set_v4lstd()`: force NTSC-M, clear LCOMB in
`0x047b`, select the format, set the `pal_m` bit in `0x0403`. The Windows
session never writes `0x0403`, so a PAL selection copied from the capture alone
gives line lock with wrong colour. `captures/session-palm.txt` documents the
worked sequence.

Geometry must follow - 240 active lines per field for 525/60, 288 for 625/50 -
and the active-window timing registers `0x0470`–`0x047c` still hold the captured
NTSC values (`7e 00 2d 5b`, `19 70 1e 1e`, `1f 02 58 66`, `1f 7c 08 00`) where
`cx25840_std_setup()` would use 132/720 and 36/288/46 for 625/50. That is a
limit of the userland replay only. The kernel driver leaves the standard to the
in-kernel `cx25840` driver, which computes the window from the standard.

## Driver design

Staged deliberately, cheapest-information-first. All three stages are done; the
third is `driver/`.

**Stage 1 - userland, libusb.** `gbcap`. Async isochronous on EP 0x82 at alt 1,
raw dump or decoded frames, everything debuggable with gdb and nothing able to
panic the machine. This is also where the protocol got documented well enough to
write anything else.

**Stage 2 - userland daemon + `v4l2loopback`.** Makes the device usable by real
applications while the wire format and bring-up are still being refined. Costs a
memory copy and some latency; acceptable while iterating.

**Stage 3 - out-of-tree kernel V4L2 module.** `stk1160` was the structural model
(clean, modern, ~2500 lines, bridge + external I2C decoder via `v4l2_subdev`);
`usbtv` the model for the URB-completion frame assembler (~1700 lines). The
bridge registers an `i2c_adapter` whose transfer hook translates I2C messages
into command-endpoint transfers - note that this is EP `0x01`, not control
transfers, and that the transfer type follows the current alt - and then
`v4l2_i2c_new_subdev` binds the in-kernel `cx25840` driver.

**Latency.** The point of the exercise. Budget is one NTSC field (one 60 Hz
refresh), 16.7 ms. A `v4l2loopback` → `mpv`/`ffplay` pipeline spends 50–120 ms
in player buffer queues and is unusable for playing a console - `gbplay.sh` 
already passes `-fflags nobuffer -flags low_delay -framedrop` and the
low-latency mpv profile for this reason. The kernel driver itself adds 0.05 ms
(median frame age at `DQBUF`, `tools/gblat`, 600 frames on 7.2.7); the rest is
the player. The end state is a direct presenter: open the V4L2 node, `REQBUFS`
with `count = 2`, `mmap`, and `DQBUF` → GL texture upload → YUV→RGB in a
fragment shader → present, vsync off. That path should be 4–12 ms - an 
estimate, since the presenter is not built yet.

## Open questions

| Question | State |
|---|---|
| A minimal bring-up sequence for the userland tools | derived statically - `captures/minimal-init-candidate.txt`, 173 steps - but **untested**; `gbcap` replays all 3171. The kernel driver does not replay the session: it lets the in-kernel `cx25840` driver configure the decoder |
| Semantics of `0x0b`, `0x86`, `0xde` | unknown; replayed because the original sent them |
| Purpose of `0x0c`–`0x0e`, `0xdc`, `0xe8`, `0xe9`, `0xeb` | dispatched by the firmware, never used by the Windows driver. (`0x87` turned out to be the tuner's reset, see `tuner.md`) |
| Whether `0x36` is redundant given the `SET_INTERFACE` handler | untested |
| PAL active-window timing (`0x0470`–`0x047c`) in the userland replay | still carries captured NTSC values; the kernel driver is not affected |
| The ~7% microframe loss | characterised, not explained; absorbed by resync |
| Audio | **solved**: 48 kHz stereo PCM in the video stream's horizontal blanking, not on a separate endpoint |
| Tuner (XC2028, needs `xc3028-*.fw`) | loads and reaches ACTIVE; never tuned to a signal - see `tuner.md` |
