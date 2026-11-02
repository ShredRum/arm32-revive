# Local dynarmic patches

`thirdparty/dynarmic` is not in git. It is a checkout of
https://github.com/azahar-emu/dynarmic at commit
`a46601580d5512d324104f985b5f0209dc980ddc` with the patches below applied
in order (`git -C thirdparty/dynarmic apply ../../patches/dynarmic/NNNN-*.patch`).
After changing the checkout, refresh the patch with `git -C thirdparty/dynarmic diff`.

## 0001-shared-code-cache.patch

Jits created with `Jit(conf, other.GetSharedCodeCache())` compile into and
run from one code cache (arm64 backend; the x64 backend ignores it, so PC
builds keep private caches). Used by `Runtime::NewJit` for every guest Jit.
Why: each guest thread used to compile all the code it ran again, 60-160 ms
for a new worker, which opened a race window in the engine's HTTP code
(R-005 in REVIEW.md).

What makes sharing safe:

- `AddressSpace::cache_mutex` (shared_mutex) guards the block tables and
  emission; lookups take it shared, emission and the fastmem SIGSEGV callback
  exclusively. Running code takes no lock. Translation to IR (most of the
  compile time) runs outside the lock, in parallel; a thread that loses the
  race to emit a block drops its IR.
- Each thread memoizes its lookups (`tls_lookup_memo`), so dispatching does
  not contend on the lock; any invalidation bumps `generation`, which empties
  the memos.
- A new block is made coherent (`mem.invalidate`) before existing blocks are
  relinked to it.
- Relinking is safe while the block runs elsewhere: a branch site is one
  instruction (B/NOP), and the RSB return address is an aligned 64-bit
  literal loaded by one LDR and rewritten by one atomic store (upstream used
  an ADRP+ADD pair, which a concurrent fetch could see half-patched).
- The exclusive monitor processor id comes from the running Jit
  (`tls_a32_binding`, set by `Jit::Run/Step`), not from the shared config.
- A shared cache is never cleared (other threads may be inside it). When it
  has less than 2 MiB left, a thread continues in its own private overflow
  cache, which behaves like upstream (clears itself when full).
- `ClearCache` must not be used on shared Jits. `InvalidateCacheRange` on a
  shared cache is NOT reliable for a guest that rewrites code (REVIEW R-007):
  with Mono (99 Bricks) stale translations ran, 4 boots in 10 broke; with
  private caches 0 in 20. Known gaps: the ranges are applied only when the
  flushing thread's Run stops, and a thread's private overflow cache is not
  invalidated by other threads. A title whose guest has a JIT sets
  `host.jitshare = "off"` in its profile; the host then sends every
  cacheflush to every live Jit.

Measured on the sandbox (Star Wars): steady-state frame time unchanged
(500 frames 12.2-12.5 s either way); boot to the 100th frame ~0.1 s slower
(3.4-3.6 s vs 3.3-3.4 s); a new worker no longer spends 60-160 ms compiling.

Kill switch: `HARNESS_NOJITSHARE=1` (env or `files/abflags.txt`); the boot
`flags:` line shows `jitshare=on|off`.

## 0002-tpidruro.patch

`mrc p15, 0, Rt, c13, c0, 3` reads a field of the running Jit's state, set with
`Jit::SetTpidruro` (arm64 and x64 backends). No Coprocessor object is involved:
upstream's coprocessor interface compiles a fixed host address or callback
argument into the code, which on a shared code cache would be the address of
whichever thread compiled the block. Used by the `pthread_getspecific` thunk
(`kTlsGetThunk`, `Runtime::BindTls`). No guest library reads this register
itself (raw scan of all executable segments, ARM and Thumb encodings).
Refresh this patch by diffing against the tree with 0001 applied:
`git diff` alone shows both patches together.

## 0003-unattached-coprocessor.patch

An instruction for a coprocessor that is not attached is UNDEFINED: the
frontend raises `Exception::UndefinedInstruction` at that instruction (the
guest halts cleanly, LOUD). Upstream reaches `ASSERT_FALSE` in the emitter and
terminates the process while compiling; a guest branch into data decodes such
words often. Exceptions: the TPIDRURO read (0002) and the three `mcr p15`
barriers of ARMv7 (deprecated but valid), which become DMB, DSB and ISB.
`TranslationOptions::coprocessors` carries the mask from `UserConfig`.

## 0004-dispatch-counters.patch

With `DYNARMIC_DISPATCH_COUNTERS` (set by the diagnostic host build on arm64):
`A32::DispatchCount()` and `A32::DispatchMemoMissCount()`, printed in the
`[counters]` line. Nothing is compiled in otherwise.

## 0005-fast-dispatch.patch

The arm64 backend's dispatcher left generated code for C++ on every indirect
branch that the return stack did not answer. Now its stub probes a table first:
4096 entries of 32 bytes, one table per Jit, reached from `A32JitState` so that
shared code stays per thread. Only `A32AddressSpace::Dispatch` fills it.

An entry is valid for one `generation` of the code cache: the stub compares the
descriptor and the generation, so an invalidation needs no clearing, which
matters with thousands of cacheflushes a frame. A private overflow cache starts
at a far generation, so primary entries never match in it.
