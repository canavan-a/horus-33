package link

import (
	"bufio"
	"errors"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/canavan-a/horus-33/server/internal/proto"
)

// reconnectDelay mirrors tui-controller's serial link: a capture-eye restart
// re-creates the socket, so retrying on a timer recovers without intervention.
const reconnectDelay = 750 * time.Millisecond

// Unix is a Link backed by capture-eye's control relay (see
// capture-eye/src/control_relay.{h,cpp}). It reconnects on its own, so a
// capture-eye restart is transparent to callers.
type Unix struct {
	path   string
	events chan Event

	mu     sync.Mutex
	conn   net.Conn
	closed bool
	done   chan struct{}
}

// OpenUnix starts a self-reconnecting link to the relay socket at path. It
// returns immediately; connection state arrives as events.
func OpenUnix(path string) *Unix {
	u := &Unix{
		path:   path,
		events: make(chan Event, 64),
		done:   make(chan struct{}),
	}
	go u.run()
	return u
}

func (u *Unix) Events() <-chan Event { return u.events }

func (u *Unix) run() {
	defer close(u.events)

	for {
		if u.isClosed() {
			return
		}

		conn, err := net.Dial("unix", u.path)
		if err != nil {
			u.emit(Event{Disconnected: true, Err: err})
			if !u.sleep(reconnectDelay) {
				return
			}
			continue
		}

		u.mu.Lock()
		u.conn = conn
		u.mu.Unlock()

		u.emit(Event{Connected: true})
		readErr := u.readLoop(conn)

		u.mu.Lock()
		u.conn = nil
		u.mu.Unlock()
		_ = conn.Close()

		if u.isClosed() {
			return
		}
		u.emit(Event{Disconnected: true, Err: readErr})
		if !u.sleep(reconnectDelay) {
			return
		}
	}
}

// readLoop consumes lines until the socket errors out or the link is closed.
// The relay only ever forwards complete NDJSON lines (capture-eye owns the
// framing on its side), but boot-time or malformed input is handled the same
// defensive way tui-controller's serial link does, since nothing here assumes
// the relay is infallible.
func (u *Unix) readLoop(conn net.Conn) error {
	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 0, 4096), 64*1024)

	for scanner.Scan() {
		if u.isClosed() {
			return nil
		}
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		if !strings.HasPrefix(line, "{") {
			continue
		}

		msg, err := proto.Decode([]byte(line))
		if err != nil {
			var unknown proto.ErrUnknownType
			if errors.As(err, &unknown) {
				u.emit(Event{Note: "ignoring unknown message type " + unknown.Tag})
				continue
			}
			u.emit(Event{Note: "bad line: " + err.Error()})
			continue
		}
		u.emit(Event{Msg: msg})
	}
	return scanner.Err()
}

func (u *Unix) Send(m proto.Msg) error {
	line, err := proto.Encode(m)
	if err != nil {
		return err
	}

	u.mu.Lock()
	conn := u.conn
	u.mu.Unlock()
	if conn == nil {
		return errors.New("not connected")
	}
	_, err = conn.Write(line)
	return err
}

func (u *Unix) Close() error {
	u.mu.Lock()
	if u.closed {
		u.mu.Unlock()
		return nil
	}
	u.closed = true
	conn := u.conn
	close(u.done)
	u.mu.Unlock()

	if conn != nil {
		// Unblocks the read loop, which then sees closed and returns.
		return conn.Close()
	}
	return nil
}

func (u *Unix) isClosed() bool {
	select {
	case <-u.done:
		return true
	default:
		return false
	}
}

// sleep waits for d, returning false if the link was closed meanwhile.
func (u *Unix) sleep(d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-u.done:
		return false
	case <-t.C:
		return true
	}
}

func (u *Unix) emit(e Event) {
	select {
	case u.events <- e:
	case <-u.done:
	}
}
