package ui

import (
	"strings"
	"testing"
	"time"

	"github.com/canavan-a/horus-33/tui-controller/internal/link"
	"github.com/canavan-a/horus-33/tui-controller/internal/proto"
	tea "github.com/charmbracelet/bubbletea"
)

// drive runs the model's Init/Update loop against a real Fake link, pumping
// commands synchronously until the queue drains or the deadline passes. This
// exercises the whole stack (model -> link -> fake device -> model) without a
// terminal.
func drive(t *testing.T, m Model, extra ...tea.Msg) Model {
	t.Helper()

	pending := []tea.Cmd{m.Init()}
	deadline := time.After(2 * time.Second)
	queued := append([]tea.Msg(nil), extra...)

	for {
		// Deliver any queued key/tick messages first.
		if len(queued) > 0 {
			msg := queued[0]
			queued = queued[1:]
			next, cmd := m.Update(msg)
			m = next.(Model)
			if cmd != nil {
				pending = append(pending, cmd)
			}
			continue
		}
		if len(pending) == 0 {
			return m
		}

		cmd := pending[0]
		pending = pending[1:]

		// Run the command off-thread: waitFor blocks until an event arrives.
		out := make(chan tea.Msg, 1)
		go func() { out <- cmd() }()

		select {
		case msg := <-out:
			if msg == nil {
				continue
			}
			if batch, ok := msg.(tea.BatchMsg); ok {
				pending = append(pending, batch...)
				continue
			}
			// Stop pumping once we've drained the device and are just waiting.
			next, nextCmd := m.Update(msg)
			m = next.(Model)
			// The simulator's ticker re-arms itself forever, so following it
			// would keep this loop from ever draining.
			_, isTick := msg.(simTickMsg)
			if nextCmd != nil && !isTick {
				pending = append(pending, nextCmd)
			}
		case <-time.After(150 * time.Millisecond):
			// No more events pending; the link is idle.
			return m
		case <-deadline:
			t.Fatal("drive: timed out")
		}
	}
}

func newTestModel(t *testing.T) Model {
	t.Helper()
	lk := link.NewFake(0)
	t.Cleanup(func() { lk.Close() })
	return New(lk, "test")
}

func TestDescriptorBuildsControlsAndTargets(t *testing.T) {
	m := drive(t, newTestModel(t))

	if m.phase != phaseReady {
		t.Fatalf("phase = %v, want ready (status %q)", m.phase, m.status)
	}
	// "sim" is host-local and appended to whatever the device advertised.
	want := []string{"led", "motion", "axis_x", "axis_y", "sim"}
	if len(m.controls) != len(want) {
		t.Fatalf("controls = %+v", m.controls)
	}
	for i, id := range want {
		if m.controls[i].ID != id {
			t.Fatalf("controls[%d] = %q, want %q", i, m.controls[i].ID, id)
		}
	}

	// led: mode + color(3 channels) + rate_ms + brightness = 6
	// motion: 6; each axis: 11; sim: 8 = 42 focus targets.
	if len(m.targets) != 42 {
		t.Errorf("targets = %d, want 42: %+v", len(m.targets), m.targets)
	}

	v := m.values["led"]
	if v["mode"] != "blink" || v["color"] != "#8000ff" {
		t.Errorf("seeded values = %+v", v)
	}
}

func TestEditSendsSetAndDeviceEchoesState(t *testing.T) {
	m := drive(t, newTestModel(t))

	// Focus rate_ms: targets are mode, color R/G/B, rate_ms, brightness.
	m.focus = 4
	if got := m.controls[0].Fields[m.targets[m.focus].field].Key; got != "rate_ms" {
		t.Fatalf("focus landed on %q, want rate_ms", got)
	}

	// One increment of 50ms from the 50ms default, then flush immediately.
	// Incrementing rather than decrementing because the default sits on the min.
	m = drive(t, m, tea.KeyMsg{Type: tea.KeyRight}, tea.KeyMsg{Type: tea.KeyEnter})

	got, ok := proto.Num(m.values["led"]["rate_ms"])
	if !ok || got != 100 {
		t.Errorf("rate_ms = %v, want 100", m.values["led"]["rate_ms"])
	}
	if len(m.pending) != 0 {
		t.Errorf("pending should be empty after flush: %+v", m.pending)
	}
}

func TestDeviceClampingWinsOverLocalValue(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.focus = 5 // brightness, min 0

	// Drive well past the minimum. Local adjust clamps too, but the point is the
	// echoed device state is what ends up displayed.
	keys := []tea.Msg{}
	for i := 0; i < 40; i++ {
		keys = append(keys, tea.KeyMsg{Type: tea.KeyLeft})
	}
	keys = append(keys, tea.KeyMsg{Type: tea.KeyEnter})
	m = drive(t, m, keys...)

	got, _ := proto.Num(m.values["led"]["brightness"])
	if got != 0 {
		t.Errorf("brightness = %v, want 0 (clamped)", got)
	}
}

func TestEnumCyclesAndWraps(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.focus = 0 // mode

	// Default is "blink" (index 2 of off/solid/blink/breathe). Two rights wraps
	// past "breathe" back to "off".
	m = drive(t, m, tea.KeyMsg{Type: tea.KeyRight}, tea.KeyMsg{Type: tea.KeyRight},
		tea.KeyMsg{Type: tea.KeyEnter})

	if got := m.values["led"]["mode"]; got != "off" {
		t.Errorf("mode = %v, want off", got)
	}
}

func TestColorChannelEditing(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.focus = 1 // color, channel R

	m = drive(t, m, tea.KeyMsg{Type: tea.KeyRight}, tea.KeyMsg{Type: tea.KeyEnter})

	// Default #8000ff, R stepped up by 5.
	if got := m.values["led"]["color"]; got != "#8500ff" {
		t.Errorf("color = %v, want #8500ff", got)
	}
}

func TestUnknownFieldTypeRendersReadOnly(t *testing.T) {
	f := proto.Field{Key: "mystery", Type: "hologram", Label: "Mystery"}
	out := renderField(f, "whatever", false, -1)
	if !strings.Contains(out, "unsupported type") {
		t.Errorf("unknown type should render a placeholder, got %q", out)
	}
}

func TestViewRendersWithoutPanicInEveryPhase(t *testing.T) {
	m := newTestModel(t)
	for _, p := range []phase{phaseConnecting, phaseDescribing, phaseLost} {
		m.phase = p
		if m.View() == "" {
			t.Errorf("empty view in phase %v", p)
		}
	}

	ready := drive(t, newTestModel(t))
	out := ready.View()
	for _, want := range []string{"Status LED", "Mode", "Color", "Rate", "Brightness"} {
		if !strings.Contains(out, want) {
			t.Errorf("view missing %q:\n%s", want, out)
		}
	}
}

func TestDebounceCoalescesRapidEdits(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.focus = 4 // rate_ms

	// Three edits without a flush should leave exactly one pending entry, which
	// is what keeps a held arrow key from flooding the serial link.
	for i := 0; i < 3; i++ {
		next, _ := m.Update(tea.KeyMsg{Type: tea.KeyRight})
		m = next.(Model)
	}
	if len(m.pending) != 1 || len(m.pending["led"]) != 1 {
		t.Fatalf("pending = %+v, want one control with one key", m.pending)
	}
	got, _ := proto.Num(m.pending["led"]["rate_ms"])
	if got != 200 {
		t.Errorf("pending rate_ms = %v, want 200", got)
	}
}

func TestStaleFlushIsIgnored(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.focus = 4

	next, _ := m.Update(tea.KeyMsg{Type: tea.KeyLeft})
	m = next.(Model)

	// A flush tagged with an older generation must not clear pending edits.
	next, _ = m.Update(flushMsg{gen: m.gen - 1})
	m = next.(Model)
	if len(m.pending) == 0 {
		t.Error("stale flush cleared pending edits")
	}

	next, _ = m.Update(flushMsg{gen: m.gen})
	m = next.(Model)
	if len(m.pending) != 0 {
		t.Error("current flush should have drained pending edits")
	}
}

func TestPanelsWindowToTerminalHeight(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.height = 20 // too short for all four panels at once

	// Focus starts on the first control, so the last one must be off-screen.
	view := m.View()
	if !strings.Contains(view, "Status LED") {
		t.Errorf("focused panel missing from view:\n%s", view)
	}
	if strings.Contains(view, "Target simulator") {
		t.Errorf("view should not fit the last panel at height 20:\n%s", view)
	}
	if !strings.Contains(view, "more below") {
		t.Errorf("missing overflow hint:\n%s", view)
	}

	// Moving focus to the last control must bring its panel into view.
	m.focus = len(m.targets) - 1
	view = m.View()
	if !strings.Contains(view, "Target simulator") {
		t.Errorf("focused panel not shown after moving focus:\n%s", view)
	}
	if !strings.Contains(view, "more above") {
		t.Errorf("missing overflow hint:\n%s", view)
	}

	// With no size known yet, everything renders rather than nothing.
	m.height = 0
	if view = m.View(); !strings.Contains(view, "Status LED") ||
		!strings.Contains(view, "Target simulator") {
		t.Errorf("unsized view should show all panels:\n%s", view)
	}
}

func TestSimEmitsOnlyWhenEnabled(t *testing.T) {
	m := drive(t, newTestModel(t))

	if _, ok := m.simFrame(0); ok {
		t.Error("simulator should be silent until emit is turned on")
	}

	m.values[simID]["emit"] = true
	m.values[simID]["x"] = 0.4
	m.values[simID]["y"] = -0.25
	tr, ok := m.simFrame(0)
	if !ok {
		t.Fatal("simulator should emit once enabled")
	}
	if tr.X != 0.4 || tr.Y != -0.25 || tr.Lost {
		t.Errorf("manual frame = %+v, want x=0.4 y=-0.25", tr)
	}

	// "Send lost" wins over the coordinates: it is what exercises the device's
	// lost-timeout and return-to-home path.
	m.values[simID]["lost"] = true
	if tr, _ = m.simFrame(0); !tr.Lost || tr.X != 0 {
		t.Errorf("lost frame = %+v, want only lost set", tr)
	}
}

func TestSimSweepDrivesCoordinates(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.values[simID]["emit"] = true
	m.values[simID]["pattern"] = simSweepX
	m.values[simID]["amp"] = 0.5
	m.values[simID]["period"] = 4.0

	// A quarter period into a sine sweep is the positive peak.
	tr, ok := m.simFrame(time.Second)
	if !ok {
		t.Fatal("expected a frame")
	}
	if tr.X != 0.5 || tr.Y != 0 {
		t.Errorf("quarter-period frame = %+v, want x=0.5 y=0", tr)
	}
	// Three quarters in, the negative peak.
	if tr, _ = m.simFrame(3 * time.Second); tr.X != -0.5 {
		t.Errorf("three-quarter frame = %+v, want x=-0.5", tr)
	}
	// The panel doubles as a readout, so the swept value is written back.
	if got, _ := proto.Num(m.values[simID]["x"]); got != -0.5 {
		t.Errorf("readout x = %v, want -0.5", got)
	}
}

func TestSimEditsNeverReachTheDevice(t *testing.T) {
	m := drive(t, newTestModel(t))

	// Focus the simulator's first field and toggle it.
	for i, tg := range m.targets {
		if m.controls[tg.ctrl].ID == simID {
			m.focus = i
			break
		}
	}
	next, _ := m.Update(tea.KeyMsg{Type: tea.KeyRight})
	m = next.(Model)
	if len(m.pending[simID]) == 0 {
		t.Fatal("edit should have registered locally")
	}

	// Flushing must drop it rather than send a `set` the device would reject.
	m.flush()
	if len(m.pending) != 0 {
		t.Errorf("pending not drained: %+v", m.pending)
	}
	if got, _ := m.values[simID]["emit"].(bool); !got {
		t.Error("local value should still have been applied")
	}
}

func TestFramePlotsTargetAndDeadband(t *testing.T) {
	m := drive(t, newTestModel(t))
	m.values["motion"]["deadband"] = 0.1
	m.simLast, m.simLastOK = proto.Track{X: 1, Y: 1, Conf: 0.9}, true

	rows := strings.Split(m.renderFrame(), "\n")
	if len(rows) != frameH+1 {
		t.Fatalf("frame has %d rows, want %d plus the wire line", len(rows), frameH)
	}

	// +x is right and +y is up, so the top-right corner is (1, 1).
	if !strings.HasSuffix(rows[0], "●") {
		t.Errorf("target (1,1) should sit top-right, got %q", rows[0])
	}
	// ...and (-1,-1) is the bottom-left.
	m.simLast = proto.Track{X: -1, Y: -1}
	rows = strings.Split(m.renderFrame(), "\n")
	if !strings.HasPrefix(rows[frameH-1], "●") {
		t.Errorf("target (-1,-1) should sit bottom-left, got %q", rows[frameH-1])
	}

	// The deadband is drawn from the device's setting, and the wire line shows
	// the literal JSON so the display cannot drift from what was sent.
	if !strings.Contains(m.renderFrame(), "░") {
		t.Error("deadband region missing from frame")
	}
	if wire := m.renderWire(); !strings.Contains(wire, `"t":"track"`) {
		t.Errorf("wire line = %q, want the encoded track message", wire)
	}

	m.simLastOK = false
	if !strings.Contains(m.renderWire(), "emit is off") {
		t.Error("wire line should say nothing is being sent")
	}
}
