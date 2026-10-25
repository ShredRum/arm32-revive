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
| R-004 | S2 | star-wars-ii(-amz) | boot ends at "Asset downloading failed" (wants Rovio content servers that no longer exist); pre-existing, needs an offline content path | open |
| R-003 | S2 | JNI mock refs | mock jstrings never freed (~86 KB/h Rio); needs real local/global ref lifetimes | AGENTS §145 | open |

### Device gates

| date | lib build marker | fixes | result |
|---|---|---|---|
| 2026-10-25 | Oct 25 2026 14:39:58 (md5 96da05d3) | B-01..B-05, A-01/06/12, C-01 | rio + seasons boot to the main menu on the sandbox, all markers 0, death.log 0 B, no tombstone; in-level play not yet checked |
| 2026-10-25 | Oct 25 2026 14:54:04 (md5 56d53d81) | + A-03/05/07/10/11, C-02, D-06 | rio + seasons + space boot to the main menu, all markers 0, death.log 0 B, no tombstone; in-level play not yet checked |
| 2026-10-25 | Oct 25 2026 15:20:38 (md5 5e225f86) | + allocator, mmap/MappingTable, setjmp VFP, fma/remquo, file-bridge errno/dup/mkdir/mkstemp/props, asset lock | rio + seasons + space + star-wars boot to the main menu, all markers 0, death.log 0 B, no tombstone; user reported earlier frequent crashes seem gone (batch 1-2 build, light play) |

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
| B-12 | S2 | connect | non-blocking connect made synchronous, forced 10 s socket timeouts | open |
| B-13 | S2 | MapGuestPath | unmapped path halts the thread instead of ENOENT; `..` can escape the files dir | fixed 52120ed (ENOENT; .. escape open) |
| B-14 | S3 | sockets | always-on TLS ClientHello rewrite of the guest buffer in shared code | open |
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
| C-06 | S2 | mutex/rwlock/cond destroy | host object deleted while another thread is about to use it (UAF) | open |
| C-07 | S2 | guest tids | collide with Java thread ids (from 1000) after 998 pthread_create -> shared errno/TLS | fixed 0df4897 |
| C-08 | S2 | pthread_once | "done" published without memory ordering | open |
| C-09 | S2 | pthread_once | Java threads never release once controls after an abnormal halt | open |
| C-10 | S2 | thread stacks | in the heap with no guard page | open |
| C-11 | S2 | dlmalloc | 64-bit host pointers in guest-writable memory: wild guest write -> host write | open |
| C-12 | S2 | Rio init #22 | still parked; its "wild" pointer 0xFFFF0FA0 is the kuser barrier the host now emulates; 0x1aeb24 is a real flag-guarded initializer also called lazily from 4 sites -> un-park and retest | fixed a200ffe |
| C-13 | S2 | mmap | = A-02 (file mappings come back zeroed; Rio MemoryMappedFile, 3 callers of 0x2cd2b8) | fixed a1bbccd |
| C-14 | S2 | GuestMalloc | = A-08 (size wrap) | fixed 68d7bda |
| C-15 | S3 | GuestCalloc | calloc(0)=NULL (= A-16) + magic 256 MB cap | fixed 68d7bda |
| C-17 | S3 | quarantine check | any block > 16 MB treated as corrupt and leaked | open |
| C-22 | S2 | loader | guest module at VA 0: NULL reads return ELF bytes instead of halting | open |
| C-25 | S2 | PthreadMutex | every guest mutex is recursive, attributes ignored (trylock/relock/foreign unlock differ) | fixed 0df4897 |
| C-27 | S3 | pthread_key_create | writes through an unchecked guest pointer | open |
| C-28 | S3 | pthread keys | destructors never run | open |
| C-29 | S3 | threads_ | grows without bound | open |
| C-30 | S3 | PthreadCreate | std::thread exception reaches std::terminate (and leaks the cpu id) | open |
| C-31 | S3 | apphost watchdog | g_upd_jit raw pointer outlives a recreated GL thread's Jit (UAF on STUCK path) | open |
| C-32 | S3 | ActiveJit() | falls back to the OnLoad Jit for threads without one | open |
| C-33 | S3 | exception redirect | redirect state shared by all threads (SUSPECTED) | open |

**D — JNI bridge + app glue** (full report read)

| id | sev | where | finding | status |
|---|---|---|---|---|
| D-01 | S2 | 9010 GetObjectClass | real objects get a fake `java/lang/Object` class: App.quitRequested()/isSilentProfile() never reach Java (seen in logs) | open |
| D-02 | S2 | 8878, 8897, 9629, 9675 | isCopy written as 4 bytes; jboolean is 1 byte | open |
| D-03 | S2 | 8170, 8507 | real-call `...`/V forms read float as one word; C promotes to double | open |
| D-04 | S2 | 8488-8506 RealNewObject | J/D wrong in plain and A forms (copy of the marshaller) | open |
| D-05 | S2 | 8189, 8512 | any mapped mock handle is materialized as java.lang.String | open |
| D-06 | S2 | apphost.cpp:592 Enter | 8 MB stack alloc unchecked -> host write to unmapped VA on OOM | fixed 3d6fba5 |
| D-07 | S2 | 9865 GetObjectField | name/sig split heuristic ("MANUFACTURER" -> "MANU") | open |
| D-08 | S2 | 164/220/221/171/222/223 | real-wrapped strings/arrays unsupported; 223 missing | open |
| D-09 | S2 | 9686 Release mode | JNI_COMMIT treated as final release | open |
| D-10 | S2 | 8824, 8197, 8520 | strings silently truncated at 4096 bytes | open |
| D-11 | S3 | 117-151 mocks | shipping mocks shadow real Java (API 21, 800x480, Build.* "", hasSystemFeature list, static void dropped, FindClass never fails) | open |
| D-12 | S3 | 135-137 | CallStaticFloatMethod missing; AccelerometerWrapper getters resolved | open |
| D-13 | S3 | 215 RegisterNatives | registers nothing; "never ships" comment false; 6 natives without forwarders (Rio smali) | open |
| D-14 | S3 | JniNewString etc. | unbounded growth: mock strings, registry, method cookies never deduped | open |
| D-15 | S3 | apphost.cpp:281 DeathHandler | stack walk before write -> line lost on corrupt frames; ActiveJit() may print another thread's regs | open |
| D-16 | S3 | apphost.cpp:1209 | stdout "w" + stderr "a" on the same apphost.log -> overwrites | open |
| D-17 | S3 | JavaVM 3/5/7 | Detach/AttachAsDaemon/Destroy halt LOUD | open |
| D-18 | S3 | 8947, 8959 | region copy silently clamped (keep bound, report LOUD) | open |
| D-19 | S3 | gen_apphost.py | F/D/object return types broken; `$` mangling | open |
| D-20 | S3 | 9443 | CallStaticBooleanMethod plain form reads first vararg from [sp] not r3 | open |
| D-21 | S3 | 8371, 9158, 9244 | AudioOutput / startOutput name-matched scope guards in shared code | open |
| D-22 | S4 | 9410 | GetArrayLength: lock_guard is inside a comment | open |
| D-23 | S4 | 8001, 9401, 9565, 8340, apphost.cpp:2 | stale comments, dead `fn==145` branch | open |
| D-24 | S4 | apphost.cpp:1163, 1314 | abflags.txt: CRLF kept in values ("0\r" = ON), >255-byte lines split | open |
| D-25 | S4 | 7935 WrapObject | linear IsSameObject scan per wrap; permanent global refs | open |
| D-26 | S4 | apphost.cpp:1524 | JNI_OnLoad reports success after failed init/guest OnLoad | open |

D also PROVEN correct: every CallJni index vs NDK jni.h; void calls return r0=0;
forwarder AAPCS layout; no 64-bit host pointer reaches guest memory.
