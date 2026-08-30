import { RTCPeerConnection, type MediaStream } from 'react-native-webrtc'
import { accessHeaders, getConfig, whepUrl } from '../lib/config'

// Parse RFC 8288 Link headers advertising ICE servers, e.g.
//   Link: <stun:stun.l.google.com:19302>; rel="ice-server"
//   Link: <turn:turn.example.com:3478?transport=udp>; rel="ice-server"; username="u"; credential="p"
export function parseIceServers(linkHeader: string | null): RTCIceServer[] {
  if (!linkHeader) return []
  const servers: RTCIceServer[] = []
  // Split on commas that precede a `<` (each link value starts with `<url>`).
  for (const part of linkHeader.split(/,\s*(?=<)/)) {
    const urlMatch = part.match(/<([^>]+)>/)
    if (!urlMatch) continue
    if (!/rel\s*=\s*"?ice-server"?/.test(part)) continue
    const server: RTCIceServer = { urls: urlMatch[1] }
    const user = part.match(/username\s*=\s*"([^"]*)"/)
    const cred = part.match(/credential\s*=\s*"([^"]*)"/)
    if (user) server.username = user[1]
    if (cred) server.credential = cred[1]
    servers.push(server)
  }
  return servers
}

type IceServer = { urls: string; username?: string; credential?: string }
type RTCIceServer = IceServer

export interface WhepSession {
  stream: MediaStream
  close: () => void
}

// Runs the WHEP handshake and resolves once a remote track is attached.
// onState reports 'connecting' | 'live' | 'failed'.
export async function connectWhep(
  onState: (s: 'connecting' | 'live' | 'failed') => void,
): Promise<WhepSession> {
  const c = getConfig()
  const url = whepUrl(c)
  const headers = accessHeaders(c)

  onState('connecting')

  const optRes = await fetch(url, { method: 'OPTIONS', headers })
  const iceServers = parseIceServers(optRes.headers.get('Link'))

  const pc = new RTCPeerConnection({ iceServers })
  pc.addTransceiver('video', { direction: 'recvonly' })

  let resolved = false
  const session: Promise<WhepSession> = new Promise((resolve, reject) => {
    // @ts-expect-error react-native-webrtc event typing
    pc.addEventListener('track', (event: { streams: MediaStream[] }) => {
      if (resolved) return
      resolved = true
      resolve({
        stream: event.streams[0],
        close: () => pc.close(),
      })
    })
    // @ts-expect-error react-native-webrtc event typing
    pc.addEventListener('iceconnectionstatechange', () => {
      const st = (pc as unknown as { iceConnectionState: string }).iceConnectionState
      if (st === 'connected' || st === 'completed') onState('live')
      if (st === 'failed') {
        onState('failed')
        if (!resolved) reject(new Error('ICE failed'))
      }
    })

    void negotiate(pc, url, headers).catch((err) => {
      onState('failed')
      if (!resolved) reject(err)
    })
  })

  return session
}

async function negotiate(
  pc: RTCPeerConnection,
  url: string,
  headers: Record<string, string>,
) {
  const offer = await pc.createOffer({})
  await pc.setLocalDescription(offer)
  await waitForIceGathering(pc)

  const local = (pc as unknown as { localDescription: { sdp: string } }).localDescription
  const answer = await fetch(url, {
    method: 'POST',
    headers: { ...headers, 'Content-Type': 'application/sdp' },
    body: local.sdp,
  })
  if (!answer.ok) throw new Error(`WHEP POST ${answer.status}`)
  const sdp = await answer.text()
  await pc.setRemoteDescription({ type: 'answer', sdp })
}

// react-native-webrtc doesn't reliably fire icegatheringstatechange ->
// 'complete', so cap the wait.
function waitForIceGathering(pc: RTCPeerConnection, timeoutMs = 2000): Promise<void> {
  const get = () => (pc as unknown as { iceGatheringState: string }).iceGatheringState
  if (get() === 'complete') return Promise.resolve()
  return new Promise((resolve) => {
    const done = () => {
      clearTimeout(t)
      // @ts-expect-error react-native-webrtc event typing
      pc.removeEventListener('icegatheringstatechange', check)
      resolve()
    }
    const check = () => {
      if (get() === 'complete') done()
    }
    const t = setTimeout(done, timeoutMs)
    // @ts-expect-error react-native-webrtc event typing
    pc.addEventListener('icegatheringstatechange', check)
  })
}
