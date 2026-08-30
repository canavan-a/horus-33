import { createBottomTabNavigator } from '@react-navigation/bottom-tabs'
import { DarkTheme, NavigationContainer } from '@react-navigation/native'
import { createNativeStackNavigator } from '@react-navigation/native-stack'
import React, { useEffect, useState, useSyncExternalStore } from 'react'
import { ActivityIndicator, View } from 'react-native'
import MaterialCommunityIcons from 'react-native-vector-icons/MaterialCommunityIcons'
import { getConfig, isConfigured, loadConfig, subscribeConfig } from '../lib/config'
import { ClipsScreen } from '../screens/ClipsScreen'
import { SettingsScreen } from '../screens/SettingsScreen'
import { StreamScreen } from '../screens/StreamScreen'

const Tab = createBottomTabNavigator()
const Stack = createNativeStackNavigator()

const horusDark = {
  ...DarkTheme,
  colors: {
    ...DarkTheme.colors,
    background: '#111',
    card: '#111',
    text: '#e5e5e5',
    border: '#2a2a2a',
    primary: '#2563eb',
  },
}

const screenOpts = {
  headerStyle: { backgroundColor: '#111' },
  headerTintColor: '#e5e5e5',
  tabBarStyle: { backgroundColor: '#111', borderTopColor: '#2a2a2a' },
  tabBarActiveTintColor: '#2563eb',
  tabBarInactiveTintColor: '#8a8a8a',
  tabBarShowLabel: false,
} as const

const TAB_ICONS: Record<string, string> = {
  Stream: 'cctv',
  Clips: 'filmstrip-box-multiple',
  Settings: 'cog',
}

function Tabs() {
  return (
    <Tab.Navigator
      screenOptions={({ route }) => ({
        ...screenOpts,
        tabBarIcon: ({ color, size }) => (
          <MaterialCommunityIcons
            name={TAB_ICONS[route.name] ?? 'circle'}
            color={color}
            size={size}
          />
        ),
      })}
    >
      <Tab.Screen name="Stream" component={StreamScreen} />
      <Tab.Screen name="Clips" component={ClipsScreen} />
      <Tab.Screen name="Settings" component={SettingsScreen} />
    </Tab.Navigator>
  )
}

export function RootNavigator() {
  const [ready, setReady] = useState(false)
  const configured = useSyncExternalStore(subscribeConfig, () =>
    isConfigured(getConfig()),
  )

  useEffect(() => {
    loadConfig().then(() => setReady(true))
  }, [])

  if (!ready) {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: '#111' }}>
        <ActivityIndicator />
      </View>
    )
  }

  return (
    <NavigationContainer theme={horusDark}>
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
