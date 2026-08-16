#!/usr/bin/env bash
# Unconditionally stops every process dev.sh can start, by matching the exact
# commands it launches — a backstop for when Ctrl-C doesn't reach dev.sh (a
# backgrounded/nohup'd dev.sh has no controlling terminal to deliver it to)
# or its trap didn't run for any other reason. Safe to run any time, even if
# nothing is up — every step is a no-op on a process that isn't there.
set -uo pipefail

echo "› stopping horus-33 dev processes…"

pkill -f "capture-eye/build/capture-eye --config" 2>/dev/null
pkill -f "mediamtx.*capture-eye/mediamtx.yml" 2>/dev/null
pkill -f "cmd/horus-server" 2>/dev/null
pkill -f "web/node_modules/.bin/vite" 2>/dev/null
pkill -f "bash \./dev\.sh$" 2>/dev/null
pkill -f "bash -c .*HORUS_DEV_LABEL" 2>/dev/null # the setsid wrapper run() launches

sleep 1

rm -f /tmp/horus-control.sock

echo "› checking nothing is left…"
remaining=$(ps aux | grep -E "capture-eye/build/capture-eye|mediamtx.*capture-eye|cmd/horus-server|node_modules/.bin/vite" | grep -v grep || true)
if [[ -n "$remaining" ]]; then
  echo "!! still running (kill -9 by hand if this persists):"
  echo "$remaining"
  exit 1
fi
echo "› all clear"
