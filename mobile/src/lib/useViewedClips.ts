import AsyncStorage from '@react-native-async-storage/async-storage'
import { useCallback, useEffect, useState } from 'react'

const KEY = 'horus:viewedClips'

// Process-wide cache so every mounted list shares one view of the set and a
// mark from one screen is visible on another without a reload. Undefined until
// the first load resolves.
let cache: Set<string> | undefined
const listeners = new Set<() => void>()

async function ensureLoaded(): Promise<void> {
  if (cache) return
  try {
    const raw = await AsyncStorage.getItem(KEY)
    const parsed = raw ? JSON.parse(raw) : []
    cache = new Set(Array.isArray(parsed) ? (parsed as string[]) : [])
  } catch {
    // Corrupt / unavailable storage: session-only.
    cache = new Set()
  }
}

// Remembers which clips the user has opened (expanding a clip autoplays it, so
// "opened" == "watched"). Backed by a single AsyncStorage key holding a JSON
// array of clip names, so it survives app restarts.
export function useViewedClips() {
  const [, bump] = useState(0)

  useEffect(() => {
    let alive = true
    const rerender = () => {
      if (alive) bump((n) => n + 1)
    }
    ensureLoaded().then(rerender)
    listeners.add(rerender)
    return () => {
      alive = false
      listeners.delete(rerender)
    }
  }, [])

  const isViewed = useCallback((name: string) => cache?.has(name) ?? false, [])

  const markViewed = useCallback((name: string) => {
    if (!cache || cache.has(name)) return
    cache.add(name)
    AsyncStorage.setItem(KEY, JSON.stringify([...cache])).catch(() => {})
    listeners.forEach((l) => l())
  }, [])

  // Changes whenever the set grows — pass as FlatList `extraData` so rows
  // re-render when a clip elsewhere is marked viewed.
  const viewedCount = cache?.size ?? 0

  return { isViewed, markViewed, viewedCount }
}
