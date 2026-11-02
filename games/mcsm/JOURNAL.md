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

## Bring-up (2026-11-03)

Each step its own generic core fix:
- Java natives of any library Java loads (lookup beyond the merged table);
  Thumb entries allowed (CallGuest switches the mode; SDL's are Thumb).
- dlsym on a system library handle serves every egl* name (SDL loads EGL by
  dlsym); eglBindAPI, eglGetConfigs, eglGetCurrent*, eglQuerySurface/Context,
  eglSwapInterval, eglWaitGL/Native, eglReleaseThread, eglQueryString.
- GL_VERSION "OpenGL ES 2.0", GLSL "1.00", GL_EXTENSIONS filtered to those
  without entry points: the host forwards GLES2 and eglGetProcAddress offers
  nothing, so a guest told ES 3.2 used mapped buffers it never got (NULL store
  in RenderUtility::Initialize). glDepthRangef, glGetBufferParameteriv.
- pread/pwrite were stubs returning 0: the OBB header read "0 bytes" and
  TTArchive2 got no stream. Note: [stub] lines of worker threads only show
  with HARNESS_DIAG.
- Black screen at 60 fps: glShaderSource forwarded one part per call (a
  Telltale shader is a #define header + body in 3-5 parts): every compile
  failed, and the engine retried each frame. Now all parts in one call; empty
  parts allowed, no silent cut at 16; glGetShaderSource added.
- 2026-11-03: the main menu shows ("Начать"), no LOUD. Side strips on the
  20.8:9 screen: the vignette covers the 16:9 area only; his port ships
  split_widescreen/split_hide_border for it, so most likely the original's
  look (not yet compared with the native 32-bit APK).
- Open: FMOD audio ("FindClass org/fmod/AudioDevice FAILED").

## Plan

1. Profile + tree: host as which lib name, the other four guest libs.
2. Boot on the sandbox, fill missing imports/JNI.
3. Data placement and path redirect.
4. Measure both ports on the sandbox (same scene, system frame stats).
