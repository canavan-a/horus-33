import notifee from '@notifee/react-native'
import { Picker } from '@react-native-picker/picker'
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
  compareVersions,
  DowngradeBlockedError,
  fetchReleases,
  getVersionInfo,
  onDownloadProgress,
  type Release,
  runUpdate,
} from '../lib/appUpdater'
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

  const [version, setVersion] = useState<string>()
  const [releases, setReleases] = useState<Release[]>([])
  const [selectedTag, setSelectedTag] = useState<string>()
  const [updateStatus, setUpdateStatus] = useState<string>()
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    loadConfig().then(setCfg)
    getVersionInfo()
      .then((v) => setVersion(`${v.versionName} (${v.versionCode})`))
      .catch(() => setVersion('unknown'))
  }, [])

  useEffect(() => {
    if (!busy) return
    const off = onDownloadProgress(({ bytesDownloaded, bytesTotal }) => {
      const pct = bytesTotal > 0 ? Math.round((bytesDownloaded / bytesTotal) * 100) : 0
      setUpdateStatus(`Downloading… ${pct}%`)
    })
    return off
  }, [busy])

  const currentVersionName = version?.split(' ')[0]

  const onCheckUpdates = async () => {
    setUpdateStatus('Checking…')
    try {
      const rs = await fetchReleases()
      setReleases(rs)
      if (rs.length === 0) {
        setUpdateStatus('No releases found')
        return
      }
      setSelectedTag((t) => t ?? rs[0].tag)
      const latest = rs[0]
      const cmp = currentVersionName
        ? compareVersions(latest.version, currentVersionName)
        : 1
      setUpdateStatus(
        cmp > 0
          ? `Update available: v${latest.version}`
          : `Up to date (latest is v${latest.version})`,
      )
    } catch (e) {
      setUpdateStatus(`Check failed: ${String(e)}`)
    }
  }

  const onInstallUpdate = async () => {
    const target = releases.find((r) => r.tag === selectedTag)
    if (!target) return
    if (currentVersionName && compareVersions(target.version, currentVersionName) < 0) {
      Alert.alert(
        'Downgrade blocked',
        `v${target.version} is older than the installed v${currentVersionName}. Android will not replace a newer build — uninstall the app first, then install the older version.`,
      )
      return
    }
    setBusy(true)
    setUpdateStatus('Starting…')
    try {
      await runUpdate(target)
      setUpdateStatus('Opening installer…')
    } catch (e) {
      if (e instanceof DowngradeBlockedError) {
        Alert.alert('Downgrade blocked', 'Uninstall the app first to install an older version.')
        setUpdateStatus(undefined)
      } else {
        setUpdateStatus(`Update failed: ${String(e)}`)
      }
    } finally {
      setBusy(false)
    }
  }

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
    <ScrollView style={styles.scroll} contentContainerStyle={styles.container}>
      <Field label="Horus hostname">
        <TextInput
          style={styles.input}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="url"
          placeholder="horus.example.com"
          placeholderTextColor="#777"
          value={cfg.host}
          onChangeText={(t) => set('host', t.trim())}
        />
      </Field>

      <Text style={styles.section}>Cloudflare Access</Text>
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

      <Text style={styles.section}>Updates</Text>
      <Text style={styles.hint}>Current version: {version ?? '…'}</Text>

      {releases.length > 0 && (
        <View style={styles.pickerWrap}>
          <Picker
            selectedValue={selectedTag}
            onValueChange={(v) => setSelectedTag(String(v))}
            dropdownIconColor="#e5e5e5"
            style={styles.picker}
          >
            {releases.map((r, i) => (
              <Picker.Item
                key={r.tag}
                label={i === 0 ? `Latest — v${r.version}` : `v${r.version}`}
                value={r.tag}
                color="#111"
              />
            ))}
          </Picker>
        </View>
      )}

      <TouchableOpacity style={styles.btnOutline} onPress={onCheckUpdates}>
        <Text style={styles.btnOutlineText}>Check for updates</Text>
      </TouchableOpacity>
      {releases.length > 0 && (
        <TouchableOpacity
          style={[styles.btn, busy && styles.btnDisabled]}
          disabled={busy}
          onPress={onInstallUpdate}
        >
          <Text style={styles.btnText}>Download &amp; install</Text>
        </TouchableOpacity>
      )}
      {updateStatus ? <Text style={styles.hint}>{updateStatus}</Text> : null}

      <View style={styles.rowBetween}>
        <View style={{ flex: 1 }}>
          <Text style={styles.label}>Check for updates automatically</Text>
          <Text style={styles.hint}>Checks GitHub Releases when the app opens.</Text>
        </View>
        <Switch
          value={cfg.autoUpdateCheck}
          onValueChange={(v) => set('autoUpdateCheck', v)}
        />
      </View>
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
  scroll: { flex: 1, backgroundColor: '#111' },
  container: { padding: 16, gap: 12, backgroundColor: '#111' },
  field: { gap: 4 },
  label: { color: '#e5e5e5', fontSize: 13 },
  hint: { color: '#a0a0a0', fontSize: 11 },
  section: { color: '#8a8a8a', fontSize: 12, marginTop: 12, textTransform: 'uppercase' },
  input: {
    borderWidth: 1,
    borderColor: '#4a4a4a',
    borderRadius: 8,
    paddingHorizontal: 10,
    paddingVertical: 8,
    color: '#f5f5f5',
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
  btnDisabled: { opacity: 0.5 },
  pickerWrap: {
    borderWidth: 1,
    borderColor: '#3a3a3a',
    borderRadius: 8,
    overflow: 'hidden',
  },
  picker: { color: '#e5e5e5' },
  btnOutline: {
    borderWidth: 1,
    borderColor: '#3a3a3a',
    borderRadius: 8,
    paddingVertical: 12,
    alignItems: 'center',
  },
  btnOutlineText: { color: '#e5e5e5' },
})
