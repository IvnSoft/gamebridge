# The kernel driver

`driver/` builds `gamebridge.ko`, an out-of-tree Linux module. With it loaded the
GameBridge is an ordinary system device: a V4L2 capture node (`/dev/videoN`), an
ALSA capture card (`GameBridge`) and, where the kernel has rc-core, an infrared
remote. Anything that opens a camera or a microphone can use it - ffmpeg,
GStreamer, OBS, a browser.

This is the way to use the device. The userland tools in `tools/` (`gbcap`,
`gbplay.sh`, `gb-up.sh`) came first and are kept for bring-up, diagnosis and
working without the module; the rest of `docs/guide/README.md` documents them.

## What it does

| | |
|---|---|
| Firmware | uploads the 8051 image itself on plug-in: `03f3:009b` → `03f3:009c`, no manual step |
| Video | 720×480 NTSC / 720×576 PAL, UYVY, interlaced, ~30 / ~25 fps |
| Standards | any the CX25840 decodes (`V4L2_STD_ALL`); switching while idle |
| Inputs | `Composite`, `S-Video`; `Tuner` only with `tuner=1` |
| Audio | 48 kHz, 16-bit big-endian (`S16_BE`), stereo, from the video stream's blanking |
| Remote | the 28-button remote, as keycodes via rc-core (keymap `rc-gamebridge`) |

Audio and video come off one isochronous stream, so they share its lifetime:
whichever starts first brings the stream up, and it stops when the last user
leaves.

## Building

Needs the headers for the kernel you will load it into (Fedora/RHEL:
`kernel-devel`), plus `gcc` and `make`.

```sh
make -C driver                      # against the running kernel
make -C driver KDIR=/path/to/build  # against another kernel's headers
```

It has been built clean at `W=1` on mainline-based **6.16** and **7.2** (Fedora)
and on **RHEL 9's 5.14** (Rocky 9.8), and run on 7.2.7 and 5.14. Compatibility is
tested against the headers rather than the version number, because RHEL
backports parts of newer APIs and 7.x removed vb2's `wait_prepare`/`wait_finish`.

The kernel must provide the `cx25840` decoder module (`CONFIG_VIDEO_CX25840`)
and videobuf2. Without rc-core (`CONFIG_RC_CORE`, absent on RHEL 9) the module
still builds; it just has no IR receiver.

## Installing

The firmware is extracted from Adaptec's own Windows driver and is not
redistributed here, so it has to be built first:

```sh
./tools/fetch-firmware.sh
python3 tools/extract-fw.py firmware/avcgbfl.sys firmware/gb-loader-fw.bin
sudo make -C driver firmware     # -> /lib/firmware/gamebridge/gb-loader-fw.bin
sudo make -C driver install      # modules_install + depmod
sudo modprobe gamebridge
```

Then plug the device in (or replug it). The kernel log shows the upload, the
re-enumeration as `009c`, and the decoder being found:

```sh
sudo dmesg | grep -i gamebridge
```

With **Secure Boot** on, an unsigned module is refused with `Key was rejected by
service`. Sign it with an enrolled MOK key, or load it with Secure Boot off; the
same issue for v4l2loopback is described in `v4l2loopback.md`.

The upload goes to the FX2's RAM, never to its EEPROM, so it is
repeated on every plug-in. That is by design.

## Using it

Find the node and the card:

```sh
v4l2-ctl --list-devices          # "Adaptec GameBridge AVC-1410" -> /dev/videoN
arecord -l                       # card "GameBridge"
```

**Watch** with ffplay:

```sh
ffplay -f v4l2 -input_format uyvy422 -video_size 720x480 \
       -fflags nobuffer -flags low_delay -framedrop -i /dev/videoN
```

Use ffplay rather than mpv. mpv stuttered here in every configuration tried;
part of the reason is that V4L2 timestamps count from boot, which ffplay rebases
and mpv schedules against.

**Listen** with arecord. The format is big-endian, which a WAV file cannot hold,
so ask for raw output - without `-t raw` arecord fails before capture starts:

```sh
arecord -D hw:CARD=GameBridge -t raw -f S16_BE -r 48000 -c 2 out.raw
```

In a desktop session PipeWire adopts the card as soon as it appears, so it also
shows up as an ordinary audio input.

**Standard and input**, with v4l2-ctl:

```sh
v4l2-ctl -d /dev/videoN --set-standard pal      # or ntsc, pal-m, secam ...
v4l2-ctl -d /dev/videoN --list-inputs
v4l2-ctl -d /dev/videoN --set-input 1           # 0 Composite, 1 S-Video
```

Changing the standard is refused with `EBUSY` while anything is streaming —
video **or** audio, since both share one stream whose frame size the standard
sets. Setting the standard already selected always succeeds.

The driver always delivers interlaced frames. The userland tools' 240p mode
(`gbcap --progressive`, for Atari 8-bits and other sources that never
interlace) has no equivalent in the driver yet.

**The remote**, with ir-keytable (v4l-utils):

```sh
ir-keytable                      # lists rc0 with keymap rc-gamebridge
sudo ir-keytable -t              # live scancodes and keycodes
```

Keys arrive as an ordinary input device. The mapping is in `driver/gb-ir.c` and
can be overridden with ir-keytable without recompiling; the receiver's protocol
is in `internals/ir-receiver.md`.

## Module parameters

| Parameter | Default | Effect |
|---|---|---|
| `audio` | `1` | register the ALSA card; `audio=0` for video only |
| `ir` | `1` | register the IR receiver; `ir=0` stops polling the device |
| `ir_poll_ms` | `50` | IR poll interval, floor 10. Above ~100 ms held keys repeat badly |
| `tuner` | `0` | attach the XC2028 tuner. **Unverified**: it loads, but no RF signal has been tuned (`internals/tuner.md`). Needs `xc3028-v27.fw` |
| `vid_input` | `-1` | override the CX25840 routing for the selected input; diagnostic |

The rest (`zero_buf`, `legacy_window`, `raw_dump`, `i2c_fake_tuner_read`,
`i2c_tuner_split`, `i2c_rcount2`) are labelled diagnostics for development.
Leave them off; `modinfo gamebridge` describes each.

## Unloading

```sh
sudo modprobe -r gamebridge
```

If it reports the module in use, something holds the ALSA card - PipeWire, or
on some distributions `alsa-state.service`, which keeps the control device open.
Unplugging the device while a player has it open is handled; this was tested
under KASAN with video and audio both open, in both close orders.

## Known gaps

- This release has run on xHCI only: 7.2.7 (stock and debug) and RHEL's 5.14
  (debug). Not yet on EHCI.
- Suspend and resume go through the USB core's unbind and rebind: streams stop
  and the device is set up again.
- The tuner is off by default and unverified, as above.
- No DKMS package and no udev rule yet; rebuild after a kernel update.

How the driver was tested is summarised in `SECURITY.md` at the repository root.
