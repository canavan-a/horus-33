# Horus-33 control protocol

Version `1`. Transport is the ESP32-S3 native USB CDC serial port (`/dev/ttyACM0`),
115200 baud, 8-N-1. Baud is nominal — native USB CDC ignores it, but the host still
has to pick something.

Framing is **newline-delimited JSON**: exactly one JSON object per line, terminated
by `\n`. Lines that fail to parse are answered with an `err` and otherwise ignored;
neither side ever tries to resynchronise mid-line.

The format is deliberately hand-testable. You can drive the device from
`pio device monitor` by typing a message and pressing enter.

## Design principle

The device is the source of truth. It advertises its own controls, and the host
renders whatever it is told about. The host has no compiled-in knowledge of any
specific control — adding a control to the firmware requires no host change, so long
as its fields use existing field types.

## Message envelope

Every message carries a `t` (type) tag. Host→device messages carry a monotonically
increasing `seq` which the device echoes in its `ack`/`err`, so replies can be
correlated with requests.

## Host → device

| Type | Shape | Meaning |
|---|---|---|
| `describe` | `{"t":"describe","seq":1}` | Request the control descriptor |
| `set` | `{"t":"set","seq":2,"id":"led","v":{…}}` | Apply a partial update to one control |
| `ping` | `{"t":"ping","seq":3}` | Liveness check |
| `track` | `{"t":"track","x":-0.42,"y":0.18,…}` | One object detection, for the PID loop |

`set` is a **partial** update: only the keys present in `v` are applied, the rest of
the control's state is left alone. This is what lets the host send a single changed
field on every keystroke without having to track the full state.

## Tracking stream

`track` carries one object detection per inference frame, and is the input to the
PID centring loop. It is the only message the device answers with **nothing**:
acking at frame rate would double the traffic and back up the command queue for no
benefit. Pass a `seq` to opt into an `ack`, which exists so the stream can be
hand-tested from a serial monitor.

```json
{"t":"track","x":-0.42,"y":0.18,"w":0.12,"h":0.20,"c":0.86}
{"t":"track","lost":true}
```

| Key | Range | Meaning |
|---|---|---|
| `x` | `[-1, 1]` | Horizontal offset of the box centre. `0` = frame centre, `+1` = right edge |
| `y` | `[-1, 1]` | Vertical offset. `0` = frame centre, **`+1` = top edge** |
| `w`, `h` | `[0, 1]` | Box size as a fraction of the frame. Recorded but not yet used |
| `c` | `[0, 1]` | Confidence. Gated by the `min_conf` setting. Absent means "unscored", not zero |
| `lost` | bool | `true` = no detection this frame; retires the target immediately |

Coordinates are **normalised on the host**, so the device never learns the frame
size or which model produced the box. From YOLO's normalised `cx, cy` (origin at
the top-left):

```
x = 2*cx - 1
y = 1 - 2*cy      # flipped: +y is up, so positive error means positive travel
```

Values outside `[-1, 1]` are clamped. Malformed messages are dropped silently —
the stream is fire-and-forget and must not be able to flood the link with errors.
Choosing *which* box to send when the model returns several (highest confidence,
largest, or a persistent track ID) is the host's job; the device takes one target.

If no valid detection arrives for `lost_ms`, both axes drive back to their
configured `home` position.

## Device → host

| Type | Shape | Meaning |
|---|---|---|
| `hello` | `{"t":"hello","proto":1,"device":"horus-33","fw":"…"}` | Sent once on boot |
| `descriptor` | `{"t":"descriptor","controls":[…]}` | Reply to `describe` |
| `state` | `{"t":"state","id":"led","v":{…}}` | Full current value of one control |
| `ack` | `{"t":"ack","seq":2}` | Request applied |
| `err` | `{"t":"err","seq":2,"msg":"unknown control"}` | Request rejected; `seq` omitted if unparseable |

A `state` message is emitted after every applied `set`, carrying the control's
**complete** value including any clamping the device applied. The host displays what
comes back rather than what it sent, so out-of-range input self-corrects visibly.

## Control descriptor

```json
{
  "id": "led",
  "label": "Status LED",
  "fields": [
    {"key":"mode","type":"enum","label":"Mode",
     "options":["off","solid","blink","breathe"],"default":"blink"},
    {"key":"color","type":"color","label":"Color","default":"#8000ff"},
    {"key":"rate_ms","type":"number","label":"Rate",
     "min":50,"max":5000,"step":50,"unit":"ms","default":50},
    {"key":"brightness","type":"number","label":"Brightness",
     "min":0,"max":255,"step":5,"default":10}
  ]
}
```

### Field types

| `type` | JSON value | Descriptor keys | Host widget |
|---|---|---|---|
| `number` | number | `min`, `max`, `step`, `unit` | arrow-key stepper |
| `color` | `"#rrggbb"` string | — | R/G/B channel steppers + swatch |
| `enum` | string from `options` | `options` | cycle through options |
| `bool` | `true`/`false` | — | toggle |

`type` is the extension point. A new control whose fields are all of the above needs
zero host changes. A genuinely new field type means adding one case to the host's
field widget switch — everything else stays put.

Unknown field types must be rendered as a read-only placeholder rather than causing
an error, so an older host stays usable against newer firmware.

## Session flow

```
device                                    host
  │  hello                                 │
  │ ─────────────────────────────────────► │
  │                              describe  │
  │ ◄───────────────────────────────────── │
  │  descriptor                            │
  │ ─────────────────────────────────────► │
  │  state (one per control)               │
  │ ─────────────────────────────────────► │
  │                    set {rate_ms:250}   │
  │ ◄───────────────────────────────────── │
  │  ack, state                            │
  │ ─────────────────────────────────────► │
```

The host does not depend on catching `hello` — it may attach mid-session, so it sends
`describe` on connect regardless and treats a later `hello` as a signal that the device
rebooted and should be re-described.
