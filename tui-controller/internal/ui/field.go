package ui

import (
	"fmt"
	"strings"

	"github.com/canavan-a/horus-33/tui-controller/internal/proto"
	"github.com/charmbracelet/lipgloss"
)

// target identifies one focusable thing on screen. A color field contributes
// three targets (one per channel); every other field contributes one.
type target struct {
	ctrl  int
	field int
	ch    int // color channel 0..2, or -1 for non-color fields
}

var channelNames = [3]string{"R", "G", "B"}

// targetsFor expands a control's fields into focusable targets.
func targetsFor(ctrlIdx int, c proto.Control) []target {
	var out []target
	for i, f := range c.Fields {
		if f.Type == proto.FColor {
			for ch := 0; ch < 3; ch++ {
				out = append(out, target{ctrl: ctrlIdx, field: i, ch: ch})
			}
			continue
		}
		out = append(out, target{ctrl: ctrlIdx, field: i, ch: -1})
	}
	return out
}

// adjust returns the field's value stepped by delta (+1 / -1 units). ok is false
// when the field type has no meaningful adjustment, or the value is malformed.
func adjust(f proto.Field, cur any, delta int, ch int) (next any, ok bool) {
	switch f.Type {
	case proto.FNumber:
		n, isNum := proto.Num(cur)
		if !isNum {
			return nil, false
		}
		// Snap as well as clamp: the stepped value goes on the wire, so the
		// device must not receive accumulated binary-float drift either.
		return f.Snap(f.Clamp(n + float64(delta)*f.StepSize())), true

	case proto.FColor:
		s, isStr := cur.(string)
		if !isStr {
			return nil, false
		}
		c, err := proto.ParseRGB(s)
		if err != nil {
			return nil, false
		}
		// Step channels by 5 so a full sweep is a reasonable number of presses;
		// clamp at the byte boundaries rather than wrapping.
		v := int(c.Channel(ch)) + delta*5
		if v < 0 {
			v = 0
		}
		if v > 255 {
			v = 255
		}
		return c.WithChannel(ch, uint8(v)).String(), true

	case proto.FEnum:
		s, isStr := cur.(string)
		if !isStr || len(f.Options) == 0 {
			return nil, false
		}
		idx := 0
		for i, opt := range f.Options {
			if opt == s {
				idx = i
				break
			}
		}
		// Wrap, so cycling through modes never dead-ends.
		idx = (idx + delta + len(f.Options)*2) % len(f.Options)
		return f.Options[idx], true

	case proto.FBool:
		b, isBool := cur.(bool)
		if !isBool {
			return nil, false
		}
		return !b, true
	}
	return nil, false
}

// renderField draws one field row. focusedCh is the focused color channel, or -1
// when the field is not focused at all.
func renderField(f proto.Field, cur any, focused bool, focusedCh int) string {
	label := f.Label
	if label == "" {
		label = f.Key
	}

	switch f.Type {
	case proto.FNumber:
		n, ok := proto.Num(cur)
		if !ok {
			return row(label, styleDim.Render("--"), focused)
		}
		val := f.Format(n)
		if f.Unit != "" {
			val += " " + f.Unit
		}
		return row(label, arrows(val, focused)+"  "+bar(f, n), focused)

	case proto.FColor:
		s, _ := cur.(string)
		c, err := proto.ParseRGB(s)
		if err != nil {
			return row(label, styleDim.Render("--"), focused)
		}
		var parts []string
		for ch := 0; ch < 3; ch++ {
			chFocused := focused && focusedCh == ch
			txt := fmt.Sprintf("%s %3d", channelNames[ch], c.Channel(ch))
			if chFocused {
				txt = styleFocus.Render("‹" + txt + "›")
			} else {
				txt = " " + styleDim.Render(txt) + " "
			}
			parts = append(parts, txt)
		}
		swatch := lipgloss.NewStyle().
			Background(lipgloss.Color(c.String())).Render("      ")
		return row(label, strings.Join(parts, " ")+" "+swatch+" "+styleDim.Render(c.String()), focused)

	case proto.FEnum:
		s, _ := cur.(string)
		var parts []string
		for _, opt := range f.Options {
			if opt == s {
				parts = append(parts, styleSelected.Render(opt))
				continue
			}
			parts = append(parts, styleDim.Render(opt))
		}
		return row(label, strings.Join(parts, styleDim.Render(" · ")), focused)

	case proto.FBool:
		b, _ := cur.(bool)
		txt := "off"
		if b {
			txt = "on"
		}
		return row(label, arrows(txt, focused), focused)
	}

	// Unknown field type: show it read-only rather than failing, so this host
	// stays usable against firmware that grew a new type.
	return row(label, styleDim.Render(fmt.Sprintf("%v (unsupported type %q)", cur, f.Type)), focused)
}

// arrows brackets a value with focus indicators.
func arrows(val string, focused bool) string {
	if !focused {
		return "  " + val
	}
	return styleFocus.Render("‹ " + val + " ›")
}

// bar draws a proportional meter for a bounded numeric field.
func bar(f proto.Field, n float64) string {
	if f.Min == nil || f.Max == nil || *f.Max <= *f.Min {
		return ""
	}
	const width = 20
	frac := (n - *f.Min) / (*f.Max - *f.Min)
	filled := int(frac*float64(width) + 0.5)
	if filled < 0 {
		filled = 0
	}
	if filled > width {
		filled = width
	}
	return styleBar.Render(strings.Repeat("█", filled)) +
		styleDim.Render(strings.Repeat("░", width-filled))
}

func row(label, value string, focused bool) string {
	marker := "  "
	if focused {
		marker = styleFocus.Render("▸ ")
	}
	return marker + styleLabel.Render(fmt.Sprintf("%-12s", label)) + value
}
