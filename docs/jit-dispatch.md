# JIT dispatch work (branch `jit-dispatch`)

Current-truth plan, started 2026-11-02. Survives context loss: read this first.

Why: the arm64 backend leaves generated code for C++ on every indirect branch
that is not a return (`FastDispatchHint` is a TODO there). Returns are NOT the
problem: `bx lr`, `pop {pc}` go through the return stack (RSB) in generated code.
What pays is virtual calls, function pointers, switch tables, `bx rN`. The
project is meant to scale to heavier games, so this is done whatever the light
titles gain.

Owner's rule (2026-11-02): no gain on the current games is NOT a reason to drop
it. Drop it only if it cannot be made reliable or makes something worse (faults,
R-010 more often, 99 Bricks slower through its cacheflush storms).

Idea source: phys-winner/arm64-bridge-bl2 (GPLv3, AGPL for mnbw). Ideas only:
write our own code. Their table is per Jit and cleared on every invalidation;
ours has one code cache and 99 Bricks flushes up to 15000 times a frame.

## Steps

1. Done: patch 0003 (unattached coprocessor = clean halt, mcr p15 barriers run).
2. Measure: diag-only counters, C++ dispatches per second and `tls_lookup_memo`
   hits; Rio, Seasons, 99 Bricks incl. the first-brick freeze. simpleperf does
   not work on the sandbox, so the time share is an estimate. This is the
   before/after baseline, not a go/no-go.
3. Synthetic heavy load: guest code with many calls and returns, on the sandbox,
   to see the real benefit.
4. FastDispatch: table per thread, lazy reset through a generation counter (as
   `tls_lookup_memo`). Open: table size, where generated code finds the table,
   how to read the generation cheaply.
5. Verify: fmt_test arm64 with invalidation from another thread, six gates,
   repeated 99 Bricks boots (R-010 rate), numbers before/after.
6. The owner plays, then merge to master.

Later, each alone and measured: return stack (inline dispatch), fastmem FPSR,
relaxed halt, instruction patterns. Not here: GL thread, shader cache, directory
cache (ideas for heavy games). Not taken: the fully mapped 4 GB memory model.

## Measured

`host/src/dispatch_bench.cpp`, sandbox, shared cache, one thread, one target
(best case for the memo), 2026-11-02, before FastDispatch:
loop iteration 2.1 ns; call and return +3.0 ns; indirect call and return +17.5 ns.
So one trip through the C++ dispatcher costs about 14.5 ns.

Games, diag build, sandbox, 2026-11-02 (menus and first screens, blind taps):
Rio 0.94 M dispatches/s, memo misses ~0; Seasons 6.7 M/s, ~0; 99 Bricks 1.2 M/s,
all memo misses because the memo is off with private caches (jitshare=off), so
each one takes the shared lock too. At 14.5 ns: Rio ~1.4 %, Seasons ~10 % of one
core, more on heavier code. An estimate, not a profile.

## Design decided

- Table lives with the Jit (pointer in A32JitState), reached from Xstate, so
  shared generated code stays per thread. 4096 entries of 32 bytes, calloc'ed.
- Entry = descriptor, code pointer, generation. The stub compares descriptor AND
  generation: an entry from before an invalidation simply misses. No clearing,
  so 99 Bricks' 15000 flushes a frame cost nothing here.
- Only the C++ dispatcher fills it, from the primary cache. A private overflow
  cache starts at a far generation, so its stub never matches primary entries.
- Hint terminals keep jumping to the one stub; no per-site inline cache.

## Result (2026-11-02)

Patch 0005, `fastdispatch=on` in the `flags:` line, `HARNESS_NOFASTDISPATCH=1`
turns it off. dispatch_bench on the sandbox: indirect call and return +17.0 ns
without, +2.95 ns with it; a plain call and return is +2.8 ns. fmt_test has a
stale-code test across two Jits; with the generation compare removed it fails,
so it does guard that compare. fmt_test ALL OK on PC and arm64, six gates clean
on build Nov 2 04:14:33, screenshots read, harness baselines checked. 99 Bricks
12 boots: no Mono error. That is weak against a 1-in-11 fault (R-010): about 32 %
chance of seeing none even if the rate is unchanged, so it shows no jump only.

Open: the owner plays this build, then merge. Later: return stack leak above
(R-007 lead), then each small patch alone with a measurement.

## R-007 lead (not proven)

Each thread's RSB (on its host stack) holds code pointers. It is reset only on
entering Run. `InvalidateBasicBlocks` removes blocks from the tables and relinks
them, but does not touch other threads' RSBs: a return whose descriptor matches
jumps into the old block. That is a third suspect for stale translations under
a shared cache. Any per-thread table has the same trap: an entry must not
survive an invalidation, which is why it needs the generation check.

## State

See `git log master..jit-dispatch`.
