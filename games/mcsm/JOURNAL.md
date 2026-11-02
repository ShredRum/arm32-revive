# Minecraft: Story Mode (Telltale) — journal

Goal (owner, 2026-11-03): run the original 32-bit game on our core and compare
performance against phys-winner's arm64 bridge port of the same version.

## Inputs (not in git)

- Original: `games/mcsm/original/Minecraft-Story-Mode-v1-37-Mali-unlocked.apk`
  (owner's copy, from `~/Рабочий стол/MCSM_arm32`): com.telltalegames.minecraft100
  1.37 (10137), min sdk 10, libs armeabi-v7a + x86 only, ETC1 (Mali) data.
  Data next to it: Android/obb (main.10129 777 MB, patch.10135), Android/data
  (4.5 GB downloaded episodes, *_etc1), Telltale/*.mft.
- His port: `~/Рабочий стол/Minecraft.Story.Mode.v.1.37.b10137_modern_mod.apks`
  (split APKs, base + 7 episodes, 6.4 GB): min sdk 24, arm64 libmain.so host,
  guest libs in assets/guestlib/armeabi-v7a, OBB inside assets/telltale/obb,
  launcher TelltaleActivity (skips the downloader). Guest libs identical to the
  original except libGameEngine.so: 4 bytes differ at file offset 5757853.
- Sandbox GPU: real Xclipse 540 via ANGLE/Vulkan (the phone's driver). ETC1,
  ETC2, ASTC, DXT1 only; no ATC/PVRTC.

## Findings

- Java (SDLActivity <clinit>) loads SDL2, fmod, fmodstudio, GameEngine, main.
  Launcher: expansion SampleDownloaderActivity (checks OBB), then TelltaleActivity.
- Data lives on external storage (/sdcard/Android/obb, /sdcard/Android/data,
  /sdcard/Telltale). On the sandbox /sdcard is the phone's real storage: never
  write there. Plan (owner, agreed): data in the app's private dir, native
  paths redirected by the host; a minimal Java edit only if the downloader
  check blocks, recorded as this title's exception.
- harness (PC) on libGameEngine.so alone: 211/212 static inits clean.

## Port setup (2026-11-03)

- Host as libmain.so (the name Java loads last); host.stub_libs gives the
  other four names empty arm64 libraries, so Java's loadLibrary calls pass.
- guest.java_loads: every guest library Java loads runs its initialisers and
  JNI_OnLoad in Java's order (marked "java" in guest.libs; titles without the
  key keep the old primary-only behaviour).
- gen_apphost.py scans every smali tree (natives outside com/rovio); the
  generated file is byte-identical for the earlier titles. 29 natives matched.
- host.private_sdcard = on: all shared storage is the app's files/sdcard for
  the guest. Java's expansion downloader built the OBB path from
  Environment.getExternalStorageDirectory(); games/mcsm/smali.patch makes
  Helpers.getSaveFilePath use getFilesDir()/sdcard instead: this title's one
  Java edit (applied by prepare-tree.sh), so Java and the host see one place.
- First boot without data: Java "Telltale File Validation Failed" (OBB check),
  as expected. Data copied to files/sdcard with the sdcard layout.

## Plan

1. Profile + tree: host as which lib name, the other four guest libs.
2. Boot on the sandbox, fill missing imports/JNI.
3. Data placement and path redirect.
4. Measure both ports on the sandbox (same scene, system frame stats).
