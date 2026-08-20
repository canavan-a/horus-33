import { useCallback, useEffect, useState } from 'react'
import { ChevronDown, ChevronUp, Trash2 } from 'lucide-react'
import type { Clip, ClippingStatus } from '@/lib/proto'
import { deleteClip, listClips, setClippingEnabled } from '@/lib/api'
import { usePersistedCollapse } from '@/hooks/usePersistedCollapse'
import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Label } from '@/components/ui/label'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog'

function formatSize(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.round(bytes / 1024)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

interface ClipRowProps {
  clip: Clip
  expanded: boolean
  onToggle: () => void
  onDeleted: () => void
}

function ClipRow({ clip, expanded, onToggle, onDeleted }: ClipRowProps) {
  const [confirmOpen, setConfirmOpen] = useState(false)
  const [deleting, setDeleting] = useState(false)
  const [error, setError] = useState<string | undefined>(undefined)

  const handleDelete = useCallback(() => {
    setDeleting(true)
    deleteClip(clip.name)
      .then(() => {
        setConfirmOpen(false)
        onDeleted()
      })
      .catch((err) => setError(err.message))
      .finally(() => setDeleting(false))
  }, [clip.name, onDeleted])

  return (
    <Card>
      <CardContent
        className={cn('flex gap-3 py-3', expanded ? 'flex-col' : 'items-center')}
      >
        {expanded ? (
          // eslint-disable-next-line jsx-a11y/media-has-caption
          <video
            controls
            autoPlay
            className="aspect-video w-full rounded bg-muted"
            src={`/api/clips/${encodeURIComponent(clip.name)}`}
          />
        ) : (
          <button
            type="button"
            onClick={onToggle}
            className="relative aspect-video w-64 shrink-0 overflow-hidden rounded bg-muted"
          >
            {clip.thumbnail ? (
              <img
                src={`/api/clips/${encodeURIComponent(clip.name)}/thumbnail`}
                alt=""
                className="size-full object-cover"
              />
            ) : (
              <div className="flex size-full items-center justify-center text-xs text-muted-foreground">
                no preview
              </div>
            )}
          </button>
        )}

        <div className={cn('flex items-center gap-3', expanded && 'w-full')}>
          <div className="min-w-0 flex-1">
            <p className="truncate text-sm font-medium">{clip.name}</p>
            <p className="text-xs text-muted-foreground">
              {new Date(clip.modTime).toLocaleString()} · {formatSize(clip.size)}
            </p>
            {error && <p className="text-xs text-destructive">{error}</p>}
          </div>

          {expanded && (
            <Button variant="outline" size="sm" onClick={onToggle}>
              collapse
            </Button>
          )}

          <AlertDialog open={confirmOpen} onOpenChange={setConfirmOpen}>
            <Button
              variant="outline"
              size="icon-sm"
              aria-label="delete clip"
              onClick={() => setConfirmOpen(true)}
            >
              <Trash2 />
            </Button>
            <AlertDialogContent>
            <AlertDialogHeader>
              <AlertDialogTitle>Delete this clip?</AlertDialogTitle>
              <AlertDialogDescription>
                {clip.name} and its thumbnail will be permanently deleted. This can't be undone.
              </AlertDialogDescription>
            </AlertDialogHeader>
            <AlertDialogFooter>
              <AlertDialogCancel disabled={deleting}>cancel</AlertDialogCancel>
              <AlertDialogAction
                onClick={(e) => {
                  e.preventDefault()
                  handleDelete()
                }}
                disabled={deleting}
                className="bg-destructive text-white hover:bg-destructive/90"
              >
                {deleting ? 'deleting…' : 'delete'}
              </AlertDialogAction>
            </AlertDialogFooter>
            </AlertDialogContent>
          </AlertDialog>
        </div>
      </CardContent>
    </Card>
  )
}

interface ClipsPanelProps {
  clipping?: ClippingStatus
}

export function ClipsPanel({ clipping }: ClipsPanelProps) {
  const [clips, setClips] = useState<Clip[]>([])
  const [error, setError] = useState<string | undefined>(undefined)
  const [toggling, setToggling] = useState(false)
  const [expanded, setExpanded] = useState<string | undefined>(undefined)
  // Local, optimistic view of clipping status. Seeded from the prop (the
  // WS-pushed status) but overwritten immediately with the toggle response
  // rather than waiting on the next status broadcast, which can lag up to
  // clipStatusPollInterval (3s) behind — that lag was reading as "the toggle
  // is broken" since the switch would snap back to the old value.
  const [local, setLocal] = useState<ClippingStatus | undefined>(clipping)
  const [collapsed, toggleCollapsed] = usePersistedCollapse('panel:clipping')

  useEffect(() => {
    setLocal(clipping)
  }, [clipping])

  const refresh = useCallback(() => {
    listClips()
      .then(setClips)
      .catch((err) => setError(err.message))
  }, [])

  useEffect(() => {
    refresh()
  }, [refresh])

  // Re-fetch the list whenever the recording state changes — a status
  // broadcast is enough to know a clip probably appeared or is in progress,
  // without a separate live push of the list itself.
  useEffect(() => {
    refresh()
  }, [clipping?.recording, refresh])

  const handleToggle = useCallback(() => {
    if (local === undefined) return
    const next = !local.enabled
    setToggling(true)
    setLocal({ ...local, enabled: next })
    setClippingEnabled(next)
      .then(setLocal)
      .then(() => setError(undefined))
      .catch((err) => {
        setLocal(clipping) // roll back the optimistic flip
        setError(err.message)
      })
      .finally(() => setToggling(false))
  }, [local, clipping])

  const handleRowToggle = useCallback((name: string) => {
    setExpanded((current) => (current === name ? undefined : name))
  }, [])

  const handleDeleted = useCallback((name: string) => {
    setClips((current) => current.filter((c) => c.name !== name))
    setExpanded((current) => (current === name ? undefined : current))
  }, [])

  return (
    <div className="space-y-4">
      <Card>
        <CardHeader
          className="flex-row items-center justify-between space-y-0 cursor-pointer select-none"
          onClick={toggleCollapsed}
        >
          <CardTitle>clipping</CardTitle>
          <div className="flex items-center gap-2">
            {error && <p className="text-sm text-destructive">{error}</p>}
            <Button variant="ghost" size="icon-sm" aria-label={collapsed ? 'expand' : 'collapse'}>
              {collapsed ? <ChevronDown className="size-4" /> : <ChevronUp className="size-4" />}
            </Button>
          </div>
        </CardHeader>
        {!collapsed && (
          <CardContent className="space-y-3">
            <div className="flex items-center justify-between">
              <Label>record clips when someone is in frame</Label>
              <div className="flex items-center gap-2">
                {local?.recording && (
                  <span className="text-xs font-medium text-destructive">recording…</span>
                )}
                <Button
                  type="button"
                  size="sm"
                  variant={local?.enabled ? 'default' : 'outline'}
                  disabled={toggling || local === undefined}
                  onClick={handleToggle}
                  className={local?.enabled ? 'bg-green-600 hover:bg-green-700' : ''}
                >
                  {toggling ? 'saving…' : local?.enabled ? 'ON' : 'OFF'}
                </Button>
              </div>
            </div>
            {local === undefined && (
              <p className="text-xs text-muted-foreground">clip admin not available</p>
            )}
          </CardContent>
        )}
      </Card>

      {clips.length === 0 && (
        <p className="py-8 text-center text-sm text-muted-foreground">no clips yet</p>
      )}

      <div className="space-y-2">
        {clips.map((clip) => (
          <ClipRow
            key={clip.name}
            clip={clip}
            expanded={expanded === clip.name}
            onToggle={() => handleRowToggle(clip.name)}
            onDeleted={() => handleDeleted(clip.name)}
          />
        ))}
      </div>
    </div>
  )
}
