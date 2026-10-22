# Angry Birds Star Wars II 1.9.25 — port journal

The port core is title-agnostic (see ../seasons/JOURNAL.md for the shared
harness history); this file is the Star Wars II log.

## 2026-10-22 (night) — the port starts: the tree, the JNI bridge, the first APK

**Measured before touching anything:**

- The Java side is the same `com.rovio.fusion` framework as the shipped
  titles (App, `com/rovio/fusionapp/Gen`, the wrappers) ⇒ the same bridge,
  the same activity, the same lifecycle.
- The guest `libAngryBirdsStarWarsII.so`: armeabi-v7a, 8 296 812 B, sha256
  `b7152ac9…` (re-verified from the APK), NEEDED = libandroid / libGLESv2 /
  liblog / libstdc++ / libm / libc / libdl (the same set), no build-id,
  DT_INIT_ARRAY = 0x240 B = **144** entries (Seasons 143, Star Wars I 97).
- The imports: **321** undefined symbols -- against Seasons only `ceilf` and
  `powf` are new (math). The ones that differ from Star Wars I
  (`_exit`, `ftruncate`, `mkstemp`, `__page_size`, `uname`,
  `__android_log_write`) are all ALREADY implemented in the harness.
- The guest imports `btowc`/`wctob`/`wctype` and `getauxval` ⇒ it gets the
  wide-ctype fix and the honest AT_HWCAP answer for free.
- The assets: 1189 entries under assets/data (410 deflated, 779 stored); the
  largest is 5 MB, well under the AAsset bridge's 32 MB limit. The bridge
  inflates the deflated ones (raw inflate via zlib).
- The native harness runs the guest's init array cleanly:
  **init done: 143/144 clean, svc=4457**, and the only stub in the tail is
  `__cxa_atexit` (the documented "not implemented" one) ⇒ the loader and the
  import set need nothing new.
- The keys: the Ghidra pseudocode of this guest is already dumped
  (`Ghidra_AngryBirdsStarWars2/out/`), so the key hunt used the same method
  as for Star Wars I.

**The tree and the bridge**

- `prepare-tree.sh star-wars-ii` ⇒ the decode + the port edits (the guest
  .so → `assets/guestlib/armeabi-v7a/` STORED, `lib/armeabi-v7a` and
  `lib/x86` removed, `Gen.smali`: `"libAngryBirdsStarWarsII"` → `"abhost"`,
  `doNotCompress` updated). The guest's sha256 was re-verified after the
  move ⇒ the guest is moved, never modified.
- The JNI signatures: 74 entries, from the guest's exports and the decoded
  smali (the same validated tool: it reproduces the Seasons and Space files
  byte for byte). 72 of them are already forwarded by the union; **two are
  new** and both belong to a class no other title exports:
  - `com/rovio/fusion/CameraWrapper nativeCameraStatus (JILjava/lang/String;)V`
  - `com/rovio/fusion/CameraWrapper nativeProcessFrame (J[BII)V`
  (the camera / telepod path). The smali declares both as `void`, so the
  generated `void` forwarders are right.
- The union grew from 73 to 75 forwarders; the diff is purely additive (the
  existing 73 are byte-identical) ⇒ `libabhost.so` changes, so all five
  titles have to be re-run on the device.

**One latent trap found and fixed while looking at this**

The JNI marshalling padded 64-bit arguments to an 8-byte-aligned register
pair -- the AAPCS32 rule. The guests are built with the **base** procedure
call standard (arm-linux-gnueabi), where a 64-bit argument may sit in an
odd-numbered pair; that is not theory, the guest's own variadic printf was
measured taking a double in the NEXT TWO registers with no even-pair padding
(2026-10-20). The padding only ever showed up on the methods where a 64-bit
argument FOLLOWS a 32-bit one -- exactly one method per title
(`com/rovio/rcs/core/Utils.handleReferrerValue`, the analytics referrer
tracker, never reached offline), which is why it stayed latent. The padding
is removed; every other method's layout is unchanged (their 64-bit
arguments come first, where no padding was inserted anyway).

**The keys** (found in the pseudocode, re-verified independently of the
subagent that found them):

- **K_g -- the game assets, PER GAME**:
  ASCII `B0pm3TAlzkN9ghzoe2NizEllPdN0hQni`,
  hex `4230706d3354416c7a6b4e3967687a6f65324e697a456c6c50644e3068516e69`.
  Source: the hardcoded 32-byte builder `FUN_0005e6ac` @ 0x5e6ac → the AES
  key setup `FUN_00444058` @ 0x444058 → `FUN_00443258` @ 0x443258 → the key
  schedule `FUN_00539a70` @ 0x539a70; the CBC chain `FUN_00547bd8` @ 0x547bd8
  zeroes the IV (`memset(auStack_6c,0,0x40)`) and `FUN_00541688` @ 0x541688
  does the block decryption.
  Verified by me: `assets/data/levels/chapter_1_light/Chapter_1_light_Comic_1.lua`
  → the LZMA-alone magic → 633 B of Lua 5.1 bytecode (`\x1bLuaQ`);
  `assets/data/animations/Levelend/LEVELEND_BIRDS_FAIL_LOOP.json` → valid
  JSON; the Star Wars I key fails on the same file ("bad decrypt") ⇒ the key
  is this game's. Mass check: **741/741** encrypted assets decrypt (740
  LZMA+Lua/JSON, 1 without LZMA: `assets/data/scripts/options.lua` is a 7z
  archive), 0 failures.
- **K_2 -- the purchase tables**: ASCII `taT3vigDoNlqd44yiPbt21biCpVma6nb`
  (source `FUN_0005e85c` @ 0x5e85c). Static evidence + a guest round-trip
  only: this class of file is not in the APK (the tables are created at
  runtime).
- **K_r -- the registry, SHARED with Rio/Seasons/Star Wars I**:
  `3a7d2e0379e64985a01fa80104d5d77da1bc7ae70363248e7ac9c0ad5f4660ea`.
  Verified by me in this guest's own pseudocode: `FUN_003d2994` (and
  `FUN_003d74cc`) build it byte by byte -- `local_40[0] = 0x3a;
  local_40[1] = 0x7d; local_40[2] = 0x2e; local_40[3] = 3; local_40[4] =
  0x79; …` -- exactly those bytes.

**The first APK**

`games/star-wars-ii/work/apk_signed.apk` (50 329 076 B),
`com.rovio.angrybirdsstarwarsii.ads` 1.9.25 (versionCode 192500),
minSdk 16 / targetSdk 26, `native-code: arm64-v8a` only, the guest inside
STORED with the profile's sha256, signed v1+v2+v3 with the local key.

**Not done yet** (the device was switched off for the night):

- No device run at all: no boot, no menu, no gameplay.
- The five titles have to be re-run on the new library (it grew by two
  forwarders): the markers for all of them and the golden check for
  Seasons' Tutorial_red.

**Still to come back to (from the Star Wars night)**

- A **differential dump of the guest's memory after init** -- our port versus
  the native reference (the original arm7 under the device's translator) --
  as a systematic detector for "tables the guest builds from our answers".
  The wide-ctype defect was found by reading the guest's disassembly; a
  differential dump would catch that whole class at once, for every future
  title, instead of one disassembly at a time.
