// Raw guest memory: one 4GB reservation, sub-mapped on demand, with a guard
// behind it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <vector>
#include <unistd.h>
#include <cstdlib>

#include "layout.h"

namespace abr {

class GuestMemory {
public:
    // Never accessible, right after the 4 GB: generated code reads and writes
    // at base + a 32-bit guest address, so an access that starts in the last
    // bytes of the guest space ends up to 7 bytes past it. Without the guard
    // those bytes are whatever the host mapped there.
    static constexpr std::size_t kGuard = 0x10000;

    GuestMemory() {
        base_ = static_cast<std::uint8_t*>(mmap(nullptr, 0x100000000ULL + kGuard, PROT_NONE,
                                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (base_ == MAP_FAILED) {
            std::perror("mmap guest reservation");
            base_ = nullptr;
        }
    }
    ~GuestMemory() {
        if (base_) {
            munmap(base_, 0x100000000ULL + kGuard);
        }
    }

    bool ok() const { return base_ != nullptr; }
    std::uint8_t* base() { return base_; }

    std::uint8_t* host(std::uint32_t guest) { return base_ + guest; }

    // Host page size: the granularity of every host mmap/mprotect below.
    // Guest code assumes 4 KiB pages (its segments are 4 KiB aligned), but
    // Android 15+ devices may run with 16 KiB pages; HARNESS_PAGESIZE lets the
    // native harness exercise that granularity on a 4 KiB host.
    static std::size_t HostPageSize() {
        static const std::size_t ps = [] {
            if (const char* e = std::getenv("HARNESS_PAGESIZE")) {
                const unsigned long v = std::strtoul(e, nullptr, 0);
                if (v >= 4096 && (v & (v - 1)) == 0) {
                    return static_cast<std::size_t>(v);
                }
            }
            const long v = ::sysconf(_SC_PAGESIZE);
            return v > 0 ? static_cast<std::size_t>(v) : std::size_t(4096);
        }();
        return ps;
    }

    // Map [guest, guest+size) with prot. Works in host pages: pages that are
    // already mapped (another segment sharing the page) keep their contents
    // and get the union of both protections; new pages are zero-filled.
    bool Map(std::uint32_t guest, std::size_t size, int prot) {
        return Apply(guest, size, prot, true);
    }

    // Change the protection of [guest, guest+size). Host pages fully inside
    // the range get exactly prot; a page only partly covered (shared with a
    // neighbouring range) keeps the union, so the neighbour stays usable.
    bool Protect(std::uint32_t guest, std::size_t size, int prot) {
        return Apply(guest, size, prot, false);
    }

private:
    static constexpr std::uint8_t kUnmapped = 0xFF;

    bool Apply(std::uint32_t guest, std::size_t size, int prot, bool map) {
        if (size == 0) {
            return true;
        }
        const std::uint64_t hp = HostPageSize();
        const std::uint64_t lo = std::uint64_t(guest);
        const std::uint64_t hi = lo + size;
        if (hi > 0x100000000ULL) {
            return false;
        }
        if (page_prot_.empty()) {
            page_prot_.assign(static_cast<std::size_t>(0x100000000ULL / hp), kUnmapped);
        }
        const std::uint64_t first = lo / hp;
        const std::uint64_t last = (hi + hp - 1) / hp;  // exclusive
        // Work out each page's new protection, then apply runs of equal state.
        std::uint64_t run_start = first;
        int run_prot = -1;
        bool run_new = false;
        auto flush = [&](std::uint64_t end_page) -> bool {
            if (run_prot < 0 || end_page <= run_start) {
                return true;
            }
            void* at = base_ + run_start * hp;
            const std::size_t len = static_cast<std::size_t>((end_page - run_start) * hp);
            if (run_new) {
                return mmap(at, len, run_prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
                       MAP_FAILED;
            }
            return mprotect(at, len, run_prot) == 0;
        };
        for (std::uint64_t pg = first; pg < last; pg++) {
            const std::uint8_t old = page_prot_[static_cast<std::size_t>(pg)];
            const bool is_new = (old == kUnmapped);
            if (is_new && !map) {
                return false;  // protecting memory that was never mapped
            }
            const bool full = (pg * hp >= lo) && ((pg + 1) * hp <= hi);
            const int np = is_new ? prot : (full ? prot : (old | prot));
            if (np != run_prot || is_new != run_new) {
                if (!flush(pg)) {
                    return false;
                }
                run_start = pg;
                run_prot = np;
                run_new = is_new;
            }
            page_prot_[static_cast<std::size_t>(pg)] = static_cast<std::uint8_t>(np);
        }
        return flush(last);
    }

    std::vector<std::uint8_t> page_prot_;  // per host page; kUnmapped if never mapped
    std::uint8_t* base_ = nullptr;
};

}  // namespace abr
