import notifee, { AndroidImportance } from '@notifee/react-native'
import { getConfig } from '../lib/config'
import { THEMES } from '../theme'

export const CH_SERVICE = 'presence-service'
export const CH_ALERTS = 'presence-alerts'

export const NID_FGS = 'horus-fgs'
export const NID_ENTER = 'horus-presence-enter'
export const NID_LEAVE = 'horus-presence-leave'

let ensured = false

export async function ensureChannels(): Promise<void> {
  if (ensured) return
  await notifee.createChannel({
    id: CH_SERVICE,
    name: 'Monitoring',
    importance: AndroidImportance.LOW,
    sound: undefined,
  })
  await notifee.createChannel({
    id: CH_ALERTS,
    name: 'Person alerts',
    importance: AndroidImportance.HIGH,
  })
  ensured = true
}

// Shared look and tap behavior for every notification. The tint follows the
// current theme's accent. The tap names MainActivity explicitly: notifee's
// 'default' launch activity resolves the launcher entry and Class.forName()s
// it, but that entry is now a per-theme <activity-alias> (IconDark, ...), not
// a class, so a plain 'default' tap silently does nothing.
export function notifAppearance() {
  return {
    smallIcon: 'ic_notification',
    color: THEMES[getConfig().themeName]?.primary ?? THEMES.dark.primary,
    pressAction: { id: 'default', launchActivity: 'com.horusmobile.MainActivity' },
  }
}
