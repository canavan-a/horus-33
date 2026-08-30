import React, { useCallback, useEffect, useState } from 'react'
import { FlatList, RefreshControl, StyleSheet, Switch, Text, View } from 'react-native'
import { getClippingStatus, listClips, setClippingEnabled } from '../api/client'
import { ClipRow } from '../components/ClipRow'
import type { Clip } from '../lib/proto'
import { useHorus } from '../ws/useHorus'

export function ClipsScreen() {
  const snap = useHorus()
  const [clips, setClips] = useState<Clip[]>([])
  const [loading, setLoading] = useState(false)
  const [enabled, setEnabled] = useState<boolean | undefined>(snap.clipping?.enabled)

  const refresh = useCallback(() => {
    setLoading(true)
    listClips()
      .then(setClips)
      .catch(() => {})
      .finally(() => setLoading(false))
  }, [])

  useEffect(() => {
    refresh()
    getClippingStatus()
      .then((s) => setEnabled(s.enabled))
      .catch(() => {})
  }, [refresh])

  // A finished recording likely dropped a new clip.
  const recording = snap.clipping?.recording
  useEffect(() => {
    refresh()
  }, [recording, refresh])

  // Track server truth when the optimistic value isn't mid-flight.
  useEffect(() => {
    if (snap.clipping) setEnabled(snap.clipping.enabled)
  }, [snap.clipping])

  const toggle = (next: boolean) => {
    setEnabled(next) // optimistic
    setClippingEnabled(next)
      .then((s) => setEnabled(s.enabled))
      .catch(() => setEnabled(!next))
  }

  return (
    <View style={styles.container}>
      <View style={styles.header}>
        <View>
          <Text style={styles.title}>Record clips when someone is in frame</Text>
          {recording ? <Text style={styles.rec}>● recording</Text> : null}
        </View>
        <Switch value={enabled ?? false} onValueChange={toggle} />
      </View>
      <FlatList
        data={clips}
        keyExtractor={(c) => c.name}
        renderItem={({ item }) => <ClipRow clip={item} onDeleted={refresh} />}
        refreshControl={<RefreshControl refreshing={loading} onRefresh={refresh} />}
        ListEmptyComponent={<Text style={styles.empty}>no clips yet</Text>}
      />
    </View>
  )
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  header: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    padding: 16,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: '#2a2a2a',
  },
  title: { color: '#e5e5e5', fontSize: 13, maxWidth: 260 },
  rec: { color: '#dc2626', fontSize: 12, marginTop: 4 },
  empty: { color: '#8a8a8a', textAlign: 'center', marginTop: 40 },
})
