import { useEffect, useRef, useState } from 'react'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { consoleRestart, consoleWsUrl } from '@/lib/api'

// Admin-only in practice: this whole panel talks to /api/console/*, which is
// meant to sit behind a Cloudflare Access policy scoped to admins (see
// server/internal/api/console.go). There is no in-app role check — anyone
// who can reach this page at all can restart capture-eye.
export function ConsolePanel() {
  const [lines, setLines] = useState<string[]>([])
  const [connected, setConnected] = useState(false)
  const [restarting, setRestarting] = useState(false)
  const [restartError, setRestartError] = useState<string | undefined>()
  const logRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const ws = new WebSocket(consoleWsUrl())
    ws.onopen = () => setConnected(true)
    ws.onclose = () => setConnected(false)
    ws.onmessage = (e) => {
      setLines((prev) => {
        const next = [...prev, String(e.data)]
        return next.length > 2000 ? next.slice(next.length - 2000) : next
      })
    }
    return () => ws.close()
  }, [])

  useEffect(() => {
    logRef.current?.scrollTo({ top: logRef.current.scrollHeight })
  }, [lines])

  const handleRestart = async () => {
    setRestarting(true)
    setRestartError(undefined)
    try {
      await consoleRestart()
    } catch (err) {
      setRestartError(err instanceof Error ? err.message : String(err))
    } finally {
      setRestarting(false)
    }
  }

  return (
    <Card>
      <CardHeader className="flex-row items-center justify-between">
        <CardTitle className="flex items-center gap-2">
          console
          <span className={`size-2 rounded-full ${connected ? 'bg-green-500' : 'bg-muted-foreground/40'}`} />
        </CardTitle>
        <Button variant="destructive" size="sm" onClick={handleRestart} disabled={restarting}>
          {restarting ? 'restarting…' : 'restart capture-eye'}
        </Button>
      </CardHeader>
      <CardContent>
        {restartError && <p className="mb-2 text-sm text-destructive">{restartError}</p>}
        <div
          ref={logRef}
          className="h-96 overflow-y-auto rounded-md bg-black p-3 font-mono text-xs whitespace-pre-wrap text-green-400"
        >
          {lines.length === 0 ? (
            <span className="text-muted-foreground">waiting for log output…</span>
          ) : (
            lines.join('\n')
          )}
        </div>
      </CardContent>
    </Card>
  )
}
