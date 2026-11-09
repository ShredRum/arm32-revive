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

## 0012-halt-poll-on-backward-links.patch

A linked block polled the halt flag with LDAR at every link. Now only a link
to the same or a lower guest PC polls it, with a plain LDR: every cycle of
blocks contains such a link, so a spinning loop still stops (fmt_test halts
one from another thread), and straight code soon reaches the dispatcher or an
SVC, which check too. dispatch_bench: branchy loop 2.50 -> 2.40 ns, call and
return 2.88 -> 2.78 ns.

## 0013-guest-pc-for-host-pc.patch

For the host's sampling profiler (host/src/guest/sampler.h):
`Jit::GuestPcForHostPc(host_pc)` answers which guest block's code holds a
host address, searching the Jit's (shared) code cache and its private
overflow cache: the guest PC with bit 0 for Thumb, -2 for the cache's
shared stubs (dispatcher), -1 when outside both. It takes the cache's lock
(shared), so the profiler calls it from its reporter thread, never from
the signal handler. The x64 backend answers -1.

## 0014-host-fpsr.patch

`OptimizationFlag::Unsafe_HostFpsr` (arm64, A32). Upstream cleared the host
FPSR before the first FP operation of each block and merged it into the state
at the block's end and before every host call; each FPSR write serialises
the pipeline (phone profile, Rio: an MSR/MRS FPSR before ~5 % of JIT samples).
With the flag, Run/Step save the host's FPSR on the stack and load the
guest's flags into it, the exit ORs the register into the state and restores
the host's; FpsrManager emits nothing; VMRS/VMSR FPSCR read and write the
register too; and `Jit::Fpscr()`/`SetFpscr()` called while the Jit runs (a
host call on its thread: our setjmp/longjmp) read and write the register.
Inexact, as the flag says: flags raised by host code during a run (callbacks,
the fastmem fallback: fmt_test sees IXC) and the QC of packed saturating ops
reach the guest's flags. No guest of ours reads them (llvm-objdump of every
guest library: only VMRS APSR_nzcv). Host switch: HARNESS_NOHOSTFPSR=1.
fmt_test (arm64): the guest's divide-by-zero seen by VMRS, live in a host
call's Fpscr(), cleared by its SetFpscr(), and the host FPSR untouched.
Idea from MrEmpathy's absw2 bridge (no licence: own code).

## 0015-set-thumb.patch

`Jit::SetThumb(bool)`: sets only the T bit. Every return from a host-served
import restores the caller's Thumb state from LR bit 0; doing it with
`Cpsr()` and `SetCpsr()` built the whole CPSR from five fields and parsed it
back (~1.5 % of Rio's game thread in a phone profile). The x64 backend does
it through Cpsr/SetCpsr.

## 0016-fast-dispatch-two-way.patch

The fast dispatch table of 0005 missed far more than it should: in Rio on
the sandbox ~8 % of host calls' worth of indirect branches still went to
the C++ dispatcher, 98 % of them because their entry held another target
(diagnostic counters `fast_evict` / `fast_stale`, added here). The index
was `(pc >> 1) ^ upper` masked: for ARM code (PC bits 0-1 zero) only half
the entries were ever used, and targets 8 KiB apart shared one.

Now: Fibonacci hashing (`descriptor * 2^64/phi`, top bits; every bit of the
descriptor counts) and two-way sets of one 64-byte cache line (way 0 the
latest target, way 1 the one before), same 4096 entries, same 128 KiB per
Jit, table 64-byte aligned. Measured, Rio on the sandbox, C++ dispatches
per host call: 7.9 % before, 5.6 % with a better shift-xor hash, 3.5 %
Fibonacci one-way, 0.27 % Fibonacci two-way (0.24 % of those were
first-time lookups and memo misses). The stub's hit path adds an LDR of the
constant and a MUL in place of LSR/EOR/AND; a hit in way 1 costs one more
LDP from the same line.

## 0017-monitor-scan-used-ids.patch

Every STREX ran `CheckAndClear`, which cleared the address in all
`processor_count` slots under the monitor's spin lock: 64 for us, about ten
in use. Now `ReadAndMark` (under the same lock) keeps `processors_marked`,
one past the highest id it ever marked, and the arm64 `CheckAndClear` scans
only that far; ids above it never held a reservation, so the result is the
same. Shorter hold of the one lock every guest atomic takes: in a phone
profile of Minecraft: Story Mode the monitor was ~3-4 % of the game thread
(CheckAndClear 0.7 %, the spin lock ~2 % as two game threads contend). The
x64 backend's inline exclusives write the table without ReadAndMark, so its
CheckAndClear is unchanged.

## 0018-per-jit-callbacks.patch

`Jit::SetCallbacks(UserCallbacks*)`: the call trampolines of the arm64
backend (memory, CallSVC, ExceptionRaised, ticks) load `this` from the
running Jit's state (`A32JitState::callbacks`, through Xstate) instead of a
literal of `UserConfig::callbacks`. Code is shared between Jits (0001), so
before this every Jit's code called one object, and the host found the
calling Jit and thread through a thread_local on every call. On Android a
thread_local of a dlopen'ed library is a call into the linker (API 29+) or
emutls (older), on the path of every guest-to-host call. Now
`Runtime::NewJit` gives each Jit its own `GuestCallbacks` that knows its
Jit; `CallSVC` needs no thread_local, and `Ret` none either (it leaves the
Thumb bit in PC bit 0, `FinishSvc` applies it once the handler returns).

Requirements: the per-Jit object has the dynamic type of
`UserConfig::callbacks` (the trampolines call that type's functions,
devirtualized at emission; asserted by comparing vtable pointers) and lives
as long as its Jit. Translation (`MemoryReadCode`, the pre-translation hook)
and exclusive accesses through the global monitor still use
`UserConfig::callbacks`. The x64 backend returns false and the host keeps
its thread_local path there.
Measured (`FMT_BENCH=1 fmt_test`, a guest loop calling getpid through the
real dispatch path, sandbox, API 26 build): 28.6-33.4 ns per call with it,
61.5-71.2 ns with `HARNESS_NOJITCALLBACKS=1`.

## 0019-relaxed-halt-poll-after-svc.patch

The two halt polls left with LDAR on hot paths are plain loads now: the
CheckHalt terminal (after every SVC, i.e. every guest-to-host call) and the
dispatcher entry. Same reasoning as 0012: a poll only has to see another
thread's halt eventually, and one requested by a callback of this thread in
program order; the exit path reads and clears the flag with LDAXR/STLXR.
Run entry keeps LDAR (once per Run). Sandbox FMT_BENCH: 30.0-30.2 -> 29.9-30.0
ns per call (noise); the phone profile of MCSM had ~0.8 % of JIT samples just
after an LDAR, to be checked there.

## 0020-generic-timer-user-reads.patch

The ARM generic timer from user mode: CNTVCT (`mrrc p15, 1, Rt, Rt2, c14`)
and CNTFRQ (`mrc p15, 0, Rt, c14, c0, 0`), with no coprocessor attached.
Linux lets user mode read them (the vDSO's clocks do), so on a phone a
32-bit process reads them directly; Dynarmic raised UndefinedInstruction
and the guest halted. Angry Birds Stella's OpenSSL reads CNTVCT at static
init (`_armv7_tick`, OPENSSL_cpuid_setup). arm64 backend: `MRS CNTVCT_EL0`
and `MRS CNTFRQ_EL0`, the same counter. x64 backend (PC tools): the host's
steady clock at 24 MHz. A coprocessor a user attaches for cp15 still wins.
Which reads are the timer is decided in one place,
`frontend/A32/a32_generic_timer.h`, used by the frontend and both backends.
fmt_test: CNTVCT twice, not decreasing; CNTFRQ nonzero.
