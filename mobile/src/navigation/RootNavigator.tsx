import { createBottomTabNavigator } from '@react-navigation/bottom-tabs'
import { NavigationContainer } from '@react-navigation/native'
import { createNativeStackNavigator } from '@react-navigation/native-stack'
import React, { useEffect, useState } from 'react'
import { ActivityIndicator, View } from 'react-native'
import { isConfigured, loadConfig } from '../lib/config'
import { ClipsScreen } from '../screens/ClipsScreen'
import { JogScreen } from '../screens/JogScreen'
import { SettingsScreen } from '../screens/SettingsScreen'
import { StreamScreen } from '../screens/StreamScreen'

const Tab = createBottomTabNavigator()
const Stack = createNativeStackNavigator()

const screenOpts = {
  headerStyle: { backgroundColor: '#111' },
  headerTintColor: '#e5e5e5',
  tabBarStyle: { backgroundColor: '#111', borderTopColor: '#2a2a2a' },
  tabBarActiveTintColor: '#2563eb',
  tabBarInactiveTintColor: '#8a8a8a',
} as const

function Tabs() {
  return (
    <Tab.Navigator screenOptions={screenOpts}>
      <Tab.Screen name="Stream" component={StreamScreen} />
      <Tab.Screen name="Jog" component={JogScreen} />
      <Tab.Screen name="Clips" component={ClipsScreen} />
      <Tab.Screen name="Settings" component={SettingsScreen} />
    </Tab.Navigator>
  )
}

export function RootNavigator() {
  const [ready, setReady] = useState(false)
  const [configured, setConfigured] = useState(false)

  useEffect(() => {
    loadConfig().then((c) => {
      setConfigured(isConfigured(c))
      setReady(true)
    })
  }, [])

  if (!ready) {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: '#111' }}>
        <ActivityIndicator />
      </View>
    )
  }

  return (
    <NavigationContainer>
      <Stack.Navigator screenOptions={screenOpts}>
        {configured ? (
          <Stack.Screen name="Main" component={Tabs} options={{ headerShown: false }} />
        ) : (
          <Stack.Screen name="Setup" component={SettingsScreen} options={{ title: 'Connect to Horus' }} />
        )}
      </Stack.Navigator>
    </NavigationContainer>
  )
}
