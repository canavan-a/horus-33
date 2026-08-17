// Mirrors server/internal/proto's JSON shapes field-for-field. Hand-kept in
// sync rather than generated (the plan's stated goal) because that package is
// itself a copy — see server/internal/proto's doc comment for why it exists
// independently of tui-controller/internal/proto. If server's proto.go
// changes, this file is the one place on the client side that needs to catch
// up; it is a straight line-by-line mirror, so the diff is always obvious.

export type FieldType = 'number' | 'color' | 'enum' | 'bool'

export interface Field {
  key: string
  type: FieldType | string // unrecognized types must render read-only, not be rejected
  label: string

  min?: number
  max?: number
  step?: number
  unit?: string

  options?: string[]

  default?: unknown
}

export interface Control {
  id: string
  label: string
  fields: Field[]
}

export interface Descriptor {
  controls: Control[]
}

export type Values = Record<string, unknown>

export interface Hello {
  proto: number
  device: string
  fw: string
}

export interface State {
  id: string
  v: Values
}

export interface ErrMsg {
  seq?: number
  msg: string
}

export type LinkStatus = 'connecting' | 'describing' | 'ready' | 'lost'

export interface LinkSnapshot {
  status: LinkStatus
  error?: string
  hello?: Hello
}

export interface ClippingStatus {
  enabled: boolean
  recording: boolean
}

export interface Clip {
  name: string
  size: number
  modTime: string
  thumbnail: boolean
}

// The WebSocket envelope horus-server sends — one shape, discriminated by
// `type`, matching server/internal/api/hub.go's wsEvent exactly.
export interface WSEvent {
  type: 'link' | 'hello' | 'descriptor' | 'state' | 'error' | 'clipping'
  status?: string
  hello?: Hello
  descriptor?: Descriptor
  state?: State
  error?: ErrMsg
  clipping?: ClippingStatus
}
