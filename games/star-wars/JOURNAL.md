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
