// TypeScript mirror of the horus-server wire types. Kept deliberately close to
// web/src/lib/proto.ts so the two clients stay in step.

export type FieldType = 'number' | 'color' | 'enum' | 'bool'

export interface Field {
  key: string
  type: FieldType | string
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

export interface State {
  id: string
  v: Values
}

export interface Hello {
  proto: number
  device: string
  fw: string
}

export interface ErrMsg {
  seq?: number
  msg: string
}

export interface ClippingStatus {
  enabled: boolean
  recording: boolean
}

export interface PresenceEvent {
  present: boolean
  conf?: number
}

export interface Clip {
  name: string
  size: number
  modTime: string
  thumbnail: boolean
}

// GET /api/clips is paginated server-side (server/internal/api/server.go's
// ClipsPage). The mobile list still renders everything, so client.ts asks for a
// large window and unwraps this.
export interface ClipsPage {
  clips: Clip[]
  total: number
}

export type LinkStatus = 'connecting' | 'describing' | 'ready' | 'lost'

// One WebSocket envelope, discriminated by `type` — matches
// server/internal/api/hub.go wsEvent.
export interface WSEvent {
  type: 'link' | 'hello' | 'descriptor' | 'state' | 'error' | 'clipping' | 'presence'
  status?: string
  hello?: Hello
  descriptor?: Descriptor
  state?: State
  error?: ErrMsg
  clipping?: ClippingStatus
  presence?: PresenceEvent
}
