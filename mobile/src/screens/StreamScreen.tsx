import { useFocusEffect } from '@react-navigation/native'
import React, { useCallback, useRef, useState } from 'react'
import {
  ActivityIndicator,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native'
import { RTCView, type MediaStream } from 'react-native-webrtc'
import MaterialCommunityIcons from 'react-native-vector-icons/MaterialCommunityIcons'
import { JogPad } from '../components/JogPad'
import { connectWhep, type WhepSession } from '../webrtc/whep'
import { useHorus } from '../ws/useHorus'

export function StreamScreen() {
  const [status, setStatus] = useState<'connecting' | 'live' | 'failed'>('connecting')
  const [stream, setStream] = useState<MediaStream | undefined>()
  const [retry, setRetry] = useState(0)
  const [controlsOpen, setControlsOpen] = useState(false)
  const session = useRef<WhepSession | undefined>()

  const snap = useHorus()
  const hasAxes = Boolean(snap.state.axis_x && snap.state.axis_y)
  const hasMotion = Boolean(snap.state.motion)

  useFocusEffect(
    useCallback(() => {
      let cancelled = false
      setStatus('connecting')
      setStream(undefined)

      connectWhep((s) => {
        if (!cancelled) setStatus(s)
      })
        .then((sess) => {
          if (cancelled) {
            sess.close()
            return
          }
          session.current = sess
          setStream(sess.stream)
        })
        .catch(() => {
          if (!cancelled) setStatus('failed')
        })

      return () => {
        cancelled = true
        session.current?.close()
        session.current = undefined
        setStream(undefined)
      }
    }, [retry]),
  )

  return (
    <View style={styles.container}>
      <View style={styles.stage}>
        {stream ? (
          <RTCView streamURL={stream.toURL()} style={styles.video} objectFit="contain" />
        ) : (
          <View style={styles.placeholder}>
            {status === 'connecting' ? (
              <>
                <ActivityIndicator color="#8a8a8a" />
                <Text style={styles.text}>Connecting…</Text>
              </>
            ) : (
              <Text style={styles.text}>No video</Text>
            )}
          </View>
        )}

        {status === 'failed' && (
          <View style={styles.failBox}>
            <Text style={styles.hint}>
              video needs the phone on the LAN or a reachable TURN server — control and
              notifications still work
            </Text>
            <TouchableOpacity style={styles.retry} onPress={() => setRetry((n) => n + 1)}>
              <Text style={styles.retryText}>Retry</Text>
            </TouchableOpacity>
          </View>
        )}
      </View>

      <View style={styles.panel}>
        <Pressable style={styles.panelHeader} onPress={() => setControlsOpen((v) => !v)}>
          <Text style={styles.panelTitle}>Controls</Text>
          <MaterialCommunityIcons
            name={controlsOpen ? 'chevron-down' : 'chevron-up'}
            color="#e5e5e5"
            size={22}
          />
        </Pressable>
        {controlsOpen && (
          <ScrollView style={styles.panelBody} contentContainerStyle={styles.panelBodyContent}>
            <JogPad
              motionValues={snap.state.motion ?? {}}
              axisXValues={snap.state.axis_x ?? {}}
              axisYValues={snap.state.axis_y ?? {}}
              hasMotion={hasMotion}
              hasAxes={hasAxes}
            />
          </ScrollView>
        )}
      </View>
    </View>
  )
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#000' },
  stage: { flex: 1 },
  video: { flex: 1 },
  placeholder: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8 },
  text: { color: '#8a8a8a' },
  failBox: { position: 'absolute', left: 0, right: 0, bottom: 0, alignItems: 'center' },
  hint: { color: '#8a8a8a', fontSize: 12, padding: 12, textAlign: 'center' },
  retry: {
    borderWidth: 1,
    borderColor: '#3a3a3a',
    borderRadius: 8,
    paddingVertical: 8,
    paddingHorizontal: 20,
    marginBottom: 12,
  },
  retryText: { color: '#e5e5e5' },
  panel: { backgroundColor: '#111', borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: '#2a2a2a' },
  panelHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: 16,
    paddingVertical: 12,
  },
  panelTitle: { color: '#e5e5e5', fontSize: 13, textTransform: 'uppercase' },
  panelBody: { maxHeight: 360 },
  panelBodyContent: { paddingHorizontal: 12, paddingBottom: 16 },
})
