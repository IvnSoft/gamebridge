# Using the GameBridge as a normal webcam (v4l2loopback)

This was stage 2: making the device usable by ordinary applications - a
browser, OBS, mpv, anything that opens a `/dev/videoN` capture node - while the
wire format was still being refined and before a kernel module existed.

> **The kernel module exists now** — [`driver.md`](driver.md). It gives a real
> `/dev/videoN` and ALSA card directly, with no pipeline to start, and is the
> way to use the device. This page still works without the module.

The pieces:

```
gbcap  --frames -   →   gbloop   →   v4l2loopback   →   /dev/video9   →   any app
(libusb, isoc,          (write()      (kernel)          (a normal
 BT.656 → UYVY)          per frame)                      capture node)
```

`gbcap` already produces whole 720×480 (or 720×240) UYVY frames on stdout as a
bare byte stream - no container, no framing. `gbloop` opens a v4l2loopback node
as a **video output** device, agrees a format with `VIDIOC_S_FMT`, and copies
frames from stdin into it, one `write()` per frame. That is the entire trick;
v4l2loopback does the rest by presenting the same buffers on the capture side.

## 1. Install v4l2loopback on Fedora

It is an out-of-tree module, packaged by RPM Fusion (free).

```sh
sudo dnf install akmod-v4l2loopback v4l2loopback
sudo dnf install v4l-utils            # v4l2-ctl, used for every check below
```

`akmod-v4l2loopback` is the source form: `akmods` rebuilds it for each new
kernel. That build happens on the next boot after a kernel update, so the
module is briefly missing after `dnf upgrade` until then. To force it now:

```sh
sudo akmods --force
sudo modprobe v4l2loopback
modinfo v4l2loopback | head -5      # confirms a module actually exists
```

**Secure Boot.** An akmod is locally built and therefore unsigned; with Secure
Boot enabled `modprobe` fails with `Key was rejected by service` (`-EKEYREJECTED`),
which reads like a missing module but is not. Either enrol a local key with
`kmodgenca` + `mokutil --import`, or turn Secure Boot off. Check with
`mokutil --sb-state`.

## 2. Create the node

> **The `modprobe` form below only works if the module is not already loaded**,
> and on a desktop it usually is - OBS inserts it the first time its virtual
> camera runs. Module parameters are read only at insertion, so the command then
> succeeds silently but does nothing. Check with `lsmod | grep v4l2loopback`
> before trusting it.

**If the module is already loaded** - add a node to it, and re-run udev, because
a node created through sysfs lands `root:root 0600` and the user who created it
cannot open it:

```sh
sudo v4l2loopback-ctl add -x 1 -b 2 -n "GameBridge AVC-1410" /dev/video9
sudo udevadm trigger --subsystem-match=video4linux --action=add
```

`-x 1` is `exclusive_caps`, `-b 2` is `max_buffers`. After the trigger the node
is `root:video 0660` with the desktop session's `uaccess` ACL, exactly as a node
created at module-load time would be. Remove it again with
`sudo v4l2loopback-ctl delete /dev/video9`.

**If the module is not loaded**, the parameter form is equivalent:

```sh
sudo modprobe v4l2loopback \
     video_nr=9 \
     card_label="GameBridge AVC-1410" \
     exclusive_caps=1 \
     max_buffers=2
```

| Option | Why |
|---|---|
| `video_nr=9` | Fixes the node at `/dev/video9` instead of taking the next free number. Scripts and OBS scene sources need a stable path. |
| `card_label` | The name applications show in their device picker. Without it the entry reads "Dummy video device (0x0000)", which is unhelpful in a list of webcams. |
| `exclusive_caps=1` | **Usually required.** See below. Some releases spell this `announce_all_caps=0`; `modinfo -p v4l2loopback` says which. |
| `max_buffers=2` | Fewer queued buffers, less latency. The default (8) adds up to eight frames - a quarter-second at 30 fps. |
| `devices=1` | Only if more than one loopback node is wanted. |
| `max_width` / `max_height` | Leave unset. If set, they cap what `VIDIOC_S_FMT` will accept and `gbloop` refuses to start rather than silently streaming the wrong geometry. |

**`exclusive_caps=1`, specifically.** A loopback node by default advertises
`V4L2_CAP_VIDEO_CAPTURE` *and* `V4L2_CAP_VIDEO_OUTPUT` at all times. Chrome,
Firefox and several GStreamer-based applications either ignore such a node or
mis-detect it, because a real webcam is never both. With `exclusive_caps=1` the
node advertises OUTPUT while unclaimed, and flips to capture-only for everyone
else the moment a producer (here, `gbloop`) opens it. The visible consequence:
**the device does not appear in a browser's camera list until `gbloop` is
running.** That is correct behaviour, not a fault.

To make it persist across reboots:

```sh
# /etc/modprobe.d/v4l2loopback.conf
options v4l2loopback video_nr=9 card_label="GameBridge AVC-1410" exclusive_caps=1 max_buffers=2

# /etc/modules-load.d/v4l2loopback.conf
v4l2loopback
```

**Permissions.** The node is `root:video`, mode `0660`. On a Fedora desktop the
logged-in user normally gets access anyway through systemd's `uaccess` ACL —
check with `getfacl /dev/video9`. If not, `sudo usermod -aG video $USER` and log
back in. `gbloop` itself does **not** need root; only `gbcap` does, for raw USB.

## 3. The pipeline

The short version, which does everything on this page including waking the
device, creating the node if it is missing and starting sound:

```sh
./tools/gb-up.sh                 # interlaced 720x480 at 30 fps; asks for sudo itself
GB_PROGRESSIVE=1 ./tools/gb-up.sh   # 240p at 60 fps, for 8-bit computers
GB_PAL=1 ./tools/gb-up.sh           # 625/50
```

The long version, and what `gb-up.sh` is doing. Wake the device first
(`docs/guide/README.md` - the firmware load is volatile and does not survive a
replug), then:

```sh
cd gamebridge                # the repository root

# 240p source (Atari 8-bits, most home computers): 60 whole frames a second
sudo ./tools/gbcap session captures/full-session-replay.txt 3600 \
     --progressive --frames - \
  | ./tools/gbloop --width 720 --height 240 /dev/video9

# genuinely interlaced source (VCR, console): 720x480, 30 fps
sudo ./tools/gbcap session captures/full-session-replay.txt 3600 \
     --frames - \
  | ./tools/gbloop --width 720 --height 480 --field interlaced /dev/video9
```

`gbloop` prints what it negotiated and then a progress line every five seconds:

```
gbloop: /dev/video9 [GameBridge AVC-1410] 720x240 UYVY none, 60 fps, 345600 bytes/frame
gbloop: reading frames from stdin
gbloop: 300 frames, 60.0 fps, 98.9 MB
```

^C stops it cleanly and prints totals. `--quiet` suppresses the periodic line.

**The geometry must match the producer exactly.** The stream has no framing to
resynchronise from, so if `gbcap` emits 240-line frames and `gbloop` is told 480,
nothing errors - the picture simply shears and never recovers. `gbcap
--progressive` ⇔ `gbloop --height 240`; no flag ⇔ `--height 480`. Getting this
pair wrong is the single most likely cause of a broken-looking result.

Watch it:

```sh
v4l2-ctl -d /dev/video9 --all          # what consumers see
mpv av://v4l2:/dev/video9
ffplay -f v4l2 -i /dev/video9
```

In a browser or OBS the entry appears under whatever `card_label` was set - but
only while `gbloop` is running, per `exclusive_caps` above.

## 3a. Sound, the same trick one layer down

`v4l2loopback` presents a pipe as a camera. `snd-aloop` presents a pipe as a
sound card, and the GameBridge's audio needs exactly that - it rides in the video
stream's horizontal blanking, so it comes out of `gbcap` as a second byte stream
with nowhere to go.

```sh
sudo modprobe snd-aloop
# producer:  gbcap --audio FIFO  ->  aplay -D hw:Loopback,0,1
# consumer:  anything            <-  hw:Loopback,1,1
```

**Subdevice 1, not 0.** PipeWire claims `hw:Loopback,0,0` the moment the card
appears, and the two error messages that result - `Device or resource busy` on
the producer, and a capture side that claims it can only do `S32_LE` - name
neither PipeWire nor the real problem. Open the producer before the consumer, and
stay off subdevice 0.

Verified end to end: the stream's native `S16_BE` at 48 kHz stereo passes through
unconverted. Hear it with

```sh
arecord -D hw:Loopback,1,1 -t raw -f S16_BE -r 48000 -c 2 \
  | aplay -t raw -f S16_BE -r 48000 -c 2 -D plug:default -
```

**`-t raw` on both ends is not optional.** `arecord` writes a WAV header by
default, and WAV cannot express big-endian samples at all, so it exits with
*"Wave doesn't support S16_BE format"* before recording a byte - and `aplay`
then reports a read error on a header that was never written. Two messages,
neither naming the cause.

Measured with music on the input and the camera on video, both read
simultaneously:

```
VIDEO  720x480, Y 16..254, 239 distinct luma values
AUDIO  L rms 244.2 peak 905  |  R rms 240.0 peak 921  |  correlation +0.662
```

Both halves come off **one** isochronous endpoint, so there is no way to have
sound without video. That is the hardware, not the plumbing.

## 4. Known limitations

**An extra full copy of every frame.** The kernel already copied the isochronous
payload into `gbcap`'s buffers; `gbcap` assembles lines into a frame; `gbloop`
copies that frame back into the kernel; v4l2loopback hands it to the consumer,
which copies it again. At 720×480×2 × 30 fps - or 720×240×2 × 60, identically -
that is 20.7 MB/s pushed across the user/kernel boundary one extra time. Not
fatal on any modern machine, but it is pure waste.

**Latency.** The point of this hardware is playing a console on a monitor, and
the budget is one NTSC field (one 60 Hz refresh), 16.7 ms. This path does not
meet it. A frame passes through a pipe buffer, the loopback's own queue (`max_buffers`), and then
the consumer's queue - `mpv`/`ffplay` alone typically hold 50–120 ms. Reduce it
with `max_buffers=2`, `mpv --profile=low-latency --untimed --no-cache`, or
`ffplay -fflags nobuffer -flags low_delay -framedrop` (which is what
`tools/gbplay.sh` already does, bypassing the loopback entirely). It stays
worse than a direct path.

**No controls, no inputs, no standards.** A real capture driver exposes
brightness/contrast/saturation as V4L2 controls, the composite/S-Video choice as
`VIDIOC_ENUMINPUT`, and NTSC/PAL as `VIDIOC_S_STD`. A loopback node has none of
that - it is a pipe with a video-shaped API. Every CX25840 setting has to be
made on the `gbcap` side, and the applications cannot see or change them.

**No negotiation.** A consumer cannot ask for a different resolution or pixel
format; it gets whatever `gbloop` set. Applications that insist on, say, MJPEG
or 1280×720 will refuse the device rather than adapt.

**Timestamps are wrong.** Buffers are stamped when `gbloop` writes them, not
when the CX25840 digitised the line, so they carry the accumulated jitter of the
whole chain. Nothing downstream can recover real capture timing.

**One producer, and the frame boundary is fragile.** v4l2loopback treats each
`write()` as one complete buffer. `gbloop` therefore issues exactly one `write()`
per frame and, on a partial write, drops the remainder and counts a torn frame
rather than continuing - writing the tail would be interpreted as a *new* frame
and would offset every frame after it by half a picture. Torn frames are
reported in the exit summary; a non-zero count there is worth investigating.

**Nothing survives a replug.** The FX2 firmware is volatile,
so a disconnect ends the stream, `gbloop` sees EOF, and the whole pipeline
has to be restarted after re-uploading firmware. The loopback node itself stays
around, advertising a camera that doesn't work.

## 5. Why the kernel driver was the goal

Written before the driver existed; `driver.md` is the result. The firmware
upload ended up in the driver itself rather than in a udev rule.

An out-of-tree V4L2 module modelled on `stk1160` removes every item above
rather than mitigating them:

- **Zero copies.** URB completion writes straight into a `videobuf2` buffer that
  the application `mmap`s. No pipe, no second trip through the kernel.
- **Real timestamps**, taken at URB completion, so downstream A/V sync is possible.
- **Real controls**: the CX25840 binds as a `v4l2_subdev` through its existing
  in-kernel driver, and brightness, contrast, input selection and video standard
  become ordinary V4L2 controls on the node.
- **Proper negotiation**, `ENUM_FMT`/`ENUM_FRAMESIZES`, so applications adapt to
  the device instead of the other way round.
- **Autoloading.** A udev rule uploads the firmware and the node appears by
  itself, instead of a two-process pipeline started by hand under `sudo`.
- **Latency.** With `REQBUFS count=2`, `mmap`, and a presenter that uploads
  straight to a GL texture, the path should be 4–12 ms (an estimate; that
  presenter is not built yet) - inside the budget the loopback route cannot
  reach. The driver's own share is measured: 0.05 ms (`tools/gblat`).

Without the module, this page is still how the device gets used.

## 6. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `gbloop: /dev/video9 is not a V4L2 device` | Module not loaded, or the wrong node number. `v4l2-ctl --list-devices`. |
| `VIDIOC_S_FMT: Device or resource busy` | A consumer already opened the node and pinned a format. Close it, start `gbloop` first, then the consumer. |
| `driver refused the format: asked 720x240 UYVY, got …` | The module was loaded with `max_width`/`max_height` set below what was asked for. Reload without them. |
| `modprobe: unknown parameter exclusive_caps` | Version difference - some releases spell it `announce_all_caps=0` instead. `modinfo -p v4l2loopback` lists what the installed one takes. |
| Device missing from the browser's camera list | Expected with `exclusive_caps=1` until `gbloop` is running. If it is running, check `getfacl /dev/video9` for access. |
| Picture shears diagonally and never settles | `gbcap --progressive` and `gbloop --height` disagree. |
| `modprobe: Key was rejected by service` | Secure Boot rejecting the unsigned akmod. |
| Non-zero "torn" count at exit | Short writes to the loopback node - report it, it has not been seen in practice and the handling is untested. |
