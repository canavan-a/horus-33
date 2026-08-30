import notifee from '@notifee/react-native'
import React, { useEffect, useState } from 'react'
import {
  Alert,
  Linking,
  Platform,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native'
import { getLink, notifySubscribe } from '../api/client'
import {
  DEFAULT_CONFIG,
  type HorusConfig,
  loadConfig,
  saveConfig,
} from '../lib/config'
import { startMonitoring, stopMonitoring } from '../service/PresenceService'
import { reconfigureHorus } from '../ws/useHorus'

export function SettingsScreen() {
  const [cfg, setCfg] = useState<HorusConfig>(DEFAULT_CONFIG)
  const [testResult, setTestResult] = useState<string>()

  useEffect(() => {
    loadConfig().then(setCfg)
  }, [])

  const set = <K extends keyof HorusConfig>(k: K, v: HorusConfig[K]) =>
    setCfg((c) => ({ ...c, [k]: v }))

  const onSave = async () => {
    await saveConfig(cfg)
    reconfigureHorus()
    notifySubscribe(true).catch(() => {})
    if (cfg.bgAlerts) {
      await notifee.requestPermission()
      await startMonitoring()
    } else {
      await stopMonitoring()
    }
    Alert.alert('Saved')
  }

  const onTest = async () => {
    setTestResult('…')
    try {
      const l = await getLink()
      setTestResult(`link: ${l.status}${l.error ? ` (${l.error})` : ''}`)
    } catch (e) {
      setTestResult(`failed: ${String(e)}`)
    }
  }

  return (
    <ScrollView contentContainerStyle={styles.container}>
      <Field label="Scheme (http / https)">
        <TextInput
          style={styles.input}
          autoCapitalize="none"
          value={cfg.scheme}
          onChangeText={(t) => set('scheme', t === 'https' ? 'https' : 'http')}
        />
      </Field>
      <Field label="Host (IP or tunnel hostname)">
        <TextInput
          style={styles.input}
          autoCapitalize="none"
          placeholder="192.168.1.50 or horus.example.com"
          placeholderTextColor="#555"
          value={cfg.host}
          onChangeText={(t) => set('host', t.trim())}
        />
      </Field>
      <Field label="API port (0 = use scheme default)">
        <TextInput
          style={styles.input}
          keyboardType="number-pad"
          value={String(cfg.apiPort)}
          onChangeText={(t) => set('apiPort', Number(t) || 0)}
        />
      </Field>
      <Field label="Media port (MediaMTX WHEP, LAN only)">
        <TextInput
          style={styles.input}
          keyboardType="number-pad"
          value={String(cfg.mediaPort)}
          onChangeText={(t) => set('mediaPort', Number(t) || 0)}
        />
      </Field>

      <Text style={styles.section}>Cloudflare Access (leave blank on LAN)</Text>
      <Field label="CF-Access-Client-Id">
        <TextInput
          style={styles.input}
          autoCapitalize="none"
          value={cfg.cfAccessClientId}
          onChangeText={(t) => set('cfAccessClientId', t.trim())}
        />
      </Field>
      <Field label="CF-Access-Client-Secret">
        <TextInput
          style={styles.input}
          autoCapitalize="none"
          secureTextEntry
          value={cfg.cfAccessClientSecret}
          onChangeText={(t) => set('cfAccessClientSecret', t.trim())}
        />
      </Field>

      <View style={styles.rowBetween}>
        <View style={{ flex: 1 }}>
          <Text style={styles.label}>Background alerts</Text>
          <Text style={styles.hint}>
            Keeps one WebSocket open in a foreground service (~2–5%/day battery) to notify
            when someone enters frame.
          </Text>
        </View>
        <Switch value={cfg.bgAlerts} onValueChange={(v) => set('bgAlerts', v)} />
      </View>

      <TouchableOpacity style={styles.btn} onPress={onSave}>
        <Text style={styles.btnText}>Save</Text>
      </TouchableOpacity>
      <TouchableOpacity style={styles.btnOutline} onPress={onTest}>
        <Text style={styles.btnOutlineText}>Test connection</Text>
      </TouchableOpacity>
      {testResult ? <Text style={styles.hint}>{testResult}</Text> : null}

      {Platform.OS === 'android' && (
        <TouchableOpacity
          style={styles.btnOutline}
          onPress={() => Linking.openSettings()}
        >
          <Text style={styles.btnOutlineText}>Battery optimization settings</Text>
        </TouchableOpacity>
      )}
    </ScrollView>
  )
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <View style={styles.field}>
      <Text style={styles.label}>{label}</Text>
      {children}
    </View>
  )
}

const styles = StyleSheet.create({
  container: { padding: 16, gap: 12 },
  field: { gap: 4 },
  label: { color: '#e5e5e5', fontSize: 13 },
  hint: { color: '#8a8a8a', fontSize: 11 },
  section: { color: '#8a8a8a', fontSize: 12, marginTop: 12, textTransform: 'uppercase' },
  input: {
    borderWidth: 1,
    borderColor: '#3a3a3a',
    borderRadius: 8,
    paddingHorizontal: 10,
    paddingVertical: 8,
    color: '#e5e5e5',
  },
  rowBetween: { flexDirection: 'row', alignItems: 'center', gap: 12, marginTop: 8 },
  btn: {
    backgroundColor: '#2563eb',
    borderRadius: 8,
    paddingVertical: 12,
    alignItems: 'center',
    marginTop: 8,
  },
  btnText: { color: '#fff', fontWeight: '600' },
  btnOutline: {
    borderWidth: 1,
    borderColor: '#3a3a3a',
    borderRadius: 8,
    paddingVertical: 12,
    alignItems: 'center',
  },
  btnOutlineText: { color: '#e5e5e5' },
})
