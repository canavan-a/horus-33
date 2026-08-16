package link

import (
	"bufio"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"github.com/canavan-a/horus-33/tui-controller/internal/proto"
	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// espVID is Espressif's USB vendor ID, used to auto-detect the board.
const espVID = "303a"

// reconnectDelay is how long to wait between reconnect attempts after the board
// drops off the bus (a reset re-enumerates the CDC port, so this is routine).
const reconnectDelay = 750 * time.Millisecond

// AutoDetect returns the port path of the first attached Espressif device.
func AutoDetect() (string, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", fmt.Errorf("enumerate serial ports: %w", err)
	}
	for _, p := range ports {
		if p.IsUSB && strings.EqualFold(p.VID, espVID) {
			return p.Name, nil
		}
	}
	return "", errors.New("no Espressif device found; pass --port explicitly")
}

// Serial is a Link backed by a serial port. It reconnects on its own, so
// resetting or replugging the board recovers without restarting the TUI.
type Serial struct {
	port   string
	baud   int
	events chan Event

	mu     sync.Mutex
	conn   serial.Port
	closed bool
	done   chan struct{}
}

// OpenSerial starts a self-reconnecting link to the named port. It returns
// immediately; connection state arrives as events.
func OpenSerial(port string, baud int) *Serial {
	s := &Serial{
		port:   port,
		baud:   baud,
		events: make(chan Event, 64),
		done:   make(chan struct{}),
	}
	go s.run()
	return s
}

func (s *Serial) Events() <-chan Event { return s.events }

func (s *Serial) run() {
	defer close(s.events)

	for {
		if s.isClosed() {
			return
		}

		conn, err := serial.Open(s.port, &serial.Mode{BaudRate: s.baud})
		if err != nil {
			s.emit(Event{Disconnected: true, Err: err})
			if !s.sleep(reconnectDelay) {
				return
			}
			continue
		}

		// A short read timeout keeps the scanner from blocking forever on a
		// silent port, so Close() is responsive.
		_ = conn.SetReadTimeout(200 * time.Millisecond)

		s.mu.Lock()
		s.conn = conn
		s.mu.Unlock()

		s.emit(Event{Connected: true})
		readErr := s.readLoop(conn)

		s.mu.Lock()
		s.conn = nil
		s.mu.Unlock()
		_ = conn.Close()

		if s.isClosed() {
			return
		}
		s.emit(Event{Disconnected: true, Err: readErr})
		if !s.sleep(reconnectDelay) {
			return
		}
	}
}

// readLoop consumes lines until the port errors out or the link is closed.
func (s *Serial) readLoop(conn serial.Port) error {
	scanner := bufio.NewScanner(conn)
	// Descriptors can be sizeable once several controls are registered.
	scanner.Buffer(make([]byte, 0, 4096), 64*1024)

	for scanner.Scan() {
		if s.isClosed() {
			return nil
		}
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		// The firmware also prints plain-text boot chatter; ignore anything that
		// is not a JSON object rather than reporting it as an error.
		if !strings.HasPrefix(line, "{") {
			continue
		}

		msg, err := proto.Decode([]byte(line))
		if err != nil {
			var unknown proto.ErrUnknownType
			if errors.As(err, &unknown) {
				s.emit(Event{Note: "ignoring unknown message type " + unknown.Tag})
				continue
			}
			s.emit(Event{Note: "bad line: " + err.Error()})
			continue
		}
		s.emit(Event{Msg: msg})
	}
	return scanner.Err()
}

func (s *Serial) Send(m proto.Msg) error {
	line, err := proto.Encode(m)
	if err != nil {
		return err
	}

	s.mu.Lock()
	conn := s.conn
	s.mu.Unlock()
	if conn == nil {
		return errors.New("not connected")
	}
	_, err = conn.Write(line)
	return err
}

func (s *Serial) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	conn := s.conn
	close(s.done)
	s.mu.Unlock()

	if conn != nil {
		// Unblocks the read loop, which then sees closed and returns.
		return conn.Close()
	}
	return nil
}

func (s *Serial) isClosed() bool {
	select {
	case <-s.done:
		return true
	default:
		return false
	}
}

// sleep waits for d, returning false if the link was closed meanwhile.
func (s *Serial) sleep(d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-s.done:
		return false
	case <-t.C:
		return true
	}
}

func (s *Serial) emit(e Event) {
	select {
	case s.events <- e:
	case <-s.done:
	}
}
