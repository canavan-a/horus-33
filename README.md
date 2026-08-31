# horus-33

A camera gimbal that detects and tracks a person: capture → inference →
serial control loop, with a web UI and a terminal UI for driving it directly.

```
                          ┌───────────────┐
   /dev/video0  ────────► │  capture-eye  │  ONNX/OpenVINO person detection,
                          │    (C++23)    │  H.264 encode, RTSP publish
                          └───┬───────┬───┘
                              │       │ track / control (Unix socket, JSON)
                    RTSP      │       │
                (via MediaMTX)│       ▼
                              │  ┌────────────┐   USB CDC serial (JSON)   ┌──────────┐
                              │  │ horus-server│ ────────────────────────►│ firmware │
                              │  │    (Go)     │◄────────────────────────│ (ESP32-S3)│
                              │  └──┬───────┬──┘                          └──────────┘
                              │     │ REST/WS      Unix socket (JSON)
                              ▼     ▼              │
                          ┌───────────┐      ┌────────────┐
                          │  web UI   │      │ horusctl   │
                          │ (React)   │      │ (TUI, Go)  │
                          │ + mobile  │      └────────────┘
                          │ (Android) │
                          └───────────┘
```

- **[`capture-eye/`](capture-eye)** — C++23 pipeline: reads V4L2 frames, runs
  person detection (ONNX Runtime, optional OpenVINO backend), encodes/publishes
  H.264 over RTSP (via [MediaMTX](https://github.com/bluenviron/mediamtx)),
  writes clips, and relays device control messages over a Unix socket. See
  [`capture-eye/docs/config.md`](capture-eye/docs/config.md) for its config
  schema.
- **[`server/`](server)** — Go REST + WebSocket API (`horus-server`). Never
  touches the serial port itself; it dials the Unix control-relay socket that
  capture-eye exposes, and optionally serves clip listings and control-value
  replay-on-reconnect.
- **[`web/`](web)** — React + TypeScript UI: live video, per-control panels,
  clip browsing.
- **[`tui-controller/`](tui-controller)** — `horusctl`, a terminal UI for
  driving the device directly (real serial link or a fake one for local
  development), plus the `config` and `service` subcommands that manage a
  deployed host (see [Operating a deployed host](#operating-a-deployed-host)).
- **[`firmware/`](firmware)** — ESP32-S3 (Arduino framework, PlatformIO)
  firmware for the gimbal itself: motor/LED controls, the device side of the
  wire protocol.
- **[`mobile/`](mobile)** — sideloaded Android client (React Native, direct
  APK, no Play Store). A deliberately smaller subset of `web/`: jog pad, WHEP
  video stream, clip list/playback, and a foreground-service WebSocket that
  raises local "person in frame" notifications in the background. See
  [`mobile/README.md`](mobile/README.md).
- **[`docs/protocol.md`](docs/protocol.md)** — the host↔device control
  protocol: newline-delimited JSON over USB CDC serial. Devices advertise
  their own controls (a descriptor), so the host renders whatever it's told
  about — no compiled-in knowledge of specific controls needed.

## Running it

`./dev.sh` runs the whole stack (MediaMTX, capture-eye, horus-server, web) in
one terminal, each service's output prefixed by name, `Ctrl-C` tears
everything down. It needs `capture-eye/dev.json` first — the script prints a
minimal example and points at
[`capture-eye/docs/config.md`](capture-eye/docs/config.md) if that file is
missing (deliberately not auto-generated: a guessed camera/format is worse
than an explicit error).

```
./dev.sh
```

Web UI is then at `http://localhost:5173`. `./dev-stop.sh` cleans up any
stragglers if a previous run didn't shut down cleanly.

## Operating a deployed host

On a host running the NixOS module, `horusctl` is on every user's PATH and
edits the same config file the `horus-capture-eye` unit reads. Retuning the
camera or moving the serial port is a one-liner — no Nix rebuild, no commit:

```
horusctl config devices                      # what is attached right now
horusctl config set-video --device /dev/video2 --size 1280x720 --fourcc MJPG
horusctl config set-serial --port /dev/serial/by-id/usb-Espressif_...-if00
horusctl config show
sudo horusctl service restart                # camera + serial bind at startup
```

Every write is checked with `capture-eye --check-config` before it replaces
the file, so a bad edit is refused and the running config is left alone. Add
`--restart` to a `config` command to write and restart in one step, and use
`horusctl service status` / `logs -f` to see the result — `status` decodes
capture-eye's exit codes (10 camera, 11 serial, 2 config) inline.

For this to work, `services.horus.configFile` must be a mutable path (its
default, `/etc/horus/capture-eye.json`) rather than a Nix store path; the
module asserts as much. To ship an initial config from Nix, set
`services.horus.seedConfigFile` — it is copied into place on first activation
and never overwrites later edits.

## Building

Everything but the firmware is covered by the repo's `flake.nix`:

```
nix develop        # Go, Node, and everything capture-eye needs (C++ toolchain, OpenCV, ONNX Runtime, ...)
nix build           # builds capture-eye (the only piece packaged as a real Nix derivation)
```

`capture-eye/flake.nix` is the narrower, faster shell to use when only working
on the C++ side.

`server/` and `tui-controller/` are plain Go modules (`go build ./...` /
`go run ./cmd/...`), not packaged as Nix derivations — see the comment in the
top-level `flake.nix` for why. `web/` is a standard Vite/npm project.

`mobile/` is a bare React Native project — `npm install` then `npm run android`
for a debug build, or Gradle `assembleRelease` for a signed APK. See
[`mobile/README.md`](mobile/README.md) for the commands and keystore setup.

Firmware is built and flashed with [PlatformIO](https://platformio.org/)
from `firmware/`:

```
pio run
pio run --target upload
pio device monitor
```

## Deployment

[`nix/module.nix`](nix/module.nix) is a NixOS module wiring capture-eye and
horus-server up as system services (with `Restart=always`, unlike `dev.sh`).

Mobile release APKs are built and published locally — see
[`mobile/README.md`](mobile/README.md#release-apk).

## License

MIT — see [`LICENSE`](LICENSE).
