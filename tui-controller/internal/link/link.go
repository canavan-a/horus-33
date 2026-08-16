// Package link carries protocol messages to and from a device. A Link is
// either a real serial port or an in-process fake; the UI cannot tell them
// apart.
package link

import "github.com/canavan-a/horus-33/tui-controller/internal/proto"

// Event is something that happened on the link. Exactly one field is set.
type Event struct {
	Msg proto.Msg // a decoded message from the device

	// Connected/Disconnected report transport state. Err carries the reason for
	// a disconnect, and is nil for a clean shutdown.
	Connected    bool
	Disconnected bool
	Err          error

	// Note carries a non-fatal problem (an undecodable line, an unknown tag)
	// that the UI may want to log but which does not end the session.
	Note string
}

// Link is a bidirectional message channel to one device.
type Link interface {
	// Events returns the stream of incoming events. It is closed when the link
	// is closed and will not be reopened.
	Events() <-chan Event

	// Send queues a message to the device. It returns an error only if the
	// message cannot be encoded or the link is closed; a transport hiccup
	// surfaces as a Disconnected event instead.
	Send(proto.Msg) error

	// Close shuts the link down and closes the event channel.
	Close() error
}
