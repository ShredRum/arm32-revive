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
