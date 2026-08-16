package ui

import (
	"math"
	"strings"
	"time"

	"github.com/canavan-a/horus-33/tui-controller/internal/proto"
)

// simID names the host-local target simulator. It looks like any other control
// in the UI, but it lives entirely on this side of the link: it is never in the
// device descriptor, and edits to it are never sent as `set`. What it does send
// is a `track` stream, so the PID loop can be exercised and tuned with no camera
// and no inference running.
const simID = "sim"

// simRate is how often a simulated detection is emitted, chosen to sit in the
// same range as a real inference pipeline.
const simRate = 30 * time.Millisecond

// Motion patterns. "manual" leaves x/y under the arrow keys, which is the one
// you want for checking direction and polarity; the rest sweep on their own so
// you can watch the loop track something moving.
const (
	simManual = "manual"
	simSweepX = "sweep-x"
	simSweepY = "sweep-y"
	simCircle = "circle"
)

func simControl() proto.Control {
	f64 := func(v float64) *float64 { return &v }
	return proto.Control{
		ID:    simID,
		Label: "Target simulator (host)",
		Fields: []proto.Field{
			{Key: "emit", Type: proto.FBool, Label: "Emit track", Default: false},
			{Key: "lost", Type: proto.FBool, Label: "Send lost", Default: false},
			{Key: "pattern", Type: proto.FEnum, Label: "Pattern",
				Options: []string{simManual, simSweepX, simSweepY, simCircle},
				Default: simManual},
			{Key: "x", Type: proto.FNumber, Label: "Target x",
				Min: f64(-1), Max: f64(1), Step: f64(0.05), Default: float64(0)},
			{Key: "y", Type: proto.FNumber, Label: "Target y",
				Min: f64(-1), Max: f64(1), Step: f64(0.05), Default: float64(0)},
			{Key: "amp", Type: proto.FNumber, Label: "Sweep amplitude",
				Min: f64(0), Max: f64(1), Step: f64(0.05), Default: float64(0.6)},
			{Key: "period", Type: proto.FNumber, Label: "Sweep period",
				Min: f64(0.5), Max: f64(60), Step: f64(0.5), Unit: "s", Default: float64(6)},
			{Key: "conf", Type: proto.FNumber, Label: "Confidence",
				Min: f64(0), Max: f64(1), Step: f64(0.05), Default: float64(0.9)},
		},
	}
}

// simFrame computes the detection to send for a given elapsed time. Pattern
// modes overwrite x/y so the panel doubles as a live readout of what is going
// out on the wire.
func (m *Model) simFrame(elapsed time.Duration) (proto.Track, bool) {
	vals, ok := m.values[simID]
	if !ok {
		return proto.Track{}, false
	}
	if emit, _ := vals["emit"].(bool); !emit {
		return proto.Track{}, false
	}

	if lost, _ := vals["lost"].(bool); lost {
		// Explicit no-detection frames: this is the path that exercises the
		// device's lost_ms timeout and return-to-home.
		return proto.Track{Lost: true}, true
	}

	num := func(key string) float64 {
		n, _ := proto.Num(vals[key])
		return n
	}

	x, y := num("x"), num("y")
	amp, period := num("amp"), num("period")
	if period <= 0 {
		period = 1
	}
	phase := 2 * math.Pi * elapsed.Seconds() / period

	switch vals["pattern"] {
	case simSweepX:
		x = amp * math.Sin(phase)
		y = 0
	case simSweepY:
		x = 0
		y = amp * math.Sin(phase)
	case simCircle:
		x = amp * math.Cos(phase)
		y = amp * math.Sin(phase)
	default: // simManual: x and y are whatever the operator dialled in
	}

	if vals["pattern"] != simManual {
		vals["x"] = round2(x)
		vals["y"] = round2(y)
	}

	return proto.Track{X: round2(x), Y: round2(y), Conf: num("conf")}, true
}

// round2 keeps the emitted numbers short and the on-screen readout stable;
// two decimals is far finer than the loop's deadband.
func round2(v float64) float64 { return math.Round(v*100) / 100 }

// Frame viewport size in terminal cells. Cells are roughly twice as tall as they
// are wide, so this ratio renders as an approximately square frame.
const (
	frameW = 41
	frameH = 17
)

// renderFrame draws the simulated camera frame: the shaded region is the
// device's deadband (a target inside it commands no motion), and the marker is
// the detection last put on the wire. Underneath is the literal JSON line, so
// what is on screen and what the device received cannot drift apart.
func (m Model) renderFrame() string {
	var b strings.Builder

	// Deadband comes from the device's own setting, so the shaded region always
	// reflects what the firmware will actually ignore.
	band := 0.0
	if mv, ok := m.values["motion"]; ok {
		band, _ = proto.Num(mv["deadband"])
	}

	// Cell centres in frame coordinates, so the marker lands where the maths
	// says it should rather than a cell off.
	colX := func(c int) float64 { return float64(c)/float64(frameW-1)*2 - 1 }
	rowY := func(r int) float64 { return 1 - float64(r)/float64(frameH-1)*2 }

	markCol, markRow := -1, -1
	if m.simLastOK && !m.simLast.Lost {
		markCol = int(math.Round((m.simLast.X + 1) / 2 * float64(frameW-1)))
		markRow = int(math.Round((1 - m.simLast.Y) / 2 * float64(frameH-1)))
	}

	for r := 0; r < frameH; r++ {
		row := make([]rune, frameW)
		for c := 0; c < frameW; c++ {
			switch {
			// The epsilon keeps a cell sitting exactly on the boundary from being
			// dropped by float error, which would render the box off-centre.
			case math.Abs(colX(c)) <= band+1e-9 && math.Abs(rowY(r)) <= band+1e-9:
				row[c] = '░' // deadband: the loop holds still in here
			case r == frameH/2 && c == frameW/2:
				row[c] = '+' // frame centre, the setpoint
			case r == frameH/2 || c == frameW/2:
				row[c] = '·' // centre lines
			default:
				row[c] = ' '
			}
		}

		if r == markRow && markCol >= 0 {
			// Style the marker on its own so it stands out against the grid.
			b.WriteString(styleDim.Render(string(row[:markCol])))
			b.WriteString(styleFocus.Render("●"))
			b.WriteString(styleDim.Render(string(row[markCol+1:])))
		} else {
			b.WriteString(styleDim.Render(string(row)))
		}
		b.WriteString("\n")
	}

	b.WriteString(styleDim.Render("sending ") + m.renderWire())
	return b.String()
}

// renderWire is the exact line the simulator last handed to the link.
func (m Model) renderWire() string {
	if !m.simLastOK {
		return styleDim.Render("— (emit is off)")
	}
	line, err := proto.Encode(m.simLast)
	if err != nil {
		return styleErr.Render(err.Error())
	}
	return styleLabel.Render(strings.TrimSpace(string(line)))
}
