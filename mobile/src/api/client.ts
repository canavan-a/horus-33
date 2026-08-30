import { accessHeaders, apiBase, getConfig } from '../lib/config'
import type { Clip, ClipsPage, ClippingStatus, Descriptor, Values } from '../lib/proto'

export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const c = getConfig()
  const res = await fetch(`${apiBase(c)}${path}`, {
    ...init,
    headers: {
      ...accessHeaders(c),
      ...(init?.headers ?? {}),
    },
  })
  const text = await res.text()
  const body = text ? JSON.parse(text) : undefined
  if (!res.ok) {
    const msg = body?.error ?? `${res.status} ${res.statusText}`
    throw new ApiError(res.status, msg)
  }
  return body as T
}

export const getLink = () => request<{ status: string; error?: string }>('/link')

export const getDescriptor = () => request<Descriptor>('/descriptor')

export const getState = () => request<Record<string, Values>>('/state')

export const patchControl = (id: string, values: Values) =>
  request<{ id: string; applied: Values }>(`/controls/${encodeURIComponent(id)}`, {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(values),
  })

export const refresh = () => request<{ status: string }>('/refresh', { method: 'POST' })

export const estop = () => request<{ status: string }>('/estop', { method: 'POST' })

// GET /api/clips is paged server-side (max 100/req). The mobile screen shows the
// whole list, so walk every page and concatenate.
export const listClips = async (): Promise<Clip[]> => {
  const clips: Clip[] = []
  for (;;) {
    const page = await request<ClipsPage>(`/clips?offset=${clips.length}&limit=100`)
    clips.push(...page.clips)
    if (clips.length >= page.total || page.clips.length === 0) return clips
  }
}

export const deleteClip = (name: string) =>
  request<{ status: string }>(`/clips/${encodeURIComponent(name)}`, { method: 'DELETE' })

export const getClippingStatus = () => request<ClippingStatus>('/clipping/status')

export const setClippingEnabled = (enabled: boolean) =>
  request<ClippingStatus>('/clipping/enabled', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled }),
  })

// Opt in (or out) of server-side presence broadcasts. Called on launch and
// whenever the host config changes.
export const notifySubscribe = (enabled = true) =>
  request<{ status: string; enabled: boolean; present: boolean }>('/notify/subscribe', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled }),
  })

// Media URLs for the clips list — plain GETs the <Video>/<Image> tags load
// directly, with the Access token in the query is not possible, so these only
// work unauthenticated (LAN) or when Cloudflare Access allows the app's
// service token via header — RN's fetch-based media loaders forward configured
// headers, but <Image>/<Video> do not, so on a tunnel the clips dir should be
// on a public Access path or bypass. Documented in Settings.
export function clipUrl(name: string): string {
  return `${apiBase(getConfig())}/clips/${encodeURIComponent(name)}`
}

export function clipThumbnailUrl(name: string): string {
  return `${apiBase(getConfig())}/clips/${encodeURIComponent(name)}/thumbnail`
}
