// D2 (2026-10-18, PLAN.md): flat index of LIVE guest allocations
// (base -> usable payload size), replacing the std::map<std::uint32_t,
// std::uint32_t> that used to be maintained on every guest alloc/free.
//
// WHY it exists (the measured shape of the old cost, not just taste):
//   * std::map allocated a node from the HOST heap on every guest malloc and
//     freed it on every guest free -- a hidden second allocator round-trip
//     (bionic malloc/free with its own locking) inside the guest's allocator,
//     plus O(log n) pointer chasing over scattered nodes on the hottest path in
//     the emulator, plus one host allocation failure mode on that path.
//   * Its main consumer (the dlmalloc overflow firewall) has been OFF by
//     default since D1, and heapwatch is off too -- yet the structure was still
//     maintained unconditionally.
// This table is flat (one 8-byte entry per live block), touches one or two
// cache lines per operation and never allocates per operation: it only grows by
// doubling, which happens O(log n) times over a whole session.
//
// SEMANTICS: base lookups (insert / erase / find) are exact, which is what the
// live consumers need (free-of-unknown whitelist, realloc guard, audio-pool
// liveness, JNI region bound). A hash table cannot answer "which block contains
// this INTERIOR pointer" (no ordering), so Runtime answers containment in two
// exact tiers: blocks larger than the probe's reach are ordered in a small
// side-index (an interior farther than the reach can only live in a block bigger
// than it), everything at or below the reach is resolved by a bounded backwards
// base probe. Measured: probe-only was NOT equivalent -- every real in-arena
// interior query is farther than 512 B back, so those tier-1 blocks carry the
// exact answer the old ordered map used to give.
//
// The randomized differential self-test against std::map lives in harness.cpp
// (HARNESS_LIVEIDX_TEST=1) so this structure is verified, not asserted.
#ifndef ABR_LIVE_HEAP_H
#define ABR_LIVE_HEAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace abr {

class LiveHeapIndex {
public:
    // Guest base 0 is never a real allocation (the arena starts high), so a 0
    // base means "empty slot". A slot with a base but size 0 is a tombstone.
    static constexpr std::uint32_t kEmpty = 0;

    LiveHeapIndex() { Rehash(1u << 10); }

    std::size_t Size() const { return live_; }
    std::size_t Capacity() const { return tab_.size(); }
    std::size_t Tombstones() const { return tomb_; }
    // Reshape accounting (D2 closing measurement): grown_ counts doublings,
    // compacted_ in-place rehashes at the same capacity, shrunk_ halvings.
    // Reshapes are the only O(n) work this structure ever does, and they happen
    // inside a guest alloc -- these counters (plus Runtime's worst-insert timer,
    // which captures the spike they cause) exist to check that claim.
    std::uint32_t Grown() const { return grown_; }
    std::uint32_t Compacted() const { return compacted_; }
    std::uint32_t Shrunk() const { return shrunk_; }
    unsigned long long Ops() const { return ops_; }
    std::size_t MaxLive() const { return max_live_; }
    // Diag-only self-check: live entries with size > sz. Proves the ordered
    // side-index of large blocks (Runtime::big_live_) stays in sync with this
    // table without trusting its own bookkeeping.
    std::size_t CountLargerThan(std::uint32_t sz) const {
        std::size_t n = 0;
        for (const Entry& e : tab_) {
            if (e.base != kEmpty && e.size != kEmpty && e.size > sz) {
                ++n;
            }
        }
        return n;
    }

    void Clear() {
        for (auto& e : tab_) {
            e.base = kEmpty;
            e.size = 0;
        }
        live_ = tomb_ = 0;
    }

    void Insert(std::uint32_t base, std::uint32_t size) {
        ++ops_;
        if (base == kEmpty) {
            return;
        }
        MaybeReshape();
        std::size_t i = Slot(base);
        std::size_t first_tomb = kNoSlot;
        for (std::size_t n = 0; n < tab_.size(); ++n, i = (i + 1) & Mask()) {
            Entry& e = tab_[i];
            if (e.base == kEmpty) {
                const std::size_t at = (first_tomb == kNoSlot) ? i : first_tomb;
                if (first_tomb != kNoSlot) {
                    --tomb_;
                }
                tab_[at].base = base;
                tab_[at].size = size;
                ++live_;
                if (live_ > max_live_) {
                    max_live_ = live_;
                }
                return;
            }
            if (e.size == kEmpty) {  // tombstone: remember the first one
                if (first_tomb == kNoSlot) {
                    first_tomb = i;
                }
                continue;
            }
            if (e.base == base) {  // update in place (no size_ change)
                e.size = size;
                return;
            }
        }
        // Ran out of slots: shape is out of policy (MaybeReshape keeps >=30%
        // free), so force a grow and retry once. Cannot recurse forever because
        // Rehash(Grow) always doubles.
        Grow();
        Insert(base, size);
    }

    // Returns the stored size, or 0 when the base is not live.
    std::uint32_t Erase(std::uint32_t base) {
        ++ops_;
        if (base == kEmpty) {
            return 0;
        }
        std::size_t i = Slot(base);
        for (std::size_t n = 0; n < tab_.size(); ++n, i = (i + 1) & Mask()) {
            Entry& e = tab_[i];
            if (e.base == kEmpty) {
                return 0;  // probe chain ended: not present
            }
            if (e.base == base && e.size != kEmpty) {
                const std::uint32_t sz = e.size;
                e.size = kEmpty;  // tombstone (base kept for compaction/debug)
                --live_;
                ++tomb_;
                return sz;
            }
        }
        return 0;
    }

    // Returns the stored size, or 0 when the base is not live.
    std::uint32_t Find(std::uint32_t base) const {
        if (base == kEmpty) {
            return 0;
        }
        std::size_t i = Slot(base);
        for (std::size_t n = 0; n < tab_.size(); ++n, i = (i + 1) & Mask()) {
            const Entry& e = tab_[i];
            if (e.base == kEmpty) {
                return 0;
            }
            if (e.base == base && e.size != kEmpty) {
                return e.size;
            }
        }
        return 0;
    }

private:
    struct Entry {
        std::uint32_t base = kEmpty;
        std::uint32_t size = 0;  // 0 = tombstone (base != 0)
    };
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    std::vector<Entry> tab_;
    std::size_t live_ = 0;
    std::size_t tomb_ = 0;
    std::size_t max_live_ = 0;
    std::uint32_t grown_ = 0;
    std::uint32_t compacted_ = 0;
    std::uint32_t shrunk_ = 0;
    unsigned long long ops_ = 0;

    std::size_t Mask() const { return tab_.size() - 1; }

    // Fibonacci multiply-shift. Bases are 8-aligned, so drop the low bits
    // first: alignment must not pile everything into a few slots.
    std::size_t Slot(std::uint32_t base) const {
        return static_cast<std::size_t>(((base >> 3) * 2654435761u)) & Mask();
    }

    void MaybeReshape() {
        // Idle shrink: after a level unloads, a huge live set leaves a huge
        // table behind. Halve once per call while it is >=8x oversized (the
        // condition stops well before the table could become small for the
        // current live set).
        if (tab_.size() > (1u << 12) && live_ * 8 <= tab_.size()) {
            ++shrunk_;
            Rehash(tab_.size() / 2);
            return;
        }
        // Keep occupancy <=70% AND tombstones below half the live set: probe
        // chains stay short without growing the table on every erase.
        if ((live_ + tomb_) * 10 >= tab_.size() * 7) {
            if (live_ * 2 <= tab_.size()) {
                ++compacted_;
                Rehash(tab_.size());  // compaction is enough
            } else {
                Grow();
            }
            return;
        }
        if (tomb_ > 64 && tomb_ > live_) {
            ++compacted_;
            Rehash(tab_.size());  // compaction
        }
    }

    void Grow() { Rehash(tab_.size() * 2); }

    void Rehash(std::size_t cap) {
        std::vector<Entry> old;
        old.swap(tab_);
        if (cap < 16) {
            cap = 16;
        }
        tab_.assign(cap, Entry{});
        live_ = 0;
        tomb_ = 0;
        if (!old.empty() && cap > old.size()) {
            ++grown_;
        }
        for (const Entry& e : old) {
            if (e.base != kEmpty && e.size != kEmpty) {
                InsertNoReshape(e.base, e.size);
            }
        }
    }

    void InsertNoReshape(std::uint32_t base, std::uint32_t size) {
        std::size_t i = Slot(base);
        while (tab_[i].base != kEmpty) {
            i = (i + 1) & Mask();
        }
        tab_[i].base = base;
        tab_[i].size = size;
        ++live_;
        if (live_ > max_live_) {
            max_live_ = live_;
        }
    }
};

}  // namespace abr

#endif  // ABR_LIVE_HEAP_H
