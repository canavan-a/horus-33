# Add any project specific keep options here:

# react-native-webrtc
-keep class org.webrtc.** { *; }
-dontwarn org.webrtc.**
-keep class com.oney.WebRTCModule.** { *; }

# notifee
-keep class app.notifee.** { *; }
-dontwarn app.notifee.**

# react-native-video (ExoPlayer)
-keep class com.google.android.exoplayer2.** { *; }
-dontwarn com.google.android.exoplayer2.**
