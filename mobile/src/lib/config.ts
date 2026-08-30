import AsyncStorage from '@react-native-async-storage/async-storage'

// The app has no window.location to inherit an origin from, so the connection
// target is stored explicitly. Two shapes work:
//   - LAN:    scheme 'http',  host '192.168.1.50', apiPort 8080, mediaPort 8889
//   - Remote: scheme 'https', host 'horus.example.com' (a cloudflared tunnel to
//             horus-server, behind Cloudflare Access), ports left at 0 so the
//             URL builders omit them and the edge's 443 is used.
export interface HorusConfig {
  scheme: 'http' | 'https'
  host: string
  apiPort: number
  mediaPort: number
  // Cloudflare Access service token. Sent as CF-Access-Client-Id /
  // CF-Access-Client-Secret on every REST call and on the WS upgrade. Empty on
  // a plain LAN setup.
  cfAccessClientId: string
  cfAccessClientSecret: string
  // Whether the background foreground-service holds a WS open for presence
  // notifications.
  bgAlerts: boolean
}

export const DEFAULT_CONFIG: HorusConfig = {
  scheme: 'http',
  host: '',
  apiPort: 8080,
  mediaPort: 8889,
  cfAccessClientId: '',
  cfAccessClientSecret: '',
  bgAlerts: false,
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

function authority(c: HorusConfig, port: number): string {
  // Port 0 (or the scheme default) => omit it, so the Cloudflare edge on 443
  // works without the user typing ":443".
  const isDefault = (c.scheme === 'https' && port === 443) || (c.scheme === 'http' && port === 80)
  return port && !isDefault ? `${c.host}:${port}` : c.host
}

export function apiBase(c: HorusConfig): string {
  return `${c.scheme}://${authority(c, c.apiPort)}/api`
}

export function wsUrl(c: HorusConfig): string {
  const wsScheme = c.scheme === 'https' ? 'wss' : 'ws'
  return `${wsScheme}://${authority(c, c.apiPort)}/api/ws`
}

// MediaMTX's WHEP endpoint. On a LAN this is the camera host directly on
// mediaPort; remotely it is assumed to be reverse-proxied under the same
// tunnel host at /whep/eye (adjust to taste in Settings).
export function whepUrl(c: HorusConfig): string {
  if (c.scheme === 'https' || !c.mediaPort) {
    return `${c.scheme}://${authority(c, c.apiPort)}/whep/eye`
  }
  return `${c.scheme}://${c.host}:${c.mediaPort}/eye/whep`
}

// Headers for Cloudflare Access, or {} when no token is configured.
export function accessHeaders(c: HorusConfig): Record<string, string> {
  if (!c.cfAccessClientId || !c.cfAccessClientSecret) return {}
  return {
    'CF-Access-Client-Id': c.cfAccessClientId,
    'CF-Access-Client-Secret': c.cfAccessClientSecret,
  }
}
