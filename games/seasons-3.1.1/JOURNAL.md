# Angry Birds Seasons 3.1.1 (2013) — journal

Original APK: sent by a forum user (`original/AngryBirdsSeasons_v3.1.1.apk`,
35 194 610 B). Signed with the public AOSP test key, not Rovio's: someone
rebuilt it; there is no Rovio 3.1.1 to compare the guest library with.
Package `com.rovio.angrybirdsseasons` (as `seasons` 6.6.2), versionCode 3101,
minSdk 8, targetSdk 13. Rovio fusion engine, GLES2, `com.rovio.fusion` Java.
The forum user wants it because it is the last version before the season
birds got special abilities.

## 2026-11-16: added

- Installs as `com.rovio.angrybirdsseasons311`, "Angry Birds Seasons 3.1.1",
  next to `seasons` (the rename from `rio-1.0.0`); targetSdk 26.
- Guest `libAngryBirdsSeasons.so` (3 213 976 B, 260 imports, 35 JNI
  exports). The host lacked `strlcat` and `dladdr`: implemented, with tests.
- `.init_array` starts with -1 and ends with 0 (an old toolchain's markers):
  the harness counts 15/17 clean, all 15 real entries.
- JNI: `MyRenderer`/`MyLegacyRenderer.nativeInit` take `(IILjava/lang/String;)`
  here and `(II)` in Stella; `nativeKeyInput` `(III)` here, `(IIII)` elsewhere.
  `host/gen_apphost.py` now emits such a method under the JNI long name, one
  forwarder per signature (the VM looks the long name up when the short one
  is absent).
- Sandbox: boots to its menu first try, LOUD 0; the other nine titles boot to
  their menus with LOUD 0 after the forwarder change (Stella and 99 Bricks
  re-run with 70 s: Play Protect / Play Games dialogs had delayed them).

Not yet: levels and sound played, the phone.
