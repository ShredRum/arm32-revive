// Guest 32-bit address-space layout (all inside one 4GB host reservation).
#pragma once
#include <cstdint>

namespace abr {

inline constexpr std::uint32_t kGuestAddrSpace = 0x100000000ULL & 0xFFFFFFFFu;  // 4GB
inline constexpr std::uint32_t kHeapBase = 0x10000000u;
inline constexpr std::uint32_t kHeapSize = 0x40000000u;  // 1GB (256MB starved
// long runs: leak-only free + per-update textures hit the ceiling ~call 30,
// mallocs return NULL -> null+off faults downstream; arena ends 0x50000000,
// mmap arena starts 0x70000000, no overlap)
inline constexpr std::uint32_t kTrampBase = 0xF0000000u;
inline constexpr std::uint32_t kTrampSize = 0x00010000u;  // 64KB
inline constexpr std::uint32_t kJniTableBase = 0xF0010000u;
inline constexpr std::uint32_t kJniTableSize = 0x00010000u;
inline constexpr std::uint32_t kGuestDataBase = 0xF1000000u;
inline constexpr std::uint32_t kGuestDataSize = 0x00010000u;
inline constexpr std::uint32_t kStackBase = 0xFE000000u;
inline constexpr std::uint32_t kStackSize = 0x00800000u;  // 8MB
inline constexpr std::uint32_t kStackTop = kStackBase + kStackSize;

// SVC numbers. Import trampoline i executes `svc #(i+1)`; swi 0 = HALT.
inline constexpr std::uint32_t kSwiHalt = 0;
inline constexpr std::uint32_t kSwiJniBase = 0x100000u;

}  // namespace abr
