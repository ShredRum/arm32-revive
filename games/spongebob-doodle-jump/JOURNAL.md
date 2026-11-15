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

## 2026-11-16: the boot hang on the Nickelodeon logo (1 boot in 4-5)

Symptom: endless `[glog] Inflate Error: ... (file zip crc32 : cff0e6ed)` =
`assets/bin/Data/sharedassets0.assets.resS` (9 MB deflate inside the APK).

Traced with temporary logs (removed): Unity reads that entry with a
read-ahead thread (t4: fread of 0x2000 chunks into two alternating buffers
on one FILE) and inflates on t24. After ~525 good chunks t24 suddenly did
fseek(entry + 0x2000) + fread itself into the buffer t4 had just filled: its
fallback for "the background read is late", with a stale position, so
inflate got chunk 1 where chunk 526 belonged.

Cause in the host: a guest signal (Mono/Boehm's GC suspend, SIGPWR 30,
installed with SA_RESTART) arriving during a sliced wait always ended the
wait after the handler (pthread_cond_[timed]wait returned 0, sem_timedwait
EINTR). On a device the kernel restarts a futex wait after an SA_RESTART
handler, so Unity's timed wait for the read-ahead never woke early. Fix:
waits go on after SA_RESTART handlers (cond: unless a wake came meanwhile);
a handler without SA_RESTART still interrupts them as before. Heat and the
logging made the hang more frequent: slower threads, wider window.

Status at writing: PC fmt_test ALL OK; a 16-boot sandbox batch running.
Also to do: 99 Bricks boot batch (same Mono; its old rare early death may be
the same family); then a calm run to check the level load (the owner saw
"loading" without end between batch runs, likely just the restarts).
