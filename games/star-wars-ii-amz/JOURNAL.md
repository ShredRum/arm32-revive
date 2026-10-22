# Angry Birds Star Wars II — Amazon build ("asset pack") — port journal

The same game as ../star-wars-ii (versionCode 192500) but the Amazon Appstore
build, found on web.archive: it ships the Rovio CONTENT SERVICE data inside
the APK, so it can bootstrap without the (long dead) backend. It is a SEPARATE
title id on purpose: a different package name
(com.rovio.angrybirdsstarwarsii.ads.amz), so it installs side by side with
the regular build and both experiments stay alive.

## 2026-10-22 — the analysis, then the port

**The analysis (before touching the tree).** Measured, not assumed:

- The guest is a DIFFERENT build: 7 989 612 B, sha256 `4269b18c…` (the
  regular 1.9.25 is 8 296 812 B, `b7152ac9…`). Same engine, same NEEDED set
  (7), same init_array size (0x240 = 144 entries), same 74 JNI exports, and
  two imports FEWER (no atoi, no tmpfile) ⇒ no new import family.
- **The asset key is the same**: K_g of the regular build decrypts this
  build's assets (40/40 files with the LZMA magic and a clean
  decompression; the Star Wars I key fails on the same file with
  "bad decrypt"). The key is a hardcoded constant of the source, so it
  carries across builds. K_r (the registry) is the engine-shared one.
- The JNI delta: 11 native methods are declared in the smali that the
  regular build does not have, 5 of the regular ones are gone. Of the 11,
  exactly 5 are exported by the guest -- the Amazon payment provider
  (AmazonPaymentProvider.requestProducts,
  AmazonPurchasingListener.{itemDataResponse,purchaseResponse,
  purchaseUpdatesResponse,restoreDone}) -- and the other 6 are the dead
  paths of this build (LocationWrapper x4, Share.onShareClosed,
  VideoPlayerBridge.onVideoPaused). The 69 common methods have identical
  descriptors (0 mismatches). So the bridge needed exactly five new
  forwarders; the union went 75 -> 80, purely additive.
- The lib is loaded under a name WITHOUT the "lib" prefix in this build
  ("AngryBirdsStarWarsII", not "libAngryBirdsStarWarsII"); the patch target
  is the same file (Gen.smali's <clinit> fills Gen.library_name).
- The assets: 1934 entries (448 deflated + 1470 stored), the largest 5 MB --
  well under the AAsset bridge's 32 MB limit.

**The port**

- `prepare-tree.sh star-wars-ii-amz`: the guest .so moved into
  assets/guestlib/armeabi-v7a/ (STORED, sha256 re-verified after the move),
  lib/armeabi-v7a and lib/x86 removed, Gen.smali patched to "abhost",
  doNotCompress updated.
- The JNI signature list (74 entries) from the guest's exports and the
  decoded smali; the union regen added the five Amazon forwarders.
- The native baseline: init done 143/144 clean, svc=4457, and the only stub
  in the tail is __cxa_atexit (the documented "not implemented" one).

**The defect the first run exposed (and it was OURS)**

The port booted, then died: `[svc] LOUD unimplemented JNI fn#50 from
pc=f00005cc -- halting`. The fn number is the index in the JNI function
table, and the harness's own numbering is the standard one (case 6 =
FindClass, case 215 = RegisterNatives). Index 50 = CallIntMethodV. The
harness implemented the Object (34-36), Boolean (37-39), Long (52-54) and
several other families -- but the Int family (49/50/51) was missing, and the
code even said so: "instance-int is 49/50/51, never hit". This build is the
first to hit it, and the unimplemented branch HALTED the guest, which took
the app down (the screen was the Android app list).

Fixed: the Int family now answers the offline default 0, exactly like the
Boolean family (false) and the Long family (0), and the unimplemented
branch now names the method (the descriptor is in the mock's method table)
so the next title does not need a DIAG run to identify the caller.

**After the fix, on the device**

- All markers 0, death.log 0 B, init 143/144, the process stays alive, and
  the game reaches its MAIN MENU (the shop, the settings, Toons.TV and the
  currency counter are on screen). Over the menu the game shows its own
  offline prompt ("get free characters by turning your internet connection
  on" + TRY AGAIN) -- the content service did not answer, which is the same
  thing the regular build does, only it stops at the account wall.
- The guest's log lines are the same as the original's: the same
  assets_service misses (ShopData, RewardData, CameraData x2, UpdateData,
  quests) and the same first-launch file creation.
- The "SIGNED OUT" screen belongs to the Amazon-signed ORIGINAL, not to our
  port: it is the game's Rovio-account wall (the device has no account). Our
  port's first run never reached that point (it halted earlier). Whether the
  same wall appears in our port after the offline prompt is dismissed is
  still to be checked.

**Still open**

- The golden check of Seasons' Tutorial_red on the new library (it changed:
  five JNI forwarders added, the JNI marshalling padding removed) -- it
  needs the level to be entered once.
- Whether our port of this build reaches the same place as the original
  after the offline prompt is dismissed.
