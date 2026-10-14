import { useEffect, useRef } from 'react'
import { AppState, NativeModules, Platform } from 'react-native'
import { type ThemeName, useTheme } from './theme'

// The launcher icon follows the theme. Android has no API to swap an app's
// icon directly; instead the manifest declares one launcher entry per theme
// (an <activity-alias>, each with its own icon) and exactly one is enabled at
// a time — see android/.../AppIconModule.kt. Ported from super-chat.

interface NativeAppIcon {
  setIcon(name: string): Promise<boolean>
  getIcon(): Promise<string>
}

function native(): NativeAppIcon | undefined {
  return Platform.OS === 'android'
    ? (NativeModules.AppIcon as NativeAppIcon | undefined)
    : undefined
}

async function setAppIcon(name: ThemeName): Promise<boolean> {
  try {
    return (await native()?.setIcon(name)) ?? false
  } catch {
    return false // a launcher hiccup must never take the app down
  }
}

async function getAppIcon(): Promise<ThemeName | null> {
  try {
    return ((await native()?.getIcon()) as ThemeName | undefined) ?? null
  } catch {
    return null
  }
}

// Keeps the launcher icon in step with the theme, but only ever changes it as
// the app goes to the background. Switching while the app is open can make
// some launchers flicker, drop a pinned shortcut, or restart the app; done
// once the user has already left, none of that is visible.
export function useThemedAppIcon(): void {
  const theme = useTheme()
  const want = useRef<ThemeName>(theme.name)
  want.current = theme.name
  // What the launcher is showing now (null until we've asked).
  const applied = useRef<ThemeName | null>(null)

  useEffect(() => {
    if (!native()) return
    let alive = true
    getAppIcon().then((cur) => {
      if (alive) applied.current = cur
    })
    const sub = AppState.addEventListener('change', (state) => {
      if (state !== 'background') return
      const target = want.current
      if (applied.current === target) return
      setAppIcon(target).then((ok) => {
        if (ok) applied.current = target
      })
    })
    return () => {
      alive = false
      sub.remove()
    }
  }, [])
}
