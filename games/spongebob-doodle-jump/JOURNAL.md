# Doodle Jump SpongeBob SquarePants 1.02 — journal

Original APK `original/com.mtvn.SpongeBobDoodleJump-1.02.apk` (49 886 443 B),
signed by MTV Networks (an earlier download was a test-key rebuild with the
same three engine libraries byte for byte, a different classes.dex).
Package `com.mtvn.SpongeBobDoodleJump`, versionCode 25, minSdk 10,
targetSdk 19. Unity 4.3.3f1 (Mono + libunity + libmain), GLES2, as 99 Bricks
(Unity 4.7.2). Launcher `com.upsight.unity.UnityPlayerNativeActivity`.

## 2026-11-16: added

- Profile after 99 Bricks: three guest libraries, jitshare off, the host as
  libmain.so. Six FMOD JNI exports; `gen_apphost.py` missed the
  package-private `.method native fmodProcessMicData` (no flag before
  `native`) and stopped: the pattern now allows no flags.
- First boot: the menu, but `[jni] LOUD GetArrayLength unknown array 00000000`
  in `ReflectionHelper.nativeProxyInvoke`. ART passes args == null to a proxy
  method without parameters (Dalvik passed an empty array); Unity 4.3 calls
  GetArrayLength on it unconditionally (libunity +0x3d3bd4), and ART aborts
  on that. `smali.patch`: the proxy's invoke() gives an empty Object[].
- Then a NULL read in Mono-generated code (`[mem] LOUD BAD read @ 0000005c`,
  pc in the JIT area): Mono makes a managed NullReferenceException from
  SIGSEGV, and the host does not deliver faults to guest handlers. The host
  now sets `MONO_DEBUG=explicit-null-checks` for every title that loads
  libmono.so (99 Bricks too): Mono tests for NULL itself and throws the same
  exception without a signal.
- Sandbox: boots to its menu, LOUD 0; 99 Bricks too with the Mono setting.
- Imports the host lacks, not called so far: `atexit`, `__div0`, `tgkill`, and
  three skinning helpers `_s_SkinVertices4Bones_*_VFP` that no library in the
  APK defines (they will say LOUD if ever called).

Not yet: a round played, sound, the phone.
