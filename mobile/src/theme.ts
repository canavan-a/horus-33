// Theming: the same named palettes as super-chat and super-badger, switched
// from Settings. Layout stays in each screen's StyleSheet; only color tokens
// come from here. Video surfaces (the live stage, clip players) stay black in
// every theme — they frame footage, not UI.
import { useSyncExternalStore } from 'react'
import { getConfig, subscribeConfig } from './lib/config'

export type ThemeName = 'light' | 'dark' | 'slate' | 'sepia' | 'ember'

export interface Theme {
  name: ThemeName
  // Drives the status bar style and the navigation base theme.
  dark: boolean
  bg: string
  surface: string
  surfaceAlt: string
  border: string
  text: string
  textMuted: string
  primary: string
  primaryText: string
  danger: string
  dangerBg: string
  success: string
  codeBg: string
  codeText: string
}

const light: Theme = {
  name: 'light',
  dark: false,
  bg: '#ffffff',
  surface: '#f6f8fa',
  surfaceAlt: '#f0f0f0',
  border: '#d0d7de',
  text: '#1b1f24',
  textMuted: '#57606a',
  primary: '#0969da',
  primaryText: '#ffffff',
  danger: '#cf222e',
  dangerBg: '#ffebe9',
  success: '#1a7f37',
  codeBg: '#f6f8fa',
  codeText: '#24292f',
}

const dark: Theme = {
  name: 'dark',
  dark: true,
  bg: '#0d1117',
  surface: '#161b22',
  surfaceAlt: '#21262d',
  border: '#30363d',
  text: '#e6edf3',
  textMuted: '#8b949e',
  primary: '#2f81f7',
  primaryText: '#ffffff',
  danger: '#f85149',
  dangerBg: '#3d1418',
  success: '#3fb950',
  codeBg: '#010409',
  codeText: '#c9d1d9',
}

const slate: Theme = {
  name: 'slate',
  dark: true,
  bg: '#1e2530',
  surface: '#262e3b',
  surfaceAlt: '#2f3947',
  border: '#3c4757',
  text: '#dde3ea',
  textMuted: '#93a0b3',
  primary: '#5aa9e6',
  primaryText: '#0d1117',
  danger: '#ef6a6a',
  dangerBg: '#3a2222',
  success: '#6fcf97',
  codeBg: '#232b37',
  codeText: '#cdd6e0',
}

const sepia: Theme = {
  name: 'sepia',
  dark: false,
  bg: '#f4ecd8',
  surface: '#ece0c6',
  surfaceAlt: '#e5d7b8',
  border: '#d8c6a0',
  text: '#3b2f1e',
  textMuted: '#7a6a4d',
  primary: '#a0651b',
  primaryText: '#fdf6e7',
  danger: '#a13c2c',
  dangerBg: '#f0dcd2',
  success: '#4e7c3a',
  codeBg: '#ece0c6',
  codeText: '#3b2f1e',
}

// Same dark base as `dark` with a warm red/orange accent; danger stays a true
// red so E-STOP and destructive actions don't blend into the accent.
const ember: Theme = {
  name: 'ember',
  dark: true,
  bg: '#050403',
  surface: '#0c0908',
  surfaceAlt: '#150f0d',
  border: '#241815',
  text: '#e8d9d0',
  textMuted: '#8a6c5f',
  primary: '#8a3620',
  primaryText: '#f1e2d9',
  danger: '#c93a3a',
  dangerBg: '#1c0d0c',
  success: '#4f9e70',
  codeBg: '#0c0908',
  codeText: '#d9c9bf',
}

export const THEMES: Record<ThemeName, Theme> = { light, dark, slate, sepia, ember }

export const THEME_OPTIONS: { name: ThemeName; label: string }[] = [
  { name: 'light', label: 'Light' },
  { name: 'dark', label: 'Dark' },
  { name: 'slate', label: 'Slate' },
  { name: 'sepia', label: 'Sepia' },
  { name: 'ember', label: 'Ember' },
]

// The theme lives in HorusConfig, so it follows the same load/save/notify
// path as every other setting — no provider needed.
export function useTheme(): Theme {
  const name = useSyncExternalStore(subscribeConfig, () => getConfig().themeName)
  return THEMES[name] ?? THEMES.dark
}

// Builds a component's stylesheet once per theme and reuses it, so styles stay
// referentially stable across renders (memoized rows don't re-render).
const styleCache = new WeakMap<object, Map<ThemeName, unknown>>()

export function useThemedStyles<T>(make: (t: Theme) => T): T {
  const theme = useTheme()
  let byTheme = styleCache.get(make)
  if (!byTheme) styleCache.set(make, (byTheme = new Map()))
  if (!byTheme.has(theme.name)) byTheme.set(theme.name, make(theme))
  return byTheme.get(theme.name) as T
}
