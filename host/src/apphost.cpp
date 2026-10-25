// apphost.cpp — Android product host (online-first milestone).
// Real Java drives: 63 Java_* forwarders (apphost_gen.inc) run guest code
// under Dynarmic with the REAL JNIEnv* (indices match NDK order, verified).
// Import thunks (libc/GLES/sockets/files/threads) are the proven harness
// ones; mocks shrink to era-tuned values only. Guest .so loads as DATA
// (mini-zip extract from our own base.apk), never via System.loadLibrary.
#include <jni.h>

#include <android/log.h>
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
#include <vector>

#include "apphost.h"
#include "appid.h"
#include "jni_call_words.h"
#include "guest/dispatch.h"
#include "guest/elf_loader.h"
#include "guest/layout.h"
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
// the stack is safe; an in-flight shared_ptr (AppHost_CallImpl) keeps the
// HostThread itself alive on its own.
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
    delete st->jit;
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
// Update thread's JIT (for stuck-PC reads) + its host tid.
std::atomic<Dynarmic::A32::Jit*> g_upd_jit{nullptr};
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
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = g_cb;
    cfg.optimizations =
        JitSafeOpts() ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    cfg.enable_cycle_counting = kJitCycleCounting;
    if (std::getenv("HARNESS_FASTMEM")) {
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(g_mem->base());
    }
    cfg.global_monitor = &g_rt->Monitor();  // one monitor for every Jit
    cfg.processor_id = st->cpu_id;
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
    // One line per real thread (bounded -- not per call): the only witness that
    // Java->native thread churn would show. Always on.
    Log("thread enter tid=%u stack=%08x attached=%d live=%zu (created=%u)", (unsigned)tid, st->stack,
        env ? 1 : 0, live, created);
    return st;
}

std::uint64_t AppHost_CallImpl(const char* name, const char* sig, void* env, void* thiz, va_list ap) {
    auto st = Enter();
    if (!st) {
        return 1;  // same as a dropped re-entrant frame: logged LOUD in Enter
    }
    if (st->in_guest.load()) {
        Log("REENTER %s while %s active -- dropping frame", name, st->in_name);
        return 1;
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
                    fr[13] = st->stack + kThreadStackSize - 0x100u;
                    fr[14] = g_mod->halt_va;
                    fr[0] = cx;
                    CallGuest(*st->jit, 0x5FE70u);
                    Dynarmic::HaltReason fhr;
                    {
                        GuestRunGuard gg(st.get(), name);
                        fhr = st->jit->Run();
                    }
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
    for (const char* p = sig; *p && *p != ')'; p++) {
        if (*p == '(') {
            continue;
        }
        if (*p == 'J') {
            const jlong v = va_arg(ap, jlong);
            AppendJniWord64(words, static_cast<std::uint64_t>(v));
        } else if (*p == 'F') {
            const double d = va_arg(ap, double);
            const float f = (float)d;
            std::uint32_t w = 0;
            std::memcpy(&w, &f, 4);
            words.push_back(w);
        } else if (*p == 'D') {
            const double d = va_arg(ap, double);
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            AppendJniWord64(words, u);
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
    const std::uint32_t spbase = st->stack + kThreadStackSize - 0x100u;
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
        // 2026-10-19 (PLAN item 6): the JNI resolve below exists only to give
        // the synthetic driver a genuine MyInputHandler mock. The driver is off
        // unless HARNESS_TAPBEAT is set, so in prod this did 2 NewGlobalRef +
        // GetObjectClass/GetFieldID/GetObjectField on the first update for
        // nothing (and printed `[tapbeat] handler mock=`). Gated on beat_on.
        if (beat_on && lastw.empty() && std::strstr(name, "nativeUpdate")) {
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
                Dynarmic::HaltReason bhr;
                {
                    GuestRunGuard gg(st.get(), name);
                    bhr = st->jit->Run();
                }
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
    Dynarmic::HaltReason hr;
    {
        GuestRunGuard gg(st.get(), name);
        hr = st->jit->Run();
    }
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

extern "C" jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    GuestCallbacks::g_real_env_hook = RealEnvForThread;
    // Logs to app files dir (pullable); stdout unbuffered.
    g_files = GuestFilesDir();
    std::error_code ec;
    std::filesystem::create_directories(g_files, ec);
    const std::string logpath = g_files + "/apphost.log";
    std::freopen(logpath.c_str(), "w", stdout);
    std::freopen(logpath.c_str(), "a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    // The zip module logs through the app's logger (it lives in its own
    // module so the unit test can reach it).
    abr::ZipSetLog(&Log);
    InstallDeathWitness(g_files);  // SIGSEGV/SIGABRT/SIGBUS witness (debuggerd is SELinux-muted here)
    std::set_terminate(TerminateWitness);  // name escaping host exceptions instead of silent abort
    Log("OnLoad: product host starting");
    Log("build marker %s %s", __DATE__, __TIME__);
    // RETSUCC was re-armed unconditionally here (2026-10-14, "real fail path
    // stalls on Fox"), but it never had a shipping effect: the injection is a
    // PROBE3 hook, and PROBE3 is only armed under HARNESS_DIAG. Measured on the
    // current baseline, the raiser it targets (guest VA 0x28a3dc) is never even
    // translated -- no `[probe-emit] ... 0028a3dc` and no `[succ]` line in any
    // diag run. So the crutch is inert; the lever is gone and the experiment
    // stays opt-in (put HARNESS_RETSUCC=1 into files/abflags.txt with diag on).
    // ---- diagnostic layer: ONE switch, OFF for shipping (2026-10-17) -------
    // HARNESS_DIAG=1 (real environment OR files/abflags.txt) re-arms the pc
    // probes, the WATCHPC hook, the per-call/per-frame tracing and the
    // hot-path logs; with it off that whole layer is skipped (no logs, no
    // per-call cost). PROBE(fill), FASTMEM and REALGL are
    // NOT diagnostics -- they change behaviour -- and stay unconditional.
    // See PLAN.md, batches A/B.
    const bool diag = DiagBootWanted(g_files);
    if (diag) {
        setenv("HARNESS_DIAG", "1", 1);
        setenv("HARNESS_PROBE2", "457f90", 1);
        setenv("HARNESS_PROBE3", "28a3dc", 1);
        setenv("HARNESS_PROBE_N", "500", 1);
        setenv("HARNESS_RDUMP", "1", 1);
    }
    // Perf A/B (behavior-neutral infra): fastmem first, SAFEOPTS after.
    // Metric = updates/min + same-behavior verdicts (sessions/rdump).
    // 2026-10-14: fastmem back ON (proven neutral; no-fastmem boot is
    // ~10x slower). WPA tick PROVEN, off (log bloat).
    setenv("HARNESS_FASTMEM", "1", 1);
    // 2026-10-16 audit: the "HUNT" block that used to sit here left this
    // FASTMEM line ON (its own commented copy never took effect), so the
    // heapwatch "hunt" was blind (no store callbacks under fastmem) and its
    // verdict was void. MALLOCPAD=32 also survived here against the Pad-OFF
    // verdict. Both removed. Experiments now go through files/abflags.txt
    // (applied after these defaults, logged as overrides) => no rebuild, and
    // the log always states what was actually on.
    // TRIAGE done (2026-10-16, AFK autopsy): NORELEASE retired -- quarantine
    // releases normally again (delay-only, cap 64). NORELEASE leaked to 947MB
    // -> thread-stack OOM -> wild thread -> SIGSEGV. Silent deaths were never
    // in mspace_free. Switch stays as dormant diag code only.
    // setenv("HARNESS_NORELEASE", "1", 1);
    // PROBE verdict (Fox layout test, 2026-10-16): pad CONFIRMED the cost
    // model, not the theory — fresh-boot OOM at brk=top via
    // __cxa_eh_globals alloc failure (pad 256B x ~4M boot allocs). Fox
    // frequency on pad: inconclusive (too few runs before OOM wall).
    // Pad OFF; layout question moves to the dlmalloc port instead.
    // setenv("HARNESS_MALLOCPAD", "256", 1);
    // TRIAGE stash stays (governor reads last_mgr164_).
    if (diag) setenv("HARNESS_WATCHPC", "250ab8", 1);
    // setenv("HARNESS_WPA", "672ea0", 1);  // tick PROVEN, off
    // Synthesis: DISABLED (POKE forces early body with unbuilt content
    // -> body returns 0 (quit) -> orderly suicide in seconds, §97.
    // Gate opens naturally only when content is ready; forcing it kills.
    // setenv("HARNESS_POKE1187", "1", 1);
    // r0=1-source hunt: probe the +912 quit-poller vcall site (c52ac)
    // -> per-hit r0 dump (ProbeDump). Slot PROBE4 (PROBE/PROBE3 taken).
    if (diag) setenv("HARNESS_PROBE4", "c52ac", 1);
    // (Removed 2026-10-19: HARNESS_SPURWAKE=1 capped cycles as spurious
    // success for untimed cond_wait. The cap is gone and the wait now
    // blocks like bionic; measured on device that behaviour is correct --
    // see PLAN.md backlog item 2.)
    // Stuck-update forensics (2026-10-14): per-thread last import is
    // always tracked now (watchdog resolves the name at dump time).
    if (diag) setenv("HARNESS_LASTSVC", "1", 1);
    // Worker import traffic for the tid=11 death forensics (one
    // diagnostic run; THREADLOG is capped at 5/import so log stays sane).
    if (diag) setenv("HARNESS_THREADLOG", "1", 1);
    // Victim-object provenance for the Fox BAD death (logging only,
    // capped; zero behavior change): first mallocs with caller lr,
    // match the crashed r4 against them from the log.
    // PERF VARIANT (uncommitted test): MALLOCLOG off for feel comparison.
    // setenv("HARNESS_MALLOCLOG", "1", 1);
    // +164 ground truth on device (read-only manager-pass entry dump;
    // closes "logically LOADING" -> proven or not).
    if (diag) setenv("HARNESS_DIAG164", "1", 1);
    // Synthetic input heartbeat: ENABLED hands-off test (2026-10-14).
    // Prior disable was for finger-interleave chaos; solo (no finger)
    // it is the only event source. Tick jumps on taps PROVEN => this
    // should pump quanta hands-off. Binary: tick advances? menu?
    // Crutch OFF again (2026-10-14): isolated test tapbeat-off +
    // RETSUCC-on (last time both were off - confounded). If it
    // self-drives, taps are unnecessary.
    // setenv("HARNESS_TAPBEAT", "1", 1);
    // Fail-functor synthesis timing: DISABLED (2026-10-14). Correlation:
    // boots WITH functor fire (14932 died, 35844 black-after-loading)
    // vs boot WITHOUT (15265 menu). Suspect: forged login-failed state
    // suppresses menu construction downstream. Binary test: off -> menu?
    // setenv("HARNESS_FUNCTORAT", "150", 1);
    // Real GLES on device (shared Handle; caller's context, no EGL init).
    // PC parity verified post-split (54/54, 948). First test: live ctx?
    // GL errors? survival? (pixels need content+burst; LOADING art first.)
    setenv("HARNESS_REALGL", "1", 1);
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
    Log("flags: fastmem=%s mallocpad=%s zerofill=%s heapwatch=%s norelease=%s clampwrite=%s diag=%s "
        "jitopt=%s",
        std::getenv("HARNESS_FASTMEM") ? "on" : "off",
        std::getenv("HARNESS_MALLOCPAD") ? std::getenv("HARNESS_MALLOCPAD") : "0",
        std::getenv("HARNESS_ZEROFILL") ? std::getenv("HARNESS_ZEROFILL") : "off",
        std::getenv("HARNESS_HEAPWATCH") ? "on" : "off",
        std::getenv("HARNESS_NORELEASE") ? "on" : "off",
        ClampWriteOn() ? "on" : "off(+jni bound)",
        diag ? "on" : "off",
        JitSafeOpts() ? "safe" : "none");
    g_apk = FindApk();
    if (g_apk.empty()) {
        Log("FATAL: own base.apk not found");
        return 0;
    }
    Log("apk=%s", g_apk.c_str());
    // The guest .so is whatever sits under assets/guestlib/ (the packaging step
    // puts exactly one there). Naming it in C would be a per-title constant.
    const std::string glib_dir = "assets/guestlib/";
    std::string guest_entry;
    for (const std::string& e : ZipListPrefix(g_apk, glib_dir)) {
        if (e.size() > 3 && e.compare(e.size() - 3, 3, ".so") == 0) {
            if (!guest_entry.empty()) {
                Log("FATAL: several guest .so under %s: %s, %s", glib_dir.c_str(),
                    guest_entry.c_str(), e.c_str());
                return 0;
            }
            guest_entry = e;
        }
    }
    if (guest_entry.empty()) {
        Log("FATAL: no guest .so under %s", glib_dir.c_str());
        return 0;
    }
    Log("guest entry %s", guest_entry.c_str());
    const std::string guest_path = g_files + "/guest.so";
    if (!ZipExtractStored(g_apk, guest_entry, guest_path)) {
        Log("FATAL: guest extract failed");
        return 0;
    }
    // Guest identity: the packaging step wrote the guest .so's SHA-256 next
    // to it inside the APK. Comparing it with the extracted image makes the
    // identity of the running guest a RUNTIME fact. The GNU build-id note is
    // missing from some titles' images (Seasons, Space), so for them this is
    // the only identity check there is; the build-id note stays as a second,
    // independent signal where the image has one.
    bool guest_identity_ok = false;
    {
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
                return 0;
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
        return 0;
    }
    ElfLoader loader(*g_mem);
    g_mod = new LoadedModule();
    std::string err;
    if (!loader.Load(guest_path.c_str(), *g_mod, err)) {
        Log("FATAL: load failed: %s", err.c_str());
        return 0;
    }
    Log("guest: %zu imports %zu init halt=%08x once=%08x", g_mod->imports.size(),
        g_mod->init_array.size(), g_mod->halt_va, g_mod->once_va);
    CheckGuestBuildId(*g_mod, guest_identity_ok);
    // The pinned-image JSON SSO fill ("{}" at guest VA 1a410c) used to be
    // armed here as HARNESS_PROBE=1a410c,fill. Measured on 2026-10-19 (lib
    // 14c43eda, Rio prod 75 s): the probe fires, but its own verdict is
    // `[probe] OUT non-empty, kept` -- the slot is already filled by the guest,
    // so the write never happens; the only effect was 16 log lines. Removed.
    // The ',fill' code stays as an opt-in experiment (HARNESS_PROBE=...,fill).
    // Trampoline map (first 48): lets any future death-pc name its import
    // offline. Slots: 0 = halt (swi 0), 1 = once return (swi kSwiOnce), then
    // import idx at slot idx+2 (swi idx+1) -- so name it by the va printed
    // here, not by slot arithmetic: idx = slot - 2.
    for (std::size_t ti = 0; ti < g_mod->imports.size() && ti < 48; ti++) {
        Log("tramp #%02zu va=%08x %s", ti, g_mod->imports[ti].tramp_va,
            g_mod->imports[ti].name.c_str());
    }
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
    // After relocation the guest's PT_GNU_RELRO is read-only in a real process;
    // refuse guest writes there so a wild write cannot zero a GOT slot (and be
    // named when it happens) instead of turning a later PLT call into pc = 0.
    g_rt->AddWriteGuard(g_mod->relro_start, g_mod->relro_size, "RELRO");
    g_rt->SetFilesDir(GuestFilesDir(), g_files);
    g_rt->SetApkPath(g_apk);
    static GuestCallbacks callbacks(g_rt);
    g_cb = &callbacks;
    g_rt->set_callbacks(&callbacks);
    auto st = Enter();  // OnLoad thread state (+ real attach)
    if (!st) {
        Log("FATAL: no thread state for the OnLoad thread");
        return 0;
    }
    g_rt->set_jit(st->jit);
    // Static init: every DT_INIT_ARRAY entry runs, in order. (Rio's #22 used
    // to be "parked" as dead-by-construction because it calls 0xFFFF0FA0;
    // that is the ARM kuser __kuser_memory_barrier, which the host now maps,
    // and the initializer runs clean: harness halts 53 -> 54.)
    std::size_t ok = 0;
    for (std::size_t k = 0; k < g_mod->init_array.size(); k++) {
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
            regs[13] = st->stack + kThreadStackSize - 8;
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
