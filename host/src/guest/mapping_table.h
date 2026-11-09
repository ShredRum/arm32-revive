#pragma once
// The table of guest memory mappings: which guest ranges exist and with which
// protection. Read on every bounds check by every guest thread without a lock,
// written rarely (boot, mmap, munmap, mprotect).
//
// Two parts:
//  - Entries: the boot mappings (modules, heap, stacks, tables). An entry
//    lives in a fixed array, so an append never moves an entry a reader is
//    looking at; it is filled in completely and only then published by a
//    release store of the count. Its protection is atomic and may change
//    (RELRO).
//  - The mmap arena [kMmapArenaBase, kTrampBase): one byte per 4 KiB guest
//    page, 0 = not mapped, else 0x80 | prot. Guest mmap/munmap/mprotect work
//    on any page range there, as the kernel's do (a MAP_FIXED over part of a
//    mapping, a partial munmap, an mprotect that removes access: Boehm's GC
//    releases and takes back heap pages this way), and freed ranges are found
//    again by the next mmap.
// Every change bumps epoch(), which lets per-thread lookup caches notice it.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "layout.h"

namespace abr {

class MappingTable {
public:
    // Boot mappings (modules, heap, stacks, tables).
    static constexpr std::size_t kCapacity = 1024;
    static constexpr std::uint32_t kArenaBase = kMmapArenaBase;
    static constexpr std::uint32_t kArenaEnd = kTrampBase;
    static constexpr std::uint32_t kArenaPages = (kArenaEnd - kArenaBase) >> 12;
    static constexpr std::uint8_t kMapped = 0x80;

    MappingTable() : arena_(new std::atomic<std::uint8_t>[kArenaPages]) {
        for (std::uint32_t i = 0; i < kArenaPages; i++) {
            arena_[i].store(0, std::memory_order_relaxed);
        }
    }

    struct Hit {
        std::uint32_t start = 0;
        std::uint32_t end = 0;  // exclusive; 0 = no mapping
        int prot = 0;
        // Every page of [start, end) has `prot`. False when the range asked
        // for spans pages that differ: `prot` is then their intersection,
        // right for that range only, and a cache must not reuse the hit for
        // a part of it.
        bool uniform = true;
    };

    static bool InArena(std::uint32_t start, std::uint64_t end) {
        return start >= kArenaBase && end <= kArenaEnd;
    }

    // Returns false when the table is full. A range inside the arena is set
    // in the page map (start and end page aligned).
    bool Add(std::uint32_t start, std::uint32_t end, int prot) {
        if (InArena(start, end)) {
            SetArena(start, end, true, prot);
            return true;
        }
        std::lock_guard<std::mutex> l(write_mu_);
        const std::uint32_t n = count_.load(std::memory_order_relaxed);
        if (n >= kCapacity) {
            return false;
        }
        entries_[n].start = start;
        entries_[n].end = end;
        entries_[n].prot.store(prot, std::memory_order_relaxed);
        count_.store(n + 1, std::memory_order_release);
        return true;
    }

    // The mapping that holds all of [addr, addr+size), with the protection
    // every byte of it has. In the arena: the pages of the range must all be
    // mapped; the hit is widened over neighbouring pages with the same
    // protection (for the callers' last-hit caches) and its protection is
    // the intersection when the range spans pages that differ. Elsewhere:
    // the first entry that contains the range.
    Hit Find(std::uint32_t addr, std::uint64_t size) const {
        const std::uint64_t end = std::uint64_t(addr) + size;
        if (addr >= kArenaBase && addr < kArenaEnd) {
            return FindArena(addr, end);
        }
        const std::uint32_t n = count_.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < n; i++) {
            const Entry& e = entries_[i];
            if (addr >= e.start && end <= e.end) {
                return Hit{e.start, e.end, e.prot.load(std::memory_order_acquire)};
            }
        }
        return Hit{};
    }

    // Changes the protection of the entry that is exactly [start, end), or
    // of the arena pages [start, end) when they are all mapped.
    bool SetProt(std::uint32_t start, std::uint32_t end, int prot) {
        if (InArena(start, end)) {
            for (std::uint32_t p = Page(start); p < Page(end); p++) {
                if (!(arena_[p].load(std::memory_order_relaxed) & kMapped)) {
                    return false;
                }
            }
            SetArena(start, end, true, prot);
            return true;
        }
        std::lock_guard<std::mutex> l(write_mu_);
        const std::uint32_t n = count_.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < n; i++) {
            if (entries_[i].start == start && entries_[i].end == end) {
                entries_[i].prot.store(prot, std::memory_order_release);
                epoch_.fetch_add(1, std::memory_order_acq_rel);
                return true;
            }
        }
        return false;
    }

    // Arena pages [start, end) (page aligned) become mapped with `prot`, or
    // not mapped.
    void SetArena(std::uint32_t start, std::uint32_t end, bool mapped, int prot) {
        std::lock_guard<std::mutex> l(write_mu_);
        const std::uint8_t v = mapped ? static_cast<std::uint8_t>(kMapped | (prot & 7)) : 0;
        for (std::uint32_t p = Page(start); p < Page(end); p++) {
            arena_[p].store(v, std::memory_order_release);
        }
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    // True when no arena page of [start, end) is mapped.
    bool ArenaFree(std::uint32_t start, std::uint64_t end) const {
        if (!InArena(start, end)) {
            return false;
        }
        for (std::uint32_t p = Page(start); p < Page(static_cast<std::uint32_t>(end - 1)) + 1; p++) {
            if (arena_[p].load(std::memory_order_acquire) & kMapped) {
                return false;
            }
        }
        return true;
    }

    // The lowest free run of `len` bytes (aligned to `align`) at or above
    // `from`, then from the arena base; 0 when there is none.
    std::uint32_t ArenaFindFree(std::uint32_t from, std::uint64_t len, std::uint32_t align) const {
        for (int pass = 0; pass < 2; pass++) {
            std::uint64_t a = pass == 0 ? from : kArenaBase;
            a = (a + align - 1) & ~std::uint64_t(align - 1);
            while (a + len <= kArenaEnd) {
                std::uint64_t used = 0;
                for (std::uint64_t q = a; q < a + len; q += 4096) {
                    if (arena_[Page(static_cast<std::uint32_t>(q))].load(std::memory_order_acquire) &
                        kMapped) {
                        used = q;
                        break;
                    }
                }
                if (!used) {
                    return static_cast<std::uint32_t>(a);
                }
                a = (used + 4096 + align - 1) & ~std::uint64_t(align - 1);
            }
        }
        return 0;
    }

    // Changes whenever a protection changes or arena pages come or go. A
    // cache that stored a Hit must drop it when the epoch moved.
    std::uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

    // Every boot entry, then every run of arena pages with one protection.
    template <typename F>
    void ForEach(F f) const {
        const std::uint32_t n = count_.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < n; i++) {
            f(entries_[i].start, entries_[i].end, entries_[i].prot.load(std::memory_order_acquire));
        }
        std::uint32_t p = 0;
        while (p < kArenaPages) {
            const std::uint8_t v = arena_[p].load(std::memory_order_acquire);
            std::uint32_t q = p + 1;
            while (q < kArenaPages && arena_[q].load(std::memory_order_acquire) == v) {
                q++;
            }
            if (v & kMapped) {
                f(kArenaBase + (p << 12), kArenaBase + (q << 12), v & 7);
            }
            p = q;
        }
    }

private:
    static std::uint32_t Page(std::uint32_t a) { return (a - kArenaBase) >> 12; }

    Hit FindArena(std::uint32_t addr, std::uint64_t end) const {
        if (end > kArenaEnd || end <= addr) {
            return Hit{};
        }
        const std::uint32_t first = Page(addr);
        const std::uint32_t last = Page(static_cast<std::uint32_t>(end - 1));
        const std::uint8_t v0 = arena_[first].load(std::memory_order_acquire);
        if (!(v0 & kMapped)) {
            return Hit{};
        }
        std::uint8_t prot = v0 & 7;
        bool uniform = true;
        for (std::uint32_t p = first + 1; p <= last; p++) {
            const std::uint8_t v = arena_[p].load(std::memory_order_acquire);
            if (!(v & kMapped)) {
                return Hit{};
            }
            uniform = uniform && v == v0;
            prot &= v & 7;
        }
        std::uint32_t lo = first, hi = last;
        if (uniform) {
            constexpr std::uint32_t kWiden = 256;
            while (lo > 0 && first - lo < kWiden &&
                   arena_[lo - 1].load(std::memory_order_acquire) == v0) {
                lo--;
            }
            while (hi + 1 < kArenaPages && hi - last < kWiden &&
                   arena_[hi + 1].load(std::memory_order_acquire) == v0) {
                hi++;
            }
        }
        return Hit{kArenaBase + (lo << 12), kArenaBase + ((hi + 1) << 12), prot, uniform};
    }

    struct Entry {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::atomic<int> prot{0};
    };
    std::array<Entry, kCapacity> entries_{};
    std::unique_ptr<std::atomic<std::uint8_t>[]> arena_;
    std::atomic<std::uint32_t> count_{0};
    std::atomic<std::uint32_t> epoch_{0};
    std::mutex write_mu_;
};

}  // namespace abr
