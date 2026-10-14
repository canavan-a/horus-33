import { useFocusEffect } from '@react-navigation/native'
import React, { useCallback, useRef, useState } from 'react'
import { Alert, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native'
import { consoleRestart } from '../api/client'
import { type Theme, useThemedStyles } from '../theme'
import { ConsoleSocket } from '../ws/ConsoleSocket'

const MAX_LINES = 2000

export function ConsoleScreen() {
  const styles = useThemedStyles(makeStyles)
  const [lines, setLines] = useState<string[]>([])
  const [connected, setConnected] = useState(false)
  const [restarting, setRestarting] = useState(false)
  const socket = useRef<ConsoleSocket>()
  const scrollRef = useRef<ScrollView>(null)

  useFocusEffect(
    useCallback(() => {
      setLines([])
      socket.current = new ConsoleSocket(
        (line) =>
          setLines((prev) => {
            const next = [...prev, line]
            return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next
          }),
        setConnected,
      )
      socket.current.start()
      return () => {
        socket.current?.close()
        socket.current = undefined
      }
    }, []),
  )

  const handleRestart = () => {
    Alert.alert('Restart capture-eye?', 'This stops video and detection until it comes back up.', [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Restart',
        style: 'destructive',
        onPress: async () => {
          setRestarting(true)
          try {
            await consoleRestart()
          } catch (err) {
            Alert.alert('Restart failed', err instanceof Error ? err.message : String(err))
          } finally {
            setRestarting(false)
          }
        },
      },
    ])
  }

  return (
    <View style={styles.container}>
      <View style={styles.toolbar}>
        <View style={styles.status}>
          <View style={[styles.dot, connected ? styles.dotOn : styles.dotOff]} />
          <Text style={styles.statusText}>{connected ? 'streaming' : 'disconnected'}</Text>
        </View>
        <TouchableOpacity style={styles.restart} onPress={handleRestart} disabled={restarting}>
          <Text style={styles.restartText}>{restarting ? 'restarting…' : 'Restart capture-eye'}</Text>
        </TouchableOpacity>
      </View>
      <ScrollView
        ref={scrollRef}
        style={styles.log}
        contentContainerStyle={styles.logContent}
        onContentSizeChange={() => scrollRef.current?.scrollToEnd({ animated: false })}
      >
        {lines.length === 0 ? (
          <Text style={styles.placeholder}>waiting for log output…</Text>
        ) : (
          lines.map((line, i) => (
            <Text key={i} style={styles.line}>
              {line}
            </Text>
          ))
        )}
      </ScrollView>
    </View>
  )
}

function makeStyles(theme: Theme) {
  return StyleSheet.create({
    container: { flex: 1, backgroundColor: theme.codeBg },
    toolbar: {
      flexDirection: 'row',
      alignItems: 'center',
      justifyContent: 'space-between',
      paddingHorizontal: 12,
      paddingVertical: 10,
      borderBottomWidth: StyleSheet.hairlineWidth,
      borderBottomColor: theme.border,
      backgroundColor: theme.surface,
    },
    status: { flexDirection: 'row', alignItems: 'center', gap: 6 },
    dot: { width: 8, height: 8, borderRadius: 4 },
    dotOn: { backgroundColor: theme.success },
    dotOff: { backgroundColor: theme.border },
    statusText: { color: theme.textMuted, fontSize: 12 },
    restart: {
      borderWidth: 1,
      borderColor: theme.danger,
      borderRadius: 8,
      paddingVertical: 6,
      paddingHorizontal: 12,
    },
    restartText: { color: theme.danger, fontSize: 12, fontWeight: '600' },
    log: { flex: 1 },
    logContent: { padding: 10 },
    placeholder: { color: theme.textMuted, fontSize: 12 },
    line: { color: theme.codeText, fontSize: 11, fontFamily: 'monospace' },
  })
}
