#pragma once
// HARNESS_PROF=1: where a slow frame's time goes. The host marks one thread
// (Unity's render thread) and times each frame there (one nativeRender call)
// and every host call the guest makes inside it (CallSVC: an import or a JNI
// function). A frame over kSlowNs is reported: its length, the time in host
// calls (the top ones by name) and the rest, which is guest code run by the
// JIT plus Dynarmic compiling new guest code. Wall-clock timing, no signals:
// sampling is not reliable in every sandbox. Diagnostics only.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace abr::prof {

inline constexpr std::uint64_t kSlowNs = 100'000'000;  // 100 ms

inline thread_local bool t_on = false;  // this thread's frames are timed
inline thread_local int t_depth = 0;    // host calls in progress (nested ones are not added twice)
inline thread_local std::unordered_map<std::uint32_t, std::uint64_t> t_ns;  // swi -> ns
inline thread_local std::unordered_map<std::uint32_t, std::uint32_t> t_calls;
// Guest cacheflushes during the frame (any thread): each drops translations
// that then compile again.
inline std::atomic<std::uint64_t> g_flushes{0}, g_flush_bytes{0};
// Guest data accesses that took the callback path instead of fastmem (this
// thread): an access that faulted once stays on the slow path.
inline thread_local std::uint64_t t_slow_mem = 0;
inline thread_local std::uint64_t t_slow_mem0 = 0;
inline thread_local std::uint64_t t_flushes0 = 0, t_flush_bytes0 = 0;

inline std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Times one host call when this thread is profiled (outermost calls only).
class CallTimer {
public:
    explicit CallTimer(std::uint32_t swi) : swi_(swi), t0_(t_on && t_depth++ == 0 ? NowNs() : 0) {}
    ~CallTimer() {
        if (!t_on) {
            return;
        }
        if (--t_depth == 0 && t0_) {
            t_ns[swi_] += NowNs() - t0_;
            t_calls[swi_]++;
        }
    }
    CallTimer(const CallTimer&) = delete;
    CallTimer& operator=(const CallTimer&) = delete;

private:
    std::uint32_t swi_;
    std::uint64_t t0_;
};

// One frame: starts the per-call totals; FrameEnd() reports a slow frame
// through `log`, naming host calls with `name(swi)`.
inline std::uint64_t FrameBegin() {
    t_on = true;
    t_depth = 0;
    t_ns.clear();
    t_calls.clear();
    t_flushes0 = g_flushes.load(std::memory_order_relaxed);
    t_flush_bytes0 = g_flush_bytes.load(std::memory_order_relaxed);
    t_slow_mem0 = t_slow_mem;
    return NowNs();
}

template <typename Name, typename Log>
void FrameEnd(std::uint64_t t0, Name name, Log log) {
    const std::uint64_t total = NowNs() - t0;
    if (total < kSlowNs) {
        return;
    }
    std::uint64_t host = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> rows;
    for (const auto& kv : t_ns) {
        host += kv.second;
        rows.emplace_back(kv.second, kv.first);
    }
    std::sort(rows.rbegin(), rows.rend());
    std::string top;
    for (std::size_t i = 0; i < rows.size() && i < 6; i++) {
        char b[160];
        std::snprintf(b, sizeof b, " %s=%.1fms/%u", name(rows[i].second).c_str(),
                      static_cast<double>(rows[i].first) / 1e6, t_calls[rows[i].second]);
        top += b;
    }
    log("[prof] slow frame %.1f ms: host calls %.1f ms, guest code + JIT compile %.1f ms "
        "(cacheflush %llu, %llu bytes; slow memory accesses %llu);%s",
        static_cast<double>(total) / 1e6, static_cast<double>(host) / 1e6,
        static_cast<double>(total - std::min(host, total)) / 1e6,
        static_cast<unsigned long long>(g_flushes.load() - t_flushes0),
        static_cast<unsigned long long>(g_flush_bytes.load() - t_flush_bytes0),
        static_cast<unsigned long long>(t_slow_mem - t_slow_mem0), top.c_str());
}

}  // namespace abr::prof
