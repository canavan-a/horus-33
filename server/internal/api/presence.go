package api

import (
	"sync"
	"time"

	"github.com/canavan-a/horus-33/server/internal/proto"
)

// PresenceEvent is the payload of a "presence" WS event: whether a person is
// currently in frame, plus the confidence of the detection that last said so.
type PresenceEvent struct {
	Present bool     `json:"present"`
	Conf    *float64 `json:"conf,omitempty"`
}

// Presence hysteresis defaults. capture-eye forwards its own `track` stream to
// relay clients (ControlRelay::publish_local); this turns that frame-rate
// stream into a debounced present/absent signal so a single dropped inference
// frame does not flip a notification.
const (
	presenceEnterFor = 300 * time.Millisecond  // continuous detection before "present"
	presenceExitFor  = 1500 * time.Millisecond // lost / silence before "absent"
	presenceTick     = 250 * time.Millisecond  // how often tick() checks the silence case
)

// presenceDetector reduces the raw track stream to edge-triggered presence.
// now is injectable so the hysteresis is testable without sleeping.
type presenceDetector struct {
	enterFor time.Duration
	exitFor  time.Duration
	now      func() time.Time
	onChange func(PresenceEvent)

	mu        sync.Mutex
	present   bool
	firstSeen time.Time // start of the current uninterrupted detection run
	lastGood  time.Time // last non-lost track
	lastConf  *float64
}

func newPresenceDetector(onChange func(PresenceEvent)) *presenceDetector {
	return &presenceDetector{
		enterFor: presenceEnterFor,
		exitFor:  presenceExitFor,
		now:      time.Now,
		onChange: onChange,
	}
}

// observe folds one track line into the presence state, emitting an edge via
// onChange when the debounced signal flips.
func (d *presenceDetector) observe(t proto.Track) {
	now := d.now()

	d.mu.Lock()
	var edge *PresenceEvent

	if t.Lost {
		d.firstSeen = time.Time{}
		if d.present && !d.lastGood.IsZero() && now.Sub(d.lastGood) >= d.exitFor {
			d.present = false
			d.lastConf = nil
			edge = &PresenceEvent{Present: false}
		}
	} else {
		d.lastGood = now
		// Protocol: an absent `c` means "unscored", not zero — only carry a
		// confidence we actually got.
		if t.Conf != 0 {
			c := t.Conf
			d.lastConf = &c
		}
		if d.firstSeen.IsZero() {
			d.firstSeen = now
		}
		if !d.present && now.Sub(d.firstSeen) >= d.enterFor {
			d.present = true
			edge = &PresenceEvent{Present: true, Conf: d.lastConf}
		}
	}
	d.mu.Unlock()

	if edge != nil {
		d.onChange(*edge)
	}
}

// tick covers the case where the track stream simply stops (capture-eye crash,
// link loss) without ever sending an explicit `lost`.
func (d *presenceDetector) tick() {
	now := d.now()

	d.mu.Lock()
	var edge *PresenceEvent
	if d.present && !d.lastGood.IsZero() && now.Sub(d.lastGood) >= d.exitFor {
		d.present = false
		d.lastConf = nil
		d.firstSeen = time.Time{}
		edge = &PresenceEvent{Present: false}
	}
	d.mu.Unlock()

	if edge != nil {
		d.onChange(*edge)
	}
}

// snapshot is the current debounced state, for the WS initial burst.
func (d *presenceDetector) snapshot() PresenceEvent {
	d.mu.Lock()
	defer d.mu.Unlock()
	return PresenceEvent{Present: d.present, Conf: d.lastConf}
}
