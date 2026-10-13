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
#include <mutex>
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
    CallGuest(*st->jit, va);
    const Dynarmic::HaltReason hr = st->jit->Run();
    if (hr != Dynarmic::HaltReason::UserDefined1) {
        Log("%s ended hr=%d r0=%08x", name, (int)hr, regs[0]);
    }
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
    va_list ap;
    va_start(ap, thiz);
    static int calln = 0;
    if (calln < 40) {
        Log("call#%d %s", calln, name);
    }
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
    return JNI_VERSION_1_6;
}

#include "apphost_gen.inc"
}  // namespace abr
