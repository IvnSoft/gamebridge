#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Bring the GameBridge up as an ordinary system camera and microphone.
#
# gbplay.sh puts a picture on *this* screen. This puts the device on the system,
# so anything that opens a /dev/videoN capture node or an ALSA capture device
# sees it: a browser, OBS, Zoom, ffmpeg, GStreamer.
#
#     gbcap --frames -  ->  gbloop  ->  v4l2loopback  ->  /dev/video9
#     gbcap --audio FIFO ->  aplay   ->  snd-aloop     ->  hw:Loopback,1,1
#
# Both halves come off ONE isochronous endpoint: the audio rides in the video
# stream's horizontal blanking, so there is no second capture to start and no
# way to have sound without video. That is a property of the hardware, not a
# limitation of this script.
#
# Usage: gb-up.sh [seconds]
#   GB_PAL=1           625/50 instead of 525/60
#   GB_PROGRESSIVE=1   one field per frame: 240p at 60 fps, half the latency
#   GB_NODE=/dev/videoN  which loopback node to feed   (default /dev/video9)
#   GB_ALSA_DEV=hw:...   which loopback PCM to feed    (default hw:Loopback,0,1)
#   GB_NOAUDIO=1       picture only
#   GB_SESSION=path    a different bring-up script
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-3600}"
NODE="${GB_NODE:-/dev/video9}"
SESSION="${GB_SESSION:-${DIR}/captures/full-session-replay.txt}"
LABEL="GameBridge AVC-1410"

say() { printf 'gb-up: %s\n' "$*" >&2; }
die() { printf 'gb-up: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- the device ---
#
# The FX2 firmware load is volatile: it lives in internal RAM and any replug, or
# a suspend that cuts VBUS, drops the device back to the loader PID with an empty
# 8051. So the first thing to establish is *which* device is on the bus, because
# 03f3:009b and 03f3:009c are the same box in two different states and only one
# of them can capture anything.
if lsusb -d 03f3:009c >/dev/null 2>&1; then
  say "device is awake at 03f3:009c"
elif lsusb -d 03f3:009b >/dev/null 2>&1; then
  FW="${DIR}/firmware/gb-loader-fw.bin"
  [ -f "${FW}" ] || die "device is at 03f3:009b and ${FW} is missing.
       Run tools/fetch-firmware.sh, then
       python3 tools/extract-fw.py firmware/avcgbfl.sys ${FW}
       (Adaptec blobs are never committed -- see NOTICE.)"
  # The pinned image only: gbfw itself checks just size and first byte.
  FW_SHA=39845e82eb7691dddcea1e0ec55c53881dba200437fb0e369e53439d5260b22f
  [ "$(sha256sum "${FW}" | cut -d' ' -f1)" = "${FW_SHA}" ] \
    || die "${FW} is not the pinned firmware image; re-run tools/extract-fw.py"
  say "device is at 03f3:009b (loader) -- uploading firmware"
  sudo "${DIR}/tools/gbfw" load "${FW}" >&2
  # It re-numerates itself, which takes a moment and changes its bus address.
  for _ in $(seq 1 20); do
    sleep 0.25
    lsusb -d 03f3:009c >/dev/null 2>&1 && break
  done
  lsusb -d 03f3:009c >/dev/null 2>&1 \
    || die "firmware uploaded but the device never came back as 03f3:009c"
  say "device re-enumerated as 03f3:009c"
else
  die "no GameBridge on the bus (looked for 03f3:009b and 03f3:009c)"
fi

# ------------------------------------------------------------ geometry -------
#
# The stream carries no framing to resynchronise from, so gbcap and gbloop must
# agree on the frame size exactly. A mismatch does not error: the picture simply
# shears and never recovers. Deriving both from one place here is the whole
# reason this script exists rather than a documented two-line pipeline.
if [ -n "${GB_PROGRESSIVE:-}" ]; then MODE="--progressive"; H=240; FPS=60
else                                  MODE="";              H=480; FPS=30
fi
if [ -n "${GB_PAL:-}" ]; then
  MODE="${MODE} --pal"
  if [ -n "${GB_PROGRESSIVE:-}" ]; then H=288; FPS=50; else H=576; FPS=25; fi
fi
FIELD="none"; [ -z "${GB_PROGRESSIVE:-}" ] && FIELD="interlaced"

# ------------------------------------------------------- the loopback node ---
#
# v4l2loopback is usually already loaded and already has a node -- OBS creates
# one the first time its virtual camera is started. Taking that node over would
# break OBS, so create our own and address it by path.
#
# v4l2loopback-ctl adds a device to the LOADED module. modprobe with parameters
# would be ignored here, because the module's parameters are only read when it
# is first inserted.
if [ ! -e "${NODE}" ]; then
  say "creating ${NODE}"
  command -v v4l2loopback-ctl >/dev/null 2>&1 \
    || die "v4l2loopback-ctl not found -- dnf install v4l2loopback"
  sudo modprobe v4l2loopback 2>/dev/null || true
  # -x 1 is exclusive_caps: the node advertises OUTPUT while unclaimed and flips
  # to capture-only once a producer opens it. Chrome, Firefox and several
  # GStreamer applications ignore a node that claims both at once, because no
  # real webcam is both. The visible consequence is that the camera does not
  # appear in a browser's device list until this script is running -- correct
  # behaviour, not a fault.
  #
  # -b 2 keeps two buffers rather than the default: fewer queued frames, less
  # latency. This hardware exists to put a console on a monitor.
  sudo v4l2loopback-ctl add -x 1 -b 2 -n "${LABEL}" "${NODE}"
  # The node is created through sysfs, which does not re-run udev, so it lands
  # root:root 0600 and the invoking user cannot open it. Nudge udev rather than
  # chmod, so the result matches what a normal boot would produce.
  sudo udevadm trigger --subsystem-match=video4linux --action=add
  for _ in $(seq 1 20); do [ -w "${NODE}" ] && break; sleep 0.1; done
fi
[ -w "${NODE}" ] || die "${NODE} exists but is not writable by $(whoami).
       Check 'getfacl ${NODE}'; a desktop session normally grants access via
       systemd's uaccess ACL, otherwise: sudo usermod -aG video \$USER"

# -------------------------------------------------------------- the sound ---
AUDIO_ARG=""
if [ -z "${GB_NOAUDIO:-}" ]; then
  sudo modprobe snd-aloop 2>/dev/null || true

  # Subdevice 1, not 0. PipeWire claims hw:Loopback,0,0 the moment the card
  # appears and holds it, so a producer opening subdevice 0 gets EBUSY -- and a
  # consumer that opens the capture side first sees PipeWire's negotiated format
  # (S32_LE) rather than ours, which reads as "the device cannot do S16_BE".
  # Neither message points at PipeWire. Use a subdevice PipeWire has not taken.
  ALSA_DEV="${GB_ALSA_DEV:-hw:Loopback,0,1}"

  if command -v aplay >/dev/null 2>&1 && [ -e /proc/asound/Loopback ]; then
    # The FIFO goes in a private 0700 directory, never straight into /tmp:
    # fs.protected_fifos forbids a process opening a FIFO it does not own inside
    # a world-writable sticky directory, and gbcap runs under sudo -- so root
    # opening the invoking user's FIFO in /tmp gets EACCES. A mktemp -d
    # directory is neither world-writable nor sticky, so the rule does not apply.
    AUDIO_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gbaudio.XXXXXX")"
    AUDIO_FIFO="${AUDIO_DIR}/pcm"
    mkfifo -m 0666 "${AUDIO_FIFO}"
    # -t raw: without it aplay tries to parse a WAV header, fails on the first
    # bytes of PCM and exits -- taking the FIFO's only reader with it, after
    # which gbcap's O_WRONLY open gets ENXIO and reports "no such device",
    # which points at entirely the wrong process.
    #
    # No `plug` here, deliberately: snd-aloop takes S16_BE natively, and going
    # through the conversion plugin would hide a format mismatch we want to see.
    aplay -q -t raw -D "${ALSA_DEV}" -f S16_BE -r 48000 -c 2 -B 80000 \
          "${AUDIO_FIFO}" &
    APLAY_PID=$!
    trap 'kill "${APLAY_PID}" 2>/dev/null || true; rm -rf "${AUDIO_DIR}"' \
         EXIT INT TERM
    sleep 0.4                    # let aplay open the read end first
    if kill -0 "${APLAY_PID}" 2>/dev/null; then
      AUDIO_ARG="--audio ${AUDIO_FIFO}"
      say "sound -> ${ALSA_DEV}   (consumers read hw:Loopback,1,1)"
    else
      say "aplay exited at once -- continuing without sound."
      say "  try a different subdevice: GB_ALSA_DEV=hw:Loopback,0,2"
    fi
  else
    say "no aplay or no snd-aloop card -- continuing without sound"
  fi
fi

# ----------------------------------------------------------------- go -------
say "${NODE} [${LABEL}] 720x${H} UYVY ${FIELD}, ${FPS} fps"
say "  watch it:  ffplay -f v4l2 -i ${NODE}"
# -t raw on BOTH ends. arecord writes a WAV header by default and WAV cannot
# express big-endian samples at all -- "Wave doesn't support S16_BE format" --
# so it dies before a byte is recorded, and aplay then fails reading a header
# that was never written. Two errors, neither naming the cause.
[ -n "${AUDIO_ARG}" ] && \
  say "  hear it:   arecord -D hw:Loopback,1,1 -t raw -f S16_BE -r 48000 -c 2 \\
gb-up:               | aplay -t raw -f S16_BE -r 48000 -c 2 -D plug:default -"
say "  Ctrl-C to stop."

# gbcap needs root for raw USB; gbloop needs only write access to the node, so
# it deliberately stays unprivileged on the other side of the pipe.
sudo "${DIR}/tools/gbcap" session "${SESSION}" "${SECS}" \
     ${MODE} ${AUDIO_ARG} --frames - \
  | "${DIR}/tools/gbloop" --width 720 --height "${H}" --field "${FIELD}" "${NODE}"
