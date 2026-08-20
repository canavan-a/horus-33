import { useCallback, useState } from 'react'
import { useHorus } from '@/hooks/useHorus'
import { ControlPanel } from '@/components/ControlPanel'
import { JogPad } from '@/components/JogPad'
import { VideoPanel } from '@/components/VideoPanel'
import { ClipsPanel } from '@/components/ClipsPanel'
import { Button } from '@/components/ui/button'
import { Badge } from '@/components/ui/badge'
import { estop } from '@/lib/api'
import { cn } from '@/lib/utils'
import type { LinkStatus } from '@/lib/proto'

const STATUS_LABEL: Record<LinkStatus, string> = {
  connecting: 'connecting…',
  describing: 'describing…',
  ready: 'connected',
  lost: 'disconnected',
}

function LinkBadge({ status }: { status: LinkStatus }) {
  return (
    <Badge
      variant={status === 'ready' ? 'default' : status === 'lost' ? 'destructive' : 'secondary'}
      className={cn(status !== 'ready' && status !== 'lost' && 'animate-pulse')}
    >
      {STATUS_LABEL[status]}
    </Badge>
  )
}

function App() {
  const { linkStatus, linkError, descriptor, state, clipping } = useHorus()
  const [stopping, setStopping] = useState(false)
  const [stopError, setStopError] = useState<string | undefined>(undefined)
  const [view, setView] = useState<'controls' | 'clips'>('controls')

  const handleEstop = useCallback(() => {
    setStopping(true)
    estop()
      .then(() => setStopError(undefined))
      .catch((err) => setStopError(err.message))
      .finally(() => setStopping(false))
  }, [])

  return (
    <div className="min-h-dvh bg-background">
      {/* Sticky header: e-stop must stay reachable with one thumb on a phone,
          scrolled to any point in a long control list. */}
      <header className="sticky top-0 z-10 flex items-center justify-between gap-3 border-b bg-background/95 px-4 py-3 backdrop-blur supports-[backdrop-filter]:bg-background/75">
        <div className="flex min-w-0 items-center gap-2">
          <h1 className="truncate text-base font-semibold sm:text-lg">horus-33</h1>
          <LinkBadge status={linkStatus} />
        </div>
        <div className="flex items-center gap-2">
          <Button
            variant={view === 'controls' ? 'default' : 'outline'}
            size="sm"
            onClick={() => setView('controls')}
          >
            controls
          </Button>
          <Button variant={view === 'clips' ? 'default' : 'outline'} size="sm" onClick={() => setView('clips')}>
            clips
          </Button>
          <Button variant="destructive" size="sm" onClick={handleEstop} disabled={stopping}>
            E-STOP
          </Button>
        </div>
      </header>

      {linkError && (
        <div className="border-b bg-destructive/10 px-4 py-2 text-sm text-destructive">{linkError}</div>
      )}
      {stopError && (
        <div className="border-b bg-destructive/10 px-4 py-2 text-sm text-destructive">
          e-stop failed: {stopError}
        </div>
      )}

      <main className="mx-auto flex max-w-3xl flex-col gap-4 p-4">
        <VideoPanel />

        {view === 'controls' ? (
          <>
            {!descriptor && (
              <p className="py-8 text-center text-sm text-muted-foreground">
                waiting for the device descriptor…
              </p>
            )}

            {descriptor && (
              <JogPad
                motion={descriptor.controls.find((c) => c.id === 'motion')}
                motionValues={state.motion ?? {}}
                axisX={descriptor.controls.find((c) => c.id === 'axis_x')}
                axisXValues={state.axis_x ?? {}}
                axisY={descriptor.controls.find((c) => c.id === 'axis_y')}
                axisYValues={state.axis_y ?? {}}
              />
            )}

            {descriptor && (
              // Single column on phones; two columns once there is room, since a
              // control panel's fields (sliders + numeric entry) want width more
              // than a phone screen wants density. Each panel collapses on its
              // own (ControlPanel), so this grid itself has nothing to toggle.
              <div className="grid grid-cols-1 gap-4 md:grid-cols-2">
                {descriptor.controls.map((control) => (
                  <ControlPanel key={control.id} control={control} values={state[control.id] ?? {}} />
                ))}
              </div>
            )}
          </>
        ) : (
          <ClipsPanel clipping={clipping} />
        )}
      </main>
    </div>
  )
}

export default App
