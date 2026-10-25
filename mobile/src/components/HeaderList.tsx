import React, { useState } from 'react'
import { StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native'
import type { AuthHeader } from '../lib/config'
import { type Theme, useTheme, useThemedStyles } from '../theme'

// Editor for the auth headers sent on every request: any number of
// name/value rows. Values are masked (they're usually secrets) until shown.
export function HeaderList({
  headers,
  onChange,
}: {
  headers: AuthHeader[]
  onChange: (next: AuthHeader[]) => void
}) {
  const theme = useTheme()
  const styles = useThemedStyles(makeStyles)
  // Indexes of rows whose value is shown in the clear. UI-only.
  const [shown, setShown] = useState<Set<number>>(new Set())

  const update = (i: number, patch: Partial<AuthHeader>) =>
    onChange(headers.map((h, j) => (j === i ? { ...h, ...patch } : h)))

  const remove = (i: number) => {
    onChange(headers.filter((_, j) => j !== i))
    setShown((s) => new Set([...s].filter((j) => j !== i).map((j) => (j > i ? j - 1 : j))))
  }

  const toggleShown = (i: number) =>
    setShown((s) => {
      const next = new Set(s)
      next.has(i) ? next.delete(i) : next.add(i)
      return next
    })

  return (
    <>
      {headers.map((h, i) => (
        <View key={i} style={styles.row}>
          <View style={styles.fields}>
            <TextInput
              style={styles.input}
              autoCapitalize="none"
              autoCorrect={false}
              placeholder="Header name"
              placeholderTextColor={theme.textMuted}
              value={h.name}
              onChangeText={(t) => update(i, { name: t.trim() })}
            />
            <View style={styles.valueRow}>
              <TextInput
                style={[styles.input, styles.value]}
                autoCapitalize="none"
                autoCorrect={false}
                secureTextEntry={!shown.has(i)}
                placeholder="Value"
                placeholderTextColor={theme.textMuted}
                value={h.value}
                onChangeText={(t) => update(i, { value: t })}
              />
              <TouchableOpacity onPress={() => toggleShown(i)} hitSlop={8}>
                <Text style={styles.linkText}>{shown.has(i) ? 'Hide' : 'Show'}</Text>
              </TouchableOpacity>
            </View>
          </View>
          <TouchableOpacity
            onPress={() => remove(i)}
            hitSlop={8}
            accessibilityLabel={`Remove header ${h.name}`}
          >
            <Text style={styles.remove}>✕</Text>
          </TouchableOpacity>
        </View>
      ))}
      <TouchableOpacity
        style={styles.btnOutline}
        onPress={() => onChange([...headers, { name: '', value: '' }])}
      >
        <Text style={styles.btnOutlineText}>Add header</Text>
      </TouchableOpacity>
    </>
  )
}

function makeStyles(theme: Theme) {
  return StyleSheet.create({
    row: {
      flexDirection: 'row',
      alignItems: 'center',
      gap: 10,
      padding: 10,
      borderWidth: 1,
      borderColor: theme.border,
      borderRadius: 8,
    },
    fields: { flex: 1, gap: 6 },
    valueRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
    value: { flex: 1 },
    input: {
      borderWidth: 1,
      borderColor: theme.border,
      borderRadius: 8,
      paddingHorizontal: 10,
      paddingVertical: 8,
      color: theme.text,
      backgroundColor: theme.surface,
    },
    linkText: { color: theme.primary, fontSize: 12 },
    remove: { color: theme.danger, fontSize: 16, paddingHorizontal: 4 },
    btnOutline: {
      borderWidth: 1,
      borderColor: theme.border,
      borderRadius: 8,
      paddingVertical: 12,
      alignItems: 'center',
    },
    btnOutlineText: { color: theme.text },
  })
}
