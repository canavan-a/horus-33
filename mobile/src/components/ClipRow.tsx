import React from 'react'
import { Alert, Image, Pressable, StyleSheet, Text, View } from 'react-native'
import Video from 'react-native-video'
import { clipThumbnailUrl, clipUrl, deleteClip } from '../api/client'
import { accessHeaders, getConfig } from '../lib/config'
import type { Clip } from '../lib/proto'

interface Props {
  clip: Clip
  viewed: boolean
  expanded: boolean
  onToggle: () => void
  onDeleted: () => void
}

export function ClipRow({ clip, viewed, expanded, onToggle, onDeleted }: Props) {
  const headers = accessHeaders(getConfig())

  // Dim only while collapsed — an expanded clip is being watched right now.
  const dim = viewed && !expanded

  const confirmDelete = () => {
    Alert.alert('Delete clip', clip.name, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Delete',
        style: 'destructive',
        onPress: () => {
          deleteClip(clip.name)
            .then(onDeleted)
            .catch((e) => Alert.alert('Delete failed', String(e)))
        },
      },
    ])
  }

  return (
    <View style={styles.row}>
      <Pressable style={styles.header} onPress={onToggle} onLongPress={confirmDelete}>
        {clip.thumbnail ? (
          <Image
            source={{ uri: clipThumbnailUrl(clip.name), headers }}
            style={[styles.thumb, dim && styles.dim]}
          />
        ) : (
          <View style={[styles.thumb, styles.thumbEmpty]} />
        )}
        <View style={styles.meta}>
          <View style={styles.nameRow}>
            <Text style={[styles.name, dim && styles.dimText]} numberOfLines={1}>
              {clip.name}
            </Text>
            {dim && <Text style={styles.badge}>viewed</Text>}
          </View>
          <Text style={styles.sub}>
            {new Date(clip.modTime).toLocaleString()} · {(clip.size / 1_000_000).toFixed(1)} MB
          </Text>
        </View>
      </Pressable>

      {expanded && (
        <Video
          source={{ uri: clipUrl(clip.name), headers }}
          style={styles.video}
          controls
          paused={false}
          resizeMode="contain"
        />
      )}
    </View>
  )
}

const styles = StyleSheet.create({
  row: { borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: '#2a2a2a' },
  header: { flexDirection: 'row', alignItems: 'center', padding: 12, gap: 12 },
  thumb: { width: 64, height: 40, borderRadius: 4, backgroundColor: '#1a1a1a' },
  thumbEmpty: { borderWidth: 1, borderColor: '#2a2a2a' },
  dim: { opacity: 0.4 },
  meta: { flex: 1 },
  nameRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  name: { color: '#e5e5e5', fontSize: 13, flexShrink: 1 },
  dimText: { color: '#8a8a8a' },
  badge: {
    color: '#8a8a8a',
    fontSize: 10,
    borderWidth: 1,
    borderColor: '#3a3a3a',
    borderRadius: 4,
    paddingHorizontal: 4,
    paddingVertical: 1,
    overflow: 'hidden',
  },
  sub: { color: '#8a8a8a', fontSize: 11, marginTop: 2 },
  video: { width: '100%', height: 220, backgroundColor: '#000' },
})
