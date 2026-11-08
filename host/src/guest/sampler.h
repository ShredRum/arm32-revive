// Sampling profiler: where each thread that runs guest code spends its CPU
// time, down to the guest block. Off unless asked for:
//   HARNESS_SAMPLE=<ms>      (env or files/abflags.txt), or
//   setprop debug.abport.sample <ms>   (Android; a shell, rish or adb, may
//                                       set debug.* properties, no root)
// <ms> is the CPU time between samples of one thread (1 if not a number).
// HARNESS_SAMPLE_CLOCK=wall / debug.abport.sample_clock wall: sample every
// <ms> of real time instead (a kernel with broken per-thread CPU clocks,
// like the sandbox's; or to see where threads wait).
//
// Each such thread gets a timer on its own CPU-time clock that sends it
// SIGPROF; the handler stores the interrupted program counter in the
// thread's ring and nothing else (async-signal-safe). A reporter thread
// wakes every 5 s, drains the rings, resolves each address (guest block
// through a resolver Jit that shares the code cache: Jit::GuestPcForHostPc,
// patches/dynarmic 0013; JIT stubs; host libraries through dladdr) and logs,
// per thread: the split guest JIT / JIT stubs / host (ours, GL driver, libc,
// other), the hottest guest blocks as <library>+<offset> (offsets as in the
// library file, for llvm-objdump), and the hottest host places as
// <library>+<offset> or <library>:<symbol> (libabhost offsets: llvm-addr2line
// on the unstripped build). Lines go to stdout (apphost.log) and, on
// Android, to logcat with the tag abhost, so a phone without root reads them.
//
// A CPU-time timer fires only while its thread runs, but a signal can still
// end a blocking call early (EINTR): a diagnostics tool, not for normal play.
// A title without the shared code cache (jitshare=off) shows its guest
// blocks as "guest (private cache)".
#pragma once

#include <cstdint>
#include <string>

namespace Dynarmic::A32 {
class Jit;
}

namespace abr::sampler {

// Reads the switch; starts the reporter when on. Call once at boot.
void Init();
bool On();
// A Jit sharing the code cache that lives as long as the process and never
// runs: the reporter resolves host addresses through it.
void SetResolver(Dynarmic::A32::Jit* jit);
// A guest library for the report: [base, end) of its guest addresses.
void AddModule(const std::string& name, std::uint32_t base, std::uint32_t end);
// The calling thread runs guest code from now on: its first call starts the
// thread's timer (stopped when the thread exits). Cheap when off or repeated.
void ThreadUses();

}  // namespace abr::sampler
