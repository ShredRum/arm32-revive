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

One further host change landed right after (`9a42892`): the *normal* `[once]`
lines (`hijack`, `routine done`) moved behind `HARNESS_DIAG` (anomalies stay
loud). It is log-only, but the installed lib changed again
(`5c49beed299e7132ee73d18fef2855df`), so Rio was prod-run once more on it:
354 lines instead of 364 -- exactly the ten once lines gone, `hijack = 0`,
`done = 0`, `abort = 0`, process alive, flag file absent.

Then the remaining *normal* dumps followed under the same flag (log-only):
`[fcntl]` 58, `[files] fopen(...) -> NULL` 32, `[realloc] pre` 30, `[strdup]` 20,
`[cond]` 4, `[netsock]` 5, `[jni] region` 8, `[poll]` 4, `[alarm]` 3 and the
repeating `[realgl] glShaderSource ... ok` 12. Rio's prod log: 354 -> **168**
lines; with `HARNESS_DIAG=1` everything comes back. Lib
`b8804206ec9cc2e3025a7778bba98fec`, installed and md5-checked.

Rio is the **pinned image** (its `LOUD build-id mismatch` line is absent, Space's
is not), so `HARNESS_PROBE=1a410c,fill` is armed for it: 15 of the 168 remaining
lines are that probe's dump. They belong to the `PROBE` crutch (PLAN.md backlog
item 3) and must not be mistaken for a regression.

### Re-check: the `cond_wait` cap + `HARNESS_SPURWAKE` removal (2026-10-19) -- MEASURED

The change (PLAN.md backlog item 2): the untimed `pthread_cond_wait` no longer
runs on a host `pthread_cond_timedwait(now+5 s)` whose `ETIMEDOUT` was turned
into a spurious success -- it blocks until signalled, as bionic does; the 5 s
clamp of a guest-supplied `pthread_cond_timedwait` deadline is gone too, and
`HARNESS_SPURWAKE` / `HARNESS_NOCAPWAIT` no longer exist. Rationale, the
pre-change measurement (the cap was **not** load-bearing) and Space's numbers:
`../space/JOURNAL.md`.

Installed lib md5 `76f2342b4d3b9dc19896b64c05c6dcb1` (checked inside the
installed `base.apk`; it is the same `libabhost.so` both titles carry). Prod
run, 75 s, no `files/abflags.txt`, sampled at 25 s and 75 s:

| metric | before (`b8804206`, cap+SPURWAKE) | after (`76f2342b`, honest wait) |
|---|---|---|
| log lines | 168 -> 171 | 168 -> 171 |
| `[regio]` | 60 | 59 (the 60th lands on the window boundary) |
| abort / HEAPCORRUPT / `[mem] BAD` | 0 / 0 / 0 | 0 / 0 / 0 |
| `[guest] LOUD` | 0 (pinned image) | 0 |
| process | alive, 64.8 % CPU, RSS 40 MB | alive, 60.4 % CPU, RSS 45 MB |
| tail | `[mutex] UNLOCK-FOREIGN` + `[unwind] LOUD nesting 513 deep` | identical |

The same guest milestones are reached (`[regio] fwrite #58`, `fread #59/#60`;
the `#N` counter is the guest's own file-call sequence, so its exact split
differs run to run). The tail markers are pre-existing (see the section above),
not a consequence of this change. Rio is behaviourally unchanged.

### Re-check: `HARNESS_RETSUCC` removal (2026-10-19) -- MEASURED

The shipping `setenv("HARNESS_RETSUCC","1")` is gone (PLAN.md item 1). It was
dead code: the injection hangs off the diag-only PROBE3 hook, and its target
raiser never translated in any diag log. Rio's prod log loses exactly the false
`RETSUCC re-armed (real fail path stalls)` line, and nothing else.

Installed lib md5 `14c43eda5500250bfdb16aeb43a35180` (same lib in both titles;
verified inside the installed `base.apk`). Prod run, 75 s, no abflags:

| metric | before (`76f2342b`) | after (`14c43eda`) |
|---|---|---|
| log lines | 168 -> 171 | 167 -> 170 (-1, the RETSUCC line) |
| `[regio]` | 59 | 60 |
| abort / HEAPCORRUPT / `[mem] BAD` | 0 / 0 / 0 | 0 / 0 / 0 |
| `[guest] LOUD` | 0 (pinned image) | 0 |
| process | alive, 60.4 % CPU | alive, 63.1 % CPU, RSS 67 MB |
| tail | `[mutex] UNLOCK-FOREIGN` + `[unwind] LOUD nesting 513 deep` | identical |

Rio is behaviourally unchanged. No new tag without explicit confirmation.

### Re-check: `HARNESS_PROBE=1a410c,fill` removal (2026-10-19) -- MEASURED

The pinned-image fill (PLAN.md item 3) is gone. Its own prod verdict was
`[probe] OUT non-empty, kept`: the probe fired at 0x1A410C on every run, but the
SSO slot was already filled by the guest, so the `"{}"` write never happened --
the only effect was 16 log lines (1 `[probe-emit]` + 15 `[probe]`). The
`,fill` code stays as an opt-in experiment, and the probe-slot comparison now
requires the slot to be non-zero (an unset slot is 0 and would have matched
`pc == 0`).

Installed lib md5 `ac70bd93ad4a29ca44a0b20969833674` (both titles carry the same
lib; checked inside the installed `base.apk`). Prod run, 75 s, no abflags:

| metric | before (`14c43eda`) | after (`ac70bd93`) |
|---|---|---|
| log lines | 167 -> 170 | 151 -> **154** (-16: the probe block) |
| `[probe]` / `[probe-emit]` | 15 / 1 | 0 / 0 |
| `[regio]` | 60 | 60 |
| abort / HEAPCORRUPT / `[mem] BAD` | 0 / 0 / 0 | 0 / 0 / 0 |
| process | alive, 63.1 % CPU | alive, 65.7 % CPU, RSS 38 MB |
| screen | menu | menu (screenshot, 2.46 MB PNG) |

Space ran as the control in the same install (it is not the pinned image, so it
never had the probe): 212 lines before and after, `[probe]`=0, menu on screen.

### Re-check: tapbeat JNI resolve gated (2026-10-19) -- MEASURED

The input-driver scaffolding (PLAN.md item 6) resolved the genuine
`MyInputHandler` through JNI on the first `nativeUpdate` even with the driver
off: two `NewGlobalRef`, `GetObjectClass`, `GetFieldID`, `GetObjectField`,
`WrapObject`, plus a `[tapbeat] handler mock=` line. It now runs only when
`HARNESS_TAPBEAT` is set.

Installed lib md5 `ae1645e2e0ac9c2af19b1e5d24acff2d`. Prod run, 75 s:

| metric | before (`ac70bd93`) | after (`ae1645e2`) |
|---|---|---|
| log lines | 151 -> 154 | 150 -> **153** (the tapbeat line is gone) |
| `[tapbeat]` / `[probe]` | 1 / 0 | 0 / 0 |
| `[regio]` | 60 | 58 (guest's own call counter; 58-60 run to run) |
| abort / `[mem] BAD` / `[guest] LOUD` | 0 / 0 / 0 | 0 / 0 / 0 |
| process | alive, 65.7 % CPU | alive, 59.5 % CPU, RSS 38 MB |
| screen | menu | menu (screenshot) |

Space in the same install: 212 lines (unchanged), `[tapbeat]`=0, menu.
No new tag without explicit confirmation.

### Re-check: honest sleeps (2026-10-19) -- MEASURED

The 5 ms clamp is gone from `nanosleep`, `usleep`/`sleep` and the
`ANDROID_ALARM_SET_AND_WAIT` ioctl (the last one was mislabelled "clock_nanosleep"
in the first audit; there is no such handler). One helper, `GuestSleepUs()`,
blocks the calling thread for the full requested time, as bionic does. Dormant
for Rio (it imports no sleeps), so this run is a control for the shared lib.

Installed lib md5 `9dfa10872d0fd60806889f7c31901fa3`. Prod run, 75 s:

| metric | before (`ae1645e2`) | after (`9dfa1087`) |
|---|---|---|
| log lines | 150 -> 153 | 150 -> 153 (unchanged) |
| `[regio]` / stall | 58 | 60, no stalled window |
| abort / `[mem] BAD` / `[guest] LOUD` | 0 / 0 / 0 | 0 / 0 / 0 |
| process | alive, 59.5 % CPU | alive, 63.6 % CPU, RSS 33 MB |
| screen | menu | menu (screenshot) |

Space in the same install: 213 lines (`[regio]` 60, `glog` 61; the +1 vs 212 is
the benign `[mutex] UNLOCK-FOREIGN` marker, which varies 4-5 per run), no stall,
menu. No new tag without explicit confirmation.

### Fixed: exidx ABI — the 0x45A778 spiral was ours (2026-10-19)

`__gnu_Unwind_Find_exidx` must return the exidx table base and write `*count`.
The pinned Rio image was kept on the legacy "single entry, no count" answer
(6de206a) because its fail path "depended" on the spiral. The guest checks
`count==0` and marks the frame `_unwindInfoMissing`: phase1 fails before any
descriptor runs. On a phone the first main-thread throw (fresh seed registry has
no `cloud.session_configuration` -> JSON ParseError) became the black screen;
the fill probe was masking exactly that throw.

Fix (4af8d8a): every image gets the ABI answer; SKIPFRAME/R7POKE removed (they
poked the live getInfo R7, not the context R7).

Harness, fresh 224 B seed registry (phone state), `HARNESS_EXIDXLOG=1`:
45A778 -> 457FE5 (__cxa_throw) -> 2A6E6B -> ... -> 1A413B, 17 lookups, no
`[unwind]`/BAD/FATAL, init 54/54, `sesswatch exit flag=1`. Space init 67/67.

Phone build (`apk_signed.apk`): md5 `d7506295a55f21d954f049a51327ca0c`, lib
`24b221a31da61916651ea0153e0f9caf`, no fill, versionCode 2676. Waiting for the
device run; `apk_fill.apk` kept only as the fallback artifact.
