// Package api implements horus-server's REST + WebSocket surface over a
// link.Link to capture-eye's control relay. The API is descriptor-driven: the
// device advertises its own controls, and this package has no compiled-in
// knowledge of what they are — see PatchControl and proto.Coerce.
package api

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/canavan-a/horus-33/server/internal/clipadmin"
	"github.com/canavan-a/horus-33/server/internal/link"
	"github.com/canavan-a/horus-33/server/internal/proto"
)

// LinkStatus mirrors the phases horusctl's UI already tracks
// (tui-controller/internal/ui/model.go), so a browser client can render the
// same status line.
type LinkStatus string

const (
	StatusConnecting LinkStatus = "connecting"
	StatusDescribing LinkStatus = "describing"
	StatusReady      LinkStatus = "ready"
	StatusLost       LinkStatus = "lost"
)

// maxInFlight bounds unacked commands. The firmware's inbound queue is 8 deep
// and drops silently on overflow (firmware/src/main.cpp:58,185); staying well
// under that keeps a burst of PATCHes from ever hitting it.
const maxInFlight = 4

// ackTimeout is how long SendAndWait waits for a matching ack/err before
// giving up. The relay's own seq rewriting (capture-eye/src/control_relay.cpp)
// guarantees replies are routed back correctly; this timeout only covers a
// genuinely unresponsive or disconnected device.
const ackTimeout = 1 * time.Second

// Server owns one link to capture-eye's control relay and the state derived
// from it: link status, the cached descriptor, and the last known value of
// every control.
type Server struct {
	lnk link.Link

	mu         sync.RWMutex
	status     LinkStatus
	lastErr    string
	hello      *proto.Hello
	descriptor proto.Descriptor
	haveDesc   bool
	state      map[string]proto.Values

	pendingMu sync.Mutex
	nextSeq   uint64
	pending   map[uint64]chan pendingResult
	inFlight  chan struct{}

	hub *hub

	// Presence: a debounced "person in frame" signal derived from the track
	// stream capture-eye now forwards to relay clients. notifyEnabled gates
	// whether edges are broadcast (POST /api/notify/subscribe); the detector
	// keeps running regardless so the snapshot stays current.
	presence      *presenceDetector
	notifyEnabled atomic.Bool

	// Replay-on-hello: the firmware persists nothing (verified against
	// firmware/src — no NVS/EEPROM/Preferences anywhere), so every setting
	// reverts to compiled defaults on reset. Persisting the last value a
	// client asked for and replaying it when hello arrives means a board
	// reset does not silently discard PID tuning. Empty replayPath disables
	// this entirely.
	replayPath string
	desiredMu  sync.Mutex
	desired    map[string]proto.Values

	// Clipping: entirely orthogonal to the ESP32 link above. clipAdmin is nil
	// when --clip-admin-socket was not given (clip listing/serving can still
	// work; live toggling and status just aren't available). clipsDir empty
	// disables listing/serving too.
	clipAdmin *clipadmin.Client
	clipsDir  string
}

type pendingResult struct {
	ack *proto.Ack
	err *proto.Err
}

// New starts consuming lnk's events immediately. replayPath may be empty to
// disable state replay. clipAdminSocket/clipsDir may both be empty, in which
// case clipping support is simply absent from this server's API surface.
func New(lnk link.Link, replayPath string, clipAdminSocket string, clipsDir string) *Server {
	s := &Server{
		lnk:        lnk,
		status:     StatusConnecting,
		state:      make(map[string]proto.Values),
		pending:    make(map[uint64]chan pendingResult),
		inFlight:   make(chan struct{}, maxInFlight),
		hub:        newHub(),
		replayPath: replayPath,
		desired:    make(map[string]proto.Values),
		clipsDir:   clipsDir,
	}
	if clipAdminSocket != "" {
		s.clipAdmin = clipadmin.New(clipAdminSocket)
	}
	s.presence = newPresenceDetector(func(ev PresenceEvent) {
		if s.notifyEnabled.Load() {
			s.hub.broadcast(wsEvent{Type: "presence", Presence: &ev})
		}
	})
	s.notifyEnabled.Store(true)
	s.loadDesired()
	go s.pump()
	go s.presenceTicker()
	if s.clipAdmin != nil {
		go s.pollClipping()
	}
	return s
}

// clipStatusPollInterval trades a few seconds of UI staleness on the
// "recording" indicator for keeping the admin protocol pure request/response
// — no unsolicited push from capture-eye to handle.
const clipStatusPollInterval = 3 * time.Second

func (s *Server) pollClipping() {
	var last clipadmin.Status
	haveLast := false
	for {
		time.Sleep(clipStatusPollInterval)
		status, err := s.clipAdmin.Status()
		if err != nil {
			continue // capture-eye restarting, or clipping off; try again next tick
		}
		if !haveLast || status != last {
			haveLast = true
			last = status
			s.hub.broadcast(wsEvent{Type: "clipping", Clipping: &ClippingStatus{
				Enabled: status.Enabled, Recording: status.Recording,
			}})
		}
	}
}

// presenceTicker catches the track stream stopping without an explicit `lost`
// (capture-eye crash, link loss) — observe() alone would leave presence stuck
// "true" forever in that case.
func (s *Server) presenceTicker() {
	for {
		time.Sleep(presenceTick)
		s.presence.tick()
	}
}

// SetNotifyEnabled toggles whether presence edges are broadcast over the hub.
// The detector keeps running either way, so the WS initial-burst snapshot is
// always current.
func (s *Server) SetNotifyEnabled(enabled bool) { s.notifyEnabled.Store(enabled) }

// NotifyEnabled reports the current presence-broadcast gate.
func (s *Server) NotifyEnabled() bool { return s.notifyEnabled.Load() }

// PresenceSnapshot is the current debounced presence state.
func (s *Server) PresenceSnapshot() PresenceEvent { return s.presence.snapshot() }

// ClippingStatus is the JSON shape exposed over REST/WS — kept as its own
// type (rather than reusing clipadmin.Status directly) so this package's
// wire format doesn't change just because the admin socket's internal client
// does.
type ClippingStatus struct {
	Enabled   bool `json:"enabled"`
	Recording bool `json:"recording"`
}

// ClippingStatus dials capture-eye's admin socket on demand — call frequency
// here is low (REST polling, occasional toggles), so there is no reason to
// hold a persistent connection open.
func (s *Server) ClippingStatus() (ClippingStatus, error) {
	if s.clipAdmin == nil {
		return ClippingStatus{}, errors.New("clip admin not configured")
	}
	status, err := s.clipAdmin.Status()
	if err != nil {
		return ClippingStatus{}, err
	}
	return ClippingStatus{Enabled: status.Enabled, Recording: status.Recording}, nil
}

func (s *Server) SetClippingEnabled(enabled bool) (ClippingStatus, error) {
	if s.clipAdmin == nil {
		return ClippingStatus{}, errors.New("clip admin not configured")
	}
	status, err := s.clipAdmin.SetEnabled(enabled)
	if err != nil {
		return ClippingStatus{}, err
	}
	return ClippingStatus{Enabled: status.Enabled, Recording: status.Recording}, nil
}

// ClipInfo is one entry in GET /api/clips.
type ClipInfo struct {
	Name      string    `json:"name"`
	Size      int64     `json:"size"`
	ModTime   time.Time `json:"modTime"`
	Thumbnail bool      `json:"thumbnail"`
}

// ClipsPage is the paginated response of GET /api/clips: one window of the
// mod-time-sorted list plus the total count so the client knows when to stop.
type ClipsPage struct {
	Clips []ClipInfo `json:"clips"`
	Total int        `json:"total"`
}

// thumbnailPath is capture-eye's own convention (clip_sink.cpp's
// generate_thumbnail): the same basename as the clip, .jpg instead of .mp4.
func thumbnailPath(clipPath string) string {
	return strings.TrimSuffix(clipPath, filepath.Ext(clipPath)) + ".jpg"
}

// ListClips reads capture-eye's clips directory directly off disk — both
// processes run on the same host (see nix/module.nix's clipsDir option), so
// there is no reason to proxy this through capture-eye at all.
func (s *Server) ListClips(offset, limit int) (ClipsPage, error) {
	if s.clipsDir == "" {
		return ClipsPage{}, errors.New("clips directory not configured")
	}
	entries, err := os.ReadDir(s.clipsDir)
	if err != nil {
		if os.IsNotExist(err) {
			return ClipsPage{Clips: []ClipInfo{}}, nil // nothing recorded yet
		}
		return ClipsPage{}, err
	}
	clips := make([]ClipInfo, 0, len(entries))
	for _, e := range entries {
		if e.IsDir() || filepath.Ext(e.Name()) != ".mp4" {
			continue // skip .mp4.tmp (in-progress) and anything else
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		fullPath := filepath.Join(s.clipsDir, e.Name())
		_, thumbErr := os.Stat(thumbnailPath(fullPath))
		clips = append(clips, ClipInfo{
			Name:      e.Name(),
			Size:      info.Size(),
			ModTime:   info.ModTime(),
			Thumbnail: thumbErr == nil,
		})
	}
	sort.Slice(clips, func(i, j int) bool { return clips[i].ModTime.After(clips[j].ModTime) })

	total := len(clips)
	if offset > total {
		offset = total
	}
	end := total
	if limit > 0 && offset+limit < end {
		end = offset + limit
	}
	return ClipsPage{Clips: clips[offset:end], Total: total}, nil
}

// ClipsDir exposes the configured directory so routes.go can join and
// validate a requested filename against it.
func (s *Server) ClipsDir() string { return s.clipsDir }

// DeleteClip removes a clip and its thumbnail (best-effort; a missing
// thumbnail — never generated, or already gone — is not an error). name must
// already have passed routes.go's clipNamePattern check.
func (s *Server) DeleteClip(name string) error {
	if s.clipsDir == "" {
		return errors.New("clips directory not configured")
	}
	fullPath := filepath.Join(s.clipsDir, name)
	if err := os.Remove(fullPath); err != nil {
		return err
	}
	if err := os.Remove(thumbnailPath(fullPath)); err != nil && !os.IsNotExist(err) {
		log.Printf("delete clip: thumbnail for %s: %v", name, err)
	}
	return nil
}

func (s *Server) pump() {
	for ev := range s.lnk.Events() {
		switch {
		case ev.Connected:
			s.setStatus(StatusDescribing, "")
			// Ask immediately; nothing else populates the descriptor.
			_ = s.lnk.Send(proto.Describe{Seq: s.allocSeq()})

		case ev.Disconnected:
			msg := ""
			if ev.Err != nil {
				msg = ev.Err.Error()
			}
			s.setStatus(StatusLost, msg)

		case ev.Note != "":
			log.Printf("link: %s", ev.Note)

		case ev.Msg != nil:
			s.handleMsg(ev.Msg)
		}
	}
}

func (s *Server) handleMsg(m proto.Msg) {
	switch v := m.(type) {
	case proto.Hello:
		s.mu.Lock()
		s.hello = &v
		s.mu.Unlock()
		s.hub.broadcast(wsEvent{Type: "hello", Hello: &v})
		s.replayDesired()

	case proto.Descriptor:
		s.mu.Lock()
		s.descriptor = v
		s.haveDesc = true
		s.status = StatusReady
		// A stale error from a prior disconnect (e.g. the startup race before
		// capture-eye has created the relay socket) must not linger once the
		// link has actually recovered — otherwise /api/link keeps reporting
		// an error a client would reasonably read as still-current.
		s.lastErr = ""
		s.mu.Unlock()
		s.hub.broadcast(wsEvent{Type: "descriptor", Descriptor: &v})
		s.hub.broadcast(wsEvent{Type: "link", Status: string(StatusReady)})

	case proto.State:
		s.mu.Lock()
		s.state[v.ID] = v.V
		s.mu.Unlock()
		s.hub.broadcast(wsEvent{Type: "state", State: &v})

	case proto.Track:
		// capture-eye forwards its own outgoing track stream to relay clients
		// (ControlRelay::publish_local); fold it into the presence signal.
		s.presence.observe(v)

	case proto.Ack:
		s.completePending(v.Seq, pendingResult{ack: &v})

	case proto.Err:
		s.completePending(v.Seq, pendingResult{err: &v})
		s.hub.broadcast(wsEvent{Type: "error", Error: &v})
	}
}

func (s *Server) setStatus(status LinkStatus, lastErr string) {
	s.mu.Lock()
	s.status = status
	if lastErr != "" {
		s.lastErr = lastErr
	}
	s.mu.Unlock()
	s.hub.broadcast(wsEvent{Type: "link", Status: string(status), Error: errToErrMsg(lastErr)})
}

func errToErrMsg(s string) *proto.Err {
	if s == "" {
		return nil
	}
	return &proto.Err{Msg: s}
}

func (s *Server) allocSeq() uint64 {
	s.pendingMu.Lock()
	defer s.pendingMu.Unlock()
	s.nextSeq++
	return s.nextSeq
}

// SendAndWait sends m (which must carry the seq allocSeq gave it) and blocks
// for a matching ack or err, bounded by ackTimeout and by maxInFlight.
func (s *Server) SendAndWait(seq uint64, m proto.Msg) (pendingResult, error) {
	select {
	case s.inFlight <- struct{}{}:
	case <-time.After(ackTimeout):
		return pendingResult{}, errors.New("too many commands in flight")
	}
	defer func() { <-s.inFlight }()

	ch := make(chan pendingResult, 1)
	s.pendingMu.Lock()
	s.pending[seq] = ch
	s.pendingMu.Unlock()
	defer func() {
		s.pendingMu.Lock()
		delete(s.pending, seq)
		s.pendingMu.Unlock()
	}()

	if err := s.lnk.Send(m); err != nil {
		return pendingResult{}, fmt.Errorf("send: %w", err)
	}

	select {
	case result := <-ch:
		return result, nil
	case <-time.After(ackTimeout):
		return pendingResult{}, errors.New("device did not respond in time")
	}
}

func (s *Server) completePending(seq uint64, result pendingResult) {
	s.pendingMu.Lock()
	ch, ok := s.pending[seq]
	s.pendingMu.Unlock()
	if !ok {
		return // unsolicited or already timed out
	}
	select {
	case ch <- result:
	default:
	}
}

// --- snapshot accessors for the REST handlers ---

type LinkSnapshot struct {
	Status LinkStatus   `json:"status"`
	Error  string       `json:"error,omitempty"`
	Hello  *proto.Hello `json:"hello,omitempty"`
}

func (s *Server) LinkSnapshot() LinkSnapshot {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return LinkSnapshot{Status: s.status, Error: s.lastErr, Hello: s.hello}
}

func (s *Server) DescriptorSnapshot() (proto.Descriptor, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.descriptor, s.haveDesc
}

func (s *Server) StateSnapshot() map[string]proto.Values {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make(map[string]proto.Values, len(s.state))
	for id, v := range s.state {
		out[id] = v
	}
	return out
}

func (s *Server) ControlField(id, key string) (proto.Field, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, c := range s.descriptor.Controls {
		if c.ID == id {
			return c.Field(key)
		}
	}
	return proto.Field{}, false
}

func (s *Server) HasControl(id string) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	for _, c := range s.descriptor.Controls {
		if c.ID == id {
			return true
		}
	}
	return false
}

// --- replay-on-hello ---

func (s *Server) rememberDesired(id string, v proto.Values) {
	if s.replayPath == "" {
		return
	}
	s.desiredMu.Lock()
	merged := s.desired[id]
	if merged == nil {
		merged = proto.Values{}
	}
	for k, val := range v {
		merged[k] = val
	}
	s.desired[id] = merged
	snapshot := make(map[string]proto.Values, len(s.desired))
	for k, val := range s.desired {
		snapshot[k] = val
	}
	s.desiredMu.Unlock()

	data, err := json.MarshalIndent(snapshot, "", "  ")
	if err != nil {
		log.Printf("replay: marshal: %v", err)
		return
	}
	if err := os.WriteFile(s.replayPath, data, 0o644); err != nil {
		log.Printf("replay: write %s: %v", s.replayPath, err)
	}
}

func (s *Server) loadDesired() {
	if s.replayPath == "" {
		return
	}
	data, err := os.ReadFile(s.replayPath)
	if err != nil {
		return // cold start; not an error
	}
	var loaded map[string]proto.Values
	if err := json.Unmarshal(data, &loaded); err != nil {
		log.Printf("replay: %s is not valid JSON, ignoring: %v", s.replayPath, err)
		return
	}
	s.desiredMu.Lock()
	s.desired = loaded
	s.desiredMu.Unlock()
}

func (s *Server) replayDesired() {
	if s.replayPath == "" {
		return
	}
	s.desiredMu.Lock()
	snapshot := make(map[string]proto.Values, len(s.desired))
	for id, v := range s.desired {
		snapshot[id] = v
	}
	s.desiredMu.Unlock()

	for id, v := range snapshot {
		seq := s.allocSeq()
		if _, err := s.SendAndWait(seq, proto.Set{Seq: seq, ID: id, V: v}); err != nil {
			log.Printf("replay: %s: %v", id, err)
		}
	}
}

// Hub exposes the WebSocket hub for the HTTP layer to upgrade connections into.
func (s *Server) Hub() *hub { return s.hub }
