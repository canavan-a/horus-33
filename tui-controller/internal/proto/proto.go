// Package proto implements the Horus-33 host<->device wire protocol: one JSON
// object per line. See docs/protocol.md for the contract.
package proto

import (
	"encoding/json"
	"fmt"
	"math"
	"strconv"
)

// Version is the protocol version this package speaks.
const Version = 1

// Message type tags.
const (
	TDescribe   = "describe"
	TSet        = "set"
	TPing       = "ping"
	TTrack      = "track"
	THello      = "hello"
	TDescriptor = "descriptor"
	TState      = "state"
	TAck        = "ack"
	TErr        = "err"
)

// Values is one control's field values, keyed by field key. A set carries a
// partial map; a state carries the complete one.
type Values map[string]any

// Msg is any protocol message. Concrete types below implement it.
type Msg interface{ Type() string }

// --- host -> device ---

type Describe struct {
	Seq uint64 `json:"seq"`
}

type Set struct {
	Seq uint64 `json:"seq"`
	ID  string `json:"id"`
	V   Values `json:"v"`
}

type Ping struct {
	Seq uint64 `json:"seq"`
}

// Track is one object detection, normalised so (0,0) is the frame centre, +x is
// right and +y is up. It carries no Seq: the device deliberately does not reply
// to it, because this is a per-frame stream rather than a request.
type Track struct {
	X    float64 `json:"x"`
	Y    float64 `json:"y"`
	W    float64 `json:"w,omitempty"`
	H    float64 `json:"h,omitempty"`
	Conf float64 `json:"c,omitempty"`
	Lost bool    `json:"lost,omitempty"`
}

func (Describe) Type() string { return TDescribe }
func (Set) Type() string      { return TSet }
func (Ping) Type() string     { return TPing }
func (Track) Type() string    { return TTrack }

// --- device -> host ---

type Hello struct {
	Proto  int    `json:"proto"`
	Device string `json:"device"`
	FW     string `json:"fw"`
}

type Descriptor struct {
	Controls []Control `json:"controls"`
}

type State struct {
	ID string `json:"id"`
	V  Values `json:"v"`
}

type Ack struct {
	Seq uint64 `json:"seq"`
}

type Err struct {
	Seq uint64 `json:"seq"`
	Msg string `json:"msg"`
}

func (Hello) Type() string      { return THello }
func (Descriptor) Type() string { return TDescriptor }
func (State) Type() string      { return TState }
func (Ack) Type() string        { return TAck }
func (Err) Type() string        { return TErr }

// --- descriptor model ---

// Field types. Anything else must be rendered read-only rather than rejected, so
// an older host stays usable against newer firmware.
const (
	FNumber = "number"
	FColor  = "color"
	FEnum   = "enum"
	FBool   = "bool"
)

type Control struct {
	ID     string  `json:"id"`
	Label  string  `json:"label"`
	Fields []Field `json:"fields"`
}

type Field struct {
	Key   string `json:"key"`
	Type  string `json:"type"`
	Label string `json:"label"`

	// number only
	Min  *float64 `json:"min,omitempty"`
	Max  *float64 `json:"max,omitempty"`
	Step *float64 `json:"step,omitempty"`
	Unit string   `json:"unit,omitempty"`

	// enum only
	Options []string `json:"options,omitempty"`

	Default any `json:"default,omitempty"`
}

// Defaults returns the control's field defaults, used to seed local state before
// the first state message arrives.
func (c Control) Defaults() Values {
	v := make(Values, len(c.Fields))
	for _, f := range c.Fields {
		if f.Default != nil {
			v[f.Key] = f.Default
		}
	}
	return v
}

// Field looks up a field by key.
func (c Control) Field(key string) (Field, bool) {
	for _, f := range c.Fields {
		if f.Key == key {
			return f, true
		}
	}
	return Field{}, false
}

// Clamp constrains a numeric value to the field's range. Fields without bounds
// pass through unchanged.
func (f Field) Clamp(n float64) float64 {
	if f.Min != nil && n < *f.Min {
		n = *f.Min
	}
	if f.Max != nil && n > *f.Max {
		n = *f.Max
	}
	return n
}

// StepSize is the field's step, defaulting to 1 when unspecified.
func (f Field) StepSize() float64 {
	if f.Step != nil && *f.Step != 0 {
		return *f.Step
	}
	return 1
}

// maxDecimals bounds the search in Decimals. Nothing on this wire is meaningful
// past six places, and stopping keeps a pathological step from looping long.
const maxDecimals = 6

// Decimals is the number of decimal places the field's step implies: 0 for a
// step of 50, 2 for a step of 0.05.
func (f Field) Decimals() int {
	step := math.Abs(f.StepSize())
	for d := 0; d < maxDecimals; d++ {
		scaled := step * math.Pow10(d)
		if math.Abs(scaled-math.Round(scaled)) < 1e-9 {
			return d
		}
	}
	return maxDecimals
}

// Snap rounds to the precision the step implies. Repeatedly adding a step like
// 0.05 in binary floating point drifts (0.15000000000000002), and without this
// that drift would be both displayed and put on the wire.
func (f Field) Snap(n float64) float64 {
	p := math.Pow10(f.Decimals())
	return math.Round(n*p) / p
}

// Format renders a value for display: snapped to the field's precision and
// written in plain notation, so a large bound like 1000000 does not turn into
// 1e+06.
func (f Field) Format(n float64) string {
	return strconv.FormatFloat(f.Snap(n), 'f', -1, 64)
}

// --- encoding ---

// Encode marshals a message to a single line including its trailing newline.
func Encode(m Msg) ([]byte, error) {
	// Marshal the concrete type, then splice in the tag. Doing it this way keeps
	// the tag out of every struct definition.
	body, err := json.Marshal(m)
	if err != nil {
		return nil, err
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(body, &fields); err != nil {
		return nil, err
	}
	tag, err := json.Marshal(m.Type())
	if err != nil {
		return nil, err
	}
	fields["t"] = tag

	out, err := json.Marshal(fields)
	if err != nil {
		return nil, err
	}
	return append(out, '\n'), nil
}

// ErrUnknownType reports a message whose tag this host does not handle.
type ErrUnknownType struct{ Tag string }

func (e ErrUnknownType) Error() string { return fmt.Sprintf("unknown message type %q", e.Tag) }

// Decode parses one line into a concrete message. The trailing newline is
// optional. Unknown tags return ErrUnknownType so callers can choose to skip
// them rather than treat them as a protocol failure.
func Decode(line []byte) (Msg, error) {
	var probe struct {
		T string `json:"t"`
	}
	if err := json.Unmarshal(line, &probe); err != nil {
		return nil, fmt.Errorf("parse line: %w", err)
	}

	var m Msg
	switch probe.T {
	case TDescribe:
		m = &Describe{}
	case TSet:
		m = &Set{}
	case TPing:
		m = &Ping{}
	case TTrack:
		m = &Track{}
	case THello:
		m = &Hello{}
	case TDescriptor:
		m = &Descriptor{}
	case TState:
		m = &State{}
	case TAck:
		m = &Ack{}
	case TErr:
		m = &Err{}
	default:
		return nil, ErrUnknownType{Tag: probe.T}
	}

	if err := json.Unmarshal(line, m); err != nil {
		return nil, fmt.Errorf("parse %s: %w", probe.T, err)
	}
	// Return values, not pointers, so callers can type-switch on the plain types.
	switch v := m.(type) {
	case *Describe:
		return *v, nil
	case *Set:
		return *v, nil
	case *Ping:
		return *v, nil
	case *Track:
		return *v, nil
	case *Hello:
		return *v, nil
	case *Descriptor:
		return *v, nil
	case *State:
		return *v, nil
	case *Ack:
		return *v, nil
	case *Err:
		return *v, nil
	}
	return nil, ErrUnknownType{Tag: probe.T}
}
