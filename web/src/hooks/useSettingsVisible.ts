import { useCallback, useEffect, useState } from 'react'

const KEY = 'horus:settingsVisible'
// Same-tab instances of this hook don't see each other's localStorage writes
// (the browser's `storage` event only fires in *other* tabs), so broadcast
// changes on a custom event too, to keep every mounted instance in sync.
const EVENT = 'horus:settingsVisibleChange'

function read(): boolean {
  try {
    return localStorage.getItem(KEY) !== '0'
  } catch {
    // Private browsing / disabled storage: fall back to in-memory only.
    return true
  }
}

function write(visible: boolean) {
  try {
    localStorage.setItem(KEY, visible ? '1' : '0')
  } catch {
    // Ignore -- see read().
  }
  window.dispatchEvent(new Event(EVENT))
}

// Global on/off for the per-axis control panels (PID gains, offsets, etc.).
// Defaults to visible so existing users see no change until they opt out.
export function useSettingsVisible(): [boolean, () => void] {
  const [visible, setVisible] = useState(() => read())

  useEffect(() => {
    const onChange = () => setVisible(read())
    window.addEventListener(EVENT, onChange)
    window.addEventListener('storage', onChange)
    return () => {
      window.removeEventListener(EVENT, onChange)
      window.removeEventListener('storage', onChange)
    }
  }, [])

  const toggle = useCallback(() => {
    write(!read())
  }, [])

  return [visible, toggle]
}
