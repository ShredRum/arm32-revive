# JIT dispatch work (branch `jit-dispatch`)

Current-truth plan, started 2026-11-02. Survives context loss: read this first.

Why: the arm64 backend leaves generated code for C++ on every indirect branch
and return (`FastDispatchHint` is a TODO there). The project is meant to scale
to heavier games, so this is done whatever the light titles gain.

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

## State

See `git log master..jit-dispatch`.
