package service

import (
	"bytes"
	"strings"
	"testing"
)

// The decoder reads systemd's own wording, so the input here is copied from a
// real `systemctl status` of a running capture-eye.
func TestDecodeExitNamesTheUsualFailures(t *testing.T) {
	for _, tc := range []struct {
		status string
		want   string
	}{
		{"Main PID: 30869 (code=exited, status=10/n/a)", "no camera"},
		{"Main PID: 30869 (code=exited, status=11/n/a)", "no ESP32"},
		{"Main PID: 30869 (code=exited, status=2/n/a)", "bad config"},
		{"Main PID: 30869 (code=exited, status=7/n/a)", "exit 7"},
		{"Active: active (running) since Fri 2026-08-21", ""},
		{"Main PID: 30869 (code=exited, status=0/SUCCESS)", ""},
	} {
		got := decodeExit(tc.status)
		if tc.want == "" {
			if got != "" {
				t.Errorf("decodeExit(%q) = %q, want nothing", tc.status, got)
			}
			continue
		}
		if !strings.Contains(got, tc.want) {
			t.Errorf("decodeExit(%q) = %q, want it to mention %q", tc.status, got, tc.want)
		}
	}
}

type fakeRunner struct {
	units string
	ran   []string
}

func (f *fakeRunner) Run(name string, args ...string) error {
	f.ran = append(f.ran, name+" "+strings.Join(args, " "))
	return nil
}

func (f *fakeRunner) Output(name string, args ...string) (string, error) {
	if len(args) > 0 && args[0] == "list-unit-files" {
		return f.units, nil
	}
	return "", nil
}

func TestRestartActsOnTheCaptureEyeUnit(t *testing.T) {
	runner := &fakeRunner{units: Unit + " enabled enabled"}
	var out bytes.Buffer
	if err := (Manager{Runner: runner, Out: &out}).Restart(); err != nil {
		t.Fatalf("Restart: %v", err)
	}
	want := "systemctl restart " + Unit
	if len(runner.ran) != 1 || runner.ran[0] != want {
		t.Errorf("ran %v, want [%q]", runner.ran, want)
	}
	// The command is echoed so a polkit denial explains what to re-run.
	if !strings.Contains(out.String(), want) {
		t.Errorf("output %q should name the command it ran", out.String())
	}
}

// On a dev.sh checkout there is no unit; that is not an error worth systemd's
// wording, so it becomes a pointer at ./dev.sh instead.
func TestUnknownUnitPointsAtTheDevScripts(t *testing.T) {
	runner := &fakeRunner{units: ""}
	err := (Manager{Runner: runner, Out: &bytes.Buffer{}}).Restart()
	if err == nil || !strings.Contains(err.Error(), "dev-stop.sh") {
		t.Errorf("err = %v, want a dev-script hint", err)
	}
	if len(runner.ran) != 0 {
		t.Errorf("nothing should have been run, got %v", runner.ran)
	}
}
