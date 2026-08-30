import React, { useEffect } from 'react'
import { AppState, StatusBar } from 'react-native'
import { GestureHandlerRootView } from 'react-native-gesture-handler'
import { SafeAreaProvider } from 'react-native-safe-area-context'
import { notifySubscribe } from './api/client'
import { getConfig, isConfigured, loadConfig } from './lib/config'
import { clearPresenceAlerts } from './notifications/presence'
import { RootNavigator } from './navigation/RootNavigator'
import { startMonitoring } from './service/PresenceService'

export default function App() {
  useEffect(() => {
    loadConfig().then((c) => {
      if (!isConfigured(c)) return
      notifySubscribe(true).catch(() => {})
      // FGS can only be started while foreground (Android 12+); do it now.
      if (c.bgAlerts) startMonitoring().catch(() => {})
    })

    const sub = AppState.addEventListener('change', (s) => {
      if (s === 'active') {
        clearPresenceAlerts().catch(() => {})
        if (isConfigured(getConfig()) && getConfig().bgAlerts) {
          startMonitoring().catch(() => {})
        }
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
