# Angry Birds Seasons 6.6.2 — port journal

**Status: PORT IN PROGRESS — harness boots to menu, Play hangs in the guest
loop (2026-10-20).** The original APK runs natively on the sandbox and reaches
gameplay; the remaining bug is in the harness/JIT path, not in the game data.

Identity / machine-readable facts: `profile.toml` (same directory).

## Control run: the official APK works natively (2026-10-20)

Installed the untouched `original/angry-birds-seasons-6-6-2.apk` on the SSH
sandbox (`arm64-v8a`, `abilist=arm64-v8a,armeabi-v7a,armeabi`, Android 7.1.2 /
SDK 25). The package manager selected `primaryCpuAbi=armeabi-v7a`; no harness,
no JIT, no patched guest.

- launch: `am start -n com.rovio.angrybirdsseasons/com.rovio.fusion.App`
- the main menu rendered fully (background, logo, Play, settings), process
  alive (PID 9067, top-activity, PSS ~350 MB at the menu);
- the user pressed Play **by hand**: the level loaded, the slingshot tutorial
  appeared;
- logcat has no `FATAL` / `SIGSEGV` / `SIGILL`; the usual non-fatal warnings
  only (`cloudManifest.json` missing, missing sprites, ad SDK noise).

Artifacts: `/tmp/opencode/official_now.png`, `/tmp/opencode/official_logcat2.txt`
on the host. This is the control we needed:

> The game, encrypted assets, Lua/JSON data, camera/worldScale math and the
> 32-bit ARM runtime all work natively. The Play hang is specific to the
> harness/JIT path.

The official APK replaced the diag APK on the sandbox. The diag build is still
on the host at `work/apk_diag.apk`; reinstall it when harness debugging resumes.

## Harness state

Working (sandbox, DIAG build):

- `init done: 142/142` (143 entries, one null), `JNI_OnLoad` returns
  `hr=16777216 r0=00010006`, `nativeResume` runs, menu renders;
- host asset bridge: `AAssetManager_open`, `AAsset_getBuffer`,
  `AAsset_getLength64`, `AAsset_close`; ~1620 asset opens; last
  `TUTORIALS_SHEET_1_NORMAL_1.pvr`;
- `nativeInit(2248,1080)` and `nativeResize(2248,1080)`; no zero dimensions;
- `death.log` 0 B, no unimplemented syscall/halt at the hang.

Broken:

- after Play: black `LOADING`, process alive at high CPU, watchdog reports
  `STUCK update` in the guest loop at `0x9ff34`; no LOUD halt;
- live Lua state (JIT build, loop `0x9ff34`):
  `GameLua=0x1101f298`, `GameLua+0x34ac=0.0`, `+0x34b4=NaN`,
  `+0x34b8=+inf`;
- `gamelua` env: `screenWidth=2248`, `screenHeight=1080`,
  `deviceModel='android'`, `worldScale=0`, `minWorldScale=0`;
- `objects.castleCameraData` exists with `android` / `iphone` / `ipad` /
  `version='0.03'`, but the live floats are garbage
  (`sx=0`, `px=inf`, `py=-inf`, `left=nan`, ...), while the source JSON
  `Tutorial_red.json` has finite values (`ipad sx=1.18626`,
  `iphone sx=0.59654`);
- the loop is a true infinite loop: target `+inf`, cursor `2^31`, step 699,
  divisor `+0x34ac=0`, `sx` cursor.

## Native reference: Lua scan at the tutorial

A read-only scanner over `/proc/9067/mem` (same Lua 5.1 layout: 8-byte TValue,
20-byte Node, TString data at `+0x10`) found the interned strings and table
nodes for the native run:

- `worldScale` TString `0x12aef480`; the exact `gamelua` environment node is
  `0x12a3e88c`, value `1.00666`;
- `screenWidth` TString `0x126ac380`; exact env node `0x12a42ec8`, value
  `2248`; `screenHeight` node `0x12a3fc78`, value `1080`;
- `minWorldScale` node `0x12a39d64`, value `0.709066`;
- `deviceModel` node `0x12a3f110`, value `'android'`.

**Phase A result (2026-10-20, native PID 9067 at the tutorial):**

- `gamelua`: `worldScale=1.00666`, `minWorldScale=0.709066`,
  `screenWidth=2248`, `screenHeight=1080`, `deviceModel='android'`.
- `objects` table `0x126db5c0`; `objects.castleCameraData` table `0x2ff76180`,
  `version='0.03'`.
- `android` (`0x12685400`): `sx=sy=1.00666`, `px=1337.27`, `py=-546.768`,
  `left=220.708`, `right=2453.83`, `top=-1083.19`, `bottom=-10.3412`,
  `screenWidth=2248`, `screenHeight=1080`.
- `iphone` (`0x2ff761c0`): `sx=sy=0.59654`, `px=1337.27`, `py=-546.768`,
  `left=532.63`, `right=2141.91`, `top=-1083.19`, `bottom=-10.3412`,
  `screenWidth=960`, `screenHeight=640`.
- `ipad` (`0x2ff761a0`): `sx=sy=1.18626`, `px=1399.44`, `py=-486.377`,
  `left=536.224`, `right=2262.66`, `top=-1133.79`, `bottom=161.035`,
  `screenWidth=2048`, `screenHeight=1536`.

These are the expected same-point values. The harness has `worldScale=0` and
garbage `castleCameraData`, so Phase B can now measure the first bad write
against a concrete reference.

Scripts: `/tmp/opencode/seasons_analysis/native_lua.py`,
`native_refs.py`, `native_env.py`, `native_table.py` (read-only,
`/proc/<pid>/mem`).

## Keys (app-fixed, verified by decrypt — do not publish)

The engine keys are hardcoded, not device-bound. They are recorded here because
the user asked for the journal to be the single log (even though
`games/rio/work/KEYS.md` says "do not commit"). Treat this section as private.

### K_a — Seasons level/game assets (`DecryptInputStream`)

- ASCII (32 B): `zePhest5faQuX2S2Apre@4reChAtEvUt`
- Hex: `7a65506865737435666151755832533241707265403472654368417445765574`
- Mode: AES-256-CBC, zero IV, PKCS#7. The plaintext is often a 9-byte
  LZMA-alone magic (`89 4c 5a 4d 41 0d 0a 1a 0a`) followed by an LZMA stream;
  after decompression it is JSON or Lua 5.1 bytecode with 4-byte floats.
- Source: `content_work/extract.py` (batch extractor).
- Verified on
  `assets/data/levels/ArcticEggspedition/Level_ArcticEggspedition_1.json`
  (3824 B ciphertext -> 26748 B JSON) and on the whole
  `content_work/dec_full` tree.
- Rio `K_g` (`USCaPQpA4TSNVxMI1v9SK9UC0yZuAnb2`) and Rio `K_2`
  (`44iUY5aTrlaYoet9lapRlaK1Ehlec5i0`) do **not** decrypt this asset.

### K_r — registry/config (engine-shared with Rio)

- Hex (32 B): `3a7d2e0379e64985a01fa80104d5d77da1bc7ae70363248e7ac9c0ad5f4660ea`
- AES-256-CBC, zero IV; plaintext is JSON plus a Crypto++-style random pad; no
  32-byte header.
- Source: Rio `work/KEYS.md`; verified on Seasons
  `files/fusion.registry` (160 B -> `{"fusion":...}` + pad).
- The Rio key decrypts the Seasons registry unchanged, so this key is shared
  across the engine family.

### K_2 — not verified for Seasons

- Rio `K_2` ASCII: `44iUY5aTrlaYoet9lapRlaK1Ehlec5i0`
- Tested against the Seasons level asset: rejected. Whether Seasons has a
  second asset-class key is being checked by a subagent; do not assume `K_2`
  applies.

### Recipes

- Asset: `openssl enc -d -aes-256-cbc -K <K_a hex> -iv 0`, strip PKCS#7; if the
  first 9 bytes are the LZMA magic, `lzma.decompress(d[9:], FORMAT_ALONE)`.
- Registry: `openssl enc -d -aes-256-cbc -K <K_r hex> -iv 0 -nopad`.

## Lead from the control run: proven vs not proven

**Proven by the native run:**

- the game, encrypted assets, JSON/Lua data, camera/worldScale code and the
  32-bit ARM runtime all work on this device when the untouched guest is run
  natively;
- therefore the Play hang is not caused by the game data, the asset cipher, the
  ARM32 environment or the camera math itself;
- the official run is a working reference for the same level/tutorial.

**Not proven:**

- which harness component corrupts the state. The measured divergence is
  `worldScale=0` and garbage `castleCameraData` floats in the harness versus
  nonzero `worldScale` nodes in the native process, but the exact
  `gamelua` table and the first wrong write are not pinned yet;
- whether the corruption comes from JSON/`strtod`/JIT double semantics, the
  asset bridge, or call order/timing. These remain candidates, not findings.

**Verdict:** the native run is a strong **lead**, not the root cause. It proves
where the bug is *not* and gives a same-point reference; it does not identify
the faulty instruction or import.

## Debugger / SELinux plan

SELinux is **Permissive** on the sandbox since 2026-10-20 (was `Enforcing`).
That removes policy blocks for `ptrace`/`/proc/<pid>/mem`; it is a sandbox-only
change and resets on reboot (`setenforce 1` restores it). Root `/proc/<pid>/mem`
reads already worked under Enforcing, so permissive is a convenience, not the
fix.

The Android debugger **will help**, but only if used on the harness process,
not on the working native process:

- attach `gdb`/`lldb` to the diag harness after Play and set a **hardware
  watchpoint** on the host address corresponding to the guest
  `worldScale` value (guest Lua table address + guest base);
- when it fires, read `g_upd_jit` / the saved guest registers to get the guest
  PC and the writing instruction, then map that PC to `guest.dis` and the JIT
  translation;
- repeat for the first `castleCameraData` float (`sx`, `px`, `left`, ...).
  This catches the **first bad write**, including bulk/NEON stores that the
  harness's `HARNESS_WPA` may bypass;
- optionally break at `initCameras` / `setWorldScale` / the JSON number parser
  and log arguments/returns; this separates "parsed wrong" from "computed
  wrong";
- the native process can be inspected with the same debugger only as a
  reference (its `gamelua` table/values); it is not the target.

Requirements: reinstall `work/apk_diag.apk` (the official APK currently
occupies the package), reproduce Play, and have the user press Play by hand.
No taps are injected. A hardware watchpoint is 4 bytes; if the value is written
by a wider store, watch the containing word and confirm the adjacent fields.

## Action plan (measured, no guessing)

Each phase ends with a binary measurement. If a gate is inconclusive, stop and
report it; do not replace it with another hypothesis.

**Phase A — pin the native reference (no APK change).**
Read PID 9067 at the tutorial and pin the exact `gamelua` environment table:
`worldScale`, `minWorldScale`, `screenWidth/Height`, `deviceModel`,
`objects.castleCameraData` (`android`/`ipad`/`iphone`) and 2–3 object floats.
Gate A: exact table identified and values finite/nonzero; if not, use the Lua
state/global-table pointer or the debugger, and stop if still unpinned.

**Phase B — find the first divergence in the harness.**
Reinstall `work/apk_diag.apk`, reproduce Play (user presses Play), attach
`gdb`/`lldb`, and either break at `initCameras` or set a hardware watchpoint on
the live `worldScale` value word. Dump `castleCameraData` at `initCameras`
entry and after it; dump `worldScale` before/after `setWorldScale`.
Gate B1: garbage already at `initCameras` entry -> parse/asset path.
Gate B2: clean at entry, garbage after -> `initCameras` math/call order.
Gate B3: catch the first writer of `worldScale=0`; record guest PC, instruction,
JIT translation and operands. That is the root-cause candidate, not a guess.

**Phase C — fix the first measured divergence and verify.**
Change only the measured component (JIT FP/double semantics, import behaviour,
asset bridge, or call order). Rebuild diag, reinstall, reproduce Play. Pass:
`worldScale` nonzero, camera fields finite, no `STUCK`, tutorial visible; user
tests gameplay by hand. If it still hangs, measure the next divergence; do not
add a crutch.

**Phase D — ship.**
Diff against `eaa0a5c`, remove crutches, gate ordinary diagnostics, fix
`jni_exports` 72 -> 73, build prod, verify the installed stripped lib md5,
discuss targetSdk 22, tag only with explicit confirmation.

**Immediate next action:** Phase A can run now against the official process
without touching the installed APK. Phase B needs the diag APK restored.

## Current investigation (no root cause claimed yet)

The evidence narrows the bug to the harness's handling of the level-load /
camera path. It does **not** yet prove which component corrupts the floats.

Next checks, in order:

0. Reinstall the diag APK, reproduce Play, attach `gdb`/`lldb`, and put a
   hardware watchpoint on the guest `worldScale` word. Record the first writer
   (guest PC, instruction, JIT translation). Repeat for the first camera float.

1. Pin the exact native `gamelua` environment table and read
   `worldScale` / `minWorldScale` / `objects.castleCameraData` at the tutorial;
   compare field-by-field with the harness state.
2. Reinstall the diag APK, reproduce Play, and dump all JSON-derived floats
   (a bird/object `x,y,sx,sy`, not just the camera). If **all** level floats are
   garbage, the JSON/float parse or JIT double path is the suspect. If only the
   camera is garbage, the camera init/call-order path is the suspect.
3. Add one `HARNESS_DIAG`-gated hook at `initCameras` entry and at
   `setWorldScale` / `setCameraViewport`, logging args and LR; capture
   `castleCameraData` immediately after JSON parse and after `initCameras`.
4. Fix the first measured divergence; re-test Play in the sandbox; do not
   inject taps — the user tests gameplay by hand.
5. If the comparison stays inconclusive, say so explicitly rather than adding
   another hypothesis.

## Device / constraints

- SSH sandbox: `u0_a41@192.168.1.202:8022`, Android 7.1.2 / SDK 25,
  `arm64-v8a` with 32-bit ARM support. SELinux was switched to **Permissive**
  on 2026-10-20 (`getenforce` was `Enforcing`) so `/proc/<pid>/mem` and
  debugger work cannot be blocked by policy.
- The guest `.so` is never patched; the original APK is read-only; dlmalloc is
  kept.
- Diagnostics behind `HARNESS_DIAG`; anomalies stay LOUD unconditionally.
- Do not inject taps; the user drives Play/gameplay.
- Do not create a git tag without explicit confirmation.

## Packaging / remaining

- `games/seasons/profile.toml`: `jni_exports` is 72, but the union build has
  73 forwarders; fix before the prod diff.
- Final diff must be relative to `eaa0a5c` (the commit before Seasons
  development), with no leftover crutches; ordinary diagnostics gated.
- Device install of the prod APK is blocked by
  `INSTALL_FAILED_DEPRECATED_SDK_VERSION` (targetSdk 22 < 24); the sandbox
  accepts it. The user deferred the targetSdk discussion.

## Tooling

- `/tmp/opencode/seasons_analysis/lua_table.py` — live Lua 5.1 table dumper
  (harness).
- `/tmp/opencode/seasons_analysis/jit_read.py` — read-only JIT dumper, new-build
  offsets (`g_upd_*`, `g_mem`).
- `/tmp/opencode/seasons_analysis/peek_guest.py` — read-only guest-memory
  peeker.
- `/tmp/opencode/seasons_analysis/guest.dis` — exact guest disassembly; loop
  `0x9fec0..0x9ff48`, `setWorldScale` / `setCameraViewport`.
- `/tmp/opencode/gl_dis_full.txt` — full recursive `gamelogic.lua` bytecode;
  `#321`–`#323` camera code.
- `/tmp/opencode/luadis51.py` — patched Lua 5.1 disassembler for decrypted
  bytecode.

## 2026-10-20 — measured `setWorldScale(0)`: Lua-side, not JIT/FPU

- `HARNESS_WATCHPC=9ae2c` (diag run, fastmem on) fired 4 times at the
  `worldScale` setter: `r1=1.11773`, `1.11773`, `1.0`, `0.0`. All four had
  `lr=0x90d5c` (C thunk at `0x90d08`). The setter receives `0.0` from Lua;
  JIT/FPU/setter are not the first divergence.
- `gamelogic.lua` function `main.323` (lines 13509–13811) executes
  `worldScale = objects.castleCameraData[deviceModel].sx` and then
  `setWorldScale(worldScale)` (pc 272/274; second copy at pc 328/330).
- Tutorial level JSON (`Tutorial_red.json` and neighbours) contains only
  `iphone` and `ipad` camera profiles; there is no `android` key. Native
  `castleCameraData.android` is therefore a runtime-synthesized profile built
  in `initCameras` from `screenWidth`/`screenHeight`/`originalCcd`. Native at
  the tutorial had `android.sx = 1.00666` with 2248×1080.
- Working hypothesis (not yet proven): at `initCameras` the harness still has
  zero/missing screen or level-camera data, so the synthetic `android.sx`
  becomes 0. Then camera math divides by `worldScale=0` → NaN/inf → `STUCK`.
  The slow first `LOADING` may share the same call-order/timing root.
- Next single diagnostic: at `setWorldScale` / `initCameras`, dump
  `screenWidth`, `screenHeight`, `g_updatedScreenWidth`, `g_updatedScreenHeight`,
  `g_levelLoaded`, and `castleCameraData.android.sx`; compare with native.
  Do not patch the guest `.so`; do not add a gameplay crutch.
- HD hypothesis checked quickly: package is `com.rovio.angrybirdsseasons`
  (non-HD); `isHDVersion` only changes `CAMERA_PROFILES` order and `android`
  is first in both branches, so it does not by itself explain `sx=0`.
- `/tmp` artifacts are ephemeral. Key parsers, bytecode dumps and reports were
  copied to `games/seasons/work/analysis_2026-10-20/`.

## 2026-10-20 (evening) — where the level numbers break: JSON number callback

Measured, on device, with `HARNESS_WATCHPC` (diag build, fastmem on):

- The JSON-number callback is guest `0x3f816c`; it has **two** `lua_pushnumber`
  sites: `0x3f81d4` (array/index branch) and `0x3f8248` (string-key branch).
  The level object table is keyed by strings, so only `0x3f8248` sees the level
  numbers — an earlier watchpoint on `0x3f81d4` saw menu data only.
- Guest `.so` is loaded with `p_vaddr=0` (`[phdr] LOAD vaddr=00000000`), so
  **guest VA == file VA**; the Ghidra project (base `0x10000`) shows
  `guest + 0x10000`. No address shift is needed.
- Values captured at `0x3f8248`:
  - boot/menu: `5.0, 10.0, 25.0, 1.0` (integers — correct);
  - after Play: `0.0, +inf, -0.0, 0.0` — already broken **before**
    `lua_pushnumber`, so the damage is in the JSON parse or in the visitor,
    not in the Lua push.
- JSON node layout (guest `0x437988`, the `type==2` branch of the node
  dispatcher `FUN_00437854`): `+0` = inline key text (8 bytes), `+8` = the
  double, `+0x10` = type byte. Visitor vtable slots: `+0x8` null, `+0xc`
  bool, `+0x10` number, `+0x14` string, `+0x18` array, `+0x20` object.
- A TRCALL on `0x437988` (22k hits) shows the node's stored double as
  garbage (`3.17e+180`) while the inline key text is intact (`"Facebook"`),
  i.e. keys/strings parse correctly, the numeric value does not.

Harness/tooling lessons (do not repeat):

- `HARNESS_TRCALL=<start>,<end>` takes an **end address**, not a length.
  `3f816c,4` silently disables the hook (end `0x4` < start).
- `HARNESS_TRCALL` on a hot guest function is a foot-gun: `0x3f816c` is
  entered ~27k times / 2 min with `r0=8` (null-ish calls), which wrote 35 MB
  of log and starved the guest into a grey screen. Only trace cold sites, or
  cap the output.
- `HARNESS_WATCHPC` fires at most 4 times per process. The counter lives in
  the host lib (`libabhost.so` + `0x431528` = `wn`, `+0x43152c` = `wseq`) and
  can be reset at runtime as root through `/proc/<pid>/mem` (write 4 zero
  bytes) — no rebuild needed. Boot JSON burns the 4 slots, so reset after the
  menu is up.
- `HARNESS_PROBE2/3/4` and `HARNESS_PROBE_N` from `abflags.txt` override the
  diag defaults (abflags are applied after them), and can be used to silence
  the probe dumps.

Level-data provenance (measured, not assumed):

- The whole run contains only **4** asset opens, all for
  `data/images/{1024x768,common}_cloud/loadlist.json` -> NULL. No
  `data/levels/...` asset is ever opened, and no level path appears in the
  stdio (`[files] fopen`) trace either — yet the level object table appears
  in Lua after Play. So the level JSON does **not** come from the APK through
  the asset bridge; it is either embedded in the guest `.so` or served by
  another path. (Open question, being investigated in background.)
- In the stuck LOADING phase the guest parses JSON numbers at ~630/s
  (TRCALL measurement) — a retry loop, consistent with the missing
  `loadlist.json` cloud files (the APK ships `1024x768/loadlist.json`
  *without* the `_cloud` suffix).

Current hypothesis (to be confirmed): the JSON **fractional** value is
corrupted while integers survive, so the fault is in the guest's
text->double conversion (custom parser, or a double/pow/FPU path in the
harness) rather than in the Lua write path.

## 2026-10-20 (night) — root cause found: variadic double ABI in the harness

Measured chain (guest VA == file VA, `.so` loaded with `p_vaddr=0`):

- The level JSON is **not** parsed into numbers directly. `GameLua::loadLevel`
  (guest `0xc61b0`) reads `data/levels/HalloweenForest/Tutorial_red.json`
  from the APK (AES-256-CBC + LZMA, decrypted by the guest — no error strings
  in the log), builds a JSON DOM, and then **serialises the DOM into Lua
  source**, which the embedded Lua 5.1 compiles (`luaL_loadbuffer` guest
  `0x58c88c`, called at guest `0x3f3d98`).
- So every level number makes a **text round trip inside the guest**:
  number -> text -> (Lua compiler) -> number.
- The number->text step is libc++ `num_put`: `modf` (guest `0x4406ec`) then
  `vsprintf` through the Thumb shim at guest `0x625d64` (`setlocale` ->
  `strcmp(locale,"C")` -> `blx vsprintf@plt` with a va_list that is the
  **caller's stack area**, `sp+0x28` = the shim's entry SP).
- Proof that the double itself is still correct at that point: the log shows
  `modf(07c84b5e c0336b3d ...)` = exactly `-19.4189` (lo/hi words), then
  `modf(52bd3c36 40468996 ...)` = `45.0749`, `modf(8a71de6a 40587c8e ...)` =
  `97.9462`, `modf(2d0e5604 c04375b2 ...)` = `-38.9195`.
- The corruption is therefore in the **harness' printf-family argument
  reader**: `Runtime::ExpandPrintf` read variadic double-words as two
  consecutive 32-bit words, ignoring AAPCS32 (a double-word argument must
  occupy an **even-numbered** core register pair, otherwise it goes on the
  stack at an 8-byte aligned address). Verified against the NDK ABI
  (NDK r27 clang, `-mfloat-abi=softfp`): `printf("v=%f",d)` puts the double
  in `r2:r3` (r1 skipped), `snprintf(b,64,"x=%f",d)` puts it on the stack,
  `sprintf(b,"x=%f",d)` in `r2:r3`.
- Symptom match: `%d`/integers stayed correct (4-byte args are unaffected),
  every fractional field became `0 / +inf / -0 / nan`; the Lua-visible
  values were exactly the bits of a mis-read double.

Fix (host-side, no guest patch): `host/src/guest/dispatch.cpp`,
`Runtime::ExpandPrintf::next64()` now implements AAPCS32: round the next
core register up to an even index, and fall back to an 8-byte aligned stack
slot; the `va_list` path aligns its position to 8 bytes before reading a
double-word.

Build: `scripts/build-lib.sh` + `DIAG=1 scripts/build-apk.sh seasons`
=> `games/seasons/work/apk_diag.apk` md5 `ed1a4c5341f3f94f950f8ce96a0cb1ee`
(installed stripped `libabhost.so` md5 `2d62efbd8eece2fd96899e076765fc13`).

Known remaining harness bugs from the same audit (NOT fixed yet, listed for
later; none of them is on the level-number path):
- `__android_log_print` has its own mini formatter: no `%f/%e/%g`, and the
  `default` branch does not consume the argument, so later varargs shift.
- `vsscanf` is handled as `sscanf` (the va_list pointer is taken as the first
  variadic argument).
- `math_thunks.inc`: `fma` reads its third double from `r2:r3` instead of
  `r4:r5`; `remquo` writes the quotient pointer to `r2` instead of `r4`.
- `log1p`, `scalbnl` and the `__aeabi_d*` soft-float helpers are not
  implemented and silently return 0.

### 2026-10-20 (night) — fix verified on device

Installed `apk_diag.apk` md5 `ed1a4c5341f3f94f950f8ce96a0cb1ee`; the installed
stripped `libabhost.so` md5 `2d62efbd8eece2fd96899e076765fc13` matches the
build. After Play the level now matches the native reference exactly:

| value | harness (before) | harness (after) | native |
|---|---|---|---|
| `worldScale` | 0 | 1.006661 | 1.00666 |
| `minWorldScale` | 0 | 0.7090657 | 0.709066 |
| `maxLevelWidth` | inf | 3170.369 | - |
| `screen.x / .y` | inf / -inf | 1337.27 / -546.768 | 1337.27 / -546.768 |
| `screen.left/right/top/bottom` | nan/inf/-inf/nan | 220.7076 / 2453.833 / -1083.195 / -10.34125 | 220.708 / 2453.83 / -1083.19 / -10.3412 |
| `castleCameraData.android.sx` | 0 | 1.006661 | 1.00666 |
| `castleCameraData.iphone.sx` | 0 | 0.59654 | 0.59654 |
| `castleCameraData.ipad.sx` | -5.729413e+19 | 1.18626 | 1.18626 |
| `BoomBoxSpawnRect.bottom/left/right/top` | 0 / inf / -0 / 0 | -19.4189 / 45.0749 / 97.9462 / -38.9195 | same as JSON |
| `WoodBlock6_7.angle` | 0 | 1.57087 | 1.5708 |

The screen shows the actual tutorial level (slingshot, bird, pigs, trajectory,
tutorial card), `files/death.log` is 0 B and no `STUCK update` occurs after
Play (the single `STUCK update #9` in the log is the pre-existing boot-time
stall, present in every run including the broken ones).

Tooling note: the host library's `.bss` layout moved with this build
(`g_mem` 0x431208 -> 0x4313c8, `WatchPC::wn` 0x431528 -> 0x4316e8,
`strtod_shown` 0x4314b4 -> 0x431674). The read-only device tools
(`lua_watch.py`, `find_tables.py`, `heap_grep.py`, `peek_guest.py`,
`lua_table.py`) hardcode `G_MEM`; they must be re-synced after every
harness rebuild, and the `HARNESS_WATCHPC` counter reset trick must use the
current offset (and the current `libabhost.so` base, which changes when the
APK is reinstalled).

## 2026-10-20 (night) — boot speed: import dispatch (A) + guest-side VFP sqrt (B)

Measured cause of the slow boot (prod log, before): one `nativeUpdate` call
blocked **33.7 s** ("STUCK update #9"); the STUCK sampler put 1526 of 2000
samples inside the *sqrt* import trampoline, with `lr=004cc06c` (a guest-side
sort whose comparator calls sqrt).

Why the call was so expensive: `Runtime::CallImport` resolved every import
by walking a chain of ~194 `n == "..."` string comparisons, and the math
block sits at the very END of that chain (`math_thunks.inc` was included at
line 5087). One `sqrt` cost ~209 string compares -- more than the libm call
itself. The guest imports exactly fmod, cos, sin, floor, sqrt, pow, atan, tan.

Fix A (host, behaviour-preserving): the identity of those math imports is
resolved ONCE at load time (`Import::fast_math`, `FastMathForName`), and
`Runtime::CallFastMath` dispatches them with a switch. Same libm calls, same
softfp convention (double in r0:r1, low word first) as `math_thunks.inc`.

Fix B (host, no guest patch): the guest's `sqrt` import is answered by
**guest-side ARM32 code** instead of an SVC into the host -- a 16-byte thunk
in the trampoline page:

    ec410b10  vmov   d0, r0, r1     ; d0 = the double (r0 = low word)
    eeb10bc0  vsqrt.f64 d0, d0
    ec510b10  vmov   r0, r1, d0     ; r0:r1 = the result
    e12fff1e  bx     lr

The bytes are the NDK's own codegen for `sqrt(double)` under
`-mfloat-abi=softfp` (assembled with the same NDK that built the guest, then
disassembled back to check the operand order -- it is the toolchain's
convention, not a guess). The call now never leaves the JIT: no SVC, no host
libm, no import dispatch. `Import::call_va` is the address the guest
branches to (== `tramp_va` unless a VFP thunk is used); the 4-byte svc slots
keep their layout so the STUCK diagnostics still map a pc to an import name.

Evidence after A+B (diag build, HARNESS_DIAG=1):
- the log has **no** `STUCK update` line at all; the largest gap between
  nativeUpdate stamps is 17.3 s and is 244 normal frames (~70 ms each);
- **zero** host-side sqrt calls (`[call] ... sqrt(` and `[import] first call:
  sqrt` -> 0 hits) -- the VFP thunk is the path actually taken;
- the other math imports still go to the host, now through the O(1) dispatch;
- the user reports the load is visibly faster.

Robustness guards added: the svc slots may not grow into the VFP thunk area
(load fails loudly instead of overlapping), and `ResolveData` keeps
`call_va` in sync for data imports.

Prod build with A+B: `games/seasons/work/apk_signed.apk`
md5 `550b3195c1abc278c33218add0c0d599`, installed stripped
`libabhost.so` md5 `19de988ed91ab4e5ab5ca16b52c96027`.

Note: every harness rebuild moves the host lib's `.bss`, so the read-only
device tools hardcode `G_MEM` (currently `0x431dc8`) and must be re-synced;
the `HARNESS_WATCHPC` counter-reset trick needs the current offset and the
current `libabhost.so` base (it changes when the APK is reinstalled).

### 2026-10-20 (night) — A+B verified: level numbers are bit-identical to the JSON

Regression check on the level the user loaded ("Hammier Things" #3 =
`Tutorial_bomb`, from the decrypted `episodesLatest.json`: levels =
Tutorial_red, Tutorial_terence, Tutorial_bomb, ...). The level JSON was
decrypted from the APK (AES-256-CBC + LZMA) and compared field by field
with the live guest Lua state:

| field | JSON | live Lua |
|---|---|---|
| BoomBoxSpawnRect top/right/left/bottom | -75.6951 / 113.45 / 34.2515 / -11.4584 | identical |
| castleCameraData.iphone sx / px / py | 0.316265 / 908.021 / -754.826 | identical |
| castleCameraData.iphone left/right/top/bottom | -609.694 / 2425.74 / -1766.64 / 256.984 | identical |
| castleCameraData.iphone screen | 960x640 | identical |
| castleCameraData.ipad sx / px / py | 0.432764 / 1203.34 / -828.318 | identical |
| castleCameraData.ipad left/right/top/bottom | 20.2469 / 2386.43 / -1715.64 / 59.0017 | identical |
| castleCameraData.ipad screen | 1024x768 | identical |

`castleCameraData.android` is absent from this level's JSON; the live value
(sx=0.5336972, px=908.021, py=-754.826, 2248x1080) is the game's
runtime-synthesized android profile and is finite. On the pre-fix build the
same fields were 0 / inf / -0 / nan.

TypeSafe Jev gate on the A+B diff with that evidence: 6/6 claims verified
(0 contradicted, 0 unsupported; 4 auto, 2 review). The patch review itself
returns `escalate` with correctness 1.64 (confidence 0.47) and test_gap 1.05
(confidence 0.59) — the residual concern is the **blast radius**: the fast
path and the VFP thunk live in the shared harness, so Rio/Space inherit them
too (they are also ARM32 softfp, so the thunk is valid for them, but they
have not been re-run since this change).

### 2026-10-20 (night) — blast radius closed: Rio and Space re-run on the same lib

A+B live in the shared harness, so every title inherits them. All three were
built from the same host library and verified on the device:

| title | APK | installed stripped `libabhost.so` | log | death.log | screen |
|---|---|---|---|---|---|
| Rio | `games/rio/work/apk_signed.apk` md5 `8fd50a01b60c634729723e0d42a83cc8` | `19de988ed91ab4e5ab5ca16b52c96027` | no LOUD/ERROR at all (Rio's image carries the pinned build-id) | 0 B | level select, played by the user |
| Space | `games/space/work/apk_signed.apk` md5 `fbf1b8885187c035e3a9ea69ab83d850` | `19de988ed91ab4e5ab5ca16b52c96027` | only the known `LOUD build-id mismatch` (Space's image has no build-id note, like Seasons) | 0 B | main menu renders |
| Seasons | `games/seasons/work/apk_signed.apk` md5 `550b3195c1abc278c33218add0c0d599` | `19de988ed91ab4e5ab5ca16b52c96027` | clean | 0 B | level 3, numbers bit-identical to the JSON |

All three guests are the same ABI, which is what makes the VFP thunk valid
for each of them: `Tag_FP_arch: VFPv3`, `Tag_ABI_HardFP_use: Deprecated`
(base PCS -> a double arrives in r0:r1 with the low word first),
`Tag_ABI_FP_number_model: IEEE 754`, `Tag_ABI_FP_denormal: Needed`, and no
`Tag_ABI_VFP_args`. All three import the same eight math functions
(sqrt, atan, sin, cos, floor, fmod, pow, tan), so both the O(1) dispatch and
the guest-side sqrt thunk apply to each.
