#pragma once
// The table of guest memory mappings: which guest ranges exist and with which
// protection. Read on every bounds check by every guest thread without a lock,
// written rarely (boot, mmap, munmap).
//
// Thread safety: entries live in a fixed array, so an append never moves an
// entry a reader is looking at (a std::vector push_back could reallocate under
// a concurrent reader). A new entry is filled in completely and only then
// published by a release store of the count. The protection of an entry is
// atomic and may change (munmap); every change bumps epoch(), which lets
// per-thread lookup caches notice it.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace abr {

class MappingTable {
public:
    // Boot mappings (modules, heap, stacks, tables) plus every guest mmap.
    static constexpr std::size_t kCapacity = 1024;

    struct Hit {
        std::uint32_t start = 0;
        std::uint32_t end = 0;  // exclusive; 0 = no mapping
        int prot = 0;
    };

    // Returns false when the table is full.
    bool Add(std::uint32_t start, std::uint32_t end, int prot) {
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

    // First entry that contains [addr, addr+size). Entries never overlap in
    // practice; "first" keeps the historical rule if they ever do.
    Hit Find(std::uint32_t addr, std::uint64_t size) const {
        const std::uint64_t end = std::uint64_t(addr) + size;
        const std::uint32_t n = count_.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < n; i++) {
            const Entry& e = entries_[i];
            if (addr >= e.start && end <= e.end) {
                return Hit{e.start, e.end, e.prot.load(std::memory_order_acquire)};
            }
        }
        return Hit{};
    }

    // Changes the protection of the entry that is exactly [start, end).
    bool SetProt(std::uint32_t start, std::uint32_t end, int prot) {
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

    // Changes whenever an existing entry's protection changes. A cache that
    // stored a Hit must drop it when the epoch moved.
    std::uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

    template <typename F>
    void ForEach(F f) const {
        const std::uint32_t n = count_.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < n; i++) {
            f(entries_[i].start, entries_[i].end, entries_[i].prot.load(std::memory_order_acquire));
        }
    }

private:
    struct Entry {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::atomic<int> prot{0};
    };
    std::array<Entry, kCapacity> entries_{};
    std::atomic<std::uint32_t> count_{0};
    std::atomic<std::uint32_t> epoch_{0};
    std::mutex write_mu_;
};

}  // namespace abr
