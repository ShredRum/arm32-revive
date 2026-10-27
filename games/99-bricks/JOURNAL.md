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
