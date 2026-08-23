package main

import (
	"errors"
	"flag"
	"fmt"
	"os"

	"github.com/canavan-a/horus-33/tui-controller/internal/service"
)

const serviceUsage = `horusctl service — control ` + service.Unit + `

Usage: horusctl service <command>

  status            what the unit is doing, with capture-eye's exit code decoded
  start             start it
  stop              stop it (a Restart=always unit stays stopped until started)
  restart           restart it — how a config change takes effect
  logs [-f] [-n N]  tail its journal

Mutating commands need root: sudo horusctl service restart
`

func runService(args []string) error {
	mgr := service.Manager{
		Runner: service.Exec{Stdout: os.Stdout, Stderr: os.Stderr},
		Out:    os.Stdout,
	}
	if len(args) == 0 {
		fmt.Print(serviceUsage)
		return nil
	}
	switch args[0] {
	case "status":
		return mgr.Status()
	case "start":
		return mgr.Start()
	case "stop":
		return mgr.Stop()
	case "restart":
		return mgr.Restart()
	case "logs":
		fs := flag.NewFlagSet("service logs", flag.ContinueOnError)
		follow := fs.Bool("f", false, "follow the journal")
		lines := fs.Int("n", 50, "how many lines of history to show")
		if err := fs.Parse(args[1:]); err != nil {
			return err
		}
		return mgr.Logs(*follow, *lines)
	case "-h", "--help", "help":
		fmt.Print(serviceUsage)
		return nil
	default:
		return errors.New("unknown service command " + args[0] + " (try: horusctl service --help)")
	}
}
