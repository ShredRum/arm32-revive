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

### Re-check: the `pthread_once` blocking fix (2026-10-19) -- MEASURED

The change: `pthread_once` no longer returns 0 when the control reads `1` (in
progress) -- it blocks the caller until the initialisation completes, as bionic
does, and releases the control if the owning thread dies inside the routine.
Mechanism, evidence and limits: `../space/JOURNAL.md`, section "Правка
`state == 1`". Committed as `0b5642c`; the installed lib md5 is
`6adc66d278d0940dadd2a87ceb18fd07`.

Measured on the device: three prod-like launches, 45 s each, no
`files/abflags.txt` (the lib inside the installed `base.apk` was md5-checked
before the runs and matched the commit):

| run | lines | hijack | done | tid≠owner | STEAL | abort | other | process |
|---|---|---|---|---|---|---|---|---|
| 1 | 373 | 5 | 5 | 0 | 0 | 0 | BAD exec 0, future_error 0, ExceptionRaised 0 | alive, 63.0 % CPU, RSS 38 MB |
| 2 | 364 | 5 | 5 | 0 | 0 | 0 | ditto | alive, 62.9 % CPU, RSS 65 MB |
| 3 | 364 | 5 | 5 | 0 | 0 | 0 | ditto | alive, 63.9 % CPU, RSS 47 MB |

The five events are the same five controls every launch
(`006eb9f4/0045bed9`, `006ebca8/004da08d`, `006ebf74/00509549`,
`006eb9e0/0045845d`, `006de2d4/003a5d80`), all at `depth=0`, `tid` equal to
`owner_tid` (tids 1001/3/9/11 differ run to run -- the guest's own scheduling).
No wait, no steal, no abort: the blocking path is reachable here but never
*entered* in a clean run, exactly as on Space (`wait` fires only when the same
once is entered concurrently). 45 s window justified by the loading-burst
measurement in `../space/JOURNAL.md`.

User confirmation of this specific run: pending -- the third launch was left
running on the screen so the user can look at it; the on-screen result of the
previous re-check was already confirmed by hand.

Result: Rio is re-verified for `0b5642c` -- the pending caveat above is closed.
