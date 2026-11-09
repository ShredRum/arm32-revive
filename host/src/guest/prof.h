#pragma once
// HARNESS_PROF=1: where a frame's time goes. The host times each frame on
// the thread that makes them (one nativeRender call for Unity, one
// nativeUpdate for the Rovio fusion engine) and every host call the guest
// makes inside it (CallSVC: an import or a JNI function). A frame over
// kSlowNs is reported on its own: its length, the time in host calls (the
// top ones by name) and the rest, which is guest code run by the JIT plus
// Dynarmic compiling new guest code. Every kSummaryNs the thread also logs
// the averages per frame: frame, host calls, guest, and the host calls with
// the most time (ms per frame, calls per frame, ns per call). Wall-clock
// timing, no signals: sampling is not reliable in every sandbox (the
// sandbox delivers a timer's signal only at a system call). The timing
// itself costs two clock reads per host call. Diagnostics only.
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
inline constexpr std::uint64_t kSummaryNs = 10'000'000'000;  // 10 s

// Set once at boot when HARNESS_PROF is on: with it off, the hot paths test
// this plain global and touch no thread_local (emulated TLS on Android < 29
// is a call, not an instruction).
inline bool g_enabled = false;
inline thread_local bool t_on = false;  // this thread's frames are timed
inline thread_local int t_depth = 0;    // host calls in progress (nested ones are not added twice)
inline thread_local std::unordered_map<std::uint32_t, std::uint64_t> t_ns;  // swi -> ns
inline thread_local std::unordered_map<std::uint32_t, std::uint32_t> t_calls;
// Guest cacheflushes during the frame (any thread): each drops translations
// that then compile again.
inline std::atomic<std::uint64_t> g_flushes{0}, g_flush_bytes{0};
// Guest data accesses that took the callback path instead of fastmem (this
// thread): an access that faulted once stays on the slow path.
inline thread_local std::uint64_t t_slow_mem = 0;  // counted only when g_enabled
inline thread_local std::uint64_t t_slow_mem0 = 0;
inline thread_local std::uint64_t t_flushes0 = 0, t_flush_bytes0 = 0;
// The summary's totals since its last line (this thread).
inline thread_local std::uint64_t t_sum_start = 0, t_sum_frames = 0, t_sum_total = 0;
inline thread_local std::unordered_map<std::uint32_t, std::uint64_t> t_sum_ns;
inline thread_local std::unordered_map<std::uint32_t, std::uint64_t> t_sum_calls;

inline std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Times one host call when this thread is profiled (outermost calls only).
class CallTimer {
public:
    explicit CallTimer(std::uint32_t swi)
        : swi_(swi), t0_(g_enabled && t_on && t_depth++ == 0 ? NowNs() : 0) {}
    ~CallTimer() {
        if (!g_enabled || !t_on) {
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
void Summary(std::uint64_t now, Name name, Log log) {
    if (t_sum_frames == 0) {
        return;
    }
    const double f = static_cast<double>(t_sum_frames);
    std::uint64_t host = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> rows;
    for (const auto& kv : t_sum_ns) {
        host += kv.second;
        rows.emplace_back(kv.second, kv.first);
    }
    std::sort(rows.rbegin(), rows.rend());
    std::uint64_t calls = 0;
    for (const auto& kv : t_sum_calls) {
        calls += kv.second;
    }
    log("[prof] %llu frames in %.1f s: frame %.3f ms = host calls %.3f ms (%.0f calls) + guest %.3f ms",
        static_cast<unsigned long long>(t_sum_frames), static_cast<double>(now - t_sum_start) / 1e9,
        static_cast<double>(t_sum_total) / 1e6 / f, static_cast<double>(host) / 1e6 / f,
        static_cast<double>(calls) / f,
        static_cast<double>(t_sum_total - std::min(host, t_sum_total)) / 1e6 / f);
    for (std::size_t i = 0; i < rows.size() && i < 12; i++) {
        const std::uint64_t n = t_sum_calls[rows[i].second];
        log("[prof]   %-28s %.3f ms/frame %8.1f calls/frame %6.0f ns/call", name(rows[i].second).c_str(),
            static_cast<double>(rows[i].first) / 1e6 / f, static_cast<double>(n) / f,
            n ? static_cast<double>(rows[i].first) / static_cast<double>(n) : 0.0);
    }
    t_sum_frames = 0;
    t_sum_total = 0;
    t_sum_ns.clear();
    t_sum_calls.clear();
    t_sum_start = now;
}

template <typename Name, typename Log>
void FrameEnd(std::uint64_t t0, Name name, Log log) {
    const std::uint64_t now = NowNs();
    const std::uint64_t total = now - t0;
    if (t_sum_start == 0) {
        t_sum_start = t0;
    }
    t_sum_frames++;
    t_sum_total += total;
    for (const auto& kv : t_ns) {
        t_sum_ns[kv.first] += kv.second;
    }
    for (const auto& kv : t_calls) {
        t_sum_calls[kv.first] += kv.second;
    }
    if (now - t_sum_start >= kSummaryNs) {
        Summary(now, name, log);
    }
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
