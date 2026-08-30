import { useCallback, useState } from 'react'

const KEY = 'horus:viewedClips'

function read(): Set<string> {
  try {
    const raw = localStorage.getItem(KEY)
    if (!raw) return new Set()
    const parsed = JSON.parse(raw)
    return Array.isArray(parsed) ? new Set(parsed as string[]) : new Set()
  } catch {
    // Private browsing / disabled storage / corrupt value: session-only.
    return new Set()
  }
}

function write(names: Set<string>) {
  try {
    localStorage.setItem(KEY, JSON.stringify([...names]))
  } catch {
    // Ignore -- see read().
  }
}

// Remembers which clips the user has opened (expanding a clip autoplays it, so
// "opened" == "watched"). Backed by a single localStorage key holding a JSON
// array of clip names, so it survives reloads.
export function useViewedClips() {
  const [viewed, setViewed] = useState<Set<string>>(read)

  const isViewed = useCallback((name: string) => viewed.has(name), [viewed])

  const markViewed = useCallback((name: string) => {
    setViewed((current) => {
      if (current.has(name)) return current
      const next = new Set(current)
      next.add(name)
      write(next)
      return next
    })
  }, [])

  return { isViewed, markViewed }
}
