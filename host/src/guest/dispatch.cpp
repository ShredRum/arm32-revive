#include "dispatch.h"
#include "sampler.h"

#ifdef __ANDROID__
#include <jni.h>  // real-Java delegation (product host only)
#endif

#include "guest/bsd_random.h"
#include "guest/prof.h"
#include "guest/sha1.h"
#include "realgl.h"

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_types.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <EGL/egl.h>
#include <netdb.h>
#include <poll.h>
#ifdef __ANDROID__
#include <android/configuration.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_window_jni.h>
#include <android/sensor.h>
#endif
#include <sys/epoll.h>
#include <pwd.h>
#include <grp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <set>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/vfs.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <fnmatch.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <utime.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

// libabhost load base, captured once at init (dladdr must NOT be called from
// the corruption hook: mid-abort, mutex held). LibBaseAnchor() only exists as
// a dladdr probe address.
static std::uintptr_t g_lib_base = 0;
static void LibBaseAnchor() {}

#include <cstring>

namespace abr {

// The per-thread state every guest->host call reads, in one thread_local so
// that a call reaches it with one TLS lookup (each thread_local is a call to
// the TLS resolver in a dlopened library: profiled on a phone, 2026-11-08).
// CallSVC takes a reference once; the rest read single fields as before.
struct HotThread {
    // The Jit running guest code on this thread (null on the main thread:
    // jit_) and the guest thread id (0 there: 1).
    Dynarmic::A32::Jit* active_jit = nullptr;
    std::uint32_t tid = 0;
    // Set while a proxy delivers signals for another thread (see t_sig).
    bool is_proxy = false;
    // Guest->host calls of this thread (Runtime::ThreadSvcCount).
    std::uint64_t svc_count = 0;
};
thread_local HotThread t_hot;

// This thread's pthread_cleanup list head (guest address) and guest stack.
thread_local std::uint32_t t_cleanup_head = 0;
// Direct buffers this thread handed to the guest during the current call.
thread_local std::vector<void*> t_direct_used;
// pthread_exit was called: the worker's Run stopped with kThreadExitHalt.
thread_local bool t_thread_exiting = false;
thread_local std::uint32_t t_exit_value = 0;
// A pthread_create worker's own Jit (null on every other thread). Unlike
// t_hot.active_jit it does not change while a callback Jit runs.
thread_local Dynarmic::A32::Jit* t_worker_jit = nullptr;
thread_local std::uint32_t t_stack_lo = 0;
thread_local std::uint32_t t_stack_size = 0;
// RunJit nesting on this thread (a callback Jit inside a host call adds one).
thread_local int t_jit_depth = 0;
// Set while a proxy delivers: the interrupted registers of the thread it
// stands in for (DeliverPendingSignals uses them instead of the active Jit's).
// Only the proxy's first delivery uses them: once a handler runs, a nested
// delivery (a signal taken inside the handler's own wait) goes below the
// handler's frame, like on the thread itself. t_hot.is_proxy marks the thread.
thread_local const std::array<std::uint32_t, 16>* t_proxy_regs = nullptr;
thread_local std::uint32_t t_proxy_cpsr = 0x10;
// Local frames of this thread (Runtime::PushLocalFrame): the real handles
// made in each, released when it is popped; guest_frames = how many of them
// the guest pushed (PushLocalFrame) on top of the host's.
thread_local std::vector<std::vector<std::uint32_t>> t_local_frames;
thread_local int t_guest_frames = 0;
// Guest-signal state of this thread (RegisterSigThread) and its Runtime.
thread_local std::shared_ptr<Runtime::SigThread> t_sig;
thread_local Runtime* t_rt = nullptr;

extern "C" {
typedef void* dl_mspace;
dl_mspace create_mspace_with_base(void*, size_t, int);
void* mspace_malloc(dl_mspace, size_t);
void mspace_free(dl_mspace, void*);
void* mspace_realloc(dl_mspace, void*, size_t);
void* mspace_memalign(dl_mspace, size_t, size_t);
void* mspace_calloc(dl_mspace, size_t, size_t);
size_t mspace_usable_size(const void*);
}  // extern "C"



// HARNESS_TIMEWARP=<sec>: guest clock shift (defined here to avoid crossing
// goto bad_ptr initializations inside CallImport).
static std::int64_t TimeWarp() {
    static std::int64_t w = -2;
    if (w == -2) {
        w = 0;
        if (const char* e = std::getenv("HARNESS_TIMEWARP")) {
            w = (std::int64_t)std::strtoll(e, nullptr, 0);
        }
    }
    return w;
}

// Honest guest sleep (2026-10-19, PLAN item 8). bionic blocks the calling
// thread for the requested time; the old 5 ms clamp was a bring-up guard
// against delay loops hanging the emulator. Neither shipped guest sleeps on a
// normal frame path: Rio's nanosleep is a dead PLT entry (no call site), and
// Space's sleep_ms is reached only on fatal paths right before abort(). So this
// is fidelity, not a prod behaviour change.
static void GuestSleepUs(std::uint64_t us);
static void GuestSleepUsPlain(std::uint64_t us) {
    if (!us) {
        return;
    }
    struct timespec ts {};
    ts.tv_sec = static_cast<time_t>(us / 1000000u);
    ts.tv_nsec = static_cast<long>((us % 1000000u) * 1000u);
    while (::nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

// Cached env flag (perf): getenv() is a linear environ scan; per-malloc /
// per-free / per-cond-wait calls must not pay it. All HARNESS_* setenv happen
// once in the boot block before any guest runs (apphost.cpp) and nothing ever
// unsets them, so read-once caching is semantics-preserving.
//
// Lock-free and allocation-free (2026-10-17, PLAN.md B3): every call site
// passes a string LITERAL (19 distinct keys today), so the cache is a fixed
// table keyed by pointer identity -- no mutex, no std::string, no hashing on
// the hot path. A duplicate write under a race is benign (same key+value).
bool EnvOn(const char* name) {
    struct Entry {
        const char* key;
        char val;
    };
    static constexpr int kCap = 64;
    static Entry table[kCap];
    static std::atomic<int> used{0};
    if (!name) {
        return false;
    }
    const int n = used.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (table[i].key == name) {  // pointer identity: literals only
            return table[i].val != 0;
        }
    }
    const char v = std::getenv(name) != nullptr ? 1 : 0;
    if (n < kCap) {
        table[n].key = name;
        table[n].val = v;
        used.store(n + 1, std::memory_order_release);
        return v != 0;
    }
    return v != 0;  // safety valve: more than kCap distinct keys
}

// Explicit truthiness for the ONE diagnostic switch (unlike the presence-based
// EnvOn above, here "HARNESS_DIAG=0" must really mean off -- a dev writing that
// expects a quiet run). Read once: the boot block sets the env before any guest
// code runs, and nothing unsets it later.
bool DiagOn() {
    static const bool on = [] {
        const char* e = std::getenv("HARNESS_DIAG");
        if (!e || !e[0]) {
            return false;
        }
        if (std::strcmp(e, "-") == 0 || std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0 ||
            std::strcmp(e, "false") == 0) {
            return false;
        }
        return true;
    }();
    return on;
}

// Real GLES backend instance (created on first gl* import).
RealGL* Runtime::RealGLBackend() {
    if (!realgl_) {
        realgl_ = new RealGL(this);
    }
    return realgl_;
}

// TEMP-DIAG (registry ParseError): registry FILE* set + dump counter.
static std::set<FILE*> g_regfiles;
static int g_regdump_n = 0;

Runtime::Runtime(GuestMemory& mem, LoadedModule& mod) : mem_(mem), mod_(mod) {
    // Room for imports made at run time (RuntimeImport): the table must not
    // move while other threads index it.
    mod_.imports.reserve(mod_.imports.size() + 512);
    // HARNESS_HEAPBASE=hex: relocate heap start (address-sensitivity probe).
    if (const char* e = std::getenv("HARNESS_HEAPBASE")) {
        heap_base_ = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }
    // Heap + stacks + trampolines + tables + guest data are all committable.
    mem_.Map(heap_base_, kHeapSize, PROT_READ | PROT_WRITE);
    AddMapping(heap_base_, kHeapSize, PROT_READ | PROT_WRITE);
    mem_.Map(kStackBase, kStackSize, PROT_READ | PROT_WRITE);
    AddMapping(kStackBase, kStackSize, PROT_READ | PROT_WRITE);
    mem_.Map(kJniTableBase, kJniTableSize, PROT_READ | PROT_WRITE);
    AddMapping(kJniTableBase, kJniTableSize, PROT_READ | PROT_WRITE);
    mem_.Map(kGuestDataBase, kGuestDataSize, PROT_READ | PROT_WRITE);
    AddMapping(kGuestDataBase, kGuestDataSize, PROT_READ | PROT_WRITE);
    mem_.Map(kTlsBase, kTlsBlocks * kTlsBlockSize, PROT_READ | PROT_WRITE);
    AddMapping(kTlsBase, kTlsBlocks * kTlsBlockSize, PROT_READ | PROT_WRITE);
    // ARM32 kuser helper page: present before any guest instruction runs.
    InitUserHelpers();
    // Trampoline page is mapped by the loader; track it as RX here.
    AddMapping(kTrampBase, kTrampSize, PROT_READ | PROT_EXEC);
    // NOTE: InitDataImports() is called by the harness AFTER module segments
    // are AddMapping'd (it range-checks its writes).
    // JNIEnv function table: 256 slots -> loader-emitted `svc` trampolines
    // (even VAs = ARM state, matching blx-from-ARM call sites).
    jni_table_ = kJniTableBase;
    for (std::uint32_t i = 0; i < 256; i++) {
        const std::uint32_t tramp = mod_.jni_tramp_base + i * 4;
        std::memcpy(H(jni_table_ + i * 4), &tramp, 4);
    }
    // JNIEnv*: guest-data word holding the table VA; string arena after it.
    jni_env_ = kGuestDataBase + 0x2000;
    std::memcpy(H(jni_env_), &jni_table_, 4);
    jni_str_next_ = kJniStringStart;
    // JavaVM table: 8 slots -> loader-emitted trampolines (swi kSwiJniBase+0x100+i).
    // JNIInvokeInterface order incl. reserved0-2 (NDK r27 jni.h): 3 Destroy,
    // 4 AttachCurrentThread, 5 Detach, 6 GetEnv, 7 AttachAsDaemon.
    jvm_table_ = kJniTableBase + 0x1000;
    for (std::uint32_t i = 0; i < 8; i++) {
        const std::uint32_t tramp = mod_.jvm_tramp_base + i * 4;
        std::memcpy(H(jvm_table_ + i * 4), &tramp, 4);
    }
    jvm_ = kGuestDataBase + 0x2004;
    std::memcpy(H(jvm_), &jvm_table_, 4);
    jni_obj_next_ = kJniObjectStart;  // class/method handle arena
    // Guest heap: dlmalloc mspace over the fixed arena (no sbrk/mmap escape;
    // exhaustion returns 0 like before). Bionic-era layout (8B align, chunk
    // headers, LIFO reuse) instead of dense bump packing.
    {
        Dl_info di{};
        if (dladdr(reinterpret_cast<void*>(&LibBaseAnchor), &di) && di.dli_fbase) {
            g_lib_base = reinterpret_cast<std::uintptr_t>(di.dli_fbase);
        }
    }
    heap_ms_ = create_mspace_with_base(H(heap_base_), kHeapSize, 0);
    if (!heap_ms_) {
        std::printf("[heap] LOUD mspace creation failed -- halting\n");
        stop_step_ = true;
    }
}

void Runtime::AddMapping(std::uint32_t start, std::uint32_t size, int prot) {
    if (!mappings_.Add(start, start + size, prot)) {
        std::printf("[mmap] LOUD mapping table full (%zu entries), [%08x+%x) not recorded\n",
                    MappingTable::kCapacity, start, size);
    }
}

// RELRO page range as bionic protects it: start rounded down, end rounded up
// (all six guests end RELRO on a page boundary).
static void RelroPages(std::uint32_t start, std::uint32_t size, std::uint32_t* ps,
                       std::uint32_t* pe) {
    *ps = start & ~0xFFFu;
    *pe = static_cast<std::uint32_t>((std::uint64_t(start) + size + 0xFFFu) &
                                     ~std::uint64_t(0xFFFu));
}

// Every loaded library's RELRO as {start, size} (size 0 entries skipped).
static std::vector<std::pair<std::uint32_t, std::uint32_t>> RelroRanges(const LoadedModule& mod) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> r;
    if (mod.relro_size != 0) {
        r.emplace_back(mod.relro_start, mod.relro_size);
    }
    for (const auto& x : mod.more) {
        if (x.relro_size != 0) {
            r.emplace_back(x.relro_start, x.relro_size);
        }
    }
    return r;
}

void Runtime::AddModuleSegments(const LoadedModule& mod) {
    const auto relros = RelroRanges(mod);
    for (const auto& seg : mod.segments) {
        const std::uint64_t end = std::uint64_t(seg.start) + seg.size;
        bool split = false;
        for (const auto& rr : relros) {
            std::uint32_t ps = 0, pe = 0;
            RelroPages(rr.first, rr.second, &ps, &pe);
            if (seg.start <= ps && pe < end) {
                AddMapping(seg.start, pe - seg.start, seg.prot);
                AddMapping(pe, static_cast<std::uint32_t>(end - pe), seg.prot);
                split = true;
                break;
            }
        }
        if (!split) {
            AddMapping(seg.start, seg.size, seg.prot);
        }
    }
}

void Runtime::ProtectRelro(const LoadedModule& mod) {
    for (const auto& rr : RelroRanges(mod)) {
        ProtectOneRelro(rr.first, rr.second);
    }
}

void Runtime::ProtectOneRelro(std::uint32_t relro_start, std::uint32_t relro_size) {
    std::uint32_t ps = 0, pe = 0;
    RelroPages(relro_start, relro_size, &ps, &pe);
    const MappingTable::Hit h = mappings_.Find(ps, pe - ps);
    if (h.end == 0) {
        std::printf("[guest] LOUD RELRO [%08x,%08x) has no mapping entry -- left writable\n", ps, pe);
        return;
    }
    // Table first (host thunks check it), then the host pages.
    const int ro = h.prot & ~PROT_WRITE;
    if (h.start == ps && h.end == pe) {
        mappings_.SetProt(h.start, h.end, ro);
    } else {
        std::printf("[guest] LOUD RELRO [%08x,%08x) shares a mapping entry -- table not narrowed\n",
                    ps, pe);
    }
    if (!mem_.Protect(ps, pe - ps, PROT_READ)) {
        std::printf("[guest] LOUD mprotect(RELRO [%08x,%08x), PROT_READ) failed\n", ps, pe);
    }
    AddWriteGuard(relro_start, relro_size, "RELRO");  // names slow-path hits
}

void Runtime::AddWriteGuard(std::uint32_t start, std::uint32_t size, const char* what) {
    if (size == 0) {
        return;
    }
    write_guards_.push_back({start, start + size, what});
}

bool Runtime::WriteGuardHit(std::uint32_t addr, std::size_t size, const char** what) {
    const std::uint64_t end = std::uint64_t(addr) + size;
    for (const WriteGuard& g : write_guards_) {
        if (addr >= g.start && end <= g.end) {
            *what = g.what;
            return true;
        }
    }
    return false;
}

// dlmalloc corruption tripwire (see abr_heap_hook.h): runs INSTEAD of a bare
// abort() when dlmalloc detects heap corruption. Async-safe: atomics, TLS
// read, raw write(2) only (stderr is freopen'd to apphost.log). The macro
// aborts right after, so this line is the corruption's own testimony.
static std::atomic<unsigned> g_heap_corrupt_n{0};
static std::atomic<std::uint32_t> g_heap_corrupt_pc{0};
static std::uint32_t g_last_free_addr = 0;
static std::uint32_t g_last_free_size = 0;
static void DeathHex2(char*& p, std::uint32_t v) {
    static const char* Hx = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        *p++ = Hx[(v >> (i * 4)) & 0xFu];
    }
}
static void Hex64(char*& p, std::uint64_t v) {
    static const char* Hx = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        *p++ = Hx[(v >> (i * 4)) & 0xFu];
    }
}

// ---------------------------------------------------------------------------
// D2 (2026-10-18): live-block index surface.
// The old structure was a std::map<base, usable>: exact base lookups PLUS
// ORDERED interval containment for interior pointers. The new index (live_heap.h)
// is flat and allocation-free per operation and exact for bases; containment is
// answered EXACTLY by two tiers: an ordered side-index of blocks larger than the
// probe's reach (big_live_) plus a bounded back-probe for every smaller block.
// Why not the probe alone: a real session showed every in-arena containment
// query is either an exact base or an interior farther than 512 B back
// (contain_ok=0), so a probe-only answer silently dropped the sscanf field clamp
// on 1218 boot-path destinations -- a bounded probe is not containment. The
// two-tier answer was then PROVEN equal to the old full ordered map on that same
// workload (temporary DiagOn oracle: contain_diff=0 over 1242 interior queries +
// boot, big==bigscan each report), which is why the oracle is gone and only the
// plain counters below remain.
// Containment is still only ever asked by cold consumers (the diagnostic
// firewall, heapwatch, sscanf's %s/%[/%c destination, the two JNI region bounds).
// Every call site already holds mutex_.
// ---------------------------------------------------------------------------

// Ordered containment over a map<base, usable>: an exact base hit, else the
// interval of the immediately preceding live block if the address falls inside
// it. This is the probe's tier-1 replacement (see the D2 block above): the only
// blocks whose interiors a hash cannot reach, ordered because ordering is
// exactly what containment needs.
static bool MapContaining(const std::map<std::uint32_t, std::uint32_t>& m, std::uint32_t addr,
                          std::uint32_t* base, std::uint32_t* size) {
    const auto it = m.find(addr);
    if (it != m.end()) {
        *base = addr;
        *size = it->second;
        return true;
    }
    auto jt = m.upper_bound(addr);
    if (jt != m.begin()) {
        --jt;
        if ((std::uint64_t)addr - jt->first < jt->second) {
            *base = jt->first;
            *size = jt->second;
            return true;
        }
    }
    return false;
}

void Runtime::LiveIdxInsert(std::uint32_t base, std::uint32_t size) {
    live_idx_.Insert(base, size);
    // Large-block side-index (exact containment tier). Safe to insert without
    // checking for a previous entry: callers only insert a base that is not live
    // (fresh malloc/calloc, or realloc right after its erase). Small blocks pay
    // nothing here -- and that is the hot path.
    if (size > kProbeReach) {
        big_live_[base] = size;
        ++big_ins_n_;
    }
}

std::uint32_t Runtime::LiveIdxErase(std::uint32_t base) {
    const std::uint32_t got = live_idx_.Erase(base);
    if (got > kProbeReach) {
        big_live_.erase(base);
        ++big_del_n_;
    }
    return got;
}

std::uint32_t Runtime::LiveIdxFind(std::uint32_t base) {
    std::lock_guard<std::recursive_mutex> heap(heap_mu_);
    return live_idx_.Find(base);
}

bool Runtime::LiveIdxContaining(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size) {
    std::lock_guard<std::recursive_mutex> heap(heap_mu_);
    // EXACT, two tiers, cheapest first:
    //   1. a live block whose base is this very address (one hash lookup);
    //   2. a block LARGER than the probe's reach -- the only blocks whose
    //      interiors the probe below cannot reach (one ordered lookup, small map);
    //   3. a block at most kProbeReach payload: its base is at most 64 x 8 B
    //      behind the 8-aligned address (bounded back-probe).
    // Cost note: only the cold consumers ask this (sscanf's %s/%[/%c bound, the
    // JNI region bounds, the diagnostic firewall/heapwatch) -- never the
    // alloc/free hot path.
    std::uint32_t b = 0, s = 0;
    bool found = false;
    const std::uint32_t exact = live_idx_.Find(addr);
    if (exact) {
        b = addr;
        s = exact;
        found = true;
    } else if (addr >= heap_base_ && (std::uint64_t)addr < (std::uint64_t)heap_base_ + kHeapSize) {
        // Interior pointer. Only an in-arena address can be inside a live block,
        // so the common non-heap destination (guest stack, module) costs one
        // range check and nothing else.
        if (MapContaining(big_live_, addr, &b, &s)) {
            found = true;
            ++hx_contain_big_;
        }
        if (!found) {
            const std::uint32_t a8 = addr & ~std::uint32_t(7);
            for (unsigned k = 1; k <= 64; ++k) {
                const std::uint32_t off = k * 8u;
                if (a8 < heap_base_ + off) {
                    break;  // walked past the start of the arena
                }
                const std::uint32_t cand = a8 - off;
                const std::uint32_t cs = live_idx_.Find(cand);
                if (cs && (std::uint64_t)addr - cand < cs) {
                    b = cand;
                    s = cs;
                    found = true;
                    ++hx_contain_ok_;
                    break;
                }
            }
        }
        if (!found) {
            ++hx_contain_miss_;
        }
    }
    if (found) {
        *base = b;
        *size = s;
    }
    return found;
}

void Runtime::PrintHeapIdx() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::lock_guard<std::recursive_mutex> heap(heap_mu_);  // order: mutex_ -> heap_mu_
    // bigscan= walks the flat table independently: it must equal big= (the
    // ordered side-index size) or the two tiers have drifted apart.
    const std::size_t bigscan = live_idx_.CountLargerThan(kProbeReach);
    std::printf(
        "[heapidx] live=%zu max=%zu cap=%zu grown=%u compacted=%u shrunk=%u ops=%llu "
        "contain_ok=%llu contain_big=%llu contain_miss=%llu free_unk=%llu "
        "realloc_unk=%llu pool_stale=%llu big=%zu bigscan=%zu "
        "big_ins=%llu big_del=%llu usedKB=%llu\n",
        live_idx_.Size(), live_idx_.MaxLive(), live_idx_.Capacity(), live_idx_.Grown(),
        live_idx_.Compacted(), live_idx_.Shrunk(), live_idx_.Ops(), hx_contain_ok_,
        hx_contain_big_, hx_contain_miss_, hx_free_unknown_,
        hx_realloc_unknown_, hx_pool_stale_, big_live_.size(),
        bigscan, big_ins_n_, big_del_n_,
        (unsigned long long)(heap_used_ >> 10));
}

std::uint32_t Runtime::ClampHeapWrite(std::uint32_t dst, std::uint32_t len, const char* n,
                                      bool force) {
    // D1: OFF by default (see dispatch.h). Bypass before the lock so the hot
    // memory/string paths pay nothing, not even an uncontended lock. force=true
    // is used only by the JNI region paths, whose return value is a functional
    // bound.
    if (!force && !ClampWriteOn()) {
        return len;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Find the containing live block (exact base, else an interior resolved by
    // the large-block side-index or the bounded back-probe). Not inside one ->
    // unclamped, same as before.
    std::uint32_t base = 0, size = 0;
    if (!LiveIdxContaining(dst, &base, &size)) {
        // Not inside any LIVE allocation. Inside the arena this means a dlmalloc
        // header word, a free chunk, the top chunk or a quarantined (freed)
        // block -- i.e. exactly the bump-era overflow/UAF-write that bump
        // tolerated (never reused) and dlmalloc cannot (that memory is metadata
        // now). The write goes through UNCLAMPED (host-side: heapwatch only sees
        // guest stores). Log it (capped) so the writer is named before the
        // metadata detonates. NOTE (D2): `base=0` means "no containing live
        // block"; it replaced the old `nxt=` successor head, which a hash index
        // cannot cheaply produce (and whose only consumer was this log line).
        if (dst >= heap_base_ && (std::uint64_t)dst < (std::uint64_t)heap_base_ + kHeapSize) {
            static int gn = 0;
            if (gn < 40) {
                gn++;
                std::uint32_t lr = 0, pc = 0;
                Dynarmic::A32::Jit* aj = ActiveJit();
                if (aj) {
                    lr = aj->Regs()[14];
                    pc = aj->Regs()[15];
                }
                std::printf("[heap] GAPWRITE %s dst=%08x len=%u base=%08x pc=%08x lr=%08x\n", n,
                            dst, len, 0, pc, lr);
            }
        }
        return len;  // gap/quarantine/module: not ours
    }
    const std::uint64_t lim = (std::uint64_t)base + size;
    const std::uint64_t end = (std::uint64_t)dst + len;
    if (end > lim) {
        static int ocn = 0;
        if (ocn < 10) {
            ocn++;
            std::uint32_t lr = 0;
            Dynarmic::A32::Jit* aj = ActiveJit();
            if (aj) {
                lr = aj->Regs()[14];
            }
            std::printf("[heap] LOUD %s overflow dst=%08x len=%u base=%08x size=%u lr=%08x\n", n,
                        dst, len, base, size, lr);
        }
        return (std::uint32_t)(lim > dst ? lim - dst : 0);
    }
    return len;
}

bool Runtime::CheckAccess(std::uint32_t addr, std::size_t size, bool write, bool exec) {
    const std::uint64_t end = std::uint64_t(addr) + size;
    // Last-hit cache (perf): string/scan loops probe per byte. The cached
    // extent is valid while the table's epoch is unchanged (it moves when an
    // entry's protection changes, e.g. munmap).
    // One thread_local (one TLS lookup), not four.
    thread_local struct {
        std::uint32_t s = 1, e = 0, epoch = 0;
        int p = 0;
    } hit;
    const std::uint32_t ep = mappings_.epoch();
    if (ep != hit.epoch || !(addr >= hit.s && end <= hit.e)) {
        const MappingTable::Hit h = mappings_.Find(addr, size);
        if (h.end == 0) {
            return false;
        }
        hit.s = h.start;
        hit.e = h.end;
        hit.p = h.prot;
        hit.epoch = ep;
    }
    if (exec) {
        return (hit.p & PROT_EXEC) != 0;
    }
    if (write) {
        return (hit.p & PROT_WRITE) != 0;
    }
    return (hit.p & PROT_READ) != 0;
}

static std::uint32_t PadGlobal() {
    // Parsed once (getenv per call = linear environ scan on hot paths).
    static std::int64_t v = -1;
    if (v < 0) {
        v = 0;
        if (const char* pe = std::getenv("HARNESS_MALLOCPAD")) {
            v = (std::int64_t)std::strtoul(pe, nullptr, 0);
            if (v > 4096) {
                v = 4096;
            }
        }
    }
    return (std::uint32_t)v;
}

// Zero-fill policy (bump parity). The bump allocator only ever handed out
// virgin arena, so a fresh block read as zeros. dlmalloc reuses freed chunks,
// so a fresh block can contain stale bytes -- and a stale pointer-shaped value
// read as a pointer is a wild dereference. HARNESS_ZEROFILL=<bytes>|all zeroes
// every request up to <bytes> ("all" = no limit). Default off (dlmalloc
// semantics, fastest); parsed once.
static std::uint32_t ZeroFillLimit() {
    static std::int64_t v = -2;
    if (v == -2) {
        v = 0;
        if (const char* ze = std::getenv("HARNESS_ZEROFILL")) {
            if (std::strcmp(ze, "all") == 0) {
                v = 0xFFFFFFFFLL;
            } else {
                v = (std::int64_t)std::strtoul(ze, nullptr, 0);
            }
        }
    }
    return (std::uint32_t)v;
}

std::uint32_t Runtime::GuestMalloc(std::uint32_t size) { return GuestMemalign(0, size); }

// align == 0: plain malloc. Otherwise a power of two (memalign semantics);
// the block is a normal heap block for free/realloc/usable_size.
// The heap's debug knobs, read once (EnvOn scans its table on every call,
// and malloc/free are hot). The environment is final before guest code runs.
enum HeapKnobId { kMallocLog, kNoRelease };
static bool HeapKnob(HeapKnobId k) {
    static const bool on[2] = {EnvOn("HARNESS_MALLOCLOG"), EnvOn("HARNESS_NORELEASE")};
    return on[k];
}

std::uint32_t Runtime::GuestMemalign(std::uint32_t align, std::uint32_t size) {
    RtLock lock(*this, 1, &heap_mu_);
    // HARNESS_MALLOCPAD=N: extra deterministic slack per allocation (layout
    // disease test for wall4: game memsets 255B into 144B objects; dense bump
    // packing puts live neighbors in the blast radius, dlmalloc may not).
    // Parsed once (getenv per malloc = linear environ scan on the hot path).
    static std::int64_t mallocpad = -1;
    if (mallocpad < 0) {
        mallocpad = 0;
        if (const char* pe = std::getenv("HARNESS_MALLOCPAD")) {
            mallocpad = (std::int64_t)std::strtoul(pe, nullptr, 0);
            if (mallocpad > 4096) {
                mallocpad = 4096;
            }
        }
    }
    const std::uint32_t pad = (std::uint32_t)mallocpad;
    // Round in 64 bits: (size + 7) used to wrap for sizes >= 0xFFFFFFF9 and
    // hand back an 8-byte block for a near-4 GB request. Anything larger
    // than the whole arena fails like a real malloc (NULL).
    const std::uint64_t rounded =
        std::max<std::uint64_t>((std::uint64_t(size) + 7) & ~std::uint64_t(7), 8);
    if (rounded + pad > kHeapSize) {
        return 0;
    }
    size = static_cast<std::uint32_t>(rounded);
    const std::uint32_t total =
        size + pad;  // pad kept: MALLOCPAD layout probe stays available
    void* hp = !heap_ms_ ? nullptr
               : align > 8 ? mspace_memalign(heap_ms_, align, total)
                           : mspace_malloc(heap_ms_, total);
    std::uint32_t out = 0;
    if (hp) {
        const std::uint64_t off =
            (std::uint64_t)(static_cast<std::uint8_t*>(hp) - H(heap_base_));
        if (off + total <= kHeapSize) {
            out = heap_base_ + (std::uint32_t)off;
        } else {
            // Arena escape (should not happen: no sbrk/mmap backend) --
            // hand back and fail loud instead of forging a wild VA.
            mspace_free(heap_ms_, hp);
            hp = nullptr;
        }
    }
    if (!out) {
        // LOUD OOM (2026-10-14): silent 0 -> guest bad_alloc -> unwind
        // spiral -> stack smash -> death. Capped (sustained-OOM spam).
        static int oomn = 0;
        if (++oomn <= 10) {
            std::uint32_t lr = 0;
            Dynarmic::A32::Jit* aj = ActiveJit();
            if (aj) {
                lr = aj->Regs()[14];
            }
            std::printf("[heap] OOM #%d size=%u used=%llu lr=%08x thr=%u thrMB=%llu\n", oomn, size,
                        (unsigned long long)heap_used_, lr, thr_stacks_,
                        (unsigned long long)(thr_stack_bytes_ >> 20));
            if (oomn == 1) {
                DumpEventRing();
            }
        }
        return 0;
    }
    try {
        // Honest bound for the firewall/heapwatch: the REAL payload dlmalloc
        // handed out (>= the request). The old max(size,16) claim left an
        // 8-byte blind spot on small blocks -- exactly where the next chunk's
        // header lives -- so a guest overflow there stayed invisible until
        // mspace_free walked the smeared metadata (2026-10-17 crash).
        std::uint32_t usable = (std::uint32_t)mspace_usable_size(hp);
        if (usable < size) {
            usable = size;  // paranoia: never below the request
        }
        LiveIdxInsert(out, usable);
        heap_used_ += total;
#ifdef ABR_DIAG_COUNTERS
        if (heap_used_ > diag_.heap_peak) {
            diag_.heap_peak = heap_used_;
        }
#endif
    } catch (...) {
        // Host-side OOM on bookkeeping (not guest OOM): hand the block back,
        // fail like OOM. Otherwise a std::bad_alloc would escape through the
        // thunk into terminate -> silent SIGABRT.
        mspace_free(heap_ms_, hp);
        return 0;
    }
    // NOTE (2026-10-16): blanket zero-fill was removed here (a full memset per
    // malloc incl. multi-MB textures cost 1fps + audio lag). It is back behind
    // HARNESS_ZEROFILL (see ZeroFillLimit) so the "fresh memory must read as
    // zeros (bump parity)" hypothesis can be tested without a rebuild; the
    // shipping default stays off.
    {
        const std::uint32_t zl = ZeroFillLimit();
        if (zl && size <= zl) {
            std::memset(H(out), 0, size);
        }
    }
    // HARNESS_MALLOCLOG=1: ring (addr,size,caller-lr), no cap needed (fixed
    // RAM, last 200k kept). Zero steady-state I/O; dumped on crash/hang.
    if (HeapKnob(kMallocLog)) {
        // ActiveJit (thread-local): worker mallocs get the worker lr.
        std::uint32_t lr = 0;
        Dynarmic::A32::Jit* aj = ActiveJit();
        if (aj) {
            lr = aj->Regs()[14];
        }
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "[malloc] %08x size=%u lr=%08x", out, size, lr);
        RingPush(tmp);
    }
    return out;
}

std::uint32_t Runtime::GuestCalloc(std::uint32_t n, std::uint32_t size) {
    // calloc(n, size): the product is computed in 64 bits, so overflow of the
    // guest's 32-bit size_t is a plain "too large" -> NULL. A zero product
    // still returns a unique minimum block, like bionic/dlmalloc (it used to
    // return NULL, which guest code treats as out-of-memory). The limit is
    // the arena itself (there was an unexplained 256 MB cap).
    std::uint64_t total = std::uint64_t(n) * size;
    if (total > kHeapSize) {
        return 0;
    }
    if (total == 0) {
        n = 1;
        size = 1;
        total = 1;
    }
    // mspace_calloc zeroes itself; keep our size map + telemetry in sync.
    std::lock_guard<std::recursive_mutex> lock(heap_mu_);
    void* hp = heap_ms_ ? mspace_calloc(heap_ms_, (std::size_t)n, size) : nullptr;
    if (!hp) {
        std::printf("[heap] OOM calloc n=%u size=%u used=%llu\n", n, size,
                    (unsigned long long)heap_used_);
        return 0;
    }
    const std::uint64_t off =
        (std::uint64_t)(static_cast<std::uint8_t*>(hp) - H(heap_base_));
    if (off + total > kHeapSize) {
        mspace_free(heap_ms_, hp);
        return 0;
    }
    const std::uint32_t out = heap_base_ + (std::uint32_t)off;
    // Honest real payload (same as GuestMalloc: no max(size,16) blind spot).
    std::uint32_t usable = (std::uint32_t)mspace_usable_size(hp);
    if (usable < total) {
        usable = (std::uint32_t)total;
    }
    LiveIdxInsert(out, usable);
    heap_used_ += total;
#ifdef ABR_DIAG_COUNTERS
    if (heap_used_ > diag_.heap_peak) {
        diag_.heap_peak = heap_used_;
    }
#endif
    return out;
}

std::uint32_t Runtime::GuestRealloc(std::uint32_t addr, std::uint32_t size) {
    if (addr == 0) {
        return GuestMalloc(size);
    }
    if (size == 0) {
        GuestFree(addr);
        return 0;
    }
    // Unknown-addr realloc (heap audit 2026-10-15: all guest callers pass
    // heap-or-NULL, so a miss degrades to the existing LOUD OOM path, never
    // silent). Old code copied `size` garbage bytes from an arbitrary address
    // (wild addr = host SIGSEGV inside the reservation). Fail loud instead.
    std::uint32_t old = 0;
    bool known = false;
    std::lock_guard<std::recursive_mutex> lock(heap_mu_);
    {
        old = LiveIdxFind(addr);
        known = (old != 0);
    }
    if (!known) {
        ++hx_realloc_unknown_;
        std::uint32_t lr = 0;
        Dynarmic::A32::Jit* aj = ActiveJit();
        if (aj) {
            lr = aj->Regs()[14];
        }
        std::printf("[heap] LOUD realloc unknown addr=%08x size=%u lr=%08x\n", addr, size, lr);
        DumpEventRing();
        return 0;
    }
    // dlmalloc copies min(old,new) itself; migrate our size map + used
    // counter. Also drop any audio-pool reference: the block may move.
    // Lock held across (USE_LOCKS=0: no internal dlmalloc lock).
    // Pre-dump chunk header zone (writers smash here; names the content).
    {
        static int rhd = 0;
        if (DiagOn() && rhd < 30 && addr >= 32 && CheckRange(addr - 32, 32, false)) {
            rhd++;
            const std::uint8_t* hp = H<const std::uint8_t>(addr - 32);
            std::printf("[realloc] pre %08x:", addr);
            for (int i = 0; i < 32; i++) {
                std::printf(" %02x", hp[i]);
            }
            std::printf("\n");
        }
    }
    if (std::uint64_t(size) + PadGlobal() > kHeapSize) {
        return 0;  // too large: the old block stays valid (realloc contract)
    }
    const std::uint32_t rtotal = size + PadGlobal();
    void* hp = heap_ms_ ? mspace_realloc(heap_ms_, H(addr), rtotal) : nullptr;
    if (!hp) {
        return 0;  // old block stays valid (realloc contract), LOUD via OOM path next
    }
    const std::uint64_t off =
        (std::uint64_t)(static_cast<std::uint8_t*>(hp) - H(heap_base_));
    if (off + rtotal > kHeapSize) {
        // Escaped the arena (should not happen: no backend). Erase the stale
        // entry and LEAK deliberately: the old block is already consumed, and
        // quarantining it would later mspace_free a moved chunk (double-free).
        std::printf("[heap] LOUD realloc escaped arena addr=%08x size=%u\n", addr, size);
        LiveIdxErase(addr);
        return 0;
    }
    const std::uint32_t out = heap_base_ + (std::uint32_t)off;
    {
        LiveIdxErase(addr);
        // Honest real payload again (no max(size,16) blind spot).
        std::uint32_t usable = (std::uint32_t)mspace_usable_size(hp);
        if (usable < rtotal) {
            usable = rtotal;
        }
        LiveIdxInsert(out, usable);
        heap_used_ += (std::uint64_t)rtotal > old ? (std::uint64_t)rtotal - old : 0;
        if ((std::uint64_t)old > rtotal) {
            heap_used_ -= (std::uint64_t)old - rtotal;
        }
#ifdef ABR_DIAG_COUNTERS
        if (heap_used_ > diag_.heap_peak) {
            diag_.heap_peak = heap_used_;
        }
#endif
        for (auto it = array_pool_.begin(); it != array_pool_.end(); ++it) {
            if (it->first == addr) {
                array_pool_.erase(it);
                break;
            }
        }
    }
    // Zero-fill policy: the bytes past the old size are fresh to the guest.
    // Bump parity says they read as zeros; dlmalloc's copy leaves whatever the
    // destination chunk held. Zero only the grown tail (never the payload).
    {
        const std::uint32_t zl = ZeroFillLimit();
        if (zl && size <= zl && (std::uint64_t)size > old) {
            std::memset(H(out) + old, 0, size - old);
        }
    }
    return out;
}

void Runtime::GuestFree(std::uint32_t addr) {
    // The block goes straight back to dlmalloc. No quarantine (REVIEW R-013:
    // a delay before reuse only hides a use-after-free; the owner ruled it
    // out). A free of an address the heap never handed out (or freed twice)
    // is ignored and counted, as before.
    if (!addr) {
        return;
    }
    RtLock lock(*this, 2, &heap_mu_);
#ifdef ABR_DIAG_COUNTERS
    struct InLock {
        std::atomic<std::uint64_t>& acc;
        const std::uint64_t t0 = DiagTicks();
        ~InLock() { acc.fetch_add(DiagTicks() - t0, std::memory_order_relaxed); }
    } in_lock{diag_.free_in_lock};
#endif
    if (!norelease_set_.empty() && norelease_set_.count(addr)) {
        std::printf("[heap] LOUD double free of a held block %08x (NORELEASE)\n", addr);
        return;
    }
    // One hash lookup yields the size and removes the entry. 0 = "never
    // handed out" (or already freed) -> ignored, counted.
#ifdef ABR_DIAG_COUNTERS
    const std::uint64_t le0 = DiagTicks();
#endif
    const std::uint32_t size = LiveIdxErase(addr);
#ifdef ABR_DIAG_COUNTERS
    diag_.free_live.fetch_add(DiagTicks() - le0, std::memory_order_relaxed);
#endif
    const auto log_free = [&](std::uint32_t sz) {
        std::uint32_t lr = 0;
        if (Dynarmic::A32::Jit* aj = ActiveJit()) {
            lr = aj->Regs()[14];
        }
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "[mfree] %08x size=%u lr=%08x", addr, sz, lr);
        RingPush(tmp);
    };
    if (!size) {
        ++hx_free_unknown_;
        if (HeapKnob(kMallocLog)) {
            log_free(0xFFFFFFFFu);
        }
        return;
    }
    // Drop any audio-pool reference (stale reuse would alias).
    for (auto pit = array_pool_.begin(); pit != array_pool_.end(); ++pit) {
        if (pit->first == addr) {
            array_pool_.erase(pit);
            break;
        }
    }
    if (HeapKnob(kMallocLog)) {
        log_free(size);
    }
    // HARNESS_NORELEASE=1 (triage): hold every freed block for good.
    if (HeapKnob(kNoRelease)) {
        norelease_held_.push_back({addr, size});
        norelease_set_.insert(addr);
        return;
    }
    g_last_free_addr = addr;
    g_last_free_size = size;
    if (heap_ms_) {
#ifdef ABR_DIAG_COUNTERS
        const std::uint64_t mf0 = DiagTicks();
#endif
        mspace_free(heap_ms_, H(addr));
#ifdef ABR_DIAG_COUNTERS
        diag_.free_mspace.fetch_add(DiagTicks() - mf0, std::memory_order_relaxed);
#endif
    }
    heap_used_ = heap_used_ >= size ? heap_used_ - size : 0;
}

// Host side of guest arena pages [addr, addr+len): `anon` gives fresh zero
// pages with `hprot`, otherwise the file at `offset`. With 4 KiB host pages
// (or a host-page-aligned range) the kernel does it exactly. A larger host
// page cannot be remapped in 4 KiB parts: anonymous memory is then zeroed in
// place and the host pages stay read-write (the page map holds the guest's
// protection and every bounds check enforces it); a file there fails.
// Returns 0 or an errno value.
int Runtime::HostMapArena(std::uint32_t addr, std::uint64_t len, int hprot, int share, bool anon,
                          int host_fd, std::uint64_t offset) {
    const std::uint64_t hp = GuestMemory::HostPageSize();
    void* want = H(addr);
    if (hp == 4096 || ((addr % hp) == 0 && (len % hp) == 0)) {
        void* r = anon ? ::mmap(want, len, hprot, share | MAP_ANONYMOUS | MAP_FIXED, -1, 0)
                       : ::mmap(want, len, hprot, share | MAP_FIXED, host_fd,
                                static_cast<off_t>(offset));
        if (r == MAP_FAILED) {
            const int e = errno;
            ::mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            return e;
        }
        return 0;
    }
    if (!anon) {
        return EINVAL;
    }
    const std::uint64_t lo = std::uint64_t(addr) & ~(hp - 1);
    const std::uint64_t hi = (std::uint64_t(addr) + len + hp - 1) & ~(hp - 1);
    ::mprotect(H(static_cast<std::uint32_t>(lo)), static_cast<std::size_t>(hi - lo),
               PROT_READ | PROT_WRITE);
    std::memset(want, 0, len);
    return 0;
}

std::uint32_t Runtime::GuestMmap(std::uint32_t addr, std::uint32_t size, int prot, int flags,
                                 int host_fd, std::uint64_t offset) {
    // Guest flag/prot values are the Linux ones on both ABIs (asm-generic
    // mman: MAP_SHARED 1, MAP_PRIVATE 2, MAP_FIXED 0x10, MAP_ANONYMOUS 0x20).
    // Mappings live in the mmap arena, page by page (MappingTable): a
    // MAP_FIXED replaces whatever pages were there, as the kernel's does.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const bool anon = (flags & MAP_ANONYMOUS) != 0;
    const int share = flags & (MAP_SHARED | MAP_PRIVATE);
    if (size == 0 || (share != MAP_SHARED && share != MAP_PRIVATE) || (!anon && host_fd < 0) ||
        (offset & 0xFFFu) != 0) {
        SetGuestErrno(anon || host_fd >= 0 ? EINVAL : EBADF);
        return 0xFFFFFFFFu;
    }
    const std::uint64_t len = (std::uint64_t(size) + 0xFFFu) & ~std::uint64_t(0xFFFu);
    const int hprot = prot & (PROT_READ | PROT_WRITE | PROT_EXEC);
    std::uint32_t out = 0;
    if (flags & MAP_FIXED) {
        const std::uint64_t end = std::uint64_t(addr) + len;
        if ((addr & 0xFFFu) != 0 || addr < kMmapArenaBase || end > kTrampBase) {
            std::printf("[mmap] LOUD MAP_FIXED %08x+%llx outside the mmap arena -- refused\n", addr,
                        (unsigned long long)len);
            SetGuestErrno(EINVAL);
            return 0xFFFFFFFFu;
        }
        out = addr;
    } else {
        // A free, aligned hint is honoured; otherwise the next free run.
        if (addr != 0 && (addr & 0xFFFu) == 0 && mappings_.ArenaFree(addr, std::uint64_t(addr) + len)) {
            out = addr;
        } else {
            out = mappings_.ArenaFindFree(mmap_next_, len, 0x10000u);
        }
        if (out == 0) {
            SetGuestErrno(ENOMEM);
            return 0xFFFFFFFFu;
        }
        mmap_next_ = static_cast<std::uint32_t>(out + len);
    }
    const int e = HostMapArena(out, len, hprot, share, anon, host_fd, offset);
    if (e != 0) {
        if (flags & MAP_FIXED) {
            // The kernel would have dropped the old pages already.
            mappings_.SetArena(out, static_cast<std::uint32_t>(out + len), false, 0);
        }
        SetGuestErrno(e);
        return 0xFFFFFFFFu;
    }
    mappings_.SetArena(out, static_cast<std::uint32_t>(out + len), true, hprot);
    return out;
}

int Runtime::GuestMprotect(std::uint32_t addr, std::uint32_t size, int prot) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::uint64_t len = (std::uint64_t(size) + 0xFFFu) & ~std::uint64_t(0xFFFu);
    if ((addr & 0xFFFu) != 0 || len == 0) {
        SetGuestErrno(EINVAL);
        return -1;
    }
    const int hprot = prot & (PROT_READ | PROT_WRITE | PROT_EXEC);
    if (MappingTable::InArena(addr, std::uint64_t(addr) + len)) {
        // mmap arena: any page range, adding or removing access (the kernel's
        // semantics); every page must be mapped (else ENOMEM).
        const std::uint32_t end = static_cast<std::uint32_t>(addr + len);
        if (mappings_.Find(addr, len).end == 0) {
            SetGuestErrno(ENOMEM);
            return -1;
        }
        const std::uint64_t hp = GuestMemory::HostPageSize();
        if (hp == 4096 || ((addr % hp) == 0 && (len % hp) == 0)) {
            ::mprotect(H(addr), static_cast<std::size_t>(len), hprot);
        }
        // (Larger host pages: they stay read-write, the page map enforces.)
        mappings_.SetProt(addr, end, hprot);
        return 0;
    }
    // Boot mappings: protection changes only ever ADD access. Such an entry
    // cannot be split, and the host pages must allow everything the table
    // allows, or a host-side access would fault the host instead of halting
    // the guest. So the entry and the host pages get old|new; a request to
    // remove access (a guard page) is accepted and not enforced. Enough for
    // a guest JIT that flips pages RW <-> RX.
    const MappingTable::Hit h = mappings_.Find(addr, len);
    if (h.end == 0) {
        SetGuestErrno(ENOMEM);  // not mapped (kernel answer)
        return -1;
    }
    const int want = h.prot | hprot;
    if (want != h.prot) {
        const std::uint64_t hp = GuestMemory::HostPageSize();
        const std::uint64_t lo = std::uint64_t(h.start) & ~(hp - 1);
        const std::uint64_t hi = (std::uint64_t(h.end) + hp - 1) & ~(hp - 1);
        ::mprotect(H(static_cast<std::uint32_t>(lo)), static_cast<std::size_t>(hi - lo), want);
        mappings_.SetProt(h.start, h.end, want);
    }
    return 0;
}

void Runtime::SetThreadStack(std::uint32_t lo, std::uint32_t size) {
    t_stack_lo = lo;
    t_stack_size = size;
    if (t_sig) {
        t_sig->stack_top = lo + size;  // where a proxy runs for a parked thread
    }
}

std::uint32_t Runtime::PthreadAttr(ImportId id, const std::string& n, std::uint32_t a0, std::uint32_t a1,
                                   std::uint32_t a2) {
    using I = ImportId;
    // bionic arm32 pthread_attr_t (NDK pthread.h), 24 bytes:
    // { u32 flags; void* stack_base; size_t stack_size; size_t guard_size;
    //   s32 sched_policy; s32 sched_priority; }. flags bit 0 = detached.
    constexpr std::uint32_t kSize = 24, kDetached = 1;
    std::uint32_t at = 0;
    if (id == I::pthread_getattr_np_) {
        at = a1;  // (thread, attr)
    } else {
        at = a0;
    }
    if (!CheckRange(at, kSize, true)) {
        std::printf("[thread] LOUD %s: bad attr %08x -- halting\n", n.c_str(), at);
        HaltGuest();
        return EINVAL;
    }
    std::uint32_t f[6];
    std::memcpy(f, H(at), kSize);
    auto store = [&]() { std::memcpy(H(at), f, kSize); };
    auto out32 = [&](std::uint32_t p, std::uint32_t v) -> bool {
        if (!CheckRange(p, 4, true)) {
            return false;
        }
        std::memcpy(H(p), &v, 4);
        return true;
    };
    if (id == I::pthread_attr_init_) {
        // bionic defaults: joinable, 1 MiB stack (minus its own guard), one
        // page of guard, SCHED_NORMAL.
        const std::uint32_t d[6] = {0, 0, 1024u * 1024u, 4096u, 0, 0};
        std::memcpy(f, d, kSize);
        store();
        return 0;
    }
    if (id == I::pthread_attr_destroy_) {
        std::memset(f, 0, kSize);
        store();
        return 0;
    }
    if (id == I::pthread_getattr_np_) {
        // Only the calling thread's own stack is known here (enough for the
        // GC/stack-bounds use); other threads get the defaults.
        std::uint32_t lo = t_stack_lo, sz = t_stack_size;
        if (sz == 0) {
            lo = kStackBase;
            sz = kStackSize;
        }
        const bool self = (a0 == GuestTid());
        const std::uint32_t d[6] = {0, self ? lo : 0u, self ? sz : 1024u * 1024u, 4096u, 0, 0};
        std::memcpy(f, d, kSize);
        store();
        return 0;
    }
    if (id == I::pthread_attr_getstack_) {  // (attr, void** addr, size_t* size)
        return (out32(a1, f[1]) && out32(a2, f[2])) ? 0u : static_cast<std::uint32_t>(EINVAL);
    }
    if (id == I::pthread_attr_getstacksize_) {
        return out32(a1, f[2]) ? 0u : static_cast<std::uint32_t>(EINVAL);
    }
    if (id == I::pthread_attr_setstacksize_) {
        // PTHREAD_STACK_MIN of the guest's bionic: 8192 on LP32 (NDK
        // pthread.h; 16384 is the LP64 value). FMOD asks for 8192 for its
        // stream threads; refusing it failed every music stream with
        // FMOD_ERR_INTERNAL.
        if (a1 < 8192u) {
            return EINVAL;
        }
        f[2] = a1;
        store();
        return 0;
    }
    if (id == I::pthread_attr_getdetachstate_) {
        return out32(a1, (f[0] & kDetached) ? 1u : 0u) ? 0u : static_cast<std::uint32_t>(EINVAL);
    }
    if (id == I::pthread_attr_setdetachstate_) {
        if (a1 > 1) {
            return EINVAL;
        }
        f[0] = a1 ? (f[0] | kDetached) : (f[0] & ~kDetached);
        store();
        return 0;
    }
    if (id == I::pthread_attr_setschedpolicy_) {
        f[4] = a1;
        store();
        return 0;
    }
    if (id == I::pthread_attr_setschedparam_) {  // (attr, const sched_param*)
        if (!CheckRange(a1, 4, false)) {
            return EINVAL;
        }
        std::memcpy(&f[5], H(a1), 4);
        store();
        return 0;
    }
    return EINVAL;
}

namespace {
// The CLOCK_REALTIME instant `ns` nanoseconds from now.
struct timespec RealtimeIn(long ns) {
    struct timespec t {};
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += ns;
    while (t.tv_nsec >= 1000000000L) {
        t.tv_nsec -= 1000000000L;
        t.tv_sec++;
    }
    return t;
}
bool Before(const struct timespec& a, const struct timespec& b) {
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}
}  // namespace

std::uint32_t Runtime::GuestSem(ImportId id, const std::string& n, std::uint32_t sem, std::uint32_t a1,
                                std::uint32_t a2) {
    using I = ImportId;
    // A guest sem_t (4 bytes on arm32) is backed by a host sem_t, keyed by
    // its address like the mutex map. Blocking waits run outside mutex_.
    if (!CheckRange(sem, 4, true)) {
        return kSemBadPtr;
    }
    std::shared_ptr<sem_t> s;
    if (id == I::sem_post_ || id == I::sem_wait_ || id == I::sem_trywait_ || id == I::sem_timedwait_ ||
        id == I::sem_getvalue_) {
        // The hot path only reads the map: its own reader lock, never the
        // runtime's mutex_ (as the guest mutex map).
        std::shared_lock<std::shared_mutex> map_lock(sem_map_mu_);
        const auto it = sem_map_.find(sem);
        if (it == sem_map_.end()) {
            SetGuestErrno(EINVAL);
            return 0xFFFFFFFFu;
        }
        s = it->second;
    } else {
        std::unique_lock<std::shared_mutex> map_lock(sem_map_mu_);
        if (id == I::sem_init_) {  // (sem, pshared, value)
            auto ns = std::shared_ptr<sem_t>(new sem_t, [](sem_t* p) {
                ::sem_destroy(p);
                delete p;
            });
            if (::sem_init(ns.get(), 0, a2) != 0) {
                SetGuestErrno(errno);
                return 0xFFFFFFFFu;
            }
            sem_map_[sem] = ns;  // a re-init replaces it (waiters keep theirs)
            return 0;
        }
        const auto it = sem_map_.find(sem);
        if (it == sem_map_.end()) {
            SetGuestErrno(EINVAL);
            return 0xFFFFFFFFu;
        }
        s = it->second;
        if (id == I::sem_destroy_) {
            sem_map_.erase(it);
            return 0;
        }
    }
    int r = 0;
    if (id == I::sem_post_) {
        r = ::sem_post(s.get());
    } else if (id == I::sem_trywait_) {
        r = ::sem_trywait(s.get());
    } else if (id == I::sem_getvalue_) {  // (sem, int* value)
        int v = 0;
        r = ::sem_getvalue(s.get(), &v);
        if (r == 0) {
            if (!CheckRange(a1, 4, true)) {
                return kSemBadPtr;
            }
            std::memcpy(H(a1), &v, 4);
        }
    } else if (id == I::sem_wait_ && SignalsInUse()) {
        // Interruptible: a handler runs between slices, then EINTR (sem_wait
        // is never restarted after a handler, whatever SA_RESTART says) --
        // for an app targeting 24 or later. Below that bionic waits on
        // (semaphore.cpp, the target-SDK check in sem_wait), and an engine
        // built then takes a -1 for "not acquired" (Unity's Semaphore logs
        // "Failed to wait on a semaphore" and runs on without it).
        const bool eintr = guest_target_sdk_ == 0 || guest_target_sdk_ >= 24;
        for (;;) {
            const struct timespec slice = RealtimeIn(kSigSliceNs);
            r = ::sem_timedwait(s.get(), &slice);
            if (r == 0 || (errno != ETIMEDOUT && errno != EINTR)) {
                break;
            }
            if (SignalDeliverable()) {
                DeliverPendingSignals();
                if (eintr) {
                    errno = EINTR;
                    r = -1;
                    break;
                }
            }
        }
    } else if (id == I::sem_wait_) {
        do {
            r = ::sem_wait(s.get());
        } while (r != 0 && errno == EINTR);
    } else {  // sem_timedwait(sem, const struct timespec* abs CLOCK_REALTIME)
        if (!CheckRange(a1, 8, false)) {
            return kSemBadPtr;
        }
        std::int32_t g[2] = {0, 0};
        std::memcpy(g, H(a1), 8);
        if (g[1] < 0 || g[1] >= 1000000000) {
            SetGuestErrno(EINVAL);
            return 0xFFFFFFFFu;
        }
        struct timespec ts {};
        ts.tv_sec = g[0];
        ts.tv_nsec = g[1];
        for (;;) {
            struct timespec slice = ts;
            if (SignalsInUse()) {
                const struct timespec soon = RealtimeIn(kSigSliceNs);
                if (Before(soon, ts)) {
                    slice = soon;
                }
            }
            r = ::sem_timedwait(s.get(), &slice);
            if (r == 0 || (errno != ETIMEDOUT && errno != EINTR)) {
                break;
            }
            if (SignalDeliverable()) {
                DeliverPendingSignals();
                errno = EINTR;
                r = -1;
                break;
            }
            if (errno == ETIMEDOUT && !Before(slice, ts)) {
                break;  // the caller's deadline
            }
        }
    }
    if (r != 0) {
        SetGuestErrno(errno);
        return 0xFFFFFFFFu;
    }
    return 0;
}

int Runtime::GuestMunmap(std::uint32_t addr, std::uint32_t size) {
    // Any page range of the mmap arena, mapped or not (the kernel's rule):
    // the pages leave the page map first, so guest accesses halt instead of
    // reaching pages being replaced, then the host drops them (PROT_NONE; on a
    // larger host page that cannot be split, the contents are zeroed). The
    // range can be handed out again by a later mmap.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::uint64_t len = (std::uint64_t(size) + 0xFFFu) & ~std::uint64_t(0xFFFu);
    const std::uint64_t end = std::uint64_t(addr) + len;
    if (size == 0 || (addr & 0xFFFu) != 0 || addr < kMmapArenaBase || end > kTrampBase) {
        SetGuestErrno(EINVAL);
        return -1;
    }
    mappings_.SetArena(addr, static_cast<std::uint32_t>(end), false, 0);
    const std::uint64_t hp = GuestMemory::HostPageSize();
    if (hp == 4096 || ((addr % hp) == 0 && (len % hp) == 0)) {
        ::mmap(H(addr), len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    } else {
        HostMapArena(addr, len, PROT_NONE, MAP_PRIVATE, true, -1, 0);
    }
    return 0;
}

const char* Runtime::GuestStr(std::uint32_t guest) {
    // Thread-local scratch: worker threads share this Runtime; a shared
    // std::string here corrupted the host heap under concurrency (found via
    // gdb backtrace into unordered_map). Immediate use only.
    // The bound was 511 bytes, which silently cut a long guest string -- the
    // printf family then rendered a truncated string AND reported the
    // truncated length. It is now a generous sanity limit.
    thread_local static std::string scratch;
    scratch.clear();
    if (!CheckAccess(guest, 1, false, false)) {
        return scratch.c_str();
    }
    constexpr std::size_t kMax = 1u << 16;
    std::size_t n = 0;
    while (n < kMax) {
        if (!CheckAccess(guest + static_cast<std::uint32_t>(n), 1, false, false)) {
            break;
        }
        if (*H<char>(guest + static_cast<std::uint32_t>(n)) == '\0') {
            break;
        }
        ++n;
    }
    scratch.assign(reinterpret_cast<const char*>(H<char>(guest)), n);
    return scratch.c_str();
}

bool Runtime::GuestStrnlen(std::uint32_t s, std::uint32_t max, std::uint32_t* len) {
    std::uint32_t done = 0;
    while (done < max) {
        const std::uint32_t cur = s + done;
        if (cur < s) {
            return false;  // wrapped past 4 GB
        }
        // Readable extent that contains cur (same first-match rule as
        // CheckRange).
        const MappingTable::Hit h = mappings_.Find(cur, 1);
        if (h.end == 0 || (h.prot & PROT_READ) == 0) {
            return false;
        }
        const std::uint64_t ext_end = h.end;
        const std::uint64_t avail = ext_end - cur;
        const std::uint32_t chunk =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(avail, max - done));
        const void* hit = std::memchr(H<const char>(cur), 0, chunk);
        if (hit) {
            *len = done + static_cast<std::uint32_t>(static_cast<const char*>(hit) -
                                                     H<const char>(cur));
            return true;
        }
        done += chunk;
    }
    *len = max;
    return true;
}

// Validates a guest range before host-side bulk access. Halts cleanly on violation.
bool Runtime::CheckRange(std::uint32_t guest, std::size_t size, bool write) {
    if (size == 0) {
        return true;
    }
    // Ranges must not wrap and must lie inside one mapping.
    const std::uint64_t end = std::uint64_t(guest) + size;
    if (end > 0x100000000ULL) {
        return false;
    }
    // Same epoch-checked last-hit cache as CheckAccess.
    // One thread_local (one TLS lookup), not four.
    thread_local struct {
        std::uint32_t s = 1, e = 0, epoch = 0;
        int p = 0;
    } hit;
    const std::uint32_t ep = mappings_.epoch();
    if (ep != hit.epoch || !(guest >= hit.s && end <= hit.e)) {
        const MappingTable::Hit h = mappings_.Find(guest, size);
        if (h.end == 0) {
            return false;
        }
        hit.s = h.start;
        hit.e = h.end;
        hit.p = h.prot;
        hit.epoch = ep;
    }
    return write ? ((hit.p & PROT_WRITE) != 0) : ((hit.p & PROT_READ) != 0);
}

void Runtime::Ret(Regs& regs, std::uint32_t r0, std::uint32_t r1) {
    regs[0] = r0;
    regs[1] = r1;
    // A guest SVC is an exception boundary: the import trampoline is ARM
    // code, so the caller's Thumb state must be restored here from LR bit 0.
    // Only the T bit changes (Jit::SetThumb, patches/dynarmic 0015: building
    // the whole CPSR and parsing it back cost ~1.5 % of a game thread on a
    // phone). The mode stays user: guest code starts in it (CallGuest) and
    // nothing changes it; NZCV/GE stay as the handler left them.
    const std::uint32_t lr = regs[14];
    Dynarmic::A32::Jit* jit = ActiveJit();
    if (jit == nullptr) {
        // No JIT attached: the unit test drives the dispatcher directly and
        // there is no CPSR to restore. (In every real run a JIT is set.)
        regs[15] = lr & ~std::uint32_t(1);
        return;
    }
    jit->SetThumb((lr & 1u) != 0);
    regs[15] = lr & ~std::uint32_t(1);
}

bool Runtime::IsCodeAddress(std::uint32_t va) {
    if (va >= kTrampBase && va < kTrampBase + kTrampSize) {
        return true;
    }
    const MappingTable::Hit h = mappings_.Find(va, 1);
    return h.end != 0 && (h.prot & PROT_EXEC) != 0;
}

// Bulk-write watchpoint: scan a just-written guest range for the WP value.
static std::int64_t WriteWP() {
    static std::int64_t wp = -2;
    if (wp == -2) {
        wp = -1;
        if (const char* e = std::getenv("HARNESS_WP")) {
            wp = static_cast<std::int64_t>(std::strtoul(e, nullptr, 16));
        }
    }
    return wp;
}
void ScanWP(Runtime* rt, std::uint32_t guest, std::size_t len, const char* what) {
    const std::int64_t wp = WriteWP();
    if (wp < 0 || len < 4) {
        return;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(wp);
    for (std::size_t i = 0; i + 4 <= len; i++) {
        std::uint32_t v = 0;
        std::memcpy(&v, rt->H(guest + static_cast<std::uint32_t>(i)), 4);
        if (v == w) {
            std::printf("[wp-bulk] %08x now at %08x via %s guest-pc=%08x\n", w,
                        guest + static_cast<std::uint32_t>(i), what, rt->jit()->Regs()[15]);
            return;
        }
    }
}

std::uint64_t Runtime::ThreadSvcCount() { return t_hot.svc_count; }

void Runtime::OnSvc(std::uint32_t swi, Regs& regs) {
#ifdef ABR_DIAG_COUNTERS
    // Read only by the [counters] line; in a shipping build this shared
    // counter would be one contended cache line on every guest->host call.
    svc_count.fetch_add(1, std::memory_order_relaxed);
#endif
    if (swi == kSwiOnce) {
        // The pthread_once init routine returned via ITS OWN trampoline (only
        // PthreadOnce() ever puts once_va in LR). Unambiguous by construction:
        // a thread exit or a qsort comparator return arrives as kSwiHalt and
        // can no longer be misread as "the init routine finished".
        if (once_depth_ <= 0) {
            std::printf("[once] LOUD kSwiOnce with no init in progress -- halting\n");
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        // Innermost hijacked once-routine finished. Mark its control done,
        // restore ITS caller context, resume after that once call. Outer
        // levels stay on the stack (an init routine may itself call
        // pthread_once on a different control).
        OnceHijack& top = once_stack_[once_depth_ - 1];
        const std::uint32_t halt_tid = GuestTid();
        if (halt_tid != top.owner_tid) {
            // Only reachable if the guest branched to once_va itself: our
            // trampoline address is never a valid guest return address.
            std::printf("[once] STEAL: control=%08x owner_tid=%u halt_tid=%u depth=%d "
                        "sp=%08x -- init return from a foreign thread\n",
                        top.control, top.owner_tid, halt_tid, once_depth_, regs[13]);
        }
        // Release: the init routine's stores must be visible to any thread
        // that sees "done" (PthreadOnce reads the word with acquire).
        __atomic_store_n(H<std::uint32_t>(top.control), 2u, __ATOMIC_RELEASE);
        for (int r = 0; r <= 12; r++) {
            regs[r] = top.saved_regs[r];
        }
        regs[13] = top.saved_regs[13];
        regs[14] = top.saved_regs[14];
        regs[0] = 0;  // pthread_once() return value
        // Resume AFTER the once call. The saved LR is a Thumb branch
        // return address (bit 0 = interworking marker), so PC must have
        // that bit stripped -- exactly like Ret(). Leaving it in PC makes
        // the JIT fetch from an odd address and mis-decode everything
        // after (Space init #9 died with a wild read that way).
        regs[15] = top.saved_regs[14] & ~std::uint32_t(1);
        // The hijack redirected pc without Ret(), leaving handler CPSR
        // (SVC mode). Rebuild exactly like Ret(): USER + caller T-bit from
        // LR, NZCV/GE preserved (thunks never set flags intentionally).
        {
            const std::uint32_t lr = top.saved_regs[14];
            std::uint32_t cpsr = ActiveJit()->Cpsr();
            cpsr = (cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10) |
                   ((lr & 1u) ? std::uint32_t(0x20) : std::uint32_t(0));
            ActiveJit()->SetCpsr(cpsr);
        }
        once_depth_--;
        // Normal event, not an anomaly: behind the flag, like the rest of the
        // diagnostic layer. The loud lines in this file (STEAL, still waiting,
        // stopped inside an init routine) stay unconditional on purpose.
        if (DiagOn()) {
            std::printf("[once] routine done, resumed (depth now %d) control=%08x tid=%u "
                        "owner_tid=%u\n",
                        once_depth_, top.control, halt_tid, top.owner_tid);
        }
        return;  // keep running, do NOT halt
    }
    if (swi == kSwiHalt) {
        // `svc #0` is BOTH our generic halt trampoline and the ARM EABI raw
        // syscall. Distinguish by PC: the trampoline lives at mod_.halt_va and
        // arrives there via `bl`; a guest syscall is issued somewhere in the
        // module. Treating the latter as a halt killed breakpad's sigaltstack.
        if (mod_.halt_va != 0 && regs[15] - 4 != mod_.halt_va) {
            OnGuestSyscall(regs);
            return;
        }
        halt_count++;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    // Must precede the JNI range: kSwiKuserBase > kSwiJniBase.
    if (swi >= kSwiKuserBase && swi < kSwiKuserBase + 8u) {
        OnKuser(swi - kSwiKuserBase, regs);
        return;
    }
    if (swi >= kSwiJniBase) {
        CallJni(swi - kSwiJniBase, regs);
        return;
    }
    const std::uint32_t idx = swi - 1;
    if (idx >= mod_.imports.size()) {
        std::printf("[svc] bad import swi=%u pc=%08x\n", swi, regs[15] - 4);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    // Entry trace: identifies the killer call if the thunk itself crashes.
    // Throttled per import to avoid spam from tight loops. HARNESS_CALLMAX=N:
    // also log the first N *total* import calls (first retry cycle anatomy).
    // Diagnostic layer (PLAN.md B1): skipped entirely under HARNESS_DIAG=0 --
    // this is a per-call "#trace:"+name std::string + map probe + mutex.
    if (DiagOn()) {
        // Locked briefly: worker threads share stub_hits_/call_total.
        // Printf + string formatting happen OUTSIDE the lock (was per-import
        // global-lock + string alloc + map probe on every call, print itself
        // only 3x per import).
        std::uint64_t h = 0, total = 0, cm = 0;
        bool lineal = false;
        std::string iname;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            h = ++stub_hits_["#trace:" + mod_.imports[idx].name];
            static std::uint64_t callmax = 0;
            static bool callmax_init = false;
            if (!callmax_init) {
                callmax_init = true;
                if (const char* e = std::getenv("HARNESS_CALLMAX")) {
                    callmax = static_cast<std::uint64_t>(std::strtoul(e, nullptr, 10));
                }
            }
            static std::uint64_t call_total = 0;
            // HARNESS_THREADLOG=1 keeps worker-thread logs; default main only.
            static bool threadlog = EnvOn("HARNESS_THREADLOG");
            lineal = (GuestTid() == 1) || threadlog;
            total = call_total++;
            cm = callmax;
            iname = mod_.imports[idx].name;
        }
        if ((h <= 3 || total < cm) && lineal) {
            std::printf("[call][t%u]%s %s(%08x %08x %08x %08x) lr=%08x\n", GuestTid(),
                        GuestTid() == 1 ? "" : "(T2+)", iname.c_str(), regs[0], regs[1],
                        regs[2], regs[3], regs[14]);
        }
        // HARNESS_LASTSVC=1: remember each worker's current import (parked-
        // SVC triage for stuck workers; read by ThreadCensus).
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            last_svc_idx_ = idx;
            last_svc_tid_ = GuestTid();
            static int lsen = -1;
            if (lsen < 0) {
                lsen = std::getenv("HARNESS_LASTSVC") ? 1 : 0;
            }
            if (lsen && GuestTid() != 1) {
                const auto it = threads_.find(GuestTid());
                if (it != threads_.end()) {
                    std::lock_guard<std::mutex> l(it->second->m);
                    it->second->last_svc = mod_.imports[idx].name;
                    // Ring history (last ~8 MEANINGFUL calls; housekeeping
                    // noise excluded so the job's real path survives).
                    static const char* noise[] = {
                        "poll",         "free",         "malloc",       "pthread_mutex_lock",
                        "pthread_mutex_unlock", "clock_gettime", "pthread_cond_signal",
                        "__aeabi_memcpy", "memcmp",     "memset",       "pthread_mutex_destroy",
                        "gettimeofday",  nullptr};
                    bool skip = false;
                    for (int k = 0; noise[k]; k++) {
                        if (mod_.imports[idx].name == noise[k]) {
                            skip = true;
                            break;
                        }
                    }
                    if (!skip) {
                        std::string& hst = it->second->svc_hist;
                        if (hst.size() < 320) {
                            if (!hst.empty()) {
                                hst += ",";
                            }
                            hst += mod_.imports[idx].name;
                        } else {
                            const std::size_t c = hst.find(',');
                            hst = (c == std::string::npos ? "" : hst.substr(c + 1) + ",") +
                                  mod_.imports[idx].name;
                        }
                    }
                }
            }
        }
    }
#ifdef ABR_DIAG_COUNTERS
    // Per-import calls and host time (diag builds only): which imports a
    // game leans on, and what one call costs. The arm64 virtual counter is
    // read directly (one instruction): clock_gettime is a ~390 ns syscall on
    // the sandbox (no vDSO), which swamped the numbers. Ticks -> ns at dump.
    if (idx < DiagCounters::kImportSlots) {
        const std::uint64_t t0 = DiagTicks();
        CallImport(idx, regs);
        const std::uint64_t t1 = DiagTicks();
        diag_.import_calls[idx].fetch_add(1, std::memory_order_relaxed);
        diag_.import_ns[idx].fetch_add(t1 - t0, std::memory_order_relaxed);
        return;
    }
#endif
    CallImport(idx, regs);
}

std::string Runtime::ExpandPrintf(const std::string& fmt, GuestFmtArgs& st,
                                 std::size_t* total_out) {
    auto next32 = [&]() -> std::uint32_t {
        std::uint32_t v = 0;
        if (st.via_va) {
            if (st.stack != 0 && CheckRange(st.stack, 4, false)) {
                std::memcpy(&v, H(st.stack), 4);
            }
            st.stack += 4;
            return v;
        }
        if (st.ai < 4) {
            return st.gargs[st.ai++];
        }
        if (CheckRange(st.stack, 4, false)) {
            std::memcpy(&v, H(st.stack), 4);
        }
        st.stack += 4;
        return v;
    };
    // Variadic double-word arguments (double, long long) follow AAPCS32:
    // they occupy an EVEN-numbered core register pair (r0:r1, r2:r3, ...).
    // If the next free register is odd, it is skipped; if the even pair is
    // no longer available, the argument goes on the stack at an 8-byte
    // aligned address. Low word first.
    //
    // Proven from the guests, not from a compiler guess. The WebP Xing
    // parser in Rio/Space/Star Wars calls
    //   fprintf(fp, "Note: Xing stream size %lu differs by %f%% ...",
    //           lu, d);
    // with the %lu in r2 and the double stored entirely at [sp]
    // (`vstr d8,[sp]`), r3 unused. Under base PCS the low word would be in
    // r3 and only the high word at [sp]. See also the even argument-passing
    // `vmov` pairs in all six guest libraries.
    auto next64 = [&]() -> std::uint64_t {
        if (st.via_va) {
            // va_list position: a double-word sits at an 8-byte aligned slot.
            st.stack = (st.stack + 7u) & ~7u;
            std::uint64_t v = 0;
            if (st.stack != 0 && CheckRange(st.stack, 8, false)) {
                std::memcpy(&v, H(st.stack), 8);
            }
            st.stack += 8;
            return v;
        }
        if (st.ai & 1) {
            st.ai++;  // AAPCS32: a double-word starts on an even register
        }
        if (st.ai + 2 <= 4) {
            const std::uint32_t lo = st.gargs[st.ai];
            const std::uint32_t hi = st.gargs[st.ai + 1];
            st.ai += 2;
            return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        }
        // No complete even pair left: the caller put the whole argument on
        // the stack. Once that happens, later arguments are on the stack too.
        st.ai = 4;
        st.stack = (st.stack + 7u) & ~7u;
        std::uint64_t v = 0;
        if (CheckRange(st.stack, 8, false)) {
            std::memcpy(&v, H(st.stack), 8);
        }
        st.stack += 8;
        return v;
    };

    // C99: snprintf returns the number of characters that WOULD have been
    // written, and the guests size their own buffers from that number. The
    // old expansion stopped at 4096 and reported the TRUNCATED length, so a
    // guest could cut its own buffer short. The text is still bounded (a
    // sanity limit on memory), but the length is counted past the bound.
    constexpr std::size_t kMaxExpand = 1u << 20;
    std::string out;
    std::size_t total = 0;
    auto emit = [&](const char* p, std::size_t n) {
        if (out.size() < kMaxExpand) {
            out.append(p, std::min(n, kMaxExpand - out.size()));
        }
        total += n;
    };
    std::size_t fi = 0;
    while (fi < fmt.size() && out.size() < kMaxExpand) {
        const char c = fmt[fi++];
        if (c != '%') {
            emit(&c, 1);
            continue;
        }
        if (fi >= fmt.size()) {
            out += '%';
            break;
        }
        if (fmt[fi] == '%') {
            fi++;
            emit("%", 1);
            continue;
        }
        std::string flags;
        for (; fi < fmt.size(); fi++) {
            const char f = fmt[fi];
            if (f == '-' || f == '+' || f == ' ' || f == '#' || f == '0' || f == '\'') {
                if (flags.find(f) == std::string::npos) {
                    flags += f;
                }
            } else {
                break;
            }
        }
        std::string width;
        if (fi < fmt.size() && fmt[fi] == '*') {
            fi++;
            const int w = (int)next32();
            if (w < 0) {
                if (flags.find('-') == std::string::npos) {
                    flags += '-';
                }
                width = std::to_string(-w);
            } else {
                width = std::to_string(w);
            }
        } else {
            while (fi < fmt.size() && fmt[fi] >= '0' && fmt[fi] <= '9') {
                width += fmt[fi++];
            }
        }
        std::string prec;
        if (fi < fmt.size() && fmt[fi] == '.') {
            fi++;
            prec = ".";
            if (fi < fmt.size() && fmt[fi] == '*') {
                fi++;
                prec += std::to_string((int)next32());
            } else {
                while (fi < fmt.size() && fmt[fi] >= '0' && fmt[fi] <= '9') {
                    prec += fmt[fi++];
                }
            }
        }
        // Length modifier. The guest is a 32-bit ARM build (ILP32, AAPCS32):
        //     int / long / ptrdiff_t / size_t = 4 bytes
        //     long long / intmax_t             = 8 bytes
        //     long double                      = double (8 bytes)
        // `abits` is the width of the ARGUMENT in the variadic area; `trunc`
        // how many bits of the value survive (the guest promotes short/char
        // to int before the call, so those arguments are still 4 bytes wide
        // and only the PRINTED value is narrowed); `nbytes` is the width %n
        // writes.
        int abits = 32;  // width of the argument
        int trunc = 0;   // 0 none, 8 char, 16 short
        int nbytes = 4;  // width written by %n
        while (fi < fmt.size()) {
            const char L = fmt[fi];
            if (L == 'h') {
                fi++;
                trunc = 16;
                nbytes = 2;
                if (fi < fmt.size() && fmt[fi] == 'h') {
                    fi++;
                    trunc = 8;
                    nbytes = 1;
                }
            } else if (L == 'l') {
                fi++;
                if (fi < fmt.size() && fmt[fi] == 'l') {
                    fi++;
                    abits = 64;  // long long
                } else {
                    abits = 32;  // long
                }
                nbytes = abits / 8;
            } else if (L == 'L') {
                fi++;
                abits = 64;  // long double == double here
                nbytes = 8;
            } else if (L == 'z') {
                fi++;
                abits = 32;  // size_t
                nbytes = 4;
            } else if (L == 't') {
                fi++;
                abits = 32;  // ptrdiff_t
                nbytes = 4;
            } else if (L == 'j') {
                fi++;
                abits = 64;  // intmax_t
                nbytes = 8;
            } else {
                break;
            }
        }
        const std::string spec = flags + width + prec;
        if (fi >= fmt.size()) {  // truncated spec: emit it as text
            out += '%';
            out += spec;
            break;
        }
        const char conv = fmt[fi++];
        // The rendered conversion can be long (a %s of any length), so the
        // buffer is sized for it: the old fixed 512 bytes silently cut a
        // long string. The buffer is REUSED across conversions (a
        // thread_local vector, not a fresh allocation per conversion: this
        // is the logging path and it is called often).
        thread_local static std::vector<char> buf;
        if (buf.size() < 512) {
            buf.resize(512);
        }
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            if (abits == 64) {
                v = static_cast<long long>(next64());
            } else {
                std::int32_t w = static_cast<std::int32_t>(next32());
                if (trunc == 8) {
                    w = static_cast<std::int8_t>(w);
                } else if (trunc == 16) {
                    w = static_cast<std::int16_t>(w);
                }
                v = w;
            }
            std::snprintf(buf.data(), buf.size(), ("%" + spec + "lld").c_str(), v);
            emit(buf.data(), std::strlen(buf.data()));
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long v;
            if (abits == 64) {
                v = static_cast<unsigned long long>(next64());
            } else {
                std::uint32_t w = static_cast<std::uint32_t>(next32());
                if (trunc == 8) {
                    w = static_cast<std::uint8_t>(w);
                } else if (trunc == 16) {
                    w = static_cast<std::uint16_t>(w);
                }
                v = w;
            }
            std::snprintf(buf.data(), buf.size(), ("%" + spec + "ll" + conv).c_str(), v);
            emit(buf.data(), std::strlen(buf.data()));
            break;
        }
        case 'p': {
            std::snprintf(buf.data(), buf.size(), ("%" + spec + "llx").c_str(),
                          (unsigned long long)next32());
            emit("0x", 2);
            emit(buf.data(), std::strlen(buf.data()));
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            const std::uint64_t bits = next64();
            double d = 0;
            std::memcpy(&d, &bits, 8);
            std::snprintf(buf.data(), buf.size(), ("%" + spec + conv).c_str(), d);
            emit(buf.data(), std::strlen(buf.data()));
            break;
        }
        case 'c':
            std::snprintf(buf.data(), buf.size(), ("%" + spec + "c").c_str(), (int)(next32() & 0xFF));
            emit(buf.data(), std::strlen(buf.data()));
            break;
        case 's': {
            const char* s = GuestStr(next32());
            const std::string sv = s ? s : "";
            // The width can pad the result, so the buffer has to fit the
            // string AND the padding.
            const std::size_t wpad = width.empty() ? 0 : static_cast<std::size_t>(std::atoll(width.c_str()));
            if (buf.size() < sv.size() + wpad + 64) {
                buf.resize(sv.size() + wpad + 64);
            }
            std::snprintf(buf.data(), buf.size(), ("%" + spec + "s").c_str(), sv.c_str());
            emit(buf.data(), std::strlen(buf.data()));
            break;
        }
        case 'n': {
            const std::uint32_t p = next32();
            // %n writes the number of characters written so far; the width
            // follows the length modifier (hh 1, h 2, ll/j 8, else 4). The
            // range check must match the width actually written: it used to
            // validate 4 bytes and then write 8.
            if (p != 0 && CheckRange(p, nbytes, true)) {
                if (nbytes == 8) {
                    const std::uint64_t n = total;
                    std::memcpy(H(p), &n, 8);
                } else if (nbytes == 4) {
                    const std::uint32_t n = (std::uint32_t)total;
                    std::memcpy(H(p), &n, 4);
                } else if (nbytes == 2) {
                    const std::uint16_t n = (std::uint16_t)total;
                    std::memcpy(H(p), &n, 2);
                } else {
                    const std::uint8_t n = (std::uint8_t)total;
                    std::memcpy(H(p), &n, 1);
                }
            }
            break;
        }
        default:
            // An invalid conversion is printed verbatim (like glibc) and
            // consumes no argument.
            emit("%", 1);
            emit(spec.c_str(), spec.size());
            emit(&conv, 1);
            break;
        }
    }
    if (total_out) {
        *total_out = total;
    }
    return out;
}

// Fast path for the math imports (fast_math.h): the id was resolved once at
// load time, so a call is one indexed jump instead of the import-name chain.
// Softfp as in math_thunks.inc: float in r0, doubles in r0:r1 and r2:r3.
void Runtime::CallFastMath(std::uint8_t id, Regs& regs, std::uint32_t a0, std::uint32_t a1,
                           std::uint32_t a2, std::uint32_t a3) {
    const FastMathEntry& e = kFastMath[id - 1u];
    auto f32 = [](std::uint32_t b) {
        float v = 0;
        std::memcpy(&v, &b, 4);
        return v;
    };
    auto f64 = [](std::uint32_t lo, std::uint32_t hi) {
        double v = 0;
        const std::uint32_t w[2] = {lo, hi};
        std::memcpy(&v, w, 8);
        return v;
    };
    auto ret32 = [&](float v) {
        std::uint32_t b = 0;
        std::memcpy(&b, &v, 4);
        Ret(regs, b);
    };
    auto ret64 = [&](double v) {
        std::uint32_t w[2] = {0, 0};
        std::memcpy(w, &v, 8);
        Ret(regs, w[0], w[1]);
    };
    switch (e.kind) {
    case MathKind::F_F:
        ret32(e.f_f(f32(a0)));
        break;
    case MathKind::D_D:
        ret64(e.d_d(f64(a0, a1)));
        break;
    case MathKind::FF_F:
        ret32(e.ff_f(f32(a0), f32(a1)));
        break;
    case MathKind::DD_D:
        ret64(e.dd_d(f64(a0, a1), f64(a2, a3)));
        break;
    }
}

// Bionic 7.1.2's C-locale character classes: U=0x01 L=0x02 N=0x04 S=0x08
// P=0x10 C=0x20 X=0x40 B=0x80. The X/B order is easy to get backwards; it is
// pinned by libc/include/ctype.h at android-7.1.2_r39 (NOT U L N S P C B X).
// ONE source of truth for the byte table the guest reads through its
// `_ctype_` data import and for iswctype(): the guests build their OWN
// classification tables from these answers at init (measured in the Star
// Wars guest: a 256-entry btowc table and a 16-entry wctype table), so
// the two must agree -- a wrong answer poisons those tables silently.
static std::uint8_t BionicCtypeClass(int c) {
    std::uint8_t b = 0;
    const bool up = (c >= 'A' && c <= 'Z'), lo = (c >= 'a' && c <= 'z');
    const bool dg = (c >= '0' && c <= '9');
    // Bionic's _ctype_ marks only the LETTERS with _X; digits carry _N and
    // isxdigit() tests _N|_X. Setting _X on digits is benign for the bool
    // test but makes the byte table (and the exact return value) wrong.
    const bool xd = (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    const bool sp = (c == ' ' || (c >= '\t' && c <= '\r'));
    // _B is part of the printable mask _CTYPE_R (NDK ctype.h:65), so only
    // SPACE carries it; TAB is _C|_S (BSD ctype_.c). isblank() is a separate
    // explicit SPACE/TAB test (iswctype case 3).
    const bool bl = (c == ' ');
    const bool ct = (c < 0x20 || c == 0x7F);
    const bool gr = (c > 0x20 && c < 0x7F);
    if (up) {
        b |= 0x01;
    }
    if (lo) {
        b |= 0x02;
    }
    if (dg) {
        b |= 0x04;
    }
    if (sp) {
        b |= 0x08;
    }
    if (gr && !up && !lo && !dg) {
        b |= 0x10;
    }
    if (ct) {
        b |= 0x20;
    }
    if (xd) {
        b |= 0x40;
    }
    if (bl) {
        b |= 0x80;
    }
    return b;
}

void Runtime::CallImport(std::uint32_t idx, Regs& regs) {
    const Import& imp = mod_.imports[idx];
    const std::string& n = imp.name;
    const ImportId id = imp.import_id;
    using I = ImportId;
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    // GL fast path. gl* calls are the hottest imports (12-20 per draw): send
    // them to the real driver before the long name chain below, and without
    // the runtime's mutex_ (RealGL has its own lock). Unhandled names and
    // threads without a GL context fall through to the fake shim.
    if (n.size() > 2 && n[0] == 'g' && n[1] == 'l' &&
        RealGLBackend()->Handle(imp.gl_id, n.c_str(), regs, a0, a1, a2, a3)) {
        return;
    }
    // OpenSL ES (opensl.cpp): slCreateEngine and the "SL:" interface methods,
    // names outside the import id table.
    if (id == I::None && !n.empty() && (n[0] == 'S' || n[0] == 's') && OpenSlCall(n, regs)) {
        return;
    }
    // TEMP-DIAG (worker hang): unthrottled socket-family logging (global
    // budgets hide worker calls). Diagnostic layer (PLAN.md A3): gated; with
    // DIAG off this also stops paying the ~17 string compares per call.
    if (DiagOn() &&
        (id == I::socket_ || id == I::connect_ || id == I::send_ || id == I::sendto_ || id == I::recv_ ||
         id == I::recvfrom_ || id == I::bind_ || id == I::listen_ || id == I::setsockopt_ ||
         id == I::shutdown_)) {
        std::printf("[netsock][t%u]%s %s(%08x %08x %08x %08x) lr=%08x\n", GuestTid(),
                    GuestTid() == 1 ? "" : "(T2+)", n.c_str(), a0, a1, a2, a3, regs[14]);
    }

    // DIAG-only audit: record every distinct guest import actually CALLED.
    // Used to find host-side service gaps of the kind that broke Space (a
    // missing strtod left luaO_str2d's endptr uninitialised, so Lua's
    // string->number coercion mis-reported and drawSprite picked the wrong
    // argument branch). Off in prod.
    if (DiagOn()) {
        static std::vector<std::string> seen_imports;
        bool known = false;
        for (const std::string& s : seen_imports) {
            if (s == n) {
                known = true;
                break;
            }
        }
        if (!known && seen_imports.size() < 900) {
            seen_imports.push_back(n);
            std::printf("[import] first call: %s\n", n.c_str());
        }
    }

    // Fast path first (after the diagnostic bookkeeping above, so the
    // "first call" audit still sees these imports): the math functions the
    // guest hammers in loops are dispatched by their load-time id instead of
    // the import-name chain below.
    if (imp.fast_math != 0) {
        CallFastMath(imp.fast_math, regs, a0, a1, a2, a3);
        return;
    }
    switch (imp.hot) {
        case HotImport::TlsGet:
            BindTls();
            Ret(regs, PthreadGetSpecific(a0));
            return;
        case HotImport::TlsSet:
            Ret(regs, PthreadSetSpecific(a0, a1));
            return;
        case HotImport::MutexLock:
        case HotImport::MutexUnlock:
            Ret(regs, PthreadMutex(id, n, a0, a1));
            return;
        case HotImport::MemCopy:
            if (!CheckRange(a0, a2, true) || !CheckRange(a1, a2, false)) {
                goto bad_ptr;
            }
            GuestMemCopy(id, regs, n, a0, a1, a2);
            return;
        case HotImport::MemSet:
            if (!CheckRange(a0, a2, true)) {
                goto bad_ptr;
            }
            GuestMemSet(id, regs, n, a0, static_cast<int>(a1 & 0xFF), a2);
            return;
        case HotImport::MemSetAeabi:
            if (!CheckRange(a0, a1, true)) {
                goto bad_ptr;
            }
            GuestMemSet(id, regs, n, a0, static_cast<int>(a2 & 0xFF), a1);
            return;
        case HotImport::MemClr:
            if (!CheckRange(a0, a1, true)) {
                goto bad_ptr;
            }
            GuestMemSet(id, regs, n, a0, 0, a1);
            return;
        case HotImport::None:
            break;
    }

    // --- memory management ---
    // Direct jump to the first handler that can take this import (labels
    // placed by script, 2026-11-04: before the first statement that names the
    // import, or that could take any import -- prefix rules, generic tests).
    // Every handler after a label runs as in the chain; an unknown id starts
    // at the top. No case breaks: falling on is the chain.
    switch (id) {
    default:
    case I::calloc_:
    case I::malloc_:
    case I::realloc_:
    if (id == I::malloc_ || id == I::calloc_ || id == I::realloc_) {
        const std::uint32_t r = (id == I::malloc_)   ? GuestMalloc(a0)
                                : (id == I::calloc_) ? GuestCalloc(a0, a1)
                                                  : GuestRealloc(a0, a1);
        // realloc(p, 0) frees and legitimately returns NULL.
        if (r == 0 && !(id == I::realloc_ && a1 == 0)) {
            SetGuestErrno(ENOMEM);
        }
        Ret(regs, r);
        return;
    }
    case I::cfree_:
    case I::free_:
    if (id == I::free_ || id == I::cfree_) {
        GuestFree(a0);
        RetVoid(regs);
        return;
    }
    case I::madvise_:
    if (id == I::madvise_) {
        // madvise(addr, len, advice) on guest memory. MADV_DONTNEED (4) must
        // keep its meaning for private anonymous memory -- the range reads
        // back as zeros -- so it is honoured exactly: by the host kernel when
        // its pages are the guest's 4 KiB, else by zeroing the range (a
        // 16 KiB-page host would drop more than asked). Every other advice is
        // a hint and is accepted.
        if ((a0 & 0xFFFu) != 0) {
            SetGuestErrno(EINVAL);
            Ret(regs, static_cast<std::uint32_t>(-1));
            return;
        }
        const std::uint32_t len = (a1 + 0xFFFu) & ~0xFFFu;
        if (a2 == MADV_DONTNEED && len != 0) {
            if (!CheckRange(a0, len, true)) {
                SetGuestErrno(ENOMEM);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (::sysconf(_SC_PAGESIZE) != 4096 || ::madvise(H(a0), len, MADV_DONTNEED) != 0) {
                std::memset(H(a0), 0, len);
            }
        }
        Ret(regs, 0);
        return;
    }
    case I::mmap_:
    case I::mmap64_:
    if (id == I::mmap_ || id == I::mmap64_) {
        // mmap(addr, len, prot, flags, int fd, off_t off): fd at [sp], the
        // 32-bit off_t at [sp+4]. mmap64's off64_t is 8-aligned at [sp+8].
        const bool m64 = (id == I::mmap64_);
        if (!CheckRange(regs[13], m64 ? 16 : 8, false)) {
            goto bad_ptr;
        }
        std::int32_t gfd = 0;
        std::memcpy(&gfd, H(regs[13]), 4);
        std::uint64_t off = 0;
        if (m64) {
            std::memcpy(&off, H(regs[13] + 8), 8);
        } else {
            std::uint32_t o32 = 0;
            std::memcpy(&o32, H(regs[13] + 4), 4);
            off = o32;  // off_t is signed, but a negative offset is EINVAL anyway
            if (static_cast<std::int32_t>(o32) < 0) {
                off = ~std::uint64_t(0);
            }
        }
        int hfd = -1;
        if (!(a3 & MAP_ANONYMOUS) && gfd >= 0) {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = fd_map_.find(static_cast<std::uint32_t>(gfd));
            if (it != fd_map_.end()) {
                hfd = it->second;
            }
        }
        // A packed file maps its window of the APK; the packer aligns each
        // entry to 16 KiB, so a page-aligned file offset stays page-aligned.
        // Otherwise (or for a range past the file's end) a private copy.
        if (const std::shared_ptr<abvfs::Slice> sl =
                (hfd >= 0 && off != ~std::uint64_t(0)) ? FdSlice(static_cast<std::uint32_t>(gfd))
                                                       : nullptr) {
            const std::uint64_t at = sl->off + off;
            const std::uint64_t page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
            if (at % page == 0 && off + a1 <= sl->size) {
                Ret(regs, GuestMmap(a0, a1, static_cast<int>(a2), static_cast<int>(a3), hfd, at));
                return;
            }
            const int flags = (static_cast<int>(a3) & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
            const std::uint32_t g = GuestMmap(a0, a1, static_cast<int>(a2) | PROT_WRITE, flags, -1, 0);
            if (g != 0xFFFFFFFFu && CheckRange(g, a1, true)) {
                abvfs::SlicePread(*sl, hfd, H(g), a1, off);  // past the end stays zero
            }
            Ret(regs, g);
            return;
        }
        Ret(regs, GuestMmap(a0, a1, static_cast<int>(a2), static_cast<int>(a3), hfd, off));
        return;
    }
    case I::mremap_:
    if (id == I::mremap_) {
        // mremap(old, old_size, new_size, flags): a shrink drops the tail; a
        // growth extends in place when the following pages are free, else
        // moves under MREMAP_MAYMOVE (1): a new mapping with the same
        // protection, the contents copied, the old pages released.
        // MREMAP_FIXED is not offered (EINVAL).
        const std::uint32_t old_len = (a1 + 0xFFFu) & ~0xFFFu, new_len = (a2 + 0xFFFu) & ~0xFFFu;
        if ((a0 & 0xFFFu) != 0 || new_len == 0 || old_len == 0 || (a3 & ~1u) != 0) {
            SetGuestErrno(EINVAL);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        const MappingTable::Hit h = mappings_.Find(a0, old_len);
        if (h.end == 0 || !MappingTable::InArena(a0, std::uint64_t(a0) + old_len)) {
            SetGuestErrno(EFAULT);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        if (new_len <= old_len) {
            if (new_len < old_len) {
                GuestMunmap(a0 + new_len, old_len - new_len);
            }
            Ret(regs, a0);
            return;
        }
        const std::uint64_t grow_end = std::uint64_t(a0) + new_len;
        if (mappings_.ArenaFree(a0 + old_len, grow_end)) {
            const std::uint32_t r = GuestMmap(a0 + old_len, new_len - old_len, h.prot,
                                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            Ret(regs, r == 0xFFFFFFFFu ? r : a0);
            return;
        }
        if (!(a3 & 1u)) {
            SetGuestErrno(ENOMEM);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        const std::uint32_t moved = GuestMmap(0, new_len, h.prot | PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (moved == 0xFFFFFFFFu) {
            Ret(regs, moved);
            return;
        }
        std::memcpy(H(moved), H(a0), old_len);
        GuestMprotect(moved, new_len, h.prot);
        GuestMunmap(a0, old_len);
        Ret(regs, moved);
        return;
    }
    case I::munmap_:
    if (id == I::munmap_) {
        Ret(regs, static_cast<std::uint32_t>(GuestMunmap(a0, a1)));
        return;
    }
    case I::strdup_:
    case I::strndup_:
    if (id == I::strdup_ || id == I::strndup_) {
        std::uint32_t len = 0;
        if (!GuestStrnlen(a0, id == I::strndup_ ? a1 : 0xFFFFFFFFu, &len) || len == 0xFFFFFFFFu) {
            goto bad_ptr;
        }
        if (DiagOn()) {
            static int sdn = 0;
            if (sdn < 20) {
                sdn++;
                std::printf("[strdup] %08x \"%.48s\"\n", a0,
                            std::string(H<const char>(a0), std::min<std::uint32_t>(len, 48)).c_str());
            }
        }
        const std::uint32_t out = GuestMalloc(len + 1);
        if (out) {
            std::memcpy(H(out), H<const char>(a0), len);
            *H<std::uint8_t>(out + len) = 0;
        } else {
            SetGuestErrno(ENOMEM);
        }
        Ret(regs, out);
        return;
    }
    // --- libc odds and ends the guests actually call -------------------
    // Measured on the guests' disassembly (Seasons): strcoll x3, memmem x6,
    // sysconf x2, getpid x7, syscall x5 (224 gettid x4, 268 tgkill x1).
    // Before this they all fell into the generic tail, which answers 0 --
    // "the two strings are equal", "not found", "0 bytes", "thread id 0".
    case I::strcoll_:
    if (id == I::strcoll_) {
        // The guest puts itself into the "C" locale (it calls setlocale("C")
        // itself), where the collation is the byte order -- so the C
        // library's strcoll in the harness' own locale is the guest's
        // answer. (GuestStr hands out a shared scratch buffer, so both
        // strings have to be copied before the second call.)
        const std::string sa(GuestStr(a0));
        const std::string sb(GuestStr(a1));
        Ret(regs, static_cast<std::uint32_t>(std::strcoll(sa.c_str(), sb.c_str())));
        return;
    }
    case I::memmem_:
    if (id == I::memmem_) {
        // memmem(haystack, haylen, needle, needlelen) returns a POINTER
        // into the haystack -- a guest address here.
        if (!CheckRange(a0, a1, false) || !CheckRange(a2, a3, false)) {
            goto bad_ptr;
        }
        if (a3 == 0) {
            Ret(regs, a0);
            return;
        }
        if (a3 > a1) {
            Ret(regs, 0);
            return;
        }
        const std::uint8_t* hay = H<const std::uint8_t>(a0);
        const std::uint8_t* ndl = H<const std::uint8_t>(a2);
        const std::uint8_t* hit = std::search(hay, hay + a1, ndl, ndl + a3);
        // std::search returns last1 (not nullptr) when there is no match.
        if (hit == hay + a1) {
            Ret(regs, 0);
            return;
        }
        Ret(regs, static_cast<std::uint32_t>(a0 + (hit - hay)));
        return;
    }
    case I::sysconf_:
    if (id == I::sysconf_) {
        // The guest asks for the page size and friends. The _SC_* values
        // are the Linux ones on both ABIs, so the host's answer is the
        // guest's.
        Ret(regs, static_cast<std::uint32_t>(::sysconf(static_cast<int>(a0))));
        return;
    }
    case I::getpid_:
    if (id == I::getpid_) {
        Ret(regs, static_cast<std::uint32_t>(::getpid()));
        return;
    }
    case I::btowc_:
    case I::iswctype_:
    case I::wctob_:
    case I::wctype_:
    if (id == I::btowc_ || id == I::wctob_ || id == I::wctype_ || id == I::iswctype_) {
        // The wide-character ctype (the C locale, like the byte one).
        // The guest builds its own classification tables from these answers
        // at init (measured: Star Wars fills a 256-entry btowc table and a
        // 16-entry wctype table), so the answers must be the bionic ones --
        // answering 0 silently produces an all-zero table (no letter, no
        // space, no digit recognized) and the text comes out wrong.
        if (id == I::wctype_) {
            // Bionic 7.1.2: wctype(const char*) maps a property NAME to the
            // WC_TYPE_* enum 1..12; it is not a character classifier. The
            // order is the one in libc/include/wchar.h.
            static const char* const kProperties[] = {
                "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                "lower", "print", "punct", "space", "upper", "xdigit"};
            // Do not pass this through GuestStr(): it silently returns "" for
            // an unreadable pointer or a string with no NUL. The guest builds
            // its ctype tables from this answer, so a silent miss poisons them.
            // A property name is at most 6 bytes; 64 is a sanity bound, not a
            // format limit. An unreadable/unterminated name is a guest bug:
            // report it LOUD and stop rather than answer 0 as if it were
            // merely an unknown property. NULL is checked separately because
            // guest address 0 is mapped (the ELF header lives there).
            constexpr std::uint32_t kMaxPropertyLen = 64;
            auto loud_bad_name = [&](const char* why, std::uint32_t off) {
                std::printf("[ctype] LOUD wctype %s at %08x+%u -- halting\n", why, a0, off);
                if (Dynarmic::A32::Jit* jit = ActiveJit()) {
                    jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                }
            };
            if (a0 == 0) {
                loud_bad_name("NULL property", 0);
                Ret(regs, 0);
                return;
            }
            std::uint32_t len = 0;
            for (;;) {
                if (len == kMaxPropertyLen) {
                    loud_bad_name("unterminated property", len);
                    Ret(regs, 0);
                    return;
                }
                const std::uint64_t at = std::uint64_t(a0) + len;
                if (at > 0xFFFFFFFFu ||
                    !CheckAccess(static_cast<std::uint32_t>(at), 1, false, false)) {
                    loud_bad_name("unreadable property", len);
                    Ret(regs, 0);
                    return;
                }
                if (*H<char>(static_cast<std::uint32_t>(at)) == '\0') {
                    break;
                }
                ++len;
            }
            // Every byte below has been checked, so the bounded compare is safe.
            for (std::uint32_t i = 0; i < 12; ++i) {
                const char* name = kProperties[i];
                if (std::strlen(name) == len &&
                    std::memcmp(H<char>(a0), name, len) == 0) {
                    Ret(regs, i + 1);
                    return;
                }
            }
            Ret(regs, 0);
            return;
        }
        if (id == I::iswctype_) {
            // Bionic 7.1.2: iswctype(wint_t, wctype_t) switches on the enum
            // and calls the narrow classifier. Those classifiers return the
            // class MASK, not merely 1 (isblank is the one explicit 0/1
            // case). The guests only test zero/nonzero, but returning the
            // same value pins the bit order and catches a swapped X/B.
            const std::uint32_t wc = a0;
            const std::uint32_t cls =
                wc < 128 ? BionicCtypeClass(static_cast<int>(wc)) : 0u;
            std::uint32_t r = 0;
            switch (a1) {
            case 1:  // alnum: isalnum() -> U|L|N
                r = cls & (0x01 | 0x02 | 0x04);
                break;
            case 2:  // alpha: isalpha() -> U|L
                r = cls & (0x01 | 0x02);
                break;
            case 3:  // blank: isblank() is the explicit SPACE/TAB test
                r = (wc == ' ' || wc == '\t') ? 1u : 0u;
                break;
            case 4:  // cntrl: iscntrl() -> C
                r = cls & 0x20;
                break;
            case 5:  // digit: isdigit() -> N
                r = cls & 0x04;
                break;
            case 6:  // graph: isgraph() -> P|U|L|N
                r = cls & (0x10 | 0x01 | 0x02 | 0x04);
                break;
            case 7:  // lower: islower() -> L
                r = cls & 0x02;
                break;
            case 8:  // print: isprint() -> P|U|L|N|B
                r = cls & (0x10 | 0x01 | 0x02 | 0x04 | 0x80);
                break;
            case 9:  // punct: ispunct() -> P
                r = cls & 0x10;
                break;
            case 10:  // space: isspace() -> S
                r = cls & 0x08;
                break;
            case 11:  // upper: isupper() -> U
                r = cls & 0x01;
                break;
            case 12:  // xdigit: isxdigit() -> N|X
                r = cls & (0x04 | 0x40);
                break;
            default:
                r = 0;
                break;
            }
            Ret(regs, r);
            return;
        }
        if (id == I::btowc_) {
            // btowc(int c): the wide char for a single byte, WEOF (-1) when
            // the locale cannot represent it (C locale: ASCII only).
            const std::uint32_t c = a0;
            Ret(regs, c < 128 ? c : 0xFFFFFFFFu);
            return;
        }
        // wctob(wint_t wc): the byte for a wide char, EOF (-1) when there is
        // none. The guest loops `while (wctob(c) + 1 != 0)`, i.e. it stops
        // on EOF, so -1 (not 0) is the correct "no byte" answer.
        const std::uint32_t wc = a0;
        Ret(regs, wc < 128 ? wc : 0xFFFFFFFFu);
        return;
    }
    case I::getauxval_:
    if (id == I::getauxval_) {
        // The measured Star Wars consumer sets r0=AT_HWCAP at 0x6708e4,
        // calls getauxval at 0x6708e6, shifts the result left by 19 (tests
        // bit 12), and on set reads AT_HWCAP2 at 0x6708f0. On ARM32 bit 12
        // is NEON, not
        // FAST_MULT; the old label was copied from the arm64 layout. Keep
        // NEON set because the guest takes that path and Dynarmic implements
        // ASIMD. HWCAP2 stays zero: no crypto extensions are claimed.
        //
        // The host is arm64, so its own AT_HWCAP cannot be forwarded: the
        // bit positions mean different things. Answer the armv7-a baseline
        // the guests were built for, with only bits this process actually
        // provides. The old mask advertised invented FP/VFPv3_D32 features
        // and set eight false bits.
        switch (a0) {
        case 16: {  // AT_HWCAP
            Ret(regs, kArmV7Hwcap);
            return;
        }
        case 26:  // AT_HWCAP2: no crypto extensions claimed
            Ret(regs, 0);
            return;
        case 6:   // AT_PAGESZ
        case 30:  // AT_PAGESIZE (the arm64 name for the same thing)
            Ret(regs, static_cast<std::uint32_t>(::sysconf(_SC_PAGESIZE)));
            return;
        case 11:  // AT_UID
            Ret(regs, static_cast<std::uint32_t>(::getuid()));
            return;
        case 12:  // AT_EUID
            Ret(regs, static_cast<std::uint32_t>(::geteuid()));
            return;
        case 13:  // AT_GID
            Ret(regs, static_cast<std::uint32_t>(::getgid()));
            return;
        case 14:  // AT_EGID
            Ret(regs, static_cast<std::uint32_t>(::getegid()));
            return;
        case 17:  // AT_CLKTCK
            Ret(regs, 100);
            return;
        case 15: {  // AT_PLATFORM: an arm32 Android device answers "v7l"
            // The value is a POINTER into the guest's address space, so the
            // string has to live there (one allocation, cached).
            if (platform_va_ == 0) {
                platform_va_ = GuestMalloc(8);
                if (platform_va_ != 0) {
                    static const char kPlatform[] = "v7l";
                    std::memcpy(H(platform_va_), kPlatform, sizeof(kPlatform));
                }
            }
            Ret(regs, platform_va_);
            return;
        }
        default:
            // An unknown type: the kernel answers 0 (and sets errno).
            Ret(regs, 0);
            return;
        }
    }
    case I::syscall_:
    if (id == I::syscall_) {
        // The raw syscall() wrapper. The guests' call sites pass only two
        // numbers (measured on the Seasons disassembly): 224 = gettid and
        // 268 = tgkill.
        if (a0 == 224) {  // __NR_gettid
            Ret(regs, GuestTid());
            return;
        }
        if (a0 == 268) {  // __NR_tgkill
            // The guest signals ITSELF (pid = getpid, tid = gettid). The
            // harness has no per-guest-thread signal delivery, and the
            // guest's sigaction is a no-op, so a delivered signal would take
            // the whole app down instead of reaching the guest's handler.
            // Report success, like the generic tail did.
            Ret(regs, 0);
            return;
        }
        Ret(regs, static_cast<std::uint32_t>(-ENOSYS));
        return;
    }
    // --- setjmp / longjmp -----------------------------------------------
    // The jmp_buf belongs to the guest, but its LAYOUT is ours: the guest
    // only ever hands the pointer back to longjmp. The guests keep the
    // buffer on their stack (add r0, sp, #8..#24 inside a 268..284 byte
    // frame) and call longjmp(buf, 1).
    // Saved, like bionic's arm setjmp: r4-r11 (callee-saved), sp, the return
    // address, then the VFP callee-saved d8-d15 (s16-s31) and FPSCR:
    // 10 + 16 + 1 words = 108 bytes, inside bionic's 64-word arm jmp_buf.
    // softfp only changes how arguments are passed; guest code keeps values
    // in d8-d15 across calls (e.g. vstr d8/d13 in the WebP fprintf callers),
    // and Lua's error path is a longjmp.
    case I::__sigsetjmp_:
    case I::_setjmp_:
    case I::setjmp_:
    case I::sigsetjmp_:
    if (id == I::setjmp_ || id == I::_setjmp_ || id == I::sigsetjmp_ || id == I::__sigsetjmp_) {
        if (!CheckRange(a0, kGuestJmpBufBytes, true)) {
            goto bad_ptr;
        }
        std::uint32_t* buf = H<std::uint32_t>(a0);
        for (int i = 0; i < 8; i++) {
            buf[i] = regs[4 + i];  // r4..r11
        }
        buf[8] = regs[13];        // sp
        buf[9] = regs[14];        // lr: where the guest continues after longjmp
        if (Dynarmic::A32::Jit* jit = ActiveJit()) {
            const auto& ext = jit->ExtRegs();
            for (int i = 0; i < 16; i++) {
                buf[10 + i] = ext[16 + i];  // s16..s31 = d8..d15
            }
            buf[26] = jit->Fpscr();
        }
        Ret(regs, 0);
        return;
    }
    case I::_longjmp_:
    case I::longjmp_:
    case I::siglongjmp_:
    if (id == I::longjmp_ || id == I::_longjmp_ || id == I::siglongjmp_) {
        if (!CheckRange(a0, kGuestJmpBufBytes, false)) {
            goto bad_ptr;
        }
        const std::uint32_t* buf = H<const std::uint32_t>(a0);
        for (int i = 0; i < 8; i++) {
            regs[4 + i] = buf[i];
        }
        regs[13] = buf[8];  // sp
        regs[14] = buf[9];  // the saved return address: Ret() jumps there
        if (Dynarmic::A32::Jit* jit = ActiveJit()) {
            auto& ext = jit->ExtRegs();
            for (int i = 0; i < 16; i++) {
                ext[16 + i] = buf[10 + i];
            }
            jit->SetFpscr(buf[26]);
        }
        // C99: longjmp(env, 0) must still make setjmp return 1.
        Ret(regs, a1 != 0 ? a1 : 1);
        return;
    }
    // --- libc odds and ends the guests actually call -------------------
    case I::gethostname_:
    if (id == I::gethostname_) {
        // gethostname(buf, len): POSIX returns 0 on SUCCESS and -1 on error
        // (bionic truncates and NUL-terminates the name). The guest uses
        // the return value AS A POINTER in one place ("if (r != 0) return
        // (char*)r"), so answering with the length -- as a first revision of
        // this handler did -- made it dereference 9 and the game's HTTP init
        // died with "Unable to open a cURL easy handle".
        if (a1 == 0) {
            Ret(regs, static_cast<std::uint32_t>(-1));
            return;
        }
        std::string name = "localhost";
        char host[256];
        if (::gethostname(host, sizeof(host)) == 0 && host[0] != '\0') {
            name = host;
        }
        const std::size_t n = std::min<std::size_t>(name.size(), (std::size_t)a1 - 1);
        if (!CheckRange(a0, n + 1, true)) {
            goto bad_ptr;
        }
        std::memcpy(H(a0), name.data(), n);
        H(a0)[n] = 0;
        Ret(regs, 0);
        return;
    }
    // --- raw memory (host implementations over translated pointers) ---
    // Every bulk access is range-checked first: a wild guest pointer must
    // halt cleanly, never segfault the host. memcpy/memmove/memset and their
    // __aeabi_ forms: HotImport.
    case I::bcmp_:
    case I::memcmp_:
    if (id == I::memcmp_ || id == I::bcmp_) {
        if (!CheckRange(a0, a2, false) || !CheckRange(a1, a2, false)) {
            goto bad_ptr;
        }
        Ret(regs, static_cast<std::uint32_t>(std::memcmp(H<const void>(a0), H<const void>(a1), a2)));
        return;
    }
    case I::strlen_:
    case I::strnlen_:
    if (id == I::strlen_ || id == I::strnlen_) {
        std::uint32_t len = 0;
        if (!GuestStrnlen(a0, id == I::strnlen_ ? a1 : 0xFFFFFFFFu, &len)) {
            goto bad_ptr;
        }
        Ret(regs, len);
        return;
    }
    case I::strcasecmp_:
    case I::strcmp_:
    case I::strncasecmp_:
    case I::strncmp_:
    if (id == I::strcmp_ || id == I::strncmp_ || id == I::strcasecmp_ || id == I::strncasecmp_) {
        // Compare in guest memory. Both strings are validated up to their NUL
        // (or up to n for the n-variants), so the host compare never reads an
        // unmapped byte.
        const bool bounded = (id == I::strncmp_ || id == I::strncasecmp_);
        const std::uint32_t lim = bounded ? a2 : 0xFFFFFFFFu;
        std::uint32_t la = 0, lb = 0;
        if (!GuestStrnlen(a0, lim, &la) || !GuestStrnlen(a1, lim, &lb)) {
            goto bad_ptr;
        }
        const char* pa = H<const char>(a0);
        const char* pb = H<const char>(a1);
        int r = 0;
        if (id == I::strcmp_) {
            r = std::strcmp(pa, pb);
        } else if (id == I::strncmp_) {
            r = std::strncmp(pa, pb, a2);
        } else if (id == I::strcasecmp_) {
            r = strcasecmp(pa, pb);
        } else {
            r = strncasecmp(pa, pb, a2);
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case I::strcat_:
    case I::strcpy_:
    case I::strncat_:
    case I::strncpy_:
    if (id == I::strcpy_ || id == I::strncpy_ || id == I::strcat_ || id == I::strncat_) {
        // All four work in guest memory. Source and destination are validated
        // for exactly the bytes the real function touches:
        //   strcpy:  src up to NUL; dst [0, slen]
        //   strcat:  dst up to its NUL (dlen); dst [dlen, dlen+slen]
        //   strncpy: src up to NUL or n; dst [0, n) (zero padded, no NUL
        //            added when src has >= n bytes)
        //   strncat: src up to NUL or n; dst [dlen, dlen+min(slen,n)] + NUL
        // ClampHeapWrite is the optional overflow firewall (off by default);
        // when it shortens a write the result is still NUL terminated.
        const bool cat = (id == I::strcat_ || id == I::strncat_);
        const bool bounded = (id == I::strncpy_ || id == I::strncat_);
        std::uint32_t slen = 0, dlen = 0;
        if (!GuestStrnlen(a1, bounded ? a2 : 0xFFFFFFFFu, &slen)) {
            goto bad_ptr;
        }
        if (cat && !GuestStrnlen(a0, 0xFFFFFFFFu, &dlen)) {
            goto bad_ptr;
        }
        const std::uint32_t dst = a0 + dlen;
        // Bytes written at dst: strncpy writes exactly n; the others copy the
        // string plus a terminating NUL.
        const std::uint64_t want = (id == I::strncpy_) ? std::uint64_t(a2) : std::uint64_t(slen) + 1;
        if (want > 0xFFFFFFFFu || !CheckRange(dst, static_cast<std::size_t>(want), true)) {
            goto bad_ptr;
        }
        const std::uint32_t room =
            ClampHeapWrite(dst, static_cast<std::uint32_t>(want), n.c_str());
        if (id == I::strncpy_) {
            const std::uint32_t copy = std::min(slen, room);
            std::memmove(H(dst), H<const void>(a1), copy);
            if (room > copy) {
                std::memset(H(dst + copy), 0, room - copy);
            }
            if (room < want && room > 0) {
                *H<char>(dst + room - 1) = '\0';  // firewall truncation
            }
        } else if (room > 0) {
            const std::uint32_t copy = std::min(slen, room - 1);
            std::memmove(H(dst), H<const void>(a1), copy);
            *H<char>(dst + copy) = '\0';
        }
        Ret(regs, a0);
        return;
    }
    // --- string search/scan family (host-forwarded, range-checked) ---
    case I::memchr_:
    case I::memrchr_:
    if (id == I::memchr_ || id == I::memrchr_) {
        if (!CheckRange(a0, a2, false)) {
            goto bad_ptr;
        }
        // HARNESS_MEMDUMP=<hexlr>: hex+ascii dump of the scanned buffer when
        // the caller's lr matches (JSON-parse forensics: input lives on the
        // heap and drifts between runs due to worker-thread racing, so
        // post-mortem dumps are unreliable — capture at call time).
        // Read once: memchr is hot (JSON/string parsing) and getenv is a
        // linear scan of environ.
        static const std::int64_t memdump_lr = [] {
            const char* e = std::getenv("HARNESS_MEMDUMP");
            return e ? static_cast<std::int64_t>(std::strtoul(e, nullptr, 16)) : std::int64_t(-1);
        }();
        if (memdump_lr >= 0) {
            const std::uint32_t mlr = static_cast<std::uint32_t>(memdump_lr);
            if (regs[14] == mlr && a2 <= 0x10000) {
                std::printf("[memdump] %s(%08x,'%c',%u) lr=%08x:\n", n.c_str(), a0,
                            (a1 & 0xFF) >= 32 ? (char)(a1 & 0xFF) : '?', a2, regs[14]);
                const std::uint8_t* bp = H<const std::uint8_t>(a0);
                for (std::uint32_t i = 0; i < a2; i += 16) {
                    std::printf("[memdump] %08x:", a0 + i);
                    for (std::uint32_t j = i; j < i + 16 && j < a2; j++) {
                        std::printf(" %02x", bp[j]);
                    }
                    std::printf("  |");
                    for (std::uint32_t j = i; j < i + 16 && j < a2; j++) {
                        std::printf("%c", (bp[j] >= 32 && bp[j] < 127) ? (char)bp[j] : '.');
                    }
                    std::printf("|\n");
                }
            }
        }
        const char* p = H<const char>(a0);
        const char* f = (id == I::memchr_)
                            ? static_cast<const char*>(std::memchr(p, a1 & 0xFF, a2))
                            : nullptr;
        if (id == I::memrchr_) {
            f = nullptr;
            for (std::uint32_t i = a2; i > 0; i--) {
                if (p[i - 1] == static_cast<char>(a1 & 0xFF)) {
                    f = p + i - 1;
                    break;
                }
            }
        }
        Ret(regs, f ? a0 + static_cast<std::uint32_t>(f - p) : 0u);
        return;
    }
    case I::strchr_:
    case I::strcspn_:
    case I::strpbrk_:
    case I::strrchr_:
    case I::strspn_:
    case I::strstr_:
    if (id == I::strchr_ || id == I::strrchr_ || id == I::strstr_ || id == I::strpbrk_ || id == I::strspn_ ||
        id == I::strcspn_) {
        // Bounded guest strlen for the haystack (and needle where needed).
        auto gstrlen = [&](std::uint32_t s) -> std::uint32_t {
            for (std::uint32_t i = 0; i < 0x100000u; i++) {
                if (!CheckRange(s + i, 1, false)) {
                    return 0xFFFFFFFFu;
                }
                if (*H<const char>(s + i) == '\0') {
                    return i;
                }
            }
            return 0xFFFFFFFFu;
        };
        const std::uint32_t hl = gstrlen(a0);
        if (hl == 0xFFFFFFFFu) {
            goto bad_ptr;
        }
        const char* h = H<const char>(a0);
        const char* f = nullptr;
        if (id == I::strchr_ || id == I::strrchr_) {
            const char c = static_cast<char>(a1 & 0xFF);
            f = (id == I::strchr_) ? std::strchr(h, c) : std::strrchr(h, c);
        } else {
            const std::uint32_t nl = gstrlen(a1);
            if (nl == 0xFFFFFFFFu) {
                goto bad_ptr;
            }
            const char* nd = H<const char>(a1);
            if (id == I::strstr_) {
                f = std::strstr(h, nd);
            } else if (id == I::strpbrk_) {
                f = std::strpbrk(h, nd);
            } else if (id == I::strspn_) {
                Ret(regs, static_cast<std::uint32_t>(std::strspn(h, nd)));
                return;
            } else {
                Ret(regs, static_cast<std::uint32_t>(std::strcspn(h, nd)));
                return;
            }
        }
        Ret(regs, f ? a0 + static_cast<std::uint32_t>(f - h) : 0u);
        return;
    }
    case I::strtok_:
    case I::strtok_r_:
    if (id == I::strtok_ || id == I::strtok_r_) {
        // Manual strtok on guest memory (host strtok_r can't use guest saveptr:
        // it stores host pointers). Delim = NUL-terminated guest string.
        // thread_local: legacy strtok state is per-thread (a global tore
        // tokens across main/worker and crashed with wild pointers).
        thread_local std::uint32_t strtok_state = 0;
        std::uint32_t state = 0;
        if (id == I::strtok_r_) {
            if (!CheckRange(a2, 4, false)) {
                goto bad_ptr;
            }
            std::memcpy(&state, H(a2), 4);
            if (a0 != 0) {
                state = a0;
            }
        } else {
            if (a0 != 0) {
                strtok_state = a0;
            }
            state = strtok_state;
        }
        auto gstrlen2 = [&](std::uint32_t p) -> std::uint32_t {
            for (std::uint32_t i = 0; i < 0x100000u; i++) {
                if (!CheckRange(p + i, 1, false)) {
                    return 0xFFFFFFFFu;
                }
                if (*H<const char>(p + i) == '\0') {
                    return i;
                }
            }
            return 0xFFFFFFFFu;
        };
        const std::uint32_t dl = gstrlen2(a1);
        if (dl == 0xFFFFFFFFu || state == 0) {
            if (id == I::strtok_r_ && state == 0) {
                Ret(regs, 0);
                return;
            }
            goto bad_ptr;
        }
        std::string delim(H<const char>(a1), dl);
        // Skip leading delimiters (needs write access: we NUL-terminate).
        std::uint32_t cur = state;
        for (;;) {
            if (!CheckRange(cur, 1, false)) {
                goto bad_ptr;
            }
            char c = *H<const char>(cur);
            if (c == '\0') {
                if (id == I::strtok_r_) {
                    std::uint32_t z = 0;
                    if (CheckRange(a2, 4, true)) {
                        std::memcpy(H(a2), &z, 4);
                    }
                } else {
                    strtok_state = 0;
                }
                Ret(regs, 0);
                return;
            }
            if (delim.find(c) == std::string::npos) {
                break;
            }
            if (!CheckRange(cur, 1, true)) {
                goto bad_ptr;
            }
            cur++;
        }
        const std::uint32_t tok = cur;
        // Scan to next delimiter/NUL.
        for (;;) {
            if (!CheckRange(cur, 1, false)) {
                goto bad_ptr;
            }
            char c = *H<const char>(cur);
            if (c == '\0') {
                if (id == I::strtok_r_) {
                    if (CheckRange(a2, 4, true)) {
                        std::memcpy(H(a2), &cur, 4);
                    }
                } else {
                    strtok_state = cur;
                }
                Ret(regs, tok);
                return;
            }
            if (delim.find(c) != std::string::npos) {
                if (!CheckRange(cur, 1, true)) {
                    goto bad_ptr;
                }
                *H<char>(cur) = '\0';
                const std::uint32_t next = cur + 1;
                if (id == I::strtok_r_) {
                    if (CheckRange(a2, 4, true)) {
                        std::memcpy(H(a2), &next, 4);
                    }
                } else {
                    strtok_state = next;
                }
                Ret(regs, tok);
                return;
            }
            cur++;
        }
    }
    case I::fscanf_:
    case I::sscanf_:
    case I::vsscanf_:
    if (id == I::sscanf_ || id == I::vsscanf_ || id == I::fscanf_) {
        // Manual sscanf (host va_list can't be synthesized; semantics follow
        // C99: ws-skip (not %c/%[/%n), literals, widths, suppression,
        // lengths hh/h/l/ll, conversions d i u o x X f e g a s c [ p n %).
        // Guest long = 32-bit (narrow host longs); long double = double.
        // fscanf(stream, fmt, ...): the same engine over what is left of the
        // stream (up to 64 KiB), then the stream is put back right after the
        // characters the conversion consumed -- exact for seekable files
        // (the /proc and data files guests read); other streams are refused.
        const bool from_stream = (id == I::fscanf_);
        FILE* sf = nullptr;
        long sf_start = 0;
        if (from_stream) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto it = file_map_.find(a0);
                sf = it == file_map_.end() ? nullptr : it->second.f;
            }
            sf_start = sf ? std::ftell(sf) : -1;
            if (!sf || sf_start < 0 || !CheckRange(a1, 1, false)) {
                std::printf("[svc] fscanf(%08x): not a seekable guest stream -> EOF\n", a0);
                SetGuestErrno(sf ? ESPIPE : EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
        } else if (!CheckRange(a0, 1, false) || !CheckRange(a1, 1, false)) {
            goto bad_ptr;
        }
        std::string stream_text;
        if (from_stream) {
            stream_text.resize(65536);
            stream_text.resize(std::fread(&stream_text[0], 1, stream_text.size(), sf));
            const std::size_t nul = stream_text.find('\0');
            if (nul != std::string::npos) {
                stream_text.resize(nul);  // the engine reads a C string
            }
        }
        const std::string inp(from_stream ? stream_text : std::string(GuestStr(a0)));
        const std::string fmt(GuestStr(a1));
        // TEMP-DIAG (worker jobs): log parsed inputs (URL flows).
        if (DiagOn()) {  // PLAN.md A: diagnostic layer
            static int sc = 0;
            if (sc < 30) {
                sc++;
                std::printf("[sscanf][t%u]%s \"%s\" fmt \"%s\"\n", GuestTid(),
                            GuestTid() == 1 ? "" : "(T2+)",
                            inp.c_str(), fmt.c_str());
            }
        }
        std::uint32_t gargs[4] = {a0, a1, a2, a3};
        std::uint32_t ai = 2;  // first vararg: r2
        std::uint32_t stack = regs[13];
        // vsscanf(str, fmt, va_list): these guests use the ARM AAPCS32
        // variadic ABI, where a va_list is a single word -- a POINTER to a
        // flat argument area -- and it arrives in a core register, not in
        // memory. vsscanf has two named arguments (str, fmt), so the area
        // pointer is the third argument: r2 == a2. The area holds the
        // variadic arguments back to back: a 4-byte argument is read AT the
        // pointer (pointer += 4); an 8-byte one is also read AT the pointer
        // (pointer += 8) -- the CALLER aligns the 8-byte slots, the consumer
        // does not shift the pointer.
        //
        // Ground truth (not from memory): the guests' own code does
        // `add r3, sp, #20` + `bl vsnprintf@plt` (the va_list is a pointer in
        // r3). The same AAPCS32 even-pair rule is visible in the WebP
        // `fprintf(fp, "%lu %f", lu, d)` call: the double is wholly at [sp],
        // not in r3.
        const bool via_va = (id == I::vsscanf_);
        std::uint32_t va_area = 0;
        if (via_va) {
            va_area = a2;
            if (!CheckRange(va_area, 4, false)) {
                goto bad_ptr;
            }
        }
        auto next_ptr = [&]() -> std::uint32_t {
            std::uint32_t v = 0;
            if (via_va) {
                if (CheckRange(va_area, 4, false)) {
                    std::memcpy(&v, H(va_area), 4);
                }
                va_area += 4;
                return v;
            }
            if (ai < 4) {
                v = gargs[ai++];
            } else {
                if (CheckRange(stack, 4, false)) {
                    std::memcpy(&v, H(stack), 4);
                }
                stack += 4;
            }
            return v;
        };
        auto is_ws = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        };
        std::size_t ii = 0, fi = 0;
        int assigned = 0;
        bool fail = false, eof = false;
        while (fi < fmt.size() && !fail) {
            char fc = fmt[fi];
            if (is_ws(fc)) {
                while (fi < fmt.size() && is_ws(fmt[fi])) {
                    fi++;
                }
                while (ii < inp.size() && is_ws(inp[ii])) {
                    ii++;
                }
                continue;
            }
            if (fc != '%') {
                if (ii >= inp.size() || inp[ii] != fc) {
                    fail = true;
                    break;
                }
                ii++;
                fi++;
                continue;
            }
            fi++;  // '%'
            if (fi < fmt.size() && fmt[fi] == '%') {
                if (ii >= inp.size() || inp[ii] != '%') {
                    fail = true;
                    break;
                }
                ii++;
                fi++;
                continue;
            }
            bool suppress = false;
            if (fi < fmt.size() && fmt[fi] == '*') {
                suppress = true;
                fi++;
            }
            int width = 0;
            while (fi < fmt.size() && fmt[fi] >= '0' && fmt[fi] <= '9') {
                width = width * 10 + (fmt[fi] - '0');
                fi++;
            }
            // The DESTINATION width follows the length modifier (C99 7.21.6.2):
            //   integers: hh 1, h 2, none/l/z/t 4, ll/j 8
            //   floats:   none 4 (float*), l/L 8 (double* / long double*)
            // The old mapping lumped ll/j/z/t into one bucket and wrote
            // FOUR bytes for all of them, so %lld and %jd filled only half
            // of the guest's long long / intmax_t; size_t and ptrdiff_t (4
            // bytes on this ILP32 guest) happened to be right.
            int len = 4;   // integer destination width in bytes
            int flen = 4;  // float destination width in bytes
            if (fi < fmt.size() && (fmt[fi] == 'h' || fmt[fi] == 'l' || fmt[fi] == 'L' ||
                                    fmt[fi] == 'j' || fmt[fi] == 'z' || fmt[fi] == 't')) {
                char lc = fmt[fi++];
                if (lc == 'h') {
                    if (fi < fmt.size() && fmt[fi] == 'h') {
                        fi++;
                        len = 1;
                    } else {
                        len = 2;
                    }
                } else if (lc == 'l') {
                    if (fi < fmt.size() && fmt[fi] == 'l') {
                        fi++;
                        len = 8;  // long long
                    } else {
                        len = 4;  // long
                    }
                    flen = 8;  // %lf -> double*
                } else if (lc == 'L') {
                    len = 8;
                    flen = 8;  // %Lf -> long double* (== double here)
                } else if (lc == 'z' || lc == 't') {
                    len = 4;  // size_t / ptrdiff_t are 4 bytes on this guest
                    flen = 8;
                } else {
                    len = 8;  // j: intmax_t
                    flen = 8;
                }
            }
            if (fi >= fmt.size()) {
                break;
            }
            char conv = fmt[fi++];
            if (conv == 'n') {
                std::uint32_t dp = suppress ? 0 : next_ptr();
                // %n writes 4B: route through the firewall (interior dp near
                // an allocation end would smash neighbor headers).
                if (dp && ClampHeapWrite(dp, 4, "sscanf-%n") >= 4 && CheckRange(dp, 4, true)) {
                    std::int32_t c = (std::int32_t)ii;
                    std::memcpy(H(dp), &c, 4);
                }
                continue;  // %n never counts, never fails
            }
            // Input exhaustion check (except %c which reads NUL? no: fail).
            if (conv != 'c' && conv != '[') {
                while (ii < inp.size() && is_ws(inp[ii])) {
                    ii++;
                }
            }
            if (ii >= inp.size()) {
                if (assigned == 0) {
                    eof = true;
                }
                fail = true;
                break;
            }
            std::uint32_t dp = suppress ? 0 : next_ptr();
            int maxw = width > 0 ? width : 1000000;
            // Overflow firewall: clamp the field to the remaining heap
            // allocation at dp (mapping checks pass arena-wide; without this
            // %s/%[/%c smash chunk neighbors -> silent dlmalloc death).
            // Non-heap dp (stack/module) keeps legacy boundless behavior.
            // %c writes no NUL; %s/%[ need room for it.
            if (dp && !suppress) {
                std::lock_guard<std::recursive_mutex> flock(mutex_);
                bool in_heap = false;
                std::uint64_t rem = 0;
                // D2: one containment query instead of find + upper_bound.
                std::uint32_t cbase = 0, csize = 0;
                if (LiveIdxContaining(dp, &cbase, &csize)) {
                    in_heap = true;
                    rem = (std::uint64_t)cbase + csize - dp;
                }
                if (in_heap) {
                    if (conv == 'c') {
                        // No NUL appended: cap chars at remaining bytes.
                        if ((std::uint64_t)maxw > rem) {
                            maxw = (int)rem;
                        }
                    } else if (rem == 0) {
                        maxw = 0;  // no room even for NUL: convert nothing
                    } else if ((std::uint64_t)maxw + 1 > rem) {
                        // Chars + NUL must fit rem.
                        maxw = (int)(rem - 1);
                    }
                }
            }
            if (conv == 'c') {
                int want = (width > 0) ? width : 1;
                int k = 0;
                while (k < want && k < maxw && ii < inp.size()) {
                    if (dp && CheckRange(dp + (std::uint32_t)k, 1, true)) {
                        H(dp)[k] = (std::uint8_t)inp[ii];
                    }
                    ii++;
                    k++;
                }
                if (k < want) {
                    fail = true;
                } else if (!suppress) {
                    assigned++;
                }
            } else if (conv == 's') {
                int k = 0;
                while (k < maxw && ii < inp.size() && !is_ws(inp[ii])) {
                    if (dp && CheckRange(dp + (std::uint32_t)k, 1, true)) {
                        H(dp)[k] = (std::uint8_t)inp[ii];
                    }
                    ii++;
                    k++;
                }
                if (k == 0) {
                    fail = true;
                } else {
                    if (dp && CheckRange(dp + (std::uint32_t)k, 1, true)) {
                        H(dp)[k] = 0;
                    }
                    if (!suppress) {
                        assigned++;
                    }
                }
            } else if (conv == '[') {
                bool neg = false;
                if (fi < fmt.size() && fmt[fi] == '^') {
                    neg = true;
                    fi++;
                }
                bool set[256] = {false};
                if (fi < fmt.size() && fmt[fi] == ']') {
                    set[(unsigned char)']'] = true;
                    fi++;
                }
                while (fi < fmt.size() && fmt[fi] != ']') {
                    if (fi + 2 < fmt.size() && fmt[fi + 1] == '-' && fmt[fi + 2] != ']') {
                        for (int c = (unsigned char)fmt[fi]; c <= (unsigned char)fmt[fi + 2]; c++) {
                            set[c] = true;
                        }
                        fi += 3;
                    } else {
                        set[(unsigned char)fmt[fi]] = true;
                        fi++;
                    }
                }
                if (fi < fmt.size()) {
                    fi++;  // ']'
                }
                int k = 0;
                while (k < maxw && ii < inp.size() &&
                       (set[(unsigned char)inp[ii]] != neg)) {
                    if (dp && CheckRange(dp + (std::uint32_t)k, 1, true)) {
                        H(dp)[k] = (std::uint8_t)inp[ii];
                    }
                    ii++;
                    k++;
                }
                if (k == 0) {
                    fail = true;
                } else {
                    if (dp && CheckRange(dp + (std::uint32_t)k, 1, true)) {
                        H(dp)[k] = 0;
                    }
                    if (!suppress) {
                        assigned++;
                    }
                }
            } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o' || conv == 'x' ||
                       conv == 'X' || conv == 'p') {
                int base = 10;
                if (conv == 'o') {
                    base = 8;
                } else if (conv == 'x' || conv == 'X' || conv == 'p') {
                    base = 16;
                } else if (conv == 'i') {
                    base = 0;
                }
                std::string tok;
                std::size_t j = ii;
                int k = 0;
                if (k < maxw && j < inp.size() && (inp[j] == '+' || inp[j] == '-')) {
                    tok += inp[j++];
                    k++;
                }
                if (base == 16 && k + 1 < maxw && j + 1 < inp.size() && inp[j] == '0' &&
                    (inp[j + 1] == 'x' || inp[j + 1] == 'X')) {
                    tok += inp[j++];
                    tok += inp[j++];
                    k += 2;
                } else if (base == 0) {
                    if (k < maxw && j < inp.size() && inp[j] == '0') {
                        tok += inp[j++];
                        k++;
                        if (k < maxw && j < inp.size() && (inp[j] == 'x' || inp[j] == 'X')) {
                            tok += inp[j++];
                            k++;
                            base = 16;
                        } else {
                            base = 8;
                        }
                    } else {
                        base = 10;
                    }
                }
                auto digok = [&](char c) {
                    if (base == 16) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                               (c >= 'A' && c <= 'F');
                    }
                    if (base == 8) {
                        return c >= '0' && c <= '7';
                    }
                    return c >= '0' && c <= '9';
                };
                while (k < maxw && j < inp.size() && digok(inp[j])) {
                    tok += inp[j++];
                    k++;
                }
                bool hassign = !tok.empty() && !(tok.size() == 1 && (tok[0] == '+' || tok[0] == '-'));
                // A bare 0x prefix with no digits: only "0" converts.
                if (tok == "0x" || tok == "0X" || tok == "+0x" || tok == "-0x" || tok == "+0X" ||
                    tok == "-0X") {
                    tok = tok.substr(0, tok.size() - 1);
                    j--;
                }
                if (!hassign || tok.empty() || tok == "+" || tok == "-") {
                    fail = true;
                } else {
                    ii = j;
                    if (!suppress) {
                        bool neg = tok[0] == '-';
                        // C99: a value that does not fit SATURATES to the
                        // target type's range and the conversion still
                        // succeeds. The old code let stoull throw, which
                        // aborted the whole scan (fail) and lost every
                        // conversion after it.
                        unsigned long long uv = 0;
                        {
                            errno = 0;
                            char* endp = nullptr;
                            const std::string num =
                                (tok[0] == '+' || tok[0] == '-') ? tok.substr(1) : tok;
                            uv = std::strtoull(num.c_str(), &endp, base == 0 ? 10 : base);
                            if (errno == ERANGE) {
                                uv = ULLONG_MAX;
                            }
                        }
                        // Saturation applies to the PARSE (long long), like
                        // strtoll does; the narrow target type is then reached
                        // by the usual conversion (300 -> short = 44).
                        long long sv;
                        if (neg) {
                            sv = (uv > (unsigned long long)LLONG_MAX + 1ULL)
                                     ? LLONG_MIN
                                     : -(long long)uv;
                        } else {
                            sv = (uv > (unsigned long long)LLONG_MAX) ? LLONG_MAX
                                                                       : (long long)uv;
                        }
                        bool is_signed = (conv == 'd' || conv == 'i');
                        if (len == 8) {
                            // long long / intmax_t: the guest's slot is 8 bytes.
                            if (is_signed) {
                                long long w64 = sv;
                                if (dp && ClampHeapWrite(dp, 8, "sscanf-num") >= 8 && CheckRange(dp, 8, true)) {
                                    std::memcpy(H(dp), &w64, 8);
                                    }
                            } else {
                                unsigned long long w64 = uv;
                                if (dp && ClampHeapWrite(dp, 8, "sscanf-num") >= 8 && CheckRange(dp, 8, true)) {
                                    std::memcpy(H(dp), &w64, 8);
                                    }
                            }
                        } else if (len == 1) {
                            std::int8_t w8 =
                                is_signed ? (std::int8_t)sv : (std::int8_t)(std::uint8_t)uv;
                            if (dp && ClampHeapWrite(dp, 1, "sscanf-num") >= 1 && CheckRange(dp, 1, true)) {
                                std::memcpy(H(dp), &w8, 1);
                                }
                        } else if (len == 2) {
                            std::int16_t w16 =
                                is_signed ? (std::int16_t)sv : (std::int16_t)(std::uint16_t)uv;
                            if (dp && ClampHeapWrite(dp, 2, "sscanf-num") >= 2 && CheckRange(dp, 2, true)) {
                                std::memcpy(H(dp), &w16, 2);
                                }
                        } else if (!is_signed && conv == 'p') {
                            // %p writes an int*.
                            std::int32_t w32 = (std::int32_t)uv;
                            if (dp && ClampHeapWrite(dp, 4, "sscanf-num") >= 4 && CheckRange(dp, 4, true)) {
                                std::memcpy(H(dp), &w32, 4);
                                }
                        } else {
                            // int / long / size_t / ptrdiff_t: 4 bytes.
                            if (is_signed) {
                                std::int32_t w32 = (std::int32_t)sv;
                                if (dp && ClampHeapWrite(dp, 4, "sscanf-num") >= 4 && CheckRange(dp, 4, true)) {
                                    std::memcpy(H(dp), &w32, 4);
                                    }
                            } else {
                                std::uint32_t wu = (std::uint32_t)uv;
                                if (dp && ClampHeapWrite(dp, 4, "sscanf-num") >= 4 && CheckRange(dp, 4, true)) {
                                    std::memcpy(H(dp), &wu, 4);
                                    }
                            }
                        }
                        assigned++;
                    }
                }
            } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' ||
                       conv == 'G' || conv == 'a' || conv == 'A') {
                // Maximal munch for float syntax, then stod for value+length.
                std::string tok;
                std::size_t j = ii;
                int k = 0;
                while (k < maxw && j < inp.size() &&
                       ((inp[j] >= '0' && inp[j] <= '9') || inp[j] == '+' || inp[j] == '-' ||
                        inp[j] == '.' || inp[j] == 'e' || inp[j] == 'E')) {
                    tok += inp[j++];
                    k++;
                }
                // Hex-float / inf / nan tails: extend on demand (rare here).
                double dv = 0;
                std::size_t pos = 0;
                bool ok = false;
                if (!tok.empty()) {
                    try {
                        dv = std::stod(tok, &pos);
                        ok = (pos > 0);
                    } catch (...) {
                        ok = false;
                    }
                }
                if (!ok) {
                    fail = true;
                } else {
                    ii += pos;
                    if (!suppress) {
                        if (flen == 8) {
                            // %lf / %Lf: the guest's slot is a double (8B).
                            if (dp && ClampHeapWrite(dp, 8, "sscanf-num") >= 8 && CheckRange(dp, 8, true)) {
                                std::memcpy(H(dp), &dv, 8);
                                }
                        } else {
                            float fv = (float)dv;
                            if (dp && ClampHeapWrite(dp, 4, "sscanf-num") >= 4 && CheckRange(dp, 4, true)) {
                                std::memcpy(H(dp), &fv, 4);
                                }
                        }
                        assigned++;
                    }
                }
            } else {
                // Unsupported conversion: loud halt (no silent corruption).
                std::printf("[svc] LOUD sscanf unsupported %%%c -- halting\n", conv);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
        }
        if (from_stream) {
            std::clearerr(sf);
            std::fseek(sf, sf_start + static_cast<long>(ii), SEEK_SET);
        }
        // assigned = number of successful conversions (EOF => -1).
        Ret(regs, eof ? static_cast<std::uint32_t>(-1)
                      : static_cast<std::uint32_t>(assigned));
        return;
    }
    case I::atoi_:
    case I::atol_:
    case I::strtol_:
    case I::strtoll_:
    case I::strtoul_:
    case I::strtoull_:
    if (id == I::strtol_ || id == I::strtoul_ || id == I::strtoll_ || id == I::strtoull_ || id == I::atoi_ ||
        id == I::atol_) {
        // Guest ABI: strtoX(nptr=a0, endptr=a1, base=a2); atoi/atol take
        // only a0. Guest `long` is 32-bit while the host's is 64-bit, so the
        // 32-bit variants parse with the 64-bit host functions and saturate
        // like a 32-bit libc: strtol -> LONG_MIN/LONG_MAX + ERANGE, strtoul ->
        // ULONG_MAX + ERANGE (a leading '-' negates modulo 2^32, as C says).
        // bionic's atoi/atol are (int)strtol(s, NULL, 10): same saturation.
        if (!CheckRange(a0, 1, false)) {
            goto bad_ptr;
        }
        const std::string s(GuestStr(a0));
        char* end = nullptr;
        auto strtol32 = [&](int base, char** endp, int* err) -> std::uint32_t {
            errno = 0;
            const long long v = std::strtoll(s.c_str(), endp, base);
            *err = errno;
            if (v > INT32_MAX) {
                *err = ERANGE;
                return static_cast<std::uint32_t>(INT32_MAX);
            }
            if (v < INT32_MIN) {
                *err = ERANGE;
                return static_cast<std::uint32_t>(INT32_MIN);
            }
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
        };
        if (id == I::atoi_ || id == I::atol_) {
            int err = 0;
            Ret(regs, strtol32(10, nullptr, &err));
            return;
        }
        const int base = static_cast<int>(a2);
        auto store_end = [&](std::uint32_t slot) -> bool {
            if (slot == 0) {
                return true;
            }
            if (!CheckRange(slot, 4, true)) {
                return false;
            }
            const std::uint32_t ev = a0 + static_cast<std::uint32_t>(end - s.c_str());
            std::memcpy(H(slot), &ev, 4);
            return true;
        };
        int err = 0;
        if (id == I::strtoll_ || id == I::strtoull_) {
            errno = 0;
            const unsigned long long v = (id == I::strtoull_) ? std::strtoull(s.c_str(), &end, base)
                                                           : (unsigned long long)std::strtoll(
                                                                 s.c_str(), &end, base);
            err = errno;
            if (!store_end(a1)) {
                goto bad_ptr;
            }
            Ret(regs, static_cast<std::uint32_t>(v & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
        } else {
            std::uint32_t v = 0;
            if (id == I::strtoul_) {
                errno = 0;
                const unsigned long long u = std::strtoull(s.c_str(), &end, base);
                err = errno;
                // Magnitude check: strtoull already negated a leading '-'.
                const char* q = s.c_str();
                while (*q == ' ' || (*q >= '\t' && *q <= '\r')) {
                    q++;
                }
                const bool neg = (*q == '-');
                const unsigned long long mag = neg ? (0ULL - u) : u;
                if (err == ERANGE || mag > 0xFFFFFFFFULL) {
                    err = ERANGE;
                    v = 0xFFFFFFFFu;
                } else {
                    v = neg ? static_cast<std::uint32_t>(0u - static_cast<std::uint32_t>(mag))
                            : static_cast<std::uint32_t>(mag);
                }
            } else {
                v = strtol32(base, &end, &err);
            }
            if (!store_end(a1)) {
                goto bad_ptr;
            }
            Ret(regs, v);
        }
        if (err != 0) {
            SetGuestErrno(err);
        }
        return;
    }
    case I::strtod_:
    if (id == I::strtod_) {
        // Guest ABI: strtod(nptr=a0, endptr=a1) -> double in r0:r1 (softfp:
        // little-endian, low word in r0 -- see math_thunks.inc and the guest's
        // luaO_str2d, which reads the value back as CONCAT44(r1, r0)).
        //
        // This one is load-bearing: the engine's Lua string->number coercion
        // (luaO_str2d) declares `char *endptr;` uninitialised and relies on
        // strtod to write it. If the call returns without touching a1, endptr
        // keeps stack garbage, so luaO_str2d most often reports "converted OK"
        // -- and lua_isnumber() then claims arbitrary strings are numbers.
        // Symptom: drawSprite() takes its numeric-argument branch for calls
        // that pass a sprite NAME (e.g. Image.lua: res.drawSprite("",
        // "LOGO_ROVIO", 0, 0, "HPIVOT","VPIVOT",w,h)) -> "Excepted type NUMBER
        // at stack position 2 but found STRING" -> uncaught LuaException ->
        // engine's terminate handler -> abort(), every frame.
        if (!CheckRange(a0, 1, false)) {
            goto bad_ptr;
        }
        const std::string s(GuestStr(a0));
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (a1 != 0) {
            if (!CheckRange(a1, 4, true)) {
                goto bad_ptr;
            }
            const std::uint32_t ev = a0 + static_cast<std::uint32_t>(end - s.c_str());
            std::memcpy(H(a1), &ev, 4);
        }
        if (DiagOn()) {
            static int strtod_shown = 0;
            if (strtod_shown < 8) {
                ++strtod_shown;
                std::printf("[svc] strtod(\"%.40s\") -> %g consumed=%d\n", s.c_str(), v,
                            static_cast<int>(end - s.c_str()));
            }
        }
        std::uint32_t w64[2] = {0, 0};
        std::memcpy(w64, &v, 8);
        Ret(regs, w64[0], w64[1]);
        return;
    }
    case I::__errno_:
    if (id == I::__errno_) {
        Ret(regs, ErrnoSlot());
        return;
    }
    case I::strerror_:
    case I::strerror_r_:
    if (id == I::strerror_ || id == I::strerror_r_) {
        // Must hand back a GUEST string: a host pointer is read as guest memory
        // (the engine's own log then printed the .so's ELF header as the
        // "errno text"). Per-host-thread guest scratch; callers never free it.
        const int e = (int)a0;
        const char* msg = ::strerror(e == 0 ? EINVAL : e);
        if (!msg) {
            msg = "Unknown error";
        }
        if (id == I::strerror_r_) {
            // bionic: int strerror_r(int err, char* buf, size_t len).
            if (a1 == 0 || a2 == 0) {
                Ret(regs, static_cast<std::uint32_t>(EINVAL));
                return;
            }
            const std::size_t cap = (std::size_t)a2;
            const std::size_t w = std::min<std::size_t>(std::strlen(msg), cap - 1);
            if (!CheckRange(a1, w + 1, true)) {
                Ret(regs, static_cast<std::uint32_t>(ERANGE));
                return;
            }
            std::memcpy(H(a1), msg, w);
            H(a1)[w] = 0;
            Ret(regs, 0);
            return;
        }
        static thread_local std::uint32_t sbuf = 0;
        if (sbuf == 0) {
            sbuf = GuestMalloc(128);
        }
        if (sbuf == 0 || !CheckRange(sbuf, 1, true)) {
            Ret(regs, 0);
            return;
        }
        const std::size_t w = std::min<std::size_t>(std::strlen(msg), 127);
        std::memcpy(H(sbuf), msg, w);
        H(sbuf)[w] = 0;
        Ret(regs, sbuf);
        return;
    }
    // --- time (32-bit guest layouts: two u32 words, NOT host structs) ---
    // HARNESS_TIMEWARP=<sec>: shift all guest clocks forward (timer-gate
    // tests: loading/menu timeouts expire without wall waiting).
    case I::clock_gettime_:
    if (id == I::clock_gettime_) {
        struct timespec ts {};
        const int r = clock_gettime(a0, &ts);
        if (r == 0) {
            ts.tv_sec += (time_t)TimeWarp();
            const std::uint32_t sec = static_cast<std::uint32_t>(ts.tv_sec);
            const std::uint32_t nsec = static_cast<std::uint32_t>(ts.tv_nsec);
            if (CheckRange(a1, 8, true)) {
                std::memcpy(H(a1), &sec, 4);
                std::memcpy(H(a1 + 4), &nsec, 4);
            }
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case I::gettimeofday_:
    if (id == I::gettimeofday_) {
        struct timeval tv {};
        const int r = gettimeofday(&tv, nullptr);
        if (r == 0 && a0 != 0) {
            tv.tv_sec += (time_t)TimeWarp();
            const std::uint32_t sec = static_cast<std::uint32_t>(tv.tv_sec);
            const std::uint32_t usec = static_cast<std::uint32_t>(tv.tv_usec);
            if (CheckRange(a0, 8, true)) {
                std::memcpy(H(a0), &sec, 4);
                std::memcpy(H(a0 + 4), &usec, 4);
            }
        }
        // Timezone out-param not supported; ignored (always NULL on Android).
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case I::clock_:
    case I::time_:
    if (id == I::time_ || id == I::clock_) {
        std::time_t t = (id == I::time_) ? std::time(nullptr) : std::clock();
        if (id == I::time_) {
            t += (time_t)TimeWarp();
        }
        if (id == I::time_ && a0 != 0 && CheckRange(a0, 4, true)) {
            const std::uint32_t w = static_cast<std::uint32_t>(t & 0xFFFFFFFFu);
            std::memcpy(H(a0), &w, 4);
        }
        Ret(regs, static_cast<std::uint32_t>(t & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(((std::uint64_t)t >> 32) & 0xFFFFFFFFu));
        return;
    }
    case I::gmtime_:
    case I::gmtime_r_:
    case I::localtime_:
    case I::localtime_r_:
    if (id == I::localtime_r_ || id == I::gmtime_r_ || id == I::localtime_ || id == I::gmtime_) {
        // Bionic ARM struct tm (44B): 9x int + long gmtoff + char* zone.
        // localtime()/gmtime() use a per-thread static buffer: allocate the
        // guest block once, then return the same pointer on every call. The
        // _r forms keep writing the caller's buffer.
        const bool is_local = (n[0] == 'l');
        const bool use_static = (id == I::localtime_ || id == I::gmtime_);
        thread_local std::uint32_t static_tm = 0;
        std::int32_t gt = 0;
        if (CheckRange(a0, 4, false)) {
            std::memcpy(&gt, H(a0), 4);
        }
        std::time_t tt = (std::time_t)gt;
        if (is_local) {
            tt += (time_t)TimeWarp();
        }
        thread_local struct tm scratch {};
        struct tm* ht = is_local ? localtime_r(&tt, &scratch) : gmtime_r(&tt, &scratch);
        std::uint32_t out = a1;
        if (use_static) {
            if (!static_tm) {
                static_tm = GuestMalloc(44);
#ifdef ABR_DIAG_COUNTERS
                if (static_tm) {
                    diag_.localtime_allocs.fetch_add(1, std::memory_order_relaxed);
                    diag_.localtime_bytes.fetch_add(44, std::memory_order_relaxed);
                }
#endif
            }
            out = static_tm;
        }
        if (out && CheckRange(out, 44, true) && ht) {
            std::int32_t f[9] = {ht->tm_sec,  ht->tm_min,  ht->tm_hour, ht->tm_mday, ht->tm_mon,
                                 ht->tm_year, ht->tm_wday, ht->tm_yday, ht->tm_isdst};
            for (int i = 0; i < 9; i++) {
                std::memcpy(H(out + (std::uint32_t)(i * 4)), &f[i], 4);
            }
            std::int32_t off = (std::int32_t)ht->tm_gmtoff;
            std::memcpy(H(out + 36), &off, 4);
            std::uint32_t z = 0;
            std::memcpy(H(out + 40), &z, 4);
            Ret(regs, out);
        } else {
            Ret(regs, 0);
        }
        return;
    }
    case I::__android_log_print_:
    if (id == I::__android_log_print_) {
        // Expand guest printf-style args so the game's own diagnostics (e.g.
        // "Parsing JSON failed! Reason: ...") become visible. This used to
        // carry its OWN formatter, which had diverged from the printf
        // family: it printed %f/%e/%g as %g, printed %p without the 0x,
        // ignored the length modifiers and the flags/width/precision, and
        // read a double from an AAPCS32 EVEN register pair -- the same rule
        // the guests use (proved by the WebP `%lu %f` fprintf call).
        // One formatter now: Runtime::ExpandPrintf.
        const std::string fmt(GuestStr(a2));  // copy: later GuestStr calls reuse scratch
        GuestFmtArgs st{};
        st.gargs[0] = a0;
        st.gargs[1] = a1;
        st.gargs[2] = a2;
        st.gargs[3] = a3;
        st.ai = 3;  // r0-r2 are prio, tag and fmt; the variadic part starts at r3
        st.stack = regs[13];
        std::size_t total = 0;
        const std::string out = ExpandPrintf(fmt, st, &total);
        std::printf("[glog] %s\n", out.c_str());
        Ret(regs, static_cast<std::uint32_t>(total));
        return;
    }
    case I::__android_log_vprint_:
    if (id == I::__android_log_vprint_) {
        // (prio, tag, fmt, va_list): the arguments start at the va pointer.
        const std::string fmt(GuestStr(a2));
        GuestFmtArgs st{};
        st.via_va = true;
        st.ai = 3;
        st.stack = a3;
        std::size_t total = 0;
        const std::string out = ExpandPrintf(fmt, st, &total);
        std::printf("[glog] %s\n", out.c_str());
        Ret(regs, static_cast<std::uint32_t>(total));
        return;
    }
    case I::__android_log_write_:
    if (id == I::__android_log_write_) {
        // No printf spec: (prio, tag, text). Same sink as log_print.
        const std::string tag(GuestStr(a1));
        const std::string text(GuestStr(a2));
        std::printf("[glog] %s: %s\n", tag.c_str(), text.c_str());
        Ret(regs, static_cast<std::uint32_t>(text.size()));
        return;
    }
    case I::printf_:
    case I::vprintf_:
    if (id == I::printf_ || id == I::vprintf_) {
        // Host stdout (the game gated its own logging off, but a crash-time
        // path may call it). GuestFmtArgs consumes r1-r3 then stack args.
        const std::string fmt(GuestStr(a0));
        GuestFmtArgs st{};
        st.gargs[0] = a0;
        st.gargs[1] = a1;
        st.gargs[2] = a2;
        st.gargs[3] = a3;
        if (id == I::printf_) {
            st.ai = 1;
            st.stack = regs[13];
        } else {
            st.via_va = true;
            st.ai = 1;
            st.stack = a1;
        }
        std::size_t total = 0;
        const std::string out = ExpandPrintf(fmt, st, &total);
        std::fwrite(out.data(), 1, out.size(), stdout);
        Ret(regs, static_cast<std::uint32_t>(total));
        return;
    }
    case I::uname_:
    if (id == I::uname_) {
        // Breakpad stores this in the minidump; machine must look like the
        // guest ABI (armv7l), the rest is informational. struct utsname on
        // arm32 bionic is 6 fields x 65 bytes.
        if (!CheckRange(a0, 390, true)) {
            goto bad_ptr;
        }
        {
            char u[390] = {};
            std::strncpy(u + 0, "Linux", 64);
            std::strncpy(u + 65, "localhost", 64);
            std::strncpy(u + 130, "3.10.0", 64);
            std::strncpy(u + 195, "#1 SMP PREEMPT", 64);
            std::strncpy(u + 260, "armv7l", 64);
            std::strncpy(u + 325, "localdomain", 64);
            std::memcpy(H(a0), u, 390);
        }
        Ret(regs, 0);
        return;
    }
    // --- printf family writing into GUEST buffers ---
    // Space's engine imports these; the stub tail used to make them silent
    // no-ops, so every buffer it formatted (its Lua error text, symbol names,
    // JSON fragments) came out untouched/garbage. C99 truncation semantics:
    // at most cap-1 chars + NUL, return = untruncated length.
    // --- libc calls that used to fall into the generic 0-stub ("success")
    //     although all six titles import them (G-02). Layouts from the NDK
    //     headers, sizes checked with the NDK clang for armv7a: struct
    //     sigaction 16 B, sigset_t 4 B, wchar_t 4 B.
    case I::vasprintf_:
    if (id == I::vasprintf_) {
        // int vasprintf(char** strp, const char* fmt, va_list ap)
        if (!CheckRange(a0, 4, true) || !CheckRange(a1, 1, false)) {
            goto bad_ptr;
        }
        const std::string fmt(GuestStr(a1));
        GuestFmtArgs st;
        st.gargs[0] = a0;
        st.gargs[1] = a1;
        st.gargs[2] = a2;
        st.gargs[3] = a3;
        st.ai = 2;
        st.via_va = true;
        st.stack = a2;
        const std::string out = ExpandPrintf(fmt, st);
        const std::uint32_t buf = GuestMalloc(static_cast<std::uint32_t>(out.size() + 1));
        if (!buf) {
            SetGuestErrno(ENOMEM);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        std::memcpy(H(buf), out.c_str(), out.size() + 1);
        std::memcpy(H(a0), &buf, 4);
        Ret(regs, static_cast<std::uint32_t>(out.size()));
        return;
    }
    case I::fnmatch_:
    if (id == I::fnmatch_) {
        // Guest (bionic) flags: NOESCAPE 1, PATHNAME 2, PERIOD 4 -- glibc
        // numbers them differently, so translate. FNM_NOMATCH = 1 in both.
        std::uint32_t lp = 0, ls = 0;
        if (!GuestStrnlen(a0, 0xFFFFFFFFu, &lp) || !GuestStrnlen(a1, 0xFFFFFFFFu, &ls)) {
            goto bad_ptr;
        }
        int hf = 0;
        if (a2 & 0x1u) hf |= FNM_NOESCAPE;
        if (a2 & 0x2u) hf |= FNM_PATHNAME;
        if (a2 & 0x4u) hf |= FNM_PERIOD;
        const int r = ::fnmatch(H<const char>(a0), H<const char>(a1), hf);
        Ret(regs, r == 0 ? 0u : 1u);
        return;
    }
    case I::basename_:
    if (id == I::basename_) {
        // POSIX basename: last path component, "/" for all-slash input, "."
        // for NULL/"". The result lives in a per-thread guest buffer (bionic
        // also returns thread-local storage).
        std::string path;
        if (a0 != 0) {
            std::uint32_t len = 0;
            if (!GuestStrnlen(a0, 0xFFFFFFFFu, &len)) {
                goto bad_ptr;
            }
            path.assign(H<const char>(a0), len);
        }
        std::string base;
        if (path.empty()) {
            base = ".";
        } else {
            std::size_t e = path.find_last_not_of('/');
            if (e == std::string::npos) {
                base = "/";
            } else {
                const std::size_t b = path.find_last_of('/', e);
                base = path.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
            }
        }
        thread_local std::uint32_t tbuf = 0;
        constexpr std::uint32_t kBasenameBuf = 4096;  // PATH_MAX
        if (base.size() >= kBasenameBuf) {
            SetGuestErrno(ENAMETOOLONG);
            Ret(regs, 0);
            return;
        }
        if (!tbuf) {
            tbuf = GuestMalloc(kBasenameBuf);
        }
        if (!tbuf) {
            Ret(regs, 0);
            return;
        }
        std::memcpy(H(tbuf), base.c_str(), base.size() + 1);
        Ret(regs, tbuf);
        return;
    }
    case I::setlocale_:
    if (id == I::setlocale_) {
        // Only the "C"/"POSIX" locale exists for the guest (bionic of that era
        // had no others): query or set "C" -> "C", anything else -> NULL.
        static std::uint32_t c_str = 0;
        bool want_c = true;
        if (a1 != 0) {
            std::uint32_t len = 0;
            if (!GuestStrnlen(a1, 256, &len)) {
                goto bad_ptr;
            }
            const std::string loc(H<const char>(a1), len);
            want_c = loc.empty() || loc == "C" || loc == "POSIX";
        }
        if (!want_c) {
            Ret(regs, 0);
            return;
        }
        if (!c_str) {
            c_str = GuestMalloc(2);
            if (c_str) {
                std::memcpy(H(c_str), "C", 2);
            }
        }
        Ret(regs, c_str);
        return;
    }
    // --- libc pieces first needed by Mono (99 Bricks). Not imported by the
    // Rovio titles, so their behaviour is unchanged.
    case I::bsd_signal_:
    if (id == I::bsd_signal_) {
        // signal(sig, handler): guest handlers are never delivered (see
        // sigaction below); report SIG_DFL as the previous disposition.
        Ret(regs, 0);
        return;
    }
    case I::gettid_:
    if (id == I::gettid_) {
        Ret(regs, GuestTid());
        return;
    }
    case I::getdtablesize_:
    if (id == I::getdtablesize_) {
        // The descriptor table size (RLIMIT_NOFILE soft limit); Android's
        // default is 1024. Mono's io-layer sizes its handle table from it.
        struct rlimit rl {};
        const bool ok = ::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY;
        Ret(regs, ok && rl.rlim_cur < 65536 ? static_cast<std::uint32_t>(rl.rlim_cur) : 1024u);
        return;
    }
    case I::dlclose_:
    case I::dlerror_:
    case I::dlopen_:
    case I::dlsym_:
    if (id == I::dlopen_ || id == I::dlsym_ || id == I::dlclose_ || id == I::dlerror_) {
        // Guest libraries this module already holds (several guest .so, e.g.
        // Unity's libmain dlopens libmono/libunity): a handle per library,
        // dlsym from the module's definitions. Any other name -- host
        // libraries such as libc.so -- gets NULL as it always did here.
        // dlerror's message of this thread's last failure (bionic wording).
        thread_local std::string dl_error;
        if (id == I::dlopen_) {
            // dlopen(NULL): the program's own handle (app_process). Its dlsym
            // searches the executable's global group, which libraries loaded
            // from Java are not part of: no guest symbol is found through it.
            std::uint32_t h = 0;
            if (a0 == 0) {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (!global_dl_handle_) {
                    global_dl_handle_ = GuestMalloc(16);
                }
                h = global_dl_handle_;
            } else {
                const std::string want = GuestStr(a0);
                h = GuestLibraryHandle(want);
                // A system library: a handle whose dlsym finds what the host
                // serves of it (an app may always dlopen these on Android).
                static const char* const kSystem[] = {
                    "libandroid.so", "libc.so", "libm.so", "libdl.so", "liblog.so", "libEGL.so",
                    "libGLESv1_CM.so", "libGLESv2.so", "libz.so", "libstdc++.so", "libOpenSLES.so"};
                const std::string base = want.substr(want.find_last_of('/') + 1);
                for (const char* k : kSystem) {
                    if (!h && base == k) {
                        std::lock_guard<std::recursive_mutex> lock(mutex_);
                        if (!sys_dl_handle_) {
                            sys_dl_handle_ = GuestMalloc(16);
                        }
                        h = sys_dl_handle_;
                    }
                }
            }
            if (DiagOn() || h) {
                std::printf("[dl] dlopen(\"%s\") -> %08x\n", a0 ? GuestStr(a0) : "NULL", h);
            }
            if (!h) {
                dl_error = std::string("dlopen failed: library \"") + GuestStr(a0) + "\" not found";
            }
            Ret(regs, h);
            return;
        }
        if (id == I::dlsym_) {
            // That library's own definition first (dlsym(handle) searches the
            // library, then its dependencies: the merged table stands in).
            // RTLD_DEFAULT (0, or 0xffffffff in the LP32 headers of the
            // guests' era) also searches the caller's own group: the merged
            // definitions stand in for it.
            std::uint32_t v = 0;
            const int lib = GuestLibraryIndex(a0);
            if ((a0 == 0 || a0 == 0xFFFFFFFFu) && a1) {
                if (const auto jt = mod_.defined.find(GuestStr(a1)); jt != mod_.defined.end()) {
                    v = jt->second;
                }
            } else if (lib >= 0 && a1) {
                const std::string sym = GuestStr(a1);
                const auto& own = mod_.lib_defined[static_cast<std::size_t>(lib)];
                const auto it = own.find(sym);
                if (it != own.end()) {
                    v = it->second;
                } else if (const auto jt = mod_.defined.find(sym); jt != mod_.defined.end()) {
                    v = jt->second;
                }
            }
            if (!v && a1 && (a0 == 0 || a0 == 0xFFFFFFFFu || (sys_dl_handle_ && a0 == sys_dl_handle_))) {
                // What a system library exports and the host serves: a
                // trampoline made now.
                const std::string sym = GuestStr(a1);
                // EGL: one host handler answers every egl* name (the ones it
                // does not offer return 0, LOUD); SDL loads EGL this way.
                if (NdkServes(sym) || OpenSlServes(sym) || sym.compare(0, 3, "egl") == 0) {
                    v = RuntimeImport(sym);
                } else if (sym.compare(0, 7, "SL_IID_") == 0) {
                    v = OpenSlIid(sym);  // a data symbol: the variable's address
                }
            }
            if (!v) {
                dl_error = std::string("undefined symbol: ") + (a1 ? GuestStr(a1) : "(null)");
            }
            Ret(regs, v);
            return;
        }
        if (id == I::dlerror_) {
            // The message once, then NULL until the next failure; the text
            // lives in a per-thread guest buffer.
            if (dl_error.empty()) {
                Ret(regs, 0);
                return;
            }
            thread_local std::uint32_t buf = 0;
            if (!buf) {
                buf = GuestMalloc(512);
            }
            if (!buf) {
                Ret(regs, 0);
                return;
            }
            const std::size_t len = dl_error.size() < 511 ? dl_error.size() : 511;
            std::memcpy(H(buf), dl_error.data(), len);
            *H<char>(buf + static_cast<std::uint32_t>(len)) = '\0';
            dl_error.clear();
            Ret(regs, buf);
            return;
        }
        Ret(regs, 0);  // dlclose: loaded libraries stay
        return;
    }
    case I::memalign_:
    case I::posix_memalign_:
    if (id == I::memalign_ || id == I::posix_memalign_) {
        // memalign(align, size) -> ptr; posix_memalign(void** out, align,
        // size) -> errno (EINVAL unless align is a power of two that is a
        // multiple of sizeof(void*)). bionic's memalign rounds a bad
        // alignment up to a power of two.
        const bool posix = (id == I::posix_memalign_);
        std::uint32_t align = posix ? a1 : a0;
        const std::uint32_t size = posix ? a2 : a1;
        const bool pow2 = align != 0 && (align & (align - 1)) == 0;
        if (posix && (!pow2 || align < 4)) {
            Ret(regs, EINVAL);
            return;
        }
        if (!pow2) {
            std::uint32_t p = 8;
            while (p < align && p < 0x80000000u) {
                p <<= 1;
            }
            align = p;
        }
        const std::uint32_t blk = GuestMemalign(align, size);
        if (posix) {
            if (!CheckRange(a0, 4, true)) {
                goto bad_ptr;
            }
            if (blk) {
                std::memcpy(H(a0), &blk, 4);
            }
            Ret(regs, blk ? 0u : static_cast<std::uint32_t>(ENOMEM));
            return;
        }
        if (!blk) {
            SetGuestErrno(ENOMEM);
        }
        Ret(regs, blk);
        return;
    }
    case I::strlcpy_:
    if (id == I::strlcpy_) {
        // strlcpy(dst, src, size): copies at most size-1 bytes, always
        // NUL-terminates when size > 0, returns strlen(src).
        std::uint32_t len = 0;
        if (!GuestStrnlen(a1, 0xFFFFFFFFu, &len)) {
            goto bad_ptr;
        }
        if (a2 != 0) {
            const std::uint32_t c = len < a2 - 1 ? len : a2 - 1;
            if (!CheckRange(a0, c + 1, true)) {
                goto bad_ptr;
            }
            std::memmove(H(a0), H(a1), c);
            *H<std::uint8_t>(a0 + c) = 0;
        }
        Ret(regs, len);
        return;
    }
    case I::clock_getres_:
    if (id == I::clock_getres_) {
        // clock_getres(clk, struct timespec* res): 32-bit {time_t, long}.
        struct timespec ts {};
        if (::clock_getres(static_cast<clockid_t>(a0), &ts) != 0) {
            SetGuestErrno(errno);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        if (a1 != 0) {
            if (!CheckRange(a1, 8, true)) {
                goto bad_ptr;
            }
            const std::int32_t g[2] = {static_cast<std::int32_t>(ts.tv_sec),
                                       static_cast<std::int32_t>(ts.tv_nsec)};
            std::memcpy(H(a1), g, 8);
        }
        Ret(regs, 0);
        return;
    }
    case I::mprotect_:
    if (id == I::mprotect_) {
        Ret(regs, static_cast<std::uint32_t>(GuestMprotect(a0, a1, static_cast<int>(a2))));
        return;
    }
    case I::readlink_:
    if (id == I::readlink_) {
        // readlink(path, buf, size): through the file bridge. A link target
        // inside the host files dir goes back in guest spelling. No match
        // (unmapped path) is ENOENT; a plain file or dir is EINVAL as usual.
        if (!CheckRange(a1, a2 ? a2 : 1, true)) {
            goto bad_ptr;
        }
        const std::string hp = MapGuestPath(a0);
        if (hp.empty()) {
            SetGuestErrno(ENOENT);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        char buf[4096];
        const ssize_t r = ::readlink(hp.c_str(), buf, sizeof(buf) - 1);
        if (r < 0) {
            SetGuestErrno(errno);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        std::string target(buf, static_cast<std::size_t>(r));
        if (!files_hostbase_.empty() && target.compare(0, files_hostbase_.size(), files_hostbase_) == 0) {
            target = files_prefix_ + target.substr(files_hostbase_.size());
        }
        const std::size_t len = target.size() < a2 ? target.size() : a2;  // no NUL, may truncate
        std::memcpy(H(a1), target.data(), len);
        Ret(regs, static_cast<std::uint32_t>(len));
        return;
    }
    case I::pthread_attr_destroy_:
    case I::pthread_attr_getdetachstate_:
    case I::pthread_attr_getstack_:
    case I::pthread_attr_getstacksize_:
    case I::pthread_attr_init_:
    case I::pthread_attr_setdetachstate_:
    case I::pthread_attr_setschedparam_:
    case I::pthread_attr_setschedpolicy_:
    case I::pthread_attr_setstacksize_:
    case I::pthread_getattr_np_:
    if (id == I::pthread_attr_init_ || id == I::pthread_attr_destroy_ || id == I::pthread_getattr_np_ ||
        id == I::pthread_attr_getstack_ || id == I::pthread_attr_getstacksize_ ||
        id == I::pthread_attr_setstacksize_ || id == I::pthread_attr_getdetachstate_ ||
        id == I::pthread_attr_setdetachstate_ || id == I::pthread_attr_setschedpolicy_ ||
        id == I::pthread_attr_setschedparam_) {
        Ret(regs, PthreadAttr(id, n, a0, a1, a2));
        return;
    }
    case I::__pthread_cleanup_pop_:
    case I::__pthread_cleanup_push_:
    if (id == I::__pthread_cleanup_push_ || id == I::__pthread_cleanup_pop_) {
        // bionic __pthread_cleanup_t { prev; routine; arg; } (12 B), a
        // per-thread list. pop(c, execute) runs routine(arg) when asked.
        if (!CheckRange(a0, 12, id == I::__pthread_cleanup_push_)) {
            goto bad_ptr;
        }
        if (id == I::__pthread_cleanup_push_) {
            const std::uint32_t c[3] = {t_cleanup_head, a1, a2};
            std::memcpy(H(a0), c, 12);
            t_cleanup_head = a0;
            Ret(regs, 0);
            return;
        }
        std::uint32_t c[3] = {0, 0, 0};
        std::memcpy(c, H(a0), 12);
        t_cleanup_head = c[0];
        if (a1 != 0 && c[1] != 0) {
            std::int32_t ignored = 0;
            if (!RunComparator(c[1], c[2], 0, ignored)) {
                std::printf("[thread] LOUD cleanup routine %08x(%08x) failed -- halting\n", c[1],
                            c[2]);
                HaltGuest();
                return;
            }
        }
        Ret(regs, 0);
        return;
    }
    case I::sem_destroy_:
    case I::sem_getvalue_:
    case I::sem_init_:
    case I::sem_post_:
    case I::sem_timedwait_:
    case I::sem_trywait_:
    case I::sem_wait_:
    if (id == I::sem_init_ || id == I::sem_destroy_ || id == I::sem_post_ || id == I::sem_wait_ ||
        id == I::sem_trywait_ || id == I::sem_timedwait_ || id == I::sem_getvalue_) {
        const std::uint32_t r = GuestSem(id, n, a0, a1, a2);
        if (r == kSemBadPtr) {
            goto bad_ptr;
        }
        Ret(regs, r);
        return;
    }
    // Guest signals: dispositions, masks and thread-directed signals (see
    // Runtime::GuestSendSignal). sigaction/sigprocmask/tkill/kill/sigsuspend
    // return -1 + errno; pthread_sigmask/pthread_kill return the errno.
    case I::kill_:
    case I::pthread_kill_:
    case I::pthread_sigmask_:
    case I::sigaction_:
    case I::sigprocmask_:
    case I::sigsuspend_:
    case I::tkill_:
    if (id == I::sigaction_ || id == I::sigprocmask_ || id == I::pthread_sigmask_ || id == I::tkill_ ||
        id == I::pthread_kill_ || id == I::kill_ || id == I::sigsuspend_) {
        std::uint32_t e = 0;
        if (id == I::sigaction_) {
            e = GuestSigaction(a0, a1, a2);
        } else if (id == I::sigprocmask_ || id == I::pthread_sigmask_) {
            e = GuestSigmask(a0, a1, a2);
        } else if (id == I::tkill_ || id == I::pthread_kill_) {
            e = GuestSendSignal(a0, a1);
        } else if (id == I::kill_) {
            // Only this process exists for the guest: a signal to it goes to
            // the calling thread, as the kernel picks one that can take it.
            e = (a0 == static_cast<std::uint32_t>(::getpid()) || a0 == 0)
                    ? GuestSendSignal(GuestTid(), a1)
                    : static_cast<std::uint32_t>(ESRCH);
        } else {
            e = GuestSigsuspend(a0);
        }
        if (e == EFAULT) {
            goto bad_ptr;
        }
        if (id == I::pthread_sigmask_ || id == I::pthread_kill_) {
            Ret(regs, e);
        } else if (e != 0) {
            SetGuestErrno(static_cast<int>(e));
            Ret(regs, 0xFFFFFFFFu);
        } else {
            Ret(regs, 0);
        }
        return;
    }
    case I::raise_:
    if (id == I::raise_) {
        if (a0 == 0) {
            Ret(regs, 0);  // signal 0: existence check only
            return;
        }
        // Fatal-by-default signals end the process on a real device (the
        // guests raise SIGFPE from __aeabi_ldiv0 and SIGABRT on abort paths):
        // halt. Others would only reach a handler, and guest handlers are
        // never installed (sigaction above), so they are ignored.
        // ARM Linux numbers: ILL 4, TRAP 5, ABRT 6, BUS 7, FPE 8, KILL 9, SEGV 11.
        const bool fatal = (a0 == 4 || a0 == 5 || a0 == 6 || a0 == 7 || a0 == 8 || a0 == 9 ||
                            a0 == 11);
        if (!fatal) {
            Ret(regs, 0);
            return;
        }
        std::printf("[svc] FATAL raise(%u) pc=%08x lr=%08x -- halting\n", a0, regs[15] - 4, regs[14]);
        stop_step_ = true;
        if (Dynarmic::A32::Jit* jit = ActiveJit()) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
        }
        return;
    }
    case I::geteuid_:
    if (id == I::geteuid_) {
        Ret(regs, static_cast<std::uint32_t>(::geteuid()));
        return;
    }
    case I::towlower_:
    case I::towupper_:
    if (id == I::towlower_ || id == I::towupper_) {
        // C locale: only ASCII letters change.
        std::uint32_t c = a0;
        if (id == I::towlower_ && c >= 'A' && c <= 'Z') c += 32;
        if (id == I::towupper_ && c >= 'a' && c <= 'z') c -= 32;
        Ret(regs, c);
        return;
    }
    case I::wcslen_:
    if (id == I::wcslen_) {
        std::uint32_t i = 0;
        for (;; i++) {
            if (!CheckRange(a0 + i * 4, 4, false)) {
                goto bad_ptr;
            }
            std::uint32_t w = 0;
            std::memcpy(&w, H(a0 + i * 4), 4);
            if (w == 0) {
                break;
            }
        }
        Ret(regs, i);
        return;
    }
    case I::wmemchr_:
    case I::wmemcmp_:
    case I::wmemcpy_:
    case I::wmemmove_:
    case I::wmemset_:
    if (id == I::wmemchr_ || id == I::wmemcmp_ || id == I::wmemcpy_ || id == I::wmemmove_ || id == I::wmemset_) {
        // n counts 4-byte wchar_t units.
        const std::uint64_t bytes = std::uint64_t(a2) * 4;
        if (bytes > 0xFFFFFFFFu) {
            goto bad_ptr;
        }
        const std::size_t nb = static_cast<std::size_t>(bytes);
        if (id == I::wmemset_) {
            if (nb && !CheckRange(a0, nb, true)) {
                goto bad_ptr;
            }
            for (std::uint32_t i = 0; i < a2; i++) {
                std::memcpy(H(a0 + i * 4), &a1, 4);
            }
            Ret(regs, a0);
            return;
        }
        if (id == I::wmemcpy_ || id == I::wmemmove_) {
            if (nb && (!CheckRange(a0, nb, true) || !CheckRange(a1, nb, false))) {
                goto bad_ptr;
            }
            std::memmove(H(a0), H(a1), nb);
            Ret(regs, a0);
            return;
        }
        if (id == I::wmemcmp_) {
            if (nb && (!CheckRange(a0, nb, false) || !CheckRange(a1, nb, false))) {
                goto bad_ptr;
            }
            for (std::uint32_t i = 0; i < a2; i++) {
                std::int32_t x = 0, y = 0;  // bionic wchar_t is signed 32-bit
                std::memcpy(&x, H(a0 + i * 4), 4);
                std::memcpy(&y, H(a1 + i * 4), 4);
                if (x != y) {
                    Ret(regs, x < y ? 0xFFFFFFFFu : 1u);
                    return;
                }
            }
            Ret(regs, 0);
            return;
        }
        // wmemchr(s, c, n)
        for (std::uint32_t i = 0; i < a2; i++) {
            if (!CheckRange(a0 + i * 4, 4, false)) {
                goto bad_ptr;
            }
            std::uint32_t w = 0;
            std::memcpy(&w, H(a0 + i * 4), 4);
            if (w == a1) {
                Ret(regs, a0 + i * 4);
                return;
            }
        }
        Ret(regs, 0);
        return;
    }
    case I::snprintf_:
    case I::sprintf_:
    case I::vsnprintf_:
    case I::vsprintf_:
    if (id == I::snprintf_ || id == I::vsnprintf_ || id == I::sprintf_ || id == I::vsprintf_) {
        const bool sized = (id == I::snprintf_ || id == I::vsnprintf_);
        const bool via_va = (id == I::vsnprintf_ || id == I::vsprintf_);
        const std::uint32_t dst = a0;
        const std::uint32_t cap = sized ? a1 : 0xFFFFFFFFu;
        const std::uint32_t fmtp = sized ? a2 : a1;
        if (!CheckRange(fmtp, 1, false)) {
            goto bad_ptr;
        }
        const std::string fmt(GuestStr(fmtp));  // copy: GuestStr reuses scratch
        GuestFmtArgs st;
        st.gargs[0] = a0;
        st.gargs[1] = a1;
        st.gargs[2] = a2;
        st.gargs[3] = a3;
        st.ai = sized ? 3 : 2;                              // first vararg core reg
        st.via_va = via_va;
        st.stack = via_va ? (sized ? a3 : a2) : regs[13];   // va_list or stack args
        std::size_t total = 0;
        const std::string out = ExpandPrintf(fmt, st, &total);
        if (cap > 0 && CheckRange(dst, 1, true)) {
            // The write goes through the overflow firewall (like memcpy and
            // sscanf do): a guest buffer too small for the result must not
            // smash its neighbour in the heap arena -- the class of the
            // 2026-10-18 "GOT slot zeroed" incident.
            const std::size_t want =
                std::min<std::size_t>(out.size(), (std::size_t)cap - 1) + 1;  // + NUL
            const std::uint32_t allowed = ClampHeapWrite(dst, (std::uint32_t)want, "printf");
            if (allowed > 0 && CheckRange(dst, allowed, true)) {
                const std::size_t w = std::min<std::size_t>(out.size(), allowed - 1);
                std::memcpy(H(dst), out.data(), w);
                H(dst)[w] = 0;
            }
        }
        // C99: the return value is the length that WOULD have been written,
        // not the number of characters that fit.
        Ret(regs, static_cast<std::uint32_t>(total));
        return;
    }
    // --- sleeps: honest (PLAN item 8) --------------------------------------
    case I::nanosleep_:
    if (id == I::nanosleep_) {
        // nanosleep(const struct timespec* req, struct timespec* rem); guest
        // timespec = {long sec; long nsec} (2 x 32 bits). Live: Rio's
        // std::this_thread::sleep_for (0x4428fc).
        if (a0 == 0) {
            SetGuestErrno(EFAULT);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        if (!CheckRange(a0, 8, false) || (a1 != 0 && !CheckRange(a1, 8, true))) {
            goto bad_ptr;
        }
        std::int32_t sec = 0, nsec = 0;
        std::memcpy(&sec, H(a0), 4);
        std::memcpy(&nsec, H(a0 + 4), 4);
        if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
            SetGuestErrno(EINVAL);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        GuestSleepUs(std::uint64_t(sec) * 1000000u + std::uint32_t(nsec) / 1000u);
        if (a1) {
            const std::uint32_t z = 0;
            std::memcpy(H(a1), &z, 4);
            std::memcpy(H(a1 + 4), &z, 4);
        }
        Ret(regs, 0);
        return;
    }
    // --- loud exits ---
    case I::__cxa_guard_abort_:
    case I::__cxa_guard_acquire_:
    case I::__cxa_guard_release_:
    if (id == I::__cxa_guard_acquire_ || id == I::__cxa_guard_release_ || id == I::__cxa_guard_abort_) {
        const std::uint32_t r = CxaGuard(id, n, a0);
        if (r == kSemBadPtr) {
            goto bad_ptr;
        }
        Ret(regs, r);
        return;
    }
    // Destructors registered for exit never run: an Android app process is
    // killed, it does not exit. Registering succeeds, as in bionic.
    case I::__aeabi_atexit_:
    case I::__cxa_atexit_:
    if (id == I::__cxa_atexit_ || id == I::__aeabi_atexit_) {
        Ret(regs, 0);
        return;
    }
    case I::__cxa_finalize_:
    if (id == I::__cxa_finalize_) {
        RetVoid(regs);
        return;
    }
    // __cxa_pure_virtual: a call through a pure virtual slot; libstdc++
    // aborts.
    case I::__assert2_:
    case I::__cxa_pure_virtual_:
    case I::__stack_chk_fail_:
    case I::_exit_:
    case I::abort_:
    case I::exit_:
    if (id == I::abort_ || id == I::__stack_chk_fail_ || id == I::__assert2_ || id == I::exit_ ||
        id == I::_exit_ || id == I::__cxa_pure_virtual_) {
        std::printf("[svc] FATAL %s() called (r0=%08x) pc=%08x lr=%08x -- halting\n", n.c_str(), a0,
                    regs[15] - 4, regs[14]);
        DumpEventRing();
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined3);
        return;
    }
    // pthread_mutexattr_t is one long on the guest (NDK bits/pthread_types.h);
    // bionic keeps the mutex type in its low 4 bits (NORMAL 0, RECURSIVE 1,
    // ERRORCHECK 2). These used to fall into the generic stub, so the type a
    // guest asked for never reached pthread_mutex_init.
    case I::pthread_mutexattr_destroy_:
    case I::pthread_mutexattr_gettype_:
    case I::pthread_mutexattr_init_:
    case I::pthread_mutexattr_settype_:
    if (id == I::pthread_mutexattr_init_ || id == I::pthread_mutexattr_destroy_ ||
        id == I::pthread_mutexattr_settype_ || id == I::pthread_mutexattr_gettype_) {
        if (!CheckRange(a0, 4, id != I::pthread_mutexattr_gettype_)) {
            Ret(regs, EINVAL);
            return;
        }
        std::uint32_t v = 0;
        std::memcpy(&v, H(a0), 4);
        if (id == I::pthread_mutexattr_init_) {
            v = 0;  // PTHREAD_MUTEX_DEFAULT = NORMAL
        } else if (id == I::pthread_mutexattr_settype_) {
            if (a1 > 2) {
                Ret(regs, EINVAL);
                return;
            }
            v = (v & ~0xFu) | a1;
        } else if (id == I::pthread_mutexattr_gettype_) {
            if (!CheckRange(a1, 4, true)) {
                Ret(regs, EINVAL);
                return;
            }
            const std::uint32_t ty = v & 0xFu;
            std::memcpy(H(a1), &ty, 4);
            Ret(regs, 0);
            return;
        }
        std::memcpy(H(a0), &v, 4);
        Ret(regs, 0);
        return;
    }
    // --- pthread: real mutex/cond/key maps, create/join stubbed for now ---
    case I::pthread_mutex_destroy_:
    case I::pthread_mutex_init_:
    case I::pthread_mutex_lock_:
    case I::pthread_mutex_trylock_:
    case I::pthread_mutex_unlock_:
    if (id == I::pthread_mutex_init_ || id == I::pthread_mutex_destroy_ || id == I::pthread_mutex_lock_ ||
        id == I::pthread_mutex_unlock_ || id == I::pthread_mutex_trylock_) {
        Ret(regs, PthreadMutex(id, n, a0, a1));
        return;
    }
    case I::pthread_rwlock_destroy_:
    case I::pthread_rwlock_init_:
    case I::pthread_rwlock_rdlock_:
    case I::pthread_rwlock_unlock_:
    case I::pthread_rwlock_wrlock_:
    if (id == I::pthread_rwlock_init_ || id == I::pthread_rwlock_destroy_ || id == I::pthread_rwlock_rdlock_ ||
        id == I::pthread_rwlock_wrlock_ || id == I::pthread_rwlock_unlock_) {
        Ret(regs, PthreadRwlock(id, n, a0));
        return;
    }
    case I::pthread_cond_broadcast_:
    case I::pthread_cond_destroy_:
    case I::pthread_cond_init_:
    case I::pthread_cond_signal_:
    case I::pthread_cond_timedwait_:
    case I::pthread_cond_timedwait_monotonic_:
    case I::pthread_cond_timedwait_monotonic_np_:
    case I::pthread_cond_wait_:
    if (id == I::pthread_cond_init_ || id == I::pthread_cond_destroy_ || id == I::pthread_cond_signal_ ||
        id == I::pthread_cond_broadcast_ || id == I::pthread_cond_wait_ ||
        id == I::pthread_cond_timedwait_ || id == I::pthread_cond_timedwait_monotonic_ ||
        id == I::pthread_cond_timedwait_monotonic_np_) {
        // The _np name is bionic's other export of the same function
        // (absolute CLOCK_MONOTONIC deadline).
        const bool mono = (id == I::pthread_cond_timedwait_monotonic_ ||
                           id == I::pthread_cond_timedwait_monotonic_np_);
        std::string base = mono ? "pthread_cond_timedwait" : n;
        Ret(regs, PthreadCond(mono ? I::pthread_cond_timedwait_ : id, base, a0, a1, a2, mono));
        return;
    }
    case I::pthread_key_create_:
    if (id == I::pthread_key_create_) {
        Ret(regs, PthreadKeyCreate(a0, a1));
        return;
    }
    case I::pthread_key_delete_:
    if (id == I::pthread_key_delete_) {
        Ret(regs, PthreadKeyDelete(a0));
        return;
    }
    case I::pthread_exit_:
    if (id == I::pthread_exit_) {
        // Only a worker's own code can end its thread (the thread was made by
        // pthread_create and this is its Jit, not a callback or a Java thread).
        if (t_worker_jit == nullptr || ActiveJit() != t_worker_jit) {
            std::printf("[thread] LOUD pthread_exit outside a worker's own code (tid=%u) -- "
                        "halting\n", GuestTid());
            HaltGuest();
            return;
        }
        // Cleanup handlers, innermost first, then the thread ends.
        while (t_cleanup_head != 0) {
            std::uint32_t c[3] = {0, 0, 0};
            if (!CheckRange(t_cleanup_head, 12, false)) {
                break;
            }
            std::memcpy(c, H(t_cleanup_head), 12);
            t_cleanup_head = c[0];
            std::int32_t ignored = 0;
            if (c[1] && !RunComparator(c[1], c[2], 0, ignored)) {
                std::printf("[thread] LOUD cleanup routine %08x(%08x) failed -- halting\n", c[1],
                            c[2]);
                HaltGuest();
                return;
            }
        }
        t_exit_value = a0;
        t_thread_exiting = true;
        ActiveJit()->HaltExecution(kThreadExitHalt);
        return;
    }
    case I::pthread_getspecific_:
    if (id == I::pthread_getspecific_) {
        Ret(regs, PthreadGetSpecific(a0));
        return;
    }
    case I::pthread_setspecific_:
    if (id == I::pthread_setspecific_) {
        Ret(regs, PthreadSetSpecific(a0, a1));
        return;
    }
    case I::pthread_gettid_np_:
    case I::pthread_self_:
    if (id == I::pthread_self_ || id == I::pthread_gettid_np_) {
        Ret(regs, GuestTid());
        return;
    }
    case I::pthread_equal_:
    if (id == I::pthread_equal_) {
        Ret(regs, a0 == a1 ? 1 : 0);
        return;
    }
    case I::pthread_create_:
    if (id == I::pthread_create_) {
        // HARNESS_NOWORKER=1: skip thread creation (return 0, no thread).
        // Diagnosis-only: removes worker/main interleaving nondeterminism
        // (shared bump-heap addresses -> hash orders -> path variance).
        // Joins will hang if reached; the font wall dies long before.
        // NOTE: any value (even empty) counts as set; use env -u to run real
        // worker threads.
        if (EnvOn("HARNESS_NOWORKER")) {
            std::printf("[thread] NOWORKER: skip create start=%08x\n", a2);
            if (a0 && CheckRange(a0, 4, true)) {
                std::uint32_t zero = 0;
                std::memcpy(H(a0), &zero, 4);
            }
            Ret(regs, 0);
            return;
        }
        // TEMP-DIAG (worker hang): log the job name (inline C string at
        // [arg+0x20], seen as "Configuration") so runs inventory jobs.
        // Also scan [arg,arg+0x200] for an http URL (job identity).
        if (DiagOn()) {  // job-name / URL sniffing is diagnostics only
            std::string jn = "?";
            if (CheckRange(a3 + 0x20, 32, false)) {
                char tmp[33] = {0};
                for (int i = 0; i < 32; i++) {
                    char c = 0;
                    std::memcpy(&c, H(a3 + 0x20 + (std::uint32_t)i), 1);
                    if (!c) {
                        break;
                    }
                    tmp[i] = (c >= 32 && c < 127) ? c : '?';
                }
                if (tmp[0]) {
                    jn = tmp;
                }
            }
            std::string surl = "-";
            if (CheckRange(a3, 0x200, false)) {
                for (std::uint32_t k = 0; k + 8 < 0x200; k++) {
                    char c4[5] = {0};
                    std::memcpy(c4, H(a3 + k), 4);
                    if (!std::strcmp(c4, "http")) {
                        char ub[81] = {0};
                        for (int j = 0; j < 80; j++) {
                            char c = 0;
                            if (!CheckRange(a3 + k + j, 1, false)) {
                                break;
                            }
                            std::memcpy(&c, H(a3 + k + j), 1);
                            if (!c) {
                                break;
                            }
                            ub[j] = (c >= 32 && c < 127) ? c : '?';
                        }
                        surl = ub;
                        break;
                    }
                }
            }
            std::printf("[thread] create start=%08x arg=%08x job=\"%s\" url=\"%s\"\n", a2, a3,
                        jn.c_str(), surl.c_str());
        }
        Ret(regs, PthreadCreate(a0, a1, a2, a3));
        return;
    }
    case I::pthread_join_:
    if (id == I::pthread_join_) {
        Ret(regs, PthreadJoin(a0, a1));
        return;
    }
    case I::pthread_detach_:
    if (id == I::pthread_detach_) {
        Ret(regs, PthreadDetach(a0));
        return;
    }
    case I::sched_yield_:
    if (id == I::sched_yield_) {
        std::this_thread::yield();
        Ret(regs, 0);
        return;
    }
    case I::poll_:
    if (id == I::poll_) {
        // poll(struct pollfd* fds, nfds_t nfds, int timeout). struct pollfd is
        // {int fd; short events; short revents} = 8 bytes on both sides.
        // POSIX: an entry with fd < 0 is ignored and gets revents = 0; an fd
        // that is not open gets POLLNVAL and counts in the result. The
        // timeout is passed through unchanged (-1 = wait forever).
        if (a1 > kGuestPollMaxFds) {
            SetGuestErrno(EINVAL);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        if (a1 != 0 && !CheckRange(a0, static_cast<std::size_t>(a1) * 8, true)) {
            goto bad_ptr;
        }
        std::vector<struct pollfd> pfds(a1);
        std::vector<char> invalid(a1, 0);
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            for (std::uint32_t i = 0; i < a1; i++) {
                std::int32_t gfd = 0;
                std::int16_t ev = 0;
                std::memcpy(&gfd, H(a0 + i * 8), 4);
                std::memcpy(&ev, H(a0 + i * 8 + 4), 2);
                pfds[i].events = ev;
                pfds[i].revents = 0;
                if (gfd < 0) {
                    pfds[i].fd = -1;  // ignored by the kernel too
                } else if (gfd <= 2) {
                    pfds[i].fd = gfd;  // stdio is shared with the host
                } else {
                    const auto it = fd_map_.find(static_cast<std::uint32_t>(gfd));
                    if (it == fd_map_.end()) {
                        pfds[i].fd = -1;
                        invalid[i] = 1;
                    } else {
                        pfds[i].fd = it->second;
                    }
                }
            }
        }
        std::uint32_t n_invalid = 0;
        for (char c : invalid) {
            n_invalid += c ? 1u : 0u;
        }
        // An fd that is not open is reported at once, as the kernel does: no
        // wait when at least one entry is already "ready" with POLLNVAL.
        const int timeout = n_invalid ? 0 : static_cast<int>(a2);
        if (DiagOn()) {
            static std::uint64_t poll_hits = 0;
            if (++poll_hits <= 400) {
                std::printf("[poll][t%u] nfds=%u to=%d invalid=%u\n", GuestTid(), a1,
                            static_cast<int>(a2), n_invalid);
            }
        }
        int r = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout);
        if (r < 0) {
            SetGuestErrno(errno);
            Ret(regs, 0xFFFFFFFFu);
            return;
        }
        for (std::uint32_t i = 0; i < a1; i++) {
            std::int16_t re = pfds[i].revents;
            if (invalid[i]) {
                re = static_cast<std::int16_t>(POLLNVAL);
            }
            std::memcpy(H(a0 + i * 8 + 6), &re, 2);
        }
        Ret(regs, static_cast<std::uint32_t>(r) + n_invalid);
        return;
    }
    case I::pthread_once_:
    if (id == I::pthread_once_) {
        if (PthreadOnce(a0, a1, regs)) {
            return;  // hijacked: control redirected, no Ret()
        }
        Ret(regs, 0);
        return;
    }
    case I::__gnu_Unwind_Find_exidx_:
    if (id == I::__gnu_Unwind_Find_exidx_) {
        // Nest guard (subagent #6): the phase driver can re-present the same
        // handler frame forever (pad transfer never happens) — 131K nested
        // throws, 64B/level, 8MB stack blow. Legit retry loops REUSE stack;
        // nesting marches sp down monotonically. Break on same-pc + strictly
        // descending sp past the threshold: return can't-unwind (0) so the
        // run terminates with the ORIGINAL exception visible instead of a
        // silent overflow. Matches device semantics (uncaught -> terminate).
        // a0 = pc, a1 = frame sp. Tracked per stack range (main vs workers).
        struct NestTrack {
            std::uint32_t pc = 0xFFFFFFFFu;
            std::uint32_t sp = 0xFFFFFFFFu;
            unsigned depth = 0;
        };
        static NestTrack tracks[2];
        NestTrack& tr = (a1 >= abr::kStackBase && a1 < abr::kStackTop) ? tracks[0] : tracks[1];
        if (a0 == tr.pc && a1 != 0 && a1 < tr.sp) {
            tr.depth++;
        } else {
            tr.depth = 0;
            tr.pc = a0;
            tr.sp = a1;
        }
        if (a0 == tr.pc && a1 != 0) {
            tr.sp = a1;
        }
        // HARNESS_NESTMAX overrides the abort depth (default 512) to test
        // whether a deeper handler catches (fallback paths) vs true spiral.
        static std::int64_t nestmax = -2;
        if (nestmax == -2) {
            nestmax = 512;
            if (const char* ne = std::getenv("HARNESS_NESTMAX")) {
                nestmax = (std::int64_t)std::strtoul(ne, nullptr, 0);
            }
        }
        if ((std::int64_t)tr.depth > nestmax) {
            // Returning 0 does NOT stop this driver (it takes the NULL path
            // and re-presents). Halt loud: the stack holds the whole spiral
            // and the original throw is ~512 levels up, intact for forensics.
            std::printf("[unwind] LOUD nesting %u deep at pc=%08x sp=%08x -- halting\n", tr.depth,
                        a0, a1);
            // Forensics for this fatal halt: R7 and the words the SU16
            // pop-frame reads ([R7-12..R7+8]).
            {
                std::uint32_t r7v = regs[7];
                std::printf("[unwind] R7=%08x SP=%08x LR=%08x : ", r7v, regs[13], regs[14]);
                for (int k = -12; k <= 8; k += 4) {
                    std::uint32_t w = 0xDEADBEEFu;
                    if (CheckRange(r7v + k, 4, false)) {
                        std::memcpy(&w, H<const std::uint8_t>(r7v + k), 4);
                    }
                    std::printf("[%+d]=%08x ", k, w);
                }
                std::printf("\n");
            }
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        // HARNESS_EXIDXLOG=1: temp-diag (unwinder spiral): log every lookup.
        if (EnvOn("HARNESS_EXIDXLOG")) {
            std::printf("[exidxlog] pc=%08x -> entry=%08x R7=%08x SP=%08x LR=%08x\n", a0,
                        FindExidx(a0), regs[7], regs[13], regs[14]);
        }
        // The imported ABI is (pc, int* count) -> the exidx TABLE BASE of the
        // module holding pc, with *count = number of 8-byte entries; the caller
        // (the guest's own statically linked libgcc) then binary-searches that
        // table. This used to return the single table ENTRY for pc and never
        // write count, so a caller took one entry as the table base. The pinned
        // Rio image was deliberately kept on that legacy answer because its
        // fail path "depended" on the 0x45A778 spiral (which RETSUCC masked).
        // That was a crutch, and on a phone it is fatal: with count==0 the
        // guest's setInfo marks the frame `_unwindInfoMissing`, phase1 returns
        // FATAL before any descriptor runs, and the first main-thread throw
        // wedges the updater (black screen). The ABI answer is correct for
        // every image: frame #1 then pops its saved LR (0x457FE7) and the walk
        // continues into __cxa_throw and the game's own handler.
        const std::uint32_t pc = a0 & ~std::uint32_t(1);
        std::uint32_t base = 0, count = 0;
        if (IsCodeAddress(pc)) {
            ExidxFor(pc, &base, &count);
        }
        if (a1 != 0 && CheckRange(a1, 4, true)) {
            std::memcpy(H(a1), &count, 4);
        }
        Ret(regs, base);
        return;
    }
    // --- file bridge (bring-up) ---
    // Guest paths under files_prefix_ map to files_hostbase_ (harness sets it
    // to <project>/work/files). /dev/null|zero|urandom pass to host same.
    // Anything else fails LOUD (discovery: see what the engine wants).
    // NOTE: guest struct stat uses the NDK-r27 ARM layout (88B+... see below);
    // 2014-bionic compat verified only by game behavior (st_size/st_mode reads).
    // Locking: NO whole-bridge guard (it used to be mutex_ across everything,
    // and a worker parked in blocking ::read froze all threads). fd/file/dir
    // maps use brief io_mutex_ holds at each touch; blocking syscalls always
    // run unlocked. files_prefix_/hostbase_ are startup-immutable (no lock).
    case I::accept_:
    case I::access_:
    case I::bind_:
    case I::chdir_:
    case I::chmod_:
    case I::clearerr_:
    case I::close_:
    case I::closedir_:
    case I::connect_:
    case I::dup_:
    case I::dup2_:
    case I::epoll_create_:
    case I::epoll_ctl_:
    case I::epoll_wait_:
    case I::fclose_:
    case I::fcntl_:
    case I::fdopen_:
    case I::feof_:
    case I::ferror_:
    case I::fflush_:
    case I::fgets_:
    case I::fileno_:
    case I::flock_:
    case I::fopen_:
    case I::fprintf_:
    case I::fputc_:
    case I::fputs_:
    case I::fread_:
    case I::fseek_:
    case I::fseeko_:
    case I::fstat_:
    case I::fsync_:
    case I::ftell_:
    case I::ftello_:
    case I::ftruncate_:
    case I::fwrite_:
    case I::getc_:
    case I::getcwd_:
    case I::getpeername_:
    case I::getsockname_:
    case I::getsockopt_:
    case I::inet_addr_:
    case I::inet_aton_:
    case I::inet_ntoa_:
    case I::inet_ntop_:
    case I::inet_pton_:
    case I::ioctl_:
    case I::isatty_:
    case I::listen_:
    case I::lseek_:
    case I::lstat_:
    case I::mkdir_:
    case I::mkstemp_:
    case I::open_:
    case I::opendir_:
    case I::pathconf_:
    case I::pipe_:
    case I::pipe2_:
    case I::pread_:
    case I::putc_:
    case I::pwrite_:
    case I::read_:
    case I::readdir_:
    case I::readdir_r_:
    case I::recv_:
    case I::recvfrom_:
    case I::recvmsg_:
    case I::remove_:
    case I::rename_:
    case I::rmdir_:
    case I::select_:
    case I::send_:
    case I::sendfile_:
    case I::sendmsg_:
    case I::sendto_:
    case I::setsockopt_:
    case I::setvbuf_:
    case I::shutdown_:
    case I::socket_:
    case I::stat_:
    case I::statfs_:
    case I::tmpfile_:
    case I::truncate_:
    case I::ungetc_:
    case I::unlink_:
    case I::utime_:
    case I::vfprintf_:
    case I::write_:
    case I::writev_:
    {
        auto map_path = [this](std::uint32_t gp) -> std::string {
            return MapGuestPath(gp);
        };
        // A guest path outside the mapped areas (files dir, assets, /proc,
        // /sys, /dev ...) does not exist for the guest: fail the call with
        // ENOENT like the kernel would (it used to halt the whole thread on a
        // mere existence check). The result is NULL for the pointer-returning
        // calls and -1 for the rest; the caller just returns afterwards.
        auto need_host = [&](const std::string& h, const char* what) -> bool {
            if (!h.empty()) {
                return true;
            }
            static std::atomic<int> unmapped_logs{0};
            if (unmapped_logs.fetch_add(1) < 20) {
                std::printf("[files] %s(\"%s\"): unmapped guest path -> ENOENT\n", what,
                            CheckRange(a0, 1, false) ? GuestStr(a0) : "?");
            }
            SetGuestErrno(ENOENT);
            const bool ptr_result = std::strcmp(what, "fopen") == 0 ||
                                    std::strcmp(what, "opendir") == 0 ||
                                    std::strcmp(what, "freopen") == 0;
            Ret(regs, ptr_result ? 0u : 0xFFFFFFFFu);
            return false;
        };
        auto ensure_parent = [&](const std::string& hp) {
            const std::size_t slash = hp.rfind('/');
            if (slash != std::string::npos && slash > 0) {
                std::error_code ec;
                std::filesystem::create_directories(hp.substr(0, slash), ec);
            }
        };
        // Packed files (apk_vfs.h): the packed file at host path hp when
        // there is no real one (a real file, even a dangling link, wins).
        auto packed_at = [&](const std::string& hp) -> const abvfs::File* {
            if (!vfs_on_) {
                return nullptr;
            }
            struct stat st {};
            if (::lstat(hp.c_str(), &st) == 0 || errno != ENOENT) {
                return nullptr;
            }
            return vfs_.Find(hp);
        };
        // Makes the real file a write needs at hp: an empty one when the
        // caller truncates anyway, else a copy of the packed one.
        auto materialize = [&](const abvfs::File& pf, const std::string& hp, bool truncating) -> bool {
            if (truncating) {
                const int fd = ::open(hp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                if (fd < 0) {
                    return false;
                }
                ::close(fd);
                return true;
            }
            return vfs_.CopyUp(pf, hp);
        };
        auto file_of = [&](std::uint32_t h) -> FILE* {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = file_map_.find(h);
            return it == file_map_.end() ? nullptr : it->second.f;
        };
        auto new_file_handle = [&]() -> std::uint32_t {
            const std::uint32_t h = GuestMalloc(96);  // opaque, Bionic-compatible size
            if (!h) {
                return 0;
            }
            std::memset(H(h), 0, 96);
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                file_map_[h] = HostFile{nullptr};
            }
            return h;
        };
        // The guest fd table is a 15-bit namespace (a FILE* carries its fd in
        // a 16-bit _file field). Reuse only numbers whose host fd is closed
        // and whose FILE* (if any) is gone; otherwise a stale FILE* could
        // later erase a new mapping.
        auto take_guest_fd_locked = [&]() -> std::uint32_t {
            if (!free_guest_fds_.empty()) {
                const std::uint32_t g = free_guest_fds_.back();
                free_guest_fds_.pop_back();
                return g;
            }
            if (next_guest_fd_ >= 0x8000u) {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true)) {
                    std::printf("[files] LOUD guest fd table exhausted (cap %u)\n", 0x8000u);
                }
                return 0xFFFFFFFFu;
            }
            return next_guest_fd_++;
        };
        auto fd_referenced_by_file_locked = [&](std::uint32_t g) -> bool {
            for (const auto& kv : file_map_) {
                if (kv.second.gfd == g) {
                    return true;
                }
            }
            return false;
        };
        auto release_guest_fd_locked = [&](std::uint32_t g) {
            if (g < 100u || g >= 0x8000u || fd_referenced_by_file_locked(g)) {
                return;
            }
            if (std::find(free_guest_fds_.begin(), free_guest_fds_.end(), g) ==
                free_guest_fds_.end()) {
                free_guest_fds_.push_back(g);
            }
        };
        auto discard_file_handle = [&](std::uint32_t h) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                file_map_.erase(h);
            }
            GuestFree(h);
        };
        // Bind a host FILE*: allocate/reuse a guest fd (or adopt an existing
        // fd for fdopen), expose it at Bionic __sFile offset +14 (_file,
        // ldrsh-loaded inline by guest code for fsync etc.) and in fd_map_ for
        // fd-based thunks. Returns false on exhaustion.
        auto bind_file = [&](std::uint32_t h, FILE* f, std::uint32_t existing_gfd = 0xFFFFFFFFu,
                             int host_fd = -1,
                             std::shared_ptr<abvfs::Slice> slice = nullptr) -> bool {
            std::uint32_t g = existing_gfd;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                if (g == 0xFFFFFFFFu) {
                    g = take_guest_fd_locked();
                    if (g == 0xFFFFFFFFu) {
                        return false;
                    }
                } else if (fd_map_.find(g) == fd_map_.end()) {
                    return false;
                }
                fd_map_[g] = host_fd >= 0 ? host_fd : fileno(f);
                SetFdSliceLocked(g, std::move(slice));
                file_map_[h].f = f;
                file_map_[h].gfd = g;
            }
            const std::uint16_t g16 = static_cast<std::uint16_t>(g);
            std::memcpy(H(h + 14), &g16, 2);
            return true;
        };
        // Brief guarded host-fd lookup (-1 if unmapped). I/O on the result
        // runs UNLOCKED — never hold io_mutex_ across syscalls.
        auto host_fd_of = [&](std::uint32_t g) -> int {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = fd_map_.find(g);
            return it == fd_map_.end() ? -1 : it->second;
        };
        auto alloc_guest_fd = [&](int hfd) -> std::uint32_t {
            std::lock_guard<std::mutex> io(io_mutex_);
            const std::uint32_t g = take_guest_fd_locked();
            if (g == 0xFFFFFFFFu) {
                return g;
            }
            fd_map_[g] = hfd;
            SetFdSliceLocked(g, nullptr);
            return g;
        };
        auto erase_guest_fd = [&](std::uint32_t g) -> int {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = fd_map_.find(g);
            if (it == fd_map_.end()) {
                return -1;
            }
            const int h = it->second;
            fd_map_.erase(it);
            SetFdSliceLocked(g, nullptr);
            release_guest_fd_locked(g);
            return h;
        };
        // Guest errno cell write (per-thread slot) + poll wait helper.
        // Defined here (used by both the read cap below and the socket
        // bridge further down).
        auto set_guest_errno = [&](int e) {
            const std::uint32_t s = ErrnoSlot();
            if (CheckRange(s, 4, true)) {
                const std::int32_t v = static_cast<std::int32_t>(e);
                std::memcpy(H(s), &v, 4);
            }
        };
        // Offline by design (R-005): unless the title's packaging enabled the
        // network (profile.toml `network = "online"`), the engine never talks
        // to Rovio's servers. Some are dead; live ones answer with states these
        // old builds cannot survive (Space: 573 "App is disabled" -> abort;
        // SW2 amz: "Signed out", saved, aborting on every boot). As on a phone
        // in airplane mode, a connect (TCP or UDP) or sendto to a non-loopback
        // IPv4/IPv6 address fails at once with ENETUNREACH; the bundled DNS
        // resolver then reports "Could not contact DNS servers", the shape of
        // the original Rio's airplane-mode logcat. The guest sockaddr layout is
        // the host's (family u16 LE, port BE, then the address).
        auto offline_refuses = [&](std::uint32_t addr, std::uint32_t len) -> bool {
            if (network_allowed_) {
                return false;
            }
            const std::uint8_t* sa = H<const std::uint8_t>(addr);
            const unsigned fam = len >= 2 ? (sa[0] | (sa[1] << 8)) : 0u;
            bool remote = false;
            if (fam == AF_INET && len >= 8) {
                remote = sa[4] != 127;
            } else if (fam == AF_INET6 && len >= 24) {
                static const std::uint8_t kLoop6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                        0, 0, 0, 0, 0, 0, 0, 1};
                static const std::uint8_t kMapped4[12] = {0, 0, 0, 0, 0, 0,
                                                          0, 0, 0, 0, 0xFF, 0xFF};
                const std::uint8_t* ad = sa + 8;
                remote = std::memcmp(ad, kLoop6, 16) != 0 &&
                         !(std::memcmp(ad, kMapped4, 12) == 0 && ad[12] == 127);
            }
            if (remote) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 8) {
                    std::printf("[net] offline: %s to %s port %u refused\n", n.c_str(),
                                fam == AF_INET ? "IPv4" : "IPv6",
                                (unsigned)((sa[2] << 8) | sa[3]));
                }
            }
            return remote;
        };
        auto fill_stat = [&](std::uint32_t dst, const struct stat& st) -> bool {
            // Bionic arm32 struct stat, 104 bytes (NDK sys/stat.h; offsets
            // checked with offsetof under the NDK clang for armv7a). Note the
            // 4-byte pads after st_dev and st_rdev and the 8-byte alignment of
            // st_size/st_blocks; Rio reads st_size with ldrd [sp,#48].
            if (!CheckRange(dst, kGuestStatSize, true)) {
                return false;
            }
            std::uint8_t* p = H<std::uint8_t>(dst);
            std::memset(p, 0, kGuestStatSize);
            auto w64 = [&](std::size_t o, std::uint64_t v) { std::memcpy(p + o, &v, 8); };
            auto w32 = [&](std::size_t o, std::uint32_t v) { std::memcpy(p + o, &v, 4); };
            w64(0, (std::uint64_t)st.st_dev);
            w32(12, (std::uint32_t)st.st_ino);  // __st_ino
            w32(16, (std::uint32_t)st.st_mode);
            w32(20, (std::uint32_t)st.st_nlink);
            w32(24, (std::uint32_t)st.st_uid);
            w32(28, (std::uint32_t)st.st_gid);
            w64(32, (std::uint64_t)st.st_rdev);
            w64(48, (std::uint64_t)st.st_size);
            w32(56, (std::uint32_t)st.st_blksize);
            w64(64, (std::uint64_t)st.st_blocks);
            w32(72, (std::uint32_t)st.st_atim.tv_sec);
            w32(76, (std::uint32_t)st.st_atim.tv_nsec);
            w32(80, (std::uint32_t)st.st_mtim.tv_sec);
            w32(84, (std::uint32_t)st.st_mtim.tv_nsec);
            w32(88, (std::uint32_t)st.st_ctim.tv_sec);
            w32(92, (std::uint32_t)st.st_ctim.tv_nsec);
            w64(96, (std::uint64_t)st.st_ino);
            return true;
        };
        auto fill_statfs = [&](std::uint32_t dst, const struct statfs& st) -> bool {
            // Bionic arm32 struct statfs, 88 bytes (NDK sys/vfs.h; offsets
            // checked with offsetof under the NDK clang for armv7a): u32
            // f_type@0, f_bsize@4; u64 f_blocks@8, f_bfree@16, f_bavail@24,
            // f_files@32, f_ffree@40; fsid[2]@48; u32 f_namelen@56,
            // f_frsize@60, f_flags@64; spare[4]@68.
            if (!CheckRange(dst, kGuestStatfsSize, true)) {
                return false;
            }
            std::uint8_t* p = H<std::uint8_t>(dst);
            std::memset(p, 0, kGuestStatfsSize);
            auto w64 = [&](std::size_t o, std::uint64_t v) { std::memcpy(p + o, &v, 8); };
            auto w32 = [&](std::size_t o, std::uint64_t v) {
                const std::uint32_t w = static_cast<std::uint32_t>(v);
                std::memcpy(p + o, &w, 4);
            };
            w32(0, (std::uint64_t)st.f_type);
            w32(4, (std::uint64_t)st.f_bsize);
            w64(8, (std::uint64_t)st.f_blocks);
            w64(16, (std::uint64_t)st.f_bfree);
            w64(24, (std::uint64_t)st.f_bavail);
            w64(32, (std::uint64_t)st.f_files);
            w64(40, (std::uint64_t)st.f_ffree);
            // f_fsid: glibc names the array __val, bionic val; copy the bytes.
            std::memcpy(p + 48, &st.f_fsid, 8);
            w32(56, (std::uint64_t)st.f_namelen);
            w32(60, (std::uint64_t)st.f_frsize);
            w32(64, (std::uint64_t)st.f_flags);
            return true;
        };
        if (id == I::fopen_) {
            const std::string hp = map_path(a0);
            if (!need_host(hp, "fopen")) {
                return;
            }
            const std::string mode = GuestStr(a1);
            if (const abvfs::File* pf = packed_at(hp)) {
                if (mode.find('x') != std::string::npos) {
                    SetGuestErrno(EEXIST);
                    Ret(regs, 0);
                    return;
                }
                if (mode[0] == 'r' && mode.find('+') == std::string::npos) {
                    // Read-only: a FILE over the packed bytes, with a guest fd.
                    std::shared_ptr<abvfs::Slice> sl;
                    const int hfd = vfs_.Open(*pf, &sl);
                    FILE* pfile = hfd >= 0 ? abvfs::SliceFile(hfd, sl) : nullptr;
                    if (!pfile) {
                        SetGuestErrno(errno ? errno : EIO);
                        if (hfd >= 0) {
                            ::close(hfd);
                        }
                        Ret(regs, 0);
                        return;
                    }
                    const std::uint32_t h = new_file_handle();
                    if (!h || !bind_file(h, pfile, 0xFFFFFFFFu, hfd, sl)) {
                        std::fclose(pfile);
                        if (h) {
                            discard_file_handle(h);
                        }
                        Ret(regs, 0);
                        return;
                    }
                    Ret(regs, h);
                    return;
                }
                // "w" truncates; "r+" and "a" keep the bytes: copy up first.
                if (!materialize(*pf, hp, mode[0] == 'w')) {
                    SetGuestErrno(errno ? errno : EIO);
                    Ret(regs, 0);
                    return;
                }
            }
            if (mode.find_first_of("wax+") != std::string::npos) {
                ensure_parent(hp);
            }
            FILE* f = std::fopen(hp.c_str(), mode.c_str());
            if (EnvOn("HARNESS_PATHLOG")) {
                std::printf("[files][t%u] fopen(\"%s\",\"%s\") -> %s\n", GuestTid(), hp.c_str(),
                            mode.c_str(), f ? "ok" : "NULL");
            }
            if (!f) {
                if (DiagOn()) {
                    std::printf("[files] fopen(\"%s\",\"%s\") -> NULL (errno %d)\n", hp.c_str(),
                                mode.c_str(), errno);
                }
                SetGuestErrno(errno);
                Ret(regs, 0);
                return;
            }
            const std::uint32_t h = new_file_handle();
            if (!h) {
                std::fclose(f);
                Ret(regs, 0);
                return;
            }
            if (!bind_file(h, f)) {
                std::fclose(f);
                discard_file_handle(h);
                Ret(regs, 0);
                return;
            }
            if (DiagOn()) {
                std::printf("[files][t%u] fopen(\"%s\",\"%s\") -> %08x\n", GuestTid(), hp.c_str(),
                            mode.c_str(), h);
            }
            // TEMP-DIAG (registry ParseError): track registry handles to dump
            // their exact bytes on fread/fwrite below.
            // Diagnostics only: in shipping this dumped save-file bytes to the log.
            if (DiagOn() && hp.find("registry") != std::string::npos) {
                std::lock_guard<std::mutex> io(io_mutex_);
                g_regfiles.insert(f);
            }
            Ret(regs, h);
            return;
        }
        if (id == I::fflush_ && a0 == 0) {
            // fflush(NULL) flushes every output stream and returns 0.
            std::fflush(nullptr);
            Ret(regs, 0);
            return;
        }
        if (id == I::fclose_ || id == I::fflush_) {
            FILE* f = file_of(a0);
            if (!f) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (id == I::fclose_) {
                // The guest's std streams point into __sF; closing them must
                // not shut down the host's own stdout/stderr. Flush and report
                // success (bionic fclose on a standard stream does close the fd,
                // but on Android that only affects the process's own copy --
                // the host bridge's stderr must stay open for LOUD diagnostics).
                if (f == stdout || f == stderr || f == stdin) {
                    std::fflush(f);
                    Ret(regs, 0);
                    return;
                }
            }
            if (id == I::fclose_ && DiagOn()) {
                std::lock_guard<std::mutex> io(io_mutex_);
                g_regfiles.erase(f);  // a reused FILE* must not inherit the tag
            }
            int r = (id == I::fclose_) ? std::fclose(f) : std::fflush(f);
            if (id == I::fclose_) {
                // fclose owns both the host fd and the 96-byte guest FILE.
                // Erase the fd mapping and only then make the number
                // reusable; GuestFree must run outside io_mutex_.
                std::uint32_t gfd = 0xFFFFFFFFu;
                {
                    std::lock_guard<std::mutex> io(io_mutex_);
                    const auto it = file_map_.find(a0);
                    if (it != file_map_.end()) {
                        gfd = it->second.gfd;
                        file_map_.erase(it);
                    }
                    if (gfd != 0xFFFFFFFFu) {
                        fd_map_.erase(gfd);
                        SetFdSliceLocked(gfd, nullptr);
                        release_guest_fd_locked(gfd);
                    }
                }
                GuestFree(a0);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::fread_) {
            // fread(ptr=a0, size=a1, nmemb=a2, stream=a3): the handle is LAST.
            // (An earlier revision keyed on a0 and silently returned 0 — every
            // file read was empty; empty seed files masked it.)
            FILE* f = file_of(a3);
            if (!f || !CheckRange(a0, 1, false) || a1 == 0 || a2 == 0) {
                Ret(regs, 0);
                return;
            }
            // Clamp absurd sizes (guest bug shield; host fread would try).
            if (std::uint64_t(a1) * a2 > 0x1000000u && !CheckRange(a0, (std::size_t)a1 * a2, true)) {
                Ret(regs, 0);
                return;
            }
            if (!CheckRange(a0, (std::size_t)a1 * a2, true)) {
                Ret(regs, 0);
                return;
            }
            // Overflow firewall (same class as memcpy): the arena-wide
            // CheckRange above passes short game buffers; clamp the item
            // count to the mapped allocation so ::fread can't smash chunk
            // neighbors (silent host SIGSEGV in dlmalloc metadata).
            const std::uint64_t want64 = (std::uint64_t)a1 * a2;
            const std::uint32_t wbytes =
                ClampHeapWrite(a0, want64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (std::uint32_t)want64,
                               "fread");
            const std::uint32_t wn = a1 ? wbytes / a1 : 0;
            std::size_t got = std::fread(H(a0), a1, wn, f);
            if (got < wn && wn == a2) {
                SetGuestErrno(errno ? errno : EIO);
            }
            if (wn < a2) {
                static int frc = 0;
                if (frc < 10) {
                    frc++;
                    std::printf("[heap] LOUD fread short items=%u want=%u\n", wn, a2);
                }
            }
            // TEMP-DIAG (registry ParseError): dump exact bytes read.
            bool regdump = false;
            if (DiagOn()) {
                std::lock_guard<std::mutex> io(io_mutex_);
                regdump = g_regfiles.count(f) && g_regdump_n < 60;
            }
            if (regdump) {
                g_regdump_n++;
                std::size_t nb = got * a1;
                if (nb > 192) {
                    nb = 192;
                }
                const std::uint8_t* p = H<const std::uint8_t>(a0);
                std::printf("[regio] fread #%d %zu items(%zuB):", g_regdump_n, static_cast<std::size_t>(got),
                            static_cast<std::size_t>(got) * a1);
                for (std::size_t i = 0; i < nb; i++) {
                    std::printf(" %02x", p[i]);
                }
                std::printf("\n");
            }
            Ret(regs, static_cast<std::uint32_t>(got));
            return;
        }
        if (id == I::fwrite_) {
            // fwrite(ptr=a0, size=a1, nmemb=a2, stream=a3): handle is LAST
            // (same a0-keying bug as fread had — fixed together).
            FILE* f = file_of(a3);
            if (!f || a1 == 0 || a2 == 0 || !CheckRange(a0, (std::size_t)a1 * a2, false)) {
                Ret(regs, 0);
                return;
            }
            // TEMP-DIAG (registry ParseError): dump exact bytes written.
            bool regdump = false;
            if (DiagOn()) {
                std::lock_guard<std::mutex> io(io_mutex_);
                regdump = g_regfiles.count(f) && g_regdump_n < 60;
            }
            if (regdump) {
                g_regdump_n++;
                std::size_t nb = (std::size_t)a1 * a2;
                std::size_t show = nb > 192 ? 192 : nb;
                const std::uint8_t* p = H<const std::uint8_t>(a0);
                std::printf("[regio] fwrite #%d %uB:", g_regdump_n, (unsigned)nb);
                for (std::size_t i = 0; i < show; i++) {
                    std::printf(" %02x", p[i]);
                }
                std::printf("%s\n", nb > show ? " ..." : "");
            }
            const std::size_t wrote = std::fwrite(H(a0), a1, a2, f);
            if (wrote < a2) {
                SetGuestErrno(errno ? errno : EIO);
            }
            Ret(regs, static_cast<std::uint32_t>(wrote));
            return;
        }
        if (id == I::fseek_ || id == I::fseeko_ || id == I::ftell_ || id == I::ftello_) {
            FILE* f = file_of(a0);
            if (!f) {
                Ret(regs, (id == I::fseek_ || id == I::fseeko_) ? static_cast<std::uint32_t>(-1) : 0u);
                return;
            }
            if (id == I::ftell_ || id == I::ftello_) {
                Ret(regs, static_cast<std::uint32_t>(std::ftell(f)));
                return;
            }
            // fseek/fseeko: the guest's long/off_t is a signed 32-bit value
            // (Rio calls fseeko(f, -n, SEEK_CUR) at 0x274314).
            const int r = std::fseek(f, static_cast<long>(static_cast<std::int32_t>(a1)), (int)a2);
            if (r != 0) {
                SetGuestErrno(errno);
            }
            if (EnvOn("HARNESS_PATHLOG")) {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 4000) {
                    std::printf("[files][t%u] fseek(%08x, %d, %u) -> pos %ld\n", GuestTid(), a0,
                                static_cast<std::int32_t>(a1), a2, std::ftell(f));
                }
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::feof_ || id == I::ferror_ || id == I::clearerr_ || id == I::fileno_) {
            FILE* f = file_of(a0);
            if (!f) {
                Ret(regs, (id == I::fileno_) ? static_cast<std::uint32_t>(-1) : 0u);
                return;
            }
            if (id == I::feof_) {
                Ret(regs, (std::uint32_t)std::feof(f));
            } else if (id == I::ferror_) {
                Ret(regs, (std::uint32_t)std::ferror(f));
            } else if (id == I::clearerr_) {
                std::clearerr(f);
                Ret(regs, 0);
            } else {
                // Return the guest fd stored at +14 (matches inline reads).
                std::uint16_t g = 0;
                std::memcpy(&g, H(a0 + 14), 2);
                Ret(regs, static_cast<std::uint32_t>(g ? g : fileno(f)));
            }
            return;
        }
        if (id == I::fgets_ || id == I::fputs_ || id == I::fputc_ || id == I::putc_ || id == I::getc_ ||
            id == I::ungetc_) {
            if (id == I::fgets_) {
                FILE* f = file_of(a2);
                if (!f || a1 == 0 || !CheckRange(a0, a1, true)) {
                    Ret(regs, 0);
                    return;
                }
                // Same overflow firewall as fread (short line buffer).
                const std::uint32_t wlen = ClampHeapWrite(a0, a1, "fgets");
                if (wlen == 0) {
                    Ret(regs, 0);
                    return;
                }
                char* r = std::fgets(reinterpret_cast<char*>(H(a0)), (int)wlen, f);
                Ret(regs, r ? a0 : 0u);
                return;
            }
            // getc(FILE*) has the handle in a0; fputs/fputc/putc/ungetc put it
            // in a1 (C signature: value first, stream last). The old code keyed
            // fputc on a0, so every fputc call was a silent no-op.
            FILE* f = file_of(id == I::getc_ ? a0 : a1);
            if (!f) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (id == I::fputs_) {
                if (!CheckRange(a0, 1, false)) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                Ret(regs, static_cast<std::uint32_t>(std::fputs(GuestStr(a0), f)));
                return;
            }
            if (id == I::fputc_ || id == I::putc_) {
                Ret(regs, static_cast<std::uint32_t>(std::fputc((int)a0, f)));
                return;
            }
            if (id == I::ungetc_) {
                Ret(regs, static_cast<std::uint32_t>(std::ungetc((int)a0, f)));
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(std::fgetc(f)));
            return;
        }
        if (id == I::vfprintf_) {
            // vfprintf(stream, fmt, va_list): the arguments start at the va
            // pointer, as in vprintf.
            FILE* f = file_of(a0);
            if (!f || !CheckRange(a1, 1, false)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::string fmt(GuestStr(a1));
            GuestFmtArgs st{};
            st.via_va = true;
            st.ai = 2;
            st.stack = a2;
            const std::string out = ExpandPrintf(fmt, st);
            Ret(regs, static_cast<std::uint32_t>(std::fwrite(out.data(), 1, out.size(), f)));
            return;
        }
        if (id == I::setvbuf_) {
            // setvbuf(stream, buf, mode, size): the mode reaches the host
            // stream (bionic and the host share _IOFBF 0, _IOLBF 1, _IONBF
            // 2); a guest buffer cannot back a host FILE, so the host picks.
            // stdin/stdout/stderr are the host's own (stdout is apphost.log,
            // unbuffered so a crash loses no line): accepted, left alone.
            FILE* f = file_of(a0);
            if (!f || a2 > 2) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (f == stdin || f == stdout || f == stderr) {
                Ret(regs, 0);
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(
                          std::setvbuf(f, nullptr, static_cast<int>(a2), a3)));
            return;
        }
        if (id == I::fprintf_) {
            // fprintf(stream=a0, fmt=a1, ...): handle is FIRST here.
            FILE* f = file_of(a0);
            if (!f || !CheckRange(a1, 1, false)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::string fmt(GuestStr(a1));  // copy: GuestStr reuses scratch
            GuestFmtArgs st{};
            st.gargs[0] = a0;
            st.gargs[1] = a1;
            st.gargs[2] = a2;
            st.gargs[3] = a3;
            st.ai = 2;  // first vararg core reg (r2)
            st.stack = regs[13];
            const std::string out = ExpandPrintf(fmt, st);
            const std::size_t w = std::fwrite(out.data(), 1, out.size(), f);
            Ret(regs, static_cast<std::uint32_t>(w));
            return;
        }
        if (id == I::fdopen_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                Ret(regs, 0);
                return;
            }
            // A packed file: a read-only FILE over the same open file (its
            // position is the fd's, as fdopen shares the description).
            std::shared_ptr<abvfs::Slice> sl = FdSlice(a0);
            if (sl && std::strpbrk(GuestStr(a1), "wa+") != nullptr) {
                set_guest_errno(EINVAL);  // the fd is open read-only
                Ret(regs, 0);
                return;
            }
            FILE* f = sl ? abvfs::SliceFile(hfd, sl) : ::fdopen(hfd, GuestStr(a1));
            if (!f) {
                Ret(regs, 0);
                return;
            }
            const std::uint32_t h = new_file_handle();
            if (!h) {
                std::fclose(f);
                Ret(regs, 0);
                return;
            }
            if (!bind_file(h, f, a0, hfd, sl)) {
                std::fclose(f);
                discard_file_handle(h);
                Ret(regs, 0);
                return;
            }
            Ret(regs, h);
            return;
        }
        if (id == I::tmpfile_) {
            FILE* f = std::tmpfile();
            if (!f) {
                Ret(regs, 0);
                return;
            }
            const std::uint32_t h = new_file_handle();
            if (!h) {
                std::fclose(f);
                Ret(regs, 0);
                return;
            }
            if (!bind_file(h, f)) {
                std::fclose(f);
                discard_file_handle(h);
                Ret(regs, 0);
                return;
            }
            Ret(regs, h);
            return;
        }
        if (id == I::open_) {
            const std::string hp = map_path(a0);
            if (!need_host(hp, "open")) {
                return;
            }
            if (const abvfs::File* pf = packed_at(hp)) {
                // Guest open flags are the arm32 Linux values, as the host's
                // for these bits (O_ACCMODE, O_CREAT, O_EXCL, O_TRUNC).
                if ((a1 & O_CREAT) && (a1 & O_EXCL)) {
                    SetGuestErrno(EEXIST);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                if ((a1 & O_ACCMODE) == O_RDONLY && !(a1 & O_TRUNC)) {
                    std::shared_ptr<abvfs::Slice> sl;
                    const int hfd = vfs_.Open(*pf, &sl);
                    if (hfd < 0) {
                        SetGuestErrno(errno);
                        Ret(regs, 0xFFFFFFFFu);
                        return;
                    }
                    const std::uint32_t g = alloc_guest_fd(hfd);
                    if (g == 0xFFFFFFFFu) {
                        ::close(hfd);
                        SetGuestErrno(EMFILE);
                        Ret(regs, 0xFFFFFFFFu);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> io(io_mutex_);
                        SetFdSliceLocked(g, std::move(sl));
                    }
                    Ret(regs, g);
                    return;
                }
                if (!materialize(*pf, hp, (a1 & O_TRUNC) != 0)) {
                    SetGuestErrno(errno ? errno : EIO);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
            }
            if ((a1 & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0) {
                ensure_parent(hp);
            }
            int fd = ::open(hp.c_str(), (int)a1, (mode_t)a2);
            if (fd < 0) {
                std::printf("[files] open(\"%s\",%o) -> -1 (errno %d)\n", hp.c_str(), a1, errno);
                SetGuestErrno(errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::uint32_t g = alloc_guest_fd(fd);
            if (std::strcmp(GuestStr(a0), "/dev/alarm") == 0) {
                NoteAlarmFd(g);
                std::printf("[files][t%u] alarm fd %u (clocks served, not null)\n", GuestTid(),
                            g);
            }
            if (DiagOn() || EnvOn("HARNESS_PATHLOG")) {
                std::printf("[files][t%u] open(\"%s\",%o) -> fd %u\n", GuestTid(), hp.c_str(), a1,
                            g);
            }
            Ret(regs, g);
            return;
        }
        if (id == I::mkstemp_) {
            // Breakpad's descriptor writer creates the dump file with
            // mkstemp(dirname(FILES_PATH) + "/hockeyapp_errors_XXXXXX").
            const std::string hp = map_path(a0);
            if (!need_host(hp, "mkstemp")) {
                return;
            }
            ensure_parent(hp);
            std::vector<char> tmpl(hp.begin(), hp.end());
            tmpl.push_back('\0');
            const int fd = ::mkstemp(tmpl.data());
            if (fd < 0) {
                SetGuestErrno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            // The kernel rewrites only the six trailing 'X'. Copy those six
            // characters back to the end of the GUEST template (the host path
            // has a different length, so its offsets do not apply).
            std::uint32_t gl = 0;
            if (hp.size() >= 6 && hp.compare(hp.size() - 6, 6, "XXXXXX") == 0 &&
                GuestStrnlen(a0, 0xFFFFFFFFu, &gl) && gl >= 6 &&
                CheckRange(a0 + gl - 6, 6, true)) {
                std::memcpy(H(a0 + gl - 6), tmpl.data() + hp.size() - 6, 6);
            }
            const std::uint32_t g = alloc_guest_fd(fd);
            if (g == 0xFFFFFFFFu) {
                ::close(fd);
                SetGuestErrno(EMFILE);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (DiagOn()) {
                std::printf("[files][t%u] mkstemp(\"%s\") -> fd %u\n", GuestTid(), tmpl.data(), g);
            }
            Ret(regs, g);
            return;
        }
        if (id == I::ftruncate_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                SetGuestErrno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const int r = ::ftruncate(hfd, static_cast<off_t>(static_cast<std::int32_t>(a1)));
            if (r != 0) {
                SetGuestErrno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::close_ || id == I::fsync_) {
            const int hfd = (id == I::close_) ? erase_guest_fd(a0) : host_fd_of(a0);
            if (id == I::close_) {
                ForgetAlarmFd(a0);
            }
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            // Unlocked I/O: fsync can block; close must not run under the map
            // lock either (a concurrent in-flight read keeps its own copy).
            const int r = (id == I::close_) ? ::close(hfd) : ::fsync(hfd);
            if (r != 0) {
                set_guest_errno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::isatty_ || id == I::flock_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, id == I::isatty_ ? 0u : 0xFFFFFFFFu);
                return;
            }
            int r = 0;
            if (id == I::isatty_) {
                r = ::isatty(hfd);
            } else {
                r = ::flock(hfd, static_cast<int>(a1));
            }
            if ((id == I::isatty_ && r == 0) || (id == I::flock_ && r != 0)) {
                set_guest_errno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::sendfile_) {
            // sendfile(out_fd, in_fd, off_t* offset, size_t count); the guest
            // off_t is 32 bits.
            const int ho = host_fd_of(a0), hi = host_fd_of(a1);
            if (ho < 0 || hi < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (a2 && !CheckRange(a2, 4, true)) {
                goto bad_ptr;
            }
            off_t off = 0;
            if (a2) {
                std::int32_t o32 = 0;
                std::memcpy(&o32, H(a2), 4);
                off = o32;
            }
            ssize_t r = 0;
            if (const std::shared_ptr<abvfs::Slice> sl = FdSlice(a1)) {
                // A packed input: copy through a buffer from the slice
                // (at *offset, which is advanced, or at the fd position).
                std::vector<std::uint8_t> buf(std::min<std::uint32_t>(a3, 1u << 20));
                std::uint64_t at = 0;
                if (a2) {
                    at = static_cast<std::uint64_t>(off);
                } else {
                    std::lock_guard<std::mutex> l(sl->mu);
                    at = sl->pos;
                }
                std::uint64_t done = 0;
                while (done < a3) {
                    const std::size_t want =
                        static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), a3 - done));
                    const ssize_t k = abvfs::SlicePread(*sl, hi, buf.data(), want, at + done);
                    if (k <= 0) {
                        if (k < 0 && done == 0) r = -1;
                        break;
                    }
                    const ssize_t w = ::write(ho, buf.data(), static_cast<std::size_t>(k));
                    if (w <= 0) {
                        if (done == 0) r = -1;
                        break;
                    }
                    done += static_cast<std::uint64_t>(w);
                    if (w < k) break;
                }
                if (r == 0) {
                    r = static_cast<ssize_t>(done);
                    if (a2) {
                        off = static_cast<off_t>(at + done);
                    } else {
                        std::lock_guard<std::mutex> l(sl->mu);
                        sl->pos = at + done;
                    }
                }
            } else {
                r = ::sendfile(ho, hi, a2 ? &off : nullptr, a3);
            }
            if (r < 0) {
                set_guest_errno(errno);
            } else if (a2) {
                const std::int32_t o32 = static_cast<std::int32_t>(off);
                std::memcpy(H(a2), &o32, 4);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::dup2_) {
            // dup2(old, new) onto a guest fd that is open: that fd now names
            // old's file. Guest fds 0-2 are the host's own streams (stdout is
            // apphost.log) and a free number is not ours to hand out: both
            // are refused (EBADF, the "newfd out of range" answer).
            const int src = host_fd_of(a0);
            if (src < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (a0 == a1) {
                Ret(regs, a1);
                return;
            }
            if (a1 <= 2 || host_fd_of(a1) < 0) {
                std::printf("[files] dup2(%u, %u) refused: target is %s\n", a0, a1,
                            a1 <= 2 ? "a host std stream" : "not an open guest fd");
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const int nfd = ::dup(src);
            if (nfd < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            int old_host = -1;
            {
                std::shared_ptr<abvfs::Slice> ss = FdSlice(a0);
                std::lock_guard<std::mutex> io(io_mutex_);
                int& slot = fd_map_[a1];
                old_host = slot;
                slot = nfd;
                SetFdSliceLocked(a1, std::move(ss));  // shares the open file
            }
            ::close(old_host);
            Ret(regs, a1);
            return;
        }
        if (id == I::dup_) {
            // dup(fd): a new guest fd for the same open file. It fell into the
            // generic stub and returned 0, so the guest wrote "its" copy to
            // stdin (Rio 0x4da11a, 0x4da16e). Guest fds 0-2 are the host's.
            const int src = (a0 <= 2) ? static_cast<int>(a0) : host_fd_of(a0);
            if (src < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const int nfd = ::dup(src);
            if (nfd < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const std::uint32_t g = alloc_guest_fd(nfd);
            if (g == 0xFFFFFFFFu) {
                ::close(nfd);
                set_guest_errno(EMFILE);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (std::shared_ptr<abvfs::Slice> ss = FdSlice(a0)) {
                std::lock_guard<std::mutex> io(io_mutex_);
                SetFdSliceLocked(g, std::move(ss));  // shares the open file
            }
            Ret(regs, g);
            return;
        }
        if (id == I::utime_) {
            // utime(path, const struct utimbuf* t): guest utimbuf is
            // {time_t actime; time_t modtime} = 2 x 32 bits; NULL = now.
            const std::string hp = map_path(a0);
            if (!need_host(hp, "utime")) {
                return;
            }
            if (const abvfs::File* pf = packed_at(hp)) {
                if (!vfs_.CopyUp(*pf, hp)) {  // the times are the file's own
                    set_guest_errno(errno ? errno : EIO);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
            }
            struct utimbuf ub {};
            const struct utimbuf* up = nullptr;
            if (a1 != 0) {
                if (!CheckRange(a1, 8, false)) {
                    set_guest_errno(EFAULT);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                std::int32_t t[2] = {0, 0};
                std::memcpy(t, H(a1), 8);
                ub.actime = t[0];
                ub.modtime = t[1];
                up = &ub;
            }
            const int r = ::utime(hp.c_str(), up);
            if (r != 0) {
                set_guest_errno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::pipe_ || id == I::pipe2_) {
            // Fills int[2] with guest fds. The old stub returned 0 WITHOUT
            // writing fds (stack garbage downstream -> throw-nest). Real ones.
            if (!CheckRange(a0, 8, true)) {
                set_guest_errno(EFAULT);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            int hfds[2] = {-1, -1};
            const int r = (id == I::pipe_) ? ::pipe(hfds) : ::pipe2(hfds, (int)a1);
            if (r != 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const std::uint32_t g0 = alloc_guest_fd(hfds[0]);
            const std::uint32_t g1 = (g0 == 0xFFFFFFFFu) ? 0xFFFFFFFFu : alloc_guest_fd(hfds[1]);
            if (g0 == 0xFFFFFFFFu || g1 == 0xFFFFFFFFu) {
                if (g0 != 0xFFFFFFFFu) {
                    erase_guest_fd(g0);
                }
                ::close(hfds[0]);
                ::close(hfds[1]);
                set_guest_errno(EMFILE);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            std::memcpy(H(a0), &g0, 4);
            std::memcpy(H(a0 + 4), &g1, 4);
            if (DiagOn()) {
                std::printf("[files][t%u] pipe() -> [%u %u]\n", GuestTid(), g0, g1);
            }
            Ret(regs, 0);
            return;
        }
        if (id == I::pread_ || id == I::pwrite_) {
            // (fd, buf, count, off_t offset): bionic arm32 off_t is 32-bit
            // and signed (the 64-bit forms are pread64/pwrite64).
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (static_cast<std::int32_t>(a3) < 0) {
                set_guest_errno(EINVAL);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (!CheckRange(a1, a2, id == I::pread_)) {
                set_guest_errno(EFAULT);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const std::shared_ptr<abvfs::Slice> sl = FdSlice(a0);
            const ssize_t r = id == I::pread_
                                  ? (sl ? abvfs::SlicePread(*sl, hfd, H(a1), a2, a3)
                                        : ::pread(hfd, H(a1), a2, static_cast<off_t>(a3)))
                                  : ::pwrite(hfd, H(a1), a2, static_cast<off_t>(a3));
            if (r < 0) {
                set_guest_errno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::read_ || id == I::write_) {
            if (a0 <= 2) {  // stdio: read(0)=EOF, write(1/2)->host log
                if (id == I::read_) {
                    Ret(regs, 0);
                    return;
                }
                if (!CheckRange(a1, a2, false)) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                std::string s(reinterpret_cast<const char*>(H(a1)),
                              std::min<std::uint32_t>(a2, 512));
                std::printf("[guest%s] %s\n", a0 == 1 ? "out" : "err", s.c_str());
                Ret(regs, a2);
                return;
            }
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (!CheckRange(a1, a2, id == I::read_)) {
                set_guest_errno(EFAULT);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            // Unlocked: pipes/sockets block here; the map lock must be free
            // so other threads keep running (2026-10-10 deadlock fix). A read
            // blocks exactly as long as the real one would (a 5 s FIFO cap
            // that returned EAGAIN on a blocking fd is gone).
            const std::shared_ptr<abvfs::Slice> sl = id == I::read_ ? FdSlice(a0) : nullptr;
            ssize_t r = sl ? abvfs::SliceRead(*sl, hfd, H(a1), a2)
                           : (id == I::read_) ? ::read(hfd, H(a1), a2) : ::write(hfd, H(a1), a2);
            // TEMP-DIAG (worker hang): log pipe traffic (fds >= 100 that are
            // pipes show up here via fd_map_; files too, capped). Skip
            // regular files (/proc/cpuinfo floods the cap): pipes+sockets.
            // Diagnostic layer (PLAN.md B7): with DIAG off skip the per-I/O
            // fstat + io_mutex + fd_map probe that only fed this capped log.
            if (DiagOn() && (id == I::write_ || id == I::read_) && r > 0) {
                static int wlog = 0;
                bool interesting = false;
                {
                    std::lock_guard<std::mutex> io(io_mutex_);
                    const auto it = fd_map_.find(a0);
                    if (it != fd_map_.end()) {
                        struct stat fs {};
                        if (::fstat(it->second, &fs) == 0 &&
                            (S_ISFIFO(fs.st_mode) || S_ISSOCK(fs.st_mode))) {
                            interesting = true;
                        }
                    } else if (a0 <= 2) {
                        interesting = true;
                    }
                }
                if (interesting && wlog < 200) {
                    wlog++;
                    const std::uint8_t* p = H<const std::uint8_t>(a1);
                    std::string hex;
                    char b[8];
                    ssize_t show = r > 32 ? 32 : r;
                    for (ssize_t i = 0; i < show; i++) {
                        std::snprintf(b, sizeof(b), "%02x", p[i]);
                        hex += b;
                    }
                    // Thread marker: GuestTid directly (1=main, else worker).
                    std::printf("[pipelog][t%u]%s %s gfd=%u %zdB: %s%s\n", GuestTid(),
                                GuestTid() == 1 ? "" : "(T2+)", n.c_str(), a0, r, hex.c_str(),
                                r > show ? "..." : "");
                }
            }
            if (r < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::lseek_) {
            // lseek(int fd, off_t off, int whence): off_t is a signed 32-bit
            // long on the guest, so the offset must be sign-extended (a
            // negative SEEK_CUR/SEEK_END offset used to become +4 GB).
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                SetGuestErrno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            const std::shared_ptr<abvfs::Slice> sl = FdSlice(a0);
            const off_t r =
                sl ? static_cast<off_t>(abvfs::SliceSeek(*sl, static_cast<std::int32_t>(a1), (int)a2))
                   : ::lseek(hfd, static_cast<off_t>(static_cast<std::int32_t>(a1)), (int)a2);
            if (EnvOn("HARNESS_PATHLOG")) {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 400) {
                    std::printf("[files][t%u] lseek(fd %u, %d, %u) -> %lld\n", GuestTid(), a0,
                                static_cast<std::int32_t>(a1), a2, static_cast<long long>(r));
                }
            }
            if (r < 0) {
                SetGuestErrno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (r > INT32_MAX) {
                SetGuestErrno(EOVERFLOW);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::writev_) {
            // writev(fd, const struct iovec* iov, int iovcnt); guest iovec =
            // {void* base; size_t len} = 2 x 32 bits. One host writev keeps
            // the call's atomicity (pipes/sockets).
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (a2 > IOV_MAX || !CheckRange(a1, (std::size_t)a2 * 8, false)) {
                set_guest_errno(a2 > IOV_MAX ? EINVAL : EFAULT);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            std::vector<struct iovec> iov(a2);
            for (std::uint32_t i = 0; i < a2; i++) {
                std::uint32_t base = 0, len = 0;
                std::memcpy(&base, H(a1 + i * 8), 4);
                std::memcpy(&len, H(a1 + i * 8 + 4), 4);
                if (!CheckRange(base, len, false)) {
                    set_guest_errno(EFAULT);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                iov[i].iov_base = H(base);
                iov[i].iov_len = len;
            }
            const ssize_t w = ::writev(hfd, iov.data(), static_cast<int>(a2));
            if (w < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(w));
            return;
        }
        // --- socket bridge (host-backed; identical path runs on device) ---
        // The game bundles its own DNS-over-UDP resolver (raw socket/
        // connect/send/recvfrom, no getaddrinfo import) for cloud.rovio.com
        // (session/time, sessions, apdrive variantconfig). These used to hit
        // the generic stub ->0 (socket()=0 ALIASED STDIN; connect()=fake
        // success; send/recv()=0 = empty replies -> 4x retry then null-call).
        // Real host sockets: Bionic/Linux errno values + sockaddr layout
        // match, so forward directly, with the blocking/timeout semantics the
        // guest set itself (fcntl, setsockopt). Network destinations are
        // refused unless the title is online (offline_refuses above).
        // sendmsg/recvmsg marshal the 32-bit msghdr (data only, see there).
        if (id == I::socket_) {
            int hfd = ::socket((int)a0, (int)a1, (int)a2);
            if (hfd < 0) {
                set_guest_errno(errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::uint32_t g = alloc_guest_fd(hfd);
            if (DiagOn()) {
                std::printf("[netsock] socket(%u %u %u) -> gfd %u\n", a0, a1, a2, g);
            }
            Ret(regs, g);
            return;
        }
        if (id == I::connect_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (a2 == 0 || a2 > 128 || !CheckRange(a1, a2, false)) {
                set_guest_errno(EFAULT);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (offline_refuses(a1, a2)) {
                set_guest_errno(ENETUNREACH);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const int r = ::connect(hfd, reinterpret_cast<const struct sockaddr*>(H(a1)),
                                    (socklen_t)a2);
            if (r != 0) {
                set_guest_errno(errno);
            }
            if (DiagOn()) {
                std::printf("[netsock]%s connect(gfd %u) -> %d (errno %d)\n",
                            GuestTid() == 1 ? "" : "(T2+)", a0, r, r != 0 ? errno : 0);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::accept_) {
            // accept(fd, sockaddr* addr, socklen_t* len): the new connection
            // becomes a guest fd; the sockaddr layout is the host's.
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            socklen_t len = 0;
            if (a1) {
                if (!CheckRange(a2, 4, true)) {
                    goto bad_ptr;
                }
                std::uint32_t l32 = 0;
                std::memcpy(&l32, H(a2), 4);
                if (l32 > 128 || !CheckRange(a1, l32, true)) {
                    goto bad_ptr;
                }
                len = l32;
            }
            const int nfd = ::accept(hfd, a1 ? reinterpret_cast<struct sockaddr*>(H(a1)) : nullptr,
                                     a1 ? &len : nullptr);
            if (nfd < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (a1) {
                const std::uint32_t l32 = len;
                std::memcpy(H(a2), &l32, 4);
            }
            const std::uint32_t g = alloc_guest_fd(nfd);
            if (g == 0xFFFFFFFFu) {
                ::close(nfd);
                set_guest_errno(EMFILE);
            }
            Ret(regs, g);
            return;
        }
        if (id == I::sendmsg_ || id == I::recvmsg_) {
            // (fd, msghdr*, flags). Guest msghdr (LP32, NDK): name @0,
            // namelen @4, iov @8, iovlen @12, control @16, controllen @20,
            // flags @24; iovec {base, len} = 8 bytes. Ancillary data is not
            // carried: its header differs between arm32 and arm64 (12 vs 16
            // bytes), so sendmsg with control data is refused and recvmsg
            // reports any it got as truncated (MSG_CTRUNC).
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (!CheckRange(a1, 28, id == I::recvmsg_)) {
                goto bad_ptr;
            }
            std::uint32_t m[7];
            std::memcpy(m, H(a1), 28);
            const bool rec = (id == I::recvmsg_);
            if (m[3] > 1024 || (m[3] && !CheckRange(m[2], m[3] * 8, false)) ||
                (m[0] && (m[1] > 128 || !CheckRange(m[0], m[1], rec)))) {
                goto bad_ptr;
            }
            if (!rec && m[5] != 0) {
                std::printf("[netsock] sendmsg with ancillary data is not carried -> EOPNOTSUPP\n");
                set_guest_errno(EOPNOTSUPP);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (!rec && m[0] && offline_refuses(m[0], m[1])) {
                set_guest_errno(ENETUNREACH);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            std::vector<struct iovec> iov(m[3]);
            for (std::uint32_t i = 0; i < m[3]; i++) {
                std::uint32_t v[2];
                std::memcpy(v, H(m[2] + i * 8), 8);
                if (v[1] && !CheckRange(v[0], v[1], rec)) {
                    goto bad_ptr;
                }
                iov[i].iov_base = v[1] ? H(v[0]) : nullptr;
                iov[i].iov_len = v[1];
            }
            struct msghdr hm {};
            hm.msg_name = m[0] ? H(m[0]) : nullptr;
            hm.msg_namelen = m[0] ? m[1] : 0;
            hm.msg_iov = iov.data();
            hm.msg_iovlen = iov.size();
            char control[256];
            if (rec && m[5]) {
                hm.msg_control = control;
                hm.msg_controllen = sizeof(control);
            }
            const ssize_t r = rec ? ::recvmsg(hfd, &hm, static_cast<int>(a2))
                                  : ::sendmsg(hfd, &hm, static_cast<int>(a2));
            if (r < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (rec) {
                std::uint32_t flags = static_cast<std::uint32_t>(hm.msg_flags);
                if (hm.msg_controllen != 0) {
                    flags |= MSG_CTRUNC;  // ancillary data dropped (see above)
                }
                m[1] = m[0] ? static_cast<std::uint32_t>(hm.msg_namelen) : m[1];
                m[5] = 0;
                m[6] = flags;
                std::memcpy(H(a1), m, 28);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::select_) {
            // select(nfds, fd_set* r, fd_set* w, fd_set* e, timeval* t) over
            // guest fd numbers, run as a host poll (host fds may exceed
            // FD_SETSIZE). Guest fd_set = 1024 bits; timeval {long, long},
            // updated with the time left, as Linux does.
            if (a0 > 1024) {
                set_guest_errno(EINVAL);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            std::uint32_t tv_ptr = 0;
            if (!CheckRange(regs[13], 4, false)) {
                goto bad_ptr;
            }
            std::memcpy(&tv_ptr, H(regs[13]), 4);
            const std::uint32_t set_bytes = (a0 + 7) / 8;
            const std::uint32_t sets[3] = {a1, a2, a3};
            for (std::uint32_t k = 0; k < 3; k++) {
                if (sets[k] && set_bytes && !CheckRange(sets[k], set_bytes, true)) {
                    goto bad_ptr;
                }
            }
            if (tv_ptr && !CheckRange(tv_ptr, 8, true)) {
                goto bad_ptr;
            }
            auto has = [&](std::uint32_t set, std::uint32_t fd) {
                return set && (*H<std::uint8_t>(set + fd / 8) >> (fd % 8)) & 1u;
            };
            std::vector<struct pollfd> pfds;
            std::vector<std::uint32_t> gfds;
            for (std::uint32_t fd = 0; fd < a0; fd++) {
                short ev = 0;
                if (has(a1, fd)) ev |= POLLIN;
                if (has(a2, fd)) ev |= POLLOUT;
                if (has(a3, fd)) ev |= POLLPRI;
                if (!ev) {
                    continue;
                }
                const int hfd = fd <= 2 ? static_cast<int>(fd) : host_fd_of(fd);
                if (hfd < 0) {
                    set_guest_errno(EBADF);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                pfds.push_back({hfd, ev, 0});
                gfds.push_back(fd);
            }
            int timeout = -1;
            std::int32_t tv[2] = {0, 0};
            if (tv_ptr) {
                std::memcpy(tv, H(tv_ptr), 8);
                if (tv[0] < 0 || tv[1] < 0) {
                    set_guest_errno(EINVAL);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                const long long ms = static_cast<long long>(tv[0]) * 1000 + (tv[1] + 999) / 1000;
                timeout = ms > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(ms);
            }
            const auto t0 = std::chrono::steady_clock::now();
            const int r = ::poll(pfds.data(), pfds.size(), timeout);
            if (r < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            for (std::uint32_t k = 0; k < 3; k++) {
                if (sets[k] && set_bytes) {
                    std::memset(H(sets[k]), 0, set_bytes);
                }
            }
            std::uint32_t count = 0;
            for (std::size_t i = 0; i < pfds.size(); i++) {
                const std::uint32_t fd = gfds[i];
                const short re = pfds[i].revents;
                auto mark = [&](std::uint32_t set) {
                    *H<std::uint8_t>(set + fd / 8) |= static_cast<std::uint8_t>(1u << (fd % 8));
                    count++;
                };
                if (a1 && (pfds[i].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))) {
                    mark(a1);
                }
                if (a2 && (pfds[i].events & POLLOUT) && (re & (POLLOUT | POLLERR))) {
                    mark(a2);
                }
                if (a3 && (pfds[i].events & POLLPRI) && (re & POLLPRI)) {
                    mark(a3);
                }
            }
            if (tv_ptr) {
                const long long spent = std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count();
                long long left = static_cast<long long>(tv[0]) * 1000000 + tv[1] - spent;
                if (left < 0 || r == 0) {
                    left = 0;
                }
                const std::int32_t o[2] = {static_cast<std::int32_t>(left / 1000000),
                                           static_cast<std::int32_t>(left % 1000000)};
                std::memcpy(H(tv_ptr), o, 8);
            }
            Ret(regs, count);
            return;
        }
        if (id == I::epoll_create_ || id == I::epoll_ctl_ || id == I::epoll_wait_) {
            // Guest struct epoll_event (arm EABI, not packed): events @0,
            // 64-bit data @8; 16 bytes. The data word is the guest's, kept
            // as given.
            if (id == I::epoll_create_) {
                if (static_cast<std::int32_t>(a0) <= 0) {
                    set_guest_errno(EINVAL);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                const int efd = ::epoll_create1(0);
                if (efd < 0) {
                    set_guest_errno(errno);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                const std::uint32_t g = alloc_guest_fd(efd);
                if (g == 0xFFFFFFFFu) {
                    ::close(efd);
                    set_guest_errno(EMFILE);
                }
                Ret(regs, g);
                return;
            }
            const int efd = host_fd_of(a0);
            if (efd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (id == I::epoll_ctl_) {
                // (epfd, op, fd, event*)
                const int hfd = a2 <= 2 ? static_cast<int>(a2) : host_fd_of(a2);
                if (hfd < 0) {
                    set_guest_errno(EBADF);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                struct epoll_event ev {};
                if (a1 != EPOLL_CTL_DEL) {
                    if (!CheckRange(a3, 16, false)) {
                        goto bad_ptr;
                    }
                    std::uint32_t e = 0;
                    std::uint64_t d = 0;
                    std::memcpy(&e, H(a3), 4);
                    std::memcpy(&d, H(a3 + 8), 8);
                    ev.events = e;
                    ev.data.u64 = d;
                }
                const int r = ::epoll_ctl(efd, static_cast<int>(a1), hfd, &ev);
                if (r != 0) {
                    set_guest_errno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            // epoll_wait(epfd, events*, maxevents, timeout)
            const std::int32_t maxev = static_cast<std::int32_t>(a2);
            if (maxev <= 0 || maxev > 4096) {
                set_guest_errno(EINVAL);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            if (!CheckRange(a1, static_cast<std::uint32_t>(maxev) * 16u, true)) {
                goto bad_ptr;
            }
            std::vector<struct epoll_event> evs(static_cast<std::size_t>(maxev));
            const int r = ::epoll_wait(efd, evs.data(), maxev, static_cast<int>(a3));
            if (r < 0) {
                set_guest_errno(errno);
                Ret(regs, 0xFFFFFFFFu);
                return;
            }
            for (int i = 0; i < r; i++) {
                const std::uint32_t at = a1 + static_cast<std::uint32_t>(i) * 16u;
                const std::uint32_t e = evs[static_cast<std::size_t>(i)].events;
                const std::uint64_t d = evs[static_cast<std::size_t>(i)].data.u64;
                std::memset(H(at), 0, 16);
                std::memcpy(H(at), &e, 4);
                std::memcpy(H(at + 8), &d, 8);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::shutdown_ || id == I::bind_ || id == I::listen_ || id == I::setsockopt_ ||
            id == I::getsockopt_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            int r = -1;
            if (id == I::shutdown_) {
                r = ::shutdown(hfd, (int)a1);
            } else if (id == I::bind_ || id == I::listen_) {
                if (id == I::bind_) {
                    if (a2 == 0 || a2 > 128 || !CheckRange(a1, a2, false)) {
                        set_guest_errno(EFAULT);
                        Ret(regs, static_cast<std::uint32_t>(-1));
                        return;
                    }
                    r = ::bind(hfd, reinterpret_cast<const struct sockaddr*>(H(a1)),
                               (socklen_t)a2);
                } else {
                    r = ::listen(hfd, (int)a1);  // a1 = backlog, no pointer
                }
            } else {
                // setsockopt(fd, level, name, const void* val, socklen_t len):
                //   the 5th argument (len) is passed BY VALUE at [sp].
                // getsockopt(fd, level, name, void* val, socklen_t* len):
                //   the 5th argument is a POINTER at [sp].
                // SO_RCVTIMEO/SO_SNDTIMEO carry struct timeval, 8 bytes on the
                // 32-bit guest ({long,long}) vs 16 on the host: converted.
                // Every other option value is ints/structs of ints, the same
                // on both sides (asm-generic/socket.h is shared by arm/arm64).
                std::uint32_t arg5 = 0;
                if (!CheckRange(regs[13], 4, false)) {
                    set_guest_errno(EFAULT);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                std::memcpy(&arg5, H(regs[13]), 4);
                const bool is_set = (id == I::setsockopt_);
                const bool tv_opt = ((int)a1 == SOL_SOCKET &&
                                     ((int)a2 == SO_RCVTIMEO || (int)a2 == SO_SNDTIMEO));
                std::uint32_t len = 0;
                if (is_set) {
                    len = arg5;
                } else {
                    if (arg5 == 0 || !CheckRange(arg5, 4, true)) {
                        set_guest_errno(EFAULT);
                        Ret(regs, 0xFFFFFFFFu);
                        return;
                    }
                    std::memcpy(&len, H(arg5), 4);
                }
                if (len > 256 || (len != 0 && !CheckRange(a3, len, !is_set))) {
                    set_guest_errno(len > 256 ? EINVAL : EFAULT);
                    Ret(regs, 0xFFFFFFFFu);
                    return;
                }
                if (tv_opt) {
                    if (len < 8) {
                        set_guest_errno(EINVAL);
                        Ret(regs, 0xFFFFFFFFu);
                        return;
                    }
                    struct timeval tv {};
                    if (is_set) {
                        std::int32_t g[2] = {0, 0};
                        std::memcpy(g, H(a3), 8);
                        tv.tv_sec = g[0];
                        tv.tv_usec = g[1];
                        r = ::setsockopt(hfd, (int)a1, (int)a2, &tv, sizeof(tv));
                    } else {
                        socklen_t sl = sizeof(tv);
                        r = ::getsockopt(hfd, (int)a1, (int)a2, &tv, &sl);
                        if (r == 0) {
                            const std::int32_t g[2] = {static_cast<std::int32_t>(tv.tv_sec),
                                                       static_cast<std::int32_t>(tv.tv_usec)};
                            std::memcpy(H(a3), g, 8);
                            const std::uint32_t back = 8;
                            std::memcpy(H(arg5), &back, 4);
                        }
                    }
                } else if (is_set) {
                    r = ::setsockopt(hfd, (int)a1, (int)a2, len ? H(a3) : nullptr, (socklen_t)len);
                } else {
                    socklen_t sl = (socklen_t)len;
                    r = ::getsockopt(hfd, (int)a1, (int)a2, len ? H(a3) : nullptr, &sl);
                    if (r == 0) {
                        const std::uint32_t back = (std::uint32_t)sl;
                        std::memcpy(H(arg5), &back, 4);
                    }
                }
            }
            if (r != 0) {
                set_guest_errno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::send_ || id == I::sendto_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            // sendto: addr=a4, addrlen=a5 live on guest stack (ARM APCS).
            std::uint32_t addr = 0, addrlen = 0;
            if (id == I::sendto_) {
                if (!CheckRange(regs[13] + 4, 4, false)) {
                    set_guest_errno(EFAULT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                std::memcpy(&addr, H(regs[13]), 4);
                std::memcpy(&addrlen, H(regs[13] + 4), 4);
                if ((addr == 0) != (addrlen == 0) || addrlen > 128 ||
                    (addr != 0 && !CheckRange(addr, addrlen, false))) {
                    set_guest_errno(EFAULT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                if (addr != 0 && offline_refuses(addr, addrlen)) {
                    set_guest_errno(ENETUNREACH);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
            }
            if (!CheckRange(a1, a2, false)) {
                set_guest_errno(EFAULT);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            ssize_t r =
                (id == I::send_)
                    ? ::send(hfd, H(a1), a2, (int)a3)
                    : ::sendto(hfd, H(a1), a2, (int)a3,
                               addr ? reinterpret_cast<const struct sockaddr*>(H(addr)) : nullptr,
                               (socklen_t)addrlen);
            if (r < 0) {
                set_guest_errno(errno);
                if (DiagOn()) {
                    std::printf("[netsock]%s %s(gfd %u %uB) -> -1 errno %d\n",
                                GuestTid() == 1 ? "" : "(T2+)", n.c_str(), a0, a2, errno);
                }
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (id == I::recv_ || id == I::recvfrom_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            std::uint32_t addr = 0, addrlenptr = 0, addrlen = 0;
            if (id == I::recvfrom_) {
                if (!CheckRange(regs[13] + 4, 4, false)) {
                    set_guest_errno(EFAULT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                std::memcpy(&addr, H(regs[13]), 4);
                std::memcpy(&addrlenptr, H(regs[13] + 4), 4);
                if (addrlenptr && CheckRange(addrlenptr, 4, false)) {
                    std::memcpy(&addrlen, H(addrlenptr), 4);
                }
                if ((addr == 0) != (addrlenptr == 0) || addrlen > 128 ||
                    (addr != 0 && !CheckRange(addr, addrlen, true))) {
                    set_guest_errno(EFAULT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
            }
            if (!CheckRange(a1, a2, true)) {
                set_guest_errno(EFAULT);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            struct sockaddr_storage ss {};
            socklen_t sl = (socklen_t)(addrlen != 0 ? addrlen : sizeof(ss));
            ssize_t r =
                (id == I::recv_)
                    ? ::recv(hfd, H(a1), a2, (int)a3)
                    : ::recvfrom(hfd, H(a1), a2, (int)a3,
                                 addr ? reinterpret_cast<struct sockaddr*>(&ss) : nullptr,
                                 addr ? &sl : nullptr);
            if (r < 0) {
                set_guest_errno(errno);
                if (EnvOn("HARNESS_POLLDBG")) {
                    std::printf("[recv-dbg] %s(gfd %u) r=-1 errno=%d\n", n.c_str(), a0, errno);
                }
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (r == 0 && EnvOn("HARNESS_POLLDBG")) {
                std::printf("[recv-dbg] %s(gfd %u) r=0 (EOF)\n", n.c_str(), a0);
            }
            // TEMP-DIAG (network bring-up): inbound bytes are the signal
            // (DNS replies, HTTP bodies). Capped hex.
            {
                static int nlog = 0;
                if (DiagOn() && nlog < 40 && r > 0) {
                    nlog++;
                    const std::uint8_t* p = H<const std::uint8_t>(a1);
                    std::string hex;
                    char b[8];
                    ssize_t show = r > 32 ? 32 : r;
                    for (ssize_t i = 0; i < show; i++) {
                        std::snprintf(b, sizeof(b), "%02x", p[i]);
                        hex += b;
                    }
                    std::printf("[netsock]%s %s(gfd %u) %zdB: %s%s\n",
                                GuestTid() == 1 ? "" : "(T2+)", n.c_str(), a0, r, hex.c_str(),
                                r > show ? "..." : "");
                }
            }
            if (addr && (std::uint32_t)sl <= addrlen) {
                std::memcpy(H(addr), &ss, sl);
                if (CheckRange(addrlenptr, 4, true)) {
                    const std::uint32_t back = (std::uint32_t)sl;
                    std::memcpy(H(addrlenptr), &back, 4);
                }
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        // getpeername/getsockname: the engine stringifies socket addrs
        // (sa_addr inet_ntop) and logs "Trying %s...". Stubs left the
        // buffers zeroed (family 0) -> ntop failed -> HttpRequestException.
        if (id == I::getpeername_ || id == I::getsockname_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            std::uint32_t len = 0;
            if (a2 == 0 || !CheckRange(a2, 4, false)) {
                set_guest_errno(EFAULT);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            std::memcpy(&len, H(a2), 4);
            if (len > 128 || (len != 0 && !CheckRange(a1, len, true))) {
                set_guest_errno(EFAULT);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            struct sockaddr_storage ss {};
            socklen_t sl = (socklen_t)(len != 0 ? len : sizeof(ss));
            int r = (id == I::getpeername_ ? ::getpeername(hfd, reinterpret_cast<struct sockaddr*>(&ss),
                                                       &sl)
                                        : ::getsockname(hfd, reinterpret_cast<struct sockaddr*>(&ss),
                                                        &sl));
            if (r != 0) {
                set_guest_errno(errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if ((std::uint32_t)sl <= len) {
                std::memcpy(H(a1), &ss, sl);
                const std::uint32_t back = (std::uint32_t)sl;
                if (CheckRange(a2, 4, true)) {
                    std::memcpy(H(a2), &back, 4);
                }
            }
            Ret(regs, 0);
            return;
        }
        // fcntl: the TLS state machine needs REAL O_NONBLOCK (fake-success
        // stub kept fds blocking -> handshake breakage -> HttpRequestException).
        if (id == I::fcntl_) {
            // TEMP-DIAG (stuck workers): pinpoint the parking line.
            static int fcl = 0;
            const int fldiag = (DiagOn() && fcl < 40) ? fcl++ : -1;
            if (fldiag >= 0) {
                std::printf("[fcntl] #%d enter tid=%u fd=%d cmd=%d arg=%d\n", fldiag, GuestTid(),
                            (int)a0, (int)a1, (int)a2);
            }
            const int hfd = host_fd_of(a0);
            if (fldiag >= 0) {
                std::printf("[fcntl] #%d mapped tid=%u hfd=%d\n", fldiag, GuestTid(), hfd);
            }
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const int cmd = (int)a1;
            if (cmd == F_GETFL || cmd == F_GETFD) {
                int r = ::fcntl(hfd, cmd);
                if (r < 0) {
                    set_guest_errno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (cmd == F_SETFL || cmd == F_SETFD) {
                int r = ::fcntl(hfd, cmd, (int)a2);
                if (r < 0) {
                    set_guest_errno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
                int r = ::fcntl(hfd, cmd, (int)a2);
                if (r < 0) {
                    set_guest_errno(errno);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                const std::uint32_t g = alloc_guest_fd(r);
                if (g == 0xFFFFFFFFu) {
                    ::close(r);
                    set_guest_errno(EMFILE);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                if (std::shared_ptr<abvfs::Slice> ss = FdSlice(a0)) {
                    std::lock_guard<std::mutex> io(io_mutex_);
                    SetFdSliceLocked(g, std::move(ss));  // shares the open file
                }
                Ret(regs, g);
                return;
            }
            // File locks etc.: success-lie (single-process bring-up; revisit
            // if a game path depends on lock contention).
            Ret(regs, 0);
            return;
        }
        if (id == I::ioctl_ && IsAlarmFd(a0)) {
            // Android alarm driver: the game's TICK source
            // (tick-fn opens /dev/alarm, GET_TIME per init; observed live:
            // cmd 0x40086134 = GET_TIME(ELAPSED_REALTIME)). Served from
            // host clocks; previously stub->0 with unwritten buffer =
            // frozen tick = dt 0. Non-alarm ioctls fall through to the
            // legacy stub below (behavior unchanged).
            const std::uint32_t cmd = a1;
            if (((cmd >> 8) & 0xFFu) == (std::uint32_t)'a') {
                const unsigned op = cmd & 0xFu;
                const unsigned type = (cmd >> 4) & 0xFu;
                if (op == 4) {  // ANDROID_ALARM_GET_TIME(type)
                    clockid_t cid = CLOCK_BOOTTIME;
                    if (type <= 1) {
                        cid = CLOCK_REALTIME;  // RTC_WAKEUP / RTC
                    } else if (type == 4) {
                        cid = CLOCK_MONOTONIC;  // SYSTEMTIME
                    }  // else ELAPSED_REALTIME* -> BOOTTIME
                    struct timespec ts {};
                    if (::clock_gettime(cid, &ts) == 0 && CheckRange(a2, 8, true)) {
                        const std::uint32_t s = static_cast<std::uint32_t>(ts.tv_sec);
                        const std::uint32_t ns = static_cast<std::uint32_t>(ts.tv_nsec);
                        std::memcpy(H(a2), &s, 4);
                        std::memcpy(H(a2 + 4), &ns, 4);
                        static int alog = 0;
                        if (DiagOn() && alog < 3) {
                            alog++;
                            std::printf("[alarm] GET_TIME type=%u -> %u.%09u\n", type, s,
                                        ns);
                        }
                        Ret(regs, 0);
                        return;
                    }
                    set_guest_errno(EFAULT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                if (op == 0 || op == 2) {  // CLEAR / SET
                    Ret(regs, 0);
                    return;
                }
                if (op == 3) {  // SET_AND_WAIT: sleep till target (honest, item 8)
                    std::uint32_t gsec = 0, gnsec = 0;
                    if (CheckRange(a2, 8, false)) {
                        std::memcpy(&gsec, H(a2), 4);
                        std::memcpy(&gnsec, H(a2 + 4), 4);
                    }
                    clockid_t cid = (type <= 1) ? CLOCK_REALTIME : CLOCK_BOOTTIME;
                    struct timespec now {};
                    ::clock_gettime(cid, &now);
                    const std::int64_t want =
                        std::int64_t(gsec) * 1000000000 + std::int64_t(gnsec);
                    const std::int64_t have =
                        std::int64_t(now.tv_sec) * 1000000000 + std::int64_t(now.tv_nsec);
                    std::uint64_t us = 0;
                    if (want > have) {
                        us = (std::uint64_t)(want - have) / 1000u;
                    }
                    GuestSleepUs(us);
                    Ret(regs, 0);
                    return;
                }
            }
        }
        if (id == I::inet_addr_ || id == I::inet_aton_ || id == I::inet_pton_ || id == I::inet_ntoa_ ||
            id == I::inet_ntop_) {
            InetThunk(id, regs, n, a0, a1, a2, a3);
            return;
        }
        if (id == I::stat_ || id == I::access_ || id == I::mkdir_ || id == I::unlink_ || id == I::remove_ ||
            id == I::rename_ || id == I::rmdir_ || id == I::chmod_ || id == I::getcwd_ ||
            id == I::pathconf_ || id == I::statfs_ || id == I::lstat_ || id == I::truncate_ ||
            id == I::chdir_) {
            if (id == I::chdir_) {
                // The new directory in guest spelling (relative to the
                // current one, normalized); it must be a directory the
                // bridge can reach.
                if (!CheckRange(a0, 1, false)) {
                    goto bad_ptr;
                }
                std::string g = GuestStr(a0);
                if (g.empty()) {
                    SetGuestErrno(ENOENT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                if (g[0] != '/') {
                    g = GuestCwd() + "/" + g;
                }
                std::string norm = std::filesystem::path(g).lexically_normal().string();
                while (norm.size() > 1 && norm.back() == '/') {
                    norm.pop_back();
                }
                const std::string hp = MapGuestPathString(norm);
                struct stat st {};
                if (hp.empty() || ::stat(hp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                    SetGuestErrno(hp.empty() ? ENOENT : (errno ? errno : ENOTDIR));
                    if (!hp.empty() && S_ISREG(st.st_mode)) {
                        SetGuestErrno(ENOTDIR);
                    }
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                {
                    std::lock_guard<std::mutex> l(cwd_mu_);
                    cwd_ = (norm == files_prefix_) ? std::string() : norm;
                }
                Ret(regs, 0);
                return;
            }
            if (id == I::getcwd_) {
                // The guest's view of the CWD: its files dir, which is what
                // bare relative names resolve to (MapGuestPath), in GUEST
                // spelling. The host path leaked into guest paths and was
                // mapped again on the next call (on the PC the two differ;
                // on a device they are the same string).
                const std::string cwd = GuestCwd();
                const std::uint32_t need = static_cast<std::uint32_t>(cwd.size() + 1);
                // getcwd(buf, size): ERANGE when it does not fit (callers grow
                // the buffer and retry; writing past it smashed the heap).
                // bionic: buf == NULL allocates max(size, need) bytes.
                std::uint32_t dst = a0;
                if (dst == 0) {
                    dst = GuestMalloc(a1 > need ? a1 : need);
                    if (!dst) {
                        SetGuestErrno(ENOMEM);
                        Ret(regs, 0);
                        return;
                    }
                } else if (a1 == 0) {
                    SetGuestErrno(EINVAL);
                    Ret(regs, 0);
                    return;
                } else if (a1 < need) {
                    SetGuestErrno(ERANGE);
                    Ret(regs, 0);
                    return;
                } else if (!CheckRange(dst, need, true)) {
                    goto bad_ptr;
                }
                std::memcpy(H(dst), cwd.c_str(), need);
                Ret(regs, dst);
                return;
            }
            if (id == I::pathconf_) {
                // Faithful host query. This MUST NOT be a 0 stub: the engine
                // sizes its path/dirent buffers from _PC_PATH_MAX (=4) and
                // would otherwise overflow a 24-byte heap block on the second
                // run (once cloud_fs/ already exists and the scan proceeds).
                const std::string hp = map_path(a0);
                if (!need_host(hp, "pathconf")) {
                    return;
                }
                errno = 0;
                const long r = ::pathconf(hp.c_str(), static_cast<int>(a1));
                if (r < 0) {
                    SetGuestErrno(errno);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            const std::string hp = map_path(a0);
            if (EnvOn("HARNESS_PATHLOG")) {
                std::printf("[files] %s(\"%s\") -> host \"%s\"\n", n.c_str(),
                            CheckRange(a0, 1, false) ? GuestStr(a0) : "?", hp.c_str());
            }
            if (!need_host(hp, n.c_str())) {
                return;
            }
            // Packed files (apk_vfs.h): a path with only a packed file.
            if (const abvfs::File* pf = packed_at(hp)) {
                auto fail = [&](int e) {
                    SetGuestErrno(e);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                };
                if (id == I::stat_ || id == I::lstat_) {
                    struct stat st {};
                    vfs_.Stat(*pf, &st);
                    if (!fill_stat(a1, st)) {
                        fail(EFAULT);
                        return;
                    }
                    Ret(regs, 0);
                    return;
                }
                if (id == I::access_) {
                    // The app's own file: readable and writable (by copy-up).
                    if (a1 & X_OK) {
                        fail(EACCES);
                        return;
                    }
                    Ret(regs, 0);
                    return;
                }
                if (id == I::unlink_ || id == I::remove_) {
                    vfs_.MarkDeleted(hp);
                    Ret(regs, 0);
                    return;
                }
                if (id == I::rename_) {
                    const std::string hp2 = map_path(a1);
                    if (!need_host(hp2, "rename-dst")) {
                        return;
                    }
                    if (hp2 == hp) {
                        Ret(regs, 0);
                        return;
                    }
                    ensure_parent(hp2);
                    if (!vfs_.CopyUp(*pf, hp2)) {
                        fail(errno ? errno : EIO);
                        return;
                    }
                    vfs_.MarkDeleted(hp);
                    Ret(regs, 0);
                    return;
                }
                if (id == I::mkdir_) {
                    fail(EEXIST);
                    return;
                }
                if (id == I::rmdir_) {
                    fail(ENOTDIR);
                    return;
                }
                if (id == I::chmod_) {
                    // The mode of the app's private file is not kept (stat
                    // reports 0600 either way); copying a packed archive for
                    // it would be the only effect.
                    Ret(regs, 0);
                    return;
                }
                if (id == I::truncate_) {
                    if (!materialize(*pf, hp, static_cast<std::int32_t>(a1) == 0)) {
                        fail(errno ? errno : EIO);
                        return;
                    }
                    // the real truncate below
                } else if (id == I::statfs_) {
                    const std::size_t sl = hp.rfind('/');
                    struct statfs st {};
                    if (::statfs(hp.substr(0, sl == std::string::npos ? 0 : sl).c_str(), &st) != 0 ||
                        !fill_statfs(a1, st)) {
                        fail(errno);
                        return;
                    }
                    Ret(regs, 0);
                    return;
                }
            } else if (vfs_on_) {
                // A real path that also has packed content under or at it.
                if (id == I::rmdir_ && !vfs_.Children(hp).empty()) {
                    SetGuestErrno(ENOTEMPTY);  // packed entries are still in it
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                if (id == I::rename_ && !vfs_.Children(hp).empty()) {
                    // Packed entries cannot move with the directory: the
                    // cross-device answer, after which callers copy.
                    SetGuestErrno(EXDEV);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
            }
            if (id == I::truncate_) {
                const int r = ::truncate(hp.c_str(),
                                         static_cast<off_t>(static_cast<std::int32_t>(a1)));
                if (r != 0) {
                    SetGuestErrno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (id == I::stat_ || id == I::lstat_) {
                struct stat st {};
                const int sr = id == I::stat_ ? ::stat(hp.c_str(), &st) : ::lstat(hp.c_str(), &st);
                if (sr != 0 || !fill_stat(a1, st)) {
                    SetGuestErrno(errno);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                Ret(regs, 0);
                return;
            }
            if (id == I::statfs_) {
                // statfs(path, buf): the guest asks how much room its data
                // directory has.
                struct statfs st {};
                if (::statfs(hp.c_str(), &st) != 0 || !fill_statfs(a1, st)) {
                    SetGuestErrno(errno);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                Ret(regs, 0);
                return;
            }
            if (id == I::access_) {
                const int r = ::access(hp.c_str(), (int)a1);
                if (r != 0) {
                    SetGuestErrno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (id == I::mkdir_) {
                // POSIX mkdir: one level, EEXIST if present, ENOENT if the
                // parent is missing, honours the mode. (create_directories
                // made it "mkdir -p" and left a stale errno on EEXIST.)
                const int r = ::mkdir(hp.c_str(), static_cast<mode_t>(a1));
                if (r != 0) {
                    SetGuestErrno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (id == I::unlink_ || id == I::remove_) {
                const int r = ::unlink(hp.c_str());
                if (r != 0) {
                    SetGuestErrno(errno);
                } else if (vfs_on_) {
                    vfs_.MarkDeleted(hp);  // a packed file it hid stays gone
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (id == I::rename_) {
                const std::string hp2 = map_path(a1);
                if (!need_host(hp2, "rename-dst")) {
                    return;
                }
                ensure_parent(hp2);
                const int r = ::rename(hp.c_str(), hp2.c_str());
                if (r != 0) {
                    SetGuestErrno(errno);
                } else if (vfs_on_) {
                    vfs_.MarkDeleted(hp);  // a packed file it hid stays gone
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            if (id == I::rmdir_) {
                const int r = ::rmdir(hp.c_str());
                if (r != 0) {
                    SetGuestErrno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
                return;
            }
            // chmod
            {
                const int r = ::chmod(hp.c_str(), (mode_t)a1);
                if (r != 0) {
                    SetGuestErrno(errno);
                }
                Ret(regs, static_cast<std::uint32_t>(r));
            }
            return;
        }
        if (id == I::fstat_) {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                SetGuestErrno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            struct stat st {};
            const std::shared_ptr<abvfs::Slice> sl = FdSlice(a0);
            if (sl) {
                abvfs::SliceStat(*sl, &st);
            }
            if ((!sl && ::fstat(hfd, &st) != 0) || !fill_stat(a1, st)) {
                SetGuestErrno(errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            Ret(regs, 0);
            return;
        }
        if (id == I::opendir_) {
            const std::string hp = map_path(a0);
            if (!need_host(hp, "opendir")) {
                return;
            }
            // TEMP-DIAG (worker dir scans): which trees does it list?
            {
                static int odlog = 0;
                if (odlog < 15 && DiagOn()) {
                    odlog++;
                    std::printf("[files]%s opendir(\"%s\")%s\n", GuestTid() == 1 ? "" : "(T2+)",
                                hp.c_str(), odlog == 15 ? " (quiet henceforth)" : "");
                }
            }
            DIR* d = ::opendir(hp.c_str());
            if (!d) {
                SetGuestErrno(errno);
                Ret(regs, 0);
                return;
            }
            const std::uint32_t h = GuestMalloc(16);
            if (!h) {
                ::closedir(d);
                Ret(regs, 0);
                return;
            }
            // Packed entries this directory lists after its real ones: those
            // with no real entry of the same name (ignoring case).
            std::vector<std::pair<std::string, bool>> packed;
            if (vfs_on_) {
                packed = vfs_.Children(hp);
                if (!packed.empty()) {
                    std::unordered_set<std::string> real;
                    if (DIR* scan = ::opendir(hp.c_str())) {
                        while (const dirent* e = ::readdir(scan)) {
                            std::string lc = e->d_name;
                            for (char& c : lc) {
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            }
                            real.insert(lc);
                        }
                        ::closedir(scan);
                    }
                    packed.erase(std::remove_if(packed.begin(), packed.end(),
                                                [&](const std::pair<std::string, bool>& c) {
                                                    std::string lc = c.first;
                                                    for (char& ch : lc) {
                                                        ch = static_cast<char>(std::tolower(
                                                            static_cast<unsigned char>(ch)));
                                                    }
                                                    return real.count(lc) != 0;
                                                }),
                                 packed.end());
                }
            }
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                HostDir hd;
                hd.d = d;
                hd.path = hp;
                hd.packed = std::move(packed);
                dir_map_[h] = std::move(hd);
            }
            Ret(regs, h);
            return;
        }
        // The next packed entry of guest DIR a0 once its real entries are
        // done (apk_vfs.h); false when there is none left.
        auto next_packed = [&](std::uint32_t dirh, struct dirent* out) -> bool {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = dir_map_.find(dirh);
            if (it == dir_map_.end() || it->second.packed_next >= it->second.packed.size()) {
                return false;
            }
            const std::size_t i = it->second.packed_next++;
            const auto& c = it->second.packed[i];
            std::memset(out, 0, sizeof *out);
            out->d_ino = static_cast<ino_t>(std::hash<std::string>{}(it->second.path + "/" + c.first) |
                                            (1ull << 62)) &
                         ~(1ull << 63);
            out->d_off = static_cast<off_t>(0x40000000 + i);
            out->d_type = c.second ? DT_DIR : DT_REG;
            std::snprintf(out->d_name, sizeof out->d_name, "%s", c.first.c_str());
            return true;
        };
        if (id == I::readdir_r_) {
            // readdir_r(DIR* a0, struct dirent* a1, struct dirent** a2) -> 0 or
            // an error number. Bionic arm32 struct dirent (NDK dirent.h,
            // checked with offsetof): u64 d_ino@0, s64 d_off@8, u16 d_reclen@16,
            // u8 d_type@18, char d_name[256]@19; sizeof 280. Like bionic's
            // readdir_r, copy exactly d_reclen bytes: the header, the name and
            // its NUL, padded to 8. Guest callers size the buffer from
            // pathconf(_PC_NAME_MAX) + 20 (Rio 0x2c65f4), so a full 280-byte
            // write would overrun them.
            DIR* dd = nullptr;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto it = dir_map_.find(a0);
                if (it == dir_map_.end()) {
                    Ret(regs, static_cast<std::uint32_t>(EBADF));
                    return;
                }
                dd = it->second.d;
            }
            if (!CheckRange(a2, 4, true)) {
                goto bad_ptr;
            }
            errno = 0;
            const struct dirent* de = ::readdir(dd);
            struct dirent packed_de {};
            if (!de && errno == 0 && next_packed(a0, &packed_de)) {
                de = &packed_de;
            }
            if (!de) {
                const std::uint32_t z = 0;
                std::memcpy(H(a2), &z, 4);
                Ret(regs, static_cast<std::uint32_t>(errno));
                return;
            }
            const std::size_t nm_len = ::strnlen(de->d_name, 255);
            const std::uint32_t reclen =
                static_cast<std::uint32_t>((kGuestDirentNameOff + nm_len + 1 + 7) & ~std::size_t(7));
            if (!CheckRange(a1, reclen, true)) {
                goto bad_ptr;
            }
            std::uint8_t* p = H<std::uint8_t>(a1);
            std::memset(p, 0, reclen);
            const std::uint64_t ino = de->d_ino;
            const std::int64_t off = de->d_off;
            const std::uint16_t rl = static_cast<std::uint16_t>(reclen);
            std::memcpy(p + 0, &ino, 8);
            std::memcpy(p + 8, &off, 8);
            std::memcpy(p + 16, &rl, 2);
            p[18] = de->d_type;
            std::memcpy(p + kGuestDirentNameOff, de->d_name, nm_len);
            std::memcpy(H(a2), &a1, 4);
            Ret(regs, 0);
            return;
        }
        if (id == I::readdir_) {
            // readdir(DIR*) -> struct dirent*: the entry lives in a buffer
            // that belongs to the DIR (as in bionic), overwritten by the
            // next call; same layout as readdir_r above. NULL at the end
            // (errno untouched) or on error (errno set).
            DIR* dd = nullptr;
            std::uint32_t ent = 0;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto it = dir_map_.find(a0);
                if (it == dir_map_.end()) {
                    set_guest_errno(EBADF);
                    Ret(regs, 0);
                    return;
                }
                dd = it->second.d;
                ent = it->second.ent;
            }
            if (!ent) {
                ent = GuestMalloc(280);
                if (!ent) {
                    set_guest_errno(ENOMEM);
                    Ret(regs, 0);
                    return;
                }
                std::lock_guard<std::mutex> io(io_mutex_);
                dir_map_[a0].ent = ent;
            }
            errno = 0;
            const struct dirent* de = ::readdir(dd);
            struct dirent packed_de {};
            if (!de && errno == 0 && next_packed(a0, &packed_de)) {
                de = &packed_de;
            }
            if (!de) {
                if (errno != 0) {
                    set_guest_errno(errno);
                }
                Ret(regs, 0);
                return;
            }
            const std::size_t nm_len = ::strnlen(de->d_name, 255);
            const std::uint32_t reclen = static_cast<std::uint32_t>(
                (kGuestDirentNameOff + nm_len + 1 + 7) & ~std::size_t(7));
            std::uint8_t* p = H<std::uint8_t>(ent);
            std::memset(p, 0, 280);
            const std::uint64_t ino = de->d_ino;
            const std::int64_t off = de->d_off;
            const std::uint16_t rl = static_cast<std::uint16_t>(reclen);
            std::memcpy(p + 0, &ino, 8);
            std::memcpy(p + 8, &off, 8);
            std::memcpy(p + 16, &rl, 2);
            p[18] = de->d_type;
            std::memcpy(p + kGuestDirentNameOff, de->d_name, nm_len);
            Ret(regs, ent);
            return;
        }
        if (id == I::closedir_) {
            // Frees the guest DIR handle and readdir's entry with it.
            DIR* dd = nullptr;
            std::uint32_t ent = 0;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto it = dir_map_.find(a0);
                if (it == dir_map_.end()) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                dd = it->second.d;
                ent = it->second.ent;
                dir_map_.erase(it);
            }
            int r = ::closedir(dd);
            GuestFree(a0);
            if (ent) {
                GuestFree(ent);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
    }
    // --- APK asset bridge (AAssetManager_*): getBuffer model over APK entries.
    // Each handler locks mutex_ only around the maps it touches; reading and
    // inflating an asset runs unlocked (a whole-block lock used to serialise
    // every import that reached this point, and a 32 MB inflate, against all
    // other guest threads). apk_index_ is built once and then only read.
    case I::AAssetManager_fromJava_:
    case I::AAssetManager_open_:
    case I::AAsset_close_:
    case I::AAsset_getBuffer_:
    case I::AAsset_getLength64_:
    {
        if (id == I::AAssetManager_fromJava_) {
            if (!EnsureApkIndex()) {
                std::printf("[asset] LOUD no apk index -- halting\n");
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const std::uint32_t m = next_asset_mgr_++;
            asset_mgr_map_[m] = 1;
            Ret(regs, m);
            return;
        }
        if (id == I::AAssetManager_open_) {
            // NOTE: no mutex held across GuestMalloc (it locks internally).
            const ApkEntry* found = nullptr;
            std::string want;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                if (asset_mgr_map_.find(a0) == asset_mgr_map_.end() || !apk_indexed_) {
                    std::printf("[asset] LOUD open bad mgr %08x -- halting\n", a0);
                    stop_step_ = true;
                    ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                    return;
                }
                want = GuestStr(a1);
                auto it = apk_index_.find(want);
                if (it != apk_index_.end()) {
                    found = &it->second;
                } else {
                    it = apk_index_.find("assets/" + want);
                    if (it != apk_index_.end()) {
                        found = &it->second;
                    }
                }
            }
            if (!found) {
                std::printf("[asset] open(\"%s\") -> NULL (not in apk)\n", want.c_str());
                Ret(regs, 0);
                return;
            }
            // An empty asset is legal (length 0, a 1-byte buffer as handle);
            // the only real limit is the guest heap.
            if (found->usize >= kHeapSize) {
                std::printf("[asset] LOUD open(\"%s\") size %llu exceeds the guest heap\n",
                            want.c_str(), (unsigned long long)found->usize);
                Ret(regs, 0);
                return;
            }
            FILE* f = std::fopen(apk_path_.c_str(), "rb");
            if (!f) {
                Ret(regs, 0);
                return;
            }
            const std::uint32_t gbuf =
                GuestMalloc(static_cast<std::uint32_t>(found->usize ? found->usize : 1));
            if (!gbuf) {
                std::fclose(f);
                Ret(regs, 0);
                return;
            }
            bool ok = false;
            if (found->usize == 0) {
                ok = true;
            } else if (found->method == 0) {
                std::fseek(f, (long)found->data_off, SEEK_SET);
                ok = std::fread(H(gbuf), 1, (std::size_t)found->usize, f) ==
                     (std::size_t)found->usize;
            } else if (found->method == 8) {
                std::vector<std::uint8_t> comp((std::size_t)found->csize);
                std::fseek(f, (long)found->data_off, SEEK_SET);
                if (std::fread(comp.data(), 1, comp.size(), f) == comp.size()) {
                    // APK entries are RAW deflate (no zlib wrapper), so
                    // ::uncompress (expects a zlib header) fails Z_DATA_ERROR.
                    // Use raw inflate instead (verified offline vs python zlib).
                    z_stream strm{};
                    strm.next_in = comp.data();
                    strm.avail_in = (uInt)comp.size();
                    strm.next_out = H(gbuf);
                    strm.avail_out = (uInt)found->usize;
                    if (::inflateInit2(&strm, -MAX_WBITS) == Z_OK) {
                        int zr = ::inflate(&strm, Z_FINISH);
                        ::inflateEnd(&strm);
                        ok = (zr == Z_STREAM_END && strm.total_out == found->usize);
                    }
                    if (!ok) {
                        std::printf("[asset] inflate(\"%s\") failed\n", want.c_str());
                    }
                }
            } else {
                std::printf("[asset] LOUD open(\"%s\") method %u -- halting\n", want.c_str(),
                            found->method);
                std::fclose(f);
                GuestFree(gbuf);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            std::fclose(f);
            if (!ok) {
                std::printf("[asset] open(\"%s\") extract failed\n", want.c_str());
                GuestFree(gbuf);
                Ret(regs, 0);
                return;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                asset_map_[gbuf] = HostAsset{gbuf, found->usize};
#ifdef ABR_DIAG_COUNTERS
                std::uint64_t open_bytes = 0;
                for (const auto& kv : asset_map_) {
                    open_bytes += kv.second.size;
                }
                if (asset_map_.size() > diag_.asset_peak_open) {
                    diag_.asset_peak_open = asset_map_.size();
                }
                if (open_bytes > diag_.asset_peak_bytes) {
                    diag_.asset_peak_bytes = open_bytes;
                }
#endif
            }
            if (EnvOn("HARNESS_FILESLOG")) {
                std::printf("[asset] open(\"%s\") -> %08x (%llu bytes, %s)\n", want.c_str(),
                            gbuf, (unsigned long long)found->usize,
                            found->method == 0 ? "stored" : "deflated");
            }
            Ret(regs, gbuf);  // handle IS the guest buffer (getBuffer model)
            return;
        }
        if (id == I::AAsset_close_) {
            bool known = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                known = asset_map_.erase(a0) != 0;
            }
            // The handle IS the guest buffer (getBuffer model), so closing must
            // free it exactly like AAsset_close on the NDK -- otherwise every
            // opened asset leaks its decompressed bytes (862 opens/boot ~21 MB).
            // Free only handles we actually handed out, never a random address.
            if (known) {
                GuestFree(a0);
            }
            Ret(regs, 0);
            return;
        }
        if (id == I::AAsset_getBuffer_) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = asset_map_.find(a0);
            if (it == asset_map_.end()) {
                Ret(regs, 0);
                return;
            }
            Ret(regs, it->second.guest_ptr);
            return;
        }
        if (id == I::AAsset_getLength64_) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = asset_map_.find(a0);
            if (it == asset_map_.end()) {
                Ret(regs, 0, 0);
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(it->second.size & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((it->second.size >> 32) & 0xFFFFFFFFu));
            return;
        }
    }
    // --- libm: PURE-MATH-BEGIN ---
    case I::__fpclassifyd_:
    case I::__fpclassifyf_:
    case I::__isfinite_:
    case I::__isfinitef_:
    case I::__isnan_:
    case I::__isnanf_:
    case I::__isnormal_:
    case I::__isnormalf_:
    case I::__signbit_:
    case I::__signbitf_:
    case I::__signbitl_:
    case I::acos_:
    case I::acosf_:
    case I::acosh_:
    case I::asin_:
    case I::asinf_:
    case I::asinh_:
    case I::atan_:
    case I::atan2_:
    case I::atan2f_:
    case I::atanf_:
    case I::atanh_:
    case I::cbrt_:
    case I::cbrtf_:
    case I::ceil_:
    case I::ceilf_:
    case I::copysign_:
    case I::copysignf_:
    case I::cos_:
    case I::cosf_:
    case I::cosh_:
    case I::coshf_:
    case I::erf_:
    case I::erfc_:
    case I::erfcf_:
    case I::erff_:
    case I::exp_:
    case I::exp2_:
    case I::exp2f_:
    case I::expf_:
    case I::expm1_:
    case I::expm1f_:
    case I::fdim_:
    case I::fdimf_:
    case I::floor_:
    case I::floorf_:
    case I::fma_:
    case I::fmaf_:
    case I::fmax_:
    case I::fmaxf_:
    case I::fmin_:
    case I::fminf_:
    case I::fmod_:
    case I::fmodf_:
    case I::frexp_:
    case I::frexpf_:
    case I::hypot_:
    case I::hypotf_:
    case I::ilogb_:
    case I::ilogbf_:
    case I::isnan_:
    case I::ldexp_:
    case I::ldexpf_:
    case I::lgamma_:
    case I::lgammaf_:
    case I::llrint_:
    case I::llrintf_:
    case I::llround_:
    case I::llroundf_:
    case I::log_:
    case I::log10_:
    case I::log10f_:
    case I::log1p_:
    case I::log2_:
    case I::log2f_:
    case I::logb_:
    case I::logbf_:
    case I::logf_:
    case I::lrint_:
    case I::lrintf_:
    case I::lround_:
    case I::lroundf_:
    case I::modf_:
    case I::modff_:
    case I::nan_:
    case I::nanf_:
    case I::nanl_:
    case I::nearbyint_:
    case I::nearbyintf_:
    case I::nextafter_:
    case I::nextafterf_:
    case I::nexttoward_:
    case I::nexttowardf_:
    case I::pow_:
    case I::powf_:
    case I::remainder_:
    case I::remainderf_:
    case I::remquo_:
    case I::remquof_:
    case I::rint_:
    case I::rintf_:
    case I::round_:
    case I::roundf_:
    case I::scalbln_:
    case I::scalblnf_:
    case I::scalbn_:
    case I::scalbnf_:
    case I::scalbnl_:
    case I::sin_:
    case I::sinf_:
    case I::sinh_:
    case I::sinhf_:
    case I::sqrt_:
    case I::sqrtf_:
    case I::tan_:
    case I::tanf_:
    case I::tanh_:
    case I::tanhf_:
    case I::tgamma_:
    case I::tgammaf_:
    case I::trunc_:
    case I::truncf_:
#include "math_thunks.inc"
    // --- libm: PURE-MATH-END ---
    case I::glBufferData_:
    case I::glCompressedTexImage2D_:
    case I::glCopyTexImage2D_:
    case I::glCreateProgram_:
    case I::glCreateShader_:
    case I::glGenBuffers_:
    case I::glGenFramebuffers_:
    case I::glGenRenderbuffers_:
    case I::glGenTextures_:
    case I::glGetAttribLocation_:
    case I::glGetIntegerv_:
    case I::glGetProgramInfoLog_:
    case I::glGetProgramiv_:
    case I::glGetShaderInfoLog_:
    case I::glGetShaderiv_:
    case I::glGetString_:
    case I::glGetUniformLocation_:
    case I::glReadPixels_:
    case I::glShaderSource_:
    case I::glTexImage2D_:
    case I::glTexSubImage2D_:
    case I::glVertexAttribPointer_:
#ifndef __ANDROID__
    // --- GLES fake-success shim, PC harness only (no GL there). On Android
    // every gl* import the titles have goes to the driver (RealGL), with or
    // without a current context, exactly as the original library would.
    // Bring-up history: it let shader/program setup
    // pass so update->render progresses to the first frame. Real GLES thunks
    // (surfaceless EGL + host forwarding) come next; this shim is LOUD about
    // what it fakes. Nonzero handles, status queries report GL_TRUE, logs
    // empty. Everything else falls to the generic [stub]->0 tail.
    if (n.compare(0, 2, "gl") == 0) {
        // The real backend was tried at the top of CallImport; this is the
        // fake shim (PC harness without GL, threads without a context).
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (id == I::glGetString_) {
            // No GL context in the harness (real EGL comes later), so report
            // era-correct 2014-Android strings. EXTENSIONS claims ETC1 (the
            // game's sprite textures use it); real decode/forwarding is the
            // GLES milestone's job. NULL here killed texture loading
            // ("Unsupported image file format" -> io::IOException).
            const char* s = "";
            // HARNESS_GLVENDOR / HARNESS_GLRENDERER override (renderer-keyed
            // texture quirk tables exist in-engine; candidates: Mali-400 MP,
            // Adreno, PowerVR SGX 540, Tegra).
            if (a0 == 0x1F00) {
                s = "ARM";  // GL_VENDOR
                if (const char* e = std::getenv("HARNESS_GLVENDOR")) {
                    s = e;
                }
            } else if (a0 == 0x1F01) {
                s = "Mali-400 MP";  // GL_RENDERER (era-plausible)
                if (const char* e = std::getenv("HARNESS_GLRENDERER")) {
                    s = e;
                }
            } else if (a0 == 0x1F02) {
                s = "OpenGL ES 2.0";  // GL_VERSION
            } else if (a0 == 0x1F03) {
                s = "GL_OES_compressed_ETC1_RGB8_texture "
                    "GL_OES_texture_npot "
                    "GL_OES_vertex_half_float "
                    "GL_OES_element_index_uint "
                    "GL_OES_mapbuffer "
                    "GL_EXT_texture_format_BGRA8888";  // GL_EXTENSIONS
            } else if (a0 == 0x1F04) {
                s = "2.0";  // GL_SHADING_LANGUAGE_VERSION
            }
            // One static guest slot per name (idx by low nibble-ish switch).
            static std::uint32_t slots[5] = {0, 0, 0, 0, 0};
            int si = (a0 == 0x1F00) ? 0 : (a0 == 0x1F01) ? 1 : (a0 == 0x1F02) ? 2
                                        : (a0 == 0x1F03)   ? 3
                                        : (a0 == 0x1F04)   ? 4
                                                           : -1;
            std::uint32_t out = 0;
            if (si >= 0) {
                if (!slots[si]) {
                    slots[si] = GuestMalloc(512);
                    if (slots[si]) {
                        std::strncpy(reinterpret_cast<char*>(H(slots[si])), s, 511);
                    }
                }
                out = slots[si];
            }
            std::printf("[gl] GetString(%04x) -> \"%s\"\n", a0, s);
            Ret(regs, out);
            return;
        }
        if (id == I::glCreateShader_ || id == I::glCreateProgram_) {
            static std::uint32_t gl_next = 0;
            Ret(regs, ++gl_next);
            return;
        }
        if (id == I::glGetIntegerv_) {
            // Era-correct Mali-400-ish limits (verified enum vs
            // /usr/include/GLES2/gl2.h). Signature: (pname=a0, params=a1).
            std::uint32_t v0 = 0, v1 = 0, cnt = 1;
            switch (a0) {
                case 0x0D33: v0 = 4096; break;                 // MAX_TEXTURE_SIZE
                case 0x84E8: v0 = 4096; break;                 // MAX_RENDERBUFFER_SIZE
                case 0x8872: v0 = 8; break;                   // MAX_TEXTURE_IMAGE_UNITS
                case 0x8B4D: v0 = 8; break;                   // MAX_COMBINED_TIU
                case 0x8869: v0 = 16; break;                  // MAX_VERTEX_ATTRIBS
                case 0x8DFB: v0 = 128; break;                  // MAX_VERTEX_UNIFORM_VECTORS
                case 0x8DFC: v0 = 8; break;                    // MAX_VARYING_VECTORS
                case 0x8DFF: v0 = 16; break;                   // MAX_FRAGMENT_UNIFORM_VECTORS
                case 0x86A2: v0 = 1; break;                    // NUM_COMPRESSED_TEXTURE_FORMATS
                case 0x86A3: v0 = 0x8D64; break;               // COMPRESSED_FORMATS = ETC1
                case 0x0D3A: v0 = 4096; v1 = 4096; cnt = 2; break;  // MAX_VIEWPORT_DIMS
                case 0x0D50: v0 = 4; break;                    // SUBPIXEL_BITS
                case 0x0D56: v0 = 16; break;                   // DEPTH_BITS
                case 0x0D57: v0 = 8; break;                    // STENCIL_BITS
                default: break;                                // unknown -> 0, logged
            }
            if (CheckRange(a1, 4 * cnt, true)) {
                std::memcpy(H(a1), &v0, 4);
                if (cnt > 1) {
                    std::memcpy(H(a1 + 4), &v1, 4);
                }
            }
            std::printf("[gl] GetIntegerv(%04x) -> %u%s\n", a0, v0,
                        cnt > 1 ? ",..." : "");
            Ret(regs, 0);
            return;
        }
        if (id == I::glGetShaderiv_ || id == I::glGetProgramiv_) {            if (CheckRange(a2, 4, true)) {
                const std::uint32_t one = 1;  // COMPILE/LINK_STATUS = GL_TRUE
                std::memcpy(H(a2), &one, 4);
            }
            std::printf("[gl] %s(%u,%04x) -> TRUE\n", n.c_str(), a0, a1);
            Ret(regs, 0);
            return;
        }
        if (id == I::glGetShaderInfoLog_ || id == I::glGetProgramInfoLog_) {
            if (a2 && CheckRange(a2, 4, true)) {
                const std::uint32_t zero = 0;
                std::memcpy(H(a2), &zero, 4);
            }
            if (a3 && CheckRange(a3, 1, true)) {
                H(a3)[0] = 0;
            }
            std::printf("[gl] %s(%u) -> empty\n", n.c_str(), a0);
            Ret(regs, 0);
            return;
        }
        if (id == I::glGenTextures_ || id == I::glGenBuffers_ || id == I::glGenFramebuffers_ ||
            id == I::glGenRenderbuffers_) {
            // (n=a0, handles=a1): write incrementing nonzero handles.
            static std::uint32_t gl_handle = 100;
            if (a0 > 0 && a0 < 1024 && CheckRange(a1, 4 * a0, true)) {
                for (std::uint32_t i = 0; i < a0; i++) {
                    std::uint32_t h = ++gl_handle;
                    std::memcpy(H(a1 + i * 4), &h, 4);
                }
            }
            std::printf("[gl] %s(%u) -> handles from %u\n", n.c_str(), a0, gl_handle);
            Ret(regs, 0);
            return;
        }
        if (id == I::glGetUniformLocation_ || id == I::glGetAttribLocation_) {
            // Fake but stable locations (hash of name, nonzero).
            std::uint32_t h = 0;
            if (a1 && CheckRange(a1, 1, false)) {
                const char* s = reinterpret_cast<const char*>(H(a1));
                std::size_t k = 0;
                while (k < 128 && CheckRange(a1 + (std::uint32_t)k, 1, false) && s[k]) {
                    h = h * 33 + (unsigned char)s[k];
                    k++;
                }
                h = (h % 64) + 1;
            }
            std::printf("[gl] %s(%u) -> %u\n", n.c_str(), a0, h);
            Ret(regs, h);
            return;
        }
        if (id == I::glTexImage2D_ || id == I::glTexSubImage2D_ || id == I::glCompressedTexImage2D_ ||
            id == I::glCopyTexImage2D_ || id == I::glReadPixels_ || id == I::glBufferData_ ||
            id == I::glVertexAttribPointer_ || id == I::glShaderSource_) {
            // Accepted and ignored (no real GL yet): fake upload succeeds.
            // Bulk-pointer args are range-checked but not consumed.
            // TEMP-DIAG (real-GLES prep): log call signatures to learn what
            // the host GPU must accept (formats, sizes, shader sources).
            if (id == I::glShaderSource_ && a1 > 0 && a1 < 16 && CheckRange(a2, 4 * a1, false)) {
                std::printf("[glsrc] shader=%u count=%u\n", a0, a1);
                for (std::uint32_t i = 0; i < a1; i++) {
                    std::uint32_t sp = 0;
                    std::memcpy(&sp, H(a2 + i * 4), 4);
                    std::int32_t slen = -1;
                    if (a3 && CheckRange(a3 + i * 4, 4, false)) {
                        std::memcpy(&slen, H(a3 + i * 4), 4);
                    }
                    if (!sp || !CheckRange(sp, 1, false)) {
                        std::printf("[glsrc] #%u NULL\n", i);
                        continue;
                    }
                    std::size_t L = 0;
                    const char* s = reinterpret_cast<const char*>(H(sp));
                    if (slen < 0) {
                        while (L < 4096 && CheckRange(sp + (std::uint32_t)L, 1, false) && s[L]) {
                            L++;
                        }
                    } else {
                        L = (std::size_t)slen > 65536 ? 65536 : (std::size_t)slen;
                    }
                    std::printf("[glsrc] #%u ptr=%08x slen=%d : %.200s\n", i, sp, slen,
                                std::string(s, L > 200 ? 200 : L).c_str());
                    // TEMP-DIAG (real-GLES prep): stash full source for
                    // offline compile tests (host FS, harness-only).
#ifndef __ANDROID__
                    if (L > 16 && L <= 65536) {
                        std::filesystem::create_directories("work/shaders");
                        char fn[64];
                        std::snprintf(fn, sizeof(fn), "work/shaders/src_%u_%u.glsl", a0, i);
                        FILE* f = std::fopen(fn, "wb");
                        if (f) {
                            std::fwrite(s, 1, L, f);
                            std::fclose(f);
                        }
                    }
#endif
                }
            }
            if ((id == I::glTexImage2D_ || id == I::glCompressedTexImage2D_) && a1 <= 8) {
                // APCS: (target,level,intfmt,width) in regs; the rest on guest
                // stack: height,border,format,type,data.
                std::uint32_t stk[5] = {0};
                for (int i = 0; i < 5; i++) {
                    if (CheckRange(regs[13] + (std::uint32_t)(i * 4), 4, false)) {
                        std::memcpy(&stk[i], H(regs[13] + (std::uint32_t)(i * 4)), 4);
                    }
                }
                std::printf("[gltex] %s tgt=%04x lvl=%u ifmt=%04x %ux%u fmt=%04x typ=%04x data=%08x\n",
                            n.c_str(), a0, a1, a2, a3, stk[0], stk[2], stk[3], stk[4]);
            }
#ifndef __ANDROID__
            std::printf("[gl] %s accepted (fake)\n", n.c_str());
#endif
            Ret(regs, 0);
            return;
        }
    }
#endif  // !__ANDROID__
    case I::__system_property_get_:
    if (id == I::__system_property_get_) {
        // int __system_property_get(const char* name, char* value): copies the
        // value (at most PROP_VALUE_MAX = 92 bytes incl. NUL) and returns its
        // length; a missing property gives "" and 0. On the device the real
        // property service answers (it used to be a table of invented values:
        // sdk 21, model "izan", DNS 8.8.8.8). The native harness has no
        // property service and keeps a small table (HARNESS_DNS1/2 override).
        if (!CheckRange(a1, kGuestPropValueMax, true)) {
            goto bad_ptr;
        }
        std::uint32_t klen = 0;
        if (!GuestStrnlen(a0, kGuestPropValueMax * 4, &klen)) {
            goto bad_ptr;
        }
        const std::string key(H<const char>(a0), klen);
        std::string val;
#ifdef __ANDROID__
        {
            char buf[PROP_VALUE_MAX] = {0};
            const int len = ::__system_property_get(key.c_str(), buf);
            if (len > 0) {
                val.assign(buf, static_cast<std::size_t>(len));
            }
        }
#else
        if (key == "net.dns1" || key == "net.dns2") {
            val = (key == "net.dns1") ? "8.8.8.8" : "8.8.4.4";
            if (const char* e = std::getenv(key == "net.dns1" ? "HARNESS_DNS1" : "HARNESS_DNS2")) {
                val = e;
            }
        } else if (key == "ro.build.version.sdk") {
            val = "21";
        }
#endif
        if (val.size() >= kGuestPropValueMax) {
            val.resize(kGuestPropValueMax - 1);
        }
        std::memcpy(H(a1), val.c_str(), val.size() + 1);
        if (DiagOn()) {
            std::printf("[prop]%s \"%s\" -> \"%s\"\n", GuestTid() == 1 ? "" : "(T2+)", key.c_str(),
                        val.c_str());
        }
        Ret(regs, static_cast<std::uint32_t>(val.size()));
        return;
    }
    case I::difftime_:
    case I::mktime_:
    case I::strftime_:
    if (id == I::mktime_ || id == I::difftime_ || id == I::strftime_) {
        // Real time-lib thunks (were stub->0: frozen daily/session timers).
        // Guest Bionic struct tm (44B): 9x int + long gmtoff + char* zone.
        if (id == I::mktime_) {
            std::int32_t f[9] = {};
            if (CheckRange(a0, 36, false)) {
                for (int i = 0; i < 9; i++) {
                    std::memcpy(&f[i], H(a0 + (std::uint32_t)(i * 4)), 4);
                }
            }
            struct tm ht = {};
            ht.tm_sec = f[0];
            ht.tm_min = f[1];
            ht.tm_hour = f[2];
            ht.tm_mday = f[3];
            ht.tm_mon = f[4];
            ht.tm_year = f[5];
            ht.tm_wday = f[6];
            ht.tm_yday = f[7];
            ht.tm_isdst = f[8];
            const std::time_t r = std::mktime(&ht);
            if (CheckRange(a0, 36, true)) {
                const std::int32_t o[9] = {ht.tm_sec, ht.tm_min, ht.tm_hour, ht.tm_mday,
                                           ht.tm_mon, ht.tm_year, ht.tm_wday, ht.tm_yday,
                                           ht.tm_isdst};
                for (int i = 0; i < 9; i++) {
                    std::memcpy(H(a0 + (std::uint32_t)(i * 4)), &o[i], 4);
                }
            }
            Ret(regs, static_cast<std::uint32_t>(r & 0xFFFFFFFFu));
            return;
        }
        if (id == I::difftime_) {
            const double d = std::difftime((std::time_t)(std::int32_t)a0,
                                           (std::time_t)(std::int32_t)a1);
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            Ret(regs, static_cast<std::uint32_t>(u & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((u >> 32) & 0xFFFFFFFFu));
            return;
        }
        // strftime(a0=buf, a1=max, a2=fmt, a3=tm*): the guest's buffer is
        // a1 bytes. The old code clamped that to 256, so a longer result
        // was silently cut; the cap is now only a sanity bound.
        std::uint32_t cap = a1 > 4096u ? 4096u : a1;
        std::int32_t f[9] = {};
        if (CheckRange(a3, 36, false)) {
            for (int i = 0; i < 9; i++) {
                std::memcpy(&f[i], H(a3 + (std::uint32_t)(i * 4)), 4);
            }
        }
        struct tm ht = {};
        ht.tm_sec = f[0];
        ht.tm_min = f[1];
        ht.tm_hour = f[2];
        ht.tm_mday = f[3];
        ht.tm_mon = f[4];
        ht.tm_year = f[5];
        ht.tm_wday = f[6];
        ht.tm_yday = f[7];
        ht.tm_isdst = f[8];
        std::vector<char> tmp(cap ? cap : 1);
        const std::size_t o = std::strftime(tmp.data(), tmp.size(), GuestStr(a2), &ht);
        if (o == 0 || o + 1 > cap || !CheckRange(a0, o + 1, true)) {
            Ret(regs, 0);
            return;
        }
        std::memcpy(H(a0), tmp.data(), o + 1);
        Ret(regs, static_cast<std::uint32_t>(o));
        return;
    }
    case I::rand_:
    case I::random_:
    case I::srand_:
    case I::srandom_:
    if (id == I::rand_ || id == I::random_ || id == I::srand_ || id == I::srandom_) {
        // bionic's rand() IS random() and srand() IS srandom() (AOSP
        // libc/bionic/rand.cpp), and bionic's random() is the BSD TYPE_3
        // generator -- see guest/bsd_random.h. The drand48 LCG that used to
        // answer these calls is a DIFFERENT generator (its constants only
        // describe srand48/lrand48/drand48), so the guests' sequences were
        // not a device's.
        static abr::BsdRandom rng;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (id == I::srand_ || id == I::srandom_) {
            rng.Seed(a0);
            Ret(regs, 0);
            return;
        }
        Ret(regs, static_cast<std::uint32_t>(rng.Next()));
        return;
    }
    case I::lrand48_:
    case I::srand48_:
    if (id == I::srand48_ || id == I::lrand48_) {
        // Real 48-bit LCG (was stub->0: EVERY lrand48() returned 0, so all
        // per-bird animation phases / blink thresholds / interval picks
        // collapsed to one value -> synchronous jumps + every-frame blinks).
        // Same constants as Bionic/glibc (a=0x5DEECE66D, c=0xB), so sequences
        // match the device for the same seed. Own state (no platform
        // srand48_r dependence: NDK availability varies); guarded by mutex_
        // (workers call concurrently). Guest long = 32-bit.
        static std::uint64_t rng_x = 0x1234ABCD330Eull;  // default seed state
        static bool rng_seeded = false;
        const std::uint64_t rng_a = 0x5DEECE66Dull, rng_c = 0xBull;
        const std::uint64_t rng_m = (1ull << 48) - 1;
        auto rng_next = [&]() -> std::uint64_t {
            rng_x = (rng_a * rng_x + rng_c) & rng_m;
            return rng_x;
        };
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (id == I::srand48_) {
            rng_x = ((std::uint64_t)(a0 & 0xFFFFFFFFu) << 16) | 0x330Eull;
            rng_seeded = true;
            (void)rng_seeded;
            Ret(regs, 0);
            return;
        }
        // lrand48: [0,2^31) (RAND_MAX on both Bionic and glibc).
        Ret(regs, static_cast<std::uint32_t>((rng_next() >> 17) & 0x7FFFFFFFu));
        return;
    }
    case I::qsort_:
    if (id == I::qsort_) {
        // Real sort (was silent no-op stub): host stable_sort over indices,
        // guest comparator via callback JIT (PvZ re-enter pattern: separate
        // Jit, lr=halt slot). Atomic commit: guest memory written only on
        // full success; any comparator/run failure leaves the array untouched.
        const std::uint32_t base = a0, cnt = a1, sz = a2, compar = a3;
        if (cnt <= 1 || sz == 0) {
            Ret(regs, 0);
            return;
        }
        const std::uint64_t total = (std::uint64_t)cnt * sz;
        if (!base || !compar || total == 0 || total > 0x4000000u ||
            !CheckRange(base, (std::size_t)total, false)) {
            goto bad_ptr;
        }
        std::vector<std::uint8_t> buf((std::size_t)total);
        std::memcpy(buf.data(), H(base), (std::size_t)total);
        std::vector<std::uint32_t> idx((std::size_t)cnt);
        for (std::uint32_t i = 0; i < cnt; i++) {
            idx[i] = i;
        }
        bool ok = true;
        std::stable_sort(idx.begin(), idx.end(), [&](std::uint32_t x, std::uint32_t y) {
            if (!ok) {
                return false;
            }
            std::int32_t r = 0;
            if (!RunComparator(compar, base + x * sz, base + y * sz, r)) {
                ok = false;
                return false;
            }
            return r < 0;
        });
        if (!ok) {
            std::printf("[qsort] LOUD comparator failed (%08x n=%u hr=%d) -- array untouched\n",
                        compar, cnt, cb_last_hr_);
            Ret(regs, 0);
            return;
        }
        std::vector<std::uint8_t> out((std::size_t)total);
        for (std::uint32_t i = 0; i < cnt; i++) {
            std::memcpy(out.data() + (std::size_t)i * sz, buf.data() + (std::size_t)idx[i] * sz,
                        sz);
        }
        if (!CheckRange(base, (std::size_t)total, true)) {
            goto bad_ptr;
        }
        std::memcpy(H(base), out.data(), (std::size_t)total);
        Ret(regs, 0);
        return;
    }
    case I::bsearch_:
    if (id == I::bsearch_) {
        // Real binary search (was always-NULL stub): key=a0, base=a1,
        // nmemb=a2, size=a3, compar on guest stack. Returns element VA or 0.
        const std::uint32_t key = a0, base = a1, cnt = a2, sz = a3;
        std::uint32_t compar = 0;
        if (!CheckRange(regs[13], 4, false)) {
            goto bad_ptr;
        }
        std::memcpy(&compar, H(regs[13]), 4);
        if (cnt == 0 || sz == 0 || !compar) {
            Ret(regs, 0);
            return;
        }
        const std::uint64_t total = (std::uint64_t)cnt * sz;
        if (!base || !key || total == 0 || total > 0x4000000u ||
            !CheckRange(key, sz, false) || !CheckRange(base, (std::size_t)total, false)) {
            goto bad_ptr;
        }
        std::uint32_t lo = 0, hi = cnt, found = 0;
        bool ok = true;
        while (lo < hi && ok) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            std::int32_t r = 0;
            if (!RunComparator(compar, key, base + mid * sz, r)) {
                ok = false;
                break;
            }
            if (r < 0) {
                hi = mid;
            } else if (r > 0) {
                lo = mid + 1;
            } else {
                found = base + mid * sz;
                break;
            }
        }
        if (!ok) {
            std::printf("[qsort] LOUD bsearch comparator failed (%08x hr=%d) -- NULL\n", compar,
                        cb_last_hr_);
        }
        Ret(regs, found);
        return;
    }
    case I::getenv_:
    if (id == I::getenv_) {
        if (!CheckRange(a0, 1, false)) {
            goto bad_ptr;
        }
        const std::string key = GuestStr(a0);
        const std::uint32_t v = GuestGetenv(key);
        {
            // Which names a guest asks for: once per name (a port needs to
            // see them; the old stub printed the first five calls).
            static std::set<std::string> seen;
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (seen.size() < 200 && seen.insert(key).second) {
                std::printf("[env] getenv(\"%s\") -> %s\n", key.c_str(), v ? GuestStr(v) : "NULL");
            }
        }
        Ret(regs, v);
        return;
    }
    case I::setenv_:
    case I::unsetenv_:
    if (id == I::setenv_ || id == I::unsetenv_) {
        if (!CheckRange(a0, 1, false) || (id == I::setenv_ && !CheckRange(a1, 1, false))) {
            goto bad_ptr;
        }
        const std::string key = GuestStr(a0);
        const std::string val = id == I::setenv_ ? std::string(GuestStr(a1)) : std::string();
        const std::uint32_t e = GuestSetenv(key, id == I::setenv_ ? &val : nullptr, a2 != 0);
        if (e != 0) {
            SetGuestErrno(static_cast<int>(e));
        }
        Ret(regs, e == 0 ? 0u : 0xFFFFFFFFu);
        return;
    }
    case I::crc32_:
    case I::deflate_:
    case I::deflateEnd_:
    case I::deflateInit2__:
    case I::deflateReset_:
    case I::inflate_:
    case I::inflateEnd_:
    case I::inflateInit2__:
    case I::inflateInit__:
    case I::inflateReset_:
    if (id == I::crc32_ || id == I::inflate_ || id == I::inflateInit__ || id == I::inflateInit2__ ||
        id == I::inflateEnd_ || id == I::inflateReset_ || id == I::deflate_ || id == I::deflateInit2__ ||
        id == I::deflateEnd_ || id == I::deflateReset_) {
        if (!ZlibCall(id, n, regs)) {
            goto bad_ptr;
        }
        return;
    }
    case I::eglBindAPI_:
    case I::eglChooseConfig_:
    case I::eglCreateContext_:
    case I::eglCreateWindowSurface_:
    case I::eglDestroyContext_:
    case I::eglDestroySurface_:
    case I::eglGetConfigAttrib_:
    case I::eglGetConfigs_:
    case I::eglGetCurrentContext_:
    case I::eglGetCurrentDisplay_:
    case I::eglGetCurrentSurface_:
    case I::eglGetDisplay_:
    case I::eglGetError_:
    case I::eglGetProcAddress_:
    case I::eglInitialize_:
    case I::eglMakeCurrent_:
    case I::eglQueryContext_:
    case I::eglQueryString_:
    case I::eglQuerySurface_:
    case I::eglReleaseThread_:
    case I::eglSwapBuffers_:
    case I::eglSwapInterval_:
    case I::eglTerminate_:
    case I::eglWaitGL_:
    case I::eglWaitNative_:
    case I::execl_:
    case I::execv_:
    case I::execve_:
    if (n[0] == 'e' && EglCall(id, n, regs)) {
        return;
    }
    case I::AConfiguration_delete_:
    case I::AConfiguration_fromAssetManager_:
    case I::AConfiguration_new_:
    case I::AInputEvent_getDeviceId_:
    case I::AInputEvent_getSource_:
    case I::AInputEvent_getType_:
    case I::AInputQueue_attachLooper_:
    case I::AInputQueue_detachLooper_:
    case I::AInputQueue_finishEvent_:
    case I::AInputQueue_getEvent_:
    case I::AInputQueue_preDispatchEvent_:
    case I::AKeyEvent_getAction_:
    case I::AKeyEvent_getKeyCode_:
    case I::AKeyEvent_getMetaState_:
    case I::ALooper_forThread_:
    case I::ALooper_pollAll_:
    case I::ALooper_pollOnce_:
    case I::ALooper_prepare_:
    case I::ALooper_wake_:
    case I::AMotionEvent_getAction_:
    case I::AMotionEvent_getDownTime_:
    case I::AMotionEvent_getEdgeFlags_:
    case I::AMotionEvent_getEventTime_:
    case I::AMotionEvent_getFlags_:
    case I::AMotionEvent_getHistoricalEventTime_:
    case I::AMotionEvent_getHistoricalOrientation_:
    case I::AMotionEvent_getHistoricalPressure_:
    case I::AMotionEvent_getHistoricalRawX_:
    case I::AMotionEvent_getHistoricalRawY_:
    case I::AMotionEvent_getHistoricalSize_:
    case I::AMotionEvent_getHistoricalToolMajor_:
    case I::AMotionEvent_getHistoricalToolMinor_:
    case I::AMotionEvent_getHistoricalTouchMajor_:
    case I::AMotionEvent_getHistoricalTouchMinor_:
    case I::AMotionEvent_getHistoricalX_:
    case I::AMotionEvent_getHistoricalY_:
    case I::AMotionEvent_getHistorySize_:
    case I::AMotionEvent_getMetaState_:
    case I::AMotionEvent_getOrientation_:
    case I::AMotionEvent_getPointerCount_:
    case I::AMotionEvent_getPointerId_:
    case I::AMotionEvent_getPressure_:
    case I::AMotionEvent_getRawX_:
    case I::AMotionEvent_getRawY_:
    case I::AMotionEvent_getSize_:
    case I::AMotionEvent_getToolMajor_:
    case I::AMotionEvent_getToolMinor_:
    case I::AMotionEvent_getTouchMajor_:
    case I::AMotionEvent_getTouchMinor_:
    case I::AMotionEvent_getX_:
    case I::AMotionEvent_getXPrecision_:
    case I::AMotionEvent_getY_:
    case I::AMotionEvent_getYPrecision_:
    case I::ANativeWindow_acquire_:
    case I::ANativeWindow_fromSurface_:
    case I::ANativeWindow_release_:
    case I::ANativeWindow_setBuffersGeometry_:
    case I::ASensorEventQueue_getEvents_:
    case I::ASensorEventQueue_hasEvents_:
    case I::ASensorManager_createEventQueue_:
    case I::ASensorManager_destroyEventQueue_:
    case I::ASensorManager_getDefaultSensor_:
    case I::ASensorManager_getInstance_:
    case I::ASensorManager_getSensorList_:
    if (n[0] == 'A' && NdkCall(id, n, regs)) {
        return;
    }
    case I::__get_h_errno_:
    case I::freeaddrinfo_:
    case I::gai_strerror_:
    case I::getaddrinfo_:
    case I::gethostbyaddr_:
    case I::gethostbyname_:
    if (id == I::getaddrinfo_ || id == I::freeaddrinfo_ || id == I::gai_strerror_ || id == I::gethostbyname_ ||
        id == I::gethostbyaddr_ || id == I::__get_h_errno_) {
        if (!NetDbCall(id, n, regs)) {
            goto bad_ptr;
        }
        return;
    }
    case I::getwc_:
    case I::mbrtowc_:
    case I::putwc_:
    case I::ungetwc_:
    case I::wcrtomb_:
    case I::wcscoll_:
    case I::wcsftime_:
    case I::wcsxfrm_:
    if (id == I::mbrtowc_ || id == I::wcrtomb_ || id == I::wcscoll_ || id == I::wcsxfrm_ ||
        id == I::wcsftime_ || id == I::getwc_ || id == I::putwc_ || id == I::ungetwc_) {
        if (!WideCall(id, n, regs)) {
            goto bad_ptr;
        }
        return;
    }
    case I::SHA1Final_:
    case I::SHA1Init_:
    case I::SHA1Update_:
    if (id == I::SHA1Init_ || id == I::SHA1Update_ || id == I::SHA1Final_) {
        if (!Sha1Call(id, n, regs)) {
            goto bad_ptr;
        }
        return;
    }
    // --- processes. The emulator cannot give a guest a child process (a
    //     fork would duplicate the whole host, exec would replace it), so
    //     these fail the way a system without the facility answers.
    case I::fork_:
    case I::pclose_:
    case I::popen_:
    case I::ptrace_:
    case I::system_:
    case I::waitpid_:
    if (id == I::fork_ || id == I::execv_ || id == I::execve_ || id == I::execl_ || id == I::popen_ ||
        id == I::system_ || id == I::pclose_ || id == I::waitpid_ || id == I::ptrace_) {
        {
            static std::set<std::string> said;
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (said.insert(n).second) {
                std::printf("[svc] %s is not offered (no guest processes) -> failure\n", n.c_str());
            }
        }
        if (id == I::system_ && a0 == 0) {
            Ret(regs, 0);  // system(NULL): no command processor available
            return;
        }
        SetGuestErrno(id == I::pclose_ || id == I::waitpid_ ? ECHILD : id == I::ptrace_ ? EPERM : ENOSYS);
        Ret(regs, id == I::popen_ ? 0u : 0xFFFFFFFFu);
        return;
    }
    case I::setresuid_:
    if (id == I::setresuid_) {
        // An app may "change" to the ids it already has; anything else needs
        // privileges it does not have (EPERM). -1 keeps an id.
        uid_t r = 0, e = 0, sv = 0;
        ::getresuid(&r, &e, &sv);
        auto same = [](std::uint32_t want, uid_t have) {
            return want == 0xFFFFFFFFu || want == have;
        };
        if (same(a0, r) && same(a1, e) && same(a2, sv)) {
            Ret(regs, 0);
            return;
        }
        SetGuestErrno(EPERM);
        Ret(regs, 0xFFFFFFFFu);
        return;
    }
    case I::setitimer_:
    if (id == I::setitimer_) {
        // setitimer(which, const itimerval* new, itimerval* old). Interval
        // timers would have to raise guest signals; none is offered, so only
        // disarming succeeds (old reads as disarmed).
        if ((a1 && !CheckRange(a1, 16, false)) || (a2 && !CheckRange(a2, 16, true))) {
            goto bad_ptr;
        }
        if (a2) {
            std::memset(H(a2), 0, 16);
        }
        std::uint32_t v[4] = {0, 0, 0, 0};
        if (a1) {
            std::memcpy(v, H(a1), 16);
        }
        if (v[2] == 0 && v[3] == 0) {  // it_value zero: disarm
            Ret(regs, 0);
            return;
        }
        std::printf("[svc] setitimer(%u) arming is not offered -> ENOSYS\n", a0);
        SetGuestErrno(ENOSYS);
        Ret(regs, 0xFFFFFFFFu);
        return;
    }
    // --- ctype functions (for guests that call them rather than reading
    //     _ctype_). Bionic 7.1.2 takes them from OpenBSD's isctype.c: EOF
    //     -> 0, otherwise the byte table entry of (unsigned char)c & mask.
    case I::isalnum_:
    case I::isalpha_:
    case I::isspace_:
    case I::isxdigit_:
    if (id == I::isalnum_ || id == I::isalpha_ || id == I::isspace_ || id == I::isxdigit_) {
        const std::int32_t c = static_cast<std::int32_t>(a0);
        const std::uint8_t mask = id == I::isalnum_   ? 0x07   // _U|_L|_N
                                  : id == I::isalpha_ ? 0x03   // _U|_L
                                  : id == I::isspace_ ? 0x08   // _S
                                                   : 0x44;  // _N|_X
        Ret(regs, c == -1 ? 0u : BionicCtypeClass(static_cast<std::uint8_t>(c)) & mask);
        return;
    }
    case I::tolower_:
    if (id == I::tolower_) {
        // OpenBSD tolower: values above 255 (and EOF) unchanged, else the
        // _tolower_tab_ entry (C locale: only A-Z change).
        Ret(regs, a0 >= 'A' && a0 <= 'Z' ? a0 + 32 : a0);
        return;
    }
    case I::div_:
    case I::perror_:
    case I::putchar_:
    case I::puts_:
    case I::strcasestr_:
    case I::strsep_:
    case I::strxfrm_:
    {
        // Bounded guest strlen; 0xFFFFFFFF = unreadable or unterminated.
        auto guest_strlen = [&](std::uint32_t s) -> std::uint32_t {
            for (std::uint32_t i = 0; i < 0x100000u; i++) {
                if (!CheckRange(s + i, 1, false)) {
                    return 0xFFFFFFFFu;
                }
                if (*H<const char>(s + i) == '\0') {
                    return i;
                }
            }
            return 0xFFFFFFFFu;
        };
        if (id == I::strcasestr_) {
            const std::uint32_t hl = guest_strlen(a0), nl = guest_strlen(a1);
            if (hl == 0xFFFFFFFFu || nl == 0xFFFFFFFFu) {
                goto bad_ptr;
            }
            const char* h = H<const char>(a0);
            const char* nd = H<const char>(a1);
            auto low = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
            for (std::uint32_t i = 0; i + nl <= hl; i++) {
                std::uint32_t k = 0;
                while (k < nl && low(h[i + k]) == low(nd[k])) {
                    k++;
                }
                if (k == nl) {
                    Ret(regs, a0 + i);
                    return;
                }
            }
            Ret(regs, 0);
            return;
        }
        if (id == I::strsep_) {
            // strsep(char** stringp, delim): returns *stringp, ends that token at
            // the first delimiter and points *stringp past it (NULL at the end).
            if (!CheckRange(a0, 4, true)) {
                goto bad_ptr;
            }
            std::uint32_t sp = 0;
            std::memcpy(&sp, H(a0), 4);
            if (sp == 0) {
                Ret(regs, 0);
                return;
            }
            const std::uint32_t sl = guest_strlen(sp), dl = guest_strlen(a1);
            if (sl == 0xFFFFFFFFu || dl == 0xFFFFFFFFu) {
                goto bad_ptr;
            }
            char* str = H<char>(sp);
            const char* delim = H<const char>(a1);
            const std::size_t tok = std::strcspn(str, delim);
            std::uint32_t next = 0;
            if (tok < sl) {
                str[tok] = '\0';
                next = sp + static_cast<std::uint32_t>(tok) + 1;
            }
            std::memcpy(H(a0), &next, 4);
            Ret(regs, sp);
            return;
        }
        if (id == I::strxfrm_) {
            // C locale (OpenBSD): strlcpy(dst, src, n), or strlen(src) for n = 0.
            const std::uint32_t sl = guest_strlen(a1);
            if (sl == 0xFFFFFFFFu) {
                goto bad_ptr;
            }
            if (a2 != 0) {
                const std::uint32_t cp = sl < a2 ? sl : a2 - 1;
                if (!CheckRange(a0, cp + 1, true)) {
                    goto bad_ptr;
                }
                std::memmove(H(a0), H(a1), cp);
                *H<char>(a0 + cp) = '\0';
            }
            Ret(regs, sl);
            return;
        }
        if (id == I::puts_ || id == I::putchar_ || id == I::perror_) {
            // stdout/stderr of the guest are the host's (as for printf).
            if (id == I::putchar_) {
                std::fputc(static_cast<int>(a0 & 0xFFu), stdout);
                Ret(regs, a0 & 0xFFu);
                return;
            }
            if (a0 != 0 && guest_strlen(a0) == 0xFFFFFFFFu) {
                goto bad_ptr;
            }
            if (id == I::puts_) {
                std::fputs(GuestStr(a0), stdout);
                std::fputc('\n', stdout);
                Ret(regs, '\n');  // OpenBSD puts: a non-negative value
                return;
            }
            // perror(s): "s: <message for errno>", or just the message.
            std::int32_t e = 0;
            const std::uint32_t es = ErrnoSlot();
            if (CheckRange(es, 4, false)) {
                std::memcpy(&e, H(es), 4);
            }
            const std::string pre = (a0 != 0 && *GuestStr(a0)) ? std::string(GuestStr(a0)) + ": " : "";
            std::fprintf(stderr, "%s%s\n", pre.c_str(), std::strerror(e));
            RetVoid(regs);
            return;
        }
        if (id == I::div_) {
            // div_t div(int, int): an 8-byte struct, returned through the hidden
            // pointer in r0 (AAPCS32); the operands are in r1, r2. The two cases
            // C leaves undefined would trap the host divider: answer 0.
            if (!CheckRange(a0, 8, true)) {
                goto bad_ptr;
            }
            const std::int32_t num = static_cast<std::int32_t>(a1);
            const std::int32_t den = static_cast<std::int32_t>(a2);
            std::int32_t qr[2] = {0, 0};
            if (den != 0 && !(num == INT32_MIN && den == -1)) {
                qr[0] = num / den;
                qr[1] = num % den;
            } else {
                std::printf("[svc] LOUD div(%d, %d) is undefined -- answering 0\n", num, den);
            }
            std::memcpy(H(a0), qr, 8);
            RetVoid(regs);
            return;
        }
    }
    case I::sleep_:
    case I::usleep_:
    if (id == I::usleep_ || id == I::sleep_) {
        // Signal-aware like nanosleep; the full time always elapses.
        GuestSleepUs(id == I::usleep_ ? std::uint64_t(a0) : std::uint64_t(a0) * 1000000u);
        Ret(regs, 0);
        return;
    }
    case I::sched_get_priority_max_:
    case I::sched_get_priority_min_:
    if (id == I::sched_get_priority_max_ || id == I::sched_get_priority_min_) {
        // Policy numbers are the kernel's on both sides.
        const int r = id == I::sched_get_priority_max_ ? ::sched_get_priority_max(static_cast<int>(a0))
                                                    : ::sched_get_priority_min(static_cast<int>(a0));
        if (r < 0) {
            SetGuestErrno(errno);
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case I::pthread_getschedparam_:
    case I::pthread_setschedparam_:
    if (id == I::pthread_getschedparam_ || id == I::pthread_setschedparam_) {
        // Guest threads keep the policy they started with, SCHED_OTHER at
        // priority 0 (an app's threads); asking for a real-time policy is
        // refused as Android refuses it to an app (EPERM).
        // get(thread, int* policy, sched_param* p); set(thread, policy, p).
        const bool get = (id == I::pthread_getschedparam_);
        if (!CheckRange(a2, 4, get) || (get && !CheckRange(a1, 4, true))) {
            goto bad_ptr;
        }
        if (get) {
            const std::uint32_t z = 0;
            std::memcpy(H(a1), &z, 4);  // SCHED_OTHER
            std::memcpy(H(a2), &z, 4);
            Ret(regs, 0);
            return;
        }
        std::int32_t prio = 0;
        std::memcpy(&prio, H(a2), 4);
        Ret(regs, (a1 == SCHED_OTHER && prio == 0) ? 0u : static_cast<std::uint32_t>(EPERM));
        return;
    }
    case I::getpriority_:
    case I::setpriority_:
    if (id == I::getpriority_ || id == I::setpriority_) {
        // (which, who[, prio]). A PRIO_PROCESS `who` is a thread id on Linux:
        // a guest tid (gettid) maps to its host thread; 0 and the process id
        // pass through, other ids are unknown (ESRCH).
        int who = static_cast<int>(a1);
        if (a0 == PRIO_PROCESS && who != 0 && who != ::getpid()) {
            who = HostTidOf(a1);
            if (who == 0) {
                SetGuestErrno(ESRCH);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
        }
        errno = 0;
        const int r = id == I::getpriority_
                          ? ::getpriority(static_cast<int>(a0), static_cast<id_t>(who))
                          : ::setpriority(static_cast<int>(a0), static_cast<id_t>(who),
                                          static_cast<int>(a2));
        if (errno != 0) {
            SetGuestErrno(errno);
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case I::getrusage_:
    if (id == I::getrusage_) {
        // Guest struct rusage (LP32, NDK): two timevals of 2 longs, then 14
        // longs; 72 bytes. Values are truncated to 32 bits like the kernel's
        // compat layer does.
        if (!CheckRange(a1, 72, true)) {
            goto bad_ptr;
        }
        struct rusage ru {};
        if (::getrusage(static_cast<int>(a0), &ru) != 0) {
            SetGuestErrno(errno);
            Ret(regs, static_cast<std::uint32_t>(-1));
            return;
        }
        const long v[18] = {ru.ru_utime.tv_sec, ru.ru_utime.tv_usec, ru.ru_stime.tv_sec,
                            ru.ru_stime.tv_usec, ru.ru_maxrss, ru.ru_ixrss, ru.ru_idrss,
                            ru.ru_isrss, ru.ru_minflt, ru.ru_majflt, ru.ru_nswap, ru.ru_inblock,
                            ru.ru_oublock, ru.ru_msgsnd, ru.ru_msgrcv, ru.ru_nsignals,
                            ru.ru_nvcsw, ru.ru_nivcsw};
        for (int i = 0; i < 18; i++) {
            const std::int32_t w = static_cast<std::int32_t>(v[i]);
            std::memcpy(H(a1 + static_cast<std::uint32_t>(i) * 4), &w, 4);
        }
        Ret(regs, 0);
        return;
    }
    case I::prctl_:
    if (id == I::prctl_) {
        // Only the thread name (PR_SET_NAME / PR_GET_NAME: 16 bytes with the
        // NUL) reaches the host thread; other options are not offered.
        if (a0 == PR_SET_NAME || a0 == PR_GET_NAME) {
            if (!CheckRange(a1, 16, a0 == PR_GET_NAME)) {
                goto bad_ptr;
            }
            char nm[16] = {};
            if (a0 == PR_SET_NAME) {
                std::memcpy(nm, H(a1), 15);
                ::prctl(PR_SET_NAME, nm, 0, 0, 0);
            } else {
                ::prctl(PR_GET_NAME, nm, 0, 0, 0);
                std::memcpy(H(a1), nm, 16);
            }
            Ret(regs, 0);
            return;
        }
        std::printf("[svc] prctl(%u) is not offered -> EINVAL\n", a0);
        SetGuestErrno(EINVAL);
        Ret(regs, static_cast<std::uint32_t>(-1));
        return;
    }
    case I::getuid_:
    if (id == I::getuid_) {
        Ret(regs, static_cast<std::uint32_t>(::getuid()));
        return;
    }
    case I::getresuid_:
    if (id == I::getresuid_) {
        if (!CheckRange(a0, 4, true) || !CheckRange(a1, 4, true) || !CheckRange(a2, 4, true)) {
            goto bad_ptr;
        }
        uid_t r = 0, e = 0, sv = 0;
        ::getresuid(&r, &e, &sv);
        const std::uint32_t w[3] = {r, e, sv};
        std::memcpy(H(a0), &w[0], 4);
        std::memcpy(H(a1), &w[1], 4);
        std::memcpy(H(a2), &w[2], 4);
        Ret(regs, 0);
        return;
    }
    case I::getgrgid_:
    case I::getgrnam_:
    case I::getpwnam_:
    case I::getpwuid_:
    if (id == I::getpwuid_ || id == I::getpwnam_ || id == I::getgrgid_ || id == I::getgrnam_) {
        // The host's answer (on a device the system's own bionic), in the
        // guest's LP32 structs: passwd {name, passwd, uid, gid, dir, shell}
        // = 24 bytes (pw_gecos aliases the NULL pw_passwd), group {name,
        // passwd, gid, mem} = 16 bytes. Like bionic, one result buffer per
        // thread, overwritten by the next call.
        const bool by_name = (id == I::getpwnam_ || id == I::getgrnam_);
        if (by_name && !CheckRange(a0, 1, false)) {
            goto bad_ptr;
        }
        const std::string key = by_name ? std::string(GuestStr(a0)) : std::string();
        std::vector<std::string> strs;  // passwd: name dir shell; group: name, members...
        std::uint32_t id1 = 0, id2 = 0;
        const bool pw = (id == I::getpwuid_ || id == I::getpwnam_);
        if (pw) {
            const struct passwd* p = by_name ? ::getpwnam(key.c_str()) : ::getpwuid(a0);
            if (p) {
                strs = {p->pw_name ? p->pw_name : "", p->pw_dir ? p->pw_dir : "/",
                        p->pw_shell ? p->pw_shell : "/system/bin/sh"};
                id1 = p->pw_uid;
                id2 = p->pw_gid;
            }
        } else {
            const struct group* g = by_name ? ::getgrnam(key.c_str()) : ::getgrgid(a0);
            if (g) {
                strs = {g->gr_name ? g->gr_name : ""};
                for (char** m = g->gr_mem; m && *m && strs.size() < 64; m++) {
                    strs.push_back(*m);
                }
                id1 = g->gr_gid;
            }
        }
        if (strs.empty()) {
            SetGuestErrno(ENOENT);
            Ret(regs, 0);
            return;
        }
        // Layout: struct, member array (group), strings.
        const std::uint32_t head = pw ? 24u : 16u;
        const std::uint32_t members = pw ? 0u : static_cast<std::uint32_t>(strs.size()) * 4u;
        std::uint32_t need = head + members;
        for (const std::string& x : strs) {
            need += static_cast<std::uint32_t>(x.size() + 1);
        }
        thread_local std::uint32_t buf = 0, buf_size = 0;
        if (buf_size < need) {
            if (buf) {
                GuestFree(buf);
            }
            buf = GuestMalloc(need);
            buf_size = buf ? need : 0;
            if (!buf) {
                SetGuestErrno(ENOMEM);
                Ret(regs, 0);
                return;
            }
        }
        std::memset(H(buf), 0, need);
        std::uint32_t at = buf + head + members;
        std::vector<std::uint32_t> ptrs;
        for (const std::string& x : strs) {
            std::memcpy(H(at), x.c_str(), x.size() + 1);
            ptrs.push_back(at);
            at += static_cast<std::uint32_t>(x.size() + 1);
        }
        auto put32 = [&](std::uint32_t off, std::uint32_t v) { std::memcpy(H(buf + off), &v, 4); };
        if (pw) {
            put32(0, ptrs[0]);
            put32(8, id1);
            put32(12, id2);
            put32(16, ptrs[1]);
            put32(20, ptrs[2]);
        } else {
            put32(0, ptrs[0]);
            put32(8, id1);
            put32(12, buf + head);
            // gr_mem: the members, NULL-terminated (the slot of the name
            // string becomes the terminator).
            for (std::size_t i = 1; i < ptrs.size(); i++) {
                put32(head + static_cast<std::uint32_t>(i - 1) * 4u, ptrs[i]);
            }
            put32(head + static_cast<std::uint32_t>(ptrs.size() - 1) * 4u, 0);
        }
        Ret(regs, buf);
        return;
    }
    {
        // Locked: worker threads share stub_hits_ with main. Diagnostic layer
        // (PLAN.md A5/B13): with DIAG off this skips the per-call map probe on
        // An import the harness does not implement: answer 0 (the guest gets
        // a plausible "success") and SAY SO -- exactly what a port needs
        // to see, so this is NOT behind HARNESS_DIAG. The log stays capped
        // at 5 lines per import name: a run adds a few dozen lines
        // (measured: 69 [stub] lines in a DIAG run), not a flood.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint64_t& hits = stub_hits_[n];
        // Enriched logging for environment queries: names matter.
        if (id == I::__system_property_get_ || id == I::dlopen_ || id == I::open_ ||
            id == I::fopen_ || id == I::stat_ || id == I::access_) {
            std::printf("[env]%s %s(\"%s\") lr=%08x\n", GuestTid() == 1 ? "" : "(T2+)", n.c_str(),
                        GuestStr(a0), regs[14]);
        }
        hits++;
        static bool threadlog = std::getenv("HARNESS_THREADLOG") != nullptr;
        if (hits <= 5 && (GuestTid() == 1 || threadlog)) {
            std::printf("[stub]%s %s(%08x %08x lr=%08x) -> 0%s\n", GuestTid() == 1 ? "" : "(T2+)",
                        n.c_str(), a0, a1, regs[14], hits == 5 ? " (quiet henceforth)" : "");
        }
    }
    }  // switch (id)

    Ret(regs, 0);
    return;

    // Wild guest pointer in a bulk-memory thunk: halt cleanly, never segfault.
bad_ptr:
    std::printf("[svc] LOUD BAD guest pointer in %s(%08x %08x %08x %08x) lr=%08x -- halting\n", n.c_str(),
                a0, a1, a2, a3, regs[14]);
    // HARNESS_BADCTX=1: caller context forensics (regs + stack top) at the
    // faulting import. Catches state bugs the JIT probes miss (e.g. calls
    // converging inside a probed function without hitting its entry hook).
    if (std::getenv("HARNESS_BADCTX")) {
        auto& rr = ActiveJit()->Regs();
        std::printf("[badctx] r0-r3=%08x %08x %08x %08x r4-r7=%08x %08x %08x %08x\n", rr[0], rr[1],
                    rr[2], rr[3], rr[4], rr[5], rr[6], rr[7]);
        std::printf("[badctx] r8-r11=%08x %08x %08x %08x sp=%08x lr=%08x\n", rr[8], rr[9], rr[10],
                    rr[11], rr[13], rr[14]);
        for (int o = 0; o < 96; o += 16) {
            if (!CheckRange(rr[13] + o, 16, false)) {
                std::printf("[badctx] sp+%02x: <unmapped>\n", o);
                break;
            }
            const std::uint8_t* p = H<const std::uint8_t>(rr[13] + o);
            std::printf("[badctx] sp+%02x:", o);
            for (int j = 0; j < 16; j++) {
                std::printf(" %02x", p[j]);
            }
            std::printf("  |");
            for (int j = 0; j < 16; j++) {
                std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
            }
            std::printf("|\n");
        }
    }
    std::printf("[svc] mappings:\n");
    mappings_.ForEach([](std::uint32_t st, std::uint32_t en, int pr) {
        std::printf("    [%08x-%08x] prot=%d\n", st, en, pr);
    });
    stop_step_ = true;
    if (Dynarmic::A32::Jit* jit = ActiveJit()) {
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }
}

// Host mutex of the guest's type. NORMAL must stay NORMAL: bionic lets any
// thread unlock a normal mutex (the games do), while a recursive/errorcheck
// host mutex answers EPERM and stays locked. Making every guest mutex
// recursive (as before) also turned a self-relock or trylock of a normal
// mutex into a silent success.
static std::shared_ptr<Runtime::HostMutex> NewHostMutex(int type) {
    return std::make_shared<Runtime::HostMutex>(type);
}

Runtime::HostMutex::HostMutex(int t) : type(t) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, t == 1   ? PTHREAD_MUTEX_RECURSIVE
                                     : t == 2 ? PTHREAD_MUTEX_ERRORCHECK
                                              : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(&m, &attr);
    pthread_mutexattr_destroy(&attr);
}

Runtime::HostMutex::~HostMutex() { pthread_mutex_destroy(&m); }

// The holder book of one guest mutex, kept in its host object: written only
// by the thread that owns the lock (or a foreign unlocker of a normal one),
// so it needs no lock. Read for the stall dump.
void Runtime::NoteLocked(HostMutex& hm) {
    hm.holder.store(GuestTid(), std::memory_order_relaxed);
    hm.depth.fetch_add(1, std::memory_order_relaxed);
}

void Runtime::NoteUnlocking(HostMutex& hm, std::uint32_t obj) {
    const std::uint32_t tid = GuestTid();
    const std::uint32_t holder = hm.holder.load(std::memory_order_relaxed);
    if (holder != tid) {
        // A normal mutex unlocked by another thread (bionic allows it). Name
        // the first few, count the rest (MutexOwnersDump).
        const unsigned long k = mutex_foreign_unlocks_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 5) {
            std::printf("[mutex] UNLOCK-FOREIGN obj=%08x by=%u owner=%u%s\n", obj, tid, holder,
                        k == 5 ? " (further ones only counted)" : "");
        }
    }
    if (hm.depth.fetch_sub(1, std::memory_order_relaxed) <= 1) {
        hm.depth.store(0, std::memory_order_relaxed);
        hm.holder.store(0, std::memory_order_relaxed);
    }
}

std::uint32_t Runtime::PthreadMutex(ImportId id, const std::string& n, std::uint32_t obj, std::uint32_t attr) {
    using I = ImportId;
    // Resolve the host mutex under guard, then operate OUTSIDE it. The old
    // code held mutex_ across pthread_mutex_lock: a thread blocking on a
    // game mutex (held by a halted worker, or ABBA with a guest critical
    // section spanning an SVC) froze every mutex_-touching thread
    // (gdb-proven total freeze, 8 threads in pthread_mutex_lock, 2026-10-10).
    std::shared_ptr<HostMutex> m;
    if (id == I::pthread_mutex_init_ || id == I::pthread_mutex_destroy_) {
        // Writers of the map (their own lock, not the runtime's mutex_: lock
        // and unlock, the hot path, only read it and run in parallel).
        std::unique_lock<std::shared_mutex> map_lock(mutex_map_mu_);
        if (id == I::pthread_mutex_init_) {
            // Type from the attribute word (NULL = NORMAL); bionic also keeps
            // it in the mutex word at bits 14-15 (PTHREAD_*_INITIALIZER).
            std::uint32_t type = 0;
            if (attr != 0 && CheckRange(attr, 4, false)) {
                std::memcpy(&type, H(attr), 4);
                type &= 0xFu;
            }
            if (type > 2) {
                return EINVAL;
            }
            if (CheckRange(obj, 4, true)) {
                const std::uint32_t word = (type & 3u) << 14;
                std::memcpy(H(obj), &word, 4);
            }
            auto old = mutex_map_.find(obj);
            if (old != mutex_map_.end()) {
                // Re-init of an existing guest mutex: a fresh host mutex (a
                // thread still on the old one keeps it alive, shared_ptr).
                std::printf("[mutex] re-init obj=%08x (t%u) lr=%08x\n", obj, GuestTid(),
                            ActiveJit() ? ActiveJit()->Regs()[14] : 0u);
                old->second = NewHostMutex(static_cast<int>(type));
            } else {
                mutex_map_[obj] = NewHostMutex(static_cast<int>(type));
            }
            return 0;
        }
        if (id == I::pthread_mutex_destroy_) {
            // As bionic: a held mutex is refused with EBUSY (trylock); a free
            // one leaves the map. A thread still using it keeps its own
            // reference (shared_ptr), so nothing is freed under it.
            auto it = mutex_map_.find(obj);
            if (it != mutex_map_.end()) {
                m = it->second;
                if (pthread_mutex_trylock(&m->m) != 0) {
                    if (mutex_destroy_skipped_++ < 5) {
                        std::printf("[mutex] DESTROY-HELD obj=%08x holder=%u -- EBUSY\n", obj,
                                    m->holder.load(std::memory_order_relaxed));
                    }
                    return EBUSY;
                }
                pthread_mutex_unlock(&m->m);
                mutex_map_.erase(it);
            }
            return 0;
        }
    }
    {
#ifdef ABR_DIAG_COUNTERS
        const std::uint64_t w0 = DiagTicks();
#endif
        std::shared_lock<std::shared_mutex> map_lock(mutex_map_mu_);
#ifdef ABR_DIAG_COUNTERS
        diag_.rt_wait[0].fetch_add(DiagTicks() - w0, std::memory_order_relaxed);
        diag_.rt_wait_n[0].fetch_add(1, std::memory_order_relaxed);
#endif
        const auto it = mutex_map_.find(obj);
        if (it != mutex_map_.end()) {
            m = it->second;
        }
    }
    if (!m) {
        std::unique_lock<std::shared_mutex> map_lock(mutex_map_mu_);
        auto it = mutex_map_.find(obj);  // another thread may have added it
        if (it == mutex_map_.end()) {
            // Statically initialized guest mutex: the initializer word carries
            // the type in bits 14-15 (NDK pthread.h: NORMAL 0, RECURSIVE 0x4000,
            // ERRORCHECK 0x8000).
            std::uint32_t word = 0;
            if (CheckRange(obj, 4, false)) {
                std::memcpy(&word, H(obj), 4);
            }
            const int type = static_cast<int>((word >> 14) & 3u);
            it = mutex_map_.emplace(obj, NewHostMutex(type == 3 ? 0 : type)).first;
        }
        m = it->second;
    }  // map lock released: the blocking lock/unlock below holds none.
    if (id == I::pthread_mutex_lock_) {
        int r = 0;
        if (SignalsInUse()) {
            // Interruptible (see PthreadCond): handlers run between slices,
            // then the lock attempt goes on (a mutex lock is never EINTR).
            // A free mutex (the common case) is taken at once: the slices and
            // their clock read (a ~390 ns syscall on the sandbox) only start
            // when it is held. trylock answers as lock does for an owner of a
            // recursive one; any other refusal goes on to the full lock.
            r = pthread_mutex_trylock(&m->m);
#ifdef ABR_DIAG_COUNTERS
            const std::uint64_t w0 = DiagTicks();
#endif
            while (r != 0) {
                const struct timespec slice = RealtimeIn(kSigSliceNs);
                r = pthread_mutex_timedlock(&m->m, &slice);
                if (r != ETIMEDOUT) {
                    break;
                }
                if (SignalDeliverable()) {
                    DeliverPendingSignals();
                }
            }
#ifdef ABR_DIAG_COUNTERS
            diag_.guest_lock_wait.fetch_add(DiagTicks() - w0, std::memory_order_relaxed);
#endif
        } else {
#ifdef ABR_DIAG_COUNTERS
            const std::uint64_t w0 = DiagTicks();
            r = pthread_mutex_lock(&m->m);
            diag_.guest_lock_wait.fetch_add(DiagTicks() - w0, std::memory_order_relaxed);
#else
            r = pthread_mutex_lock(&m->m);
#endif
        }
        if (r == 0) {
            NoteLocked(*m);
        }
        return static_cast<std::uint32_t>(r);
    }
    if (id == I::pthread_mutex_unlock_) {
        // The book is updated while the lock is still held (after the unlock
        // another thread may already own it). A recursive/errorcheck mutex
        // unlocked by a non-owner is refused by the host (EPERM): its book
        // stays as it is.
        if (m->type != 0 && m->holder.load(std::memory_order_relaxed) != GuestTid()) {
            return static_cast<std::uint32_t>(pthread_mutex_unlock(&m->m));
        }
        NoteUnlocking(*m, obj);
        return static_cast<std::uint32_t>(pthread_mutex_unlock(&m->m));
    }
    if (id == I::pthread_mutex_trylock_) {
        const int r = pthread_mutex_trylock(&m->m);
        if (r == 0) {
            NoteLocked(*m);
        }
        return static_cast<std::uint32_t>(r);
    }
    return 22;
}

static std::shared_ptr<pthread_rwlock_t> NewHostRwlock() {
    auto* r = new pthread_rwlock_t;
    pthread_rwlock_init(r, nullptr);
    return std::shared_ptr<pthread_rwlock_t>(r, [](pthread_rwlock_t* p) {
        pthread_rwlock_destroy(p);
        delete p;
    });
}

std::uint32_t Runtime::PthreadRwlock(ImportId id, const std::string& n, std::uint32_t obj) {
    using I = ImportId;
    // Same discipline as PthreadMutex: resolve under guard (taking a
    // reference), operate outside. Lazy-creates static initializers
    // (PTHREAD_RWLOCK_INITIALIZER).
    std::shared_ptr<pthread_rwlock_t> rw;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (id == I::pthread_rwlock_init_) {
            rwlock_map_[obj] = NewHostRwlock();
            return 0;
        }
        if (id == I::pthread_rwlock_destroy_) {
            rwlock_map_.erase(obj);  // freed when the last user lets go
            return 0;
        }
        auto it = rwlock_map_.find(obj);
        if (it == rwlock_map_.end()) {
            it = rwlock_map_.emplace(obj, NewHostRwlock()).first;
        }
        rw = it->second;
    }  // guard released: blocking rdlock/wrlock below must not hold mutex_.
    pthread_rwlock_t* r = rw.get();
    if (id == I::pthread_rwlock_rdlock_) {
        return static_cast<std::uint32_t>(pthread_rwlock_rdlock(r));
    }
    if (id == I::pthread_rwlock_wrlock_) {
        return static_cast<std::uint32_t>(pthread_rwlock_wrlock(r));
    }
    if (id == I::pthread_rwlock_unlock_) {
        return static_cast<std::uint32_t>(pthread_rwlock_unlock(r));
    }
    return EINVAL;
}

// Host condition variables always run on CLOCK_MONOTONIC so that a wall-clock
// jump cannot stretch or cut a wait. Guest deadlines come in two clocks:
// pthread_cond_timedwait_monotonic (bionic's legacy API, the one all six
// titles use, e.g. Rio clock_gettime(CLOCK_MONOTONIC) at 0x38f45c then the
// call at 0x38f530) passes a CLOCK_MONOTONIC deadline; plain
// pthread_cond_timedwait passes a CLOCK_REALTIME one, converted below. No
// title imports pthread_condattr_setclock, so cond attributes carry no clock.
static std::shared_ptr<Runtime::HostCond> NewHostCond() {
    auto* c = new Runtime::HostCond;
    pthread_mutex_init(&c->m, nullptr);
    return std::shared_ptr<Runtime::HostCond>(c, [](Runtime::HostCond* p) {
        pthread_mutex_destroy(&p->m);
        delete p;
    });
}

std::uint32_t Runtime::PthreadCond(ImportId id, const std::string& n, std::uint32_t cond, std::uint32_t arg1,
                                   std::uint32_t arg2, bool monotonic) {
    using I = ImportId;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (id == I::pthread_cond_init_ || id == I::pthread_cond_destroy_) {
        if (id == I::pthread_cond_init_) {
            if (cond_map_.count(cond) != 0) {
                std::printf("[cond] re-init obj=%08x (t%u)\n", cond, GuestTid());
            }
            cond_map_[cond] = NewHostCond();
        } else {
            cond_map_.erase(cond);  // freed when the last waiter lets go
        }
        return 0;
    }
    auto it = cond_map_.find(cond);
    if (it == cond_map_.end()) {
        // Lazy creation for statically-initialized condvars
        // (PTHREAD_COND_INITIALIZER is all zeros on bionic).
        it = cond_map_.emplace(cond, NewHostCond()).first;
    }
    // Own references: the cond and mutex stay alive while this thread waits,
    // even if another thread destroys them meanwhile.
    const std::shared_ptr<HostCond> hc = it->second;
    using Waiter = HostCond::Waiter;
    auto unlink = [&](Waiter* w) {
        (w->prev ? w->prev->next : hc->head) = w->next;
        (w->next ? w->next->prev : hc->tail) = w->prev;
        w->next = w->prev = nullptr;
    };
    if (id == I::pthread_cond_signal_ || id == I::pthread_cond_broadcast_) {
        // The oldest sleeper (a broadcast: every sleeper) takes a wake; with
        // nobody asleep the wake is gone, as POSIX says.
        pthread_mutex_lock(&hc->m);
        if (hc->head) {
            hc->last_tid.store(GuestTid(), std::memory_order_relaxed);
        }
        while (Waiter* w = hc->head) {
            unlink(w);
            w->woken = true;
            hc->wakes++;
            pthread_cond_signal(&w->cv);
            if (id == I::pthread_cond_signal_) {
                break;
            }
        }
        pthread_mutex_unlock(&hc->m);
        return 0;
    }
    std::shared_ptr<HostMutex> mx;
    {
        std::shared_lock<std::shared_mutex> map_lock(mutex_map_mu_);
        const auto mit = mutex_map_.find(arg1);
        if (mit == mutex_map_.end()) {
            return EINVAL;
        }
        mx = mit->second;
    }
    // The guest mutex back, interruptibly while guest signals are in use
    // (as PthreadMutex locks it).
    auto relock = [&] {
        if (!SignalsInUse()) {
            pthread_mutex_lock(&mx->m);
            return;
        }
        for (;;) {
            const struct timespec slice = RealtimeIn(kSigSliceNs);
            if (pthread_mutex_timedlock(&mx->m, &slice) != ETIMEDOUT) {
                return;
            }
            if (SignalDeliverable()) {
                DeliverPendingSignals();
            }
        }
    };
    // The wait: queued under the cond's own lock BEFORE the guest mutex is
    // let go, so a wake that follows the caller's unlock reaches it. While
    // guest signals are in use it sleeps in short slices; a pending signal
    // runs its handler and the wait returns 0, as bionic's does when the
    // futex wait is interrupted (EINTR -> 0), and the caller's predicate loop
    // looks again. A sleeper leaving without a wake (timeout, handler) leaves
    // the queue; a wake that came first is taken instead.
    auto wait = [&](const struct timespec* deadline_mono) -> int {
        Waiter me;
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&me.cv, &attr);
        pthread_condattr_destroy(&attr);
        pthread_mutex_lock(&hc->m);
        me.prev = hc->tail;
        (hc->tail ? hc->tail->next : hc->head) = &me;
        hc->tail = &me;
        if (t_sig) {
            t_sig->wait_wakes.store(hc->wakes, std::memory_order_relaxed);
            t_sig->wait_cv.store(hc.get());
        }
        mutex_.unlock();
        // The guest mutex is let go for the wait: its holder book too (as an
        // unlock by its owner would), restored after the relock below.
        const std::int32_t held_depth = mx->depth.exchange(0, std::memory_order_relaxed);
        mx->holder.store(0, std::memory_order_relaxed);
        pthread_mutex_unlock(&mx->m);
        int r = 0;
        while (!me.woken) {
            struct timespec now {};
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (deadline_mono && !Before(now, *deadline_mono)) {
                r = ETIMEDOUT;
                break;
            }
            const struct timespec* until = deadline_mono;
            struct timespec slice {};
            if (SignalsInUse()) {
                slice = now;
                slice.tv_nsec += kSigSliceNs;
                if (slice.tv_nsec >= 1000000000L) {
                    slice.tv_nsec -= 1000000000L;
                    slice.tv_sec++;
                }
                if (!deadline_mono || Before(slice, *deadline_mono)) {
                    until = &slice;
                }
            }
            if (until) {
                pthread_cond_timedwait(&me.cv, &hc->m, until);
            } else {
                pthread_cond_wait(&me.cv, &hc->m);
            }
            if (!me.woken && SignalDeliverable()) {
                r = -1;  // a handler to run first
                break;
            }
        }
        if (me.woken) {
            r = 0;
        } else {
            unlink(&me);
        }
        if (t_sig) {
            t_sig->wait_cv.store(nullptr);
        }
        pthread_mutex_unlock(&hc->m);
        pthread_cond_destroy(&me.cv);
        if (r < 0) {
            DeliverPendingSignals();
            r = 0;
        }
        relock();
        mx->holder.store(GuestTid(), std::memory_order_relaxed);
        mx->depth.store(held_depth > 0 ? held_depth : 1, std::memory_order_relaxed);
        mutex_.lock();
        return r;
    };
    if (id == I::pthread_cond_wait_) {
        // Untimed wait: block until signalled, as bionic does.
        return static_cast<std::uint32_t>(wait(nullptr));
    }
    if (id == I::pthread_cond_timedwait_) {
        // Guest struct timespec is {long tv_sec; long tv_nsec;}, 2 x 32 bits.
        if (!CheckRange(arg2, 8, false)) {
            return EINVAL;
        }
        std::int32_t gsec = 0, gnsec = 0;
        std::memcpy(&gsec, H(arg2), 4);
        std::memcpy(&gnsec, H(arg2 + 4), 4);
        if (gnsec < 0 || gnsec >= 1000000000) {
            return EINVAL;
        }
        struct timespec ts {};
        ts.tv_sec = static_cast<time_t>(static_cast<std::uint32_t>(gsec));
        ts.tv_nsec = gnsec;
        if (!monotonic) {
            // Convert the CLOCK_REALTIME deadline to CLOCK_MONOTONIC.
            struct timespec now_rt {}, now_mono {};
            clock_gettime(CLOCK_REALTIME, &now_rt);
            clock_gettime(CLOCK_MONOTONIC, &now_mono);
            std::int64_t left_ns = (static_cast<std::int64_t>(ts.tv_sec) - now_rt.tv_sec) *
                                       1000000000LL +
                                   (ts.tv_nsec - now_rt.tv_nsec);
            if (left_ns < 0) {
                left_ns = 0;
            }
            const std::int64_t mono_ns =
                static_cast<std::int64_t>(now_mono.tv_sec) * 1000000000LL + now_mono.tv_nsec +
                left_ns;
            ts.tv_sec = static_cast<time_t>(mono_ns / 1000000000LL);
            ts.tv_nsec = static_cast<long>(mono_ns % 1000000000LL);
        }
        return static_cast<std::uint32_t>(wait(&ts));
    }
    return EINVAL;
}

std::uint32_t Runtime::PthreadKeyCreate(std::uint32_t key_ptr, std::uint32_t destr) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!CheckRange(key_ptr, 4, true)) {
        return EINVAL;  // a wild key pointer must not become a host write
    }
    if (next_key_ >= kTlsKeys) {
        return EAGAIN;  // as bionic answers when its keys run out
    }
    const std::uint32_t key = next_key_++;
    key_map_[key] = destr;
    std::memcpy(H(key_ptr), &key, 4);
    return 0;
}

// pthread_key_delete: the key and every thread's value go; no destructor
// runs (POSIX).
std::uint32_t Runtime::PthreadKeyDelete(std::uint32_t key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (key_map_.erase(key) == 0) {
        return EINVAL;
    }
    for (auto& kv : tls_blocks_) {
        __atomic_store_n(H<std::uint32_t>(kv.second + key * 4), 0u, __ATOMIC_RELAXED);
    }
    return 0;
}

void Runtime::RunKeyDestructors() {
    const std::uint32_t tls = TlsFor(GuestTid());
    for (int round = 0; round < 4; round++) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;  // destructor, value
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            for (const auto& kd : key_map_) {
                if (kd.second == 0 || kd.first >= kTlsKeys) {
                    continue;
                }
                const std::uint32_t v =
                    __atomic_exchange_n(H<std::uint32_t>(tls + kd.first * 4), 0u, __ATOMIC_RELAXED);
                if (v != 0) {
                    calls.emplace_back(kd.second, v);
                }
            }
        }
        if (calls.empty()) {
            return;
        }
        for (const auto& c : calls) {
            std::int32_t ignored = 0;
            if (!RunComparator(c.first, c.second, 0, ignored)) {
                std::printf("[thread] LOUD key destructor %08x(%08x) failed (hr=%d) -- the "
                            "thread's other destructors are skipped\n",
                            c.first, c.second, cb_last_hr_);
                return;
            }
        }
    }
}

// The calling thread's block is looked up once per guest thread identity
// (a proxy changes it while it stands in) and again after a thread ended: its
// block may serve another thread by then.
std::uint32_t Runtime::TlsFor(std::uint32_t tid) {
    thread_local const Runtime* cached_rt = nullptr;
    thread_local std::uint32_t cached_tid = 0;
    thread_local std::uint32_t cached_reaps = 0;
    thread_local std::uint32_t cached_va = 0;
    if (cached_rt == this && cached_tid == tid &&
        cached_reaps == tls_reaps_.load(std::memory_order_acquire)) {
        return cached_va;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::uint32_t va = 0;
    const auto it = tls_blocks_.find(tid);
    if (it != tls_blocks_.end()) {
        va = it->second;
    } else if (!tls_free_.empty()) {
        va = tls_free_.back();
        tls_free_.pop_back();
        std::memset(H(va), 0, kTlsBlockSize);
    } else if (next_tls_block_ < kTlsBlocks - 1) {
        va = kTlsBase + next_tls_block_++ * kTlsBlockSize;
    } else {
        // Far beyond what the guest heap can give stacks to. No thread may read
        // another's values: this one stops, the last block catches what it
        // still does on the way out.
        std::printf("[thread] LOUD out of TLS blocks at tid %u -- halting\n", tid);
        stop_step_ = true;
        if (Dynarmic::A32::Jit* jit = ActiveJit()) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
        return kTlsBase + (kTlsBlocks - 1) * kTlsBlockSize;
    }
    tls_blocks_[tid] = va;
    cached_rt = this;
    cached_tid = tid;
    cached_reaps = tls_reaps_.load(std::memory_order_relaxed);
    cached_va = va;
    return va;
}

// Only for a call from guest code, where ActiveJit is the Jit running the
// caller. A Jit holds its current thread's block or 0: wherever a Jit may
// change threads, the block goes back to 0 (ThreadEnter, RunNested) and the
// next pthread_getspecific comes here through the thunk's slow path.
void Runtime::BindTls() {
    static const bool on = TlsThunkOn();
    Dynarmic::A32::Jit* jit = ActiveJit();
    if (on && jit != nullptr) {
        jit->SetTpidruro(TlsFor(GuestTid()));
    }
}

std::uint32_t Runtime::PthreadGetSpecific(std::uint32_t key) {
    // bionic returns NULL for a key it does not know.
    return key < kTlsKeys ? __atomic_load_n(H<std::uint32_t>(TlsFor(GuestTid()) + key * 4),
                                            __ATOMIC_RELAXED)
                          : 0;
}

std::uint32_t Runtime::PthreadSetSpecific(std::uint32_t key, std::uint32_t value) {
    if (key >= kTlsKeys) {
        return EINVAL;
    }
    __atomic_store_n(H<std::uint32_t>(TlsFor(GuestTid()) + key * 4), value, __ATOMIC_RELAXED);
    return 0;
}

// memcpy too: a copy with overlapping ranges works, as on bionic.
void Runtime::GuestMemCopy(ImportId id, Regs& regs, const std::string& n, std::uint32_t dst, std::uint32_t src,
                           std::uint32_t len) {
    using I = ImportId;
    const std::uint32_t wlen = ClampHeapWrite(dst, len, n.c_str());
    std::memmove(H(dst), H<const void>(src), wlen);
    ScanWP(this, dst, wlen, n.c_str());
    Ret(regs, dst);
}

void Runtime::GuestMemSet(ImportId id, Regs& regs, const std::string& n, std::uint32_t dst, int fill,
                          std::uint32_t len) {
    using I = ImportId;
    // TEMP-DIAG (wall4 second filler): log non-clr memset traffic.
    // Diagnostic layer (PLAN.md A4): off in shipping.
    if (DiagOn()) {
        static int mcount = 0;
        if (n.compare(0, 14, "__aeabi_memclr") != 0 && mcount < 400) {
            mcount++;
            std::uint32_t lr = ActiveJit() ? ActiveJit()->Regs()[14] : 0;
            std::printf("[memset] %s dst=%08x len=%u fill=%02x lr=%08x\n", n.c_str(), dst, len,
                        fill & 0xFF, lr);
        }
    }
    const std::uint32_t wlen = ClampHeapWrite(dst, len, n.c_str());
    std::memset(H(dst), fill, wlen);
    ScanWP(this, dst, wlen, n.c_str());
    Ret(regs, dst);
}

bool Runtime::PthreadOnce(std::uint32_t control, std::uint32_t routine,
                          std::array<std::uint32_t, 16>& regs) {
    // unique_lock (not lock_guard): the state==1 case below has to release the
    // dispatcher lock while it waits.
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (!CheckRange(control, 4, false)) {
        std::printf("[once] BAD control %08x -- halting\n", control);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return false;
    }
    // Same control already held by THIS thread => pathological recursion (break
    // it). This must be decided before the wait below: waiting on our own init
    // would never end. A DIFFERENT control while one is in progress is
    // legitimate nesting: the active init routine is initialising a second
    // once-guard. Space does this in its init array (control 0x73f0d8 nested
    // inside 0x73f0cc).
    for (int i = 0; i < once_depth_; i++) {
        if (once_stack_[i].control == control) {
            std::printf("[once] LOUD recursive once (control=%08x routine=%08x) -- treating as done\n",
                        control, routine);
            return false;
        }
    }
    std::uint32_t state = __atomic_load_n(H<std::uint32_t>(control), __ATOMIC_ACQUIRE);
    if (state == 1) {
        // IN PROGRESS on another thread. bionic blocks the caller here until the
        // control reads DONE; returning 0 at once hands out a half-initialised
        // object. That was measured, not guessed: the caller went straight into
        // the singleton and aborted (guest pc 0x5cf449, its entropy code, once
        // per 10 launches) -- see games/space/JOURNAL.md.
        //
        // The dispatcher lock is released while waiting: the owner is running
        // guest code and calls back into us (open/fcntl/read/pthread_key_create),
        // so holding it would deadlock against the very init we await. Same
        // idiom as the guest pthread_cond_wait path.
        if (DiagOn()) {
            // NB: the tid below is the CALLER (the one that blocks), not the
            // thread that owns the init. Ownership is thread-local, so the
            // waiter cannot name the owner -- do not read this as the owner.
            std::printf("[once] wait: control=%08x in progress on another thread -- caller tid=%u "
                        "blocks here\n",
                        control, GuestTid());
        }
        // int64: a stuck wait may run long, and signed overflow here would be
        // undefined behaviour for no reason at all.
        std::int64_t waited_us = 0;
        int logged_s = 0;
        lock.unlock();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            lock.lock();
            state = __atomic_load_n(H<std::uint32_t>(control), __ATOMIC_ACQUIRE);
            if (state != 1) {
                break;
            }
            lock.unlock();
            waited_us += 500;
            if (waited_us >= (static_cast<std::int64_t>(logged_s) + 1) * 1000000) {
                logged_s++;
                // Not normal: an init routine alive for a whole second means the
                // owner is stuck (guest deadlock, or a JIT halt inside it). No
                // timeout and no bail-out -- bionic would block too -- but a
                // hang must be diagnosable.
                std::printf("[once] LOUD still waiting %d s for control=%08x (tid=%u) -- the "
                            "thread that started this init has not finished it\n",
                            logged_s, control, GuestTid());
            }
        }
        if (DiagOn()) {
            std::printf("[once] wait over: control=%08x state=%u -- caller tid=%u resumes\n", control,
                        state, GuestTid());
        }
        if (state != 0) {
            return false;  // the init completed while we waited: normal return 0
        }
        // state == 0: released without completing -- the owner thread died (see
        // ReleaseOnceControlsOnExit). Fall through and run the init ourselves,
        // exactly as a fresh caller would.
    } else if (state != 0) {
        return false;  // already initialised (2): normal return 0
    }
    if (once_depth_ >= kOnceMaxDepth) {
        std::printf("[once] LOUD once depth %d exceeded (control=%08x routine=%08x) -- treating as done\n",
                    once_depth_, control, routine);
        return false;
    }
    if (!IsCodeAddress(routine & ~std::uint32_t(1))) {
        std::printf("[once] LOUD bad routine %08x -- halting\n", routine);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return false;
    }
    const std::uint32_t owner = GuestTid();
    if (DiagOn()) {
        std::printf("[once] hijack: control=%08x routine=%08x depth=%d tid=%u\n", control, routine,
                    once_depth_, owner);
    }
    OnceHijack& slot = once_stack_[once_depth_++];
    slot.control = control;
    slot.owner_tid = owner;
    slot.saved_regs = regs;
    const std::uint32_t one = 1;  // in-progress marker
    std::memcpy(H(control), &one, 4);
    // Redirect: run routine with HALT trampoline as its return address.
    // Force USER mode too (callback context is SVC; the routine is guest code).
    if (routine & 1u) {
        std::uint32_t cpsr = ActiveJit()->Cpsr();
        ActiveJit()->SetCpsr((cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10) | std::uint32_t(0x20));
        regs[15] = routine & ~std::uint32_t(1);
    } else {
        std::uint32_t cpsr = ActiveJit()->Cpsr();
        ActiveJit()->SetCpsr((cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10));
        regs[15] = routine;
    }
    regs[14] = mod_.once_va;
    return true;
}

// A guest thread can stop while it still owns pthread_once controls that read
// "in progress" (state == 1): a fatal guest error halts its JIT inside the init
// routine, or its start routine returns from inside it. Any other thread asking
// for such a control would then wait in PthreadOnce forever. Reset the control
// so a later call runs the init again -- the init never completed, so re-running
// it is the honest recovery (bionic has no such case: its init cannot be killed
// underneath itself). Runs on the dying thread, over its thread_local stack.
void Runtime::ReleaseOnceControlsOnExit(std::uint32_t tid) {
    if (once_depth_ <= 0) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (int i = 0; i < once_depth_; i++) {
        const std::uint32_t control = once_stack_[i].control;
        std::uint32_t cur = 0;
        if (!CheckRange(control, 4, false)) {
            continue;
        }
        std::memcpy(&cur, H(control), 4);
        if (cur == 1) {
            const std::uint32_t zero = 0;
            std::memcpy(H(control), &zero, 4);
        }
        std::printf("[once] LOUD tid=%u stopped inside an init routine (control=%08x, state was %u)"
                    " -- control released; a later call re-runs that init\n",
                    tid, control, cur);
    }
    once_depth_ = 0;
}

void Runtime::ExidxFor(std::uint32_t pc, std::uint32_t* base, std::uint32_t* count) const {
    // The library whose mapped range holds pc (one library: its own table).
    *base = mod_.exidx_va;
    *count = mod_.exidx_count;
    for (const auto& x : mod_.more) {
        if (pc >= x.span_lo && pc < x.span_hi) {
            *base = x.exidx_va;
            *count = x.exidx_count;
            return;
        }
    }
}

std::uint32_t Runtime::FindExidx(std::uint32_t pc) {
    std::uint32_t base = 0, count = 0;
    ExidxFor(pc & ~std::uint32_t(1), &base, &count);
    if (base == 0 || count == 0) {
        return 0;
    }
    pc &= ~std::uint32_t(1);  // exidx function addresses exclude the Thumb bit
    // Table sorted by function address ascending. prel31: bit30 is the sign
    // (verified: exact fn matches for 0x2F91B0/0x45A758/0xC1550, 0 violations).
    auto fn_of = [&](std::uint32_t idx) -> std::uint32_t {
        std::uint32_t w = 0;
        std::memcpy(&w, H(base + idx * 8), 4);
        std::int32_t off;
        if (w & 0x40000000u) {
            off = -static_cast<std::int32_t>(0x40000000u - (w & 0x3FFFFFFFu));
        } else {
            off = static_cast<std::int32_t>(w & 0x3FFFFFFFu);
        }
        return (base + idx * 8) + static_cast<std::uint32_t>(off);
    };
    // Bounds-check the whole table first (host-side bulk access discipline).
    if (!CheckRange(base, count * 8, false)) {
        return 0;
    }
    std::uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (fn_of(mid) <= pc) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return 0;  // pc below the first entry
    }
    return base + (lo - 1) * 8;
}

void Runtime::InitDataImports() {
    // NOTE: no mutex here — GuestMalloc locks internally (single-threaded bring-up).
    auto find_import = [&](const char* name) -> std::uint32_t {
        for (std::uint32_t i = 0; i < mod_.imports.size(); i++) {
            if (mod_.imports[i].name == name) {
                return i;
            }
        }
        return 0xFFFFFFFFu;
    };
    // Bionic ctype classes (hex 01..80): U L N S P C X B (X=0x40, B=0x80).
    std::uint8_t cls[256] = {};
    for (int c = 0; c < 256; c++) {
        cls[c] = BionicCtypeClass(c);
    }
    // Block: [257B ctype (EOF slot + 256)] + pad + [257 x u16 lower] + [257 x u16 upper].
    // 2014-era Bionic layout (the game was built against it): short tables
    // with index shift — game reads tab[c+1] as u16 (EOF at [0]). Our old
    // u32 fill read back zeros through the game's u16 lens, breaking
    // toupper-dependent paths (ext lookup "pvr"->"PVR" failed -> NULL ->
    // "Unsupported image file format" IOException). Verified vs NDK ctype.h
    // (modern Bionic inlines everything; only _ctype_ remains exported).
    const std::uint32_t total = 260 + 516 + 516;
    const std::uint32_t blk = GuestMalloc(total);
    if (!blk) {
        std::printf("[data] LOUD ctype block alloc failed -- halting\n");
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    if (!CheckRange(blk, total, true)) {
        std::printf("[data] LOUD ctype block out of range -- halting\n");
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    std::uint8_t* p = H<std::uint8_t>(blk);
    std::memset(p, 0, total);
    p[0] = 32;  // EOF slot [-1]: control class
    for (int c = 0; c < 256; c++) {
        p[1 + c] = cls[c];
    }
    // NOTE: slots below need blk (table base, EOF at [0]); the guest adds +1.
    const std::uint32_t lower_va = (blk + 260 + 3) & ~std::uint32_t(3);
    const std::uint32_t upper_va = lower_va + 516;
    // Bionic C-locale short tables: [0] = EOF mapping (-1), [c+1] = case(c).
    {
        std::uint16_t eof = 0xFFFFu;
        std::memcpy(H(lower_va), &eof, 2);
        std::memcpy(H(upper_va), &eof, 2);
    }
    for (int c = 0; c < 256; c++) {
        std::uint16_t lo16 =
            (std::uint16_t)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
        std::uint16_t up16 =
            (std::uint16_t)((c >= 'a' && c <= 'z') ? (c - 32) : c);
        std::memcpy(H(lower_va + 2 + c * 2), &lo16, 2);
        std::memcpy(H(upper_va + 2 + c * 2), &up16, 2);
    }
    struct Slot {
        const char* name;
        std::uint32_t addr;
    };
    // POINTER CELLS (proven by the 0x45C174 double-deref): GLOB_DAT slots must
    // hold &variable (standard), NOT the value. The guest does slot->[var]->use,
    // so each data import gets a 4B guest variable cell holding the real value.
    // _ctype_ var = table base (guest adds +1 for the EOF [-1] slot itself).
    // guard var = the harness canary (guestdata+0, written before this call).
    std::printf("[data] ctype blk=%08x lower=%08x upper=%08x\n", blk, lower_va, upper_va);
    const std::uint32_t var_ctype = GuestMalloc(4);
    const std::uint32_t var_guard = GuestMalloc(4);
    const std::uint32_t var_lower = GuestMalloc(4);
    const std::uint32_t var_upper = GuestMalloc(4);
    const std::uint32_t var_page = GuestMalloc(4);
    // bionic std streams: __sF[3], sizeof(FILE)=84 on this arm32 ABI (stderr is
    // &__sF[2] == base+168; verified at call sites 0x5d4270 and 0x586dc8).
    const std::uint32_t sF = GuestMalloc(3 * 84);
    // environ (char**): the variable points at the guest's environment
    // (env_, see GuestSetenv), empty unless the host seeds it. std::nothrow
    // is an empty object only used by address (operator new(size, nothrow)).
    const std::uint32_t var_environ = GuestMalloc(4);
    const std::uint32_t env_array = GuestMalloc(4);
    const std::uint32_t var_nothrow = GuestMalloc(4);
    if (!var_ctype || !var_guard || !var_lower || !var_upper || !var_page || !sF ||
        !var_environ || !env_array || !var_nothrow) {
        std::printf("[data] LOUD var alloc failed -- halting\n");
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    std::uint32_t canary = 0xA5A5A5A5u;
    std::memcpy(&canary, H(kGuestDataBase + 0), 4);
    if (canary == 0) {
        canary = 0xA5A5A5A5u;
    }
    std::memcpy(H(var_ctype), &blk, 4);
    std::memcpy(H(var_guard), &canary, 4);
    // tolower/toupper vars hold the TABLE address; slots hold &var (the game
    // double-derefs: ldr r2,[slot]; ldr r4,[r2] at 0x387E64/6C — PROVEN live:
    // single-level patch fed [table]=0xFFFF as base -> "__1" key garbage).
    std::memcpy(H(var_lower), &lower_va, 4);
    std::memcpy(H(var_upper), &upper_va, 4);
    // __page_size is a bionic object (GLOB_DAT), not a function. Breakpad's
    // LSS reads it for page rounding; bionic's linker sets it to the real
    // page size (4096 on arm32 Android).
    const std::uint32_t page_size = 4096;
    std::memcpy(H(var_page), &page_size, 4);
    std::memset(H(env_array), 0, 4);
    std::memcpy(H(var_environ), &env_array, 4);
    {
        std::lock_guard<std::mutex> l(env_mu_);
        environ_var_ = var_environ;
        environ_array_ = env_array;
        RebuildEnvironLocked();  // entries the host set before this point
    }
    std::memset(H(var_nothrow), 0, 4);
    // __sF: bind the three bionic std streams to the host's. The GOT slot must
    // hold the ARRAY BASE (the guest does a single deref: ldr rX,[got]; add
    // rX,rX,#168), unlike the pointer-cell variables above.
    std::memset(H(sF), 0, 3 * 84);
    {
        FILE* streams[3] = {stdin, stdout, stderr};
        for (int i = 0; i < 3; i++) {
            const std::uint32_t a = sF + static_cast<std::uint32_t>(i) * 84;
            const std::uint16_t fd16 = static_cast<std::uint16_t>(i);  // _file @ +14
            std::memcpy(H(a + 14), &fd16, 2);
            std::lock_guard<std::mutex> io(io_mutex_);
            file_map_[a] = HostFile{streams[i]};
            fd_map_[static_cast<std::uint32_t>(i)] = fileno(streams[i]);
        }
    }
    const Slot slots[] = {
        {"_ctype_", var_ctype},
        {"__stack_chk_guard", var_guard},
        {"_tolower_tab_", var_lower},
        {"_toupper_tab_", var_upper},
        {"__page_size", var_page},
        {"__sF", sF},
        {"environ", var_environ},
        {"_ZSt7nothrow", var_nothrow},
    };
    // Bind a data import to a guest address: patch every GOT slot that
    // refers to it and point the import itself at the address (a data
    // import has no call target -- its "trampoline" is a bogus address the
    // guest must never dereference).
    auto bind_data_import = [&](std::uint32_t idx, std::uint32_t addr) -> bool {
        for (const PendingData& pd : mod_.pending_data) {
            if (pd.import_idx != idx) {
                continue;
            }
            if (!CheckRange(pd.loc_va, 4, true)) {
                std::printf("[data] LOUD import %u loc %08x out of range -- halting\n", idx,
                            pd.loc_va);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return false;
            }
        }
        return ResolveDataImport(mem_, mod_, idx, addr);
    };
    std::vector<std::uint32_t> covered;
    for (const Slot& s : slots) {
        const std::uint32_t idx = find_import(s.name);
        if (idx == 0xFFFFFFFFu) {
            continue;
        }
        int patched = 0;
        for (const PendingData& pd : mod_.pending_data) {
            if (pd.import_idx != idx) {
                continue;
            }
            if (!CheckRange(pd.loc_va, 4, true)) {
                std::printf("[data] LOUD %s loc %08x out of range -- halting\n", s.name, pd.loc_va);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            const std::uint32_t v = pd.addend + s.addr;
            std::memcpy(H(pd.loc_va), &v, 4);
            patched++;
        }
        std::printf("[data] %s -> %08x (%d slots)\n", s.name, s.addr, patched);
        covered.push_back(idx);
    }
    // OpenSL ES interface IDs (SL_IID_<X>) imported directly: the variables
    // the OpenSL bridge keeps (opensl.cpp).
    {
        std::vector<std::uint32_t> sl;
        for (std::uint32_t i = 0; i < mod_.imports.size(); i++) {
            if (!mod_.imports[i].is_function && mod_.imports[i].name.compare(0, 7, "SL_IID_") == 0) {
                sl.push_back(i);
            }
        }
        for (const std::uint32_t i : sl) {
            const std::string name = mod_.imports[i].name;
            const std::uint32_t var = OpenSlIid(name);
            if (var && bind_data_import(i, var)) {
                std::printf("[data] %s -> %08x (OpenSL ES)\n", name.c_str(), var);
                covered.push_back(i);
            }
        }
    }
    // Coverage guard: a data import that is NOT one of the slots above would
    // keep the address of its TRAMPOLINE (a code address in the trampoline
    // page) and the guest would read a pointer into code as data. The
    // guests' data imports are all covered today (measured), so this
    // never fires -- but a new .so or a new bionic symbol would otherwise
    // fail silently. Bind such an import to a zero page instead, and say
    // so loudly.
    for (std::uint32_t i = 0; i < mod_.imports.size(); i++) {
        if (mod_.imports[i].is_function) {
            continue;
        }
        if (std::find(covered.begin(), covered.end(), i) != covered.end()) {
            continue;
        }
        const std::uint32_t page = GuestMalloc(4096);
        if (!page || !bind_data_import(i, page)) {
            std::printf("[data] LOUD uncovered data import %s -- cannot bind\n",
                        mod_.imports[i].name.c_str());
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        std::printf("[data] LOUD uncovered data import \"%s\" -> zero page %08x "
                    "(the guest expected a real libc object)\n",
                    mod_.imports[i].name.c_str(), page);
    }
}

// ---- ARM32 kuser helper page ----
// The kernel exposes four helpers at fixed VAs in the high vector page. Old
// bionic/libgcc code branches to them directly (Seasons' static GNU libstdc++
// does this for __kuser_memory_barrier). We cannot execute the kernel's bytes:
// map the page ourselves, put one `svc` at each entry, and emulate the helper
// in the host. Every other word is the same trap so a bogus entry cannot run
// zeroes as `andeq r0,r0,r0` and walk off into garbage.
void Runtime::InitUserHelpers() {
    if (!mem_.ok() ||
        !mem_.Map(kUserHelperBase, kUserHelperSize, PROT_READ | PROT_WRITE)) {
        std::fprintf(stderr, "[kuser] LOUD map %08x failed\n", kUserHelperBase);
        std::abort();
    }
    auto* p = reinterpret_cast<std::uint32_t*>(mem_.host(kUserHelperBase));
    const std::uint32_t trap = 0xEF000000u | (kSwiKuserBad & 0x00FFFFFFu);
    for (std::uint32_t i = 0; i < kUserHelperSize / 4; i++) {
        p[i] = trap;
    }
    auto put_svc = [&](std::uint32_t va, std::uint32_t swi) {
        p[(va - kUserHelperBase) / 4] = 0xEF000000u | (swi & 0x00FFFFFFu);
    };
    put_svc(kUserCmpxchg64, kSwiKuserCmpxchg64);
    put_svc(kUserBarrier, kSwiKuserBarrier);
    put_svc(kUserCmpxchg, kSwiKuserCmpxchg);
    put_svc(kUserGetTls, kSwiKuserGetTls);
    p[(kUserTlsSlot - kUserHelperBase) / 4] = 0;                  // software TLS value
    p[(kUserVersion - kUserHelperBase) / 4] = kUserVersionValue;  // 5 helpers

    if (!mem_.Protect(kUserHelperBase, kUserHelperSize, PROT_READ | PROT_EXEC)) {
        std::fprintf(stderr, "[kuser] LOUD protect %08x failed\n", kUserHelperBase);
        std::abort();
    }
    AddMapping(kUserHelperBase, kUserHelperSize, PROT_READ | PROT_EXEC);
    std::printf("[kuser] page %08x mapped RX (barrier/cmpxchg/cmpxchg64/get_tls)\n",
                kUserHelperBase);
}

void Runtime::KuserSetFlags(bool ok) {
    std::uint32_t cpsr = ActiveJit()->Cpsr();
    cpsr &= ~std::uint32_t(0xF0000000u);           // N Z C V
    cpsr |= ok ? 0x60000000u : 0x80000000u;        // success: Z|C; failure: N
    ActiveJit()->SetCpsr(cpsr);
}

void Runtime::OnKuser(std::uint32_t idx, Regs& regs) {
    const std::uint64_t nth = kuser_hits_[idx % 5].fetch_add(1, std::memory_order_relaxed);
    if (nth == 0 && DiagOn()) {
        std::printf("[kuser] first idx=%u r0=%08x r1=%08x r2=%08x lr=%08x tid=%u\n", idx,
                    regs[0], regs[1], regs[2], regs[14], GuestTid());
    }
    switch (idx) {
    case 0:  // __kuser_memory_barrier: no register/flag changes
        std::atomic_thread_fence(std::memory_order_seq_cst);
        Ret(regs, regs[0], regs[1]);
        return;
    case 1: {  // __kuser_cmpxchg: r0=old, r1=new, r2=ptr; r0=0/-1, C=success
        const std::uint32_t p = regs[2];
        if ((p & 3u) != 0 || !CheckRange(p, 4, true)) {
            std::printf("[kuser] LOUD cmpxchg bad ptr=%08x pc=%08x lr=%08x -- halting\n", p,
                        regs[15] - 4, regs[14]);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        std::uint32_t expected = regs[0];
        const std::uint32_t desired = regs[1];
        const bool ok = __atomic_compare_exchange_n(
            reinterpret_cast<std::uint32_t*>(H(p)), &expected, desired, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        KuserSetFlags(ok);
        Ret(regs, ok ? 0u : 0xFFFFFFFFu, regs[1]);
        return;
    }
    case 2: {  // __kuser_cmpxchg64: r0=&old, r1=&new, r2=ptr
        const std::uint32_t oldp = regs[0], newp = regs[1], p = regs[2];
        if ((p & 7u) != 0 || !CheckRange(oldp, 8, false) || !CheckRange(newp, 8, false) ||
            !CheckRange(p, 8, true)) {
            std::printf("[kuser] LOUD cmpxchg64 bad ptr=%08x old=%08x new=%08x pc=%08x -- halting\n",
                        p, oldp, newp, regs[15] - 4);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        std::uint64_t oldv = 0, newv = 0;
        std::memcpy(&oldv, H(oldp), 8);
        std::memcpy(&newv, H(newp), 8);
        const bool ok = __atomic_compare_exchange_n(
            reinterpret_cast<std::uint64_t*>(H(p)), &oldv, newv, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        KuserSetFlags(ok);
        Ret(regs, ok ? 0u : 0xFFFFFFFFu, regs[1]);
        return;
    }
    case 3: {  // __kuser_get_tls: kernel returns the software TLS word (0 here)
        std::uint32_t tls = 0;
        std::memcpy(&tls, H(kUserTlsSlot), 4);
        Ret(regs, tls, regs[1]);
        return;
    }
    default:  // 0xffff0f80 or any non-entry word in the page
        std::printf("[kuser] LOUD unimplemented entry idx=%u pc=%08x lr=%08x -- halting\n", idx,
                    regs[15] - 4, regs[14]);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
}

// Raw ARM EABI syscall from guest code (`svc #0`, number in r7). The bundled
// breakpad LSS does not go through libc imports, so only the calls reached on
// the startup path are emulated; anything else is a LOUD halt, never a fake 0.
// ARM error return convention: r0 = -errno (guest errno is set by LSS itself).
void Runtime::OnGuestSyscall(Regs& regs) {
    const std::uint32_t nr = regs[7];
    const std::uint32_t r0 = regs[0], r1 = regs[1];
    if (DiagOn()) {
        const std::uint64_t bit = nr < 63 ? (std::uint64_t(1) << nr) : (std::uint64_t(1) << 63);
        if ((syscall_seen_.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            std::printf("[syscall] first nr=%u r0=%08x r1=%08x r2=%08x lr=%08x tid=%u\n", nr, r0,
                        r1, regs[2], regs[14], GuestTid());
        }
    }
    switch (nr) {
    case 3:    // read(fd, buf, count)
    case 4: {  // write(fd, buf, count)
        int hfd = -1;
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = fd_map_.find(r0);
            if (it != fd_map_.end()) {
                hfd = it->second;
            }
        }
        if (hfd < 0) {
            regs[0] = static_cast<std::uint32_t>(-EBADF);
            return;
        }
        const bool is_write = (nr == 4);
        if (!CheckRange(r1, regs[2], !is_write)) {
            regs[0] = static_cast<std::uint32_t>(-EFAULT);
            return;
        }
        const std::shared_ptr<abvfs::Slice> sl = is_write ? nullptr : FdSlice(r0);
        const ssize_t n = is_write ? ::write(hfd, H(r1), regs[2])
                          : sl     ? abvfs::SliceRead(*sl, hfd, H(r1), regs[2])
                                   : ::read(hfd, H(r1), regs[2]);
        regs[0] = (n < 0) ? static_cast<std::uint32_t>(-errno) : static_cast<std::uint32_t>(n);
        return;
    }
    case 6: {  // close(fd)
        int hfd = -1;
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            const auto it = fd_map_.find(r0);
            if (it != fd_map_.end()) {
                hfd = it->second;
                fd_map_.erase(it);
                SetFdSliceLocked(r0, nullptr);
            }
        }
        if (hfd < 0) {
            regs[0] = static_cast<std::uint32_t>(-EBADF);
            return;
        }
        regs[0] = (::close(hfd) == 0) ? 0 : static_cast<std::uint32_t>(-errno);
        return;
    }
    case 186: {  // sigaltstack(const stack_t* ss, stack_t* old)
        // stack_t (arm32): { void* ss_sp; int ss_flags; size_t ss_size; } = 12B.
        // We do not deliver guest signals, so the alternate stack is inert;
        // report "disabled" as the previous state, like a kernel would on the
        // first call, and accept a valid new stack. This is the only raw
        // syscall hit during breakpad's start-up.
        if (r1 != 0) {
            if (!CheckRange(r1, 12, true)) {
                regs[0] = static_cast<std::uint32_t>(-EFAULT);
                return;
            }
            const std::uint32_t old[3] = {0, 2 /*SS_DISABLE*/, 0};
            std::memcpy(H(r1), old, 12);
        }
        if (r0 != 0 && !CheckRange(r0, 12, false)) {
            regs[0] = static_cast<std::uint32_t>(-EFAULT);
            return;
        }
        regs[0] = 0;
        return;
    }
    case 20:  // getpid
        regs[0] = static_cast<std::uint32_t>(::getpid());
        return;
    case 0xF0002: {  // __ARM_NR_cacheflush(start, end, flags): a guest wrote code
        // Drop the translations of [start, end); Dynarmic does it when a
        // Jit's Run stops at the next block boundary (RunJit resumes it).
        // A shared cache is invalidated for every thread at once; private
        // caches each hold their own copy of the old code, so every live Jit
        // drops it (InvalidateCacheRange may be called from any thread).
        if (r1 > r0) {
            if (prof::g_enabled) {
                prof::g_flushes.fetch_add(1, std::memory_order_relaxed);
                prof::g_flush_bytes.fetch_add(r1 - r0, std::memory_order_relaxed);
            }
            if (JitShareOn()) {
                if (Dynarmic::A32::Jit* jit = ActiveJit()) {
                    jit->InvalidateCacheRange(r0, r1 - r0);
                }
            } else {
                std::lock_guard<std::mutex> l(live_jits_mu_);
                for (Dynarmic::A32::Jit* jit : live_jits_) {
                    jit->InvalidateCacheRange(r0, r1 - r0);
                }
            }
        }
        regs[0] = 0;
        return;
    }
    case 224:  // gettid
        regs[0] = GuestTid();
        return;
    default:
        std::printf("[syscall] LOUD unimplemented nr=%u r0=%08x r1=%08x r2=%08x pc=%08x "
                    "lr=%08x -> -ENOSYS\n",
                    nr, r0, r1, regs[2], regs[15] - 4, regs[14]);
        // Not implemented: answer -ENOSYS like a real kernel does, instead of
        // killing the guest thread: a guest that checks the error can carry
        // on, while the old halt turned a missing syscall into a dead
        // game. (Measured: the guests use only nr=186 gettid and nr=4 write,
        // so this never fired -- it is a safety net, not a hot path.)
        regs[0] = static_cast<std::uint32_t>(-ENOSYS);
        return;
    }
}

// ---- real guest threads ----
// Per-thread Dynarmic state. Main thread: t_hot.active_jit == nullptr (uses jit_).

// Per-thread pthread_once hijack stack (see dispatch.h): shared state here
// would let one thread's return pop another thread's in-progress entry.
thread_local std::array<Runtime::OnceHijack, Runtime::kOnceMaxDepth> Runtime::once_stack_{};
thread_local int Runtime::once_depth_ = 0;

Dynarmic::A32::Jit* Runtime::ActiveJit() {
    return t_hot.active_jit ? t_hot.active_jit : jit_;
}

std::uint32_t Runtime::GuestTid() {
    return t_hot.tid ? t_hot.tid : 1;
}

// dlmalloc DEBUG assert reporter: expression + line, async-safe (literals
// only, raw write). Called from the vendored assert macro, then ABORT.
extern "C" void AbrAssertHook(const char* expr, int line) {
    char buf[256];
    char* p = buf;
    const char* t = "HEAPASSERT ";
    while (*t) {
        *p++ = *t++;
    }
    while (expr && *expr && (p - buf) < 200) {
        *p++ = *expr++;
    }
    *p++ = ' ';
    // line, decimal, no library calls.
    char nb[12];
    int nl = 0;
    int v = line;
    if (v == 0) {
        nb[nl++] = '0';
    } else {
        while (v > 0 && nl < 11) {
            nb[nl++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    for (int i = nl - 1; i >= 0; i--) {
        *p++ = nb[i];
    }
    *p++ = '\n';
    (void)write(STDERR_FILENO, buf, (std::size_t)(p - buf));
    g_heap_corrupt_n.fetch_add(1, std::memory_order_relaxed);
}
extern "C" __attribute__((noinline)) void AbrHeapCorruptionHook(void) {
    g_heap_corrupt_n.fetch_add(1, std::memory_order_relaxed);
    char buf[384];
    char* p = buf;
    const char* t = "HEAPCORRUPT pc=";
    while (*t) {
        *p++ = *t++;
    }
    std::uint32_t pc = 0, r0 = 0, r1 = 0;
    Dynarmic::A32::Jit* aj = t_hot.active_jit ? t_hot.active_jit : nullptr;
    if (aj) {
        pc = aj->Regs()[15];
        r0 = aj->Regs()[0];
        r1 = aj->Regs()[1];
    }
    g_heap_corrupt_pc.store(pc, std::memory_order_relaxed);
    DeathHex2(p, pc);
    const char* t0 = " r0=";
    while (*t0) {
        *p++ = *t0++;
    }
    DeathHex2(p, r0);
    const char* t1 = " r1=";
    while (*t1) {
        *p++ = *t1++;
    }
    DeathHex2(p, r1);
    const char* t2 = " free=";
    while (*t2) {
        *p++ = *t2++;
    }
    DeathHex2(p, g_last_free_addr);
    const char* t3 = " sz=";
    while (*t3) {
        *p++ = *t3++;
    }
    DeathHex2(p, g_last_free_size);
    // ra: the dlmalloc.c call site of the abort macro (offset in libabhost) --
    // names the exact invariant that fired. lr: guest caller of the mspace_*
    // wrapper. fr: raw host frame-walk offsets (symbolizable offline).
    {
        const std::uintptr_t ra = (std::uintptr_t)__builtin_return_address(0);
        const char* t4 = " ra=";
        while (*t4) {
            *p++ = *t4++;
        }
        Hex64(p, ra - g_lib_base);
        const char* t5 = " lr=";
        while (*t5) {
            *p++ = *t5++;
        }
        DeathHex2(p, aj ? aj->Regs()[14] : 0);
        const char* t6 = " fr=";
        while (*t6) {
            *p++ = *t6++;
        }
        std::uint64_t fp = (std::uint64_t)__builtin_frame_address(0);
        for (int f = 0; f < 5 && fp > 0x7000000000ull && (p - buf) < 340; f++) {
            const std::uint64_t* fr = (const std::uint64_t*)fp;
            const std::uint64_t next = fr[0];
            const std::uint64_t ret = fr[1];
            if (next <= fp) {
                break;
            }
            fp = next;
            Hex64(p, ret - g_lib_base);
            *p++ = ' ';
        }
    }
    *p++ = '\n';
    (void)write(STDERR_FILENO, buf, (std::size_t)(p - buf));
}

unsigned Runtime::HeapCorruptionCount() {
    return g_heap_corrupt_n.load(std::memory_order_relaxed);
}

bool Runtime::HeapWatchViolation(std::uint32_t addr, std::size_t size) {
    // Cheap prefilter (no lock): outside the heap arena entirely.
    const std::uint64_t a = addr;
    if (a < heap_base_ || a + size > (std::uint64_t)heap_base_ + kHeapSize) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::lock_guard<std::recursive_mutex> heap(heap_mu_);  // order: mutex_ -> heap_mu_
    // D2: one containment query covers both old branches (an exact base hit is
    // just containment with base == addr).
    std::uint32_t cbase = 0, csize = 0;
    if (LiveIdxContaining(addr, &cbase, &csize) &&
        (std::uint64_t)addr + size <= (std::uint64_t)cbase + csize) {
        return false;  // fully inside a live allocation
    }
    // Quarantined (freed, held)? Distinct verdict: UAF write.
    for (const auto& q : norelease_held_) {
        if (a >= q.addr && a + size <= (std::uint64_t)q.addr + q.size) {
            static int qn = 0;
            if (qn < 30) {
                qn++;
                std::printf("[heapwatch] UAF-WRITE addr=%08x size=%zu q=%08x+%u\n", addr, size,
                            q.addr, q.size);
            }
            return true;
        }
    }
    {
        static int wn = 0;
        if (wn < 30) {
            wn++;
            std::uint32_t lr = 0;
            Dynarmic::A32::Jit* aj = ActiveJit();
            if (aj) {
                lr = aj->Regs()[14];
            }
            std::uint32_t pc = 0;
            if (aj) {
                pc = aj->Regs()[15];
            }
            std::printf("[heapwatch] WILD-WRITE addr=%08x size=%zu pc=%08x lr=%08x\n", addr, size,
                        pc, lr);
        }
    }
    return true;
}

std::uint32_t Runtime::HeapCorruptionPc() {
    return g_heap_corrupt_pc.load(std::memory_order_relaxed);
}

void Runtime::ThreadEnter(Dynarmic::A32::Jit* jit, std::uint32_t tid) {
    t_hot.active_jit = jit;
    t_hot.tid = tid;
    if (jit) {
        jit->SetTpidruro(0);  // BindTls
        RegisterSigThread(tid, jit);
        sampler::ThreadUses();  // the profiler's timer for this thread, when on
    } else if (t_sig) {
        std::lock_guard<std::mutex> l(sig_mu_);  // see GuestSendSignal
        t_sig->jit.store(nullptr, std::memory_order_release);  // its Jit is going away
    }
}

bool Runtime::RunComparator(std::uint32_t compar, std::uint32_t pa, std::uint32_t pb,
                            std::int32_t& out) {
    return RunNested(compar, pa, pb, 0, 0, nullptr, out);
}

bool Runtime::RunNestedOnStack(std::uint32_t entry, std::uint32_t r0, std::uint32_t r1,
                               std::uint32_t r2, std::uint32_t sp, const std::uint32_t* r4_r11) {
    std::int32_t ignored = 0;
    return RunNested(entry, r0, r1, r2, sp, r4_r11, ignored);
}

// Runs guest entry(r0, r1, r2) on a callback Jit and returns r0 in out.
// sp == 0: the slot's own stack; otherwise that guest sp, with r4-r11 seeded
// from r4_r11 (a signal handler on the interrupted thread's stack).
bool Runtime::RunNested(std::uint32_t compar, std::uint32_t pa, std::uint32_t pb,
                        std::uint32_t pc_arg, std::uint32_t sp, const std::uint32_t* r4_r11,
                        std::int32_t& out) {
    // Cheap validation first (wild compar = guest bug, not a callback).
    if (!compar || !IsCodeAddress(compar & ~std::uint32_t(1))) {
        cb_last_hr_ = -2;  // rejected before first Run
        return false;
    }
    CbSlot* slot = nullptr;
    std::size_t slot_idx = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (cb_slots_.empty()) {
            cb_slots_.resize(8);
        }
        for (std::size_t i = 0; i < cb_slots_.size(); i++) {
            if (!cb_slots_[i].busy) {
                slot = &cb_slots_[i];
                slot_idx = i;
                slot->busy = true;
                break;
            }
        }
        if (slot && !slot->jit) {
            slot->stack = GuestMalloc(0x200000u);
            if (!slot->stack) {
                slot->busy = false;
                return false;
            }
            Dynarmic::A32::UserConfig cfg;
            cfg.callbacks = cb_;
            cfg.optimizations =
                JitOptimizations();
            cfg.unsafe_optimizations = JitUnsafeOptimizations();
            cfg.enable_cycle_counting = kJitCycleCounting;
            if (JitFastmemOn()) {
                cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(mem().base());
            }
            // Comparator slots are permanent: their ids are never released.
            std::size_t cpu_id = 0;
            if (!AcquireCpuId(&cpu_id)) {
                GuestFree(slot->stack);
                slot->stack = 0;
                slot->busy = false;
                return false;
            }
            cfg.global_monitor = &monitor_;
            cfg.processor_id = cpu_id;
            slot->jit = NewJit(cfg);
        }
    }
    if (!slot) {
        cb_last_hr_ = -3;  // pool exhausted: who holds slots?
        {
            unsigned mask = 0;
            for (std::size_t b = 0; b < cb_slots_.size(); b++) {
                if (cb_slots_[b].busy) {
                    mask |= 1u << b;
                }
            }
            std::printf("[qsort] pool exhausted: busy mask=%02x of %u (t=%u)\n", mask,
                        (unsigned)cb_slots_.size(), GuestTid());
        }
        return false;  // nested comparator storm: caller aborts LOUD
    }
    Dynarmic::A32::Jit* jit = slot->jit.get();
    jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    auto& regs = jit->Regs();
    regs.fill(0);
    regs[13] = sp ? sp : slot->stack + 0x200000u - 8;
    regs[14] = mod_.halt_va;
    regs[0] = pa;
    regs[1] = pb;
    regs[2] = pc_arg;
    if (r4_r11) {
        for (int i = 0; i < 8; i++) {
            regs[4 + i] = r4_r11[i];
        }
    }
    if (compar & 1u) {
        std::uint32_t cpsr = jit->Cpsr();
        jit->SetCpsr((cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10) |
                     std::uint32_t(0x20));
        regs[15] = compar & ~std::uint32_t(1);
    } else {
        std::uint32_t cpsr = jit->Cpsr();
        jit->SetCpsr((cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10));
        regs[15] = compar;
    }
    // Swap ActiveJit so nested thunks route to THIS jit (regs/Ret/cpsr);
    // restore before returning to the outer Run.
    Dynarmic::A32::Jit* outer = t_hot.active_jit;
    t_hot.active_jit = jit;
    jit->SetTpidruro(0);  // slots serve any thread (BindTls)
    Dynarmic::HaltReason hr = RunJit(*jit);
    t_hot.active_jit = outer;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        slot->busy = false;
    }
    cb_last_hr_ = static_cast<int>(hr);
    if (hr != Dynarmic::HaltReason::UserDefined1) {
        return false;
    }
    out = static_cast<std::int32_t>(regs[0]);
    return true;
}

// ---------------------------------------------------------------------------
// Guest signals (see dispatch.h). arm32 Linux numbers: SIGKILL 9, SIGSTOP 19;
// sigaction flags SA_SIGINFO 4, SA_ONSTACK 0x08000000, SA_RESTART 0x10000000,
// SA_NODEFER 0x40000000, SA_RESETHAND 0x80000000; how SIG_BLOCK 0, SIG_UNBLOCK
// 1, SIG_SETMASK 2. Bit (sig - 1) of a 32-bit mask stands for signal sig.

void DeliverGuestSignalsHere() {
    if (t_rt) {
        t_rt->DeliverPendingSignals();
    }
}

namespace {
constexpr std::uint32_t kSigBit(std::uint32_t sig) { return 1u << (sig - 1); }
constexpr std::uint32_t kUnblockable = (1u << (9 - 1)) | (1u << (19 - 1));
bool SigDefaultIgnores(std::uint32_t sig) {
    return sig == 17 || sig == 18 || sig == 23 || sig == 28;  // CHLD CONT URG WINCH
}
}  // namespace

void Runtime::RegisterSigThread(std::uint32_t tid, Dynarmic::A32::Jit* jit) {
    std::lock_guard<std::mutex> l(sig_mu_);
    std::shared_ptr<SigThread>& e = sig_threads_[tid];
    if (!e) {
        e = std::make_shared<SigThread>();
    }
    e->jit.store(jit, std::memory_order_release);
    e->host_tid.store(static_cast<int>(::syscall(SYS_gettid)), std::memory_order_relaxed);
    if (t_stack_size) {
        e->stack_top = t_stack_lo + t_stack_size;
    }
    t_sig = e;
    t_rt = this;
}

int Runtime::HostTidOf(std::uint32_t tid) {
    std::lock_guard<std::mutex> l(sig_mu_);
    const auto it = sig_threads_.find(tid);
    return it != sig_threads_.end() ? it->second->host_tid.load(std::memory_order_relaxed) : 0;
}

namespace {
std::string BaseName(const std::string& p) {
    const std::size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
}  // namespace

int Runtime::GuestLibraryByName(const std::string& name) const {
    const std::string want = BaseName(name);
    if (BaseName(mod_.path) == want) {
        return 0;
    }
    for (std::size_t i = 0; i < mod_.more.size(); i++) {
        if (BaseName(mod_.more[i].path) == want) {
            return static_cast<int>(i + 1);
        }
    }
    return -1;
}

std::uint32_t Runtime::GuestLibraryHandle(const std::string& name) {
    const int lib = GuestLibraryByName(name);
    if (lib < 0) {
        return 0;
    }
    // dlopen initialises the library (and the guest libraries it needs) the
    // first time, outside the runtime lock: initialisers may start threads.
    if (!InitGuestLibrary(lib)) {
        return 0;
    }
    const std::size_t n = mod_.more.size() + 1;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (lib_handles_.size() < n) {
        lib_handles_.resize(n, 0);
    }
    if (!lib_handles_[static_cast<std::size_t>(lib)]) {
        lib_handles_[static_cast<std::size_t>(lib)] = GuestMalloc(16);  // opaque, never read
    }
    return lib_handles_[static_cast<std::size_t>(lib)];
}

void Runtime::MarkGuestLibraryInitialised(int lib) {
    std::lock_guard<std::mutex> l(lib_init_mu_);
    if (lib_init_state_.size() < mod_.libs.size()) {
        lib_init_state_.resize(mod_.libs.size(), 0);
    }
    if (lib >= 0 && static_cast<std::size_t>(lib) < lib_init_state_.size()) {
        lib_init_state_[static_cast<std::size_t>(lib)] = 2;
    }
}

bool Runtime::InitGuestLibrary(int lib) {
    if (lib < 0 || static_cast<std::size_t>(lib) >= mod_.libs.size()) {
        return true;  // a module without per-library records: nothing deferred
    }
    const std::uint32_t me = (GuestTid() + 1) << 2 | 1u;
    {
        std::unique_lock<std::mutex> l(lib_init_mu_);
        if (lib_init_state_.size() < mod_.libs.size()) {
            lib_init_state_.resize(mod_.libs.size(), 0);
        }
        std::uint32_t& st = lib_init_state_[static_cast<std::size_t>(lib)];
        // Another thread initialising it: wait. This thread (a dlopen from
        // inside an initialiser): go on, as the linker does.
        lib_init_cv_.wait(l, [&] { return st == 0 || st == 2 || st == me; });
        if (st != 0) {
            return true;
        }
        st = me;
    }
    bool ok = true;
    for (const std::string& dep : mod_.libs[static_cast<std::size_t>(lib)].needed) {
        const int d = GuestLibraryByName(dep);
        if (d >= 0 && d != lib) {
            ok = ok && InitGuestLibrary(d);
        }
    }
    const LoadedModule::LibInfo& info = mod_.libs[static_cast<std::size_t>(lib)];
    for (std::uint32_t k = info.init_begin; ok && k < info.init_end; k++) {
        const std::uint32_t fn = mod_.init_array[k];
        if (fn == 0) {
            continue;
        }
        std::int32_t ignored = 0;
        if (!RunNested(fn, 0, 0, 0, 0, nullptr, ignored)) {
            std::printf("[dl] LOUD %s init #%u @%08x did not return (hr=%d)\n",
                        BaseName(lib == 0 ? mod_.path : mod_.more[static_cast<std::size_t>(lib - 1)].path)
                            .c_str(),
                        k - info.init_begin, fn, cb_last_hr_);
            ok = false;
        }
    }
    {
        std::lock_guard<std::mutex> l(lib_init_mu_);
        lib_init_state_[static_cast<std::size_t>(lib)] = ok ? 2u : 0u;
    }
    lib_init_cv_.notify_all();
    std::printf("[dl] %s initialised (%u init entries)%s\n",
                BaseName(lib == 0 ? mod_.path : mod_.more[static_cast<std::size_t>(lib - 1)].path).c_str(),
                info.init_end - info.init_begin, ok ? "" : " -- FAILED");
    return ok;
}

int Runtime::GuestLibraryIndex(std::uint32_t h) const {
    if (h == 0) {
        return -1;
    }
    for (std::size_t i = 0; i < lib_handles_.size() && i < mod_.lib_defined.size(); i++) {
        if (lib_handles_[i] == h) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Runtime::ForgetSigThreadJit(std::uint32_t tid) {
    std::lock_guard<std::mutex> l(sig_mu_);
    const auto it = sig_threads_.find(tid);
    if (it != sig_threads_.end()) {
        it->second->jit.store(nullptr, std::memory_order_release);
    }
}

// The environ array holds the env_ strings in order, then NULL. A new array
// replaces the old one (freed: like bionic's setenv, it does not outlive the
// next change).
void Runtime::RebuildEnvironLocked() {
    if (!environ_var_) {
        return;  // before InitDataImports: built there
    }
    const std::uint32_t arr = GuestMalloc(static_cast<std::uint32_t>(env_.size() + 1) * 4u);
    if (!arr) {
        std::printf("[env] LOUD no guest heap for environ -- left as it was\n");
        return;
    }
    for (std::size_t i = 0; i < env_.size(); i++) {
        std::memcpy(H(arr + static_cast<std::uint32_t>(i) * 4u), &env_[i].second, 4);
    }
    std::memset(H(arr + static_cast<std::uint32_t>(env_.size()) * 4u), 0, 4);
    std::memcpy(H(environ_var_), &arr, 4);
    if (environ_array_) {
        GuestFree(environ_array_);
    }
    environ_array_ = arr;
}

std::uint32_t Runtime::GuestGetenv(const std::string& key) {
    std::lock_guard<std::mutex> l(env_mu_);
    for (const auto& kv : env_) {
        if (kv.first == key) {
            return kv.second + static_cast<std::uint32_t>(key.size()) + 1u;  // past "K="
        }
    }
    return 0;
}

// setenv (value) / unsetenv (null value). Returns 0 or an errno value. An
// old "K=V" string is not freed: a pointer getenv returned may still be in
// use (bionic leaks it too).
std::uint32_t Runtime::GuestSetenv(const std::string& key, const std::string* value,
                                   bool overwrite) {
    if (key.empty() || key.find('=') != std::string::npos) {
        return EINVAL;
    }
    std::lock_guard<std::mutex> l(env_mu_);
    auto it = std::find_if(env_.begin(), env_.end(),
                           [&](const auto& kv) { return kv.first == key; });
    if (!value) {
        if (it != env_.end()) {
            env_.erase(it);
            RebuildEnvironLocked();
        }
        return 0;
    }
    if (it != env_.end() && !overwrite) {
        return 0;
    }
    const std::string kv = key + "=" + *value;
    const std::uint32_t str = GuestMalloc(static_cast<std::uint32_t>(kv.size() + 1));
    if (!str) {
        return ENOMEM;
    }
    std::memcpy(H(str), kv.c_str(), kv.size() + 1);
    if (it != env_.end()) {
        it->second = str;
    } else {
        env_.emplace_back(key, str);
    }
    RebuildEnvironLocked();
    return 0;
}

void Runtime::SetGuestEnv(const std::string& key, const std::string& value) {
    GuestSetenv(key, &value, true);
}

// zlib for guest streams. The guest's z_stream is the 32-bit zlib layout
// (NDK, measured): next_in @0, avail_in @4, total_in @8, next_out @12,
// avail_out @16, total_out @20, msg @24, state @28, zalloc @32, zfree @36,
// opaque @40, data_type @44, adler @48, reserved @52; 56 bytes. Each guest
// stream has a host z_stream (zstreams_, keyed by the guest address); every
// call copies the fields in, runs host zlib on the guest buffers, and copies
// them back. The guest's zalloc/zfree are not used: the host stream's memory
// is the host's. `state` reads as the guest stream itself (non-NULL while
// initialised, as zlib callers test), msg as a guest copy of the host text.
bool Runtime::ZlibCall(ImportId id, const std::string& n, Regs& regs) {
    using I = ImportId;
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2];
    if (id == I::crc32_) {
        // uLong crc32(uLong crc, const Bytef* buf, uInt len); buf NULL -> 0.
        if (a1 == 0) {
            Ret(regs, 0);
            return true;
        }
        if (a2 && !CheckRange(a1, a2, false)) {
            return false;
        }
        Ret(regs, static_cast<std::uint32_t>(::crc32(a0, H<const Bytef>(a1), a2)));
        return true;
    }
    if (!CheckRange(a0, 56, true)) {
        return false;
    }
    std::uint32_t g[14];
    std::memcpy(g, H(a0), 56);
    std::lock_guard<std::mutex> l(zlib_mu_);
    const bool init = id == I::inflateInit__ || id == I::inflateInit2__ || id == I::deflateInit2__;
    if (init) {
        // The version/size handshake is with the guest's zlib.h: the major
        // version must match and the size must be the 32-bit z_stream.
        const std::uint32_t ver = id == I::inflateInit__ ? a1 : id == I::inflateInit2__ ? a2 : 0;
        std::uint32_t ver_ptr = ver, size = id == I::inflateInit__ ? a2 : regs[3];
        if (id == I::deflateInit2__) {
            // (strm, level, method, windowBits, memLevel, strategy, version,
            // stream_size): the last four on the stack.
            if (!CheckRange(regs[13], 16, false)) {
                return false;
            }
            std::memcpy(&ver_ptr, H(regs[13] + 8), 4);
            std::memcpy(&size, H(regs[13] + 12), 4);
        }
        if (!ver_ptr || !CheckRange(ver_ptr, 1, false) || *H<const char>(ver_ptr) != ZLIB_VERSION[0] ||
            size != 56) {
            Ret(regs, static_cast<std::uint32_t>(Z_VERSION_ERROR));
            return true;
        }
        auto zs = std::make_unique<z_stream>();
        int r = Z_OK;
        if (id == I::inflateInit__) {
            r = ::inflateInit(zs.get());
        } else if (id == I::inflateInit2__) {
            r = ::inflateInit2(zs.get(), static_cast<int>(a1));
        } else {
            std::int32_t mem_level = 0, strategy = 0;
            std::memcpy(&mem_level, H(regs[13]), 4);
            std::memcpy(&strategy, H(regs[13] + 4), 4);
            r = ::deflateInit2(zs.get(), static_cast<int>(a1), static_cast<int>(a2),
                               static_cast<int>(regs[3]), mem_level, strategy);
        }
        if (r == Z_OK) {
            zstreams_[a0] = std::move(zs);
            g[7] = a0;  // state
            g[2] = g[5] = 0;
            g[6] = 0;   // msg
            std::memcpy(H(a0), g, 56);
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    }
    const auto it = zstreams_.find(a0);
    if (it == zstreams_.end()) {
        Ret(regs, static_cast<std::uint32_t>(Z_STREAM_ERROR));
        return true;
    }
    z_stream* zs = it->second.get();
    if (id == I::inflateEnd_ || id == I::deflateEnd_) {
        const int r = id == I::inflateEnd_ ? ::inflateEnd(zs) : ::deflateEnd(zs);
        zstreams_.erase(it);
        g[7] = 0;
        std::memcpy(H(a0), g, 56);
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    }
    // Buffers: avail_* bytes at next_* (a zero length needs no pointer).
    const std::uint32_t in = g[0], in_len = g[1], out = g[3], out_len = g[4];
    if ((in_len && !CheckRange(in, in_len, false)) || (out_len && !CheckRange(out, out_len, true))) {
        return false;
    }
    zs->next_in = in_len ? H<Bytef>(in) : nullptr;
    zs->avail_in = in_len;
    zs->next_out = out_len ? H<Bytef>(out) : nullptr;
    zs->avail_out = out_len;
    zs->total_in = g[2];
    zs->total_out = g[5];
    zs->data_type = static_cast<int>(g[11]);
    int r = Z_STREAM_ERROR;
    if (id == I::inflate_) {
        r = ::inflate(zs, static_cast<int>(a1));
    } else if (id == I::deflate_) {
        r = ::deflate(zs, static_cast<int>(a1));
    } else if (id == I::inflateReset_) {
        r = ::inflateReset(zs);
    } else if (id == I::deflateReset_) {
        r = ::deflateReset(zs);
    }
    const std::uint32_t used_in = in_len - zs->avail_in, used_out = out_len - zs->avail_out;
    g[0] = in + (in_len ? used_in : 0);
    g[1] = zs->avail_in;
    g[2] = static_cast<std::uint32_t>(zs->total_in);
    g[3] = out + (out_len ? used_out : 0);
    g[4] = zs->avail_out;
    g[5] = static_cast<std::uint32_t>(zs->total_out);
    g[11] = static_cast<std::uint32_t>(zs->data_type);
    g[12] = static_cast<std::uint32_t>(zs->adler);
    g[6] = 0;
    if (zs->msg) {
        std::uint32_t& m = zlib_msgs_[zs->msg];
        if (!m) {
            const std::size_t len = std::strlen(zs->msg) + 1;
            m = GuestMalloc(static_cast<std::uint32_t>(len));
            if (m) {
                std::memcpy(H(m), zs->msg, len);
            }
        }
        g[6] = m;
    }
    std::memcpy(H(a0), g, 56);
    Ret(regs, static_cast<std::uint32_t>(r));
    return true;
}

bool Runtime::Sha1Call(ImportId id, const std::string& n, Regs& regs) {
    using I = ImportId;
    // SHA1Init(ctx); SHA1Update(ctx, data, len); SHA1Final(digest[20], ctx).
    const std::uint32_t ctx = id == I::SHA1Final_ ? regs[1] : regs[0];
    if (!CheckRange(ctx, sizeof(GuestSha1Ctx), true)) {
        return false;
    }
    GuestSha1Ctx c;
    std::memcpy(&c, H(ctx), sizeof(c));
    if (id == I::SHA1Init_) {
        Sha1Init(&c);
    } else if (id == I::SHA1Update_) {
        if (regs[2] && !CheckRange(regs[1], regs[2], false)) {
            return false;
        }
        Sha1Update(&c, regs[2] ? H<const std::uint8_t>(regs[1]) : nullptr, regs[2]);
    } else {
        if (!CheckRange(regs[0], 20, true)) {
            return false;
        }
        Sha1Final(H<std::uint8_t>(regs[0]), &c);
    }
    std::memcpy(H(ctx), &c, sizeof(c));
    RetVoid(regs);
    return true;
}

namespace {
// bionic 7 mbrtoc32 over a 4-byte mbstate (the bytes of a sequence so far).
// Returns the byte count, 0 for NUL, (size_t)-2 incomplete, (size_t)-1 with
// *err = EILSEQ.
std::uint32_t Utf8Decode(std::uint8_t st[4], const std::uint8_t* s, std::uint32_t n,
                         std::uint32_t* out, int* err) {
    auto init = [&]() { return st[0] == 0 && st[1] == 0 && st[2] == 0 && st[3] == 0; };
    auto illegal = [&]() {
        std::memset(st, 0, 4);
        *err = EILSEQ;
        return 0xFFFFFFFFu;
    };
    if (n == 0) {
        return 0;
    }
    if (init() && (s[0] & ~0x7Fu) == 0) {
        *out = s[0];
        return s[0] != 0 ? 1u : 0u;
    }
    const std::uint32_t so_far = st[2] ? 3u : st[1] ? 2u : st[0] ? 1u : 0u;
    const std::uint8_t ch = so_far ? st[0] : s[0];
    std::uint32_t mask = 0, length = 0, lower = 0;
    if ((ch & 0x80) == 0) {
        mask = 0x7F, length = 1, lower = 0;
    } else if ((ch & 0xE0) == 0xC0) {
        mask = 0x1F, length = 2, lower = 0x80;
    } else if ((ch & 0xF0) == 0xE0) {
        mask = 0x0F, length = 3, lower = 0x800;
    } else if ((ch & 0xF8) == 0xF0) {
        mask = 0x07, length = 4, lower = 0x10000;
    } else {
        return illegal();
    }
    const std::uint32_t wanted = length - so_far;
    std::uint32_t i = 0;
    for (; i < wanted && i < n; i++) {
        if (!init() && (s[i] & 0xC0) != 0x80) {
            return illegal();
        }
        st[so_far + i] = s[i];
    }
    if (i < wanted) {
        return 0xFFFFFFFEu;
    }
    std::uint32_t c = st[0] & mask;
    for (std::uint32_t k = 1; k < length; k++) {
        c = (c << 6) | (st[k] & 0x3Fu);
    }
    if (c < lower || (c >= 0xD800 && c <= 0xDFFF) || c == 0xFFFE || c == 0xFFFF) {
        return illegal();
    }
    *out = c;
    std::memset(st, 0, 4);
    return c == 0 ? 0u : wanted;
}

// bionic 7 c32rtomb: 1-4 bytes into out, or (size_t)-1 (EILSEQ).
std::uint32_t Utf8Encode(std::uint32_t c, std::uint8_t out[4]) {
    std::uint32_t length = 0;
    std::uint8_t lead = 0;
    if ((c & ~0x7Fu) == 0) {
        out[0] = static_cast<std::uint8_t>(c);
        return 1;
    }
    if (c <= 0x7FF) {
        lead = 0xC0, length = 2;
    } else if (c <= 0xFFFF) {
        lead = 0xE0, length = 3;
    } else if (c <= 0x1FFFFF) {
        lead = 0xF0, length = 4;
    } else {
        return 0xFFFFFFFFu;
    }
    for (std::uint32_t i = length - 1; i > 0; i--) {
        out[i] = static_cast<std::uint8_t>((c & 0x3F) | 0x80);
        c >>= 6;
    }
    out[0] = static_cast<std::uint8_t>((c & 0xFF) | lead);
    return length;
}
}  // namespace

bool Runtime::WideCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs) {
    using I = ImportId;
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    // A NUL-terminated guest wide string: its length, or -1 if unreadable.
    auto wlen = [&](std::uint32_t w) -> std::uint32_t {
        for (std::uint32_t i = 0; i < 0x100000u; i++) {
            if (!CheckRange(w + i * 4, 4, false)) {
                return 0xFFFFFFFFu;
            }
            std::uint32_t c = 0;
            std::memcpy(&c, H(w + i * 4), 4);
            if (c == 0) {
                return i;
            }
        }
        return 0xFFFFFFFFu;
    };
    auto file = [&](std::uint32_t h) -> FILE* {
        std::lock_guard<std::mutex> io(io_mutex_);
        const auto it = file_map_.find(h);
        return it == file_map_.end() ? nullptr : it->second.f;
    };
    if (id == I::mbrtowc_) {
        // (wchar_t* pwc, const char* s, size_t n, mbstate_t* ps); s NULL is
        // mbrtowc(NULL, "", 1, ps).
        std::uint8_t* st = mbrtowc_state_;
        if (a3) {
            if (!CheckRange(a3, 4, true)) {
                return false;
            }
            st = H<std::uint8_t>(a3);
        }
        std::uint32_t pwc = a0, src = a1, len = a2;
        static const std::uint8_t kEmpty = 0;
        const std::uint8_t* bytes = &kEmpty;
        if (src == 0) {
            pwc = 0;
            len = 1;
        } else {
            len = len > 4 ? 4 : len;  // a sequence is at most 4 bytes
            if (len && !CheckRange(src, len, false)) {
                // Fewer readable bytes than n: take what is mapped.
                while (len && !CheckRange(src, len, false)) {
                    len--;
                }
                if (!len) {
                    return false;
                }
            }
            bytes = H<const std::uint8_t>(src);
        }
        std::uint32_t c = 0;
        int err = 0;
        const std::uint32_t r = Utf8Decode(st, bytes, len, &c, &err);
        if (err) {
            SetGuestErrno(err);
        } else if (pwc && r != 0xFFFFFFFEu) {
            if (!CheckRange(pwc, 4, true)) {
                return false;
            }
            std::memcpy(H(pwc), &c, 4);
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::wcrtomb_) {
        // (char* s, wchar_t wc, mbstate_t* ps); s NULL resets: returns 1.
        if (a2 && !CheckRange(a2, 4, true)) {
            return false;
        }
        std::uint8_t* st = a2 ? H<std::uint8_t>(a2) : nullptr;
        if (a0 == 0) {
            if (st) {
                std::memset(st, 0, 4);
            }
            Ret(regs, 1);
            return true;
        }
        if (st && (st[0] | st[1] | st[2] | st[3])) {
            std::memset(st, 0, 4);
            SetGuestErrno(EILSEQ);
            Ret(regs, 0xFFFFFFFFu);
            return true;
        }
        std::uint8_t b[4];
        const std::uint32_t k = Utf8Encode(a1, b);
        if (k == 0xFFFFFFFFu) {
            SetGuestErrno(EILSEQ);
            Ret(regs, k);
            return true;
        }
        if (!CheckRange(a0, k, true)) {
            return false;
        }
        std::memcpy(H(a0), b, k);
        Ret(regs, k);
        return true;
    }
    if (id == I::wcscoll_) {
        // The C locale: wcscmp (wchar_t compares as a signed 32-bit int).
        const std::uint32_t la = wlen(a0), lb = wlen(a1);
        if (la == 0xFFFFFFFFu || lb == 0xFFFFFFFFu) {
            return false;
        }
        for (std::uint32_t i = 0;; i++) {
            std::int32_t x = 0, y = 0;
            std::memcpy(&x, H(a0 + i * 4), 4);
            std::memcpy(&y, H(a1 + i * 4), 4);
            if (x != y || x == 0) {
                Ret(regs, x < y ? 0xFFFFFFFFu : x > y ? 1u : 0u);
                return true;
            }
        }
    }
    if (id == I::wcsxfrm_) {
        // The C locale: wcslcpy(dst, src, n), the length of src.
        const std::uint32_t l = wlen(a1);
        if (l == 0xFFFFFFFFu) {
            return false;
        }
        if (a2) {
            const std::uint32_t cp = l < a2 ? l : a2 - 1;
            if (!CheckRange(a0, (cp + 1) * 4, true)) {
                return false;
            }
            std::memmove(H(a0), H(a1), cp * 4);
            std::memset(H(a0 + cp * 4), 0, 4);
        }
        Ret(regs, l);
        return true;
    }
    if (id == I::wcsftime_) {
        // (wchar_t* s, size_t max, const wchar_t* fmt, const struct tm* t):
        // as FreeBSD does it -- the format narrowed to UTF-8, strftime, the
        // result widened back; 0 when it does not fit.
        const std::uint32_t fl = wlen(a2);
        if (fl == 0xFFFFFFFFu || !CheckRange(a3, 36, false)) {
            return false;
        }
        std::string fmt;
        for (std::uint32_t i = 0; i < fl; i++) {
            std::uint32_t c = 0;
            std::memcpy(&c, H(a2 + i * 4), 4);
            std::uint8_t b[4];
            const std::uint32_t k = Utf8Encode(c, b);
            if (k == 0xFFFFFFFFu) {
                SetGuestErrno(EILSEQ);
                Ret(regs, 0);
                return true;
            }
            fmt.append(reinterpret_cast<const char*>(b), k);
        }
        std::int32_t f[9] = {};
        std::memcpy(f, H(a3), 36);
        struct tm ht = {};
        ht.tm_sec = f[0];
        ht.tm_min = f[1];
        ht.tm_hour = f[2];
        ht.tm_mday = f[3];
        ht.tm_mon = f[4];
        ht.tm_year = f[5];
        ht.tm_wday = f[6];
        ht.tm_yday = f[7];
        ht.tm_isdst = f[8];
        char out[1024];
        const std::size_t o = std::strftime(out, sizeof out, fmt.c_str(), &ht);
        std::vector<std::uint32_t> wide;
        std::uint8_t st[4] = {0, 0, 0, 0};
        for (std::size_t i = 0; i < o;) {
            std::uint32_t c = 0;
            int err = 0;
            const std::uint32_t k = Utf8Decode(st, reinterpret_cast<const std::uint8_t*>(out + i),
                                               static_cast<std::uint32_t>(o - i), &c, &err);
            if (err || k == 0 || k == 0xFFFFFFFEu) {
                break;
            }
            wide.push_back(c);
            i += k;
        }
        if (o == 0 || wide.size() + 1 > a1 || !CheckRange(a0, (wide.size() + 1) * 4, true)) {
            Ret(regs, 0);
            return true;
        }
        wide.push_back(0);
        std::memcpy(H(a0), wide.data(), wide.size() * 4);
        Ret(regs, static_cast<std::uint32_t>(wide.size() - 1));
        return true;
    }
    // Wide stdio: the stream holds UTF-8; WEOF = 0xFFFFFFFF.
    if (id == I::putwc_) {
        FILE* f = file(a1);
        std::uint8_t b[4];
        const std::uint32_t k = f ? Utf8Encode(a0, b) : 0xFFFFFFFFu;
        if (k == 0xFFFFFFFFu || std::fwrite(b, 1, k, f) != k) {
            SetGuestErrno(f ? EILSEQ : EBADF);
            Ret(regs, 0xFFFFFFFFu);
            return true;
        }
        Ret(regs, a0);
        return true;
    }
    if (id == I::getwc_) {
        FILE* f = file(a0);
        if (!f) {
            SetGuestErrno(EBADF);
            Ret(regs, 0xFFFFFFFFu);
            return true;
        }
        std::uint8_t st[4] = {0, 0, 0, 0};
        for (;;) {
            const int ch = std::fgetc(f);
            if (ch == EOF) {
                Ret(regs, 0xFFFFFFFFu);
                return true;
            }
            const std::uint8_t b = static_cast<std::uint8_t>(ch);
            std::uint32_t c = 0;
            int err = 0;
            const std::uint32_t k = Utf8Decode(st, &b, 1, &c, &err);
            if (err) {
                SetGuestErrno(EILSEQ);
                Ret(regs, 0xFFFFFFFFu);
                return true;
            }
            if (k != 0xFFFFFFFEu) {
                Ret(regs, c);
                return true;
            }
        }
    }
    if (id == I::ungetwc_) {
        // The host stream can push back one byte for sure: an ASCII
        // character is pushed back; a wider one is refused (WEOF).
        FILE* f = file(a1);
        if (!f || a0 == 0xFFFFFFFFu || a0 > 0x7F || std::ungetc(static_cast<int>(a0), f) == EOF) {
            Ret(regs, 0xFFFFFFFFu);
            return true;
        }
        Ret(regs, a0);
        return true;
    }
    return true;
}

namespace {
// bionic's EAI_ numbers (netdb.h, measured with the NDK); glibc's are
// negative and different, so a host answer is translated.
std::uint32_t BionicEai(int host) {
    switch (host) {
        case 0: return 0;
#ifdef EAI_ADDRFAMILY
        case EAI_ADDRFAMILY: return 1;
#endif
        case EAI_AGAIN: return 2;
        case EAI_BADFLAGS: return 3;
        case EAI_FAIL: return 4;
        case EAI_FAMILY: return 5;
        case EAI_MEMORY: return 6;
#if defined(EAI_NODATA) && (!defined(EAI_ADDRFAMILY) || EAI_NODATA != EAI_ADDRFAMILY)
        case EAI_NODATA: return 7;
#endif
        case EAI_NONAME: return 8;
        case EAI_SERVICE: return 9;
        case EAI_SOCKTYPE: return 10;
        case EAI_SYSTEM: return 11;
#ifdef EAI_OVERFLOW
        case EAI_OVERFLOW: return 14;
#endif
        default: return 4;  // EAI_FAIL
    }
}
// bionic AI_ flags -> the host's (glibc numbers several differently).
int HostAiFlags(std::uint32_t g) {
    int f = 0;
    if (g & 1) f |= AI_PASSIVE;
    if (g & 2) f |= AI_CANONNAME;
    if (g & 4) f |= AI_NUMERICHOST;
    if (g & 8) f |= AI_NUMERICSERV;
    if (g & 2048) f |= AI_V4MAPPED;
    if (g & 256) f |= AI_ALL;
    if (g & 1024) f |= AI_ADDRCONFIG;
    return f;
}
// And back, for the flags a result entry carries.
std::int32_t GuestAiFlags(int h) {
    std::int32_t g = 0;
    if (h & AI_PASSIVE) g |= 1;
    if (h & AI_CANONNAME) g |= 2;
    if (h & AI_NUMERICHOST) g |= 4;
    if (h & AI_NUMERICSERV) g |= 8;
    if (h & AI_V4MAPPED) g |= 2048;
    if (h & AI_ALL) g |= 256;
    if (h & AI_ADDRCONFIG) g |= 1024;
    return g;
}
bool NumericHost(const char* h) {
    std::uint8_t buf[16];
    return ::inet_pton(AF_INET, h, buf) == 1 || ::inet_pton(AF_INET6, h, buf) == 1;
}
}  // namespace

bool Runtime::NetDbCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs) {
    using I = ImportId;
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    // This thread's h_errno (a guest int, per thread as in bionic).
    auto h_errno_cell = [&]() -> std::uint32_t {
        thread_local std::uint32_t cell = 0;
        if (!cell) {
            cell = GuestMalloc(4);
            if (cell) {
                std::memset(H(cell), 0, 4);
            }
        }
        return cell;
    };
    auto set_h_errno = [&](std::int32_t v) {
        if (const std::uint32_t c = h_errno_cell()) {
            std::memcpy(H(c), &v, 4);
        }
    };
    auto guest_str = [&](const std::string& t) -> std::uint32_t {
        const std::uint32_t g = GuestMalloc(static_cast<std::uint32_t>(t.size() + 1));
        if (g) {
            std::memcpy(H(g), t.c_str(), t.size() + 1);
        }
        return g;
    };
    if (id == I::__get_h_errno_) {
        Ret(regs, h_errno_cell());
        return true;
    }
    if (id == I::gai_strerror_) {
        // NetBSD's texts, which bionic uses.
        static const char* const kText[] = {
            "Success", "Address family for hostname not supported",
            "Temporary failure in name resolution", "Invalid value for ai_flags",
            "Non-recoverable failure in name resolution", "ai_family not supported",
            "Memory allocation failure", "No address associated with hostname",
            "hostname nor servname provided, or not known", "servname not supported for ai_socktype",
            "ai_socktype not supported", "System error returned in errno", "Invalid value for hints",
            "Resolved protocol is unknown", "Argument buffer overflow"};
        const int code = static_cast<std::int32_t>(a0);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t& g = gai_msgs_[code];
        if (!g) {
            g = guest_str(code >= 0 && code <= 14 ? kText[code] : "Unknown error");
        }
        Ret(regs, g);
        return true;
    }
    if (id == I::freeaddrinfo_) {
        // Our chains: one guest block per entry (struct, address, name).
        for (std::uint32_t ai = a0; ai != 0;) {
            if (!CheckRange(ai, 32, false)) {
                return false;
            }
            std::uint32_t next = 0;
            std::memcpy(&next, H(ai + 28), 4);
            GuestFree(ai);
            ai = next;
        }
        RetVoid(regs);
        return true;
    }
    if (id == I::getaddrinfo_) {
        // (node, service, hints, res**). Guest struct addrinfo (LP32): flags
        // @0, family @4, socktype @8, protocol @12, addrlen @16, canonname
        // @20, addr @24, next @28; 32 bytes.
        if ((a0 && !CheckRange(a0, 1, false)) || (a1 && !CheckRange(a1, 1, false)) ||
            (a2 && !CheckRange(a2, 32, false)) || !CheckRange(a3, 4, true)) {
            return false;
        }
        const std::string node = a0 ? GuestStr(a0) : "";
        const std::string serv = a1 ? GuestStr(a1) : "";
        std::uint32_t z = 0;
        std::memcpy(H(a3), &z, 4);
        struct addrinfo hints {};
        if (a2) {
            std::int32_t h[4];
            std::memcpy(h, H(a2), 16);
            hints.ai_flags = HostAiFlags(static_cast<std::uint32_t>(h[0]));
            hints.ai_family = h[1];
            hints.ai_socktype = h[2];
            hints.ai_protocol = h[3];
        }
        if (!network_allowed_ && a0 && !NumericHost(node.c_str()) && node != "localhost") {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 8) {
                std::printf("[net] offline: getaddrinfo(\"%s\") -> EAI_NODATA\n", node.c_str());
            }
            Ret(regs, 7);  // EAI_NODATA
            return true;
        }
        struct addrinfo* res = nullptr;
        const int r = ::getaddrinfo(a0 ? node.c_str() : nullptr, a1 ? serv.c_str() : nullptr,
                                    a2 ? &hints : nullptr, &res);
        if (r != 0) {
            if (r == EAI_SYSTEM) {
                SetGuestErrno(errno);
            }
            Ret(regs, BionicEai(r));
            return true;
        }
        std::uint32_t head = 0, prev = 0;
        for (const struct addrinfo* p = res; p; p = p->ai_next) {
            const std::uint32_t alen = static_cast<std::uint32_t>(p->ai_addrlen);
            const std::string cname = p->ai_canonname ? p->ai_canonname : "";
            const std::uint32_t size = 32 + ((alen + 3) & ~3u) + (p->ai_canonname ? cname.size() + 1 : 0);
            const std::uint32_t g = GuestMalloc(size);
            if (!g) {
                break;
            }
            std::memset(H(g), 0, size);
            const std::int32_t f[5] = {GuestAiFlags(p->ai_flags), p->ai_family, p->ai_socktype, p->ai_protocol,
                                       static_cast<std::int32_t>(alen)};
            std::memcpy(H(g), f, 20);
            const std::uint32_t addr = g + 32;
            if (p->ai_addr && alen) {
                std::memcpy(H(addr), p->ai_addr, alen);  // sockaddr layouts match
                std::memcpy(H(g + 24), &addr, 4);
            }
            if (p->ai_canonname) {
                const std::uint32_t cn = addr + ((alen + 3) & ~3u);
                std::memcpy(H(cn), cname.c_str(), cname.size() + 1);
                std::memcpy(H(g + 20), &cn, 4);
            }
            if (prev) {
                std::memcpy(H(prev + 28), &g, 4);
            } else {
                head = g;
            }
            prev = g;
        }
        ::freeaddrinfo(res);
        if (!head) {
            Ret(regs, 6);  // EAI_MEMORY
            return true;
        }
        std::memcpy(H(a3), &head, 4);
        Ret(regs, 0);
        return true;
    }
    if (id == I::gethostbyname_ || id == I::gethostbyaddr_) {
        // struct hostent (LP32): name @0, aliases @4, addrtype @8, length @12,
        // addr_list @16; 20 bytes. One result per thread, overwritten by the
        // next call (bionic's per-thread buffer).
        std::string name;
        std::vector<std::string> addrs;
        int type = AF_INET, length = 4;
        static std::mutex host_resolver_mu;  // the host's gethostby* share a static result
        std::lock_guard<std::mutex> resolver_lock(host_resolver_mu);
        if (id == I::gethostbyname_) {
            if (!CheckRange(a0, 1, false)) {
                return false;
            }
            name = GuestStr(a0);
            if (!network_allowed_ && !NumericHost(name.c_str()) && name != "localhost") {
                set_h_errno(1);  // HOST_NOT_FOUND
                Ret(regs, 0);
                return true;
            }
            const struct hostent* he = ::gethostbyname(name.c_str());
            if (!he) {
                set_h_errno(h_errno);
                Ret(regs, 0);
                return true;
            }
            name = he->h_name ? he->h_name : name;
            type = he->h_addrtype;
            length = he->h_length;
            for (char** p = he->h_addr_list; p && *p && addrs.size() < 16; p++) {
                addrs.emplace_back(*p, static_cast<std::size_t>(length));
            }
        } else {
            // (const void* addr, socklen_t len, int type)
            if (a1 == 0 || a1 > 16 || !CheckRange(a0, a1, false)) {
                return false;
            }
            const std::uint8_t* ad = H<const std::uint8_t>(a0);
            const bool loop = (a2 == AF_INET && a1 == 4 && ad[0] == 127) ||
                              (a2 == AF_INET6 && a1 == 16 &&
                               std::memcmp(ad, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16) == 0);
            if (!network_allowed_ && !loop) {
                set_h_errno(1);
                Ret(regs, 0);
                return true;
            }
            const struct hostent* he = ::gethostbyaddr(ad, a1, static_cast<int>(a2));
            if (!he) {
                set_h_errno(h_errno);
                Ret(regs, 0);
                return true;
            }
            name = he->h_name ? he->h_name : "";
            type = he->h_addrtype;
            length = he->h_length;
            addrs.emplace_back(reinterpret_cast<const char*>(ad), a1);
        }
        // Layout: hostent, aliases {NULL}, addr_list {..., NULL}, addresses, name.
        const std::uint32_t na = static_cast<std::uint32_t>(addrs.size());
        const std::uint32_t need = 20 + 4 + (na + 1) * 4 + na * 16 + static_cast<std::uint32_t>(name.size() + 1);
        thread_local std::uint32_t buf = 0, buf_size = 0;
        if (buf_size < need) {
            if (buf) {
                GuestFree(buf);
            }
            buf = GuestMalloc(need);
            buf_size = buf ? need : 0;
        }
        if (!buf) {
            set_h_errno(3);  // NO_RECOVERY
            Ret(regs, 0);
            return true;
        }
        std::memset(H(buf), 0, need);
        const std::uint32_t aliases = buf + 20, list = aliases + 4, data = list + (na + 1) * 4,
                            nm = data + na * 16;
        for (std::uint32_t i = 0; i < na; i++) {
            const std::uint32_t at = data + i * 16;
            std::memcpy(H(at), addrs[i].data(), addrs[i].size() < 16 ? addrs[i].size() : 16);
            std::memcpy(H(list + i * 4), &at, 4);
        }
        std::memcpy(H(nm), name.c_str(), name.size() + 1);
        const std::uint32_t he32[5] = {nm, aliases, static_cast<std::uint32_t>(type),
                                       static_cast<std::uint32_t>(length), list};
        std::memcpy(H(buf), he32, 20);
        Ret(regs, buf);
        return true;
    }
    return true;
}

// Builds the APK's asset index once (the central directory: name -> stored
// or deflated entry); false when there is no APK or no entry.
bool Runtime::EnsureApkIndex() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (apk_indexed_) {
        return !apk_index_.empty();
    }
    apk_indexed_ = true;
    if (apk_path_.empty()) {
        return false;
    }
    FILE* f = std::fopen(apk_path_.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    const long scan = size > 65558 ? 65558 : size;
    std::vector<std::uint8_t> tail((std::size_t)scan);
    std::fseek(f, size - scan, SEEK_SET);
    if (std::fread(tail.data(), 1, tail.size(), f) != tail.size()) {
        std::fclose(f);
        return false;
    }
    long eocd = -1;
    for (long i = (long)tail.size() - 22; i >= 0; i--) {
        if (tail[(std::size_t)i] == 0x50 && tail[(std::size_t)i + 1] == 0x4B &&
            tail[(std::size_t)i + 2] == 0x05 && tail[(std::size_t)i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        std::fclose(f);
        return false;
    }
    auto rd16 = [&](long o) -> std::uint32_t {
        return (std::uint32_t)tail[(std::size_t)o] |
               ((std::uint32_t)tail[(std::size_t)o + 1] << 8);
    };
    auto rd32 = [&](long o) -> std::uint32_t {
        return rd16(o) | (rd16(o + 2) << 16);
    };
    const std::uint32_t count = rd16(eocd + 10);
    const std::uint32_t cdoff = rd32(eocd + 16);
    std::fclose(f);
    FILE* g = std::fopen(apk_path_.c_str(), "rb");
    if (!g) {
        return false;
    }
    for (std::uint32_t i = 0, off = cdoff; i < count; i++) {
        std::uint8_t hdr[46];
        std::fseek(g, (long)off, SEEK_SET);
        if (std::fread(hdr, 1, 46, g) != 46 || hdr[0] != 0x50 || hdr[1] != 0x4B ||
            hdr[2] != 0x01 || hdr[3] != 0x02) {
            break;
        }
        auto h16 = [&](int o) -> std::uint32_t { return hdr[o] | (hdr[o + 1] << 8); };
        auto h32 = [&](int o) -> std::uint32_t { return h16(o) | (h16(o + 2) << 16); };
        const std::uint32_t nlen = h16(28), elen = h16(30), clen = h16(32);
        std::string name(nlen, '\0');
        std::fseek(g, (long)(off + 46), SEEK_SET);
        if (nlen && std::fread(name.data(), 1, nlen, g) != nlen) {
            break;
        }
        off += 46 + nlen + elen + clen;
        ApkEntry e;
        e.name = name;
        e.method = h16(10);
        e.csize = h32(20);
        e.usize = h32(24);
        const std::uint32_t lho = h32(42);
        std::uint8_t lh[30];
        std::fseek(g, (long)lho, SEEK_SET);
        if (std::fread(lh, 1, 30, g) != 30 || lh[0] != 0x50 || lh[1] != 0x4B ||
            lh[2] != 0x03 || lh[3] != 0x04) {
            continue;
        }
        auto l16 = [&](int o) -> std::uint32_t { return lh[o] | (lh[o + 1] << 8); };
        e.data_off = (std::uint64_t)lho + 30 + l16(26) + l16(28);
        (void)elen;
        (void)clen;
        apk_index_[e.name] = e;
    }
    std::fclose(g);
    std::printf("[asset] indexed %zu entries from apk\n", apk_index_.size());
    return !apk_index_.empty();
}

std::uint32_t Runtime::NewGuestAssetManager() {
    if (!EnsureApkIndex()) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::uint32_t m = next_asset_mgr_++;
    asset_mgr_map_[m] = 1;
    return m;
}

std::uint32_t Runtime::PtrHandle(void* p) {
    if (!p) {
        return 0;
    }
    std::lock_guard<std::mutex> l(ptr_mu_);
    const auto it = ptr_fwd_.find(p);
    if (it != ptr_fwd_.end()) {
        return it->second;
    }
    std::uint32_t h = 0;
    if (!ptr_free_.empty()) {
        h = ptr_free_.back();
        ptr_free_.pop_back();
    } else {
        h = ptr_next_;
        ptr_next_ += 4;
    }
    ptr_fwd_[p] = h;
    ptr_rev_[h] = p;
    return h;
}

void* Runtime::PtrOf(std::uint32_t h) {
    std::lock_guard<std::mutex> l(ptr_mu_);
    const auto it = ptr_rev_.find(h);
    return it == ptr_rev_.end() ? nullptr : it->second;
}

void Runtime::DropHandle(std::uint32_t h) {
    std::lock_guard<std::mutex> l(ptr_mu_);
    const auto it = ptr_rev_.find(h);
    if (it == ptr_rev_.end()) {
        return;
    }
    ptr_fwd_.erase(it->second);
    ptr_rev_.erase(it);
    ptr_free_.push_back(h);
}

bool Runtime::NdkServes(const std::string& n) {
    static const std::set<std::string> kNames = {
        "ALooper_prepare", "ALooper_forThread", "ALooper_wake", "ALooper_pollAll",
        "ALooper_pollOnce", "AInputQueue_attachLooper", "AInputQueue_detachLooper",
        "AInputQueue_getEvent", "AInputQueue_preDispatchEvent", "AInputQueue_finishEvent",
        "AInputEvent_getType", "AInputEvent_getDeviceId", "AInputEvent_getSource",
        "AKeyEvent_getAction", "AKeyEvent_getKeyCode", "AKeyEvent_getMetaState",
        "AMotionEvent_getAction", "AMotionEvent_getFlags", "AMotionEvent_getMetaState",
        "AMotionEvent_getEdgeFlags", "AMotionEvent_getPointerCount", "AMotionEvent_getPointerId",
        "AMotionEvent_getHistorySize", "AMotionEvent_getDownTime", "AMotionEvent_getEventTime",
        "AMotionEvent_getHistoricalEventTime", "AMotionEvent_getXPrecision",
        "AMotionEvent_getYPrecision", "AMotionEvent_getX", "AMotionEvent_getY",
        "AMotionEvent_getRawX", "AMotionEvent_getRawY", "AMotionEvent_getPressure",
        "AMotionEvent_getSize", "AMotionEvent_getToolMajor", "AMotionEvent_getToolMinor",
        "AMotionEvent_getTouchMajor", "AMotionEvent_getTouchMinor", "AMotionEvent_getOrientation",
        "AMotionEvent_getHistoricalX", "AMotionEvent_getHistoricalY",
        "AMotionEvent_getHistoricalRawX", "AMotionEvent_getHistoricalRawY",
        "AMotionEvent_getHistoricalPressure", "AMotionEvent_getHistoricalSize",
        "AMotionEvent_getHistoricalTouchMajor", "AMotionEvent_getHistoricalTouchMinor",
        "AMotionEvent_getHistoricalToolMajor", "AMotionEvent_getHistoricalToolMinor",
        "AMotionEvent_getHistoricalOrientation", "AConfiguration_new", "AConfiguration_delete",
        "AConfiguration_fromAssetManager", "ANativeWindow_fromSurface", "ANativeWindow_acquire",
        "ANativeWindow_release", "ANativeWindow_setBuffersGeometry"};
    return kNames.count(n) != 0;
}

std::uint32_t Runtime::RuntimeImport(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const Import& imp : mod_.imports) {
        if (imp.is_function && imp.name == name) {
            return imp.call_va;
        }
    }
    const std::uint32_t va = mod_.next_tramp;
    // The last 4 KiB of the page hold the VFP thunks.
    if (mod_.imports.size() >= mod_.imports.capacity() ||
        va + 4 > kTrampBase + kTrampSize - 0x1000) {
        std::printf("[dl] LOUD no room for a run-time import of %s\n", name.c_str());
        return 0;
    }
    Import imp;
    imp.name = name;
    imp.is_function = true;
    imp.tramp_va = imp.call_va = va;
    imp.blocking = BlockingImportName(name);
    imp.hot = HotImportForName(name);
    imp.fast_math = FastMathForName(name.c_str());
    imp.gl_id = GlIdForName(name.c_str());
    imp.import_id = ImportIdForName(name.c_str());
    const std::uint32_t swi = static_cast<std::uint32_t>(mod_.imports.size()) + 1;  // swi = idx + 1
    const std::uint32_t insn = 0xEF000000u | swi;
    mod_.imports.push_back(imp);  // before the address is handed out
    mem_.Protect(kTrampBase, kTrampSize, PROT_READ | PROT_WRITE);
    std::memcpy(H(va), &insn, 4);
    mem_.Protect(kTrampBase, kTrampSize, PROT_READ | PROT_EXEC);
    mod_.next_tramp = va + 4;
    std::printf("[dl] run-time import %s at %08x\n", name.c_str(), va);
    return va;
}

bool Runtime::NdkCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs) {
    using I = ImportId;
    const bool ours = n.compare(0, 7, "ALooper") == 0 || n.compare(0, 11, "AInputQueue") == 0 ||
                      n.compare(0, 11, "AInputEvent") == 0 || n.compare(0, 12, "AMotionEvent") == 0 ||
                      n.compare(0, 9, "AKeyEvent") == 0 || n.compare(0, 14, "AConfiguration") == 0 ||
                      n.compare(0, 13, "ANativeWindow") == 0 || n.compare(0, 7, "ASensor") == 0;
    if (!ours) {
        return false;
    }
#ifndef __ANDROID__
    // No Android framework on the PC: every NDK object is absent.
    std::printf("[ndk] %s: no NDK on this host -> 0\n", n.c_str());
    Ret(regs, 0);
    return true;
#else
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    auto ret_f = [&](float f) {
        std::uint32_t w = 0;
        std::memcpy(&w, &f, 4);
        Ret(regs, w);
    };
    auto ret_64 = [&](std::int64_t v) {
        Ret(regs, static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32));
    };
    auto out32 = [&](std::uint32_t at, std::uint32_t v) {
        if (at && CheckRange(at, 4, true)) {
            std::memcpy(H(at), &v, 4);
        }
    };
    auto ev = [&]() { return static_cast<const AInputEvent*>(PtrOf(a0)); };
    // --- looper
    if (id == I::ALooper_prepare_) {
        Ret(regs, PtrHandle(::ALooper_prepare(static_cast<int>(a0))));
        return true;
    }
    if (id == I::ALooper_forThread_) {
        Ret(regs, PtrHandle(::ALooper_forThread()));
        return true;
    }
    if (id == I::ALooper_wake_) {
        if (auto* l = static_cast<ALooper*>(PtrOf(a0))) {
            ::ALooper_wake(l);
        }
        RetVoid(regs);
        return true;
    }
    if (id == I::ALooper_pollAll_ || id == I::ALooper_pollOnce_) {
        // (timeout, int* outFd, int* outEvents, void** outData): the data is
        // the guest word given at registration, returned as it was.
        int fd = 0, events = 0;
        void* data = nullptr;
        // pollAll is pollOnce repeated while it only ran callbacks (the
        // NDK no longer declares pollAll itself).
        int r = 0;
        do {
            r = ::ALooper_pollOnce(static_cast<int>(a0), &fd, &events, &data);
        } while (id == I::ALooper_pollAll_ && r == ALOOPER_POLL_CALLBACK);
        out32(a1, static_cast<std::uint32_t>(fd));
        out32(a2, static_cast<std::uint32_t>(events));
        out32(a3, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data)));
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    }
    // --- input queue
    if (id == I::AInputQueue_attachLooper_) {
        // (queue, looper, ident, callback, data@[sp]). A guest callback
        // cannot be a host ALooper callback: the queue is polled by ident.
        std::uint32_t data = 0;
        if (CheckRange(regs[13], 4, false)) {
            std::memcpy(&data, H(regs[13]), 4);
        }
        if (a3 != 0) {
            std::printf("[ndk] LOUD AInputQueue_attachLooper with a callback (%08x): polled by "
                        "ident %d instead\n", a3, static_cast<int>(a2));
        }
        auto* q = static_cast<AInputQueue*>(PtrOf(a0));
        auto* l = static_cast<ALooper*>(PtrOf(a1));
        if (q && l) {
            ::AInputQueue_attachLooper(q, l, static_cast<int>(a2), nullptr,
                                       reinterpret_cast<void*>(static_cast<std::uintptr_t>(data)));
        }
        RetVoid(regs);
        return true;
    }
    if (id == I::AInputQueue_detachLooper_) {
        if (auto* q = static_cast<AInputQueue*>(PtrOf(a0))) {
            ::AInputQueue_detachLooper(q);
        }
        RetVoid(regs);
        return true;
    }
    if (id == I::AInputQueue_getEvent_) {
        auto* q = static_cast<AInputQueue*>(PtrOf(a0));
        AInputEvent* e = nullptr;
        const int r = q ? ::AInputQueue_getEvent(q, &e) : -1;
        out32(a1, r >= 0 ? PtrHandle(e) : 0);
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    }
    if (id == I::AInputQueue_preDispatchEvent_) {
        auto* q = static_cast<AInputQueue*>(PtrOf(a0));
        auto* e = static_cast<AInputEvent*>(PtrOf(a1));
        const int r = (q && e) ? ::AInputQueue_preDispatchEvent(q, e) : 0;
        if (r != 0) {
            DropHandle(a1);  // the IME took it; the system finishes it
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    }
    if (id == I::AInputQueue_finishEvent_) {
        auto* q = static_cast<AInputQueue*>(PtrOf(a0));
        auto* e = static_cast<AInputEvent*>(PtrOf(a1));
        if (q && e) {
            ::AInputQueue_finishEvent(q, e, static_cast<int>(a2));
        }
        DropHandle(a1);
        RetVoid(regs);
        return true;
    }
    // --- input events (int32 in r0, int64 in r0:r1, float bits in r0)
    if (id == I::AInputEvent_getType_ || id == I::AInputEvent_getDeviceId_ || id == I::AInputEvent_getSource_) {
        const AInputEvent* e = ev();
        const std::int32_t v = !e ? 0
                               : id == I::AInputEvent_getType_     ? ::AInputEvent_getType(e)
                               : id == I::AInputEvent_getDeviceId_ ? ::AInputEvent_getDeviceId(e)
                                                                : ::AInputEvent_getSource(e);
        Ret(regs, static_cast<std::uint32_t>(v));
        return true;
    }
    if (id == I::AKeyEvent_getAction_ || id == I::AKeyEvent_getKeyCode_ || id == I::AKeyEvent_getMetaState_) {
        const AInputEvent* e = ev();
        const std::int32_t v = !e ? 0
                               : id == I::AKeyEvent_getAction_  ? ::AKeyEvent_getAction(e)
                               : id == I::AKeyEvent_getKeyCode_ ? ::AKeyEvent_getKeyCode(e)
                                                             : ::AKeyEvent_getMetaState(e);
        Ret(regs, static_cast<std::uint32_t>(v));
        return true;
    }
    if (n.compare(0, 12, "AMotionEvent") == 0) {
        const AInputEvent* e = ev();
        if (!e) {
            Ret(regs, 0);
            return true;
        }
        const std::size_t i = a1, h = a2;
        if (id == I::AMotionEvent_getAction_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getAction(e))); return true; }
        if (id == I::AMotionEvent_getFlags_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getFlags(e))); return true; }
        if (id == I::AMotionEvent_getMetaState_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getMetaState(e))); return true; }
        if (id == I::AMotionEvent_getEdgeFlags_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getEdgeFlags(e))); return true; }
        if (id == I::AMotionEvent_getPointerCount_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getPointerCount(e))); return true; }
        if (id == I::AMotionEvent_getPointerId_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getPointerId(e, i))); return true; }
        if (id == I::AMotionEvent_getHistorySize_) { Ret(regs, static_cast<std::uint32_t>(::AMotionEvent_getHistorySize(e))); return true; }
        if (id == I::AMotionEvent_getDownTime_) { ret_64(::AMotionEvent_getDownTime(e)); return true; }
        if (id == I::AMotionEvent_getEventTime_) { ret_64(::AMotionEvent_getEventTime(e)); return true; }
        if (id == I::AMotionEvent_getHistoricalEventTime_) { ret_64(::AMotionEvent_getHistoricalEventTime(e, i)); return true; }
        if (id == I::AMotionEvent_getXPrecision_) { ret_f(::AMotionEvent_getXPrecision(e)); return true; }
        if (id == I::AMotionEvent_getYPrecision_) { ret_f(::AMotionEvent_getYPrecision(e)); return true; }
        if (id == I::AMotionEvent_getX_) { ret_f(::AMotionEvent_getX(e, i)); return true; }
        if (id == I::AMotionEvent_getY_) { ret_f(::AMotionEvent_getY(e, i)); return true; }
        if (id == I::AMotionEvent_getPressure_) { ret_f(::AMotionEvent_getPressure(e, i)); return true; }
        if (id == I::AMotionEvent_getSize_) { ret_f(::AMotionEvent_getSize(e, i)); return true; }
        if (id == I::AMotionEvent_getToolMajor_) { ret_f(::AMotionEvent_getToolMajor(e, i)); return true; }
        if (id == I::AMotionEvent_getToolMinor_) { ret_f(::AMotionEvent_getToolMinor(e, i)); return true; }
        if (id == I::AMotionEvent_getTouchMajor_) { ret_f(::AMotionEvent_getTouchMajor(e, i)); return true; }
        if (id == I::AMotionEvent_getTouchMinor_) { ret_f(::AMotionEvent_getTouchMinor(e, i)); return true; }
        if (id == I::AMotionEvent_getOrientation_) { ret_f(::AMotionEvent_getOrientation(e, i)); return true; }
        if (id == I::AMotionEvent_getHistoricalX_) { ret_f(::AMotionEvent_getHistoricalX(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalY_) { ret_f(::AMotionEvent_getHistoricalY(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalPressure_) { ret_f(::AMotionEvent_getHistoricalPressure(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalSize_) { ret_f(::AMotionEvent_getHistoricalSize(e, i, h)); return true; }
        if (id == I::AMotionEvent_getRawX_) { ret_f(::AMotionEvent_getRawX(e, i)); return true; }
        if (id == I::AMotionEvent_getRawY_) { ret_f(::AMotionEvent_getRawY(e, i)); return true; }
        if (id == I::AMotionEvent_getHistoricalRawX_) { ret_f(::AMotionEvent_getHistoricalRawX(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalRawY_) { ret_f(::AMotionEvent_getHistoricalRawY(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalTouchMajor_) { ret_f(::AMotionEvent_getHistoricalTouchMajor(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalTouchMinor_) { ret_f(::AMotionEvent_getHistoricalTouchMinor(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalToolMajor_) { ret_f(::AMotionEvent_getHistoricalToolMajor(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalToolMinor_) { ret_f(::AMotionEvent_getHistoricalToolMinor(e, i, h)); return true; }
        if (id == I::AMotionEvent_getHistoricalOrientation_) { ret_f(::AMotionEvent_getHistoricalOrientation(e, i, h)); return true; }
    }
    // --- configuration
    if (id == I::AConfiguration_new_) {
        Ret(regs, PtrHandle(::AConfiguration_new()));
        return true;
    }
    if (id == I::AConfiguration_delete_) {
        if (auto* c = static_cast<AConfiguration*>(PtrOf(a0))) {
            ::AConfiguration_delete(c);
        }
        DropHandle(a0);
        RetVoid(regs);
        return true;
    }
    if (id == I::AConfiguration_fromAssetManager_) {
        auto* c = static_cast<AConfiguration*>(PtrOf(a0));
        void* mgr = nullptr;
        {
            std::lock_guard<std::mutex> l(ptr_mu_);
            const auto it = real_asset_mgrs_.find(a1);
            mgr = it == real_asset_mgrs_.end() ? nullptr : it->second;
        }
        if (c && mgr) {
            ::AConfiguration_fromAssetManager(c, static_cast<AAssetManager*>(mgr));
        }
        RetVoid(regs);
        return true;
    }
    // --- native window
    if (id == I::ANativeWindow_fromSurface_) {
        // (JNIEnv*, jobject surface): the real env of this thread, the real
        // Surface behind the guest's handle.
        JNIEnv* re = GuestCallbacks::g_real_env_hook
                         ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                         : nullptr;
        void* surface = UnwrapObject(a1);
        ANativeWindow* w = (re && surface) ? ::ANativeWindow_fromSurface(re, static_cast<jobject>(surface)) : nullptr;
        Ret(regs, PtrHandle(w));
        return true;
    }
    if (id == I::ANativeWindow_acquire_ || id == I::ANativeWindow_release_) {
        if (auto* w = static_cast<ANativeWindow*>(PtrOf(a0))) {
            if (id == I::ANativeWindow_acquire_) {
                ::ANativeWindow_acquire(w);
            } else {
                ::ANativeWindow_release(w);  // the handle stays: the window may live on
            }
        }
        RetVoid(regs);
        return true;
    }
    if (id == I::ANativeWindow_setBuffersGeometry_) {
        auto* w = static_cast<ANativeWindow*>(PtrOf(a0));
        Ret(regs, static_cast<std::uint32_t>(
                      w ? ::ANativeWindow_setBuffersGeometry(w, static_cast<int32_t>(a1),
                                                             static_cast<int32_t>(a2), static_cast<int32_t>(a3))
                        : -22));
        return true;
    }
    // --- sensors: none is offered (the device reads as having no sensor
    //     of any type); the event queue itself is real and stays empty.
    {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            std::printf("[ndk] sensors: none offered to the guest\n");
        }
    }
    if (id == I::ASensorManager_getInstance_) {
        Ret(regs, PtrHandle(::ASensorManager_getInstance()));
        return true;
    }
    if (id == I::ASensorManager_getSensorList_) {
        out32(a1, 0);
        Ret(regs, 0);
        return true;
    }
    if (id == I::ASensorManager_createEventQueue_) {
        // (manager, looper, ident, callback, data@[sp])
        std::uint32_t data = 0;
        if (CheckRange(regs[13], 4, false)) {
            std::memcpy(&data, H(regs[13]), 4);
        }
        auto* m = static_cast<ASensorManager*>(PtrOf(a0));
        auto* l = static_cast<ALooper*>(PtrOf(a1));
        ASensorEventQueue* q = (m && l) ? ::ASensorManager_createEventQueue(
                                              m, l, static_cast<int>(a2), nullptr,
                                              reinterpret_cast<void*>(static_cast<std::uintptr_t>(data)))
                                        : nullptr;
        Ret(regs, PtrHandle(q));
        return true;
    }
    if (id == I::ASensorManager_destroyEventQueue_) {
        auto* m = static_cast<ASensorManager*>(PtrOf(a0));
        auto* q = static_cast<ASensorEventQueue*>(PtrOf(a1));
        const int r = (m && q) ? ::ASensorManager_destroyEventQueue(m, q) : -22;
        DropHandle(a1);
        Ret(regs, static_cast<std::uint32_t>(r));
        return true;
    }
    if (id == I::ASensorEventQueue_hasEvents_ || id == I::ASensorEventQueue_getEvents_ ||
        id == I::ASensorManager_getDefaultSensor_) {
        Ret(regs, 0);  // no sensor: no events
        return true;
    }
    // enable/disable/setEventRate on a sensor the guest cannot have, and
    // the ASensor_get* accessors of one.
    Ret(regs, n.compare(0, 8, "ASensor_") == 0 ? 0u : static_cast<std::uint32_t>(-22));
    return true;
#endif
}

bool Runtime::EglCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs) {
    using I = ImportId;
    if (n.compare(0, 3, "egl") != 0) {
        return false;
    }
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    auto bad = [&](const char* what) {
        std::printf("[egl] LOUD %s: bad guest pointer -- halting\n", what);
        HaltGuest();
        return true;
    };
    // A guest attribute list, EGL_NONE-terminated (at most 64 pairs).
    auto attribs = [&](std::uint32_t at, std::vector<EGLint>& out) -> bool {
        out.clear();
        if (at == 0) {
            return true;
        }
        for (std::uint32_t i = 0; i < 129; i++) {
            if (!CheckRange(at + i * 4, 4, false)) {
                return false;
            }
            EGLint v = 0;
            std::memcpy(&v, H(at + i * 4), 4);
            out.push_back(v);
            if (v == EGL_NONE && (i % 2) == 0) {
                return true;
            }
        }
        return false;
    };
    auto out32 = [&](std::uint32_t at, std::uint32_t v) -> bool {
        if (at == 0) {
            return true;
        }
        if (!CheckRange(at, 4, true)) {
            return false;
        }
        std::memcpy(H(at), &v, 4);
        return true;
    };
    auto dpy = [&](std::uint32_t h) { return static_cast<EGLDisplay>(PtrOf(h)); };
    if (id == I::eglGetDisplay_) {
        // Only the default display exists for an app.
        Ret(regs, PtrHandle(::eglGetDisplay(EGL_DEFAULT_DISPLAY)));
        return true;
    }
    if (id == I::eglInitialize_) {
        EGLint major = 0, minor = 0;
        const EGLBoolean r = ::eglInitialize(dpy(a0), &major, &minor);
        if (!out32(a1, static_cast<std::uint32_t>(major)) || !out32(a2, static_cast<std::uint32_t>(minor))) {
            return bad("eglInitialize");
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::eglTerminate_) {
        Ret(regs, ::eglTerminate(dpy(a0)));
        return true;
    }
    if (id == I::eglGetError_) {
        Ret(regs, static_cast<std::uint32_t>(::eglGetError()));
        return true;
    }
    if (id == I::eglChooseConfig_) {
        // (dpy, attrib_list, EGLConfig* configs, config_size, EGLint* num@[sp])
        std::vector<EGLint> al;
        std::uint32_t num_ptr = 0;
        if (!attribs(a1, al) || !CheckRange(regs[13], 4, false)) {
            return bad("eglChooseConfig");
        }
        std::memcpy(&num_ptr, H(regs[13]), 4);
        const EGLint size = static_cast<EGLint>(a3);
        if (a2 && (size < 0 || size > 4096 || !CheckRange(a2, static_cast<std::uint32_t>(size) * 4u, true))) {
            return bad("eglChooseConfig configs");
        }
        std::vector<EGLConfig> cfg(a2 && size > 0 ? static_cast<std::size_t>(size) : 0);
        EGLint num = 0;
        const EGLBoolean r = ::eglChooseConfig(dpy(a0), al.empty() ? nullptr : al.data(),
                                               a2 ? cfg.data() : nullptr, a2 ? size : 0, &num);
        if (r && a2) {
            for (EGLint i = 0; i < num && i < size; i++) {
                const std::uint32_t h = PtrHandle(cfg[static_cast<std::size_t>(i)]);
                std::memcpy(H(a2 + static_cast<std::uint32_t>(i) * 4u), &h, 4);
            }
        }
        if (!out32(num_ptr, static_cast<std::uint32_t>(num))) {
            return bad("eglChooseConfig num");
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::eglGetConfigAttrib_) {
        EGLint v = 0;
        const EGLBoolean r = ::eglGetConfigAttrib(dpy(a0), static_cast<EGLConfig>(PtrOf(a1)),
                                                  static_cast<EGLint>(a2), &v);
        if (!out32(a3, static_cast<std::uint32_t>(v))) {
            return bad("eglGetConfigAttrib");
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::eglCreateWindowSurface_ || id == I::eglCreateContext_) {
        // (dpy, config, window | share_context, attrib_list)
        std::vector<EGLint> al;
        if (!attribs(a3, al)) {
            return bad(n.c_str());
        }
        void* third = PtrOf(a2);
        void* r = nullptr;
        if (id == I::eglCreateWindowSurface_) {
            r = ::eglCreateWindowSurface(dpy(a0), static_cast<EGLConfig>(PtrOf(a1)),
                                         reinterpret_cast<EGLNativeWindowType>(third),
                                         al.empty() ? nullptr : al.data());
        } else {
            r = ::eglCreateContext(dpy(a0), static_cast<EGLConfig>(PtrOf(a1)),
                                   static_cast<EGLContext>(third), al.empty() ? nullptr : al.data());
        }
        std::printf("[egl] %s -> %s (0x%x)\n", n.c_str(), r ? "ok" : "FAILED", r ? 0 : ::eglGetError());
        Ret(regs, PtrHandle(r));
        return true;
    }
    if (id == I::eglDestroySurface_ || id == I::eglDestroyContext_) {
        void* obj = PtrOf(a1);
        const EGLBoolean r = id == I::eglDestroySurface_
                                 ? ::eglDestroySurface(dpy(a0), static_cast<EGLSurface>(obj))
                                 : ::eglDestroyContext(dpy(a0), static_cast<EGLContext>(obj));
        if (r) {
            DropHandle(a1);
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::eglMakeCurrent_) {
        ForgetGlBindings();
        const EGLBoolean r =
            ::eglMakeCurrent(dpy(a0), static_cast<EGLSurface>(PtrOf(a1)), static_cast<EGLSurface>(PtrOf(a2)),
                             static_cast<EGLContext>(PtrOf(a3)));
        std::printf("[egl] eglMakeCurrent(ctx %08x) on t%u -> %u\n", a3, GuestTid(), r);
        Ret(regs, r);
        return true;
    }
    if (id == I::eglSwapBuffers_) {
        egl_swaps_.fetch_add(1, std::memory_order_relaxed);
        Ret(regs, ::eglSwapBuffers(dpy(a0), static_cast<EGLSurface>(PtrOf(a1))));
        return true;
    }
    if (id == I::eglGetConfigs_) {
        // (dpy, EGLConfig* configs, config_size, EGLint* num)
        const EGLint size = static_cast<EGLint>(a2);
        if (a1 && (size < 0 || size > 4096 || !CheckRange(a1, static_cast<std::uint32_t>(size) * 4u, true))) {
            return bad("eglGetConfigs configs");
        }
        std::vector<EGLConfig> cfg(a1 && size > 0 ? static_cast<std::size_t>(size) : 0);
        EGLint num = 0;
        const EGLBoolean r = ::eglGetConfigs(dpy(a0), a1 ? cfg.data() : nullptr, a1 ? size : 0, &num);
        if (r && a1) {
            for (EGLint i = 0; i < num && i < size; i++) {
                const std::uint32_t h = PtrHandle(cfg[static_cast<std::size_t>(i)]);
                std::memcpy(H(a1 + static_cast<std::uint32_t>(i) * 4u), &h, 4);
            }
        }
        if (!out32(a3, static_cast<std::uint32_t>(num))) {
            return bad("eglGetConfigs num");
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::eglGetCurrentDisplay_) {
        Ret(regs, PtrHandle(::eglGetCurrentDisplay()));
        return true;
    }
    if (id == I::eglGetCurrentContext_) {
        Ret(regs, PtrHandle(::eglGetCurrentContext()));
        return true;
    }
    if (id == I::eglGetCurrentSurface_) {
        Ret(regs, PtrHandle(::eglGetCurrentSurface(static_cast<EGLint>(a0))));
        return true;
    }
    if (id == I::eglQuerySurface_ || id == I::eglQueryContext_) {
        // (dpy, surface | context, attribute, EGLint* value)
        EGLint v = 0;
        const EGLBoolean r =
            id == I::eglQuerySurface_
                ? ::eglQuerySurface(dpy(a0), static_cast<EGLSurface>(PtrOf(a1)), static_cast<EGLint>(a2), &v)
                : ::eglQueryContext(dpy(a0), static_cast<EGLContext>(PtrOf(a1)), static_cast<EGLint>(a2), &v);
        if (!out32(a3, static_cast<std::uint32_t>(v))) {
            return bad(n.c_str());
        }
        Ret(regs, r);
        return true;
    }
    if (id == I::eglSwapInterval_) {
        Ret(regs, ::eglSwapInterval(dpy(a0), static_cast<EGLint>(a1)));
        return true;
    }
    if (id == I::eglWaitGL_ || id == I::eglWaitNative_ || id == I::eglReleaseThread_) {
        if (id == I::eglReleaseThread_) {
            ForgetGlBindings();
        }
        Ret(regs, id == I::eglWaitGL_ ? ::eglWaitGL() : id == I::eglWaitNative_ ? ::eglWaitNative(static_cast<EGLint>(a0))
                                                                         : ::eglReleaseThread());
        return true;
    }
    if (id == I::eglQueryString_) {
        // (dpy, name): one stable guest copy per name, as the driver's
        // strings do not change for a display.
        static std::map<std::uint32_t, std::uint32_t> copies;
        const char* str = ::eglQueryString(dpy(a0), static_cast<EGLint>(a1));
        std::uint32_t out = 0;
        if (str) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto it = copies.find(a1);
            if (it == copies.end()) {
                const std::uint32_t len = static_cast<std::uint32_t>(std::strlen(str)) + 1;
                const std::uint32_t g = GuestMalloc(len);
                if (g) {
                    std::memcpy(H(g), str, len);
                    it = copies.emplace(a1, g).first;
                }
            }
            out = it != copies.end() ? it->second : 0;
        }
        Ret(regs, out);
        return true;
    }
    if (id == I::eglBindAPI_) {
        // (api): the host's contexts are OpenGL ES ones; any other API fails
        // as on a device without it (EGL_OPENGL_ES_API = 0x30A0).
        Ret(regs, a0 == 0x30A0 ? 1u : 0u);
        return true;
    }
    if (id == I::eglGetProcAddress_) {
        // Extension entry points are not offered: NULL, as for an extension
        // the driver lacks. Named once each, so a port sees what was wanted.
        static std::set<std::string> asked;
        const std::string name = CheckRange(a0, 1, false) ? GuestStr(a0) : "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (asked.insert(name).second) {
                std::printf("[egl] eglGetProcAddress(\"%s\") -> NULL (not offered)\n", name.c_str());
            }
        }
        Ret(regs, 0);
        return true;
    }
    std::printf("[egl] LOUD %s is not offered -> 0\n", n.c_str());
    Ret(regs, 0);
    return true;
}

// bionic's __cxa_guard (ARM): 0 = not constructed, bit 0 = constructed; we
// mark "under construction" with 0x100 (the guard's other bits are the
// implementation's own, and every guest library calls this one). A thread
// that finds the object under construction waits for release or abort;
// while guest signals are in use it waits in slices and runs its handlers
// in between, with guard_mu_ released (a GC may stop the constructing
// thread and needs this one's ack).
std::uint32_t Runtime::CxaGuard(ImportId id, const std::string& n, std::uint32_t guard) {
    using I = ImportId;
    constexpr std::uint32_t kDone = 1, kUnderway = 0x100;
    if (!CheckRange(guard, 4, true) || (guard & 3u)) {
        return kSemBadPtr;
    }
    std::uint32_t* const g = H<std::uint32_t>(guard);
    std::unique_lock<std::mutex> l(guard_mu_);
    if (id == I::__cxa_guard_release_ || id == I::__cxa_guard_abort_) {
        __atomic_store_n(g, id == I::__cxa_guard_release_ ? kDone : 0u, __ATOMIC_RELEASE);
        guard_cv_.notify_all();
        return 0;
    }
    for (;;) {
        const std::uint32_t v = __atomic_load_n(g, __ATOMIC_ACQUIRE);
        if (v & kDone) {
            return 0;
        }
        if (v == 0) {
            __atomic_store_n(g, kUnderway, __ATOMIC_RELAXED);
            return 1;
        }
        if (!SignalsInUse()) {
            guard_cv_.wait(l);
            continue;
        }
        guard_cv_.wait_for(l, std::chrono::nanoseconds(kSigSliceNs));
        if (SignalDeliverable()) {
            l.unlock();
            DeliverPendingSignals();
            l.lock();
        }
    }
}

// Guest sleeps: while guest signals are in use, in slices with handlers run
// in between (the sleep then goes on, as SA_RESTART would have it).
static void GuestSleepUs(std::uint64_t us) {
    if (!t_rt || !t_rt->SignalsInUse()) {
        GuestSleepUsPlain(us);
        return;
    }
    const std::uint64_t slice_us = static_cast<std::uint64_t>(Runtime::kSigSliceNs / 1000);
    while (us > 0) {
        const std::uint64_t now = us < slice_us ? us : slice_us;
        GuestSleepUsPlain(now);
        us -= now;
        if (t_rt->SignalDeliverable()) {
            t_rt->DeliverPendingSignals();
        }
    }
}

bool Runtime::SignalDeliverable() const {
    return t_sig && (t_sig->pending.load(std::memory_order_acquire) &
                     ~t_sig->blocked.load(std::memory_order_relaxed)) != 0;
}

bool Runtime::DeliverPendingSignals() {
    if (!t_sig) {
        return true;
    }
    // One handler at a time per guest thread, whether it runs on the thread
    // itself or on a proxy (recursive: a handler's own waits deliver more).
    const std::shared_ptr<SigThread> me = t_sig;
    std::lock_guard<std::recursive_mutex> tok(me->token);
    for (;;) {
        const std::uint32_t avail = t_sig->pending.load(std::memory_order_acquire) &
                                    ~t_sig->blocked.load(std::memory_order_relaxed);
        if (avail == 0) {
            return true;
        }
        const std::uint32_t sig = static_cast<std::uint32_t>(__builtin_ctz(avail)) + 1;
        t_sig->pending.fetch_and(~kSigBit(sig), std::memory_order_acq_rel);
        GuestSigAct act;
        {
            std::lock_guard<std::mutex> l(sig_mu_);
            act = sig_acts_[sig];
            if (act.handler > 1 && (act.flags & 0x80000000u)) {
                sig_acts_[sig] = GuestSigAct{};  // SA_RESETHAND
            }
        }
        if (act.handler == 1 || (act.handler == 0 && SigDefaultIgnores(sig))) {
            continue;  // SIG_IGN, or ignored by default
        }
        if (act.handler == 0) {
            std::printf("[signal] FATAL signal %u with the default action -- halting\n", sig);
            HaltGuest();
            return false;
        }
        Dynarmic::A32::Jit* cur = t_hot.active_jit;
        if (!cur && !t_proxy_regs) {
            return true;
        }
        const std::array<std::uint32_t, 16>* const proxy_regs = t_proxy_regs;
        const auto& regs = proxy_regs ? *proxy_regs : cur->Regs();
        std::uint32_t saved[8];
        for (int i = 0; i < 8; i++) {
            saved[i] = regs[4 + i];
        }
        // Frame below the interrupted sp (128-byte red zone): siginfo (128 B)
        // and a ucontext whose mcontext holds the interrupted registers.
        constexpr std::uint32_t kInfo = 128, kUc = 0x300;
        const std::uint32_t info = (regs[13] - 128 - kInfo) & ~7u;
        const std::uint32_t uc = (info - kUc) & ~7u;
        if (!CheckRange(uc, kUc + kInfo, true)) {
            std::printf("[signal] LOUD no room for a signal frame below sp=%08x -- halting\n",
                        regs[13]);
            HaltGuest();
            return false;
        }
        std::memset(H(uc), 0, kUc + kInfo);
        const std::int32_t si[3] = {static_cast<std::int32_t>(sig), 0, -6 /*SI_TKILL*/};
        std::memcpy(H(info), si, sizeof(si));
        // ucontext: uc_flags, uc_link, uc_stack (12 B), then mcontext:
        // trap_no, error_code, oldmask, r0-r15, cpsr, fault_address.
        const std::uint32_t old_blocked = t_sig->blocked.load(std::memory_order_relaxed);
        std::memcpy(H(uc + 28), &old_blocked, 4);
        for (int i = 0; i < 16; i++) {
            std::memcpy(H(uc + 32 + 4 * i), &regs[i], 4);
        }
        const std::uint32_t cpsr = proxy_regs ? t_proxy_cpsr : cur->Cpsr();
        std::memcpy(H(uc + 96), &cpsr, 4);
        std::memcpy(H(uc + 104), &old_blocked, 4);  // uc_sigmask
        std::uint32_t during = old_blocked | act.mask;
        if (!(act.flags & 0x40000000u)) {
            during |= kSigBit(sig);  // SA_NODEFER clear: block it while it runs
        }
        t_sig->blocked.store(during & ~kUnblockable, std::memory_order_relaxed);
        t_proxy_regs = nullptr;  // deliveries inside the handler use its own frame
        const bool ok = RunNestedOnStack(act.handler, sig, info, uc, uc & ~7u, saved);
        t_proxy_regs = proxy_regs;
        t_sig->blocked.store(old_blocked, std::memory_order_relaxed);
        if (!ok) {
            std::printf("[signal] LOUD handler %08x for signal %u did not return (hr=%d) -- "
                        "halting\n", act.handler, sig, cb_last_hr_);
            HaltGuest();
            return false;
        }
    }
}

std::uint32_t Runtime::GuestSigaction(std::uint32_t sig, std::uint32_t act, std::uint32_t old) {
    // arm32 struct sigaction: { handler; sa_mask (32 bits); sa_flags; sa_restorer; }.
    if (sig < 1 || sig > 64 || (act && (sig == 9 || sig == 19))) {
        return EINVAL;
    }
    if ((old && !CheckRange(old, 16, true)) || (act && !CheckRange(act, 16, false))) {
        return EFAULT;
    }
    if (sig > 32) {
        // Real-time signals: valid, but no guest sends them (the 32-bit
        // sigset_t cannot name them); accepted as before, nothing kept.
        if (old) {
            std::memset(H(old), 0, 16);
        }
        return 0;
    }
    std::lock_guard<std::mutex> l(sig_mu_);
    if (old) {
        const std::uint32_t o[4] = {sig_acts_[sig].handler, sig_acts_[sig].mask,
                                    sig_acts_[sig].flags, 0};
        std::memcpy(H(old), o, 16);
    }
    if (act) {
        std::uint32_t a[4];
        std::memcpy(a, H(act), 16);
        sig_acts_[sig] = GuestSigAct{a[0], a[1] & ~kUnblockable, a[2]};
        // Interruptible (sliced) waits start with the first signal SENT
        // (GuestSendSignal), not here. 906d206 had turned them on at the
        // first non-fault handler install because Mono's finalizer, blocked
        // in a plain sem_wait, never saw Boehm's suspend signal; the proxy
        // delivery (81bdbc3) has run the handler for such a thread since. A
        // handler installed and never used (MCSM's SIGINT) left ~45 idle
        // workers waking every 2 ms (~1500 times a second each).
    }
    return 0;
}

std::uint32_t Runtime::GuestSigmask(std::uint32_t how, std::uint32_t set, std::uint32_t old) {
    if ((old && !CheckRange(old, 4, true)) || (set && !CheckRange(set, 4, false))) {
        return EFAULT;
    }
    if (!t_sig) {
        // A thread the runtime does not track: nothing can be pending for it.
        if (old) {
            std::memset(H(old), 0, 4);
        }
        return 0;
    }
    const std::uint32_t cur = t_sig->blocked.load(std::memory_order_relaxed);
    if (old) {
        std::memcpy(H(old), &cur, 4);
    }
    if (!set) {
        return 0;
    }
    std::uint32_t m = 0;
    std::memcpy(&m, H(set), 4);
    std::uint32_t next = cur;
    if (how == 0) {
        next = cur | m;
    } else if (how == 1) {
        next = cur & ~m;
    } else if (how == 2) {
        next = m;
    } else {
        return EINVAL;
    }
    t_sig->blocked.store(next & ~kUnblockable, std::memory_order_relaxed);
    if (SignalDeliverable()) {
        if (Dynarmic::A32::Jit* j = t_hot.active_jit) {
            j->HaltExecution(kSignalHalt);  // deliver once back in guest code
        }
    }
    return 0;
}

std::uint32_t Runtime::GuestSendSignal(std::uint32_t tid, std::uint32_t sig) {
    if (sig > 32) {
        return EINVAL;
    }
    // Under sig_mu_ throughout: ForgetSigThreadJit takes it too, so the
    // target's Jit cannot be deleted while it is being stopped here.
    std::lock_guard<std::mutex> l(sig_mu_);
    const auto it = sig_threads_.find(tid);
    if (it == sig_threads_.end()) {
        return ESRCH;
    }
    if (sig == 0) {
        return 0;  // existence check
    }
    const GuestSigAct act = sig_acts_[sig];
    if (act.handler == 1 || (act.handler == 0 && SigDefaultIgnores(sig))) {
        return 0;  // discarded at generation, as the kernel does
    }
    if (!signals_in_use_.exchange(true, std::memory_order_acq_rel)) {
        std::printf("[signal] first signal sent (%u to tid %u): interruptible waits on\n", sig, tid);
    }
    const std::shared_ptr<SigThread> st = it->second;
    st->pending.fetch_or(kSigBit(sig), std::memory_order_acq_rel);
    if (Dynarmic::A32::Jit* j = st->jit.load(std::memory_order_acquire)) {
        j->HaltExecution(kSignalHalt);  // taken at its next block boundary
    }
    // Checked after the halt: a thread that went outside meanwhile is not
    // missed (a proxy finding nothing left to deliver is harmless).
    if (st->outside.load(std::memory_order_acquire) &&
        !st->proxy.exchange(true, std::memory_order_acq_rel)) {
        try {
            std::thread([this, st, tid]() { ProxyDeliver(st, tid); }).detach();
        } catch (const std::system_error&) {
            st->proxy.store(false, std::memory_order_release);
            std::printf("[signal] LOUD no host thread for a proxy delivery to tid %u\n", tid);
        }
    }
    return 0;
}

// On a proxy (t_hot.is_proxy) the thread's state stays as the proxy found
// it: the handler's own host calls and Run levels are the proxy's, not the
// thread's.
void Runtime::SigEnterHostCall(const std::array<std::uint32_t, 16>& regs, std::uint32_t cpsr,
                               bool blocking) {
    if (!blocking || !t_sig || t_hot.is_proxy) {
        return;  // a short call: the thread stops when it is back in guest code
    }
    const bool on = SignalsInUse();
    if (on) {
        t_sig->snap = regs;
        t_sig->snap_cpsr = cpsr;
    }
    t_sig->snap_valid.store(on, std::memory_order_relaxed);
    t_sig->parked.store(false, std::memory_order_relaxed);
    t_sig->outside.store(true, std::memory_order_release);
}

void Runtime::SigLeaveHostCall(bool blocking) {
    if (!blocking || !t_sig || t_hot.is_proxy) {
        return;
    }
    if (SignalsInUse()) {
        std::lock_guard<std::recursive_mutex> tok(t_sig->token);  // wait out a proxy
        t_sig->outside.store(false, std::memory_order_release);
        return;
    }
    t_sig->outside.store(false, std::memory_order_release);
}

void Runtime::ProxyDeliver(std::shared_ptr<SigThread> st, std::uint32_t tid) {
    for (;;) {
        {
            std::lock_guard<std::recursive_mutex> tok(st->token);
            const bool deliverable = (st->pending.load(std::memory_order_acquire) &
                                      ~st->blocked.load(std::memory_order_relaxed)) != 0;
            if (deliverable && st->outside.load(std::memory_order_acquire)) {
                // Stand in for the thread: its guest identity, and the
                // registers it left guest code with (none when parked).
                std::array<std::uint32_t, 16> regs = st->snap;
                std::uint32_t cpsr = st->snap_cpsr;
                if (!st->snap_valid.load(std::memory_order_relaxed)) {
                    // A host call entered before signals were in use: the
                    // thread's stopped Jit holds its registers.
                    if (Dynarmic::A32::Jit* j = st->jit.load(std::memory_order_acquire)) {
                        regs = j->Regs();
                        cpsr = j->Cpsr();
                    }
                }
                if (st->parked.load(std::memory_order_relaxed)) {
                    regs.fill(0);
                    const std::uint32_t top = st->stack_top ? st->stack_top : kStackTop;
                    regs[13] = (top - 0x400u) & ~7u;
                    cpsr = 0x10;
                }
                t_sig = st;
                t_rt = this;
                t_hot.tid = tid;
                t_proxy_regs = &regs;
                t_proxy_cpsr = cpsr;
                t_hot.is_proxy = true;
                DeliverPendingSignals();
                t_hot.is_proxy = false;
                t_proxy_regs = nullptr;
                t_hot.tid = 0;
                t_sig.reset();
            }
        }
        st->proxy.store(false, std::memory_order_release);
        const bool again = (st->pending.load(std::memory_order_acquire) &
                            ~st->blocked.load(std::memory_order_relaxed)) != 0 &&
                           st->outside.load(std::memory_order_acquire);
        if (!again || st->proxy.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
    }
}

// A nested Run (a callback Jit, or a Java -> native call made from inside a
// JNI call) makes host calls of its own, which overwrite the thread's
// register snapshot; the outer host call's snapshot (its r4-r11 may hold the
// only references to live objects, which a GC must see) is put back when the
// nested Run ends.
namespace {
struct SnapSave {
    std::array<std::uint32_t, 16> regs;
    std::uint32_t cpsr;
    bool valid;
    bool outside;  // the outer host call blocks (proxy may stand in) or not
};
thread_local std::vector<SnapSave> t_snap_stack;
}  // namespace

void GuestSigEnterJit() {
    t_jit_depth++;
    if (t_jit_depth > 1 && t_sig && !t_hot.is_proxy) {
        t_snap_stack.push_back({t_sig->snap, t_sig->snap_cpsr, t_sig->snap_valid.load(),
                                t_sig->outside.load()});
    }
    GuestSigResumeJit();
}

void GuestSigResumeJit() {
    if (!t_rt || !t_sig || t_hot.is_proxy) {
        return;
    }
    if (t_rt->SignalsInUse()) {
        std::lock_guard<std::recursive_mutex> tok(t_sig->token);  // wait out a proxy
        t_sig->parked.store(false, std::memory_order_relaxed);
        t_sig->outside.store(false, std::memory_order_release);
        return;
    }
    t_sig->parked.store(false, std::memory_order_relaxed);
    t_sig->outside.store(false, std::memory_order_release);
}

void GuestSigLeaveJit() {
    t_jit_depth--;
    if (!t_rt || !t_sig || t_hot.is_proxy) {
        return;
    }
    // Back in a host call (a callback Jit ended) or back in the host.
    if (t_jit_depth > 0 && !t_snap_stack.empty()) {
        const SnapSave& sv = t_snap_stack.back();
        t_sig->snap = sv.regs;
        t_sig->snap_cpsr = sv.cpsr;
        t_sig->snap_valid.store(sv.valid, std::memory_order_relaxed);
        const bool outside = sv.outside;
        t_snap_stack.pop_back();
        // Back inside the outer host call, which a proxy may serve only if
        // it blocks.
        t_sig->parked.store(false, std::memory_order_relaxed);
        t_sig->outside.store(outside, std::memory_order_release);
        return;
    }
    t_sig->parked.store(t_jit_depth <= 0, std::memory_order_relaxed);
    t_sig->outside.store(true, std::memory_order_release);
}

std::uint32_t Runtime::GuestSigsuspend(std::uint32_t mask_ptr) {
    // Wait with the given mask until a signal it does not block is delivered;
    // always ends with -1/EINTR.
    if (!CheckRange(mask_ptr, 4, false)) {
        return EFAULT;
    }
    if (!t_sig) {
        return EINTR;
    }
    std::uint32_t m = 0;
    std::memcpy(&m, H(mask_ptr), 4);
    const std::uint32_t old = t_sig->blocked.load(std::memory_order_relaxed);
    t_sig->blocked.store(m & ~kUnblockable, std::memory_order_relaxed);
    while (!SignalDeliverable()) {
        struct timespec ts {0, kSigSliceNs};
        ::nanosleep(&ts, nullptr);
    }
    DeliverPendingSignals();
    t_sig->blocked.store(old, std::memory_order_relaxed);
    return EINTR;
}

std::uint32_t Runtime::ErrnoSlot() {
    const std::uint32_t tid = GuestTid();
    if (tid == 1) {
        return kGuestDataBase + 0x100;  // main thread (legacy layout)
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = errno_slots_.find(tid);
    if (it != errno_slots_.end()) {
        return it->second;
    }
    // G2.6: reuse a finished thread's slot before bumping (the pool is finite).
    std::uint32_t s = 0;
    if (!errno_free_.empty()) {
        s = errno_free_.back();
        errno_free_.pop_back();
    } else {
        s = kGuestDataBase + 0x104 + next_errno_slot_ * 4;
        next_errno_slot_++;
        if (std::uint64_t(s) + 4 > std::uint64_t(kGuestDataBase) + 0x2000) {
            // Slot pool exhausted (~1979 threads alive at once): alias the main
            // slot. LOUD once: two threads sharing an errno cell is a real (if
            // rare) divergence, so it must never be silent.
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                std::printf("[thread] LOUD errno slots exhausted -- aliasing main slot\n");
            }
            return kGuestDataBase + 0x100;
        }
    }
    errno_slots_[tid] = s;
    return s;
}

void Runtime::SetGuestErrno(int e) {
    const std::uint32_t s = ErrnoSlot();
    if (CheckRange(s, 4, true)) {
        std::memcpy(H(s), &e, 4);
    }
}

bool Runtime::AcquireCpuId(std::size_t* id) {
    std::lock_guard<std::mutex> l(cpu_ids_mu_);
    for (std::size_t i = 0; i < kMaxGuestCpus; i++) {
        if (!cpu_ids_used_[i]) {
            cpu_ids_used_[i] = true;
            *id = i;
            return true;
        }
    }
    std::printf("[thread] LOUD all %zu exclusive-monitor processor ids in use\n", kMaxGuestCpus);
    return false;
}

Runtime::JitPtr Runtime::NewJit(const Dynarmic::A32::UserConfig& cfg) {
    JitPtr jit(nullptr, JitDrop{this});
    if (!JitShareOn()) {
        jit.reset(new Dynarmic::A32::Jit(cfg));
    } else {
        std::lock_guard<std::mutex> l(code_cache_mu_);
        for (const CodeCacheEntry& e : code_caches_) {
            const Dynarmic::A32::UserConfig& c = e.cfg;
            if (cfg.callbacks == c.callbacks && cfg.global_monitor == c.global_monitor &&
                cfg.fastmem_pointer == c.fastmem_pointer && cfg.optimizations == c.optimizations &&
                cfg.enable_cycle_counting == c.enable_cycle_counting) {
                jit.reset(new Dynarmic::A32::Jit(cfg, e.cache));
                break;
            }
        }
        if (!jit) {
            jit.reset(new Dynarmic::A32::Jit(cfg));
            if (auto cache = jit->GetSharedCodeCache()) {  // nullptr: backend cannot share
                code_caches_.push_back({cfg, std::move(cache)});
            }
        }
    }
    std::lock_guard<std::mutex> l(live_jits_mu_);
    live_jits_.insert(jit.get());
    return jit;
}

void JitDrop::operator()(Dynarmic::A32::Jit* j) const {
    if (rt) {
        rt->ReleaseJit(j);
    } else {
        delete j;
    }
}

#ifdef ABR_DIAG_COUNTERS
void Runtime::DiagCheckCode() {
#ifdef DYNARMIC_DISPATCH_COUNTERS
    std::lock_guard<std::mutex> l(live_jits_mu_);
    std::size_t total = 0;
    std::string ranges;
    for (Dynarmic::A32::Jit* jit : live_jits_) {
        total += jit->DiagChangedCode(ranges);
        if (JitShareOn()) {
            break;  // one cache for all
        }
    }
    if (total) {
        std::printf("[codewatch] %zu translated blocks changed without a cacheflush:%s\n", total,
                    ranges.c_str());
    }
#endif
}
#endif

void Runtime::ReleaseJit(Dynarmic::A32::Jit* j) {
    if (!j) {
        return;
    }
    {
        std::lock_guard<std::mutex> l(live_jits_mu_);
        live_jits_.erase(j);
    }
    delete j;
}

void Runtime::ReleaseCpuId(std::size_t id) {
    if (id >= kMaxGuestCpus) {
        return;
    }
    monitor_.ClearProcessor(id);  // a recycled id must not inherit a reservation
    std::lock_guard<std::mutex> l(cpu_ids_mu_);
    cpu_ids_used_[id] = false;
}

std::uint32_t Runtime::PthreadCreate(std::uint32_t tid_out, std::uint32_t attr, std::uint32_t start,
                                     std::uint32_t arg) {
    if (!IsCodeAddress(start & ~std::uint32_t(1))) {
        std::printf("[thread] LOUD bad start %08x -- halting\n", start);
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0xFFFFFFFFu;
    }
    if (attr != 0) {
        std::printf("[thread] NOTE non-null attr %08x (ignored)\n", attr);
    }
    if (!cb_) {
        std::printf("[thread] LOUD no callbacks registered -- halting\n");
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0xFFFFFFFFu;
    }
    // 8MB guest stack from the heap. Stack-overflow smash guard (triage
    // 2026-10-16). The block is owned by the ThreadState now and handed back by
    // ReapThreadStack (2026-10-18: it used to be leaked outright).
    const std::uint32_t stack = GuestMalloc(kThreadStackSize);
    if (!stack) {
        return ENOMEM;
    }
    std::size_t cpu_id = 0;
    if (!AcquireCpuId(&cpu_id)) {
        GuestFree(stack);
        return EAGAIN;  // never share a processor id: exclusives would break
    }
    auto st = std::make_shared<ThreadState>();
    std::uint32_t tid = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        tid = AllocTid();
        st->stack = stack;
        thr_stacks_++;
        thr_stack_bytes_ += kThreadStackSize;
        threads_[tid] = st;
    }
    if (!CheckRange(tid_out, 4, true)) {
        std::printf("[thread] LOUD bad tid_out %08x -- halting\n", tid_out);
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0xFFFFFFFFu;
    }
    std::memcpy(H(tid_out), &tid, 4);
    if (DiagOn()) {
        std::printf("[thread] create tid=%u start=%08x arg=%08x stack=%08x\n", tid, start, arg,
                    stack);
    }
    // std::thread throws std::system_error when the host cannot create a
    // thread; that must become EAGAIN for the guest, not std::terminate.
    try {
    st->th = std::thread([this, st, tid, start, arg, stack, cpu_id]() {
        t_hot.tid = tid;
        SetThreadStack(stack, kThreadStackSize);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = cb_;
        cfg.optimizations =
            JitOptimizations();
        cfg.unsafe_optimizations = JitUnsafeOptimizations();
        cfg.enable_cycle_counting = kJitCycleCounting;
        if (JitFastmemOn()) {
            cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(mem().base());
        }
        cfg.global_monitor = &monitor_;
        cfg.processor_id = cpu_id;
        const JitPtr jit_owner = NewJit(cfg);
        Dynarmic::A32::Jit& jit = *jit_owner;
        t_hot.active_jit = &jit;
        t_worker_jit = &jit;
        RegisterSigThread(tid, &jit);
        PushLocalFrame();  // real handles made here go with the thread, as on a JVM
        // HARNESS_WBEAT=1: sample worker guest PC every 5s (finds stuck
        // workers; racy values, liveness only — same caveat as main beat).
        if (std::getenv("HARNESS_WBEAT")) {
            std::thread([st, tid, &jit]() {
                for (int i = 0; i < 60; i++) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    bool dd = false;
                    {
                        std::lock_guard<std::mutex> l(st->m);
                        dd = st->done;
                    }
                    if (dd) {
                        break;
                    }
                    auto& rr = jit.Regs();
                    std::printf("[wbeat] tid=%u pc=%08x lr=%08x sp=%08x\n", tid, rr[15],
                                rr[14], rr[13]);
                }
            }).detach();
        }
        auto& regs = jit.Regs();
        regs.fill(0);
        regs[13] = stack + kThreadStackSize - 8;  // 8-aligned top
        regs[14] = mod_.halt_va;           // routine return ends the thread
        regs[0] = arg;
        if (start & 1u) {
            std::uint32_t cpsr = jit.Cpsr();
            jit.SetCpsr((cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10) |
                        std::uint32_t(0x20));
            regs[15] = start & ~std::uint32_t(1);
        } else {
            std::uint32_t cpsr = jit.Cpsr();
            jit.SetCpsr((cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10));
            regs[15] = start;
        }
        const Dynarmic::HaltReason hr = RunJit(jit);
        if (t_thread_exiting) {
            regs[0] = t_exit_value;  // pthread_exit(value)
        }
        // A clean end (the start routine returned, or pthread_exit) runs the
        // key destructors on this thread; a halted guest runs nothing more.
        if (hr == Dynarmic::HaltReason::UserDefined1 || t_thread_exiting) {
            const std::uint32_t retval = regs[0];
            RunKeyDestructors();
            regs[0] = retval;
        }
        // Dying here must not strand a caller that is waiting in PthreadOnce.
        ReleaseOnceControlsOnExit(tid);
        while (!t_local_frames.empty()) {
            PopLocalFrame();  // the guest's own frames too, if left pushed
        }
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->retval = regs[0];
            st->done = true;
        }
        if (DiagOn()) std::printf("[thread] tid=%u done retval=%08x\n", tid, regs[0]);
        st->cv.notify_all();
        // Guest code has stopped, so its stack is dead: hand it back now (a
        // joiner only needs st->retval). Idempotent with join/detach.
        ReapThreadStack(tid);
        t_hot.active_jit = nullptr;
        t_worker_jit = nullptr;
        ForgetSigThreadJit(tid);
        ReleaseCpuId(cpu_id);  // the Jit no longer runs
        {
            // A detached thread is gone once it ends (a joinable one waits
            // for pthread_join, which drops it).
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (st->detached) {
                threads_.erase(tid);
            }
        }
    });
    } catch (const std::system_error&) {
        ReapThreadStack(tid);
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            threads_.erase(tid);
        }
        ReleaseCpuId(cpu_id);
        std::printf("[thread] LOUD host thread creation failed -- pthread_create = EAGAIN\n");
        return EAGAIN;
    }
    st->th.detach();
    return 0;
}

// End-of-run census (see dispatch.h): detached workers can't be joined, so
// report tid/done/retval here instead of losing them to _exit.
void Runtime::DumpGuestThreads() {
    std::vector<std::pair<std::uint32_t, std::shared_ptr<SigThread>>> all;
    {
        std::lock_guard<std::mutex> l(sig_mu_);
        for (const auto& kv : sig_threads_) {
            all.emplace_back(kv.first, kv.second);
        }
    }
    auto import_at = [&](std::uint32_t pc) -> std::string {
        // Import trampoline i sits at kTrampBase + 4 * (i + 2) (slots 0/1 are
        // halt/once); a thread stopped in a host call reports the pc past
        // its svc, so the svc is at pc - 4.
        for (std::uint32_t p : {pc - 4}) {
            if (p >= kTrampBase + 8 && p < kTrampBase + kTrampSize) {
                const std::uint32_t i = (p - kTrampBase) / 4 - 2;
                if (i < mod_.imports.size()) {
                    return mod_.imports[i].name;
                }
            }
        }
        return "";
    };
    for (const auto& [tid, st] : all) {
        const bool outside = st->outside.load(), parked = st->parked.load();
        std::uint32_t pc = 0, lr = 0, sp = 0;
        if (outside && !parked) {
            pc = st->snap[15];
            lr = st->snap[14];
            sp = st->snap[13];
        } else if (Dynarmic::A32::Jit* j = st->jit.load()) {
            pc = j->Regs()[15];
            lr = j->Regs()[14];
            sp = j->Regs()[13];
        }
        const std::string imp = import_at(pc);
        std::string samples;
        if (!outside) {
            // Running guest code: a few pc samples tell a spin from progress.
            if (Dynarmic::A32::Jit* j = st->jit.load()) {
                for (int k = 0; k < 5; k++) {
                    char b[16];
                    std::snprintf(b, sizeof b, " %08x", j->Regs()[15]);
                    samples += b;
                    ::usleep(20000);
                }
            }
        }
        // A guest cond wait: its cond and mutex, and whether the host cond it
        // sleeps on is still the one the guest cond maps to (a signal goes to
        // the mapped one).
        std::string wait;
        if (const void* wcv = st->wait_cv.load(); wcv && outside && !parked) {
            const std::uint32_t gc = st->snap[0];
            const char* same = "?";
            std::uint64_t wakes = 0;
            std::uint32_t last_by = 0;
            std::unique_lock<std::recursive_mutex> g(mutex_, std::try_to_lock);
            if (g.owns_lock()) {
                const auto it = cond_map_.find(gc);
                same = it == cond_map_.end() ? "unmapped" : it->second.get() == wcv ? "yes" : "NO";
                if (it != cond_map_.end() && pthread_mutex_trylock(&it->second->m) == 0) {
                    wakes = it->second->wakes;
                    pthread_mutex_unlock(&it->second->m);
                    last_by = it->second->last_tid.load();
                }
            }
            char b[160];
            std::snprintf(b, sizeof b,
                          " cond=%08x mutex=%08x mapped-same=%s wakes=%llu at-sleep=%llu last-by=%u",
                          gc, st->snap[1], same, (unsigned long long)wakes,
                          (unsigned long long)st->wait_wakes.load(), last_by);
            wait = b;
        }
        // The first argument of a blocking host call (with signals in use its
        // registers are kept): for pthread_mutex_lock, the mutex it waits for.
        char a0s[24] = "";
        if (outside && wait.empty() && st->snap_valid.load(std::memory_order_relaxed)) {
            std::snprintf(a0s, sizeof a0s, " a0=%08x", st->snap[0]);
        }
        std::printf("[threads] tid=%u host=%d %s pc=%08x lr=%08x sp=%08x%s%s%s%s pending=%08x%s%s\n",
                    tid, st->host_tid.load(), parked ? "in-host" : outside ? "host-call" : "guest", pc,
                    lr, sp, imp.empty() ? "" : " import=", imp.c_str(), a0s, wait.c_str(),
                    st->pending.load(), samples.empty() ? "" : " samples:", samples.c_str());
    }
}

void Runtime::ThreadCensus() {
    // Watchdog robustness: never block here (see MutexOwnersDump).
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::printf("[census] SKIPPED (map guard contested)\n");
        return;
    }
    for (const auto& kv : threads_) {
        bool done = false;
        std::uint32_t rv = 0;
        std::string ls = "?";
        std::string hist;
        {
            // Never block census on a worker mutex (a stuck join/worker must
            // not freeze the watchdog's only witness).
            std::unique_lock<std::mutex> l(kv.second->m, std::try_to_lock);
            if (!l.owns_lock()) {
                std::printf("[census] tid=%u SKIPPED (worker mutex contested)\n", kv.first);
                continue;
            }
            done = kv.second->done;
            rv = kv.second->retval;
            ls = kv.second->last_svc;
            hist = kv.second->svc_hist;
        }
        std::printf("[census] tid=%u done=%d retval=%08x last=%s hist=%.320s\n", kv.first,
                    (int)done, rv, ls.c_str(), hist.c_str());
    }
}

#ifdef ABR_DIAG_COUNTERS
// Stage D: one line every 30 s. Snapshot under try-locks (the watchdog must
// never wedge on a guest-held mutex), then print after releasing them. The
// line names each quantity and keeps used/high-water side by side.
void Runtime::DumpDiagCounters() {
    std::uint64_t heap_used = 0, heap_peak = 0;
    std::uint32_t str_used = 0, str_high = 0, obj_used = 0, obj_high = 0;
    std::uint64_t str_heap = 0;
    std::size_t n_strings = 0, n_objects = 0, n_rev = 0, n_fwd = 0, n_utf = 0;
    std::uint64_t utf_allocs = 0, utf_releases = 0;
    std::size_t n_tls = 0, n_errno = 0, n_errno_free = 0;
    std::uint64_t tls_reaps = 0;
    std::uint32_t errno_next = 0;
    std::uint64_t wrapg = 0, newg = 0, delg = 0;
    std::uint64_t loc_allocs = 0, loc_bytes = 0;
    std::size_t n_assets = 0, n_threads = 0;
    std::uint64_t asset_bytes = 0, asset_peak_open = 0, asset_peak_bytes = 0;
    std::size_t n_fd = 0, n_file = 0, n_fd_free = 0;
    std::uint32_t fd_next = 0;
    bool fd_ok = false;
    {
        std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            std::printf("[counters] SKIPPED (runtime mutex contested)\n");
            return;
        }
        std::unique_lock<std::recursive_mutex> heap(heap_mu_, std::try_to_lock);
        if (!heap.owns_lock()) {
            std::printf("[counters] SKIPPED (heap lock contested)\n");
            return;
        }
        heap_used = heap_used_;
        heap_peak = diag_.heap_peak;
        str_used = jni_str_next_ >= kJniStringStart ? jni_str_next_ - kJniStringStart : 0;
        str_high = diag_.jni_str_high;
        str_heap = diag_.jni_str_heap_bytes;
        obj_used = jni_obj_next_ >= kJniObjectStart
                       ? (jni_obj_next_ - kJniObjectStart) / 4u
                       : 0;
        obj_high = diag_.jni_obj_high;
        n_strings = jni_strings_.size();
        n_objects = jni_objects_.size();
        n_rev = obj_rev_.size();
        n_fwd = obj_fwd_.size();
        n_utf = jni_utf_chars_.size();
        utf_allocs = diag_.utf_allocs.load(std::memory_order_relaxed);
        utf_releases = diag_.utf_releases.load(std::memory_order_relaxed);
        n_threads = threads_.size();
        n_tls = tls_blocks_.size();
        n_errno = errno_slots_.size();
        n_errno_free = errno_free_.size();
        errno_next = next_errno_slot_;
        tls_reaps = diag_.tls_reaps.load(std::memory_order_relaxed);
        wrapg = diag_.wrap_globals;
        newg = diag_.jni_new_global_refs.load(std::memory_order_relaxed);
        delg = diag_.jni_delete_global_refs.load(std::memory_order_relaxed);
        loc_allocs = diag_.localtime_allocs.load(std::memory_order_relaxed);
        loc_bytes = diag_.localtime_bytes.load(std::memory_order_relaxed);
        n_assets = asset_map_.size();
        for (const auto& kv : asset_map_) {
            asset_bytes += kv.second.size;
        }
        asset_peak_open = diag_.asset_peak_open;
        asset_peak_bytes = diag_.asset_peak_bytes;
        // fd_map_/file_map_ use the leaf io_mutex_, not mutex_. Try it only;
        // if a guest is inside a file syscall, print '?' for those fields.
        std::unique_lock<std::mutex> io(io_mutex_, std::try_to_lock);
        if (io.owns_lock()) {
            n_fd = fd_map_.size();
            n_file = file_map_.size();
            n_fd_free = free_guest_fds_.size();
            fd_next = next_guest_fd_;
            fd_ok = true;
        }
    }
    char fd[112];
    if (fd_ok) {
        const unsigned long long fd_free =
            (fd_next < 0x8000u ? 0x8000u - fd_next : 0) + n_fd_free;
        std::snprintf(fd, sizeof(fd), "fd=%zu,next=%u,free=%llu,file=%zu", n_fd, fd_next,
                      fd_free, n_file);
    } else {
        std::snprintf(fd, sizeof(fd), "fd=?,next=?,free=?,file=?");
    }
    {
        // Imports since the last dump: the total and the ten heaviest by
        // host time (calls, average ns per call).
        static std::uint64_t last_calls[DiagCounters::kImportSlots];
        static std::uint64_t last_ns[DiagCounters::kImportSlots];
        struct Row {
            std::uint32_t idx;
            std::uint64_t calls, ns;
        };
        std::vector<Row> rows;
        std::uint64_t all_calls = 0, all_ns = 0;
        const std::size_t n = std::min<std::size_t>(mod_.imports.size(), DiagCounters::kImportSlots);
        const double ns_per_tick = 1e9 / static_cast<double>(DiagTickHz());
        for (std::size_t i = 0; i < n; i++) {
            const std::uint64_t c = diag_.import_calls[i].load(std::memory_order_relaxed);
            const std::uint64_t t = static_cast<std::uint64_t>(
                static_cast<double>(diag_.import_ns[i].load(std::memory_order_relaxed)) * ns_per_tick);
            const Row r{static_cast<std::uint32_t>(i), c - last_calls[i], t - last_ns[i]};
            last_calls[i] = c;
            last_ns[i] = t;
            if (r.calls) {
                all_calls += r.calls;
                all_ns += r.ns;
                rows.push_back(r);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
        {
            // Host time of the imports that do not wait (no sem_wait, sleeps,
            // swaps): host CPU work, to set against the process CPU time.
            std::uint64_t work_ns = 0, work_calls = 0;
            for (const Row& r : rows) {
                if (!mod_.imports[r.idx].blocking) {
                    work_ns += r.ns;
                    work_calls += r.calls;
                }
            }
            std::printf("[counters] non-waiting imports: %llu calls, %llu ms host time\n",
                        (unsigned long long)work_calls, (unsigned long long)(work_ns / 1000000));
        }
        {
            // The busiest imports that still walk CallImport's name chain (no
            // GL id, fast-math id or hot id).
            std::vector<Row> chain;
            for (const Row& r : rows) {
                const Import& im = mod_.imports[r.idx];
                if (im.gl_id == GlId::None && im.fast_math == 0 && im.hot == HotImport::None) {
                    chain.push_back(r);
                }
            }
            std::sort(chain.begin(), chain.end(),
                      [](const Row& a, const Row& b) { return a.calls > b.calls; });
            std::uint64_t cc = 0;
            for (const Row& r : chain) {
                cc += r.calls;
            }
            std::printf("[counters] name-chain imports: %llu calls; busiest:\n", (unsigned long long)cc);
            for (std::size_t k = 0; k < chain.size() && k < 20; k++) {
                std::printf("[counters]   chain %-26s %10llu calls %6llu ns/call\n",
                            mod_.imports[chain[k].idx].name.c_str(),
                            (unsigned long long)chain[k].calls,
                            (unsigned long long)(chain[k].ns / chain[k].calls));
            }
        }
        std::printf("[counters] imports: %llu calls, %llu ms host time since the last dump\n",
                    (unsigned long long)all_calls, (unsigned long long)(all_ns / 1000000));
        static std::uint64_t last_w[3], last_wn[3], last_g;
        const char* const site[3] = {"mutex map", "malloc", "free"};
        for (int k = 0; k < 3; k++) {
            const std::uint64_t w = diag_.rt_wait[k].load(std::memory_order_relaxed);
            const std::uint64_t wn = diag_.rt_wait_n[k].load(std::memory_order_relaxed);
            std::printf("[counters]   runtime lock wait at %-9s %10llu takes %7llu ms\n", site[k],
                        (unsigned long long)(wn - last_wn[k]),
                        (unsigned long long)(static_cast<double>(w - last_w[k]) * ns_per_tick / 1e6));
            last_w[k] = w;
            last_wn[k] = wn;
        }
        const std::uint64_t g = diag_.guest_lock_wait.load(std::memory_order_relaxed);
        {
            static std::uint64_t l_in, l_live, l_ms;
            const std::uint64_t in = diag_.free_in_lock.load(), lv = diag_.free_live.load(),
                                ms = diag_.free_mspace.load();
            auto ms_of = [&](std::uint64_t t) {
                return (unsigned long long)(static_cast<double>(t) * ns_per_tick / 1e6);
            };
            std::printf("[counters]   free: %llu ms under the lock, of it live index %llu ms, "
                        "mspace_free %llu ms\n",
                        ms_of(in - l_in), ms_of(lv - l_live), ms_of(ms - l_ms));
            l_in = in;
            l_live = lv;
            l_ms = ms;
        }
        std::printf("[counters]   inside the host lock of guest mutexes %7llu ms\n",
                    (unsigned long long)(static_cast<double>(g - last_g) * ns_per_tick / 1e6));
        last_g = g;
        for (std::size_t k = 0; k < rows.size() && k < 12; k++) {
            std::printf("[counters]   %-28s %10llu calls %7llu ms %6llu ns/call\n",
                        mod_.imports[rows[k].idx].name.c_str(), (unsigned long long)rows[k].calls,
                        (unsigned long long)(rows[k].ns / 1000000),
                        (unsigned long long)(rows[k].ns / rows[k].calls));
        }
    }
    RealGlDiagDump();
#ifdef DYNARMIC_DISPATCH_COUNTERS
    std::printf("[counters] dispatch=%llu memo_miss=%llu fast_evict=%llu fast_stale=%llu\n",
                (unsigned long long)Dynarmic::A32::DispatchCount(),
                (unsigned long long)Dynarmic::A32::DispatchMemoMissCount(),
                (unsigned long long)Dynarmic::A32::DispatchEvictCount(),
                (unsigned long long)Dynarmic::A32::DispatchStaleCount());
#endif
    std::printf(
        "[counters] heap=%llu/%llu jni_str=%u/%u,heap=%llu jni_obj=%u/%u "
        "maps=str=%zu,obj=%zu,rev=%zu,fwd=%zu utf=%zu,%llu/%llu wrapg=%llu newg=%llu delg=%llu "
        "%s localtime=%llu/%llu assets=%zu/%llu,peak=%llu/%llu "
        "tls=%zu/%llu errno=%zu/%zu,next=%u threads=%zu svc=%llu\n",
        (unsigned long long)heap_used, (unsigned long long)heap_peak, str_used, str_high,
        (unsigned long long)str_heap, obj_used, obj_high, n_strings, n_objects, n_rev, n_fwd, n_utf,
        (unsigned long long)utf_allocs, (unsigned long long)utf_releases,
        (unsigned long long)wrapg, (unsigned long long)newg, (unsigned long long)delg, fd,
        (unsigned long long)loc_allocs, (unsigned long long)loc_bytes, n_assets,
        (unsigned long long)asset_bytes, (unsigned long long)asset_peak_open,
        (unsigned long long)asset_peak_bytes, n_tls, (unsigned long long)tls_reaps, n_errno,
        n_errno_free, errno_next,
        n_threads, (unsigned long long)svc_count.load(std::memory_order_relaxed));
}
#endif

// Free a finished thread's 8MB guest stack exactly once. Before 2026-10-18
// nothing freed it at all ("leak accepted in bring-up"), which is how the
// VideoRewards / time-sync retry loop (one pthread_create per iteration, guest
// site 0x38bb48) drove the heap to 985MB used, then OOM (10x, size=8388608 =
// this stack), then a bad free -> USAGE_ERROR_ACTION -> SIGABRT. The ThreadState
// stays in threads_ so a later guest pthread_join still finds tid/retval.
void Runtime::ReapThreadStack(std::uint32_t tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = threads_.find(tid);
    if (it == threads_.end() || !it->second->stack) {
        return;
    }
    const std::uint32_t s = it->second->stack;
    it->second->stack = 0;
    thr_stacks_--;
    thr_stack_bytes_ -= kThreadStackSize;
    if (DiagOn()) {
        static int rn = 0;
        if (rn < 40) {
            rn++;
            std::printf("[thread] reap tid=%u stack=%08x live=%u\n", tid, s, thr_stacks_);
        }
    }
    GuestFree(s);
    ReapThreadTls(tid);
}

// Product host thread exit: drop the tid's errno/TLS state before the caller
// frees its guest stack and deletes its JIT.
void Runtime::ReapHostThread(std::uint32_t tid) {
    ReapThreadTls(tid);
}

// G2.6 (2026-10-18): per-tid state must not outlive the thread. Hands the errno
// slot back for reuse (the pool is finite) and drops the thread's TLS entries
// (keyed (key<<32)|tid). Idempotent.
void Runtime::ReapThreadTls(std::uint32_t tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
#ifdef ABR_DIAG_COUNTERS
    diag_.tls_reaps.fetch_add(1, std::memory_order_relaxed);
#endif
    const auto it = errno_slots_.find(tid);
    if (it != errno_slots_.end()) {
        errno_free_.push_back(it->second);
        errno_slots_.erase(it);
    }
    const auto tb = tls_blocks_.find(tid);
    if (tb != tls_blocks_.end()) {
        tls_free_.push_back(tb->second);
        tls_blocks_.erase(tb);
        tls_reaps_.fetch_add(1, std::memory_order_release);
    }
}

std::uint32_t Runtime::PthreadJoin(std::uint32_t tid, std::uint32_t retval_out) {
    if (tid == GuestTid()) {
        return EDEADLK;  // joining yourself would wait forever
    }
    std::shared_ptr<ThreadState> st;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = threads_.find(tid);
        if (it == threads_.end() || it->second->detached || it->second->joined) {
            return 3;  // ESRCH
        }
        st = it->second;
        st->joined = true;
    }
    {
        // Join-visibility (a join on a never-done parked worker hangs the
        // joiner forever and freezes st->m for census: name it if it happens;
        // joins are rare, cap generously). Chunked 30s waits: still POSIX
        // blocking, but each chunk names the stuck worker (tid/last_svc)
        // instead of hanging silent. Observe-only (no guest semantics changed).
        static int jn = 0;
        if (jn < 20) {
            jn++;
            if (DiagOn()) std::printf("[thread] join tid=%u waiting (done=%d)\n", tid, (int)st->done);
        }
        std::unique_lock<std::mutex> lock(st->m);
        int chunks = 0;
        while (!st->cv.wait_for(lock, std::chrono::seconds(30),
                                [&] { return st->done; })) {
            if (DiagOn()) {
                std::printf("[thread] join tid=%u STILL waiting chunk=%d last=%s\n", tid,
                            ++chunks, st->last_svc.c_str());
            }
        }
    }
    if (retval_out) {
        if (!CheckRange(retval_out, 4, true)) {
            return 14;  // EFAULT
        }
        std::memcpy(H(retval_out), &st->retval, 4);
    }
    ReapThreadStack(tid);
    // A joined thread's id is invalid afterwards (POSIX): drop its state, or
    // the per-iteration create/join loops grow threads_ without bound.
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        threads_.erase(tid);
    }
    return 0;
}

std::uint32_t Runtime::PthreadDetach(std::uint32_t tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = threads_.find(tid);
    if (it == threads_.end()) {
        return 3;  // ESRCH
    }
    it->second->detached = true;
    if (it->second->done) {
        ReapThreadStack(tid);
        threads_.erase(it);
    }
    return 0;
}

// ---- JNI bridge (bring-up) ----

#ifdef __ANDROID__
// Current thread's real JNIEnv* (product host) or null (harness).
static JNIEnv* RealEnv() {
    if (GuestCallbacks::g_real_env_hook) {
        return static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook());
    }
    return nullptr;
}
#endif

std::uint32_t Runtime::JniEnv() {
    return jni_env_;
}

std::uint32_t Runtime::Jvm() {
    return jvm_;
}

// A wrapped handle that is a java.lang.String: the real string, else null.
void* Runtime::RealString(std::uint32_t handle) {
#ifdef __ANDROID__
    void* real = UnwrapObject(handle);
    JNIEnv* re = real ? RealEnv() : nullptr;
    jclass sc = re ? static_cast<jclass>(RealClass(re, "java/lang/String")) : nullptr;
    if (sc && re->IsInstanceOf(static_cast<jobject>(real), sc)) {
        return real;
    }
#else
    (void)handle;
#endif
    return nullptr;
}

// The text of a jstring handle: a guest string, or a wrapped java.lang.String
// (modified UTF-8 either way).
bool Runtime::JniStringText(std::uint32_t handle, std::string& out) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_strings_.find(handle);
        if (it != jni_strings_.end()) {
            out = it->second;
            return true;
        }
    }
#ifdef __ANDROID__
    if (void* real = RealString(handle)) {
        JNIEnv* re = RealEnv();
        if (const char* p = re->GetStringUTFChars(static_cast<jstring>(real), nullptr)) {
            out = p;
            re->ReleaseStringUTFChars(static_cast<jstring>(real), p);
            return true;
        }
        re->ExceptionClear();
    }
#endif
    return false;
}

std::uint32_t Runtime::WrapObject(void* real, bool permanent) {
    if (!real) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto new_handle = [&]() -> std::uint32_t {
        // Fresh numbers first (a stale handle then reads as unknown instead
        // of naming another object); released ones once the range is used.
        if (next_obj_ < 0xF2000000u) {
            const std::uint32_t h = next_obj_;
            next_obj_ += 4;
            return h;
        }
        if (obj_free_.empty()) {
            return 0;
        }
        const std::uint32_t h = obj_free_.back();
        obj_free_.pop_back();
        return h;
    };
#ifdef __ANDROID__
    // Product host: the reference Java handed us may be call-local; ours is
    // a global ref, released with the handle (see dispatch.h).
    if (JNIEnv* re = RealEnv()) {
        jobject g = re->NewGlobalRef(static_cast<jobject>(real));
        if (g) {
            const std::uint32_t h = new_handle();
            if (!h) {
                re->DeleteGlobalRef(g);
                std::printf("[jni] LOUD no object handle left (%zu live)\n", obj_rev_.size());
                return 0;
            }
            obj_fwd_[g] = h;
            obj_rev_[h] = g;
            if (permanent) {
                obj_permanent_.insert(h);
            } else if (!t_local_frames.empty()) {
                t_local_frames.back().push_back(h);
            }
#ifdef ABR_DIAG_COUNTERS
            ++diag_.wrap_globals;
#endif
            return h;
        }
        // NewGlobalRef failed: fall through to raw (mock path will cope).
    }
#endif
    const auto it = obj_fwd_.find(real);
    if (it != obj_fwd_.end()) {
        return it->second;
    }
    const std::uint32_t h = new_handle();
    if (!h) {
        return 0;
    }
    obj_fwd_[real] = h;
    obj_rev_[h] = real;
    return h;
}

void Runtime::ReleaseHandleLocked(std::uint32_t h) {
    const auto it = obj_rev_.find(h);
    if (it == obj_rev_.end()) {
        return;
    }
#ifdef __ANDROID__
    if (JNIEnv* re = RealEnv()) {
        re->DeleteGlobalRef(static_cast<jobject>(it->second));
    }
#endif
    obj_fwd_.erase(it->second);
    obj_rev_.erase(it);
    obj_permanent_.erase(h);
    jni_classes_.erase(h);
    jni_objclass_.erase(h);
    obj_free_.push_back(h);
}

void Runtime::DropLocalLocked(std::uint32_t h) {
    for (auto& fr : t_local_frames) {
        const auto at = std::find(fr.begin(), fr.end(), h);
        if (at != fr.end()) {
            fr.erase(at);
            break;
        }
    }
    ReleaseHandleLocked(h);
}

void Runtime::PushLocalFrame() {
    t_local_frames.emplace_back();
}

void Runtime::PopLocalFrame() {
    if (t_local_frames.empty()) {
        return;
    }
    std::vector<std::uint32_t> frame = std::move(t_local_frames.back());
    t_local_frames.pop_back();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const std::uint32_t h : frame) {
        if (!obj_permanent_.count(h)) {
            ReleaseHandleLocked(h);
        }
    }
}

long Runtime::WrappedObjectCount() {
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
    return lock.owns_lock() ? static_cast<long>(obj_rev_.size()) : -1;
}

void* Runtime::UnwrapObject(std::uint32_t mock) {
    if (!mock) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = obj_rev_.find(mock);
    return it != obj_rev_.end() ? it->second : nullptr;
}

// The class name ("a/b/C") behind a jclass handle: a mock class from
// FindClass, or a real Class object Java handed in (a native's jclass or
// Class argument), named through Class.getName() once and then cached --
// with the object itself as the real class, found by Java's own loader.
std::string Runtime::JniClassName(std::uint32_t handle) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_classes_.find(handle);
        if (it != jni_classes_.end()) {
            return it->second;
        }
    }
#ifdef __ANDROID__
    JNIEnv* re = RealEnv();
    void* obj = UnwrapObject(handle);
    if (!re || !obj) {
        return "";
    }
    jclass cls_class = re->FindClass("java/lang/Class");
    if (!cls_class || !re->IsInstanceOf(static_cast<jobject>(obj), cls_class)) {
        re->ExceptionClear();
        if (cls_class) {
            re->DeleteLocalRef(cls_class);  // a native-attached thread keeps locals until detach
        }
        return "";
    }
    jmethodID get_name = re->GetMethodID(cls_class, "getName", "()Ljava/lang/String;");
    jstring js = get_name ? static_cast<jstring>(re->CallObjectMethod(static_cast<jobject>(obj), get_name))
                          : nullptr;
    re->DeleteLocalRef(cls_class);
    if (!js || re->ExceptionCheck()) {
        re->ExceptionClear();
        return "";
    }
    const char* utf = re->GetStringUTFChars(js, nullptr);
    std::string name = utf ? utf : "";
    if (utf) {
        re->ReleaseStringUTFChars(js, utf);
    }
    re->DeleteLocalRef(js);
    std::replace(name.begin(), name.end(), '.', '/');
    if (!name.empty()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        jni_classes_[handle] = name;
        if (real_classes_.find(name) == real_classes_.end()) {
            real_classes_[name] = re->NewGlobalRef(static_cast<jobject>(obj));
        }
    }
    return name;
#else
    return "";
#endif
}

void Runtime::FlushDirectBuffers() {
    if (t_direct_used.empty()) {
        return;
    }
    for (void* host : t_direct_used) {
        DirectBuf db;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = direct_bufs_.find(host);
            if (it == direct_bufs_.end()) {
                continue;
            }
            db = it->second;
        }
        if (db.guest && CheckRange(db.guest, db.size, false)) {
            std::memcpy(host, H(db.guest), db.size);
        }
    }
    t_direct_used.clear();
}

void* Runtime::RealFieldId(std::uint32_t cookie, bool is_static) {
#ifdef __ANDROID__
    JNIEnv* re = RealEnv();
    if (!re) {
        return nullptr;
    }
    std::string d;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto& cache = is_static ? real_static_fields_ : real_fields_;
        const auto c = cache.find(cookie);
        if (c != cache.end()) {
            return c->second;
        }
        const auto it = jni_methods_.find(cookie);
        if (it == jni_methods_.end()) {
            return nullptr;
        }
        d = it->second;
    }
    const std::string pfx = "field ";
    if (d.compare(0, pfx.size(), pfx) != 0) {
        return nullptr;
    }
    d = d.substr(pfx.size());
    const std::size_t colon = d.find(':');
    const std::size_t dot = colon == std::string::npos ? std::string::npos : d.rfind('.', colon);
    if (dot == std::string::npos) {
        return nullptr;
    }
    const std::string cls = d.substr(0, dot), name = d.substr(dot + 1, colon - dot - 1),
                      sig = d.substr(colon + 1);
    jclass rc = static_cast<jclass>(RealClass(re, cls));
    if (!rc) {
        return nullptr;
    }
    jfieldID f = is_static ? re->GetStaticFieldID(rc, name.c_str(), sig.c_str())
                           : re->GetFieldID(rc, name.c_str(), sig.c_str());
    if (!f || re->ExceptionCheck()) {
        re->ExceptionClear();
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (is_static ? real_static_fields_ : real_fields_)[cookie] = f;
    return f;
#else
    (void)cookie;
    (void)is_static;
    return nullptr;
#endif
}

std::uint32_t Runtime::JniArrayElemSize(std::uint32_t array_handle) {
#ifdef __ANDROID__
    JNIEnv* re = RealEnv();
    jobject a = static_cast<jobject>(UnwrapObject(array_handle));
    if (!re || !a) {
        return 0;
    }
    jclass c = re->GetObjectClass(a);
    const std::uint32_t h = c ? WrapObject(c) : 0;
    if (c) {
        re->DeleteLocalRef(c);
    }
    const std::string n = h ? JniClassName(h) : "";
    if (h) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        DropLocalLocked(h);  // only its name was wanted
    }
    if (n.size() == 2 && n[0] == '[') {
        switch (n[1]) {
            case 'Z': case 'B': return 1;
            case 'C': case 'S': return 2;
            case 'I': case 'F': return 4;
            case 'J': case 'D': return 8;
            default: break;
        }
    }
#else
    (void)array_handle;
#endif
    return 0;
}

std::uint32_t Runtime::CookieForReflected(std::uint32_t member, bool field) {
#ifdef __ANDROID__
    JNIEnv* re = RealEnv();
    jobject m = static_cast<jobject>(UnwrapObject(member));
    if (!re || !m) {
        std::printf("[jni] FromReflected %08x: cannot describe it (step 1)\n", member);
        return 0;
    }
    // Calls an instance method with no arguments by name and signature.
    auto call_obj = [&](jobject o, const char* name, const char* sig) -> jobject {
        jclass c = re->GetObjectClass(o);
        jmethodID id = c ? re->GetMethodID(c, name, sig) : nullptr;
        jobject r = id ? re->CallObjectMethod(o, id) : nullptr;
        if (c) {
            re->DeleteLocalRef(c);
        }
        if (re->ExceptionCheck()) {
            re->ExceptionClear();
            return nullptr;
        }
        return r;
    };
    auto str = [&](jobject js) -> std::string {
        if (!js) {
            return "";
        }
        const char* u = re->GetStringUTFChars(static_cast<jstring>(js), nullptr);
        std::string out = u ? u : "";
        if (u) {
            re->ReleaseStringUTFChars(static_cast<jstring>(js), u);
        }
        re->DeleteLocalRef(js);
        return out;
    };
    // The JNI descriptor of a Class object.
    auto desc = [&](jobject cls) -> std::string {
        jclass cc = re->FindClass("java/lang/Class");
        jmethodID prim = re->GetMethodID(cc, "isPrimitive", "()Z");
        const bool p = re->CallBooleanMethod(cls, prim);
        re->DeleteLocalRef(cc);
        std::string n = str(call_obj(cls, "getName", "()Ljava/lang/String;"));
        if (p) {
            static const std::pair<const char*, const char*> kPrim[] = {
                {"int", "I"}, {"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"},
                {"long", "J"}, {"float", "F"}, {"double", "D"}, {"void", "V"}};
            for (const auto& kv : kPrim) {
                if (n == kv.first) {
                    return kv.second;
                }
            }
            return "";
        }
        std::replace(n.begin(), n.end(), '.', '/');
        return n.empty() || n[0] == '[' ? n : "L" + n + ";";
    };
    jobject decl = call_obj(m, "getDeclaringClass", "()Ljava/lang/Class;");
    if (!decl) {
        std::printf("[jni] FromReflected %08x: cannot describe it (step 2)\n", member);
        return 0;
    }
    std::string cname = str(call_obj(decl, "getName", "()Ljava/lang/String;"));
    std::replace(cname.begin(), cname.end(), '.', '/');
    re->DeleteLocalRef(decl);
    jclass mc = re->GetObjectClass(m);
    jmethodID mods = mc ? re->GetMethodID(mc, "getModifiers", "()I") : nullptr;
    const bool is_static = mods && (re->CallIntMethod(m, mods) & 0x0008) != 0;
    jclass ctor_class = re->FindClass("java/lang/reflect/Constructor");
    const bool is_ctor = !field && ctor_class && re->IsInstanceOf(m, ctor_class);
    if (ctor_class) {
        re->DeleteLocalRef(ctor_class);
    }
    if (mc) {
        re->DeleteLocalRef(mc);
    }
    std::string text;
    if (field) {
        const std::string name = str(call_obj(m, "getName", "()Ljava/lang/String;"));
        jobject type = call_obj(m, "getType", "()Ljava/lang/Class;");
        const std::string d = type ? desc(type) : "";
        if (type) {
            re->DeleteLocalRef(type);
        }
        if (name.empty() || d.empty()) {
            std::printf("[jni] FromReflected %08x: cannot describe it (step 3)\n", member);
            return 0;
        }
        text = "field " + cname + "." + name + ":" + d;
    } else {
        const std::string name = is_ctor ? "<init>" : str(call_obj(m, "getName", "()Ljava/lang/String;"));
        std::string sig = "(";
        jobjectArray params = static_cast<jobjectArray>(call_obj(m, "getParameterTypes", "()[Ljava/lang/Class;"));
        const jsize np = params ? re->GetArrayLength(params) : 0;
        for (jsize i = 0; i < np; i++) {
            jobject pc = re->GetObjectArrayElement(params, i);
            sig += desc(pc);
            re->DeleteLocalRef(pc);
        }
        if (params) {
            re->DeleteLocalRef(params);
        }
        sig += ")";
        if (is_ctor) {
            sig += "V";
        } else {
            jobject rt = call_obj(m, "getReturnType", "()Ljava/lang/Class;");
            sig += rt ? desc(rt) : "";
            if (rt) {
                re->DeleteLocalRef(rt);
            }
        }
        if (name.empty() || sig.back() == ')') {
            std::printf("[jni] FromReflected %08x: cannot describe it (step 4)\n", member);
            return 0;
        }
        text = std::string(is_static ? "static " : "") + cname + "." + name + sig;
    }
    re->ExceptionClear();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::uint32_t c = 0;
    if (!JniAllocObjectHandle(&c)) {
        std::printf("[jni] FromReflected %08x: cannot describe it (step 5)\n", member);
        return 0;
    }
    std::memcpy(H(c), &c, 4);
    jni_methods_[c] = text;
    {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 60) {
            std::printf("[jni] FromReflected %s -> %08x\n", text.c_str(), c);
        }
    }
    return c;
#else
    (void)member;
    (void)field;
    return 0;
#endif
}

#ifdef __ANDROID__
// Java copies of guest strings live for one guest JNI call: a thread attached
// from native code has no frame to drop them, they would stay until it detaches
// and fill its local reference table.
thread_local std::vector<void*> t_java_copies;

Runtime::JavaCopies::JavaCopies() : from_(t_java_copies.size()) {}

Runtime::JavaCopies::~JavaCopies() {
    if (t_java_copies.size() > from_) {
        JNIEnv* re = RealEnv();
        while (t_java_copies.size() > from_) {
            re->DeleteLocalRef(static_cast<jobject>(t_java_copies.back()));
            t_java_copies.pop_back();
        }
    }
}

// The one Java object a guest string locks on: a copy made for each call would
// be a new monitor every time. Kept like the jni_strings_ entry it stands for.
void* Runtime::StringMonitorObject(std::uint32_t handle) {
    JNIEnv* re = RealEnv();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto js = jni_strings_.find(handle);
    if (!re || js == jni_strings_.end()) {
        return nullptr;
    }
    const auto it = jni_string_monitors_.find(handle);
    if (it != jni_string_monitors_.end()) {
        return it->second;
    }
    jobject local = re->NewStringUTF(js->second.c_str());
    void* global = local ? re->NewGlobalRef(local) : nullptr;
    if (local) {
        re->DeleteLocalRef(local);
    }
    if (global) {
        jni_string_monitors_[handle] = global;
    }
    return global;
}
#endif

// A guest string is text with no Java object behind it: one line per kind of
// use as an object, the first time.
static void NoteStringAsObject(unsigned kind, const char* use) {
    static std::atomic<unsigned> said{0};
    if (!(said.fetch_or(kind) & kind)) {
        std::printf("[jni] a guest string used as an object: %s\n", use);
    }
}

void* Runtime::JavaObjectFor(std::uint32_t handle) {
    if (!handle) {
        return nullptr;
    }
    if (void* o = UnwrapObject(handle)) {
        return o;
    }
#ifdef __ANDROID__
    JNIEnv* re = RealEnv();
    if (!re) {
        return nullptr;
    }
    std::string text, cls;
    bool is_string = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto js = jni_strings_.find(handle);
        if (js != jni_strings_.end()) {
            text = js->second;
            is_string = true;
        } else {
            const auto jc = jni_classes_.find(handle);
            if (jc != jni_classes_.end()) {
                cls = jc->second;
            }
        }
    }
    if (is_string) {
        jstring copy = re->NewStringUTF(text.c_str());
        if (copy) {
            t_java_copies.push_back(copy);
        }
        return copy;
    }
    if (!cls.empty()) {
        return RealClass(re, cls);
    }
#endif
    return nullptr;
}

#ifdef __ANDROID__
void* Runtime::RealClass(void* real_env, const std::string& cls) {
    JNIEnv* re = static_cast<JNIEnv*>(real_env);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = real_classes_.find(cls);
        if (it != real_classes_.end()) {
            return it->second;
        }
    }
    jclass lc = re->FindClass(cls.c_str());
    if (!lc || re->ExceptionCheck()) {
        re->ExceptionClear();
        lc = nullptr;
        if (void* app_loader = app_class_loader_.load()) {
            // A thread attached from native code: ask the app's loader.
            jobject loader = static_cast<jobject>(app_loader);
            jclass lcls = re->GetObjectClass(loader);
            jmethodID load = lcls ? re->GetMethodID(lcls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
            std::string dotted = cls;
            std::replace(dotted.begin(), dotted.end(), '/', '.');
            jstring jn = re->NewStringUTF(dotted.c_str());
            lc = load ? static_cast<jclass>(re->CallObjectMethod(loader, load, jn)) : nullptr;
            if (re->ExceptionCheck()) {
                re->ExceptionClear();
                lc = nullptr;
            }
            re->DeleteLocalRef(jn);
            if (lcls) {
                re->DeleteLocalRef(lcls);
            }
        }
        if (!lc) {
            return nullptr;
        }
    }
    jclass g = static_cast<jclass>(re->NewGlobalRef(lc));
    re->DeleteLocalRef(lc);
    if (!g) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = real_classes_.emplace(cls, g);
    if (!it.second) {
        re->DeleteGlobalRef(g);  // another thread cached it first
    }
    return it.first->second;
}
#endif

#ifdef __ANDROID__
// Clears a pending Java exception, naming it first (capped): a bridged call
// that failed on the Java side used to vanish into a NULL/0 result.
static void ClearJavaException(JNIEnv* re, const std::string& what) {
    jthrowable t = re->ExceptionOccurred();
    if (!t) {
        return;
    }
    re->ExceptionClear();
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 50) {
        jclass c = re->GetObjectClass(t);
        jmethodID ts = c ? re->GetMethodID(c, "toString", "()Ljava/lang/String;") : nullptr;
        jstring js = ts ? static_cast<jstring>(re->CallObjectMethod(t, ts)) : nullptr;
        re->ExceptionClear();
        const char* u = js ? re->GetStringUTFChars(js, nullptr) : nullptr;
        std::printf("[jni] Java exception in %.160s: %s\n", what.c_str(), u ? u : "?");
        if (u) {
            re->ReleaseStringUTFChars(js, u);
        }
        if (js) {
            re->DeleteLocalRef(js);
        }
        if (c) {
            re->DeleteLocalRef(c);
        }
    }
    re->DeleteLocalRef(t);
}
#endif

#ifdef __ANDROID__
// Real-call delegation for Call*Method (product host): resolve mock
// class/method cookies to real ART refs (cached), marshal guest args to
// jvalues (unwrapping objects, materializing strings), invoke the real
// A-form, wrap object results. Returns true if fully handled (Ret done).
// Any failure -> false (caller falls through to tuned mocks).
bool Runtime::RealCallObject(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1,
                             std::uint32_t a2, std::uint32_t a3, Regs& regs,
                             const std::string& mdesc) {
    if (DiagOn()) {
        std::printf("[realcall] fn=%u obj=%08x m=%.90s\n", fn, a1, mdesc.c_str());
    }
    JNIEnv* re = GuestCallbacks::g_real_env_hook
                       ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                       : nullptr;
    if (!re) {
        return false;
    }
    // JNINativeInterface: Call<T>Method / Call<T>MethodV / Call<T>MethodA come
    // in triples from 34 (Object) to 61-63 (Void); the static family from 114
    // to 141-143 (jni.h). The position in the triple is the argument form.
    const bool is_static = fn >= 114;
    const std::uint32_t family_base = is_static ? 114u : 34u;
    if (fn < family_base || fn > family_base + 29u) {
        return false;
    }
    const int vtag = static_cast<int>((fn - family_base) % 3u);
    // Split "[static|field ]com/a/B.name(sig)" -> cls, name, sig, ret.
    // (Instance methods carry a legacy "field " tag from the mock path.)
    std::string d = mdesc;
    for (const std::string& pfx : {"static ", "field "}) {
        if (d.compare(0, pfx.size(), pfx) == 0) {
            d = d.substr(pfx.size());
            break;
        }
    }
    const std::size_t dot = d.rfind('.');
    const std::size_t par = d.find('(');
    if (dot == std::string::npos || par == std::string::npos || dot > par) {
        return false;
    }
    const std::string cls = d.substr(0, dot);
    const std::string name = d.substr(dot + 1, par - dot - 1);
    const std::string sig = d.substr(par);
    const std::size_t rp = sig.find(')');
    if (rp == std::string::npos) {
        return false;
    }
    const std::string argstr = sig.substr(1, rp - 1);
    const std::string retstr = sig.substr(rp + 1);
    // An offline title must not learn the phone's real network state from
    // rcs.core.Utils.networkType() (wifi/cellular): the mock answers like a
    // phone without network (see offline_refuses).
    if (!network_allowed_ && cls == "com/rovio/rcs/core/Utils" && name == "networkType") {
        return false;
    }
    // Real class (global-cached) + method (per-cookie cached).
    jclass rclazz = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = real_classes_.find(cls);
        if (it != real_classes_.end()) {
            rclazz = static_cast<jclass>(it->second);
        }
    }
    if (!rclazz) {
        rclazz = static_cast<jclass>(RealClass(re, cls));  // + the app loader fallback
    }
    if (!rclazz) {
        jclass lc = re->FindClass(cls.c_str());
        if (!lc || re->ExceptionCheck()) {
            ClearJavaException(re, mdesc);
            return false;
        }
        rclazz = static_cast<jclass>(re->NewGlobalRef(lc));
        re->DeleteLocalRef(lc);
        if (!rclazz) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        real_classes_[cls] = rclazz;
    }
    jmethodID rmid = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = real_methods_.find(a2);
        if (it != real_methods_.end()) {
            rmid = static_cast<jmethodID>(it->second);
        }
    }
    if (!rmid) {
        rmid = is_static ? re->GetStaticMethodID(rclazz, name.c_str(), sig.c_str())
                         : re->GetMethodID(rclazz, name.c_str(), sig.c_str());
        if (!rmid || re->ExceptionCheck()) {
            ClearJavaException(re, mdesc);
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        real_methods_[a2] = rmid;
    }
    jobject robj = nullptr;
    if (!is_static) {
        robj = static_cast<jobject>(UnwrapObject(a1));
        if (!robj) {
            bool guest_string = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                guest_string = jni_strings_.count(a1) != 0;
            }
            if (guest_string) {
                NoteStringAsObject(1, "method receiver");
                robj = static_cast<jobject>(JavaObjectFor(a1));
            }
        }
        if (!robj) {
            return false;
        }
    }
    // Marshal guest args -> jvalues (absolute AAPCS word index from r0).
    jvalue jv[16];
    int nj = 0;
    std::uint32_t word_idx = 3;  // next unread word position (r3 first)
    auto next_word = [&]() -> std::uint32_t {
        std::uint32_t w = 0;
        if (word_idx == 3) {
            w = a3;
        } else if (CheckRange(regs[13] + (word_idx - 4) * 4, 4, false)) {
            std::memcpy(&w, H(regs[13] + (word_idx - 4) * 4), 4);
        }
        word_idx++;
        return w;
    };
    auto next_va = [&]() -> std::uint32_t {
        // V form: a3 = guest va_list (sequential words); A form handled below.
        std::uint32_t w = 0;
        if (CheckRange(a3 + (word_idx - 3) * 4, 4, false)) {
            std::memcpy(&w, H(a3 + (word_idx - 3) * 4), 4);
        }
        word_idx++;
        return w;
    };
    bool ok = true;
    for (std::size_t i = 0; i < argstr.size() && ok;) {
        char c = argstr[i];
        std::string full;
        if (c == 'L') {
            const std::size_t sc = argstr.find(';', i);
            if (sc == std::string::npos) {
                ok = false;
                break;
            }
            full = argstr.substr(i, sc - i + 1);
            i = sc + 1;
        } else if (c == '[') {
            std::size_t j = i;
            while (j < argstr.size() && argstr[j] == '[') {
                j++;
            }
            if (j < argstr.size() && argstr[j] == 'L') {
                const std::size_t sc = argstr.find(';', j);
                if (sc == std::string::npos) {
                    ok = false;
                    break;
                }
                full = argstr.substr(i, sc - i + 1);
                i = sc + 1;
            } else {
                full = argstr.substr(i, j - i + 1);
                i = j + 1;
            }
        } else {
            full = std::string(1, c);
            i++;
        }
        if (nj >= 16) {
            ok = false;
            break;
        }
        const char k = full[0];
        // A float passed through "..." (and so through a va_list) is promoted
        // to double (C default argument promotions): in the plain and V forms
        // it occupies an aligned 8-byte slot like D. Only the A form (jvalue
        // array) carries a real 4-byte float.
        const bool promoted_float = (k == 'F' && vtag != 2);
        if (k == 'J' || k == 'D' || promoted_float) {
            if (vtag == 2) {  // A form: 8B slots
                std::uint64_t u = 0;
                if (CheckRange(a3 + (std::uint32_t)nj * 8, 8, false)) {
                    std::memcpy(&u, H(a3 + (std::uint32_t)nj * 8), 8);
                }
                if (k == 'J') {
                    jv[nj].j = (jlong)u;
                } else {
                    double dd = 0;
                    std::memcpy(&dd, &u, 8);
                    jv[nj].d = dd;
                }
            } else {
                if (vtag == 0 && ((word_idx) & 1u)) {
                    next_word();  // AAPCS align
                } else if (vtag == 1) {
                    // AAPCS va_arg: 8-byte reads align the ADDRESS up. The
                    // compiler inserts a pad word when the va base is
                    // 4-mod-8 (proven live: AudioOutput.<init>(JIIII)V reads
                    // sane rate/ch/bits only past it). Absolute-address rule:
                    // correct whether or not a pad exists.
                    const std::uint32_t at = a3 + (word_idx - 3u) * 4u;
                    if (at & 7u) {
                        next_va();
                    }
                }
                const std::uint32_t lo = (vtag == 0) ? next_word() : next_va();
                const std::uint32_t hi = (vtag == 0) ? next_word() : next_va();
                if (k == 'J') {
                    jv[nj].j = ((std::uint64_t)hi << 32) | lo;
                } else {
                    const std::uint64_t u = ((std::uint64_t)hi << 32) | lo;
                    double dd = 0;
                    std::memcpy(&dd, &u, 8);
                    if (promoted_float) {
                        jv[nj].f = static_cast<float>(dd);
                    } else {
                        jv[nj].d = dd;
                    }
                }
            }
        } else if (k == 'F') {  // A form only (see promoted_float above)
            std::uint32_t w = 0;
            if (CheckRange(a3 + (std::uint32_t)nj * 8, 4, false)) {
                std::memcpy(&w, H(a3 + (std::uint32_t)nj * 8), 4);
            }
            float f = 0;
            std::memcpy(&f, &w, 4);
            jv[nj].f = f;
        } else if (k == 'L' || k == '[') {
            std::uint32_t w = 0;
            if (vtag == 2) {
                std::uint64_t u = 0;
                if (CheckRange(a3 + (std::uint32_t)nj * 8, 8, false)) {
                    std::memcpy(&u, H(a3 + (std::uint32_t)nj * 8), 8);
                }
                w = (std::uint32_t)u;
            } else {
                w = (vtag == 0) ? next_word() : next_va();
            }
            jobject ro = nullptr;
            if (w) {
                // A wrapped object, a guest string as a java.lang.String, a
                // class from FindClass as its real class. Any other guest
                // handle (mock object) has none: the real call is abandoned.
                ro = static_cast<jobject>(JavaObjectFor(w));
                if (!ro) {
                    ok = false;
                    break;
                }
            }
            jv[nj].l = ro;
        } else {
            std::uint32_t w = 0;
            if (vtag == 2) {
                if (CheckRange(a3 + (std::uint32_t)nj * 8, 4, false)) {
                    std::memcpy(&w, H(a3 + (std::uint32_t)nj * 8), 4);
                }
            } else {
                w = (vtag == 0) ? next_word() : next_va();
            }
            jv[nj].i = (jint)w;
        }
        nj++;
    }
    if (!ok) {
        return false;
    }
    // Invoke by return type (A-forms).
    if (retstr == "V") {
        if (is_static) {
            re->CallStaticVoidMethodA(rclazz, rmid, jv);
        } else {
            re->CallVoidMethodA(robj, rmid, jv);
        }
        if (re->ExceptionCheck()) {
            ClearJavaException(re, mdesc);
        }
        Ret(regs, 0);
        return true;
    }
    if (retstr.size() == 1 && std::string("ZBCSIJFD").find(retstr[0]) != std::string::npos) {
        jlong lv = 0;
        switch (retstr[0]) {
            case 'Z':
                lv = is_static ? re->CallStaticBooleanMethodA(rclazz, rmid, jv)
                               : re->CallBooleanMethodA(robj, rmid, jv);
                break;
            case 'B':
                lv = is_static ? re->CallStaticByteMethodA(rclazz, rmid, jv)
                               : re->CallByteMethodA(robj, rmid, jv);
                break;
            case 'C':
                lv = is_static ? re->CallStaticCharMethodA(rclazz, rmid, jv)
                               : re->CallCharMethodA(robj, rmid, jv);
                break;
            case 'S':
                lv = is_static ? re->CallStaticShortMethodA(rclazz, rmid, jv)
                               : re->CallShortMethodA(robj, rmid, jv);
                break;
            case 'I':
                lv = is_static ? re->CallStaticIntMethodA(rclazz, rmid, jv)
                               : re->CallIntMethodA(robj, rmid, jv);
                break;
            case 'J':
                lv = is_static ? re->CallStaticLongMethodA(rclazz, rmid, jv)
                               : re->CallLongMethodA(robj, rmid, jv);
                break;
            case 'F': {
                const jfloat f = is_static ? re->CallStaticFloatMethodA(rclazz, rmid, jv)
                                           : re->CallFloatMethodA(robj, rmid, jv);
                std::uint32_t w = 0;
                std::memcpy(&w, &f, 4);
                lv = w;
                break;
            }
            case 'D': {
                const jdouble d = is_static ? re->CallStaticDoubleMethodA(rclazz, rmid, jv)
                                           : re->CallDoubleMethodA(robj, rmid, jv);
                std::uint64_t u = 0;
                std::memcpy(&u, &d, 8);
                Ret(regs, (std::uint32_t)u, (std::uint32_t)(u >> 32));
                if (re->ExceptionCheck()) {
                    ClearJavaException(re, mdesc);
                }
                return true;
            }
        }
        if (retstr[0] == 'J') {
            Ret(regs, (std::uint32_t)(std::uint64_t)lv, (std::uint32_t)((std::uint64_t)lv >> 32));
        } else {
            Ret(regs, (std::uint32_t)(std::uint64_t)lv);
        }
        if (re->ExceptionCheck()) {
            ClearJavaException(re, mdesc);
        }
        return true;
    }
    // Object/String/array: real call, then guest-usable result.
    {
        jobject ro = is_static ? re->CallStaticObjectMethodA(rclazz, rmid, jv)
                               : re->CallObjectMethodA(robj, rmid, jv);
        if (re->ExceptionCheck() || !ro) {
            ClearJavaException(re, mdesc);
            Ret(regs, 0);
            return true;
        }
        if (retstr == "Ljava/lang/String;") {
            const char* p = re->GetStringUTFChars(static_cast<jstring>(ro), nullptr);
            std::uint32_t g = 0;
            std::string content;
            if (p) {
                const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(p));
                content.assign(p, n);
                g = GuestMalloc(n + 1);
                if (g) {
                    std::memcpy(H(g), p, n + 1);
                }
                re->ReleaseStringUTFChars(static_cast<jstring>(ro), p);
            }
            re->DeleteLocalRef(ro);
            if (!g) {
                return false;
            }
            // Intern the delegated copy (see above).
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                jni_strings_[g] = content;
            }
            jni_string_results_.fetch_add(1, std::memory_order_relaxed);
            Ret(regs, g);
            return true;
        }
        const std::uint32_t h = WrapObject(ro);
        re->DeleteLocalRef(ro);
        Ret(regs, h);
        return true;
    }
}

// Real NewObject (product host): resolve mock class/<init> cookies to
// real ART refs (cached), marshal the V-form va_list (a3) to jvalues,
// NewObjectA, Wrap. False = caller falls through to mock objects.
// Argument layout follows AAPCS32 / C varargs like RealCallObject: J/D (and
// F, promoted to double) take an aligned word pair; the A form is a jvalue
// array with one 8-byte slot per argument.
bool Runtime::RealNewObject(std::uint32_t fn, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3,
                            Regs& regs, const std::string& mdesc) {
    if (DiagOn()) {
        std::printf("[realnew] %s\n", mdesc.c_str());
    }
    JNIEnv* re = GuestCallbacks::g_real_env_hook
                     ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                     : nullptr;
    if (!re) {
        std::printf("[realnew] no real env\n");
        return false;
    }
    const int vtag = (fn == 28) ? 0 : ((fn == 29) ? 1 : 2);
    std::string d = mdesc;
    const std::size_t dot = d.find(".<init>");
    const std::size_t par = d.find('(');
    if (dot == std::string::npos || par == std::string::npos || dot > par) {
        return false;
    }
    const std::string cls = d.substr(0, dot);
    const std::string sig = d.substr(par);
    const std::size_t rp = sig.find(')');
    if (rp == std::string::npos) {
        return false;
    }
    // Blast-radius guard (2026-10-14): real objects break mock-handle
    // lookups downstream (GetObjectClass/jni_arrays/jni_strings miss on
    // wrapped handles -> game diverges -> black after LOADING). Only
    // AudioOutput goes real until lookups delegate too.
    {
        // Every class gets a real object (it used to be Rovio's AudioOutput
        // only; any other class got a mock without fields, and a Unity guest
        // reads DisplayMetrics.widthPixels off its own new DisplayMetrics()).
        // Each class is named once, so a gate shows what a title constructs.
        static std::set<std::string> seen;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (seen.insert(cls).second) {
            std::printf("[jni] NewObject %s: a real object\n", cls.c_str());
        }
    }
    const std::string argstr = sig.substr(1, rp - 1);
    jclass rclazz = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = real_classes_.find(cls);
        if (it != real_classes_.end()) {
            rclazz = static_cast<jclass>(it->second);
        }
    }
    if (!rclazz) {
        rclazz = static_cast<jclass>(RealClass(re, cls));  // + the app loader fallback
    }
    if (!rclazz) {
        jclass lc = re->FindClass(cls.c_str());
        if (!lc || re->ExceptionCheck()) {
            std::printf("[realnew] FindClass %s FAILED\n", cls.c_str());
            ClearJavaException(re, mdesc);
            return false;
        }
        rclazz = static_cast<jclass>(re->NewGlobalRef(lc));
        re->DeleteLocalRef(lc);
        if (!rclazz) {
            std::printf("[realnew] NewGlobalRef FAILED\n");
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        real_classes_[cls] = rclazz;
    }
    jmethodID rmid = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = real_methods_.find(a2);
        if (it != real_methods_.end()) {
            rmid = static_cast<jmethodID>(it->second);
        }
    }
    if (!rmid) {
        rmid = re->GetMethodID(rclazz, "<init>", sig.c_str());
        if (!rmid || re->ExceptionCheck()) {
            std::printf("[realnew] GetMethodID <init>%s FAILED\n", sig.c_str());
            ClearJavaException(re, mdesc);
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        real_methods_[a2] = rmid;
    }
    jvalue jv[16];
    int nj = 0;
    std::uint32_t word_idx = 3;
    auto next_va = [&]() -> std::uint32_t {
        std::uint32_t w = 0;
        if (CheckRange(a3 + (word_idx - 3) * 4, 4, false)) {
            std::memcpy(&w, H(a3 + (word_idx - 3) * 4), 4);
        }
        word_idx++;
        return w;
    };
    auto next_w = [&]() -> std::uint32_t {
        // vtag 0 (direct): r3 then guest stack; else va_list. Only V (1)
        // and A (2) occur for ctors in practice; A-form reads jvalue slots.
        std::uint32_t w = 0;
        if (vtag == 0) {
            if (word_idx == 3) {
                w = a3;
            } else if (CheckRange(regs[13] + (word_idx - 4) * 4, 4, false)) {
                std::memcpy(&w, H(regs[13] + (word_idx - 4) * 4), 4);
            }
            word_idx++;
        } else if (vtag == 1) {
            w = next_va();
        } else {
            std::uint64_t u = 0;
            if (CheckRange(a3 + (std::uint32_t)nj * 8, 8, false)) {
                std::memcpy(&u, H(a3 + (std::uint32_t)nj * 8), 8);
            }
            w = (std::uint32_t)u;
        }
        return w;
    };
    bool ok = true;
    for (std::size_t i = 0; i < argstr.size() && ok;) {
        char c = argstr[i];
        std::string full;
        if (c == 'L') {
            const std::size_t sc = argstr.find(';', i);
            if (sc == std::string::npos) {
                ok = false;
                break;
            }
            full = argstr.substr(i, sc - i + 1);
            i = sc + 1;
        } else if (c == '[') {
            std::size_t j = i;
            while (j < argstr.size() && argstr[j] == '[') {
                j++;
            }
            if (j < argstr.size() && argstr[j] == 'L') {
                const std::size_t sc = argstr.find(';', j);
                if (sc == std::string::npos) {
                    ok = false;
                    break;
                }
                full = argstr.substr(i, sc - i + 1);
                i = sc + 1;
            } else {
                full = argstr.substr(i, j - i + 1);
                i = j + 1;
            }
        } else {
            full = std::string(1, c);
            i++;
        }
        if (nj >= 16) {
            ok = false;
            break;
        }
        const char k = full[0];
        // J/D take two words; so does F in the plain/V forms, where C's
        // default argument promotion turns a float into a double. The A form
        // is a jvalue array: one 8-byte slot per argument, read whole.
        const bool promoted_float = (k == 'F' && vtag != 2);
        if (k == 'J' || k == 'D' || promoted_float) {
            std::uint64_t u = 0;
            if (vtag == 2) {
                if (CheckRange(a3 + (std::uint32_t)nj * 8, 8, false)) {
                    std::memcpy(&u, H(a3 + (std::uint32_t)nj * 8), 8);
                }
            } else {
                if (vtag == 0 && (word_idx & 1u)) {
                    next_w();  // AAPCS32: 64-bit values take an even register pair
                } else if (vtag == 1) {
                    // va_arg of an 8-byte value aligns the address up (pad word
                    // when the va base is 4-mod-8), as in RealCallObject.
                    const std::uint32_t at = a3 + (word_idx - 3u) * 4u;
                    if (at & 7u) {
                        next_w();
                    }
                }
                const std::uint32_t lo = next_w();
                const std::uint32_t hi = next_w();
                u = ((std::uint64_t)hi << 32) | lo;
            }
            if (k == 'J') {
                jv[nj].j = (jlong)u;
            } else {
                double dd = 0;
                std::memcpy(&dd, &u, 8);
                if (promoted_float) {
                    jv[nj].f = static_cast<float>(dd);
                } else {
                    jv[nj].d = dd;
                }
            }
        } else if (k == 'F') {  // A form: the float in the low 4 bytes of its slot
            const std::uint32_t w = next_w();
            float f = 0;
            std::memcpy(&f, &w, 4);
            jv[nj].f = f;
        } else if (k == 'L' || k == '[') {
            const std::uint32_t w = next_w();
            jobject ro = nullptr;
            if (w) {
                // A wrapped object, a guest string as a java.lang.String, a
                // class from FindClass as its real class. Any other guest
                // handle (mock object) has none: the real call is abandoned.
                ro = static_cast<jobject>(JavaObjectFor(w));
                if (!ro) {
                    ok = false;
                    break;
                }
            }
            jv[nj].l = ro;
        } else {
            jv[nj].i = (jint)next_w();
        }
        nj++;
    }
    if (!ok) {
        return false;
    }
    jobject ro = re->NewObjectA(rclazz, rmid, jv);
    if (re->ExceptionCheck() || !ro) {
        // Name the exception (audio ctor diagnosis).
        jthrowable ex = re->ExceptionOccurred();
        if (ex) {
            jclass ecl = re->GetObjectClass(ex);
            jmethodID tn = ecl ? re->GetMethodID(ecl, "toString", "()Ljava/lang/String;")
                               : nullptr;
            jstring js = (ecl && tn) ? static_cast<jstring>(re->CallObjectMethod(ex, tn))
                                     : nullptr;
            if (js && !re->ExceptionCheck()) {
                const char* c = re->GetStringUTFChars(js, nullptr);
                if (c) {
                    std::printf("[realnew] NewObjectA THREW %s\n", c);
                    re->ReleaseStringUTFChars(js, c);
                }
                re->DeleteLocalRef(js);
            }
            if (ecl) {
                re->DeleteLocalRef(ecl);
            }
            re->DeleteLocalRef(ex);
        } else {
            std::printf("[realnew] NewObjectA returned null\n");
        }
        ClearJavaException(re, mdesc);
        return false;
    }
    const std::uint32_t h = WrapObject(ro);
    re->DeleteLocalRef(ro);
    // Track obj -> class for GetObjectClass (fn#31), exactly like the mock
    // NewObject path does: without this, real objects get a fabricated
    // java/lang/Object and every downstream real lookup misses (e.g. the
    // AudioOutput startOutput chain stays mock -> total silence).
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t ch = 0;
        for (const auto& kv : jni_classes_) {
            if (kv.second == cls) {
                ch = kv.first;
                break;
            }
        }
        if (!ch) {
            if (!JniAllocObjectHandle(&ch)) {
                // Fatal and already halted. Return true so the caller does not
                // fall through to the mock NewObject path after the halt.
                return true;
            }
            jni_classes_[ch] = cls;
        }
        jni_objclass_[h] = ch;
    }
    Ret(regs, h);
    return true;
}
#endif

void Runtime::SetFilesDir(const std::string& guest_prefix, const std::string& host_base) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    files_prefix_ = guest_prefix;
    files_hostbase_ = host_base;
    // /data/data/<pkg>/files or /data/user/<N>/<pkg>/files: the package names
    // the app's external dir (Context.getExternalFilesDir and Unity's
    // persistentDataPath when the storage permission is granted).
    std::string pkg;
    const std::filesystem::path gp(guest_prefix);
    if (gp.filename() == "files" && gp.has_parent_path()) {
        pkg = gp.parent_path().filename().string();
    }
    if (!pkg.empty() && !host_base.empty()) {
        ext_tail_ = "/Android/data/" + pkg;
        ext_hostbase_ = std::filesystem::path(host_base).parent_path().string() + "/ext";
    }
    std::printf("[files] guest \"%s\" -> host \"%s\"\n", guest_prefix.c_str(), host_base.c_str());
}

void Runtime::SetApkPath(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    apk_path_ = path;
    apk_indexed_ = false;
    apk_index_.clear();
    std::printf("[asset] apk \"%s\"\n", path.c_str());
}

std::string Runtime::InitPackedFiles() {
    if (apk_path_.empty() || files_hostbase_.empty()) {
        return "off (no apk or files dir)";
    }
    vfs_.AddRoot("files", files_hostbase_);
    if (!ext_hostbase_.empty()) {
        vfs_.AddRoot("ext", ext_hostbase_);
    }
    if (private_sdcard_) {
        vfs_.AddRoot("sdcard", files_hostbase_ + "/sdcard");
    }
    // The deletion marks live next to the files dir, out of the guest's view.
    const std::string marks =
        std::filesystem::path(files_hostbase_).parent_path().string() + "/abport_deleted.txt";
    const std::string line = vfs_.Build(abvfs::AppApks(apk_path_), marks);
    vfs_on_ = !vfs_.Empty();
    return line;
}

bool JniStringFits(std::uint32_t next, std::size_t len, std::uint32_t* out) {
    if (next < kJniStringStart) {
        return false;
    }
    // Do the arithmetic in 64 bits: next+3 and next+len must not wrap in the
    // guest's 32-bit address space.
    const std::uint64_t aligned = (std::uint64_t(next) + 3u) & ~std::uint64_t(3u);
    if (aligned > 0xFFFFFFFFu || std::uint64_t(len) > 0xFFFFFFFFu) {
        return false;
    }
    if (aligned + static_cast<std::uint64_t>(len) >
        static_cast<std::uint64_t>(kJniStringLimit)) {
        return false;
    }
    if (out) {
        *out = static_cast<std::uint32_t>(aligned);
    }
    return true;
}

bool JniObjectHandleFits(std::uint32_t next, std::uint32_t* out) {
    if (next < kJniObjectStart) {
        return false;
    }
    const std::uint64_t aligned = (std::uint64_t(next) + 3u) & ~std::uint64_t(3u);
    if (aligned > 0xFFFFFFFFu ||
        aligned + 4u > static_cast<std::uint64_t>(kJniObjectLimit)) {
        return false;
    }
    if (out) {
        *out = static_cast<std::uint32_t>(aligned);
    }
    return true;
}

bool Runtime::JniAllocObjectHandle(std::uint32_t* out) {
    std::uint32_t slot = 0;
    if (!JniObjectHandleFits(jni_obj_next_, &slot)) {
        std::printf("[jni] LOUD object-handle arena exhausted next=%08x limit=%08x -- halting\n",
                    jni_obj_next_, kJniObjectLimit);
        stop_step_ = true;
        if (Dynarmic::A32::Jit* jit = ActiveJit()) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
        return false;
    }
    jni_obj_next_ = slot + 4;
#ifdef ABR_DIAG_COUNTERS
    const std::uint32_t obj_used = (jni_obj_next_ - kJniObjectStart) / 4u;
    if (obj_used > diag_.jni_obj_high) {
        diag_.jni_obj_high = obj_used;
    }
#endif
    if (out) {
        *out = slot;
    }
    return true;
}

// jni_strings_ holds modified UTF-8, or the plain UTF-8 a guest gave
// NewStringUTF. Bytes of the character at s[i]; a broken sequence counts as
// one byte, one unit.
static std::size_t Utf8CharBytes(const std::string& s, std::size_t i) {
    const std::uint8_t c = static_cast<std::uint8_t>(s[i]);
    const std::size_t n = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF8 ? 4 : 1;
    if (i + n > s.size()) {
        return 1;
    }
    for (std::size_t k = 1; k < n; k++) {
        if ((static_cast<std::uint8_t>(s[i + k]) & 0xC0) != 0x80) {
            return 1;
        }
    }
    return n;
}

// The code point at s[i]; returns its bytes.
static std::size_t Utf8Char(const std::string& s, std::size_t i, std::uint32_t* cp) {
    const std::size_t n = Utf8CharBytes(s, i);
    std::uint32_t c = static_cast<std::uint8_t>(s[i]);
    if (n > 1) {
        c &= 0xFFu >> (n + 1);
        for (std::size_t k = 1; k < n; k++) {
            c = (c << 6) | (static_cast<std::uint8_t>(s[i + k]) & 0x3Fu);
        }
    }
    *cp = c;
    return n;
}

static std::u16string Utf16OfMutf8(const std::string& s) {
    std::u16string u;
    u.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = 0;
        i += Utf8Char(s, i, &cp);
        if (cp >= 0x10000u) {
            u.push_back(static_cast<char16_t>(0xD800u + ((cp - 0x10000u) >> 10)));
            u.push_back(static_cast<char16_t>(0xDC00u + ((cp - 0x10000u) & 0x3FFu)));
        } else {
            u.push_back(static_cast<char16_t>(cp));
        }
    }
    return u;
}

// Utf16OfMutf8(s).size(), which GetStringLength asks for on every call.
static std::size_t Utf16Length(const std::string& s) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < s.size();) {
        std::uint32_t cp = 0;
        i += Utf8Char(s, i, &cp);
        units += cp >= 0x10000u ? 2 : 1;
    }
    return units;
}

// The bytes of UTF-16 units [start, start + len) of s. Half a surrogate pair
// is three bytes, as ART writes a lone surrogate.
static std::string Mutf8Slice(const std::string& s, std::size_t start, std::size_t len) {
    const std::size_t end = start + len;
    std::string out;
    std::size_t unit = 0;
    for (std::size_t i = 0; i < s.size() && unit < end;) {
        std::uint32_t cp = 0;
        const std::size_t n = Utf8Char(s, i, &cp);
        const std::size_t units = cp >= 0x10000u ? 2 : 1;
        if (unit >= start && unit + units <= end) {
            out.append(s, i, n);
        } else if (units == 2) {
            const std::size_t k = unit >= start ? 0 : 1;  // the half inside
            if (unit + k >= start && unit + k < end) {
                const std::uint32_t half = k == 0 ? 0xD800u + ((cp - 0x10000u) >> 10)
                                                  : 0xDC00u + ((cp - 0x10000u) & 0x3FFu);
                out.push_back(static_cast<char>(0xE0u | (half >> 12)));
                out.push_back(static_cast<char>(0x80u | ((half >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (half & 0x3Fu)));
            }
        }
        unit += units;
        i += n;
    }
    return out;
}

// For the watchdog, which must not wait: false when the lock is busy.
bool Runtime::JniStringStats(std::size_t* count, std::size_t* bytes) {
    std::unique_lock<std::recursive_mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    *count = jni_strings_.size();
    *bytes = 0;
    for (const auto& kv : jni_strings_) {
        *bytes += kv.second.size() + 1;
    }
    return true;
}

std::uint32_t Runtime::NewStringUTF(const char* s) {
    return JniNewString(s ? s : "");
}

std::uint32_t Runtime::JniNewString(const std::string& s) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // +1 for the NUL. Check the payload before adding one: a size of SIZE_MAX
    // would wrap len to zero. The region is only 3840 bytes, so anything huge
    // is a fatal invariant violation, not something to truncate.
    if (s.size() >= 0xFFFFFFFFu) {
        std::printf("[jni] LOUD string too large payload=%zu limit=%08x -- halting\n",
                    s.size(), kJniStringLimit);
        stop_step_ = true;
        if (Dynarmic::A32::Jit* jit = ActiveJit()) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
        return 0;
    }
    const std::size_t len = s.size() + 1;
    std::uint32_t out = 0;
    if (JniStringFits(jni_str_next_, len, &out)) {
        jni_str_next_ = out + static_cast<std::uint32_t>(len);
    } else {
        // The 3840-byte arena only covers boot. Rio keeps creating mock
        // jstrings during play (~24 B/s measured on device, 2026-10-25) and
        // filled it in 160 s, which halted nativeUpdate -> app closed. Past the
        // arena, strings go to the guest heap, like the delegated real-String
        // copies already do. They are never freed: the mock Delete*Ref are
        // no-ops and NewGlobalRef returns the same handle, so freeing on
        // DeleteLocalRef would free a live global.
        out = GuestMalloc(static_cast<std::uint32_t>(len));
        if (!out) {
            std::printf("[jni] LOUD string alloc failed len=%zu -- halting\n", len);
            stop_step_ = true;
            if (Dynarmic::A32::Jit* jit = ActiveJit()) {
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            return 0;
        }
        static bool noted = false;
        if (!noted) {
            noted = true;
            std::printf("[jni] string arena full at %08x, further jstrings on the guest heap\n",
                        jni_str_next_);
        }
#ifdef ABR_DIAG_COUNTERS
        diag_.jni_str_heap_bytes += len;
#endif
    }
    std::memcpy(H(out), s.c_str(), len);
#ifdef ABR_DIAG_COUNTERS
    const std::uint32_t str_used = jni_str_next_ - kJniStringStart;
    if (str_used > diag_.jni_str_high) {
        diag_.jni_str_high = str_used;
    }
#endif
    jni_strings_[out] = s;
    return out;  // opaque jstring handle == UTF-8 buffer VA
}

std::uint32_t Runtime::NewByteArray(std::uint32_t len) {
    const std::uint32_t out = GuestMalloc(len ? len : 1);  // locks internally
    if (!out) {
        return 0;
    }
    std::memset(H(out), 0, len ? len : 1);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    jni_array_lens_[out] = len;
    return out;  // opaque jbyteArray handle == buffer VA
}

void Runtime::CallJni(std::uint32_t fn, Regs& regs) {
    ForgetGlBindings();  // Java code may run: its GL context may change
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
#ifdef __ANDROID__
    JavaCopies java_copies;
#endif
    // Locked scope: worker threads share stub_hits_ with main (map insert is
    // UB raced). Scoped (not whole-body): case thunks lock mutex_ themselves.
    // Diagnostic layer (PLAN.md B2): under HARNESS_DIAG=0 skip the per-call
    // std::to_string(fn) + string concat + map probe entirely.
    if (DiagOn()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint64_t& hits = stub_hits_["#jni:" + std::to_string(fn)];
    // HARNESS_JNIMAX=N: log the first N *total* JNI calls (one full retry cycle
    // is ~15 calls) instead of 5-per-function. Behavior unchanged, log stays small.
    static std::uint64_t jnimax = 0;  // default: legacy 5-per-fn throttle only
    static bool jnimax_init = false;
    if (!jnimax_init) {
        jnimax_init = true;
        if (const char* e = std::getenv("HARNESS_JNIMAX")) {
            jnimax = static_cast<std::uint64_t>(std::strtoul(e, nullptr, 10));
        }
    }
    static std::uint64_t jni_total = 0;
    static bool threadlog = std::getenv("HARNESS_THREADLOG") != nullptr;
    const bool lineal = (GuestTid() == 1) || threadlog;
    if ((++hits <= 5 || jni_total < jnimax) && lineal) {
        std::printf("[jni]%s fn#%u(%08x %08x %08x %08x) lr=%08x%s\n",
                    GuestTid() == 1 ? "" : "(T2+)", fn, a0, a1, a2, a3, regs[14],
                    hits == 5 ? " (quiet henceforth)" : "");
    }
    jni_total++;
    }  // release gate lock before case thunks (they lock themselves)
    // JNI function table indices (jni.h JNINativeInterface, INCLUDES the 4
    // reserved0-3 slots at 0-3; verified against NDK r27 jni.h 2026-10-09).
    // fn >= 0x100: JavaVM table (JNIInvokeInterface, jni.h: 3 reserved,
    // 3 DestroyJavaVM, 4 AttachCurrentThread, 5 DetachCurrentThread,
    // 6 GetEnv, 7 AttachCurrentThreadAsDaemon). Guest threads share the one
    // mock JNIEnv, so attach/detach have nothing else to do.
    if (fn >= 0x100) {
        const std::uint32_t jfn = fn - 0x100;
        if (jfn == 5) {  // DetachCurrentThread
            if (t_worker_jit && t_local_frames.size() == 1) {
                PopLocalFrame();  // the thread's local references end here
                PushLocalFrame();
            }
            Ret(regs, 0);  // JNI_OK
            return;
        }
        if (jfn == 3) {  // DestroyJavaVM: not allowed for an app
            Ret(regs, static_cast<std::uint32_t>(-1));  // JNI_ERR
            return;
        }
        if (jfn == 6 || jfn == 4 || jfn == 7) {  // GetEnv / Attach / AttachAsDaemon
            if (!CheckRange(a1, 4, true)) {
                std::printf("[jvm] fn#%u BAD env_out %08x -- halting\n", jfn, a1);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            std::uint32_t env = jni_env_;
            std::memcpy(H(a1), &env, 4);  // *env_out = JNIEnv*, return JNI_OK
            Ret(regs, 0);
            return;
        }
        std::printf("[svc] LOUD unimplemented JavaVM fn#%u from pc=%08x -- halting\n", jfn,
                    regs[15] - 4);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
#ifdef __ANDROID__
    // Every Call<T>Method / CallStatic<T>Method family goes to the real Java
    // method first (the Object families 34-36/114-116 already did); the mock
    // answers below are only the fallback when the call cannot be made real
    // (no JVM on this thread, a mock-only class/object). The mocks were
    // device answers from 2014 (API 21, 800x480, density group 1...) and
    // dropped every void call, e.g. App.quitRequested (which only calls
    // stopLockTask() in all six titles).
    if ((fn >= 37 && fn <= 63) || (fn >= 117 && fn <= 143)) {
        std::string mdesc;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        if (!mdesc.empty() && RealCallObject(fn, a0, a1, a2, a3, regs, mdesc)) {
            return;
        }
    }
#endif
    switch (fn) {
    // String thunks work off the jni_strings_ registry (mock content model).
    // Unknown handle (e.g. a method cookie misused as a string) = LOUD halt:
    // silent misreads here caused the 131K-throw saga once already.
    case 164: {  // GetStringLength(env, str) -> jsize (UTF-16 units)
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_strings_.find(a1);
        if (it == jni_strings_.end()) {
#ifdef __ANDROID__
            if (void* real = RealString(a1)) {
                Ret(regs, static_cast<std::uint32_t>(
                              RealEnv()->GetStringLength(static_cast<jstring>(real))));
                return;
            }
#endif
            std::printf("[jni] LOUD GetStringLength unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        Ret(regs, static_cast<std::uint32_t>(Utf16Length(it->second)));
        return;
    }
    case 167: {  // NewStringUTF(env, bytes) -> jstring (verified vs NDK jni.h
                 // table order: 164/168/169/170/171 anchors match)
        std::string s;
        if (a1 != 0) {
            if (!CheckRange(a1, 1, false)) {
                std::printf("[jni] LOUD NewStringUTF bad ptr %08x -- halting\n", a1);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            std::uint32_t len = 0;  // no length cap: NewStringUTF has none
            if (!GuestStrnlen(a1, 0xFFFFFFFFu, &len)) {
                std::printf("[jni] LOUD NewStringUTF unterminated/unreadable %08x -- halting\n", a1);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            s.assign(reinterpret_cast<const char*>(H(a1)), len);
        }
        Ret(regs, JniNewString(s));
        return;
    }
    case 168: {  // GetStringUTFLength(env, str) -> jsize (bytes, no NUL)
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_strings_.find(a1);
        if (it == jni_strings_.end()) {
#ifdef __ANDROID__
            // Product host: entry-wrapped real ART string -> UNWRAP FIRST
            // (passing the mock handle to ART aborts: invalid jobject).
            if (void* real = UnwrapObject(a1)) {
                if (JNIEnv* re = RealEnv()) {
                    Ret(regs, static_cast<std::uint32_t>(re->GetStringUTFLength(
                                   static_cast<jstring>(real))));
                    return;
                }
            }
#endif
            std::printf("[jni] LOUD GetStringUTFLength unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        Ret(regs, static_cast<std::uint32_t>(it->second.size()));
        return;
    }
    case 169: {  // GetStringUTFChars(env, str, isCopy) -> const char*
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (jni_strings_.find(a1) == jni_strings_.end()) {
#ifdef __ANDROID__
            // Product host: entry-wrapped real ART string -> UNWRAP FIRST,
            // copy bytes into the guest heap, and release ART's characters
            // immediately. The guest copy is tracked separately from
            // jni_strings_ because it is a const char*, not a jstring;
            // ReleaseStringUTFChars frees it.
            if (void* real = UnwrapObject(a1)) {
                if (JNIEnv* re = RealEnv()) {
                    const char* p = re->GetStringUTFChars(static_cast<jstring>(real),
                                                           nullptr);
                    if (p) {
                        const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(p));
                        const std::uint32_t g = GuestMalloc(n + 1);
                        if (g) {
                            std::memcpy(H(g), p, n + 1);
                            jni_utf_chars_.insert(g);
#ifdef ABR_DIAG_COUNTERS
                            diag_.utf_allocs.fetch_add(1, std::memory_order_relaxed);
#endif
                            re->ReleaseStringUTFChars(static_cast<jstring>(real), p);
                            if (a2 && CheckRange(a2, 1, true)) {  // jboolean* isCopy: 1 byte (jni.h)
                                *H<std::uint8_t>(a2) = 1;
                            }
                            Ret(regs, g);
                            return;
                        }
                        // Allocation failed: still give ART its buffer back
                        // before the LOUD unknown-string path.
                        re->ReleaseStringUTFChars(static_cast<jstring>(real), p);
                    }
                }
            }
#endif
            std::printf("[jni] LOUD GetStringUTFChars unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (a2 && CheckRange(a2, 1, true)) {  // jboolean* isCopy: 1 byte (jni.h)
            *H<std::uint8_t>(a2) = 0;
        }
        Ret(regs, a1);  // handle IS the UTF-8 buffer
        return;
    }
    case 220:  // GetStringRegion(env, str, start, len, buf) -> void (jchar units)
    case 221:  // GetStringUTFRegion(env, str, start, len, buf) -> void (bytes)
    {
        // 5th arg (buf) rides the guest stack at [sp].
        std::uint32_t buf = 0;
        if (!CheckRange(regs[13], 4, false)) {
            std::printf("[jni] region BAD sp %08x -- halting\n", regs[13]);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        std::memcpy(&buf, H(regs[13]), 4);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_strings_.find(a1);
        std::size_t total = 0;
#ifdef __ANDROID__
        JNIEnv* re = nullptr;
        jstring js = nullptr;
#endif
        if (it != jni_strings_.end()) {
            total = Utf16Length(it->second);
        } else {
#ifdef __ANDROID__
            js = static_cast<jstring>(RealString(a1));
#endif
            bool known = false;
#ifdef __ANDROID__
            if (js) {
                re = RealEnv();
                total = static_cast<std::size_t>(re->GetStringLength(js));
                known = true;
            }
#endif
            if (!known) {
                std::printf("[jni] LOUD region unknown str %08x -- halting\n", a1);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
        }
        if (std::uint64_t(a2) + a3 > total) {
            std::printf("[jni] LOUD region [%u+%u) over len %zu -- halting\n", a2, a3, total);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (fn == 220) {
            const std::uint32_t want = a3 * 2;
            if (!CheckRange(buf, want, true)) {
                std::printf("[jni] region BAD buf %08x -- halting\n", buf);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            // BOUND THE WRITE BY THE DESTINATION ALLOCATION (2026-10-17; kept
            // under D1 as force=true because it is a functional bound, not a
            // firewall): the game sizes `buf` from its own length bookkeeping,
            // so an unchecked UTF-16 widen overran a 24 B block into the next
            // chunk's head -- exactly the metadata smear that detonated in
            // mspace_free (ok_inuse failed on a head overwritten by wide
            // chars). CheckRange only proves the mapping is writable (the whole
            // arena is).
            const std::uint32_t wlen = ClampHeapWrite(buf, want, "GetStringRegion", true);
            std::u16string units;
#ifdef __ANDROID__
            if (js) {
                units.resize(wlen / 2);
                re->GetStringRegion(js, static_cast<jsize>(a2), static_cast<jsize>(units.size()),
                                    reinterpret_cast<jchar*>(&units[0]));
                re->ExceptionClear();
            } else
#endif
            {
                units = Utf16OfMutf8(it->second).substr(a2, wlen / 2);
            }
            std::memcpy(H(buf), units.data(), units.size() * 2);
        } else {
            std::string bytes;
#ifdef __ANDROID__
            if (js) {
                bytes.resize(std::size_t(a3) * 3 + 1);  // and ART's NUL, on some versions
                re->GetStringUTFRegion(js, static_cast<jsize>(a2), static_cast<jsize>(a3), &bytes[0]);
                re->ExceptionClear();
                bytes.resize(std::strlen(bytes.c_str()));
            } else
#endif
            {
                bytes = Mutf8Slice(it->second, a2, a3);
            }
            const std::uint32_t want = static_cast<std::uint32_t>(bytes.size());
            if (!CheckRange(buf, want, true)) {
                std::printf("[jni] region BAD buf %08x -- halting\n", buf);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            const std::uint32_t wlen = ClampHeapWrite(buf, want, "GetStringUTFRegion", true);
            std::memcpy(H(buf), bytes.data(), wlen);
        }
        {
            // Triage (capped): name the string, the destination and the size we
            // believe the destination allocation has. alloc=0 = not a heap
            // block (jni arena / static) -> ClampHeapWrite left it unclamped.
            static int rn = 0;
            if (DiagOn() && rn < 8) {
                rn++;
                std::uint32_t asz = LiveIdxFind(buf);
                std::printf("[jni] region%s str=%08x start=%u len=%u buf=%08x alloc=%u\n",
                            fn == 220 ? "U" : "", a1, a2, a3, buf, asz);
            }
        }
        Ret(regs, 0);
        return;
    }
    case 222: {  // GetPrimitiveArrayCritical(env, array, isCopy) -> void*
#ifdef __ANDROID__
        // A real Java array: a guest copy (the element size from the array's
        // class, "[I" and so on), copied back by ReleasePrimitiveArrayCritical.
        if (JNIEnv* re = RealEnv()) {
            if (jarray ra = static_cast<jarray>(UnwrapObject(a1))) {
                const std::uint32_t es = JniArrayElemSize(a1);
                const jsize n = re->GetArrayLength(ra);
                const std::uint64_t bytes = std::uint64_t(n > 0 ? n : 0) * es;
                const std::uint32_t gb = (es && bytes < 0x10000000u)
                                             ? GuestMalloc(static_cast<std::uint32_t>(bytes ? bytes : 1))
                                             : 0;
                void* hp = gb ? re->GetPrimitiveArrayCritical(ra, nullptr) : nullptr;
                if (hp) {
                    std::memcpy(H(gb), hp, static_cast<std::size_t>(bytes));
                    re->ReleasePrimitiveArrayCritical(ra, hp, JNI_ABORT);
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    array_pins_[gb] = ArrayPin{ra, nullptr, gb, static_cast<std::uint32_t>(bytes), -1};
                } else if (gb) {
                    GuestFree(gb);
                }
                re->ExceptionClear();
                if (a2 != 0 && CheckRange(a2, 1, true)) {
                    *H(a2) = 1;
                }
                Ret(regs, hp ? gb : 0);
                return;
            }
        }
#endif
        // Our arrays ARE guest buffers (NewByteArray/JniNewArray hand out the
        // buffer VA), so the critical pointer is the handle itself and isCopy
        // is JNI_FALSE (0). (Was mis-mapped as GetStringUTFRegion, which read a
        // non-existent 5th stack arg as a destination -- see the 220/221 shift.)
        if (a2 != 0 && CheckRange(a2, 1, true)) {
            *H(a2) = 0;
        }
        Ret(regs, a1);
        return;
    }
    case 170: {  // ReleaseStringUTFChars(env, str, chars) -> void
        // A guest copy from the real-wrapped GetStringUTFChars path is ours;
        // free it. Mock jstrings return the handle itself and are not in this
        // set, so their registry entry stays intact.
        bool tracked = false;
        if (a2) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            tracked = jni_utf_chars_.erase(a2) != 0;
        }
        if (tracked) {
#ifdef ABR_DIAG_COUNTERS
            diag_.utf_releases.fetch_add(1, std::memory_order_relaxed);
#endif
            GuestFree(a2);
        }
        Ret(regs, 0);  // void JNI: zero r0 (device Dalvik leaves 0; game reads it)
        return;
    }
    case 24: {  // IsSameObject(env, obj1, obj2) -> jboolean
        // Two references to one real object are two handles: the JVM decides.
        if (a1 != a2) {
#ifdef __ANDROID__
            void* r1 = a2 ? JavaObjectFor(a1) : nullptr;
            void* r2 = a1 ? JavaObjectFor(a2) : nullptr;
            JNIEnv* re = (r1 && r2) ? RealEnv() : nullptr;
            if (re) {
                Ret(regs, re->IsSameObject(static_cast<jobject>(r1), static_cast<jobject>(r2)) ? 1u : 0u);
                return;
            }
#endif
            Ret(regs, 0);
            return;
        }
        Ret(regs, 1);  // covers NULL==NULL
        return;
    }
    case 31: {  // GetObjectClass(env, obj) -> jclass (verified vs NDK jni.h)
#ifdef __ANDROID__
        // A real Java object (wrapped handle): its runtime class, by name, so
        // GetMethodID/GetFieldID on it resolve against the real class (they
        // used to land on a made-up java/lang/Object and fall back to mocks).
        if (void* real = UnwrapObject(a1)) {
            if (JNIEnv* re = RealEnv()) {
                std::string cname;
                jclass oc = re->GetObjectClass(static_cast<jobject>(real));
                jclass cc = oc ? re->FindClass("java/lang/Class") : nullptr;
                jmethodID gn = cc ? re->GetMethodID(cc, "getName", "()Ljava/lang/String;") : nullptr;
                jstring js = gn ? static_cast<jstring>(re->CallObjectMethod(oc, gn)) : nullptr;
                if (js) {
                    if (const char* u = re->GetStringUTFChars(js, nullptr)) {
                        cname = u;
                        re->ReleaseStringUTFChars(js, u);
                    }
                    re->DeleteLocalRef(js);
                }
                if (cc) {
                    re->DeleteLocalRef(cc);
                }
                if (oc) {
                    re->DeleteLocalRef(oc);
                }
                if (re->ExceptionCheck()) {
                    re->ExceptionClear();
                    cname.clear();
                }
                for (char& c : cname) {
                    if (c == '.') {
                        c = '/';  // Class.getName() is dotted; JNI names use '/'
                    }
                }
                if (!cname.empty()) {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    std::uint32_t ch = 0;
                    for (const auto& kv : jni_classes_) {
                        if (kv.second == cname) {
                            ch = kv.first;
                            break;
                        }
                    }
                    if (!ch) {
                        if (!JniAllocObjectHandle(&ch)) {
                            return;
                        }
                        std::memcpy(H(ch), &ch, 4);  // self-marked handle word
                        jni_classes_[ch] = cname;
                    }
                    Ret(regs, ch);
                    return;
                }
            }
        }
#endif
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t ch = 0;
        const auto it = jni_objclass_.find(a1);
        const bool guest_string = jni_strings_.count(a1) != 0;
        if (it != jni_objclass_.end()) {
            ch = it->second;
        } else {
            // Unknown object (e.g. array): fabricate java/lang/Object.
            const char* const cname = guest_string ? "java/lang/String" : "java/lang/Object";
            if (guest_string) {
                NoteStringAsObject(2, "GetObjectClass");
            }
            for (const auto& kv : jni_classes_) {
                if (kv.second == cname) {
                    ch = kv.first;
                    break;
                }
            }
            if (!ch) {
                if (!JniAllocObjectHandle(&ch)) {
                    return;
                }
                jni_classes_[ch] = cname;
            }
            if (a1) {
                jni_objclass_[a1] = ch;
            }
        }
        if (DiagOn()) {
            std::printf("[jni] GetObjectClass %08x -> %08x\n", a1, ch);
        }
        Ret(regs, ch);
        return;
    }
    case 23:   // DeleteLocalRef(env, obj) -> void
    case 22:   // DeleteGlobalRef(env, obj) -> void
    case 227:  // DeleteWeakGlobalRef(env, ref) -> void
        // A real object's handle of the matching kind goes (a local one also
        // leaves its frame); mock handles need nothing.
        // PROVEN load-bearing: caller @0x267FD0 treats r0!=0 as error and
        // throws "Java method threw an exception". Zero it like Dalvik does.
#ifdef ABR_DIAG_COUNTERS
        if (fn == 22) {
            diag_.jni_delete_global_refs.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (a1 && obj_rev_.count(a1) && (obj_permanent_.count(a1) != 0) == (fn != 23)) {
                if (fn == 23) {
                    DropLocalLocked(a1);
                } else {
                    ReleaseHandleLocked(a1);
                }
            }
        }
        Ret(regs, 0);
        return;
    case 25:   // NewLocalRef(env, ref) -> a new local reference
    case 21:   // NewGlobalRef(env, obj) -> a new global reference
    case 226:  // NewWeakGlobalRef(env, obj) -> (held strongly)
        // A real object gets a new handle of that kind; a mock handle is its
        // own reference, as before.
#ifdef ABR_DIAG_COUNTERS
        if (fn == 21) {
            diag_.jni_new_global_refs.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        if (void* real = UnwrapObject(a1)) {
            Ret(regs, WrapObject(real, fn != 25));
            return;
        }
        Ret(regs, a1);
        return;
    case 32: {  // IsInstanceOf(env, obj, clazz) -> jboolean (NULL: true)
        if (a1 == 0) {
            Ret(regs, 1);
            return;
        }
        std::uint32_t r = 0;
#ifdef __ANDROID__
        if (JNIEnv* re = RealEnv()) {
            const std::string cname = JniClassName(a2);
            void* obj = JavaObjectFor(a1);
            jclass rc = cname.empty() ? nullptr : static_cast<jclass>(RealClass(re, cname));
            if (obj && rc) {
                r = re->IsInstanceOf(static_cast<jobject>(obj), rc) ? 1u : 0u;
            }
            re->ExceptionClear();
        }
#endif
        Ret(regs, r);
        return;
    }
    case 230:    // GetDirectBufferAddress(env, buf) -> guest shadow address
    case 231: {  // GetDirectBufferCapacity(env, buf) -> jlong (r0:r1)
        std::uint32_t addr = 0;
        std::int64_t cap = -1;
#ifdef __ANDROID__
        if (JNIEnv* re = RealEnv()) {
            jobject b = static_cast<jobject>(UnwrapObject(a1));
            void* host = b ? re->GetDirectBufferAddress(b) : nullptr;
            cap = b ? re->GetDirectBufferCapacity(b) : -1;
            if (fn == 230 && host && cap > 0 && cap < 0x10000000) {
                DirectBuf db;
                {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    db = direct_bufs_[host];
                }
                if (db.size != static_cast<std::uint32_t>(cap)) {
                    if (db.guest) {
                        GuestFree(db.guest);
                    }
                    db.guest = GuestMalloc(static_cast<std::uint32_t>(cap));
                    db.size = db.guest ? static_cast<std::uint32_t>(cap) : 0;
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    direct_bufs_[host] = db;
                }
                if (db.guest) {
                    std::memcpy(H(db.guest), host, db.size);
                    t_direct_used.push_back(host);
                    addr = db.guest;
                }
            }
        }
#endif
        if (fn == 230) {
            Ret(regs, addr);
        } else {
            Ret(regs, static_cast<std::uint32_t>(cap), static_cast<std::uint32_t>(static_cast<std::uint64_t>(cap) >> 32));
        }
        return;
    }
    case 223: {  // ReleasePrimitiveArrayCritical(env, array, carray, mode)
#ifdef __ANDROID__
        ArrayPin pin;
        bool have = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = array_pins_.find(a2);
            if (it != array_pins_.end() && it->second.type == -1) {
                pin = it->second;
                if (a3 != 1) {
                    array_pins_.erase(it);
                }
                have = true;
            }
        }
        JNIEnv* re = RealEnv();
        if (have && re) {
            if ((a3 == 0 || a3 == 1) && CheckRange(pin.guest_buf, pin.len, false)) {
                void* hp = re->GetPrimitiveArrayCritical(static_cast<jarray>(pin.real_arr), nullptr);
                if (hp) {
                    std::memcpy(hp, H(pin.guest_buf), pin.len);
                    re->ReleasePrimitiveArrayCritical(static_cast<jarray>(pin.real_arr), hp, 0);
                }
            }
            if (a3 != 1) {
                GuestFree(pin.guest_buf);
            }
            re->ExceptionClear();
        }
#endif
        Ret(regs, 0);
        return;
    }
    case 96: case 97: case 98: case 99: case 100: case 101: case 102: case 103:
    case 104: case 105: case 106: case 107: case 108: case 109: case 110: case 111: case 112:
    case 152: case 153: case 154: case 155: case 156: case 157: case 158: case 159:
    case 160: case 161: case 162: {
        // Get<T>Field 96-103 / Set<T>Field 104-112 (obj, field[, value]) and
        // GetStatic{Float,Double}Field 152-153 / SetStatic<T>Field 154-162
        // (class, field[, value]) on the real object or class. Values per
        // AAPCS32 softfp: 32-bit in r2 (a float as its bits), 64-bit in r2:r3,
        // an object as a guest handle; results the same way in r0(:r1).
        const bool stat = fn >= 152;
        const bool set = (fn >= 104 && fn <= 112) || fn >= 154;
        // Type letter, JNI order: object, boolean, byte, char, short, int,
        // long, float, double.
        static const char kT[] = "LZBCSIJFD";
        const char t = set ? kT[stat ? fn - 154 : fn - 104] : (stat ? kT[fn - 152 + 7] : kT[fn - 96 + 1]);
#ifdef __ANDROID__
        JNIEnv* re = RealEnv();
        jfieldID fid = re ? static_cast<jfieldID>(RealFieldId(a2, stat)) : nullptr;
        jobject obj = nullptr;
        jclass cls = nullptr;
        if (stat) {
            const std::string cname = JniClassName(a1);
            cls = cname.empty() ? nullptr : static_cast<jclass>(RealClass(re, cname));
        } else {
            obj = static_cast<jobject>(JavaObjectFor(a1));
        }
        if (fid && (stat ? cls != nullptr : obj != nullptr)) {
            std::uint64_t out = 0;
            if (!set) {
                switch (t) {
                    case 'Z': out = stat ? re->GetStaticBooleanField(cls, fid) : re->GetBooleanField(obj, fid); break;
                    case 'B': out = static_cast<std::uint32_t>(static_cast<std::int32_t>(stat ? re->GetStaticByteField(cls, fid) : re->GetByteField(obj, fid))); break;
                    case 'C': out = stat ? re->GetStaticCharField(cls, fid) : re->GetCharField(obj, fid); break;
                    case 'S': out = static_cast<std::uint32_t>(static_cast<std::int32_t>(stat ? re->GetStaticShortField(cls, fid) : re->GetShortField(obj, fid))); break;
                    case 'I': out = static_cast<std::uint32_t>(stat ? re->GetStaticIntField(cls, fid) : re->GetIntField(obj, fid)); break;
                    case 'J': out = static_cast<std::uint64_t>(stat ? re->GetStaticLongField(cls, fid) : re->GetLongField(obj, fid)); break;
                    case 'F': {
                        const float f = stat ? re->GetStaticFloatField(cls, fid) : re->GetFloatField(obj, fid);
                        std::uint32_t w = 0;
                        std::memcpy(&w, &f, 4);
                        out = w;
                        break;
                    }
                    case 'D': {
                        const double d = stat ? re->GetStaticDoubleField(cls, fid) : re->GetDoubleField(obj, fid);
                        std::memcpy(&out, &d, 8);
                        break;
                    }
                    default: break;
                }
                re->ExceptionClear();
                Ret(regs, static_cast<std::uint32_t>(out), static_cast<std::uint32_t>(out >> 32));
                return;
            }
            // Set (env, target, field, value): a 32-bit value is r3; a 64-bit
            // one takes the next even pair, which is past r3: [sp], 8-aligned.
            std::uint64_t v = a3;
            if (t == 'J' || t == 'D') {
                if (!CheckRange(regs[13], 8, false)) {
                    Ret(regs, 0);
                    return;
                }
                std::memcpy(&v, H(regs[13]), 8);
            }
            const std::uint32_t lo = static_cast<std::uint32_t>(v);
            switch (t) {
                case 'L': {
                    jobject o = static_cast<jobject>(JavaObjectFor(lo));
                    stat ? re->SetStaticObjectField(cls, fid, o) : re->SetObjectField(obj, fid, o);
                    break;
                }
                case 'Z': stat ? re->SetStaticBooleanField(cls, fid, static_cast<jboolean>(lo & 0xFF)) : re->SetBooleanField(obj, fid, static_cast<jboolean>(lo & 0xFF)); break;
                case 'B': stat ? re->SetStaticByteField(cls, fid, static_cast<jbyte>(lo)) : re->SetByteField(obj, fid, static_cast<jbyte>(lo)); break;
                case 'C': stat ? re->SetStaticCharField(cls, fid, static_cast<jchar>(lo)) : re->SetCharField(obj, fid, static_cast<jchar>(lo)); break;
                case 'S': stat ? re->SetStaticShortField(cls, fid, static_cast<jshort>(lo)) : re->SetShortField(obj, fid, static_cast<jshort>(lo)); break;
                case 'I': stat ? re->SetStaticIntField(cls, fid, static_cast<jint>(lo)) : re->SetIntField(obj, fid, static_cast<jint>(lo)); break;
                case 'J': stat ? re->SetStaticLongField(cls, fid, static_cast<jlong>(v)) : re->SetLongField(obj, fid, static_cast<jlong>(v)); break;
                case 'F': {
                    float f = 0;
                    std::memcpy(&f, &lo, 4);
                    stat ? re->SetStaticFloatField(cls, fid, f) : re->SetFloatField(obj, fid, f);
                    break;
                }
                case 'D': {
                    double d = 0;
                    std::memcpy(&d, &v, 8);
                    stat ? re->SetStaticDoubleField(cls, fid, d) : re->SetDoubleField(obj, fid, d);
                    break;
                }
                default: break;
            }
            re->ExceptionClear();
            Ret(regs, 0);
            return;
        }
        {
            std::string what = "?", field = "?";
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (const auto it = jni_objects_.find(a1); it != jni_objects_.end()) {
                what = it->second;
            } else if (const auto jc = jni_classes_.find(a1); jc != jni_classes_.end()) {
                what = "class " + jc->second;
            }
            if (const auto jf = jni_methods_.find(a2); jf != jni_methods_.end()) {
                field = jf->second;
            }
            std::printf("[jni] LOUD field fn#%u: no real field %08x (%s) on %08x (%s) -- halting\n", fn,
                        a2, field.c_str(), a1, what.c_str());
        }
        HaltGuest();
        return;
#endif
        (void)t;
        Ret(regs, 0);
        return;
    }
    case 165:    // GetStringChars(env, str, isCopy) -> const jchar*
    case 224: {  // GetStringCritical(env, str, isCopy)
        // A guest copy of the UTF-16 text (NUL-terminated for convenience),
        // freed by ReleaseStringChars/ReleaseStringCritical. Guest text is
        // decoded here, not by ART: the length GetStringLength gave must hold.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::u16string units;
        bool known = false;
        const auto it = jni_strings_.find(a1);
        if (it != jni_strings_.end()) {
            units = Utf16OfMutf8(it->second);
            known = true;
        } else {
#ifdef __ANDROID__
            if (jstring js = static_cast<jstring>(RealString(a1))) {
                JNIEnv* re = RealEnv();
                units.resize(static_cast<std::size_t>(re->GetStringLength(js)));
                re->GetStringRegion(js, 0, static_cast<jsize>(units.size()),
                                    reinterpret_cast<jchar*>(&units[0]));
                re->ExceptionClear();
                known = true;
            }
#endif
        }
        if (!known) {
            std::printf("[jni] LOUD GetStringChars unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        const std::uint32_t n = static_cast<std::uint32_t>(units.size());
        const std::uint32_t g = GuestMalloc(n * 2u + 2u);
        if (g) {
            std::memcpy(H(g), units.data(), std::size_t(n) * 2);
            std::memset(H(g + n * 2u), 0, 2);
            jni_char_copies_[g] = 1;
        }
        if (a2 && CheckRange(a2, 1, true)) {
            *H<std::uint8_t>(a2) = 1;
        }
        Ret(regs, g);
        return;
    }
    case 166:    // ReleaseStringChars(env, str, chars)
    case 225: {  // ReleaseStringCritical(env, str, carray)
        bool ours = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            ours = jni_char_copies_.erase(a2) != 0;
        }
        if (ours) {
            GuestFree(a2);
        }
        Ret(regs, 0);
        return;
    }
#ifdef __ANDROID__
#ifdef __ANDROID__
    case 163: {  // NewString(env, const jchar* unicode, jsize len) -> jstring
        JNIEnv* re = RealEnv();
        if (!re || (a2 && !CheckRange(a1, a2 * 2u, false))) {
            Ret(regs, 0);
            return;
        }
        jstring js = re->NewString(a2 ? H<const jchar>(a1) : nullptr, static_cast<jsize>(a2));
        const std::uint32_t h = js ? WrapObject(js) : 0;
        if (js) {
            re->DeleteLocalRef(js);
        }
        re->ExceptionClear();
        Ret(regs, h);
        return;
    }
#endif
    case 172: {  // NewObjectArray(env, len, elementClass, initial) -> jobjectArray
        JNIEnv* re = RealEnv();
        const std::string cname = JniClassName(a2);
        jclass rc = (re && !cname.empty()) ? static_cast<jclass>(RealClass(re, cname)) : nullptr;
        jobjectArray arr = rc ? re->NewObjectArray(static_cast<jsize>(a1), rc,
                                                   static_cast<jobject>(JavaObjectFor(a3)))
                              : nullptr;
        const std::uint32_t h = arr ? WrapObject(arr) : 0;
        if (arr) {
            re->DeleteLocalRef(arr);
        }
        if (re) {
            re->ExceptionClear();
        }
        Ret(regs, h);
        return;
    }
    case 173:    // GetObjectArrayElement(env, array, index) -> jobject
    case 174: {  // SetObjectArrayElement(env, array, index, value)
        JNIEnv* re = RealEnv();
        jobjectArray arr = static_cast<jobjectArray>(UnwrapObject(a1));
        std::uint32_t h = 0;
        if (re && arr) {
            if (fn == 173) {
                jobject o = re->GetObjectArrayElement(arr, static_cast<jsize>(a2));
                h = o ? WrapObject(o) : 0;
                if (o) {
                    re->DeleteLocalRef(o);
                }
            } else {
                re->SetObjectArrayElement(arr, static_cast<jsize>(a2),
                                          static_cast<jobject>(JavaObjectFor(a3)));
            }
            re->ExceptionClear();
        }
        Ret(regs, h);
        return;
    }
    case 175: case 176: case 177: case 178: case 179: case 180: case 181: case 182: {
        // New<T>Array(env, len): a real Java array.
        JNIEnv* re = RealEnv();
        jarray arr = nullptr;
        const jsize n = static_cast<jsize>(a1);
        if (re) {
            switch (fn) {
                case 175: arr = re->NewBooleanArray(n); break;
                case 176: arr = re->NewByteArray(n); break;
                case 177: arr = re->NewCharArray(n); break;
                case 178: arr = re->NewShortArray(n); break;
                case 179: arr = re->NewIntArray(n); break;
                case 180: arr = re->NewLongArray(n); break;
                case 181: arr = re->NewFloatArray(n); break;
                default: arr = re->NewDoubleArray(n); break;
            }
            re->ExceptionClear();
        }
        const std::uint32_t h = arr ? WrapObject(arr) : 0;
        if (arr) {
            re->DeleteLocalRef(arr);
        }
        Ret(regs, h);
        return;
    }
    case 217:    // MonitorEnter(env, obj) -> jint
    case 218: {  // MonitorExit(env, obj) -> jint
        JNIEnv* re = RealEnv();
        jobject o = static_cast<jobject>(StringMonitorObject(a1));
        if (o) {
            NoteStringAsObject(4, "monitor");
        } else {
            o = static_cast<jobject>(JavaObjectFor(a1));
        }
        jint r = -1;
        if (re && o) {
            r = fn == 217 ? re->MonitorEnter(o) : re->MonitorExit(o);
            re->ExceptionClear();
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case 27: {  // AllocObject(env, clazz) -> jobject
        JNIEnv* re = RealEnv();
        const std::string cname = JniClassName(a1);
        jclass rc = (re && !cname.empty()) ? static_cast<jclass>(RealClass(re, cname)) : nullptr;
        jobject o = rc ? re->AllocObject(rc) : nullptr;
        const std::uint32_t h = o ? WrapObject(o) : 0;
        if (o) {
            re->DeleteLocalRef(o);
        }
        if (re) {
            re->ExceptionClear();
        }
        Ret(regs, h);
        return;
    }
    case 229: {  // NewDirectByteBuffer(env, void* address, jlong capacity in r2:r3)
        // Guest memory is host memory that stays put, so the buffer can view
        // it directly.
        JNIEnv* re = RealEnv();
        const std::int64_t cap = static_cast<std::int64_t>(std::uint64_t(a2) | (std::uint64_t(a3) << 32));
        jobject b = (re && cap > 0 && cap < 0x40000000 && CheckRange(a1, static_cast<std::uint32_t>(cap), true))
                        ? re->NewDirectByteBuffer(H(a1), cap)
                        : nullptr;
        const std::uint32_t h = b ? WrapObject(b) : 0;
        if (b) {
            re->DeleteLocalRef(b);
        }
        if (re) {
            re->ExceptionClear();
        }
        Ret(regs, h);
        return;
    }
#endif
    case 4:  // GetVersion(env) -> JNI 1.6
        Ret(regs, 0x00010006u);
        return;
    case 5:  // DefineClass(env, name, loader, buf, len): not offered (NULL)
        std::printf("[jni] DefineClass is not offered -> NULL\n");
        Ret(regs, 0);
        return;
    case 7:  // FromReflectedMethod(env, method) -> jmethodID
    case 8:  // FromReflectedField(env, field) -> jfieldID
        Ret(regs, CookieForReflected(a1, fn == 8));
        return;
#ifdef __ANDROID__
    case 9:     // ToReflectedMethod(env, cls, methodID, isStatic) -> jobject
    case 12: {  // ToReflectedField(env, cls, fieldID, isStatic) -> jobject
        JNIEnv* re = RealEnv();
        const std::string cname = JniClassName(a1);
        jclass rc = (re && !cname.empty()) ? static_cast<jclass>(RealClass(re, cname)) : nullptr;
        jobject o = nullptr;
        if (rc && fn == 12) {
            if (jfieldID f = static_cast<jfieldID>(RealFieldId(a2, a3 != 0))) {
                o = re->ToReflectedField(rc, f, static_cast<jboolean>(a3 != 0));
            }
        } else if (rc) {
            std::string d;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                const auto it = jni_methods_.find(a2);
                d = it == jni_methods_.end() ? "" : it->second;
            }
            if (d.compare(0, 7, "static ") == 0) {
                d = d.substr(7);
            }
            const std::size_t dot = d.rfind('.', d.find('('));
            const std::size_t par = d.find('(');
            if (dot != std::string::npos && par != std::string::npos) {
                const std::string name = d.substr(dot + 1, par - dot - 1), sig = d.substr(par);
                jmethodID id = a3 ? re->GetStaticMethodID(rc, name.c_str(), sig.c_str())
                                  : re->GetMethodID(rc, name.c_str(), sig.c_str());
                if (id) {
                    o = re->ToReflectedMethod(rc, id, static_cast<jboolean>(a3 != 0));
                }
            }
        }
        if (re) {
            re->ExceptionClear();
        }
        const std::uint32_t h = o ? WrapObject(o) : 0;
        if (o) {
            re->DeleteLocalRef(o);
        }
        Ret(regs, h);
        return;
    }
    case 10: {  // GetSuperclass(env, cls) -> jclass
        JNIEnv* re = RealEnv();
        const std::string cname = JniClassName(a1);
        jclass rc = (re && !cname.empty()) ? static_cast<jclass>(RealClass(re, cname)) : nullptr;
        jclass sc = rc ? re->GetSuperclass(rc) : nullptr;
        const std::uint32_t h = sc ? WrapObject(sc) : 0;
        if (sc) {
            re->DeleteLocalRef(sc);
        }
        Ret(regs, h);
        return;
    }
    case 11: {  // IsAssignableFrom(env, c1, c2) -> jboolean
        JNIEnv* re = RealEnv();
        const std::string n1 = JniClassName(a1), n2 = JniClassName(a2);
        jclass c1 = (re && !n1.empty()) ? static_cast<jclass>(RealClass(re, n1)) : nullptr;
        jclass c2 = (re && !n2.empty()) ? static_cast<jclass>(RealClass(re, n2)) : nullptr;
        Ret(regs, (c1 && c2 && re->IsAssignableFrom(c1, c2)) ? 1u : 0u);
        return;
    }
    case 13:    // Throw(env, throwable) -> jint
    case 14: {  // ThrowNew(env, cls, msg) -> jint
        // The exception is raised in the real JVM and stays pending: when the
        // guest native returns, Java sees it, as it would on a device.
        JNIEnv* re = RealEnv();
        jint r = -1;
        if (re && fn == 13) {
            if (jthrowable t = static_cast<jthrowable>(JavaObjectFor(a1))) {
                r = re->Throw(t);
            }
        } else if (re) {
            const std::string cname = JniClassName(a1);
            jclass rc = cname.empty() ? nullptr : static_cast<jclass>(RealClass(re, cname));
            if (rc) {
                r = re->ThrowNew(rc, a2 && CheckRange(a2, 1, false) ? GuestStr(a2) : nullptr);
            }
        }
        std::printf("[jni] %s -> %d\n", fn == 13 ? "Throw" : "ThrowNew", r);
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
#endif
    case 232:  // GetObjectRefType(env, obj): every guest-held object is a global ref
        Ret(regs, a1 ? 2u : 0u);  // JNIGlobalRefType
        return;
    case 16:  // ExceptionDescribe(env): the bridge clears exceptions as they come
        Ret(regs, 0);
        return;
    case 18: {  // FatalError(env, msg): the JVM aborts the process
        std::printf("[jni] FATAL FatalError(\"%s\") -- halting\n",
                    CheckRange(a1, 1, false) ? GuestStr(a1) : "?");
        HaltGuest();
        return;
    }
    case 19:  // PushLocalFrame(env, capacity) -> 0
        PushLocalFrame();
        t_guest_frames++;
        Ret(regs, 0);
        return;
    case 26:  // EnsureLocalCapacity(env, capacity) -> 0: no limit here
        Ret(regs, 0);
        return;
    case 20: {  // PopLocalFrame(env, result) -> result as a reference of the outer frame
        if (t_guest_frames <= 0) {
            Ret(regs, a1);  // nothing the guest pushed: the host's frames stay
            return;
        }
        t_guest_frames--;
        void* keep = nullptr;
#ifdef __ANDROID__
        JNIEnv* re = RealEnv();
        if (void* real = UnwrapObject(a1); real && re) {
            keep = re->NewGlobalRef(static_cast<jobject>(real));
        }
#endif
        PopLocalFrame();
        std::uint32_t out = keep ? 0u : a1;
#ifdef __ANDROID__
        if (keep) {
            out = WrapObject(keep);
            re->DeleteGlobalRef(static_cast<jobject>(keep));
        }
#endif
        Ret(regs, out);
        return;
    }
    case 216: {  // UnregisterNatives(env, clazz) -> jint
        std::string cname;
        cname = JniClassName(a1);
        std::int32_t r = 0;
#ifdef __ANDROID__
        if (JNIEnv* re = RealEnv(); re && !cname.empty()) {
            if (jclass rc = static_cast<jclass>(RealClass(re, cname))) {
                r = re->UnregisterNatives(rc);
                re->ExceptionClear();
            }
        }
#endif
        std::printf("[jni] UnregisterNatives %s -> %d\n", cname.empty() ? "?" : cname.c_str(), r);
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    case 17:  // ExceptionClear(env) -> void (mock never raises)
        Ret(regs, 0);
        return;
    case 15:  // ExceptionOccurred(env) -> NULL (mock never raises)
        Ret(regs, 0);
        return;
    case 228:  // ExceptionCheck(env) -> jboolean FALSE (mock never raises)
        Ret(regs, 0);
        return;
    case 219: {  // GetJavaVM(env, JavaVM** out) -> 0, the runtime's JavaVM
        if (!CheckRange(a1, 4, true)) {
            std::printf("[jni] LOUD GetJavaVM: bad out pointer %08x -- halting\n", a1);
            HaltGuest();
            return;
        }
        const std::uint32_t vm = Jvm();
        std::memcpy(H(a1), &vm, 4);
        Ret(regs, 0);
        return;
    }
    case 28:  // NewObject(V/A)(env, class, method, ...) -> fresh mock object
    case 29:   // (verified vs NDK jni.h; ctor args ignored, offline fine)
    case 30: {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
#ifdef __ANDROID__
        // Product host first: real ART object (audio + future stateful
        // objects need real fields/arrays; mocks below = fallback).
        if (RealNewObject(fn, a1, a2, a3, regs, mdesc)) {
            return;
        }
#endif
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t o = 0;
        if (!JniAllocObjectHandle(&o)) {
            return;
        }
        std::memcpy(H(o), &o, 4);
        jni_objects_[o] = "newobj<" + mdesc + ">";
        // Track obj -> class handle for GetObjectClass (fn#31): parse the
        // class from the ctor desc ("pkg/Class.<init>..."), find-or-create.
        {
            auto dot = mdesc.find(".<init>");
            std::string cname = (dot == std::string::npos) ? "java/lang/Object"
                                                           : mdesc.substr(0, dot);
            std::uint32_t ch = 0;
            for (const auto& kv : jni_classes_) {
                if (kv.second == cname) {
                    ch = kv.first;
                    break;
                }
            }
            if (!ch) {
                if (!JniAllocObjectHandle(&ch)) {
                    return;
                }
                jni_classes_[ch] = cname;
            }
            jni_objclass_[o] = ch;
        }
        const char* vtag = (fn == 28) ? "" : ((fn == 29) ? "V" : "A");
        if (DiagOn()) {
            std::printf("[jni] NewObject%s %s -> %08x\n", vtag, mdesc.c_str(), o);
        }
        // The native audio handle (NewObjectV varargs live at the va_list
        // pointer a3, not at sp).
        if (DiagOn() && mdesc.find("AudioOutput.<init>") != std::string::npos) {
            std::uint32_t w0 = 0, w1 = 0;
            if (CheckRange(a3, 4, false)) {
                std::memcpy(&w0, H(a3), 4);
            }
            if (CheckRange(a3 + 4, 4, false)) {
                std::memcpy(&w1, H(a3 + 4), 4);
            }
            std::printf("[audio] AudioOutput ctor J: lo=%08x hi=%08x\n", w0, w1);
        }
        Ret(regs, o);
        return;
    }
    case 37:  // CallBooleanMethod(V/A)(env, obj, method, ...) -> jboolean
    case 38:
    case 39: {
        const char* vtag = (fn == 37) ? "" : ((fn == 38) ? "V" : "A");
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        // AudioOutput.startOutput()Z: our mixer is live (mixdata proven) —
        // report started, as on device. Everything else defaults to false
        // (logged-out services etc.).
#ifdef __ANDROID__
        // Product host: real startOutput on real objects (starts real
        // audio -> real callbacks -> mixdata). No-arg boolean only; other
        // shapes stay mocked below.
        if (mdesc.find("startOutput()Z") != std::string::npos) {
            JNIEnv* re = GuestCallbacks::g_real_env_hook
                             ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                             : nullptr;
            void* robj = UnwrapObject(a1);
            if (re && robj) {
                std::string d = mdesc;
                const std::size_t dot = d.rfind('.');
                const std::size_t par = d.find('(');
                if (dot != std::string::npos && par != std::string::npos && dot < par) {
                    const std::string cls = d.substr(0, dot);
                    jclass rclazz = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> lock(mutex_);
                        const auto it = real_classes_.find(cls);
                        if (it != real_classes_.end()) {
                            rclazz = static_cast<jclass>(it->second);
                        }
                    }
                    if (!rclazz) {
                        rclazz = static_cast<jclass>(RealClass(re, cls));  // + the app loader fallback
                    }
                    if (!rclazz) {
                        jclass lc = re->FindClass(cls.c_str());
                        if (lc && !re->ExceptionCheck()) {
                            rclazz = static_cast<jclass>(re->NewGlobalRef(lc));
                            re->DeleteLocalRef(lc);
                            if (rclazz) {
                                std::lock_guard<std::recursive_mutex> lock(mutex_);
                                real_classes_[cls] = rclazz;
                            }
                        } else {
                            re->ExceptionClear();
                        }
                    }
                    jmethodID rmid = nullptr;
                    if (rclazz) {
                        rmid = re->GetMethodID(rclazz, "startOutput", "()Z");
                        if (!rmid || re->ExceptionCheck()) {
                            re->ExceptionClear();
                            rmid = nullptr;
                        }
                    }
                    if (rmid) {
                        jboolean rr = re->CallBooleanMethodA(static_cast<jobject>(robj),
                                                             rmid, nullptr);
                        if (!re->ExceptionCheck()) {
                            std::printf("[jni] CallBooleanMethod startOutput -> %u (real)\n",
                                        (unsigned)rr);
                            Ret(regs, rr ? 1u : 0u);
                            return;
                        }
                        re->ExceptionClear();
                    }
                }
            }
        }
#endif
        const std::uint32_t rv =
            (mdesc.find("startOutput") != std::string::npos) ? 1 : 0;
        if (DiagOn()) {
            std::printf("[jni] CallBooleanMethod%s %s -> %u\n", vtag, mdesc.c_str(), rv);
        }
        Ret(regs, rv);
        return;
    }
    case 61:  // CallVoidMethod(V/A)(env, obj, method, ...) -> void
    case 62:   // (verified vs NDK jni.h)
    case 63: {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        const char* vtag = (fn == 61) ? "" : ((fn == 62) ? "V" : "A");
        if (DiagOn()) {
            std::printf("[jni] CallVoidMethod%s %s\n", vtag, mdesc.c_str());
        }
#ifdef __ANDROID__
        // Product host: the guest's audio engine calls AudioOutput.stopOutput()
        // on pause and AudioOutput.requestExclusiveAudio(Z) on focus changes.
        // Java never calls stopOutput (no caller in the dex); the only caller
        // is the native side, so the generic void mock below would drop it and
        // leave the AudioTrack playing after the app is backgrounded. Forward
        // exactly these two to the real object; every other void call stays
        // mocked (services/analytics are intentionally inert).
        if (mdesc.find("AudioOutput.stopOutput") != std::string::npos ||
            mdesc.find("AudioOutput.requestExclusiveAudio") != std::string::npos) {
            if (RealCallObject(fn, a0, a1, a2, a3, regs, mdesc)) {
                return;
            }
            // A real AudioOutput that cannot take its own method is an anomaly,
            // not a mock path. The old silent drop WAS the background-audio bug.
            std::printf("[jni] LOUD %s real call failed -- audio state not applied\n",
                        mdesc.c_str());
        }
#endif
        Ret(regs, 0);  // void JNI: zero r0
        return;
    }
    case 114:  // CallStaticObjectMethod(V/A)(env, class, method, ...) -> mock obj
    case 115:
    case 116:
    case 34:  // CallObjectMethod(V/A)(env, obj, method, ...) -> same shape
    case 35:
    case 36: {
        const bool is_static = fn >= 114;
        const char* vtag = (fn == 114 || fn == 34) ? "" : ((fn == 115 || fn == 35) ? "V" : "A");
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        // Methods returning String produce registered jstrings with mock
        // content (the game immediately reads them via GetStringRegion etc.).
        // Known: getPathToFileCacheDirectory -> files-dir path (real-world
        // value; host file bridge lands later). Unknown String method: "".
#ifdef __ANDROID__
        // Product host first: real ART call (tuned mocks below = fallback).
        if (RealCallObject(fn, a0, a1, a2, a3, regs, mdesc)) {
            return;
        }
        {
            // A mock answer where the real call was meant to run: the guest
            // gets "" or an inert object, so name the method.
            static std::atomic<int> mock_logs{0};
            if (mock_logs.fetch_add(1) < 40) {
                std::printf("[jni] Call%sObject %s: no real call, a mock result\n",
                            is_static ? "Static" : "", mdesc.c_str());
            }
        }
#endif
        if (mdesc.size() >= 20 &&
            mdesc.compare(mdesc.size() - 20, 20, "()Ljava/lang/String;") == 0) {
            std::string content;
            if (mdesc.find("getPathToFileCacheDirectory") != std::string::npos) {
                content = GuestFilesPrefix();
            }
            // HARNESS_DEVID=<hex16> overrides the mock device ID (default: a
            // fixed plausible ANDROID_ID; empty broke key-derivation paths).
            if (mdesc.find("getUniqueId") != std::string::npos) {
                content = "0123456789abcdef";
                if (const char* d = std::getenv("HARNESS_DEVID")) {
                    content = d;
                }
            }
            // HARNESS_LOCALE=<tag> overrides the mock locale (default en_US;
            // empty broke locale-dependent paths). Real tag: e.g. en_US.
            if (mdesc.find("systemLocale") != std::string::npos) {
                content = "en_US";
                if (const char* d = std::getenv("HARNESS_LOCALE")) {
                    content = d;
                }
            }
            // rcs.core.Utils.networkType() of an offline title (an online
            // one asks the real method): "" is what the real method returns
            // without network (getActiveNetworkInfo() == null -> exception
            // -> ""), next to "wifi" and "cellular". HARNESS_NETTYPE overrides.
            if (mdesc.find("networkType") != std::string::npos) {
                content = "";
                if (const char* d = std::getenv("HARNESS_NETTYPE")) {
                    content = d;
                }
            }
            // UUID.toString() must be a valid UUID (empty string stalled the
            // installation-ID flow into a forever poll-wait; device always
            // returns a real UUID here).
            if (content.empty() && mdesc.find("toString") != std::string::npos && !is_static) {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                const auto ito = jni_objects_.find(a1);
                if (ito != jni_objects_.end() && ito->second.find("UUID") != std::string::npos) {
                    content = "01234567-89ab-cdef-0123-456789abcdef";
                }
            }
            const std::uint32_t js = JniNewString(content);
            if (DiagOn()) {
                std::printf("[jni] Call%sObject%s %s -> str %08x \"%s\"\n", is_static ? "Static" : "",
                            vtag, mdesc.c_str(), js, content.c_str());
            }
            Ret(regs, js);
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t o = 0;
        if (!JniAllocObjectHandle(&o)) {
            return;
        }
        std::memcpy(H(o), &o, 4);
        jni_objects_[o] = "obj<" + mdesc + ">";
        if (DiagOn()) {
            std::printf("[jni] Call%sObject%s %s -> %08x\n", is_static ? "Static" : "", vtag,
                        mdesc.c_str(), o);
        }
        Ret(regs, o);
        return;
    }
    case 145: {  // GetStaticObjectField(env, class, field) -> jobject
        std::string fdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                fdesc = it->second;
            }
        }
#ifdef __ANDROID__
        // The real object first (Unity reads fields of what it gets here).
        if (JNIEnv* re = RealEnv()) {
            const std::string cname = JniClassName(a1);
            jclass rc = cname.empty() ? nullptr : static_cast<jclass>(RealClass(re, cname));
            jfieldID fid = rc ? static_cast<jfieldID>(RealFieldId(a2, true)) : nullptr;
            jobject o = fid ? re->GetStaticObjectField(rc, fid) : nullptr;
            re->ExceptionClear();
            if (o) {
                const std::uint32_t h = WrapObject(o);
                re->DeleteLocalRef(o);
                if (DiagOn()) {
                    std::printf("[jni] GetStaticObjectField %s -> %08x (real)\n", fdesc.c_str(), h);
                }
                Ret(regs, h);
                return;
            }
        }
#endif
        // No real value: the game does NOT null-check (Build.MANUFACTURER
        // NULL -> instant GetStringLength(NULL) death), so never return NULL.
        // String-typed -> registered empty jstring; else fresh mock object.
        if (fdesc.size() >= 18 &&
            fdesc.compare(fdesc.size() - 18, 18, "Ljava/lang/String;") == 0) {
            const std::uint32_t js = JniNewString("");
            if (DiagOn()) {
                std::printf("[jni] GetStaticObjectField %s -> str %08x\n", fdesc.c_str(), js);
            }
            Ret(regs, js);
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t o = 0;
        if (!JniAllocObjectHandle(&o)) {
            return;
        }
        std::memcpy(H(o), &o, 4);
        jni_objects_[o] = "field<" + fdesc + ">";
        if (DiagOn()) {
            std::printf("[jni] GetStaticObjectField %s -> %08x\n", fdesc.c_str(), o);
        }
        Ret(regs, o);
        return;
    }
    case 146:  // GetStaticBooleanField -> jboolean (0/1 in r0)
    case 147:  // GetStaticByteField / 148 Char / 149 Short / 150 Int / 151 Long
    case 148:  // (verified vs NDK jni.h; readers for GetStaticFieldID cookies)
    case 149:
    case 150:
    case 151: {
        std::string fdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                fdesc = it->second;
            }
        }
#ifdef __ANDROID__
        // The real value first (constants such as ActivityInfo.
        // SCREEN_ORIENTATION_PORTRAIT = 1: answering 0 turned a request for
        // portrait into landscape).
        if (JNIEnv* re = RealEnv()) {
            const std::string cname = JniClassName(a1);
            jclass rc = cname.empty() ? nullptr : static_cast<jclass>(RealClass(re, cname));
            jfieldID fid = rc ? static_cast<jfieldID>(RealFieldId(a2, true)) : nullptr;
            if (fid) {
                std::uint64_t v = 0;
                switch (fn) {
                    case 146: v = re->GetStaticBooleanField(rc, fid); break;
                    case 147: v = static_cast<std::uint32_t>(static_cast<std::int32_t>(re->GetStaticByteField(rc, fid))); break;
                    case 148: v = re->GetStaticCharField(rc, fid); break;
                    case 149: v = static_cast<std::uint32_t>(static_cast<std::int32_t>(re->GetStaticShortField(rc, fid))); break;
                    case 150: v = static_cast<std::uint32_t>(re->GetStaticIntField(rc, fid)); break;
                    default: v = static_cast<std::uint64_t>(re->GetStaticLongField(rc, fid)); break;
                }
                re->ExceptionClear();
                if (DiagOn()) {
                    std::printf("[jni] GetStaticField#%u %s -> %llu (real)\n", fn, fdesc.c_str(),
                                static_cast<unsigned long long>(v));
                }
                Ret(regs, static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(v >> 32));
                return;
            }
        }
#endif
        // No real field: 0 (logged).
        if (DiagOn()) {
            std::printf("[jni] GetStaticField#%u %s -> 0\n", fn, fdesc.c_str());
        }
        if (fn == 145) {
            Ret(regs, 0);
        } else if (fn == 151) {
            Ret(regs, 0, 0);
        } else {
            Ret(regs, 0);
        }
        return;
    }
    case 171: {  // GetArrayLength(env, array) -> jsize
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_array_lens_.find(a1);
            if (it != jni_array_lens_.end()) {
                Ret(regs, it->second);
                return;
            }
        }
#ifdef __ANDROID__
        // A real Java array handed to the guest (wrapped handle): ask the JVM.
        if (void* real = UnwrapObject(a1)) {
            if (JNIEnv* re = RealEnv()) {
                const jsize n = re->GetArrayLength(static_cast<jarray>(real));
                if (re->ExceptionCheck()) {
                    re->ExceptionClear();
                }
                Ret(regs, static_cast<std::uint32_t>(n));
                return;
            }
        }
#endif
        std::printf("[jni] LOUD GetArrayLength unknown array %08x -- halting\n", a1);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    case 117:  // CallStaticBooleanMethod(V/A)(env, class, method, ...) -> jboolean
    case 118:   // (verified vs NDK jni.h; offline default false, logged)
    case 119: {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        const char* vtag = (fn == 117) ? "" : ((fn == 118) ? "V" : "A");
        // The feature string argument (jstring) of hasSystemFeature. V/A
        // pass args behind a pointer (va_list / jvalue*).
        std::string farg;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            std::uint32_t h = 0;
            if (fn == 118 || fn == 119) {
                if (CheckRange(a3, 4, false)) {
                    std::memcpy(&h, H(a3), 4);
                }
            } else {
                // Plain "..." form: env, clazz, method fill r0-r2, so the
                // first variadic argument is in r3 (AAPCS32), not at [sp].
                h = a3;
            }
            if (h) {
                JniStringText(h, farg);
            }
        }
        // hasSystemFeature policy (device-plausible): true for core mobile
        // features, false for exotic ones. Tunable per feature string.
        std::uint32_t v = 0;
        if (mdesc.find("hasSystemFeature") != std::string::npos) {
            if (farg.find("touchscreen") != std::string::npos ||
                farg.find("opengles") != std::string::npos ||
                farg.find("wifi") != std::string::npos ||
                farg.find("telephony") != std::string::npos ||
                farg.find("location") != std::string::npos) {
                v = 1;
            }
        }
        if (DiagOn()) {
            std::printf("[jni] CallStaticBooleanMethod%s %s(\"%s\") -> %u\n", vtag, mdesc.c_str(),
                        farg.c_str(), v);
        }
        Ret(regs, v);
        return;
    }
    case 129:  // CallStaticIntMethod(V/A)(env, class, method, ...) -> jint
    case 130:   // (verified vs NDK jni.h)
    case 131: {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        // Mock integer policy (tunable, logged): era-correct API level 21 for
        // getAPILevel (2014 game; avoids unreleased-API forks); display
        // 800x480 matching the harness nativeInit dims (0 broke layout and
        // possibly texture sizing downstream); 0 otherwise.
        std::uint32_t v = 0;
        if (mdesc.find("getAPILevel") != std::string::npos) {
            v = 21;
        } else if (mdesc.find("notifyAfter") != std::string::npos) {
            // PROVEN (notify7): pending-notifications value tracks this
            // return; 0 makes the game store bool-true, which its JSON
            // writer emits as raw 0x01 (ParseError). Real Java returns a
            // nonzero notification id -> decimal text -> parses. Use 1
            // (first id, device-plausible).
            v = 1;
        } else if (mdesc.find("getDisplayWidth") != std::string::npos) {
            v = 800;
        } else if (mdesc.find("getDisplayHeight") != std::string::npos) {
            v = 480;
        } else if (mdesc.find("getViewWidth") != std::string::npos) {
            // Utils.getViewWidth (0 broke downstream layout while
            // DeviceInfoWrapper reports 800; keep consistent).
            v = 800;
        } else if (mdesc.find("getViewHeight") != std::string::npos) {
            v = 480;
        } else if (mdesc.find("getPPI") != std::string::npos) {
            // 800x480 era phone (~4in, 233ppi); 240 keeps density math sane.
            v = 240;
        } else if (mdesc.find("getDisplayDensityGroup") != std::string::npos) {
            // Unknown bucket encoding; 1 = baseline bucket guess (tunable;
            // 0 selected no/fallback assets downstream).
            v = 1;
        } else if (mdesc.find("getDisplayConfigurationGroup") != std::string::npos) {
            v = 1;
        }
        const char* vtag = (fn == 129) ? "" : ((fn == 130) ? "V" : "A");
        if (DiagOn()) {
            std::printf("[jni] CallStaticIntMethod%s %s -> %u\n", vtag, mdesc.c_str(), v);
        }
        Ret(regs, v);
        return;
    }
    case 49:  // CallIntMethod(V/A)(env, obj, method, ...) -> jint
    case 50:
    case 51: {
        // The Int family. It was "never hit" until the Star Wars II (Amazon
        // build) called CallIntMethodV during its shutdown -- and the
        // unimplemented branch HALTED the guest, which took the app down
        // with it. Same shape as the Long family below: the mock env has no
        // way to run an arbitrary Java method, so the answer is the offline
        // default 0 (the same convention as the Boolean family, which
        // answers false, and the Long family, which answers 0).
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        const char* vtag = (fn == 49) ? "" : ((fn == 50) ? "V" : "A");
        if (DiagOn()) {
            std::printf("[jni] CallIntMethod%s %s -> 0\n", vtag, mdesc.c_str());
        }
        Ret(regs, 0, 0);  // jint 0
        return;
    }
    case 52:  // CallLongMethod(V/A)(env, obj, method, ...) -> jlong
    case 53:   // (verified vs NDK jni.h)
    case 54: {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        const char* vtag = (fn == 52) ? "" : ((fn == 53) ? "V" : "A");
        if (DiagOn()) {
            std::printf("[jni] CallLongMethod%s %s -> 0\n", vtag, mdesc.c_str());
        }
        Ret(regs, 0, 0);  // jlong 0
        return;
    }
    case 140:  // CallStaticDoubleMethodA(141/142 = CallStaticVoidMethod(V))
    case 141:   // (verified vs NDK jni.h; game calls 142 = FlurryAgent.init V)
    case 142: {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        // Offline default: 0 (0.0 for double-A, void for 141/142).
        // Tunable per method if a branch needs better.
        if (DiagOn()) {
            std::printf("[jni] CallStatic%s %s -> 0\n",
                        fn == 140 ? "DoubleMethodA" : "VoidMethod", mdesc.c_str());
        }
        Ret(regs, 0, 0);
        return;
    }
    case 184: {  // GetByteArrayElements(env, array, isCopy) -> jbyte*
#ifdef __ANDROID__
        // Real arrays (audio buffers): pin host elems, copy into a guest
        // buffer (guest only ever sees 32-bit), record the pin for
        // Release copy-back. Falls through to mocks for mock arrays.
        {
            void* rarr = UnwrapObject(a1);
            JNIEnv* re = GuestCallbacks::g_real_env_hook
                             ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                             : nullptr;
            if (re && rarr) {
                jsize len = re->GetArrayLength(static_cast<jarray>(rarr));
                if (!re->ExceptionCheck() && len > 0 && len < 10000000) {
                    // Pool first (exact len): kills the per-callback orphan
                    // leak without changing copy semantics (fresh bytes are
                    // always memcpied in below).
                    std::uint32_t gb = 0;
                    {
                        std::lock_guard<std::recursive_mutex> lock(heap_mu_);  // array_pool_
                        for (auto it = array_pool_.begin(); it != array_pool_.end(); ++it) {
                            if (it->second == (std::uint32_t)len) {
                                if (LiveIdxFind(it->first)) {
                                    gb = it->first;
                                    array_pool_.erase(it);
                                    break;
                                }
                                // Stale pool entry (its block was freed): counted,
                                // then skipped -- a fresh block is allocated below.
                                ++hx_pool_stale_;
                            }
                        }
                    }
                    if (!gb) {
                        gb = GuestMalloc((std::uint32_t)len);
                    }
                    jbyte* hp = gb ? re->GetByteArrayElements(static_cast<jbyteArray>(rarr),
                                                              nullptr)
                                   : nullptr;
                    if (gb && hp && !re->ExceptionCheck()) {
                        std::memcpy(H(gb), hp, (std::size_t)len);
                        {
                            std::lock_guard<std::recursive_mutex> lock(mutex_);
                            array_pins_[a1] = ArrayPin{rarr, hp, gb, (std::uint32_t)len};
                        }
                        if (a2 && CheckRange(a2, 1, true)) {  // jboolean* isCopy: 1 byte (jni.h)
                            *H<std::uint8_t>(a2) = 1;
                        }
                        if (DiagOn()) {
                            std::printf("[jni] GetByteArrayElements real len=%d -> %08x\n",
                                        (int)len, gb);
                        }
                        Ret(regs, gb);
                        return;
                    }
                    if (hp) {
                        re->ReleaseByteArrayElements(static_cast<jbyteArray>(rarr), hp,
                                                     JNI_ABORT);
                    }
                    re->ExceptionClear();
                } else {
                    static int glr = 0;
                    if (glr < 10) {
                        glr++;
                        std::printf("[jni] GetElem len/exc refused a1=%08x exc=%d len=%d\n", a1,
                                    (int)re->ExceptionCheck(), (int)len);
                    }
                    re->ExceptionClear();
                }
            } else {
                // Real-path refusal reason (was silent fallthrough -> mock
                // miss -> LOUD halt with no cause). Capped: fires per call
                // only while triaging (first 10), then silent fallthrough.
                static int grr = 0;
                if (grr < 10) {
                    grr++;
                    std::printf("[jni] GetElem real-path refused a1=%08x re=%p rarr=%p\n", a1,
                                (void*)re, rarr);
                }
            }
        }
#endif
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_array_lens_.find(a1);
        if (it == jni_array_lens_.end() || !CheckRange(a1, it->second, false)) {
            std::printf("[jni] LOUD GetByteArrayElements bad array %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (a2 && CheckRange(a2, 1, true)) {  // jboolean* isCopy: 1 byte (jni.h)
            *H<std::uint8_t>(a2) = 0;
        }
        Ret(regs, a1);  // handle IS the buffer
        return;
    }
    case 183: case 185: case 186: case 187: case 188: case 189: case 190:
    case 191: case 193: case 194: case 195: case 196: case 197: case 198:
    case 199: case 200: case 201: case 202: case 203: case 204: case 205: case 206:
    case 207: case 208: case 209: case 210: case 211: case 212: case 213: case 214: {
        // The other primitive arrays (the byte ones keep their own path
        // above): Get<T>ArrayElements 183-190 / Release 191-198 pin a guest
        // copy of a real array, copied back per mode; Get/Set<T>ArrayRegion
        // 199-206 / 207-214 copy a range. Types in JNI order: boolean, byte,
        // char, short, int, long, float, double.
        static const std::uint32_t kSize[8] = {1, 1, 2, 2, 4, 8, 4, 8};
        const bool get_el = fn >= 183 && fn <= 190, rel = fn >= 191 && fn <= 198;
        const bool get_rg = fn >= 199 && fn <= 206;
        const int t = static_cast<int>(get_el ? fn - 183 : rel ? fn - 191 : get_rg ? fn - 199 : fn - 207);
        const std::uint32_t es = kSize[t];
#ifdef __ANDROID__
        JNIEnv* re = RealEnv();
        jarray ra = static_cast<jarray>(UnwrapObject(a1));
        if (re && ra) {
            if (get_el) {
                const jsize n = re->GetArrayLength(ra);
                void* hp = nullptr;
                switch (t) {
                    case 0: hp = re->GetBooleanArrayElements(static_cast<jbooleanArray>(ra), nullptr); break;
                    case 2: hp = re->GetCharArrayElements(static_cast<jcharArray>(ra), nullptr); break;
                    case 3: hp = re->GetShortArrayElements(static_cast<jshortArray>(ra), nullptr); break;
                    case 4: hp = re->GetIntArrayElements(static_cast<jintArray>(ra), nullptr); break;
                    case 5: hp = re->GetLongArrayElements(static_cast<jlongArray>(ra), nullptr); break;
                    case 6: hp = re->GetFloatArrayElements(static_cast<jfloatArray>(ra), nullptr); break;
                    case 7: hp = re->GetDoubleArrayElements(static_cast<jdoubleArray>(ra), nullptr); break;
                    default: break;
                }
                const std::uint64_t bytes = static_cast<std::uint64_t>(n > 0 ? n : 0) * es;
                const std::uint32_t gb = (hp && bytes < 0x10000000u) ? GuestMalloc(static_cast<std::uint32_t>(bytes ? bytes : 1)) : 0;
                if (!gb) {
                    re->ExceptionClear();
                    Ret(regs, 0);
                    return;
                }
                std::memcpy(H(gb), hp, static_cast<std::size_t>(bytes));
                {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    array_pins_[gb] = ArrayPin{ra, hp, gb, static_cast<std::uint32_t>(bytes), t};
                }
                if (a2 && CheckRange(a2, 1, true)) {
                    *H<std::uint8_t>(a2) = 1;  // isCopy
                }
                Ret(regs, gb);
                return;
            }
            if (rel) {
                // (env, array, elems, mode): the pin is keyed by the guest copy.
                ArrayPin pin;
                bool have = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    const auto it = array_pins_.find(a2);
                    if (it != array_pins_.end()) {
                        pin = it->second;
                        if (a3 != 1) {
                            array_pins_.erase(it);
                        }
                        have = true;
                    }
                }
                if (have) {
                    if ((a3 == 0 || a3 == 1) && CheckRange(pin.guest_buf, pin.len, false)) {
                        std::memcpy(pin.host_elems, H(pin.guest_buf), pin.len);
                    }
                    const jint mode = static_cast<jint>(a3);
                    switch (pin.type) {
                        case 0: re->ReleaseBooleanArrayElements(static_cast<jbooleanArray>(pin.real_arr), static_cast<jboolean*>(pin.host_elems), mode); break;
                        case 2: re->ReleaseCharArrayElements(static_cast<jcharArray>(pin.real_arr), static_cast<jchar*>(pin.host_elems), mode); break;
                        case 3: re->ReleaseShortArrayElements(static_cast<jshortArray>(pin.real_arr), static_cast<jshort*>(pin.host_elems), mode); break;
                        case 4: re->ReleaseIntArrayElements(static_cast<jintArray>(pin.real_arr), static_cast<jint*>(pin.host_elems), mode); break;
                        case 5: re->ReleaseLongArrayElements(static_cast<jlongArray>(pin.real_arr), static_cast<jlong*>(pin.host_elems), mode); break;
                        case 6: re->ReleaseFloatArrayElements(static_cast<jfloatArray>(pin.real_arr), static_cast<jfloat*>(pin.host_elems), mode); break;
                        case 7: re->ReleaseDoubleArrayElements(static_cast<jdoubleArray>(pin.real_arr), static_cast<jdouble*>(pin.host_elems), mode); break;
                        default: break;
                    }
                    if (a3 != 1) {
                        GuestFree(pin.guest_buf);
                    }
                }
                re->ExceptionClear();
                Ret(regs, 0);
                return;
            }
            // Region: (env, array, start, len, buf@[sp])
            std::uint32_t buf = 0;
            if (!CheckRange(regs[13], 4, false)) {
                goto jni_bad;
            }
            std::memcpy(&buf, H(regs[13]), 4);
            const std::uint64_t bytes = std::uint64_t(a3) * es;
            if (bytes > 0x10000000u || (bytes && !CheckRange(buf, static_cast<std::uint32_t>(bytes), get_rg))) {
                goto jni_bad;
            }
            void* hb = bytes ? H(buf) : nullptr;
            const jsize st = static_cast<jsize>(a2), n = static_cast<jsize>(a3);
            switch (t + (get_rg ? 0 : 8)) {
                case 0: re->GetBooleanArrayRegion(static_cast<jbooleanArray>(ra), st, n, static_cast<jboolean*>(hb)); break;
                case 1: re->GetByteArrayRegion(static_cast<jbyteArray>(ra), st, n, static_cast<jbyte*>(hb)); break;
                case 2: re->GetCharArrayRegion(static_cast<jcharArray>(ra), st, n, static_cast<jchar*>(hb)); break;
                case 3: re->GetShortArrayRegion(static_cast<jshortArray>(ra), st, n, static_cast<jshort*>(hb)); break;
                case 4: re->GetIntArrayRegion(static_cast<jintArray>(ra), st, n, static_cast<jint*>(hb)); break;
                case 5: re->GetLongArrayRegion(static_cast<jlongArray>(ra), st, n, static_cast<jlong*>(hb)); break;
                case 6: re->GetFloatArrayRegion(static_cast<jfloatArray>(ra), st, n, static_cast<jfloat*>(hb)); break;
                case 7: re->GetDoubleArrayRegion(static_cast<jdoubleArray>(ra), st, n, static_cast<jdouble*>(hb)); break;
                case 8: re->SetBooleanArrayRegion(static_cast<jbooleanArray>(ra), st, n, static_cast<const jboolean*>(hb)); break;
                case 9: re->SetByteArrayRegion(static_cast<jbyteArray>(ra), st, n, static_cast<const jbyte*>(hb)); break;
                case 10: re->SetCharArrayRegion(static_cast<jcharArray>(ra), st, n, static_cast<const jchar*>(hb)); break;
                case 11: re->SetShortArrayRegion(static_cast<jshortArray>(ra), st, n, static_cast<const jshort*>(hb)); break;
                case 12: re->SetIntArrayRegion(static_cast<jintArray>(ra), st, n, static_cast<const jint*>(hb)); break;
                case 13: re->SetLongArrayRegion(static_cast<jlongArray>(ra), st, n, static_cast<const jlong*>(hb)); break;
                case 14: re->SetFloatArrayRegion(static_cast<jfloatArray>(ra), st, n, static_cast<const jfloat*>(hb)); break;
                case 15: re->SetDoubleArrayRegion(static_cast<jdoubleArray>(ra), st, n, static_cast<const jdouble*>(hb)); break;
                default: break;
            }
            if (re->ExceptionCheck()) {
                re->ExceptionClear();  // ArrayIndexOutOfBounds: nothing copied
            }
            Ret(regs, 0);
            return;
        }
#endif
        std::printf("[jni] LOUD array fn#%u on a non-Java array %08x -- halting\n", fn, a1);
        HaltGuest();
        return;
    jni_bad:
        std::printf("[jni] LOUD array region fn#%u: bad guest buffer -- halting\n", fn);
        HaltGuest();
        return;
    }
    case 192: {  // ReleaseByteArrayElements(env, array, elems, mode) -> void
#ifdef __ANDROID__
        // Pinned real array: copy back per mode. JNI modes: 0 = copy back and
        // release, JNI_COMMIT (1) = copy back but KEEP the buffer (a later
        // release follows), JNI_ABORT (2) = release without copying.
        {
            ArrayPin pin;
            bool have = false;
            const bool commit_only = (a3 == 1);
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                const auto it = array_pins_.find(a1);
                if (it != array_pins_.end()) {
                    pin = it->second;
                    if (!commit_only) {
                        array_pins_.erase(it);
                    }
                    have = true;
                }
            }
            if (have) {
                JNIEnv* re = GuestCallbacks::g_real_env_hook
                                 ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                                 : nullptr;
                if (re && pin.host_elems &&
                    CheckRange(pin.guest_buf, pin.len, false)) {
                    if (a3 == 0 || a3 == 1) {  // 0/JNI_COMMIT: copy back
                        std::memcpy(pin.host_elems, H(pin.guest_buf), pin.len);
                    }
                    re->ReleaseByteArrayElements(static_cast<jbyteArray>(pin.real_arr),
                                                 static_cast<jbyte*>(pin.host_elems),
                                                 (jint)a3);
                    if (re->ExceptionCheck()) {
                        re->ExceptionClear();
                    }
                }
                // Recycle the guest copy (cap 8; overflow leaks as before).
                // Only properly-Released buffers: the guest cannot hold them
                // (it must Release before the next Get on the same array).
                // After JNI_COMMIT the guest still owns the buffer.
                if (!commit_only && pin.guest_buf && pin.len) {
                    std::lock_guard<std::recursive_mutex> lock(heap_mu_);  // array_pool_
                    if (array_pool_.size() < 8) {
                        array_pool_.push_back({pin.guest_buf, pin.len});
                    }
                }
                Ret(regs, 0);
                return;
            }
        }
#endif
        Ret(regs, 0);  // direct pointer: nothing to copy back or free
        return;
    }
    case 215: {  // RegisterNatives(env, clazz, methods, nMethods) -> jint
        // methods = JNINativeMethod[nMethods] {name, signature, fnPtr}, 12
        // bytes each on the guest. Product host: each guest fnPtr gets a host
        // stub (g_native_stub_hook) that the real JVM calls and that runs the
        // guest function; the real RegisterNatives result is returned. PC
        // tools (no real JVM) only log the table and return JNI_OK.
        std::string cname;
        cname = JniClassName(a1);
        struct Entry {
            std::string name, sig;
            std::uint32_t fn;
        };
        std::vector<Entry> entries;
        bool bad = cname.empty() || static_cast<std::int32_t>(a3) < 0 || a3 > 4096u ||
                   (a3 && !CheckRange(a2, a3 * 12u, false));
        for (std::uint32_t k = 0; !bad && k < a3; k++) {
            std::uint32_t nm = 0, sg = 0, fp = 0;
            std::memcpy(&nm, H(a2 + k * 12), 4);
            std::memcpy(&sg, H(a2 + k * 12 + 4), 4);
            std::memcpy(&fp, H(a2 + k * 12 + 8), 4);
            if (!nm || !sg || !fp || !CheckRange(nm, 1, false) || !CheckRange(sg, 1, false)) {
                bad = true;
                break;
            }
            entries.push_back({GuestStr(nm), GuestStr(sg), fp});
            std::printf("[jni] RegisterNatives %s.%s%s -> %08x\n", cname.c_str(),
                        entries.back().name.c_str(), entries.back().sig.c_str(), fp);
        }
        if (bad) {
            std::printf("LOUD [jni] RegisterNatives class %08x (%s) methods %08x n=%u: bad table\n",
                        a1, cname.empty() ? "?" : cname.c_str(), a2, a3);
            Ret(regs, static_cast<std::uint32_t>(-1));  // JNI_ERR
            return;
        }
#ifdef __ANDROID__
        if (JNIEnv* re = RealEnv(); re && GuestCallbacks::g_native_stub_hook) {
            jclass rc = static_cast<jclass>(RealClass(re, cname));
            if (!rc) {
                std::printf("LOUD [jni] RegisterNatives: no real class %s\n", cname.c_str());
                Ret(regs, static_cast<std::uint32_t>(-1));  // JNI_ERR
                return;
            }
            std::vector<JNINativeMethod> ms;
            for (const Entry& e : entries) {
                void* stub = GuestCallbacks::g_native_stub_hook(cname.c_str(), e.name.c_str(),
                                                                e.sig.c_str(), e.fn);
                if (!stub) {
                    std::printf("LOUD [jni] RegisterNatives %s.%s%s: no host stub left\n",
                                cname.c_str(), e.name.c_str(), e.sig.c_str());
                    Ret(regs, static_cast<std::uint32_t>(-1));  // JNI_ERR
                    return;
                }
                ms.push_back({e.name.c_str(), e.sig.c_str(), stub});
            }
            const jint r = re->RegisterNatives(rc, ms.data(), static_cast<jint>(ms.size()));
            if (r != JNI_OK || re->ExceptionCheck()) {
                // NoSuchMethodError: a method the Java class does not declare.
                std::printf("LOUD [jni] RegisterNatives %s -> %d\n", cname.c_str(), (int)r);
                re->ExceptionClear();
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
#endif
        Ret(regs, 0);  // JNI_OK
        return;
    }
    case 6: {  // FindClass(env, name) -> jclass (interned mock)
        if (!CheckRange(a1, 1, false)) {
            std::printf("[jni] FindClass BAD name %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        const std::string name = GuestStr(a1);
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& kv : jni_classes_) {
            if (kv.second == name) {
                Ret(regs, kv.first);  // already interned
                return;
            }
        }
        std::uint32_t h = 0;
        if (!JniAllocObjectHandle(&h)) {
            return;
        }
        std::memcpy(H(h), &h, 4);  // self-marked handle word
        jni_classes_[h] = name;
        if (DiagOn()) {
            std::printf("[jni] FindClass \"%s\" -> %08x\n", name.c_str(), h);
        }
        Ret(regs, h);
        return;
    }
    case 33:   // GetMethodID(env, class, name, sig) -> jmethodID cookie
    case 94:   // GetFieldID(env, class, name, sig) -> jfieldID cookie
    case 113:  // GetStaticMethodID(env, class, name, sig) -> jmethodID cookie
    case 144:  // GetStaticFieldID(env, class, name, sig) -> jfieldID cookie
    {
        if (!CheckRange(a2, 1, false) || !CheckRange(a3, 1, false)) {
            std::printf("[jni] Get%sMethodID BAD name/sig -- halting\n",
                        fn == 33 ? "" : "Static");
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        const std::string mname = GuestStr(a2);
        const std::string msig = GuestStr(a3);
        std::string cname = JniClassName(a1);
        if (cname.empty()) {
            cname = "?";
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t c = 0;
        if (!JniAllocObjectHandle(&c)) {
            return;
        }
        std::memcpy(H(c), &c, 4);
        // Methods: "Cls.name(sig)ret" (the signature starts with '(').
        // Fields: "field Cls.name:sig" -- a field descriptor follows the name
        // with no delimiter of its own, and ':' can appear in neither a Java
        // identifier nor a type descriptor.
        const bool is_field = (fn == 94 || fn == 144);
        jni_methods_[c] =
            (fn == 33 ? std::string("")
                      : fn == 113 ? std::string("static ")
                                  : std::string("field ")) +
            cname + "." + mname + (is_field ? ":" : "") + msig;
        if (DiagOn()) {
            std::printf("[jni] Get%s%sID %s -> %08x\n", (fn == 33 || fn == 94) ? "" : "Static",
                        (fn == 33 || fn == 113) ? "Method" : "Field", jni_methods_[c].c_str(), c);
        }
        Ret(regs, c);
        return;
    }
    case 95: {  // GetObjectField(env, obj, field) -> value
#ifdef __ANDROID__
        // Real-first (audio buffer fields live on real objects).
        // Mock field cookies carry "field Cls.name(sig)".
        {
            std::string fdesc = "?";
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                const auto it = jni_methods_.find(a2);
                if (it != jni_methods_.end()) {
                    fdesc = it->second;
                }
            }
            JNIEnv* re = GuestCallbacks::g_real_env_hook
                             ? static_cast<JNIEnv*>(GuestCallbacks::g_real_env_hook())
                             : nullptr;
            void* robj = UnwrapObject(a1);
            std::string d = fdesc;
            const std::string pfx = "field ";
            if (re && robj && d.compare(0, pfx.size(), pfx) == 0) {
                d = d.substr(pfx.size());
                // "Cls.name:sig" (see GetFieldID).
                std::string cls, name, sig;
                const std::size_t colon = d.find(':');
                if (colon != std::string::npos) {
                    const std::string head = d.substr(0, colon);
                    sig = d.substr(colon + 1);
                    const std::size_t dot = head.rfind('.');
                    if (dot != std::string::npos) {
                        cls = head.substr(0, dot);
                        name = head.substr(dot + 1);
                    }
                }
                jfieldID rfid = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    const auto it = real_fields_.find(a2);
                    if (it != real_fields_.end()) {
                        rfid = static_cast<jfieldID>(it->second);
                    }
                }
                if (!rfid && !cls.empty() && !name.empty() && !sig.empty()) {
                    jclass rclazz = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> lock(mutex_);
                        const auto it = real_classes_.find(cls);
                        if (it != real_classes_.end()) {
                            rclazz = static_cast<jclass>(it->second);
                        }
                    }
                    if (!rclazz) {
                        rclazz = static_cast<jclass>(RealClass(re, cls));  // + the app loader fallback
                    }
                    if (!rclazz) {
                        jclass lc = re->FindClass(cls.c_str());
                        if (lc && !re->ExceptionCheck()) {
                            rclazz = static_cast<jclass>(re->NewGlobalRef(lc));
                            re->DeleteLocalRef(lc);
                            if (rclazz) {
                                std::lock_guard<std::recursive_mutex> lock(mutex_);
                                real_classes_[cls] = rclazz;
                            }
                        } else {
                            re->ExceptionClear();
                        }
                    }
                    if (rclazz) {
                        rfid = re->GetFieldID(rclazz, name.c_str(), sig.c_str());
                        if (!rfid || re->ExceptionCheck()) {
                            re->ExceptionClear();
                            rfid = nullptr;
                        } else {
                            std::lock_guard<std::recursive_mutex> lock(mutex_);
                            real_fields_[a2] = rfid;
                        }
                    }
                }
                if (rfid) {
                    const char rk = sig.empty() ? 'L' : sig[0];
                    if (rk == 'L' || rk == '[') {
                        jobject ro = re->GetObjectField(static_cast<jobject>(robj), rfid);
                        if (!re->ExceptionCheck()) {
                            const std::uint32_t h = WrapObject(ro);
                            if (ro) {
                                re->DeleteLocalRef(ro);
                            }
                            std::printf("[jni] GetObjectField %s -> %08x (real)\n",
                                        fdesc.c_str(), h);
                            Ret(regs, h);
                            return;
                        }
                        re->ExceptionClear();
                    } else if (rk == 'I' || rk == 'Z' || rk == 'B' || rk == 'S' ||
                               rk == 'C') {
                        jint v = 0;
                        if (rk == 'I') {
                            v = re->GetIntField(static_cast<jobject>(robj), rfid);
                        } else if (rk == 'Z') {
                            v = re->GetBooleanField(static_cast<jobject>(robj), rfid);
                        } else if (rk == 'B') {
                            v = re->GetByteField(static_cast<jobject>(robj), rfid);
                        } else if (rk == 'S') {
                            v = re->GetShortField(static_cast<jobject>(robj), rfid);
                        } else {
                            v = re->GetCharField(static_cast<jobject>(robj), rfid);
                        }
                        if (!re->ExceptionCheck()) {
                            std::printf("[jni] GetObjectField %s -> %d (real)\n",
                                        fdesc.c_str(), (int)v);
                            Ret(regs, (std::uint32_t)v);
                            return;
                        }
                        re->ExceptionClear();
                    }
                }
            }
        }
#endif
        std::printf("[jni] LOUD GetObjectField unhandled -- halting\n");
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    default:
        break;
    }
    // Name the method when we can: the descriptor is in the mock's method
    // table (a2), and every implemented case logs it the same way.
    {
        std::string mdesc = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_methods_.find(a2);
            if (it != jni_methods_.end()) {
                mdesc = it->second;
            }
        }
        std::printf("[svc] LOUD unimplemented JNI fn#%u (%s) from pc=%08x -- halting\n", fn,
                    mdesc.c_str(), regs[15] - 4);
    }
    stop_step_ = true;
    ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

void Runtime::SvcHistDump() {
    if (!std::getenv("HARNESS_SVCHIST")) {
        return;
    }
    std::vector<std::pair<std::string, std::uint64_t>> v;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& kv : stub_hits_) {
            if (kv.first.rfind("#trace:", 0) == 0 || kv.first.rfind("#jni:", 0) == 0) {
                v.emplace_back(kv.first, kv.second);
            }
        }
    }
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("[svchist] top %zu of %zu:\n", std::min<std::size_t>(v.size(), 100), v.size());
    for (std::size_t i = 0; i < v.size() && i < 100; i++) {
        std::printf("[svchist] %12llu %s\n", (unsigned long long)v[i].second, v[i].first.c_str());
    }
}

// ---- memory callbacks ----

// Value watchpoints (HARNESS_WP=hexvalue): trace where a value is written/loaded.
static std::int64_t ReadWP() {
    return WriteWP();
}

// Address watchpoint (HARNESS_WPA=hexaddr): log every data read from addr.
static std::int64_t WatchAddr() {
    static std::int64_t wa = -2;
    if (wa == -2) {
        wa = -1;
        if (const char* e = std::getenv("HARNESS_WPA")) {
            wa = static_cast<std::int64_t>(std::strtoul(e, nullptr, 16));
        }
    }
    return wa;
}

template <typename T>
T GuestCallbacks::Read(VAddr addr, bool exec) {
    if (prof::g_enabled) {
        prof::t_slow_mem += !exec;
    }
    if (!rt_->CheckAccess(addr, sizeof(T), false, exec)) {
        if (rt_->NoteBad()) {
            std::printf("[mem][t%u] BAD %s @ %08x size %zu guest-pc=%08x lr=%08x r4=%08x\n",
                        t_hot.tid, exec ? "exec" : "read", addr, sizeof(T),
                        rt_->ActiveJit()->Regs()[15], rt_->ActiveJit()->Regs()[14],
                        rt_->ActiveJit()->Regs()[4]);
            rt_->DumpEventRing();
        }
        rt_->stop_step_ = true;
        rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    // A naturally aligned access is one single-copy-atomic host load (ARMv7
    // guarantees that for aligned LDR/LDRH/LDRD-by-LDREXD and the exclusive
    // reads route through here); only unaligned accesses go byte by byte.
    // Guest and host are both little-endian.
    T v = 0;
    if ((addr & (sizeof(T) - 1)) == 0) {
        v = __atomic_load_n(rt_->H<T>(addr), __ATOMIC_RELAXED);
    } else {
        const std::uint8_t* p = rt_->H<const std::uint8_t>(addr);
        for (std::size_t i = 0; i < sizeof(T); i++) {
            v |= T(p[i]) << (8 * i);
        }
    }
    // Data-load trace (HARNESS_TRACELOADS=1, step mode): every data read.
    static std::int64_t tl = -2;
    if (tl == -2) {
        tl = -1;
        if (std::getenv("HARNESS_TRACELOADS")) {
            tl = 1;
        }
    }
    if (tl > 0 && !exec && addr >= 0x650000u) {
        std::printf("[ld] pc=%08x [%08x] = %08x\n", rt_->ActiveJit()->Regs()[15], addr,
                    static_cast<std::uint32_t>(v));
    }
    if (sizeof(T) == 4) {
        const std::int64_t wp = ReadWP();
        if (wp >= 0 && v == static_cast<T>(wp)) {
            auto& regs = rt_->ActiveJit()->Regs();
            std::printf("[wpr] %08x loaded from %08x guest-pc=%08x\n", static_cast<std::uint32_t>(v),
                        addr, regs[15]);
            std::printf("[wpr] regs:");
            for (int r = 0; r < 16; r++) {
                std::printf(" r%d=%08x", r, regs[r]);
            }
            std::printf("\n");
        }
        const std::int64_t wa = WatchAddr();
        if (wa >= 0 && addr == static_cast<std::uint32_t>(wa)) {
            std::printf("[wpa] read from %08x = %08x guest-pc=%08x\n", addr,
                        static_cast<std::uint32_t>(v), rt_->ActiveJit()->Regs()[15]);
        }
    }
    return v;
}

template <typename T>
void GuestCallbacks::Write(VAddr addr, T v) {
    if (prof::g_enabled) {
        prof::t_slow_mem++;
    }
    {
        const char* gwhat = nullptr;
        if (rt_->WriteGuardHit(addr, sizeof(T), &gwhat)) {
            // RELRO after relocation: the real loader makes these pages
            // read-only, so this write cannot be legitimate. Refuse it (a
            // zeroed GOT slot turns the next PLT call into `pc = 0`), and name
            // the writer: pc + caller LR + the args that name the victim/source.
            if (rt_->NoteWriteGuard()) {
                std::printf("[mem] %s write REFUSED @ %08x size %zu guest-pc=%08x lr=%08x\n",
                            gwhat, addr, sizeof(T), rt_->ActiveJit()->Regs()[15],
                            rt_->ActiveJit()->Regs()[14]);
                std::printf("[mem] %s REFUSED regs r0=%08x r1=%08x r2=%08x r3=%08x "
                            "r4=%08x r5=%08x r6=%08x r7=%08x\n",
                            gwhat, rt_->ActiveJit()->Regs()[0], rt_->ActiveJit()->Regs()[1],
                            rt_->ActiveJit()->Regs()[2], rt_->ActiveJit()->Regs()[3],
                            rt_->ActiveJit()->Regs()[4], rt_->ActiveJit()->Regs()[5],
                            rt_->ActiveJit()->Regs()[6], rt_->ActiveJit()->Regs()[7]);
            }
            return;
        }
    }
    if (!rt_->CheckAccess(addr, sizeof(T), true, false)) {
        if (rt_->NoteBad()) {
            std::printf("[mem] BAD write @ %08x size %zu guest-pc=%08x -- halting\n", addr,
                        sizeof(T), rt_->ActiveJit()->Regs()[15]);
            // Fox-hunt (2026-10-15): GC mark-store faults need the list owner
            // (r4), victim object (r6) and marker source (r7) to name the
            // writer. Capped by NoteBad like the line above.
            std::printf("[mem] BAD regs lr=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n",
                        rt_->ActiveJit()->Regs()[14], rt_->ActiveJit()->Regs()[4],
                        rt_->ActiveJit()->Regs()[5], rt_->ActiveJit()->Regs()[6],
                        rt_->ActiveJit()->Regs()[7]);
            // Fox-hunt: identify the gray object at r7 (header dump names the
            // type: table/stack/closure) + find the garbage slot nearby (the
            // deterministic heap makes its address stable across boots ->
            // range-watch it next run to catch the writer). Capped by NoteBad.
            {
                const std::uint32_t o7 = rt_->ActiveJit()->Regs()[7];
                if (rt_->CheckRange(o7 - 16, 512, false)) {
                    const std::uint8_t* p = rt_->H<const std::uint8_t>(o7 - 16);
                    std::printf("[mem] BAD r7obj:");
                    for (int j = 0; j < 512; j++) {
                        std::printf(" %02x", p[j]);
                    }
                    std::printf("\n");
                }
            }
            rt_->DumpEventRing();
        }
        rt_->stop_step_ = true;
        rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    const std::int64_t wa = WatchAddr();
    if (wa >= 0 && addr == static_cast<std::uint32_t>(wa)) {
        std::printf("[wpa-w] write to %08x = %08x guest-pc=%08x\n", addr,
                    static_cast<std::uint32_t>(v), rt_->ActiveJit()->Regs()[15]);
    }
    // HUNT watch: every legal guest store checked against live heap.
    // Static tristate (no EnvOn mutex on this path: millions of stores).
    {
        static int hwen = -1;
        if (hwen < 0) {
            hwen = std::getenv("HARNESS_HEAPWATCH") ? 1 : 0;
        }
        if (hwen) {
            rt_->HeapWatchViolation(addr, sizeof(T));
        }
    }
    if ((addr & (sizeof(T) - 1)) == 0) {
        __atomic_store_n(rt_->H<T>(addr), v, __ATOMIC_RELAXED);  // single-copy atomic
    } else {
        std::uint8_t* p = rt_->H<std::uint8_t>(addr);
        for (std::size_t i = 0; i < sizeof(T); i++) {
            p[i] = std::uint8_t(v >> (8 * i));
        }
    }
}

std::uint8_t GuestCallbacks::MemoryRead8(VAddr a) { return Read<std::uint8_t>(a, false); }
std::uint16_t GuestCallbacks::MemoryRead16(VAddr a) { return Read<std::uint16_t>(a, false); }
std::uint32_t GuestCallbacks::MemoryRead32(VAddr a) { return Read<std::uint32_t>(a, false); }
std::uint64_t GuestCallbacks::MemoryRead64(VAddr a) { return Read<std::uint64_t>(a, false); }
void* (*GuestCallbacks::g_real_env_hook)() = nullptr;
void* (*GuestCallbacks::g_native_stub_hook)(const char*, const char*, const char*,
                                            std::uint32_t) = nullptr;
// HARNESS_PCLOG=addr[,addr...]: each time a guest block starting at one of
// these addresses runs, log its r0-r2 and lr (the first 200 per address). For
// "is this function called, and with what?" without touching guest code.
namespace {
struct PcLog {
    std::vector<std::uint32_t> pcs;
    PcLog() {
        if (const char* e = std::getenv("HARNESS_PCLOG")) {
            for (const char* p = e; *p;) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p) {
                    break;
                }
                pcs.push_back(static_cast<std::uint32_t>(v));
                p = *end == ',' ? end + 1 : end;
            }
        }
    }
};
const PcLog& PcLogConfig() {
    static const PcLog cfg;
    return cfg;
}
void PcLogHit(std::uint64_t pc, std::uint64_t r01, std::uint64_t r23) {
    static std::mutex mu;
    static std::unordered_map<std::uint64_t, int> seen;
    std::lock_guard<std::mutex> l(mu);
    if (seen[pc]++ < 200) {
        std::printf("[pclog][t%u] %08llx r0=%08llx r1=%08llx r2=%08llx lr=%08llx\n", t_hot.tid,
                    static_cast<unsigned long long>(pc),
                    static_cast<unsigned long long>(r01 & 0xFFFFFFFFu),
                    static_cast<unsigned long long>(r01 >> 32),
                    static_cast<unsigned long long>(r23 & 0xFFFFFFFFu),
                    static_cast<unsigned long long>(r23 >> 32));
    }
}
}  // namespace

void GuestCallbacks::PreCodeTranslationHook(bool is_thumb, VAddr pc,
                                               Dynarmic::A32::IREmitter& ir) {
    for (const std::uint32_t want : PcLogConfig().pcs) {
        if (want == pc) {
            using Dynarmic::A32::Reg;
            auto pair = [&](Reg lo, Reg hi) {
                return ir.Pack2x32To1x64(ir.GetRegister(lo), ir.GetRegister(hi));
            };
            ir.CallHostFunction(&PcLogHit, ir.Imm64(pc), pair(Reg::R0, Reg::R1), pair(Reg::R2, Reg::LR));
        }
    }
    // Translation-time diagnostics only (nothing is emitted into the IR).
    // HARNESS_TRANSLOG=1: every translated block start.
    // HARNESS_PCTRACE=start,len[,cap]: block starts inside [start, start+len).
    // Env read once (thread-safe magic statics), not per translated block.
    static const bool translog = std::getenv("HARNESS_TRANSLOG") != nullptr;
    struct PcTrace {
        std::uint32_t start = 0, end = 0;
        std::int64_t cap = 5000;
        PcTrace() {
            if (const char* e = std::getenv("HARNESS_PCTRACE")) {
                unsigned s = 0, l = 0, c = 5000;
                if (std::sscanf(e, "%x,%x,%x", &s, &l, &c) >= 2) {
                    start = s;
                    end = s + l;
                    cap = (std::int64_t)c;
                }
            }
        }
    };
    static const PcTrace pctrace;
    static std::atomic<std::int64_t> pcn{0};
    if (pctrace.end > pctrace.start && pc >= pctrace.start && pc < pctrace.end &&
        pcn.fetch_add(1, std::memory_order_relaxed) < pctrace.cap) {
        std::printf("[pctrace] %08x\n", pc);
    }
    if (translog) {
        std::printf("[tr] %c %08x\n", is_thumb ? 'T' : 'A', pc);
    }
}

std::optional<std::uint32_t> GuestCallbacks::MemoryReadCode(VAddr a) {
    // Unreadable or non-executable code: no word. Dynarmic then ends the
    // block with a NoExecuteFault that is raised only if a thread reaches
    // that instruction (ExceptionRaised halts it). Returning 0 used to cache
    // a block of "andeq r0, r0, r0" that ran on for any later caller (F-23).
    if (!rt_->CheckAccess(a & ~std::uint32_t(3), 4, false, true)) {
        if (rt_->NoteBad()) {
            std::printf("[mem] BAD exec @ %08x (faults when reached)\n", a);
            rt_->DumpEventRing();
        }
        return std::nullopt;
    }
    std::uint32_t v = 0;
    std::memcpy(&v, rt_->H(a & ~std::uint32_t(3)), 4);
    return v;
}
void GuestCallbacks::MemoryWrite8(VAddr a, std::uint8_t v) { Write(a, v); }
void GuestCallbacks::MemoryWrite16(VAddr a, std::uint16_t v) { Write(a, v); }
void GuestCallbacks::MemoryWrite32(VAddr a, std::uint32_t v) {
    Write(a, v);
    // Value watchpoint (HARNESS_WP=hexvalue): who materializes a value?
    const std::int64_t wp = WriteWP();
    if (wp >= 0 && v == static_cast<std::uint32_t>(wp)) {
        std::printf("[wp] %08x written to %08x guest-pc=%08x\n", v, a, rt_->ActiveJit()->Regs()[15]);
    }
    // Address watchpoint, write side (HARNESS_WPA=hexaddr also arms this).
    // NOTE: bulk stores (guest memset/memcpy thunks) bypass callbacks.
    const std::int64_t wa = WatchAddr();
    if (wa >= 0 && a == static_cast<std::uint32_t>(wa)) {
        auto& regs = rt_->ActiveJit()->Regs();
        std::printf("[wpa] WRITE to %08x = %08x guest-pc=%08x%s\n", a, v, regs[15],
                    rt_->GuestTid() == 1 ? "" : " (T2+)");
        std::printf("[wpa] regs:");
        for (int r = 0; r < 12; r++) {
            std::printf(" r%d=%08x", r, regs[r]);
        }
        std::printf("\n");
    }
}

void GuestCallbacks::MemoryWrite64(VAddr a, std::uint64_t v) { Write(a, v); }

void GuestCallbacks::InterpreterFallback(VAddr pc, std::size_t n) {
    std::printf("[jit] InterpreterFallback pc=%08x n=%zu\n", pc, n);
    rt_->stop_step_ = true;
    rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

void GuestCallbacks::CallSVC(std::uint32_t swi) {
    HotThread& hot = t_hot;  // the one TLS lookup of a plain call
    hot.svc_count++;
    Dynarmic::A32::Jit* jit = hot.active_jit ? hot.active_jit : rt_->main_jit();
    auto& regs = jit->Regs();
    // Where the thread is must be known before signals are ever used: a
    // thread that went to sleep in a host call earlier must not look like it
    // has no guest frames when the first signal comes (a proxy would run the
    // handler over its live stack).
    const bool blocking = rt_->SvcBlocks(swi);
    // The CPSR only matters to a blocking call (a proxy may deliver signals
    // from its snapshot); building it for every call cost ~0.7 %.
    rt_->SigEnterHostCall(regs, blocking ? jit->Cpsr() : 0u, blocking);
    {
        const prof::CallTimer timer(swi);  // HARNESS_PROF only (a thread_local test otherwise)
        rt_->OnSvc(swi, regs);
    }
    rt_->SigLeaveHostCall(blocking);
}

std::string Runtime::SvcName(std::uint32_t swi) const {
    if (swi >= kSwiJniBase && swi < kSwiJniBase + 0x100) {
        return "jni#" + std::to_string(swi - kSwiJniBase);
    }
    if (swi >= 1 && swi - 1 < mod_.imports.size()) {
        return mod_.imports[swi - 1].name;
    }
    return "svc#" + std::to_string(swi);
}

// inet_* (inet_addr/inet_aton/inet_pton/inet_ntoa/inet_ntop). File scope:
// CallImport's gotos to bad_ptr cannot cross automatic inits, so this lives
// here (same convention as TimeWarp). All paths Ret()+return (handled).
void Runtime::InetThunk(ImportId id, Regs& regs, const std::string& n, std::uint32_t a0, std::uint32_t a1,
                        std::uint32_t a2, std::uint32_t a3) {
    using I = ImportId;
    // TEMP-DIAG (ntop failures): log args, capped (ntop has its own cap;
    // pton spam must not eat it).
    if (DiagOn()) {  // PLAN.md A: diagnostic layer
        static int ilog = 0, olog = 0;
        if (id == I::inet_ntop_ && olog < 12) {
            olog++;
            std::printf("[netsock] %s(%u %08x %08x %08x)%s\n", n.c_str(), a0, a1, a2, a3,
                        olog == 12 ? " (quiet henceforth)" : "");
        } else if (id == I::inet_pton_ && ilog < 4) {
            ilog++;
            std::printf("[netsock] %s(%u %08x %08x %08x)\n", n.c_str(), a0, a1, a2, a3);
        }
    }
    if (id == I::inet_ntoa_ || id == I::inet_ntop_) {
        // inet_ntoa(struct in_addr): addr comes BY VALUE in r0 (NOT a
        // pointer). inet_ntop(af, src, dst, size): dst=a2, size=a3.
        struct in_addr ia {};
        const void* src = nullptr;
        std::uint32_t dst = 0;
        std::uint32_t dstsize = 16;
        if (id == I::inet_ntoa_) {
            std::uint32_t v = a0;
            std::memcpy(&ia, &v, 4);
            src = &ia;
        } else {
            // AF_INET (16B min) and AF_INET6 (46B min).
            const std::uint32_t need = (a0 == 10 /*AF_INET6*/) ? 46u : 16u;
            if ((a0 != 2 && a0 != 10) || a3 < need || !CheckRange(a1, 4, false) ||
                !CheckRange(a2, need, true)) {
                if (DiagOn()) {  // which check failed
                    const std::uint8_t* sp2 =
                        (a1 && CheckRange(a1, 16, false)) ? H<const std::uint8_t>(a1) : nullptr;
                    std::printf("[netsock] ntop-FAIL af=%u size=%u src=%08x dst=%08x%s", a0, a3,
                                a1, a2, sp2 ? " bytes:" : " (src unreadable)");
                    if (sp2) {
                        for (int q = 0; q < 16; q++) {
                            std::printf(" %02x", sp2[q]);
                        }
                    }
                    std::printf("\n");
                }
                SetGuestErrno(a0 != 2 && a0 != 10 ? EAFNOSUPPORT : ENOSPC);
                Ret(regs, 0);
                return;
            }
            src = H(a1);
            dst = a2;
            dstsize = a3;
        }
        char tmp[64];
        if (!inet_ntop(a0 == 10 ? AF_INET6 : AF_INET, src, tmp, sizeof(tmp))) {
            Ret(regs, 0);
            return;
        }
        if (id == I::inet_ntoa_) {
            // Fresh 16B guest buffer per call (16B leak, bring-up):
            // avoids thread_local (goto-crossing) and shared-slot
            // races. ntoa is logging-path rare.
            dst = GuestMalloc(16);
            if (!dst || !CheckRange(dst, 16, true)) {
                Ret(regs, 0);
                return;
            }
        }
        const std::size_t L = std::strlen(tmp) + 1;
        if (L > dstsize) {
            Ret(regs, 0);
            return;
        }
        std::memcpy(H(dst), tmp, L);
        Ret(regs, dst);
        return;
    }
    // String -> binary. Bionic == POSIX here; forward to host.
    // inet_pton(af, src, dst): the string is a1 (NOT a0).
    // NOTE: plain pointer (GuestStr scratch), never std::string here:
    // later `goto bad_ptr` sites cannot jump over non-trivial inits.
    const std::uint32_t sptr = (id == I::inet_pton_) ? a1 : a0;
    if (!CheckRange(sptr, 1, false)) {
        Ret(regs, (id == I::inet_addr_) ? 0xFFFFFFFFu : 0u);
        return;
    }
    const char* s = GuestStr(sptr);
    if (id == I::inet_addr_) {
        Ret(regs, static_cast<std::uint32_t>(::inet_addr(s)));
        return;
    }
    if (id == I::inet_aton_) {
        // inet_aton(cp, struct in_addr* inp) -> 1 valid, 0 not; inp may be
        // NULL (validation only).
        struct in_addr ia {};
        const int r = ::inet_aton(s, &ia);
        if (r && a1) {
            if (!CheckRange(a1, 4, true)) {
                Ret(regs, 0);
                return;
            }
            std::memcpy(H(a1), &ia.s_addr, 4);
        }
        Ret(regs, r ? 1u : 0u);
        return;
    }
    // inet_pton(af, src, dst): AF_INET (4B) and AF_INET6 (16B).
    const std::uint32_t need = (a0 == 10 /*AF_INET6*/) ? 16u : 4u;
    if ((a0 != 2 && a0 != 10) || a2 == 0 || !CheckRange(a2, need, true)) {
        Ret(regs, 0);
        return;
    }
    unsigned char b[16] = {};
    const int r = ::inet_pton(a0 == 10 ? AF_INET6 : AF_INET, s, b);
    if (r == 1) {
        std::memcpy(H(a2), b, need);
    }
    Ret(regs, static_cast<std::uint32_t>(r));
    return;
}

void GuestCallbacks::ExceptionRaised(VAddr pc, Exception ex) {
    // Emulate the classic ARM switch idiom `add pc, rN, pc` (Rd==pc, Rm==pc),
    // which Dynarmic reports as UnpredictableInstruction. Semantics: branch to
    // rN + (insn+8), staying in ARM state. Narrow match: ADD-register, S==0,
    // no shift, Rd==15, Rm==15 (any cond, any Rn).
    if (ex == Exception::UnpredictableInstruction) {
        const std::uint32_t a = pc & ~std::uint32_t(3);
        std::uint32_t w = 0xDEADDEADu;
        const bool ok = rt_->CheckRange(a, 4, false);
        if (ok) {
            std::memcpy(&w, rt_->H(a), 4);
        }
        std::printf("[jit] unpred @%08x ok=%d w=%08x\n", a, (int)ok, w);
        // NOTE: in Run mode Dynarmic reports the BLOCK start, so `a` is the
        // `ldr rN, [pc, #imm]` while the unpredictable `add pc, rN, pc` sits
        // at a+4. Match the 2-insn pair and emulate both (idempotent if the
        // ldr already executed: same value rewritten).
        if (ok && rt_->CheckRange(a + 4, 4, false)) {
            std::uint32_t w1 = 0;
            std::memcpy(&w1, rt_->H(a + 4), 4);
            const bool is_ldr = ((w >> 26) & 3u) == 1 && ((w >> 25) & 1u) == 0 &&
                                ((w >> 20) & 1u) == 1 && ((w >> 16) & 0xFu) == 0xFu;
            const std::uint32_t rt = (w >> 12) & 0xFu;
            const bool is_add =
                ((w1 >> 26) & 3u) == 0 && ((w1 >> 25) & 1u) == 0 && ((w1 >> 21) & 0xFu) == 4 &&
                ((w1 >> 20) & 1u) == 0 && ((w1 >> 12) & 0xFu) == 0xFu && (w1 & 0xFu) == 0xFu &&
                (w1 & 0xFF0u) == 0 && ((w1 >> 16) & 0xFu) == rt;
            if (is_ldr && is_add) {
                const std::uint32_t off = w & 0xFFFu;
                const std::uint32_t lit = ((w >> 23) & 1u) ? (a + 8 + off) : (a + 8 - off);
                if (rt_->CheckRange(lit, 4, false)) {
                    std::uint32_t rval = 0;
                    std::memcpy(&rval, rt_->H(lit), 4);
                    const std::uint32_t target = rval + (a + 4) + 8;
                    auto& regs = rt_->ActiveJit()->Regs();
                    regs[rt] = rval;
                    regs[15] = target;
                    // Entries are ARM (pad does `bx pc` to switch); the JIT may
                    // still be Thumb (it falls through `bx pc` without switching).
                    // Force USER+ARM, NZCV/GE preserved — same contract as Ret().
                    {
                        std::uint32_t cpsr = rt_->ActiveJit()->Cpsr();
                        cpsr = (cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10);
                        rt_->ActiveJit()->SetCpsr(cpsr);
                    }
                    static std::uint64_t addpc_hits = 0;
                    if (++addpc_hits <= 3) {
                        std::printf("[jit] ldr/add-pc @%08x r%u=%08x -> %08x\n", a, rt, rval,
                                    target);
                    }
                    return;  // resume, do NOT halt
                }
            }
        }
        if (ok) {
            const bool is_add = ((w >> 26) & 3u) == 0 && ((w >> 25) & 1u) == 0 &&
                                ((w >> 21) & 0xFu) == 4 && ((w >> 20) & 1u) == 0;
            if (is_add && ((w >> 12) & 0xFu) == 0xFu && (w & 0xFu) == 0xFu &&
                (w & 0xFF0u) == 0) {
                auto& regs = rt_->ActiveJit()->Regs();
                const std::uint32_t rn = (w >> 16) & 0xFu;
                const std::uint32_t target = regs[rn] + a + 8;
                static std::uint64_t addpc_hits = 0;
                if (++addpc_hits <= 3) {
                    std::printf("[jit] add-pc idiom @%08x r%u=%08x -> %08x\n", a, rn, regs[rn],
                                target);
                }
                regs[15] = target;
                return;  // resume, do NOT halt
            }
        }
    }
    std::printf("[jit] ExceptionRaised pc=%08x ex=%d -- halting\n", pc, static_cast<int>(ex));
    // DIAG: mini-backtrace for null-jumps (pc=0): lr + stack code-pointers.
    {
        auto& regs = rt_->ActiveJit()->Regs();
        std::printf("[jit] lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x%s\n", regs[14], regs[13],
                    regs[0], regs[1], regs[2], regs[3], rt_->GuestTid() == 1 ? "" : " (T2+)");
        std::uint32_t sp = regs[13];
        // Module text range: [0, 0x6EE000) per loader layout (bias 0).
        int shown = 0;
        for (std::uint32_t a = sp & ~3u; a < sp + 512 && shown < 8; a += 4) {
            std::uint32_t w = 0;
            if (!rt_->CheckRange(a, 4, false)) {
                break;
            }
            std::memcpy(&w, rt_->H(a), 4);
            if (w >= 0x1000 && w < 0x6EE000 && (w & 1) == 0) {
                std::printf("[jit] stack+%u: ret %08x\n", a - sp, w);
                shown++;
            }
        }
    }
    rt_->stop_step_ = true;
    rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

// Map a guest path to the host path (file bridge). Extracted from the
// file-bridge block so that other imports (statfs) can use it too.
Runtime::StackScan Runtime::ScanStackForReturnAddresses(std::uint32_t sp,
                                                         std::uint32_t max_bytes,
                                                         StackHit* hits, unsigned hit_cap) {
    StackScan res;
    for (std::uint32_t off = 0; off < max_bytes; off += 4) {
        if (!CheckRange(sp + off, 4, false)) {
            break;  // the mapped region ends here
        }
        std::uint32_t v = 0;
        std::memcpy(&v, H(sp + off), 4);
        res.words_read++;
        if (res.hits < hit_cap && IsCodeAddress(v & ~std::uint32_t(1))) {
            if (hits) {
                hits[res.hits].off = off;
                hits[res.hits].value = v;
            }
            res.hits++;
        }
    }
    return res;
}

// /proc/cpuinfo and /proc/self/auxv as an arm64 kernel shows them to a
// 32-bit task (arch/arm64/kernel/cpuinfo.c: compat feature names and a
// "model name ... (v8l)" line; auxv in 32-bit pairs), built once from the
// host's file and the same AT_HWCAP getauxval answers (kArmV7Hwcap). The
// host's own 64-bit view named no "neon"/"vfp", which the guests parse
// for. "" when the files cannot be made (the host's then pass through).
std::string Runtime::Proc32File(const std::string& g) {
    static std::once_flag once;
    static std::string dir;
    std::call_once(once, [this] {
        if (files_hostbase_.empty()) {
            return;
        }
        const std::string d = std::filesystem::path(files_hostbase_).parent_path().string() + "/proc32";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        static const char* const kNames[] = {
            "swp", "half", "thumb", "26bit", "fastmult", "fpa", "vfp", "edsp", "java", "iwmmxt",
            "crunch", "thumbee", "neon", "vfpv3", "vfpv3d16", "tls", "vfpv4", "idiva", "idivt",
            "vfpd32", "lpae", "evtstrm"};
        std::string features;
        for (std::uint32_t b = 0; b < sizeof(kNames) / sizeof(kNames[0]); b++) {
            if (kArmV7Hwcap & (1u << b)) {
                features += features.empty() ? "" : " ";
                features += kNames[b];
            }
        }
        // cpuinfo: each host block with the compat Features and model name.
        std::string out, block;
        auto flush = [&] {
            if (block.empty()) {
                return;
            }
            std::string rev = "0";
            static const std::string kRev = "CPU revision\t: ";
            const std::size_t r = block.find(kRev);
            if (r != std::string::npos) {
                const std::size_t v = r + kRev.size();
                rev = block.substr(v, block.find('\n', v) - v);
            }
            std::string res;
            std::size_t pos = 0;
            while (pos < block.size()) {
                const std::size_t e = block.find('\n', pos);
                std::string line = block.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                pos = e == std::string::npos ? block.size() : e + 1;
                if (line.compare(0, 8, "Features") == 0) {
                    line = "Features\t: " + features;
                }
                res += line + "\n";
                if (line.compare(0, 9, "processor") == 0) {
                    res += "model name\t: ARMv8 Processor rev " + rev + " (v8l)\n";
                }
            }
            out += res;
            block.clear();
        };
        if (FILE* f = std::fopen("/proc/cpuinfo", "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                if (line[0] == '\n') {
                    flush();
                    out += "\n";
                } else {
                    block += line;
                }
            }
            flush();
            std::fclose(f);
        }
        // auxv: the entries getauxval answers, then AT_NULL.
        const std::uint32_t aux[] = {16, kArmV7Hwcap, 26, 0, 6, 4096, 17, 100,
                                     11, static_cast<std::uint32_t>(::getuid()),
                                     12, static_cast<std::uint32_t>(::geteuid()),
                                     13, static_cast<std::uint32_t>(::getgid()),
                                     14, static_cast<std::uint32_t>(::getegid()), 0, 0};
        FILE* c = std::fopen((d + "/cpuinfo").c_str(), "w");
        FILE* a = std::fopen((d + "/auxv").c_str(), "wb");
        const bool ok = c && a && !out.empty() && std::fwrite(out.data(), 1, out.size(), c) == out.size() &&
                        std::fwrite(aux, sizeof aux, 1, a) == 1;
        if (c) {
            std::fclose(c);
        }
        if (a) {
            std::fclose(a);
        }
        if (ok) {
            dir = d;
        }
    });
    if (dir.empty()) {
        return "";
    }
    return dir + (g == "/proc/cpuinfo" ? "/cpuinfo" : "/auxv");
}

std::string Runtime::GuestCwd() {
    std::lock_guard<std::mutex> l(cwd_mu_);
    return cwd_.empty() ? (files_prefix_.empty() ? std::string("/") : files_prefix_) : cwd_;
}

std::string Runtime::MapGuestPath(std::uint32_t gp) {
    return MapGuestPathString(GuestStr(gp));
}

// base + rest, where each component of rest that does not exist as spelled
// takes the name of an entry of its directory that matches it ignoring case
// (the device's shared storage ignores case). A component with no match, and
// everything after it, keeps its spelling (a file about to be created).
static std::string CaseInsensitivePath(const std::string& base, const std::string& rest) {
    std::string out = base + rest;
    struct stat st {};
    if (rest.empty() || ::stat(out.c_str(), &st) == 0) {
        return out;
    }
    out = base;
    std::size_t i = 0;
    bool matching = true;
    while (i < rest.size()) {
        if (rest[i] == '/') {
            out += '/';
            i++;
            continue;
        }
        const std::size_t j = rest.find('/', i);
        const std::string part = rest.substr(i, j == std::string::npos ? std::string::npos : j - i);
        std::string name = part;
        if (matching && ::stat((out + part).c_str(), &st) != 0) {
            matching = false;
            if (DIR* d = ::opendir(out.empty() ? "/" : out.c_str())) {
                while (const dirent* e = ::readdir(d)) {
                    if (::strcasecmp(e->d_name, part.c_str()) == 0) {
                        name = e->d_name;
                        matching = true;
                        break;
                    }
                }
                ::closedir(d);
            }
        }
        out += name;
        i = j == std::string::npos ? rest.size() : j;
    }
    return out;
}

std::string Runtime::MapGuestPathString(const std::string& g) {
        // After a chdir, a relative name is the new directory's entry.
        if (!g.empty() && g[0] != '/') {
            std::unique_lock<std::mutex> l(cwd_mu_);
            if (!cwd_.empty()) {
                const std::string abs = cwd_ + "/" + g;
                l.unlock();
                return MapGuestPathString(abs);
            }
        }
        if (g == "/dev/null" || g == "/dev/zero" || g == "/dev/urandom") {
            return g;
        }
        // The app's own APK (Unity reads its data and assemblies out of it):
        // an app may read it; the directory is not writable by the app.
        if (!apk_path_.empty() && g == apk_path_) {
            return g;
        }
        if (g == "/dev/alarm") {
            return "/dev/null";  // Android alarm driver: no host equivalent;
                                 // timer ioctls are stubbed, sink is fine
        }
        // Read-only host passthrough for /sys (CPU topology: the game
        // sizes thread pools from /sys/devices/system/cpu) and /proc
        // (cpuinfo/meminfo reads). Host values are fine for bring-up
        // (sizing/info only). Writes to these trees fail on the host
        // side naturally (read-only fs semantics), which is correct.
        // /etc/hosts + /etc/resolv.conf: the game's bundled HTTP/DNS
        // stack reads them (observed: worker fopen("/etc/hosts") while
        // resolving cloud.rovio.com). HARNESS_HOSTSHACK=1 serves a
        // synthetic hosts (see below) instead of the host file.
        // What a 32-bit process reads there on an arm64 kernel (the host
        // process is 64-bit and would get its own view): see Proc32File.
        if (g == "/proc/cpuinfo" || g == "/proc/self/auxv") {
            const std::string p = Proc32File(g);
            if (!p.empty()) {
                return p;
            }
        }
        if (g.compare(0, 5, "/sys/") == 0 || g.compare(0, 6, "/proc/") == 0 ||
            g == "/etc/resolv.conf") {
            return g;
        }
        if (g == "/etc/hosts") {
            if (EnvOn("HARNESS_HOSTSHACK")) {
                // Synthetic hosts for DNS-hack experiments (no system
                // change: only this guest path is virtualized).
                static std::string hackpath;
                if (hackpath.empty()) {
                    hackpath = files_hostbase_ + "/synth.hosts";
                    FILE* hf = std::fopen(hackpath.c_str(), "r");
                    if (!hf) {
                        hf = std::fopen(hackpath.c_str(), "w");
                        if (hf) {
                            std::fputs("127.0.0.1 localhost\n127.0.0.2 cloud.rovio.com\n",
                                       hf);
                            std::fclose(hf);
                        }
                    } else {
                        std::fclose(hf);
                    }
                }
                std::printf("[files] hosts HACK -> %s\n", hackpath.c_str());
                return hackpath;
            }
            return g;
        }
        if (!files_prefix_.empty() && g.compare(0, files_prefix_.size(), files_prefix_) == 0 &&
            !files_hostbase_.empty()) {
            return files_hostbase_ + g.substr(files_prefix_.size());
        }
        // Any guest path shaped /data/{data,user/<N>}/<pkg>/files<rest> is
        // this app's files dir. We host exactly one guest, so the package
        // component need not be known here: the offline harness has no
        // cmdline package (its SetFilesDir prefix is a caller choice), and
        // on a real device the guest can only reach its own package's dir.
        if (!files_hostbase_.empty() && (g.compare(0, 11, "/data/data/") == 0 ||
                                         g.compare(0, 11, "/data/user/") == 0)) {
            const std::size_t at = g.find("/files");
            if (at != std::string::npos && (g.size() == at + 6 || g[at + 6] == '/')) {
                return files_hostbase_ + g.substr(at + 6);
            }
        }
        // Package data root: breakpad (HockeyApp) builds its minidump path
        // as dirname(FILES_PATH)/hockeyapp_errors_XXXXXX, i.e.
        // /data/data/<pkg>/... — one level ABOVE the files dir, so the
        // /files rule above does not match. Map it to parent(files host).
        if (!files_hostbase_.empty() && (g.compare(0, 11, "/data/data/") == 0 ||
                                         g.compare(0, 11, "/data/user/") == 0)) {
            std::size_t pk = std::string::npos;
            if (g.compare(0, 11, "/data/data/") == 0) {
                pk = g.find('/', 11);
            } else {
                const std::size_t u = g.find('/', 11);  // after the user id
                if (u != std::string::npos) {
                    pk = g.find('/', u + 1);  // after the package
                }
            }
            if (pk != std::string::npos) {
                return std::filesystem::path(files_hostbase_).parent_path().string() +
                       g.substr(pk);
            }
        }
        // The app's own external dir under any storage root
        // (/storage/emulated/<N>, /sdcard, /mnt/sdcard ...): the guest keeps
        // the path Android gave it, the bytes live in the app's data dir
        // (removed with the app, like the real one; nothing is written to
        // shared storage).
        if (!ext_tail_.empty() && (g.compare(0, 9, "/storage/") == 0 ||
                                   g.compare(0, 7, "/sdcard") == 0 ||
                                   g.compare(0, 5, "/mnt/") == 0)) {
            const std::size_t at = g.find(ext_tail_);
            const std::size_t end = at + ext_tail_.size();
            if (at != std::string::npos && (g.size() == end || g[end] == '/')) {
                std::call_once(ext_made_, [this] {
                    std::error_code ec;
                    std::filesystem::create_directories(ext_hostbase_, ec);
                });
                return ext_hostbase_ + g.substr(end);
            }
        }
        // The directories above a storage root exist on a device (a guest
        // that creates its path one level at a time from "/" gets EEXIST
        // there, not ENOENT): an empty host directory stands in for each.
        if (!files_hostbase_.empty()) {
            std::string d = g;
            while (d.size() > 1 && d.back() == '/') {
                d.pop_back();
            }
            if (d == "/storage" || d == "/storage/emulated" || d == "/storage/self" || d == "/mnt") {
                const std::string base = std::filesystem::path(files_hostbase_).parent_path().string() + "/storage_parent";
                std::call_once(storage_parent_made_, [&base] {
                    std::error_code ec;
                    std::filesystem::create_directories(base, ec);
                });
                return base;
            }
        }
        // host.private_sdcard: the rest of shared storage is the app's own
        // "<files>/sdcard", where its data was put (nothing reaches the
        // device's shared storage; Java's downloader looks there too).
        if (private_sdcard_ && !files_hostbase_.empty()) {
            static const char* const kRoots[] = {"/storage/emulated/0", "/storage/self/primary", "/sdcard",
                                                  "/mnt/sdcard"};
            for (const char* root : kRoots) {
                const std::size_t n = std::strlen(root);
                if (g.compare(0, n, root) == 0 && (g.size() == n || g[n] == '/')) {
                    const std::string base = files_hostbase_ + "/sdcard";
                    std::call_once(sdcard_made_, [&base] {
                        std::error_code ec;
                        std::filesystem::create_directories(base, ec);
                    });
                    // Shared storage is case-insensitive on a device (Telltale
                    // asks for "android/obb/..." of "Android/obb/...").
                    return CaseInsensitivePath(base, g.substr(n));
                }
            }
        }
        // Bare relative names (no leading '/'): the game opens seeds like
        // "highscores.lua" CWD-relative (original CWD = files dir). Route
        // to the files dir (fopen fails gracefully if missing — same as
        // device semantics). "data/*" stays unmapped (asset API serves it;
        // PC runs prove the game never fopens data/* directly).
        if (!g.empty() && g[0] != '/' && g.compare(0, 5, "data/") != 0 &&
            !files_hostbase_.empty()) {
            return files_hostbase_ + "/" + g;
        }
        static std::uint64_t unmapped_hits = 0;
        if (++unmapped_hits <= 5) {
            std::printf("[files] unmapped guest path \"%s\"%s\n", g.c_str(),
                        unmapped_hits == 5 ? " (quiet henceforth)" : "");
        }
        return std::string();
    }


}  // namespace abr
