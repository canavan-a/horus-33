#!/usr/bin/env bash
# Run the whole horus-33 stack live, in dev mode, in one terminal — no tmux,
# no systemd, nothing added to configuration.nix. Ctrl-C stops everything.
#
#   ./dev.sh
#
# Each service's stdout/stderr is prefixed so it's clear who said what.
# capture-eye needs capture-eye/dev.json first — see the error below if it's
# missing (deliberately not auto-generated: a guessed camera/format is worse
# than an error that tells you what to write — see capture-eye/docs/config.md).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG="$ROOT/capture-eye/dev.json"

if [[ ! -f "$CONFIG" ]]; then
  cat >&2 <<EOF
!! missing $CONFIG

   Create it first — see capture-eye/docs/config.md for the full schema.
   Minimal example:

     {
       "capture": { "device": "/dev/video0", "width": 1280, "height": 720, "fourcc": "MJPG", "fps": 30 },
       "ingress": { "socket_path": "/tmp/horus-control.sock" }
     }

   Find your camera's real modes with:
     nix develop $ROOT/capture-eye --command $ROOT/capture-eye/build/capture-eye --list-formats --device /dev/video0
EOF
  exit 1
fi

# Always runs, not just when build/ is missing — ninja's incremental build is
# a fast no-op when nothing changed, and the alternative (only building once)
# means every C++ edit needs a manual rebuild before dev.sh picks it up.
echo "› building capture-eye…"
nix develop "$ROOT/capture-eye" --command bash -c \
  "cmake -B '$ROOT/capture-eye/build' -G Ninja -S '$ROOT/capture-eye' && ninja -C '$ROOT/capture-eye/build'" \
  || exit 1

pids=()
cleaned_up=0
cleanup() {
  # INT's default disposition is to exit, which then also fires the EXIT
  # trap — without this guard "stopping…" prints twice for one Ctrl-C.
  [[ "$cleaned_up" == 1 ]] && return
  cleaned_up=1
  echo
  echo "› stopping…"
  for pid in "${pids[@]}"; do
    # Negative PID = kill the whole process group, not just the tracked PID.
    # Without this, only the `run()` subshell dies — its actual child (nix
    # develop, and *its* child mediamtx/capture-eye/go run/vite) is orphaned
    # and keeps running, because a plain `cmd &` in a non-interactive script
    # never gets its own process group to begin with. Confirmed happening:
    # Ctrl-C left every service alive after the script itself exited.
    kill -TERM -- "-$pid" 2>/dev/null
  done
  wait 2>/dev/null
}
trap cleanup INT TERM EXIT

# Runs one command with its output prefixed, backgrounded, and its PID
# tracked so cleanup() can stop it.
#
# The whole job — command, pipe, and sed — runs inside one `setsid`, not just
# the leaf command. $! after `setsid ... &` is setsid's own PID, and setsid
# makes that PID double as the new process group ID for everything it and its
# descendants spawn (nix develop -> mediamtx, in particular) as long as none
# of them call setsid again. That's what makes the negative-PID kill in
# cleanup() reach the whole tree instead of just the immediate child.
run() {
  local name="$1"
  shift
  HORUS_DEV_LABEL="$(printf '%-12s' "$name")" \
    setsid bash -c '"$@" 2>&1 | sed -u "s/^/[$HORUS_DEV_LABEL] /"' _ "$@" &
  pids+=("$!")
}

# Polls until a TCP port accepts connections, instead of a fixed sleep. A
# blind `sleep 1` was flaky in practice: `nix develop`'s own flake-eval
# overhead varies run to run, and capture-eye does not retry a frame sink
# that fails to connect at startup — it exits outright (this is exactly why
# the NixOS module needs Restart=always; dev.sh has no such safety net, so
# the ordering has to actually be right instead of probably right).
wait_for_port() {
  local port="$1" tries=0
  while ! (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; do
    exec 3<&- 2>/dev/null || true
    tries=$((tries + 1))
    if (( tries > 100 )); then
      echo "!! timed out waiting for 127.0.0.1:$port" >&2
      return 1
    fi
    sleep 0.1
  done
  exec 3<&- 2>/dev/null || true
}

run mediamtx  nix develop "$ROOT/capture-eye" --command mediamtx "$ROOT/capture-eye/mediamtx.yml"
wait_for_port 8554 # MediaMTX's RTSP listener — capture-eye's publish target

run capture-eye nix develop "$ROOT/capture-eye" --command \
  "$ROOT/capture-eye/build/capture-eye" --config "$CONFIG"

run horus-server nix develop "$ROOT" --command bash -c \
  "cd '$ROOT/server' && go run ./cmd/horus-server --socket /tmp/horus-control.sock --listen :8090 \
    --clips-dir '$ROOT/capture-eye/dev-clips' --clip-admin-socket /tmp/horus-clip-admin.sock"

run web nix develop "$ROOT" --command bash -c \
  "cd '$ROOT/web' && [ -d node_modules ] || npm install; npm run dev -- --host 0.0.0.0 --port 5173"

# --host 0.0.0.0 above puts the vite dev server on every interface, not just
# loopback, so another device on the LAN can smoke-test against it — the
# proxy targets in vite.config.ts stay 127.0.0.1 regardless, since those run
# server-side against horus-server/MediaMTX on this same machine.
lan_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
# horusctl edits /etc/horus/capture-eye.json by default — the deployed path.
# Pointing it at the dev config makes `horusctl config …` work here too.
echo "› horusctl against this run: export HORUS_CONFIG=$CONFIG"
echo "› all services starting — web UI at http://localhost:5173${lan_ip:+ and http://$lan_ip:5173} (Ctrl-C to stop everything)"
wait
