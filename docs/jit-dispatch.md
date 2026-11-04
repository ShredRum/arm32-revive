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

2026-11-03: the owner played this build (sandbox and phone APKs): all works.
Merged to master. Next: R-007 lead below (own branch), with the after-numbers
of the game counters; then each small patch alone with a measurement.

## Small patches (branch `jit-opts`, 2026-11-03)

Each measured with dispatch_bench on the sandbox; one gate of all six at the end.
- fastmem-fpsr: taken as 0009. FP and memory loop 25.8 -> 9.9 ns.
- vldr-literal: not taken. It only feeds Dynarmic's read-only folding, which
  our host never enables (IsReadOnlyMemory false). Enabling it would bake
  constants into blocks far from where they live; a later mprotect to writable
  would need a full cache clear, which a shared cache never does (R-007 kind).
- vmov-select: not taken. Measured slower: conditional vmov loop 10.2 -> 11.9 ns
  (the block split it removes is cheap here, the select costs register moves).
  Its fmt_test stays: conditional vmov semantics.
- branch-fusion: not taken. Branchy loop 2.61 -> 2.59 ns (noise): block links
  are cheap here. Its test (a beq in a loop) stays.
- wide vector transfers: taken (VLD1/VST1 part) as 0010. NEON loop 24.0 -> 23.0.
- fpcr-cluster: taken as 0011. Four NEON ops 41.9 -> 19.8 ns.
- relaxed-halt: taken as 0012 (link terminals only; CheckHalt keeps LDAR).
  Branchy loop 2.50 -> 2.40 ns, call and return 2.88 -> 2.78 ns.
- inline-dispatch: not done. FastDispatch already keeps 94-98 % of indirect
  branches in generated code; nothing left to measure for it.
Gate of all six on build Nov 2 21:13:26 (0009-0012) clean; the owner played
it (sandbox and phone APKs): all works. Merged to master 2026-11-03.

## R-007 (branch `r007`, 2026-11-03)

Proven: fmt_test "R-007" failed on arm64 (a return reached the block another
Jit had flushed), passes with patch 0006. dispatch_bench: call and return still
+2.8 ns, indirect +3.4 ns (was +3.0). Remaining stale-code paths, unproven: a
block that is running while another thread flushes it (until its next exit),
and a private overflow cache, which other Jits never invalidate. 99 Bricks with jitshare on and 0006 only: 3 broken
boots of 16 (Mono `idx < t->rows`, TypeLoadException); private caches ~1 in 11,
shared before 0006: 4 in 10. No overflow cache seen. Second path proven and
fixed: 0007 (a block translated from bytes flushed meanwhile was emitted).
With 0006 and 0007, jitshare on: 2 broken boots of 5 (the same Mono errors),
stopped there. So another path remains; 99 Bricks stays jitshare=off. 0006 and
0007 stay: each fixes a bug its test reproduces, in the shared core.
Diag check (0008, `[codewatch]`, every 0.5 s in diag builds): a jitshare-on
boot broke (`idx < t->rows`, ~10 s in) with no translated block changed without
a flush. So "Mono rewrites code without cacheflush" is ruled out for it. Also
ruled out: constant folding of guest memory (the host never says read-only).
Private caches, 15 boots: 1 TypeLoadException (R-010 rate); the check then ran
every 10 s only, so it says nothing about R-010.
Gate of all six on build Nov 2 20:42:40 (0006, 0007) clean; merged to master
2026-11-03. Stopped here: no lead with evidence left. 99 Bricks stays
jitshare=off. Unchecked idea for later: a thread still inside an old block
while another flushes it.

## R-007 lead (proven above)

Each thread's RSB (on its host stack) holds code pointers. It is reset only on
entering Run. `InvalidateBasicBlocks` removes blocks from the tables and relinks
them, but does not touch other threads' RSBs: a return whose descriptor matches
jumps into the old block. That is a third suspect for stale translations under
a shared cache. Any per-thread table has the same trap: an entry must not
survive an invalidation, which is why it needs the generation check.

## Host import overhead (2026-11-04, branch `mcsm`)

Measured with the diag per-import timing (MCSM episode scene, numbers in
games/mcsm/JOURNAL.md): fast math table, GL by load-time id, one EGL context
check per thread, trylock before the sliced mutex wait, own lock for the
guest mutex map. 40-49 -> 56-60 fps on the sandbox.

Ideas kept for later, each only with a measurement that shows the need:
- Cached buffer bindings in realgl. Today BoundBuffer asks the driver
  (glGetIntegerv, ~1 us on the sandbox) at every glVertexAttribPointer and
  glDrawElements: ~17 ms/s in MCSM, ~0.35 ms a frame. A host copy was REVIEW
  E-03 (suspected, never seen failing): bindings belong to the GL context,
  and for the Rovio titles Java's GLSurfaceView creates contexts behind the
  host, so a stale "bound" would hand a buffer offset to the driver as a host
  pointer. Safe form if ever needed: cache only where the host sees every
  eglMakeCurrent (titles whose guest drives EGL, like MCSM), reset it there
  and in glDeleteBuffers; keep asking the driver for the others.
- free(): ~10 us a call inside the runtime besides the lock wait (MCSM),
  cause not found; the numbers are wall time with ~45 threads on 8 cores.
- malloc/free still take the runtime-wide mutex_.

## State

See `git log master..jit-dispatch`.
