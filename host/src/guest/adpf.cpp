#include "adpf.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace abr {
bool EnvOn(const char* name);  // dispatch.cpp: environment or files/abflags.txt
}

namespace abr::adpf {

namespace {

std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint64_t> g_cpu_ns{0};
std::atomic<std::uint64_t> g_cycles{0};
std::atomic<std::uint64_t> g_cycles_cpu_ns{0};
// Until the display answers: 120 Hz, the short side (see RefreshRate).
std::atomic<std::uint64_t> g_target_ns{1000000000ull / 120};
std::atomic<bool> g_session_live{false};

std::uint64_t ThreadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
}

std::uint64_t WallNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
}

void Note(const char* line) {
    std::printf("[adpf] %s\n", line);
    std::fflush(stdout);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "abhost", "[adpf] %s", line);
#endif
}

#if defined(__ANDROID__)
// The current clock of a core in kHz (cpufreq), 0 if unknown. One open file
// per core, read again with pread (sysfs gives the fresh value at offset 0).
constexpr int kMaxCores = 32;
std::atomic<int> g_freq_fd[kMaxCores];  // 0: not opened yet; -1: unreadable
std::uint64_t CoreKhz(int cpu) {
    if (cpu < 0 || cpu >= kMaxCores) {
        return 0;
    }
    int fd = g_freq_fd[cpu].load(std::memory_order_relaxed);
    if (fd == 0) {
        char path[80];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        int opened = open(path, O_RDONLY | O_CLOEXEC);
        if (opened <= 0) {
            opened = -1;  // (0 is never ours here: stdin)
        }
        int expected = 0;
        if (!g_freq_fd[cpu].compare_exchange_strong(expected, opened, std::memory_order_relaxed)) {
            if (opened > 0) close(opened);  // another thread opened it first
        }
        fd = g_freq_fd[cpu].load(std::memory_order_relaxed);
    }
    if (fd < 0) {
        return 0;
    }
    char buf[24] = {};
    const ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    return n > 0 ? std::strtoull(buf, nullptr, 10) : 0;
}
#endif

// The frame thread's own state (one thread makes the frames at a time; a new
// one, e.g. after the GL thread was recreated, takes the session over).
struct FrameThread {
    std::uint64_t last_cpu = 0;    // thread CPU time at the previous mark
    std::uint64_t next_check = 0;  // wall time of the next refresh-rate check
};
thread_local FrameThread t_frame;

#if defined(__ANDROID__)
// libandroid.so, Android 13+ (API 33) and 15+ (setPreferPowerEfficiency);
// the host targets older APIs, so they are looked up at run time.
struct Manager;
struct Session;
using GetManagerFn = Manager* (*)();
using CreateSessionFn = Session* (*)(Manager*, const std::int32_t*, std::size_t, std::int64_t);
using UpdateTargetFn = int (*)(Session*, std::int64_t);
using ReportFn = int (*)(Session*, std::int64_t);
using CloseFn = void (*)(Session*);
using PreferEfficiencyFn = int (*)(Session*, bool);

bool g_on = false;
JavaVM* g_vm = nullptr;
GetManagerFn g_get_manager = nullptr;
CreateSessionFn g_create = nullptr;
UpdateTargetFn g_update_target = nullptr;
ReportFn g_report = nullptr;
CloseFn g_close = nullptr;
PreferEfficiencyFn g_prefer_efficiency = nullptr;

std::mutex g_mu;            // the session: reports, changes
Session* g_session = nullptr;
pid_t g_session_tid = 0;
std::uint64_t g_last_report = 0;  // wall time of the owner's last report

bool Cleared(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

// The default display's refresh rate through Java, from an attached thread:
// the current mode's rate, else Display.getRefreshRate(), else the highest
// rate a mode supports (when unsure, a shorter target is the safe side: the
// system may then run the cores faster than needed, never slower). 0 if
// none can be read; `how` says which one answered or where it failed.
float RefreshRate(const char** how) {
    *how = "no JNIEnv on this thread";
    JNIEnv* env = nullptr;
    if (!g_vm || g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        return 0.f;
    }
    if (env->PushLocalFrame(32) != 0) {
        Cleared(env);
        *how = "PushLocalFrame";
        return 0.f;
    }
    float hz = 0.f;
    do {
        *how = "ActivityThread.currentApplication";
        jclass at = env->FindClass("android/app/ActivityThread");
        jmethodID cur = at ? env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;") : nullptr;
        jobject app = cur ? env->CallStaticObjectMethod(at, cur) : nullptr;
        if (Cleared(env) || !app) break;
        *how = "getSystemService(display)";
        jclass ctx = env->FindClass("android/content/Context");
        jmethodID gss = ctx ? env->GetMethodID(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
        jstring name = env->NewStringUTF("display");
        jobject dm = (gss && name) ? env->CallObjectMethod(app, gss, name) : nullptr;
        if (Cleared(env) || !dm) break;
        *how = "DisplayManager.getDisplay(0)";
        jclass dmc = env->FindClass("android/hardware/display/DisplayManager");
        jmethodID gd = dmc ? env->GetMethodID(dmc, "getDisplay", "(I)Landroid/view/Display;") : nullptr;
        jobject disp = gd ? env->CallObjectMethod(dm, gd, 0) : nullptr;  // DEFAULT_DISPLAY
        if (Cleared(env) || !disp) break;
        jclass dc = env->FindClass("android/view/Display");
        jclass mc = env->FindClass("android/view/Display$Mode");
        if (Cleared(env) || !dc || !mc) {
            *how = "Display classes";
            break;
        }
        jmethodID mrate = env->GetMethodID(mc, "getRefreshRate", "()F");
        // 1. The current mode (API 23+).
        jmethodID gm = env->GetMethodID(dc, "getMode", "()Landroid/view/Display$Mode;");
        jobject mode = (gm && !Cleared(env)) ? env->CallObjectMethod(disp, gm) : nullptr;
        if (!Cleared(env) && mode && mrate) {
            const float v = env->CallFloatMethod(mode, mrate);
            if (!Cleared(env) && v >= 20.f) {
                hz = v;
                *how = "Display.getMode()";
                break;
            }
        }
        // 2. Display.getRefreshRate().
        jmethodID rr = env->GetMethodID(dc, "getRefreshRate", "()F");
        if (rr && !Cleared(env)) {
            const float v = env->CallFloatMethod(disp, rr);
            if (!Cleared(env) && v >= 20.f) {
                hz = v;
                *how = "Display.getRefreshRate()";
                break;
            }
        }
        // 3. The highest supported mode.
        *how = "Display.getSupportedModes()";
        jmethodID sm = env->GetMethodID(dc, "getSupportedModes", "()[Landroid/view/Display$Mode;");
        auto modes = (sm && !Cleared(env)) ? static_cast<jobjectArray>(env->CallObjectMethod(disp, sm)) : nullptr;
        if (Cleared(env) || !modes || !mrate) break;
        const jsize n = env->GetArrayLength(modes);
        for (jsize i = 0; i < n; i++) {
            jobject m = env->GetObjectArrayElement(modes, i);
            const float v = m ? env->CallFloatMethod(m, mrate) : 0.f;
            if (!Cleared(env) && v > hz) hz = v;
            env->DeleteLocalRef(m);
        }
    } while (false);
    Cleared(env);
    env->PopLocalFrame(nullptr);
    return hz;
}

std::uint64_t PeriodFromRate(float hz) {
    if (!(hz >= 20.f && hz <= 500.f)) {
        return 0;
    }
    return static_cast<std::uint64_t>(1e9 / hz);
}

// The session for the calling thread, created (or moved here) if needed.
// Under g_mu.
void OpenSessionHere(pid_t tid) {
    if (g_session) {
        g_close(g_session);
        g_session = nullptr;
        g_session_live.store(false, std::memory_order_relaxed);
    }
    Manager* manager = g_get_manager();
    if (!manager) {
        Note("no performance hint manager: off");
        g_on = false;
        return;
    }
    const char* how = nullptr;
    const float hz = RefreshRate(&how);
    if (const std::uint64_t p = PeriodFromRate(hz)) {
        g_target_ns.store(p, std::memory_order_relaxed);
    }
    char rline[128];
    std::snprintf(rline, sizeof rline, "refresh rate %.1f Hz (%s)%s", hz, how,
                  PeriodFromRate(hz) ? "" : ": target stays at the default");
    Note(rline);
    const std::int32_t tids[1] = {static_cast<std::int32_t>(tid)};
    const std::int64_t target = static_cast<std::int64_t>(g_target_ns.load(std::memory_order_relaxed));
    g_session = g_create(manager, tids, 1, target);
    if (!g_session) {
        Note("createSession failed: off");
        g_on = false;
        return;
    }
    const bool efficient = g_prefer_efficiency && g_prefer_efficiency(g_session, true) == 0;
    g_session_tid = tid;
    g_session_live.store(true, std::memory_order_relaxed);
    char line[128];
    std::snprintf(line, sizeof line, "session for tid %d, target %.2f ms%s", tid, target / 1e6,
                  efficient ? ", prefer power efficiency" : "");
    Note(line);
}
#endif

}  // namespace

void Init(void* java_vm) {
#if defined(__ANDROID__)
    g_vm = static_cast<JavaVM*>(java_vm);
    if (EnvOn("HARNESS_NOADPF")) {
        Note("off (HARNESS_NOADPF)");
        return;
    }
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.abport.adpf", prop) > 0 && std::strcmp(prop, "0") == 0) {
        Note("off (debug.abport.adpf 0)");
        return;
    }
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) {
        Note("no libandroid.so: off");
        return;
    }
    g_get_manager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
    g_create = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
    g_update_target = reinterpret_cast<UpdateTargetFn>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    g_report = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    g_close = reinterpret_cast<CloseFn>(dlsym(lib, "APerformanceHint_closeSession"));
    g_prefer_efficiency =
        reinterpret_cast<PreferEfficiencyFn>(dlsym(lib, "APerformanceHint_setPreferPowerEfficiency"));
    if (!g_get_manager || !g_create || !g_update_target || !g_report || !g_close) {
        Note("no APerformanceHint (Android 13+): off");
        return;
    }
    g_on = true;
    Note("on");
#else
    (void)java_vm;
#endif
}

void FrameDone() {
    FrameThread& t = t_frame;
    const std::uint64_t now = ThreadCpuNs();
    if (t.last_cpu == 0) {
        t.last_cpu = now;  // the first mark of this thread only starts the count
        return;
    }
    const std::uint64_t work = now - t.last_cpu;
    t.last_cpu = now;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_cpu_ns.fetch_add(work, std::memory_order_relaxed);
#if defined(__ANDROID__)
    if (const std::uint64_t khz = CoreKhz(sched_getcpu())) {
        g_cycles.fetch_add(work * khz / 1000000u, std::memory_order_relaxed);  // ns * kHz / 1e6
        g_cycles_cpu_ns.fetch_add(work, std::memory_order_relaxed);
    }
#endif
#if defined(__ANDROID__)
    if (!g_on) {
        return;
    }
    const pid_t tid = gettid();
    const std::uint64_t wall = WallNs();
    // The refresh rate can change (adaptive displays, power saving): the
    // target follows it every 2 s, read outside the lock (a Java call). The
    // target is never taken from the frames themselves: a slow frame would
    // raise it and invite lower clocks.
    std::uint64_t period = 0;
    if (wall >= t.next_check) {
        t.next_check = wall + 2000000000ull;
        const char* how = nullptr;
        period = PeriodFromRate(RefreshRate(&how));
    }
    std::lock_guard<std::mutex> l(g_mu);  // once a frame, uncontended
    if (!g_on) {
        return;
    }
    if (g_session_tid != tid) {
        // Another thread owns the session. Take it over only when the owner
        // has stopped making frames (a recreated GL thread), not when two
        // threads both mark frames.
        if (g_session && wall - g_last_report < 1000000000ull) {
            return;
        }
        OpenSessionHere(tid);
        g_last_report = wall;
        return;
    }
    if (!g_session) {
        return;
    }
    if (work > 0) {
        g_report(g_session, static_cast<std::int64_t>(work));
    }
    g_last_report = wall;
    if (period && period != g_target_ns.load(std::memory_order_relaxed)) {
        g_target_ns.store(period, std::memory_order_relaxed);
        g_update_target(g_session, static_cast<std::int64_t>(period));
        char line[64];
        std::snprintf(line, sizeof line, "target %.2f ms", period / 1e6);
        Note(line);
    }
#endif
}

bool TakeStats(Stats* s) {
    s->frames = g_frames.exchange(0, std::memory_order_relaxed);
    s->cpu_ns = g_cpu_ns.exchange(0, std::memory_order_relaxed);
    s->cycles = g_cycles.exchange(0, std::memory_order_relaxed);
    s->cycles_cpu_ns = g_cycles_cpu_ns.exchange(0, std::memory_order_relaxed);
    s->target_ns = g_target_ns.load(std::memory_order_relaxed);
    s->session = g_session_live.load(std::memory_order_relaxed);
    return s->frames != 0;
}

}  // namespace abr::adpf
