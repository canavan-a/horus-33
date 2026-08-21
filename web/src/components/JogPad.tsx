import { useCallback, useEffect, useRef, useState } from 'react'
import { ArrowDown, ArrowLeft, ArrowRight, ArrowUp, Settings } from 'lucide-react'
import type { Control, Values } from '@/lib/proto'
import { patchControl } from '@/lib/api'
import { Card, CardContent, CardHeader } from '@/components/ui/card'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'
import { useSettingsVisible } from '@/hooks/useSettingsVisible'

// A tap shorter than this is a burst (fixed-duration nudge); held longer, it
// becomes continuous jogging that stops on release. The protocol has no
// "move N steps" command (docs/protocol.md) -- this is the host-timed `run`
// on/off fallback the issue calls out, less precise than firmware-side step
// counting but needing no device change.
const HOLD_THRESHOLD_MS = 250
const BURST_MS = 120

type Direction = 'up' | 'down' | 'left' | 'right'

interface AxisMove {
  controlId: string
  dir: 'fwd' | 'rev'
}

// Direction sense is per-axis and already corrected on the device via
// invert_dir (docs/protocol.md), so this mapping is just a label -- it does
// not need to know which way is physically "up".
const MOVES: Record<Direction, AxisMove> = {
  up: { controlId: 'axis_y', dir: 'fwd' },
  down: { controlId: 'axis_y', dir: 'rev' },
  left: { controlId: 'axis_x', dir: 'rev' },
  right: { controlId: 'axis_x', dir: 'fwd' },
}

interface ActiveMove {
  holdTimer: ReturnType<typeof setTimeout>
  burstTimer?: ReturnType<typeof setTimeout>
  held: boolean
}

interface JogPadProps {
  motion?: Control
  motionValues: Values
  axisX?: Control
  axisXValues: Values
  axisY?: Control
  axisYValues: Values
}

export function JogPad({ motion, motionValues, axisX, axisXValues, axisY, axisYValues }: JogPadProps) {
  const activeRef = useRef<Partial<Record<Direction, ActiveMove>>>({})
  const [pressed, setPressed] = useState<Partial<Record<Direction, boolean>>>({})
  const [settingsVisible, toggleSettingsVisible] = useSettingsVisible()
  const mode = typeof motionValues.mode === 'string' ? motionValues.mode : 'manual'
  const manual = mode === 'manual'

  const setRun = useCallback((controlId: string, dir: 'fwd' | 'rev', run: boolean) => {
    patchControl(controlId, run ? { run: true, dir } : { run: false }).catch(() => {})
  }, [])

  const stop = useCallback(
    (direction: Direction) => {
      const active = activeRef.current[direction]
      if (!active) return
      clearTimeout(active.holdTimer)
      if (active.burstTimer) clearTimeout(active.burstTimer)
      delete activeRef.current[direction]
      setPressed((p) => ({ ...p, [direction]: false }))
      const move = MOVES[direction]
      setRun(move.controlId, move.dir, false)
    },
    [setRun],
  )

  const start = useCallback(
    (direction: Direction) => {
      if (!manual || activeRef.current[direction]) return
      const move = MOVES[direction]
      setRun(move.controlId, move.dir, true)
      setPressed((p) => ({ ...p, [direction]: true }))
      const holdTimer = setTimeout(() => {
        const active = activeRef.current[direction]
        if (active) active.held = true
      }, HOLD_THRESHOLD_MS)
      activeRef.current[direction] = { holdTimer, held: false }
    },
    [manual, setRun],
  )

  // Release before the hold threshold still gets a full burst: keep the axis
  // running until BURST_MS total has elapsed instead of stopping the instant
  // the pointer/key comes up, so a fast tap isn't a smaller nudge than a slow
  // one.
  const release = useCallback(
    (direction: Direction) => {
      const active = activeRef.current[direction]
      if (!active) return
      if (active.held) {
        stop(direction)
        return
      }
      clearTimeout(active.holdTimer)
      active.burstTimer = setTimeout(() => stop(direction), BURST_MS)
    },
    [stop],
  )

  useEffect(() => {
    const active = activeRef.current
    return () => {
      Object.values(active).forEach((move) => {
        clearTimeout(move.holdTimer)
        if (move.burstTimer) clearTimeout(move.burstTimer)
      })
    }
  }, [])

  useEffect(() => {
    if (!manual) return
    const keyToDirection: Record<string, Direction> = {
      ArrowUp: 'up',
      ArrowDown: 'down',
      ArrowLeft: 'left',
      ArrowRight: 'right',
    }
    const onKeyDown = (e: KeyboardEvent) => {
      const direction = keyToDirection[e.key]
      // e.repeat filters OS key-repeat so a held arrow key doesn't retrigger
      // start() -- the hold/burst distinction is timed here, not by the OS.
      if (!direction || e.repeat) return
      e.preventDefault()
      start(direction)
    }
    const onKeyUp = (e: KeyboardEvent) => {
      const direction = keyToDirection[e.key]
      if (!direction) return
      e.preventDefault()
      release(direction)
    }
    window.addEventListener('keydown', onKeyDown)
    window.addEventListener('keyup', onKeyUp)
    return () => {
      window.removeEventListener('keydown', onKeyDown)
      window.removeEventListener('keyup', onKeyUp)
    }
  }, [manual, start, release])

  if (!axisX || !axisY) return null

  const toggleMode = () => {
    if (!motion) return
    patchControl(motion.id, { mode: manual ? 'pid' : 'manual' }).catch(() => {})
  }

  // `home` mode is its own loop on the device (motion_control.cpp), separate
  // from manual/pid, so it seeks `home` regardless of whether pid is enabled
  // or actively tracking a target -- no dependency on "nothing being tracked."
  const goHome = () => {
    if (!motion) return
    patchControl(motion.id, { mode: 'home' }).catch(() => {})
  }

  // "Set home" is "navigate here, then remember it": home := pos, which
  // leaves the two equal regardless of whatever pos was before -- including
  // if pos had drifted from missed steps (no encoder exists to know). That
  // makes it self-correcting for drift too: everything downstream (PID's
  // homeStep, the planned soft endstops in #5) only cares that pos and home
  // agree, not their absolute values, so there's no separate "fix drift"
  // operation needed -- redefining home to wherever you're physically
  // standing already re-syncs both. Both axes at once, since the common case
  // is framing a whole scene, not one axis independently.
  const setHome = () => {
    const x = axisXValues.pos
    const y = axisYValues.pos
    if (typeof x === 'number') patchControl(axisX.id, { home: x }).catch(() => {})
    if (typeof y === 'number') patchControl(axisY.id, { home: y }).catch(() => {})
  }

  const pad = (direction: Direction, Icon: typeof ArrowUp, gridArea: string) => (
    <Button
      key={direction}
      type="button"
      variant={pressed[direction] ? 'default' : 'outline'}
      size="icon"
      className={cn('size-14', !manual && 'opacity-40')}
      disabled={!manual}
      style={{ gridArea }}
      onPointerDown={(e) => {
        e.preventDefault()
        e.currentTarget.setPointerCapture(e.pointerId)
        start(direction)
      }}
      onPointerUp={() => release(direction)}
      onPointerCancel={() => release(direction)}
    >
      <Icon className="size-6" />
    </Button>
  )

  return (
    <Card>
      {motion && (
        <CardHeader className="flex-row items-center space-y-0">
          <Button type="button" size="sm" variant={manual ? 'default' : 'outline'} onClick={toggleMode}>
            {mode}
          </Button>
        </CardHeader>
      )}
      <CardContent>
        <div
          className="mx-auto grid w-fit gap-2"
          style={{
            gridTemplateAreas: '". up ." "left . right" ". down ."',
            gridTemplateColumns: 'repeat(3, 3.5rem)',
            gridTemplateRows: 'repeat(3, 3.5rem)',
          }}
        >
          {pad('up', ArrowUp, 'up')}
          {pad('left', ArrowLeft, 'left')}
          {pad('right', ArrowRight, 'right')}
          {pad('down', ArrowDown, 'down')}
        </div>
        {!manual && (
          <p className="mt-2 text-center text-xs text-muted-foreground">switch to manual mode to jog</p>
        )}
        {manual && (
          <p className="mt-2 text-center text-xs text-muted-foreground">
            tap to nudge, hold to jog -- arrow keys work too
          </p>
        )}
        {motion && (
          <div className="mt-3 flex flex-wrap items-center justify-center gap-2">
            <Button type="button" size="sm" variant="outline" onClick={setHome} disabled={!manual}>
              Set home
            </Button>
            <Button type="button" size="sm" variant="outline" onClick={goHome}>
              Home
            </Button>
            <Button
              type="button"
              size="icon-sm"
              variant={settingsVisible ? 'default' : 'outline'}
              aria-label={settingsVisible ? 'hide settings' : 'show settings'}
              onClick={toggleSettingsVisible}
            >
              <Settings className="size-4" />
            </Button>
          </div>
        )}
      </CardContent>
    </Card>
  )
}
