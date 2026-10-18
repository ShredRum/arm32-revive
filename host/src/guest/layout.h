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
inline constexpr std::uint32_t kSwiHalt = 0;
inline constexpr std::uint32_t kSwiJniBase = 0x100000u;

}  // namespace abr
