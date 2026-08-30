import notifee, {
  AndroidForegroundServiceType,
  AndroidImportance,
} from '@notifee/react-native'
import { getConfig } from '../lib/config'
import { CH_SERVICE, NID_FGS, ensureChannels } from '../notifications/channels'
import { onPresence, resetPresenceEdge } from '../notifications/presence'
import { HorusSocket } from '../ws/HorusSocket'

// The body of the Notifee foreground service. Runs for as long as the FGS
// notification is shown; we keep it alive by never resolving until Notifee
// tears the service down (which rejects the promise / calls it again).
let socket: HorusSocket | undefined

export function backgroundRunner(): Promise<void> {
  return new Promise(() => {
    void ensureChannels()
    resetPresenceEdge()

    socket = new HorusSocket('background')
    socket.onPresence((ev) => {
      void onPresence(ev)
    })
    socket.start()
    // Never resolves. stopMonitoring() below closes the socket; Notifee kills
    // the task when stopForegroundService() is called.
  })
}

// Start the FGS + runner. Must be called while the app is foreground
// (Android 12+ restriction).
export async function startMonitoring(): Promise<void> {
  if (!getConfig().host) return
  await ensureChannels()
  await notifee.displayNotification({
    id: NID_FGS,
    title: 'Horus monitoring',
    body: 'Watching for people in frame',
    android: {
      channelId: CH_SERVICE,
      importance: AndroidImportance.LOW,
      smallIcon: 'ic_launcher',
      ongoing: true,
      asForegroundService: true,
      foregroundServiceTypes: [AndroidForegroundServiceType.FOREGROUND_SERVICE_TYPE_DATA_SYNC],
    },
  })
}

export async function stopMonitoring(): Promise<void> {
  socket?.close()
  socket = undefined
  await notifee.stopForegroundService()
}
