#!/usr/bin/env bash
# Arducam USB camera (/dev/video2 today) at its MJPG maximum: 1280x720 @30fps.
# YUYV is not an option here — it drops to 10fps at this size.
#
#   ./run-arducam.sh                 # tracking only, no video out
#   ./run-arducam.sh --preview       # local window
#   ./run-arducam.sh --stream        # browser at http://127.0.0.1:8889/eye
#
# Any other capture-eye flag passes straight through.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

readonly rtsp_url="rtsp://127.0.0.1:8554/eye"
readonly watch_url="http://127.0.0.1:8889/eye"

# The by-id path survives replug and enumeration order; /dev/videoN does not,
# and with two cameras attached the numbering genuinely can swap between boots.
device="/dev/v4l/by-id/usb-Arducam_Technology_Co.__Ltd._USB_Camera_SN0001-video-index0"
[[ -e $device ]] || device=/dev/video2
if [[ ! -e $device ]]; then
  echo "run-arducam: no Arducam found (tried by-id and /dev/video2)" >&2
  echo "             plugged in? check: ls /dev/v4l/by-id/" >&2
  exit 1
fi

# --stream is ours, not capture-eye's: it means "publish, and bring up the
# server if it isn't already there". Everything else is forwarded untouched.
stream=0
args=()
for arg in "$@"; do
  if [[ $arg == --stream ]]; then stream=1; else args+=("$arg"); fi
done

listening() { (exec 3<>/dev/tcp/127.0.0.1/8554) 2>/dev/null; }

mediamtx_pid=""
cleanup() {
  # Only ever kill a server this script started. One already running belongs to
  # someone else's terminal and is not ours to take down.
  [[ -n $mediamtx_pid ]] && kill "$mediamtx_pid" 2>/dev/null || true
}
trap cleanup EXIT

if (( stream )); then
  if listening; then
    echo "run-arducam: using the MediaMTX already on :8554"
  else
    echo "run-arducam: starting MediaMTX"
    mediamtx mediamtx.yml >/tmp/mediamtx-run-arducam.log 2>&1 &
    mediamtx_pid=$!

    # Publishing before the listener is up is the "connection refused" trap.
    for _ in {1..50}; do
      listening && break
      kill -0 "$mediamtx_pid" 2>/dev/null || {
        echo "run-arducam: MediaMTX exited at startup; see /tmp/mediamtx-run-arducam.log" >&2
        exit 1
      }
      sleep 0.1
    done
    listening || { echo "run-arducam: MediaMTX never opened :8554" >&2; exit 1; }
  fi

  args+=(--rtsp "$rtsp_url")
  echo "run-arducam: watch it at $watch_url"
fi

./build/capture-eye \
  --device "$device" \
  --size 1280x720 \
  --fourcc MJPG \
  --fps 30 \
  "${args[@]}"
