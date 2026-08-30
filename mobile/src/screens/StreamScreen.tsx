import { useFocusEffect } from '@react-navigation/native'
import React, { useCallback, useRef, useState } from 'react'
import { StyleSheet, Text, View } from 'react-native'
import { RTCView, type MediaStream } from 'react-native-webrtc'
import { connectWhep, type WhepSession } from '../webrtc/whep'

export function StreamScreen() {
  const [status, setStatus] = useState<'connecting' | 'live' | 'failed'>('connecting')
  const [stream, setStream] = useState<MediaStream | undefined>()
  const session = useRef<WhepSession | undefined>()

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
    }, []),
  )

  return (
    <View style={styles.container}>
      {stream ? (
        <RTCView streamURL={stream.toURL()} style={styles.video} objectFit="contain" />
      ) : (
        <View style={styles.placeholder}>
          <Text style={styles.text}>
            {status === 'connecting' ? 'connecting…' : 'no video'}
          </Text>
        </View>
      )}
      {status === 'failed' && (
        <Text style={styles.hint}>
          video needs the phone on the LAN or a reachable TURN server — control and
          notifications still work
        </Text>
      )}
    </View>
  )
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: '#000' },
  video: { flex: 1 },
  placeholder: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  text: { color: '#8a8a8a' },
  hint: { color: '#8a8a8a', fontSize: 12, padding: 12, textAlign: 'center' },
})
