# Working minimum: guards and leftovers to drop (inventory, 2026-11-04)

Owner's goal: keep a universal working minimum, drop crutches that no longer
do anything. Much of this code went in during the October crash hunts,
several edits at a time, and stayed after the real causes were fixed (the
heap quarantine, R-013, is the proven example: off, six titles clean on the
phone). Each item says what it guards, where it came from, what it costs on
the hot path, and what to do. "Hot" = runs on every import, lock or free in
a shipping build.

How to remove: a group at a time behind a switch or in one test build, all
titles on the sandbox and the phone; a regression is bisected. A short run
cannot show a guard for a rare event is useless, so each item also needs
the reason it went in to be fixed already (as shared_ptr fixed the mutex
destroy race).

## A. Hot path, shipping build: real cost

| # | what | where | origin | why it may be dead | do |
|---|---|---|---|---|---|
| A1 DONE | `svc_count.fetch_add` on every guest->host call (~540 k/s in MCSM, one shared cache line for all threads) | dispatch.cpp OnSvc | counters | read only by the diag `[counters]` line | diag builds only |
| A2 DONE | mutex owner books: `io_mutex_` (shared with the fd maps) + unordered_map on every lock and unlock | dispatch.h MutexNote* | c86817e 2026-10-14, hang triage | destroy no longer needs them (A4); the stall dump can read a holder kept in the host mutex object itself | holder tid/depth as atomics in the mutex object; no map, no lock |
| A3 DONE | pending-lock witness: 4 shared atomic stores per lock/unlock | PthreadMutex | 790bc7d 2026-10-14 | its job (no free under a waiter) is done by shared_ptr since e221e33 (2026-10-25) | drop |
| A4 DONE | destroy guard via books + witness | PthreadMutex destroy | 15a66e1 2026-10-15 | shared_ptr keeps a held or waited mutex alive; bionic itself only does trylock -> EBUSY | trylock only |
| A5 DONE | heap quarantine | GuestFree | 53edf4f, hunt | R-013: off by default, six titles clean | delete the ring once confirmed |
| A6 DONE | `EnvOn("HARNESS_...")` on hot paths: each call scans the cached table (~50 keys): GLDRAWLOG x24 in realgl (draws, uploads, attrib pointers), MALLOCLOG x2 + NORELEASE + QUARANTINE per free | realgl.cpp, GuestFree | debug knobs | the knobs are off in shipping | read each knob once (static const) or drop the knob (C) |
| A7 KEEP | live block index next to dlmalloc (~0.3 us per free) | LiveIdx*, GuestFree/Memalign | D2 | not a duplicate: it finds the block around an interior pointer (sscanf %s and JNI region bounds, a project rule) and lets a free of a foreign address be ignored safely | keep |
| A8 DONE | interruptible (2 ms sliced) waits from the first non-fault handler install: MCSM's never-used SIGINT handler kept ~45 idle workers waking ~1500 times a second each | GuestSigaction | 906d206 (Mono finalizer hang); the proxy delivery 81bdbc3, two commits later, solves it generally | slicing starts at the first signal sent; MCSM workers ~1450 -> ~330 wakeups/s, whole process in the episode scene 144 k -> ~15.5 k/s; 99 Bricks 5/5 boots to the menu. sem_post stays ~26 us of wall time after its map left mutex_: the poster loses the CPU to the worker it wakes (scheduler, as natively) |


## B. Survive-instead-of-crash paths (no cost unless they fire)

Removing them gains no speed; it changes what happens after a bug. Decide by
reasoning, one by one.

| # | what | where | note |
|---|---|---|---|
| B1 | heap corruption: dlmalloc hook counts HEAPCORRUPT and "limps on" instead of abort | abr_heap_hook, apphost watchdog | markers have been 0 for weeks; limping hides the next bug |
| B2 DONE | CHUNKD: a quarantined block with a bad header is leaked, zone dumped | GuestFree | goes with A5 |
| B3 DONE | DESTROY-HELD / DESTROY-PENDING: leak instead of destroy | PthreadMutex | goes with A4 |
| B4 DONE | mutex re-init of a live object: old host mutex leaked | PthreadMutex init | shared_ptr makes the leak unnecessary |
| B5 | free of an unknown address ignored ("legacy lenient no-op", counted) | GuestFree | bionic would abort; check hx_free_unknown_ on all titles first |
| B6 | realloc on a consumed block: leak deliberately | GuestMemalign/realloc | read the case before deciding |
| B7 | watchdog STUCK report, abdump, death.log | apphost | KEEP: cheap, our only eyes on the phone |

## C. Debug and triage code behind knobs (no cost when off, code weight)

About 50 `HARNESS_*` knobs and ~24 `TEMP-DIAG` blocks from finished hunts:
registry ParseError dumps, worker-hang socket/pipe/job logging, "black frame"
GL state dumps and upload checksums, wall4 memset logging, Fox-hunt GC
dumps, HEAPWATCH (needs fastmem off), NORELEASE, ZEROFILL, MALLOCPAD,
CLAMPWRITE, LASTSVC/SVCHIST, PCTRACE/PCLOG, EXIDXLOG, TRANSLOG, PATHLOG,
FILESLOG, POLLDBG, CALLMAX, JNIMAX, TIMEWARP, HOSTSHACK, DNS1/2, DEVID,
NETTYPE, SHOTSWAP/SHOTCLEAR (PC), ... Each closed hunt's block can go; keep
the few that serve every investigation: HARNESS_DIAG, ABR_DIAG_COUNTERS,
abdump, the NO* switches of the JIT features (NOJITSHARE, NOTLSTHUNK,
NOFASTDISPATCH, NOOPT), HARNESS_REALGL/PAGESIZE/HEAPBASE where tests use them.

## D. Keep (rules, not crutches)

- CheckRange/CheckAccess on every guest pointer, LOUD + clean guest halt.
- shared_ptr host mutex/rwlock/cond objects (the real fix behind A3/A4).
- Offline network policy, guest sha256 checks.
- The per-thread GL binding cache resets (correct by construction).

## E. Speed work done (2026-11-04, MCSM episode scene, sandbox)

Each one its own commit with a measurement; numbers are per call unless
said otherwise. Whole effect: 47 -> 100 fps at 120 Hz (new game, episode 1,
first 60 s, back to back), before the direct jump.

| what | before | after |
|---|---|---|
| fast-math table for every pure libm import (fast_math.h) | sinf 2.5 us | table jump |
| GL by load-time id, no per-call string/snprintf (gl_ids.h) | glUniform4fv 4.3 us | 3.1 us |
| EGL context check once per thread | 3.1 us | 2.16 us = bare driver |
| per-thread GL buffer-binding copy, dropped at every context change | glVertexAttribPointer 3.5 us | off the top list |
| mutex: trylock before the sliced wait; own map lock; holder book in the object | lock 5.7 us | ~1 us |
| semaphores: own map lock | under mutex_ | own lock (sem_post's ~20 us is the scheduler) |
| quarantine ring, then quarantine off (A5) | free 3.3 us under the lock | ~1 us |
| signals: sliced waits only after a signal is sent (A8) | 144 k wakeups/s | ~15.5 k/s |
| every import by ImportId (import_ids.h), then switch (id) | chain 1-5 us | pthread_self 99 ns, frexpf 544 ns |
| F9: GL state calls that repeat the set state are not sent: attrib enable/disable (97 % repeats), glBindBuffer (32 %), single-value uniforms (~70 %); proof of exactness in realgl.cpp (per-thread copy dropped at every context change or return to Java, one uniform thread, link/delete generation) | 84 fps (diag) | ship A/B C-A-C: 98 / 96 / 98 avg, steady half ~107 vs ~100 |
| thread_local cost (phone profiler, Rio game thread, 2 min of a level): API 29 phone build = ELF TLS (f33e610), then one TLS lookup per plain call + one for RealGL's state (d1d4752) | emutls + pthread_getspecific ~8.2 % of samples | linker64 TLS resolver 3.3 %, then 1.8 %; thread CPU per 5 s 1003 -> 966 -> 958 ms (-4.5 %, different levels each run) |

## F. Speed work not done yet (known, each needs its measurement first)

| # | what | evidence | note |
|---|---|---|---|
| F1 PART DONE | malloc/memalign/free ~2.3 us a call | MCSM ~230 k calls each per 30 s | own heap lock (ce40e83): no change per call (memalign 2257 -> 2282 ns, free 2119 -> 2270): the threads waited for each other on the one heap, not for other mutex_ users. Left: ~0.7 us lock wait + ~0.9 us under it (live index ~0.3) = ~3.5 % of one core in all. Further cut only by per-thread block caches (jemalloc-like tcache): a big change for well under 1 %; not planned |
| F2 NOT WORTH | direct jump inside the nested blocks (file bridge, assets, strings) | integer compares there, ~0.05-0.1 us, on calls that cost microseconds of syscalls | a 2200-line scripted cut for noise: dropped (2026-11-04) |
| F3 DONE | RealGL's own mutex on every GL call | one render thread mostly; ~0.3-0.6 us over the driver left | check what it guards (active_tex_, glstr slots) |
| F4 | GL on a thread of its own (queue + flush on sync calls) | driver ~1-2 us x ~150 k calls/s on the render thread | does not cut heat (same work, more threads); only if a phone measurement shows the render thread is the limit |
| F5 | the JIT itself (Dynarmic code quality, block linking, dispatch) | ~half of the process CPU is guest code | biggest reserve, biggest job; needs a profiler that works on the device |
| F6 | gettimeofday 2.8 us vs 440 ns bare on the sandbox | syscall + a loaded box | on the phone vDSO; nothing of ours, recheck there |
| F7 | sem_post ~20 us wall | the poster yields to the woken worker | scheduler, as natively: leave |
| F9 DONE | redundant GL state calls (see E) | MCSM: 2.9 M attrib, 2 M uniform calls / 30 s, mostly repeats | sandbox render thread spent ~2/3 of its time in GL (ANGLE); the phone driver is cheaper, so less gain there |
| F8 DONE | phone measurements (fps, heat) with MCSM | 2026-11-08: split APKs (packed files), 118-120 fps at 120 Hz, slightly warm; main thread ~83 %, no thread at 100 % | GL thread (F4) not needed |
| F10 PART DONE | profiler data (phone, Rio game thread) | sampler.h, 2026-11-08 | done: Host FPSR (dynarmic 0014, 7344b32): MSR/MRS FPSR before JIT samples ~2-5 % -> 0; Ret sets only the T bit (0015) and CallSVC builds the CPSR only for blocking calls; CheckRange/CheckAccess caches one thread_local (f7b3c8e); TLS resolver 1.8 -> 1.1 %. Game-thread CPU per 5 s across the session: 1003 (start) -> 966 (API 29) -> 958 (one TLS per call) -> 946 ms (FPSR, CPSR) = -5.7 %, each run a different level (+-2 %). Block lookups in the C++ dispatcher (~0.6 %): measured, 98 % were fast-table conflicts; dynarmic 0016 (Fibonacci hash, two-way sets) cuts C++ dispatches per host call 7.9 % -> 0.27 % (Rio, sandbox). Handlers' thread_locals: the sampler names the caller of linker64 (TLS resolver) and libc syscall() samples (`leaf` lines). MCSM phone profiles (2026-11-08, 120 fps both): main SDLThread guest JIT ~72 %, host ours ~11.6 %, libc ~8.7 %. syscall 5.6 % = sem_post 4.3 % + pthread_cond_signal 0.6 %: bionic's sem_post enters the kernel only when a thread waits, so these are the engine's own wakeups of its workers, the same natively. Guest atomics (LDREX/STREX through the one exclusive monitor): ~3.5 % (spin lock page ~2 %, shown as `anon 0x...40xx` and counted as guest (private cache); CheckAndClear 0.6 %; CAS 0.4 %); dynarmic 0017 (scan only used ids) changed nothing measurable: the cost is the lock line bouncing between the two game threads. t_java_copies init wrapper gone (a71d6cf). Remaining TLS resolver ~1 %, spread over CallSVC, CheckAccess, the exclusive trampolines (tls_a32_binding). |

## Order

1. A1, A6 (certain, tiny). 2. A2-A4 + B3/B4 (one change, measured).
3. A5 + B2 after a few more clean sessions. 4. C in cleanup commits, one
hunt at a time. 5. A7, B1, B5, B6 each after its own look.
