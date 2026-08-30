import React, { useEffect } from 'react'
import { Alert, AppState, StatusBar } from 'react-native'
import { GestureHandlerRootView } from 'react-native-gesture-handler'
import { SafeAreaProvider } from 'react-native-safe-area-context'
import { notifySubscribe } from './api/client'
import {
  compareVersions,
  fetchReleases,
  getVersionInfo,
} from './lib/appUpdater'
import { getConfig, isConfigured, loadConfig } from './lib/config'
import { clearPresenceAlerts } from './notifications/presence'
import { RootNavigator } from './navigation/RootNavigator'
import { startMonitoring } from './service/PresenceService'

// Throttle the foreground update check to once every 6h per app process.
let lastUpdateCheck = 0

async function maybeCheckForUpdate() {
  if (!getConfig().autoUpdateCheck) return
  if (Date.now() - lastUpdateCheck < 6 * 60 * 60 * 1000) return
  lastUpdateCheck = Date.now()
  try {
    const [releases, info] = await Promise.all([fetchReleases(), getVersionInfo()])
    const latest = releases[0]
    if (!latest) return
    if (compareVersions(latest.version, info.versionName) > 0) {
      Alert.alert(
        'Update available',
        `v${latest.version} is available (you have v${info.versionName}). Open Settings to download and install it.`,
      )
    }
  } catch {
    // Offline or rate-limited — try again next foreground.
  }
}

export default function App() {
  useEffect(() => {
    loadConfig().then((c) => {
      if (!isConfigured(c)) return
      notifySubscribe(true).catch(() => {})
      // FGS can only be started while foreground (Android 12+); do it now.
      if (c.bgAlerts) startMonitoring().catch(() => {})
    })

    maybeCheckForUpdate()

    const sub = AppState.addEventListener('change', (s) => {
      if (s === 'active') {
        clearPresenceAlerts().catch(() => {})
        if (isConfigured(getConfig()) && getConfig().bgAlerts) {
          startMonitoring().catch(() => {})
        }
        maybeCheckForUpdate()
      }
    })
    return () => sub.remove()
  }, [])

  return (
    <GestureHandlerRootView style={{ flex: 1 }}>
      <SafeAreaProvider>
        <StatusBar barStyle="light-content" backgroundColor="#111" />
        <RootNavigator />
      </SafeAreaProvider>
    </GestureHandlerRootView>
  )
}
