# AGENTS.md — ab-port: classic Angry Birds (ARM32) on arm64-only Android

Short, current-truth guide. Rewritten 2026-10-25. The old 3000-line investigation
journal moved to `docs/history/AGENTS-journal.md`: it is history written by
small-context models and contains stale and wrong claims. Use it only to learn
*why* something was tried; never cite it as evidence. When a doc and the running
build disagree, the build wins.

Active work: JIT dispatch, `docs/jit-dispatch.md`. The capital review — **read `REVIEW.md` first** (findings register,
regression gate, area status).

## 1. What this is

New phones cannot run 32-bit ARM apps. The port keeps each game's original
`armeabi-v7a` engine library **untouched** and runs it inside our arm64 host
library `libabhost.so`:

- `host/src/guest/elf_loader.*` maps the guest ELF into a 4 GB guest address space
  and applies its relocations; every import gets a trampoline (`svc`).
- Dynarmic (`thirdparty/dynarmic`, A32 frontend, arm64 backend) JIT-translates the
  guest code; fastmem is on.
- `host/src/guest/dispatch.cpp` implements every guest import: libc, pthread,
  files/stdio, sockets, time, AAsset, GLES2 and GLES 1.1 (forwarded to the real
  driver via `realgl.cpp`), and a 32-bit JNI function table that delegates to the real JVM
  where needed. Guest heap = dlmalloc mspace (1 GB virtual).
- `host/src/apphost.cpp` = `JNI_OnLoad`, per-thread JIT/stack, boot sequence, and
  the generated `Java_*` forwarders (`apphost_gen.inc`, from `host/gen_apphost.py`).
- The Java side of the original APK is unchanged except the loaded library name,
  and a title's `games/<id>/smali.patch` when it has one (applied by
  `prepare-tree.sh`; MCSM: the launcher activity, the expansion downloader
  only checked for an OBB file whose servers are dead; Stella: the Zappar
  scanner button shows "not available" instead of loading ARM32 libraries;
  Rio 1.0.0: the 2011 AdMob SDK is answered "no ad" because it crashes on
  Android 7+, and the web view's title is read on the UI thread).

Titles (`games/<id>/`, each with `profile.toml` + `JOURNAL.md`): rio, seasons,
space, star-wars, star-wars-ii, star-wars-ii-amz, stella, classic, 99-bricks,
mcsm, rio-1.0.0, seasons-3.1.1, star-wars-ii-2.1. One shared host core. Plain `star-wars-ii` is retired (its
content servers are dead, R-004): not built, not gated; the folder stays.
"All titles" below means the eleven the sandbox gate runs: rio, seasons, space,
star-wars, star-wars-ii-amz, stella, classic, 99-bricks, rio-1.0.0,
seasons-3.1.1, star-wars-ii-2.1 (MCSM is a
5.6 GB split set, `scripts/pack_apks.py`; see its journal).

`rio-1.0.0` is the 2011 release of Rio: the old `com.rovio.ka3d` Java
framework and OpenGL ES 1.1 (the GLES 1.1 part of `realgl.cpp`). Its original
package is the same as `rio`'s, so the port installs it under its own name,
`com.rovio.angrybirdsrio100` (`apk.original_package` and `apk.label` in the
profile; `build-apk.sh` rewrites the manifest's package attribute), and the
two live side by side.

## 2. Layout

- `host/` — CMake project. `src/guest/` = the core; `src/fmt_test.cpp` = unit
  tests of thunks; `src/harness.cpp` = PC runner of a guest `.so` (init only).
- `games/<id>/profile.toml` — package, versions, guest lib name + sha256,
  `host.network` = `offline` (default) or `online`: whether the engine may
  use the network (R-005; packaged into the APK by `build-apk.sh`, read at
  boot, logged as `network:`; `swap-lib.sh` keeps the packaged value).
  `games/<id>/original/` (gitignored) = original Rovio APK, read-only.
  `games/<id>/seeds/` = first-launch files. `games/<id>/work/` (gitignored) =
  decoded tree `apk_prod/`, `apk_signed.apk`, analysis leftovers.
- `scripts/` — `build-lib.sh [--native] [--diag]`, `build-apk.sh <id>`,
  `deploy.sh <id>` (both + prints device commands; never touches the device),
  `prepare-tree.sh <id>` (decode + port edits), `profile.py <id> <key>`.
- `tools/` (gitignored) — Android SDK, NDK r27 (27.2.12479018), build-tools 35,
  apktool. `thirdparty/` (gitignored) — dynarmic, boost headers. Our dynarmic
  changes (shared code cache) live in `patches/dynarmic/` with the base commit.
- `keys/` (gitignored) — the signing keystore installed devices trust. Keep a backup.

## 3. Build and verify

```
scripts/build-lib.sh --native          # PC: harness, fmt_test, other tests
./host/build-native/fmt_test           # expect "ALL OK"
./host/build-native/harness <guest.so> # expect the init line from REVIEW.md
scripts/harness-all.sh                 # the same for every title it lists, with LOUD counts
scripts/deploy.sh <id>                 # arm64 lib + signed APK + device commands
scripts/build-lib.sh && scripts/swap-lib.sh <id>...   # host-only change: ~1 s per APK
scripts/devgate.sh <id> [seconds]      # sandbox install + boot gate (default 40 s) + screenshot
```

Two Android builds of the host: `build-lib.sh` makes the phone one (API 29:
`thread_local` is ELF TLS, one instruction; API < 29 calls
`__emutls_get_address` on every access, ~8 % of Rio's game thread on a
phone), `build-lib.sh --sandbox` the sandbox one (API 26: Android 7.1
refuses a library with a TLS segment). They differ only in that. For the
sandbox: `scripts/build-lib.sh --sandbox && SANDBOX=1 scripts/swap-lib.sh
<id>` (also `SANDBOX=1 build-apk.sh`); `devgate.sh` refuses a phone build
there. APKs for the phone always carry the phone build.

Device gate policy: an ordinary change -> rio + seasons; a core-wide change
(memory, threads, JNI, loader, network, GL) -> all titles. Always look at the
screenshots: a wrong game state (dialog, black screen) does not raise markers.

Guest `.so` for the harness: `unzip -p <original apk> lib/armeabi-v7a/<lib>`, or
the copy under `games/<id>/work/**/armeabi-v7a/`; verify its sha256 against
`profile.toml`. Baselines live in `REVIEW.md` ("Regression gate").

The host lib embeds `__DATE__/__TIME__` (printed as `build marker` in
`apphost.log`), so its md5 changes on every build. Identify a build by that
marker and verify the md5 of the lib **extracted from the installed APK**.

## 4. Devices

- Sandbox (rooted "Android inside Android", 7.1.2, arm64 + armeabi-v7a):
  `ssh -p 8022 u0_a41@192.168.1.202` (key auth; DHCP — if unreachable, scan port
  8022 on 192.168.1.0/24 and check the host key). `sudo` = root. Anything inside
  the container is allowed; **never write to `/storage/emulated` (= /sdcard)**:
  it is the host phone's real storage.
- The owner's main phone (arm64-only, no root) is tested by hand; logs come as
  `adb logcat` files. All host log lines carry the tag `abhost`.
- App files: `/data/data/<pkg>/files/` — `apphost.log` (host log), `death.log`
  (signal witness: signal, host pc offset `hoff=` for `llvm-addr2line` on the
  *unstripped* build, guest regs).
- Device gate: markers HEAPCORRUPT, HEAPASSERT, CHUNKD, USAGE_ERROR, LOUD, STUCK,
  FATAL each grepped **separately** (Termux grep ignores `\|`), `death.log` 0 B,
  process alive, screenshot when rendering may be affected.

## 5. Ship configuration

The boot line `flags:` in `apphost.log` is the source of truth, currently
`fastmem=on diag=off jitopt=safe jitshare=on tlsthunk=on
fastdispatch=on hostfpsr=on jitcallbacks=on`. There is no heap quarantine (REVIEW R-013: a delay before
reuse only hides a use-after-free).

- `jitshare=on`: every guest Jit runs from one code cache (`Runtime::NewJit`,
  `patches/dynarmic/README.md`); `HARNESS_NOJITSHARE=1` restores per-thread
  caches. PC builds use dynarmic's x64 backend, which cannot share: run
  `fmt_test` on the sandbox (`cmake --build host/build-android-sandbox
  --target fmt_test`, copy, run with Termux `TMPDIR`) to test the arm64 path.

- `tlsthunk=on`: `pthread_getspecific` is answered in guest code from the
  thread's block (`kTlsBase`, `Runtime::BindTls`, `patches/dynarmic/README.md`
  0002); `HARNESS_NOTLSTHUNK=1` makes every call a host call again.

- `fastdispatch=on`: indirect branches stay in generated code through a table
  per Jit (`patches/dynarmic/README.md` 0005); `HARNESS_NOFASTDISPATCH=1` sends
  each one through the C++ dispatcher.

- `hostfpsr=on`: the guest's FP exception flags live in the host FPSR for a
  whole run (`patches/dynarmic/README.md` 0014); `HARNESS_NOHOSTFPSR=1` goes
  back to clearing and merging them per block.

- `jitcallbacks=on`: each Jit from `Runtime::NewJit` has its own
  `GuestCallbacks` (`patches/dynarmic/README.md` 0018), so a guest-to-host
  call knows its Jit without a thread_local, and `Ret` leaves the Thumb bit
  in PC bit 0 for `FinishSvc`; `HARNESS_NOJITCALLBACKS=1` goes back to the
  thread_local path. `FMT_BENCH=1 fmt_test` prints the cost of one call.

- ADPF (`host/src/guest/adpf.h`, Android 13+): the frame thread reports
  each frame's CPU time to a performance hint session whose target is the
  display period (Android 15+: prefer power efficiency). Frames end at the
  guest's eglSwapBuffers or, for the Rovio fusion engine, its per-frame
  `nativeUpdate`. Off: `setprop debug.abport.adpf 0` or `HARNESS_NOADPF=1`.
  The `[adpf]` line every 10 s (also when off) gives frames and frame-thread
  CPU per frame for A/B on a phone.

- One diagnostics switch: `HARNESS_DIAG` (env or `files/abflags.txt`, a
  KEY=VALUE override file read at boot). Never leave `abflags.txt` on a device.
- `ABR_DIAG_COUNTERS` (CMake, default OFF; `build-lib.sh --diag`, `DIAG=1
  build-apk.sh`) = periodic `[counters]` line; never in shipping builds.
- dlmalloc: `FOOTERS=0`, `DEBUG` must stay **undefined** (`DEBUG=0` defines it).

## 6. Rules

- Never patch a guest `.so` or an original APK; guest sha256 is checked at
  packaging.
- A wild guest pointer must become a guest halt with a LOUD log line, never a
  host crash and never a quiet skip. How is free: a check per call
  (`CheckRange`/`CheckAccess`, ~5 ns, `FMT_BENCH=1 fmt_test` prints it) or a
  fault handler, whichever is cheaper and proven.
- Every line that halts the guest, and every answer the host had to invent
  (an unimplemented import, a Java call that could not be made), contains
  `LOUD`: the device gate greps for it.
- A protection or a diagnostic added during an investigation is marked as
  such and removed in the commit that fixes the root cause. One that stays
  says why, in the code and in the commit message. Before calling existing
  code "needed", trace it with `git log -S`: a comment tells what was
  believed then.
- A device is never told the PC harness's invented answers (API level,
  screen size, device id): on Android the real Java method answers, or the
  call fails with a LOUD line.
- Verify against primary sources (NDK headers, jni.h, AAPCS32, GLES2 spec,
  guest disassembly with `llvm-objdump`), not against these docs or comments.
  Do address arithmetic in code, never in your head.
- Every host callback emitted into the JIT IR uses the 3-argument
  `CallHostFunction` form (the arm64 backend asserts on Void operands).
- One logical change per commit, with a test when possible; behaviour changes
  get the device gate before they are called done.
- One device session at a time.
- Offline is a requirement: the games must reach play with no network.
