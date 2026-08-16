import { useEffect, useRef, useState } from 'react'
import type { Descriptor, LinkStatus, Values, WSEvent } from '@/lib/proto'

// Mirrors reconnect intervals used throughout the rest of this stack
// (tui-controller's serial link, horus-server's Unix link): reconnect on a
// fixed timer rather than hammering a socket that just closed.
const RECONNECT_DELAY_MS = 750

export interface HorusState {
  linkStatus: LinkStatus
  linkError?: string
  descriptor?: Descriptor
  state: Record<string, Values>
}

// Owns one WebSocket to horus-server for the app's lifetime, reconnecting on
// drop. All server-pushed state funnels through here so every component sees
// the same live snapshot without each one managing its own connection.
export function useHorus(): HorusState {
  const [linkStatus, setLinkStatus] = useState<LinkStatus>('connecting')
  const [linkError, setLinkError] = useState<string | undefined>(undefined)
  const [descriptor, setDescriptor] = useState<Descriptor>()
  const [state, setState] = useState<Record<string, Values>>({})

  // Avoids a stale closure re-scheduling reconnects after unmount.
  const alive = useRef(true)

  useEffect(() => {
    alive.current = true
    let socket: WebSocket | undefined
    let timer: ReturnType<typeof setTimeout> | undefined

    const connect = () => {
      if (!alive.current) return

      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      socket = new WebSocket(`${protocol}//${window.location.host}/api/ws`)

      socket.onmessage = (event) => {
        const msg: WSEvent = JSON.parse(event.data)
        switch (msg.type) {
          case 'link':
            if (msg.status) setLinkStatus(msg.status as LinkStatus)
            setLinkError(msg.error?.msg)
            break
          case 'descriptor':
            if (msg.descriptor) setDescriptor(msg.descriptor)
            break
          case 'state':
            if (msg.state) {
              const { id, v } = msg.state
              setState((prev) => ({ ...prev, [id]: v }))
            }
            break
          case 'error':
            if (msg.error) setLinkError(msg.error.msg)
            break
        }
      }

      socket.onclose = () => {
        if (!alive.current) return
        timer = setTimeout(connect, RECONNECT_DELAY_MS)
      }
      socket.onerror = () => socket?.close()
    }

    connect()
    return () => {
      alive.current = false
      if (timer) clearTimeout(timer)
      socket?.close()
    }
  }, [])

  return { linkStatus, linkError, descriptor, state }
}
