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

## 0006-return-stack-generation.patch

Each return stack (RSB) entry also records the code cache generation at the
push, and a return compares it: an invalidation since the call sends the return
to the dispatcher. Without it, with a shared cache, a Jit returned into a block
another Jit had invalidated (the flush halts only the flushing Jit, the only
one whose RSB is reset): fmt_test "R-007" reproduces it in one thread. One of
the stale-code paths of REVIEW R-007.

## 0007-translate-again-after-flush.patch

A shared cache translates outside its lock. A block whose bytes another Jit
rewrote and flushed while it was being translated used to be emitted from the
old bytes. `GetOrEmit` now notes the generation before translating and, if it
changed by the time it holds the lock, translates again under the lock, where
no flush can come in between (so no retry loop). fmt_test "R-007" reproduces
the old behaviour in one thread.

## 0008-diag-changed-code.patch

Diagnostic builds only (`DYNARMIC_DISPATCH_COUNTERS`). Each translated A32
block remembers a hash of its guest words (read through `MemoryReadCode`, as a
block may end at an unreadable word); `Jit::DiagChangedCode` reports blocks
still in the cache whose words changed with no invalidation on two calls in a
row. The host's watchdog calls it every 10 s and prints `[codewatch]` (R-007).

## 0009-fastmem-fpsr-on-fallback.patch

A fastmem load or store spilled the FPSR (MRS, ORR, STR) before every direct
access, though only the callback branch needs it (C++ there may change the
host FPSR). The spill now happens on that branch, with a reload after the
callback when the direct branch counts on a loaded FPSR. dispatch_bench, FP and
memory loop: 25.8 ns -> 9.9 ns. fmt_test checks a guest divide-by-zero flag
across a faulting access and that the callback's own flags stay out (arm64).

## 0010-vld1-vst1-whole-registers.patch

VLD1/VST1 with one element per structure, little-endian, move each D register
with one 64-bit access instead of one access per element (up to 8 for .8).
Same bytes (fmt_test). dispatch_bench NEON load-add-store loop 24.0 -> 23.0 ns:
most of that loop is the vector add, not the memory.

## 0011-fpcr-once-per-vector-run.patch

A32 NEON float math runs in the standard FPSCR mode, so each operation set the
FPCR before and restored the guest's after itself: two MSRs, about 10 ns per
operation. A run of add/sub/mul/mul-add with only FPCR-neutral nodes between
them (register file moves, integer and bitwise work) now sets it once at its
start and restores it once at its end; both ends answer the same adjacency
question, so they agree. dispatch_bench, four NEON ops: 41.9 -> 19.8 ns.
fmt_test checks that a VFP scalar op between them keeps the guest's mode (it
fails if that op counts as neutral).
