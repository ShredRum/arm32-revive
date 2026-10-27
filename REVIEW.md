# REVIEW.md — capital review of the shared core (started 2026-10-25)

The register of every finding of the review. Read this first when resuming.
AGENTS.md / JOURNAL.md / commit messages are treated as **hypotheses**: they were
written by small-context models and contain stale or wrong claims. Truth is the
code, the guest binary (disassembly), primary specs (NDK/Bionic headers, JNI,
GLES2, ARM AAPCS) and device behaviour.

## Rules

- A finding is fixed only after Claude verified it personally (code + spec or
  disassembly or a reproduction). Subagent reports are leads, not facts.
- One logical fix per commit, with a test where one is possible.
- Behaviour-changing fixes are batched for one device gate; the regression gate
  below must stay green after every batch.
- Preferred solution = the correct one, not the smallest. Ask the user only when
  two correct options genuinely differ in trade-offs.

## Regression gate (baseline 2026-10-25, HEAD 2cba59a)

Native harness (`host/build-native/harness <guest .so>`), guest SHA verified
against `games/<id>/profile.toml`:

| title | init | svc | halts | LOUD |
|---|---|---|---|---|
| rio | 54/55 | 979 | 54 | 0 (since a200ffe: init #22 runs; was 949/53) |
| seasons | 142/143 | 3073 | 142 | 3 (build-id pin noise, R-001) |
| space | 67/68 | 1330 | 67 | 3 (same) |
| star-wars | 96/97 | 2320 | 96 | 3 (same) |
| star-wars-ii | 143/144 | 4457 | 143 | 3 (same) |
| star-wars-ii-amz | 143/144 | 4457 | 143 | 3 (same) |

Also: `fmt_test` ALL OK (shipping + `--diag`), `zip_test`, `cbnz_test`,
`smoke_a32`; Seasons golden level (85-line state + 46 JSON fields, see
games/seasons/JOURNAL.md); device gate on the sandbox
(`ssh -p 8022 u0_a41@192.168.1.202`, markers HEAPCORRUPT/HEAPASSERT/CHUNKD/
USAGE_ERROR/LOUD/STUCK/FATAL = 0, files/death.log 0 B, process alive).

## Next steps (as of 2026-10-27)

1. Diag-only leftovers (no effect on shipping builds): Rio diag VAs armed for every title under HARNESS_DIAG (G-05/G-11, F-34), build-id pin (F-pin, R-001), HARNESS_* experiment knobs (HOSTSHACK, DNS1/2, NETTYPE), remaining DiagOn-gated TEMP-DIAG logs.
2. Reliability (owner 2026-10-27: only fixes with no runtime cost; guards against cases that do not occur are not worth a cost): F-24/F-25, C-09, C-22 (risky: may stop a title that reads NULL today), D-08/D-12/D-13, static fields 145-151, G-08/G-09/G-14, H-04, F-35/F-36; E-04 and C-11 need a measured cost first; fmt_test statfs f_namelen expects 255 (fails on the sandbox's filesystem: 256).
3. S3/S4 leftovers per area; docs: games/*/JOURNAL.md stale AGENTS section references.
4. Branch `bricks` (99 Bricks / Unity bring-up) changes the shared core. Before it
   merges, gate all five titles on the sandbox; the Rovio-visible changes are listed
   in games/99-bricks/JOURNAL.md ("Rovio-visible changes to gate").

## Areas and waves

| id | area | scope | wave | status |
|---|---|---|---|---|
| A | libc core | dispatch.cpp OnSvc/ExpandPrintf/math/CallImport malloc..poll; math_thunks.inc, bsd_random.h | 1 | |
| B | stdio/files/sockets/assets/time/props | dispatch.cpp CallImport pthread_once..end, InetThunk, MapGuestPath | 1 | |
| C | memory/heap/threads/sync/kuser | dispatch.cpp 1..1103, Pthread*, kuser, syscall, exidx; live_heap.h, guest_memory.h, layout.h | 1 | |
| D | JNI bridge + app glue | dispatch.cpp JNI part; apphost.cpp, apphost_gen.inc, gen_apphost.py | 1 | |
| E | GL | realgl.cpp + gl* in dispatch.cpp | 2 | |
| F | loader + JIT callbacks + probes | elf_loader.cpp, GuestCallbacks::*, harness.cpp | 2 | |
| G | crutches / magic numbers / HARNESS_* / dead code | whole tree | 2 | |
| H | build, packaging, scripts, smali edits | scripts/, CMakeLists, prepare-tree | 2 | |

## Findings

Severity: S1 = crash/corruption/wrong semantics on a live path; S2 = latent bug
(reachable, not observed) or wrong ABI on an unused path; S3 = crutch/magic
number/misleading doc; S4 = cosmetic.
Status: open / confirmed / refuted / fixed (commit) / wontfix (reason).

| id | sev | where | finding | evidence | status |
|---|---|---|---|---|---|
| R-001 | S4 | elf_loader.h:134 CheckGuestBuildId; harness.cpp:687 | harness-only: non-Rio guests have no GNU build-id note, harness does not verify the SHA like apphost does (apphost.cpp:1484 passes identity_verified), so it prints LOUD. Bigger question for G: the shared core pins Rio VAs/hooks at all | readelf -n: no build-id in the 5 non-Rio guests; device path prints a plain line | confirmed |
| R-002 | S2 | dispatch.cpp:4114,10071,10080,10086 | printf `%u` given size_t/unsigned long | clangd -Wformat | fixed 52120ed |
| R-004 | S2 | star-wars-ii | boot ends at "Asset downloading failed": the ORIGINAL APK does the same on a real device (content servers are dead); star-wars-ii-amz ships the assets and is the playable build | wontfix (original behaviour); 2026-10-26 owner: plain star-wars-ii is no longer built or gated, `games/star-wars-ii/` is kept |
| R-006 | S3 | getcwd / relative paths (MapGuestPath) | the bridge treats the files dir as the CWD (getcwd returns it, bare relative names go there); a real Android app's CWD is "/". The comment "original CWD = files dir" is unverified (2026-10-27: getcwd now returns the guest spelling and checks the buffer, the choice of directory was kept). Check what getcwd returns in the original APK on the sandbox | open |
| R-005 | S1 | TLS ClientHello rewrite (B-14/G-12) | LOAD-BEARING: it breaks the handshake with the live Rovio servers, which is what keeps every title on its offline path. Removing it (tried 2026-10-26, reverted, never committed) let Space reach a server that answers 573 "App is disabled" -> uncaught system_error (EDEADLK) -> abort, and star-wars-ii-amz got "Signed out" and SAVED that state (fusion.registry), so it kept aborting until the app data was cleared. 2026-10-27: the EDEADLK abort is not network-specific. With connect() failing (phone offline) Star Wars and Space aborted in ~1/4-1/6 boots on the SHIPPING build too, while the original 32-bit APK under the same strace-injected ENETUNREACH booted 6/6. Cause: the engine's HTTP worker signals "done" and only then drops its reference to the object that owns its own std::thread; if the main thread drops its reference in between, the worker holds the last one and its destructor joins itself (bionic EDEADLK, gnustl throws). Natively the window is microseconds; here a new worker spent 60-90 ms of its ~100 ms life compiling ~2800 blocks again in its own Jit. Fixed by the shared code cache (jitshare, patches/dynarmic): 17/17 offline boots clean. Network policy (owner, 2026-10-27): offline by default with a per-title switch (profile.toml host.network, packaged as assets/guestlib/<abi>/network); offline titles refuse connect/sendto to non-loopback addresses with ENETUNREACH and keep rcs networkType() on the "" mock. The ClientHello rewrite and the 10 s connect crutch are gone | fixed |
| R-007 | S1 | patches/dynarmic shared code cache + guest cacheflush | a guest that rewrites code (Mono's JIT in 99 Bricks) runs stale translations with one shared cache: Mono metadata asserts (`idx < t->rows`), TypeLoadException, wrong textures in 4 boots of 10; with private caches 0 of 20. Suspects: the flush is applied only when the flushing thread's Run stops (other threads keep the old blocks meanwhile), and a thread's private overflow cache is never invalidated by another. Rovio titles never flush (no guest JIT) and keep sharing. Worked around per title: host.jitshare = off (1531802); private caches now all get every flush (13b053f) | 2026-10-28 sandbox, 10-boot batches | open (workaround) |
| R-008 | S1 | PthreadCond sliced waits | a slice that timed out re-slept although pthread_cond_timedwait may take the wake with an ETIMEDOUT: the wake was lost (Unity's file queue: the loader asleep on a read already done, asyncProcessor asleep on a non-empty queue). Hangs at the logo, at 25-30%, on placing a block | dump: futex word of the sleeper's cond had counted the wake; 3 of 5 boots hung | fixed a53149d |
| R-003 | S2 | JNI mock refs | mock jstrings never freed (~86 KB/h Rio); needs real local/global ref lifetimes | AGENTS §145 | wontfix 2026-10-27 (owner): real local/global ref lifetimes risk use-after-free for ~2 MB/day |

### Device gates

| date | lib build marker | fixes | result |
|---|---|---|---|
| 2026-10-25 | Oct 25 2026 14:39:58 (md5 96da05d3) | B-01..B-05, A-01/06/12, C-01 | rio + seasons boot to the main menu on the sandbox, all markers 0, death.log 0 B, no tombstone; in-level play not yet checked |
| 2026-10-25 | Oct 25 2026 14:54:04 (md5 56d53d81) | + A-03/05/07/10/11, C-02, D-06 | rio + seasons + space boot to the main menu, all markers 0, death.log 0 B, no tombstone; in-level play not yet checked |
| 2026-10-25 | Oct 25 2026 15:20:38 (md5 5e225f86) | + allocator, mmap/MappingTable, setjmp VFP, fma/remquo, file-bridge errno/dup/mkdir/mkstemp/props, asset lock | rio + seasons + space + star-wars boot to the main menu, all markers 0, death.log 0 B, no tombstone; user reported earlier frequent crashes seem gone (batch 1-2 build, light play) |
| 2026-10-25 | Oct 25 2026 15:34:38 (md5 c5247c11) | + guest mutex types, single tid counter | all six titles reach their usual first screen (star-wars-ii/amz: R-004 dialog), markers 0, death.log 0 B |
| 2026-10-25 | Oct 25 2026 15:47:17 (md5 c8f95ed1) | + Rio init #22 runs | rio boots to the menu, markers 0 |
| 2026-10-26 | lib md5 137e5cdf | + JNI batch (fields, float/J marshalling, JavaVM, GetArrayLength), GL fast path, glGetUniformfv, glCopyTex | rio, seasons, space, star-wars render the menu and run 100 s, markers 0, death.log 0 B, no new tombstones |
| 2026-10-26 | dead-code removal (5 commits) on top of 16 KB loader, RELRO, G-02, real-first JNI | all six titles boot, markers 0, death.log 0 B; user played one level in each title on the previous build: all OK, scale improved |
| 2026-10-27 | shared JIT code cache (jitshare=on) + F-23 | five live titles boot to their menus, markers 0, death.log 0 B, no new tombstones; offline (strace-injected ENETUNREACH on connect) Star Wars 16/16 + Space 10/10 boots clean (was ~1/4-1/6 aborts); arm64 fmt_test on the sandbox: shared cache + 4-thread LDREX 800000/800000 x3; steady-state frame time unchanged, boot +~0.1 s |
| 2026-10-27 | offline network policy (R-005), rewrite removed, APKs rebuilt with host.network=offline | five titles boot to their menus (network: offline in the log), markers 0, death.log 0 B; Star Wars and Space booted twice each |
| 2026-10-25 | Oct 25 2026 19:24:21 | + shared sync objects, once ordering, key_create, pthread_create/threads_ hardening, g_upd_jit | rio, seasons, space, star-wars alive at the menu after 100 s, markers 0, death.log 0 B, no tombstones |

### Wave 1 (areas A-D, 2026-10-25)

Line numbers refer to dispatch.cpp at HEAD 2cba59a unless a file is named.
A and D full reports were read before /tmp was wiped by a reboot; B and C only
survive as agent summaries (a re-run fills the gaps into work/review/).

**A — libc core**

| id | sev | where | finding | status |
|---|---|---|---|---|
| A-01 | S1 | 3503 poll | `fd_map_.find` without `io_mutex_` (every other access locks) — host data race | fixed e4b29a4 |
| A-02 | S1→S2 | 1773 mmap, 907 GuestMmap | fd/offset/flags dropped: file mapping returns zero pages; addr hint treated as FIXED. All 6 import mmap/munmap (code comment "not imported" is false); Rio call site 0x2cd35c (MemoryMappedFile). Not seen in device logs | fixed a1bbccd |
| A-03 | S2 | 2995 strtol/strtoul | host 64-bit result truncated: strtol("2147483648") -> INT_MIN (want LONG_MAX+ERANGE); strtoul("4294967296") -> 0; no strto*/ato* sets guest errno | fixed 6352806 |
| A-04 | S2 | math_thunks.inc:262,391 | fma addend and remquo quo read from r4/r5 (garbage); AAPCS32: [sp], remquof quo = r2. Rio imports remquo, no call site | fixed 2c64d6b |
| A-05 | S2 | 2282 strncat | CheckRange covers [dest,dest+n+1) but write is at dest+strlen(dest); n+1 wraps; unbounded host strlen | fixed 5d397e9 |
| A-06 | S2 | 3497, 3657 poll | fd<0 must give revents=0 (POSIX); unmapped fd must count POLLNVAL in return | fixed e4b29a4 |
| A-07 | S2 | 1678/1702 ctype | TAB carries B (printable) bit: `_ctype_['\t']`=0xA8 not 0x28, iswctype('\t',print)!=0 | fixed 726cd28 |
| A-08 | S2 | 549 GuestMalloc | size rounding wraps: malloc(0xFFFFFFF9+) returns 8-byte block | fixed 68d7bda |
| A-09 | S2 | 2077 setjmp/longjmp | d8-d15/FPSCR not saved; "softfp has no VFP regs" comment false | fixed 1c40191 |
| A-10 | S2 | 3329 nanosleep | guest pointers unchecked (host SIGSEGV possible); nsec>=1e9 not EINVAL. Live: Rio sleep_for 0x4428fc | fixed 701f8e7 |
| A-11 | S2 | 975 GuestStr users | strlen/strdup/strcmp/sscanf/strto*/%s truncate at 64 KiB, unreadable ptr -> "" | fixed 5d397e9 |
| A-12 | S3 | 3511 poll | timeout -1 becomes 5000 (spurious timeout); >5000 not capped despite comment | fixed e4b29a4 |
| A-13 | S3 | 2314 memchr | getenv("HARNESS_MEMDUMP") on every call in shipping | fixed febf56c |
| A-14 | S3 | 2308, 2283 | memchr/strncpy validate the full n bytes of source -> false halts | fixed 5d397e9 (strncpy; memchr open) |
| A-15 | S3 | math_thunks.inc:268, 1846 | __fpclassifyd/sysconf pass host constants (ok on bionic host, wrong in harness) | open |
| A-16 | S3 | 641 calloc | calloc(0,x) returns NULL (should return a unique pointer) | fixed 68d7bda |
| A-17 | S3 | 1550 printf %s | NULL prints ELF header (VA 0 mapped); want "(null)" | open |
| A-18 | S3 | 3311 snprintf | unmapped dst silently skipped, full length returned | open |
| A-19 | S3 | 3175, 3200 localtime | TIMEWARP applied twice (diag); tm_zone NULL | open |
| A-20 | S3 | 2070 syscall | returns -ENOSYS raw instead of -1+errno; tgkill fakes success | open |
| A-21 | S3 | 1361-1472 ExpandPrintf | trailing %/bad spec not counted; 1 MiB loop guard stops counting; %10p width placement; %*d INT_MIN | open |
| A-22 | S4 | math_thunks.inc:290,371 | __signbitl/nanl treat long double as float (dead) | open |
| A-23 | S4 | 6059 rand/random, bsd_random.h | dead (no title imports), comment describes modern bionic | open |
| A-24 | S4 | 3087 strerror | strerror(0) -> "Invalid argument" | open |
| A-25 | S4 | various | stale comments (GuestStr "512", poll cap, r4/r5 ABI, tgkill) | open |
| A-26 | S4 | 2196 | `[memcmp3]` ungated printf in native build | open |
| A-27 | S4 | 3417 pthread_create | "http" scan + job-name copy on every call with diag off | open |
| A-28 | S4 | arm_hwcap.h | NEON + VFPv3D16 combination; real kernels report VFPD32 with NEON (unverified) | open |
| A-29 | S4 | 2419 strtok | NULL state halts (not imported; strtok_r fine) | open |
| A-30 | S4 | fmt_test.cpp | missing tests: tab/print, fma/remquo args, strtol overflow/errno, poll fd<0 | open |

**B — files / sockets / assets / time** (summary only, re-run pending)

| id | sev | where | finding | status |
|---|---|---|---|---|
| B-01 | S1 | 5455 readdir_r | d_type written at +17, d_name at +18; bionic: +18/+19 (dirent.h; Rio reads name at r8+19, 0x2c67b8) — every name loses its first char | fixed bc92fa4 (gate: boot->menu ok) |
| B-02 | S1 | 3918 fill_stat | 96-byte packed layout; arm32 struct stat is 104 B: st_size +48, blksize +56, blocks +64, atim/mtim/ctim +72/+80/+88, st_ino +96 (NDK clang offsetof) | fixed 61a5ca0 (gate: boot->menu ok) |
| B-03 | S1 | 3945, 5211 statfs | branch unreachable ("statfs" missing from the outer if, falls to the 0-stub); layout uses 32-bit counts, arm32 statfs is 88 B with u64 f_blocks at +8 | fixed f534b93 (gate: boot->menu ok) |
| B-04 | S1 | 4171, 4537 fseek/fseeko/lseek | `(long)a1` zero-extends: negative offsets seek +4 GB (Rio fseeko(f,-r5,SEEK_CUR) at 0x274314) | fixed 584fb22 (gate: boot->menu ok) |
| B-05 | S1 | 4744 setsockopt | by-value optlen read as a pointer (reads VA 4) | fixed 217165e (gate: boot->menu ok) |
| B-06 | S2 | asset block | every asset import holds global `mutex_`, inflate under it | fixed 42c88ad |
| B-07 | S2 | __system_property_get | buffer unwritten for unknown keys | fixed 52120ed |
| B-08 | S2 | mkdir | stale errno on EEXIST | fixed 52120ed |
| B-09 | S2 | read/write/lseek/close | errno never set on failure | fixed 52120ed |
| B-10 | S2 | dup/freopen/utime | fall to the 0-stub; dup() makes the guest use fd 0 | fixed 52120ed (dup/utime/fflush; freopen open) |
| B-11 | S2 | mkstemp | suffix written at host path length, can overflow guest buffer | fixed 52120ed |
| B-12 | S2 | connect | non-blocking connect made synchronous, forced 10 s socket timeouts | fixed with R-005 (offline policy, rewrite removed) |
| B-13 | S2 | MapGuestPath | unmapped path halts the thread instead of ENOENT; `..` can escape the files dir | fixed 52120ed (ENOENT; .. escape open) |
| B-14 | S3 | sockets | always-on TLS ClientHello rewrite of the guest buffer in shared code | fixed with R-005 (offline policy, rewrite removed) |
| B-15 | S3 | __system_property_get | hardcoded values ("izan", sdk 21, DNS 8.8.8.8) | fixed 52120ed (on device real props) |
| B-16 | S3 | poll, FIFO read | 5 s caps (crutches) | fixed 52120ed (FIFO cap; poll done) |
| B-17 | S3 | zipfile.cpp ZipExtractStored | fwrite result ignored: partial seed file can persist | open |

**C — memory / heap / threads / sync** (full re-run report: work/review/C-memthreads.md, C-13..C-36 added there; key ones below)

| id | sev | where | finding | status |
|---|---|---|---|---|
| C-01 | S1 | 3376, 6486 PthreadCond | `pthread_cond_timedwait_monotonic`: mono flag ignored, host cond on REALTIME vs guest MONOTONIC deadline -> immediate ETIMEDOUT. Live: Rio clock_gettime(1) 0x38f45c -> timedwait_monotonic 0x38f530; all 6 import it. Comment "currently unused" false | fixed 938c961 |
| C-02 | S1 | LDREX/STREX | not atomic across guest threads: two separate static ExclusiveMonitors (Java threads vs workers), Java threads all processor 0, workers `% 8`; ExclusiveWrite byte-wise compare/store, no real CAS | fixed 3d6fba5 |
| C-03 | S2 | GuestMmap | 32-bit bound checks wrap: MAP_FIXED can reach trampolines/stacks/past 4 GB | fixed a1bbccd |
| C-04 | S2 | mappings_ | appended by mmap without sync against lock-free readers | fixed a1bbccd |
| C-05 | S2 | munmap | stale mappings + caches: guest access -> host SIGSEGV instead of halt | fixed a1bbccd |
| C-06 | S2 | mutex/rwlock/cond destroy | host object deleted while another thread is about to use it (UAF) | fixed e221e33 |
| C-07 | S2 | guest tids | collide with Java thread ids (from 1000) after 998 pthread_create -> shared errno/TLS | fixed 0df4897 |
| C-08 | S2 | pthread_once | "done" published without memory ordering | fixed 500d528 |
| C-09 | S2 | pthread_once | Java threads never release once controls after an abnormal halt | open |
| C-10 | S2 | thread stacks | in the heap with no guard page | wontfix 2026-10-27 (owner): a guard needs its own MappingTable entry per thread (linear Find, no removal) = a cost on every pointer check, for a case that does not occur |
| C-11 | S2 | dlmalloc | 64-bit host pointers in guest-writable memory: wild guest write -> host write | open |
| C-12 | S2 | Rio init #22 | still parked; its "wild" pointer 0xFFFF0FA0 is the kuser barrier the host now emulates; 0x1aeb24 is a real flag-guarded initializer also called lazily from 4 sites -> un-park and retest | fixed a200ffe |
| C-13 | S2 | mmap | = A-02 (file mappings come back zeroed; Rio MemoryMappedFile, 3 callers of 0x2cd2b8) | fixed a1bbccd |
| C-14 | S2 | GuestMalloc | = A-08 (size wrap) | fixed 68d7bda |
| C-15 | S3 | GuestCalloc | calloc(0)=NULL (= A-16) + magic 256 MB cap | fixed 68d7bda |
| C-17 | S3 | quarantine check | any block > 16 MB treated as corrupt and leaked | open |
| C-22 | S2 | loader | guest module at VA 0: NULL reads return ELF bytes instead of halting | open |
| C-25 | S2 | PthreadMutex | every guest mutex is recursive, attributes ignored (trylock/relock/foreign unlock differ) | fixed 0df4897 |
| C-27 | S3 | pthread_key_create | writes through an unchecked guest pointer | fixed 500d528 |
| C-28 | S3 | pthread keys | destructors never run | open |
| C-29 | S3 | threads_ | grows without bound | fixed 137b44f |
| C-30 | S3 | PthreadCreate | std::thread exception reaches std::terminate (and leaks the cpu id) | fixed 500d528 |
| C-31 | S3 | apphost watchdog | g_upd_jit raw pointer outlives a recreated GL thread's Jit (UAF on STUCK path) | fixed 137b44f |
| C-32 | S3 | ActiveJit() | falls back to the OnLoad Jit for threads without one | open |
| C-33 | S3 | exception redirect | redirect state shared by all threads (SUSPECTED) | open |

**D — JNI bridge + app glue** (full report read)

| id | sev | where | finding | status |
|---|---|---|---|---|
| D-01 | S2 | 9010 GetObjectClass | real objects get a fake `java/lang/Object` class: App.quitRequested()/isSilentProfile() never reach Java (seen in logs) | fixed d8adc22 (static fields 145-151 still mock) |
| D-02 | S2 | 8878, 8897, 9629, 9675 | isCopy written as 4 bytes; jboolean is 1 byte | fixed 393f105 |
| D-03 | S2 | 8170, 8507 | real-call `...`/V forms read float as one word; C promotes to double | fixed fcdca1b |
| D-04 | S2 | 8488-8506 RealNewObject | J/D wrong in plain and A forms (copy of the marshaller) | fixed fcdca1b |
| D-05 | S2 | 8189, 8512 | any mapped mock handle is materialized as java.lang.String | fixed 393f105 |
| D-06 | S2 | apphost.cpp:592 Enter | 8 MB stack alloc unchecked -> host write to unmapped VA on OOM | fixed 3d6fba5 |
| D-07 | S2 | 9865 GetObjectField | name/sig split heuristic ("MANUFACTURER" -> "MANU") | fixed 621b9cb |
| D-08 | S2 | 164/220/221/171/222/223 | real-wrapped strings/arrays unsupported; 223 missing | fixed 5f617d8 (GetArrayLength only; 164/220/221/222/223 open) |
| D-09 | S2 | 9686 Release mode | JNI_COMMIT treated as final release | fixed 393f105 |
| D-10 | S2 | 8824, 8197, 8520 | strings silently truncated at 4096 bytes | fixed 393f105 |
| D-11 | S3 | 117-151 mocks | shipping mocks shadow real Java (API 21, 800x480, Build.* "", hasSystemFeature list, static void dropped, FindClass never fails) | fixed d8adc22 (static fields 145-151 still mock) |
| D-12 | S3 | 135-137 | CallStaticFloatMethod missing; AccelerometerWrapper getters resolved | open |
| D-13 | S3 | 215 RegisterNatives | registers nothing; "never ships" comment false; 6 natives without forwarders (Rio smali) | fixed on `bricks` (real registration via host stubs); device gate of all five pending |
| D-14 | S3 | JniNewString etc. | unbounded growth: mock strings, registry, method cookies never deduped | open |
| D-15 | S3 | apphost.cpp:281 DeathHandler | stack walk before write -> line lost on corrupt frames; ActiveJit() may print another thread's regs | open |
| D-16 | S3 | apphost.cpp:1209 | stdout "w" + stderr "a" on the same apphost.log -> overwrites | open |
| D-17 | S3 | JavaVM 3/5/7 | Detach/AttachAsDaemon/Destroy halt LOUD | fixed 5f617d8 |
| D-18 | S3 | 8947, 8959 | region copy silently clamped (keep bound, report LOUD) | open |
| D-19 | S3 | gen_apphost.py | F/D/object return types broken; `$` mangling | open |
| D-20 | S3 | 9443 | CallStaticBooleanMethod plain form reads first vararg from [sp] not r3 | fixed 5f617d8 |
| D-21 | S3 | 8371, 9158, 9244 | AudioOutput / startOutput name-matched scope guards in shared code | open |
| D-22 | S4 | 9410 | GetArrayLength: lock_guard is inside a comment | fixed 5f617d8 |
| D-23 | S4 | 8001, 9401, 9565, 8340, apphost.cpp:2 | stale comments, dead `fn==145` branch | open |
| D-24 | S4 | apphost.cpp:1163, 1314 | abflags.txt: CRLF kept in values ("0\r" = ON), >255-byte lines split | open |
| D-25 | S4 | 7935 WrapObject | linear IsSameObject scan per wrap; permanent global refs | open |
| D-26 | S4 | apphost.cpp:1524 | JNI_OnLoad reports success after failed init/guest OnLoad | open |

D also PROVEN correct: every CallJni index vs NDK jni.h; void calls return r0=0;
forwarder AAPCS layout; no 64-bit host pointer reaches guest memory.

### Wave 2

**H — build / packaging** (report: work/review/H-build.md)

| id | sev | where | finding | status |
|---|---|---|---|---|
| H-01 | S2 | host/CMakeLists.txt | libabhost.so LOAD segments are 4 KB aligned; 16 KB-page devices (Android 15+ Pixels etc.) need -z max-page-size=16384 | fixed 2c2feaa |
| H-02 | S2 | elf_loader.cpp:96-117,559; guest_memory.h; GuestMmap | loader assumes 4 KB host pages: 5 of 6 guests have RW segments not 16 KB aligned (map fails), code+data share a 16 KB page in 3 | fixed 52a6ee4 |
| H-03 | S3 | apphost.cpp:1230 | stdout "w" + stderr "a" on one log: stdout overwrote stderr lines (= D-16) | fixed 12082e2 |
| H-04 | S3 | build API 26 vs sandbox API 25, minSdk 16 | lib really needs API 24; build for android-24 and declare minSdk 24 | open |
| H-05 | S3 | scripts/devgate.sh | tombstones counted without sudo; grep -w matched star-wars-ii-amz; md5 mismatch not fatal | fixed ccd4747 |
| H-06 | S3 | thirdparty/dynarmic | commit a46601580d55 (azahar-emu) not recorded in the repo | fixed: patches/dynarmic/README.md records it with the local patch |
| H-07..H-14 | S4 | see report | no -Wall (9 dead-code warnings), all symbols exported, system llvm-strip and no unstripped lib per build-id, dead strip.libs loop, gen_apphost.py float/double/$ gaps, stale profile/script comments, deploy.sh greps 5 markers, review reports unversioned | open |

**E — GL** (report: work/review/E-gl.md). All six titles import the same 68 gl*; none imports glGetError.

| id | sev | where | finding | status |
|---|---|---|---|---|
| E-01 | S1 | realgl glGetUniformfv | passes a uniform LOCATION as an index to glGetActiveUniform and sizes the host buffer by element count, not floats: vec4/mat4 overflow the host heap by 12/60 B (Rio 5 call sites) | fixed 6adfb9e |
| E-02 | S2 | glCopyTexImage2D | falls into the fake shim (never executed, logs UNHANDLED); Rio 0x2522e0 | fixed 6adfb9e |
| E-03 | S2 | realgl bound-buffer tracking | one per process, but bindings are per GL context (context loss / shared contexts) -> 32-bit offset taken as host pointer (SUSPECTED) | fixed: bindings are asked from the driver at the point of use (no host copy); gate incl. background/resume of all five |
| E-04 | S2 | client vertex arrays | only 1 byte checked at glVertexAttribPointer, nothing at draw time; bad pointer silently NULL | open |
| E-05 | S2 | tex/readback sizes | GL_UNPACK/PACK_ALIGNMENT ignored | fixed: ImageBytes uses the driver's UNPACK/PACK_ALIGNMENT |
| E-06 | S2 | glGetIntegerv(COMPRESSED_TEXTURE_FORMATS) | fixed 64-entry host array still overflowable (latent) | fixed: buffer sized from the driver's count; BLEND_COLOR and SHADER_BINARY_FORMATS counted |
| E-07 | S2 | GL error paths | SHORTBUF/SHORTIDX/BAD-PTR/SMALLBUF/UNHANDLED neither halt nor print LOUD | fixed: LOUD + HaltGuest on every GL bad-pointer/short-buffer path; svc bad_ptr line says LOUD |
| E-perf | S2 | gl path | ~241 name compares + uncached getenv + std::string alloc + snprintf per call (~0.4 us desktop, est. 1-2 ms/frame); global mutex_ held across slow driver calls blocks other threads' malloc/pthread/JNI. Design: resolve gl imports to an enum at load, drop mutex_ from the GL path, per-context state | fixed 4ae3709 (fast path + own lock; enum dispatch still open) |
| E-08.. | S3/S4 | see report | glBufferData size zero-extension, glGetString slot capacity, fake shim on context-less threads, NOBLEND per draw, unexplained \x01 shader rewrite, dead glGetError code | open |

**F — loader / JIT callbacks / probes** (report: work/review/F-loader.md; 38 findings: 3 S2, 20 S3, 15 S4)

| id | sev | where | finding | status |
|---|---|---|---|---|
| F-22 | S2 | RELRO write guard | only checked in slow-path callbacks; with fastmem (shipping) RELRO is host-writable, so the guard never fires; host memcpy/memset thunks skip it too. Fix: mprotect RELRO read-only after InitDataImports and record it read-only | fixed f4802e0 |
| F-11 | S2 | elf_loader, guest_memory | 4 KiB pages hard-wired (= H-02) | fixed 52a6ee4 |
| F-23/24 | S3 | MemoryReadCode, Read/Write | bad code read returns 0 (Dynarmic caches a zero block, wrong pc in the halt); bad data access halts only at block end, the rest of the block runs on 0. Fix: nullopt, check_halt_on_memory_access, MemoryAbort | F-23 fixed (MemoryReadCode -> nullopt, NoExecuteFault when reached, test); F-24 open |
| F-25 | S3 | InitDataImports | runs before any Jit: LOUD branches call ActiveJit()->HaltExecution on null | open |
| F-34 | S3 | diag mode | HARNESS_DIAG arms Rio probe VAs for all six titles | open |
| F-pin | S3 | elf_loader.h IsPinnedGuest | build-id pin gates nothing (only its own log); delete it, harness verifies the SHA from profile.toml (closes R-001) | open |
| F-01..03 | S4 | relocations | REL32 lacks -P (unused), weak undefined -> trampoline, GLOB_DAT addend applied (all 0 in these guests) | open |
| F-06 | S3 | Find_exidx | exidx table returned for trampoline pcs; nest guard state shared across threads | open |
| F-12..21 | S3 | elf parsing | unbounded table reads / uint32 wraps on malformed ELF (low risk: guest SHA is checked first) | open |
| F-26 | S4 | ExceptionRaised | add-pc emulation dead (Dynarmic handles add pc natively) | open |
| F-29 | S3 | Runtime::jit_ | still points at the OnLoad Jit after its thread exits (= C-32) | open |
| F-35 | S3 | gate | harness runs with fastmem off, unlike shipping | open |
| F-36 | S3 | cbnz_test | always returns 0, checks nothing | open |

**G — crutches / knobs / magic numbers / dead code** (report: work/review/G-crutches.md; 157 HARNESS_* knobs classified, ~40 magic numbers traced, ~4000 dead lines identified: dispatch.cpp ~3000, harness.cpp ~850, apphost.cpp ~200)

| id | sev | where | finding | status |
|---|---|---|---|---|
| G-01 | S2 | JNI shipping | only object methods/fields go to ART; all Boolean/Int/Long/Void calls and static fields are mocks, wrong on modern phones (density 1 vs ~420, screenLayout, 800x480, API 21, empty Build.*). App.quitRequested only calls stopLockTask() in all six titles: safe to forward (= D-01/D-11) | fixed d8adc22 (static fields 145-151 still mock) |
| G-02 | S2 | generic 0-stub | unimplemented imports answer 0: vasprintf never writes *strp (Rio libc++ 0x42690c), fnmatch always matches (Seasons 0x46100c), raise(SIGFPE) continues, geteuid = root, basename/setlocale NULL, wcslen/wmem*/towlower 0 in five titles | fixed b979090 |
| G-03 | S3 | registry dump | ungated hex dump of the first 60 reads/writes of any "registry" file into apphost.log in shipping; FILE* set never pruned | fixed 2dadd39 |
| G-04 | S3 | harness.cpp | bool skip[128] read for 143/144 inits (out of bounds) | open |
| G-05/11 | S3 | diag / FUNCTORAT / TAPBEAT | Rio VAs armed for every title | open |
| G-06 | S3 | apphost nativeDeinit | Java-stack forensics ran ungated on every exit (FindClass/NewObject/3000-char log, leaked refs) | fixed dbcb76c |
| G-08 | S3 | __page_size | fixed 4096 while sysconf/AT_PAGESZ return the host's 16384 on 16 KB devices (SUSPECTED) | open |
| G-09 | S3 | unwind nest guard | leftover racy crutch | open |
| G-10 | S3 | fake GL shim | still reachable on device (context-less threads, names RealGL doesn't handle) | fixed dbcb76c: shim is PC-only; Android sends every gl* to the driver, with or without a context |
| G-12 | S3 | TLS ClientHello rewrite | on by default, edits the guest buffer in place, also downgrades TLS 1.1 (= B-14) | fixed with R-005 (offline policy, rewrite removed) |
| G-13 | S4 | apphost | only FASTMEM and REALGL are set in shipping: make them config, not knobs | fixed dbcb76c: fastmem and RealGL hardwired on Android (JitFastmemOn), env only on the PC |
| G-14 | S3 | fcntl | lock commands report success without doing anything | open |
| G-dead | S3 | whole tree | ~4000 removable lines (TEMP-DIAG, probes, pokes, redirects, unwinder experiments) | fixed 850c027..6f762fd (-4168 lines; shipping-path TEMP-DIAG/fake GL/nativeDeinit forensics remain) |
