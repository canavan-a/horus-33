package api

import (
	"sync"
	"testing"
	"time"

	"github.com/canavan-a/horus-33/server/internal/proto"
)

// fakeClock is a manually advanced clock for the presence hysteresis tests.
type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *fakeClock) now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *fakeClock) advance(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

func newTestDetector() (*presenceDetector, *fakeClock, *[]PresenceEvent) {
	clk := &fakeClock{t: time.Unix(1700000000, 0)}
	var got []PresenceEvent
	d := newPresenceDetector(func(ev PresenceEvent) { got = append(got, ev) })
	d.now = clk.now
	return d, clk, &got
}

func TestPresenceRisingEdgeAfterEnterFor(t *testing.T) {
	d, clk, got := newTestDetector()

	d.observe(proto.Track{X: 0.1, Conf: 0.9}) // t=0, starts the run
	if len(*got) != 0 {
		t.Fatalf("fired before enterFor: %+v", *got)
	}
	clk.advance(d.enterFor)
	d.observe(proto.Track{X: 0.1, Conf: 0.8})

	if len(*got) != 1 || !(*got)[0].Present {
		t.Fatalf("want one present edge, got %+v", *got)
	}
	if (*got)[0].Conf == nil || *(*got)[0].Conf != 0.8 {
		t.Fatalf("want conf 0.8, got %+v", (*got)[0].Conf)
	}
}

func TestPresenceSingleLostDoesNotFlip(t *testing.T) {
	d, clk, got := newTestDetector()

	d.observe(proto.Track{X: 0.1, Conf: 0.9})
	clk.advance(d.enterFor)
	d.observe(proto.Track{X: 0.1, Conf: 0.9}) // present now
	*got = (*got)[:0]

	clk.advance(100 * time.Millisecond)
	d.observe(proto.Track{Lost: true}) // one dropped frame, well under exitFor
	if len(*got) != 0 {
		t.Fatalf("flipped on a single lost frame: %+v", *got)
	}
}

func TestPresenceFallingEdgeAfterExitFor(t *testing.T) {
	d, clk, got := newTestDetector()

	d.observe(proto.Track{X: 0.1, Conf: 0.9})
	clk.advance(d.enterFor)
	d.observe(proto.Track{X: 0.1, Conf: 0.9})
	*got = (*got)[:0]

	clk.advance(d.exitFor)
	d.observe(proto.Track{Lost: true})

	if len(*got) != 1 || (*got)[0].Present {
		t.Fatalf("want one absent edge, got %+v", *got)
	}
	if (*got)[0].Conf != nil {
		t.Fatalf("absent edge should carry no conf, got %+v", (*got)[0].Conf)
	}
}

func TestPresenceFallingEdgeViaTickOnSilence(t *testing.T) {
	d, clk, got := newTestDetector()

	d.observe(proto.Track{X: 0.1, Conf: 0.9})
	clk.advance(d.enterFor)
	d.observe(proto.Track{X: 0.1, Conf: 0.9})
	*got = (*got)[:0]

	// Stream just stops — no further observe(), only ticks.
	clk.advance(d.exitFor + time.Millisecond)
	d.tick()

	if len(*got) != 1 || (*got)[0].Present {
		t.Fatalf("want one absent edge from tick, got %+v", *got)
	}
	if s := d.snapshot(); s.Present {
		t.Fatalf("snapshot still present after tick flip")
	}
}

func TestPresenceUnscoredTrackKeepsNilConf(t *testing.T) {
	d, clk, got := newTestDetector()

	d.observe(proto.Track{X: 0.1}) // Conf 0 == unscored
	clk.advance(d.enterFor)
	d.observe(proto.Track{X: 0.1})

	if len(*got) != 1 || !(*got)[0].Present {
		t.Fatalf("want present edge, got %+v", *got)
	}
	if (*got)[0].Conf != nil {
		t.Fatalf("unscored track should not set conf, got %v", *(*got)[0].Conf)
	}
}
