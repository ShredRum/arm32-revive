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
| A5 | heap quarantine | GuestFree | 53edf4f, hunt | R-013: off by default, six titles clean | delete the ring once confirmed |
| A6 DONE | `EnvOn("HARNESS_...")` on hot paths: each call scans the cached table (~50 keys): GLDRAWLOG x24 in realgl (draws, uploads, attrib pointers), MALLOCLOG x2 + NORELEASE + QUARANTINE per free | realgl.cpp, GuestFree | debug knobs | the knobs are off in shipping | read each knob once (static const) or drop the knob (C) |
| A7 | live block index next to dlmalloc (~0.4 us per malloc+free in MCSM) | LiveIdx*, GuestFree/Memalign | D2 | dlmalloc knows chunk sizes; but the index also gives "free of an unknown address is ignored" and interior lookups for sscanf/JNI bounds | investigate before touching |
| A8 DONE | interruptible (2 ms sliced) waits from the first non-fault handler install: MCSM's never-used SIGINT handler kept ~45 idle workers waking ~1500 times a second each | GuestSigaction | 906d206 (Mono finalizer hang); the proxy delivery 81bdbc3, two commits later, solves it generally | slicing starts at the first signal sent; MCSM workers ~1450 -> ~330 wakeups/s, whole process in the episode scene 144 k -> ~15.5 k/s; 99 Bricks 5/5 boots to the menu. sem_post stays ~26 us of wall time after its map left mutex_: the poster loses the CPU to the worker it wakes (scheduler, as natively) |


## B. Survive-instead-of-crash paths (no cost unless they fire)

Removing them gains no speed; it changes what happens after a bug. Decide by
reasoning, one by one.

| # | what | where | note |
|---|---|---|---|
| B1 | heap corruption: dlmalloc hook counts HEAPCORRUPT and "limps on" instead of abort | abr_heap_hook, apphost watchdog | markers have been 0 for weeks; limping hides the next bug |
| B2 | CHUNKD: a quarantined block with a bad header is leaked, zone dumped | GuestFree | goes with A5 |
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

## Order

1. A1, A6 (certain, tiny). 2. A2-A4 + B3/B4 (one change, measured).
3. A5 + B2 after a few more clean sessions. 4. C in cleanup commits, one
hunt at a time. 5. A7, B1, B5, B6 each after its own look.
