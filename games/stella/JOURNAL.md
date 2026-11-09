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
- OPEN: then a host SIGSEGV inside libart (death.log: hpc in libart text,
  CheckLoadStringByTypeIdx / ErrorStringPrintf region per its .symtab, fault
  address looks like UTF-16 text), guest at the halt trampoline (a guest
  call had returned). The last JNI calls before it: GetByteArrayElements /
  ReleaseByteArrayElements of an 8192-byte array on a second thread
  (likely AudioOutput.nativeMixData, nested inside nativeInit). Suspect
  memory corruption earlier, not that call itself. Next: a run with
  HARNESS_DIAG and the JNI ref bookkeeping (local frames, wrapped objects)
  around nativeMixData; compare with Seasons, which also nests it and works.
