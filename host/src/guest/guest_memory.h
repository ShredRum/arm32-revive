// Raw guest memory: one 4GB reservation, sub-mapped on demand.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>

#include "layout.h"

namespace abr {

class GuestMemory {
public:
    GuestMemory() {
        base_ = static_cast<std::uint8_t*>(mmap(nullptr, 0x100000000ULL, PROT_NONE,
                                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (base_ == MAP_FAILED) {
            std::perror("mmap guest reservation");
            base_ = nullptr;
        }
    }
    ~GuestMemory() {
        if (base_) {
            munmap(base_, 0x100000000ULL);
        }
    }

    bool ok() const { return base_ != nullptr; }
    std::uint8_t* base() { return base_; }

    std::uint8_t* host(std::uint32_t guest) { return base_ + guest; }

    // Map a range with given prot (commits pages).
    bool Map(std::uint32_t guest, std::size_t size, int prot) {
        void* r = mmap(base_ + guest, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return r != MAP_FAILED;
    }

    bool Protect(std::uint32_t guest, std::size_t size, int prot) {
        return mprotect(base_ + guest, size, prot) == 0;
    }

private:
    std::uint8_t* base_ = nullptr;
};

}  // namespace abr
