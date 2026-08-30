import notifee, { AndroidImportance } from '@notifee/react-native'

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
