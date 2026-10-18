# Angry Birds Rio — port journal

**Status: SHIPPED** (tag `works-2026-10-18-batch-g`). Runs the untouched ARM32
guest under the Dynarmic host, offline, standalone; menu, episodes and levels
play with sound on the test device (user-verified 2026-10-18).

Profiles and machine-readable facts: `profile.toml` (same directory).
The full dated investigation log for this title is **`../../AGENTS.md`** — it is
still the Rio journal (141 entries) and is the source of truth for anything not
restated here.

## Port recipe

The complete, machine-verified edit set (PROVEN: a fresh `apktool d` of
`original/` diffed against the shipped tree differs in nothing else):

1. `lib/armeabi-v7a/libAngryBirdsRio.so` → `assets/guestlib/armeabi-v7a/` (STORED)
2. delete `lib/armeabi-v7a/` and `lib/x86/`
3. `lib/arm64-v8a/libabhost.so` — our host library (from the build)
4. `seeds/` → `assets/seeds/` (first-launch files, STORED)
5. `smali/com/rovio/fusionapp/Gen.smali`: loaded lib name `AngryBirdsRio` → `abhost`
6. `apktool.yml` `doNotCompress` += guest `.so` + seeds (the loader reads them
   raw, so they must not be deflated)

Automated by `scripts/prepare-tree.sh rio`. The guest `.so` is never patched;
its sha256 is checked at package time against `profile.toml`.

Dead SDK libraries removed: `libjs.so`, `libadcolony.so` — PROVEN byte-identical
across the whole series (`libjs.so` sha256 `5eb5b260…`, `libadcolony.so` `71b9c4fb…`).

## Ship config (the boot `flags:` line is the single source of truth)

```
fastmem=on mallocpad=0 zerofill=off heapwatch=off norelease=off clampwrite=off(+jni bound) diag=off jitopt=safe
```

Always-on behaviour crutches (behaviour, not diagnostics — do not "clean"):
RETSUCC, SPURWAKE, REALGL, FASTMEM, `HARNESS_PROBE=1a410c,fill`.

Diagnostics: exactly one switch, `HARNESS_DIAG` (env or `files/abflags.txt`).
OFF in prod. Never leave `abflags.txt` on the device.

## Verification

- Native: `./host/build-native/harness libAngryBirdsRio.so` → expect
  `init done: 54/54 clean, svc=948 halts=53`.
- Packaging: `scripts/deploy.sh rio` (local only; prints the device half and the
  md5 to confirm against the lib extracted from the INSTALLED `base.apk`).
- On device mark a run boundary, then grep the markers **separately** (Termux
  `grep` does not honour `\|`): HEAPCORRUPT, HEAPASSERT, CHUNKD, USAGE_ERROR,
  LOUD; `files/death.log` must be 0 B and no new tombstone.

## Device

SSH `u0_a41@<lan-ip>:8022` — the address is **DHCP and changes** (after the
2026-10-18 power cut it moved `.202` → `.171`; rediscover with a port-8022 scan
and check the host key). App uid 10051, `/data/data/com.rovio.angrybirdsrio` is
`drwx------` (use `sudo` for everything inside). Android 7.1 / SDK 25.
The user drives interaction by hand — synthetic taps are unreliable here.

## Re-check after the shared-host changes (2026-10-19)

The host library is shared by every title, so host changes are re-tested here.
Two commits landed after this tag: `2bb8083` (loader refuses guest writes into
`PT_GNU_RELRO`) and `79538ef` (`pthread_once` gets its own return trampoline; the
nested-hijack stack became thread-local). Mechanism, evidence, and the remaining
deviation: `../space/JOURNAL.md`.

Re-verified on the device with the new lib (md5 of the lib inside the installed
`base.apk`: `50a2933c31a582b2cb0f74d030b54d3f`; three prod-like launches, no
`files/abflags.txt` present):

- loads visually and is ready to play (user-verified while running);
- `pthread_once` **is** reachable at runtime here: 5 hijacks per launch, and none
  of them in the init array (`svc=948 halts=53` unchanged, i.e. no init return
  goes through a once trampoline). All five clean — `hijack == done`,
  `tid == owner_tid`, `STEAL = 0`, `BAD exec = 0`, `FATAL abort = 0`;
- the tail markers of a Rio start (`[cond] wait … -> 0`, `[mutex]
  UNLOCK-FOREIGN`, `[unwind] LOUD nesting 513 deep -- halting`) are pre-existing,
  not caused by these commits: the same guard and the same mutex tracking are
  present verbatim in this tag.

No new tag without explicit confirmation.
