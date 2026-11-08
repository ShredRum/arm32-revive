// Sampling profiler (sampler.h).
#include "sampler.h"

#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include "dynarmic/interface/A32/a32.h"

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

namespace abr::sampler {
namespace {

constexpr std::size_t kRing = 4096;  // samples a thread keeps between reports (4 s at 1 ms)

struct Slot {
    pid_t tid = 0;
    std::atomic<std::uint64_t> head{0};  // written by the owning thread's signal handler
    std::uint64_t tail = 0;              // reporter only
    std::atomic<bool> gone{false};       // the thread has exited
    timer_t timer{};
    std::uintptr_t pcs[kRing] = {};
};

bool g_on = false;
long g_period_ns = 1000000;
// CLOCK_THREAD_CPUTIME_ID (default): samples where the thread spends CPU.
// CLOCK_MONOTONIC ("wall"): every period of real time, wherever the thread
// is; for a kernel whose per-thread CPU clocks are wrong (the sandbox's
// reports process totals, and its timer signals then all arrive at syscall
// exits), and for seeing where a thread waits.
clockid_t g_clock = CLOCK_THREAD_CPUTIME_ID;
std::mutex g_mu;  // slots, modules, resolver
std::vector<Slot*> g_slots;
struct Module {
    std::string name;
    std::uint32_t base, end;
};
std::vector<Module> g_modules;
Dynarmic::A32::Jit* g_resolver = nullptr;
thread_local Slot* t_slot = nullptr;

void Emit(const char* line) {
    std::printf("[sample] %s\n", line);
    std::fflush(stdout);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "abhost", "[sample] %s", line);
#endif
}

void OnProf(int, siginfo_t*, void* ctx) {
    Slot* s = t_slot;
    if (!s) {
        return;
    }
    const auto* uc = static_cast<const ucontext_t*>(ctx);
#if defined(__aarch64__)
    const std::uintptr_t pc = static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#elif defined(__x86_64__)
    const std::uintptr_t pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    const std::uintptr_t pc = 0;
#endif
    const std::uint64_t h = s->head.load(std::memory_order_relaxed);
    s->pcs[h % kRing] = pc;
    s->head.store(h + 1, std::memory_order_release);
}

// Stops the thread's timer when the thread exits.
struct ThreadGuard {
    ~ThreadGuard() {
        if (Slot* s = t_slot) {
            timer_delete(s->timer);
            t_slot = nullptr;
            s->gone.store(true, std::memory_order_release);
        }
    }
};
thread_local ThreadGuard t_guard;

std::string ThreadName(pid_t tid) {
    char path[64], name[32] = "?";
    std::snprintf(path, sizeof path, "/proc/self/task/%d/comm", static_cast<int>(tid));
    if (FILE* f = std::fopen(path, "r")) {
        if (std::fgets(name, sizeof name, f)) {
            name[std::strcspn(name, "\n")] = 0;
        }
        std::fclose(f);
    }
    return name;
}

const char* Base(const char* path) {
    const char* b = std::strrchr(path, '/');
    return b ? b + 1 : path;
}

enum Class { kGuest, kGuestPrivate, kStubs, kOurs, kGl, kLibc, kOther, kClasses };
const char* const kClassName[kClasses] = {"guest JIT", "guest (private cache)", "JIT stubs",
                                          "host ours", "GL/driver", "libc", "other"};

void Report() {
    struct Tally {
        std::uint64_t total = 0;
        std::uint64_t cls[kClasses] = {};
        std::unordered_map<std::string, std::uint64_t> guest, host;
    };
    std::map<pid_t, Tally> per;
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&OnProf), &self);
    std::lock_guard<std::mutex> l(g_mu);
    for (auto it = g_slots.begin(); it != g_slots.end();) {
        Slot* s = *it;
        const std::uint64_t head = s->head.load(std::memory_order_acquire);
        std::uint64_t from = s->tail;
        if (head - from > kRing) {
            from = head - kRing;  // the ring wrapped: the oldest are lost
        }
        Tally& t = per[s->tid];
        for (std::uint64_t i = from; i < head; i++) {
            const std::uintptr_t pc = s->pcs[i % kRing];
            t.total++;
            const std::int64_t g = g_resolver ? g_resolver->GuestPcForHostPc(reinterpret_cast<void*>(pc)) : -1;
            if (g >= 0) {
                const std::uint32_t gpc = static_cast<std::uint32_t>(g) & ~1u;
                char key[96];
                const Module* m = nullptr;
                for (const Module& mm : g_modules) {
                    if (gpc >= mm.base && gpc < mm.end) m = &mm;
                }
                if (m) {
                    std::snprintf(key, sizeof key, "%s+0x%x%s", m->name.c_str(), gpc - m->base,
                                  (g & 1) ? " (thumb)" : "");
                } else {
                    std::snprintf(key, sizeof key, "guest 0x%08x", gpc);
                }
                t.cls[kGuest]++;
                t.guest[key]++;
                continue;
            }
            if (g == -2) {
                t.cls[kStubs]++;
                continue;
            }
            Dl_info di{};
            Class c = kOther;
            char key[160];
            if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_fname) {
                const char* lib = Base(di.dli_fname);
                if (di.dli_fbase == self.dli_fbase) {
                    c = kOurs;
                } else if (std::strstr(lib, "GLES") || std::strstr(lib, "EGL") || std::strstr(lib, "vulkan") ||
                           std::strstr(lib, "angle") || std::strstr(lib, "mali") || std::strstr(lib, "adreno") ||
                           std::strstr(lib, "gsl") || std::strstr(lib, "sgpu") || std::strstr(lib, "_xclipse")) {
                    c = kGl;
                } else if (!std::strcmp(lib, "libc.so") || !std::strcmp(lib, "libm.so") ||
                           !std::strcmp(lib, "libc.so.6") || !std::strcmp(lib, "libm.so.6")) {
                    c = kLibc;
                }
                if (di.dli_sname && c != kOurs) {
                    std::snprintf(key, sizeof key, "%s:%s", lib, di.dli_sname);
                } else {
                    std::snprintf(key, sizeof key, "%s+0x%zx", lib,
                                  static_cast<std::size_t>(pc - reinterpret_cast<std::uintptr_t>(di.dli_fbase)));
                }
            } else {
                // An anonymous mapping: a private overflow cache or another
                // Jit's private cache (jitshare=off), or the vdso.
                c = g_resolver ? kGuestPrivate : kOther;
                std::snprintf(key, sizeof key, "anon 0x%zx", static_cast<std::size_t>(pc));
            }
            t.cls[c]++;
            t.host[key]++;
        }
        s->tail = head;
        if (s->gone.load(std::memory_order_acquire)) {
            delete s;
            it = g_slots.erase(it);
        } else {
            ++it;
        }
    }
    // Busiest threads first.
    std::vector<std::pair<pid_t, Tally*>> order;
    std::uint64_t all = 0;
    for (auto& kv : per) {
        if (kv.second.total) order.emplace_back(kv.first, &kv.second);
        all += kv.second.total;
    }
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second->total > b.second->total; });
    char line[512];
    std::snprintf(line, sizeof line, "--- %llu samples (%.0f ms) in %zu threads",
                  static_cast<unsigned long long>(all), all * g_period_ns / 1e6, order.size());
    Emit(line);
    auto top = [&](const std::unordered_map<std::string, std::uint64_t>& m, std::uint64_t total,
                   std::size_t n, const char* what) {
        std::vector<std::pair<std::string, std::uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < v.size() && i < n; i++) {
            std::snprintf(line, sizeof line, "    %s %5.1f%%  %s", what, 100.0 * v[i].second / total,
                          v[i].first.c_str());
            Emit(line);
        }
    };
    for (std::size_t k = 0; k < order.size() && k < 6; k++) {
        const Tally& t = *order[k].second;
        int n = std::snprintf(line, sizeof line, "t%d %s: %llu (%.0f%% of samples):",
                              static_cast<int>(order[k].first), ThreadName(order[k].first).c_str(),
                              static_cast<unsigned long long>(t.total), 100.0 * t.total / all);
        for (int c = 0; c < kClasses; c++) {
            if (t.cls[c] && n < static_cast<int>(sizeof line) - 40) {
                n += std::snprintf(line + n, sizeof line - n, " %s %.1f%%", kClassName[c],
                                   100.0 * t.cls[c] / t.total);
            }
        }
        Emit(line);
        top(t.guest, t.total, 12, "guest");
        top(t.host, t.total, 8, "host ");
    }
}

}  // namespace

void Init() {
    const char* v = std::getenv("HARNESS_SAMPLE");
#if defined(__ANDROID__)
    char prop[PROP_VALUE_MAX] = {};
    if (!v && __system_property_get("debug.abport.sample", prop) > 0 && std::strcmp(prop, "0") != 0) {
        v = prop;
    }
#endif
    if (!v || !*v || !std::strcmp(v, "0")) {
        return;
    }
    const char* clock = std::getenv("HARNESS_SAMPLE_CLOCK");
#if defined(__ANDROID__)
    char cprop[PROP_VALUE_MAX] = {};
    if (!clock && __system_property_get("debug.abport.sample_clock", cprop) > 0) {
        clock = cprop;
    }
#endif
    if (clock && !std::strcmp(clock, "wall")) {
        g_clock = CLOCK_MONOTONIC;
    }
    const long ms = std::strtol(v, nullptr, 10);
    g_period_ns = (ms > 0 && ms < 1000 ? ms : 1) * 1000000L;
    struct sigaction sa {};
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, nullptr) != 0) {
        Emit("off: SIGPROF handler not installed");
        return;
    }
    g_on = true;
    char line[128];
    std::snprintf(line, sizeof line, "on: one sample per %ld ms of each thread's %s time, report every 5 s",
                  g_period_ns / 1000000L, g_clock == CLOCK_MONOTONIC ? "wall" : "CPU");
    Emit(line);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            Report();
        }
    }).detach();
}

bool On() { return g_on; }

void SetResolver(Dynarmic::A32::Jit* jit) {
    std::lock_guard<std::mutex> l(g_mu);
    g_resolver = jit;
}

void AddModule(const std::string& name, std::uint32_t base, std::uint32_t end) {
    std::lock_guard<std::mutex> l(g_mu);
    g_modules.push_back(Module{name, base, end});
}

void ThreadUses() {
    if (!g_on || t_slot) {
        return;
    }
    auto* s = new Slot();
    s->tid = static_cast<pid_t>(syscall(SYS_gettid));
    sigevent sev {};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev.sigev_notify_thread_id = s->tid;
    if (timer_create(g_clock, &sev, &s->timer) != 0) {
        delete s;
        return;
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_slots.push_back(s);
    }
    (void)&t_guard;  // makes the guard (and its destructor) exist for this thread
    t_slot = s;      // before the first signal can come
    const itimerspec its{{0, g_period_ns}, {0, g_period_ns}};
    timer_settime(s->timer, 0, &its, nullptr);
}

}  // namespace abr::sampler
