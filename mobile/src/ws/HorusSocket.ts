import { accessHeaders, getConfig, wsUrl } from '../lib/config'
import type {
  ClippingStatus,
  Descriptor,
  LinkStatus,
  PresenceEvent,
  Values,
  WSEvent,
} from '../lib/proto'

export interface HorusSnapshot {
  linkStatus: LinkStatus | 'offline'
  linkError?: string
  descriptor?: Descriptor
  state: Record<string, Values>
  clipping?: ClippingStatus
  presence?: PresenceEvent
}

const EMPTY: HorusSnapshot = { linkStatus: 'offline', state: {} }

type Listener = (snap: HorusSnapshot) => void
type PresenceListener = (ev: PresenceEvent) => void

// Foreground: keep it snappy, traffic keeps the socket warm through Cloudflare.
// Background: the socket is otherwise idle, so it must (a) ping inside
// Cloudflare's ~100s idle timeout and (b) back off on reconnect so a flapping
// network doesn't spin.
interface ModeTuning {
  reconnectBase: number
  reconnectMax: number
  pingEvery: number // 0 = no app-level ping
}

const TUNING: Record<'foreground' | 'background', ModeTuning> = {
  foreground: { reconnectBase: 750, reconnectMax: 750, pingEvery: 0 },
  background: { reconnectBase: 1000, reconnectMax: 120_000, pingEvery: 85_000 },
}

export class HorusSocket {
  private ws?: WebSocket
  private snap: HorusSnapshot = EMPTY
  private listeners = new Set<Listener>()
  private presenceListeners = new Set<PresenceListener>()
  private closed = false
  private attempt = 0
  private reconnectTimer?: ReturnType<typeof setTimeout>
  private pingTimer?: ReturnType<typeof setInterval>
  private readonly tuning: ModeTuning

  constructor(private mode: 'foreground' | 'background' = 'foreground') {
    this.tuning = TUNING[mode]
  }

  start() {
    this.closed = false
    this.connect()
  }

  close() {
    this.closed = true
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer)
    if (this.pingTimer) clearInterval(this.pingTimer)
    this.ws?.close()
    this.ws = undefined
  }

  // Close and reconnect — call after the host config changes.
  reconfigure() {
    this.close()
    this.snap = EMPTY
    this.emit()
    this.start()
  }

  getSnapshot(): HorusSnapshot {
    return this.snap
  }

  subscribe(fn: Listener): () => void {
    this.listeners.add(fn)
    fn(this.snap)
    return () => this.listeners.delete(fn)
  }

  onPresence(fn: PresenceListener): () => void {
    this.presenceListeners.add(fn)
    return () => this.presenceListeners.delete(fn)
  }

  private connect() {
    const c = getConfig()
    if (!c.host) return

    // RN's WebSocket accepts a headers option (3rd arg) — this is how the
    // Cloudflare Access service token rides the upgrade request. Browsers
    // can't do this; the web client never needed it.
    const headers = accessHeaders(c)
    this.ws = new WebSocket(
      wsUrl(c),
      undefined,
      Object.keys(headers).length ? { headers } : undefined,
    ) as WebSocket

    this.ws.onopen = () => {
      this.attempt = 0
      this.setLink('connecting')
      if (this.tuning.pingEvery > 0) {
        this.pingTimer = setInterval(() => {
          try {
            this.ws?.send(JSON.stringify({ type: 'ping' }))
          } catch {
            /* socket already dead; onclose will handle it */
          }
        }, this.tuning.pingEvery)
      }
    }

    this.ws.onmessage = (e) => {
      let ev: WSEvent
      try {
        ev = JSON.parse(String(e.data)) as WSEvent
      } catch {
        return
      }
      this.apply(ev)
    }

    this.ws.onerror = () => {
      /* onclose always follows */
    }

    this.ws.onclose = () => {
      if (this.pingTimer) clearInterval(this.pingTimer)
      this.ws = undefined
      if (this.closed) return
      this.setLink('offline')
      const delay = Math.min(
        this.tuning.reconnectBase * 2 ** this.attempt,
        this.tuning.reconnectMax,
      )
      this.attempt++
      this.reconnectTimer = setTimeout(() => this.connect(), delay)
    }
  }

  private apply(ev: WSEvent) {
    switch (ev.type) {
      case 'link':
        this.snap = {
          ...this.snap,
          linkStatus: (ev.status as LinkStatus) ?? this.snap.linkStatus,
          linkError: ev.error?.msg,
        }
        break
      case 'descriptor':
        this.snap = { ...this.snap, descriptor: ev.descriptor }
        break
      case 'state':
        if (ev.state) {
          this.snap = {
            ...this.snap,
            state: { ...this.snap.state, [ev.state.id]: ev.state.v },
          }
        }
        break
      case 'error':
        this.snap = { ...this.snap, linkError: ev.error?.msg }
        break
      case 'clipping':
        this.snap = { ...this.snap, clipping: ev.clipping }
        break
      case 'presence':
        if (ev.presence) {
          this.snap = { ...this.snap, presence: ev.presence }
          for (const fn of this.presenceListeners) fn(ev.presence)
        }
        break
      case 'hello':
        // device (re)booted; nothing to store here
        break
    }
    this.emit()
  }

  private setLink(status: HorusSnapshot['linkStatus']) {
    this.snap = { ...this.snap, linkStatus: status }
    this.emit()
  }

  private emit() {
    for (const fn of this.listeners) fn(this.snap)
  }
}
