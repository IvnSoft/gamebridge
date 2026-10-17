#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Live preview from the GameBridge.
#
# Raw UYVY has no container, so the player must be told the geometry and pixel
# format explicitly -- both ffplay and mpv show "failed to recognise format"
# without it. gbcap sends every diagnostic to stderr so stdout carries nothing
# but frame data.
#
# Usage: gbplay.sh [seconds] [ffplay|mpv]
#   GB_PAL=1          625/50 instead of 525/60
#   GB_PROGRESSIVE=1  one field per frame: half the latency, half the lines
#   GB_SESSION=path   a different bring-up script
#   GB_NOAUDIO=1      picture only
#   GB_AUDIO_DEV=...   ALSA device for sound (default plug:default)
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-30}"
PLAYER="${2:-ffplay}"
SESSION="${GB_SESSION:-${DIR}/captures/full-session-replay.txt}"

# Default: WEAVE two consecutive field runs into a 480-line frame at 30 fps.
#
# The obvious alternative -- emit each field as its own 240-line frame at 60 fps
# -- looks correct in a still and shakes vertically in motion, because the two
# fields of an NTSC frame sit half a line apart in the raster. Measured: 0.82
# lines of offset between alternate frames, which reads as a picture that will
# not quite hold still, like a mistuned TV.
#
# Weaving also does the right thing for a true 240p source such as an 8-bit
# computer, where both runs carry the same lines and the weave line-doubles them.
#
# GB_PROGRESSIVE=1 selects the 240p/60 fps path instead: half the latency and
# half the vertical resolution, and it will shake on an interlaced source.
if [ -n "${GB_PROGRESSIVE:-}" ]; then
  MODE="--progressive"; H=240; FPS=60
else
  MODE=""; H=480; FPS=30
fi

# GB_PAL=1 selects 625/50. This is more than a geometry change: gbcap also has
# to write 0x0400 = 0x04 to the CX25840, which selects PAL-BDGHI *and* clears
# ACFG_DIS so the chip configures its own active window. The captured Windows
# session sets ACFG_DIS, which pins the window to NTSC's 244 lines -- that is
# why early PAL attempts locked horizontally and still looked wrong.
#
# The decoder latches the first standard it detects and holds it until power is
# cycled, so if the picture stays NTSC-shaped, replug the device and try again.
if [ -n "${GB_PAL:-}" ]; then
  MODE="${MODE} --pal"
  if [ -z "${GB_PROGRESSIVE:-}" ]; then H=576; FPS=25; else H=288; FPS=50; fi
fi
W=720
# NTSC pixels are not square and a 240-line field is half height, so showing the
# raw geometry 1:1 gives a picture that is twice as wide as it should be. Display
# at 4:3 (640x480) regardless of the capture geometry.
DISP="640:480"

# Sound. The audio rides in the video stream's horizontal blanking as 16-bit
# big-endian PCM at 48 kHz stereo. gbcap passes it through untouched; getting it
# to a speaker took two corrections, both recorded at the aplay call below.
#
# It goes down a FIFO rather than a second pipe because stdout already carries
# the frames. aplay has to be reading before gbcap opens the FIFO: an O_WRONLY
# open of a FIFO with no reader gives ENXIO, and gbcap treats that as "carry on
# without sound" rather than as a failure -- so a dead aplay presents as silence
# with a puzzling message, which is why its liveness is checked here instead.
#
# GB_NOAUDIO=1 turns it off.
AUDIO_ARG=""
if [ -z "${GB_NOAUDIO:-}" ] && command -v aplay >/dev/null 2>&1; then
  # The FIFO goes in a private directory, NOT straight into /tmp.
  # fs.protected_fifos is 1 or 2 on most current distributions, which forbids a
  # process from opening a FIFO it does not own inside a world-writable sticky
  # directory. gbcap runs under sudo, so root opening user's FIFO in /tmp gets
  # EACCES -- "Permission denied", from root, on a mode-0666 file. A 0700
  # directory from mktemp -d is not sticky and not world-writable, so the rule
  # does not apply.
  AUDIO_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gbaudio.XXXXXX")"
  AUDIO_FIFO="${AUDIO_DIR}/pcm"
  mkfifo -m 0666 "${AUDIO_FIFO}"
  # -t raw, because without it aplay tries to parse a WAV header, fails on the
  # first bytes of PCM and exits -- taking the FIFO's only reader with it, so
  # gbcap's open then gets ENXIO and reports no sound.
  #
  # -D plug:default, because the bare `default` device on this machine refuses
  # S16_BE outright ("Sample format non available. Available formats: - U8").
  # `plug` is ALSA's conversion plugin and is always present; it takes the
  # stream's native big-endian and converts if the hardware wants otherwise.
  # GB_AUDIO_DEV overrides it.
  #
  # -B is the buffer in microseconds: keep it short, this is a latency-sensitive
  # capture and a deep audio buffer is a deep A/V offset.
  aplay -q -t raw -D "${GB_AUDIO_DEV:-plug:default}" \
        -f S16_BE -r 48000 -c 2 -B 80000 "${AUDIO_FIFO}" &
  APLAY_PID=$!
  trap 'kill "${APLAY_PID}" 2>/dev/null || true; rm -rf "${AUDIO_DIR}"' EXIT INT TERM
  sleep 0.4                      # let aplay open the read end first
  if kill -0 "${APLAY_PID}" 2>/dev/null; then
    AUDIO_ARG="--audio ${AUDIO_FIFO}"
  else
    # Diagnose here rather than let gbcap report a bare ENXIO, which says
    # nothing about which of the two processes actually failed.
    echo "note: aplay exited at once -- no sound. Try GB_AUDIO_DEV=plughw:0,0" >&2
  fi
else
  [ -n "${GB_NOAUDIO:-}" ] || echo "note: aplay not found, no sound" >&2
fi

case "${PLAYER}" in
  ffplay)
    sudo "${DIR}/tools/gbcap" session "${SESSION}" "${SECS}" ${MODE} ${AUDIO_ARG} --frames - \
      | ffplay -hide_banner -loglevel warning \
               -f rawvideo -pixel_format uyvy422 \
               -video_size ${W}x${H} -framerate ${FPS} \
               -fflags nobuffer -flags low_delay -framedrop \
               -vf "scale=${DISP}" -
    ;;
  mpv)
    sudo "${DIR}/tools/gbcap" session "${SESSION}" "${SECS}" ${MODE} ${AUDIO_ARG} --frames - \
      | mpv --demuxer=rawvideo \
            --demuxer-rawvideo-w=${W} --demuxer-rawvideo-h=${H} \
            --demuxer-rawvideo-mp-format=uyvy422 \
            --demuxer-rawvideo-fps=${FPS} \
            --untimed --profile=low-latency --no-cache \
            --video-aspect-override=4:3 -
    ;;
  *)
    echo "usage: $0 [seconds] [ffplay|mpv]" >&2; exit 64 ;;
esac
