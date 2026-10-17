# Security

## Reporting

Open an issue. If you would rather not do that in public, the address in the
commit history reaches me.

There is no bounty and no SLA. It is one person and one discontinued capture
device.

## What this is

A Linux kernel module plus a few userland tools. The module runs in kernel
context and `insmod` needs root, so anyone who can load it can already do
anything. 

## The real attack surface is the video stream

The module's data path takes isochronous USB packets from the device and
assembles them into frames, writing into videobuf2 planes. That assembler parses
BT.656 timing codes and ancillary-data blocks out of the byte stream, with the
declared length of a block taken from the stream itself. It is the only code
here that makes memory-write decisions based on data the device supplied.

A malfunctioning or hostile device on that endpoint is therefore the first
threat worth thinking about. The second is a local user: anyone who can open
`/dev/videoN` - the console user, or the `video` group - can drive the ioctls,
and twice that has been enough to write past a buffer (see below). The first
is not hypothetical either: a bug of exactly that
shape hung the development machine a couple of times, and KASAN named it as

```
BUG: KASAN: vmalloc-out-of-bounds in gb_bt656_blank+0x1df
```

That one was the driver's own lifetime bug rather than malicious input - the
assembler kept writing into vb2 planes that had been freed underneath it - but
it is the class of defect to look for here, and `gb-video.c` documents the fix
at the site.

The infrared receiver is a smaller version of the same thing: five bytes per
poll, parsed into an rc-core scancode. Byte 0 must be an ACK before the rest is
read, because the remaining four are stale otherwise, and a NEC command is
checked against its own complement before it is reported. That validates the
format, not the source: whatever the device reports becomes a key press, so a
device that only claims to be a GameBridge - or this one running other
firmware - can press any of the remote's 28 keys, OK and the arrows included.
That is true of any IR receiver; it is stated here because the firmware is
unauthenticated.

## How it has been tested

Four runs, with the real device attached:

- **August 2026 - EHCI, Fedora's stock debug kernel (6.19),** with KASAN,
  lockdep, `DEBUG_ATOMIC_SLEEP`, `DEBUG_OBJECTS` and UBSAN enabled together:
  long soaks of video and audio concurrently, repeated start/stop of streaming
  underneath live audio capture, and USB unbind during both. This is where the
  freed-plane write above was found, along with a shadow-state race that
  panicked the machine, a lock-ordering violation around `usb_clear_halt()`, and
  a probe error path that oopsed whenever probe actually failed. All fixed.
  `v4l2-compliance` passed 80/80.
- **October 2026 - xHCI, RHEL 9's 5.14 debug kernel,** with KASAN, lockdep,
  `PROVE_RCU` and `DEBUG_ATOMIC_SLEEP`, on the code in this release: every stop
  of one stream underneath the other, standard changes under load, unbind and
  physical unplug with both streams open in both close orders, a blocking
  `read()` across an unbind, and slow consumers with two buffers. **No sanitizer
  report in any test.**
- **October 2026 - xHCI, Fedora 44's stock 7.2.7,** after the fix that lets it
  build on 7.x: `v4l2-compliance` 84/84 including its streaming tests; the same
  functional list; and a physical unplug and replug with both streams open, the
  firmware re-uploaded by the driver on replug. Nothing in the kernel log.
- **October 2026 - xHCI, Fedora 44's 7.2.7 debug kernel,** with KASAN, UBSAN,
  lockdep, `PROVE_RCU`, `DEBUG_OBJECTS` and `DEBUG_ATOMIC_SLEEP`: the same
  compliance run and functional list, and a physical unplug and replug with
  both streams open. **No report from any of them**, across the whole boot.

**The stream parser is fuzzed.** `tools/fuzz/` is a libFuzzer harness around
`driver/gb-bt656.c`, the same source the module compiles, under ASan and UBSan:
one hour on 36 workers, 277,022 inputs, no report. Its corpus executes 97.8% of
the parser's lines and evaluates every branch at least once; the three lines it
never reaches are two guards no input can trigger and a callback the driver
always supplies. It covers the parser alone, not the URB handling or locking
around it.

Between the first two runs, an independent review of the driver fixed, among other
lifetime bugs, one defect reachable from ordinary userspace: `VIDIOC_S_STD`
while audio held the stream wrote 138 KB past the end of a videobuf2 plane. It
was never in a public release.

A second review, of the whole repository before it went public, found the same
overflow by another door. `VIDIOC_S_STD` refused a change while buffers existed,
but did not hold the lock that buffer allocation takes, so a second thread could
allocate and queue buffers sized for 480 lines while the standard changed to
PAL, and starting the stream then wrote 576 lines into them. Any user who can
open `/dev/videoN` could reach it. `S_STD` now holds that lock as well, and
buffers are also bounded by their real size at every point the driver writes
into them. `gbtest stdrace` reproduces the race: against the old module it
produced 154 undersized buffers in 30 seconds; against the fixed one, on the
7.2.7 debug kernel, 590 real standard changes interleaved with 307
allocate-and-stream rounds gave none and no sanitizer report, and the full
functional list, `v4l2-compliance` 84/84 and a physical unplug and replug that
followed were clean too. On RHEL 9's 5.14 debug kernel the same race test (608
changes, 330 rounds, none undersized) and the functional list were clean as
well. The same review led to probe checking the command
endpoints' types (a device answering to this ID with mismatched endpoints made
the USB core warn, which is a crash on a `panic_on_warn` system), the pinned
firmware download below, and bounds on a few tools' inputs. None of it was ever
public.

A third review, run after those fixes, found that unplugging the device could
free the video decoder's state underneath an ioctl still running against it.
The V4L2 core checks that a device node is registered once, when an ioctl
starts, and unregistering it does not wait for one already past that check;
the driver's disconnect did not take the lock those ioctls hold. It was
reproduced on the first try on the 7.2.7 debug kernel - KASAN reported a
use-after-free in the decoder driver, followed by an oops - with
`gbtest ioctlloop` keeping such ioctls in flight across an unbind. Disconnect
now holds that lock while it tears the device down. Afterwards: 40 of 40
unbinds with ioctls in flight clean, a physical unplug with ioctls, video and
audio all running clean, and the functional list, `stdrace` and
`v4l2-compliance` 84/84 clean again. Earlier unplug tests had streams open
across the disconnect, which take a different lock, and so never reached it.
Reaching it needs an unplug, or root, while a program has the device open.

A fourth review, of those fixes, found the one path into the driver they did
not cover: closing the device. A program subscribed to control-change events
unlinks that subscription on `close()` through the bridge's control list, and
disconnect had already freed that list. Closing after an unplug locked a
destroyed mutex - a warning on a debug kernel - and closing during one freed
the subscription while the decoder's control still pointed at it, which the
decoder then walked: KASAN reported a use-after-free. Both reproduced with
`gbtest ctrlevent`. `close()` now unlinks its subscriptions under the same
lock as disconnect, and the control list is left empty but alive until the
last file is closed. A fifth review, of that fix alone, found it sound and
suggested holding the lock around the unlink only, so that one stuck close
cannot stall other users; that is how it stands, retested the same way.
Afterwards, on a fresh debug boot: 20 of 20 closes after an unbind and 40 of
40 during one clean, a physical unplug with a subscriber closing at that
moment clean, and the functional list, `stdrace` and `v4l2-compliance` 84/84
clean. The review also confirmed the earlier fixes hold, with lock order and
the kernel behaviour they rely on checked against v6.16, v5.14 and CentOS
Stream 9 sources.

One path remains that this driver cannot close: root can unbind the decoder's
own driver through sysfs (`/sys/bus/i2c/drivers/cx25840/unbind`) while the
device is in use, which frees controls this driver still refers to. That is
a property of the in-kernel cx25840 driver, shared by most bridges that use
it, and it needs root.

A sixth, general review found no new memory-safety bug in the driver, and two
narrow behaviours that are left as they are, deliberately:

- After an unplug, the ALSA core drops its reference on this module one step
  before it frees the sound card, and freeing the card runs this module's
  code. Root running `rmmod gamebridge` in that window would unload code that
  is about to run. snd-usb-audio has the same ordering; a counter for
  `rmmod` to wait on would narrow the window without closing it.
- A `read()` whose copy to userspace never completes (the buffer mapped from
  a FUSE file whose server never answers, say) holds the queue lock, so an
  unplug waits for it, and other users' ioctls wait behind the unplug. That
  is how videobuf2's `read()` works for any driver.

The same review led to the firmware image being written atomically and
checked by `tools/gb-up.sh` before upload, to `gbfw load` no longer falling
back to any bare Cypress FX2 board unless given `--any-fx2`, and to two
documentation examples no longer running a tool as root against a shared
`/tmp` path or without need.

What that does **not** establish: this release has not run on EHCI, and
fuzzing covers the parser alone. A deliberately hostile device has never
been pointed at this driver.

## The firmware is unauthenticated

The device arrives as a bare Cypress EZ-USB FX2LP with its 8051 held in reset,
and will accept any code you hand it over USB into RAM. There is no signature
check in the part and nothing this project can add to it. The loader therefore
uploads whatever image you give it.

Two consequences worth stating plainly:

- Treat the firmware image as you would any executable: it runs on a processor
  attached to your USB bus. The tools here extract it from Adaptec's own driver
  binary, which you fetch yourself. `tools/fetch-firmware.sh` fetches from a
  pinned commit of a public archive and checks every file against a pinned
  SHA-256, including files already on disk; a mismatch stops it.
  `tools/extract-fw.py` then refuses to write an image whose SHA-256 is not the
  pinned one, and `make firmware` checks it again before installing it. The
  kernel's loader can check only the size and first byte, so an image put in
  `/lib/firmware/gamebridge/` by hand is trusted as it is: whoever can write
  there - normally only root - decides what the device runs.
- The load is to **RAM only**, never to the EEPROM.
  The project has no code that writes the EEPROM, so no
  mistake here can brick the hardware.

## Module parameters that can hurt

Several parameters exist only so that a measurement can be reproduced, and some
are known to break things. They all default to off, and their help text says so:

- `i2c_rcount2` - **measured to wedge the device's command endpoint.** Recovery
  needs a USB port power cycle.
- `i2c_fake_tuner_read` - answers two tuner registers from a table instead of
  the bus. It is a diagnostic "feature", and it exists because this bridge 
  cannot read that chip at all.
- `tuner` - unverified. No RF source has been connected to this driver yet.

If you are filing a bug, say which parameters you used. A report from a
non-default configuration is a different report.

## What it does not do

No network access, no setuid binaries, no daemon required for the kernel path,
and no firmware written to persistent storage on the device. The userland tools
need USB access, which means root or a udev rule you install knowingly.

They are bring-up tools, meant to be run by the person who built them. Do not
grant them to other users through sudoers or file capabilities: they write
output files wherever they are told, following symlinks, and the scripts run
binaries from the checkout, so anyone who can write to it gets what those
binaries can do.
