# Angry Birds Rio 1.0.0 (2011) — journal

Original APK: `/home/shredrum/Рабочий стол/Angry_Birds_Rio_v1.0.0.apk`
(13 410 771 B, sha256 `d29d8b7b4fb553e414ee6b6852bb071e2efef7ea7f7220e8832a76f2bc88b77d`),
signed by Rovio (signer sha256 3bf28170..., identical to other Rovio titles).
TargetSdk 8, minSdk 4, package `com.rovio.angrybirdsrio`.

## 2026-11-10: Initial Layout & Architecture Investigation

- **Framework**: `com.rovio.ka3d` (not `com.rovio.fusion`).
  - Activity: `com.rovio.ka3d.App`.
  - Library loading: `App.onCreate` reads `R.string.library_name` ("angrybirds") and calls `System.loadLibrary("angrybirds")`. Setting `host.lib = "libangrybirds.so"` and `smali_lib_name_orig = ""` allows our host to be loaded directly with zero smali modifications.
- **Rendering & Context**:
  - `com.rovio.ka3d.MySurfaceView` extends `com.rovio.ka3d.GLSurfaceView`.
  - `setEGLContextClientVersion` is never invoked, defaulting `mEGLContextClientVersion` to 0.
  - In `DefaultContextFactory`, this produces a NULL attribute list for `eglCreateContext`, creating an **OpenGL ES 1.1** context.
  - `libangrybirds.so` imports NO `egl*` functions; it runs inside the context created and made current by Java's `GLThread`.
  - The guest uses OpenGL ES 1.1 fixed-function calls (~45 imports: matrix stacks, client vertex arrays `gl*Pointer`, lighting, material, texenv).
- **Asset Loading**:
  - `libangrybirds.so` does not open the APK directly. In `io::FileInputStream`, it calls `MyRenderer.readFile(String)` via JNI with signature `(Ljava/lang/String;)[B`.
  - Java handles opening assets (`.zip` compressed via `ZipInputStream` or raw stream) from `AssetManager`.
- **Audio Output**:
  - `com.rovio.ka3d.AudioOutput` uses `AudioTrack` and receives PCM audio data from the engine via JNI export `Java_com_rovio_ka3d_AudioOutput_nativeMixData(J[BI)V`.
- **Native JNI exports (14)**:
  - `Java_com_rovio_ka3d_MyRenderer_native{Init,Deinit,Update,Pause,Resume,Resize,Input,KeyInput}`
  - `Java_com_rovio_ka3d_AudioOutput_nativeMixData`
  - `Java_com_rovio_ka3d_WebViewWrapper_{urlLoadedCallback,linkClickedCallback}`
  - `Java_com_rovio_angrybirds_AdViewWrapper_{hidingFinishedCallback,requestAdCallback}`
  - `Java_com_rovio_angrybirds_InterstitialAdWrapper_setVideoReady`

## 2026-11-10: Port Completion & Review Fixes

- Unwinder `.ARM.exidx`: registered via `PT_ARM_EXIDX` program header; handled GCC 4.4 `sh_entsize == 0`.
- Fixed-function OpenGL ES 1.1 forwarding via `libGLESv1_CM.so` and OES framebuffer extension support.
- Verified interactive gameplay on sandbox: menu navigation, level 1-1, audio playback, physics simulation, slingshot input and collision scoring (17 670 pts).
- Review fixes addressed:
  - Guarded client array bounds validation in `glDrawElements` / `glDrawArrays` behind `HasClientArrays()`, eliminating draw-call overhead on GLES 2.0+ titles.
  - Enforced `LOUD` halts on any unimplemented or missing GLES 1.1 entry point (no quiet skips).
  - Added explicit detection and `LOUD` halt for mixed IBO + client vertex arrays.
  - Resolved OES framebuffer functions directly from GLES 1.1 when available.
  - Added `CheckRange` validation on `__srget` memory writes.
  - Restored original segment protections after applying relocations in `elf_loader.cpp`.
  - Scoped `devgate.sh` reinstall fallback strictly to `INSTALL_FAILED_VERSION_DOWNGRADE`.
  - Regression gate passed across all titles (`fmt_test` ALL OK, `harness-all.sh` LOUD=0, `devgate` clean).

## 2026-11-11: AdMob Neutralization for Android 14+ / TargetSdk 26

- **Issue**: On modern Android (Android 14+, targetSdk >= 24), the 2011-era AdMob SDK (`com.admob.android.ads`) throws `SecurityException: MODE_WORLD_WRITEABLE no longer supported` in `com.admob.android.ads.j.a` during `getSharedPreferences("admob_prefs", 2)`. This caused the game to crash ~6 seconds after launch when `RequestAdRunnable` ran on the UI thread.
- **Fix**: Added `games/rio-1.0.0/smali.patch` to neutralize dead AdMob requests in `InterstitialAdWrapper`, `AdViewWrapper`, and their Runnables:
  - `InterstitialAdWrapper.requestVideoAd` and `requestAndShowVideo`: immediately post `OnFailedToReceiveInterstitialRunnable` to the GL thread (invoking `setVideoReady(handle, false)`).
  - `InterstitialAdWrapper.showVideoAd`: immediately posts `OnFailedToReceiveInterstitialRunnable` to the GL thread.
  - `AdViewWrapper.requestAd`: immediately posts `RequestAdResultRunnable(false)` to the GL thread (invoking `requestAdCallback(handle, false)`).
  - `AdViewWrapper.hide`: immediately posts `HidingFinishedRunnable` to the GL thread (invoking `hidingFinishedCallback(handle)`).
  - `AdViewWrapper.show`: converted to no-op.
  - Runnables (`RequestAdRunnable`, `MyRequestAdRunnable`, `ShowAdRunnable`): dead AdMob invocations replaced with immediate GL thread callbacks.
  - `WebViewWrapper$8.run`: on Android 4.4+, Chromium WebView forbids invoking WebView methods from non-UI threads (`RuntimeException: A WebView method was called on thread 'GLThread'`). Replaced `mWebView.getTitle()` on the GL thread with an empty string `""`, preventing GLThread termination and static screen freeze.
- **Verification**:
  - `prepare-tree.sh rio-1.0.0 --force` cleanly applies `smali.patch` with zero rejects.
  - Sandbox devgate passed clean: 0 halts, 0 LOUD markers, 0 crashes (`death=0`).
  - Phone build verified and signed.
  - Real device verified (Samsung Galaxy A56 5G, Android 15, arm64-only): boots clean, main menu, level selection, full level gameplay, slingshot, physics, audio and collisions confirmed working without crashes or freezes.

## 2026-11-15: code review of the branch (Claude) and what was done with it

`/code-review medium` over `master...rio-1.0.0`, ten findings; fixed:

- **GL client arrays after a context change.** The tracking was never reset,
  so after pause/resume it described arrays the new context does not have.
  It is now re-read from the driver (`SyncClientArrays`) after every
  `ForgetGlBindings`. `glClientActiveTexture` with a unit the host does not
  track is read back the same way instead of being taken for unit 0.
- **Text relocations.** One helper (`WriteDataSlot`) instead of three copies;
  the RW window covers the pages the word touches (a word across a page
  boundary crashed the host) and is opened only for slots in a read-only LOAD,
  so the other titles do no extra `mprotect` at boot.
- **Inline stdio.** `__srget` returned EOF without the `__SEOF` bit that the
  guest's inline `feof` reads; the FILE's two indicator bits now follow the
  host stream after every call that can change them. `tmpnam` no longer cuts
  the name at 64 bytes.
- **One package, two titles.** `rio` and `rio-1.0.0` had the same package,
  so the gate installed one over the other's saves. The port now installs
  this title as `com.rovio.angrybirdsrio100`, labelled "Angry Birds Rio 1.0"
  (profile keys `apk.original_package`, `apk.label`; `build-apk.sh` changes
  the manifest's package attribute only and refuses a manifest that uses the
  name anywhere else). Neither the engine nor the Java side reads the package
  name: the files directory comes from `getFilesDir()`. `devgate.sh` is back
  to what master has.
- **`smali.patch`.** The three Runnables the patched wrappers no longer create
  are left as they were (about 200 lines less to keep applying). The web
  view's title is no longer replaced by "": `WebViewWrapper$8` reads it in its
  constructor, which runs on the UI thread, and `run()` passes it on.

Left as it is, on purpose: `glDrawElements` with an element buffer bound and
vertex arrays in guest memory halts LOUD (`MIXEDIBO`). The combination is
legal, but checking the vertex range needs the buffer's contents; the game
did not use it in play. If the line ever appears, track the element buffer's
data at `glBufferData`. The per-draw scan of client index arrays stays too
(this title only).

Verified on PC only: builds for PC, sandbox and phone; `fmt_test` ALL OK with
new tests for `__srget` and `tmpnam`; harness of eight guests, LOUD 0;
`prepare-tree.sh --force` applies the patch; `build-apk.sh` assembles it.
**Not yet run on a device after these fixes**: the GLES 1.1 path, the new
`WebViewWrapper$8` (ART verifies it when the first page loads) and the gate
of the other titles.

The phone APK the owner tested earlier was a renamed clone made by hand
outside git (`work/apk_clone`). The build now does the same rename itself,
so that APK is reproducible.

## 2026-11-15: second review, sandbox gate and speed

A second `/code-review medium` over the fixes (`88b5f03..rio-1.0.0`): ten
findings, nine fixed (commits `ce0bac7`..`e8c8a99`); the tenth was about the
earlier commits bundling several changes, which are already history.

Sandbox, build marker `Nov 15 2026 16:26`, then the final build: this title
boots to its menu as `com.rovio.angrybirdsrio100`; after HOME and a return
the same process runs on, LOUD 0, `death.log` empty (both builds). The eight
other titles reach their menus, all markers 0, no tombstones (first build).

Speed, SW2 main menu, master vs branch, interleaved, other titles stopped
(`speed.sh` did not stop them before `e8c8a99`, and a first run was spoiled
by 99 Bricks left running): four rounds, master 9.85 / 9.76 / 10.05 / 10.69,
branch 9.40 / 9.86 / 10.18 / 10.62 ms CPU per frame. Both rise with the
device's heat; per pair no difference that four rounds resolve (±0.1 ms).
Stopped after four rounds on purpose.

Not yet: the owner's play on the phone with this build (levels, saves,
pause/resume).
