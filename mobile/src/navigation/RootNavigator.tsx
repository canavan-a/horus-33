import { createBottomTabNavigator } from '@react-navigation/bottom-tabs'
import { DarkTheme, DefaultTheme, NavigationContainer } from '@react-navigation/native'
import { createNativeStackNavigator } from '@react-navigation/native-stack'
import React, { useEffect, useMemo, useState, useSyncExternalStore } from 'react'
import { ActivityIndicator, View } from 'react-native'
import MaterialCommunityIcons from 'react-native-vector-icons/MaterialCommunityIcons'
import { getConfig, isConfigured, loadConfig, subscribeConfig } from '../lib/config'
import { ClipsScreen } from '../screens/ClipsScreen'
import { ConsoleScreen } from '../screens/ConsoleScreen'
import { SettingsScreen } from '../screens/SettingsScreen'
import { StreamScreen } from '../screens/StreamScreen'
import { type Theme, useTheme } from '../theme'

const Tab = createBottomTabNavigator()
const Stack = createNativeStackNavigator()

function navTheme(t: Theme) {
  const base = t.dark ? DarkTheme : DefaultTheme
  return {
    ...base,
    colors: {
      ...base.colors,
      background: t.bg,
      card: t.surface,
      text: t.text,
      border: t.border,
      primary: t.primary,
    },
  }
}

function screenOpts(t: Theme) {
  return {
    headerStyle: { backgroundColor: t.surface },
    headerTintColor: t.text,
    tabBarStyle: { backgroundColor: t.surface, borderTopColor: t.border },
    tabBarActiveTintColor: t.primary,
    tabBarInactiveTintColor: t.textMuted,
    tabBarShowLabel: false,
  } as const
}

const TAB_ICONS: Record<string, string> = {
  Stream: 'cctv',
  Clips: 'filmstrip-box-multiple',
  Console: 'console',
  Settings: 'cog',
}

function Tabs() {
  const theme = useTheme()
  return (
    <Tab.Navigator
      screenOptions={({ route }) => ({
        ...screenOpts(theme),
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
      <Tab.Screen name="Console" component={ConsoleScreen} />
      <Tab.Screen name="Settings" component={SettingsScreen} />
    </Tab.Navigator>
  )
}

export function RootNavigator() {
  const [ready, setReady] = useState(false)
  const configured = useSyncExternalStore(subscribeConfig, () =>
    isConfigured(getConfig()),
  )
  const theme = useTheme()
  const nav = useMemo(() => navTheme(theme), [theme])

  useEffect(() => {
    loadConfig().then(() => setReady(true))
  }, [])

  if (!ready) {
    return (
      <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: theme.bg }}>
        <ActivityIndicator color={theme.textMuted} />
      </View>
    )
  }

  return (
    <NavigationContainer theme={nav}>
      <Stack.Navigator screenOptions={screenOpts(theme)}>
        {configured ? (
          <Stack.Screen name="Main" component={Tabs} options={{ headerShown: false }} />
        ) : (
          <Stack.Screen name="Setup" component={SettingsScreen} options={{ title: 'Connect to Horus' }} />
        )}
      </Stack.Navigator>
    </NavigationContainer>
  )
}
