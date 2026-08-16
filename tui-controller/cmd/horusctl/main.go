// Command horusctl is a terminal UI for the horus-33 device. It renders
// whatever controls the device advertises, so new firmware controls appear here
// without a host change.
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
	var (
		port    = flag.String("port", "", "serial port (default: auto-detect an Espressif device)")
		baud    = flag.Int("baud", 115200, "baud rate (ignored by native USB CDC, but must be set)")
		fake    = flag.Bool("fake", false, "run against an in-process fake device, no hardware needed")
		latency = flag.Duration("fake-latency", 15*time.Millisecond, "simulated round-trip for --fake")
		list    = flag.Bool("list", false, "list candidate serial ports and exit")
	)
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
