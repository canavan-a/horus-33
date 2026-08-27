# capture-eye config file

`--config PATH` points at a JSON file. Precedence is:

```
built-in defaults  <  --config FILE  <  CLI flags
```

A key omitted from the file keeps its built-in default (or the flag value, if
one was also given). An unrecognized key at any level, or a value of the wrong
JSON type, is a hard error — nothing is ever silently ignored or coerced.

Every key here has an equivalent CLI flag (`capture-eye --help`); the file is
just a way to avoid retyping a long invocation.

## Example

```json
{
  "capture": {
    "device": "/dev/v4l/by-id/usb-<vendor>_<model>_<serial>-video-index0",
    "width": 1280,
    "height": 720,
    "fourcc": "MJPG",
    "fps": 30
  },
  "serial": {
    "port": "/dev/ttyACM0"
  },
  "sink": {
    "rtsp_url": "rtsp://127.0.0.1:8554/eye"
  }
}
```

## Schema

### `capture`

| key | type | flag |
|---|---|---|
| `device` | string | `--device` |
| `width`, `height` | integer | `--size WxH` |
| `fourcc` | string, exactly 4 chars | `--fourcc` |
| `fps` | integer | `--fps` |
| `buffer_count` | integer | — |
| `decode_scale` | integer, 1/2/4 | `--decode-scale` |
| `strict_format` | bool | `--loose-format` (sets `false`) |
| `flip_horizontal` | bool | `--flip-h` |
| `flip_vertical` | bool | `--flip-v` |

`flip_horizontal`/`flip_vertical` correct a camera's physical mount orientation (e.g.
mounted upside-down). Applied once, right after decode, before inference, overlay, or any
sink sees the frame — the whole pipeline downstream just sees corrected pixels. This is
config-only by design, not exposed through the control relay or REST API: mid-run flips
would need every in-flight detection's coordinates reinterpreted, for a correction that
only ever needs to be set once for a given physical mount.

### `model`

| key | type | flag |
|---|---|---|
| `variant` | string | `--model-variant` |
| `url` | string | `--model-url` |
| `sha256` | string | `--model-sha` |
| `path` | string | `--model` |
| `offline` | bool | `--offline` |
| `allow_unpinned` | bool | `--allow-unpinned` |

### `inference`

| key | type | flag |
|---|---|---|
| `backend` | string: `onnx`\|`openvino` | `--backend` |
| `input_size` | integer | — |
| `conf_threshold` | number, [0,1] | `--conf` |
| `intra_op_threads` | integer, ≥1 | `--intra-threads` |
| `inter_op_threads` | integer, ≥1 | — |
| `fake` | bool | `--fake-detector` |

Both backends run the same preprocessing and postprocessing and sit behind the
same detector seam, so `backend` is the only thing that changes between them.
`openvino` needs a binary configured with `-DCAPTURE_EYE_OPENVINO=ON` (the
`capture-eye-openvino` Nix package) and an explicit `model.path` pointing at an
IR `.xml` — the model store only ever downloads `.onnx`. Asking for it anywhere
else is a config error, never a silent fall back to ONNX.

### `serial`

| key | type | flag |
|---|---|---|
| `port` | string | `--serial` |
| `baud` | integer | — |
| `enabled` | bool | `--no-serial` (sets `false`) |
| `max_hz` | integer | — |
| `lost_repeat_hz` | integer | — |
| `send_seq` | bool | `--track-seq` |

### `tracking`

| key | type | flag |
|---|---|---|
| `policy` | string: `sticky`\|`largest`\|`confident`\|`closest` | `--policy` |
| `lock_iou` | number, [0,1] | — |
| `lost_grace_ms` | integer | — |

### `sink`

| key | type | flag |
|---|---|---|
| `preview` | bool | `--preview` |
| `snapshot_path` | string | `--snapshot` |
| `snapshot_every` | integer | — |
| `rtsp_url` | string | `--rtsp` |
| `bitrate_kbps` | integer, >0 | `--bitrate` |
| `hardware_encode` | bool | `--hw-encode` / `--no-hw-encode` |
| `vaapi_device` | string | `--vaapi-device` |

Hardware (VAAPI) encode is opt-in: `hardware_encode` defaults to `false`, since
not every machine has a usable render node. When it is turned on and the device
or driver is unavailable, capture-eye logs the reason and falls back to libx264.

### `ingress`

Lets another process (horusctl, a future REST server) send `describe`/`set`/
`ping` to the device through capture-eye, since only one process may hold the
serial port at a time. Off unless `socket_path` is set.

| key | type | flag |
|---|---|---|
| `socket_path` | string | `--control-socket` |
| `max_clients` | integer, ≥1 | `--control-max-clients` |

### `clipping`

Records a clip of anyone the tracker sees, straight to disk as it happens —
one frame is encoded and written per video frame, so memory use is flat
regardless of clip length; nothing about a clip is ever buffered whole in
memory. Off unless `enabled` is `true`, and `output_dir` is required once it
is. `admin_socket_path` is a second, separate, host-only Unix socket (not the
`ingress` relay above, which is a dumb ESP32-only pipe) that lets a process
like horus-server flip `enabled` live and poll status without a restart; leave
it empty if you only ever want config-file control.

`stop_after_ticks` counts consecutive **inference ticks** with nobody in
frame, not video frames and not milliseconds — capture and inference run at
different, drifting rates, so a tick count is the only thing that stays
meaningful as that drift changes.

`pre_roll_seconds` keeps a rolling in-memory buffer of that many seconds of
recent video (preallocated once, at `pre_roll_seconds * capture fps` frames),
so a clip includes a moment just before someone was first detected rather
than starting exactly on the detection instant. This is the one part of
clipping that does cost memory proportional to a setting: at 1s/30fps/720p
that's roughly 83MB; keep it modest.

Every finished clip also gets a `.jpg` thumbnail written alongside it (same
basename, e.g. `clip-123-1.mp4` + `clip-123-1.jpg`), taken from the middle of
the clip rather than the first or last frame. No config key — this always
happens once a clip finishes, and a failure to produce one (corrupt re-read,
disk full) is logged and otherwise ignored; it never affects recording itself.

| key | type | flag |
|---|---|---|
| `enabled` | bool | `--clip` (sets `true`) |
| `output_dir` | string, required if `enabled` | `--clip-dir` |
| `admin_socket_path` | string | `--clip-admin-socket` |
| `pre_roll_seconds` | number, [0.1, 10] | `--clip-preroll` |
| `stop_after_ticks` | integer, ≥1 | `--clip-stop-ticks` |
| `bitrate_kbps` | integer, >0 | — |
| `hardware_encode` | bool | — |
| `vaapi_device` | string | — |

## Errors

A bad config file fails loudly rather than silently degrading:

```
$ capture-eye --config bad.json --list-formats
capture-eye: invalid configuration: capture: unknown key 'fsp'

$ capture-eye --config missing.json --list-formats
capture-eye: invalid configuration: --config: cannot open 'missing.json'
```

Range checks (positive fps, `conf_threshold` in [0,1], etc.) run *after* the
file and flags are merged, so a bad value from the file is rejected exactly
like a bad flag.

## Exit codes

Distinct process exit codes for the failures worth telling apart at a glance
from `systemctl status`/`journalctl`, without reading the log message text —
useful under `Restart=always`, where the process keeps getting relaunched and
you want to know *why* without hunting through the journal every time.

| Code | Meaning |
|---|---|
| 0 | clean exit (`--help`, `--list-formats`, etc.) |
| 1 | other failure — decode, inference, model, sink |
| 2 | invalid configuration (bad flag or config file) |
| 10 | camera: not found, rejected format, or streaming failed |
| 11 | serial: ESP32 not found or link failed to open |

## Checking a config without starting

```
capture-eye --check-config --config /etc/horus/capture-eye.json
```

Loads the file, merges it under any flags given, validates the result, and
exits — 0 if the config is usable, 2 with the offending key on stderr if it is
not. It opens no camera, no serial port and no model, so it is safe to run as
any user while the service is running.

This is what `horusctl config` uses to check an edit before installing it: the
candidate file is written next to the real one, checked with this command, and
only then renamed into place. A rejected edit leaves the live config untouched.

## Editing a deployed config

On a host running the NixOS module, prefer `horusctl` over editing this file by
hand — it validates before it writes and restarts the unit for you:

```
horusctl config show
horusctl config set-video --device /dev/video2 --size 1280x720 --fourcc MJPG
horusctl config set-serial --port /dev/serial/by-id/usb-Espressif_...-if00
horusctl config devices          # what is actually attached right now
horusctl service restart         # camera and serial are negotiated at startup
```

`horusctl config edit` opens the file in `$EDITOR` and validates on the way
back out, for changes the typed flags above do not cover.
