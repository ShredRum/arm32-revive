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
