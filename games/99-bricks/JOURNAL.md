# 99 Bricks Wizard Academy — bring-up journal

Branch `bricks`. Original APK: `games/99-bricks/original/99_Bricks_base.apk`
(gitignored), package `nl.weirdbeard.brickswizardacademy` 2.5.0 (versionCode 18),
sha256 7bb96a2b0f6f3dc45556199ef35d60ba222fa17f6eaf427ee46e2ff2f55019b0.

## What the title is

- Unity 4.x (strings: 4.7.2f1), scripts in C# on Mono (Boehm GC).
- armeabi-v7a libs: `libmain.so` (Unity loader), `libunity.so` (engine, needs
  libmono + libmain + libEGL/GLESv2/android/z), `libmono.so` (needs only libc,
  libm, libdl, liblog, libstdc++).
- Launcher: `com.prime31.UnityPlayerNativeActivity` (NativeActivity: input via
  AInputQueue/ALooper, own EGL context, sensors).
- `libmono.so` makes 363 raw `svc` syscalls and JIT-compiles C# to ARM at run time.

## Spike 1 (2026-10-27): Mono under our JIT on the PC — works except GC

Tool: `host/build-native/guestrun` (loads any guest .so, runs its inits, calls
exports; `@read32`/`@str` print results). Assemblies extracted to
`games/99-bricks/work/files/Managed` (+ `Managed/mono/2.0/mscorlib.dll`).

```
guestrun games/99-bricks/work/lib/libmono.so --files games/99-bricks/work/files \
  --pkg nl.weirdbeard.brickswizardacademy \
  --call mono_set_dirs s:$M s:$M --call mono_set_assemblies_path s:$M \
  --call mono_jit_init_version "s:Unity Root Domain" s:v2.0.50727 ...
```

Result: `mono_jit_init_version` returns a domain; `Environment.ProcessorCount`
and `"hello from 99 bricks".ToUpper()` run (C# JIT-compiled by Mono, translated by
Dynarmic): `"HELLO FROM 99 BRICKS"`.

Host changes it took (all generic, none imported by the Rovio titles except
getcwd): `__ARM_NR_cacheflush` -> Jit::InvalidateCacheRange, `RunJit` resumes a
Run stopped for cache invalidation; getcwd returns the guest spelling and checks
the buffer size (it leaked the host path and overflowed small buffers); real
readlink, clock_getres, gettid, getdtablesize, strlcpy, mprotect (adds access
only), pthread_attr_* + pthread_getattr_np (thread stack bounds),
__pthread_cleanup_push/pop, sem_*.

Blocker: `mono_gc_collect` hangs. Boehm stops the world with
`tkill(tid, 30)` and waits for the target thread's signal handler to ack; we do
not deliver guest signals (tkill is a 0 stub). Next: guest signal delivery
(pending set per guest thread, interrupt the target's Run or blocking host call,
run the guest handler on a guest signal frame, sigreturn, sigsuspend).

Still to come after that: several guest libraries linked to each other,
NativeActivity entry + input, guest-owned EGL, ~150 more libc/zlib imports.

## Plan: guest signals (for Boehm stop-the-world), decided 2026-10-27

What Mono/Boehm uses: sigaction, sigprocmask/pthread_sigmask, sigsuspend,
tkill/pthread_kill (suspend = SIGPWR 30, seen live; restart = SIGXCPU 24 expected),
sem_post/sem_wait for acks. NullReferenceException: avoid SIGSEGV delivery by
running Mono with MONO_DEBUG=explicit-null-checks (served by our getenv for this
title only), so no fault -> guest-handler path is needed at first.

Design (Rovio unaffected: everything stays off until a guest installs a handler):
1. Process table of guest dispositions (sigaction: handler, flags, mask); per guest
   thread: blocked mask, pending set, tid -> {Jit, host thread} registry.
2. tkill/pthread_kill/kill(self): mark pending on the target; if it runs guest code,
   HaltExecution(signal reason) on its Jit; if it waits in a host call, it notices
   on its next wait slice (see 4).
3. Delivery points: (a) RunJit sees the signal halt -> save regs/cpsr/ext regs,
   run the handler on the same Jit (pc = handler, lr = sigreturn trampoline =
   halt, r0 = sig, r1 = siginfo*, r2 = ucontext* on the guest stack), restore,
   continue; (b) inside a blocking import: run the handler on a per-thread
   "signal Jit" (shared code cache, cheap) seeded with the thread's r4-r11/sp so
   the handler's register spill puts live values on the stack for the GC scan;
   then restart the wait (SA_RESTART) or return EINTR.
4. Interruptible waits only while signals are in use: sem_wait, mutex lock, cond
   wait, nanosleep/usleep, poll/select/epoll_wait, pthread_join, blocking read ->
   wait in short slices (timed variants) and deliver pending signals between
   slices. sigsuspend(mask): set mask, wait for an unblocked pending signal,
   deliver it, restore mask, return -1/EINTR.
5. Test with guestrun: mono_gc_collect with the finalizer thread alive; then a C#
   allocation loop that triggers GC by itself.

## Guest signals implemented (2026-10-27)

Runtime: sigaction/sigprocmask/pthread_sigmask keep real dispositions and per-thread
masks; tkill/pthread_kill/kill mark a signal pending on the target and stop its Jit
(kSignalHalt); RunJit runs the handler; waits (sem, cond, mutex, sleeps) go in 2 ms
slices once any signal was sent, with handlers between slices; sigsuspend. The
handler runs on a callback Jit (RunNested) on the target's own guest stack with its
r4-r11, so the GC's register spill and stack scan see live values. Rovio titles
never send signals: their paths are unchanged (harness baselines identical).

Test (guestrun): two mono_gc_collect with the finalizer thread alive pass; a string
held by a GC handle survives both and still runs ToUpper -> "SURVIVES THE GC".

## Several guest libraries (2026-10-27)

ElfLoader::Load(path, mod, err, bias) adds a library to an existing module: its own
bias (span recorded), undefined symbols bind first to libraries loaded before
(dependencies first, the linker's search order), then to host imports sharing one
trampoline table; per-library exidx/RELRO (LoadedModule::more). Rovio = one library at
bias 0, unchanged. Also memalign/posix_memalign on the guest heap.

    cd games/99-bricks/work/lib
    guestrun libmono.so --lib libmain.so --lib libunity.so --base 0x1000000

-> libmono @01000000, libmain @02000000, libunity @03000000; all 236 inits run
clean. NULL stays unmapped (first library above 16 MiB).

## How Unity 4 starts, and what the host must provide (plan, 2026-10-27)

Java: UnityPlayerNativeActivity (extends NativeActivity, lib_name "main") loads
libmain. libmain's JNI_OnLoad RegisterNatives com/unity3d/player/NativeLoader
(load/unload). NativeLoader.load -> dlopen("libmono.so"), dlopen("libunity.so"),
libunity's JNI_OnLoad -> RegisterNatives for UnityPlayer. ANativeActivity_onCreate
(libmain) receives the window/input queue; Unity renders with its own EGL context.
FMOD audio: Java_org_fmod_* exports in libunity (plain exports, like Rovio's Java_*).

Host work, in order:
1. dlopen/dlsym/dlclose for guest libraries already loaded (handle per library,
   dlsym from mod.defined); dlsym of a host function needs a run-time import
   (new trampoline slot).
2. RegisterNatives (D-13): a pool of host stubs taking (env, obj, x2..x7, d0..d7);
   each registered method gets one, marshals its args per the JNI signature into
   the 32-bit guest call (AAPCS32), returns per the return type.
3. Host side of NativeActivity: our host is libmain for Java (APK lib name);
   ANativeActivity_onCreate builds a guest ANativeActivity + callbacks table, the
   host's real callbacks forward to the guest's (onStart/Resume/Pause/Stop/
   Destroy, onNativeWindowCreated/Destroyed, onInputQueueCreated/Destroyed,
   onWindowFocusChanged, onConfigurationChanged, onLowMemory).
4. Handles for ANativeWindow / AInputQueue / AInputEvent / ALooper /
   AConfiguration / ASensor*; forward A* calls to the real NDK functions.
5. EGL forwarding (eglGetDisplay..eglSwapBuffers) with guest handles for
   display/config/context/surface; RealGL then sees a current context on the
   guest's render thread.
6. Packaging: prepare-tree for a Unity title (our lib named libmain.so, the three
   guest libs under assets/guestlib, manifest unchanged); MONO_DEBUG served by
   getenv.

## Unity's native start-up chain runs (2026-10-27)

dlopen/dlsym for guest libraries already in the module (handle per library; dlsym
searches that library first, then the merged table; other names still NULL), JNI
GetJavaVM (fn 219). guestrun: `--call JNI_OnLoad @vm 0` (libmain) registers
NativeLoader.load/unload; `--call 0x0200334c @env 0 j:<libdir>` (NativeLoader.load)
dlopens libmono + libunity and runs libunity's JNI_OnLoad, which registers 25
UnityPlayer natives (initJni, nativeRender, nativeResize, nativePause/Resume,
nativeRecreateGfxState(Surface), nativeInjectEvent(InputEvent), UnitySendMessage...),
6 org/fmod/FMODAudioDevice natives and 2 ReflectionHelper natives -> returns true.

Next: the host side of RegisterNatives (real Java must call these guest functions),
then the NativeActivity/EGL/input bridge and packaging.

## RegisterNatives binds guest natives in the real JVM (2026-10-27)

Product host: RegisterNatives gives each guest entry a host stub (pool in apphost.cpp:
256 integer/reference-returning, 32 float, 32 double; one per class.method+sig+entry,
re-registering reuses it) and calls the real RegisterNatives on the real class. A stub
is what ART calls with fixed AAPCS64 arguments; it reads them through its own `...`
(exact on AArch64 Linux) and lays them out for the guest with the same marshaller as
the generated Java_* forwarders (AppendJniArgs: Z/C zero-, B/S sign-extended, J/D
aligned pairs, F bits), runs the guest on the thread's Jit, and returns per the return
type (softfp float/double bits, references through Runtime::JavaObjectFor). PC tools
only log. fmt_test: both read modes on one 24-word signature (the raw one only runs on
arm64: run fmt_test on the sandbox).

Behaviour change for Rovio too (D-13): a Rovio guest that calls RegisterNatives on the
device now really binds those natives (the real game does the same on ARM32). Gate all
five titles; apphost.log lists every registration as `[jni] RegisterNatives`.

Known gaps for Unity, to handle when they show up on the device:
- WrapObject makes a global ref per new incoming object and scans all of them; per-event
  objects (nativeInjectEvent's InputEvent) would grow it without bound.
- A native called by Java while the same thread is inside the guest (Java callback ->
  native) is dropped as REENTER.

Next: NativeActivity host bridge, window/input handles, EGL, packaging.

## Imports the three libraries miss (2026-10-27)

Unity's libs import ~200 names the host does not implement (they fall into the
`[stub] -> 0` path). Done so far: weak references the system does not export
resolve to 0 (`__data_start`, `data_start`, `__cxa_begin_cleanup`,
`__cxa_type_match`, `__cxa_call_unexpected`; NDK r27 stubs, API 21-25),
`__cxa_guard_*` (a 0 meant "already constructed"), `__cxa_pure_virtual`, atexit
family, `environ` (empty) and `_ZSt7nothrow` data objects. Mono GC test still
passes (mono_gc_collect x2, heap 128 KiB).

Still missing, by group: libc (ctype isalnum/isalpha/isspace/isxdigit/tolower,
usleep/sleep, readdir, lstat, chdir, dup2, isatty, setenv/unsetenv, strcasestr,
strsep, strxfrm, puts/putchar/perror/vfprintf/setvbuf/fscanf, div,
pthread_key_delete/exit/sched*, getuid/getpwuid, getrusage, madvise, mremap, flock,
truncate, prctl, select, epoll_*, sendfile, dlerror, wide-char), zlib
(inflate*/deflate*/crc32), GL (stencil, blend separate, framebuffer status,
mipmaps...), process/network refusals (fork/exec/popen/system, getaddrinfo...),
then NDK input/looper/configuration/sensors/window and EGL (device).

## GC and threads outside the guest (found and fixed 2026-10-27)

Test: `mono_thread_create(domain, mono_gc_collect, 0)` while the guestrun main
thread sleeps in the host (`@sleep`). The new thread's stop-the-world signals the
main thread (Boehm knows it: it initialised Mono), but a thread that is not running
guest code and not inside a guest wait cannot take a signal: the collection waited
until the main thread re-entered the guest 1.5 s later. On the device that is every
Java thread attached to Mono (UI thread, GL thread) between two native calls: a GC
from another thread would stall until each of them calls native again, or forever
if one waits in Java for that thread. Needed: deliver to a parked thread on its
behalf (a helper runs the handler as that tid, on its guest stack; the thread's next
entry into the guest waits until the handler is done, which keeps the world
stopped). Also: key destructors do not run when a Java-attached thread exits
(HostThreadRelease).

Fixed the same day: waits became interruptible only with the first signal sent, so
Mono's finalizer, already blocked in sem_wait, never acked (906d206).

Fix: proxy delivery. A thread outside guest code (in a host call, or back in the
host) is marked `outside`; a signal for it starts a proxy host thread that runs the
handler as that guest thread (tid, TLS, errno, mask) on its guest stack: below the
sp of its host call with its r4-r11, or from its stack top when parked. A per-thread
token (recursive mutex) is held by whoever runs a handler for the thread; the thread
takes it before executing guest code again, so it stays stopped while the handler
waits in sigsuspend for the restart signal. Only while signals are in use. Test:
the worker's collection now ends while the main thread still sleeps; 5/5 runs of six
thread+GC rounds pass.

## Rovio-visible changes to gate (branch bricks, 2026-10-27)

Harness baselines are identical after every commit (init only). What a Rovio title
can see at run time, to check on the sandbox (all five, apphost.log + screenshots):

- RegisterNatives really binds (D-13): `[jni] RegisterNatives` lines; Rio's dynamic
  natives now run guest code when Java calls them.
- setvbuf on the host std streams is accepted and ignored; strxfrm copies (4 titles).
- getpwuid returns a real passwd; pthread_getschedparam writes SCHED_OTHER/0;
  pthread_setschedparam refuses a real-time policy with EPERM (all five).
- closedir frees the guest DIR (leak fix); dlerror returns bionic's text; dlopen(NULL)
  returns a handle (all five import opendir/closedir/dl*).
- getenv: still NULL for every name; the `[env]` line now appears once per name.
- Thread-key destructors run when a worker ends (POSIX), pthread_exit works.
- Wide characters (mbrtowc/wcrtomb/...) answer as bionic instead of 0 (4 titles).
- `[signal] ... interruptible waits on` must NOT appear for a Rovio title (breakpad
  installs fault handlers only); CallSVC then stays on its old path.
- `[svc] ... is not offered` / `prctl(...) is not offered` lines, if any, name a call
  a Rovio title makes that now fails honestly instead of returning 0.

Sandbox gate 2026-10-27 (lib md5 c415b994..., after 6b807a7): all five titles boot to
their menus (SW2 amz shows its usual offline dialog), markers 0, death.log empty,
no `[signal]` / `not offered` / LOUD lines in any apphost.log. arm64 fmt_test: only
the known statfs f_namelen 256 failure; the AAPCS64 fixed-argument read passes.
Found on arm64 only: Boehm's GC_unmap (PROT_NONE MAP_FIXED over part of its heap) was
refused by the one-entry-per-mmap table and Mono aborted; fixed by the per-page arena
(6b807a7); Mono thread+GC stress 5/5 on arm64.
Still to gate by playing: Rio's dynamic natives (RegisterNatives) and worker exits.

## First device runs (2026-10-27)

APK: scripts/prepare-tree.sh 99-bricks && scripts/build-apk.sh 99-bricks; devgate works
(the original APK had to be uninstalled from the sandbox: different signature). Unity
starts: three libraries, Mono loads mscorlib, the EGL context goes current, nativeRender
runs. Fixed on the way: PushLocalFrame/PopLocalFrame, UnregisterNatives, IsInstanceOf,
FatalError, direct buffers (FMOD audio), all primitive-array element/region calls, a
jclass handed in by Java, and an old realgl bug: glUniform{1-4}{f,i}v read the value
pointer from r3 (only the Matrix variants have a transpose argument; Rovio uses only
those).
Seen: landscape instead of portrait and a black screen (Unity probably did not read its
player settings / data yet); a permissions dialog (targetSdk 24: Unity 4.7 asks).

JNI table audit (Call<T>Method/CallStatic<T>Method go to Java generically): still
missing ExceptionDescribe, AllocObject, CallNonvirtual* (64-93), Get/Set<T>Field for
primitives (96-112) and statics (152-162), NewString, GetStringChars/ReleaseStringChars,
NewObjectArray/Get/SetObjectArrayElement, New<T>Array (175-182), MonitorEnter/Exit,
ReleasePrimitiveArrayCritical, Get/ReleaseStringCritical, NewDirectByteBuffer,
GetObjectRefType. Next: implement them all at once (real-Java delegation), then look
at GL_EXTENSIONS vs eglGetProcAddress (Unity sees extensions we return NULL for).

## Open: a rare hang at the logo, inside ART (2026-10-27)

Seen a few times before the loading bar: the Java render thread (UnityMain, tid 5)
sits in JNI GetArrayLength, in ART's ConditionVariable wait (transition to Runnable,
i.e. a pending thread suspension that is never lifted); FMODAudioDevice waits the same
way in ReleasePrimitiveArray. No guest-attached thread is among ART's threads. Not yet
explained; traces in work/review/traces_bricks*.txt. (The other hang, Unity's
asyncProcessor queue, was dumped once with the queue empty and no waiter.)

## Milestone (2026-10-27): into a level on the sandbox

Unity splash, loading bar, quality dialog (touch works), title screen, a level.
Key fixes on the way: guest-signal proxy delivery (thread state always known,
outer snapshot restored after nested runs, proxies only for blocking host calls),
per-page mmap arena (Boehm's partial MAP_FIXED), real JNI everywhere (static fields,
NewObject for every class, reflection, FindClass classes as arguments, guest threads
attached to the JVM with the app class loader), nested Java -> native calls,
system-library dlopen + run-time import trampolines.

Open, in order:
1. Saves: Application.persistentDataPath arrives empty ("/save_premium.wb.tmp").
2. The rare hang at the logo (see above): next time take `debuggerd -b <pid>`.
3. Performance (8-30 fps at times): later.
4. Rovio gate of the core changes (real NewObject / static fields / attached
   worker threads / proxy rules) before anything merges to master.

## 2026-10-28: saves, the hangs, the stale-code asserts

- Saves: Unity puts persistentDataPath in the app's external dir when the
  storage permission is granted; the bridge did not map it, Unity kept an empty
  path. Now <storage>/Android/data/<pkg>/... lives in /data/data/<pkg>/ext
  (2574672). save_premium.wb is written.
- The hangs (logo, 25-30%, placing a block): a lost cond wake in our sliced
  waits (REVIEW R-008), fixed a53149d. The [threads] dump shows a cond waiter's
  cond, mutex and the wakes handed out before/since it slept.
- What remained looked the same (bar stuck at 25-30%) but was Mono asserting on
  its own metadata: stale translations of Mono-JITed code in the shared code
  cache (REVIEW R-007). host.jitshare = off for this title: 0 of 20 boots broken
  (was 4 of 10).
- sem_wait: the original targets 21, where bionic's sem_wait does not return
  EINTR; it no longer does here (a3c3402). Unity's "Failed to wait on a
  semaphore" is gone.
- scripts/swap-lib.sh used to leave the old host in this APK (libmain.so).

Open: no music (sound effects work); a short freeze when a tower level is
reached; R-007 itself (the shared cache with a guest JIT).
Rovio-visible changes added today: cond waits (a53149d), Jit registry and
cacheflush to all private Jits (13b053f), packaged target_sdk (Seasons targets
22: its sem_wait no longer returns EINTR) and jitshare (Rovio: on).

## 2026-10-29: touch lag, page-turn freezes, music

- Touch lag that grew with play time: every Java object reaching the guest
  stayed a global ref, found again by an O(n) IsSameObject scan under the
  runtime lock. Handles now have JNI lifetimes (25dbb2a); 32 wrapped objects
  stay 32.
- Lesson page turn ~650 -> ~450 ms on the sandbox: pthread_getspecific
  (~200k per turn) took the runtime lock and a map (52af6ae). The original
  APK on the same sandbox also freezes there, about half as long; the rest
  is guest code under Dynarmic (~280 ms, repeats every turn, no cacheflush,
  few slow-path accesses). HARNESS_PROF=1 breaks slow frames down.
- No music: FMOD's stream thread asked for an 8 KiB stack; we refused
  anything under 16 KiB (LP64 PTHREAD_STACK_MIN); FMOD_ERR_INTERNAL in
  createSound for every stream (ea6f723). Found via HARNESS_PCLOG and the
  error Unity keeps at audio manager +0x9c/+0xa0 (manager 3 = 0x03b939e8).
- /proc/cpuinfo and /proc/self/auxv are now the 32-bit task's view (edeec19).

Open (in order):
1. Rovio before merging bricks: boot gate of all five done 2026-11-01. Still
   to play by hand: JNI ref lifetimes, cond waits, glUniform*v, key
   destructors, Seasons sem_wait (target 22).
2. A one-time freeze after the first brick drops (none in the original).
   Page turn: see 2026-11-01.
3. REVIEW R-010 (Mono metadata assert, ~1 in 11 boots), R-007, R-009.
4. CallNonvirtual* JNI halts LOUD; UNLOCK-FOREIGN log noise.
Sandbox: SELinux left permissive (setenforce 1 restores).

## 2026-11-01: page turns twice as fast; a JNI string bug in Rovio found

Slow frame on a lesson page turn, `HARNESS_PROF=1`, same build, 18 turns each:

| `tlsthunk` | Frame | Host calls | Guest code + JIT |
|---|---|---|---|
| off | 435-495 ms | 162-187 ms | 273-308 ms |
| on | 175-235 ms | 51-87 ms | 120-151 ms |

`pthread_getspecific` (~200k calls a frame) no longer leaves guest code. The
profiler's own clock reads made the off column worse than it is unprofiled, but
the guest-code share fell too, so the gain is not only the 110 ms the host calls
showed. The owner saw the difference by eye.

The Rovio check found a regression of this branch, not of this work: since
4e99625 a real Java String reached the guest as a wrapped object, and
GetStringLength(Build.MANUFACTURER) halted Rio, Seasons, Space and Star Wars on
a black screen. The first fix copied every String into jni_strings_; review
(/code-review high) showed that broke String receivers, leaked, and missed
paths. Now wrapped Strings stay Java objects and the string functions accept
them; lengths and regions count UTF-16 units, also for jni_strings_ text.

Same review: the TLS area had taken the PtrHandle range, which must stay
unmapped (moved to 0xF8000000); a thread past the last TLS block now halts;
the statfs test no longer fails on the sandbox's filesystem, so `fmt_test` is
ALL OK on arm64.

Left as it was: a String-typed method result is still a jni_strings_ copy,
kept for the life of the process and not a Java receiver.

The Java copy of a guest string made for one JNI call (a method argument,
IsInstanceOf, a field, an array element, Monitor*) is now deleted when the call
ends (TempRefs): an attached native thread has no frame to drop it. Monitor* on
such a copy still locks a temporary object, so Enter and Exit do not pair.

jni_strings_ is never freed, so its growth was measured with the new
`[strings]` line, on the sandbox with the owner playing about 5 minutes each:
99 Bricks 163 -> 196 strings (4.6 KB, 15 from Java method results), Seasons 35
-> 499 strings (5 KB, 19 from Java method results). Neither grows with play
time beyond a few KB, so the copies stay: wrapping them would touch the most
used string path of the Rovio titles for no measurable gain.

