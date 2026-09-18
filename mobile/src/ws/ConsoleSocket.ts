import { accessHeaders, consoleWsUrl, getConfig } from '../lib/config'

// Plain tail of capture-eye's journal — one text message per line, no JSON
// envelope (server/internal/api/console.go writes raw journalctl -o cat
// output). Kept separate from HorusSocket: this is a per-screen connection,
// not something the rest of the app needs a snapshot of, and reconnecting a
// live log tail on every screen focus is simpler than folding it into the
// always-on link socket.
export class ConsoleSocket {
  private ws?: WebSocket
  private closed = false

  constructor(
    private onLine: (line: string) => void,
    private onStatus: (connected: boolean) => void,
  ) {}

  start() {
    this.closed = false
    this.connect()
  }

  close() {
    this.closed = true
    this.ws?.close()
    this.ws = undefined
  }

  private connect() {
    const c = getConfig()
    if (!c.host) return

    const headers = accessHeaders(c)
    this.ws = new WebSocket(
      consoleWsUrl(c),
      undefined,
      Object.keys(headers).length ? { headers } : undefined,
    ) as WebSocket

    this.ws.onopen = () => this.onStatus(true)
    this.ws.onmessage = (e) => this.onLine(String(e.data))
    this.ws.onerror = () => {
      /* onclose always follows */
    }
    this.ws.onclose = () => {
      this.ws = undefined
      this.onStatus(false)
      if (this.closed) return
      setTimeout(() => this.connect(), 1500)
    }
  }
}
