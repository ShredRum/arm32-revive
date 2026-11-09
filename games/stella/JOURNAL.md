# Angry Birds Stella 1.1.5 — journal

Original APK: `original/com.rovio.angrybirdsstella.1152.apk` (52 408 504 B),
signed by Rovio (signer sha256 3bf28170..., the same as the five live
titles): the Google Play original, not rebuilt.

## Layout (2026-11-09)

- Engine: `libPurple.so` (Rovio fusion; `Gen.library_name` "Purple", loaded
  by `App.onCreate`). JNI: 44 com.rovio.fusion + 39 com.rovio.skynest
  exports; 82 have a Java `native` declaration (`SocialSharingViewWrapper.
  onSharingViewStartSharing` is exported but never declared: no forwarder).
- `com.extrareality.SceneGraph` (Zappar scanner: ers_cpu_check,
  stlport_shared, scenegraph or scenegraph_neon by NEON) loads only when the
  scanner opens (`ZapparController` -> `ZapparEmbed`). Not served yet: the
  tree drops lib/armeabi-v7a. Opening the scanner will fail to load them.
- targetSdk 21 -> 24 in the profile (Android 14+ refuses below 24).

## 2026-11-09: first boots

- PC harness: init 165/167, then OpenSSL's `_armv7_tick`
  (`mrrc p15, 1, r0, r1, c14`, CNTVCT) was an undefined instruction. Fixed
  in dynarmic 0020 (generic timer readable from user mode, as under Linux):
  init 166/166 (entry 166 is the -1 terminator).
- Sandbox: `nativeInit` threw io::IOException for the missing
  files/highscores.lua (first run) and it reached terminate. Stella's
  unwinder defines its own __gnu_Unwind_Find_exidx, which calls bionic's
  `dl_unwind_find_exidx` (link.h, same contract); that import was not
  served. Now it shares the exidx handler: the exception is caught, the
  engine goes on, offline network errors are caught by the engine too.
- Then a host SIGSEGV inside libart on the UI thread. Fixed (below).

## 2026-11-09: the libart crash = a direct-buffer copy-back into freed memory

- death.log now keeps the interrupted thread's own frame chain, its
  registers and /proc/self/maps. libart's text is linked at +0x23000 from
  its file offset: the crash was mspace_malloc in ART's **non-moving
  space**, following a free-list pointer read out of high-entropy bytes
  (earlier symbolization without the bias pointed at DexFileVerifier: wrong).
- On Android 7 a direct ByteBuffer is a non-movable Java array in that
  space. During nativeInit the engine reads data through many short-lived
  direct buffers (GetDirectBufferAddress, then DeleteLocalRef). The host
  gave the guest a shadow copy and wrote it back only when nativeInit
  returned, seconds later: by then the collector had freed some of those
  buffers, so the copy-back wrote over freed heap. Now the shadow goes back
  when the guest releases the reference (still alive then), or at the end
  of the call, whichever comes first.
- CheckJNI (DIAG=1 debuggable twin) reported nothing: the bad write was a
  plain memcpy, not a JNI call.
- Result: boots to the island menu, 60 s alive, death.log empty. The five
  live titles pass the sandbox gate with this build.
- Not reproduced: one jitcallbacks=on DIAG log lacked the nested
  nativeMixData's ReleaseByteArrayElements line; the next run had it
  (lines from several threads interleave in apphost.log).
- Phone (owner, 2026-11-09): levels play, pause/resume works, sound on.

## 2026-11-09: the eye button (Zappar scanner) says "not available"

- Pressing the eye crashed: Java's System.loadLibrary("ers_cpu_check")
  (com.extrareality.SceneGraph) threw UnsatisfiedLinkError; the scanner's
  own ARM32 libraries cannot load in an arm64-only process.
- Not worth porting: after a scan its libraries fetch the code's content
  from pkginfo.zappar.com (403) and pkg.zprcdn1.com (no DNS any more); no
  codes to test with; the game gives no reward of its own for it.
- Script facts (work/KEYS.md: scripts are AES-256-CBC + 7z/LZMA; Lua 5.1
  bytecode, 4-byte floats): ZapparHandler.launchZappar mutes and restores
  audio only on iOS; on Android it calls native_launchZappar ->
  ZapparController.startComponent() directly. The Telepods page is a
  separate QR scanner whose codes are checked by a Rovio redeem service
  (redeemCode -> "Validation Service unavailable" offline): 30 outfits in
  telepod_configuration.dat, also unreachable in the original today.
- smali.patch: startComponent() shows a toast ("Сканер недоступен в этой
  версии" / "The scanner is not available in this version", by the system
  language) on the UI thread instead of starting Zappar. Sandbox: the toast
  shows, the game stays in the menu.
- Next: the Telepods page (does it open and fail cleanly offline?).
