import React, { useState } from 'react'
import { Alert, Image, Pressable, StyleSheet, Text, View } from 'react-native'
import Video from 'react-native-video'
import { clipThumbnailUrl, clipUrl, deleteClip } from '../api/client'
import { accessHeaders, getConfig } from '../lib/config'
import type { Clip } from '../lib/proto'

export function ClipRow({ clip, onDeleted }: { clip: Clip; onDeleted: () => void }) {
  const [expanded, setExpanded] = useState(false)
  const headers = accessHeaders(getConfig())

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
      <Pressable
        style={styles.header}
        onPress={() => setExpanded((v) => !v)}
        onLongPress={confirmDelete}
      >
        {clip.thumbnail ? (
          <Image
            source={{ uri: clipThumbnailUrl(clip.name), headers }}
            style={styles.thumb}
          />
        ) : (
          <View style={[styles.thumb, styles.thumbEmpty]} />
        )}
        <View style={styles.meta}>
          <Text style={styles.name}>{clip.name}</Text>
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
  meta: { flex: 1 },
  name: { color: '#e5e5e5', fontSize: 13 },
  sub: { color: '#8a8a8a', fontSize: 11, marginTop: 2 },
  video: { width: '100%', height: 220, backgroundColor: '#000' },
})
