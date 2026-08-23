package hostconfig

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLoadMissingFileIsEmptyNotAnError(t *testing.T) {
	doc, err := Load(filepath.Join(t.TempDir(), "absent.json"))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if doc.Existed() {
		t.Error("a file that is not there should not report as existing")
	}
	if _, err := doc.Get("capture.device"); err == nil {
		t.Error("expected an unset key to report as unset")
	}
}

// The point of the whole generic-JSON approach: keys horusctl has never heard
// of must come back out exactly as they went in.
func TestUnknownKeysSurviveAnEdit(t *testing.T) {
	path := write(t, `{"capture":{"device":"/dev/video0","width":1280},"future":{"nested":[1,2]}}`)
	doc, err := Load(path)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if err := doc.Set("capture.device", "/dev/video2"); err != nil {
		t.Fatalf("Set: %v", err)
	}
	out := reparse(t, doc)

	capture := out["capture"].(map[string]any)
	if capture["device"] != "/dev/video2" {
		t.Errorf("device = %v, want /dev/video2", capture["device"])
	}
	// UseNumber is what keeps this an integer; without it 1280 re-renders as
	// 1.28e+03 and capture-eye rejects a key nobody touched.
	if got := capture["width"].(json.Number).String(); got != "1280" {
		t.Errorf("width = %s, want 1280", got)
	}
	if _, ok := out["future"]; !ok {
		t.Error("an unrecognized top-level key was dropped")
	}
}

func TestSetCreatesIntermediateObjects(t *testing.T) {
	doc, _ := Load(filepath.Join(t.TempDir(), "new.json"))
	if err := doc.Set("serial.port", "/dev/ttyACM0"); err != nil {
		t.Fatalf("Set: %v", err)
	}
	value, err := doc.Get("serial.port")
	if err != nil || value != "/dev/ttyACM0" {
		t.Fatalf("Get = %v, %v", value, err)
	}
}

func TestSetThroughAScalarIsRefused(t *testing.T) {
	path := write(t, `{"serial":"ttyACM0"}`)
	doc, _ := Load(path)
	if err := doc.Set("serial.port", "/dev/ttyACM0"); err == nil {
		t.Error("expected an error when a path runs through a scalar")
	}
}

func TestUnsetIsIdempotent(t *testing.T) {
	path := write(t, `{"capture":{"fps":30}}`)
	doc, _ := Load(path)
	for range 2 {
		if err := doc.Unset("capture.fps"); err != nil {
			t.Fatalf("Unset: %v", err)
		}
	}
	if _, err := doc.Get("capture.fps"); err == nil {
		t.Error("key should be gone")
	}
}

func TestParseValueTypes(t *testing.T) {
	for _, tc := range []struct {
		text string
		want any
	}{
		{"true", true},
		{`"MJPG"`, "MJPG"},
		{"MJPG", "MJPG"},               // bare strings need no quoting
		{"/dev/video0", "/dev/video0"}, // nor do paths
		{"1280x720", "1280x720"},       // trailing junk must not decode as 1280
	} {
		if got := ParseValue(tc.text); got != tc.want {
			t.Errorf("ParseValue(%q) = %#v, want %#v", tc.text, got, tc.want)
		}
	}
	if got := ParseValue("30"); got.(json.Number).String() != "30" {
		t.Errorf("ParseValue(30) = %#v, want the number 30", got)
	}
}

// A store path is read-only and rebuilt on every nixos-rebuild; refusing it
// with an explanation is the whole reason this CLI exists.
func TestSaveRefusesAStorePath(t *testing.T) {
	doc, _ := Load("/nix/store/whatever-capture-eye.json")
	err := doc.SaveUnchecked()
	if err == nil || !strings.Contains(err.Error(), "seedConfigFile") {
		t.Errorf("err = %v, want a store-path refusal naming seedConfigFile", err)
	}
}

// Validation failure must leave the original file byte-for-byte intact — that
// is what makes `config set` safe to run against a live service.
func TestSaveLeavesTheFileAloneWhenValidationFails(t *testing.T) {
	original := `{"capture":{"fps":30}}`
	path := write(t, original)
	doc, _ := Load(path)
	if err := doc.Set("capture.fps", "thirty"); err != nil {
		t.Fatalf("Set: %v", err)
	}

	// A stand-in for capture-eye that rejects everything, so the test does not
	// need the real binary built to prove the rollback behavior.
	if err := doc.install(func(string) error { return os.ErrInvalid }); err == nil {
		t.Fatal("expected the failing validator to abort the save")
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != original {
		t.Errorf("file was modified despite a failed validation:\n%s", after)
	}
	// And no temp file left behind.
	entries, _ := os.ReadDir(filepath.Dir(path))
	if len(entries) != 1 {
		t.Errorf("expected only the config file to remain, got %d entries", len(entries))
	}
}

func TestValidateReportsAMissingBinary(t *testing.T) {
	if err := Validate("capture-eye-that-does-not-exist", "/dev/null"); err == nil {
		t.Fatal("expected an error")
	} else if !strings.Contains(err.Error(), "not found") {
		t.Errorf("err = %v, want a not-found error the caller can fall back on", err)
	}
}

func TestResolvePathPrecedence(t *testing.T) {
	t.Setenv(EnvPath, "/from/env.json")
	if got := ResolvePath("/from/flag.json"); got != "/from/flag.json" {
		t.Errorf("flag should win, got %s", got)
	}
	if got := ResolvePath(""); got != "/from/env.json" {
		t.Errorf("env should beat the default, got %s", got)
	}
	t.Setenv(EnvPath, "")
	if got := ResolvePath(""); got != DefaultPath {
		t.Errorf("got %s, want %s", got, DefaultPath)
	}
}

func write(t *testing.T, contents string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "capture-eye.json")
	if err := os.WriteFile(path, []byte(contents), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func reparse(t *testing.T, doc *Doc) map[string]any {
	t.Helper()
	raw, err := doc.Render()
	if err != nil {
		t.Fatal(err)
	}
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.UseNumber()
	var out map[string]any
	if err := dec.Decode(&out); err != nil {
		t.Fatal(err)
	}
	return out
}
