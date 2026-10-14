// apphost.cpp — Android product host (online-first milestone).
// Real Java drives: 63 Java_* forwarders (apphost_gen.inc) run guest code
// under Dynarmic with the REAL JNIEnv* (indices match NDK order, verified).
// Import thunks (libc/GLES/sockets/files/threads) are the proven harness
// ones; mocks shrink to era-tuned values only. Guest .so loads as DATA
// (mini-zip extract from our own base.apk), never via System.loadLibrary.
#include <jni.h>

#include <android/log.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <random>
#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "apphost.h"
#include "guest/dispatch.h"
#include "guest/elf_loader.h"
#include "guest/layout.h"

namespace abr {
namespace {


Runtime* g_rt = nullptr;
GuestMemory* g_mem = nullptr;
LoadedModule* g_mod = nullptr;
GuestCallbacks* g_cb = nullptr;
JavaVM* g_vm = nullptr;
std::string g_files;  // app files dir (guest sees it as .../files)
std::string g_apk;    // our own base.apk (asset source)

struct HostThread {
    Dynarmic::A32::Jit* jit = nullptr;
    std::uint32_t stack = 0;  // 2MB guest stack base
    std::uint32_t tid = 0;    // bridge tid (errno/TLS routing)
    JNIEnv* env = nullptr;    // real, attached
};
std::mutex g_tmu;
std::unordered_map<pid_t, std::shared_ptr<HostThread>> g_threads;
std::uint32_t g_next_tid = 1000;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[apphost] %s\n", buf);
    __android_log_print(ANDROID_LOG_INFO, "AngryBirdsRio", "[apphost] %s", buf);
}

// Stuck-update watchdog (2026-10-14): a guest-mutex deadlock wedged the
// GL thread inside nativeUpdate with zero witnesses (BlueSky freeze).
// Tracks update entry/return; a detached thread dumps ThreadCensus +
// mutex owners + last import when an update exceeds 30s. Observe-only.
std::atomic<long long> g_upd_entry_ms{0};
std::atomic<long long> g_upd_done_ms{0};
std::atomic<int> g_upd_entry_n{0};

long long WallMs() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void Watchdog() {
    long long reported = 0;
    while (true) {
        sleep(10);
        if (!g_rt) {
            continue;
        }
        const long long entry = g_upd_entry_ms.load();
        const long long done = g_upd_done_ms.load();
        if (!entry || done >= entry) {
            continue;
        }
        const long long stuck_ms = WallMs() - entry;
        if (stuck_ms < 30000 || WallMs() - reported < 30000) {
            continue;
        }
        reported = WallMs();
        Log("STUCK update #%d entered %lldms ago (entry=%lld)",
            g_upd_entry_n.load(), stuck_ms, entry);
        g_rt->ThreadCensus();
        g_rt->MutexOwnersDump();
        const std::uint32_t plo = g_rt->pend_lock_obj_.load();
        const std::uint32_t plt = g_rt->pend_lock_tid_.load();
        if (plo) {
            Log("STUCK pending-lock obj=%08x tid=%u", plo, plt);
        } else {
            Log("STUCK pending-lock: none (not inside mutex_lock)");
        }
        const std::uint32_t si = g_rt->last_svc_idx_.load();
        const std::uint32_t st = g_rt->last_svc_tid_.load();
        const char* nm = "?";
        if (g_mod && si < g_mod->imports.size()) {
            nm = g_mod->imports[si].name.c_str();
        }
        Log("STUCK last-svc idx=%u tid=%u %s", si, st, nm);
    }
}

void CallGuest(Dynarmic::A32::Jit& jit, std::uint32_t target) {
    if (target & 1u) {
        jit.SetCpsr(0x30);
        jit.Regs()[15] = target & ~std::uint32_t(1);
    } else {
        jit.SetCpsr(0x10);
        jit.Regs()[15] = target;
    }
}

// Mini-zip STORED-only extract (packaging guarantees STORED via doNotCompress).
bool ZipExtractStored(const std::string& apk, const std::string& inner, const std::string& out) {
    FILE* f = std::fopen(apk.c_str(), "rb");
    if (!f) {
        Log("zip open %s failed", apk.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    // EOCD scan (last 64K+22).
    const long scan0 = sz > 66000 ? sz - 66000 : 0;
    long cd_off = -1, cd_n = 0;
    std::vector<std::uint8_t> tail((std::size_t)(sz - scan0));
    std::fseek(f, scan0, SEEK_SET);
    if (std::fread(tail.data(), 1, tail.size(), f) != tail.size()) {
        std::fclose(f);
        return false;
    }
    auto rd32 = [&](std::size_t o) -> std::uint32_t {
        return (std::uint32_t)tail[o] | ((std::uint32_t)tail[o + 1] << 8) |
               ((std::uint32_t)tail[o + 2] << 16) | ((std::uint32_t)tail[o + 3] << 24);
    };
    auto rd16 = [&](std::size_t o) -> std::uint32_t {
        return (std::uint32_t)tail[o] | ((std::uint32_t)tail[o + 1] << 8);
    };
    for (std::size_t o = 0; o + 22 <= tail.size(); o++) {
        if (rd32(o) == 0x06054B50u) {
            cd_n = rd16(o + 10);
            cd_off = (long)rd32(o + 16);
        }
    }
    if (cd_off < 0) {
        std::fclose(f);
        Log("zip EOCD not found");
        return false;
    }
    // Walk central directory.
    std::fseek(f, cd_off, SEEK_SET);
    bool found = false;
    std::uint32_t data_off = 0, usize = 0;
    std::uint16_t method = 0;
    for (long i = 0; i < cd_n && !found; i++) {
        std::uint8_t h[46];
        if (std::fread(h, 1, 46, f) != 46) {
            break;
        }
        auto h32 = [&](int o) -> std::uint32_t {
            return (std::uint32_t)h[o] | ((std::uint32_t)h[o + 1] << 8) |
                   ((std::uint32_t)h[o + 2] << 16) | ((std::uint32_t)h[o + 3] << 24);
        };
        auto h16 = [&](int o) -> std::uint32_t {
            return (std::uint32_t)h[o] | ((std::uint32_t)h[o + 1] << 8);
        };
        if (h32(0) != 0x02014B50u) {
            break;
        }
        const std::uint32_t nl = h16(28), el = h16(30), cl = h16(32);
        std::string nm(nl, '\0');
        if (nl && std::fread(nm.data(), 1, nl, f) != nl) {
            break;
        }
        std::fseek(f, el + cl, SEEK_CUR);
        if (nm == inner) {
            method = (std::uint16_t)h16(10);
            usize = h32(24);
            data_off = h32(42);
            found = true;
        }
    }
    if (!found || method != 0) {
        std::fclose(f);
        Log("zip entry %s %s", inner.c_str(), found ? "not STORED" : "missing");
        return false;
    }
    // Local header -> data.
    std::fseek(f, data_off, SEEK_SET);
    std::uint8_t lh[30];
    if (std::fread(lh, 1, 30, f) != 30) {
        std::fclose(f);
        return false;
    }
    const std::uint32_t lnl = (std::uint32_t)lh[26] | ((std::uint32_t)lh[27] << 8);
    const std::uint32_t lel = (std::uint32_t)lh[28] | ((std::uint32_t)lh[29] << 8);
    std::fseek(f, data_off + 30 + lnl + lel, SEEK_SET);
    FILE* o = std::fopen(out.c_str(), "wb");
    if (!o) {
        std::fclose(f);
        return false;
    }
    std::vector<std::uint8_t> buf(1 << 20);
    std::uint32_t left = usize;
    while (left) {
        const std::uint32_t n = left > buf.size() ? (std::uint32_t)buf.size() : left;
        if (std::fread(buf.data(), 1, n, f) != n) {
            break;
        }
        std::fwrite(buf.data(), 1, n, o);
        left -= n;
    }
    std::fclose(o);
    std::fclose(f);
    Log("zip extracted %s (%uB)", inner.c_str(), usize - left);
    return left == 0;
}

std::shared_ptr<HostThread> Enter() {
    const pid_t me = gettid();
    {
        std::lock_guard<std::mutex> l(g_tmu);
        const auto it = g_threads.find(me);
        if (it != g_threads.end()) {
            g_rt->ThreadEnter(it->second->jit, it->second->tid);
            return it->second;
        }
    }
    auto st = std::make_shared<HostThread>();
    st->stack = g_rt->GuestMalloc(0x200000u);
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = g_cb;
    cfg.optimizations = std::getenv("HARNESS_SAFEOPTS") ? Dynarmic::all_safe_optimizations
                                                        : Dynarmic::no_optimizations;
    if (std::getenv("HARNESS_FASTMEM")) {
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(g_mem->base());
    }
    static Dynarmic::ExclusiveMonitor shared_mon(8);
    cfg.global_monitor = &shared_mon;
    st->jit = new Dynarmic::A32::Jit(cfg);
    // Real attach: Java-created threads are attached already; our workers not.
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            Log("AttachCurrentThread failed");
            env = nullptr;
        }
    }
    st->env = env;
    std::uint32_t tid = 0;
    {
        std::lock_guard<std::mutex> l(g_tmu);
        tid = g_next_tid++;
        st->tid = tid;
        g_threads[me] = st;
    }
    g_rt->ThreadEnter(st->jit, tid);
    Log("thread enter tid=%u stack=%08x attached=%d", (unsigned)tid, st->stack, env ? 1 : 0);
    return st;
}

std::uint64_t AppHost_CallImpl(const char* name, const char* sig, void* env, void* thiz, va_list ap) {
    auto st = Enter();
    // RATE telemetry (2026-10-14, tap-freeze triage): uncapped entry stamp
    // for the 3 frame-path methods. Answers pump-alive vs pump-dead without
    // taps. Observe-only (no guest state touched).
    if (std::strstr(name, "nativeUpdate") || std::strstr(name, "nativeRender") ||
        std::strstr(name, "nativeInput")) {
        struct timespec rts {};
        ::clock_gettime(CLOCK_MONOTONIC, &rts);
        const long long rms = (long long)rts.tv_sec * 1000 + rts.tv_nsec / 1000000;
        std::printf("[rate] %s t=%lld tid=%d\n", name, rms, (int)gettid());
    }
    // Watchdog entry stamp (nativeUpdate only).
    static int upd_seq = 0;
    const bool is_update = std::strstr(name, "nativeUpdate") != nullptr;
    if (is_update) {
        g_upd_entry_n = ++upd_seq;
        g_upd_entry_ms = WallMs();
    }
    // HARNESS_FUNCTORAT=N: after the Nth nativeUpdate, forge a login-
    // functor ctx (PC RAWBUILD recipe: vtable 0x65E650 + zeroed
    // account block) and BL the fail functor 0x5FE70 once, then
    // continue normally. Binary: clean return? "Login failed"?
    // burst (+164? builders? menu)? PC proved clean+log (functor1).
    {
        static int un = 0, fired = 0;
        static int at = -1;
        if (at < 0) {
            at = 0;
            if (const char* e = std::getenv("HARNESS_FUNCTORAT")) {
                at = std::atoi(e);
            }
        }
        if (at > 0 && !fired && std::strstr(name, "nativeUpdate")) {
            if (++un >= at) {
                fired = 1;
                const std::uint32_t zb = g_rt->GuestCalloc(1, 0x2C);
                const std::uint32_t cx = g_rt->GuestCalloc(1, 0x20);
                if (zb && cx) {
                    const std::uint32_t fv = 0x65E650u;
                    std::memcpy(g_rt->H(cx), &fv, 4);
                    std::memcpy(g_rt->H(cx + 4), &zb, 4);
                    Log("[functor] forged ctx=%08x acct=%08x, calling 0x5FE70", cx, zb);
                    auto& fr = st->jit->Regs();
                    fr.fill(0);
                    fr[13] = st->stack + 0x200000u - 0x100u;
                    fr[14] = g_mod->halt_va;
                    fr[0] = cx;
                    CallGuest(*st->jit, 0x5FE70u);
                    Dynarmic::HaltReason fhr = st->jit->Run();
                    Log("[functor] hr=%d r0=%08x", (int)fhr, fr[0]);
                } else {
                    Log("[functor] alloc failed");
                }
            }
        }
    }
    static std::unordered_map<std::string, std::uint32_t> vacache;
    std::uint32_t va = 0;
    {
        static std::mutex cmu;
        std::lock_guard<std::mutex> l(cmu);
        const auto it = vacache.find(name);
        if (it != vacache.end()) {
            va = it->second;
        } else {
            const auto jt = g_mod->defined.find(name);
            if (jt == g_mod->defined.end() || (jt->second & 1u)) {
                // Thumb entries not expected (all 63 ARM per inventory); log loudly.
                Log("no guest VA for %s", name);
                return 0;
            }
            va = jt->second;
            vacache[name] = va;
        }
    }
    // AAPCS words: mock env + wrapped thiz (guest only ever sees 32-bit
    // mock handles; real 64-bit pointers never enter guest regs/fields),
    // then sig-driven (J 8-aligned + split, F via double promotion,
    // I/Z/B/S/C ints, L/[ wrapped).
    std::vector<std::uint32_t> words;
    words.push_back(g_rt->JniEnv());
    words.push_back(g_rt->WrapObject(thiz));
    for (const char* p = sig; *p && *p != ')'; p++) {
        if (*p == '(') {
            continue;
        }
        if (*p == 'J') {
            if (words.size() & 1u) {
                words.push_back(0);
            }
            const jlong v = va_arg(ap, jlong);
            words.push_back((std::uint32_t)(std::uint64_t)v);
            words.push_back((std::uint32_t)((std::uint64_t)v >> 32));
        } else if (*p == 'F') {
            const double d = va_arg(ap, double);
            const float f = (float)d;
            std::uint32_t w = 0;
            std::memcpy(&w, &f, 4);
            words.push_back(w);
        } else if (*p == 'D') {
            if (words.size() & 1u) {
                words.push_back(0);
            }
            const double d = va_arg(ap, double);
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            words.push_back((std::uint32_t)u);
            words.push_back((std::uint32_t)(u >> 32));
        } else if (*p == 'L' || *p == '[') {
            void* o = va_arg(ap, void*);
            words.push_back(g_rt->WrapObject(o));
            if (*p == 'L') {
                while (*p && *p != ';') {
                    p++;
                }
            } else {
                while (*p == '[') {
                    p++;
                }
                if (*p == 'L') {
                    while (*p && *p != ';') {
                        p++;
                    }
                }
            }
        } else {
            const int v = va_arg(ap, int);
            words.push_back((std::uint32_t)v);
        }
    }
    auto& regs = st->jit->Regs();
    regs.fill(0);
    const std::uint32_t spbase = st->stack + 0x200000u - 0x100u;
    regs[13] = spbase;
    for (std::size_t i = 0; i < words.size() && i < 4; i++) {
        regs[i] = words[i];
    }
    for (std::size_t i = 4; i < words.size(); i++) {
        std::memcpy(g_mem->host(spbase + (std::uint32_t)(i - 4) * 4), &words[i], 4);
    }
    regs[14] = g_mod->halt_va;
#ifdef __ANDROID__
    // Synthetic input heartbeat (tapbeat): when no real finger input for
    // a while, replay the last input words with action forced to MOVE(2)
    // (no press-flag side effects) ahead of the update, so event-driven
    // timers/quantums advance without a finger. Pacing-only; game logic
    // untouched. HARNESS_TAPBEAT=1 enables (test), unset = off.
    // Stash last real input (words + time) for replay shape.
    {
        static std::vector<std::uint32_t> lastw;
        static long long lastms = 0;
        static int upd_n = 0;
        static int beat_on = -1;
        if (beat_on < 0) {
            beat_on = std::getenv("HARNESS_TAPBEAT") ? 1 : 0;
        }
        // Seed template (PLAY center) so the pulse fires even with
        // zero real taps. thiz MUST be the genuine MyInputHandler mock:
        // synthetic DOWN with thiz=0 throws in guest (unwind LR=45AF67,
        // UD2 halt) and never sets the press-flag. Resolved once via
        // the real NativeApplication.f field (no finger needed).
        // Seed on the first update regardless of words shape (splash
        // calls carry <6 words; updates are the beat domain anyway).
        static void* g_real_app = nullptr;
        static std::uint32_t g_handler_mock = 0;
        if (lastw.empty() && std::strstr(name, "nativeUpdate")) {
            if (!g_real_app && thiz &&
                abr::GuestCallbacks::g_real_env_hook) {
                JNIEnv* re =
                    static_cast<JNIEnv*>(abr::GuestCallbacks::g_real_env_hook());
                if (re && !re->ExceptionCheck()) {
                    g_real_app = re->NewGlobalRef(static_cast<jobject>(thiz));
                    if (re->ExceptionCheck() || !g_real_app) {
                        re->ExceptionClear();
                        g_real_app = nullptr;
                    }
                }
            }
            if (!g_handler_mock && g_real_app &&
                abr::GuestCallbacks::g_real_env_hook) {
                JNIEnv* re =
                    static_cast<JNIEnv*>(abr::GuestCallbacks::g_real_env_hook());
                if (re && !re->ExceptionCheck()) {
                    jclass acl = re->GetObjectClass(static_cast<jobject>(g_real_app));
                    jfieldID ff = acl ? re->GetFieldID(
                        acl, "f", "Lcom/rovio/fusion/MyInputHandler;") : nullptr;
                    jobject h = (acl && ff && !re->ExceptionCheck())
                                    ? re->GetObjectField(static_cast<jobject>(g_real_app),
                                                         ff)
                                    : nullptr;
                    if (h && !re->ExceptionCheck()) {
                        jobject gh = re->NewGlobalRef(h);
                        if (gh && !re->ExceptionCheck()) {
                            g_handler_mock = g_rt->WrapObject(gh);
                            std::printf("[tapbeat] handler mock=%08x\n", g_handler_mock);
                        }
                        re->DeleteLocalRef(h);
                    } else {
                        re->ExceptionClear();
                    }
                    if (acl) {
                        re->DeleteLocalRef(acl);
                    }
                }
            }
            float dx = 1124.0f, dy = 629.0f;
            std::uint32_t xb = 0, yb = 0;
            std::memcpy(&xb, &dx, 4);
            std::memcpy(&yb, &dy, 4);
            lastw = {g_rt->JniEnv(), g_handler_mock, 2, xb, yb, 0};
        }
        if (lastw.empty() && words.size() >= 6) {
            lastw = words;
            float dx = 1124.0f, dy = 629.0f;
            std::uint32_t xb = 0, yb = 0;
            std::memcpy(&xb, &dx, 4);
            std::memcpy(&yb, &dy, 4);
            lastw[1] = words[1];  // genuine mock (never 0: DOWN needs thiz)
            lastw[2] = 2;
            lastw[3] = xb;
            lastw[4] = yb;
            lastw[5] = 0;
        }
        auto now_ms = []() -> long long {
            struct timespec ts {};
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        };
        if (std::strstr(name, "nativeInput") && words.size() >= 6) {
            lastw = words;
            lastms = now_ms();
        } else if (beat_on && std::strstr(name, "nativeUpdate")) {
            upd_n++;
            const long long idle = lastms ? (now_ms() - lastms) : 1000000;
            // Governor v3 (2026-10-14, user-corrected twice): splashes
            // want taps ASAP, loading wants ~3min untapped (jobs cook),
            // menu wants taps. +164==2 from boot (useless as phase).
            // Hybrid: beat during the first K updates (splash window),
            // hold until wall-delay (loading cooks), beat after (menu).
            static long long t0 = 0;
            if (!t0) {
                t0 = now_ms();
            }
            static long long delay_ms = -1;
            if (delay_ms < 0) {
                delay_ms = 300000;
                if (const char* e = std::getenv("HARNESS_TAPBEAT_DELAY")) {
                    delay_ms = (long long)std::atoi(e) * 1000;
                }
            }
            const bool splash_win = (upd_n <= 400);
            const bool soaked = (now_ms() - t0 >= delay_ms);
            const int every = (splash_win || soaked) ? 30 : 0;
            // Synthetic nativeInput driver: DOWN(0)/UP(1) must land in
            // SEPARATE updates (finger: began -> updates -> ended; the
            // press-flag/skip arms between). Back-to-back in one beat =
            // strobe flash, no skip (observed live).
            auto fire = [&](int act) {
                std::vector<std::uint32_t> w = lastw;
                w[2] = (std::uint32_t)act;
                auto& br = st->jit->Regs();
                br.fill(0);
                for (std::size_t i = 0; i < w.size() && i < 4; i++) {
                    br[i] = w[i];
                }
                for (std::size_t i = 4; i < w.size(); i++) {
                    std::memcpy(g_mem->host(spbase + (std::uint32_t)(i - 4) * 4), &w[i],
                                4);
                }
                br[14] = g_mod->halt_va;
                br[13] = spbase;  // (was 0 from fill -> guest stack underflow
                // to 0xFFFFFFxx -> BAD halt; the normal path sets it, the
                // synthetic one forgot. Caught live 2026-10-14.)
                CallGuest(*st->jit, 0xC23E4u);
                Dynarmic::HaltReason bhr = st->jit->Run();
                std::printf("[tapbeat] #%d act=%d idle=%lldms hr=%d r0=%08x\n", upd_n, act,
                            idle, (int)bhr, br[0]);
                if (bhr != Dynarmic::HaltReason::UserDefined1) {
                    Log("[tapbeat] ABNORMAL hr=%d r0=%08x", (int)bhr, br[0]);
                }
            };
            static int pend_up = 0;
            if (pend_up && !lastw.empty() && lastw.size() >= 6) {
                pend_up = 0;  // release lands on the update right after press
                fire(1);
            }
            // Coexistence (2026-10-14): a live finger OWNS the game.
            // After any real input hold beats for QUIET_MS (synthetic
            // press-flag steals finger taps otherwise: menu goes dead
            // while beats interleave). 15s stillness => gentle pump.
            static long long quiet_ms = -1;
            if (quiet_ms < 0) {
                quiet_ms = 15000;
                if (const char* e = std::getenv("HARNESS_TAPBEAT_QUIET")) {
                    quiet_ms = (long long)std::atoi(e) * 1000;
                }
            }
            // Every 30th update (~1/s at 30fps) and quiet.
            if (every && !lastw.empty() && lastw.size() >= 6 && idle > quiet_ms &&
                (upd_n % every == 0)) {
                // Full synthetic tap: DOWN(0) now, UP(1) on next update.
                // MOVE-only never sets the press-flag / skip logic.
                // Solo, no finger => no interleave hazard.
                fire(0);
                pend_up = 1;
            }
        }
    }
#endif
    CallGuest(*st->jit, va);
    const Dynarmic::HaltReason hr = st->jit->Run();
    if (is_update) {
        g_upd_done_ms = WallMs();
    }
#ifdef __ANDROID__
    // Input-stream forensics (PLAY button animates but never fires):
    // log every nativeInput action + coords (capped). words[2]=action,
    // [3]=x-bits, [4]=y-bits (IJNI: env,thiz,action,x,y,?).
    if (std::strstr(name, "nativeInput") && words.size() >= 5) {
        static int in_n = 0;
        if (in_n < 60) {
            float xf = 0, yf = 0;
            std::uint32_t xw = words[3], yw = words[4];
            std::memcpy(&xf, &xw, 4);
            std::memcpy(&yf, &yw, 4);
            Log("[input] act=%u x=%.0f y=%.0f r0=%08x", words[2], xf, yf, regs[0]);
        }
        in_n++;
    }
#endif
    if (hr != Dynarmic::HaltReason::UserDefined1) {
        Log("%s ended hr=%d r0=%08x", name, (int)hr, regs[0]);
    }
#ifdef __ANDROID__
    // r0-polarity forensics: log EVERY nativeUpdate return (clean ones
    // are silent above). Java quits on r0=1; need per-update 0/1.
    // Capped (updates are ~30/s idle).
    else if (std::strstr(name, "nativeUpdate")) {
        static int un = 0;
        if (un < 400) {
            Log("%s r0=%08x", name, regs[0]);
        }
        un++;
    }
#endif
    return (std::uint64_t)regs[0] | ((std::uint64_t)regs[1] << 32);
}

std::string FindApk() {
    // Our own base.apk: derived from our own .so path via dladdr (no
    // permission issues: own maps always readable). .../lib/arm64/xxx.so
    // -> .../base.apk. Falls back to /data/app scan (rooted shells).
    {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&FindApk), &info) && info.dli_fname) {
            std::string self = info.dli_fname;
            const std::size_t p = self.find("/lib/");
            if (p != std::string::npos) {
                const std::string cand = self.substr(0, p) + "/base.apk";
                FILE* f = std::fopen(cand.c_str(), "rb");
                if (f) {
                    std::fclose(f);
                    return cand;
                }
            }
        }
    }
    const char* roots[] = {"/data/app", nullptr};
    for (int r = 0; roots[r]; r++) {
        DIR* d = opendir(roots[r]);
        if (!d) {
            continue;
        }
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") {
                continue;
            }
            for (const std::string& tail :
                 {"/com.rovio.angrybirdsrio/base.apk", "/com.rovio.angrybirdsrio-1/base.apk",
                  "/com.rovio.angrybirdsrio-2/base.apk"}) {
                const std::string cand = std::string(roots[r]) + "/" + n + tail;
                FILE* f = std::fopen(cand.c_str(), "rb");
                if (f) {
                    std::fclose(f);
                    closedir(d);
                    return cand;
                }
            }
            // direct child already the package dir
            if (n.find("com.rovio.angrybirdsrio") == 0) {
                const std::string cand = std::string(roots[r]) + "/" + n + "/base.apk";
                FILE* f = std::fopen(cand.c_str(), "rb");
                if (f) {
                    std::fclose(f);
                    closedir(d);
                    return cand;
                }
            }
        }
        closedir(d);
    }
    return "";
}

}  // namespace

std::uint64_t AppHost_Call(const char* name, const char* sig, void* env, void* thiz, ...) {
#ifdef __ANDROID__
    // Deinit forensics: who calls nativeDeinit (orderly-suicide chain)?
    // Dump the Java stack via real ART (no race: runs synchronously).
    if (std::strstr(name, "nativeDeinit") && abr::GuestCallbacks::g_real_env_hook) {
        JNIEnv* re = static_cast<JNIEnv*>(abr::GuestCallbacks::g_real_env_hook());
        if (re && !re->ExceptionCheck()) {
            jclass exCls = re->FindClass("java/lang/Exception");
            jmethodID exCtor = exCls ? re->GetMethodID(exCls, "<init>", "()V") : nullptr;
            jobject ex = (exCls && exCtor) ? re->NewObject(exCls, exCtor) : nullptr;
            jclass logCls = re->FindClass("android/util/Log");
            jmethodID strace = logCls ? re->GetStaticMethodID(logCls, "getStackTraceString",
                                                              "(Ljava/lang/Throwable;)Ljava/lang/String;")
                                      : nullptr;
            jstring js = (logCls && strace && ex)
                             ? static_cast<jstring>(re->CallStaticObjectMethod(logCls, strace, ex))
                             : nullptr;
            if (js && !re->ExceptionCheck()) {
                const char* c = re->GetStringUTFChars(js, nullptr);
                if (c) {
                    Log("deinit-JAVA-STACK:\n%.3000s", c);
                    re->ReleaseStringUTFChars(js, c);
                }
                re->DeleteLocalRef(js);
            } else {
                re->ExceptionClear();
            }
            if (ex) {
                re->DeleteLocalRef(ex);
            }
        }
    }
#endif
    va_list ap;
    va_start(ap, thiz);
    static int calln = 0;
#ifdef __ANDROID__
    // Lifecycle inventory: log every DISTINCT native once (uncapped by
    // design; ~63 max). Answers "which Java->native traffic exists".
    {
        static std::unordered_set<std::string> seen;
        static std::mutex smu;
        std::lock_guard<std::mutex> l(smu);
        if (seen.insert(name).second) {
            Log("uniqcall %s", name);
        }
    }
    if (calln < 400) {  // device runs long; 40 hid continuation (§94)
#else
    if (calln < 40) {
#endif
        Log("call#%d %s", calln, name);
#ifdef __ANDROID__
    }
#else
    }
#endif
    calln++;
    const std::uint64_t rv = AppHost_CallImpl(name, sig, env, thiz, ap);
    va_end(ap);
    return rv;
}

namespace {
// Real-Java delegation for JNI thunks (product host): the calling host
// thread's real JNIEnv*, or null (harness has none -> mocks stay).
void* RealEnvForThread() {
    const pid_t me = gettid();
    std::lock_guard<std::mutex> l(g_tmu);
    const auto it = g_threads.find(me);
    return it != g_threads.end() ? static_cast<void*>(it->second->env) : nullptr;
}
}  // namespace

extern "C" jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    GuestCallbacks::g_real_env_hook = RealEnvForThread;
    // Logs to app files dir (pullable); stdout unbuffered.
    g_files = "/data/data/com.rovio.angrybirdsrio/files";
    std::error_code ec;
    std::filesystem::create_directories(g_files, ec);
    const std::string logpath = g_files + "/apphost.log";
    std::freopen(logpath.c_str(), "w", stdout);
    std::freopen(logpath.c_str(), "a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Log("OnLoad: product host starting");
    Log("build marker %s %s", __DATE__, __TIME__);
    // Ship strategy (user-approved): canned sessions success (dead 2026
    // servers can't answer; success path throws nothing, needs no catch).
    // The harness RETSUCC machinery (PROBE3 raiser hook) does the work.
    setenv("HARNESS_RETSUCC", "1", 1);
    setenv("HARNESS_PROBE", "1a410c,fill", 1);
    setenv("HARNESS_PROBE2", "457f90", 1);
    setenv("HARNESS_PROBE3", "28a3dc", 1);
    setenv("HARNESS_PROBE_N", "500", 1);
    setenv("HARNESS_RDUMP", "1", 1);
    // Perf A/B (behavior-neutral infra): fastmem first, SAFEOPTS after.
    // Metric = updates/min + same-behavior verdicts (sessions/rdump).
    // 2026-10-14: fastmem back ON (proven neutral; no-fastmem boot is
    // ~10x slower). WPA tick PROVEN, off (log bloat).
    setenv("HARNESS_FASTMEM", "1", 1);
    // TRIAGE stash stays (governor reads last_mgr164_).
    setenv("HARNESS_WATCHPC", "250ab8", 1);
    // setenv("HARNESS_WPA", "672ea0", 1);  // tick PROVEN, off
    // Synthesis: DISABLED (POKE forces early body with unbuilt content
    // -> body returns 0 (quit) -> orderly suicide in seconds, §97.
    // Gate opens naturally only when content is ready; forcing it kills.
    // setenv("HARNESS_POKE1187", "1", 1);
    // r0=1-source hunt: probe the +912 quit-poller vcall site (c52ac)
    // -> per-hit r0 dump (ProbeDump). Slot PROBE4 (PROBE/PROBE3 taken).
    setenv("HARNESS_PROBE4", "c52ac", 1);
    // POSIX-truth (2026-10-14, proven by worker abort): untimed
    // cond_wait must never fail with ETIMEDOUT (game aborts on 110:
    // audio loader died this way). Cap cycles as SPURIOUS success (0)
    // so predicates re-check; true deadlocks stay visible in census.
    setenv("HARNESS_SPURWAKE", "1", 1);
    // Stuck-update forensics (2026-10-14): per-thread last import is
    // always tracked now (watchdog resolves the name at dump time).
    setenv("HARNESS_LASTSVC", "1", 1);
    // Worker import traffic for the tid=11 death forensics (one
    // diagnostic run; THREADLOG is capped at 5/import so log stays sane).
    setenv("HARNESS_THREADLOG", "1", 1);
    // +164 ground truth on device (read-only manager-pass entry dump;
    // closes "logically LOADING" -> proven or not).
    setenv("HARNESS_DIAG164", "1", 1);
    // Synthetic input heartbeat: ENABLED hands-off test (2026-10-14).
    // Prior disable was for finger-interleave chaos; solo (no finger)
    // it is the only event source. Tick jumps on taps PROVEN => this
    // should pump quanta hands-off. Binary: tick advances? menu?
    setenv("HARNESS_TAPBEAT", "1", 1);
    // Fail-functor synthesis timing: DISABLED (2026-10-14). Correlation:
    // boots WITH functor fire (14932 died, 35844 black-after-loading)
    // vs boot WITHOUT (15265 menu). Suspect: forged login-failed state
    // suppresses menu construction downstream. Binary test: off -> menu?
    // setenv("HARNESS_FUNCTORAT", "150", 1);
    // Real GLES on device (shared Handle; caller's context, no EGL init).
    // PC parity verified post-split (54/54, 948). First test: live ctx?
    // GL errors? survival? (pixels need content+burst; LOADING art first.)
    setenv("HARNESS_REALGL", "1", 1);
    Log("RETSUCC-as-ship armed");
    g_apk = FindApk();
    if (g_apk.empty()) {
        Log("FATAL: own base.apk not found");
        return 0;
    }
    Log("apk=%s", g_apk.c_str());
    const std::string guest_path = g_files + "/guest.so";
    if (!ZipExtractStored(g_apk, "assets/guestlib/armeabi-v7a/libAngryBirdsRio.so", guest_path)) {
        Log("FATAL: guest extract failed");
        return 0;
    }
    g_mem = new GuestMemory();
    if (!g_mem->ok()) {
        Log("FATAL: guest reservation failed");
        return 0;
    }
    ElfLoader loader(*g_mem);
    g_mod = new LoadedModule();
    std::string err;
    if (!loader.Load(guest_path.c_str(), *g_mod, err)) {
        Log("FATAL: load failed: %s", err.c_str());
        return 0;
    }
    Log("guest: %zu imports %zu init halt=%08x", g_mod->imports.size(), g_mod->init_array.size(),
        g_mod->halt_va);
    g_rt = new Runtime(*g_mem, *g_mod);
    for (const auto& seg : g_mod->segments) {
        g_rt->AddMapping(seg.start, seg.size, seg.prot);
    }
    {
        std::random_device rd;
        const std::uint32_t canary = (static_cast<std::uint32_t>(rd()) << 1) | 1u;
        std::memcpy(g_mem->host(abr::kGuestDataBase + 0), &canary, 4);
    }
    g_rt->InitDataImports();
    g_rt->SetFilesDir("/data/data/com.rovio.angrybirdsrio/files", g_files);
    g_rt->SetApkPath(g_apk);
    static GuestCallbacks callbacks(g_rt);
    g_cb = &callbacks;
    g_rt->set_callbacks(&callbacks);
    auto st = Enter();  // OnLoad thread state (+ real attach)
    g_rt->set_jit(st->jit);
    // Static init (park the dead-by-construction #22 stub 0x5885C).
    std::size_t ok = 0;
    for (std::size_t k = 0; k < g_mod->init_array.size(); k++) {
        if (g_mod->init_array[k] == 0x5885Cu) {
            Log("[init] #%zu @0005885c PARKED (dead-by-construction)", k);
            ok++;
            continue;
        }
        auto& regs = st->jit->Regs();
        regs.fill(0);
        regs[13] = st->stack + 0x200000u - 8;
        regs[14] = g_mod->halt_va;
        CallGuest(*st->jit, g_mod->init_array[k]);
        const Dynarmic::HaltReason hr = st->jit->Run();
        st->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        if (hr == Dynarmic::HaltReason::UserDefined1) {
            ok++;
        } else {
            Log("[init] #%zu @%08x hr=%d", k, g_mod->init_array[k], (int)hr);
        }
    }
    Log("init done: %zu/%zu", ok, g_mod->init_array.size());
    // Run the guest JNI_OnLoad (registers dynamic natives incl. Lua pump).
    {
        const auto it = g_mod->defined.find("JNI_OnLoad");
        if (it != g_mod->defined.end() && !(it->second & 1u)) {
            auto& regs = st->jit->Regs();
            regs.fill(0);
            regs[13] = st->stack + 0x200000u - 8;
            regs[14] = g_mod->halt_va;
            // Guest OnLoad expects a JavaVM* (mock handle; real pointers
            // are 64-bit and must never enter 32-bit guest regs). Our JVM
            // trampolines (fn 6/4) serve GetEnv/Attach off it.
            regs[0] = g_rt->Jvm();
            regs[1] = 0;
            CallGuest(*st->jit, it->second);
            const Dynarmic::HaltReason hr = st->jit->Run();
            st->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
            Log("guest OnLoad hr=%d r0=%08x", (int)hr, regs[0]);
        } else {
            Log("guest JNI_OnLoad missing?!");
        }
    }
    Log("OnLoad done, product live");
    static bool wdog_once = false;
    if (!wdog_once) {
        wdog_once = true;
        std::thread(Watchdog).detach();
        Log("watchdog armed (30s stuck-update dumps)");
    }
    return JNI_VERSION_1_6;
}

#include "apphost_gen.inc"
}  // namespace abr
