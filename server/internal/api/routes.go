package api

import (
	"encoding/json"
	"net/http"

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

	return mux
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

	s.hub.Serve(w, r, initial)
}
