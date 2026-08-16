package api

import (
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/canavan-a/horus-33/server/internal/proto"
)

// wsEvent is one push to a WebSocket client. Exactly the same shape whether it
// originated from a link status change, a descriptor, a state update, or an
// error — one JSON envelope, discriminated by Type, matching how the browser
// naturally wants to switch on incoming messages.
type wsEvent struct {
	Type       string            `json:"type"` // "link" | "hello" | "descriptor" | "state" | "error"
	Status     string            `json:"status,omitempty"`
	Hello      *proto.Hello      `json:"hello,omitempty"`
	Descriptor *proto.Descriptor `json:"descriptor,omitempty"`
	State      *proto.State      `json:"state,omitempty"`
	Error      *proto.Err        `json:"error,omitempty"`
}

// outboundCapacity bounds a client's backlog. A browser tab that stops
// reading (backgrounded, dev tools paused on a breakpoint) must not block
// delivery to every other client — it gets dropped instead.
const outboundCapacity = 64

const writeTimeout = 5 * time.Second

var upgrader = websocket.Upgrader{
	// LAN-only, no auth (see the plan's "Decisions taken") — Origin checking
	// exists to stop a hostile *webpage* from puppeteering a browser's
	// same-origin credentials against this API, which is not a concern here
	// since there is nothing credentialed to steal. Every browser on the LAN
	// is already an equally trusted caller of the plain REST routes.
	CheckOrigin: func(*http.Request) bool { return true },
}

type client struct {
	conn *websocket.Conn
	out  chan wsEvent
}

// hub fans server-side events out to every connected WebSocket client. One
// broadcaster; per-client delivery happens on each client's own writer
// goroutine so a slow client can never stall another.
type hub struct {
	mu      sync.Mutex
	clients map[*client]struct{}
}

func newHub() *hub {
	return &hub{clients: make(map[*client]struct{})}
}

func (h *hub) broadcast(ev wsEvent) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for c := range h.clients {
		select {
		case c.out <- ev:
		default:
			// Full backlog: drop the event for this client rather than block
			// the broadcaster, which every other client is also waiting on.
		}
	}
}

// Serve upgrades the request to a WebSocket and services it until the client
// disconnects. Blocking call — meant to be the entire handler body.
func (h *hub) Serve(w http.ResponseWriter, r *http.Request, initial []wsEvent) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return // upgrader already wrote the error response
	}

	c := &client{conn: conn, out: make(chan wsEvent, outboundCapacity)}
	h.mu.Lock()
	h.clients[c] = struct{}{}
	h.mu.Unlock()

	defer func() {
		h.mu.Lock()
		delete(h.clients, c)
		h.mu.Unlock()
		_ = conn.Close()
	}()

	for _, ev := range initial {
		select {
		case c.out <- ev:
		default:
		}
	}

	// A WebSocket needs someone reading, even if this API never expects
	// client-to-server messages, or a client-initiated close (or a dead TCP
	// connection surfacing as a read error) is never noticed.
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			if _, _, err := conn.NextReader(); err != nil {
				return
			}
		}
	}()

	for {
		select {
		case ev := <-c.out:
			_ = conn.SetWriteDeadline(time.Now().Add(writeTimeout))
			if err := conn.WriteJSON(ev); err != nil {
				return
			}
		case <-done:
			return
		}
	}
}
