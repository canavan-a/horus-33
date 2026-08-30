import 'react-native-gesture-handler'
import notifee from '@notifee/react-native'
import { AppRegistry } from 'react-native'
import { name as appName } from './app.json'
import App from './src/App'
import { backgroundRunner } from './src/service/PresenceService'

// The Notifee foreground service that holds the background WebSocket open for
// presence notifications.
notifee.registerForegroundService(() => backgroundRunner())

AppRegistry.registerComponent(appName, () => App)
