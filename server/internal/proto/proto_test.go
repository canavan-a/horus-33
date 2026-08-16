package proto

import (
	"bytes"
	"encoding/json"
	"errors"
	"testing"
)

func TestEncodeAddsTagAndNewline(t *testing.T) {
	line, err := Encode(Set{Seq: 7, ID: "led", V: Values{"rate_ms": 250}})
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	if !bytes.HasSuffix(line, []byte("\n")) {
		t.Errorf("encoded line missing trailing newline: %q", line)
	}
	if bytes.Count(line, []byte("\n")) != 1 {
		t.Errorf("encoded line has embedded newlines: %q", line)
	}

	var got map[string]any
	if err := json.Unmarshal(bytes.TrimSpace(line), &got); err != nil {
		t.Fatalf("re-parse: %v", err)
	}
	if got["t"] != TSet {
		t.Errorf("tag = %v, want %q", got["t"], TSet)
	}
	if got["id"] != "led" {
		t.Errorf("id = %v, want led", got["id"])
	}
}

func TestRoundTrip(t *testing.T) {
	cases := []Msg{
		Describe{Seq: 1},
		Set{Seq: 2, ID: "led", V: Values{"mode": "blink"}},
		Ping{Seq: 3},
		Hello{Proto: Version, Device: "horus-33", FW: "abc123"},
		State{ID: "led", V: Values{"brightness": float64(64)}},
		Ack{Seq: 4},
		Err{Seq: 5, Msg: "unknown control"},
		Descriptor{Controls: []Control{{
			ID:     "led",
			Label:  "Status LED",
			Fields: []Field{{Key: "mode", Type: FEnum, Options: []string{"off", "solid"}}},
		}}},
	}

	for _, want := range cases {
		line, err := Encode(want)
		if err != nil {
			t.Fatalf("Encode(%T): %v", want, err)
		}
		got, err := Decode(line)
		if err != nil {
			t.Fatalf("Decode(%T): %v", want, err)
		}
		if got.Type() != want.Type() {
			t.Errorf("type = %q, want %q", got.Type(), want.Type())
		}
		// Compare via re-encoding: Values round-trips through JSON's number type,
		// so a direct DeepEqual would compare int against float64.
		reGot, _ := Encode(got)
		if !bytes.Equal(line, reGot) {
			t.Errorf("%T round-trip mismatch:\n got %s\nwant %s", want, reGot, line)
		}
	}
}

func TestDecodeUnknownTag(t *testing.T) {
	_, err := Decode([]byte(`{"t":"telemetry","v":1}`))
	var unknown ErrUnknownType
	if !errors.As(err, &unknown) {
		t.Fatalf("err = %v, want ErrUnknownType", err)
	}
	if unknown.Tag != "telemetry" {
		t.Errorf("Tag = %q, want telemetry", unknown.Tag)
	}
}

func TestDecodeGarbage(t *testing.T) {
	if _, err := Decode([]byte("not json at all")); err == nil {
		t.Fatal("expected error for unparseable line")
	}
}

func TestDecodeTrailingNewlineOptional(t *testing.T) {
	for _, line := range []string{`{"t":"ack","seq":9}`, "{\"t\":\"ack\",\"seq\":9}\n"} {
		m, err := Decode([]byte(line))
		if err != nil {
			t.Fatalf("Decode(%q): %v", line, err)
		}
		if m.(Ack).Seq != 9 {
			t.Errorf("Seq = %d, want 9", m.(Ack).Seq)
		}
	}
}

func f64(v float64) *float64 { return &v }

func TestFieldClamp(t *testing.T) {
	f := Field{Min: f64(50), Max: f64(5000)}
	for _, tc := range []struct{ in, want float64 }{
		{10, 50}, {50, 50}, {500, 500}, {5000, 5000}, {9999, 5000},
	} {
		if got := f.Clamp(tc.in); got != tc.want {
			t.Errorf("Clamp(%v) = %v, want %v", tc.in, got, tc.want)
		}
	}

	// A field with no bounds must not alter the value.
	unbounded := Field{}
	if got := unbounded.Clamp(-1e9); got != -1e9 {
		t.Errorf("unbounded Clamp altered value: %v", got)
	}
}

func TestFieldStepSizeDefaultsToOne(t *testing.T) {
	if got := (Field{}).StepSize(); got != 1 {
		t.Errorf("StepSize() = %v, want 1", got)
	}
	if got := (Field{Step: f64(0)}).StepSize(); got != 1 {
		t.Errorf("StepSize() with zero step = %v, want 1", got)
	}
	if got := (Field{Step: f64(25)}).StepSize(); got != 25 {
		t.Errorf("StepSize() = %v, want 25", got)
	}
}

func TestControlDefaults(t *testing.T) {
	c := Control{Fields: []Field{
		{Key: "mode", Default: "blink"},
		{Key: "rate_ms", Default: float64(500)},
		{Key: "nodefault"},
	}}
	d := c.Defaults()
	if d["mode"] != "blink" || d["rate_ms"] != float64(500) {
		t.Errorf("Defaults() = %v", d)
	}
	if _, ok := d["nodefault"]; ok {
		t.Error("field without a default should be absent from Defaults()")
	}
}

func TestControlFieldLookup(t *testing.T) {
	c := Control{Fields: []Field{{Key: "color", Type: FColor}}}
	if f, ok := c.Field("color"); !ok || f.Type != FColor {
		t.Errorf("Field(color) = %v, %v", f, ok)
	}
	if _, ok := c.Field("missing"); ok {
		t.Error("Field(missing) should report not found")
	}
}

func TestParseRGB(t *testing.T) {
	for _, in := range []string{"#0000ff", "0000ff"} {
		c, err := ParseRGB(in)
		if err != nil {
			t.Fatalf("ParseRGB(%q): %v", in, err)
		}
		if (c != RGB{0, 0, 255}) {
			t.Errorf("ParseRGB(%q) = %v", in, c)
		}
	}
	if got := (RGB{0, 0, 255}).String(); got != "#0000ff" {
		t.Errorf("String() = %q", got)
	}
	if _, err := ParseRGB("#fff"); err == nil {
		t.Error("expected error for short hex")
	}
}

func TestRGBChannels(t *testing.T) {
	c := RGB{10, 20, 30}
	for i, want := range []uint8{10, 20, 30} {
		if got := c.Channel(i); got != want {
			t.Errorf("Channel(%d) = %d, want %d", i, got, want)
		}
	}
	if got := c.WithChannel(1, 99); (got != RGB{10, 99, 30}) {
		t.Errorf("WithChannel(1,99) = %v", got)
	}
}

func TestFieldSnapAndFormat(t *testing.T) {
	f64 := func(v float64) *float64 { return &v }
	fine := Field{Key: "deadband", Type: FNumber, Min: f64(0), Max: f64(0.5), Step: f64(0.05)}
	coarse := Field{Key: "pos", Type: FNumber, Min: f64(-1000000), Max: f64(1000000), Step: f64(10)}

	if got := fine.Decimals(); got != 2 {
		t.Errorf("Decimals(step 0.05) = %d, want 2", got)
	}
	if got := coarse.Decimals(); got != 0 {
		t.Errorf("Decimals(step 10) = %d, want 0", got)
	}

	// Three steps of 0.05 accumulate to 0.15000000000000002. Built in a loop
	// rather than as a constant expression, which Go would fold exactly.
	drifted := 0.0
	for i := 0; i < 3; i++ {
		drifted += fine.StepSize()
	}
	if drifted == 0.15 {
		t.Fatal("expected accumulated float drift to reproduce")
	}
	if got := fine.Snap(drifted); got != 0.15 {
		t.Errorf("Snap(%v) = %v, want 0.15", drifted, got)
	}
	if got := fine.Format(drifted); got != "0.15" {
		t.Errorf("Format(%v) = %q, want \"0.15\"", drifted, got)
	}

	// Large bounded values must not fall into scientific notation.
	if got := coarse.Format(1000000); got != "1000000" {
		t.Errorf("Format(1e6) = %q, want \"1000000\"", got)
	}

	// Coerce snaps too, so the fake device echoes clean values like the real one.
	v, err := Coerce(fine, drifted)
	if err != nil || v != 0.15 {
		t.Errorf("Coerce = %v, %v; want 0.15", v, err)
	}
}
