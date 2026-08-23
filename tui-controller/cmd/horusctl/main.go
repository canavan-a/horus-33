// Command horusctl drives a horus-33 host.
//
// With no subcommand it is a terminal UI for the device, rendering whatever
// controls the firmware advertises, so new firmware controls appear here
// without a host change. Two subcommands manage the host side instead:
//
//	horusctl config …   edit capture-eye's config file (camera, serial port)
//	horusctl service …  start/stop/restart the capture-eye unit
//
// Together those make retuning video input a one-line change on the host,
// rather than an edit to the machine's Nix config and a rebuild.
package main

import (
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/canavan-a/horus-33/tui-controller/internal/link"
	"github.com/canavan-a/horus-33/tui-controller/internal/ui"
	tea "github.com/charmbracelet/bubbletea"
)

func main() {
	// Subcommands are dispatched before flag.Parse so the TUI keeps its exact
	// existing invocation: a bare `horusctl`, or `horusctl --port ... --fake`,
	// behaves as it always has. Only a first argument that names a subcommand
	// takes the other path.
	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "config":
			if err := runConfig(os.Args[2:]); err != nil {
				fatal(err)
			}
			return
		case "service":
			if err := runService(os.Args[2:]); err != nil {
				fatal(err)
			}
			return
		}
	}

	var (
		port    = flag.String("port", "", "serial port (default: auto-detect an Espressif device)")
		baud    = flag.Int("baud", 115200, "baud rate (ignored by native USB CDC, but must be set)")
		fake    = flag.Bool("fake", false, "run against an in-process fake device, no hardware needed")
		latency = flag.Duration("fake-latency", 15*time.Millisecond, "simulated round-trip for --fake")
		list    = flag.Bool("list", false, "list candidate serial ports and exit")
	)
	flag.Usage = usage
	flag.Parse()

	if *list {
		if err := listPorts(); err != nil {
			fatal(err)
		}
		return
	}

	lk, desc, err := open(*fake, *port, *baud, *latency)
	if err != nil {
		fatal(err)
	}
	defer lk.Close()

	if _, err := tea.NewProgram(ui.New(lk, desc)).Run(); err != nil {
		fatal(err)
	}
}

func open(fake bool, port string, baud int, latency time.Duration) (link.Link, string, error) {
	if fake {
		return link.NewFake(latency), "fake device", nil
	}
	if port == "" {
		detected, err := link.AutoDetect()
		if err != nil {
			return nil, "", err
		}
		port = detected
	}
	return link.OpenSerial(port, baud), port, nil
}

func listPorts() error {
	p, err := link.AutoDetect()
	if err != nil {
		return err
	}
	fmt.Println(p)
	return nil
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "horusctl:", err)
	os.Exit(1)
}

func usage() {
	fmt.Fprint(os.Stderr, `horusctl — horus-33 device and host control

Usage:
  horusctl [flags]        terminal UI for the device (flags below)
  horusctl config ...     edit capture-eye's config: camera, serial port
  horusctl service ...    start/stop/restart the capture-eye unit

TUI flags:
`)
	flag.PrintDefaults()
}
