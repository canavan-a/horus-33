// Package service drives the systemd unit capture-eye runs as on a deployed
// host — start/stop/restart/status/logs — so retuning the camera or the serial
// port is one tool's job end to end rather than a config edit here and a
// remembered systemctl incantation there.
//
// Scope is deliberately the one unit: horus-capture-eye is what a config
// change affects, and horus-server/mediamtx/nginx are plain systemd units with
// no horusctl-specific behavior worth wrapping.
package service

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
)

// Unit is the systemd unit nix/module.nix defines for capture-eye.
const Unit = "horus-capture-eye.service"

// devHint is printed when systemd has never heard of the unit — the common
// case being a dev.sh checkout, where the services are plain processes.
const devHint = "no " + Unit + " on this host.\n" +
	"If this is a dev checkout, use ./dev.sh and ./dev-stop.sh instead."

// Runner executes the systemctl/journalctl commands. Real use is Exec; tests
// substitute their own.
type Runner interface {
	Run(name string, args ...string) error
	Output(name string, args ...string) (string, error)
}

// Exec runs commands for real, wiring stdout/stderr straight through so
// `horusctl service status` looks exactly like `systemctl status`.
type Exec struct {
	Stdout io.Writer
	Stderr io.Writer
}

func (e Exec) Run(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdout = e.Stdout
	cmd.Stderr = e.Stderr
	cmd.Stdin = os.Stdin // systemctl may want a polkit password
	return cmd.Run()
}

func (e Exec) Output(name string, args ...string) (string, error) {
	out, err := exec.Command(name, args...).CombinedOutput()
	return string(out), err
}

// Manager operates on Unit.
type Manager struct {
	Runner Runner
	Out    io.Writer
}

// Start, Stop and Restart are plain systemctl verbs. Stop really stops a
// Restart=always unit — systemd does not restart a unit it was told to stop —
// so no masking is involved and `start` brings it straight back.
func (m Manager) Start() error   { return m.act("start") }
func (m Manager) Stop() error    { return m.act("stop") }
func (m Manager) Restart() error { return m.act("restart") }

func (m Manager) act(verb string) error {
	if err := m.requireUnit(); err != nil {
		return err
	}
	// Echoed so a polkit or permission failure explains itself: the next thing
	// to try is the same line with sudo in front.
	fmt.Fprintf(m.Out, "› systemctl %s %s\n", verb, Unit)
	if err := m.Runner.Run("systemctl", verb, Unit); err != nil {
		return fmt.Errorf("systemctl %s failed: %w (try: sudo horusctl service %s)", verb, err, verb)
	}
	return nil
}

// Status prints `systemctl status` and, when the unit died, decodes the exit
// code against capture-eye's table (capture-eye/docs/config.md) so the usual
// failures are readable without opening the journal.
func (m Manager) Status() error {
	if err := m.requireUnit(); err != nil {
		return err
	}
	out, _ := m.Runner.Output("systemctl", "status", "--no-pager", Unit)
	fmt.Fprint(m.Out, out)
	if hint := decodeExit(out); hint != "" {
		fmt.Fprintf(m.Out, "\n› %s\n", hint)
	}
	return nil
}

// Logs tails the unit's journal. follow maps to -f, lines to -n.
func (m Manager) Logs(follow bool, lines int) error {
	if err := m.requireUnit(); err != nil {
		return err
	}
	args := []string{"-u", Unit, "-n", strconv.Itoa(lines), "--no-pager"}
	if follow {
		args = append(args, "-f")
	}
	return m.Runner.Run("journalctl", args...)
}

// requireUnit turns "systemctl is missing" and "unit is unknown" into one
// message that names the dev workflow, rather than systemd's own wording for
// a situation that is usually not an error at all.
func (m Manager) requireUnit() error {
	if _, err := exec.LookPath("systemctl"); err != nil {
		return errors.New(devHint)
	}
	out, err := m.Runner.Output("systemctl", "list-unit-files", "--no-legend", Unit)
	if err != nil || strings.TrimSpace(out) == "" {
		return errors.New(devHint)
	}
	return nil
}

// exitPattern matches systemd's own rendering, e.g.
// "Main PID: 123 (code=exited, status=11/n/a)".
var exitPattern = regexp.MustCompile(`code=exited, status=(\d+)`)

func decodeExit(status string) string {
	match := exitPattern.FindStringSubmatch(status)
	if match == nil {
		return ""
	}
	switch match[1] {
	case "0":
		return ""
	case "2":
		return "exit 2: bad config — run `horusctl config show` and `capture-eye --check-config --config <path>`"
	case "10":
		return "exit 10: no camera, or the camera rejected the requested format — `horusctl config devices`"
	case "11":
		return "exit 11: no ESP32, or the serial link failed to open — `horusctl config devices`"
	default:
		return "exit " + match[1] + ": see `horusctl service logs`"
	}
}
