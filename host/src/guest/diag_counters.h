// Stage D diagnostic counters. The whole struct exists only when the host is
// configured with -DABR_DIAG_COUNTERS=ON; the shipping build has no fields,
// no increments and no dump strings.
#pragma once

#ifdef ABR_DIAG_COUNTERS
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace abr {

// A cheap timestamp for diag timing: the arm64 virtual counter (readable from
// user space on Linux/Android), steady_clock elsewhere (PC builds).
inline std::uint64_t DiagTicks() {
#if defined(__aarch64__)
    std::uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
inline std::uint64_t DiagTickHz() {
#if defined(__aarch64__)
    std::uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::period::den /
                                      std::chrono::steady_clock::period::num);
#endif
}

struct DiagCounters {
    // JNI bump arenas: bytes/handles used and high-water. They only grow
    // today, but keep the high-water explicit so a future reuse cannot hide a
    // peak.
    std::uint32_t jni_str_high = 0;  // bytes from kJniStringStart
    std::uint32_t jni_obj_high = 0;  // 4-byte handles from kJniObjectStart
    // Mock jstring bytes placed on the guest heap after the arena filled.
    std::uint64_t jni_str_heap_bytes = 0;
    // Guest heap: live bytes (heap_used_) high-water.
    std::uint64_t heap_peak = 0;
    // Per-thread errno/TLS reaps (bridge worker exit or host thread exit).
    std::atomic<std::uint64_t> tls_reaps{0};
    // localtime()/gmtime() static-buffer allocations: cumulative calls/bytes.
    // Updated outside mutex_ (CallImport does not hold it), hence atomics.
    std::atomic<std::uint64_t> localtime_allocs{0};
    std::atomic<std::uint64_t> localtime_bytes{0};
    // Real ART global refs created by WrapObject (one per unique object).
    std::uint64_t wrap_globals = 0;
    // Guest calls to JNI NewGlobalRef/DeleteGlobalRef. They are no-ops in the
    // mock layer; these counts show whether the guest ever asks to release a
    // ref (Stage E decision).
    std::atomic<std::uint64_t> jni_new_global_refs{0};
    std::atomic<std::uint64_t> jni_delete_global_refs{0};
    // Real-wrapped GetStringUTFChars: guest copies allocated/released.
    // live = jni_utf_chars_.size() at dump time.
    std::atomic<std::uint64_t> utf_allocs{0};
    std::atomic<std::uint64_t> utf_releases{0};
    // AAsset getBuffer model: peak simultaneously open handles/bytes. The
    // current values are recomputed from asset_map_ at dump time.
    std::uint64_t asset_peak_open = 0;
    std::uint64_t asset_peak_bytes = 0;
    // Guest import calls and host time (DiagTicks units), per import index.
    static constexpr std::size_t kImportSlots = 8192;
    // Waits for the runtime's mutex_ at three hot sites (0 guest-mutex map
    // lookup, 1 malloc, 2 free), and time inside the host lock of a guest
    // pthread_mutex_lock (the guest's own contention). DiagTicks units.
    std::atomic<std::uint64_t> rt_wait[3] = {};
    std::atomic<std::uint64_t> rt_wait_n[3] = {};
    std::atomic<std::uint64_t> guest_lock_wait = 0;
    std::atomic<std::uint64_t> import_calls[kImportSlots] = {};
    std::atomic<std::uint64_t> import_ns[kImportSlots] = {};
};

}  // namespace abr

#endif  // ABR_DIAG_COUNTERS
