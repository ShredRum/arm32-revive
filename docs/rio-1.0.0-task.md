# Task: port Angry Birds Rio 1.0.0 (2011) as a new title

You are working in the ab-port repository: original 32-bit ARM game engines run
unmodified under a Dynarmic JIT inside our arm64 host library `libabhost.so`.
Read `AGENTS.md` first (rules, build, devices), then `games/classic/JOURNAL.md`
and commit `cf8d809` as the example of adding a title. The owner does not write
C++ and speaks Russian: answer him in Russian, in plain words, and say exactly
what you verified and what you did not.

## Goal

`/home/shredrum/Рабочий стол/Angry_Birds_Rio_v1.0.0.apk` boots to its main menu,
a level can be played, and sound works, on the sandbox (Android 7.1) and then on
an arm64-only phone (Android 15). The eight existing titles must behave exactly
as before.

Work on a new branch `rio-1.0.0` created from `master`. Do not merge it and do
not publish anything: the owner will have the branch reviewed first.

## What is already known (from aapt2 and llvm-readelf; verify before relying on it)

- Package `com.rovio.angrybirdsrio`, versionCode 100, minSdk 4, targetSdk 8.
  It is the same package as the existing title `rio` (versionCode 2676), so the
  two cannot be installed together and installing this one over `rio` is a
  downgrade. Give the new title its own id (for example `rio-1.0.0`); do not
  rename the package.
- Guest: `lib/armeabi-v7a/libangrybirds.so`, 2017205 bytes, ARMv7 + VFPv2.
  NEEDED: libc, libm, libGLESv1_CM, libz, liblog. 183 undefined symbols.
- This is the old `com.rovio.ka3d` framework, not `com.rovio.fusion`:
  launch activity `com.rovio.ka3d.App`; exports
  `Java_com_rovio_ka3d_MyRenderer_native{Init,Deinit,Update,Pause,Resume,Resize,Input,KeyInput}`,
  `Java_com_rovio_ka3d_AudioOutput_nativeMixData`,
  `Java_com_rovio_ka3d_WebViewWrapper_*`, `Java_com_rovio_angrybirds_AdViewWrapper_*`,
  `Java_com_rovio_angrybirds_InterstitialAdWrapper_setVideoReady`.
- It draws with OpenGL ES 1.1 fixed function: glMatrixMode, glLoadMatrixf,
  glVertexPointer / glColorPointer / glTexCoordPointer / glNormalPointer,
  glEnableClientState, glClientActiveTexture, glTexEnvi, glLightfv,
  glMaterialfv, glColor4f, and the `*OES` framebuffer / renderbuffer calls.
  The host forwards only GLES2 today (`host/src/guest/realgl.cpp`).
- Imports the host does not have besides GL: `__srget`, `tmpnam`
  (and the data symbol `__dso_handle`: check how the loader treats it).

## The work, in the order I would do it

1. Decode the APK's smali and find out, from the code and not from guesses:
   who creates the EGL context and with which version (a Java `GLSurfaceView`
   is likely, in which case the host only forwards calls to the context that is
   already current); where `System.loadLibrary("angrybirds")` is; how the engine
   reads its assets (the `.zip` files under `assets/data` and libz suggest it
   opens the APK itself with a path given by Java); what
   `AudioOutput.nativeMixData` receives.
2. Profile and tree: `games/<id>/profile.toml`, `JOURNAL.md`, the loaded-library
   edit in `scripts/prepare-tree.sh` terms, JNI forwarders through
   `host/gen_apphost.py`. Reuse the scripts; do not fork them per title.
3. A GLES 1.1 forwarding layer next to the GLES2 one: load `libGLESv1_CM.so`
   and forward the roughly 45 functions the guest imports. The hard part is the
   client arrays: `gl*Pointer` hands the driver a guest pointer that is read
   later, at `glDrawArrays` / `glDrawElements`. Translate it to a host address,
   and check the range at draw time, when count and stride are known, for every
   enabled array (a pointer is an offset when a buffer object is bound).
   Matrices, lights and materials read a fixed number of floats: check those at
   the call.
4. The two libc imports, implemented for real.
5. PC: `scripts/build-lib.sh --native`, `./host/build-native/fmt_test`, a
   harness run of the new guest; add its expected line to `scripts/harness-all.sh`
   and `REVIEW.md` the way the other titles have it.
6. Sandbox: boot gate of the new title with a screenshot, then play a level.
   Then the regression gate on the others (below).

## Rules that are easy to break here

- Never modify the guest `.so` or the original APK. Its sha256 goes into the
  profile and is checked at packaging.
- A wild guest pointer must become a guest halt with a log line containing
  `LOUD`, never a host crash and never a silent skip. Every invented answer
  also logs `LOUD`. The device gate greps for it.
- On Android, never answer with PC-harness inventions (screen size, API level,
  device id): the real Java method answers, or the call fails with `LOUD`.
- Verify against primary sources: NDK headers (`GLES/gl.h`, `jni.h`), the GLES
  1.1 specification, guest disassembly with `llvm-objdump`. Not against
  comments or docs in this repository.
- Other people's ports (MrEmpathy, phys-winner, ZettaBridge) may be read for
  ideas only. Do not copy their code: some of it has no license.
- targetSdk 8: Android 14 and later refuse to install it from a file. Do not
  decide this yourself. For testing use `adb install --bypass-low-target-sdk-block`;
  then tell the owner the options (keep the manifest and install through adb,
  or raise targetSdk and what that changes) and let him choose.
- Sandbox: `ssh -p 8022 u0_a41@192.168.1.202`, `sudo` is root. It is off
  unless the owner says it is on: ask him. **Never write to `/storage/emulated`
  (`/sdcard`) or `/data/media` there, and never run a recursive command that
  could reach them**: it is the real storage of the owner's phone. Delete only
  by literal path. Never leave `abflags.txt` on a device. One device session at
  a time.
- One logical change per commit, each with a test where one is possible.
  Commits are allowed; pushing is not.

## Done means

- `fmt_test` prints ALL OK; `scripts/harness-all.sh` shows the same numbers as
  before for the seven existing guests, LOUD 0, plus the new guest's line.
- Sandbox: the new title reaches the menu, a level plays, sound is heard or
  shown to be produced; all seven markers (HEAPCORRUPT, HEAPASSERT, CHUNKD,
  USAGE_ERROR, LOUD, STUCK, FATAL) are 0 and `death.log` is empty. Look at the
  screenshot: a black screen or a dialog raises no marker.
- Regression: `scripts/devgate.sh` for all eight existing titles (rio, seasons,
  space, star-wars, star-wars-ii-amz, stella, classic, 99-bricks) still reach
  their menus with markers 0, since the GL and import tables are shared.
  Installing the old `rio` back after the new title needs an uninstall or a
  downgrade install; say which you did.
- `games/<id>/JOURNAL.md` records what was found, what was done and why, and
  anything left open.

## If it does not go as planned

Stop and report rather than work around. In particular: if the driver has no
usable ES 1.1 context on the phone, or the engine needs something from Java
that Android 15 no longer provides, describe the evidence and the options; do
not add a fallback that hides it. Report at the end, in Russian: what works,
what was verified on which device, what was not verified, and every place where
you were unsure.
