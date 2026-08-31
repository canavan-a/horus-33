# Horus mobile (Android)

A sideloaded (direct-APK, no Play Store) Android client for horus-33. Deliberately
smaller than `web/`: jog pad + its buttons, the WHEP video stream, clips, and
background "person in frame" notifications. No per-axis settings panel.

## Stack

- React Native 0.75 (bare CLI), Hermes, old architecture.
- `react-native-webrtc` for the WHEP viewer, `react-native-video` for clip playback.
- `@notifee/react-native` for the notification channels **and** the foreground
  service that holds the background WebSocket open.
- `@react-navigation` bottom tabs: Stream / Jog / Clips / Settings.

## Connecting

Settings screen. Two shapes:

- **LAN** — scheme `http`, host `192.168.1.50`, API port `8080` (or `8090` for
  `dev.sh`), media port `8889`.
- **Remote** — scheme `https`, host = a `cloudflared` tunnel hostname pointing at
  `horus-server`, behind Cloudflare Access. Fill in the **CF-Access-Client-Id /
  Secret** service-token fields; they ride every REST call and the WS upgrade.
  Live video then needs a TURN server advertised by MediaMTX (media can't cross
  the Cloudflare HTTP proxy); control, clips and notifications work regardless.

## Background notifications

Enable "Background alerts" in Settings. A Notifee foreground service
(`dataSync` type) keeps one `wss://` connection to `/api/ws` open and raises a
local notification on the server's `presence` rising edge; the falling edge
clears it and posts "Person left frame". Opening the app clears both.

The service can only be (re)started while the app is foreground (Android 12+), so
open the app once after a reboot to re-arm alerts. Grant the notification
permission and add the app to the battery-optimization allowlist when prompted.
Expected cost is that of one idle TLS socket held by an FGS (~2–5%/day).

## Develop

```
npm install
npm start                 # Metro
npm run android           # debug build onto a connected device
```

## Release APK

```
cd android
HORUS_KEYSTORE_FILE=$PWD/app/release.keystore \
HORUS_KEYSTORE_PASSWORD=... HORUS_KEY_ALIAS=horus HORUS_KEY_PASSWORD=... \
./gradlew --no-daemon assembleRelease -PversionName=0.0.1 -PversionCode=1
```

Output: `android/app/build/outputs/apk/release/app-release.apk`.

Releases are built and published locally from this command; there is no release CI.

Create the keystore once:

```
keytool -genkeypair -v -keystore release.keystore -storetype PKCS12 \
  -alias horus -keyalg RSA -keysize 2048 -validity 10000 \
  -dname "CN=Horus, O=horus-33, C=US"
base64 -w0 release.keystore   # -> HORUS_KEYSTORE_BASE64 secret
```
