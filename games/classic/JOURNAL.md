# Angry Birds (Classic) 8.0.3 — journal

Original APK: `original/com.rovio.angrybirds.8.0.3.apk` (103 362 395 B,
sha256 3c86364e...), signed by Rovio (signer sha256 3bf28170..., the same as
the other Rovio titles): the Google Play original.

## Layout (2026-11-09)

- Engine: `libAngryBirdsClassic.so` (Rovio fusion, the Star Wars II
  generation; `Gen.library_name` "AngryBirdsClassic"). JNI: 72 exports
  (31 com.rovio.fusion + 41 com.rovio.rcs); the Java side declares 80
  natives. 8 have no export (CameraWrapper nativeCameraStatus /
  nativeProcessFrame, LocationWrapper's four callbacks, Share.onShareClosed,
  VideoPlayerBridge.onVideoPaused): the original would fail them the same
  way; their forwarders are inert.
- libjs.so / libadcolony.so stripped, as for the regular Star Wars II.
- Levels, scripts, images and audio ship inside the APK (4.4k files).
- The union of forwarders grew by 5 (177 -> 182); nothing removed.

## 2026-11-09: first boot

- Sandbox, first try: 60 s alive, every marker 0, death.log 0 B; the owner
  reached the first level (tutorial popup). No host change was needed.
- To look at: the Android status bar shows over the game on the sandbox
  (the other titles hide it).
