import AsyncStorage from '@react-native-async-storage/async-storage'
import type { ThemeName } from '../theme'

// A header sent on every request, e.g. a Cloudflare Access service token or a
// bearer token for some other proxy.
export interface AuthHeader {
  name: string
  value: string
}

// The app has no window.location to inherit an origin from, so the connection
// target is stored explicitly. The deployment is a single Cloudflare tunnel
// host served over HTTPS on 443: horus-server's REST/WS API lives under /api and
// MediaMTX's WHEP endpoint is reverse-proxied under /whep/eye (rewritten to
// /eye/whep upstream), so the client only ever needs the hostname. WebRTC media
// is negotiated via ICE (TURN-relayed) and never touches this host directly.
export interface HorusConfig {
  // Tunnel hostname, e.g. 'horus.example.com'. No scheme, no port.
  host: string
  // Any number of auth headers (e.g. a Cloudflare Access service token), sent
  // on every REST call, WHEP request and WS upgrade. Rows with a blank name
  // are ignored.
  headers: AuthHeader[]
  // Whether the background foreground-service holds a WS open for presence
  // notifications.
  bgAlerts: boolean
  // Whether to check GitHub Releases for a newer APK when the app comes to the
  // foreground. The in-app updater's manual "Check for updates" button always
  // works regardless of this flag.
  autoUpdateCheck: boolean
  // Hidden (no settings row; long-press the version line to toggle): removes
  // every audio control and keeps playback muted. Audio is still recorded and
  // streamed — this only changes what the app plays.
  hideAudio: boolean
  // Color palette (see src/theme.ts). Saved the moment it's picked.
  themeName: ThemeName
}

export const DEFAULT_CONFIG: HorusConfig = {
  host: '',
  headers: [],
  bgAlerts: false,
  autoUpdateCheck: true,
  hideAudio: false,
  themeName: 'dark',
}

const KEY = 'horus.config'

// A synchronous cache so non-React callers (the WS client, the background
// service) can read config without awaiting. Kept fresh by load/save.
let cache: HorusConfig = DEFAULT_CONFIG

// Observers notified whenever the config cache changes (load or save). Lets
// RootNavigator re-evaluate isConfigured() after first-run setup.
const listeners = new Set<() => void>()

export function subscribeConfig(cb: () => void): () => void {
  listeners.add(cb)
  return () => listeners.delete(cb)
}

function notify(): void {
  listeners.forEach((l) => l())
}

export function getConfig(): HorusConfig {
  return cache
}

export function isConfigured(c: HorusConfig): boolean {
  return c.host.trim().length > 0
}

export async function loadConfig(): Promise<HorusConfig> {
  try {
    const raw = await AsyncStorage.getItem(KEY)
    if (raw) {
      const { cfAccessClientId, cfAccessClientSecret, ...saved } = JSON.parse(raw) as Partial<HorusConfig> &
        LegacyConfig
      const migrated = !Array.isArray(saved.headers)
      cache = { ...DEFAULT_CONFIG, ...saved }
      if (migrated) cache.headers = legacyHeaders({ cfAccessClientId, cfAccessClientSecret })
      notify()
      if (migrated) await AsyncStorage.setItem(KEY, JSON.stringify(cache))
    }
  } catch {
    // Corrupt or unavailable storage — fall back to defaults.
  }
  return cache
}

// Before custom headers, the Cloudflare Access service token had fields of
// its own. A config saved then carries them over as two ordinary headers.
interface LegacyConfig {
  cfAccessClientId?: string
  cfAccessClientSecret?: string
}

function legacyHeaders(c: LegacyConfig): AuthHeader[] {
  if (!c.cfAccessClientId || !c.cfAccessClientSecret) return []
  return [
    { name: 'CF-Access-Client-Id', value: c.cfAccessClientId },
    { name: 'CF-Access-Client-Secret', value: c.cfAccessClientSecret },
  ]
}

export async function saveConfig(next: HorusConfig): Promise<void> {
  cache = next
  notify()
  await AsyncStorage.setItem(KEY, JSON.stringify(next))
}

// --- URL builders ---
//
// Everything is HTTPS on the tunnel host's default port; the Cloudflare edge on
// 443 terminates TLS and routes by path.

export function apiBase(c: HorusConfig): string {
  return `https://${c.host}/api`
}

export function wsUrl(c: HorusConfig): string {
  return `wss://${c.host}/api/ws`
}

// Live journalctl stream for capture-eye.
export function consoleWsUrl(c: HorusConfig): string {
  return `wss://${c.host}/api/console/ws`
}

// MediaMTX's WHEP endpoint, reverse-proxied under the tunnel host at /whep/eye.
export function whepUrl(c: HorusConfig): string {
  return `https://${c.host}/whep/eye`
}

// The configured auth headers, or {} when there are none.
export function accessHeaders(c: HorusConfig): Record<string, string> {
  const out: Record<string, string> = {}
  for (const h of c.headers) {
    const name = h.name.trim()
    if (name) out[name] = h.value.trim()
  }
  return out
}
