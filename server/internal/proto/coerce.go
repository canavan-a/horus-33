package proto

import "fmt"

// Coerce validates and normalises a value against its field descriptor,
// returning the value the device would store. Numbers are clamped to range,
// colors normalised to "#rrggbb", enums checked against options.
//
// Unknown field types pass through untouched so that a host built against an
// older protocol stays usable against newer firmware.
func Coerce(f Field, v any) (any, error) {
	switch f.Type {
	case FNumber:
		n, ok := toFloat(v)
		if !ok {
			return nil, fmt.Errorf("field %q: want number, got %T", f.Key, v)
		}
		return f.Snap(f.Clamp(n)), nil

	case FColor:
		s, ok := v.(string)
		if !ok {
			return nil, fmt.Errorf("field %q: want color string, got %T", f.Key, v)
		}
		c, err := ParseRGB(s)
		if err != nil {
			return nil, fmt.Errorf("field %q: %w", f.Key, err)
		}
		return c.String(), nil

	case FEnum:
		s, ok := v.(string)
		if !ok {
			return nil, fmt.Errorf("field %q: want enum string, got %T", f.Key, v)
		}
		for _, opt := range f.Options {
			if opt == s {
				return s, nil
			}
		}
		return nil, fmt.Errorf("field %q: %q is not one of %v", f.Key, s, f.Options)

	case FBool:
		b, ok := v.(bool)
		if !ok {
			return nil, fmt.Errorf("field %q: want bool, got %T", f.Key, v)
		}
		return b, nil
	}
	return v, nil
}

// toFloat accepts the numeric shapes that survive a JSON round-trip as well as
// the plain ints a Go caller is likely to write literally.
func toFloat(v any) (float64, bool) {
	switch n := v.(type) {
	case float64:
		return n, true
	case float32:
		return float64(n), true
	case int:
		return float64(n), true
	case int64:
		return float64(n), true
	case uint8:
		return float64(n), true
	}
	return 0, false
}

// Num reads a field value as a float, reporting whether it was numeric.
func Num(v any) (float64, bool) { return toFloat(v) }
