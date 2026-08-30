import { useCallback, useEffect, useRef, useState } from 'react'
import { Trash2 } from 'lucide-react'
import type { Clip, ClippingStatus } from '@/lib/proto'
import { deleteClip, listClips, setClippingEnabled } from '@/lib/api'
import { useViewedClips } from '@/hooks/useViewedClips'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader } from '@/components/ui/card'
import { Label } from '@/components/ui/label'

const PAGE = 30
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
  viewed: boolean
  onToggle: () => void
  onDeleted: () => void
}

function ClipRow({ clip, expanded, viewed, onToggle, onDeleted }: ClipRowProps) {
  // Dim only when collapsed — an expanded clip is being watched right now, so
  // it always shows in full colour.
  const dim = viewed && !expanded
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
        className={cn(
          'group flex gap-3 py-3',
          expanded ? 'flex-col' : 'items-center',
        )}
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
            className="relative aspect-video w-32 shrink-0 overflow-hidden rounded bg-muted sm:w-64"
          >
            {clip.thumbnail ? (
              <img
                src={`/api/clips/${encodeURIComponent(clip.name)}/thumbnail`}
                alt=""
                className={cn(
                  'size-full object-cover transition',
                  dim &&
                    'opacity-40 grayscale group-hover:opacity-100 group-hover:grayscale-0',
                )}
              />
            ) : (
              <div className="flex size-full items-center justify-center text-xs text-muted-foreground">
                no preview
              </div>
            )}
          </button>
        )}

        <div
          className={cn(
            'flex flex-col gap-3 sm:flex-row sm:items-center',
            expanded && 'w-full',
          )}
        >
          <div className="min-w-0 flex-1">
            <div className="flex items-center gap-2">
              <p
                className={cn(
                  'min-w-0 break-words text-sm font-medium transition-colors',
                  dim && 'text-muted-foreground group-hover:text-foreground',
                )}
              >
                {clip.name}
              </p>
              {dim && (
                <Badge
                  variant="secondary"
                  className="shrink-0 font-normal text-muted-foreground"
                >
                  viewed
                </Badge>
              )}
            </div>
            <p className="text-xs text-muted-foreground">
              {new Date(clip.modTime).toLocaleString()} · {formatSize(clip.size)}
            </p>
            {error && <p className="text-xs text-destructive">{error}</p>}
          </div>

          <div className="flex items-center gap-3 self-end sm:self-auto">
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
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | undefined>(undefined)
  const [toggling, setToggling] = useState(false)
  const [expanded, setExpanded] = useState<string | undefined>(undefined)
  const { isViewed, markViewed } = useViewedClips()
  const sentinelRef = useRef<HTMLDivElement>(null)
  // Guards against overlapping fetches without waiting on a state re-render.
  const loadingRef = useRef(false)
  // Local, optimistic view of clipping status. Seeded from the prop (the
  // WS-pushed status) but overwritten immediately with the toggle response
  // rather than waiting on the next status broadcast, which can lag up to
  // clipStatusPollInterval (3s) behind — that lag was reading as "the toggle
  // is broken" since the switch would snap back to the old value.
  const [local, setLocal] = useState<ClippingStatus | undefined>(clipping)

  useEffect(() => {
    setLocal(clipping)
  }, [clipping])

  // Fetch one page. `replace` starts the list over from the top (initial load
  // or after a new clip likely landed); otherwise the page is appended.
  const loadPage = useCallback((offset: number, replace: boolean) => {
    if (loadingRef.current) return
    loadingRef.current = true
    setLoading(true)
    listClips(offset, PAGE)
      .then((page) => {
        setTotal(page.total)
        setClips((prev) => {
          const base = replace ? [] : prev
          const seen = new Set(base.map((c) => c.name))
          // Dedupe by name: a clip recorded mid-scroll shifts every offset by
          // one, which would otherwise re-serve a row we already have.
          return [...base, ...page.clips.filter((c) => !seen.has(c.name))]
        })
        setError(undefined)
      })
      .catch((err) => setError(err.message))
      .finally(() => {
        loadingRef.current = false
        setLoading(false)
      })
  }, [])

  useEffect(() => {
    loadPage(0, true)
  }, [loadPage])

  // Re-fetch from the top whenever the recording state changes — a status
  // broadcast is enough to know a clip probably appeared, without a separate
  // live push of the list itself.
  useEffect(() => {
    loadPage(0, true)
  }, [clipping?.recording, loadPage])

  // Grow the list as the bottom sentinel scrolls into view. Re-created on every
  // count/total/loading change so the callback always sees fresh values.
  useEffect(() => {
    const el = sentinelRef.current
    if (!el || clips.length >= total) return
    const io = new IntersectionObserver(
      (entries) => {
        if (entries[0].isIntersecting && !loadingRef.current) {
          loadPage(clips.length, false)
        }
      },
      { rootMargin: '400px' },
    )
    io.observe(el)
    return () => io.disconnect()
  }, [clips.length, total, loading, loadPage])

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
    setExpanded((current) => {
      if (current === name) return undefined
      markViewed(name) // expanding autoplays the clip — count it as watched
      return name
    })
  }, [markViewed])

  const handleDeleted = useCallback((name: string) => {
    setClips((current) => current.filter((c) => c.name !== name))
    setTotal((t) => Math.max(0, t - 1))
    setExpanded((current) => (current === name ? undefined : current))
  }, [])

  return (
    <div className="space-y-4">
      <Card>
        {error && (
          <CardHeader className="space-y-0">
            <p className="text-sm text-destructive">{error}</p>
          </CardHeader>
        )}
        <CardContent className="space-y-3">
          <div className="flex items-center justify-between gap-2">
            <Label className="min-w-0">record clips when someone is in frame</Label>
            <div className="flex shrink-0 items-center gap-2">
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
      </Card>

      {!loading && total === 0 && (
        <p className="py-8 text-center text-sm text-muted-foreground">no clips yet</p>
      )}

      <div className="space-y-2">
        {clips.map((clip) => (
          <ClipRow
            key={clip.name}
            clip={clip}
            expanded={expanded === clip.name}
            viewed={isViewed(clip.name)}
            onToggle={() => handleRowToggle(clip.name)}
            onDeleted={() => handleDeleted(clip.name)}
          />
        ))}
      </div>

      <div ref={sentinelRef} className="h-px" />

      {loading && (
        <p className="py-4 text-center text-xs text-muted-foreground">loading…</p>
      )}
      {!loading && total > 0 && clips.length >= total && (
        <p className="py-4 text-center text-xs text-muted-foreground">
          {total} clip{total === 1 ? '' : 's'}
        </p>
      )}
    </div>
  )
}
