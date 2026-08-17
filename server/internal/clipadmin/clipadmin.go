// Package clipadmin is a minimal client for capture-eye's clip admin socket
// (capture-eye/src/clip_admin.{h,cpp}) — a second, separate Unix socket from
// the device relay, used only to toggle clip recording live and poll its
// status. Unlike internal/link's Unix client, this is request/response only:
// no reconnect loop, no event channel. The admin protocol is deliberately
// simple (one JSON line in, one JSON line out) so a fresh dial-per-call is
// the whole client — there is nothing long-lived to maintain.
package clipadmin

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"time"
)

const dialTimeout = 1 * time.Second
const callTimeout = 2 * time.Second

// Status mirrors ClipRuntime::Status (capture-eye/src/clip_sink.h).
type Status struct {
	Enabled   bool `json:"enabled"`
	Recording bool `json:"recording"`
}

type response struct {
	OK        bool   `json:"ok"`
	Enabled   bool   `json:"enabled"`
	Recording bool   `json:"recording"`
	Error     string `json:"error"`
}

// Client dials capture-eye's admin socket fresh for every call — see the
// package doc for why that is fine here, unlike link.Unix.
type Client struct {
	path string
}

// New does not dial anything yet; every call below dials on demand. path may
// point at a socket that does not exist (clipping disabled entirely in
// capture-eye's config) — calls simply fail, which callers must treat as "no
// clip admin available", not a fatal condition.
func New(path string) *Client {
	return &Client{path: path}
}

func (c *Client) call(req any) (Status, error) {
	conn, err := net.DialTimeout("unix", c.path, dialTimeout)
	if err != nil {
		return Status{}, fmt.Errorf("dial clip admin: %w", err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(callTimeout))

	line, err := json.Marshal(req)
	if err != nil {
		return Status{}, err
	}
	line = append(line, '\n')
	if _, err := conn.Write(line); err != nil {
		return Status{}, fmt.Errorf("write clip admin: %w", err)
	}

	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 0, 4096), 64*1024)
	if !scanner.Scan() {
		if err := scanner.Err(); err != nil {
			return Status{}, fmt.Errorf("read clip admin: %w", err)
		}
		return Status{}, errors.New("clip admin closed the connection with no reply")
	}

	var resp response
	if err := json.Unmarshal(scanner.Bytes(), &resp); err != nil {
		return Status{}, fmt.Errorf("clip admin: bad reply: %w", err)
	}
	if !resp.OK {
		return Status{}, fmt.Errorf("clip admin: %s", resp.Error)
	}
	return Status{Enabled: resp.Enabled, Recording: resp.Recording}, nil
}

// Status polls capture-eye for the current clipping.enabled / recording state.
func (c *Client) Status() (Status, error) {
	return c.call(map[string]string{"cmd": "status"})
}

// SetEnabled flips clipping on or off live, no capture-eye restart needed.
func (c *Client) SetEnabled(enabled bool) (Status, error) {
	return c.call(map[string]any{"cmd": "set_enabled", "enabled": enabled})
}
