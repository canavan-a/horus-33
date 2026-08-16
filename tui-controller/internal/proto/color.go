package proto

import "fmt"

// RGB is a color field's value.
type RGB struct{ R, G, B uint8 }

// String renders the "#rrggbb" form used on the wire.
func (c RGB) String() string { return fmt.Sprintf("#%02x%02x%02x", c.R, c.G, c.B) }

// ParseRGB reads a "#rrggbb" or "rrggbb" hex string.
func ParseRGB(s string) (RGB, error) {
	h := s
	if len(h) > 0 && h[0] == '#' {
		h = h[1:]
	}
	if len(h) != 6 {
		return RGB{}, fmt.Errorf("bad color %q: want #rrggbb", s)
	}
	var c RGB
	if _, err := fmt.Sscanf(h, "%02x%02x%02x", &c.R, &c.G, &c.B); err != nil {
		return RGB{}, fmt.Errorf("bad color %q: %w", s, err)
	}
	return c, nil
}

// Channel returns one channel by index (0=R, 1=G, 2=B).
func (c RGB) Channel(i int) uint8 {
	switch i {
	case 0:
		return c.R
	case 1:
		return c.G
	default:
		return c.B
	}
}

// WithChannel returns a copy with one channel replaced.
func (c RGB) WithChannel(i int, v uint8) RGB {
	switch i {
	case 0:
		c.R = v
	case 1:
		c.G = v
	default:
		c.B = v
	}
	return c
}
