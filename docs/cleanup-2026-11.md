# Cleanup 2026-11: crutches, leftovers and hot-path checks

Branch `cleanup-crutches`. Goal (owner, 2026-11-10): remove every leftover
crutch and diagnostic remnant from the shipping core, and make the checks
that stay cheap on hot paths. Source: two read-only audits of the code at
baacdf6, every item traced through git history (appendices A and B, line
numbers as of baacdf6). One logical change per commit; after each batch:
fmt_test (PC + sandbox), harness on every title, device gate of all live
titles when the sandbox is up.

Key fact: the 2026-10-16/17 heap-corruption hunt put an "overflow firewall"
on every writer; its root cause was the JNI table off by one (1177bac), and
27e2161 turned the firewall off by default, but parts of it still run.

## Batches (tick when committed)

1. Heap firewall leftovers
   - [x] ClampHeapWrite and HARNESS_CLAMPWRITE gone from every caller
         (memcpy/memset/str*, printf family, fread, fgets, sscanf
         numerics) — A1, A5
   - [x] JNI GetString(UTF)Region force clamp — A4 / B-R1
   - [x] sscanf %s/%c/%[ field clamp — A2
   - [x] big_live_ side index — A3
2. [x] JNI audio buffer pool `array_pool_` (scan on every free/realloc, leaks
   past 8) — B-R2
3. Retired knobs and diag remnants — A6-A9, B
   - [x] MALLOCPAD, ZEROFILL, NORELEASE, HEAPBASE, realloc pre-dump,
         HEAPWATCH/WP/WPA/TRACELOADS, Fox-hunt dump (dbc4c53)
   - [x] TIMEWARP, NOWORKER, WBEAT (also the worker-Jit use-after-free),
         MEMDUMP, HOSTSHACK (2856fd2)
   - [x] closed-hunt TEMP-DIAG logs in dispatch.cpp: preamble socket log,
         pthread_create job/URL sniffing, sscanf inputs, registry [regio]
         dumps + FILE* tagging, pipe log, recv inbound bytes, fcntl parking
         line, opendir log, fake-GL [glsrc]/[gltex] + work/shaders stash,
         memset wall4 log, inet_ntop/pton logs (87d73b8). The [import]
         "first call" audit stays (general tool).
   - [x] apphost.cpp RunGuestNative/AppHost_Call forensics: [screen],
         [rate], PrintHeapIdx (+ its two counters), [input], r0 polarity,
         uniqcall/call# (be88baa); none is named as a tool in the docs
   - [x] realgl.cpp black-frame dumps, active_tex_/bound_tex2d_/tex_size_,
         SHOTSWAP/SHOTCLEAR (d7578bc); plain GLDRAWLOG lines stay
   - [ ] left: harness.cpp (PC only) [memselftest] and the "[heap] top"
         exit line still carry TEMP-DIAG tags; POLLDBG, DNS1/2, NETTYPE,
         DEVID, LOCALE knobs (minimum.md list) not part of this batch
4. Dead or title-specific code — B-R3/R4, A-C1
   - [x] "add pc" emulation in ExceptionRaised (unreachable; fmt_test case
         runs `add pc, r0, pc` on the JIT) (d5404b0)
   - [x] ExceptionRaised backtrace: ScanStackForReturnAddresses instead of
         Rio's [0x1000, 0x6EE000); the halting line says LOUD (62b1471)
   - [x] unwind nest guard + HARNESS_NESTMAX / HARNESS_EXIDXLOG and the
         now-unused FindExidx; cause fixed 6de206a / 4af8d8a (55db929)
   Batches 3-4 verified: native build without new warnings, fmt_test ALL
   OK, harness init lines unchanged on all six titles, sandbox lib builds.
   Device gate: not run yet.
5. Cheaper hot paths (MOVE): per-byte CheckRange in strchr/strstr/strtok
   family -> GuestStrnlen; GuestStrnlen without a cache; strcmp double
   pass; JNIEnv lookup, per-call RegisterSigThread, RealCallObject
   re-parsing; cond/rwlock under the global mutex — A/B MOVE lists
6. Correctness found on the way: RELRO write guard drops instead of
   halting; halting lines without "LOUD" are invisible to the gate;
   wild pointers that fail silently (fread dst, snprintf dst)
7. Android mocks of 2014 (API 21, 800x480...) and AudioOutput special
   cases: PC-only — B-R6/R7

Owner decisions pending: ensure_parent (directories on every write-open;
the owner kept it in 96acf7f), the generic 0-stub for unimplemented imports
(LOUD halt vs ENOSYS).

---

## Appendix A — audit part 1 (dispatch.cpp 1-9000, headers)

# Audit part 1: removable / movable checks, guards, crutches

Scope: host/src/guest/dispatch.cpp lines 1-9000, dispatch.h, mapping_table.h, guest_memory.h,
live_heap.h, layout.h. Code at HEAD baacdf6 (branch perf-gap). Read-only.
History note: 77f53c2 (2026-10-10) is the first commit of the repo (an import "checkpoint");
anything whose -S search lands there is older than the history and its origin is unknown.

Legend: Ship = runs in a shipping build (diag off, no env knob). Hot = per-frame hot path.
Verdict REMOVE / MOVE / KEEP, confidence H/M/L.

## A. Heap / overflow firewall family (the 2026-10-16/17 heap-corruption hunt)

Timeline (git): 53edf4f 10-16 dlmalloc replaces bump -> 8a81005 / 1bcd3b9 / 808feaa / 5744783 /
9627345 (10-16 01:12-02:17) "overflow firewall" on memcpy/memset/fread/fgets/sscanf ->
bf470b9 10-17 18:17 JNI region clamp -> **1177bac 10-17 18:28 ROOT CAUSE: JNI table off by one**
(GetStringUTFRegion served as GetStringRegion, 2*len bytes into len-byte buffer) ->
27e2161 10-18 "D1: firewall OFF by default ... That bug class is gone (1177bac)" ->
289739a / 9e0d960 10-18 D2 live index + big_live_ side index kept "exact" for the sscanf clamp.

| # | where | what | ship | cost | origin | verdict |
|---|---|---|---|---|---|---|
| A1 | dispatch.cpp:569 ClampHeapWrite (non-force), dispatch.h:202 ClampWriteOn | firewall, OFF unless HARNESS_CLAMPWRITE; but every caller still calls it and ClampWriteOn() = EnvOn() = linear pointer scan of the EnvOn table (~15-20 entries after boot) + acquire load | the bypass test runs in ship | **hot**: every memcpy/memmove/memset/__aeabi_mem*/strcpy/strcat/strncpy/strncat (GuestMemCopy 9946, GuestMemSet 9955, 3012), snprintf family 4761, fread 5813, fgets 5938, sscanf numerics 3428-3733 | 8a81005, 1bcd3b9, 9627345, 96acf7f (printf), off since 27e2161 | REMOVE (H). Root cause 1177bac; 27e2161 itself says the class is gone. Minimal step: make ClampWriteOn a `static const bool`. Also removes the strncpy/strcpy "firewall truncation" branches (3016-3027). |
| A2 | dispatch.cpp:3449-3475 sscanf %s/%c/%[ field clamp | ALWAYS ON: takes mutex_ (global runtime lock!) + LiveIdxContaining (heap_mu_, hash, std::map, up to 64-step back-probe) per string field; silently truncates a field a real sscanf would overflow | yes | per sscanf string field; sscanf is not per-frame for Rovio but mutex_ is the global lock | 5744783 (10-16 02:02, hunt); root cause 1177bac | REMOVE (H). Leftover of the same hunt that D1 turned off for every other writer; it is the only reason big_live_ (A3) exists. Real bionic has no such clamp: it hides guest overflows. |
| A3 | dispatch.cpp:469-487 LiveIdxInsert/Erase big_live_, dispatch.h:1058 std::map big_live_ | ordered std::map of every live block > 512 B: host node alloc/free + O(log n) on every >512 B malloc/calloc/realloc/free | yes | **hot-ish**: ~2.5 % of heap ops measured (9e0d960), each a host malloc/free inside heap_mu_ | 9e0d960 (10-18): "probe-only silently dropped the sscanf field clamp" | REMOVE with A2 and A4 (H). Remaining consumers of LiveIdxContaining: sscanf (A2), ClampHeapWrite (A1 + JNI force path, other agent's range :15787/15821), HeapWatchViolation (diag). live_heap.h's own header argues against a host-allocating map on this path. |
| A4 | JNI GetString(UTF)Region force=true clamp (dispatch.cpp:15787, 15821, outside my range) | the owner's example | yes | cold | bf470b9, root cause 1177bac | REMOVE (H) - noted only because it keeps A1/A3 alive. |
| A5 | dispatch.cpp:3797-3826 fread | (a) "Clamp absurd sizes" line 3799 is redundant: the next `if` returns 0 on the same condition; (b) ClampHeapWrite + "[heap] LOUD fread short" (only fires with CLAMPWRITE); (c) bad dest pointer -> silent 0, not a halt | yes | per fread | (a) pre-history 77f53c2; (b) 1bcd3b9 | REMOVE (a)(b) (H). (c) KEEP-fix: wild dst should be LOUD+halt per the rules (now silent). |
| A6 | dispatch.cpp:663 PadGlobal, :713-735 mallocpad parse, total=size+pad | HARNESS_MALLOCPAD, two parsers of one knob; realloc calls PadGlobal twice | parse once; static check per malloc/realloc | negligible | 261591a/12e6a5c; verdicts: 0bd5a51 "pad caused fresh-boot OOM", fd590f1 "MALLOCPAD=32 survived against the Pad-OFF verdict" | REMOVE (H): retired experiment. |
| A7 | dispatch.cpp:681 ZeroFillLimit + uses in malloc/realloc | HARNESS_ZEROFILL | static check per malloc | negligible | fd590f1; blanket zero-fill 1db67e6 reverted 7085a00; REVIEW R-013 precedent | REMOVE (M): hypothesis knob; zerofill=off has shipped since. |
| A8 | dispatch.cpp:704-708 HeapKnob, 987, 1026-1031, dispatch.h:977-980 | HARNESS_NORELEASE + norelease_set_ check per free; HARNESS_MALLOCLOG ring | per free: empty() check | negligible | 49f6fce (10-16 triage, quarantine era; quarantine removed R-013) | REMOVE NORELEASE (H) (quarantine gone, it now just leaks); MALLOCLOG KEEP as diag or remove (L). |
| A9 | dispatch.cpp:902-914 realloc pre-dump | DiagOn hex dump of chunk header | diag | - | b75f9ac "Diag ... to see smash content" (hunt) | REMOVE (H). |
| A10 | dispatch.cpp:775-783, 853-856, 937-940 mspace_usable_size per alloc | "honest bound for firewall/heapwatch" | yes | one extra call per malloc | 40b1c6c | MOVE (L): after A1-A4 the index only needs the request size (free/realloc validation, audio pool). Keep if heapwatch stays. |
| A11 | dispatch.cpp:244-249 HARNESS_HEAPBASE | relocates heap, no overlap check vs other regions | boot | - | pre-history 77f53c2 | REMOVE (H). |
| A12 | dispatch.cpp:1013-1022, 951-957 array_pool_ scan on every free/realloc | audio-buffer pool coherence (<= 8 entries, linear) | yes | per free, tiny | 67520fb / 53edf4f | KEEP (pool itself is the JNI agent's call). |
| A13 | GuestFree unknown/double free ignored (hx_free_unknown_), realloc unknown -> LOUD 0 | live-index validation of free | yes | one hash op per free | 289739a, 7d5a304 | KEEP (H): dlmalloc headers are host size_t in guest memory (C-11); a wild free would drive host writes. Notable: an unknown free is silent (only a diag counter); consider one LOUD line. |
| A14 | heap_used_ | malloc adds the request, free subtracts the usable size: drifts down | yes | - | - | KEEP-fix (telemetry only). |
| A15 | dispatch.h:500-502 comment "under DiagOn() they cross-check the (temporary, full) std::map oracle" | the oracle is gone (dispatch.cpp:436) | - | - | 9e0d960 | REMOVE stale comment (H). |

## B. String/memory import hot paths

| # | where | what | ship | cost | origin | verdict |
|---|---|---|---|---|---|---|
| B1 | dispatch.cpp:3089-3099 strchr/strrchr/strstr/strpbrk/strspn/strcspn gstrlen; 3155-3165 strtok gstrlen2; 8945-8955 strcasestr/strsep guest_strlen | **per-byte** CheckRange loop (thread_local + epoch load per byte) with an arbitrary 1 MiB cap (a longer valid string halts the guest as bad_ptr) | yes | **hot** for parsers (strchr/strstr) | pre-history 77f53c2 | MOVE (H): use GuestStrnlen (memchr per mapping), as strlen/strcmp already do (the same per-byte pattern in GuestStr was ~2.5 % of SW2's game thread on a phone). |
| B2 | dispatch.cpp:1495 GuestStrnlen | calls mappings_.Find uncached each time; for an mmap-arena address FindArena widens up to 256 pages each way (up to ~512 atomic loads) per call | yes | per strlen/strcmp/strdup/strcpy... (heap/module strings hit the linear entry list, heap is entry 0: cheap) | - | MOVE (M): use the CheckRange last-hit cache or a non-widening lookup. Matters for strings in mmap'd/packed data (Mono, packed files). |
| B3 | dispatch.cpp:2955-2975 strcmp family | GuestStrnlen on both strings (two memchr passes) then host strcmp (third pass); still reads guest memory in place after the check (racing writer -> could run off a mapping, the case fixed for GuestStr) | yes | hot | - | MOVE candidate for a fault-handler design (see D). Correctness note: compare with the lengths found (memcmp over min(la,lb)+1) to close the race. |
| B4 | dispatch.cpp:1604-1626 WriteWP/ScanWP | HARNESS_WP bulk watchpoint: an out-of-line call + static check on every memcpy/memset | yes (no-op) | hot, small | pre-history 77f53c2 | REMOVE or `if (DiagOn())` inline (H). |
| B5 | dispatch.cpp:3037-3065 HARNESS_MEMDUMP in memchr | lr-matched hexdump | static check per memchr | negligible | pre-history (JSON forensics) | REMOVE (H). |
| B6 | dispatch.cpp:9958-9967 GuestMemSet TEMP-DIAG (wall4) | DiagOn memset log | diag | - | - | REMOVE (M). |
| B7 | CheckAccess (628) vs CheckRange (1525) | two copies of the same epoch-checked last-hit cache, two thread_locals (emulated TLS = function call on the API-26 sandbox build) | yes | hot | - | MOVE/merge (M): one helper, one thread_local. Cost is small on the phone (ELF TLS). |
| B8 | dispatch.cpp:2264 GL fast path test `n.size()>2 && n[0]=='g'&&n[1]=='l'` + RealGLBackend() lazy non-atomic init | per import call | yes | every import | 4ae3709 | MOVE (L): test imp.gl_id (already resolved at load) only. |
| B9 | dispatch.cpp:4757 snprintf | `cap>0 && CheckRange(dst,1)` false -> silently nothing written, full length returned (A-18 open); ClampHeapWrite (A1) | yes | per sprintf | 96acf7f (printf firewall) | REMOVE clamp (H); KEEP-fix: wild dst -> LOUD halt. |

## C. Other crutches / knobs / diag leftovers in range

| # | where | what | ship | cost | origin | verdict |
|---|---|---|---|---|---|---|
| C1 | dispatch.cpp:5150-5216 __gnu_Unwind_Find_exidx nest guard + HARNESS_NESTMAX + HARNESS_EXIDXLOG | static NestTrack tracks[2] shared by all threads, unlocked; "sp" heuristic; 512 depth | yes, every exidx lookup (each unwind frame of each C++ throw) | per throw frame | pre-history 77f53c2 ("subagent #6", RETSUCC era); ROOT CAUSE fixed 6de206a 2026-10-18 "exidx ABI fix" (table base + count), docs d35f3b5 | REMOVE (M-H): the spiral came from the wrong ABI answer (the comment at 5217-5229 says so). Racy and unproven since. |
| C2 | dispatch.cpp:4989-5001 HARNESS_NOWORKER | pthread_create returns 0 with no thread | EnvOn per create | rare | pre-history (b8be7a1 era, "bump-heap nondeterminism") | REMOVE (H). |
| C3 | dispatch.cpp:5004-5048 pthread_create job/URL sniffing | TEMP-DIAG worker hang | diag | rare | pre-history | REMOVE (M). |
| C4 | dispatch.cpp:145-157 TimeWarp + uses 3942-3990 (+ localtime >9000) | HARNESS_TIMEWARP; A-19 applied twice | static check per clock_gettime/gettimeofday/time | hot-ish, negligible | 9dda957 "TIMEWARP (timer theory dead)" | REMOVE (H). |
| C5 | dispatch.cpp:239-241, 5727-5735, 5761, 5827-5846, 5858-5874 registry TEMP-DIAG | g_regfiles / g_regdump_n hex dumps (now DiagOn) | diag (DiagOn check + io_mutex_ lock only under diag) | - | b8be7a1 (10-10); "wall" fixed in the same commit (notifyAfter=1); G-03 gated it 2dadd39 | REMOVE (H). |
| C6 | dispatch.cpp:7262-7322 fcntl | TEMP-DIAG "[fcntl] stuck workers" log; unknown cmds (F_SETLK/F_GETLK/...) return 0 "success-lie" | yes | rare | a76f59d (10-10 bring-up) | REMOVE diag (H); success-lie = G-14 open: KEEP-fix (forward with a 32-bit struct flock translation or EINVAL). |
| C7 | dispatch.cpp:5365-5371 ensure_parent (fopen w/a/+, open O_CREAT/WR, mkstemp, rename) | create_directories of the parent on every write-open: masks a guest save-path bug | yes | per write-open: one stat per path component | pre-history; owner decided to keep in 96acf7f ("removing it ... could regress the saves") | KEEP-but-notable (M): a known crutch; a cheaper exact fix = create the dirs the guest expects once at boot (seeds) and drop it. Needs evidence which path. |
| C8 | dispatch.cpp:6131-6132 `[files] open(...) -> -1` | ungated printf on every failed open() | yes | per failed open (probing opens) | - | MOVE to DiagOn or cap (M). Also need_host 5355 (capped 20): fine. |
| C9 | dispatch.cpp:2265-2274, 2281-2296 preamble DiagOn socket log + "first call" audit | diag | DiagOn static check x2 per import | trivial | - | KEEP (cheap). |
| C10 | dispatch.cpp:185-212 EnvOn | presence = ON ("KEY=0" is on); comment "19 distinct keys" stale; linear scan per call | yes | becomes hot only through ClampWriteOn (A1) | 261591a | KEEP; fix A1 so no per-call EnvOn remains on hot paths. |
| C11 | dispatch.cpp:2784-2786 getauxval AT_PAGESZ, 2862 sysconf, 2393 madvise | host page size (16384 on 16 KB devices) vs guest __page_size 4096 (G-08 open); madvise calls ::sysconf per call instead of GuestMemory::HostPageSize (ignores HARNESS_PAGESIZE) | yes | rare | - | KEEP-fix (M): guest page = 4096 everywhere. |
| C12 | dispatch.cpp:8577 qsort/bsearch `total > 0x4000000` | arbitrary 64 MiB cap -> bad_ptr halt | yes | rare | 67520fb | KEEP-fix (L): bound by CheckRange only. |
| C13 | dispatch.cpp:3448 sscanf width sentinel 1000000 | magic | - | - | - | cosmetic. |
| C14 | guest_memory.h:38-50 HARNESS_PAGESIZE | test knob, read once | boot | - | 52a6ee4 | KEEP (useful 16 KB test). |
| C15 | dispatch.h:65-147 JitSafeOpts/FastDispatchOn/HostFpsrOn/JitCallbacksOn/JitShareOn/TlsThunkOn | A/B knobs, read at NewJit | boot/NewJit | none | documented in AGENTS.md | KEEP. (Ret's NOJITCALLBACKS branch: an atomic load per import, decided "keep" in REVIEW 2026-11-09.) |
| C16 | dispatch.cpp:1696-1830 OnSvc trace/LASTSVC/CALLMAX | diag (DiagOn) | static check | trivial | 7d5a304 | KEEP or trim (L). |
| C17 | dispatch.cpp:8173-8391 fake GL shim | `#ifndef __ANDROID__` now | no | - | dbcb76c | KEEP (PC harness). |
| C18 | dispatch.cpp:750-772 OOM / arena-escape branches | LOUD OOM + "escape" (impossible with a fixed mspace) | yes | cold | - | KEEP (cold, cheap). |

## D. Moving pointer validation off hot paths (owner's question)

- What CheckRange buys today on a hot path: memcpy/memset/memcmp do size==0, wrap test,
  one thread_local struct (ELF TLS on the phone; `__emutls_get_address` call on the API-26 sandbox),
  one acquire load of the epoch, two compares. That is a few ns: the real waste in my range is
  A1 (EnvOn scan per memcpy/memset/strcpy), B1 (per-byte checks), B2/B3 (extra scans, uncached Find).
- A fault-handler design (like fastmem): run the host-side bulk op directly and catch SIGSEGV whose
  host pc lies inside a small set of "guest-access" routines (own memcpy/memset/memchr/strlen
  copies compiled into a dedicated section), recover via a per-thread jump buffer, then LOUD+halt.
  It only replaces the "unmapped" half of the check: arena pages are host-RW regardless of guest
  prot (16 KB hosts, heap arena), so guest-prot (RELRO, PROT_READ-only mmaps) still needs the
  page map for writes. Realistic win: strlen/strcmp/strchr family (no pre-scan) more than memcpy.

---

## Appendix B — audit part 2 (dispatch.cpp 9000-end, apphost, realgl, loader, other)

# Audit part 2 — crutches / guards / leftovers
Scope: dispatch.cpp 9000-18724, apphost.cpp, realgl.cpp, elf_loader.*, opensl.cpp, apk_vfs.*, adpf.cpp,
sampler.cpp, prof.h, fast_math.h, math_thunks.inc, patches/dynarmic. HEAD = perf-gap baacdf6 (2026-11-10).
Read-only. Line numbers are current. "Ship" = runs in the shipping Android build with no knob.
Hot-path reference (docs/minimum.md, 2026-11-09): ~5200 host calls/frame in the SW2 menu
(memcmp ~2370, strlen ~1000, strcmp ~371, free, memcpy, floor, malloc, GL); JNI/Java->native are per-frame
but few.

## REMOVE

| # | where | what | ship? | origin / evidence | cost | verdict |
|---|---|---|---|---|---|---|
| R1 | dispatch.cpp:15779-15787 (GetStringRegion), 15821 (GetStringUTFRegion), CallJni case 220/221 | `ClampHeapWrite(buf, want, ..., force=true)`: takes mutex_, LiveIdxContaining, silently truncates the copy (log capped 10) | yes | bf470b9 2026-10-17 18:17 (clamp for heap smear); root cause 1177bac 18:28 (JNI table off by one: UTF region served as UTF-16 widen). The handler now already checks [start+len) <= length (LOUD halt) and CheckRange on buf. | rare (string regions), takes global mutex_ | REMOVE, high. A too-small buffer is a guest bug; truncating hides it. Also drop the DiagOn "Triage" block 15823-15834. |
| R2 | dispatch.cpp:17251-17270 (Get<T>ArrayElements pool reuse), 17501-17510 (ReleaseByteArrayElements push, "cap 8; overflow leaks as before"); plus scans in GuestRealloc :952 and GuestFree :1017 | `array_pool_`: Released guest copy buffers kept for reuse by exact length instead of freed; every guest free()/realloc() scans the pool; when the pool holds 8, a released buffer is LEAKED | yes | 67520fb 2026-10-16 00:29 (bump allocator era: "instead of orphaning 32KB per callback"); 53edf4f 01:07 same night replaced bump with dlmalloc (free works) and added "audio-pool coherence on free/realloc" scans instead of dropping the pool | scan on EVERY guest free/realloc (hot: hundreds/frame) + LiveIdxFind per Get; leak past 8 | REMOVE, high: GuestFree(pin.guest_buf) on release (JNI_COMMIT keeps it), GuestMalloc on Get; delete the free/realloc scans and hx_pool_stale_. |
| R3 | dispatch.cpp:18286-18343 GuestCallbacks::ExceptionRaised | "add pc, rN, pc" and "ldr/add pc" emulation on UnpredictableInstruction | yes (path) | 77f53c2 2026-10-10 (bring-up). thirdparty dynarmic arm_ADD_reg (data_processing.cpp:114-130) executes ADD Rd=PC with S=0 natively; UnpredictableInstruction only for S=1, which the emulation never matches -> unreachable (F-26, PROVEN, re-verified) | none (dead) | REMOVE, high. |
| R4 | dispatch.cpp:18355-18375 ExceptionRaised mini-backtrace | stack scan for "return addresses" in [0x1000, 0x6EE000) — Rio's text range hard-wired in the shared core; line says "-- halting" without LOUD | yes | e6e4b2b 2026-10-10 (null-jump backtrace, Rio bring-up) | rare | REMOVE the Rio range: use IsCodeAddress (as ScanStackForReturnAddresses already does) or ScanStackForReturnAddresses itself; add LOUD to the line. high |
| R5 | dispatch.cpp:17931-17950 GuestCallbacks::Write | "Fox-hunt" BAD-write dump: r4-r7 + 512-byte hex dump around r7 (Lua GC gray object) | yes (capped by NoteBad, NOT DiagOn) | 0b0c980 2026-10-15 "Hunt ... diag only" — commit claims diag-only but it is ungated | rare (bad write only) | REMOVE, high (hunt over; 512 B of noise per bad write). |
| R6 | dispatch.cpp:17868-17878 GuestCallbacks::Read | HARNESS_TRACELOADS with Rio-only VA filter `addr >= 0x650000u` | knob (static cached), slow path only | 77f53c2 2026-10-10 | static test per slow read | REMOVE, high (Rio VA in the shared core, and under fastmem — hardwired on Android — only faulting reads reach it). |
| R7 | dispatch.cpp:17964-17972 (Write: HARNESS_HEAPWATCH), 17822-17836/17880-17896/18090-18110 (HARNESS_WP / WPA value/address watchpoints in Read/Write callbacks) | heap/value/address watch on guest stores/loads | knob only; on Android fastmem is hardwired (G-13 fix dbcb76c) so normal guest stores never reach these callbacks -> they see almost nothing on device | HEAPWATCH 2026-10-1x heap hunt; WP/WPA 77f53c2 | static tristates, ~free | REMOVE (or #ifndef __ANDROID__), medium: they only work in the PC harness with fastmem off; on device they silently cover nothing. HeapWatchViolation still consults norelease_held_ (NORELEASE knob) — remove with it. |
| R8 | dispatch.cpp:18591-18612 MapGuestPathString | HARNESS_HOSTSHACK: synthetic /etc/hosts mapping cloud.rovio.com -> 127.0.0.2 | knob | 385a291 2026-10-10 (DNS-hack experiments); made obsolete by the offline policy (R-005: offline titles refuse connect with ENETUNREACH) | /etc/hosts open only | REMOVE, high (Rovio host name in shared core; REVIEW "Next steps 1" already lists it). |
| R9 | dispatch.cpp:16728-16795 (CallBooleanMethod case 37-39 startOutput real block + `startOutput -> 1` mock), 16817-16835 (CallVoidMethod AudioOutput.stopOutput/requestExclusiveAudio re-call) | name-matched AudioOutput special cases (D-21) | yes (Android), only reached after the generic real call failed | cf5c181 2026-10-14 (startOutput), a299190 2026-10-19 (stopOutput); superseded by d8adc22 2026-10-26: CallJni prologue (15596-15608) sends every Call<T>Method 37-63/117-143 to RealCallObject first. On Android these blocks run only when that real call already failed; the startOutput block retries the same call; the stop path calls RealCallObject a 2nd time with identical args | strstr/find on mdesc per call of these families (fallback only) | REMOVE, high. On Android a failed real call should log LOUD once (generic), not answer startOutput=1. |
| R10 | dispatch.cpp:17133-17170 (CallStaticInt mocks: getAPILevel 21, 800x480, PPI 240, density 1, notifyAfter 1...), 17106-17115 (hasSystemFeature substring list), 16874-16915 (String mocks, HARNESS_DEVID/LOCALE/NETTYPE) | 2014-era mock answers | Android: fallback only after the real call fails; harness: primary | 77f53c2 2026-10-10; real-first since d8adc22 | mdesc.find chains, fallback only | MOVE to `#ifndef __ANDROID__` (harness only), medium-high: on a device a fabricated 800x480/API 21 after a failed real call is a silent lie. Keep networkType "" for offline titles (that is policy, 14625). |
| R11 | dispatch.cpp:9958-9967 GuestMemSet | TEMP-DIAG "wall4 second filler" memset logging | DiagOn only, but DiagOn() is evaluated on every memset | wall4 era (2026-10-09/10) | one static-bool load per memset (hot) | REMOVE, high (hunt closed long ago). |
| R12 | realgl.cpp:1269-1276, 1306-1326, 1451-1458, 1484-1515, 833-837, 1219-1225; realgl.h:48-49 (active_tex_, bound_tex2d_, tex_size_) | TEMP-DIAG "black frame" draw/upload dumps and the texture-size tracking that feeds them | GlDrawLog (static const) only | 94144d9 2026-10-10 | ~free | REMOVE, medium (clarity; black-frame hunt closed). |
| R13 | dispatch.cpp:18164-18177, 18195-18205 InetThunk | TEMP-DIAG ntop failures | DiagOn | 2026-10-1x net bring-up | rare | REMOVE (low value). |
| R14 | apphost.cpp:884-900, 909-914, 958-971, 979-990 RunGuestNative | Diag forensics per Java->native call: [screen], [rate], PrintHeapIdx every 600 updates, [input] PLAY-button, nativeUpdate r0-polarity | DiagOn (static) | 2026-10-14..20 triage | DiagOn short-circuits strstr | REMOVE, medium (all named triages are closed). Also apphost.cpp:1316-1331 uniqcall/call# logs. |
| R15 | dispatch.cpp:13356-13373 PthreadCreate | HARNESS_WBEAT sampler thread captures `&jit` of the worker by reference and may read it after the worker exits (G-18 UAF) | knob, getenv per pthread_create | 2026-10-1x | getenv on each create | REMOVE, high. |

## MOVE (keep the protection/semantics, make the common case cheap)

| # | where | what | ship? | origin | cost | verdict |
|---|---|---|---|---|---|---|
| M1 | dispatch.cpp:9946-9953 GuestMemCopy, 9955-9973 GuestMemSet -> ClampHeapWrite(!force) -> dispatch.h:202 `ClampWriteOn()` = EnvOn("HARNESS_CLAMPWRITE") | memcpy/memmove/memset call ClampHeapWrite, which does an EnvOn table walk (pointer compares over ~20 cached keys) before bailing out | yes | firewall 8a81005 2026-10-16, made opt-in later (D1) | per memcpy/memset (hot) | MOVE: `static const bool` (or drop the knob entirely: CLAMPWRITE "keep as diag only, or delete" per G-crutches; R-013 precedent). ScanWP (WriteWP static) is already cheap. medium |
| M2 | apphost.cpp:733-737 Enter() -> dispatch.cpp:10814 ThreadEnter -> 10969 RegisterSigThread | every Java->native call (and each nested Run, twice) re-registers the thread: sig_mu_ lock, sig_threads_[tid] map op, a raw `syscall(SYS_gettid)`, shared_ptr copy into t_sig; plus sampler::ThreadUses | yes | signal proxy work (81bdbc3 era, 2026-11) | ~1 us per Java->native call (syscall ~0.4 us on sandbox), per frame 1-3x | MOVE: register once per host thread; on later calls only store the Jit pointer if it changed. medium |
| M3 | apphost.cpp RealEnvForThread (~1368) used via GuestCallbacks::g_real_env_hook / RealEnv() (dispatch.cpp:13896) | each RealEnv() takes g_tmu + unordered_map<pid_t> find + gettid; RealCallObject, WrapObject, JavaCopies dtor, FlushDirectOne each call it -> several per guest JNI call | yes | JNI real-call work 2026-10-14+ | several locks per JNI call | MOVE: cache JNIEnv* in a thread_local (one TLS read; ELF TLS on the phone build) set in Enter()/attach. medium |
| M4 | dispatch.cpp:14578-14660 RealCallObject (and 15596-15607 prologue) | per call: mutex_ + jni_methods_ find + std::string copy of mdesc, then parse into 6 std::strings, network-policy string compares, 2 more mutex_+map finds (class, method), per-arg std::string | yes | d8adc22 2026-10-26 | ~1-3 us per guest Call*Method | MOVE: cache a parsed {jclass, jmethodID, ret char, arg kinds, offline-blocked} per method cookie (a2) once. medium (value depends on JNI calls/frame; not top of the menu profile) |
| M5 | dispatch.cpp:15518 CallJni -> realgl.cpp:436 ForgetGlBindings | every guest JNI call (all 229 fns, incl. GetStringLength, GetArrayLength...) drops the GL binding/uniform caches (unordered_map clear of by_prog) | yes | F9 df99d4e 2026-11-05 (exactness proof: "return to Java") | map clear + lost uniform elision after each JNI call | MOVE: drop only for the families that run Java code (Call*/NewObject/static calls, 28-30, 34-63, 114-143) — string/array/ref functions cannot change the GL context. low-medium |
| M6 | dispatch.cpp:9622-9640 PthreadCond (signal/broadcast/wait entry) | takes the global recursive mutex_ + cond_map_ find + shared_ptr copy on every pthread_cond_signal/broadcast/wait; wait re-locks mutex_ after waking (9763) only to release it at return | yes | cond-queue rewrite (a53149d R-008 era) | per cond op; contends with every mutex_ user (JNI, malloc logs, threads) | MOVE: own shared_mutex for cond_map_ like mutex_map_mu_ (done for mutexes on 2026-11-04 "mutex trylock + map lock"). medium for MCSM/99 Bricks (many workers), low for Rovio |
| M7 | dispatch.cpp:9572-9600 PthreadRwlock | every rdlock/wrlock/unlock takes global mutex_ + rwlock_map_ lookup | yes | 2026-10 | per rwlock op | MOVE: same shared_mutex pattern. low-medium (only if a title uses rwlocks per frame) |
| M8 | dispatch.cpp:9976-9990 PthreadOnce (called from CallImport) | takes global mutex_ and CheckRange before looking at the control word; bionic's fast path is one acquire load for "done" | yes | 2026-10 | per pthread_once call | MOVE: acquire-load the control first, return at once if 2. low |
| M9 | apphost.cpp:1000-1036 AppHost_CallImpl | static unordered_map<std::string,...> keyed by name: constructs a std::string (heap alloc) + global mutex per Java->native call | yes | 2026-10 | ~100-200 ns per Java call | MOVE: per-forwarder static VA (generated) or pointer-keyed cache like frame_native. low |
| M10 | apphost.cpp:929 RunGuestNative `WrapObject(thiz)` + 14012 ReleaseHandleLocked at frame pop | each Java->native call: NewGlobalRef + 2 map inserts under mutex_, then DeleteGlobalRef + 3 erases | yes | real-object model | per Java call | MOVE (optional): wrap thiz lazily (only when the guest uses it) or keep one permanent handle per class/instance seen. low |
| M11 | apphost.cpp:917 `strstr(name, "nativeUpdate")` per Java->native call (is_update watchdog stamp) | ungated strstr | yes | watchdog | ~50 ns | MOVE: compare by pointer like frame_native (1347). low |

## KEEP but notable (fix or document)

| # | where | what | evidence | verdict |
|---|---|---|---|---|
| K1 | dispatch.cpp:17905-17924 GuestCallbacks::Write (and dispatch.h ExclusiveWrite) | RELRO write guard: a slow-path write into RELRO is REFUSED and the guest CONTINUES (no halt) | 2bb8083 2026-10-19 added it; f4802e0 2026-10-26 made RELRO PROT_READ + read-only in the mapping table "a clean halt in every mode". But under fastmem a RELRO store faults, falls back to this callback, and the guard drops it silently instead of halting. | REMOVE the guard (CheckAccess already refuses read-only; that path halts), or make it LOUD+halt. high that it is now a crutch |
| K2 | gate markers: dispatch.cpp:17846 "[mem][t] BAD read", 17929 "[mem] BAD write ... -- halting", 18070 "BAD exec", 17911 "write REFUSED", 18115 InterpreterFallback, 18354 ExceptionRaised "-- halting", 15731 and the two "region BAD buf" lines "[jni] region BAD", 15577 "[jvm] BAD env_out", 9981 "[once] BAD control", 9338 "[svc] LOUD..." ok | clean guest halts from wild pointers do not print "LOUD", so devgate (greps LOUD) misses them, esp. on worker threads | scripts/devgate.sh:54 | add LOUD to every halting line (one-line edits). Also NoteBad's bad_count is a plain non-atomic shared int. |
| K3 | dispatch.cpp:13292-13294 PthreadCreate | `attr` ignored ("NOTE non-null attr ignored", ungated printf on every create with attr): PTHREAD_CREATE_DETACHED set via pthread_attr_setdetachstate (stored by PthreadAttr, dispatch.cpp:1264) is dropped -> such threads stay in threads_ forever; stacksize ignored (always 8 MB from the 1 GB guest heap) | 77f53c2 2026-10-10 | fix: honour detachstate (st->detached) and stacksize; drop the per-create printf. medium |
| K4 | dispatch.cpp:9308-9325 CallImport default | unimplemented import -> Ret 0 + "[stub]" (5 lines/name), stub_hits_ map under mutex_ | G-02 b979090 implemented the known ones; tail kept by design | decide: LOUD halt (AGENTS "no fabrication") or ENOSYS; low frequency |
| K5 | realgl.cpp:897-902 glShaderSource | every 0x01 byte in shader source becomes '\n' | 94144d9 2026-10-10 PC bring-up (Mesa); no evidence any title's shader contains 0x01 on a device driver | needs evidence (log once if a 0x01 is ever replaced); boot-only cost |
| K6 | realgl.cpp:760-768 glGetFramebufferAttachmentParameteriv/glGetVertexAttribiv, 783-786 glGetVertexAttribPointerv | bad out pointer -> silent return (no LOUD, no halt), unlike E-07's other GL paths | E-07 fix | make LOUD+halt for consistency. low |
| K7 | realgl.cpp:1437-1447 glVertexAttribPointer | client pointer checked for 1 byte only; bad pointer silently NULL (E-04 open) | — | the 1-byte check protects nothing at draw time; real check belongs at draw (count*stride) — or in a fault handler. keep open |
| K8 | games/99-bricks/profile.toml:43 jitshare = off | per-title workaround for R-007 | 1531802 2026-10-28; root fixes 0006/0007 (5ced121, 01ebfe4, 2026-11-02) did NOT cure it (44e0c59 same day) | KEEP (still needed) |
| K9 | dispatch.cpp:10066-10073 (recursive once / depth>16 "treating as done"), 10101 ReleaseOnceControlsOnExit, 10020-10045 1 s "still waiting" LOUD | once guards | semantics/diagnostics for halted inits | KEEP |
| K10 | dispatch.cpp:9413-9545 PthreadMutex holder book (NoteLocked/NoteUnlocking) | 2 relaxed atomics + GuestTid TLS per lock/unlock; used for stall dump, UNLOCK-FOREIGN log, cond-wait restore | R-012 2026-11-04 | KEEP (cheap); could be diag-only later |
| K11 | dispatch.cpp:14988-15003 RealNewObject | "Blast-radius guard" comment describes a guard that no longer exists (every class goes real) | — | comment cleanup |
| K12 | dispatch.cpp:10566-10570 sigaltstack comment "We do not deliver guest signals" | stale since the signal work | — | comment cleanup |
| K13 | elf_loader.cpp:132-139 RELRO comment "The host keeps the pages RW" | stale since f4802e0 | — | comment cleanup |
| K14 | dispatch.cpp:14623-14627 RealCallObject networkType offline block; 16899 mock | title-specific (com/rovio/rcs) in shared core, but it is the R-005 offline policy | — | KEEP (policy) |
| K15 | dispatch.cpp:17426, 15848, 17249 size caps (0x10000000, 10000000) | arbitrary caps on JNI arrays; exceed -> LOUD halt | — | KEEP, name the constants |
| K16 | dispatch.cpp:17449-17451 Get/Set<T>ArrayRegion | Java ArrayIndexOutOfBounds silently cleared (guest continues, natively the exception would be pending) | — | low; LOUD once |
| K17 | apphost.cpp:440-680 watchdog (STUCK, 2000x1 ms sampling, stack scan 0x4000) | diagnostic thread, every 10 s | — | KEEP (off the hot path) |
| K18 | patches/dynarmic 0001-0021 | no crutch-like patch found: 0006/0007 are root fixes, 0008/0004 diag-only builds, 0003/0020 correct semantics | README.md | KEEP |
| K19 | opensl.cpp, apk_vfs.cpp, adpf.cpp, sampler.cpp, prof.h, fast_math.h | no crutches found; prof/sampler are gated by plain globals (no TLS when off) | — | KEEP |
