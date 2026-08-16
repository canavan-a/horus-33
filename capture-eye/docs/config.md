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
| `input_size` | integer | — |
| `conf_threshold` | number, [0,1] | `--conf` |
| `intra_op_threads` | integer, ≥1 | `--intra-threads` |
| `inter_op_threads` | integer, ≥1 | — |
| `fake` | bool | `--fake-detector` |

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
| `hardware_encode` | bool | `--no-hw-encode` (sets `false`) |
| `vaapi_device` | string | `--vaapi-device` |

### `ingress`

Lets another process (horusctl, a future REST server) send `describe`/`set`/
`ping` to the device through capture-eye, since only one process may hold the
serial port at a time. Off unless `socket_path` is set.

| key | type | flag |
|---|---|---|
| `socket_path` | string | `--control-socket` |
| `max_clients` | integer, ≥1 | `--control-max-clients` |

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
