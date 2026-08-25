import { useCallback, useEffect, useRef, useState } from 'react'
import { Maximize, Minimize } from 'lucide-react'
import { Card, CardContent } from '@/components/ui/card'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'

// WHEP (WebRTC-HTTP Egress Protocol) against MediaMTX: one POST with our SDP
// offer, one SDP answer back, then media flows over ICE. No signalling
// server beyond that single request — this is the whole protocol.
//
// /whep/eye is the public shape (see the plan's M9/M10); the proxy (Vite in
// dev, nginx in production) rewrites it to MediaMTX's actual endpoint shape,
// /eye/whep, so this component never needs to know MediaMTX's convention.
const WHEP_URL = '/whep/eye'

// WHEP servers advertise STUN/TURN servers via `Link: <url>; rel="ice-server"`
// response headers (RFC 8288 style), so the viewer doesn't need to know them
// up front. Parsed from an OPTIONS response, since that's available before we
// have anything to offer -- MediaMTX answers OPTIONS with the same headers
// as the POST, without side effects.
function parseIceServersFromLinkHeader(linkHeader: string | null): RTCIceServer[] {
  if (!linkHeader) return []
  const servers: RTCIceServer[] = []
  // A single Link header can carry multiple comma-separated entries.
  for (const entry of linkHeader.split(/,(?=\s*<)/)) {
    const urlMatch = entry.match(/<([^>]+)>/)
    if (!urlMatch || !/rel="?ice-server"?/.test(entry)) continue
    const server: RTCIceServer = { urls: urlMatch[1] }
    const usernameMatch = entry.match(/username="([^"]*)"/)
    const credentialMatch = entry.match(/credential="([^"]*)"/)
    if (usernameMatch) server.username = usernameMatch[1]
    if (credentialMatch) server.credential = credentialMatch[1]
    servers.push(server)
  }
  return servers
}

export function VideoPanel() {
  const containerRef = useRef<HTMLDivElement>(null)
  const videoRef = useRef<HTMLVideoElement>(null)
  const [status, setStatus] = useState<'connecting' | 'live' | 'error'>('connecting')
  const [error, setError] = useState<string | undefined>(undefined)
  const [isFullscreen, setIsFullscreen] = useState(false)

  useEffect(() => {
    const onFullscreenChange = () => {
      setIsFullscreen(document.fullscreenElement === containerRef.current)
    }
    document.addEventListener('fullscreenchange', onFullscreenChange)
    return () => document.removeEventListener('fullscreenchange', onFullscreenChange)
  }, [])

  const toggleFullscreen = useCallback(() => {
    if (document.fullscreenElement) {
      document.exitFullscreen()
    } else {
      containerRef.current?.requestFullscreen()
    }
  }, [])

  useEffect(() => {
    let cancelled = false
    let pc: RTCPeerConnection | undefined

    async function connect() {
      const optionsRes = await fetch(WHEP_URL, { method: 'OPTIONS' })
      const iceServers = parseIceServersFromLinkHeader(optionsRes.headers.get('Link'))
      if (cancelled) return

      pc = new RTCPeerConnection({ iceServers })
      pc.ontrack = (event) => {
        if (videoRef.current) videoRef.current.srcObject = event.streams[0]
      }
      pc.oniceconnectionstatechange = () => {
        if (!pc || cancelled) return
        if (pc.iceConnectionState === 'connected') setStatus('live')
        if (pc.iceConnectionState === 'failed' || pc.iceConnectionState === 'disconnected') {
          setStatus('error')
          setError('ICE connection lost')
        }
      }
      // recvonly: this is a viewer, not a publisher — capture-eye is the only
      // publisher, over RTSP directly to MediaMTX (see capture-eye/src/h264_sink.cpp).
      pc.addTransceiver('video', { direction: 'recvonly' })

      const offer = await pc.createOffer()
      await pc.setLocalDescription(offer)

      // No trickle-ICE PATCH endpoint is implemented here, so wait for the
      // full candidate set (including srflx/relay, once STUN/TURN resolve)
      // before sending the offer.
      if (pc.iceGatheringState !== 'complete') {
        await new Promise<void>((resolve) => {
          const check = () => {
            if (pc?.iceGatheringState === 'complete') {
              pc.removeEventListener('icegatheringstatechange', check)
              resolve()
            }
          }
          pc?.addEventListener('icegatheringstatechange', check)
        })
      }
      if (cancelled) return

      const res = await fetch(WHEP_URL, {
        method: 'POST',
        headers: { 'Content-Type': 'application/sdp' },
        body: pc.localDescription?.sdp ?? offer.sdp,
      })
      if (!res.ok) throw new Error(`WHEP offer rejected: ${res.status}`)
      const answerSdp = await res.text()
      if (cancelled) return
      await pc.setRemoteDescription({ type: 'answer', sdp: answerSdp })
    }

    connect().catch((err) => {
      if (cancelled) return
      setStatus('error')
      setError(err instanceof Error ? err.message : String(err))
    })

    return () => {
      cancelled = true
      pc?.close()
    }
  }, [])

  return (
    <Card className="overflow-hidden py-0">
      <CardContent
        ref={containerRef}
        className={cn('relative aspect-video p-0', isFullscreen && 'flex items-center bg-black')}
      >
        <video ref={videoRef} autoPlay playsInline muted className="size-full bg-black object-contain" />
        {status !== 'live' && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/60 text-sm text-white">
            {status === 'connecting' ? 'connecting to stream…' : error ?? 'stream unavailable'}
          </div>
        )}
        <Button
          type="button"
          variant="secondary"
          size="icon-sm"
          aria-label={isFullscreen ? 'exit fullscreen' : 'fullscreen'}
          onClick={toggleFullscreen}
          className="absolute right-2 bottom-2 opacity-80 hover:opacity-100"
        >
          {isFullscreen ? <Minimize /> : <Maximize />}
        </Button>
      </CardContent>
    </Card>
  )
}
