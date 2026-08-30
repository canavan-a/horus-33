import { useEffect, useState } from 'react'

export type AppMode = 'simple' | 'advanced'

const KEY = 'horus:mode'
// Same-tab instances of this hook don't see each other's localStorage writes
// (the browser's `storage` event only fires in *other* tabs), so broadcast
// changes on a custom event too, to keep every mounted instance in sync.
const EVENT = 'horus:modeChange'

function read(): AppMode {
  try {
    // Anything other than an explicit 'advanced' (unset, 'simple', junk,
    // storage disabled) means the locked-down default.
    return localStorage.getItem(KEY) === 'advanced' ? 'advanced' : 'simple'
  } catch {
    return 'simple'
  }
}

function write(mode: AppMode) {
  try {
    localStorage.setItem(KEY, mode)
  } catch {
    // Ignore -- see read().
  }
  window.dispatchEvent(new Event(EVENT))
}

// Display mode for the whole app. 'simple' (the default when unset) hides the
// camera controls and locks the clip-recording toggle, leaving a view-only
// stream + clips UI. 'advanced' is the full UI, reached via the secret menu.
export function useAppMode(): [AppMode, (mode: AppMode) => void] {
  const [mode, setMode] = useState<AppMode>(() => read())

  useEffect(() => {
    const onChange = () => setMode(read())
    window.addEventListener(EVENT, onChange)
    window.addEventListener('storage', onChange)
    return () => {
      window.removeEventListener(EVENT, onChange)
      window.removeEventListener('storage', onChange)
    }
  }, [])

  return [mode, write]
}
