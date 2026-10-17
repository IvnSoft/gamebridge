# Roadmap

## Done

- [x] Identify the silicon: Cypress EZ-USB FX2LP, C0 EEPROM, 8051 held in reset
- [x] Establish that `03f3:009b` is a loader shell and `03f3:009c` is the device
- [x] Locate the real firmware (embedded in `avcgbfl.sys`, not the `.rom` files)
- [x] Extract it - `tools/extract-fw.py`, 558 hex records → 7624 bytes
- [x] Upload it and re-enumerate the device — `tools/gbfw`
- [x] Capture the real `009c` descriptors and the isochronous bandwidth ladder

- [x] Establish whether the firmware streams autonomously — **it does not**;
      the device answers isochronous INs with clean zero-length packets and
      needs a start command (`tools/gbcap`)

## Done - video works end to end

- [x] Firmware extracted and uploaded; device wakes as `03f3:009c`
- [x] Command protocol decoded (19 opcodes; `0x08` write, `0x09` read, `0x36`/`0x37`
      stream, `0x83` decoder reset)
- [x] Chips identified by opening the case: CX25840, FX2LP, XC2028, 24C02, AK5357
- [x] Windows session captured and replayed; video streams at 21.36 MB/s
- [x] Wire format identified as raw BT.656, no vendor framing
- [x] **Colour correct**, verified against a DSC-WX1 on composite
- [x] **PAL works** — `ACFG_DIS` (`0x400` bit 5) had to be cleared
- [x] `makoaudC.rom` identified as CX25840 audio microcode (I2C `0x0802`, 59-byte chunks)
- [x] Frame assembly: complete (476–478 of 480 rows), on time (29.98 fps, 1.78 ms jitter)
- [x] Usable by ordinary applications before any kernel module: `tools/gb-up.sh`,
      `gbcap → gbloop → v4l2loopback` plus `snd-aloop` for the sound - 60.0 fps
      sustained, 0 torn frames

## Done - audio

 - 48 kHz stereo PCM embedded in the video stream's horizontal blanking,
   no second endpoint involved.

## Done - the infrared receiver

**The remote works, and it is a standard Linux rc-core device.** `rc0`, keymap
`rc-gamebridge`, extended NEC, all 28 buttons mapped, plus a lirc scancode node.

This was the one feature with **no vendor implementation to work from** -
Adaptec's driver contains no IR support at all, so nothing here was ported. The
receiver is command `0x85`, which had been sitting quietly in the notes
since the first week labelled as a counter. It looked inert because the Windows
capture contains 2563 polls of it and **not one keypress**.

The keymap was read back twice, on two machines, through two entirely different
code paths - `gbcap irwatch` under libusb on xHCI/6.16, and the driver itself on
EHCI/6.19+debug - and both runs returned the same 28 codes in the same order.
Two structural properties agree with it too : the five blue-printed buttons are
exactly the complete `0x01`-`0x05` bank, and the digit columns run consecutively
in BCD.

Note: `START` is mapped to `KEY_MEDIA`.

## Done - the kernel driver

- [x] Out-of-tree module, `driver/`: uploads the firmware itself on plug-in,
      exposes the device's I2C bus so the kernel's own `cx25840` driver runs the
      decoder, and provides a V4L2 capture node, an ALSA capture card and an
      rc-core remote
- [x] The frame assembler is one source file, `driver/gb-bt656.c`, compiled into
      both the module and `gbcap` - so every change can be replayed against a
      saved capture in userspace before it reaches the kernel
- [x] `v4l2-compliance` 80/80 in August, 84/84 on 7.2.7 in October
- [x] Tested under KASAN and lockdep three times: on EHCI in August, which found the
      bug behind three hard lockups; and on xHCI in October, after an independent
      review, on RHEL's 5.14 and on mainline 7.2.7, with no sanitizer report
      in any test (see `SECURITY.md`)
- [x] The front LED kept flashing after a stream: the stop command was being
      acknowledged and silently dropped (a USB data-toggle mismatch after every
      alternate-setting switch), so the device really kept streaming. Fixed by
      resetting the command endpoints after every switch
- [x] Builds on mainline 6.16 and 7.2 and on RHEL 9's 5.14, by testing kernel headers
      rather than version numbers; the IR receiver is optional where the kernel
      has no rc-core
- [x] Runs on 7.2.7: the full functional list, all 28 remote buttons, and a
      physical unplug and replug that re-uploads the firmware
- [x] The stream parser is fuzzed: `tools/fuzz/`, one hour, no report

## TODO - the tuner

**Loaded and ACTIVE, never tuned.**

The XC2028 accepts its firmware over this bridge - 8706 bytes of base image plus
SCODE, thirteen bytes per I2C write, zero splats under KASAN. The reset callback
(command `0x87`, port D bit 7) works. The V4L2 plumbing is in: a `Tuner` input
typed `V4L2_INPUT_TYPE_TUNER`, `g/s_tuner` and `g/s_frequency`, `V4L2_CAP_TUNER`
advertised only when a subdev attaches.

TODO: Point it at my Atari's RF output on channel 3 and see whether a
picture appears; then capture what Windows does when its tuner is enabled, to
learn whether Adaptec's driver ever *reads* `0x61` at all.

Both `tuner=1` and `i2c_fake_tuner_read=1` are off by default and must stay off
until a picture has been seen.

## Next

- Run this release on EHCI; so far it has run on xHCI only (7.2.7 stock and
  debug, RHEL's 5.14 debug)
- A DKMS package, so the module rebuilds itself after a kernel update
- A 240p mode in the driver, for sources that never interlace (the userland
  tools have one: `gbcap --progressive`)
- Proper suspend and resume; today the USB core unbinds and rebinds the driver
- Harden the userland tools, starting with the replay parser ( which accepts
  malformed input among others )
- Upstreaming - blocked on firmware redistribution; see `NOTICE`

