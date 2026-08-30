import { useEffect, useRef, useState } from 'react'
import { useHorus } from '@/hooks/useHorus'
import { ControlPanel } from '@/components/ControlPanel'
import { JogPad } from '@/components/JogPad'
import { VideoPanel } from '@/components/VideoPanel'
import { ClipsPanel } from '@/components/ClipsPanel'
import { SecretMenu } from '@/components/SecretMenu'
import { Button } from '@/components/ui/button'
import { Badge } from '@/components/ui/badge'
import { cn } from '@/lib/utils'
import { useSettingsVisible } from '@/hooks/useSettingsVisible'
import { useAppMode } from '@/hooks/useAppMode'
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
  const [view, setView] = useState<'controls' | 'clips'>('controls')
  const [settingsVisible] = useSettingsVisible()
  const [mode] = useAppMode()
  const simple = mode === 'simple'

  // Tap the header logo six times (with no pause longer than TAP_RESET_MS
  // between taps) to open the hidden display-mode menu.
  const [secretOpen, setSecretOpen] = useState(false)
  const tapCountRef = useRef(0)
  const tapTimerRef = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)

  useEffect(() => () => clearTimeout(tapTimerRef.current), [])

  const handleLogoTap = () => {
    clearTimeout(tapTimerRef.current)
    tapCountRef.current += 1
    if (tapCountRef.current >= 6) {
      tapCountRef.current = 0
      setSecretOpen(true)
      return
    }
    tapTimerRef.current = setTimeout(() => {
      tapCountRef.current = 0
    }, 600)
  }

  return (
    <div className="min-h-dvh bg-background">
      <header className="sticky top-0 z-10 flex items-center justify-between gap-3 border-b bg-background/95 px-4 py-3 backdrop-blur supports-[backdrop-filter]:bg-background/75">
        <div className="flex min-w-0 items-center gap-2">
          <button
            type="button"
            onClick={handleLogoTap}
            aria-label="horus-33"
            className="shrink-0 rounded outline-none"
          >
            <img src="/eye-of-horus.svg" alt="" className="size-6 rounded" />
          </button>
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
        </div>
      </header>

      {linkError && (
        <div className="border-b bg-destructive/10 px-4 py-2 text-sm text-destructive">{linkError}</div>
      )}

      <main className="mx-auto flex max-w-3xl flex-col gap-4 p-4">
        <VideoPanel />

        {view === 'controls' ? (
          <>
            {!simple && !descriptor && (
              <p className="py-8 text-center text-sm text-muted-foreground">
                waiting for the device descriptor…
              </p>
            )}

            {!simple && descriptor && (
              <JogPad
                motion={descriptor.controls.find((c) => c.id === 'motion')}
                motionValues={state.motion ?? {}}
                axisX={descriptor.controls.find((c) => c.id === 'axis_x')}
                axisXValues={state.axis_x ?? {}}
                axisY={descriptor.controls.find((c) => c.id === 'axis_y')}
                axisYValues={state.axis_y ?? {}}
              />
            )}

            {!simple && descriptor && settingsVisible && (
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

      <SecretMenu open={secretOpen} onOpenChange={setSecretOpen} />
    </div>
  )
}

export default App
