// apphost.cpp — Android product host (online-first milestone).
// Real Java drives: 63 Java_* forwarders (apphost_gen.inc) run guest code
// under Dynarmic with the REAL JNIEnv* (indices match NDK order, verified).
// Import thunks (libc/GLES/sockets/files/threads) are the proven harness
// ones; mocks shrink to era-tuned values only. Guest .so loads as DATA
// (mini-zip extract from our own base.apk), never via System.loadLibrary.
#include <jni.h>

#include <android/log.h>
#include <android/native_activity.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <exception>
#include <typeinfo>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <functional>
#include <algorithm>
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
#include <utility>
#include <vector>

#include "apphost.h"
#include "appid.h"
#include "jni_call_words.h"
#include "guest/dispatch.h"
#include "guest/adpf.h"
#include "guest/sampler.h"
#include "guest/elf_loader.h"
#include "guest/layout.h"
#include "guest/prof.h"
#include "zipfile.h"
#include "guest/sha256.h"

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
    std::uint32_t stack = 0;  // guest stack base (8MB, see GuestMalloc below)
    std::uint32_t tid = 0;    // bridge tid (errno/TLS routing)
    JNIEnv* env = nullptr;    // real, attached
    std::size_t cpu_id = SIZE_MAX;  // exclusive-monitor processor id (Runtime pool)
    // Re-entrancy guard: a nested JNI entry on the same thread while its
    // Jit runs trips Dynarmic's is_executing assert (tombstone). Drop the
    // nested frame LOUD instead (names the re-entrant path); e.g. a dropped
    // audio-priming buffer is inaudible, a tombstone is not.
    std::atomic<int> in_guest{0};
    const char* in_name = "?";
    // A Java -> native call that arrives while this thread is already in the
    // guest (Java called back into native from inside a JNI call: Unity's
    // nativeResize during nativeRender) runs on the next of these Jits, on
    // the same guest stack below the interrupted frame, as the nested call
    // would on an ARM device. depth = how many are running.
    std::vector<Dynarmic::A32::Jit*> nested;
    int depth = 0;
};
std::mutex g_tmu;
std::unordered_map<pid_t, std::shared_ptr<HostThread>> g_threads;

// Re-entrancy guard body (see HostThread): nested entry drops LOUD.
struct GuestRunGuard {
    HostThread* st;
    bool ok = false;
    GuestRunGuard(HostThread* s, const char* n) : st(s) {
        int e = 0;
        if (st->in_guest.compare_exchange_strong(e, 1)) {
            st->in_name = n;
            ok = true;
        }
    }
    ~GuestRunGuard() {
        if (ok) {
            st->in_guest = 0;
        }
    }
};

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[apphost] %s\n", buf);
    // One tag for every title the host serves (the library is libabhost.so);
    // the package is named in the OnLoad line instead.
    __android_log_print(ANDROID_LOG_INFO, "abhost", "[apphost] %s", buf);
}

// First abnormal guest halt: the LOUD reason is printed to files/apphost.log
// (stdout is redirected there), which is invisible in logcat and unreadable on
// a non-debuggable install. Mirror the tail of that file into logcat exactly
// once, so the trigger can be identified from a plain logcat dump.
void DumpHaltTailOnce(const char* name, int hr) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    const std::string path = g_files + "/apphost.log";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        Log("[halt] no log file at %s", path.c_str());
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    const long kTail = 16384;
    const long from = sz > kTail ? sz - kTail : 0;
    std::fseek(f, from, SEEK_SET);
    std::string buf(static_cast<std::size_t>(sz - from), '\0');
    const std::size_t rd = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(rd);
    Log("[halt] %s hr=%08x: apphost.log tail:", name, static_cast<unsigned>(hr));
    std::size_t p = 0;
    while (p < buf.size()) {
        std::size_t e = buf.find('\n', p);
        if (e == std::string::npos) {
            e = buf.size();
        }
        if (e > p) {
            Log("[halt] | %.*s", static_cast<int>(e - p), buf.data() + p);
        }
        p = e + 1;
    }
}

// G2.2 (2026-10-18): a real thread that enters the guest keeps its 8 MB guest
// stack + private Jit in g_threads forever (nothing erased them). A pthread TLS
// destructor releases both when the real thread exits, so thread churn cannot
// accumulate 8 MB per thread -- the F1 shape, on the Java->native path. By then
// the thread has stopped running guest code, so deleting the Jit and freeing
// the stack is safe; an in-flight shared_ptr (RunGuestNative) keeps the
// HostThread itself alive on its own.
// Update thread's JIT, for the watchdog's stuck-PC reads. Cleared when that
// thread's JIT is deleted (GLSurfaceView recreates its thread on pause/resume),
// so the watchdog never reads a freed Jit.
std::atomic<Dynarmic::A32::Jit*> g_upd_jit{nullptr};
pthread_key_t g_hostthread_key;
pthread_once_t g_hostthread_once = PTHREAD_ONCE_INIT;
std::atomic<unsigned> g_ht_created{0};

void HostThreadRelease(void* p) {
    const pid_t me = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(p));
    std::shared_ptr<HostThread> st;
    std::size_t live = 0;
    {
        std::lock_guard<std::mutex> l(g_tmu);
        const auto it = g_threads.find(me);
        if (it == g_threads.end()) {
            return;
        }
        st = it->second;
        g_threads.erase(it);
        live = g_threads.size();
    }
    Log("thread leave tid=%u stack=%08x live=%zu (created=%u)", st->tid, st->stack, live,
        g_ht_created.load());
    if (g_rt) {
        // Drop this tid's errno/TLS before the guest stack and JIT disappear.
        g_rt->ReapHostThread(st->tid);
        if (st->stack) {
            g_rt->GuestFree(st->stack);
            st->stack = 0;
        }
    }
    Dynarmic::A32::Jit* dying = st->jit;
    g_upd_jit.compare_exchange_strong(dying, nullptr);
    if (g_rt) {
        g_rt->ForgetSigThreadJit(st->tid);
    }
    // Jits come from NewJit (released): ReleaseJit takes them off record.
    for (Dynarmic::A32::Jit* j : st->nested) {
        g_rt ? g_rt->ReleaseJit(j) : delete j;
    }
    g_rt ? g_rt->ReleaseJit(st->jit) : delete st->jit;
    st->nested.clear();
    st->jit = nullptr;
    if (g_rt && st->cpu_id != SIZE_MAX) {
        g_rt->ReleaseCpuId(st->cpu_id);
        st->cpu_id = SIZE_MAX;
    }
}

void HostThreadKeyInit() {
    pthread_key_create(&g_hostthread_key, HostThreadRelease);
}

// App data dir (G4.2, 2026-10-18). /data/data/<pkg> is only the user-0 alias of
// /data/user/0/<pkg>; on a work profile or secondary user the real path is
// /data/user/<user>/<pkg>. Derive the user id from our own uid (uid = user *
// 100000 + appid) instead of assuming user 0. For user 0 this returns exactly
// the old hardcoded string.
std::string GuestFilesDir() {
    const unsigned user = static_cast<unsigned>(getuid() / 100000);
    const std::string base =
        user == 0 ? std::string("/data/data/") : "/data/user/" + std::to_string(user) + "/";
    return base + AppPackage() + "/files";
}

// Death witness (debuggerd is SELinux-muted on this device: native crashes
// leave no tombstone). Pre-opened fd + alt stack + async-safe-only handler:
// fault addr + guest regs into files/death.log, then re-raise to die as before.
static int g_death_fd = -1;
// Load base of this library (set in InstallDeathWitness via dladdr). The
// handler prints host offsets relative to it so llvm-addr2line can name the
// exact crashing host function/line of the *deployed* build.
static std::uintptr_t g_self_base = 0;
static void DeathHex(char*& p, std::uint64_t v, int digits) {
    static const char* H = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        *p++ = H[(v >> (i * 4)) & 0xFu];
    }
}
static void DeathHandler(int sig, siginfo_t* info, void* ucv) {
    char buf[512];
    char* p = buf;
    const char* t = "DEATH sig=";
    while (*t) {
        *p++ = *t++;
    }
    DeathHex(p, (std::uint64_t)sig, 2);
    // Faulting HOST instruction (the real crash site when the host, not the
    // guest, dereferenced garbage): raw pc + offset from our load base.
    if (ucv) {
        const std::uintptr_t hpc = (std::uintptr_t)((ucontext_t*)ucv)->uc_mcontext.pc;
        const char* th = " hpc=";
        while (*th) {
            *p++ = *th++;
        }
        DeathHex(p, hpc, 16);
        const char* tho = " hoff=";
        while (*tho) {
            *p++ = *tho++;
        }
        DeathHex(p, hpc - g_self_base, 16);
    }
    // Sender forensics: si_code/si_pid/si_uid tell abort()-in-thread apart
    // from an external tgkill (SI_TKILL) or ART runtime abort.
    const char* tc = " code=";
    while (*tc) {
        *p++ = *tc++;
    }
    DeathHex(p, (std::uint64_t)(info ? info->si_code : 0), 8);
    const char* tp = " pid=";
    while (*tp) {
        *p++ = *tp++;
    }
    DeathHex(p, (std::uint64_t)(info ? info->si_pid : 0), 8);
    const char* t2 = " addr=";
    while (*t2) {
        *p++ = *t2++;
    }
    DeathHex(p, (std::uint64_t)(info ? info->si_addr : nullptr), 16);
    if (g_rt) {
        Dynarmic::A32::Jit* aj = g_rt->ActiveJit();
        if (aj) {
            auto& r = aj->Regs();
            const char* t3 = " pc=";
            while (*t3) {
                *p++ = *t3++;
            }
            DeathHex(p, r[15], 8);
            const char* t4 = " lr=";
            while (*t4) {
                *p++ = *t4++;
            }
            DeathHex(p, r[14], 8);
            const char* t5 = " sp=";
            while (*t5) {
                *p++ = *t5++;
            }
            DeathHex(p, r[13], 8);
            const char* t6 = " r0=";
            while (*t6) {
                *p++ = *t6++;
            }
            DeathHex(p, r[0], 8);
            // Host frame-pointer walk (pure x29-chain reads, async-safe):
            // names the aborter (dlmalloc vs ART vs terminate) when the
            // guest pc alone can't. Capped to fit buf.
            const char* t7 = " host=";
            while (*t7) {
                *p++ = *t7++;
            }
            {
                std::uint64_t fp = (std::uint64_t)__builtin_frame_address(0);
                for (int f = 0; f < 8 && fp >= 0x7000000000ull && (p - buf) < 460; f++) {
                    const std::uint64_t* fr = (const std::uint64_t*)fp;
                    std::uint64_t next = fr[0];
                    std::uint64_t ret = fr[1];
                    if (next <= fp) {
                        break;
                    }
                    fp = next;
                    DeathHex(p, ret, 16);
                    *p++ = '-';
                    DeathHex(p, ret - g_self_base, 16);  // '<-base' offset
                    *p++ = ' ';
                }
            }
        }
    }
    *p++ = '\n';
    if (g_death_fd >= 0) {
        (void)write(g_death_fd, buf, (std::size_t)(p - buf));
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
// Uncaught-exception witness (Bionic's default terminate is logcat-only,
// invisible here): write the exception type to apphost.log, then die as
// before. Answers whether silent SIGABRTs are escaping host exceptions.
static void TerminateWitness() {
    char buf[256];
    char* p = buf;
    const char* t = "TERMINATE type=";
    while (*t) {
        *p++ = *t++;
    }
    // Copy what() INSIDE the catch (the pointer dangles after it).
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            const char* w = e.what();
            while (w && *w && (p - buf) < 240) {
                *p++ = *w++;
            }
        } catch (...) {
            const char* w = "non-std";
            while (*w) {
                *p++ = *w++;
            }
        }
    } else {
        const char* w = "no-exception (terminate without active)";
        while (*w) {
            *p++ = *w++;
        }
    }
    *p++ = '\n';
    (void)write(STDERR_FILENO, buf, (std::size_t)(p - buf));
    abort();
}

static void InstallDeathWitness(const std::string& dir) {
    const std::string p = dir + "/death.log";
    g_death_fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_death_fd < 0) {
        return;
    }
    Dl_info di{};
    if (dladdr(reinterpret_cast<void*>(&DeathHandler), &di) && di.dli_fbase) {
        g_self_base = reinterpret_cast<std::uintptr_t>(di.dli_fbase);
    }
    static std::uint8_t altstack[32768];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = DeathHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    // Silent-death expansion: SIGILL (bad JIT emit), SIGFPE, SIGTRAP, SIGSYS
    // (seccomp) all die without a word here; the witness names them.
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
    sigaction(SIGSYS, &sa, nullptr);
}

// Stuck-update watchdog (2026-10-14): a guest-mutex deadlock wedged the
// GL thread inside nativeUpdate with zero witnesses (BlueSky freeze).
// Tracks update entry/return; a detached thread dumps ThreadCensus +
// mutex owners + last import when an update exceeds 30s. Observe-only.
std::atomic<long long> g_upd_entry_ms{0};
std::atomic<long long> g_upd_done_ms{0};
std::atomic<int> g_upd_entry_n{0};
// Update thread's host tid (its JIT pointer is g_upd_jit, declared above).
std::atomic<int> g_upd_tid{0};

long long WallMs() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Name a guest PC on the import trampoline page.  The first import sits at
// kTrampBase + 8 (slots 0/1 are halt/once), so import idx = slot - 2.  The
// async PC read may land either on the svc (tramp) or one word after it, so
// callers print both this and the pc-4 name instead of guessing.
std::string ImportNameAt(std::uint32_t pc) {
    if (!g_mod || pc < abr::kTrampBase + 8 || pc >= g_mod->jni_tramp_base) {
        return {};
    }
    if (((pc - abr::kTrampBase) & 3u) != 0) {
        return {};
    }
    const std::int64_t idx = static_cast<std::int64_t>((pc - abr::kTrampBase) / 4) - 2;
    if (idx >= 0 && idx < static_cast<std::int64_t>(g_mod->imports.size())) {
        return g_mod->imports[idx].name;
    }
    // Guest-side VFP thunks (sqrt) live outside the 4-byte slot area; find the
    // import by the address the guest actually branches to.
    for (const abr::Import& im : g_mod->imports) {
        if (im.call_va != 0 && im.call_va == pc) {
            return im.name;
        }
    }
    return {};
}

void Watchdog() {
    long long reported = 0;
    unsigned last_corrupt = 0;
#ifdef ABR_DIAG_COUNTERS
    // Stage D: one [counters] line every 30 s. The watchdog already wakes
    // every 10 s; this adds no per-frame work to the guest.
    long long last_counters = WallMs();
#endif
#ifdef ABR_DIAG_COUNTERS
    // [codewatch] twice a second: Mono's boot failures (R-010) come ~10 s in,
    // and a change is reported on its second look.
    std::thread([]() {
        while (true) {
            usleep(500 * 1000);
            if (g_rt) {
                g_rt->DiagCheckCode();
            }
        }
    }).detach();
#endif
    while (true) {
        sleep(10);
        if (!g_rt) {
            continue;
        }
#ifdef ABR_DIAG_COUNTERS
        if (WallMs() - last_counters >= 30000) {
            last_counters = WallMs();
            g_rt->DumpDiagCounters();
        }
#endif
        // dlmalloc corruption tripwire (replaces abort): report transitions
        // with the guest pc captured at the trip. The game limps on past
        // dropped chunks, so this names WHEN headers break, not just death.
        {
            // A stall report on request: files/abdump (removed once read).
            const std::string req = g_files + "/abdump";
            if (::access(req.c_str(), F_OK) == 0) {
                // Optional lines "mem <guest addr> <bytes>": hex dumps.
                if (FILE* rf = std::fopen(req.c_str(), "r")) {
                    char line[128];
                    while (std::fgets(line, sizeof line, rf)) {
                        unsigned long a = 0, n = 0;
                        if (std::sscanf(line, "mem %lx %lu", &a, &n) == 2 && n <= 4096 &&
                            g_rt->CheckRange(static_cast<std::uint32_t>(a), n, false)) {
                            for (unsigned long o = 0; o < n; o += 16) {
                                char hex[80];
                                int k = 0;
                                for (unsigned long b = o; b < o + 16 && b < n; b += 4) {
                                    std::uint32_t w = 0;
                                    std::memcpy(&w, g_mem->host(static_cast<std::uint32_t>(a + b)), 4);
                                    k += std::snprintf(hex + k, sizeof hex - k, " %08x", w);
                                }
                                Log("[mem] %08lx:%s", a + o, hex);
                            }
                        }
                    }
                    std::fclose(rf);
                }
                ::unlink(req.c_str());
                Log("DUMP on request");
                g_rt->DumpGuestThreads();
                g_rt->ThreadCensus();
                g_rt->MutexOwnersDump();
            }
        }
        {
            // A guest that presents its own frames (EGL): how many per 10 s.
            static std::uint64_t last_swaps = 0;
            const std::uint64_t sw = g_rt->egl_swaps_.load(std::memory_order_relaxed);
            if (sw != last_swaps) {
                Log("[frames] %llu presented in the last 10 s, %ld wrapped Java objects",
                    (unsigned long long)(sw - last_swaps), g_rt->WrappedObjectCount());
                last_swaps = sw;
            }
        }
        {
            // The frame thread's CPU time per frame and the ADPF target, also
            // when ADPF is off (adpf.h): the number to compare on a phone.
            abr::adpf::Stats st;
            if (abr::adpf::TakeStats(&st)) {
                char clock[96] = "clock unreadable";
                if (st.cycles && st.cycles_cpu_ns) {
                    // cycles / CPU time = the time-weighted clock; per frame
                    // scaled to the frames the cycles cover.
                    const double mhz = st.cycles * 1e3 / st.cycles_cpu_ns;
                    const double per_frame = st.cycles / 1e6 / st.frames * st.cpu_ns / st.cycles_cpu_ns;
                    std::snprintf(clock, sizeof clock, "at %.0f MHz = %.2f Mcycles per frame", mhz, per_frame);
                }
                Log("[adpf] %llu frames in the last 10 s, frame thread CPU %.2f ms per frame %s, "
                    "target %.2f ms, session %s",
                    (unsigned long long)st.frames, st.cpu_ns / 1e6 / st.frames, clock, st.target_ns / 1e6,
                    st.session ? "on" : "off");
            }
        }
        {
            // jni_strings_ is never freed, and handles held by real objects
            // are global refs with a limit: a line when either has grown.
            static std::size_t last_strings = 0;
            static long top_objects = 0;
            std::size_t n = 0, bytes = 0;
            const long objects = g_rt->WrappedObjectCount();
            if (g_rt->JniStringStats(&n, &bytes) && (n != last_strings || objects > top_objects)) {
                last_strings = n;
                top_objects = std::max(top_objects, objects);
                Log("[strings] %zu jstrings kept, %zu bytes; %llu from Java method results; %ld wrapped Java objects",
                    n, bytes, (unsigned long long)g_rt->jni_string_results_.load(std::memory_order_relaxed),
                    objects);
            }
        }
        const unsigned cn = g_rt->HeapCorruptionCount();
        if (cn != last_corrupt) {
            last_corrupt = cn;
            Log("HEAPCORRUPT #%u pc=%08x (dlmalloc dropped poisoned chunks, limping on)", cn,
                g_rt->HeapCorruptionPc());
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
        // Lock-free first (atomics + JIT regs): these always print, even if
        // the map dumps below wedge on a leaked guard.
        const std::uint32_t si = g_rt->last_svc_idx_.load();
        const std::uint32_t st = g_rt->last_svc_tid_.load();
        const char* nm = "?";
        if (g_mod && si < g_mod->imports.size()) {
            nm = g_mod->imports[si].name.c_str();
        }
        Log("STUCK last-svc idx=%u tid=%u %s", si, st, nm);
        // Stuck guest PC of the update thread (SVC trampoline => import
        // name; module => code site; elsewhere => suspect). Sampled 3x with
        // 100ms gaps: async reg reads can tear, so only a STABLE pc proves a
        // parked thread (resume-loop or SVC-entry wedge); a wandering pc =
        // stale sample + guest spin.
        Dynarmic::A32::Jit* uj = g_upd_jit.load();
        if (uj && g_mod) {
            std::uint32_t pcs[3] = {0, 0, 0};
            for (int k = 0; k < 3; k++) {
                pcs[k] = uj->Regs()[15];
                if (k < 2) {
                    usleep(100000);
                }
            }
            const std::uint32_t pc = pcs[2];
            const std::uint32_t lr = uj->Regs()[14];
            const std::uint32_t sp = uj->Regs()[13];
            const std::string in0 = ImportNameAt(pc);
            const std::string in4 = ImportNameAt(pc - 4);
            std::string where = "other";
            std::string what = "";
            if (pc == g_mod->halt_va) {
                where = "HALT (returned?)";
            } else if (!in0.empty() || !in4.empty()) {
                where = "import-svc";
                what = "pc=" + (in0.empty() ? std::string("?") : in0) + " pc-4=" +
                       (in4.empty() ? std::string("?") : in4);
            } else if (pc >= g_mod->jni_tramp_base && pc < abr::kTrampBase + abr::kTrampSize) {
                where = "jni-svc";
                what = "fn#" + std::to_string((pc - g_mod->jni_tramp_base) / 4);
            } else if (g_rt->IsCodeAddress(pc & ~std::uint32_t(1))) {
                where = "guest-code (spin?)";
            }
            const bool stable = (pcs[0] == pcs[1] && pcs[1] == pcs[2]);
            Log("STUCK upc=%08x tid=%d %s %s lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x %s",
                pc, g_upd_tid.load(), where.c_str(), what.c_str(), lr, sp, uj->Regs()[0],
                uj->Regs()[1], uj->Regs()[2], uj->Regs()[3], stable ? "STABLE" : "wandering");
            // One short PC/LR histogram per stuck update sequence.  The async
            // PC alone alternates between the svc slot and its caller; the
            // histogram names the actual loop and its call sites.
            static int sampled_seq = -1;
            const int seq = g_upd_entry_n.load();
            if (seq != sampled_seq) {
                sampled_seq = seq;
                std::unordered_map<std::uint32_t, unsigned> ph, lh;
                for (int i = 0; i < 2000; i++) {
                    ph[uj->Regs()[15]]++;
                    lh[uj->Regs()[14]]++;
                    usleep(1000);
                }
                auto top = [](const std::unordered_map<std::uint32_t, unsigned>& m) {
                    std::vector<std::pair<unsigned, std::uint32_t>> v;
                    v.reserve(m.size());
                    for (const auto& kv : m) {
                        v.emplace_back(kv.second, kv.first);
                    }
                    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                        return a.first != b.first ? a.first > b.first : a.second < b.second;
                    });
                    if (v.size() > 12) {
                        v.resize(12);
                    }
                    return v;
                };
                for (const auto& e : top(ph)) {
                    const std::string n0 = ImportNameAt(e.second);
                    const std::string n4 = ImportNameAt(e.second - 4);
                    Log("STUCKSAMPLE pc %08x x%u %s", e.second, e.first,
                        !n0.empty() ? ("pc=" + n0 + (n4.empty() ? "" : " pc-4=" + n4)).c_str()
                                    : (g_rt->IsCodeAddress(e.second & ~1u) ? "guest-code" : "other"));
                }
                for (const auto& e : top(lh)) {
                    Log("STUCKSAMPLE lr %08x x%u %s", e.second, e.first,
                        g_rt->IsCodeAddress(e.second & ~1u) ? "guest-code" : "other");
                }
                // Return addresses on the guest stack: a cheap frame-chain
                // substitute.  Only scan ranges we know are mapped.
                if (g_mem && ((sp >= abr::kHeapBase && sp < abr::kHeapBase + abr::kHeapSize) ||
                              (sp >= abr::kStackBase && sp < abr::kStackTop))) {
                    // The scan only reads mapped words (the guest address
                    // space is one PROT_NONE 4GB reservation: a read past
                    // the end of the stack would take the HOST down).
                    abr::Runtime::StackHit hits[32];
                    const abr::Runtime::StackScan sc =
                        g_rt->ScanStackForReturnAddresses(sp, 0x4000, hits, 32);
                    for (unsigned i = 0; i < sc.hits; i++) {
                        Log("STUCK stack+%04x=%08x", hits[i].off, hits[i].value);
                    }
                }
            }
        }
        // Hang forensics: recent malloc/free history around the wedge.
        g_rt->DumpEventRing();
        // Map dumps last: they try_lock and skip on contention (a leaked
        // guard must not wedge the watchdog itself).
        g_rt->ThreadCensus();
        g_rt->MutexOwnersDump();
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

// A Jit for a host thread's guest code (its own, or a nested one: the same
// processor id, since only one of them runs at a time).
Dynarmic::A32::Jit* NewThreadJit(std::size_t cpu_id) {
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = g_cb;
    cfg.optimizations =
        JitOptimizations();
    cfg.unsafe_optimizations = JitUnsafeOptimizations();
    cfg.enable_cycle_counting = kJitCycleCounting;
    if (JitFastmemOn()) {
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(g_mem->base());
    }
    cfg.global_monitor = &g_rt->Monitor();  // one monitor for every Jit
    cfg.processor_id = cpu_id;
    return g_rt->NewJit(cfg).release();
}

std::shared_ptr<HostThread> Enter() {
    const pid_t me = gettid();
    {
        std::lock_guard<std::mutex> l(g_tmu);
        const auto it = g_threads.find(me);
        if (it != g_threads.end()) {
            g_rt->ThreadEnter(it->second->jit, it->second->tid);
            abr::Runtime::SetThreadStack(it->second->stack, kThreadStackSize);
            return it->second;
        }
    }
    auto st = std::make_shared<HostThread>();
    st->stack = g_rt->GuestMalloc(kThreadStackSize);  // 8MB: see above
    if (!st->stack) {
        Log("LOUD Enter: no guest heap for an 8 MB thread stack -- refusing the call");
        return nullptr;
    }
    if (!g_rt->AcquireCpuId(&st->cpu_id)) {
        Log("LOUD Enter: no exclusive-monitor processor id left -- refusing the call");
        g_rt->GuestFree(st->stack);
        return nullptr;
    }
    st->jit = NewThreadJit(st->cpu_id);
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
    std::size_t live = 0;
    {
        std::lock_guard<std::mutex> l(g_tmu);
        tid = g_rt->AllocTid();
        st->tid = tid;
        g_threads[me] = st;
        live = g_threads.size();
    }
    // Arm the exit destructor (G2.2) once per process, then point this thread's
    // key at its own tid so HostThreadRelease can find and free its entry.
    pthread_once(&g_hostthread_once, HostThreadKeyInit);
    pthread_setspecific(g_hostthread_key, reinterpret_cast<void*>(static_cast<std::intptr_t>(me)));
    const unsigned created = ++g_ht_created;
    g_rt->ThreadEnter(st->jit, tid);
    abr::Runtime::SetThreadStack(st->stack, kThreadStackSize);
    // One line per real thread (bounded -- not per call): the only witness that
    // Java->native thread churn would show. Always on.
    Log("thread enter tid=%u stack=%08x attached=%d live=%zu (created=%u)", (unsigned)tid, st->stack,
        env ? 1 : 0, live, created);
    return st;
}

// Runs guest `va` on this thread with AAPCS32 argument words (four in r0-r3,
// the rest on the guest stack) and returns when the guest returns; *used is
// the Jit it ran on (its registers hold the result). The outermost call runs
// on the thread's Jit from its stack top; a nested one (see HostThread) on
// the next nested Jit, below the sp of the frame it interrupts.
Dynarmic::HaltReason RunWordsOn(HostThread& st, const char* name, std::uint32_t va,
                                const std::vector<std::uint32_t>& words,
                                Dynarmic::A32::Jit** used) {
    if (st.in_guest.load()) {
        {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 40) {
                Log("nested %s inside %s (depth %d) lr=%08x", name, st.in_name, st.depth,
                    (st.depth == 0 ? st.jit : st.nested[static_cast<std::size_t>(st.depth - 1)])->Regs()[14]);
            }
        }
        Dynarmic::A32::Jit* outer = st.depth == 0 ? st.jit : st.nested[static_cast<std::size_t>(st.depth - 1)];
        if (st.nested.size() <= static_cast<std::size_t>(st.depth)) {
            Dynarmic::A32::Jit* j = NewThreadJit(st.cpu_id);  // the outer one is stopped
            st.nested.push_back(j);
        }
        Dynarmic::A32::Jit* j = st.nested[static_cast<std::size_t>(st.depth)];
        *used = j;
        const std::uint32_t outer_sp = outer->Regs()[13];
        const std::uint32_t nstack = words.size() > 4 ? static_cast<std::uint32_t>(words.size() - 4) * 4u : 0;
        const std::uint32_t sp = ((outer_sp - 0x100u) - nstack) & ~7u;
        auto& r = j->Regs();
        r.fill(0);
        r[13] = sp;
        for (std::size_t i = 0; i < words.size() && i < 4; i++) {
            r[i] = words[i];
        }
        for (std::size_t i = 4; i < words.size(); i++) {
            std::memcpy(g_mem->host(sp + (std::uint32_t)(i - 4) * 4), &words[i], 4);
        }
        r[14] = g_mod->halt_va;
        CallGuest(*j, va);
        st.depth++;
        g_rt->ThreadEnter(j, st.tid);
        const Dynarmic::HaltReason hr = RunJit(*j);
        j->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        st.depth--;
        g_rt->ThreadEnter(outer, st.tid);
        return hr;
    }
    *used = st.jit;
    auto& regs = st.jit->Regs();
    regs.fill(0);
    const std::uint32_t spbase = st.stack + kThreadStackSize - 0x100u;
    regs[13] = spbase;
    for (std::size_t i = 0; i < words.size() && i < 4; i++) {
        regs[i] = words[i];
    }
    for (std::size_t i = 4; i < words.size(); i++) {
        std::memcpy(g_mem->host(spbase + (std::uint32_t)(i - 4) * 4), &words[i], 4);
    }
    regs[14] = g_mod->halt_va;
    CallGuest(*st.jit, va);
    GuestRunGuard gg(&st, name);
    return RunJit(*st.jit);
}

// A guest function called by the host itself (a NativeActivity callback):
// r0 | r1 << 32, or 0 when it could not run.
std::uint64_t CallGuestFromHost(const char* name, std::uint32_t va,
                                const std::vector<std::uint32_t>& words) {
    auto st = Enter();
    if (!st || va == 0) {
        return 0;
    }
    Dynarmic::A32::Jit* used = nullptr;
    const Dynarmic::HaltReason hr = RunWordsOn(*st, name, va, words, &used);
    auto& regs = used->Regs();
    if (hr != Dynarmic::HaltReason::UserDefined1) {
        Log("%s ended hr=%d r0=%08x pc=%08x lr=%08x sp=%08x", name, (int)hr, regs[0], regs[15],
            regs[14], regs[13]);
        DumpHaltTailOnce(name, (int)hr);
        return 0;
    }
    return (std::uint64_t)regs[0] | ((std::uint64_t)regs[1] << 32);
}

// Runs the guest native at `va` for a Java call: (env, thiz, args per sig)
// marshalled to AAPCS32 on this thread's guest stack; returns r0 | r1 << 32.
std::uint64_t RunGuestNative(const char* name, std::uint32_t va, const char* sig, void* thiz,
                             va_list ap, JniFloatArgs floats) {
    // Every Java->guest call comes here (generated forwarders and
    // RegisterNatives stubs). Java ran before it, so this thread's GL context
    // may be another one now.
    ForgetGlBindings();
    auto st = Enter();
    if (!st) {
        return 1;  // same as a dropped re-entrant frame: logged LOUD in Enter
    }
    // Surface-size witnesses (2026-10-20, Play worldScale=0 triage). The guest
    // Lua camera code consumes screenWidth/screenHeight from these two calls;
    // a zero here makes worldScale unrecoverably zero. Diagnostic only.
    if (DiagOn() && (std::strstr(name, "nativeInit") || std::strstr(name, "nativeResize"))) {
        va_list ap2;
        va_copy(ap2, ap);
        const int sw = va_arg(ap2, int);
        const int sh = va_arg(ap2, int);
        va_end(ap2);
        std::printf("[screen] %s(%d,%d)\n", name, sw, sh);
    }
    // RATE telemetry (2026-10-14, tap-freeze triage): uncapped entry stamp
    // for the 3 frame-path methods. Answers pump-alive vs pump-dead without
    // taps. Observe-only (no guest state touched). Diagnostic layer: only
    // under HARNESS_DIAG (stdout is unbuffered, so this is a write() syscall
    // per frame otherwise -- PLAN.md A2).
    if (DiagOn() && (std::strstr(name, "nativeUpdate") || std::strstr(name, "nativeRender") ||
                     std::strstr(name, "nativeInput"))) {
        struct timespec rts {};
        ::clock_gettime(CLOCK_MONOTONIC, &rts);
        const long long rms = (long long)rts.tv_sec * 1000 + rts.tv_nsec / 1000000;
        std::printf("[rate] %s t=%lld tid=%d\n", name, rms, (int)gettid());
    }
    // D2 telemetry (2026-10-18): the live-block index plus its consumer
    // counters, every ~600 nativeUpdate frames (~10 s). Answers "what did the
    // old std::map actually cost, and did its odd paths (interior containment,
    // free-of-unknown, stale pool entries) ever fire" with numbers instead of
    // assumptions. Diag only, exactly like the rate stamp above.
    if (DiagOn() && std::strstr(name, "nativeUpdate")) {
        static unsigned long n = 0;
        if (++n % 600 == 0 && g_rt) {
            g_rt->PrintHeapIdx();
        }
    }
    // Watchdog entry stamp (nativeUpdate only).
    static int upd_seq = 0;
    const bool is_update = std::strstr(name, "nativeUpdate") != nullptr;
    if (is_update) {
        g_upd_entry_n = ++upd_seq;
        g_upd_entry_ms = WallMs();
        g_upd_jit = st->jit;
        g_upd_tid = (int)gettid();
    }
    // The guest's register words: mock env + wrapped thiz (the guest only
    // ever sees 32-bit mock handles; real 64-bit pointers never enter guest
    // regs/fields), then sig-driven (J split in two with 8-byte alignment,
    // F via double promotion, I/Z/B/S/C ints, L/[ wrapped).
    //
    // 64-bit arguments take an EVEN core-register pair, or an 8-byte aligned
    // stack slot when no pair is left. That is measured against the guest's
    // own native entry, not assumed: Utils.handleReferrerValue
    // (Ljava/lang/String;JJ)V reads its two longs at entry_sp+0 and +8,
    // which is exactly where the aligned layout puts the low words. Do not
    // remove the padding: this is the only method in the union where a
    // 64-bit argument follows a 32-bit one, and dropping the pad silently
    // hands the guest the HIGH words (usually zero) instead of the values.
    // The variadic printf reader is a separate path with its own rule.
    std::vector<std::uint32_t> words;
    words.push_back(g_rt->JniEnv());
    words.push_back(g_rt->WrapObject(thiz));
    AppendJniArgs(words, sig, ap, floats, [](void* o) { return g_rt->WrapObject(o); });
    // HARNESS_PROF: a render-thread frame that takes too long is broken down.
    const bool frame_prof = abr::prof::g_enabled && !st->in_guest.load() && std::strstr(name, "nativeRender");
    const std::uint64_t frame_t0 = frame_prof ? abr::prof::FrameBegin() : 0;
    Dynarmic::A32::Jit* used = nullptr;
    const Dynarmic::HaltReason hr = RunWordsOn(*st, name, va, words, &used);
    if (frame_prof) {
        abr::prof::FrameEnd(frame_t0, [](std::uint32_t swi) { return g_rt->SvcName(swi); },
                            [](const char* fmt, auto... a) { Log(fmt, a...); });
    }
    auto& regs = used->Regs();
    g_rt->FlushDirectBuffers();  // what the guest wrote into direct buffers
    if (is_update) {
        g_upd_done_ms = WallMs();
    }
#ifdef __ANDROID__
    // Input-stream forensics (PLAY button animates but never fires):
    // log every nativeInput action + coords (capped). words[2]=action,
    // [3]=x-bits, [4]=y-bits (IJNI: env,thiz,action,x,y,?).
    if (DiagOn() && std::strstr(name, "nativeInput") && words.size() >= 5) {
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
        Log("%s ended hr=%d r0=%08x pc=%08x lr=%08x sp=%08x", name, (int)hr, regs[0],
            regs[15], regs[14], regs[13]);
        DumpHaltTailOnce(name, (int)hr);
    }
#ifdef __ANDROID__
    // r0-polarity forensics: log EVERY nativeUpdate return (clean ones
    // are silent above). Java quits on r0=0 (smali: if-nez v0 skips EXITING,
    // so nonzero = continue); need per-update 0/1.
    // Capped (updates are ~30/s idle).
    else if (DiagOn() && std::strstr(name, "nativeUpdate")) {
        static int un = 0;
        if (un < 400) {
            Log("%s r0=%08x", name, regs[0]);
        }
        un++;
    }
#endif
    return (std::uint64_t)regs[0] | ((std::uint64_t)regs[1] << 32);
}

// A generated Java_* forwarder: the guest export of the same name.
// Every guest library Java loads (System.loadLibrary), in that order; empty
// when only the primary is (guest.libs lines marked "java").
std::vector<int> g_java_libs;

std::uint64_t AppHost_CallImpl(const char* name, const char* sig, void* thiz, va_list ap) {
    static std::unordered_map<std::string, std::uint32_t> vacache;
    std::uint32_t va = 0;
    {
        static std::mutex cmu;
        std::lock_guard<std::mutex> l(cmu);
        const auto it = vacache.find(name);
        if (it != vacache.end()) {
            va = it->second;
        } else {
            std::uint32_t at = 0;
            bool found = false;
            if (const auto jt = g_mod->defined.find(name); jt != g_mod->defined.end()) {
                at = jt->second;
                found = true;
            }
            // A native of another library Java loads (SDL's in libSDL2).
            for (std::size_t k = 0; !found && k < g_java_libs.size(); k++) {
                const auto& defs = g_mod->lib_defined[static_cast<std::size_t>(g_java_libs[k])];
                if (const auto jt = defs.find(name); jt != defs.end()) {
                    at = jt->second;
                    found = true;
                }
            }
            // A Thumb entry keeps its low bit: CallGuest switches the mode.
            if (!found) {
                std::string where;
                for (std::size_t k = 0; k < g_mod->lib_defined.size(); k++) {
                    if (g_mod->lib_defined[k].count(name)) {
                        where += " lib#" + std::to_string(k);
                    }
                }
                Log("no guest VA for %s (merged %zu names, java libs %zu, in:%s)", name, g_mod->defined.size(),
                    g_java_libs.size(), where.empty() ? " none" : where.c_str());
                return 0;
            }
            va = at;
            vacache[name] = va;
        }
    }
    return RunGuestNative(name, va, sig, thiz, ap, JniFloatArgs::kPromoted);
}

// --- RegisterNatives: host stubs the real JVM calls for guest natives ---
//
// A guest that registers natives at run time (Unity's libmain/libunity) hands
// RegisterNatives guest entries; ART needs host functions. Each registered
// method takes one stub from a fixed pool. The JVM calls a stub with fixed
// AAPCS64 arguments, which the stub reads through its own `...`
// (JniFloatArgs::kRaw explains why that is exact). Stubs come in three
// return families, since AAPCS64 returns integers and references in x0, a
// float in s0 and a double in d0.
struct NativeSlot {
    std::uint32_t va = 0;  // guest entry (bit 0 = Thumb)
    char ret = 'V';        // first char of the return type
    std::string name;      // "cls.method", for logs
    std::string sig;
};
constexpr std::size_t kWordStubs = 256;
constexpr std::size_t kFloatStubs = 32;
constexpr std::size_t kDoubleStubs = 32;
NativeSlot g_word_slots[kWordStubs];
NativeSlot g_float_slots[kFloatStubs];
NativeSlot g_double_slots[kDoubleStubs];

std::uint64_t RunSlot(const NativeSlot& s, void* thiz, va_list ap) {
    return RunGuestNative(s.name.c_str(), s.va, s.sig.c_str(), thiz, ap, JniFloatArgs::kRaw);
}

// One Java -> native call's local frame: the handles made while it runs
// are its local references and go when it returns.
struct HostLocalFrame {
    HostLocalFrame() { abr::Runtime::PushLocalFrame(); }
    ~HostLocalFrame() {
        if (g_rt) {
            g_rt->PopLocalFrame();
        }
    }
    HostLocalFrame(const HostLocalFrame&) = delete;
    HostLocalFrame& operator=(const HostLocalFrame&) = delete;
};

// The guest's r0 (r1:r0 for J) as the JNI return value of type `ret`; a
// reference becomes a local one of the JVM's (the handle goes with the frame).
jlong WordReturn(JNIEnv* env, char ret, std::uint64_t r) {
    const std::uint32_t r0 = static_cast<std::uint32_t>(r);
    switch (ret) {
        case 'Z': return static_cast<std::uint8_t>(r0) ? 1 : 0;
        case 'B': return static_cast<std::int8_t>(r0);
        case 'C': return static_cast<std::uint16_t>(r0);
        case 'S': return static_cast<std::int16_t>(r0);
        case 'I': return static_cast<std::int32_t>(r0);
        case 'J': return static_cast<jlong>(r);
        case 'L':
        case '[': {
            abr::Runtime::JavaCopies copies;
            jobject o = static_cast<jobject>(g_rt->JavaObjectFor(r0));
            return reinterpret_cast<jlong>(o ? env->NewLocalRef(o) : nullptr);
        }
        default: return 0;  // V
    }
}

template <std::size_t N>
jlong WordStub(JNIEnv* env, jobject thiz, ...) {
    HostLocalFrame frame;
    va_list ap;
    va_start(ap, thiz);
    const std::uint64_t r = RunSlot(g_word_slots[N], thiz, ap);
    va_end(ap);
    return WordReturn(env, g_word_slots[N].ret, r);
}

template <std::size_t N>
jfloat FloatStub(JNIEnv*, jobject thiz, ...) {
    HostLocalFrame frame;
    va_list ap;
    va_start(ap, thiz);
    const std::uint32_t r0 = static_cast<std::uint32_t>(RunSlot(g_float_slots[N], thiz, ap));
    va_end(ap);
    float f = 0;
    std::memcpy(&f, &r0, 4);  // softfp: the float's bits in r0
    return f;
}

template <std::size_t N>
jdouble DoubleStub(JNIEnv*, jobject thiz, ...) {
    HostLocalFrame frame;
    va_list ap;
    va_start(ap, thiz);
    const std::uint64_t r = RunSlot(g_double_slots[N], thiz, ap);
    va_end(ap);
    double d = 0;
    std::memcpy(&d, &r, 8);  // softfp: r1:r0
    return d;
}

template <std::size_t... I>
std::vector<void*> WordStubs(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&WordStub<I>)...};
}
template <std::size_t... I>
std::vector<void*> FloatStubs(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&FloatStub<I>)...};
}
template <std::size_t... I>
std::vector<void*> DoubleStubs(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&DoubleStub<I>)...};
}

// GuestCallbacks::g_native_stub_hook. One stub per (class, method, sig,
// entry): registering the same native again reuses its stub.
void* NativeStubFor(const char* cls, const char* name, const char* sig, std::uint32_t fn) {
    static std::mutex mu;
    static std::unordered_map<std::string, void*> by_key;
    static std::size_t used[3] = {0, 0, 0};
    static const std::vector<void*> stubs[3] = {
        WordStubs(std::make_index_sequence<kWordStubs>()),
        FloatStubs(std::make_index_sequence<kFloatStubs>()),
        DoubleStubs(std::make_index_sequence<kDoubleStubs>())};
    NativeSlot* const slots[3] = {g_word_slots, g_float_slots, g_double_slots};
    const char* rp = std::strchr(sig, ')');
    if (!rp || !rp[1]) {
        return nullptr;
    }
    const char ret = rp[1];
    const int family = ret == 'F' ? 1 : ret == 'D' ? 2 : 0;
    char vab[12];
    std::snprintf(vab, sizeof vab, "@%08x", fn);
    const std::string key = std::string(cls) + "." + name + sig + vab;
    std::lock_guard<std::mutex> l(mu);
    const auto it = by_key.find(key);
    if (it != by_key.end()) {
        return it->second;
    }
    if (used[family] >= stubs[family].size()) {
        Log("LOUD RegisterNatives: the %s stub pool is full",
            family == 0 ? "word" : family == 1 ? "float" : "double");
        return nullptr;
    }
    const std::size_t n = used[family]++;
    NativeSlot& s = slots[family][n];
    s.va = fn;
    s.ret = ret;
    s.name = std::string(cls) + "." + name;
    s.sig = sig;
    by_key[key] = stubs[family][n];
    return stubs[family][n];
}

// Lowercase hex SHA-256 of a file ("" if unreadable).
std::string FileSha256(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return "";
    }
    abr::Sha256 sh;
    std::vector<std::uint8_t> buf(1 << 20);
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        sh.Update(buf.data(), n);
    }
    std::fclose(f);
    std::uint8_t out[32];
    sh.Final(out);
    static const char* hex = "0123456789abcdef";
    std::string got;
    for (int i = 0; i < 32; i++) {
        got.push_back(hex[out[i] >> 4]);
        got.push_back(hex[out[i] & 0x0f]);
    }
    return got;
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
    // /data/app/<pkg>, <pkg>-1, <pkg>-2 ...: every install-generation directory
    // starts with the package name, so one prefix match covers them all.
    const std::string& pkg = AppPackage();
    const char* roots[] = {"/data/app", nullptr};
    for (int r = 0; roots[r]; r++) {
        DIR* d = opendir(roots[r]);
        if (!d) {
            continue;
        }
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() <= pkg.size() || n.compare(0, pkg.size(), pkg) != 0) {
                continue;
            }
            const std::string cand = std::string(roots[r]) + "/" + n + "/base.apk";
            FILE* f = std::fopen(cand.c_str(), "rb");
            if (f) {
                std::fclose(f);
                closedir(d);
                return cand;
            }
        }
        closedir(d);
    }
    return "";
}

#ifdef __ANDROID__
// The loader of the app's own classes, taken once from the first native
// method Java calls: its `thiz` is an app class (static native) or an object
// of one. JNI_OnLoad's context loader can be the system loader, which does
// not know the app's classes (Minecraft: Story Mode: org.fmod.AudioDevice not
// found from FMOD's thread, so no audio and the sound thread never answered).
void AdoptAppClassLoader(JNIEnv* env, void* thiz) {
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_relaxed) || !env || !thiz || !g_rt) {
        return;
    }
    if (done.exchange(true)) {
        return;
    }
    jobject self = static_cast<jobject>(thiz);
    jclass class_class = env->FindClass("java/lang/Class");
    jclass app_class = nullptr;
    if (class_class && env->IsInstanceOf(self, class_class)) {
        app_class = static_cast<jclass>(env->NewLocalRef(self));
    } else {
        app_class = env->GetObjectClass(self);
    }
    jmethodID gcl = class_class ? env->GetMethodID(class_class, "getClassLoader",
                                                   "()Ljava/lang/ClassLoader;")
                                : nullptr;
    jobject cl = (app_class && gcl) ? env->CallObjectMethod(app_class, gcl) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        cl = nullptr;
    }
    if (cl) {
        // The previous loader's global ref stays: another thread may be
        // using it right now (one ref, once per process).
        g_rt->SetAppClassLoader(env->NewGlobalRef(cl));
        Log("app class loader: from the first native call's class");
        env->DeleteLocalRef(cl);
    }
    if (app_class) {
        env->DeleteLocalRef(app_class);
    }
    if (class_class) {
        env->DeleteLocalRef(class_class);
    }
}
#endif

}  // namespace

std::uint64_t AppHost_Call(const char* name, const char* sig, void* env, void* thiz, ...) {
    va_list ap;
    va_start(ap, thiz);
    static int calln = 0;
#ifdef __ANDROID__
    // Lifecycle inventory: log every DISTINCT native once (uncapped by
    // design; ~63 max). Answers "which Java->native traffic exists".
    {
        static std::unordered_set<std::string> seen;
        static std::mutex smu;
        if (DiagOn()) {
            std::lock_guard<std::mutex> l(smu);
            if (seen.insert(name).second) {
                Log("uniqcall %s", name);
            }
        }
    }
    if (DiagOn() && calln < 400) {  // device runs long; 40 hid continuation (§94)
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
    HostLocalFrame frame;
#ifdef __ANDROID__
    AdoptAppClassLoader(static_cast<JNIEnv*>(env), thiz);
#endif
    const std::uint64_t rv = AppHost_CallImpl(name, sig, thiz, ap);
    va_end(ap);
    // The Rovio fusion engine renders from a Java GLSurfaceView: its
    // SingleThreadWrapper.onFrame calls doUpdate -> nativeUpdate once per
    // frame on the GL thread (update and render; nativeRender is declared
    // but never called), which marks the frame (ADPF, adpf.h). Guests that
    // present their own frames mark them at eglSwapBuffers.
    if (std::strcmp(name, "Java_com_rovio_fusion_NativeApplication_nativeUpdate") == 0) {
        abr::adpf::FrameDone();
    }
    return rv;
}

namespace {
// Real-Java delegation for JNI thunks (product host): the calling host
// thread's real JNIEnv*, or null (harness has none -> mocks stay).
// A guest-created thread (pthread_create) that calls into Java is attached to
// the JVM the first time, as the guest's own AttachCurrentThread would on a
// device, and detached when it exits (ART aborts on an attached thread that
// exits without detaching).
struct GuestThreadAttach {
    JNIEnv* env = nullptr;
    ~GuestThreadAttach() {
        if (env && g_vm) {
            g_vm->DetachCurrentThread();
        }
    }
};

void* RealEnvForThread() {
    const pid_t me = gettid();
    {
        std::lock_guard<std::mutex> l(g_tmu);
        const auto it = g_threads.find(me);
        if (it != g_threads.end()) {
            return static_cast<void*>(it->second->env);
        }
    }
    thread_local GuestThreadAttach attach;
    if (!attach.env && g_vm) {
        JNIEnv* env = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
            return env;  // attached by someone else (not ours to detach)
        }
        if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attach.env = env;
        }
    }
    return attach.env;
}
}  // namespace

// HARNESS_DIAG is also honoured from files/abflags.txt, which has NOT been
// parsed yet at this point (shipping defaults are set first, then abflags
// overrides them). Read just that key directly: one small file read per boot.
static bool DiagBootWanted(const std::string& files_dir) {
    if (const char* e = std::getenv("HARNESS_DIAG")) {
        if (e[0] && std::strcmp(e, "-") != 0 && std::strcmp(e, "0") != 0 &&
            std::strcmp(e, "off") != 0 && std::strcmp(e, "false") != 0) {
            return true;
        }
    }
    const std::string fp = files_dir + "/abflags.txt";
    FILE* ff = std::fopen(fp.c_str(), "r");
    if (!ff) {
        return false;
    }
    bool on = false;
    char line[256];
    while (std::fgets(line, sizeof(line), ff)) {
        char* nl = std::strchr(line, '\n');
        if (nl) {
            *nl = 0;
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char* eq = std::strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char* v = eq + 1;
        if (std::strcmp(line, "HARNESS_DIAG") == 0 && v[0] && std::strcmp(v, "-") != 0 &&
            std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0 &&
            std::strcmp(v, "false") != 0) {
            on = true;
        }
    }
    std::fclose(ff);
    return on;
}

namespace {
int g_primary = 0;  // the guest library Java loads (index in the module)
}  // namespace

// Loads and starts the guest once, from whichever entry Android uses first:
// JNI_OnLoad (System.loadLibrary) or, for a NativeActivity title,
// ANativeActivity_onCreate, which the framework calls before Java loads the
// library. False when the guest cannot run.
static bool Boot(JavaVM* vm) {
    static std::mutex boot_mu;
    static int booted = 0;  // 0 not yet, 1 ok, -1 failed
    std::lock_guard<std::mutex> boot_lock(boot_mu);
    if (booted != 0) {
        return booted > 0;
    }
    booted = -1;
    g_vm = vm;
    GuestCallbacks::g_real_env_hook = RealEnvForThread;
    GuestCallbacks::g_native_stub_hook = NativeStubFor;
    // Logs to app files dir (pullable); stdout unbuffered.
    g_files = GuestFilesDir();
    std::error_code ec;
    std::filesystem::create_directories(g_files, ec);
    const std::string logpath = g_files + "/apphost.log";
    // One open file for both streams: truncate once, then stderr shares
    // stdout's descriptor (and so its O_APPEND offset). Separate "w"/"a"
    // opens gave stdout its own offset, so later stdout lines overwrote what
    // stderr had appended (HEAPCORRUPT/HEAPASSERT/terminate witness).
    if (std::FILE* t = std::fopen(logpath.c_str(), "w")) {
        std::fclose(t);
    }
    std::freopen(logpath.c_str(), "a", stdout);
    ::dup2(fileno(stdout), fileno(stderr));
    setvbuf(stdout, nullptr, _IONBF, 0);
    // The zip module logs through the app's logger (it lives in its own
    // module so the unit test can reach it).
    abr::ZipSetLog(&Log);
    InstallDeathWitness(g_files);  // SIGSEGV/SIGABRT/SIGBUS witness (debuggerd is SELinux-muted here)
    std::set_terminate(TerminateWitness);  // name escaping host exceptions instead of silent abort
    Log("OnLoad: product host starting");
    Log("build marker %s %s", __DATE__, __TIME__);
    // ---- diagnostic layer: ONE switch, OFF for shipping (2026-10-17) -------
    // HARNESS_DIAG=1 (real environment OR files/abflags.txt) re-arms the
    // per-call/per-frame tracing and the hot-path logs; with it off that whole
    // layer is skipped (no logs, no per-call cost). FASTMEM and REALGL are
    // NOT diagnostics -- they change behaviour -- and stay unconditional.
    const bool diag = DiagBootWanted(g_files);
    if (diag) {
        setenv("HARNESS_DIAG", "1", 1);
    }
    // Experiments go through files/abflags.txt (applied after these defaults,
    // logged as overrides), so the log always states what was actually on.
    // Per-thread last import for the stuck-update watchdog dump.
    if (diag) setenv("HARNESS_LASTSVC", "1", 1);
    // Worker import traffic (THREADLOG is capped at 5/import).
    if (diag) setenv("HARNESS_THREADLOG", "1", 1);
    // Device-side experiment flags: <files>/abflags.txt, one KEY=VALUE per
    // line ('#' comments ok), applied AFTER the shipping defaults so a pushed
    // file overrides them without a rebuild. Every override is logged, so a
    // stale file can never silently change a run (the 2026-10-16 hunt died of
    // exactly that: an experiment premise that was not in the log).
    {
        const std::string fpath = g_files + "/abflags.txt";
        if (FILE* ff = std::fopen(fpath.c_str(), "r")) {
            char line[256];
            while (std::fgets(line, sizeof(line), ff)) {
                char* nl = std::strchr(line, '\n');
                if (nl) {
                    *nl = 0;
                }
                if (!line[0] || line[0] == '#') {
                    continue;
                }
                char* eq = std::strchr(line, '=');
                if (!eq) {
                    continue;
                }
                *eq = 0;
                if (eq[1] == 0 || std::strcmp(eq + 1, "-") == 0) {
                    unsetenv(line);  // "KEY=" or "KEY=-": turn a default OFF
                    Log("override %s=<unset>", line);
                    continue;
                }
                setenv(line, eq + 1, 1);
                Log("override %s=%s", line, eq + 1);
            }
            std::fclose(ff);
        }
    }
    g_apk = FindApk();
    if (g_apk.empty()) {
        Log("FATAL: own base.apk not found");
        return false;
    }
    Log("apk=%s", g_apk.c_str());
    // The guest .so is whatever sits under assets/guestlib/ (the packaging step
    // puts exactly one there). Naming it in C would be a per-title constant.
    const std::string glib_dir = "assets/guestlib/";
    // Several guest libraries (Unity, SDL titles): <abi>/guest.libs lists them
    // in load order, one "<sha256> <name> [primary] [java]" per line; the
    // primary one is the library Java loads last, "java" marks every one Java
    // loads itself. Each is extracted under its own name (a guest dlopen finds
    // it by name) and checked against its digest.
    struct GuestLib {
        std::string name, sha, path;
        bool primary = false;
        bool java = false;
    };
    std::vector<GuestLib> multi;
    std::string multi_dir;
    for (const std::string& e : ZipListPrefix(g_apk, glib_dir)) {
        if (e.size() > 11 && e.compare(e.size() - 11, 11, "/guest.libs") == 0) {
            multi_dir = e.substr(0, e.size() - 10);
            const std::string lp = g_files + "/guest.libs";
            FILE* lf = ZipExtractStored(g_apk, e, lp) ? std::fopen(lp.c_str(), "rb") : nullptr;
            char line[512];
            while (lf && std::fgets(line, sizeof(line), lf)) {
                char sha[80] = {0}, name[256] = {0}, f1[32] = {0}, f2[32] = {0};
                if (std::sscanf(line, "%79s %255s %31s %31s", sha, name, f1, f2) >= 2) {
                    const auto has = [&](const char* flag) {
                        return std::strcmp(f1, flag) == 0 || std::strcmp(f2, flag) == 0;
                    };
                    multi.push_back({name, sha, g_files + "/guestlib/" + name, has("primary"), has("java")});
                }
            }
            if (lf) {
                std::fclose(lf);
            }
        }
    }
    std::string guest_entry;
    if (!multi.empty()) {
        std::filesystem::create_directories(g_files + "/guestlib", ec);
        for (std::size_t i = 0; i < multi.size(); i++) {
            const GuestLib& gl = multi[i];
            if (!ZipExtractStored(g_apk, multi_dir + gl.name, gl.path)) {
                Log("FATAL: guest %s extract failed", gl.name.c_str());
                return false;
            }
            const std::string got = FileSha256(gl.path);
            if (got != gl.sha) {
                Log("[guest] LOUD %s sha256 mismatch: apk says %s, image is %s -- refusing to run",
                    gl.name.c_str(), gl.sha.c_str(), got.c_str());
                return false;
            }
            Log("[guest] %s sha256 OK %s%s", gl.name.c_str(), got.c_str(), gl.primary ? " (primary)" : "");
            if (gl.primary) {
                g_primary = static_cast<int>(i);
                guest_entry = multi_dir + gl.name;
            }
            if (gl.java) {
                g_java_libs.push_back(static_cast<int>(i));
            }
        }
    }
    for (const std::string& e : multi.empty() ? ZipListPrefix(g_apk, glib_dir) : std::vector<std::string>()) {
        if (e.size() > 3 && e.compare(e.size() - 3, 3, ".so") == 0) {
            if (!guest_entry.empty()) {
                Log("FATAL: several guest .so under %s: %s, %s", glib_dir.c_str(),
                    guest_entry.c_str(), e.c_str());
                return false;
            }
            guest_entry = e;
        }
    }
    if (guest_entry.empty()) {
        Log("FATAL: no guest .so under %s", glib_dir.c_str());
        return false;
    }
    Log("guest entry %s", guest_entry.c_str());
    const std::string guest_path = g_files + "/guest.so";
    if (multi.empty() && !ZipExtractStored(g_apk, guest_entry, guest_path)) {
        Log("FATAL: guest extract failed");
        return false;
    }
    // Guest identity: the packaging step wrote the guest .so's SHA-256 next
    // to it inside the APK. Comparing it with the extracted image makes the
    // identity of the running guest a RUNTIME fact. The GNU build-id note is
    // missing from some titles' images (Seasons, Space), so for them this is
    // the only identity check there is; the build-id note stays as a second,
    // independent signal where the image has one.
    bool guest_identity_ok = !multi.empty();  // every library checked above
    if (multi.empty()) {
        const std::size_t slash = guest_entry.find_last_of('/');
        const std::string digest_entry =
            (slash == std::string::npos ? std::string() : guest_entry.substr(0, slash + 1)) +
            "guest.sha256";
        const std::string want_path = g_files + "/guest.sha256";
        std::string want;
        if (ZipExtractStored(g_apk, digest_entry, want_path)) {
            FILE* wf = std::fopen(want_path.c_str(), "rb");
            if (wf) {
                char line[160] = {0};
                if (std::fgets(line, sizeof(line), wf)) {
                    want = line;
                    while (!want.empty() && (want.back() == '\n' || want.back() == '\r' ||
                                             want.back() == ' ' || want.back() == '\t')) {
                        want.pop_back();
                    }
                }
                std::fclose(wf);
            }
        }
        std::string got;
        FILE* gf = std::fopen(guest_path.c_str(), "rb");
        if (gf) {
            abr::Sha256 sh;
            std::vector<std::uint8_t> buf(1 << 20);
            std::size_t n;
            while ((n = std::fread(buf.data(), 1, buf.size(), gf)) > 0) {
                sh.Update(buf.data(), n);
            }
            std::fclose(gf);
            std::uint8_t out[32];
            sh.Final(out);
            static const char* hex = "0123456789abcdef";
            got.reserve(64);
            for (int i = 0; i < 32; i++) {
                got.push_back(hex[out[i] >> 4]);
                got.push_back(hex[out[i] & 0x0f]);
            }
        }
        std::string wlow;
        for (char c : want) {
            wlow.push_back(static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c));
        }
        if (!want.empty() && !got.empty()) {
            if (wlow == got) {
                guest_identity_ok = true;
                Log("[guest] sha256 OK %s", got.c_str());
            } else {
                Log("[guest] LOUD guest sha256 mismatch: apk says %s, image is %s -- refusing to run",
                    wlow.c_str(), got.c_str());
                return false;
            }
        } else {
            Log("[guest] no guest fingerprint in the apk (older packaging) -- identity unverified");
        }
    }
    // First-launch seeds (root-free fresh installs): extract only what is
    // missing, never overwrite user state (registry keeps tokens/progress).
    // Whatever the title ships under assets/seeds/ is what it needs; a title
    // without that directory simply has none (Space).
    {
        const std::string seeds_dir = "assets/seeds/";
        for (const std::string& e : ZipListPrefix(g_apk, seeds_dir)) {
            const std::string name = e.substr(seeds_dir.size());
            if (name.empty() || name.find('/') != std::string::npos) {
                continue;  // flat files only
            }
            const std::string dst = g_files + "/" + name;
            FILE* probe = std::fopen(dst.c_str(), "rb");
            if (probe) {
                std::fclose(probe);
                continue;
            }
            if (ZipExtractStored(g_apk, e, dst)) {
                Log("seeded %s", name.c_str());
            } else {
                Log("seed MISSING from apk: %s", e.c_str());
            }
        }
    }
    g_mem = new GuestMemory();
    if (!g_mem->ok()) {
        Log("FATAL: guest reservation failed");
        return false;
    }
    ElfLoader loader(*g_mem);
    g_mod = new LoadedModule();
    std::string err;
    if (multi.empty()) {
        if (!loader.Load(guest_path.c_str(), *g_mod, err)) {
            Log("FATAL: load failed: %s", err.c_str());
            return false;
        }
        {  // one library, at its linked addresses: offsets are its own
            const std::size_t sl = guest_path.find_last_of('/');
            abr::sampler::AddModule(sl == std::string::npos ? guest_path : guest_path.substr(sl + 1), 0,
                                    g_mod->span_hi);
        }
    } else {
        // Each library at its own 16 MiB-aligned bias above the NULL guard,
        // dependencies first (the listed order).
        std::uint32_t at = 0x1000000u;
        for (const GuestLib& gl : multi) {
            if (!loader.Load(gl.path.c_str(), *g_mod, err, at)) {
                Log("FATAL: load %s failed: %s", gl.name.c_str(), err.c_str());
                return false;
            }
            const std::uint32_t hi = g_mod->more.empty() ? g_mod->span_hi : g_mod->more.back().span_hi;
            Log("guest %s at %08x", gl.name.c_str(), at);
            abr::sampler::AddModule(gl.name, at, hi);
            at = (hi + 0xFFFFFFu) & ~0xFFFFFFu;
        }
    }
    Log("guest: %zu imports %zu init halt=%08x once=%08x", g_mod->imports.size(),
        g_mod->init_array.size(), g_mod->halt_va, g_mod->once_va);
    CheckGuestBuildId(*g_mod, guest_identity_ok);
    // Trampoline map (first 48): lets any future death-pc name its import
    // offline. Slots: 0 = halt (swi 0), 1 = once return (swi kSwiOnce), then
    // import idx at slot idx+2 (swi idx+1) -- so name it by the va printed
    // here, not by slot arithmetic: idx = slot - 2.
    for (std::size_t ti = 0; ti < g_mod->imports.size() && ti < 48; ti++) {
        Log("tramp #%02zu va=%08x %s", ti, g_mod->imports[ti].tramp_va,
            g_mod->imports[ti].name.c_str());
    }
    g_rt = new Runtime(*g_mem, *g_mod);
    g_rt->AddModuleSegments(*g_mod);
    {
        std::random_device rd;
        const std::uint32_t canary = (static_cast<std::uint32_t>(rd()) << 1) | 1u;
        std::memcpy(g_mem->host(abr::kGuestDataBase + 0), &canary, 4);
    }
    g_rt->InitDataImports();
    // After relocation the guest's PT_GNU_RELRO is read-only in a real process;
    // refuse guest writes there so a wild write cannot zero a GOT slot (and be
    // named when it happens) instead of turning a later PLT call into pc = 0.
    g_rt->ProtectRelro(*g_mod);
    g_rt->SetFilesDir(GuestFilesDir(), g_files);
    g_rt->SetApkPath(g_apk);
    // Network policy (R-005): the packaging writes "offline" or "online" from
    // profile.toml into <guestlib dir>/network. Anything else, or no file:
    // offline, the safe side.
    {
        const std::size_t slash = guest_entry.find_last_of('/');
        const std::string dir =
            slash == std::string::npos ? std::string() : guest_entry.substr(0, slash + 1);
        const std::string entry = dir + "network";
        const std::string path = g_files + "/network";
        char word[16] = {0};
        if (ZipExtractStored(g_apk, entry, path)) {
            if (FILE* f = std::fopen(path.c_str(), "rb")) {
                if (std::fscanf(f, "%15s", word) != 1) {
                    word[0] = 0;
                }
                std::fclose(f);
            }
        }
        const bool online = std::strcmp(word, "online") == 0;
        g_rt->SetNetworkAllowed(online);
        Log("network: %s (packaging: %s)", online ? "online" : "offline", word[0] ? word : "none");
        // The original app's targetSdkVersion, packaged next to it: the libc
        // behaviour the guest was built for. No file: 0, the current one.
        const std::string sdk_path = g_files + "/target_sdk";
        int sdk = 0;
        if (ZipExtractStored(g_apk, dir + "target_sdk", sdk_path)) {
            if (FILE* f = std::fopen(sdk_path.c_str(), "rb")) {
                if (std::fscanf(f, "%d", &sdk) != 1) {
                    sdk = 0;
                }
                std::fclose(f);
            }
        }
        g_rt->SetGuestTargetSdk(sdk);
        Log("guest target sdk: %d", sdk);
        // Shared code cache (host.jitshare): "off" for a title whose guest
        // has a JIT of its own (Mono): its cacheflushes left stale code in
        // the shared cache (99 Bricks: Mono metadata asserts, 4 boots in 10;
        // none in 10 with private caches). abflags.txt still wins.
        const std::string js_path = g_files + "/jitshare";
        char js[16] = {0};
        if (ZipExtractStored(g_apk, dir + "jitshare", js_path)) {
            if (FILE* f = std::fopen(js_path.c_str(), "rb")) {
                if (std::fscanf(f, "%15s", js) != 1) {
                    js[0] = 0;
                }
                std::fclose(f);
            }
        }
        if (std::strcmp(js, "off") == 0) {
            setenv("HARNESS_NOJITSHARE", "1", 0);
        }
        // host.private_sdcard: shared storage is the app's "<files>/sdcard".
        const std::string sd_path = g_files + "/sdcard_mode";
        char sd[16] = {0};
        if (ZipExtractStored(g_apk, dir + "sdcard", sd_path)) {
            if (FILE* f = std::fopen(sd_path.c_str(), "rb")) {
                if (std::fscanf(f, "%15s", sd) != 1) {
                    sd[0] = 0;
                }
                std::fclose(f);
            }
        }
        g_rt->SetPrivateSdcard(std::strcmp(sd, "private") == 0);
        Log("shared storage: %s", std::strcmp(sd, "private") == 0 ? "private (files/sdcard)" : "none");
    }
    // Packed files (apk_vfs.h): assets/abport/ of base.apk and its splits.
    Log("packed files: %s", g_rt->InitPackedFiles().c_str());
    abr::prof::g_enabled = EnvOn("HARNESS_PROF");  // diagnostics: slow-frame breakdown
    // Every switch is final here (no Jit exists yet): the line states what runs.
    Log("flags: fastmem=%s mallocpad=%s zerofill=%s heapwatch=%s norelease=%s "
        "clampwrite=%s diag=%s jitopt=%s jitshare=%s tlsthunk=%s fastdispatch=%s hostfpsr=%s "
        "jitcallbacks=%s",
        JitFastmemOn() ? "on" : "off",
        std::getenv("HARNESS_MALLOCPAD") ? std::getenv("HARNESS_MALLOCPAD") : "0",
        std::getenv("HARNESS_ZEROFILL") ? std::getenv("HARNESS_ZEROFILL") : "off",
        std::getenv("HARNESS_HEAPWATCH") ? "on" : "off",
        std::getenv("HARNESS_NORELEASE") ? "on" : "off",
        ClampWriteOn() ? "on" : "off(+jni bound)",
        diag ? "on" : "off",
        JitSafeOpts() ? "safe" : "none", JitShareOn() ? "on" : "off",
        TlsThunkOn() ? "on" : "off", FastDispatchOn() ? "on" : "off",
        JitUnsafeOptimizations() ? "on" : "off", JitCallbacksOn() ? "on" : "off");
    abr::adpf::Init(RealEnvForThread);  // after abflags: HARNESS_NOADPF
    static GuestCallbacks callbacks(g_rt);
    g_cb = &callbacks;
    // Sampling profiler (HARNESS_SAMPLE or debug.abport.sample): its
    // resolver is a Jit on the shared code cache that never runs.
    abr::sampler::Init();
    if (abr::sampler::On()) {
        abr::sampler::SetResolver(NewThreadJit(0));
    }
    // Host callbacks into guest code (OpenSL ES buffer queues and the like).
    abr::Runtime::g_call_guest_hook = CallGuestFromHost;
    g_rt->set_callbacks(&callbacks);
    auto st = Enter();  // OnLoad thread state (+ real attach)
    if (!st) {
        Log("FATAL: no thread state for the OnLoad thread");
        return false;
    }
    g_rt->set_jit(st->jit);
    // Static init: every DT_INIT_ARRAY entry runs, in order. (Rio's #22 used
    // to be "parked" as dead-by-construction because it calls 0xFFFF0FA0;
    // that is the ARM kuser __kuser_memory_barrier, which the host now maps,
    // and the initializer runs clean: harness halts 53 -> 54.)
    std::size_t ok = 0;
    if (!multi.empty()) {
        // As the linker: each library Java loads (and the guest libraries
        // it needs) now, in Java's order; the others when the guest dlopens
        // them.
        for (const int lib : g_java_libs.empty() ? std::vector<int>{g_primary} : g_java_libs) {
            if (!g_rt->InitGuestLibrary(lib)) {
                Log("LOUD guest library #%d initialisers failed", lib);
            }
        }
    }
    for (std::size_t k = 0; multi.empty() && k < g_mod->init_array.size(); k++) {
        if (g_mod->init_array[k] == 0) {
            // A NULL slot of the guest's DT_INIT_ARRAY: kept in the vector so
            // the indices match the guest's, but there is nothing to run.
            continue;
        }
        auto& regs = st->jit->Regs();
        regs.fill(0);
        regs[13] = st->stack + kThreadStackSize - 8;
        regs[14] = g_mod->halt_va;
        CallGuest(*st->jit, g_mod->init_array[k]);
        const Dynarmic::HaltReason hr = RunJit(*st->jit);
        st->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        if (hr == Dynarmic::HaltReason::UserDefined1) {
            ok++;
        } else {
            Log("[init] #%zu @%08x hr=%d", k, g_mod->init_array[k], (int)hr);
        }
    }
    if (multi.empty()) {
        Log("init done: %zu/%zu", ok, g_mod->init_array.size());
        g_rt->MarkGuestLibraryInitialised(0);
    }
    // Run the guest JNI_OnLoad (registers dynamic natives incl. Lua pump).
    booted = 1;
    return true;
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    if (!Boot(vm)) {
        return 0;
    }
    {
        // The app's class loader (this Java thread's context loader), for
        // threads attached from native code, whose FindClass sees only the
        // system classes.
        JNIEnv* env = nullptr;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
            jclass tc = env->FindClass("java/lang/Thread");
            jmethodID cur = tc ? env->GetStaticMethodID(tc, "currentThread", "()Ljava/lang/Thread;") : nullptr;
            jmethodID gcl = tc ? env->GetMethodID(tc, "getContextClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
            jobject th = cur ? env->CallStaticObjectMethod(tc, cur) : nullptr;
            jobject cl = (th && gcl) ? env->CallObjectMethod(th, gcl) : nullptr;
            if (cl) {
                g_rt->SetAppClassLoader(env->NewGlobalRef(cl));
            }
            env->ExceptionClear();
        }
    }
    auto st = Enter();
    if (!st) {
        return 0;
    }
    // Each library Java loads runs its JNI_OnLoad, in Java's order.
    for (const int lib : g_java_libs.empty() ? std::vector<int>{g_primary} : g_java_libs) {
        const auto& defs = g_mod->lib_defined[static_cast<std::size_t>(lib)];
        const auto it = defs.find("JNI_OnLoad");
        if (it != defs.end()) {
            auto& regs = st->jit->Regs();
            regs.fill(0);
            regs[13] = st->stack + kThreadStackSize - 8;
            regs[14] = g_mod->halt_va;
            // Guest OnLoad expects a JavaVM* (mock handle; real pointers
            // are 64-bit and must never enter 32-bit guest regs). Our JVM
            // trampolines (fn 6/4) serve GetEnv/Attach off it.
            regs[0] = g_rt->Jvm();
            regs[1] = 0;
            CallGuest(*st->jit, it->second);
            const Dynarmic::HaltReason hr = RunJit(*st->jit);
            st->jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
            Log("guest OnLoad #%d hr=%d r0=%08x", lib, (int)hr, regs[0]);
        } else if (g_java_libs.empty()) {
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

// --- NativeActivity (a Unity 4 title): the framework loads our library as
// the activity's native code and calls ANativeActivity_onCreate. The guest's
// own ANativeActivity_onCreate gets a guest ANativeActivity (LP32, 40 bytes:
// callbacks, vm, env, clazz, internalDataPath, externalDataPath, sdkVersion,
// instance, assetManager, obbPath) with a 16-entry callbacks table it fills
// in; the real activity's callbacks are ours and call the guest's entry of
// the same slot, if it set one (read at call time: a guest may change them).
namespace {
std::uint32_t g_gact = 0;  // the guest ANativeActivity

enum GuestCb {
    kOnStart, kOnResume, kOnSaveInstanceState, kOnPause, kOnStop, kOnDestroy,
    kOnWindowFocusChanged, kOnNativeWindowCreated, kOnNativeWindowResized,
    kOnNativeWindowRedrawNeeded, kOnNativeWindowDestroyed, kOnInputQueueCreated,
    kOnInputQueueDestroyed, kOnContentRectChanged, kOnConfigurationChanged, kOnLowMemory,
};

std::uint64_t CallGuestCb(int slot, const char* name, std::vector<std::uint32_t> args) {
    std::uint32_t table = 0, fn = 0;
    if (!g_gact || !g_rt->CheckRange(g_gact, 4, false)) {
        return 0;
    }
    std::memcpy(&table, g_mem->host(g_gact), 4);
    if (!table || !g_rt->CheckRange(table + slot * 4u, 4, false)) {
        return 0;
    }
    std::memcpy(&fn, g_mem->host(table + slot * 4u), 4);
    if (!fn) {
        return 0;
    }
    args.insert(args.begin(), g_gact);
    return CallGuestFromHost(name, fn, args);
}

void CbStart(ANativeActivity*) { CallGuestCb(kOnStart, "onStart", {}); }
void CbResume(ANativeActivity*) { CallGuestCb(kOnResume, "onResume", {}); }
void CbPause(ANativeActivity*) { CallGuestCb(kOnPause, "onPause", {}); }
void CbStop(ANativeActivity*) { CallGuestCb(kOnStop, "onStop", {}); }
void CbDestroy(ANativeActivity*) { CallGuestCb(kOnDestroy, "onDestroy", {}); }
void CbConfigurationChanged(ANativeActivity*) {
    CallGuestCb(kOnConfigurationChanged, "onConfigurationChanged", {});
}
void CbLowMemory(ANativeActivity*) { CallGuestCb(kOnLowMemory, "onLowMemory", {}); }
void CbWindowFocusChanged(ANativeActivity*, int focus) {
    CallGuestCb(kOnWindowFocusChanged, "onWindowFocusChanged", {static_cast<std::uint32_t>(focus)});
}
void CbWindowCreated(ANativeActivity*, ANativeWindow* w) {
    CallGuestCb(kOnNativeWindowCreated, "onNativeWindowCreated", {g_rt->PtrHandle(w)});
}
void CbWindowResized(ANativeActivity*, ANativeWindow* w) {
    CallGuestCb(kOnNativeWindowResized, "onNativeWindowResized", {g_rt->PtrHandle(w)});
}
void CbWindowRedrawNeeded(ANativeActivity*, ANativeWindow* w) {
    CallGuestCb(kOnNativeWindowRedrawNeeded, "onNativeWindowRedrawNeeded", {g_rt->PtrHandle(w)});
}
void CbWindowDestroyed(ANativeActivity*, ANativeWindow* w) {
    CallGuestCb(kOnNativeWindowDestroyed, "onNativeWindowDestroyed", {g_rt->PtrHandle(w)});
}
void CbInputQueueCreated(ANativeActivity*, AInputQueue* q) {
    CallGuestCb(kOnInputQueueCreated, "onInputQueueCreated", {g_rt->PtrHandle(q)});
}
void CbInputQueueDestroyed(ANativeActivity*, AInputQueue* q) {
    const std::uint32_t h = g_rt->PtrHandle(q);
    CallGuestCb(kOnInputQueueDestroyed, "onInputQueueDestroyed", {h});
    g_rt->DropHandle(h);
}
void CbContentRectChanged(ANativeActivity*, const ARect* r) {
    // A guest copy of the rect (4 int32), valid for the call.
    const std::uint32_t g = g_rt->GuestMalloc(16);
    if (!g || !r) {
        return;
    }
    std::memcpy(g_mem->host(g), r, 16);
    CallGuestCb(kOnContentRectChanged, "onContentRectChanged", {g});
    g_rt->GuestFree(g);
}
void* CbSaveInstanceState(ANativeActivity*, size_t* out_size) {
    // The guest returns a malloc'd buffer and its size; the framework frees
    // what we return with free(), so it gets a host copy.
    *out_size = 0;
    const std::uint32_t gsize = g_rt->GuestMalloc(4);
    if (!gsize) {
        return nullptr;
    }
    std::memset(g_mem->host(gsize), 0, 4);
    const std::uint32_t buf =
        static_cast<std::uint32_t>(CallGuestCb(kOnSaveInstanceState, "onSaveInstanceState", {gsize}));
    std::uint32_t size = 0;
    std::memcpy(&size, g_mem->host(gsize), 4);
    g_rt->GuestFree(gsize);
    void* out = nullptr;
    if (buf && size && g_rt->CheckRange(buf, size, false)) {
        out = std::malloc(size);
        if (out) {
            std::memcpy(out, g_mem->host(buf), size);
            *out_size = size;
        }
    }
    if (buf) {
        g_rt->GuestFree(buf);
    }
    return out;
}

std::uint32_t GuestCString(const char* t) {
    if (!t) {
        return 0;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(t) + 1);
    const std::uint32_t g = g_rt->GuestMalloc(n);
    if (g) {
        std::memcpy(g_mem->host(g), t, n);
    }
    return g;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(
    ANativeActivity* activity, void* saved_state, size_t saved_size) {
    if (!Boot(activity->vm)) {
        Log("FATAL: ANativeActivity_onCreate: the guest cannot run");
        return;
    }
    const auto& defs = g_mod->lib_defined[static_cast<std::size_t>(g_primary)];
    const auto it = defs.find("ANativeActivity_onCreate");
    if (it == defs.end()) {
        Log("LOUD ANativeActivity_onCreate: the guest library has none");
        return;
    }
    const std::uint32_t gact = g_rt->GuestMalloc(40);
    const std::uint32_t table = g_rt->GuestMalloc(64);
    if (!gact || !table) {
        Log("LOUD ANativeActivity_onCreate: no guest heap");
        return;
    }
    std::memset(g_mem->host(table), 0, 64);
    const std::uint32_t mgr = g_rt->NewGuestAssetManager();
    g_rt->SetRealAssetManager(mgr, activity->assetManager);
    const std::uint32_t f[10] = {
        table,
        g_rt->Jvm(),
        g_rt->JniEnv(),
        g_rt->WrapObject(activity->clazz, true),  // the activity's, for its life
        GuestCString(activity->internalDataPath),
        GuestCString(activity->externalDataPath),
        static_cast<std::uint32_t>(activity->sdkVersion),
        0,  // instance: the guest's own
        mgr,
        GuestCString(activity->obbPath),
    };
    std::memcpy(g_mem->host(gact), f, 40);
    g_gact = gact;
    std::uint32_t gsaved = 0;
    if (saved_state && saved_size) {
        gsaved = g_rt->GuestMalloc(static_cast<std::uint32_t>(saved_size));
        if (gsaved) {
            std::memcpy(g_mem->host(gsaved), saved_state, saved_size);
        }
    }
    Log("ANativeActivity_onCreate: guest activity %08x (sdk %d, saved %zu bytes)", gact,
        activity->sdkVersion, saved_size);
    CallGuestFromHost("ANativeActivity_onCreate", it->second,
                      {gact, gsaved, static_cast<std::uint32_t>(gsaved ? saved_size : 0)});
    ANativeActivityCallbacks* cb = activity->callbacks;
    cb->onStart = CbStart;
    cb->onResume = CbResume;
    cb->onSaveInstanceState = CbSaveInstanceState;
    cb->onPause = CbPause;
    cb->onStop = CbStop;
    cb->onDestroy = CbDestroy;
    cb->onWindowFocusChanged = CbWindowFocusChanged;
    cb->onNativeWindowCreated = CbWindowCreated;
    cb->onNativeWindowResized = CbWindowResized;
    cb->onNativeWindowRedrawNeeded = CbWindowRedrawNeeded;
    cb->onNativeWindowDestroyed = CbWindowDestroyed;
    cb->onInputQueueCreated = CbInputQueueCreated;
    cb->onInputQueueDestroyed = CbInputQueueDestroyed;
    cb->onContentRectChanged = CbContentRectChanged;
    cb->onConfigurationChanged = CbConfigurationChanged;
    cb->onLowMemory = CbLowMemory;
}

#include "apphost_gen.inc"
}  // namespace abr
