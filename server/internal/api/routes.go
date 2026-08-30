package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"

	"github.com/canavan-a/horus-33/server/internal/proto"
)

// Routes builds the full HTTP handler. Deliberately not framework-driven —
// net/http's ServeMux (Go 1.22+) already does method+pattern routing, and the
// route table below is the whole surface, so a router library would add a
// dependency without removing any code.
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /api/health", s.handleHealth)
	mux.HandleFunc("GET /api/link", s.handleLink)
	mux.HandleFunc("GET /api/descriptor", s.handleDescriptor)
	mux.HandleFunc("GET /api/state", s.handleState)
	mux.HandleFunc("PATCH /api/controls/{id}", s.handlePatchControl)
	mux.HandleFunc("POST /api/refresh", s.handleRefresh)
	mux.HandleFunc("POST /api/estop", s.handleEstop)
	mux.HandleFunc("GET /api/ws", s.handleWS)

	mux.HandleFunc("GET /api/clips", s.handleListClips)
	mux.HandleFunc("GET /api/clips/{name}", s.handleServeClip)
	mux.HandleFunc("GET /api/clips/{name}/thumbnail", s.handleServeThumbnail)
	mux.HandleFunc("DELETE /api/clips/{name}", s.handleDeleteClip)
	mux.HandleFunc("GET /api/clipping/status", s.handleClippingStatus)
	mux.HandleFunc("POST /api/clipping/enabled", s.handleSetClippingEnabled)

	mux.HandleFunc("POST /api/notify/subscribe", s.handleNotifySubscribe)

	return mux
}

// clipNamePattern rejects anything that isn't a plain filename this server
// itself would have listed — no "..", no "/", nothing that could escape
// clipsDir when joined onto it.
var clipNamePattern = regexp.MustCompile(`^[a-zA-Z0-9._-]+\.mp4$`)

func (s *Server) handleListClips(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	offset, _ := strconv.Atoi(q.Get("offset"))
	if offset < 0 {
		offset = 0
	}
	limit := 30
	if raw := q.Get("limit"); raw != "" {
		if n, err := strconv.Atoi(raw); err == nil {
			limit = n
		}
	}
	if limit < 0 {
		limit = 0
	}
	if limit > 100 {
		limit = 100
	}
	page, err := s.ListClips(offset, limit)
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, page)
}

func (s *Server) handleServeClip(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !clipNamePattern.MatchString(name) {
		writeError(w, http.StatusBadRequest, "invalid clip name")
		return
	}
	if s.ClipsDir() == "" {
		writeError(w, http.StatusServiceUnavailable, "clips directory not configured")
		return
	}
	// http.ServeFile handles Range requests natively — required for a
	// browser <video> element to seek, and nothing extra to write for it.
	http.ServeFile(w, r, filepath.Join(s.ClipsDir(), name))
}

func (s *Server) handleServeThumbnail(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !clipNamePattern.MatchString(name) {
		writeError(w, http.StatusBadRequest, "invalid clip name")
		return
	}
	if s.ClipsDir() == "" {
		writeError(w, http.StatusServiceUnavailable, "clips directory not configured")
		return
	}
	path := thumbnailPath(filepath.Join(s.ClipsDir(), name))
	if _, err := os.Stat(path); err != nil {
		writeError(w, http.StatusNotFound, "no thumbnail for this clip")
		return
	}
	http.ServeFile(w, r, path)
}

func (s *Server) handleDeleteClip(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !clipNamePattern.MatchString(name) {
		writeError(w, http.StatusBadRequest, "invalid clip name")
		return
	}
	if err := s.DeleteClip(name); err != nil {
		if os.IsNotExist(err) {
			writeError(w, http.StatusNotFound, "no such clip")
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}

func (s *Server) handleClippingStatus(w http.ResponseWriter, _ *http.Request) {
	status, err := s.ClippingStatus()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, status)
}

func (s *Server) handleSetClippingEnabled(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Enabled bool `json:"enabled"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON body: "+err.Error())
		return
	}
	status, err := s.SetClippingEnabled(body.Enabled)
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, status)
}

// handleNotifySubscribe is the client's explicit opt-in to presence events. The
// hub has no per-client identity (LAN-only, see hub.go), so this is a global
// gate rather than a real subscription: any client can enable or disable the
// "presence" broadcast. An empty body means "enable".
func (s *Server) handleNotifySubscribe(w http.ResponseWriter, r *http.Request) {
	body := struct {
		Enabled *bool `json:"enabled"`
	}{}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil && !errors.Is(err, io.EOF) {
		writeError(w, http.StatusBadRequest, "invalid JSON body: "+err.Error())
		return
	}
	enabled := true
	if body.Enabled != nil {
		enabled = *body.Enabled
	}
	s.SetNotifyEnabled(enabled)
	writeJSON(w, http.StatusOK, map[string]any{
		"status":  "subscribed",
		"enabled": enabled,
		"present": s.PresenceSnapshot().Present,
	})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) handleLink(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.LinkSnapshot())
}

func (s *Server) handleDescriptor(w http.ResponseWriter, _ *http.Request) {
	desc, ok := s.DescriptorSnapshot()
	if !ok {
		writeError(w, http.StatusServiceUnavailable, "no descriptor yet; device has not answered describe")
		return
	}
	writeJSON(w, http.StatusOK, desc)
}

func (s *Server) handleState(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.StateSnapshot())
}

// handlePatchControl validates every field client-side with proto.Coerce
// before it ever reaches the wire. This matters more than it looks: the
// firmware rejects an entire `set` on one unknown field
// (firmware/src/controls/led_control.cpp:209) and ArduinoJson's is<double>()
// is false for JSON strings, so a bad request comes back from real hardware
// as a 64-byte truncated `err`. Catching it here turns that into an ordinary
// HTTP 400 with a real message.
func (s *Server) handlePatchControl(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !s.HasControl(id) {
		writeError(w, http.StatusNotFound, "unknown control: "+id)
		return
	}

	var body map[string]any
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON body: "+err.Error())
		return
	}
	if len(body) == 0 {
		writeError(w, http.StatusBadRequest, "request body must set at least one field")
		return
	}

	coerced := proto.Values{}
	for key, raw := range body {
		field, ok := s.ControlField(id, key)
		if !ok {
			writeError(w, http.StatusBadRequest, "unknown field: "+key)
			return
		}
		value, err := proto.Coerce(field, raw)
		if err != nil {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		coerced[key] = value
	}

	seq := s.allocSeq()
	result, err := s.SendAndWait(seq, proto.Set{Seq: seq, ID: id, V: coerced})
	if err != nil {
		writeError(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if result.err != nil {
		writeError(w, http.StatusBadGateway, "device: "+result.err.Msg)
		return
	}

	s.rememberDesired(id, coerced)
	writeJSON(w, http.StatusOK, map[string]any{"id": id, "applied": coerced})
}

func (s *Server) handleRefresh(w http.ResponseWriter, _ *http.Request) {
	seq := s.allocSeq()
	if _, err := s.SendAndWait(seq, proto.Describe{Seq: seq}); err != nil {
		writeError(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "refreshed"})
}

// handleEstop is deliberately its own route rather than requiring a client to
// know that e-stop lives inside the "motion" control — the one convenience
// endpoint in an otherwise fully descriptor-driven API, because a physical
// stop button should never depend on a client having cached the descriptor
// correctly.
func (s *Server) handleEstop(w http.ResponseWriter, _ *http.Request) {
	if !s.HasControl("motion") {
		writeError(w, http.StatusServiceUnavailable, "no descriptor yet; device has not answered describe")
		return
	}
	seq := s.allocSeq()
	result, err := s.SendAndWait(seq, proto.Set{Seq: seq, ID: "motion", V: proto.Values{"estop": true}})
	if err != nil {
		writeError(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if result.err != nil {
		writeError(w, http.StatusBadGateway, "device: "+result.err.Msg)
		return
	}
	s.rememberDesired("motion", proto.Values{"estop": true})
	writeJSON(w, http.StatusOK, map[string]string{"status": "stopped"})
}

func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	var initial []wsEvent

	snapshot := s.LinkSnapshot()
	initial = append(initial, wsEvent{Type: "link", Status: string(snapshot.Status)})
	if snapshot.Hello != nil {
		initial = append(initial, wsEvent{Type: "hello", Hello: snapshot.Hello})
	}
	if desc, ok := s.DescriptorSnapshot(); ok {
		initial = append(initial, wsEvent{Type: "descriptor", Descriptor: &desc})
	}
	for id, v := range s.StateSnapshot() {
		state := proto.State{ID: id, V: v}
		initial = append(initial, wsEvent{Type: "state", State: &state})
	}
	if clipping, err := s.ClippingStatus(); err == nil {
		initial = append(initial, wsEvent{Type: "clipping", Clipping: &clipping})
	}
	if s.NotifyEnabled() {
		presence := s.PresenceSnapshot()
		initial = append(initial, wsEvent{Type: "presence", Presence: &presence})
	}

	s.hub.Serve(w, r, initial)
}
