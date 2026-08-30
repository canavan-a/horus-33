import React from 'react'
import { ScrollView, StyleSheet, Text, View } from 'react-native'
import { JogPad } from '../components/JogPad'
import { useHorus } from '../ws/useHorus'

export function JogScreen() {
  const snap = useHorus()
  const hasAxes = Boolean(snap.state.axis_x && snap.state.axis_y)
  const hasMotion = Boolean(snap.state.motion)

  return (
    <ScrollView contentContainerStyle={styles.container}>
      <View style={styles.banner}>
        <Text style={styles.bannerText}>link: {snap.linkStatus}</Text>
        {snap.linkError ? <Text style={styles.err}>{snap.linkError}</Text> : null}
      </View>
      <JogPad
        motionValues={snap.state.motion ?? {}}
        axisXValues={snap.state.axis_x ?? {}}
        axisYValues={snap.state.axis_y ?? {}}
        hasMotion={hasMotion}
        hasAxes={hasAxes}
      />
    </ScrollView>
  )
}

const styles = StyleSheet.create({
  container: { paddingVertical: 12 },
  banner: { paddingHorizontal: 16, paddingBottom: 8 },
  bannerText: { color: '#8a8a8a', fontSize: 12 },
  err: { color: '#dc2626', fontSize: 12, marginTop: 2 },
})
