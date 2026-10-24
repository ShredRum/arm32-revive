// Stage D diagnostic counters. The whole struct exists only when the host is
// configured with -DABR_DIAG_COUNTERS=ON; the shipping build has no fields,
// no increments and no dump strings.
#pragma once

#ifdef ABR_DIAG_COUNTERS
#include <atomic>
#include <cstdint>

namespace abr {

struct DiagCounters {
    // JNI bump arenas: bytes/handles used and high-water. They only grow
    // today, but keep the high-water explicit so a future reuse cannot hide a
    // peak.
    std::uint32_t jni_str_high = 0;  // bytes from kJniStringStart
    std::uint32_t jni_obj_high = 0;  // 4-byte handles from kJniObjectStart
    // Guest heap: live bytes (heap_used_) high-water.
    std::uint64_t heap_peak = 0;
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
    // AAsset getBuffer model: peak simultaneously open handles/bytes. The
    // current values are recomputed from asset_map_ at dump time.
    std::uint64_t asset_peak_open = 0;
    std::uint64_t asset_peak_bytes = 0;
};

}  // namespace abr

#endif  // ABR_DIAG_COUNTERS
