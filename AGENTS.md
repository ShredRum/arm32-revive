# AGENTS.md — ab-port: classic Angry Birds (ARM32) on arm64-only Android

Short, current-truth guide. Rewritten 2026-10-25. The old 3000-line investigation
journal moved to `docs/history/AGENTS-journal.md`: it is history written by
small-context models and contains stale and wrong claims. Use it only to learn
*why* something was tried; never cite it as evidence. When a doc and the running
build disagree, the build wins.

Active work: the capital review — **read `REVIEW.md` first** (findings register,
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
  files/stdio, sockets, time, AAsset, GLES2 (forwarded to the real driver via
  `realgl.cpp`), and a 32-bit JNI function table that delegates to the real JVM
  where needed. Guest heap = dlmalloc mspace (1 GB virtual).
- `host/src/apphost.cpp` = `JNI_OnLoad`, per-thread JIT/stack, boot sequence, and
  the generated `Java_*` forwarders (`apphost_gen.inc`, from `host/gen_apphost.py`).
- The Java side of the original APK is unchanged except the loaded library name.

Titles (`games/<id>/`, each with `profile.toml` + `JOURNAL.md`): rio, seasons,
space, star-wars, star-wars-ii, star-wars-ii-amz. One shared host core.

## 2. Layout

- `host/` — CMake project. `src/guest/` = the core; `src/fmt_test.cpp` = unit
  tests of thunks; `src/harness.cpp` = PC runner of a guest `.so` (init only).
- `games/<id>/profile.toml` — package, versions, guest lib name + sha256.
  `games/<id>/original/` (gitignored) = original Rovio APK, read-only.
  `games/<id>/seeds/` = first-launch files. `games/<id>/work/` (gitignored) =
  decoded tree `apk_prod/`, `apk_signed.apk`, analysis leftovers.
- `scripts/` — `build-lib.sh [--native] [--diag]`, `build-apk.sh <id>`,
  `deploy.sh <id>` (both + prints device commands; never touches the device),
  `prepare-tree.sh <id>` (decode + port edits), `profile.py <id> <key>`.
- `tools/` (gitignored) — Android SDK, NDK r27 (27.2.12479018), build-tools 35,
  apktool. `thirdparty/` (gitignored) — dynarmic, boost headers.
- `keys/` (gitignored) — the signing keystore installed devices trust. Keep a backup.

## 3. Build and verify

```
scripts/build-lib.sh --native          # PC: harness, fmt_test, other tests
./host/build-native/fmt_test           # expect "ALL OK"
./host/build-native/harness <guest.so> # expect the init line from REVIEW.md
scripts/deploy.sh <id>                 # arm64 lib + signed APK + device commands
scripts/build-lib.sh && scripts/swap-lib.sh <id>...   # host-only change: ~1 s per APK
scripts/devgate.sh <id> [seconds]      # sandbox install + boot gate (default 40 s) + screenshot
```

Device gate policy: an ordinary change -> rio + seasons; a core-wide change
(memory, threads, JNI, loader, network) -> all six. Always look at the
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
`fastmem=on mallocpad=0 zerofill=off heapwatch=off norelease=off
clampwrite=off(+jni bound) diag=off jitopt=safe`.

- One diagnostics switch: `HARNESS_DIAG` (env or `files/abflags.txt`, a
  KEY=VALUE override file read at boot). Never leave `abflags.txt` on a device.
- `ABR_DIAG_COUNTERS` (CMake, default OFF; `build-lib.sh --diag`, `DIAG=1
  build-apk.sh`) = periodic `[counters]` line; never in shipping builds.
- dlmalloc: `FOOTERS=0`, `DEBUG` must stay **undefined** (`DEBUG=0` defines it).

## 6. Rules

- Never patch a guest `.so` or an original APK; guest sha256 is checked at
  packaging.
- A wild guest pointer must become a clean guest halt (LOUD + HaltExecution),
  never a host SIGSEGV: keep `CheckRange`/`CheckAccess` on every guest pointer.
- Verify against primary sources (NDK headers, jni.h, AAPCS32, GLES2 spec,
  guest disassembly with `llvm-objdump`), not against these docs or comments.
  Do address arithmetic in code, never in your head.
- Every host callback emitted into the JIT IR uses the 3-argument
  `CallHostFunction` form (the arm64 backend asserts on Void operands).
- One logical change per commit, with a test when possible; behaviour changes
  get the device gate before they are called done.
- One device session at a time.
- Offline is a requirement: the games must reach play with no network.
