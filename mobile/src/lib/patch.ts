import { patchControl } from '../api/client'
import type { Values } from './proto'

// Mirror of the web ControlPanel's DEBOUNCE_MS: coalesce rapid field writes
// (slider drags, preset taps) into one PATCH per control. Jog run/stop bypass
// this entirely — they must be immediate and must never be merged with each
// other.
const DEBOUNCE_MS = 80

const pending = new Map<string, Values>()
let timer: ReturnType<typeof setTimeout> | undefined

function flush() {
  timer = undefined
  for (const [id, values] of pending) {
    patchControl(id, values).catch(() => {})
  }
  pending.clear()
}

export function patch(id: string, values: Values) {
  if ('run' in values) {
    // Immediate: jog start/stop.
    patchControl(id, values).catch(() => {})
    return
  }
  pending.set(id, { ...(pending.get(id) ?? {}), ...values })
  if (!timer) timer = setTimeout(flush, DEBOUNCE_MS)
}
