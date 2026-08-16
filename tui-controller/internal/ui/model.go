package ui

import (
	"fmt"
	"strings"
	"time"

	"github.com/canavan-a/horus-33/tui-controller/internal/link"
	"github.com/canavan-a/horus-33/tui-controller/internal/proto"
	tea "github.com/charmbracelet/bubbletea"
)

// debounce is how long an edit sits before being sent. Holding an arrow key
// coalesces into one set per burst rather than flooding the serial link.
const debounce = 80 * time.Millisecond

type phase int

const (
	phaseConnecting phase = iota
	phaseDescribing
	phaseReady
	phaseLost
)

// Model is the Bubble Tea root. It holds no knowledge of any specific control:
// everything on screen is built from the descriptor the device sent.
type Model struct {
	lk   link.Link
	desc string // human-readable link description, e.g. the port path

	phase    phase
	controls []proto.Control
	values   map[string]proto.Values
	targets  []target
	focus    int

	// pending accumulates edited keys per control until the debounce fires.
	pending map[string]proto.Values
	gen     int

	// height is the terminal height, used to window the control panels. Zero
	// until the first WindowSizeMsg arrives.
	height int

	// simT0 is the origin for the simulator's sweep patterns; simLast is the
	// detection most recently put on the wire, which the frame view draws.
	simT0     time.Time
	simLast   proto.Track
	simLastOK bool

	seq     uint64
	status  string
	statusE bool
	lastErr error
	quit    bool
}

func New(lk link.Link, desc string) Model {
	return Model{
		lk:      lk,
		desc:    desc,
		phase:   phaseConnecting,
		values:  map[string]proto.Values{},
		pending: map[string]proto.Values{},
		status:  "connecting",
		simT0:   time.Now(),
	}
}

func (m Model) Init() tea.Cmd { return tea.Batch(waitFor(m.lk), simTick()) }

// --- messages ---

type linkEventMsg link.Event
type linkClosedMsg struct{}
type flushMsg struct{ gen int }
type simTickMsg struct{}

func simTick() tea.Cmd {
	return tea.Tick(simRate, func(time.Time) tea.Msg { return simTickMsg{} })
}

// waitFor pulls one event and re-arms itself, turning the link's channel into a
// stream of Bubble Tea messages.
func waitFor(lk link.Link) tea.Cmd {
	return func() tea.Msg {
		ev, ok := <-lk.Events()
		if !ok {
			return linkClosedMsg{}
		}
		return linkEventMsg(ev)
	}
}

func (m *Model) send(msg proto.Msg) {
	if err := m.lk.Send(msg); err != nil {
		m.setStatus("send failed: "+err.Error(), true)
	}
}

func (m *Model) nextSeq() uint64 {
	m.seq++
	return m.seq
}

func (m *Model) setStatus(s string, isErr bool) {
	m.status, m.statusE = s, isErr
}

// --- update ---

func (m Model) Update(raw tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := raw.(type) {
	case tea.KeyMsg:
		return m.onKey(msg)

	case tea.WindowSizeMsg:
		m.height = msg.Height
		return m, nil

	case linkClosedMsg:
		m.phase = phaseLost
		m.setStatus("link closed", true)
		return m, nil

	case linkEventMsg:
		next, cmd := m.onEvent(link.Event(msg))
		// Always re-arm the reader, otherwise the stream stalls after one event.
		return next, tea.Batch(cmd, waitFor(m.lk))

	case flushMsg:
		// Only the newest scheduled flush wins; earlier ones are stale.
		if msg.gen != m.gen {
			return m, nil
		}
		m.flush()
		return m, nil

	case simTickMsg:
		if m.phase == phaseReady {
			t, ok := m.simFrame(time.Since(m.simT0))
			if ok {
				m.send(t)
			}
			m.simLast, m.simLastOK = t, ok
		}
		// Keep the ticker alive regardless, so toggling `emit` back on does not
		// need anything to restart it.
		return m, simTick()
	}
	return m, nil
}

func (m Model) onEvent(ev link.Event) (Model, tea.Cmd) {
	switch {
	case ev.Connected:
		m.phase = phaseDescribing
		m.setStatus("connected — requesting controls", false)
		m.send(proto.Describe{Seq: m.nextSeq()})
		return m, nil

	case ev.Disconnected:
		m.phase = phaseLost
		m.lastErr = ev.Err
		if ev.Err != nil {
			m.setStatus("disconnected: "+ev.Err.Error(), true)
		} else {
			m.setStatus("disconnected", true)
		}
		return m, nil

	case ev.Note != "":
		m.setStatus(ev.Note, false)
		return m, nil
	}

	switch msg := ev.Msg.(type) {
	case proto.Hello:
		// A hello mid-session means the board rebooted; its controls may have
		// changed, so re-describe rather than trusting what we already have.
		m.setStatus(fmt.Sprintf("%s (proto %d, fw %s)", msg.Device, msg.Proto, msg.FW), false)
		m.send(proto.Describe{Seq: m.nextSeq()})

	case proto.Descriptor:
		// The simulator is appended to whatever the device advertises, so it
		// renders and navigates exactly like a real control while staying
		// entirely host-side.
		m.controls = append(append([]proto.Control(nil), msg.Controls...), simControl())
		m.targets = nil
		for i, c := range m.controls {
			m.targets = append(m.targets, targetsFor(i, c)...)
			if _, seen := m.values[c.ID]; !seen {
				m.values[c.ID] = c.Defaults()
			}
		}
		if m.focus >= len(m.targets) {
			m.focus = 0
		}
		m.phase = phaseReady
		m.setStatus(fmt.Sprintf("%d control(s) ready", len(msg.Controls)), false)

	case proto.State:
		// The device is the source of truth: adopt whatever it reports, which
		// also makes clamping of out-of-range input visible.
		cur, ok := m.values[msg.ID]
		if !ok {
			cur = proto.Values{}
			m.values[msg.ID] = cur
		}
		for k, v := range msg.V {
			cur[k] = v
		}

	case proto.Err:
		m.setStatus("device error: "+msg.Msg, true)

	case proto.Ack:
		// Nothing to do; the state message that follows carries the payload.
	}
	return m, nil
}

func (m Model) onKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q":
		m.quit = true
		m.flush() // don't lose a pending edit on the way out
		return m, tea.Quit

	case "r":
		m.send(proto.Describe{Seq: m.nextSeq()})
		m.setStatus("refreshing controls", false)
		return m, nil

	case "tab", "down", "j":
		m.moveFocus(1)
		return m, nil

	case "shift+tab", "up", "k":
		m.moveFocus(-1)
		return m, nil

	case "right", "l", "+":
		return m, m.edit(1)

	case "left", "h", "-":
		return m, m.edit(-1)

	case "enter", " ":
		// Flush immediately rather than waiting out the debounce.
		m.flush()
		return m, nil
	}
	return m, nil
}

func (m *Model) moveFocus(delta int) {
	if len(m.targets) == 0 {
		return
	}
	m.focus = (m.focus + delta + len(m.targets)) % len(m.targets)
}

// edit applies a step to the focused field and schedules a debounced send.
func (m *Model) edit(delta int) tea.Cmd {
	if m.phase != phaseReady || len(m.targets) == 0 {
		return nil
	}
	t := m.targets[m.focus]
	ctrl := m.controls[t.ctrl]
	field := ctrl.Fields[t.field]

	cur := m.values[ctrl.ID][field.Key]
	next, ok := adjust(field, cur, delta, t.ch)
	if !ok {
		return nil
	}

	// Update locally for immediate feedback; the device's echoed state will
	// overwrite this shortly and is authoritative.
	m.values[ctrl.ID][field.Key] = next
	if m.pending[ctrl.ID] == nil {
		m.pending[ctrl.ID] = proto.Values{}
	}
	m.pending[ctrl.ID][field.Key] = next

	m.gen++
	gen := m.gen
	return tea.Tick(debounce, func(time.Time) tea.Msg { return flushMsg{gen: gen} })
}

// flush sends one set per control holding pending edits.
func (m *Model) flush() {
	for id, vals := range m.pending {
		if len(vals) == 0 || id == simID {
			// The simulator has no device-side counterpart; a `set` for it would
			// come back as "unknown control".
			continue
		}
		m.send(proto.Set{Seq: m.nextSeq(), ID: id, V: vals})
	}
	m.pending = map[string]proto.Values{}
}

// --- view ---

func (m Model) View() string {
	if m.quit {
		return ""
	}

	var b strings.Builder
	b.WriteString(styleTitle.Render("horus-33") + styleDim.Render("  "+m.desc) + "\n\n")

	switch m.phase {
	case phaseConnecting, phaseDescribing:
		b.WriteString(styleDim.Render("waiting for device…") + "\n")
	case phaseLost:
		b.WriteString(styleErr.Render("device disconnected") + "\n")
		b.WriteString(styleDim.Render("reconnect the board; press q to quit") + "\n")
	case phaseReady:
		b.WriteString(m.renderPanels())
	}

	b.WriteString(m.renderStatus())
	b.WriteString(styleHelp.Render(
		"tab/↑↓ move · ←→ adjust · enter send now · r refresh · q quit"))
	return b.String() + "\n"
}

// chrome is the number of lines View spends on things that are not panels:
// title, blank line, status and help.
const chrome = 6

// renderPanels shows as many whole control panels as fit, always including the
// focused one. Panels are windowed rather than scrolled line-by-line so a panel
// is never cut in half, which matters now that a single axis carries a dozen
// fields and would otherwise run off the bottom of the terminal.
func (m Model) renderPanels() string {
	blocks := make([]string, len(m.controls))
	costs := make([]int, len(m.controls))
	for i, c := range m.controls {
		blocks[i] = m.renderControl(i, c)
		costs[i] = strings.Count(blocks[i], "\n")
	}

	focusCtrl := 0
	if m.focus < len(m.targets) {
		focusCtrl = m.targets[m.focus].ctrl
	}

	budget := m.height - chrome
	if m.height == 0 || budget < 1 {
		// No size known yet (or a terminal too small to reason about): render
		// everything and let the terminal deal with it.
		return strings.Join(blocks, "")
	}

	// Grow outward from the focused panel while there is room. The focused panel
	// is always included even if it alone overflows -- better to show the thing
	// being edited than nothing.
	first, last := focusCtrl, focusCtrl
	used := costs[focusCtrl]
	for {
		grew := false
		if last+1 < len(blocks) && used+costs[last+1] <= budget {
			used += costs[last+1]
			last++
			grew = true
		}
		if first-1 >= 0 && used+costs[first-1] <= budget {
			used += costs[first-1]
			first--
			grew = true
		}
		if !grew {
			break
		}
	}

	var b strings.Builder
	if first > 0 {
		b.WriteString(styleDim.Render(fmt.Sprintf("↑ %d more above", first)) + "\n")
	}
	for i := first; i <= last; i++ {
		b.WriteString(blocks[i])
	}
	if last < len(blocks)-1 {
		b.WriteString(styleDim.Render(
			fmt.Sprintf("↓ %d more below", len(blocks)-1-last)) + "\n")
	}
	return b.String()
}

func (m Model) renderControl(idx int, c proto.Control) string {
	label := c.Label
	if label == "" {
		label = c.ID
	}

	var b strings.Builder
	b.WriteString(styleTitle.Render(label) + "\n")
	for fi, f := range c.Fields {
		focused, ch := m.focusOn(idx, fi)
		b.WriteString(renderField(f, m.values[c.ID][f.Key], focused, ch) + "\n")
	}
	if c.ID == simID {
		b.WriteString("\n" + m.renderFrame() + "\n")
	}
	return stylePanel.Render(strings.TrimRight(b.String(), "\n")) + "\n"
}

// focusOn reports whether the given field holds focus, and which color channel.
func (m Model) focusOn(ctrlIdx, fieldIdx int) (bool, int) {
	if m.focus >= len(m.targets) {
		return false, -1
	}
	t := m.targets[m.focus]
	if t.ctrl != ctrlIdx || t.field != fieldIdx {
		return false, -1
	}
	return true, t.ch
}

func (m Model) renderStatus() string {
	if m.statusE {
		return styleErr.Render("● " + m.status)
	}
	return styleOK.Render("● ") + styleDim.Render(m.status)
}
