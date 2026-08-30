import React, { useCallback, useRef } from 'react'
import { Pressable, StyleSheet, Text, View } from 'react-native'
import { estop } from '../api/client'
import { patch } from '../lib/patch'
import type { Values } from '../lib/proto'

// Ported from web/src/components/JogPad.tsx — identical semantics.
const HOLD_THRESHOLD_MS = 250
const BURST_MS = 120
const PID_SPEED_PRESETS = [50, 100, 200, 400, 800]

type Direction = 'up' | 'down' | 'left' | 'right'

interface AxisMove {
  controlId: string
  dir: 'fwd' | 'rev'
}

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

interface Props {
  motionValues: Values
  axisXValues: Values
  axisYValues: Values
  hasMotion: boolean
  hasAxes: boolean
}

export function JogPad({ motionValues, axisXValues, axisYValues, hasMotion, hasAxes }: Props) {
  const active = useRef<Partial<Record<Direction, ActiveMove>>>({})
  const mode = typeof motionValues.mode === 'string' ? motionValues.mode : 'manual'
  const manual = mode === 'manual'

  const setRun = useCallback((controlId: string, dir: 'fwd' | 'rev', run: boolean) => {
    patch(controlId, run ? { run: true, dir } : { run: false })
  }, [])

  const stop = useCallback(
    (d: Direction) => {
      const a = active.current[d]
      if (!a) return
      clearTimeout(a.holdTimer)
      if (a.burstTimer) clearTimeout(a.burstTimer)
      delete active.current[d]
      setRun(MOVES[d].controlId, MOVES[d].dir, false)
    },
    [setRun],
  )

  const start = useCallback(
    (d: Direction) => {
      if (!manual || active.current[d]) return
      setRun(MOVES[d].controlId, MOVES[d].dir, true)
      const holdTimer = setTimeout(() => {
        const a = active.current[d]
        if (a) a.held = true
      }, HOLD_THRESHOLD_MS)
      active.current[d] = { holdTimer, held: false }
    },
    [manual, setRun],
  )

  const release = useCallback(
    (d: Direction) => {
      const a = active.current[d]
      if (!a) return
      if (a.held) {
        stop(d)
        return
      }
      clearTimeout(a.holdTimer)
      a.burstTimer = setTimeout(() => stop(d), BURST_MS)
    },
    [stop],
  )

  const toggleMode = () => patch('motion', { mode: manual ? 'pid' : 'manual' })
  const goHome = () => patch('motion', { mode: 'home' })
  const setHome = () => {
    const x = axisXValues.pos
    const y = axisYValues.pos
    if (typeof x === 'number') patch('axis_x', { home: x })
    if (typeof y === 'number') patch('axis_y', { home: y })
  }
  const setPidSpeed = (sps: number) => {
    patch('axis_x', { max_sps: sps })
    patch('axis_y', { max_sps: sps })
  }
  const currentPidSpeed = axisXValues.max_sps

  if (!hasAxes) {
    return (
      <View style={styles.card}>
        <Text style={styles.muted}>waiting for device…</Text>
      </View>
    )
  }

  const padButton = (d: Direction, label: string, area: object) => (
    <Pressable
      key={d}
      disabled={!manual}
      onPressIn={() => start(d)}
      onPressOut={() => release(d)}
      style={({ pressed }) => [
        styles.pad,
        area,
        pressed && styles.padPressed,
        !manual && styles.disabled,
      ]}
    >
      <Text style={styles.padLabel}>{label}</Text>
    </Pressable>
  )

  return (
    <View style={styles.card}>
      {hasMotion && (
        <Pressable onPress={toggleMode} style={[styles.modeBtn, manual && styles.modeBtnActive]}>
          <Text style={styles.modeBtnText}>{mode}</Text>
        </Pressable>
      )}

      <View style={styles.grid}>
        {padButton('up', '▲', styles.up)}
        {padButton('left', '◀', styles.left)}
        {padButton('right', '▶', styles.right)}
        {padButton('down', '▼', styles.down)}
      </View>

      <Text style={styles.muted}>
        {manual ? 'tap to nudge, hold to jog' : 'switch to manual mode to jog'}
      </Text>

      {hasMotion && (
        <View style={styles.row}>
          <Btn label="Set home" onPress={setHome} disabled={!manual} />
          <Btn label="Home" onPress={goHome} />
        </View>
      )}

      {hasMotion && (
        <View style={styles.row}>
          <Text style={styles.muted}>PID speed</Text>
          {PID_SPEED_PRESETS.map((sps) => (
            <Btn
              key={sps}
              label={String(sps)}
              onPress={() => setPidSpeed(sps)}
              active={currentPidSpeed === sps}
            />
          ))}
        </View>
      )}

      <Pressable onPress={() => estop().catch(() => {})} style={styles.estop}>
        <Text style={styles.estopText}>E-STOP</Text>
      </Pressable>
    </View>
  )
}

function Btn({
  label,
  onPress,
  disabled,
  active,
}: {
  label: string
  onPress: () => void
  disabled?: boolean
  active?: boolean
}) {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      style={[styles.btn, active && styles.btnActive, disabled && styles.disabled]}
    >
      <Text style={[styles.btnText, active && styles.btnTextActive]}>{label}</Text>
    </Pressable>
  )
}

const styles = StyleSheet.create({
  card: { padding: 16, gap: 12, alignItems: 'center' },
  muted: { color: '#8a8a8a', fontSize: 12, textAlign: 'center' },
  grid: { width: 168, height: 168 },
  pad: {
    position: 'absolute',
    width: 52,
    height: 52,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: '#3a3a3a',
    alignItems: 'center',
    justifyContent: 'center',
  },
  padPressed: { backgroundColor: '#2563eb', borderColor: '#2563eb' },
  padLabel: { color: '#e5e5e5', fontSize: 18 },
  up: { top: 0, left: 58 },
  down: { bottom: 0, left: 58 },
  left: { top: 58, left: 0 },
  right: { top: 58, right: 0 },
  disabled: { opacity: 0.4 },
  row: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, alignItems: 'center', justifyContent: 'center' },
  modeBtn: {
    paddingHorizontal: 14,
    paddingVertical: 6,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#3a3a3a',
  },
  modeBtnActive: { backgroundColor: '#2563eb', borderColor: '#2563eb' },
  modeBtnText: { color: '#e5e5e5', textTransform: 'capitalize' },
  btn: {
    paddingHorizontal: 12,
    paddingVertical: 6,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#3a3a3a',
  },
  btnActive: { backgroundColor: '#2563eb', borderColor: '#2563eb' },
  btnText: { color: '#e5e5e5', fontSize: 13 },
  btnTextActive: { color: '#fff' },
  estop: {
    marginTop: 8,
    paddingHorizontal: 32,
    paddingVertical: 12,
    borderRadius: 10,
    backgroundColor: '#dc2626',
  },
  estopText: { color: '#fff', fontWeight: '700', letterSpacing: 1 },
})
