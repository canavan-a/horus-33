import type { ClipsPage, ClippingStatus, Descriptor, LinkSnapshot, Values } from '@/lib/proto'

// Every call is a relative path — dev proxies it (vite.config.ts), production
// nginx proxies it (the plan's M10). The client never needs to know a host.
const BASE = '/api'

export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(BASE + path, init)
  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }))
    throw new ApiError(res.status, body.error ?? res.statusText)
  }
  return res.json() as Promise<T>
}

export const getLink = () => request<LinkSnapshot>('/link')
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

export const listClips = (offset = 0, limit = 30) =>
  request<ClipsPage>(`/clips?offset=${offset}&limit=${limit}`)

export const getClippingStatus = () => request<ClippingStatus>('/clipping/status')

export const setClippingEnabled = (enabled: boolean) =>
  request<ClippingStatus>('/clipping/enabled', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ enabled }),
  })

export const deleteClip = (name: string) =>
  request<{ status: string }>(`/clips/${encodeURIComponent(name)}`, { method: 'DELETE' })

// Restart capture-eye's systemd unit. No in-app admin check — see
// server/internal/api/console.go's package comment: this is expected to sit
// behind a Cloudflare Access policy scoped to admins.
export const consoleRestart = () => request<{ status: string }>('/console/restart', { method: 'POST' })

// Live journalctl stream for capture-eye, relative to the current origin so
// it goes through the same dev/prod proxy as every other /api call.
export function consoleWsUrl(): string {
  const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${proto}//${window.location.host}${BASE}/console/ws`
}
