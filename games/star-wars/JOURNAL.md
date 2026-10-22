# Angry Birds Star Wars HD 1.5.13 (premium IAP) — port journal

The port core is title-agnostic (see ../seasons/JOURNAL.md for the shared
harness history); this file is the Star Wars log.

## 2026-10-22 (night) — the port starts: the tree, the JNI bridge, the first APK

**What was measured before touching anything** (no assumptions):

- The Java side is the SAME `com.rovio.fusion` framework as Seasons/Rio/Space
  (App, `com/rovio/fusionapp/Gen`, the wrappers) ⇒ the same JNI bridge, the
  same activity, the same lifecycle.
- The guest `libAngryBirdsStarWars.so`: armeabi-v7a, 7 869 668 B, sha256
  `10c050d8…` (re-verified from the APK byte by byte, not trusted from the
  profile). NEEDED = libandroid / libGLESv2 / liblog / libstdc++ / libm /
  libc / libdl (the same set as the other titles). No build-id (like
  Seasons). DT_INIT_ARRAY = 0x184 B = **97** entries (Seasons: 143).
- The imports: **315** undefined symbols, **313 of them identical to
  Seasons**. The only new ones are `ceilf` and `powf` (math). The fast_math
  table (sqrt/atan/sin/cos/floor/fmod/pow/tan) does not cover the float
  variants ⇒ they take the generic thunk path (correct, but the slower
  string-compare chain) ⇒ worth touching ONLY if they show up in a hot
  loop.
- The JNI exports of the guest: **71**.
- The assets: `assets/data/**` — .pvr (PowerVR) textures, .json animations,
  .mp3 — **deflated** in the APK. Our AAsset bridge already inflates (raw
  inflate via zlib, `dispatch.cpp:5307+`) ⇒ nothing to do here.

**The mechanical part**

- `prepare-tree.sh star-wars` ⇒ the decode + the port edits: the guest .so →
  `assets/guestlib/armeabi-v7a/` (STORED), `lib/armeabi-v7a` and `lib/x86`
  removed, `Gen.smali`: `"libAngryBirdsStarWars"` → `"abhost"` (1
  occurrence), `apktool.yml doNotCompress` += the guest .so. The guest's
  sha256 was re-verified after the move ⇒ the guest is moved, never modified.
- The JNI signatures: `games/star-wars/work/jni_signatures.txt` (71
  entries), produced by a new tool that follows the guest's **EXPORTS** and
  takes the descriptors from the decoded smali. The tool was validated
  FIRST on the finished titles: it reproduces the Seasons file byte for byte
  (72 entries) and the Space file byte for byte (62); for Rio it yields the
  same 63 entries in a different order (the sets are equal).
  Why the exports and not the declarations: the smali declares 79 native
  methods while the guest implements 71 — the other 8 are dead paths
  (Seasons: 80 declared / 72 exported — the same delta of 8).
- The union bridge: **all 71** Star Wars methods are already forwarded by
  the existing union (the same names AND the same descriptors, 0
  conflicts) ⇒ `gen_apphost.py --union` regenerates `apphost_gen.inc`
  byte-identically ⇒ `libabhost.so` does NOT change ⇒ **zero blast radius
  on rio/seasons/space**.

**The first APK**

- `build-apk.sh star-wars` ⇒ `games/star-wars/work/apk_signed.apk`
  (51 177 023 B).
- `com.rovio.angrybirdsstarwarshd.premium.iap`, versionName 1.5.13,
  versionCode 151300, minSdk 16 / targetSdk 26, `native-code: arm64-v8a`
  only (the arm7 guest rides as an asset).
- The guest inside the APK: STORED, sha256 = the profile's ✓.
- The host lib inside: the same md5 `167f1c08…` as the three shipped titles
  ⇒ the shared library is untouched by this port.
- Signing: v1+v2+v3 with the local key (CN=dbg) — the identity the device
  already has.

**Not done yet** (the device was switched off for the night):

- No device run at all: no boot, no menu, no gameplay.
- The **IAP lock** is the open unknown: the guest carries a full IAP system
  (`iapBuyItem`, `iapGetItemAt`, `g_iapEnabled`, `gamepurchases.lua`,
  `episodepurchases.lua`, `completeExternalPurchase`), and the premium
  build's Java side uses the old `com.android.vending.billing`. If the free
  episode is not enough to reach the gameplay, the fallback is
  `star-wars-ii` 1.9.25 (the `.ads` build: the same engine, no IAP) — the
  harness work would be identical.
- The asset keys: being hunted in the Ghidra pseudocode of the whole .so
  (`Ghidra_AngryBirdsStarWars/out/`), following Rio's method
  (`../rio/work/KEYS.md`: a hardcoded 32-byte filler → the AES key setup).
  The registry key K_r is engine-shared (verified for Seasons); to be
  confirmed here. Every key must be proven by a live decrypt of a real file
  from this APK, not by spotting a constant.

**Next**

1. The keys (subagent) + the proof by decrypting a real asset.
2. The device: install, boot, the menu, the markers, the unknown-import tail
   (the first place a missing import will show up).
3. The native control run (the original arm7 under the device's translator —
   the method is proven as of 2026-10-22) ⇒ the golden reference state.
4. The speed gate with the same stopwatch protocol (logo → menu).
5. The blast radius: rio/seasons/space on the same lib — the lib did NOT
   change, so a light re-run (markers + the golden checks) suffices.

## 2026-10-22 (night, part 2) — the hidden defects the first run would have shipped

The user installed the first APK on a real phone: **it runs**. Then we went
looking for what was hidden, without a device, and found two real defects in
the shared harness -- both found by reading the GUEST's code, not by guessing.

### 1. The guest builds its own character-classification tables -- we answered 0

The Star Wars guest (like seasons and space) imports `btowc`, `wctob` and
`wctype`. The native harness showed all three as unimplemented stubs, and the
disassembly shows what the guest does with them (0x5c1f8c):

    5c1f9c:  blx wctob@plt        ; loop over the bytes 0..127
    5c1fb6:  blx btowc@plt        ; loop over the bytes 0..255
    5c1fd0:  mov  r0, r6
    5c1fda:  bl   0x5c1c94        ; -> wctype, 16 entries

So the guest FILLS ITS OWN TABLES from these answers at init. Our stub
answered 0, so the tables were all zeros: no letter, no digit, no space
recognized. That is a silent wrongness -- the game keeps running, the text is
just wrong (and any isspace()-driven layout or parsing misbehaves).

Fixed: `btowc` / `wctob` / `wctype` are now implemented in the harness, with
the SAME classification the guest already reads through its `_ctype_` data
import (the BSD classes U=1 L=2 N=4 S=8 P=16 C=32 B=64 X=128). One source of
truth for both (a single `BionicCtypeClass()` helper), because the two tables
have to agree. `btowc`/`wctob` answer WEOF/EOF (-1) for a char the C locale
cannot represent -- the guest's loop literally stops on EOF.

Measured effect on the shipped titles: **none** (the native baselines do not
move: rio 948 svc, seasons 3072, space 1329 -- with and without the fix).
For Star Wars the tables go from zeroed to correct.

### 2. getauxval(AT_HWCAP) -- the guest's CPU-feature probe

All four guests import `getauxval`. The Star Wars guest asks for AT_HWCAP
(16) and AT_HWCAP2 (26) and tests the HWCAP bit 12 (FAST_MULT) to decide
whether it may use the fast multiply instructions, emulating them in
software when the bit is clear (0x6708e4).

`getauxval` was an unimplemented stub (answer 0). It is now implemented: the
arch-independent entries (page size, uid/euid/gid/egid, clk_tck, platform
"v7l" -- a string in the guest's memory) are answered honestly, and
AT_HWCAP/AT_HWCAP2 answer **0** ("no features").

Why 0 and not the armv7-a mask: claiming the mask (the JIT emulates every
one of those instructions, so it would be TRUE) takes the guest onto its fast
path, and that MOVES the shipped titles onto a different code path --
measured: rio 948 -> 949 svc, seasons 3072 -> 3073, space 1329 -> 1330. That
is a one-line flip, but it moves four titles at once and needs the golden
level checks on the device before it ships. The guest's fallback path is
supported by the game itself and gives identical results, so the safe answer
stays 0 until the checks can be run. Decision point, documented here.

### What the device run of the first Star Wars APK showed

- The APK installs and boots; `init done: 96/97` (the same as the native
  harness), the guest's sha256 verified, all markers 0 (HEAPCORRUPT,
  HEAPASSERT, CHUNKD, USAGE_ERROR, LOUD, STUCK, FATAL), death.log 0 B, the
  production log 19 753 B.
- **Zero `[stub]` lines**: every import the guest actually calls is
  implemented. The only tail entries are the DNS probes of the network
  stack (getenv LOCALDOMAIN / RES_OPTIONS, __system_property_get net.dns3).

### Open / next

- The wide-ctype fix changes the CONTENT of the tables for seasons and space
  too (their tables were zeroed before). Their APKs still carry the old
  library, so the shipped artifacts are untouched -- but the golden level
  check has to be re-run before the new library goes into their APKs.
- The IAP lock: still unverified (does a level actually start?).
- The asset keys: the subagent is still hunting them in the Ghidra
  pseudocode; every key must be proven by a live decrypt.

## 2026-10-22 (night, part 3) — the keys (found, and independently re-verified)

The engine's keys are hardcoded, not device-bound. They are recorded here
because this journal is the single log (treat the section as private; the
copy for the tooling lives in `work/KEYS.md`, which is gitignored).

### K_g — the game assets (levels, scripts, catalog) — PER GAME

- ASCII (32 B): `An8t3mn8U6spiQ0zHHr3a1loDrRa3mtE`
- Hex: `416e3874336d6e38553673706951307a4848723361316c6f44725261336d7445`
- Source: the hardcoded 32-byte filler `FUN_000519e4` @ 0x519e4 -> the AES key
  setup `FUN_00427404` @ 0x427404 -> `FUN_00426604` @ 0x426604 ->
  `FUN_004c91dc` @ 0x4c91dc -> the key schedule `FUN_004c9284` @ 0x4c9284
  (sbox 0x7b9184, rcon 0x7b9284). The factory is called from 0x5bfc8,
  0x8fd10 (the level loader), 0x90cb4, 0x912b4, 0x99d08. The decrypt path:
  `FUN_004274dc` @ 0x4274dc -> the CBC chain `FUN_004d73ec` @ 0x4d73ec
  (`memset(auStack_6c,0,0x40)` => a zero IV) -> `FUN_004d0e9c` @ 0x4d0e9c.
- Mode: AES-256-CBC, zero IV, PKCS#7. Container: the 9-byte magic
  `89 4c 5a 4d 41 0d 0a 1a 0a` ("LZMA-alone"), then an LZMA-alone stream
  (props 5d, dict 64 KB); the plaintext is Lua 5.1 bytecode with 4-byte
  floats.
- **Re-verified by me (not only by the subagent that found it):**
  `assets/data/levels/MainMenu.lua` (1840 B) -> the magic
  `894c5a4d410d0a1a0a` -> LZMA -> 4294 B of Lua bytecode (header
  `1b 4c 75 61`); `assets/data/catalog.json` (752 B) -> JSON (27 entries,
  the first "Mighty Falcon Small bundle"); `assets/data/scripts/options.lua`
  -> a raw Lua value stream WITHOUT the LZMA magic (the one exception to the
  "by magic" check); **604/604** of the APK's encrypted assets decrypt (603
  LZMA+Lua, 1 without LZMA), 0 failures.
- The asset key is PER GAME (Rio's K_g and Seasons' K_a are different) --
  the user's guess was right.

### K_2 — the purchase tables (gamepurchases / eaglepurchases / episodepurchases)

- ASCII (32 B): `e83Tph0R3aZ2jGK6eS91uLvQpL33vzNi`
- Hex: `653833547068305233615a326a474b3665533931754c7651704c3333767a4e69`
- Source: `FUN_00051b94` @ 0x51b94; the factory is reached at 0x92598
  (decrypt), 0x9cf14 / 0x9d1a4 (encrypt), 0x69f94
  (GameLua::prepareRovioChannel).
- Status: static evidence + a guest round-trip (71 B of JSON -> 80 B of
  ciphertext -> back). NOT confirmed against a file: this class is not in the
  APK (0 of 1653 assets) -- the tables are created/downloaded at runtime.

### K_r — the registry (fusion.registry) — SHARED with Rio and Seasons

- Hex (32 B): `3a7d2e0379e64985a01fa80104d5d77da1bc7ae70363248e7ac9c0ad5f4660ea`
- AES-256-CBC, zero IV, app-fixed. Source: `FUN_003a8590` @ 0x3a8590
  (encrypt), `FUN_003a8944` @ 0x3a8944 (decrypt).
- **Re-verified by me on THIS game's own registry** (pulled from the device
  after the first run): 96 B ->
  `{"ads":{"installTracker":null},"pf::LocalNotifications::sm_pendingNotifications":null}`
  plus the Crypto++ pad; the control (the asset key) gives garbage.

### The IAP lock does NOT block the port

The decrypted scripts settle it: `game.lua` carries the strings "is
unlocked", "is unlocked, first episode is unlocked" and "is locked, none of
the bonus levels is open", and there is a `levelOrder_free.lua` next to
`levelOrder.lua`. So the FIRST episode is unlocked without any purchase --
the premium content is a content matter (a purchase table), not a porting
blocker.

### Also measured

- The first launch needs NO seeds: the game created `bi_data.lua`,
  `cacert.pem`, `settings.lua` and `fusion.registry` by itself on the device
  (the same file names Rio's seeds carry).
