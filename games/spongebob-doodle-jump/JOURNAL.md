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

## 2026-11-16 (later): the signal theory was wrong; the real race is Unity's

The batch after 1971ba5 still hung (2 of 12, 1 of 16 boots), and later
traces showed no signal near the failure at all. 1971ba5 stays (an untimed
futex wait is restarted after an SA_RESTART handler, so it is closer to a
device), but it did not fix this.

What happens (timestamped traces of t4/t24 freads, locks, cond ops and
zlib calls; all removed): Unity 4.3's async file reader is one thread (t4)
serving a queue of NativeFile requests for every stream, with ONE shared
"done" cond. `WaitAsync` (libunity 0x396a70, NativeFile vtable +0x24) waits
on it once, with no predicate loop, and returns the file's result (-1 if
its read is not done). When another stream's request completes first, the
waiting inflate thread wakes with -1 and its zip stream (Read at 0x38e3a0)
falls back to a synchronous read at `base + r4[0x10]`, the offset of its last
synchronous read, which async reads never advance: it re-reads chunk 1 in
the middle of the entry and inflate fails ("invalid stored block lengths"
etc.), forever.

So it is a race in the guest; the host keeps POSIX semantics. It shows here
because waits are long: t4 waited ~0.6-0.8 ms for the queue mutex before it
could signal. An adaptive (spinning) guest mutex was tried and did not
change the rate measurably (1 of 16 vs 2 of 12), so it was not kept. Not
known yet how often a phone hits it.

## 2026-11-16: endless level loading — fixed (00c8cfc)

Pressing "Play" showed "loading" forever. BhvrMoviePlayer writes the intro
movie to `Application.temporaryCachePath + "/DJSB_Intro.mp4"`; the path was
"" ("/DJSB_Intro.mp4", IsolatedStorageException), the opening coroutine
threw and never called back. Java's getCacheDir answered
/data/user/0/<pkg>/cache, but Unity lstat's every directory above it first
and the host knew neither the package root nor /data/user/0, /data/user,
/data. They exist now (an empty stand-in above the package root).

## 2026-11-16: tilt and the logo hang with sensors — fixed (f0bcd0b)

The host offered no sensors, so tilt did nothing. With the device's sensors
forwarded, Unity's sensor queue (created WITH a looper callback) stayed
ready forever: the host had dropped the callback and registered the queue by
ident, which Unity's main loop ignores, so ALooper_pollAll(0) never timed out
and no frame was drawn. A guest looper callback now runs through a host one
(RunNested). The owner confirmed tilt works.

## 2026-11-16: micro-freezes in play — measured, not fixed yet

The owner feels micro-freezes on the phone and the sandbox (60 fps on
average). [frames] (e893cb3) over a minute of play: 1-7 frames over 33 ms per
10 s; GC stops every 10-20 s, 14-24 ms. HARNESS_PROF with a 35 ms threshold
(that run only): most slow frames (40-420 ms) are Mono's JIT compiling
methods in play: per frame up to 25k strcmp, 22k memset, 19k mutex
lock/unlock host calls and hundreds of cacheflushes; a host call costs
~0.6-1.1 us (natively ~20-50 ns). The rest are GC stops (tkill, sem_wait up
to 16 ms waiting for the suspend handshake).

Plan (core work, all titles gain): run the hot libc imports (memset, memcpy,
strcmp, strlen, an uncontended mutex) in guest code without a host call, as
tlsthunk does for pthread_getspecific; then a faster GC suspend handshake.
Also open: the logo hang (Unity's async-read race) hit 2 boots of 3 on the
phone; a phone log of a hung boot is still wanted. On the phone the game's
start also closed Termux and Telegram (memory pressure?), not checked.
