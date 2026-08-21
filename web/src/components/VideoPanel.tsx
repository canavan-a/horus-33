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
      pc = new RTCPeerConnection()
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

      const res = await fetch(WHEP_URL, {
        method: 'POST',
        headers: { 'Content-Type': 'application/sdp' },
        body: offer.sdp,
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
