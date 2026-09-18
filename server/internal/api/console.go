// console.go exposes capture-eye's systemd unit to the API: restarting it and
// streaming its journal over a WebSocket. Scoped to the one unit horusctl
// already manages (tui-controller/internal/service) — this is the same
// systemctl/journalctl surface, just callable from a browser or the phone
// app instead of a terminal.
//
// There is no in-app admin check: per the plan's "Decisions taken" (see
// hub.go), access control for this API is left entirely to Cloudflare
// Access at the edge. Whoever operates this deployment is expected to scope
// a Cloudflare Access policy to /api/console/* for admins only — anyone who
// can reach these routes at all can restart capture-eye and read its log.
package api

import (
	"context"
	"net/http"
	"os/exec"
	"strconv"
	"time"

	"github.com/gorilla/websocket"
)

// captureEyeUnit mirrors tui-controller/internal/service.Unit. Duplicated
// rather than imported: server and tui-controller are separate Go modules,
// and this is the one constant worth a copy over a shared module split.
const captureEyeUnit = "horus-capture-eye.service"

// handleConsoleRestart runs `systemctl restart` on capture-eye's unit.
func (s *Server) handleConsoleRestart(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()

	out, err := exec.CommandContext(ctx, "systemctl", "restart", captureEyeUnit).CombinedOutput()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "systemctl restart failed: "+err.Error()+": "+string(out))
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "restarted"})
}

// consoleUpgrader is separate from hub.go's upgrader only so the two call
// sites stay independently readable; the policy (no origin check — see
// hub.go's comment on the LAN/Cloudflare Access trust model) is identical.
var consoleUpgrader = websocket.Upgrader{
	CheckOrigin: func(*http.Request) bool { return true },
}

// handleConsoleWS streams `journalctl -u horus-capture-eye.service -f` to the
// client, one line per text WebSocket message, from connect until the client
// disconnects. One journalctl subprocess per connection — this is a tail,
// not a broadcast, so there is nothing to share across clients the way
// hub.go's link/state events are.
func (s *Server) handleConsoleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := consoleUpgrader.Upgrade(w, r, nil)
	if err != nil {
		return // upgrader already wrote the error response
	}
	defer conn.Close()

	lines := 200
	if raw := r.URL.Query().Get("lines"); raw != "" {
		if n, err := strconv.Atoi(raw); err == nil && n >= 0 {
			lines = n
		}
	}

	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	cmd := exec.CommandContext(ctx, "journalctl",
		"-u", captureEyeUnit,
		"-n", strconv.Itoa(lines),
		"-f", "--no-pager", "-o", "cat")
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		_ = conn.WriteMessage(websocket.TextMessage, []byte("error: "+err.Error()))
		return
	}
	if err := cmd.Start(); err != nil {
		_ = conn.WriteMessage(websocket.TextMessage, []byte("error: "+err.Error()))
		return
	}
	defer cmd.Wait()

	// journalctl -f never exits on its own; a client-initiated close (or a
	// dead TCP connection surfacing as a read error) is the only way this
	// connection ends, so a reader goroutine watches for that and tears
	// down the subprocess via ctx cancellation — the same pattern hub.go's
	// Serve uses to detect disconnects.
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			if _, _, err := conn.NextReader(); err != nil {
				return
			}
		}
	}()

	go func() {
		<-done
		cancel()
	}()

	buf := make([]byte, 0, 4096)
	chunk := make([]byte, 4096)
	for {
		n, err := stdout.Read(chunk)
		if n > 0 {
			buf = append(buf[:0], chunk[:n]...)
			_ = conn.SetWriteDeadline(time.Now().Add(writeTimeout))
			if writeErr := conn.WriteMessage(websocket.TextMessage, buf); writeErr != nil {
				return
			}
		}
		if err != nil {
			return
		}
	}
}
