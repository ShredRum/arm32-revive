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

## State 2026-11-03 night (before a context compaction)

Branch mcsm, code committed. Menu works ("Начать"). "Играть" -> storage
permission (granted) -> "ПОДКЛЮЧЕНИЕ / Проверка на наличие загружаемого
контента..." with the chest icon animated, forever (sometimes frames stop too).
Engine makes NO socket calls; worker threads idle in sem_wait; main thread t3
loops in Thread::PlatformSleep. It loaded _resdesc of episodes 102..108.
Done meanwhile (core, generic): mkdir of /storage, /storage/emulated (above a
storage root) -> an existing empty dir (was ENOENT); paths under the private
sdcard resolve case-insensitively (device shared storage ignores case; the
engine opens "android/obb/...").
Sandbox data layout now: files/sdcard/{Android/obb/<pkg>/*.obb, Telltale/},
episodes moved to /data/data/<pkg>/ext/files/{Temp,Net} (the engine's
getTelltaleStorageDirectory = <ext>/Android/data/<pkg>/files, which the
ext_tail_ rule maps to <data>/ext). After this move: still hangs (untested why).
Next ideas: Java side calls during the wait (HARNESS_DIAG with the owner
tapping; the [call] log is capped at 5 per name), isNetworkAvailable is real
(device has wifi) so the engine may wait for its HTTP (curl, services.
telltalegames.com) that never starts; check what Lua waits on; compare with
phys-winner's TWAU bridge (same engine). His 4-byte libGameEngine patch =
Application_SDL::ScaleScreen widescreen skip (b over the 16:9 fit), unrelated.
Never leave abflags.txt on the device (removed each time so far).
Before merging: gate the five Rovio titles (glGetString now says GLES2 and
filters extensions, egl/dlsym changes).

## The "Подключение" hang: audio (2026-11-03)

Thread report during the hang: main t3 in GameEngine::OnUserSpaceAvailable ->
ResourceFramer::ConstructResourceSystemFromGameDataDirectories ->
SoundSystem::ReloadMasterAudioBus -> Messages::BlockingSend, polling with
Thread::PlatformSleep for the sound thread's answer; the sound thread never
ran because FMOD found no org.fmod.AudioDevice. Cause (core, generic): the
loader RealClass falls back to came from JNI_OnLoad's context class loader,
which here was the system loader ("DexPathList[[directory "."]]"). Now the
host takes the loader of the app class behind the first Java->native call
(its thiz). After it: AudioDevice is a real object, the check passes, the
Telltale account offer shows ("Войти / Вход / Нет, спасибо").

## Speed vs phys-winner's port (2026-11-04, sandbox)

Ours, episode 1 opening (new game, first 60 s), SurfaceFlinger --latency on
the game's SurfaceView, 5 s windows: 37..53 fps, average 46 (our [frames]
counter agrees). Same package name, so the ports cannot sit side by side:
our data dir was moved aside (/data/local, same partition), his base +
split_hide_border installed (no widescreen split: same pixel count).
His port aborts ~0.5 s after the GL context on the sandbox, every launch:
stderr "assertion failed: false / Message: Interpret should never be
emitted." -- his Dynarmic met a guest instruction its arm64 backend cannot
run (strace via Termux). Not a data or sandbox-path problem. No comparison
on the sandbox therefore; the phone (owner, adb) is the only place left.
Ours restored afterwards (new uid: chown -R + chcon app_data_file), menu OK.
Note: under sudo, "am" is Termux's (no force-stop): use /system/bin/am.

## Host overhead, measured and cut (2026-11-04)

Diag builds now time every import (calls, host time; arm64 virtual counter:
clock_gettime is a ~390 ns syscall on the sandbox, no vDSO, which first
swamped the numbers). Episode 1 scene, per 30 s, sandbox:

| change | before | after |
|---|---|---|
| sinf etc. in the fast-math table (fast_math.h) | 1.5 M calls x 2.5 us | off the top list |
| GL by load-time id, no string/snprintf (gl_ids.h) | glUniform4fv 4.3 us | 3.1 us |
| eglGetCurrentContext once per thread | glUniform4fv 3.1 us | 2.16 us = bare driver |
| guest mutex: trylock before the sliced wait | lock 5.7 us | 3.2 us |
| guest mutex map: own shared_mutex | lock 3.2 us | 1.6 us |
| R-012: foreign unlock line per call | ~470 log lines/s | 5 lines, then a count |

Frames: 40-49 -> 56-60 per second (60 = display cap). Bare ANGLE on the
sandbox: glUniform4fv 2.16 us, glDisableVertexAttribArray/glBindBuffer/
glGetIntegerv ~1.04 us, eglGetCurrentContext 0.97 us (a native test).
Left on purpose: BoundBuffer asks the driver (glGetIntegerv) per
glVertexAttribPointer/glDrawElements, ~17 ms/s; a cached binding could go
stale when Java recreates a Rovio context behind the host (REVIEW E-03, a
suspected defect, never seen failing). Safe form: docs/jit-dispatch.md.
Later the same day: cached GL buffer bindings, safe by construction
(dropped at every Java->guest call, JNI call, eglMakeCurrent/ReleaseThread,
glDeleteBuffers; the context moved t3 -> t47 on resume, handled); free's
quarantine in a fixed ring (3.3 -> 1.9 us under the lock). Frames 59-60.
The numbers are wall time with ~45 threads on 8 cores (descheduling is in
them). Not measured on the phone (its driver and syscalls are cheaper).

## Before/after at 120 Hz (2026-11-04 evening)

The owner raised the sandbox display to 120 Hz; the game has no 60 fps cap
of its own (eglSwapInterval follows the display), so fps measures the core
again. Same protocol for both: new game, episode 1, first 60 s,
SurfaceFlinger counter. Morning core bc3a90b (sound fixed, no speed work):
average 47 fps (38-57 per 5 s). Current core e781c23 (fast math, GL ids,
EGL check once, GL binding cache, mutex trylock/map lock/holder book,
quarantine off, svc counter, knobs read once, signals at first send,
semaphore map lock): average 100 fps (86-107). Old core built in a
throwaway git worktree; the current APK was reinstalled after.

Caution (same evening): a later 60 s run gave 90 fps, but Rio and a hung
99 Bricks were still running from earlier gates; that number is void, and
whether other titles ran during the 47/100 pair is not known (the pair ran
back to back, so both had the same company). devgate.sh now stops every
other title of the project before it installs. Measure with only the game
under test running, and compare builds back to back (the phone heats).

Direct jump (f9a10ec) against e781c23, back to back both ways, nothing else
running: 97 / 94 (e781c23 first), 88 / 88 (current first). No fps change
beyond the run-to-run spread (~5 %). Per call the jump is a large cut
(pthread_self 1413 -> 99 ns, frexpf 5861 -> 544 ns), but those imports
were ~13 % of one core spread over worker threads: it saves CPU (heat), the
frame is set by the render thread and the GPU.

## State 2026-11-04 late (before a context compaction)

Branch mcsm, everything committed. Core today: class loader fix (audio),
R-012, fast math, GL ids, EGL check once, GL binding cache, mutex rework
(trylock, own map lock, holder book in the object, destroy as bionic, no
witness), semaphores own lock, signals sliced only after a send, heap own
lock, quarantine removed (R-013, owner: never back), every import by
ImportId + switch, RealGL lock off the call path. Lists with numbers:
docs/minimum.md (E done, F not done, B/C pending), docs/jit-dispatch.md.
MCSM sandbox: 47 -> ~90-100 fps at 120 Hz (spread ~5 %).
Measuring rules: only the game under test running (devgate stops the
others), builds back to back both orders, SurfaceFlinger counter, new game
episode 1 first 60 s.
Not done yet: a gate of all titles on this core (only rio, 99 Bricks, mcsm
booted after the last changes; 99 Bricks hit R-010 once in ~10 boots
today); the owner's phone still has the midday APKs. MCSM is not on the
phone (its data needs a place a non-root phone can fill).
Next, in order: gate all titles + send APKs to the phone; F8 (MCSM on the
phone, data outside the private dir) and phone measurements; then F4 (GL
thread) or F5 (JIT) by what the phone shows. Later: B1/B5/B6 policy, C
cleanup, R-010 (Mono metadata assert, possibly a use-after-free: fix at
the source).

## Plan

1. Profile + tree: host as which lib name, the other four guest libs.
2. Boot on the sandbox, fill missing imports/JNI.
3. Data placement and path redirect.
4. Measure both ports on the sandbox (same scene, system frame stats).

## 2026-11-05: GL or JIT? sandbox split, redundant GL state calls (F9)

Diag build, episode scene, per 30 s: process ~2.5 cores; host imports
(non-waiting) ~0.9 core, of it GL ~0.7 core, all on the render thread:
~20 s of its 30 s in GL, so on the sandbox the frame waits on GL. ANGLE
inflates GL (glClear 119 us, glDrawArrays 134 us, glUniform4fv 1.6 us),
so the phone decides GL vs JIT. Per-thread CPU cannot be read on the
sandbox: its kernel reports process totals in every task's stat.

The engine repeats state: attrib enable/disable 2.9 M calls, 97 % set the
value already set; uniforms 2 M, ~70 % (74 % of single-value calls);
glBindBuffer 32 %; glUseProgram 4 %. One thread sets uniforms; dropping
the copy at every return to Java loses no repeats. Repeats are now not
sent (realgl.cpp, the comment above UniformRepeat lists why it is exact).
Ship A/B, back to back, 60 s each: C 98, A 96, C 98 avg; the steady half
~107 vs ~100. Picture normal, markers 0.

## 2026-11-05: packed files, split APK set for phones without root (F8)

The data cannot reach a non-root phone's private dir, and Android 11+ closes
Android/obb and Android/data to file managers. Core: packed files
(host/src/guest/apk_vfs.h, 0abb119): assets/abport/<root>/ of base.apk and
its split_*.apk read in place as ordinary files (overlay: real files win,
copy-up on write, persistent deletion marks, case-insensitive). Packaging:
scripts/pack_apks.py mcsm ([packed] in profile.toml): base.apk 807 MB (the
program, main/patch OBB, shared .mft) + split_episode_02..08 (by the episode
number in the path), every entry stored and 16 KiB aligned, mcsm.apks 5.59 GB
for SAI. Launcher is GoogleActivity now (manifest edit in smali.patch: the
expansion downloader only checked for an OBB file, its servers are dead).
Sandbox: uninstall, install the 8 APKs in one pm session, clean data: boot
log "packed files: 323 files from 8 apk(s), 0 deflated", the app's data dir
25 MB, the menu intro renders, markers 0.
Data check: every episode archive against the Telltale .mft (size + md5):
303/304 good; MCSM_android_JesseMale105_dlog.ttarch2 (episode 5 dialogue)
was 1156387 bytes, the .mft says 1324719, md5 74a31d7b...; replaced in the
owner's data tree by the copy inside phys-winner's split_episode_05 (matches
the .mft exactly); the bad one kept in work/*.bad-original. (His
split_episode_05 also carries all of episode 4: a packing slip of his.)
Sent to the phone: Minecraft_Story_Mode_arm64_2026-11-05.apks (lib 9aa65b17).
Not yet run: any episode from a split (2-8), on the sandbox or the phone.

## 2026-11-08: the phone hangs on "Подключение": OpenSL ES

Phone logcat (no root): all 8 APKs found, frames presented, no FMOD output
ever made. org.fmod.FMOD.supportsLowLatency() picks OpenSL ES when the
device reports android.hardware.audio.low_latency (the phone does, the
sandbox did not); the core let the guest dlopen libOpenSLES but served no
symbol, so the sound thread never answered and the engine waited forever.
Reproduced on the sandbox by adding the feature
(/system/etc/permissions/android.hardware.audio.low_latency.xml, kept:
the sandbox now matches phones here). Fix f892088: OpenSL ES served by the
host's libOpenSLES (opensl.cpp). Sandbox with the feature: past
"Подключение", sound plays through libOpenSLES, markers 0. 99 Bricks
(Unity, also links OpenSL) did not take the OpenSL path on this run; Rio
and 99 Bricks gates clean. Google Play Games sign-in fails on both (our
signature is not Telltale's): harmless, the game goes on.
Sent: Minecraft_Story_Mode_arm64_2026-11-08.apks (lib a0b50ad5).
