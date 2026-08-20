import { useCallback, useEffect, useRef, useState } from 'react'
import { ChevronDown, ChevronUp } from 'lucide-react'
import type { Control, Field, Values } from '@/lib/proto'
import { patchControl } from '@/lib/api'
import { usePersistedCollapse } from '@/hooks/usePersistedCollapse'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Switch } from '@/components/ui/switch'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'

// Matches horusctl's debounce (tui-controller/internal/ui/model.go): the
// device's inbound queue is 8 deep, and a slider dragged at 60Hz would
// overrun it long before a human notices the lag from waiting 80ms.
const DEBOUNCE_MS = 80

function parseColor(hex: string): [number, number, number] {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex)
  if (!m) return [0, 0, 0]
  return [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)]
}

function formatColor(r: number, g: number, b: number): string {
  const c = (n: number) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, '0')
  return `#${c(r)}${c(g)}${c(b)}`
}

interface FieldRowProps {
  field: Field
  value: unknown
  onChange: (key: string, value: unknown) => void
}

function FieldRow({ field, value, onChange }: FieldRowProps) {
  switch (field.type) {
    case 'number': {
      const n = typeof value === 'number' ? value : Number(value) || 0
      return (
        <div className="space-y-1.5">
          <div className="flex items-center justify-between">
            <Label>
              {field.label}
              {field.unit ? <span className="text-muted-foreground"> ({field.unit})</span> : null}
            </Label>
            <Input
              type="number"
              min={field.min}
              max={field.max}
              step={field.step ?? 1}
              value={n}
              onChange={(e) => onChange(field.key, Number(e.target.value))}
              className="h-8 w-20 text-right"
            />
          </div>
          <input
            type="range"
            min={field.min}
            max={field.max}
            step={field.step ?? 1}
            value={n}
            onChange={(e) => onChange(field.key, Number(e.target.value))}
            className="h-6 w-full cursor-pointer accent-primary touch-none"
          />
        </div>
      )
    }

    case 'color': {
      const hex = typeof value === 'string' ? value : '#000000'
      const [r, g, b] = parseColor(hex)
      const setChannel = (i: number, v: number) => {
        const channels: [number, number, number] = [r, g, b]
        channels[i] = v
        onChange(field.key, formatColor(...channels))
      }
      return (
        <div className="space-y-1.5">
          <div className="flex items-center justify-between">
            <Label>{field.label}</Label>
            <div className="flex items-center gap-2">
              <span
                className="size-6 rounded-full border"
                style={{ background: hex }}
                aria-hidden
              />
              <input
                type="color"
                value={hex}
                onChange={(e) => onChange(field.key, e.target.value)}
                className="h-8 w-10 cursor-pointer rounded border bg-transparent p-0.5"
              />
            </div>
          </div>
          <div className="grid grid-cols-3 gap-2">
            {(['R', 'G', 'B'] as const).map((label, i) => (
              <div key={label} className="flex items-center gap-1.5">
                <span className="text-xs text-muted-foreground">{label}</span>
                <Input
                  type="number"
                  min={0}
                  max={255}
                  value={[r, g, b][i]}
                  onChange={(e) => setChannel(i, Number(e.target.value))}
                  className="h-8"
                />
              </div>
            ))}
          </div>
        </div>
      )
    }

    case 'enum': {
      const current = typeof value === 'string' ? value : ''
      return (
        <div className="space-y-1.5">
          <Label>{field.label}</Label>
          <div className="flex flex-wrap gap-1.5">
            {(field.options ?? []).map((opt) => (
              <Button
                key={opt}
                type="button"
                size="sm"
                variant={opt === current ? 'default' : 'outline'}
                onClick={() => onChange(field.key, opt)}
              >
                {opt}
              </Button>
            ))}
          </div>
        </div>
      )
    }

    case 'bool': {
      const on = Boolean(value)
      return (
        <div className="flex items-center justify-between">
          <Label>{field.label}</Label>
          <div className="flex items-center gap-2">
            <span className={cn('text-xs', on ? 'text-foreground' : 'text-muted-foreground')}>
              {on ? 'on' : 'off'}
            </span>
            <Switch checked={on} onCheckedChange={(v) => onChange(field.key, v)} />
          </div>
        </div>
      )
    }

    default:
      // Unrecognized field type: read-only, not rejected — an older client
      // stays usable against a newer firmware that adds a field type it
      // doesn't understand yet.
      return (
        <div className="flex items-center justify-between">
          <Label>{field.label}</Label>
          <span className="text-xs text-muted-foreground">
            {String(value)} (unsupported type "{field.type}")
          </span>
        </div>
      )
  }
}

interface ControlPanelProps {
  control: Control
  values: Values
}

export function ControlPanel({ control, values }: ControlPanelProps) {
  // Local draft overlays the server's live values so the UI responds
  // instantly to a drag; confirmed values arrive later over the WebSocket and
  // replace the draft once no edit is in flight.
  const [draft, setDraft] = useState<Values>({})
  const pendingRef = useRef<Values>({})
  const timerRef = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)
  const [error, setError] = useState<string | undefined>(undefined)
  const [collapsed, toggleCollapsed] = usePersistedCollapse(`panel:${control.id}`)

  useEffect(() => () => clearTimeout(timerRef.current), [])

  const flush = useCallback(() => {
    const toSend = pendingRef.current
    pendingRef.current = {}
    if (Object.keys(toSend).length === 0) return
    patchControl(control.id, toSend)
      .then(() => setError(undefined))
      .catch((err) => setError(err.message))
  }, [control.id])

  const onChange = useCallback(
    (key: string, value: unknown) => {
      setDraft((d) => ({ ...d, [key]: value }))
      pendingRef.current = { ...pendingRef.current, [key]: value }
      clearTimeout(timerRef.current)
      timerRef.current = setTimeout(flush, DEBOUNCE_MS)
    },
    [flush],
  )

  const display: Values = { ...values, ...draft }

  return (
    <Card>
      <CardHeader
        className="flex-row items-center justify-between space-y-0 cursor-pointer select-none"
        onClick={toggleCollapsed}
      >
        <CardTitle>{control.label}</CardTitle>
        <div className="flex items-center gap-2">
          {error && <p className="text-sm text-destructive">{error}</p>}
          <Button variant="ghost" size="icon-sm" aria-label={collapsed ? 'expand' : 'collapse'}>
            {collapsed ? <ChevronDown className="size-4" /> : <ChevronUp className="size-4" />}
          </Button>
        </div>
      </CardHeader>
      {!collapsed && (
        <CardContent className="space-y-4">
          {control.fields.map((field) => (
            <FieldRow key={field.key} field={field} value={display[field.key]} onChange={onChange} />
          ))}
        </CardContent>
      )}
    </Card>
  )
}
