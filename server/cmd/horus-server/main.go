// horus-server exposes capture-eye's control relay (a Unix socket speaking
// the device protocol) as a REST + WebSocket API. It never touches the
// serial port directly — only capture-eye may hold that, per
// capture-eye/src/serial_port.h — and instead dials the relay socket capture-eye
// listens on when started with --control-socket.
package main

import (
	"flag"
	"log"
	"net/http"

	"github.com/canavan-a/horus-33/server/internal/api"
	"github.com/canavan-a/horus-33/server/internal/link"
)

func main() {
	socket := flag.String("socket", "/run/horus/control.sock",
		"path to capture-eye's control relay socket")
	listen := flag.String("listen", ":8080", "address to serve the API on")
	replay := flag.String("replay", "",
		"path to persist desired control values and replay them on device hello; empty disables replay")
	flag.Parse()

	lnk := link.OpenUnix(*socket)
	defer lnk.Close()

	server := api.New(lnk, *replay)

	log.Printf("horus-server: relay=%s listen=%s replay=%q", *socket, *listen, *replay)
	if err := http.ListenAndServe(*listen, server.Routes()); err != nil {
		log.Fatal(err)
	}
}
