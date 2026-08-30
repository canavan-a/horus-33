import { useEffect, useRef, useSyncExternalStore } from 'react'
import { HorusSocket, type HorusSnapshot } from './HorusSocket'

// A single foreground socket for the app's lifetime. The background service
// owns a separate one; the server fans out to every client, so two is fine.
let shared: HorusSocket | undefined

function getShared(): HorusSocket {
  if (!shared) {
    shared = new HorusSocket('foreground')
    shared.start()
  }
  return shared
}

export function reconfigureHorus() {
  shared?.reconfigure()
}

export function useHorus(): HorusSnapshot {
  const sock = useRef(getShared()).current
  return useSyncExternalStore(
    (cb) => sock.subscribe(cb),
    () => sock.getSnapshot(),
  )
}

// Convenience: pull one control's live values (defaults to {}).
export function useControlValues(snap: HorusSnapshot, id: string) {
  return snap.state[id] ?? {}
}

// Keep the shared socket alive while any screen is mounted; never closed on
// background so a screen re-mount is instant.
export function useKeepHorusAlive() {
  useEffect(() => {
    getShared()
  }, [])
}
