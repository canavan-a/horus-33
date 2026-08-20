import { useCallback, useEffect, useState } from 'react'

const PREFIX = 'horus:collapsed:'

function read(key: string): boolean {
  try {
    return localStorage.getItem(PREFIX + key) === '1'
  } catch {
    // Private browsing / disabled storage: fall back to in-memory only, the
    // collapse toggle still works within the session.
    return false
  }
}

function write(key: string, collapsed: boolean) {
  try {
    if (collapsed) localStorage.setItem(PREFIX + key, '1')
    else localStorage.removeItem(PREFIX + key)
  } catch {
    // Ignore -- see read().
  }
}

// Per-panel collapse state, keyed by a stable id (e.g. a control's id), so
// each menu remembers its own open/closed state across reloads independently
// of the others.
export function usePersistedCollapse(key: string): [boolean, () => void] {
  const [collapsed, setCollapsed] = useState(() => read(key))

  useEffect(() => {
    setCollapsed(read(key))
  }, [key])

  const toggle = useCallback(() => {
    setCollapsed((c) => {
      const next = !c
      write(key, next)
      return next
    })
  }, [key])

  return [collapsed, toggle]
}
