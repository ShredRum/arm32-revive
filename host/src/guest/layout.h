// Guest 32-bit address-space layout (all inside one 4GB host reservation).
#pragma once
#include <cstdint>

namespace abr {

inline constexpr std::uint32_t kHeapBase = 0x10000000u;
// 1 GB (starved at 256 MB on long runs: per-update textures + guest assets hit
// the ceiling ~update 30, mallocs returned NULL -> null+off faults downstream).
// Arena ends 0x50000000, the mmap arena starts 0x70000000: no overlap. Mapped
// NORESERVE, so the size is virtual only (AGENTS §51; live usage ~250 MB).
inline constexpr std::uint32_t kHeapSize = 0x40000000u;
// Guest mmap() results live in [kMmapArenaBase, kTrampBase), above the heap.
inline constexpr std::uint32_t kMmapArenaBase = 0x70000000u;
inline constexpr std::uint32_t kTrampBase = 0xF0000000u;
inline constexpr std::uint32_t kTrampSize = 0x00010000u;  // 64KB
inline constexpr std::uint32_t kJniTableBase = 0xF0010000u;
inline constexpr std::uint32_t kJniTableSize = 0x00010000u;
inline constexpr std::uint32_t kGuestDataBase = 0xF1000000u;
inline constexpr std::uint32_t kGuestDataSize = 0x00010000u;
inline constexpr std::uint32_t kStackBase = 0xFE000000u;
inline constexpr std::uint32_t kStackSize = 0x00800000u;  // 8MB
inline constexpr std::uint32_t kStackTop = kStackBase + kStackSize;
// G5.1: guest stack handed to every CREATED thread (both the guest's own
// pthread_create threads and the Java->native host threads). Same size as the
// main stack but a distinct concept, so it gets its own name. 8MB x thread
// churn is what once exhausted the 1GB arena (F1).
inline constexpr std::uint32_t kThreadStackSize = 0x00800000u;  // 8MB

// SVC numbers. Import trampoline i executes `svc #(i+1)`; swi 0 = HALT.
// kSwiHalt is the generic "guest code returned to the host" trampoline, used as
// the LR target for init calls, thread start routines and qsort comparators --
// it says NOTHING about which of those returned. The pthread_once init routine
// gets its own trampoline so its return is unambiguous: with a shared HALT a
// bystander thread's exit used to be consumed as "the init routine finished",
// teleporting that thread onto the initialising thread's saved PC/SP.
inline constexpr std::uint32_t kSwiHalt = 0;
inline constexpr std::uint32_t kSwiOnce = 0x200000u;  // pthread_once init returned
inline constexpr std::uint32_t kSwiJniBase = 0x100000u;

// ARM32 kernel-user helper page (high vector page). Old bionic/libgcc code
// calls these fixed kernel VAs directly; the guest never maps them itself.
// Seasons is the first title that needs one (its static GNU libstdc++ calls
// 0xffff0fa0 through a function-pointer table). The whole page is emulated:
// known entries get svc trampolines, every other word is a LOUD trap.
inline constexpr std::uint32_t kUserHelperBase = 0xFFFF0000u;
inline constexpr std::uint32_t kUserHelperSize = 0x00001000u;
inline constexpr std::uint32_t kUserCmpxchg64 = 0xFFFF0F60u;
inline constexpr std::uint32_t kUserBarrier = 0xFFFF0FA0u;
inline constexpr std::uint32_t kUserCmpxchg = 0xFFFF0FC0u;
inline constexpr std::uint32_t kUserGetTls = 0xFFFF0FE0u;
inline constexpr std::uint32_t kUserTlsSlot = 0xFFFF0FF0u;
inline constexpr std::uint32_t kUserVersion = 0xFFFF0FFCu;
inline constexpr std::uint32_t kUserVersionValue = 5u;

// Kuser SVC numbers: above kSwiJniBase (0x100000) and kSwiOnce (0x200000),
// so OnSvc must check this range BEFORE the JNI range.
inline constexpr std::uint32_t kSwiKuserBase = 0x300000u;
inline constexpr std::uint32_t kSwiKuserBarrier = kSwiKuserBase + 0u;
inline constexpr std::uint32_t kSwiKuserCmpxchg = kSwiKuserBase + 1u;
inline constexpr std::uint32_t kSwiKuserCmpxchg64 = kSwiKuserBase + 2u;
inline constexpr std::uint32_t kSwiKuserGetTls = kSwiKuserBase + 3u;
inline constexpr std::uint32_t kSwiKuserBad = kSwiKuserBase + 4u;

}  // namespace abr
