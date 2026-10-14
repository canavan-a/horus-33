import React, { useCallback, useEffect, useRef, useState } from 'react'
import {
  ActivityIndicator,
  FlatList,
  RefreshControl,
  StyleSheet,
  Switch,
  Text,
  View,
} from 'react-native'
import { getClippingStatus, listClips, listClipsPage, setClippingEnabled } from '../api/client'
import { ClipRow } from '../components/ClipRow'
import type { Clip } from '../lib/proto'
import { useViewedClips } from '../lib/useViewedClips'
import { type Theme, useTheme, useThemedStyles } from '../theme'
import { useHorus } from '../ws/useHorus'

// Clips fetched per scroll page. Small so the first screenful lands fast.
const PAGE = 20

export function ClipsScreen() {
  const theme = useTheme()
  const styles = useThemedStyles(makeStyles)
  const snap = useHorus()
  const { isViewed, markViewed, viewedCount } = useViewedClips()
  const [clips, setClips] = useState<Clip[]>([])
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [loadingMore, setLoadingMore] = useState(false)
  const [enabled, setEnabled] = useState<boolean | undefined>(snap.clipping?.enabled)
  // Single-open accordion: only one clip is expanded/playing at a time.
  const [openName, setOpenName] = useState<string>()

  // Latest list for the stable callbacks below. `gen` bumps on every refresh
  // so a page that was in flight across one is dropped instead of appended to
  // a list it no longer lines up with.
  const clipsRef = useRef(clips)
  clipsRef.current = clips
  const gen = useRef(0)
  const fetchingMore = useRef(false)

  // Re-fetch everything already scrolled in (at least one page), so a refresh
  // never shrinks the list out from under the user.
  const refresh = useCallback(() => {
    const g = ++gen.current
    setLoading(true)
    listClips(Math.max(PAGE, clipsRef.current.length))
      .then(({ clips: cs, total: t }) => {
        if (g !== gen.current) return
        setClips(cs)
        setTotal(t)
        setOpenName((cur) => (cur && cs.some((c) => c.name === cur) ? cur : undefined))
      })
      .catch(() => {})
      .finally(() => {
        if (g === gen.current) setLoading(false)
      })
  }, [])

  const loadMore = useCallback(() => {
    const have = clipsRef.current.length
    if (fetchingMore.current || have === 0 || have >= total) return
    fetchingMore.current = true
    const g = gen.current
    setLoadingMore(true)
    listClipsPage(have, PAGE)
      .then(({ clips: page, total: t }) => {
        if (g !== gen.current) return
        // Offsets shift when a new clip lands mid-scroll; skip the overlap.
        setClips((cur) => {
          const seen = new Set(cur.map((c) => c.name))
          return cur.concat(page.filter((c) => !seen.has(c.name)))
        })
        setTotal(t)
      })
      .catch(() => {})
      .finally(() => {
        fetchingMore.current = false
        setLoadingMore(false)
      })
  }, [total])

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
            onToggle={handleToggle}
            onDeleted={refresh}
          />
        )}
        // Keep the mounted window tight: rows carry an <Image> (and possibly a
        // <Video>), so only a couple of screens' worth live at once and new
        // ones mount in small batches as the user scrolls.
        initialNumToRender={8}
        maxToRenderPerBatch={4}
        updateCellsBatchingPeriod={50}
        windowSize={5}
        onEndReached={loadMore}
        onEndReachedThreshold={0.5}
        refreshControl={<RefreshControl
            refreshing={loading}
            onRefresh={refresh}
            colors={[theme.primary]}
            progressBackgroundColor={theme.surface}
          />}
        ListEmptyComponent={
          loading ? null : <Text style={styles.empty}>no clips yet</Text>
        }
        ListFooterComponent={
          loadingMore ? <ActivityIndicator style={styles.more} color={theme.textMuted} /> : null
        }
      />
    </View>
  )
}

function makeStyles(theme: Theme) {
  return StyleSheet.create({
    container: { flex: 1 },
    header: {
      flexDirection: 'row',
      alignItems: 'center',
      justifyContent: 'space-between',
      padding: 16,
      borderBottomWidth: StyleSheet.hairlineWidth,
      borderBottomColor: theme.border,
    },
    title: { color: theme.text, fontSize: 13, maxWidth: 260 },
    rec: { color: theme.danger, fontSize: 12, marginTop: 4 },
    empty: { color: theme.textMuted, textAlign: 'center', marginTop: 40 },
    more: { paddingVertical: 16 },
  })
}
