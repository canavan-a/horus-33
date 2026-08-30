import AsyncStorage from '@react-native-async-storage/async-storage'

// The app has no window.location to inherit an origin from, so the connection
// target is stored explicitly. The deployment is a single Cloudflare tunnel
// host served over HTTPS on 443: horus-server's REST/WS API lives under /api and
// MediaMTX's WHEP endpoint is reverse-proxied under /whep/eye (rewritten to
// /eye/whep upstream), so the client only ever needs the hostname. WebRTC media
// is negotiated via ICE (TURN-relayed) and never touches this host directly.
export interface HorusConfig {
  // Tunnel hostname, e.g. 'horus.example.com'. No scheme, no port.
  host: string
  // Cloudflare Access service token. Sent as CF-Access-Client-Id /
  // CF-Access-Client-Secret on every REST call and on the WS upgrade.
  cfAccessClientId: string
  cfAccessClientSecret: string
  // Whether the background foreground-service holds a WS open for presence
  // notifications.
  bgAlerts: boolean
  // Whether to check GitHub Releases for a newer APK when the app comes to the
  // foreground. The in-app updater's manual "Check for updates" button always
  // works regardless of this flag.
  autoUpdateCheck: boolean
}

export const DEFAULT_CONFIG: HorusConfig = {
  host: '',
  cfAccessClientId: '',
  cfAccessClientSecret: '',
  bgAlerts: false,
  autoUpdateCheck: true,
}

const KEY = 'horus.config'

// A synchronous cache so non-React callers (the WS client, the background
// service) can read config without awaiting. Kept fresh by load/save.
let cache: HorusConfig = DEFAULT_CONFIG

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
      cache = { ...DEFAULT_CONFIG, ...(JSON.parse(raw) as Partial<HorusConfig>) }
    }
  } catch {
    // Corrupt or unavailable storage — fall back to defaults.
  }
  return cache
}

export async function saveConfig(next: HorusConfig): Promise<void> {
  cache = next
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

// MediaMTX's WHEP endpoint, reverse-proxied under the tunnel host at /whep/eye.
export function whepUrl(c: HorusConfig): string {
  return `https://${c.host}/whep/eye`
}

// Headers for Cloudflare Access, or {} when no token is configured.
export function accessHeaders(c: HorusConfig): Record<string, string> {
  if (!c.cfAccessClientId || !c.cfAccessClientSecret) return {}
  return {
    'CF-Access-Client-Id': c.cfAccessClientId,
    'CF-Access-Client-Secret': c.cfAccessClientSecret,
  }
}
