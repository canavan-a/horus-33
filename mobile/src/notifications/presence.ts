import notifee, { AndroidImportance } from '@notifee/react-native'
import type { PresenceEvent } from '../lib/proto'
import { CH_ALERTS, NID_ENTER, NID_LEAVE, ensureChannels } from './channels'

// undefined until the first event after a (re)connect, so the initial-burst
// presence snapshot re-primes the edge detector without firing a notification.
let lastPresent: boolean | undefined

export function resetPresenceEdge() {
  lastPresent = undefined
}

function now(): string {
  return new Date().toLocaleTimeString()
}

export async function onPresence(ev: PresenceEvent): Promise<void> {
  await ensureChannels()

  if (lastPresent === undefined) {
    lastPresent = ev.present
    return
  }
  if (ev.present === lastPresent) return

  if (ev.present) {
    // rising edge
    await notifee.cancelDisplayedNotification(NID_LEAVE)
    await notifee.displayNotification({
      id: NID_ENTER,
      title: 'Person entered frame',
      body: now(),
      android: {
        channelId: CH_ALERTS,
        importance: AndroidImportance.HIGH,
        smallIcon: 'ic_launcher',
        pressAction: { id: 'default' },
        timestamp: Date.now(),
        showTimestamp: true,
      },
    })
  } else {
    // falling edge — clear the "entered" alert, say they left
    await notifee.cancelDisplayedNotification(NID_ENTER)
    await notifee.displayNotification({
      id: NID_LEAVE,
      title: 'Person left frame',
      body: now(),
      android: {
        channelId: CH_ALERTS,
        importance: AndroidImportance.HIGH,
        smallIcon: 'ic_launcher',
        pressAction: { id: 'default' },
        timestamp: Date.now(),
        showTimestamp: true,
        timeoutAfter: 60_000,
        autoCancel: true,
      },
    })
  }
  lastPresent = ev.present
}

// Called from an AppState 'active' listener — the user is looking at the app,
// so the enter/leave alerts have served their purpose. The ongoing FGS
// notification is left alone.
export async function clearPresenceAlerts(): Promise<void> {
  await notifee.cancelDisplayedNotification(NID_ENTER)
  await notifee.cancelDisplayedNotification(NID_LEAVE)
}
