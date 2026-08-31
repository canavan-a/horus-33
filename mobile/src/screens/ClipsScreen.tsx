import React, { useCallback, useEffect, useState } from 'react'
import { FlatList, RefreshControl, StyleSheet, Switch, Text, View } from 'react-native'
import { getClippingStatus, listClips, setClippingEnabled } from '../api/client'
import { ClipRow } from '../components/ClipRow'
import type { Clip } from '../lib/proto'
import { useViewedClips } from '../lib/useViewedClips'
import { useHorus } from '../ws/useHorus'

export function ClipsScreen() {
  const snap = useHorus()
  const { isViewed, markViewed, viewedCount } = useViewedClips()
  const [clips, setClips] = useState<Clip[]>([])
  const [loading, setLoading] = useState(false)
  const [enabled, setEnabled] = useState<boolean | undefined>(snap.clipping?.enabled)
  // Single-open accordion: only one clip is expanded/playing at a time.
  const [openName, setOpenName] = useState<string>()

  const refresh = useCallback(() => {
    setLoading(true)
    listClips()
      .then((cs) => {
        setClips(cs)
        setOpenName((cur) => (cur && cs.some((c) => c.name === cur) ? cur : undefined))
      })
      .catch(() => {})
      .finally(() => setLoading(false))
  }, [])

  const handleToggle = useCallback(
    (name: string) => {
      setOpenName((cur) => {
        if (cur === name) return undefined
        markViewed(name) // expanding autoplays the clip — count it as watched
        return name
      })
    },
    [markViewed],
  )

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

  // Warm the clips most likely to be tapped next: the rows directly above and
  // below the open one. With none open, just warm the first clip.
  const openIdx = clips.findIndex((c) => c.name === openName)
  const preloadNames =
    openIdx >= 0
      ? [clips[openIdx - 1]?.name, clips[openIdx + 1]?.name].filter(Boolean)
      : [clips[0]?.name].filter(Boolean)
  const preloadKey = preloadNames.join(',')

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
        extraData={`${openName ?? ''}:${preloadKey}:${viewedCount}`}
        keyExtractor={(c) => c.name}
        renderItem={({ item }) => (
          <ClipRow
            clip={item}
            viewed={isViewed(item.name)}
            expanded={item.name === openName}
            preload={preloadNames.includes(item.name)}
            onToggle={() => handleToggle(item.name)}
            onDeleted={refresh}
          />
        )}
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
