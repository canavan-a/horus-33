import React from 'react'
import {
  ActivityIndicator,
  Alert,
  Image,
  PanResponder,
  Pressable,
  StyleSheet,
  Text,
  Vibration,
  View,
} from 'react-native'
import Video, { type VideoRef } from 'react-native-video'
import MaterialCommunityIcons from 'react-native-vector-icons/MaterialCommunityIcons'
import { clipThumbnailUrl, clipUrl, deleteClip } from '../api/client'
import { accessHeaders, getConfig } from '../lib/config'
import type { Clip } from '../lib/proto'

interface Props {
  clip: Clip
  viewed: boolean
  expanded: boolean
  onToggle: () => void
  onDeleted: () => void
  // When true (and not expanded), mount a hidden paused player so ExoPlayer
  // buffers the clip's opening segment ahead of the next tap.
  preload?: boolean
}

const clamp = (n: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, n))

export function ClipRow({ clip, viewed, expanded, onToggle, onDeleted, preload }: Props) {
  const headers = accessHeaders(getConfig())

  const videoRef = React.useRef<VideoRef>(null)

  // Autoplays on expand; the bottom-left button toggles this.
  const [paused, setPaused] = React.useState(false)
  const [progress, setProgress] = React.useState(0)
  const [duration, setDuration] = React.useState(0)
  const [ended, setEnded] = React.useState(false)
  // First frame shown — until then the thumbnail poster + spinner cover the black box.
  const [ready, setReady] = React.useState(false)

  // Controls are hidden until the video is tapped.
  const [controlsVisible, setControlsVisible] = React.useState(false)

  // Hold the right half of the video to play at 2x until release.
  const [boosting, setBoosting] = React.useState(false)
  const wrapWidth = React.useRef(0)
  const pressX = React.useRef(0)

  const endBoost = () => {
    if (boosting) setBoosting(false)
  }

  const [trackWidth, setTrackWidth] = React.useState(0)
  const seeking = React.useRef(false)
  // Latest values for the PanResponder closure.
  const durationRef = React.useRef(0)
  const trackWidthRef = React.useRef(0)
  durationRef.current = duration
  trackWidthRef.current = trackWidth

  const seekToFraction = (frac: number) => {
    const d = durationRef.current
    if (d <= 0) return
    const t = clamp(frac, 0, 1) * d
    setProgress(t)
    videoRef.current?.seek(t)
    setEnded(false)
  }

  const pan = React.useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponder: () => true,
      onPanResponderGrant: (e) => {
        seeking.current = true
        const w = trackWidthRef.current
        if (w > 0) {
          const frac = clamp(e.nativeEvent.locationX / w, 0, 1)
          setProgress(frac * durationRef.current)
        }
      },
      onPanResponderMove: (e) => {
        const w = trackWidthRef.current
        if (w > 0) {
          const frac = clamp(e.nativeEvent.locationX / w, 0, 1)
          setProgress(frac * durationRef.current)
        }
      },
      onPanResponderRelease: (e) => {
        const w = trackWidthRef.current
        if (w > 0) {
          seekToFraction(e.nativeEvent.locationX / w)
        }
        seeking.current = false
      },
      onPanResponderTerminate: () => {
        seeking.current = false
      },
    }),
  ).current

  const onPlayPause = () => {
    if (ended) {
      videoRef.current?.seek(0)
      setProgress(0)
      setEnded(false)
      setPaused(false)
      return
    }
    setPaused((p) => !p)
  }

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

      {preload && !expanded && (
        <Video
          source={{ uri: clipUrl(clip.name), headers }}
          style={styles.warm}
          paused
          muted
          bufferConfig={{ minBufferMs: 2000, bufferForPlaybackMs: 1000 }}
        />
      )}

      {expanded && (
        <View
          style={styles.videoWrap}
          onLayout={(e) => {
            wrapWidth.current = e.nativeEvent.layout.width
          }}
        >
          <Video
            ref={videoRef}
            source={{ uri: clipUrl(clip.name), headers }}
            style={styles.video}
            paused={paused && !boosting}
            rate={boosting ? 2 : 1}
            resizeMode="contain"
            onLoadStart={() => setReady(false)}
            onReadyForDisplay={() => setReady(true)}
            onLoad={({ duration: d }) => setDuration(d)}
            onProgress={({ currentTime }) => {
              if (!seeking.current) setProgress(currentTime)
            }}
            onEnd={() => {
              setPaused(true)
              setEnded(true)
            }}
          />
          {!ready && (
            <View style={styles.loading}>
              {clip.thumbnail && (
                <Image
                  source={{ uri: clipThumbnailUrl(clip.name), headers }}
                  style={StyleSheet.absoluteFillObject}
                  resizeMode="contain"
                />
              )}
              <ActivityIndicator color="#e5e5e5" />
            </View>
          )}
          <Pressable
            style={styles.tapLayer}
            delayLongPress={250}
            onPressIn={(e) => {
              pressX.current = e.nativeEvent.locationX
            }}
            onPress={() => setControlsVisible((v) => !v)}
            onLongPress={() => {
              if (wrapWidth.current > 0 && pressX.current > wrapWidth.current / 2) {
                Vibration.vibrate(15)
                setBoosting(true)
              }
            }}
            onPressOut={endBoost}
          />
          {boosting && (
            <View style={styles.boostBadge} pointerEvents="none">
              <Text style={styles.boostText}>2x ▶▶</Text>
            </View>
          )}
          {controlsVisible && (
            <View style={styles.bar}>
              <View
                style={styles.trackHit}
                onLayout={(e) => setTrackWidth(e.nativeEvent.layout.width)}
                {...pan.panHandlers}
              >
                <View style={styles.progressTrack}>
                  <View
                    style={[
                      styles.progressFill,
                      {
                        width:
                          duration > 0
                            ? `${Math.min(100, (progress / duration) * 100)}%`
                            : '0%',
                      },
                    ]}
                  />
                </View>
              </View>
              <Pressable style={styles.playBtn} hitSlop={12} onPress={onPlayPause}>
                <MaterialCommunityIcons
                  name={paused ? 'play' : 'pause'}
                  size={22}
                  color="#e5e5e5"
                />
              </Pressable>
            </View>
          )}
        </View>
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
  videoWrap: { width: '100%', height: 220, backgroundColor: '#000' },
  video: { ...StyleSheet.absoluteFillObject },
  warm: { width: 0, height: 0 },
  loading: {
    ...StyleSheet.absoluteFillObject,
    alignItems: 'center',
    justifyContent: 'center',
  },
  tapLayer: { ...StyleSheet.absoluteFillObject },
  boostBadge: {
    position: 'absolute',
    top: 10,
    alignSelf: 'center',
    backgroundColor: 'rgba(0,0,0,0.55)',
    borderRadius: 4,
    paddingHorizontal: 8,
    paddingVertical: 3,
  },
  boostText: { color: '#e5e5e5', fontSize: 12, fontWeight: '600' },
  bar: {
    position: 'absolute',
    left: 0,
    right: 0,
    bottom: 0,
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 8,
    paddingVertical: 6,
    backgroundColor: 'rgba(0,0,0,0.45)',
  },
  trackHit: {
    position: 'absolute',
    left: 0,
    right: 40,
    top: -10,
    height: 22,
    justifyContent: 'flex-start',
  },
  progressTrack: {
    height: 2,
    backgroundColor: 'rgba(255,255,255,0.2)',
  },
  progressFill: { height: 2, backgroundColor: '#e5e5e5' },
  playBtn: { paddingRight: 8 },
})
