package link

import (
	"fmt"
	"sync"
	"time"

	"github.com/canavan-a/horus-33/tui-controller/internal/proto"
)

// FakeDescriptor is the control set the fake device advertises. It mirrors what
// the firmware publishes, so the TUI can be developed without hardware.
func FakeDescriptor() []proto.Control {
	f64 := func(v float64) *float64 { return &v }
	return []proto.Control{{
		ID:    "led",
		Label: "Status LED",
		Fields: []proto.Field{
			{Key: "mode", Type: proto.FEnum, Label: "Mode",
				Options: []string{"off", "solid", "blink", "breathe"}, Default: "blink"},
			{Key: "color", Type: proto.FColor, Label: "Color", Default: "#8000ff"},
			{Key: "rate_ms", Type: proto.FNumber, Label: "Rate",
				Min: f64(50), Max: f64(5000), Step: f64(50), Unit: "ms", Default: float64(50)},
			{Key: "brightness", Type: proto.FNumber, Label: "Brightness",
				Min: f64(0), Max: f64(255), Step: f64(5), Default: float64(10)},
		},
	}, {
		ID:    "motion",
		Label: "Motion",
		Fields: []proto.Field{
			{Key: "mode", Type: proto.FEnum, Label: "Mode",
				Options: []string{"manual", "pid"}, Default: "manual"},
			{Key: "estop", Type: proto.FBool, Label: "E-stop", Default: false},
			{Key: "lost_ms", Type: proto.FNumber, Label: "Lost timeout",
				Min: f64(100), Max: f64(60000), Step: f64(100), Unit: "ms", Default: float64(1000)},
			{Key: "min_conf", Type: proto.FNumber, Label: "Min confidence",
				Min: f64(0), Max: f64(1), Step: f64(0.05), Default: float64(0.30)},
			{Key: "deadband", Type: proto.FNumber, Label: "Deadband",
				Min: f64(0), Max: f64(0.5), Step: f64(0.01), Default: float64(0.02)},
			{Key: "home_sps", Type: proto.FNumber, Label: "Home speed",
				Min: f64(0), Max: f64(20000), Step: f64(50), Unit: "sps", Default: float64(800)},
		},
	},
		axis("axis_x", "X Axis"),
		axis("axis_y", "Y Axis"),
	}
}

// axis builds the field set shared by both steppers; the firmware describes
// them from one class, so they stay identical here too.
func axis(id, label string) proto.Control {
	f64 := func(v float64) *float64 { return &v }
	return proto.Control{
		ID:    id,
		Label: label,
		Fields: []proto.Field{
			{Key: "enable", Type: proto.FBool, Label: "Enable", Default: false},
			{Key: "run", Type: proto.FBool, Label: "Run", Default: false},
			{Key: "dir", Type: proto.FEnum, Label: "Direction",
				Options: []string{"fwd", "rev"}, Default: "fwd"},
			{Key: "invert_dir", Type: proto.FBool, Label: "Invert polarity", Default: false},
			{Key: "speed", Type: proto.FNumber, Label: "Jog speed",
				Min: f64(0), Max: f64(20000), Step: f64(50), Unit: "sps", Default: float64(400)},
			{Key: "kp", Type: proto.FNumber, Label: "Kp",
				Min: f64(0), Max: f64(20000), Step: f64(25), Unit: "sps", Default: float64(1200)},
			{Key: "ki", Type: proto.FNumber, Label: "Ki",
				Min: f64(0), Max: f64(20000), Step: f64(25), Default: float64(0)},
			{Key: "kd", Type: proto.FNumber, Label: "Kd",
				Min: f64(0), Max: f64(20000), Step: f64(25), Default: float64(0)},
			{Key: "max_sps", Type: proto.FNumber, Label: "PID max speed",
				Min: f64(0), Max: f64(20000), Step: f64(50), Unit: "sps", Default: float64(4000)},
			{Key: "home", Type: proto.FNumber, Label: "Home",
				Min: f64(-1000000), Max: f64(1000000), Step: f64(10), Unit: "st", Default: float64(0)},
			{Key: "pos", Type: proto.FNumber, Label: "Position",
				Min: f64(-1000000), Max: f64(1000000), Step: f64(10), Unit: "st", Default: float64(0)},
		},
	}
}

// Fake is an in-process device implementing the same protocol as the firmware.
// It exists so the TUI is runnable and testable with nothing plugged in.
type Fake struct {
	events   chan Event
	controls []proto.Control
	latency  time.Duration

	mu     sync.Mutex
	state  map[string]proto.Values
	closed bool
}

// NewFake returns a started fake device. latency, if non-zero, delays replies to
// approximate a real serial round-trip.
func NewFake(latency time.Duration) *Fake {
	controls := FakeDescriptor()
	state := make(map[string]proto.Values, len(controls))
	for _, c := range controls {
		state[c.ID] = c.Defaults()
	}

	f := &Fake{
		events:   make(chan Event, 32),
		controls: controls,
		latency:  latency,
		state:    state,
	}
	// Announce ourselves the way a freshly booted board does.
	f.emit(Event{Connected: true})
	f.emit(Event{Msg: proto.Hello{Proto: proto.Version, Device: "horus-33-fake", FW: "fake"}})
	return f
}

func (f *Fake) Events() <-chan Event { return f.events }

func (f *Fake) emit(e Event) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return
	}
	select {
	case f.events <- e:
	default: // drop rather than block a caller holding the lock
	}
}

func (f *Fake) Send(m proto.Msg) error {
	f.mu.Lock()
	if f.closed {
		f.mu.Unlock()
		return fmt.Errorf("link closed")
	}
	f.mu.Unlock()

	// Reply asynchronously so Send never blocks the UI, matching the real link.
	go func() {
		if f.latency > 0 {
			time.Sleep(f.latency)
		}
		f.handle(m)
	}()
	return nil
}

func (f *Fake) handle(m proto.Msg) {
	switch msg := m.(type) {
	case proto.Describe:
		f.emit(Event{Msg: proto.Descriptor{Controls: f.controls}})
		f.emit(Event{Msg: proto.Ack{Seq: msg.Seq}})
		for _, c := range f.controls {
			f.emit(Event{Msg: proto.State{ID: c.ID, V: f.snapshot(c.ID)}})
		}

	case proto.Ping:
		f.emit(Event{Msg: proto.Ack{Seq: msg.Seq}})

	case proto.Set:
		if err := f.applySet(msg); err != nil {
			f.emit(Event{Msg: proto.Err{Seq: msg.Seq, Msg: err.Error()}})
			return
		}
		f.emit(Event{Msg: proto.Ack{Seq: msg.Seq}})
		f.emit(Event{Msg: proto.State{ID: msg.ID, V: f.snapshot(msg.ID)}})
	}
}

func (f *Fake) applySet(msg proto.Set) error {
	var ctrl proto.Control
	found := false
	for _, c := range f.controls {
		if c.ID == msg.ID {
			ctrl, found = c, true
			break
		}
	}
	if !found {
		return fmt.Errorf("unknown control %q", msg.ID)
	}

	// Coerce everything before storing anything, so a bad key cannot leave the
	// control half-updated.
	next := make(proto.Values, len(msg.V))
	for k, v := range msg.V {
		field, ok := ctrl.Field(k)
		if !ok {
			return fmt.Errorf("unknown field %q", k)
		}
		cv, err := proto.Coerce(field, v)
		if err != nil {
			return err
		}
		next[k] = cv
	}

	f.mu.Lock()
	defer f.mu.Unlock()
	for k, v := range next {
		f.state[msg.ID][k] = v
	}
	return nil
}

func (f *Fake) snapshot(id string) proto.Values {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make(proto.Values, len(f.state[id]))
	for k, v := range f.state[id] {
		out[k] = v
	}
	return out
}

func (f *Fake) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return nil
	}
	f.closed = true
	close(f.events)
	return nil
}
