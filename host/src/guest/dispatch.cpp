#include "dispatch.h"

#ifdef __ANDROID__
#include <jni.h>  // real-Java delegation (product host only)
#endif

#include "realgl.h"

#include <dynarmic/frontend/A32/a32_ir_emitter.h>

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
#include <poll.h>
#include <pthread.h>
#include <set>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

extern "C" {
typedef void* dl_mspace;
dl_mspace create_mspace_with_base(void*, size_t, int);
void* mspace_malloc(dl_mspace, size_t);
void mspace_free(dl_mspace, void*);
void* mspace_realloc(dl_mspace, void*, size_t);
void* mspace_calloc(dl_mspace, size_t, size_t);
size_t mspace_usable_size(const void*);
}  // extern "C"


// Quarantine depth: freed blocks held out of reuse (UAF oracle).
static constexpr std::uint32_t kQuarantineCap = 64;
static constexpr std::uint8_t kQuarantinePoison = 0xABu;


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
    jni_str_next_ = kGuestDataBase + 0x2100;
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
    jni_obj_next_ = kGuestDataBase + 0x3000;  // class/method handle arena
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
    mappings_.push_back({start, start + size, prot});
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

std::uint32_t Runtime::LiveIdxFind(std::uint32_t base) { return live_idx_.Find(base); }

bool Runtime::LiveIdxContaining(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size) {
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
    // Last-hit cache (perf): string/scan loops probe per byte, each probe a
    // linear walk over mappings_. Mappings only ever grow (AddMapping push;
    // GuestMunmap never erases), so a cached by-value extent stays valid
    // forever; thread_local needs no locking. Early-true only: on miss fall
    // through to the full scan (mappings may overlap).
    thread_local std::uint32_t hit_s = 1, hit_e = 0;
    thread_local int hit_p = 0;
    if (addr >= hit_s && end <= hit_e) {
        if (exec) {
            if (hit_p & PROT_EXEC) {
                return true;
            }
        } else if (write) {
            if (hit_p & PROT_WRITE) {
                return true;
            }
        } else if (hit_p & PROT_READ) {
            return true;
        }
    }
    for (const auto& m : mappings_) {
        if (addr >= m.start && end <= m.end) {
            hit_s = m.start;
            hit_e = m.end;
            hit_p = m.prot;
            if (exec) {
                return (m.prot & PROT_EXEC) != 0;
            }
            if (write) {
                return (m.prot & PROT_WRITE) != 0;
            }
            return (m.prot & PROT_READ) != 0;
        }
    }
    return false;
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

std::uint32_t Runtime::GuestMalloc(std::uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    size = (size + 7) & ~std::uint32_t(7);
    if (size == 0) {
        size = 8;
    }
    const std::uint32_t total =
        size + pad;  // pad kept: MALLOCPAD layout probe stays available
    void* hp = heap_ms_ ? mspace_malloc(heap_ms_, total) : nullptr;
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
    if (EnvOn("HARNESS_MALLOCLOG")) {
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
    const std::uint64_t total = std::uint64_t(n) * size;
    if (total == 0 || total > 0x10000000u) {
        return 0;
    }
    // mspace_calloc zeroes itself; keep our size map + telemetry in sync.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
        if (rhd < 30 && addr >= 32 && CheckRange(addr - 32, 32, false)) {
            rhd++;
            const std::uint8_t* hp = H<const std::uint8_t>(addr - 32);
            std::printf("[realloc] pre %08x:", addr);
            for (int i = 0; i < 32; i++) {
                std::printf(" %02x", hp[i]);
            }
            std::printf("\n");
        }
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
    // Real free with quarantine (UAF oracle): the block is poisoned and held
    // out of reuse for kQuarantineCap frees; on release the poison is
    // verified -- a clobbered block names a live use-after-free LOUD.
    // Double-free of a quarantined block is LOUD, not fatal.
    if (!addr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (quarantine_set_.count(addr)) {
            std::uint32_t lr = 0;
            Dynarmic::A32::Jit* aj = ActiveJit();
            if (aj) {
                lr = aj->Regs()[14];
            }
            std::printf("[heap] LOUD double free of quarantined %08x lr=%08x\n", addr, lr);
            return;
    }
    // D2: one hash lookup both yields the size and removes the entry (the old
    // map needed find + erase + a host node free). 0 = "we never handed this
    // out" -> the legacy lenient no-op, counted so a session can show whether
    // that path ever actually fires.
    const std::uint32_t size = LiveIdxErase(addr);
    if (!size) {
        ++hx_free_unknown_;
        if (EnvOn("HARNESS_MALLOCLOG")) {
            std::uint32_t lr = 0;
            Dynarmic::A32::Jit* aj = ActiveJit();
            if (aj) {
                lr = aj->Regs()[14];
            }
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "[mfree] %08x size=%u lr=%08x", addr, 0xFFFFFFFFu,
                          lr);
            RingPush(tmp);
        }
        return;  // free of unknown: ignore (legacy semantics)
    }
    // Drop any audio-pool reference (stale reuse would alias).
    for (auto pit = array_pool_.begin(); pit != array_pool_.end(); ++pit) {
        if (pit->first == addr) {
            array_pool_.erase(pit);
            break;
        }
    }
    // Poison only a validated range: a wild (but map-hit, e.g. post-move
    // stale) addr must never take a host memset/SIGSEGV with it.
    const bool range_ok = size > 0 && size < 0x10000000u && CheckRange(addr, size, true);
    if (!range_ok && size > 0) {
        std::printf("[heap] LOUD free with bad range %08x size=%u -- skipped poison\\n", addr,
                    size);
    }
    quarantine_.push_back({addr, size});
    quarantine_set_.insert(addr);
    if (EnvOn("HARNESS_MALLOCLOG")) {
        std::uint32_t lr = 0;
        Dynarmic::A32::Jit* aj = ActiveJit();
        if (aj) {
            lr = aj->Regs()[14];
        }
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "[mfree] %08x size=%u lr=%08x", addr, size, lr);
        RingPush(tmp);
    }
    // HARNESS_NORELEASE=1: hold everything in quarantine forever (leak like
    // bump, keep dlmalloc layout). Triage switch: if the silent death at
    // fclose/free vanishes with it, the killer is mspace_free detonating
    // pre-smashed headers (writer hunt continues, game runs meanwhile).
    if (EnvOn("HARNESS_NORELEASE")) {
        return;
    }
    // Delay-only quarantine (no poison, no verify: the tripwire never
    // fired in any session, while poisoned stale reads proved fatal).
    while (quarantine_.size() > kQuarantineCap) {
        const QEnt q0 = quarantine_.front();
        g_last_free_addr = q0.addr;
        g_last_free_size = q0.size;
        quarantine_set_.erase(q0.addr);
        bool skip = false;
        // HUNT (2026-10-17): verify the chunk head before handing a
        // quarantined block back to dlmalloc. mspace_free aborted at
        // dlmalloc.c:5720 (`erroraction: USAGE_ERROR_ACTION` -- ok_inuse /
        // ok_next failed) on exactly this path. Dump the zone (the smear
        // content names the writer) and LEAK the block instead of aborting.
        // FIX (2026-10-17, PLAN.md D0): the old test `(head & 3) != 3` was a
        // FALSE POSITIVE for any in-use chunk whose PREDECESSOR is free --
        // set_inuse (dlmalloc.c:3066) ORs CINUSE but PRESERVES PINUSE
        // (PINUSE_BIT=1, CINUSE_BIT=2, dlmalloc.c:2261), so `head & 3 == 2` is
        // perfectly legal. All 12 dumps of the A+B soak had exactly that shape
        // (csize == sz + 8, csize % 8 == 0) => the mitigation was leaking
        // HEALTHY blocks and CHUNKD was useless as a canary. A chunk is suspect
        // only when CINUSE is CLEAR (free / never-allocated -- cannot be a live
        // quarantined block) or the size field is nonsense. Note dlmalloc's
        // chunksize() masks FLAG_BITS (=7), not 3.
        if (heap_ms_ && q0.addr >= 64) {
            const std::uint64_t head = *H<std::uint64_t>(q0.addr - 8);
            const std::uint64_t csize = head & ~std::uint64_t(7);
            if ((head & 0x2) == 0 || csize < 24 || csize > (std::uint64_t)(1u << 24)) {
                skip = true;
                static int dvn = 0;
                if (dvn < 12) {
                    dvn++;
                    std::printf("[heap] CHUNKD addr=%08x sz=%u head=%016llx csize=%llu\n", q0.addr,
                                q0.size, (unsigned long long)head, (unsigned long long)csize);
                    const std::uint8_t* pv = H<const std::uint8_t>(q0.addr - 48);
                    std::printf("[heap] CHUNKD pre:");
                    for (int i = 0; i < 48; i++) {
                        std::printf(" %02x", pv[i]);
                    }
                    const std::uint8_t* pa = H<const std::uint8_t>(q0.addr - 16);
                    std::printf("\n[heap] CHUNKD at :");
                    for (int i = 0; i < 96; i++) {
                        std::printf(" %02x", pa[i]);
                    }
                    std::printf("\n[heap] CHUNKD asc:");
                    for (int i = 0; i < 96; i++) {
                        const unsigned char c = pa[i];
                        std::printf("%c", (c >= 32 && c < 127) ? (char)c : '.');
                    }
                    std::printf("\n");
                }
            }
        }
        if (heap_ms_ && !skip) {
            mspace_free(heap_ms_, H(q0.addr));
        }
        if (heap_used_ >= q0.size) {
            heap_used_ -= q0.size;
        } else {
            heap_used_ = 0;
        }
        quarantine_.erase(quarantine_.begin());
    }
}

std::uint32_t Runtime::GuestMmap(std::uint32_t addr, std::uint32_t size, int prot, int /*flags*/,
                                 int /*fd*/, std::uint32_t /*offset*/) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size = (size + 0xFFFu) & ~std::uint32_t(0xFFFu);
    int hprot = 0;
    if (prot & 1) {
        hprot |= PROT_READ;
    }
    if (prot & 2) {
        hprot |= PROT_WRITE;
    }
    if (prot & 4) {
        hprot |= PROT_EXEC;
    }
    if (addr != 0) {
        // MAP_FIXED-style: must land inside our reservation, outside modules.
        if (addr < 0x10000000u || addr + size > 0xF0000000u) {
            return 0xFFFFFFFFu;
        }
        // Never let a FIXED map shadow the guest heap: it would silently steal
        // the arena's address space (allocations keep "succeeding" while the
        // mapping is clobbered). LOUD once per call site is cheap; refusals are
        // already the failure mode for out-of-range requests above.
        if (addr < heap_base_ + kHeapSize) {
            std::printf("[mmap] LOUD FIXED %08x+%x overlaps heap [%08x,%08x) -- refusing\n", addr,
                        size, heap_base_, heap_base_ + kHeapSize);
            return 0xFFFFFFFFu;
        }
        if (!mem_.Map(addr, size, hprot)) {
            return 0xFFFFFFFFu;
        }
        AddMapping(addr, size, hprot);
        return addr;
    }
    mmap_next_ = (mmap_next_ + 0xFFFFu) & ~std::uint32_t(0xFFFFu);
    if (mmap_next_ + size > 0xF0000000u) {
        return 0xFFFFFFFFu;
    }
    const std::uint32_t out = mmap_next_;
    mmap_next_ += size;
    if (!mem_.Map(out, size, hprot)) {
        return 0xFFFFFFFFu;
    }
    AddMapping(out, size, hprot);
    return out;
}

int Runtime::GuestMunmap(std::uint32_t addr, std::uint32_t size) {
    size = (size + 0xFFFu) & ~std::uint32_t(0xFFFu);
    // Known gap (G3.1, 2026-10-18): this leaves the mappings_ entry behind, so
    // CheckAccess/CheckRange would still allow access to the (now PROT_NONE)
    // range. Unreachable for THIS guest -- the .so imports neither mmap nor
    // munmap (readelf --dyn-syms) -- so it is left as-is: the D3 last-hit cache
    // relies on "mappings only ever grow", and a correct fix would need epoch
    // invalidation. LOUD keeps a future guest that does call it from being
    // silent about a stale mapping.
    static std::atomic<bool> loud{false};
    if (!loud.exchange(true)) {
        std::printf("[mmap] LOUD munmap(%08x+%x) -- mappings_ keeps the old prot (G3.1)\n", addr,
                    size);
    }
    // Don't actually release (arena reuse later); just revoke access.
    if (!mem_.Protect(addr, size, PROT_NONE)) {
        return -1;
    }
    return 0;
}

const char* Runtime::GuestStr(std::uint32_t guest) {
    // Thread-local scratch: worker threads share this Runtime; a shared
    // std::string here corrupted the host heap under concurrency (found via
    // gdb backtrace into unordered_map). Bounded 511+NUL, immediate use only.
    thread_local static char scratch[512];
    std::uint32_t n = 0;
    // Bounded + access-checked: never let a wild guest pointer kill the host.
    for (std::uint32_t i = 0; i < 511; i++) {
        if (!CheckAccess(guest + i, 1, false, false)) {
            break;
        }
        char c = *H<char>(guest + i);
        if (c == '\0') {
            break;
        }
        scratch[n++] = c;
    }
    scratch[n] = '\0';
    return scratch;
}

// Validates a guest range before host-side bulk access. Halts cleanly on violation.
bool Runtime::CheckRange(std::uint32_t guest, std::size_t size, bool write) {
    if (size == 0) {
        return true;
    }
    // Ranges must not wrap and both ends must be in one mapping.
    const std::uint64_t end = std::uint64_t(guest) + size;
    if (end > 0x100000000ULL) {
        return false;
    }
    // D3 (2026-10-17): same last-hit cache as CheckAccess. String/scan loops
    // and JNI pointer validation probe the same mapping over and over, each
    // probe a linear walk over mappings_. Mappings only ever grow (AddMapping
    // pushes, GuestMunmap never erases), so a cached by-value extent stays
    // valid forever; thread_local needs no locking. Semantics unchanged: an
    // early hit returns exactly what the scan would (the scan returns on the
    // FIRST containing mapping, and the cache is only consulted for a range
    // inside that same mapping) -- the one assumption is that a later mapping
    // never lies strictly inside an earlier one, which CheckAccess already
    // assumes for its identical early-true cache. On a miss we fall through to
    // the full scan.
    thread_local std::uint32_t hit_s = 1, hit_e = 0, hit_p = 0;
    if (guest >= hit_s && end <= hit_e) {
        return write ? ((hit_p & PROT_WRITE) != 0) : ((hit_p & PROT_READ) != 0);
    }
    for (const auto& m : mappings_) {
        if (guest >= m.start && end <= m.end) {
            hit_s = m.start;
            hit_e = m.end;
            hit_p = static_cast<std::uint32_t>(m.prot);
            if (write) {
                return (m.prot & PROT_WRITE) != 0;
            }
            return (m.prot & PROT_READ) != 0;
        }
    }
    return false;
}

void Runtime::Ret(Regs& regs, std::uint32_t r0, std::uint32_t r1) {
    regs[0] = r0;
    regs[1] = r1;
    // A guest SVC is an exception boundary: Dynarmic enters ARM state for the
    // handler, so the caller's Thumb state must be restored explicitly here.
    // Rebuild CPSR as: caller's T-bit from LR bit 0, always USER mode
    // (our guest is unprivileged), NZCV/GE preserved from handler time
    // (thunks never intentionally set flags).
    const std::uint32_t lr = regs[14];
    std::uint32_t cpsr = ActiveJit()->Cpsr();
    cpsr = (cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10) |
           ((lr & 1u) ? std::uint32_t(0x20) : std::uint32_t(0));
    ActiveJit()->SetCpsr(cpsr);
    regs[15] = lr & ~std::uint32_t(1);
}

bool Runtime::IsCodeAddress(std::uint32_t va) {
    if (va >= kTrampBase && va < kTrampBase + kTrampSize) {
        return true;
    }
    for (const auto& m : mappings_) {
        if (va >= m.start && va < m.end && (m.prot & PROT_EXEC)) {
            return true;
        }
    }
    return false;
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

void Runtime::OnSvc(std::uint32_t swi, Regs& regs) {
    svc_count.fetch_add(1, std::memory_order_relaxed);
    if (swi == kSwiHalt) {
        halt_count++;
        if (once_hijack_.active) {
            // Hijacked once-routine finished (returned via HALT trampoline).
            // Mark done, restore caller context, resume after the once call.
            std::uint32_t done = 2;
            std::memcpy(H(once_hijack_.control), &done, 4);
            for (int r = 0; r <= 12; r++) {
                regs[r] = once_hijack_.saved_regs[r];
            }
            regs[13] = once_hijack_.saved_regs[13];
            regs[14] = once_hijack_.saved_regs[14];
            regs[0] = 0;  // pthread_once() return value
            regs[15] = once_hijack_.saved_regs[14];
            // The hijack redirected pc without Ret(), leaving handler CPSR
            // (SVC mode). Rebuild exactly like Ret(): USER + caller T-bit from
            // LR, NZCV/GE preserved (thunks never set flags intentionally).
            {
                const std::uint32_t lr = once_hijack_.saved_regs[14];
                std::uint32_t cpsr = ActiveJit()->Cpsr();
                cpsr = (cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10) |
                       ((lr & 1u) ? std::uint32_t(0x20) : std::uint32_t(0));
                ActiveJit()->SetCpsr(cpsr);
            }
            std::printf("[once] routine done, resumed\n");
            once_hijack_.active = false;
            return;  // keep running, do NOT halt
        }
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
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
    CallImport(idx, regs);
}

void Runtime::CallImport(std::uint32_t idx, Regs& regs) {
    const Import& imp = mod_.imports[idx];
    const std::string& n = imp.name;
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    // TEMP-DIAG (worker hang): unthrottled socket-family logging (global
    // budgets hide worker calls). Diagnostic layer (PLAN.md A3): gated; with
    // DIAG off this also stops paying the ~17 string compares per call.
    if (DiagOn() &&
        (n == "socket" || n == "connect" || n == "send" || n == "sendto" || n == "recv" ||
         n == "recvfrom" || n == "bind" || n == "listen" || n == "accept" ||
         n == "getaddrinfo" || n == "freeaddrinfo" || n == "gethostbyname" ||
         n == "setsockopt" || n == "shutdown" || n == "sendmsg" || n == "recvmsg")) {
        std::printf("[netsock][t%u]%s %s(%08x %08x %08x %08x) lr=%08x\n", GuestTid(),
                    GuestTid() == 1 ? "" : "(T2+)", n.c_str(), a0, a1, a2, a3, regs[14]);
    }

    // --- memory management ---
    if (n == "malloc") {
        Ret(regs, GuestMalloc(a0));
        return;
    }
    if (n == "calloc") {
        Ret(regs, GuestCalloc(a0, a1));
        return;
    }
    if (n == "realloc") {
        Ret(regs, GuestRealloc(a0, a1));
        return;
    }
    if (n == "free" || n == "cfree") {
        // HARNESS_EVENTPOST=1: synthetic-event invocation marker. The forged
        // vtable's +0x18 slot points at this thunk; any arg equal to the
        // recorded callable proves the walk invoked our event (arg layout
        // agnostic: all four checked). free itself is a safe no-op.
        if (eventpost_callable_ != 0 &&
            (a0 == eventpost_callable_ || a1 == eventpost_callable_ ||
             a2 == eventpost_callable_ || a3 == eventpost_callable_)) {
            std::printf("[eventpost] INVOKE callable=%08x args=%08x %08x %08x %08x\n",
                        eventpost_callable_, a0, a1, a2, a3);
        }
        GuestFree(a0);
        RetVoid(regs);
        return;
    }
    if (n == "mmap" || n == "mmap64") {
        Ret(regs, GuestMmap(a0, a1, a2, a3, 0, 0));
        return;
    }
    if (n == "munmap") {
        Ret(regs, static_cast<std::uint32_t>(GuestMunmap(a0, a1)));
        return;
    }
    if (n == "strdup" || n == "strndup") {
        const char* s = GuestStr(a0);
        // Triage (2026-10-16): log WHAT gets strdup'd (names the subsystem:
        // Lua chunknames start with @, cert names, filenames). Capped 20.
        {
            static int sdn = 0;
            if (sdn < 20) {
                sdn++;
                std::printf("[strdup] %08x \"%.48s\"\n", a0, s ? s : "?");
            }
        }
        std::size_t len = std::strlen(s);
        if (n == "strndup" && a1 < len) {
            len = a1;
        }
        const std::uint32_t out = GuestMalloc(static_cast<std::uint32_t>(len + 1));
        if (out) {
            std::memcpy(H(out), s, len);
            *H<std::uint8_t>(out + static_cast<std::uint32_t>(len)) = 0;
        }
        Ret(regs, out);
        return;
    }
    // --- raw memory (host implementations over translated pointers) ---
    // Every bulk access is range-checked first: a wild guest pointer must
    // halt cleanly, never segfault the host.
    if (n == "memcpy" || n == "__aeabi_memcpy" || n == "__aeabi_memcpy4" || n == "__aeabi_memcpy8") {
        if (!CheckRange(a0, a2, true) || !CheckRange(a1, a2, false)) {
            goto bad_ptr;
        }
        const std::uint32_t wlen = ClampHeapWrite(a0, a2, n.c_str());
        std::memmove(H(a0), H<const void>(a1), wlen);
        ScanWP(this, a0, wlen, n.c_str());
        Ret(regs, a0);
        return;
    }
    if (n == "memmove" || n == "__aeabi_memmove" || n == "__aeabi_memmove4" ||
        n == "__aeabi_memmove8") {
        if (!CheckRange(a0, a2, true) || !CheckRange(a1, a2, false)) {
            goto bad_ptr;
        }
        const std::uint32_t wlen = ClampHeapWrite(a0, a2, n.c_str());
        std::memmove(H(a0), H<const void>(a1), wlen);
        ScanWP(this, a0, wlen, n.c_str());
        Ret(regs, a0);
        return;
    }
    if (n == "memset" || n == "__aeabi_memset" || n == "__aeabi_memset4" || n == "__aeabi_memset8" ||
        n == "__aeabi_memclr" || n == "__aeabi_memclr4" || n == "__aeabi_memclr8") {
        // ABI SPLIT (wall4 root cause): plain C memset is (dest, c, n) but
        // ARM EABI __aeabi_memset* is (dest, n, c) and __aeabi_memclr* is
        // (dest, n). Swapping them filled 255B with 0x30 instead of 48B with
        // 0xFF, poisoning every Technique/Pass vector header downstream.
        const bool is_clr = n.size() >= 14 && n.compare(0, 14, "__aeabi_memclr") == 0;
        const bool is_aeabi_set =
            !is_clr && n.size() >= 14 && n.compare(0, 14, "__aeabi_memset") == 0;
        const std::uint32_t len = is_clr ? a1 : (is_aeabi_set ? a1 : a2);
        const int fill = is_clr ? 0 : (is_aeabi_set ? (int)(a2 & 0xFF) : (int)(a1 & 0xFF));
        if (!CheckRange(a0, len, true)) {
            goto bad_ptr;
        }
        // TEMP-DIAG (wall4 second filler): log non-clr memset traffic.
        // Diagnostic layer (PLAN.md A4): off in shipping.
        if (DiagOn()) {
            static int mcount = 0;
            if (!is_clr && mcount < 400) {
                mcount++;
                std::uint32_t lr = ActiveJit() ? ActiveJit()->Regs()[14] : 0;
                std::printf("[memset] %s dst=%08x len=%u fill=%02x lr=%08x\n", n.c_str(), a0, len,
                            fill & 0xFF, lr);
            }
        }
        const std::uint32_t wlen = ClampHeapWrite(a0, len, n.c_str());
        std::memset(H(a0), fill, wlen);
        ScanWP(this, a0, wlen, n.c_str());
        Ret(regs, a0);
        return;
    }
    if (n == "memcmp" || n == "bcmp") {
        if (!CheckRange(a0, a2, false) || !CheckRange(a1, a2, false)) {
            goto bad_ptr;
        }
        // TEMP-DIAG (font wall): log len-3 compares to catch the PVR one.
        // Silenced on Android (logd IPC per line taxes the slow JIT loop).
#ifndef __ANDROID__
        if (a2 == 3) {
            const std::uint8_t* p0 = H<const std::uint8_t>(a0);
            const std::uint8_t* p1 = H<const std::uint8_t>(a1);
            std::printf("[memcmp3] %08x: %02x %02x %02x vs %08x: %02x %02x %02x -> %d\n",
                        a0, p0[0], p0[1], p0[2], a1, p1[0], p1[1], p1[2],
                        std::memcmp(p0, p1, 3));
        }
#endif
        Ret(regs, static_cast<std::uint32_t>(std::memcmp(H<const void>(a0), H<const void>(a1), a2)));
        return;
    }
    if (n == "strlen" || n == "strnlen") {
        std::size_t len = std::strlen(GuestStr(a0));
        if (n == "strnlen" && a1 < len) {
            len = a1;
        }
        Ret(regs, static_cast<std::uint32_t>(len));
        return;
    }
    if (n == "strcmp" || n == "strncmp" || n == "strcasecmp" || n == "strncasecmp") {
        // NOTE: two separate buffers — GuestStr reuses one scratch area.
        const std::string sa(GuestStr(a0));
        const std::string sb(GuestStr(a1));
        int r = 0;
        if (n == "strcmp") {
            r = std::strcmp(sa.c_str(), sb.c_str());
        } else if (n == "strncmp") {
            r = std::strncmp(sa.c_str(), sb.c_str(), a2);
        } else if (n == "strcasecmp") {
            r = strcasecmp(sa.c_str(), sb.c_str());
        } else {
            r = strncasecmp(sa.c_str(), sb.c_str(), a2);
        }
        Ret(regs, static_cast<std::uint32_t>(r));
        return;
    }
    if (n == "strcpy" || n == "strncpy" || n == "strcat" || n == "strncat") {
        // Bounded guest strlen first (max 1MB), then range-check both sides.
        auto bounded_len = [&](std::uint32_t s, std::uint32_t cap) -> std::uint32_t {
            for (std::uint32_t i = 0; i < cap; i++) {
                if (!CheckRange(s + i, 1, false)) {
                    return 0xFFFFFFFFu;
                }
                if (*H<const char>(s + i) == '\0') {
                    return i;
                }
            }
            return 0xFFFFFFFFu;
        };
        if (n == "strcpy" || n == "strcat") {
            const std::uint32_t slen = bounded_len(a1, 0x100000u);
            std::uint32_t dlen = 0;
            if (n == "strcat") {
                dlen = bounded_len(a0, 0x100000u);
                if (dlen == 0xFFFFFFFFu) {
                    goto bad_ptr;
                }
            }
            if (slen == 0xFFFFFFFFu || !CheckRange(a0 + dlen, slen + 1, true)) {
                goto bad_ptr;
            }
            // Overflow firewall (same class as memcpy): clamp to the mapped
            // allocation; force NUL on truncation so downstream stays bounded.
            const std::uint32_t wlen = ClampHeapWrite(a0 + dlen, slen + 1, n.c_str());
            if (wlen <= slen) {
                if (wlen > 0) {
                    std::memcpy(H(a0 + dlen), H<const void>(a1), wlen - 1);
                    *H<char>(a0 + dlen + wlen - 1) = '\0';
                } else if (CheckRange(a0 + dlen, 1, true)) {
                    *H<char>(a0 + dlen) = '\0';  // zero room: empty string, still terminated
                }
            } else if (n == "strcpy") {
                std::strcpy(reinterpret_cast<char*>(H(a0)), GuestStr(a1));
            } else {
                std::strcat(reinterpret_cast<char*>(H(a0)), GuestStr(a1));
            }
        } else {
            if (!CheckRange(a0, a2 + (n == "strncpy" ? 0 : 1), true) ||
                !CheckRange(a1, a2, false)) {
                goto bad_ptr;
            }
            const std::uint32_t wlen = ClampHeapWrite(a0, a2 + (n == "strncpy" ? 0 : 1),
                                                      n.c_str());
            if (n == "strncpy") {
                const std::uint32_t wn = wlen < a2 ? wlen : a2;
                std::strncpy(reinterpret_cast<char*>(H(a0)), GuestStr(a1), wn);
                if (wn < a2 && wn > 0) {
                    H(a0)[wn - 1] = 0;  // truncated: terminate (strncpy wouldn't)
                }
            } else {
                if (wlen == 0) {
                    Ret(regs, a0);  // zero room: skip (strncat would write NUL past end)
                    return;
                }
                std::strncat(reinterpret_cast<char*>(H(a0)), GuestStr(a1),
                             wlen < a2 + 1 ? wlen - 1 : a2);
            }
        }
        Ret(regs, a0);
        return;
    }
    // --- string search/scan family (host-forwarded, range-checked) ---
    if (n == "memchr" || n == "memrchr") {
        if (!CheckRange(a0, a2, false)) {
            goto bad_ptr;
        }
        // HARNESS_MEMDUMP=<hexlr>: hex+ascii dump of the scanned buffer when
        // the caller's lr matches (JSON-parse forensics: input lives on the
        // heap and drifts between runs due to worker-thread racing, so
        // post-mortem dumps are unreliable — capture at call time).
        if (const char* mdl = std::getenv("HARNESS_MEMDUMP")) {
            std::uint32_t mlr = static_cast<std::uint32_t>(std::strtoul(mdl, nullptr, 16));
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
        const char* f = (n == "memchr")
                            ? static_cast<const char*>(std::memchr(p, a1 & 0xFF, a2))
                            : nullptr;
        if (n == "memrchr") {
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
    if (n == "strchr" || n == "strrchr" || n == "strstr" || n == "strpbrk" || n == "strspn" ||
        n == "strcspn") {
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
        if (n == "strchr" || n == "strrchr") {
            const char c = static_cast<char>(a1 & 0xFF);
            f = (n == "strchr") ? std::strchr(h, c) : std::strrchr(h, c);
        } else {
            const std::uint32_t nl = gstrlen(a1);
            if (nl == 0xFFFFFFFFu) {
                goto bad_ptr;
            }
            const char* nd = H<const char>(a1);
            if (n == "strstr") {
                f = std::strstr(h, nd);
            } else if (n == "strpbrk") {
                f = std::strpbrk(h, nd);
            } else if (n == "strspn") {
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
    if (n == "strtok" || n == "strtok_r") {
        // Manual strtok on guest memory (host strtok_r can't use guest saveptr:
        // it stores host pointers). Delim = NUL-terminated guest string.
        // thread_local: legacy strtok state is per-thread (a global tore
        // tokens across main/worker and crashed with wild pointers).
        thread_local std::uint32_t strtok_state = 0;
        std::uint32_t state = 0;
        if (n == "strtok_r") {
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
            if (n == "strtok_r" && state == 0) {
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
                if (n == "strtok_r") {
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
                if (n == "strtok_r") {
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
                if (n == "strtok_r") {
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
    if (n == "sscanf" || n == "vsscanf") {
        // Manual sscanf (host va_list can't be synthesized; semantics follow
        // C99: ws-skip (not %c/%[/%n), literals, widths, suppression,
        // lengths hh/h/l/ll, conversions d i u o x X f e g a s c [ p n %).
        // Guest long = 32-bit (narrow host longs); long double = double.
        if (!CheckRange(a0, 1, false) || !CheckRange(a1, 1, false)) {
            goto bad_ptr;
        }
        const std::string inp(GuestStr(a0));
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
        auto next_ptr = [&]() -> std::uint32_t {
            std::uint32_t v = 0;
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
            int len = 0;  // 0 none, 1 hh, 2 h, 3 l, 4 ll/j/z/t, 5 L
            if (fi < fmt.size() && (fmt[fi] == 'h' || fmt[fi] == 'l' || fmt[fi] == 'L' ||
                                    fmt[fi] == 'j' || fmt[fi] == 'z' || fmt[fi] == 't')) {
                char lc = fmt[fi++];
                if (lc == 'h' && fi < fmt.size() && fmt[fi] == 'h') {
                    fi++;
                    len = 1;
                } else if (lc == 'h') {
                    len = 2;
                } else if (lc == 'l' && fi < fmt.size() && fmt[fi] == 'l') {
                    fi++;
                    len = 4;
                } else if (lc == 'l') {
                    len = 3;
                } else if (lc == 'L') {
                    len = 5;
                } else {
                    len = 4;
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
                        unsigned long long uv = 0;
                        try {
                            uv = std::stoull(
                                (tok[0] == '+' || tok[0] == '-') ? tok.substr(1) : tok, nullptr,
                                base == 0 ? 10 : base);
                        } catch (...) {
                            fail = true;
                            break;
                        }
                        long long sv = neg ? -(long long)uv : (long long)uv;
                        bool is_signed = (conv == 'd' || conv == 'i');
                        if (len == 4 || (len == 3)) {
                            // guest long = 32-bit: narrow.
                            std::int32_t w32 = is_signed ? (std::int32_t)sv : (std::int32_t)uv;
                            if (dp && ClampHeapWrite(dp, 4, "sscanf-num") >= 4 && CheckRange(dp, 4, true)) {
                                std::memcpy(H(dp), &w32, 4);
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
                        } else if (len == 0 && !is_signed && conv == 'p') {
                            std::int32_t w32 = (std::int32_t)uv;
                            if (dp && ClampHeapWrite(dp, 4, "sscanf-num") >= 4 && CheckRange(dp, 4, true)) {
                                std::memcpy(H(dp), &w32, 4);
                                }
                        } else if (len == 0) {
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
                        } else {  // 64-bit (ll/j/z/t): same size both ABIs.
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
                        if (len == 3 || len == 4 || len == 5) {
                            // %lf (guest long double == double, 8B).
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
        // assigned = number of successful conversions (EOF => -1).
        Ret(regs, eof ? static_cast<std::uint32_t>(-1)
                      : static_cast<std::uint32_t>(assigned));
        return;
    }
    if (n == "strtol" || n == "strtoul" || n == "strtoll" || n == "strtoull" || n == "atoi" ||
        n == "atol" || n == "atoll") {
        // Guest ABI: strtoX(nptr=a0, endptr=a1, base=a2); atoi/atol/atoll take
        // only a0. Guest `long` is 32-bit (host long is 64-bit) — narrow results.
        // NOTE: GuestStr caps at 512 chars; numeric prefixes are short, fine.
        if (!CheckRange(a0, 1, false)) {
            goto bad_ptr;
        }
        const std::string s(GuestStr(a0));
        char* end = nullptr;
        if (n == "atoi") {
            Ret(regs, static_cast<std::uint32_t>(std::atoi(s.c_str())));
            return;
        }
        if (n == "atol") {
            const std::int32_t v = static_cast<std::int32_t>(std::atol(s.c_str()));
            Ret(regs, static_cast<std::uint32_t>(v));
            return;
        }
        if (n == "atoll") {
            const long long v = std::atoll(s.c_str());
            Ret(regs, static_cast<std::uint32_t>(v & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((std::uint64_t(v) >> 32) & 0xFFFFFFFFu));
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
        if (n == "strtoll" || n == "strtoull") {
            const unsigned long long v = (n == "strtoull") ? std::strtoull(s.c_str(), &end, base)
                                                           : (unsigned long long)std::strtoll(
                                                                 s.c_str(), &end, base);
            if (!store_end(a1)) {
                goto bad_ptr;
            }
            Ret(regs, static_cast<std::uint32_t>(v & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
        } else {
            const std::int32_t v = (n == "strtoul")
                                       ? static_cast<std::int32_t>(std::strtoul(s.c_str(), &end,
                                                                               base))
                                       : static_cast<std::int32_t>(std::strtol(s.c_str(), &end,
                                                                              base));
            if (!store_end(a1)) {
                goto bad_ptr;
            }
            Ret(regs, static_cast<std::uint32_t>(v));
        }
        return;
    }
    if (n == "__errno") {
        Ret(regs, ErrnoSlot());
        return;
    }
    // --- time (32-bit guest layouts: two u32 words, NOT host structs) ---
    // HARNESS_TIMEWARP=<sec>: shift all guest clocks forward (timer-gate
    // tests: loading/menu timeouts expire without wall waiting).
    if (n == "clock_gettime") {
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
    if (n == "gettimeofday") {
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
    if (n == "time" || n == "clock") {
        std::time_t t = (n == "time") ? std::time(nullptr) : std::clock();
        if (n == "time") {
            t += (time_t)TimeWarp();
        }
        if (n == "time" && a0 != 0 && CheckRange(a0, 4, true)) {
            const std::uint32_t w = static_cast<std::uint32_t>(t & 0xFFFFFFFFu);
            std::memcpy(H(a0), &w, 4);
        }
        Ret(regs, static_cast<std::uint32_t>(t & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(((std::uint64_t)t >> 32) & 0xFFFFFFFFu));
        return;
    }
    if (n == "localtime_r" || n == "gmtime_r" || n == "localtime" || n == "gmtime") {
        // Bionic ARM struct tm (44B): 9x int + long gmtoff + char* zone.
        // localtime(time_t*) uses a static buffer (single-thread caveat:
        // game uses _r here); gmtime same with UTC.
        const bool is_local = (n[0] == 'l');
        const bool use_static = (n == "localtime" || n == "gmtime");
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
            out = GuestMalloc(44);
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
    if (n == "__android_log_print") {
        // Expand guest printf-style args (ARM APCS: r0-r3 then stack) so the
        // game's own diagnostics (e.g. "Parsing JSON failed! Reason: ...")
        // become visible. Supports %s %d %i %u %x %X %p %c %% (+l ignored).
        const std::string fmt(GuestStr(a2));  // copy: later GuestStr calls reuse scratch
        std::uint32_t gargs[4] = {a0, a1, a2, a3};
        std::uint32_t ai = 3;  // next arg: r3 first (a0-a2 consumed: prio,tag,fmt)
        std::uint32_t stack = regs[13];
        std::string out;
        for (const char* p = fmt.c_str(); *p && out.size() < 512; p++) {
            if (*p != '%') {
                out += *p;
                continue;
            }
            p++;
            if (*p == '%') {
                out += '%';
                continue;
            }
            if (*p == 'l') {
                p++;  // ignore length modifier
            }
            auto next_arg = [&]() -> std::uint32_t {
                std::uint32_t v = 0;
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
            char tmp[64];
            switch (*p) {
            case 's': {
                std::uint32_t s = next_arg();
                out += GuestStr(s);
                break;
            }
            case 'd':
            case 'i':
                std::snprintf(tmp, sizeof(tmp), "%d", (int)next_arg());
                out += tmp;
                break;
            case 'u':
                std::snprintf(tmp, sizeof(tmp), "%u", next_arg());
                out += tmp;
                break;
            case 'x':
            case 'X':
            case 'p':
                std::snprintf(tmp, sizeof(tmp), "%x", next_arg());
                out += tmp;
                break;
            case 'c':
                out += (char)(next_arg() & 0xFF);
                break;
            default:
                out += '%';
                out += *p ? *p : '%';
                break;
            }
        }
        std::printf("[glog] %s\n", out.c_str());
        Ret(regs, 0);
        return;
    }
    // --- sleeps: real but capped, so delay loops can't hang bring-up ---
    if (n == "nanosleep") {
        std::uint32_t sec = 0, nsec = 0;
        if (a0) {
            std::memcpy(&sec, H(a0), 4);
            std::memcpy(&nsec, H(a0 + 4), 4);
        }
        std::uint64_t us = std::uint64_t(sec) * 1000000u + nsec / 1000u;
        if (us > 5000u) {
            us = 5000u;
        }
        if (us > 0) {
            usleep(static_cast<useconds_t>(us));
        }
        if (a1) {
            const std::uint32_t z = 0;
            std::memcpy(H(a1), &z, 4);
            std::memcpy(H(a1 + 4), &z, 4);
        }
        Ret(regs, 0);
        return;
    }
    if (n == "usleep" || n == "sleep") {
        std::uint64_t us = (n == "sleep") ? std::uint64_t(a0) * 1000000u : a0;
        if (us > 5000u) {
            us = 5000u;
        }
        if (us > 0) {
            usleep(static_cast<useconds_t>(us));
        }
        Ret(regs, 0);
        return;
    }
    // --- loud exits ---
    if (n == "abort" || n == "__stack_chk_fail" || n == "__assert2" || n == "exit" || n == "_exit") {
        std::printf("[svc] FATAL %s() called (r0=%08x) pc=%08x lr=%08x -- halting\n", n.c_str(), a0,
                    regs[15] - 4, regs[14]);
        DumpEventRing();
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined3);
        return;
    }
    // --- pthread: real mutex/cond/key maps, create/join stubbed for now ---
    if (n == "pthread_mutex_init" || n == "pthread_mutex_destroy" || n == "pthread_mutex_lock" ||
        n == "pthread_mutex_unlock" || n == "pthread_mutex_trylock") {
        Ret(regs, PthreadMutex(n, a0));
        return;
    }
    if (n == "pthread_rwlock_init" || n == "pthread_rwlock_destroy" || n == "pthread_rwlock_rdlock" ||
        n == "pthread_rwlock_wrlock" || n == "pthread_rwlock_unlock" ||
        n == "pthread_rwlock_tryrdlock" || n == "pthread_rwlock_trywrlock") {
        Ret(regs, PthreadRwlock(n, a0));
        return;
    }
    if (n == "pthread_cond_init" || n == "pthread_cond_destroy" || n == "pthread_cond_signal" ||
        n == "pthread_cond_broadcast" || n == "pthread_cond_wait" ||
        n == "pthread_cond_timedwait" || n == "pthread_cond_timedwait_monotonic") {
        std::string base = (n == "pthread_cond_timedwait_monotonic") ? "pthread_cond_timedwait" : n;
        const bool mono = (n == "pthread_cond_timedwait_monotonic");
        Ret(regs, PthreadCond(base, a0, a1, a2, mono));
        return;
    }
    if (n == "pthread_key_create") {
        Ret(regs, PthreadKeyCreate(a0, a1));
        return;
    }
    if (n == "pthread_getspecific") {
        Ret(regs, PthreadGetSpecific(a0));
        return;
    }
    if (n == "pthread_setspecific") {
        Ret(regs, PthreadSetSpecific(a0, a1));
        return;
    }
    if (n == "pthread_self" || n == "pthread_gettid_np") {
        Ret(regs, GuestTid());
        return;
    }
    if (n == "pthread_equal") {
        Ret(regs, a0 == a1 ? 1 : 0);
        return;
    }
    if (n == "pthread_create") {
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
        {
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
            if (DiagOn()) {
                std::printf("[thread] create start=%08x arg=%08x job=\"%s\" url=\"%s\"\n", a2, a3,
                            jn.c_str(), surl.c_str());
            }
        }
        Ret(regs, PthreadCreate(a0, a1, a2, a3));
        return;
    }
    if (n == "pthread_join") {
        Ret(regs, PthreadJoin(a0, a1));
        return;
    }
    if (n == "pthread_detach") {
        Ret(regs, PthreadDetach(a0));
        return;
    }
    if (n == "sched_yield") {
        std::this_thread::yield();
        Ret(regs, 0);
        return;
    }
    if (n == "poll") {
        // struct pollfd {int fd; short events; short revents} = 8B, same both
        // sides. Translate guest fds to host; unknown fds (unstubbed sockets)
        // get POLLNVAL. Timeout capped so bring-up can't hang (5s).
        if (!CheckRange(a0, (std::size_t)a1 * 8, true)) {
            Ret(regs, static_cast<std::uint32_t>(-1));
            return;
        }
        std::vector<struct pollfd> pfds;
        pfds.reserve(a1);
        for (std::uint32_t i = 0; i < a1; i++) {
            std::int32_t gfd = 0;
            std::int16_t ev = 0;
            std::memcpy(&gfd, H(a0 + i * 8), 4);
            std::memcpy(&ev, H(a0 + i * 8 + 4), 2);
            struct pollfd p {};
            p.events = ev;
            if (gfd < 0) {
                p.fd = -1;
            } else if (gfd <= 2) {
                p.fd = gfd;
            } else {
                const auto it = fd_map_.find((std::uint32_t)gfd);
                if (it == fd_map_.end()) {
                    p.fd = -1;
                } else {
                    p.fd = it->second;
                }
            }
            pfds.push_back(p);
        }
        int to = (int)a2;
        if (to < 0 || to > 5000) {
            to = (to < 0) ? 5000 : to;
        }
        if (DiagOn()) {  // PLAN.md A3/B: off => no per-poll fstat+snprintf+log
            static std::uint64_t poll_hits = 0;
            if (++poll_hits <= 400) {
                std::string ds;
                for (std::size_t i = 0; i < pfds.size() && i < 4; i++) {
                    char b[48];
                    std::int32_t gfd = -99;
                    std::memcpy(&gfd, H(a0 + i * 8), 4);
                    std::int16_t ev = 0;
                    std::memcpy(&ev, H(a0 + i * 8 + 4), 2);
                    char ft = '?';
                    if (pfds[i].fd >= 0) {
                        struct stat fs {};
                        if (::fstat(pfds[i].fd, &fs) == 0) {
                            ft = S_ISSOCK(fs.st_mode) ? 'S'
                                 : S_ISFIFO(fs.st_mode) ? 'P'
                                 : S_ISREG(fs.st_mode)  ? 'F'
                                                        : 'o';
                        }
                    }
                    std::snprintf(b, sizeof(b), "g%d->h%d%c:%04x ", gfd, pfds[i].fd, ft,
                                  (unsigned)ev & 0xFFFF);
                    ds += b;
                }
                std::printf("[poll][t%u]%s nfds=%u to=%d fds=[%s]%s\n", GuestTid(),
                            GuestTid() == 1 ? "" : "(T2+)", a1, (int)a2, ds.c_str(),
                            poll_hits == 5 ? " (quiet henceforth)" : "");
            }
        }
        int r = ::poll(pfds.data(), (nfds_t)pfds.size(), to);
        // TEMP-EXP (HARNESS_POLLFAIL=1): fail-fast synthesis for starved
        // dispatcher waits. A single-fd worker poll with >=10 straight zero
        // results AFTER sessions succeeded gets NVAL lied from then on
        // (device delivers fast network failure -> graceful; we hang).
        // Paced 100ms, no fd destruction (writer-safe, reversible).
        // HARNESS_POLLFAIL=err: return -1/EINTR instead (different reader
        // branch; NVAL proved swallowed).
        // HARNESS_PIPEWAKE=1: instead write a 1B wake (0x00) to the paired
        // write end (guest G+1, verified FIFO) when streak hits 10, re-arming
        // (mimics observed main/worker 1B wakes; may advance the job to its
        // next step, e.g. recv -> EAGAIN -> graceful).
        {
            static int pfmode = -1;
            static int pwake = -1;
            if (pfmode < 0) {
                pfmode = 0;
                pwake = 0;
                if (const char* e = std::getenv("HARNESS_POLLFAIL")) {
                    pfmode = (!std::strcmp(e, "err")) ? 2 : 1;
                }
                if (std::getenv("HARNESS_PIPEWAKE")) {
                    pwake = 1;
                }
            }
            extern bool g_succ_done_hack;
            static std::unordered_map<std::uint32_t, int> streak;
            static std::unordered_map<std::uint32_t, char> lying;
            if ((pfmode || pwake) && a1 == 1 && g_succ_done_hack) {
                std::int32_t gfd0 = -99;
                std::memcpy(&gfd0, H(a0), 4);
                const std::uint32_t ug = (std::uint32_t)gfd0;
                if (gfd0 > 2 && lying.find(ug) == lying.end() && streak.size() < 64) {                    if (r == 0) {
                        if (++streak[ug] == 10) {
                            if (pwake) {
                                // Wake via paired write end (guest G+1).
                                int wh = -1;
                                {
                                    std::lock_guard<std::mutex> io(io_mutex_);
                                    const auto wit = fd_map_.find(ug + 1);
                                    if (wit != fd_map_.end()) {
                                        wh = wit->second;
                                    }
                                }
                                struct stat wfs {};
                                if (wh >= 0 && ::fstat(wh, &wfs) == 0 &&
                                    S_ISFIFO(wfs.st_mode)) {
                                    const std::uint8_t wb = 0;
                                    if (::write(wh, &wb, 1) == 1) {
                                        std::printf("[pipewake] 1B -> g%d (h%d)\n", gfd0 + 1,
                                                    wh);
                                    }
                                }
                                streak[ug] = 0;  // re-arm
                            } else {
                                std::printf("[pollfail] %s starved g%d\n",
                                            pfmode == 2 ? "ERR-lie" : "NVAL-lie", gfd0);
                                lying[ug] = 1;
                            }
                        }
                    } else {
                        streak.erase(ug);
                    }
                }
                if (gfd0 > 2 && lying.find(ug) != lying.end()) {
                    ::usleep(100000);
                    if (pfmode == 2) {
                        const std::uint32_t es = ErrnoSlot();
                        if (CheckRange(es, 4, true)) {
                            const int ee = EINTR;
                            std::memcpy(H(es), &ee, 4);
                        }
                        r = -1;
                    } else {
                        r = 1;
                        for (auto& p : pfds) {
                            p.revents = (int16_t)(POLLNVAL | POLLERR);
                        }
                    }
                }
            }
        }
        // TEMP-DIAG (worker hang): log which fd fires on nonzero returns.
        // Gated default-off (was uncapped: fires on every wake, 100s/boot).
        if (r > 0 && EnvOn("HARNESS_POLLDBG")) {
            std::string ds;
            for (std::size_t i = 0; i < pfds.size() && i < 8; i++) {
                std::int32_t gfd = -99;
                std::memcpy(&gfd, H(a0 + i * 8), 4);
                char b[64];
                std::snprintf(b, sizeof(b), "g%d->h%d:ev%04x ", gfd, pfds[i].fd, pfds[i].revents);
                ds += b;
            }
            std::printf("[poll-event] r=%d %s\n", r, ds.c_str());
        }
        // TEMP-DIAG (poll-ret): was an unconditional write(2) per poll —
        // a raw syscall on the hottest path. Gated default-off.
        if (EnvOn("HARNESS_POLLDBG")) {
            char b[64];
            int n = std::snprintf(b, sizeof(b), "[poll-ret] %d\n", r);
            (void)write(STDERR_FILENO, b, (std::size_t)(n > 0 ? n : 0));
        }
        {
            static std::uint64_t pollret_hits = 0;
            if (++pollret_hits <= 5) {
                std::printf("[poll] -> %d%s\n", r,
                            pollret_hits == 5 ? " (quiet henceforth)" : "");
            }
        }
        for (std::size_t i = 0; i < pfds.size(); i++) {
            std::int16_t re = pfds[i].revents;
            if (pfds[i].fd < 0) {
                re = (std::int16_t)(POLLNVAL | POLLERR);
            }
            std::memcpy(H(a0 + i * 8 + 6), &re, 2);
        }
        Ret(regs, static_cast<std::uint32_t>(r < 0 ? -1 : r));
        return;
    }
    if (n == "pthread_once") {
        if (PthreadOnce(a0, a1, regs)) {
            return;  // hijacked: control redirected, no Ret()
        }
        Ret(regs, 0);
        return;
    }
    if (n == "__gnu_Unwind_Find_exidx") {
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
            // TEMP-DIAG (spiral root cause): dump R7 + the 6 words the SU16
            // pop-frame reads ([R7-12..R7+8]): stale self-ref vs valid frame?
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
        // HARNESS_SKIPFRAME=1: flip-test (§87, refined §89): the walk sticks
        // at frame 0x45A758 (queried as pc=0x45A778) but live R7 is getInfo's
        // frame (SP=R7-0x20 + [R7+4]=0x45AAFB constant PROVES it) — pc and R7
        // come from DIFFERENT frames, so descriptors pop the wrong words.
        // Answer for the LIVE frame instead (lr&~1 = 0x45AF66, getInfo body):
        // descriptors then match live R7 and the walk should advance through
        // real frames (0x45AAFB -> 0x45AAC0 -> ...). No guest state touched.
        // Binary: new pcs/handlers/terminate vs same loop. Harness-only.
        std::uint32_t ask_pc = a0;
        if (EnvOn("HARNESS_SKIPFRAME") && a0 == 0x45A778u) {
            ask_pc = regs[14] & ~std::uint32_t(1);
            std::printf("[skipframe] pc=0045a778 live-lr=%08x -> answering for %08x\n",
                        regs[14], ask_pc);
            // Caller hunt: the re-invoker leaves return addrs on stack.
            // Dump first code-range words in [sp,sp+512) (outer frames).
            static int skdump = 0;
            if (skdump < 2) {
                skdump++;
                std::printf("[skipframe] stack:");
                int shown = 0;
                for (std::uint32_t a = regs[13]; a < regs[13] + 512 && shown < 8; a += 4) {
                    if (!CheckRange(a, 4, false)) {
                        break;
                    }
                    std::uint32_t w = 0;
                    std::memcpy(&w, H<const std::uint8_t>(a), 4);
                    if (w >= 0x1000 && w < 0x653D1Cu) {
                        std::printf(" %08x", w);
                        shown++;
                    }
                }
                std::printf("\n");
            }
        }
        // HARNESS_R7POKE=1: flip-test (§79): the raise driver's frame #1
        // has a stale saved-LR ([R7+8] reads heap, never 0x457FE7), so
        // pop re-presents pc=0x45A778 forever. Poke the LR slot with the
        // true return (BL @0x457FE2 -> 0x457FE6|1) on the first queries
        // and watch whether the walk advances past frame #1.
        // Harness-only diag, never ships.
        if (EnvOn("HARNESS_R7POKE")) {
            static int poke_n = 0;
            const std::uint32_t r7v = regs[7];
            if (poke_n < 3 && a0 == 0x45A778u && r7v != 0 && r7v != 0xFFFFFFFFu &&
                CheckRange(r7v + 8, 4, true)) {
                std::uint32_t old = 0;
                std::memcpy(&old, H<const std::uint8_t>(r7v + 8), 4);
                const std::uint32_t fix = 0x457FE7u;
                std::memcpy(H(r7v + 8), &fix, 4);
                std::printf("[r7poke] #%d R7=%08x [R7+8] %08x -> %08x\n", poke_n, r7v,
                            old, fix);
                poke_n++;
            }
        }
        Ret(regs, FindExidx(ask_pc));
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
    {
        auto map_path = [&](std::uint32_t gp) -> std::string {
            const std::string g(GuestStr(gp));
            if (g == "/dev/null" || g == "/dev/zero" || g == "/dev/urandom") {
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
            // Same app dir via the /data/user/<N> alias (real getFilesDir on
            // device returns /data/user/0/... while our prefix is /data/data).
            // Match on the package tail so both spellings land in files/.
            {
                const std::string tail = "/com.rovio.angrybirdsrio/files";
                const std::size_t at = g.find(tail);
                if (at != std::string::npos && !files_hostbase_.empty()) {
                    return files_hostbase_ + g.substr(at + tail.size());
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
        };
        auto need_host = [&](const std::string& h, const char* what) -> bool {
            if (!h.empty()) {
                return true;
            }
            std::printf("[files] LOUD %s: unmapped guest path -- halting\n", what);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return false;
        };
        auto ensure_parent = [&](const std::string& hp) {
            const std::size_t slash = hp.rfind('/');
            if (slash != std::string::npos && slash > 0) {
                std::error_code ec;
                std::filesystem::create_directories(hp.substr(0, slash), ec);
            }
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
        // Bind a host FILE*: allocate a guest fd, expose it at Bionic __sFile
        // offset +14 (_file, ldrsh-loaded inline by guest code for fsync etc.)
        // and in fd_map_ for fd-based thunks. Returns false on exhaustion.
        auto bind_file = [&](std::uint32_t h, FILE* f) -> bool {
            std::uint32_t g = 0;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                g = next_guest_fd_++;
                if (g >= 0x8000) {
                    static std::atomic<bool> warned{false};
                    if (!warned.exchange(true)) {
                        std::printf("[files] LOUD guest fd table exhausted (cap %u)\n", 0x8000u);
                    }
                    return false;
                }
                fd_map_[g] = fileno(f);
                file_map_[h].f = f;
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
            if (next_guest_fd_ >= 0x8000u) {
                // Same ceiling as bind_file: guest fd numbers must stay
                // representable (a FILE* carries its fd in a 16-bit _file
                // field). LOUD once instead of silently truncating; the callers
                // pass the value straight to the guest, where -1 reads as a
                // plain "call failed" (no silent aliasing).
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true)) {
                    std::printf("[files] LOUD guest fd table exhausted (cap %u)\n", 0x8000u);
                }
                return 0xFFFFFFFFu;
            }
            const std::uint32_t g = next_guest_fd_++;
            fd_map_[g] = hfd;
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
        auto wait_fd = [&](int hfd, short ev, int ms) -> int {
            struct pollfd p {};
            p.fd = hfd;
            p.events = ev;
            return ::poll(&p, 1, ms);
        };
        auto fill_stat = [&](std::uint32_t dst, const struct stat& st) -> bool {
            if (!CheckRange(dst, 96, true)) {
                return false;
            }
            std::uint8_t* p = H<std::uint8_t>(dst);
            std::memset(p, 0, 96);
            auto w64 = [&](std::size_t o, std::uint64_t v) { std::memcpy(p + o, &v, 8); };
            auto w32 = [&](std::size_t o, std::uint32_t v) { std::memcpy(p + o, &v, 4); };
            w64(0, (std::uint64_t)st.st_dev);
            w32(12, (std::uint32_t)st.st_ino);
            w32(16, (std::uint32_t)st.st_mode);
            w32(20, (std::uint32_t)st.st_nlink);
            w32(24, (std::uint32_t)st.st_uid);
            w32(28, (std::uint32_t)st.st_gid);
            w64(32, (std::uint64_t)st.st_rdev);
            w64(44, (std::uint64_t)st.st_size);
            w32(52, (std::uint32_t)st.st_blksize);
            w64(56, (std::uint64_t)st.st_blocks);
            w32(64, (std::uint32_t)st.st_atime);
            w32(68, (std::uint32_t)0);
            w32(72, (std::uint32_t)st.st_mtime);
            w32(76, (std::uint32_t)0);
            w32(80, (std::uint32_t)st.st_ctime);
            w32(84, (std::uint32_t)0);
            w64(88, (std::uint64_t)st.st_ino);
            return true;
        };
        if (n == "fopen") {
            const std::string hp = map_path(a0);
            if (!need_host(hp, "fopen")) {
                return;
            }
            const std::string mode = GuestStr(a1);
            if (mode.find_first_of("wax+") != std::string::npos) {
                ensure_parent(hp);
            }
            FILE* f = std::fopen(hp.c_str(), mode.c_str());
            if (!f) {
                std::printf("[files] fopen(\"%s\",\"%s\") -> NULL (errno %d)\n", hp.c_str(),
                            mode.c_str(), errno);
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
                Ret(regs, 0);
                return;
            }
            if (DiagOn()) {
                std::printf("[files][t%u] fopen(\"%s\",\"%s\") -> %08x\n", GuestTid(), hp.c_str(),
                            mode.c_str(), h);
            }
            // TEMP-DIAG (registry ParseError): track registry handles to dump
            // their exact bytes on fread/fwrite below.
            if (hp.find("registry") != std::string::npos) {
                std::lock_guard<std::mutex> io(io_mutex_);
                g_regfiles.insert(f);
            }
            Ret(regs, h);
            return;
        }
        if (n == "fclose" || n == "fflush") {
            FILE* f = file_of(a0);
            if (!f) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            int r = (n == "fclose") ? std::fclose(f) : std::fflush(f);
            if (n == "fclose") {
                std::lock_guard<std::mutex> io(io_mutex_);
                file_map_.erase(a0);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "fread") {
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
            if (wn < a2) {
                static int frc = 0;
                if (frc < 10) {
                    frc++;
                    std::printf("[heap] LOUD fread short items=%u want=%u\n", wn, a2);
                }
            }
            // TEMP-DIAG (registry ParseError): dump exact bytes read.
            bool regdump = false;
            {
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
                std::printf("[regio] fread #%d %u items(%uB):", g_regdump_n, got, (unsigned)(got * a1));
                for (std::size_t i = 0; i < nb; i++) {
                    std::printf(" %02x", p[i]);
                }
                std::printf("\n");
            }
            Ret(regs, static_cast<std::uint32_t>(got));
            return;
        }
        if (n == "fwrite") {
            // fwrite(ptr=a0, size=a1, nmemb=a2, stream=a3): handle is LAST
            // (same a0-keying bug as fread had — fixed together).
            FILE* f = file_of(a3);
            if (!f || a1 == 0 || a2 == 0 || !CheckRange(a0, (std::size_t)a1 * a2, false)) {
                Ret(regs, 0);
                return;
            }
            // TEMP-DIAG (registry ParseError): dump exact bytes written.
            bool regdump = false;
            {
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
            Ret(regs, static_cast<std::uint32_t>(std::fwrite(H(a0), a1, a2, f)));
            return;
        }
        if (n == "fseek" || n == "fseeko" || n == "ftell" || n == "ftello" || n == "rewind") {
            FILE* f = file_of(a0);
            if (!f) {
                Ret(regs, (n == "fseek" || n == "fseeko") ? static_cast<std::uint32_t>(-1) : 0u);
                return;
            }
            if (n == "rewind") {
                std::rewind(f);
                Ret(regs, 0);
                return;
            }
            if (n == "ftell" || n == "ftello") {
                Ret(regs, static_cast<std::uint32_t>(std::ftell(f)));
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(std::fseek(f, (long)a1, (int)a2)));
            return;
        }
        if (n == "feof" || n == "ferror" || n == "clearerr" || n == "fileno") {
            FILE* f = file_of(a0);
            if (!f) {
                Ret(regs, (n == "fileno") ? static_cast<std::uint32_t>(-1) : 0u);
                return;
            }
            if (n == "feof") {
                Ret(regs, (std::uint32_t)std::feof(f));
            } else if (n == "ferror") {
                Ret(regs, (std::uint32_t)std::ferror(f));
            } else if (n == "clearerr") {
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
        if (n == "fgets" || n == "fputs" || n == "fputc" || n == "getc") {
            if (n == "fgets") {
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
            FILE* f = file_of(n == "fputs" ? a1 : a0);
            if (!f) {
                Ret(regs, (n == "fputs") ? static_cast<std::uint32_t>(-1)
                                         : static_cast<std::uint32_t>(-1));
                return;
            }
            if (n == "fputs") {
                if (!CheckRange(a0, 1, false)) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                Ret(regs, static_cast<std::uint32_t>(std::fputs(GuestStr(a0), f)));
                return;
            }
            if (n == "fputc") {
                Ret(regs, static_cast<std::uint32_t>(std::fputc((int)a0, f)));
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(std::fgetc(f)));
            return;
        }
        if (n == "fdopen") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                Ret(regs, 0);
                return;
            }
            FILE* f = ::fdopen(hfd, GuestStr(a1));
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
                Ret(regs, 0);
                return;
            }
            Ret(regs, h);
            return;
        }
        if (n == "tmpfile") {
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
                Ret(regs, 0);
                return;
            }
            Ret(regs, h);
            return;
        }
        if (n == "open") {
            const std::string hp = map_path(a0);
            if (!need_host(hp, "open")) {
                return;
            }
            if ((a1 & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0) {
                ensure_parent(hp);
            }
            int fd = ::open(hp.c_str(), (int)a1, (mode_t)a2);
            if (fd < 0) {
                std::printf("[files] open(\"%s\",%o) -> -1 (errno %d)\n", hp.c_str(), a1, errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::uint32_t g = alloc_guest_fd(fd);
            if (std::strcmp(GuestStr(a0), "/dev/alarm") == 0) {
                NoteAlarmFd(g);
                std::printf("[files][t%u] alarm fd %u (clocks served, not null)\n", GuestTid(),
                            g);
            }
            if (DiagOn()) {
                std::printf("[files][t%u] open(\"%s\",%o) -> fd %u\n", GuestTid(), hp.c_str(), a1,
                            g);
            }
            Ret(regs, g);
            return;
        }
        if (n == "close" || n == "fsync") {
            const int hfd = (n == "close") ? erase_guest_fd(a0) : host_fd_of(a0);
            if (n == "close") {
                ForgetAlarmFd(a0);
            }
            if (hfd < 0) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            // Unlocked I/O: fsync can block; close must not run under the map
            // lock either (a concurrent in-flight read keeps its own copy).
            int r = (n == "close") ? ::close(hfd) : ::fsync(hfd);
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "pipe" || n == "pipe2") {
            // Fills int[2] with guest fds. The old stub returned 0 WITHOUT
            // writing fds (stack garbage downstream -> throw-nest). Real ones.
            if (!CheckRange(a0, 8, true)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            int hfds[2] = {-1, -1};
            int r = (n == "pipe") ? ::pipe(hfds) : ::pipe2(hfds, (int)a1);
            if (r != 0) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::uint32_t g0 = alloc_guest_fd(hfds[0]);
            const std::uint32_t g1 = alloc_guest_fd(hfds[1]);
            std::memcpy(H(a0), &g0, 4);
            std::memcpy(H(a0 + 4), &g1, 4);
            std::printf("[files][t%u] pipe() -> [%u %u]\n", GuestTid(), g0, g1);
            Ret(regs, 0);
            return;
        }
        if (n == "read" || n == "write") {
            if (a0 <= 2) {  // stdio: read(0)=EOF, write(1/2)->host log
                if (n == "read") {
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
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (!CheckRange(a1, a2, n == "read")) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            // HARNESS_NONET=1: airplane-faithful — writes to sockets fail
            // (ENETUNREACH); pipes/files are unaffected (local IPC works
            // offline). The TLS stack writes via write(), not send().
            if (n == "write" && EnvOn("HARNESS_NONET")) {
                struct stat fw {};
                if (::fstat(hfd, &fw) == 0 && S_ISSOCK(fw.st_mode)) {
                    set_guest_errno(ENETUNREACH);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
            }
            // Unlocked: pipes/sockets block here; the map lock must be free
            // so other threads keep running (2026-10-10 deadlock fix).
            // Cap: FIFO reads wait max 5s (poll) then fail EAGAIN —
            // an uncapped ::read on a pipe whose writer (dead worker) never
            // delivers message #2 froze the whole run (STUCK@300s, 2026-10-10).
            // Sockets skip the cap (SO_RCVTIMEO governs; blocking semantics).
            // Regular files skip the cap (always fast). EOF semantics intact:
            // closed write-ends still read 0 via the normal path below.
            if (n == "read") {
                struct stat fst {};
                if (::fstat(hfd, &fst) == 0 && S_ISFIFO(fst.st_mode)) {
                    if (wait_fd(hfd, POLLIN, 5000) <= 0) {
                        set_guest_errno(EAGAIN);
                        Ret(regs, static_cast<std::uint32_t>(-1));
                        return;
                    }
                }
            }
            // TLS ClientHello version bump for write() path (the in-module
            // TLS stack uses plain write, not send — same fix as send()).
            // Pattern first (cheap), fstat only on match (not per-write).
            if (n == "write" && !EnvOn("HARNESS_NOTLSFIX") && a2 >= 11 &&
                CheckRange(a1, 11, true)) {
                std::uint8_t* q = H(a1);
                if (q[0] == 0x16 && q[1] == 0x03 && q[2] < 0x03 && q[5] == 0x01) {
                    struct stat fst {};
                    if (::fstat(hfd, &fst) == 0 && S_ISSOCK(fst.st_mode)) {
                        if (DiagOn()) {
                            std::printf(
                                "[netsock]%s TLS ClientHello version %02x%02x/%02x%02x -> "
                                "0301/0301 (write path)\n",
                                GuestTid() == 1 ? "" : "(T2+)", q[1], q[2], q[9], q[10]);
                        }
                        q[1] = 0x03;
                        q[2] = 0x01;
                        q[9] = 0x03;
                        q[10] = 0x01;
                    }
                }
            }
            ssize_t r = (n == "read") ? ::read(hfd, H(a1), a2) : ::write(hfd, H(a1), a2);
            // TEMP-DIAG (worker hang): log pipe traffic (fds >= 100 that are
            // pipes show up here via fd_map_; files too, capped). Skip
            // regular files (/proc/cpuinfo floods the cap): pipes+sockets.
            // Diagnostic layer (PLAN.md B7): with DIAG off skip the per-I/O
            // fstat + io_mutex + fd_map probe that only fed this capped log.
            if (DiagOn() && (n == "write" || n == "read") && r > 0) {
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
            Ret(regs, static_cast<std::uint32_t>(r < 0 ? -1 : r));
            return;
        }
        if (n == "lseek") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            off_t r = ::lseek(hfd, (off_t)a1, (int)a2);
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "writev") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0 || !CheckRange(a1, (std::size_t)a2 * 8, false)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            ssize_t total = 0;
            for (std::uint32_t i = 0; i < a2; i++) {
                std::uint32_t base = 0, len = 0;
                std::memcpy(&base, H(a1 + i * 8), 4);
                std::memcpy(&len, H(a1 + i * 8 + 4), 4);
                if (!CheckRange(base, len, false)) {
                    break;
                }
                ssize_t w = ::write(hfd, H(base), len);
                if (w < 0) {
                    total = -1;
                    break;
                }
                total += w;
                if ((std::uint32_t)w != len) {
                    break;
                }
            }
            Ret(regs, static_cast<std::uint32_t>(total < 0 ? -1 : total));
            return;
        }
        // --- socket bridge (host-backed; identical path runs on device) ---
        // The game bundles its own DNS-over-UDP resolver (raw socket/
        // connect/send/recvfrom, no getaddrinfo import) for cloud.rovio.com
        // (session/time, sessions, apdrive variantconfig). These used to hit
        // the generic stub ->0 (socket()=0 ALIASED STDIN; connect()=fake
        // success; send/recv()=0 = empty replies -> 4x retry then null-call).
        // Real host sockets: Bionic/Linux errno values + sockaddr layout
        // match, so forward directly. Sockets stay O_NONBLOCK; every op is
        // poll-capped (5s I/O, 10s TCP connect) so dead endpoints degrade to
        // the offline path (EAGAIN/ETIMEDOUT) instead of hanging.
        // HARNESS_NONET=1: socket() -> -1/ENETUNREACH (fast offline tests).
        // NOTE: fcntl/ioctl are still stub->0 (fake success, mode unchanged);
        // game retries cover it. sendmsg/recvmsg NOT marshalled (guest msghdr
        // is 32-bit layout) -> LOUD if ever called.
        // (set_guest_errno/wait_fd live with the fd helpers above.)
        if (n == "socket") {
            // HARNESS_NONET=1: airplane-mode-faithful failure (socket()
            // itself SUCCEEDS on device — no network needed to create one;
            // connect()/send()/recv() fail below). Failing socket() outright
            // sent the game down a never-tested path (BAD read @0x1A343C).
            if (EnvOn("HARNESS_NONET")) {
                std::printf("[netsock]%s NONET: socket layer down (connect/recv will fail)\n",
                            GuestTid() == 1 ? "" : "(T2+)");
            }
            int hfd = ::socket((int)a0, (int)a1, (int)a2);
            if (hfd < 0) {
                set_guest_errno(errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            // BLOCKING by default (faithful Bionic semantics: recv waits for
            // the full length). O_NONBLOCK only during connect (below), then
            // restored; stuck peers are bounded by SO_RCVTIMEO/SO_SNDTIMEO.
            // (Leaving sockets nonblocking caused short reads: the TLS stack
            // got 5B of a 115B ServerHello -> fatal alert 47. 2026-10-10.)
            const std::uint32_t g = alloc_guest_fd(hfd);
            if (DiagOn()) {
                std::printf("[netsock] socket(%u %u %u) -> gfd %u\n", a0, a1, a2, g);
            }
            Ret(regs, g);
            return;
        }
        if (n == "connect") {
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
            // Nonblocking dance for the 10s cap, then RESTORE blocking +
            // arm SO_RCVTIMEO/SO_SNDTIMEO (10s) so failed peers surface as
            // EAGAIN instead of hanging, while healthy I/O keeps full
            // blocking (short-read) semantics the game expects.
            int fl = ::fcntl(hfd, F_GETFL, 0);
            if (fl >= 0) {
                ::fcntl(hfd, F_SETFL, fl | O_NONBLOCK);
            }
            // HARNESS_PORTMAP=1: rewrite TCP dst port 443 -> 8443 (lets a
            // non-root fake origin serve the game's https:// URLs; the game
            // itself always dials 443).
            if (EnvOn("HARNESS_PORTMAP") && a2 >= 4 && CheckRange(a1, 4, true)) {
                std::uint8_t* q = H(a1);
                if (q[0] == 2 /*AF_INET*/ && q[1] == 0 && q[2] == 0x01 && q[3] == 0xBB) {
                    q[2] = 0x20;
                    q[3] = 0xFB;  // 8443
                    std::printf("[netsock]%s portmap 443 -> 8443\n",
                                GuestTid() == 1 ? "" : "(T2+)");
                }
            }
            int r = ::connect(hfd, reinterpret_cast<const struct sockaddr*>(H(a1)),
                              (socklen_t)a2);
            // TEMP-DIAG (DNS bring-up): who are we connecting to?
            {
                static int cl = 0;
                if (cl < 12 && a2 >= 8) {
                    cl++;
                    const std::uint8_t* p = H<const std::uint8_t>(a1);
                    std::uint16_t fam = (std::uint16_t)p[0] | ((std::uint16_t)p[1] << 8);
                    std::uint16_t port = ((std::uint16_t)p[2] << 8) | p[3];
                    if (DiagOn()) {
                        std::printf("[netsock]%s connect(gfd %u) fam=%u %u.%u.%u.%u:%u%s\n",
                                    GuestTid() == 1 ? "" : "(T2+)", a0, fam, p[4], p[5], p[6], p[7],
                                    port, cl == 12 ? " (quiet henceforth)" : "");
                    }
                }
            }
            if (EnvOn("HARNESS_NONET")) {
                // Airplane-faithful: UDP connect() SUCCEEDS on Linux (no
                // packets sent; only sets the default remote). Only TCP
                // connect fails (ENETUNREACH). Failing UDP connect sent the
                // bundled resolver down a never-on-device path (it aborts
                // before send with "Could not contact DNS server", while on
                // device it proceeds to sendto-fail/timeout = "no answer").
                int st = 0;
                socklen_t sl = sizeof(st);
                if (::getsockopt(hfd, SOL_SOCKET, SO_TYPE, &st, &sl) == 0 &&
                    st == SOCK_DGRAM) {
                    // Leave r as-is (real UDP connect outcome: success).
                } else {
                    // Airplane mode: TCP connect never succeeds
                    // (server unreachable). Instant, no 10s waits.
                    errno = ENETUNREACH;
                    r = -1;
                }
            } else if (r != 0 && errno == EINPROGRESS) {
                const int w = wait_fd(hfd, POLLOUT, 10000);
                if (w > 0) {
                    int se = 0;
                    socklen_t sl = sizeof(se);
                    ::getsockopt(hfd, SOL_SOCKET, SO_ERROR, &se, &sl);
                    if (se == 0) {
                        r = 0;
                    } else {
                        errno = se;
                        r = -1;
                    }
                } else {
                    if (w == 0) {
                        errno = ETIMEDOUT;
                    }
                    r = -1;
                }
            }
            if (r != 0) {
                set_guest_errno(errno);
            } else if (fl >= 0) {
                // Connected: back to blocking + kernel timeouts.
                ::fcntl(hfd, F_SETFL, fl);
                struct timeval tv {};
                tv.tv_sec = 10;
                ::setsockopt(hfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ::setsockopt(hfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            }
            if (DiagOn()) {
                std::printf("[netsock]%s connect(gfd %u) -> %d (errno %d)\n",
                            GuestTid() == 1 ? "" : "(T2+)", a0, r, r != 0 ? errno : 0);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "shutdown" || n == "bind" || n == "listen" || n == "setsockopt" ||
            n == "getsockopt") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            int r = -1;
            if (n == "shutdown") {
                r = ::shutdown(hfd, (int)a1);
            } else if (n == "bind" || n == "listen") {
                if (n == "bind") {
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
                // (level,name) in regs; value + len-ptr: a3=ptr, stack=lenptr.
                std::uint32_t lenptr = 0;
                if (CheckRange(regs[13], 4, false)) {
                    std::memcpy(&lenptr, H(regs[13]), 4);
                }
                std::uint32_t len = 0;
                if (lenptr && CheckRange(lenptr, 4, false)) {
                    std::memcpy(&len, H(lenptr), 4);
                }
                if (len > 256 || !CheckRange(a3, len, n != "setsockopt")) {
                    set_guest_errno(EFAULT);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                if (n == "setsockopt") {
                    r = ::setsockopt(hfd, (int)a1, (int)a2, H(a3), (socklen_t)len);
                } else {
                    socklen_t sl = (socklen_t)len;
                    r = ::getsockopt(hfd, (int)a1, (int)a2, H(a3), &sl);
                    if (lenptr && CheckRange(lenptr, 4, true)) {
                        const std::uint32_t back = (std::uint32_t)sl;
                        std::memcpy(H(lenptr), &back, 4);
                    }
                }
            }
            if (r != 0) {
                set_guest_errno(errno);
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "accept") {
            const int lfd = host_fd_of(a0);
            if (lfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (wait_fd(lfd, POLLIN, 5000) <= 0) {
                set_guest_errno(EAGAIN);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            struct sockaddr_storage ss {};
            socklen_t sl = sizeof(ss);
            int hfd = ::accept(lfd, reinterpret_cast<struct sockaddr*>(&ss), &sl);
            if (hfd < 0) {
                set_guest_errno(errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::uint32_t g = alloc_guest_fd(hfd);
            Ret(regs, g);
            return;
        }
        if (n == "send" || n == "sendto") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            // sendto: addr=a4, addrlen=a5 live on guest stack (ARM APCS).
            std::uint32_t addr = 0, addrlen = 0;
            if (n == "sendto") {
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
            }
            if (!CheckRange(a1, a2, false)) {
                set_guest_errno(EFAULT);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            // HARNESS_NONET=1: airplane-faithful traffic failure. Sockets
            // create fine and UDP connects (above), but no packet can leave:
            // send/sendto fail ENETUNREACH (no route), recv waits out the
            // kernel timeout below (EAGAIN). Previously sends went out over
            // the HOST uplink even under NONET (unfaithful).
            // HARNESS_DNSALLOW=1: punch UDP/53 past NONET (real 8.8.8.8
            // replies for the custom resolver; ordinary userspace traffic).
            if (EnvOn("HARNESS_NONET")) {
                bool dns53 = false;
                {
                    int st = 0;
                    socklen_t sl = sizeof(st);
                    struct sockaddr_storage ss {};
                    socklen_t slen = sizeof(ss);
                    if (::getsockopt(hfd, SOL_SOCKET, SO_TYPE, &st, &sl) == 0 &&
                        st == SOCK_DGRAM &&
                        ::getpeername(hfd, reinterpret_cast<struct sockaddr*>(&ss),
                                      &slen) == 0 &&
                        ss.ss_family == AF_INET) {
                        const auto* sa = reinterpret_cast<const struct sockaddr_in*>(&ss);
                        if (ntohs(sa->sin_port) == 53) {
                            dns53 = true;
                        }
                    }
                }
                // HARNESS_DNSHUSH=1: device-airplane DNS shape (PROVEN by
                // device logcat: "Could not resolve ... (Could not contact DNS
                // servers)" -> HttpRequestException(6) -> caught -> menu).
                // Queries are swallowed (fake full send, nothing leaves), no
                // reply ever arrives -> resolver times out instead of taking
                // the immediate-ENETUNREACH silent-park branch.
                if (dns53 && EnvOn("HARNESS_DNSHUSH")) {
                    std::printf("[netsock] dnshush swallowed %uB [t%u]\n", a2, GuestTid());
                    Ret(regs, a2);
                    return;
                }
                bool dnsout = dns53 && EnvOn("HARNESS_DNSALLOW");
                if (!dnsout) {
                    set_guest_errno(ENETUNREACH);
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
            }
            // TLS version DOWNGRADE (harness bridge): the in-module stack
            // is capped at TLS 1.0 (any 0303 ServerHello record -> fatal
            // alert 47 illegal_parameter, even though the hello body says
            // 0303). Force offer 0301/0301 so a 1.0-tolerant server answers
            // 0301. HARNESS_NOTLSFIX=1 disables (parity experiments).
            // NOTE: needs a server whose policy still allows TLS 1.0;
            // modern CloudFront usually does for old distributions.
            if (!EnvOn("HARNESS_NOTLSFIX") && a2 >= 11 && CheckRange(a1, 11, true)) {
                std::uint8_t* q = H(a1);
                if (q[0] == 0x16 && q[1] == 0x03 && q[2] < 0x03 && q[5] == 0x01) {
                    if (DiagOn()) {
                        std::printf("[netsock]%s TLS ClientHello version %02x%02x/%02x%02x -> 0301/0301\n",
                                    GuestTid() == 1 ? "" : "(T2+)", q[1], q[2], q[9], q[10]);
                    }
                    q[1] = 0x03;
                    q[2] = 0x01;
                    q[9] = 0x03;
                    q[10] = 0x01;
                }
            }
            // Blocking I/O with kernel SO_RCVTIMEO/SO_SNDTIMEO (set at
            // connect): full short-read semantics; timeouts surface as
            // EAGAIN via errno. No poll short-circuit (it caused short
            // reads on nonblocking fds -> TLS alert 47).
            ssize_t r =
                (n == "send")
                    ? ::send(hfd, H(a1), a2, (int)a3)
                    : ::sendto(hfd, H(a1), a2, (int)a3,
                               addr ? reinterpret_cast<const struct sockaddr*>(H(addr)) : nullptr,
                               (socklen_t)addrlen);
            if (r < 0) {
                set_guest_errno(errno);
                // TEMP-DIAG (network bring-up): errors are the signal.
                std::printf("[netsock]%s %s(gfd %u %uB) -> -1 errno %d\n",
                            GuestTid() == 1 ? "" : "(T2+)", n.c_str(), a0, a2, errno);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "recv" || n == "recvfrom") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                set_guest_errno(EBADF);
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            std::uint32_t addr = 0, addrlenptr = 0, addrlen = 0;
            if (n == "recvfrom") {
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
            // Blocking I/O with kernel SO_RCVTIMEO (set at connect):
            // full short-read semantics; timeout surfaces as EAGAIN.
            // (The old poll short-circuit on nonblocking fds caused short
            // reads -> TLS alert 47.)
            struct sockaddr_storage ss {};
            socklen_t sl = (socklen_t)(addrlen != 0 ? addrlen : sizeof(ss));
            ssize_t r =
                (n == "recv")
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
                if (nlog < 40 && r > 0) {
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
        if (n == "sendmsg" || n == "recvmsg") {
            std::printf("[netsock] LOUD %s unmarshal TODO -- halting\n", n.c_str());
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        // getpeername/getsockname: the engine stringifies socket addrs
        // (sa_addr inet_ntop) and logs "Trying %s...". Stubs left the
        // buffers zeroed (family 0) -> ntop failed -> HttpRequestException.
        if (n == "getpeername" || n == "getsockname") {
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
            int r = (n == "getpeername" ? ::getpeername(hfd, reinterpret_cast<struct sockaddr*>(&ss),
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
        if (n == "fcntl") {
            // TEMP-DIAG (stuck workers): pinpoint the parking line.
            static int fcl = 0;
            const int fldiag = (fcl < 40) ? fcl++ : -1;
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
                Ret(regs, alloc_guest_fd(r));
                return;
            }
            // File locks etc.: success-lie (single-process bring-up; revisit
            // if a game path depends on lock contention).
            Ret(regs, 0);
            return;
        }
        if (n == "ioctl" && IsAlarmFd(a0)) {
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
                        if (alog < 3) {
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
                if (op == 3) {  // SET_AND_WAIT: sleep till target, capped
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
                    if (us > 5000u) {
                        us = 5000u;  // same cap policy as nanosleep
                    }
                    if (us > 0) {
                        usleep(static_cast<useconds_t>(us));
                    }
                    Ret(regs, 0);
                    return;
                }
            }
        }
        if (n == "inet_addr" || n == "inet_aton" || n == "inet_pton" || n == "inet_ntoa" ||
            n == "inet_ntop") {
            InetThunk(regs, n, a0, a1, a2, a3);
            return;
        }
        if (n == "stat" || n == "access" || n == "mkdir" || n == "unlink" || n == "remove" ||
            n == "rename" || n == "rmdir" || n == "chmod" || n == "getcwd") {
            if (n == "getcwd") {
                const std::string cwd = files_hostbase_.empty() ? "/" : files_hostbase_;
                if (a1 == 0 || !CheckRange(a0, cwd.size() + 1, true)) {
                    Ret(regs, 0);
                    return;
                }
                std::memcpy(H(a0), cwd.c_str(), cwd.size() + 1);
                Ret(regs, a0);
                return;
            }
            const std::string hp = map_path(a0);
            if (!need_host(hp, n.c_str())) {
                return;
            }
            if (n == "stat") {
                struct stat st {};
                if (::stat(hp.c_str(), &st) != 0 || !fill_stat(a1, st)) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                Ret(regs, 0);
                return;
            }
            if (n == "access") {
                Ret(regs, static_cast<std::uint32_t>(::access(hp.c_str(), (int)a1)));
                return;
            }
            if (n == "mkdir") {
                std::error_code ec;
                bool ok = std::filesystem::create_directories(hp, ec);
                Ret(regs, (ok && !ec) ? 0u : static_cast<std::uint32_t>(-1));
                (void)ok;
                return;
            }
            if (n == "unlink" || n == "remove") {
                Ret(regs, static_cast<std::uint32_t>(::unlink(hp.c_str())));
                return;
            }
            if (n == "rename") {
                const std::string hp2 = map_path(a1);
                if (!need_host(hp2, "rename-dst")) {
                    return;
                }
                ensure_parent(hp2);
                Ret(regs, static_cast<std::uint32_t>(::rename(hp.c_str(), hp2.c_str())));
                return;
            }
            if (n == "rmdir") {
                Ret(regs, static_cast<std::uint32_t>(::rmdir(hp.c_str())));
                return;
            }
            // chmod
            Ret(regs, static_cast<std::uint32_t>(::chmod(hp.c_str(), (mode_t)a1)));
            return;
        }
        if (n == "fstat") {
            const int hfd = host_fd_of(a0);
            if (hfd < 0) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            struct stat st {};
            if (::fstat(hfd, &st) != 0 || !fill_stat(a1, st)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            Ret(regs, 0);
            return;
        }
        if (n == "opendir") {
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
                Ret(regs, 0);
                return;
            }
            const std::uint32_t h = GuestMalloc(16);
            if (!h) {
                ::closedir(d);
                Ret(regs, 0);
                return;
            }
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                HostDir hd;
                hd.d = d;
                hd.path = hp;
                hd.cpu_topo = (hp == "/sys/devices/system/cpu");
                dir_map_[h] = hd;
            }
            Ret(regs, h);
            return;
        }
        if (n == "readdir_r") {
            // args: DIR*(a0), dirent*(a1), dirent**(a2). Guest dirent (bionic):
            // u64 ino@0, i64 off@8, u16 reclen@16, u8 type@17, name[256]@18.
            DIR* dd = nullptr;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto it = dir_map_.find(a0);
                if (it == dir_map_.end()) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                dd = it->second.d;
            }
            // Pre-check is minimal on purpose (guest buffers can be smaller
            // than a full 280B dirent, e.g. the 24B cpu-topology buffer):
            // the sized check happens at the bounded write below.
            if (!CheckRange(a1, 19, true) || !CheckRange(a2, 4, true)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            errno = 0;
            struct dirent* de = ::readdir(dd);
            bool cpu_topo = false;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto dit = dir_map_.find(a0);
                if (dit != dir_map_.end()) {
                    cpu_topo = dit->second.cpu_topo;
                }
            }
            if (cpu_topo) {
                // Skip entries the game's own filter rejects AND that don't
                // fit its 24B buffer (18B header + name + NUL): dots + cpuN
                // pass (game skips dots itself via strcmp, keeps cpuN).
                // Bounded write below (19+len <= 24) can never touch buf+24.
                static int skiplog = 0;
                for (;;) {
                    if (!de) {
                        break;
                    }
                    const char* nm = de->d_name;
                    bool keep = false;
                    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) {
                        keep = true;
                    } else if (nm[0] == 'c' && nm[1] == 'p' && nm[2] == 'u') {
                        int nd = 0;
                        keep = true;
                        for (int i = 3; nm[i]; i++) {
                            if (nm[i] < '0' || nm[i] > '9' || ++nd > 2) {
                                keep = false;
                                break;
                            }
                        }
                        if (nd < 1) {
                            keep = false;
                        }
                    }
                    if (keep) {
                        break;
                    }
                    if (skiplog < 5) {
                        skiplog++;
                        std::printf("[files]%s readdir skip \"%s\" (cpu topo, long/non-cpu)%s\n",
                                    GuestTid() == 1 ? "" : "(T2+)", nm,
                                    skiplog == 5 ? " (quiet henceforth)" : "");
                    }
                    errno = 0;
                    de = ::readdir(dd);
                }
            } else {
                // Tripwire for future victims: names far beyond any sane
                // dirent consumer get LOUD-logged (behavior unchanged).
                if (de && std::strlen(de->d_name) > 64) {
                    static int longlog = 0;
                    if (longlog < 5) {
                        longlog++;
                        std::printf("[files]%s readdir LONG \"%s\" (%zu)%s\n",
                                    GuestTid() == 1 ? "" : "(T2+)", de->d_name,
                                    std::strlen(de->d_name),
                                    longlog == 5 ? " (quiet henceforth)" : "");
                    }
                }
            }
            if (!de) {
                std::uint32_t z = 0;
                std::memcpy(H(a2), &z, 4);
                Ret(regs, errno ? errno : 0);
                return;
            }
            // Bounded write: cpu-topo entries fit 19+len (<=24, the game's
            // buffer); other dirs keep the faithful 280B struct.
            const std::size_t nm_len = std::strlen(de->d_name);
            const std::size_t wn = cpu_topo ? (19 + nm_len) : 280;
            if (!CheckRange(a1, wn, true)) {
                // Capped loud (a too-small buffer for a kept entry would
                // previously corrupt neighbors silently).
                static int smalllog = 0;
                if (smalllog < 5) {
                    smalllog++;
                    std::printf("[files]%s readdir SMALLBUF need=%zu%s\n",
                                GuestTid() == 1 ? "" : "(T2+)", wn,
                                smalllog == 5 ? " (quiet henceforth)" : "");
                }
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            std::uint8_t* p = H<std::uint8_t>(a1);
            std::memset(p, 0, wn);
            std::uint64_t ino = de->d_ino;
            std::memcpy(p, &ino, 8);
            std::int64_t off = de->d_off;
            std::memcpy(p + 8, &off, 8);
            if (cpu_topo) {
                const std::uint16_t rl = static_cast<std::uint16_t>(wn);
                std::memcpy(p + 16, &rl, 2);
            } else {
                std::uint16_t rl = de->d_reclen;
                std::memcpy(p + 16, &rl, 2);
            }
            p[17] = de->d_type;
            std::strncpy(reinterpret_cast<char*>(p + 18), de->d_name,
                         cpu_topo ? (nm_len + 1) : 255);
            std::memcpy(H(a2), &a1, 4);
            Ret(regs, 0);
            return;
        }
        if (n == "closedir") {
            DIR* dd = nullptr;
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                const auto it = dir_map_.find(a0);
                if (it == dir_map_.end()) {
                    Ret(regs, static_cast<std::uint32_t>(-1));
                    return;
                }
                dd = it->second.d;
                dir_map_.erase(it);
            }
            int r = ::closedir(dd);
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
    }
    // --- APK asset bridge (AAssetManager_*): getBuffer model over APK entries.
    // Whole-block lock (same reasoning as the file bridge above).
    {
        std::lock_guard<std::recursive_mutex> asset_lock(mutex_);
        auto ensure_index = [&]() -> bool {
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
        };
        if (n == "AAssetManager_fromJava") {
            if (!ensure_index()) {
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
        if (n == "AAssetManager_open") {
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
            if (found->usize == 0 || found->usize > 0x2000000u) {
                std::printf("[asset] LOUD open(\"%s\") bad size %llu -- halting\n", want.c_str(),
                            (unsigned long long)found->usize);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            FILE* f = std::fopen(apk_path_.c_str(), "rb");
            if (!f) {
                Ret(regs, 0);
                return;
            }
            const std::uint32_t gbuf = GuestMalloc(static_cast<std::uint32_t>(found->usize));
            if (!gbuf) {
                std::fclose(f);
                Ret(regs, 0);
                return;
            }
            bool ok = false;
            if (found->method == 0) {
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
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            std::fclose(f);
            if (!ok) {
                std::printf("[asset] open(\"%s\") extract failed\n", want.c_str());
                Ret(regs, 0);
                return;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                asset_map_[gbuf] = HostAsset{gbuf, found->usize};
            }
            if (EnvOn("HARNESS_FILESLOG")) {
                std::printf("[asset] open(\"%s\") -> %08x (%llu bytes, %s)\n", want.c_str(),
                            gbuf, (unsigned long long)found->usize,
                            found->method == 0 ? "stored" : "deflated");
            }
            Ret(regs, gbuf);  // handle IS the guest buffer (getBuffer model)
            return;
        }
        if (n == "AAsset_close") {
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
        if (n == "AAsset_getBuffer") {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = asset_map_.find(a0);
            if (it == asset_map_.end()) {
                Ret(regs, 0);
                return;
            }
            Ret(regs, it->second.guest_ptr);
            return;
        }
        if (n == "AAsset_getLength64") {
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
#include "math_thunks.inc"
    // --- libm: PURE-MATH-END ---
    // --- GLES fake-success shim (bring-up ONLY): lets shader/program setup
    // pass so update->render progresses to the first frame. Real GLES thunks
    // (surfaceless EGL + host forwarding) come next; this shim is LOUD about
    // what it fakes. Nonzero handles, status queries report GL_TRUE, logs
    // empty. Everything else falls to the generic [stub]->0 tail.
    if (n.compare(0, 2, "gl") == 0) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // Real GLES backend (HARNESS_REALGL=1): EGL surfaceless + forward to
        // the host driver. Falls through to the fake shim for unhandled calls
        // (compressed textures) or when the flag is off.
        if (RealGLBackend()->Handle(n.c_str(), regs, a0, a1, a2, a3)) {
            return;
        }
        if (n == "glGetString") {
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
        if (n == "glCreateShader" || n == "glCreateProgram") {
            static std::uint32_t gl_next = 0;
            Ret(regs, ++gl_next);
            return;
        }
        if (n == "glGetIntegerv") {
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
        if (n == "glGetShaderiv" || n == "glGetProgramiv") {            if (CheckRange(a2, 4, true)) {
                const std::uint32_t one = 1;  // COMPILE/LINK_STATUS = GL_TRUE
                std::memcpy(H(a2), &one, 4);
            }
            std::printf("[gl] %s(%u,%04x) -> TRUE\n", n.c_str(), a0, a1);
            Ret(regs, 0);
            return;
        }
        if (n == "glGetShaderInfoLog" || n == "glGetProgramInfoLog") {
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
        if (n == "glGenTextures" || n == "glGenBuffers" || n == "glGenFramebuffers" ||
            n == "glGenRenderbuffers") {
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
        if (n == "glGetUniformLocation" || n == "glGetAttribLocation") {
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
        if (n == "glTexImage2D" || n == "glTexSubImage2D" || n == "glCompressedTexImage2D" ||
            n == "glCopyTexImage2D" || n == "glReadPixels" || n == "glBufferData" ||
            n == "glVertexAttribPointer" || n == "glShaderSource") {
            // Accepted and ignored (no real GL yet): fake upload succeeds.
            // Bulk-pointer args are range-checked but not consumed.
            // TEMP-DIAG (real-GLES prep): log call signatures to learn what
            // the host GPU must accept (formats, sizes, shader sources).
            if (n == "glShaderSource" && a1 > 0 && a1 < 16 && CheckRange(a2, 4 * a1, false)) {
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
            if ((n == "glTexImage2D" || n == "glCompressedTexImage2D") && a1 <= 8) {
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
        if (n == "glGetError") {
            Ret(regs, 0);  // GL_NO_ERROR
            return;
        }
    }
    if (n == "__system_property_get") {
        // Bionic: int(name, value_out) -> length. The generic stub returned 0
        // with an empty buffer, which silently killed the resolver worker
        // (empty net.dns1 -> resolve fails -> worker exits -> main's pipe
        // wait starves). Serve a small table (HARNESS_DNS1/2 overrides).
        const std::string key = GuestStr(a0);
        std::string val;
        if (key == "net.dns1" || key == "net.dns2") {
            val = (key == "net.dns1") ? "8.8.8.8" : "8.8.4.4";
            if (const char* e = std::getenv(key == "net.dns1" ? "HARNESS_DNS1" : "HARNESS_DNS2")) {
                val = e;
            }
        } else if (key == "ro.build.version.sdk") {
            val = "21";
        } else if (key == "ro.product.model") {
            val = "izan";
        } else if (key == "ro.product.manufacturer") {
            val = "unknown";
        }
        if (!val.empty() && CheckRange(a1, val.size() + 1, true)) {
            std::memcpy(H(a1), val.c_str(), val.size() + 1);
            if (DiagOn()) {  // PLAN.md A5: diagnostic layer
                std::printf("[prop]%s \"%s\" -> \"%s\"\n", GuestTid() == 1 ? "" : "(T2+)",
                            key.c_str(), val.c_str());
            }
            Ret(regs, static_cast<std::uint32_t>(val.size()));
            return;
        }
        // Unknown key: legacy empty behavior (length 0).
    }
    if (n == "mktime" || n == "difftime" || n == "strftime") {
        // Real time-lib thunks (were stub->0: frozen daily/session timers).
        // Guest Bionic struct tm (44B): 9x int + long gmtoff + char* zone.
        if (n == "mktime") {
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
        if (n == "difftime") {
            const double d = std::difftime((std::time_t)(std::int32_t)a0,
                                           (std::time_t)(std::int32_t)a1);
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            Ret(regs, static_cast<std::uint32_t>(u & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((u >> 32) & 0xFFFFFFFFu));
            return;
        }
        // strftime(a0=buf, a1=max, a2=fmt, a3=tm*): bounded.
        std::uint32_t cap = a1 > 256u ? 256u : a1;
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
        char tmp[256] = {};
        const std::size_t o =
            std::strftime(tmp, sizeof(tmp), GuestStr(a2), &ht);
        if (o == 0 || o + 1 > cap || !CheckRange(a0, o + 1, true)) {
            Ret(regs, 0);
            return;
        }
        std::memcpy(H(a0), tmp, o + 1);
        Ret(regs, static_cast<std::uint32_t>(o));
        return;
    }
    if (n == "srand48" || n == "srand" || n == "srandom" || n == "rand" || n == "random" ||
        n == "lrand48" || n == "mrand48" || n == "drand48" || n == "erand48" ||
        n == "jrand48" || n == "nrand48") {
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
        if (n == "srand48" || n == "srand" || n == "srandom") {
            rng_x = ((std::uint64_t)(a0 & 0xFFFFFFFFu) << 16) | 0x330Eull;
            rng_seeded = true;
            (void)rng_seeded;
            Ret(regs, 0);
            return;
        }
        if (n == "drand48") {
            const double d = (double)rng_next() / (double)(1ull << 48);
            std::uint64_t u = 0;
            std::memcpy(&u, &d, 8);
            Ret(regs, static_cast<std::uint32_t>(u & 0xFFFFFFFFu),
                static_cast<std::uint32_t>((u >> 32) & 0xFFFFFFFFu));
            return;
        }
        if (n == "erand48" || n == "jrand48" || n == "nrand48") {
            // xsubi = 3x u16 in guest memory (in/out).
            std::uint16_t xs[3] = {0x330E, 0xABCD, 0x1234};
            if (CheckRange(a0, 6, false)) {
                std::memcpy(xs, H(a0), 6);
            }
            std::uint64_t x = ((std::uint64_t)xs[2] << 32) | ((std::uint64_t)xs[1] << 16) |
                              (std::uint64_t)xs[0];
            x = (rng_a * x + rng_c) & rng_m;
            xs[0] = (std::uint16_t)(x & 0xFFFFu);
            xs[1] = (std::uint16_t)((x >> 16) & 0xFFFFu);
            xs[2] = (std::uint16_t)((x >> 32) & 0xFFFFu);
            if (CheckRange(a0, 6, true)) {
                std::memcpy(H(a0), xs, 6);
            }
            if (n == "erand48") {
                const double d = (double)x / (double)(1ull << 48);
                std::uint64_t u = 0;
                std::memcpy(&u, &d, 8);
                Ret(regs, static_cast<std::uint32_t>(u & 0xFFFFFFFFu),
                    static_cast<std::uint32_t>((u >> 32) & 0xFFFFFFFFu));
                return;
            }
            if (n == "jrand48") {
                Ret(regs, static_cast<std::uint32_t>((std::int32_t)(x >> 16)));
                return;
            }
            Ret(regs, static_cast<std::uint32_t>((x >> 17) & 0x7FFFFFFFu));  // nrand48: [0,2^31)
            return;
        }
        if (n == "mrand48") {
            Ret(regs, static_cast<std::uint32_t>((std::int32_t)(rng_next() >> 16)));
            return;
        }
        // rand/random/lrand48: [0,2^31) (RAND_MAX on both Bionic and glibc).
        Ret(regs, static_cast<std::uint32_t>((rng_next() >> 17) & 0x7FFFFFFFu));
        return;
    }
    if (n == "qsort") {
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
    if (n == "bsearch") {
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
    if (DiagOn()) {
        // Locked: worker threads share stub_hits_ with main. Diagnostic layer
        // (PLAN.md A5/B13): with DIAG off this skips the per-call map probe on
        // unhandled imports and the [env]/[stub] logs.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint64_t& hits = stub_hits_[n];
        // Enriched logging for environment queries: names matter.
        if (n == "__system_property_get" || n == "getenv" || n == "dlopen" || n == "open" ||
            n == "fopen" || n == "stat" || n == "access") {
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
    Ret(regs, 0);
    return;

    // Wild guest pointer in a bulk-memory thunk: halt cleanly, never segfault.
bad_ptr:
    std::printf("[svc] BAD guest pointer in %s(%08x %08x %08x %08x) lr=%08x -- halting\n", n.c_str(),
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
    for (const auto& m : mappings_) {
        std::printf("    [%08x-%08x] prot=%d\n", m.start, m.end, m.prot);
    }
    stop_step_ = true;
    ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

std::uint32_t Runtime::PthreadMutex(const std::string& n, std::uint32_t obj) {
    // Resolve the host mutex under guard, then operate OUTSIDE it. The old
    // code held mutex_ across pthread_mutex_lock: a thread blocking on a
    // game mutex (held by a halted worker, or ABBA with a guest critical
    // section spanning an SVC) froze every mutex_-touching thread
    // (gdb-proven total freeze, 8 threads in pthread_mutex_lock, 2026-10-10).
    pthread_mutex_t* m = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (n == "pthread_mutex_init") {
        auto* m = new pthread_mutex_t;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        mutex_map_[obj] = m;
        return 0;
    }
        if (n == "pthread_mutex_destroy") {
            auto it = mutex_map_.find(obj);
            if (it != mutex_map_.end()) {
                // Destroy-guard: never free a host mutex still held (dangling
                // futex = eternal wedge in the next locker, e.g. teardown
                // racing in-flight workers on early-tap skips). Leak instead.
                bool held = false;
                {
                    std::lock_guard<std::mutex> io(io_mutex_);
                    auto oit = mutex_owner_.find(obj);
                    held = (oit != mutex_owner_.end() && oit->second.second > 0);
                    if (held) {
                        std::printf("[mutex] DESTROY-HELD obj=%08x holder=%u depth=%d "
                                    "-- skipped (leak)\n",
                                    obj, oit->second.first, oit->second.second);
                    } else {
                        mutex_owner_.erase(obj);
                    }
                }
                if (held) {
                    return 0;
                }
                m = it->second;
                mutex_map_.erase(it);
            }
            if (m) {
                pthread_mutex_destroy(m);
                delete m;
            }
            return 0;
        }
    auto it = mutex_map_.find(obj);
    if (it == mutex_map_.end()) {
        // Statically-initialized mutex (PTHREAD_MUTEX_INITIALIZER): Bionic
        // allows use without pthread_mutex_init. Lazily create (recursive).
        auto* m = new pthread_mutex_t;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        mutex_map_[obj] = m;
        it = mutex_map_.find(obj);
    }
        m = it->second;
    }  // guard released: blocking lock/unlock below must not hold mutex_.
    if (n == "pthread_mutex_lock") {
        pend_lock_obj_ = obj;
        pend_lock_tid_ = GuestTid();
        const int r = pthread_mutex_lock(m);
        pend_lock_obj_ = 0;
        pend_lock_tid_ = 0;
        if (r == 0) {
            MutexNoteLocked(obj, GuestTid());
        }
        return static_cast<std::uint32_t>(r);
    }
    if (n == "pthread_mutex_unlock") {
        // Pending-lock witness covers unlock too (main wedged INSIDE unlock
        // with pending-lock:none is a distinct shape worth naming).
        pend_lock_obj_ = obj;
        pend_lock_tid_ = GuestTid();
        const int r = pthread_mutex_unlock(m);
        pend_lock_obj_ = 0;
        pend_lock_tid_ = 0;
        if (r == 0) {
            MutexNoteUnlocked(obj, GuestTid());
        }
        return static_cast<std::uint32_t>(r);
    }
    if (n == "pthread_mutex_trylock") {
        const int r = pthread_mutex_trylock(m);
        if (r == 0) {
            MutexNoteLocked(obj, GuestTid());
        }
        return static_cast<std::uint32_t>(r);
    }
    return 22;
}

std::uint32_t Runtime::PthreadRwlock(const std::string& n, std::uint32_t obj) {
    // Same discipline as PthreadMutex: resolve under guard, operate outside.
    // Lazy-creates static initializers (PTHREAD_RWLOCK_INITIALIZER).
    pthread_rwlock_t* rw = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (n == "pthread_rwlock_init") {
            auto* r = new pthread_rwlock_t;
            pthread_rwlock_init(r, nullptr);
            rwlock_map_[obj] = r;
            return 0;
        }
        if (n == "pthread_rwlock_destroy") {
            auto it = rwlock_map_.find(obj);
            if (it != rwlock_map_.end()) {
                rw = it->second;
                rwlock_map_.erase(it);
            }
            if (rw) {
                pthread_rwlock_destroy(rw);
                delete rw;
            }
            return 0;
        }
        auto it = rwlock_map_.find(obj);
        if (it == rwlock_map_.end()) {
            auto* r = new pthread_rwlock_t;
            pthread_rwlock_init(r, nullptr);
            rwlock_map_[obj] = r;
            it = rwlock_map_.find(obj);
        }
        rw = it->second;
    }  // guard released: blocking rdlock/wrlock below must not hold mutex_.
    if (n == "pthread_rwlock_rdlock") {
        return static_cast<std::uint32_t>(pthread_rwlock_rdlock(rw));
    }
    if (n == "pthread_rwlock_wrlock") {
        return static_cast<std::uint32_t>(pthread_rwlock_wrlock(rw));
    }
    if (n == "pthread_rwlock_unlock") {
        return static_cast<std::uint32_t>(pthread_rwlock_unlock(rw));
    }
    if (n == "pthread_rwlock_tryrdlock") {
        return static_cast<std::uint32_t>(pthread_rwlock_tryrdlock(rw));
    }
    if (n == "pthread_rwlock_trywrlock") {
        return static_cast<std::uint32_t>(pthread_rwlock_trywrlock(rw));
    }
    return 22;
}

std::uint32_t Runtime::PthreadCond(const std::string& n, std::uint32_t cond, std::uint32_t arg1,
                                   std::uint32_t arg2, bool monotonic) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (n == "pthread_cond_init" || n == "pthread_cond_destroy") {
        if (n == "pthread_cond_init") {
            auto* c = new pthread_cond_t;
            pthread_cond_init(c, nullptr);
            cond_map_[cond] = c;
        } else {
            auto it = cond_map_.find(cond);
            if (it != cond_map_.end()) {
                pthread_cond_destroy(it->second);
                delete it->second;
                cond_map_.erase(it);
            }
        }
        return 0;
    }
    auto it = cond_map_.find(cond);
    if (it == cond_map_.end()) {
        // Same lazy creation for statically-initialized condvars.
        auto* c = new pthread_cond_t;
        pthread_cond_init(c, nullptr);
        cond_map_[cond] = c;
        it = cond_map_.find(cond);
    }
    if (n == "pthread_cond_signal") {
        return static_cast<std::uint32_t>(pthread_cond_signal(it->second));
    }
    if (n == "pthread_cond_broadcast") {
        return static_cast<std::uint32_t>(pthread_cond_broadcast(it->second));
    }
    auto mit = mutex_map_.find(arg1);
    if (mit == mutex_map_.end()) {
        return 22;
    }
    if (n == "pthread_cond_wait") {
        // HARNESS_CONDSATISFY=1: satisfy lang::Signal waits. Decomp
        // (FUN_0039f2a4): predicate is mutex_obj->__size[8]&1 (set by
        // 0x39F170 writes 1 + broadcast; reset writes 0). The signaler never
        // runs here, so set [mutex+8]|=1 (arg1) and return 0 (predicate true
        // -> proceed, no abort). Plausibility-gated (flag clear); first wait
        // per cond.
        {
            static int csen = -1;
            if (csen < 0) {
                csen = std::getenv("HARNESS_CONDSATISFY") ? 1 : 0;
            }
            static std::unordered_map<std::uint32_t, char> csdone;
            if (csen && csdone.find(cond) == csdone.end() && csdone.size() < 16) {
                csdone[cond] = 1;
                bool ok = CheckRange(arg1 + 8, 1, false);
                std::uint8_t fl = 0;
                if (ok) {
                    std::memcpy(&fl, H(arg1 + 8), 1);
                    ok = (fl == 0);
                }
                std::printf("[cond] satisfy c=%08x m=%08x fl=%u %s\n", cond, arg1, fl,
                            ok ? "SATISFIED" : "skipped");
                if (ok && CheckRange(arg1 + 8, 1, true)) {
                    const std::uint8_t one = 1;
                    std::memcpy(H(arg1 + 8), &one, 1);
                    return 0;
                }
            }
        }
        // HARNESS_NOCAPWAIT=1 disables the cap (fidelity experiments).
        // Otherwise bound unbounded waits like timedwait: a worker/main
        // deadlock on a never-signaled cond (seen at loading: worker waits
        // for main's processing, main polls for worker's message) hangs
        // forever; spurious wakeups let predicates re-check instead.
        // TEMP-DIAG (rendezvous hunt): dump around cond+mutex (find the
        // predicate flag the waiter polls, e.g. Signal.cpp [r4+8]).
        {
            static int cd = 0;
            if (cd < 3 && CheckRange(cond > 64 ? cond - 64 : 0, 128, false)) {
                cd++;
                const std::uint8_t* p = H<const std::uint8_t>(cond - 64);
                std::printf("[cond] wait c=%08x m=%08x t=%u dump:", cond, arg1, GuestTid());
                for (int j = 0; j < 128; j++) {
                    std::printf(" %02x", p[j]);
                }
                std::printf("\n");
            }
        }
        // HARNESS_NOCAPWAIT=1 disables the cap (fidelity experiments).
        // Otherwise bound unbounded waits like timedwait: a worker/main
        // deadlock on a never-signaled cond (seen at loading: worker waits
        // for main's processing, main polls for worker's message) hangs
        // forever; spurious wakeups let predicates re-check instead.
        if (!EnvOn("HARNESS_NOCAPWAIT")) {
            struct timespec ts {};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            mutex_.unlock();
            const int r = pthread_cond_timedwait(it->second, mit->second, &ts);
            mutex_.lock();
            // TEMP-DIAG (cond triage): who waits/signals which cond.
            {
                static int cw = 0;
                if (cw < 10) {
                    cw++;
                    std::printf("[cond] wait c=%08x m=%08x t=%u -> %d\n", cond, arg1,
                                GuestTid(), r);
                }
            }
            // TEMP-EXP (HARNESS_SPURWAKE=1): report spurious success instead
            // of ETIMEDOUT (POSIX-legal; predicates re-check). Probes whether
            // the waiter proceeds (predicate already true) or re-waits.
            if (r != 0 && EnvOn("HARNESS_SPURWAKE")) {
                std::printf("[cond] spurwake c=%08x t=%u (was %d)\n", cond, GuestTid(), r);
                return 0;
            }
            return static_cast<std::uint32_t>(r);
        }
        mutex_.unlock();
        const int r = pthread_cond_wait(it->second, mit->second);
        mutex_.lock();
        return static_cast<std::uint32_t>(r);
    }
    if (n == "pthread_cond_timedwait") {
        // Guest ABI: absolute timeout as 2x u32 (sec, nsec) at arg2 --
        // NOT a 16B host timespec (over-read misparsed tv_sec/nsec and
        // fired the absurd-cap spuriously).
        std::uint32_t gsec = 0, gnsec = 0;
        if (CheckRange(arg2, 8, false)) {
            std::memcpy(&gsec, H(arg2), 4);
            std::memcpy(&gnsec, H(arg2 + 4), 4);
        }
        struct timespec ts {};
        ts.tv_sec = (time_t)gsec;
        ts.tv_nsec = (long)gnsec;
        // Cap absurd waits so missing signals surface instead of hanging forever.
        // Compare against the MATCHING clock (monotonic timeouts are not realtime).
        struct timespec now {};
        clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
        const std::uint64_t want =
            std::uint64_t(ts.tv_sec) * 1000000000u + std::uint64_t(ts.tv_nsec);
        const std::uint64_t have =
            std::uint64_t(now.tv_sec) * 1000000000u + std::uint64_t(now.tv_nsec);
        if (want > have + 5000000000u) {
            ts.tv_sec = now.tv_sec + 5;
            ts.tv_nsec = now.tv_nsec;
        }
        mutex_.unlock();
        const int r = pthread_cond_timedwait(it->second, mit->second, &ts);
        mutex_.lock();
        return static_cast<std::uint32_t>(r);
    }
    return 22;
}

std::uint32_t Runtime::PthreadKeyCreate(std::uint32_t key_ptr, std::uint32_t /*destr*/) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::uint32_t key = next_key_++;
    key_map_[key] = 0;
    if (key_ptr) {
        std::memcpy(H(key_ptr), &key, 4);
    }
    return 0;
}

std::uint32_t Runtime::PthreadGetSpecific(std::uint32_t key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::uint64_t id = (std::uint64_t(key) << 32) | GuestTid();
    auto it = tls_map_.find(id);
    return it == tls_map_.end() ? 0 : it->second;
}

std::uint32_t Runtime::PthreadSetSpecific(std::uint32_t key, std::uint32_t value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::uint64_t id = (std::uint64_t(key) << 32) | GuestTid();
    tls_map_[id] = value;
    return 0;
}

bool Runtime::PthreadOnce(std::uint32_t control, std::uint32_t routine,
                          std::array<std::uint32_t, 16>& regs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!CheckRange(control, 4, false)) {
        std::printf("[once] BAD control %08x -- halting\n", control);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return false;
    }
    std::uint32_t state = 0;
    std::memcpy(&state, H(control), 4);
    if (state != 0) {
        return false;  // already done: normal return 0
    }
    if (once_hijack_.active) {
        std::printf("[once] LOUD recursive once (control=%08x routine=%08x) -- treating as done\n",
                    control, routine);
        return false;
    }
    if (!IsCodeAddress(routine & ~std::uint32_t(1))) {
        std::printf("[once] LOUD bad routine %08x -- halting\n", routine);
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return false;
    }
    std::printf("[once] hijack: control=%08x routine=%08x\n", control, routine);
    once_hijack_.active = true;
    once_hijack_.control = control;
    once_hijack_.saved_regs = regs;
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
    regs[14] = mod_.halt_va;
    return true;
}

std::uint32_t Runtime::FindExidx(std::uint32_t pc) {
    const std::uint32_t base = mod_.exidx_va;
    const std::uint32_t count = mod_.exidx_count;
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
    // BSD ctype classes (octal 001..0200): U L N S P C B X.
    std::uint8_t cls[256] = {};
    for (int c = 0; c < 256; c++) {
        std::uint8_t b = 0;
        const bool up = (c >= 'A' && c <= 'Z'), lo = (c >= 'a' && c <= 'z');
        const bool dg = (c >= '0' && c <= '9');
        const bool xd = dg || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        const bool sp = (c == ' ' || (c >= '\t' && c <= '\r'));
        const bool bl = (c == ' ' || c == '\t');
        const bool ct = (c < 0x20 || c == 0x7F);
        const bool gr = (c > 0x20 && c < 0x7F);
        if (up) {
            b |= 1;
        }
        if (lo) {
            b |= 2;
        }
        if (dg) {
            b |= 4;
        }
        if (sp) {
            b |= 8;
        }
        if (gr && !up && !lo && !dg) {
            b |= 16;
        }
        if (ct) {
            b |= 32;
        }
        if (bl) {
            b |= 64;
        }
        if (xd) {
            b |= 128;
        }
        cls[c] = b;
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
    if (!var_ctype || !var_guard || !var_lower || !var_upper) {
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
    const Slot slots[] = {
        {"_ctype_", var_ctype},
        {"__stack_chk_guard", var_guard},
        {"_tolower_tab_", var_lower},
        {"_toupper_tab_", var_upper},
    };
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
    }
}

// ---- real guest threads ----
// Per-thread Dynarmic state. Main thread: t_active_jit == nullptr (uses jit_).
namespace {
thread_local Dynarmic::A32::Jit* t_active_jit = nullptr;
thread_local std::uint32_t t_tid = 0;
}  // namespace

Dynarmic::A32::Jit* Runtime::ActiveJit() {
    return t_active_jit ? t_active_jit : jit_;
}

std::uint32_t Runtime::GuestTid() {
    return t_tid ? t_tid : 1;
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
    Dynarmic::A32::Jit* aj = t_active_jit ? t_active_jit : nullptr;
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
    // D2: one containment query covers both old branches (an exact base hit is
    // just containment with base == addr).
    std::uint32_t cbase = 0, csize = 0;
    if (LiveIdxContaining(addr, &cbase, &csize) &&
        (std::uint64_t)addr + size <= (std::uint64_t)cbase + csize) {
        return false;  // fully inside a live allocation
    }
    // Quarantined (freed, held)? Distinct verdict: UAF write.
    for (const auto& q : quarantine_) {
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
    t_active_jit = jit;
    t_tid = tid;
}

bool Runtime::RunComparator(std::uint32_t compar, std::uint32_t pa, std::uint32_t pb,
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
                JitSafeOpts() ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
            cfg.enable_cycle_counting = kJitCycleCounting;
            if (std::getenv("HARNESS_FASTMEM")) {
                cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(mem().base());
            }
            static Dynarmic::ExclusiveMonitor cb_mon(8);
            cfg.global_monitor = &cb_mon;
            cfg.processor_id = (unsigned)(slot_idx & 7);
            slot->jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
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
    regs[13] = slot->stack + 0x200000u - 8;
    regs[14] = mod_.halt_va;
    regs[0] = pa;
    regs[1] = pb;
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
    Dynarmic::A32::Jit* outer = t_active_jit;
    t_active_jit = jit;
    Dynarmic::HaltReason hr = jit->Run();
    int redirs = 0;
    while (hr == Dynarmic::HaltReason::UserDefined1 && redirs++ < 4 && ApplyRedirect()) {
        hr = jit->Run();
    }
    t_active_jit = outer;
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
        return 12;  // ENOMEM
    }
    auto st = std::make_shared<ThreadState>();
    std::uint32_t tid = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        tid = next_tid_++;
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
    st->th = std::thread([this, st, tid, start, arg, stack]() {
        t_tid = tid;
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = cb_;
        cfg.optimizations =
            JitSafeOpts() ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
        cfg.enable_cycle_counting = kJitCycleCounting;
        if (std::getenv("HARNESS_FASTMEM")) {
            cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(mem().base());
        }
        static Dynarmic::ExclusiveMonitor shared_mon(8);
        cfg.global_monitor = &shared_mon;
        static std::atomic<unsigned> next_cpu{1};
        cfg.processor_id = next_cpu.fetch_add(1) % 8;
        if (cfg.processor_id == 0) {
            cfg.processor_id = 1;
        }
        Dynarmic::A32::Jit jit(cfg);
        t_active_jit = &jit;
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
        Dynarmic::HaltReason hr = jit.Run();
        // Throw-to-catch redirect: resume at the landing pad (cap loops).
        // NOTE: method on `this` (PthreadCreate is a Runtime member).
        int redirs = 0;
        while (hr == Dynarmic::HaltReason::UserDefined1 && redirs++ < 4 && ApplyRedirect()) {
            hr = jit.Run();
        }
        (void)hr;
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
        t_active_jit = nullptr;
    });
    st->th.detach();
    return 0;
}

// End-of-run census (see dispatch.h): detached workers can't be joined, so
// report tid/done/retval here instead of losing them to _exit.
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

// G2.6 (2026-10-18): per-tid state must not outlive the thread. Hands the errno
// slot back for reuse (the pool is finite) and drops the thread's TLS entries
// (keyed (key<<32)|tid). Idempotent.
void Runtime::ReapThreadTls(std::uint32_t tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = errno_slots_.find(tid);
    if (it != errno_slots_.end()) {
        errno_free_.push_back(it->second);
        errno_slots_.erase(it);
    }
    for (auto i = tls_map_.begin(); i != tls_map_.end();) {
        if (static_cast<std::uint32_t>(i->first & 0xFFFFFFFFu) == tid) {
            i = tls_map_.erase(i);
        } else {
            ++i;
        }
    }
}

std::uint32_t Runtime::PthreadJoin(std::uint32_t tid, std::uint32_t retval_out) {
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

std::uint32_t Runtime::WrapObject(void* real) {
    if (!real) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
#ifdef __ANDROID__
    // Product host: incoming refs are usually call-local (stale after the
    // JNI call returns; proven by stale-Local abort on reuse). Promote to
    // a global ref so the mock stays valid across calls. Identity is kept
    // by IsSameObject-scan over live globals (both sides valid — safe).
    if (JNIEnv* re = RealEnv()) {
        jobject g = re->NewGlobalRef(static_cast<jobject>(real));
        if (g) {
            for (const auto& kv : obj_rev_) {
                if (re->IsSameObject(static_cast<jobject>(kv.second), g)) {
                    re->DeleteGlobalRef(g);
                    return kv.first;
                }
            }
            const std::uint32_t h = next_obj_;
            next_obj_ += 4;
            obj_fwd_[g] = h;
            obj_rev_[h] = g;
            return h;
        }
        // NewGlobalRef failed: fall through to raw (mock path will cope).
    }
#endif
    const auto it = obj_fwd_.find(real);
    if (it != obj_fwd_.end()) {
        return it->second;
    }
    const std::uint32_t h = next_obj_;
    next_obj_ += 4;
    obj_fwd_[real] = h;
    obj_rev_[h] = real;
    return h;
}

void* Runtime::UnwrapObject(std::uint32_t mock) {
    if (!mock) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = obj_rev_.find(mock);
    return it != obj_rev_.end() ? it->second : nullptr;
}

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
    const bool is_static = fn >= 114;
    const int vtag = (fn == 114 || fn == 34) ? 0 : ((fn == 115 || fn == 35) ? 1 : 2);
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
        jclass lc = re->FindClass(cls.c_str());
        if (!lc || re->ExceptionCheck()) {
            re->ExceptionClear();
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
            re->ExceptionClear();
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        real_methods_[a2] = rmid;
    }
    jobject robj = nullptr;
    if (!is_static) {
        robj = static_cast<jobject>(UnwrapObject(a1));
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
        if (k == 'J' || k == 'D') {
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
                    jv[nj].d = dd;
                }
            }
        } else if (k == 'F') {
            std::uint32_t w = (vtag == 2) ? 0 : ((vtag == 0) ? next_word() : next_va());
            if (vtag == 2 && CheckRange(a3 + (std::uint32_t)nj * 8, 4, false)) {
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
                ro = static_cast<jobject>(UnwrapObject(w));
                if (!ro) {
                    // Mock string/array buffers (guest-side): materialize real.
                    if (CheckRange(w, 1, false)) {
                        std::size_t n = 0;
                        const auto* p = H<const std::uint8_t>(w);
                        while (n < 4096 && CheckRange(w + (std::uint32_t)n, 1, false) && p[n]) {
                            n++;
                        }
                        std::string s(reinterpret_cast<const char*>(p), n);
                        ro = re->NewStringUTF(s.c_str());
                    }
                    if (!ro) {
                        ok = false;
                        break;
                    }
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
            re->ExceptionClear();
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
                    re->ExceptionClear();
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
            re->ExceptionClear();
        }
        return true;
    }
    // Object/String/array: real call, then guest-usable result.
    {
        jobject ro = is_static ? re->CallStaticObjectMethodA(rclazz, rmid, jv)
                               : re->CallObjectMethodA(robj, rmid, jv);
        if (re->ExceptionCheck() || !ro) {
            re->ExceptionClear();
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
// V-form word order mirrors RealCallObject's next_va (sequential words;
// J/D = 2 words lo,hi; F = 1 word float bits, same quirk).
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
    if (cls.find("AudioOutput") == std::string::npos) {
        return false;
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
        jclass lc = re->FindClass(cls.c_str());
        if (!lc || re->ExceptionCheck()) {
            std::printf("[realnew] FindClass %s FAILED\n", cls.c_str());
            re->ExceptionClear();
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
            re->ExceptionClear();
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
        if (k == 'J' || k == 'D') {
            if (vtag == 1) {
                // Same AAPCS va_arg align-up as RealCallObject (pad word when
                // the va base is 4-mod-8).
                const std::uint32_t at = a3 + (word_idx - 3u) * 4u;
                if (at & 7u) {
                    next_w();
                }
            }
            const std::uint32_t lo = next_w();
            const std::uint32_t hi = next_w();
            if (k == 'J') {
                jv[nj].j = ((std::uint64_t)hi << 32) | lo;
            } else {
                const std::uint64_t u = ((std::uint64_t)hi << 32) | lo;
                double dd = 0;
                std::memcpy(&dd, &u, 8);
                jv[nj].d = dd;
            }
        } else if (k == 'F') {
            const std::uint32_t w = next_w();
            float f = 0;
            std::memcpy(&f, &w, 4);
            jv[nj].f = f;
        } else if (k == 'L' || k == '[') {
            const std::uint32_t w = next_w();
            jobject ro = nullptr;
            if (w) {
                ro = static_cast<jobject>(UnwrapObject(w));
                if (!ro && CheckRange(w, 1, false)) {
                    std::size_t n = 0;
                    const auto* p = H<const std::uint8_t>(w);
                    while (n < 4096 && CheckRange(w + (std::uint32_t)n, 1, false) && p[n]) {
                        n++;
                    }
                    std::string s(reinterpret_cast<const char*>(p), n);
                    ro = re->NewStringUTF(s.c_str());
                }
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
        re->ExceptionClear();
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
            jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
            ch = jni_obj_next_;
            jni_obj_next_ += 4;
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
    std::printf("[files] guest \"%s\" -> host \"%s\"\n", guest_prefix.c_str(), host_base.c_str());
}

void Runtime::SetApkPath(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    apk_path_ = path;
    apk_indexed_ = false;
    apk_index_.clear();
    std::printf("[asset] apk \"%s\"\n", path.c_str());
}

std::uint32_t Runtime::NewStringUTF(const char* s) {
    return JniNewString(s ? s : "");
}

std::uint32_t Runtime::JniNewString(const std::string& s) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::size_t len = s.size() + 1;
    // 4-align the bump pointer; keep inside guest-data (strings are short).
    jni_str_next_ = (jni_str_next_ + 3) & ~std::uint32_t(3);
    if (std::uint64_t(jni_str_next_) + len > std::uint64_t(kGuestDataBase) + kGuestDataSize) {
        std::printf("[jni] LOUD string arena exhausted -- halting\n");
        stop_step_ = true;
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    const std::uint32_t out = jni_str_next_;
    std::memcpy(H(out), s.c_str(), len);
    jni_str_next_ += static_cast<std::uint32_t>(len);
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
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
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
    // fn >= 0x100: JavaVM table (JNIInvokeInterface incl. reserved0-2):
    // 4 AttachCurrentThread, 6 GetEnv.
    if (fn >= 0x100) {
        const std::uint32_t jfn = fn - 0x100;
        if (jfn == 6 || jfn == 4) {  // GetEnv / AttachCurrentThread
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
    switch (fn) {
    // String thunks work off the jni_strings_ registry (mock content model).
    // Unknown handle (e.g. a method cookie misused as a string) = LOUD halt:
    // silent misreads here caused the 131K-throw saga once already.
    case 164: {  // GetStringLength(env, str) -> jsize (UTF-16 units)
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_strings_.find(a1);
        if (it == jni_strings_.end()) {
            std::printf("[jni] LOUD GetStringLength unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        Ret(regs, static_cast<std::uint32_t>(it->second.size()));  // ASCII: units==bytes
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
            const char* p = reinterpret_cast<const char*>(H(a1));
            std::size_t n = 0;
            while (n < 4096 && CheckRange(a1 + (std::uint32_t)n, 1, false) && p[n]) {
                n++;
            }
            s.assign(p, n);
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
            // then copy bytes into guest heap (guest-safe 32-bit pointer;
            // free is a no-op so Release stays trivial).
            if (void* real = UnwrapObject(a1)) {
                if (JNIEnv* re = RealEnv()) {
                    const char* p = re->GetStringUTFChars(static_cast<jstring>(real),
                                                           nullptr);
                    if (p) {
                        const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(p));
                        const std::uint32_t g = GuestMalloc(n + 1);
                        if (g) {
                            std::memcpy(H(g), p, n + 1);
                            // Intern (caller holds mutex_): see above.
                            jni_strings_[g] = std::string(p, n);
                            if (a2 && CheckRange(a2, 4, true)) {
                                const std::uint32_t one = 1;
                                std::memcpy(H(a2), &one, 4);
                            }
                            Ret(regs, g);
                            return;
                        }
                    }
                }
            }
#endif
            std::printf("[jni] LOUD GetStringUTFChars unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (a2 && CheckRange(a2, 4, true)) {
            std::uint32_t no = 0;  // JNI_FALSE: we return a direct pointer
            std::memcpy(H(a2), &no, 4);
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
        if (it == jni_strings_.end()) {
            std::printf("[jni] LOUD region unknown str %08x -- halting\n", a1);
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        const std::string& s = it->second;
        if (std::uint64_t(a2) + a3 > s.size()) {
            std::printf("[jni] LOUD region [%u+%u) over len %zu -- halting\n", a2, a3, s.size());
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
            for (std::uint32_t i = 0; i < a3 && (i + 1) * 2 <= wlen; i++) {
                const std::uint16_t c = static_cast<std::uint8_t>(s[a2 + i]);  // ASCII widen
                std::memcpy(H(buf + i * 2), &c, 2);
            }
        } else {
            if (!CheckRange(buf, a3, true)) {
                std::printf("[jni] region BAD buf %08x -- halting\n", buf);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            const std::uint32_t wlen = ClampHeapWrite(buf, a3, "GetStringUTFRegion", true);
            std::memcpy(H(buf), s.data() + a2, wlen);
        }
        {
            // Triage (capped): name the string, the destination and the size we
            // believe the destination allocation has. alloc=0 = not a heap
            // block (jni arena / static) -> ClampHeapWrite left it unclamped.
            static int rn = 0;
            if (rn < 8) {
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
        Ret(regs, 0);  // void JNI: zero r0 (device Dalvik leaves 0; game reads it)
        return;
    }
    case 24: {  // IsSameObject(env, obj1, obj2) -> jboolean
        Ret(regs, (a1 == a2) ? 1u : 0u);  // covers NULL==NULL; handles are VAs
        return;
    }
    case 31: {  // GetObjectClass(env, obj) -> jclass (verified vs NDK jni.h)
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint32_t ch = 0;
        const auto it = jni_objclass_.find(a1);
        if (it != jni_objclass_.end()) {
            ch = it->second;
        } else {
            // Unknown object (e.g. jstring/array): fabricate java/lang/Object.
            for (const auto& kv : jni_classes_) {
                if (kv.second == "java/lang/Object") {
                    ch = kv.first;
                    break;
                }
            }
            if (!ch) {
                jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
                ch = jni_obj_next_;
                jni_obj_next_ += 4;
                jni_classes_[ch] = "java/lang/Object";
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
    case 23:   // DeleteLocalRef(env, obj) -> void: mock handles need no GC
    case 22:   // DeleteGlobalRef(env, obj) -> void (same)
    case 227:  // DeleteWeakGlobalRef(env, ref) -> void (same)
        // PROVEN load-bearing: caller @0x267FD0 treats r0!=0 as error and
        // throws "Java method threw an exception". Zero it like Dalvik does.
        Ret(regs, 0);
        return;
    case 25:   // NewLocalRef(env, ref) -> same handle (no GC in mock world)
    case 21:   // NewGlobalRef(env, obj) -> same handle
    case 226:  // NewWeakGlobalRef(env, obj) -> same handle
        Ret(regs, a1);
        return;
    case 17:  // ExceptionClear(env) -> void (mock never raises)
        Ret(regs, 0);
        return;
    case 15:  // ExceptionOccurred(env) -> NULL (mock never raises)
        Ret(regs, 0);
        return;
    case 228:  // ExceptionCheck(env) -> jboolean FALSE (mock never raises)
        Ret(regs, 0);
        return;
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
        jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
        const std::uint32_t o = jni_obj_next_;
        jni_obj_next_ += 4;
        if (std::uint64_t(o) + 4 > std::uint64_t(kGuestDataBase) + kGuestDataSize) {
            std::printf("[jni] LOUD object arena exhausted -- halting\n");
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
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
                jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
                ch = jni_obj_next_;
                jni_obj_next_ += 4;
                jni_classes_[ch] = cname;
            }
            jni_objclass_[o] = ch;
        }
        const char* vtag = (fn == 28) ? "" : ((fn == 29) ? "V" : "A");
        if (DiagOn()) {
            std::printf("[jni] NewObject%s %s -> %08x\n", vtag, mdesc.c_str(), o);
        }
        // TEMP-DIAG (audio): capture the native audio handle. NewObjectV
        // varargs live at the va_list pointer (a3), NOT at sp. Recorded
        // for HARNESS_JLAST/JOVR "J" expansion (mixdata synthesis).
        if (mdesc.find("AudioOutput.<init>") != std::string::npos) {
            std::uint32_t w0 = 0, w1 = 0;
            if (CheckRange(a3, 4, false)) {
                std::memcpy(&w0, H(a3), 4);
            }
            if (CheckRange(a3 + 4, 4, false)) {
                std::memcpy(&w1, H(a3 + 4), 4);
            }
            audio_j_lo_ = w0;
            audio_j_hi_ = w1;
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
#endif
        if (mdesc.size() >= 20 &&
            mdesc.compare(mdesc.size() - 20, 20, "()Ljava/lang/String;") == 0) {
            std::string content;
            if (mdesc.find("getPathToFileCacheDirectory") != std::string::npos) {
                content = "/data/data/com.rovio.angrybirdsrio/files";
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
            // networkType must be a valid token ("" stalls network-state
            // waits). NONE = offline degrade (no socket attempts against our
            // stubs, which fake success and could hang); HARNESS_NETTYPE
            // overrides (e.g. WIFI if real socket forwarding lands).
            if (mdesc.find("networkType") != std::string::npos) {
                content = "NONE";
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
        jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
        const std::uint32_t o = jni_obj_next_;
        jni_obj_next_ += 4;
        if (std::uint64_t(o) + 4 > std::uint64_t(kGuestDataBase) + kGuestDataSize) {
            std::printf("[jni] LOUD object arena exhausted -- halting\n");
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
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
        // The game does NOT null-check (Build.MANUFACTURER NULL -> instant
        // GetStringLength(NULL) death), so never return NULL here.
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
        jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
        const std::uint32_t o = jni_obj_next_;
        jni_obj_next_ += 4;
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
        // Offline default: 0/NULL for every static field (logged, tunable per
        // name, e.g. android/location/Criteria.ACCURACY_COARSE).
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
    case 171: {  // GetArrayLength(env, array) -> jsize        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = jni_array_lens_.find(a1);
        if (it == jni_array_lens_.end()) {
            std::printf("[jni] LOUD GetArrayLength unknown array %08x -- halting\n", a1);
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        Ret(regs, it->second);
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
        // TEMP-DIAG (mock audit): log the feature string arg (jstring).
        // V/A pass args behind a pointer (va_list / jvalue*); plain reads
        // the jstring handle from the stack.
        std::string farg;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            std::uint32_t h = 0;
            if (fn == 118 || fn == 119) {
                if (CheckRange(a3, 4, false)) {
                    std::memcpy(&h, H(a3), 4);
                }
            } else if (CheckRange(regs[13], 4, false)) {
                std::memcpy(&h, H(regs[13]), 4);
            }
            const auto it = h ? jni_strings_.find(h) : jni_strings_.end();
            if (it != jni_strings_.end()) {
                farg = it->second;
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
    case 52:  // CallLongMethod(V/A)(env, obj, method, ...) -> jlong
    case 53:   // (verified vs NDK jni.h; instance-int is 49/50/51, never hit)
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
                        std::lock_guard<std::recursive_mutex> lock(mutex_);
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
                        if (a2 && CheckRange(a2, 4, true)) {
                            const std::uint32_t t = 1;  // JNI_TRUE: copy
                            std::memcpy(H(a2), &t, 4);
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
        if (a2 && CheckRange(a2, 4, true)) {
            std::uint32_t no = 0;  // JNI_FALSE: direct pointer, no copy
            std::memcpy(H(a2), &no, 4);
        }
        Ret(regs, a1);  // handle IS the buffer
        return;
    }
    case 192: {  // ReleaseByteArrayElements(env, array, elems, mode) -> void
#ifdef __ANDROID__
        // Pinned real array: copy back per mode, unpin.
        {
            ArrayPin pin;
            bool have = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                const auto it = array_pins_.find(a1);
                if (it != array_pins_.end()) {
                    pin = it->second;
                    array_pins_.erase(it);
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
                if (pin.guest_buf && pin.len) {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
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
        // Discovery logger: names the dynamic natives (e.g. the Java-driven
        // Lua pump). Harness-only; returns JNI_OK. Never ships.
        std::string cname = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_classes_.find(a1);
            if (it != jni_classes_.end()) {
                cname = it->second;
            }
        }
        for (std::uint32_t k = 0; k < a3 && k < 256; k++) {
            if (!CheckRange(a2 + k * 12, 12, false)) {
                break;
            }
            std::uint32_t nm = 0, sg = 0, fp = 0;
            std::memcpy(&nm, H(a2 + k * 12), 4);
            std::memcpy(&sg, H(a2 + k * 12 + 4), 4);
            std::memcpy(&fp, H(a2 + k * 12 + 8), 4);
            std::string ns = "?", ss = "?";
            if (nm && CheckRange(nm, 1, false)) {
                ns = GuestStr(nm);
            }
            if (sg && CheckRange(sg, 1, false)) {
                ss = GuestStr(sg);
            }
            std::printf("[jni] RegisterNatives %s.%s%s -> %08x\n", cname.c_str(),
                        ns.c_str(), ss.c_str(), fp);
        }
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
        jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
        const std::uint32_t h = jni_obj_next_;
        jni_obj_next_ += 4;
        if (std::uint64_t(h) + 4 > std::uint64_t(kGuestDataBase) + kGuestDataSize) {
            std::printf("[jni] LOUD handle arena exhausted -- halting\n");
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
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
        std::string cname = "?";
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            const auto it = jni_classes_.find(a1);
            if (it != jni_classes_.end()) {
                cname = it->second;
            }
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
        const std::uint32_t c = jni_obj_next_;
        jni_obj_next_ += 4;
        if (std::uint64_t(c) + 4 > std::uint64_t(kGuestDataBase) + kGuestDataSize) {
            std::printf("[jni] LOUD handle arena exhausted -- halting\n");
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        std::memcpy(H(c), &c, 4);
        jni_methods_[c] =
            (fn == 33 ? std::string("")
                      : fn == 113 ? std::string("static ")
                                  : std::string("field ")) +
            cname + "." + mname + msig;
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
                const std::size_t dot = d.rfind('.');
                const std::size_t par = d.find('(');
                // Field desc shape: "Cls.name(sig)" (no parens; tolerate).
                std::string cls, name, sig;
                if (dot != std::string::npos) {
                    cls = d.substr(0, dot);
                    const std::string rest = d.substr(dot + 1);
                    const std::size_t lp = rest.find('(');
                    if (lp != std::string::npos) {
                        name = rest.substr(0, lp);
                        sig = rest.substr(lp);
                    } else {
                        name = rest;
                    }
                }
                // Field sig follows the name directly ("name[Sig") when the
                // mock cookie was built as name+sig. Split trailing type.
                if (!name.empty() && sig.empty()) {
                    const std::size_t tp = name.find_first_of("ZBCDFIJSV[(");
                    if (tp != std::string::npos && tp > 0) {
                        sig = name.substr(tp);
                        name = name.substr(0, tp);
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
    std::printf("[svc] LOUD unimplemented JNI fn#%u from pc=%08x -- halting\n", fn, regs[15] - 4);
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
std::uint32_t g_sess_watch = 0;  // session-flag watch (armed by DumpR9)

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
    if (!rt_->CheckAccess(addr, sizeof(T), false, exec)) {
        if (rt_->NoteBad()) {
            std::printf("[mem][t%u] BAD %s @ %08x size %zu guest-pc=%08x lr=%08x r4=%08x\n",
                        t_tid, exec ? "exec" : "read", addr, sizeof(T),
                        rt_->ActiveJit()->Regs()[15], rt_->ActiveJit()->Regs()[14],
                        rt_->ActiveJit()->Regs()[4]);
            // TEMP-DIAG (sessions epilogue crash): hexdump around r4 (r8+40
            // shared_ptr block) to see the struct layout.
            {
                const std::uint32_t badr4 = rt_->ActiveJit()->Regs()[4];
                if (rt_->CheckRange(badr4 - 64, 80, false)) {
                    const std::uint8_t* p = rt_->H<const std::uint8_t>(badr4 - 64);
                    std::printf("[mem] r4-64:");
                    for (int j = 0; j < 80; j++) {
                        std::printf(" %02x", p[j]);
                    }
                    std::printf("\n");
                }
            }
            rt_->DumpEventRing();
        }
        rt_->stop_step_ = true;
        rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    T v = 0;
    const std::uint8_t* p = rt_->H<const std::uint8_t>(addr);
    for (std::size_t i = 0; i < sizeof(T); i++) {
        v |= T(p[i]) << (8 * i);
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
            std::printf("[wpr] %08x loaded from %08x guest-pc=%08x\n", v, addr, regs[15]);
            std::printf("[wpr] regs:");
            for (int r = 0; r < 16; r++) {
                std::printf(" r%d=%08x", r, regs[r]);
            }
            std::printf("\n");
        }
        const std::int64_t wa = WatchAddr();
        if (wa >= 0 && addr == static_cast<std::uint32_t>(wa)) {
            std::printf("[wpa] read from %08x = %08x guest-pc=%08x\n", addr, v,
                        rt_->ActiveJit()->Regs()[15]);
        }
        // Session-flag watch (DumpR9-armed): who observes sessions?
        if (g_sess_watch != 0 && addr == g_sess_watch) {
            std::printf("[sesswatch] t=%u read from %08x = %08x guest-pc=%08x\n", t_tid, addr,
                        v, rt_->ActiveJit()->Regs()[15]);
        }
    }
    // Same for sub-word accesses (flag protocol uses strb/ldrb!).
    if (g_sess_watch != 0 && sizeof(T) != 4 && addr == g_sess_watch) {
        std::printf("[sesswatch] t=%u read%zu from %08x = %08x guest-pc=%08x\n", t_tid,
                    sizeof(T), addr, (std::uint32_t)v, rt_->ActiveJit()->Regs()[15]);
    }
    return v;
}

template <typename T>
void GuestCallbacks::Write(VAddr addr, T v) {
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
    if (g_sess_watch != 0 && addr == g_sess_watch) {
        std::printf("[sesswatch-w] t=%u write to %08x = %08x guest-pc=%08x\n", t_tid, addr,
                    static_cast<std::uint32_t>(v), rt_->ActiveJit()->Regs()[15]);
    }
    if (g_sess_watch != 0 && sizeof(T) != 4 && addr == g_sess_watch) {
        std::printf("[sesswatch-w] t=%u write%zu to %08x = %08x guest-pc=%08x\n", t_tid,
                    sizeof(T), addr, static_cast<std::uint32_t>(v),
                    rt_->ActiveJit()->Regs()[15]);
    }
    // TEMP-EXP (sessions race): first 1 wins. Sessions #1 (success) and #2
    // (offline-fail) share one object; #2's 0 clobbers #1's 1 and kills the
    // cond predicate. Latch 1 (best-effort, triage only).
    if (g_sess_watch != 0 && addr == g_sess_watch) {
        static bool latched = false;
        if ((std::uint32_t)v == 1 || (sizeof(T) == 1 && (v & 0xFFu) == 1)) {
            latched = true;
        } else if (latched) {
            const std::uint8_t one = 1;
            std::memcpy(rt_->H(g_sess_watch), &one, 1);
            std::printf("[sesswatch] latch restored 1 (t=%u)\n", t_tid);
        }
    }
    std::uint8_t* p = rt_->H<std::uint8_t>(addr);
    for (std::size_t i = 0; i < sizeof(T); i++) {
        p[i] = std::uint8_t(v >> (8 * i));
    }
}

std::uint8_t GuestCallbacks::MemoryRead8(VAddr a) { return Read<std::uint8_t>(a, false); }
std::uint16_t GuestCallbacks::MemoryRead16(VAddr a) { return Read<std::uint16_t>(a, false); }
std::uint32_t GuestCallbacks::MemoryRead32(VAddr a) { return Read<std::uint32_t>(a, false); }
std::uint64_t GuestCallbacks::MemoryRead64(VAddr a) { return Read<std::uint64_t>(a, false); }
GuestCallbacks* GuestCallbacks::g_probe_cb = nullptr;
void* (*GuestCallbacks::g_real_env_hook)() = nullptr;
std::uint32_t GuestCallbacks::g_probe_pc = 0;
void GuestCallbacks::ProbeDump2(std::uint64_t r2, std::uint64_t r3, std::uint64_t sp) {
    std::printf("[probe] r2=%08x r3=%08x sp=%08x\n", (std::uint32_t)r2, (std::uint32_t)r3,
                (std::uint32_t)sp);
    // Stack-string peek: 16B at [sp+16] (resolver OUT / locals forensics).
    // Plus 16B at [sp+4] (often the compared key: 0x2B0A3C uses sp+4).
    {
        GuestCallbacks* cb = g_probe_cb;
        for (int k = 0; k < 2; k++) {
            std::uint32_t so = (std::uint32_t)sp + (k == 0 ? 16u : 4u);
            if (!cb || !cb->rt_->CheckRange(so, 16, false)) {
                continue;
            }
            const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(so);
            std::printf("[probe] [sp+%s]:", k == 0 ? "16" : "4");
            for (int j = 0; j < 16; j++) {
                std::printf(" %02x", p[j]);
            }
            std::printf("  |");
            for (int j = 0; j < 16; j++) {
                std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
            }
            std::printf("|\n");
        }
        {
            std::uint32_t so = (std::uint32_t)sp + 16;
            (void)so;
        }
        // r3 peek: 16B (often the compared-against string).
        {
            std::uint32_t ra = (std::uint32_t)r3;
            if (cb && cb->rt_->CheckRange(ra, 16, false)) {
                const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(ra);
                std::printf("[probe] [r3]:");
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
    }
    // HARNESS_PROBE=<pc>,fill: if the 12B SSO string at [sp+28] is empty
    // (all-zero, i.e. a missed typed-map lookup left OUT untouched), write
    // "{}" (04 7B 7D): lets the JSON parser proceed with defaults instead
    // of throwing Unexpected EOF on an empty range. Write-only-when-empty,
    // range-checked; no guest code patched.
    {
        static std::int64_t fill = -2;
        if (fill == -2) {
            fill = 0;
            if (const char* pe = std::getenv("HARNESS_PROBE")) {
                if (std::strstr(pe, "fill")) {
                    fill = 1;
                }
            }
        }
        if (fill > 0) {
            GuestCallbacks* cb = g_probe_cb;
            std::uint32_t so = (std::uint32_t)sp + 28;
            // Fill is only valid at the 0x1A410C return site (OUT = sp+28 of
            // the 0x1A40B0 frame). Anywhere else [sp+28] is unrelated stack.
            if (cb && g_probe_pc == 0x1A410Cu &&
                cb->rt_->CheckRange(so, 12, true)) {
                std::uint8_t cur[12];
                std::memcpy(cur, cb->rt_->H<const std::uint8_t>(so), 12);
                bool empty = true;
                for (int i = 0; i < 12; i++) {
                    if (cur[i]) {
                        empty = false;
                        break;
                    }
                }
                if (empty) {
                    static const std::uint8_t js[12] = {0x04, 0x7B, 0x7D, 0x00, 0x00, 0x00,
                                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                    std::memcpy(cb->rt_->H<std::uint8_t>(so), js, 12);
                    std::printf("[probe] filled {} at %08x\n", so);
                } else {
                    std::printf("[probe] OUT non-empty, kept\n");
                }
            }
        }
    }
}
bool g_succ_done_hack = false;

void GuestCallbacks::DumpR9(std::uint64_t r9, std::uint64_t, std::uint64_t) {
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    const std::uint32_t o = (std::uint32_t)r9;
    // Arm the session-flag watch (first session object wins): the Read
    // path logs every 4B read of [obj+120] with pc+tid (who observes?).
    {
        extern std::uint32_t g_sess_watch;
        if (g_sess_watch == 0 && cb->rt_->CheckRange(o + 120, 4, false)) {
            g_sess_watch = o + 120;
            std::printf("[dumpr9] watch armed @%08x\n", g_sess_watch);
        }
    }
    static int dn = 0;
    if (dn < 4 && cb->rt_->CheckRange(o, 128, false)) {
        dn++;
        std::uint8_t fl = 0;
        if (cb->rt_->CheckRange(o + 120, 1, false)) {
            std::memcpy(&fl, cb->rt_->H(o + 120), 1);
        }
        const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(o);
        std::printf("[dumpr9] obj=%08x flag120=%u head:", o, fl);
        for (int j = 0; j < 32; j++) {
            std::printf(" %02x", p[j]);
        }
        std::printf("\n");
    }
}

// HARNESS_EVENTPOST: synthetic pi#1 + array, built once at first faked
// walk entry (translation of later blocks bakes pi#1, so it must exist by
// then; entry always executes first). Same addrs forever: no staleness.
static std::uint32_t s_eventpost_pi1 = 0;
static std::uint32_t s_eventpost_arr = 0;

void GuestCallbacks::EventDecide(std::uint64_t r0pi, std::uint64_t, std::uint64_t) {
    // HARNESS_EVENTPOST=1: at 0xC465C the walk has r0=r6=natural pi (mov ran).
    // Refresh pi#1: mirror the natural pi when usable, else synthetic content.
    // The baked SetRegister(r6, pi#1) right after this call steers the walk.
    if (!std::getenv("HARNESS_EVENTPOST")) {
        return;
    }
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    Runtime* rt = cb->rt_;
    const std::uint32_t nat = (std::uint32_t)r0pi;
    const std::uint32_t pi1 = s_eventpost_pi1;
    static int enote = 0;
    if (pi1 == 0 || !rt->CheckRange(pi1, 16, true)) {
        if (enote < 2) {
            enote++;
            std::printf("[eventpost] pi#1 not ready\n");
        }
        return;
    }
    // Natural usable? non-null, readable, sane begin/end, any busy state
    // (busy is mirrored: natural alt-path behavior preserved).
    bool use_nat = false;
    std::uint32_t f[4] = {};
    if (nat != 0 && rt->CheckRange(nat, 16, false)) {
        std::memcpy(f, rt->H(nat), 16);
        if (f[0] != f[1] && rt->CheckRange(f[0], 4, false) &&
            (f[1] == f[0] + 4 || rt->CheckRange(f[1], 4, false))) {
            use_nat = true;
        }
    }
    if (use_nat) {
        std::memcpy(rt->H(pi1), f, 16);
        static bool stood = false;
        if (!stood) {
            stood = true;
            std::printf("[eventpost] NATURAL pi mirrored b=%08x e=%08x busy=%08x\n",
                        f[0], f[1], f[3]);
        }
        return;
    }
    // Synthetic: one element (built once in EventEntry; same addrs forever).
    {
        const std::uint32_t z = 0;
        std::uint32_t b = 0, e = 0;
        std::memcpy(&b, rt->H(pi1), 4);
        std::memcpy(&e, rt->H(pi1 + 4), 4);
        if (b != s_eventpost_arr || e != s_eventpost_arr + 4) {
            std::memcpy(rt->H(pi1), &s_eventpost_arr, 4);
            std::uint32_t eend = s_eventpost_arr + 4;
            std::memcpy(rt->H(pi1 + 4), &eend, 4);
            std::memcpy(rt->H(pi1 + 8), &z, 4);
            static int efn = 0;
            if (efn < 3) {
                efn++;
                std::printf("[eventpost] synthetic pi#1 armed\n");
            }
        }
        std::memcpy(rt->H(pi1 + 12), &z, 4);  // busy = 0 every walk
    }
}

void GuestCallbacks::LastThis(std::uint64_t r0obj, std::uint64_t, std::uint64_t) {
    // HARNESS_LASTTHIS=1: record the loading-builder r0 (scene object) at
    // PROBE4 for HARNESS_BUILDER (never hardcoded, drifts per run).
    if (!std::getenv("HARNESS_LASTTHIS")) {
        return;
    }
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    cb->rt_->last_this_ = (std::uint32_t)r0obj;
}

static int wseq_shared = 0;
static std::uint32_t wpc_shared = 0;
void GuestCallbacks::WatchPC(std::uint64_t r0, std::uint64_t r1, std::uint64_t lr) {
    // HARNESS_WATCHPC=<pc>: regs at pc (part 1/2; part 2 = WatchSP).
    // Capped at 4 fires. Own env/pc, consumes no probe slot.
    static int wn = 0;
    if (wn >= 4) {
        return;
    }
    wn++;
    wseq_shared++;
    std::printf("[watchpc] #%d r0=%08x r1=%08x lr=%08x\n", wseq_shared, (std::uint32_t)r0,
                (std::uint32_t)r1, (std::uint32_t)lr);
    // Session stash for the login trigger: D5994 entry with a live object.
    // (Updates pass the real session-ish object here; the forged account
    // needs it linked at +32 or the subtree faults on null+off.)
    if (wpc_shared == 0xD5994u && (std::uint32_t)r0 != 0) {
        GuestCallbacks* cb2 = g_probe_cb;
        if (cb2 && cb2->rt_->loginat_session_ == 0) {
            cb2->rt_->loginat_session_ = (std::uint32_t)r0;
            std::printf("[watchpc] stashed session=%08x\n", (std::uint32_t)r0);
        }
    }
    // Live-object snapshot for faithful forging (HARNESS_WATCHPCSNAP=<hexlen>):
    // copies [r0, r0+len) at first hit into rt_ (drift-immune, unlike
    // post-mortem HEXDUMP of a hardcoded addr). Harness-only diag.
    {
        static bool sninit = false;
        static std::uint32_t snlen = 0;
        if (!sninit) {
            sninit = true;
            if (const char* e = std::getenv("HARNESS_WATCHPCSNAP")) {
                snlen = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
                if (snlen > 64) {
                    snlen = 64;
                }
            }
        }
        GuestCallbacks* cb3 = g_probe_cb;
        if (snlen && cb3 && cb3->rt_->loginat_acct_ == 0) {
            const std::uint32_t ao = (std::uint32_t)r0;
            if (ao && cb3->rt_->CheckRange(ao, snlen, false)) {
                cb3->rt_->loginat_acct_ = ao;
                cb3->rt_->loginat_acct_len_ = snlen;
                std::memcpy(cb3->rt_->loginat_acct_snap_,
                            cb3->rt_->H<const std::uint8_t>(ao), snlen);
                std::printf("[watchpc] snap %uB @%08x:", snlen, ao);
                for (std::uint32_t j = 0; j < snlen; j++) {
                    std::printf(" %02x", cb3->rt_->loginat_acct_snap_[j]);
                }
                std::printf("\n");
            } else {
                std::printf("[watchpc] snap BAD range %08x+%u\n", ao, snlen);
            }
        }
    }
}
void GuestCallbacks::WatchSP(std::uint64_t sp, std::uint64_t, std::uint64_t) {
    // Part 2/2: stack code-pointer backtrace for the seq from WatchPC.
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    const std::uint32_t sp0 = (std::uint32_t)sp;
    std::printf("[watchpc] #%d frames:", wseq_shared);
    int n = 0;
    for (std::uint32_t a = sp0; a < sp0 + 4096 && n < 12; a += 4) {
        if (!cb->rt_->CheckRange(a, 4, false)) {
            break;
        }
        std::uint32_t w = 0;
        std::memcpy(&w, cb->rt_->H(a), 4);
        if (w < 0x1000 || w >= 0x653D1C) {
            continue;
        }
        if (!cb->rt_->CheckRange(w - 4, 4, false)) {
            continue;
        }
        std::uint32_t prev = 0;
        std::memcpy(&prev, cb->rt_->H(w - 4), 4);
        if ((prev >> 25) == 0x75) {
            std::printf(" %08x", w);
            n++;
        }
    }
    std::printf("\n");
}

void GuestCallbacks::EventEntry(std::uint64_t r1slot, std::uint64_t, std::uint64_t) {
    // HARNESS_EVENTPOST=1: at event-walk entry (0xC45FC, r1 = event slot),
    // fake a nonzero subscriber count while the natural one is 0 so the walk
    // reaches the pi acquisition (BL 0xC4654) where the pi is acquired (may be NULL).
    // Stands down (loudly) on any natural count. Run-scoped, logged.
    if (!std::getenv("HARNESS_EVENTPOST")) {
        return;
    }
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    Runtime* rt = cb->rt_;
    const std::uint32_t slot = (std::uint32_t)r1slot;
    static int enote = 0;
    if (!rt->CheckRange(slot, 4, false)) {
        if (enote < 2) {
            enote++;
            std::printf("[eventpost] slot %08x unreadable\n", slot);
        }
        return;
    }
    std::uint32_t impl = 0;
    std::memcpy(&impl, rt->H(slot), 4);
    if (impl == 0 || !rt->CheckRange(impl + 16, 4, true)) {
        if (enote < 2) {
            enote++;
            std::printf("[eventpost] no impl\n");
        }
        return;
    }
    std::uint32_t cnt = 0;
    std::memcpy(&cnt, rt->H(impl + 16), 4);
    if (cnt != 0) {
        static bool stood = false;
        if (!stood) {
            stood = true;
            std::printf("[eventpost] NATURAL count=%u, standing down\n", cnt);
        }
        return;
    }
    const std::uint32_t one = 1;
    std::memcpy(rt->H(impl + 16), &one, 4);
    // Build synthetic objects once (before any later block translates: the
    // 0xC465C override bakes pi#1 at translation time).
    if (s_eventpost_pi1 == 0) {
        std::uint32_t efree = 0;
        for (const auto& im : rt->mod().imports) {
            if (im.name == "free") {
                efree = im.tramp_va;
                break;
            }
        }
        std::uint32_t pi1 = rt->GuestCalloc(1, 16);
        std::uint32_t arr = rt->GuestCalloc(1, 4);
        std::uint32_t elem = rt->GuestCalloc(1, 0x24);
        std::uint32_t call = rt->GuestCalloc(1, 8);
        std::uint32_t vt = rt->GuestCalloc(1, 0x20);
        if (efree && pi1 && arr && elem && call && vt) {
            std::memcpy(rt->H(elem + 8), &one, 4);      // elem flag
            std::memcpy(rt->H(elem + 0x20), &call, 4);  // elem callable
            std::memcpy(rt->H(call), &vt, 4);           // callable vtable
            std::memcpy(rt->H(vt + 0x18), &efree, 4);   // vt+0x18 = free thunk
            std::memcpy(rt->H(arr), &elem, 4);          // arr[0] = elem
            s_eventpost_pi1 = pi1;
            s_eventpost_arr = arr;
            rt->eventpost_callable_ = call;
            std::printf("[eventpost] built pi1=%08x arr=%08x elem=%08x call=%08x vt=%08x free=%08x\n",
                        pi1, arr, elem, call, vt, efree);
        } else {
            std::printf("[eventpost] build failed\n");
        }
    }
    if (enote < 3) {
        enote++;
        std::uint32_t pc = rt->ActiveJit()->Regs()[15];
        std::printf("[eventpost] count faked 0->1 impl=%08x (at pc=%08x)\n", impl, pc);
    }
}

void GuestCallbacks::PokeManagerFlag(std::uint64_t r10obj, std::uint64_t, std::uint64_t) {
    // HARNESS_POKE164=1: [loading-manager+164]=0 (menu branch at 0x283C34).
    // Gated on HARNESS_RETSUCC success (a flag flip before content exists
    // just whites the screen: loading must build first, menu after).
    GuestCallbacks* cb = g_probe_cb;
    if (!cb || !g_succ_done_hack) {
        return;
    }
    const std::uint32_t o = (std::uint32_t)r10obj;
    if (!cb->rt_->CheckRange(o + 164, 4, false)) {
        return;
    }
    cb->rt_->last_mgr_ = o;  // HARNESS_SCENEAT reads the manager from here.
    std::uint32_t v = 0;
    std::memcpy(&v, cb->rt_->H(o + 164), 4);
    cb->rt_->last_mgr164_ = v;  // TAPBEAT governor (2=loading: hold beats).
    // TEMP-DIAG (always on when hooked): identify the loading-manager object
    // and its +164/+1187 over time. The FLIP below stays POKE164-gated.
    {
        static int pd = 0;
        static std::uint32_t lastv = 0xFFFFFFFFu;
        // Log first 2 fires + every CHANGE of +164 (natural transitions).
        if ((pd < 2 || v != lastv) && pd < 40) {
            pd++;
            lastv = v;
            std::uint8_t b1187 = 0;
            std::uint32_t s168 = 0;
            if (cb->rt_->CheckRange(o + 1187, 1, false)) {
                std::memcpy(&b1187, cb->rt_->H(o + 1187), 1);
            }
            if (cb->rt_->CheckRange(o + 168, 4, false)) {
                std::memcpy(&s168, cb->rt_->H(o + 168), 4);
            }
            // TEMP-DIAG (scene recipe): full manager + scene-vector state
            // (compare vs factory recipe: +172/+176/+216 nonzero? +180/+184
            // empty? scene[+188]/[+192] begin/end?).
            std::uint32_t f172 = 0, f176 = 0, f216 = 0, f180 = 0, f184 = 0;
            std::uint32_t sb = 0, se = 0;
            if (cb->rt_->CheckRange(o + 220, 4, false)) {
                std::memcpy(&f172, cb->rt_->H(o + 172), 4);
                std::memcpy(&f176, cb->rt_->H(o + 176), 4);
                std::memcpy(&f216, cb->rt_->H(o + 216), 4);
                std::memcpy(&f180, cb->rt_->H(o + 180), 4);
                std::memcpy(&f184, cb->rt_->H(o + 184), 4);
            }
            if (s168 && cb->rt_->CheckRange(s168 + 196, 4, false)) {
                std::memcpy(&sb, cb->rt_->H(s168 + 188), 4);
                std::memcpy(&se, cb->rt_->H(s168 + 192), 4);
            }
            std::printf("[diag164] obj=%08x +164=%08x +1187=%u +168=%08x succ=%d\n", o, v,
                        b1187, s168, (int)g_succ_done_hack);
            std::printf("[diag164] +172=%08x +176=%08x +216=%08x +180=%08x +184=%08x "
                        "scene188=[%08x..%08x]\n",
                        f172, f176, f216, f180, f184, sb, se);
            // TEMP-DIAG (node layout): dump the vector's first entry (node
            // pointer) + the node's head (vtable? id field? slots?).
            if (sb && se > sb && cb->rt_->CheckRange(sb, 4, false)) {
                std::uint32_t nptr = 0;
                std::memcpy(&nptr, cb->rt_->H(sb), 4);
                std::uint32_t nb[8] = {};
                if (nptr && cb->rt_->CheckRange(nptr, 32, false)) {
                    std::memcpy(nb, cb->rt_->H(nptr), 32);
                }
                std::printf("[diag164] vec0 node=%08x [node]=%08x,%08x,%08x,"
                            "%08x +16=%08x,%08x,%08x,%08x\n",
                            nptr, nb[0], nb[1], nb[2], nb[3], nb[4], nb[5],
                            nb[6], nb[7]);
            }
        }
    }
    // TEMP-DIAG (update bail): dump the Gate#1 singleton *0x672E98.
    {
        static int pg1 = 0;
        if (pg1 < 2 && cb->rt_->CheckRange(0x672E98, 4, false)) {
            pg1++;
            std::uint32_t g = 0;
            std::memcpy(&g, cb->rt_->H(0x672E98), 4);
            std::printf("[poke164] gate1 singleton *0x672E98=%08x\n", g);
        }
    }
    // TEMP-DIAG (update bail): Gate#3 byte [manager+1187] (must be 1).
    {
        static int pg3 = 0;
        if (pg3 < 2 && cb->rt_->CheckRange(o + 1187, 1, false)) {
            pg3++;
            std::uint8_t b = 0;
            std::memcpy(&b, cb->rt_->H(o + 1187), 1);
            std::printf("[poke164] gate3 [manager+1187]=%u\n", b);
        }
    }
    {
        static int pg = 0;
        if (pg < 2 && cb->rt_->CheckRange(o + 168, 4, false)) {
            pg++;
            std::uint32_t s = 0, vt = 0, slot36 = 0, st = 0;
            std::memcpy(&s, cb->rt_->H(o + 168), 4);
            if (cb->rt_->CheckRange(s, 4, false)) {
                std::memcpy(&vt, cb->rt_->H(s), 4);
            }
            if (cb->rt_->CheckRange(vt + 36, 4, false)) {
                std::memcpy(&slot36, cb->rt_->H(vt + 36), 4);
            }
            if (cb->rt_->CheckRange(s + 180, 4, false)) {
                std::memcpy(&st, cb->rt_->H(s + 180), 4);
            }
            std::printf("[poke164] surf=%08x vptr=%08x slot36=%08x state180=%u\n", s, vt, slot36,
                        st);
        }
    }
    if (v != 0) {
        // (Was: flip only when surf state180==3. Removed: surf is
        // gr::gles2::GL_Context, +180 is not a state field.)
        // NOTE (2026-10-11): the st==3 gate was based on a WRONG-TYPE
        // premise (old subagent: ShadowingSurfaceData). Live vptr 0x669404 =
        // gr::gles2::GL_Context (PROVEN via vtable-4 typeinfo), so +180 is
        // NOT a state field (reads heap garbage). Gate on succ only now.
        if (!std::getenv("HARNESS_POKE164")) {
            return;
        }
        // HARNESS_POKE164_DELAY=N: flip only on/after the Nth firing (lets
        // loading content build first; early flips white the screen).
        if (const char* de = std::getenv("HARNESS_POKE164_DELAY")) {
            static int dc = 0;
            if (++dc < (int)std::strtoul(de, nullptr, 0)) {
                return;
            }
        }
        static int pm = 0;
        if (pm < 3) {
            pm++;
            std::printf("[poke164] obj=%08x +164=%08x -> 0\n", o, v);
        }
        if (cb->rt_->CheckRange(o + 164, 4, true)) {
            const std::uint32_t z = 0;
            std::memcpy(cb->rt_->H(o + 164), &z, 4);
        }
    }
}
void GuestCallbacks::PokeShadowState(std::uint64_t r4obj, std::uint64_t, std::uint64_t) {
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    const std::uint32_t o = (std::uint32_t)r4obj;
    if (!cb->rt_->CheckRange(o + 180, 4, false)) {
        return;
    }
    std::uint32_t v = 0;
    std::memcpy(&v, cb->rt_->H(o + 180), 4);
    if (v == 3) {
        // TEMP-DIAG: dump the shadow-state words (+84..+188) once.
        static int psd = 0;
        if (psd < 2 && cb->rt_->CheckRange(o + 84, 108, false)) {
            psd++;
            const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(o);
            std::printf("[poke180] obj=%08x vptr=%08x", o, *(const std::uint32_t*)p);
            for (int k = 84; k <= 188; k += 4) {
                std::uint32_t w = 0;
                std::memcpy(&w, p + k, 4);
                std::printf(" +%d=%08x", k, w);
            }
            std::printf("\n");
        }
        std::uint32_t nv = 4;
        if (cb->rt_->CheckRange(o + 180, 4, true)) {
            std::memcpy(cb->rt_->H(o + 180), &nv, 4);
            static int ps = 0;
            if (ps < 4) {
                ps++;
                std::printf("[poke180] obj=%08x 3->4\n", o);
            }
        }
    }
}
void GuestCallbacks::PokeGateByte(std::uint64_t r7slot, std::uint64_t, std::uint64_t) {
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    const std::uint32_t slot = (std::uint32_t)r7slot;
    if (!cb->rt_->CheckRange(slot, 4, false)) {
        return;
    }
    std::uint32_t obj = 0;
    std::memcpy(&obj, cb->rt_->H(slot), 4);
    if (obj && cb->rt_->CheckRange(obj + 1187, 1, true)) {
        std::uint8_t one = 1;
        std::memcpy(cb->rt_->H(obj + 1187), &one, 1);
        // TEMP-EXP (menu gate): also set the companion event word. If the
        // advance path derefs it, we get an informative BAD (proves ptr).
        if (std::getenv("HARNESS_POKE1188") && cb->rt_->CheckRange(obj + 1188, 4, true)) {
            std::uint32_t w = 1;
            std::memcpy(cb->rt_->H(obj + 1188), &w, 4);
        }
        static int pg = 0;
        if (pg < 4) {
            pg++;
            std::printf("[poke1187] mid-update slot=%08x obj=%08x\n", slot, obj);
        }
        // TEMP-DIAG (session publication): dump the BSS session object header
        // (f1003008, written once at init) + [manager+164] across updates.
        // Reads only. Compares pre/post sessions completion.
        {
            static int dg = 0;
            if (dg < 14 && cb->rt_->CheckRange(0xF1003008, 32, false) &&
                cb->rt_->CheckRange(obj + 164, 4, false)) {
                dg++;
                std::uint32_t h[8], st = 0;
                std::memcpy(h, cb->rt_->H(0xF1003008), 32);
                std::memcpy(&st, cb->rt_->H(obj + 164), 4);
                std::printf("[sesspub] #%d succ=%d st164=%08x obj308=%08x %08x %08x %08x %08x "
                            "%08x %08x %08x\n",
                            dg, (int)g_succ_done_hack, st, h[0], h[1], h[2], h[3], h[4], h[5],
                            h[6], h[7]);
            }
        }
    }
}
void GuestCallbacks::ProbeDump3(std::uint64_t r4, std::uint64_t r5, std::uint64_t r6) {
    std::printf("[probe] r4=%08x r5=%08x r6=%08x\n", (std::uint32_t)r4, (std::uint32_t)r5,
                (std::uint32_t)r6);
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    // [r4] 16B: byte table the game actually reads (toupper base).
    if (cb->rt_->CheckRange((std::uint32_t)r4, 16, false)) {
        const std::uint8_t* p = cb->rt_->H<const std::uint8_t>((std::uint32_t)r4);
        std::printf("[probe] [r4]:");
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
namespace {
int g_rdump_n = 0;
}  // namespace
void GuestCallbacks::RaiserDump(std::uint64_t r0, std::uint64_t r1, std::uint64_t r2) {
    // HARNESS_RDUMP=1: deep dump of the HTTP raiser (0x28A3DC) entry ABI.
    // Harness-only discovery for success-injection (offline completion).
    if (++g_rdump_n > 6) {
        return;
    }
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    std::printf("[rdump] #%d r0=%08x r1=%08x r2=%08x\n", g_rdump_n, (std::uint32_t)r0,
                (std::uint32_t)r1, (std::uint32_t)r2);
    auto hex = [&](std::uint32_t a, int len, const char* tag) {
        if (!cb->rt_->CheckRange(a, (std::uint32_t)len, false)) {
            std::printf("[rdump] %s %08x: <unmapped>\n", tag, a);
            return;
        }
        const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(a);
        for (int o = 0; o < len; o += 16) {
            std::printf("[rdump] %s +%02x:", tag, o);
            for (int j = 0; j < 16 && o + j < len; j++) {
                std::printf(" %02x", p[o + j]);
            }
            std::printf("  |");
            for (int j = 0; j < 16 && o + j < len; j++) {
                std::printf("%c", (p[o + j] >= 32 && p[o + j] < 127) ? (char)p[o + j] : '.');
            }
            std::printf("|\n");
        }
    };
    auto chase = [&](std::uint32_t a, int nw, const char* tag) {
        if (!cb->rt_->CheckRange(a, (std::uint32_t)(nw * 4), false)) {
            return;
        }
        int shown = 0;
        for (int i = 0; i < nw && shown < 10; i++) {
            std::uint32_t w = 0;
            std::memcpy(&w, cb->rt_->H(a + i * 4), 4);
            if (w < 0x600000u || !cb->rt_->CheckRange(w, 48, false)) {
                continue;
            }
            const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(w);
            std::printf("[rdump] %s[+%02x] -> %08x: ", tag, i * 4, w);
            for (int j = 0; j < 600; j++) {
                if (p[j] == 0) {
                    break;
                }
                std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
            }
            std::printf("\n");
            shown++;
        }
    };
    hex((std::uint32_t)r0, 256, "r0");
    hex((std::uint32_t)r1, 128, "r1");
    chase((std::uint32_t)r0, 64, "r0");
    chase((std::uint32_t)r1, 32, "r1");
}
void GuestCallbacks::RaiserDump2(std::uint64_t r3, std::uint64_t lr, std::uint64_t sp) {
    if (g_rdump_n > 6) {
        return;
    }
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    std::printf("[rdump] r3=%08x lr=%08x sp=%08x\n", (std::uint32_t)r3, (std::uint32_t)lr,
                (std::uint32_t)sp);
    if (!cb->rt_->CheckRange((std::uint32_t)r3, 64, false)) {
        std::printf("[rdump] r3 <unmapped>\n");
        return;
    }
    const std::uint8_t* p = cb->rt_->H<const std::uint8_t>((std::uint32_t)r3);
    for (int o = 0; o < 64; o += 16) {
        std::printf("[rdump] r3 +%02x:", o);
        for (int j = 0; j < 16; j++) {
            std::printf(" %02x", p[o + j]);
        }
        std::printf("  |");
        for (int j = 0; j < 16; j++) {
            std::printf("%c", (p[o + j] >= 32 && p[o + j] < 127) ? (char)p[o + j] : '.');
        }
        std::printf("|\n");
    }
    int shown = 0;
    for (int i = 0; i < 16 && shown < 8; i++) {
        std::uint32_t w = 0;
        std::memcpy(&w, cb->rt_->H((std::uint32_t)r3 + i * 4), 4);
        if (w < 0x600000u || !cb->rt_->CheckRange(w, 48, false)) {
            continue;
        }
        const std::uint8_t* q = cb->rt_->H<const std::uint8_t>(w);
        std::printf("[rdump] r3[+%02x] -> %08x: ", i * 4, w);
        for (int j = 0; j < 48; j++) {
            if (q[j] == 0) {
                break;
            }
            std::printf("%c", (q[j] >= 32 && q[j] < 127) ? (char)q[j] : '.');
        }
        std::printf("\n");
        shown++;
    }
}
void GuestCallbacks::SuccInject(std::uint64_t r0, std::uint64_t lr, std::uint64_t) {
    // HARNESS_RETSUCC=1: synthesize HTTP success at the raiser (0x28A3DC)
    // entry for the .../time endpoint only. Fills the response struct
    // (status 200 + JSON body string) and resumes at the caller, skipping
    // the real network (which always throws offline). Other endpoints fall
    // through to the natural path.
    static int calls = 0;
    static int n = 0;
    ++calls;
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    const std::uint32_t req = (std::uint32_t)r0;
    const std::uint32_t ret = (std::uint32_t)lr;
    // URL at [req+0x58] (char* per rdump chase).
    char url[160] = {0};
    {
        std::uint32_t up = 0;
        if (!cb->rt_->CheckRange(req + 0x58, 4, false)) {
            return;
        }
        std::memcpy(&up, cb->rt_->H(req + 0x58), 4);
        for (int i = 0; i < 159; i++) {
            if (!cb->rt_->CheckRange(up + i, 1, false)) {
                break;
            }
            char c = 0;
            std::memcpy(&c, cb->rt_->H<const char>(up + i), 1);
            url[i] = c;
            if (!c) {
                break;
            }
        }
    }
    std::printf("[succ] #%d url=%.100s\n", calls, url);
    std::string u(url);
    // Response body per endpoint (time/sessions: generated JSON with
    // static-verified shapes; apdrive variantconfig: real file bytes).
    std::uint8_t* body = nullptr;
    std::size_t blen = 0;
    std::uint8_t tbuf[160];
    // Sessions schema (discovered iteratively via KeyNotFound messages;
    // phone-proven graceful path needs these present; extra keys tolerated).
    // Shared by /sessions and the singular /session/1/apps/... call.
    static const char sjson[] =
        "{\"segments\":[],\"access\":{\"token\":\"offline-test-access-12345\"},"
        "\"userAuth\":{\"accessToken\":\"offline-test-token-12345\","
        "\"refreshToken\":\"offline-test-refresh-67890\",\"expiresIn\":2000000000,"
        "\"refresh\":{}},"
        "\"profile\":{},\"config\":{\"sdkDebugLoggingEnabled\":\"true\"},\"refresh\":{}}";
    if (u.size() >= 5 && u.compare(u.size() - 5, 5, "/time") == 0) {
        struct timespec ts {};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        int w = std::snprintf((char*)tbuf, sizeof(tbuf), "{\"time\":%lld,\"signature\":\"test\"}",
                              (long long)ts.tv_sec);
        if (w <= 0 || (std::size_t)w >= sizeof(tbuf)) {
            return;
        }
        body = tbuf;
        blen = (std::size_t)w;
    } else if (u.size() >= 9 && u.compare(u.size() - 9, 9, "/sessions") == 0) {
        body = (std::uint8_t*)sjson;
        blen = sizeof(sjson) - 1;
    } else if (u.find("/session/") != std::string::npos) {
        // Singular /session/1/apps/... (authenticated session call built
        // from stored tokens; seen live on device AFTER sessions-succ
        // saved offline-test tokens to the registry). Schema TBD — start
        // with the sessions shape, iterate via KeyNotFound like before.
        body = (std::uint8_t*)sjson;
        blen = sizeof(sjson) - 1;
    } else if (u.size() >= 9 && u.compare(u.size() - 9, 9, "/refresh") == 0) {
        // Token-refresh leg (the r5-nonempty path in 0xD6C78 builds the
        // refresh token string then fetches; offline it fails -> flag 0).
        // Same sessions schema (new tokens accepted by the real parser).
        static const char rjson[] =
            "{\"segments\":[],\"userAuth\":{\"accessToken\":\"offline-test-token-12345\","
            "\"refreshToken\":\"offline-test-refresh-67890\",\"expiresIn\":2000000000,\"refresh\":{}}},"
            "\"profile\":{},\"config\":{\"sdkDebugLoggingEnabled\":\"true\"},\"refresh\":{}}";
        body = (std::uint8_t*)rjson;
        blen = sizeof(rjson) - 1;
    } else if (((u.size() >= 9 && u.compare(u.size() - 9, 9, "/register") == 0)) ||
               (u.size() >= 6 && u.compare(u.size() - 6, 6, "/login") == 0) ||
               (u.find("sessions/my") != std::string::npos)) {
        static const char qjson[] =
            "{\"segments\":[],\"userAuth\":{\"accessToken\":\"offline-test-token-12345\","
            "\"refreshToken\":\"offline-test-refresh-67890\",\"expiresIn\":2000000000,\"refresh\":{}}},"
            "\"profile\":{},\"config\":{\"sdkDebugLoggingEnabled\":\"true\"},\"refresh\":{}}";
        body = (std::uint8_t*)qjson;
        blen = sizeof(qjson) - 1;
    } else if (u.find("variantconfig.json") != std::string::npos) {
        static std::vector<std::uint8_t> vc;
        static bool vdone = false;
        if (!vdone) {
            vdone = true;
            if (FILE* f = std::fopen("/tmp/opencode/fakeserv/variantconfig.bin", "rb")) {
                std::uint8_t chunk[512];
                std::size_t r = 0;
                while ((r = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
                    vc.insert(vc.end(), chunk, chunk + r);
                }
                std::fclose(f);
            }
        }
        if (vc.empty()) {
            return;  // no local config: natural path.
        }
        body = vc.data();
        blen = vc.size();
    } else {
        return;  // unknown endpoint: natural path.
    }
    // Budget counts injected endpoints only (unknown URLs must not burn it).
    if (++n > 64) {
        return;
    }
    std::uint32_t bmem = cb->rt_->GuestMalloc((std::uint32_t)(blen + 1));
    if (!bmem || !cb->rt_->CheckRange(req + 16, 16, true)) {
        return;
    }
    std::memcpy(cb->rt_->H(bmem), body, blen);
    ((char*)cb->rt_->H(bmem))[blen] = 0;
    // Response: [req+0]=200; body string obj at [req+4] (heap form, tag
    // copied from a live 469B heap string: E1/len/ptr). [req+16..] left
    // UNTOUCHED (request leftovers): zeroing them whites the screen
    // (proven: succlong2 with leftovers renders LOADING, zero-fill whites).
    {
        const std::uint32_t st = 200;
        std::memcpy(cb->rt_->H(req), &st, 4);
        std::uint8_t* o = cb->rt_->H<std::uint8_t>(req + 4);
        o[0] = 0xE1;
        o[1] = o[2] = o[3] = 0;
        std::uint32_t L = (std::uint32_t)blen;
        std::memcpy(o + 4, &L, 4);
        std::memcpy(o + 8, &bmem, 4);
        // TEMP-EXP (P1 req-tail, PROVEN by reqdump/baddump correlation):
        // the parser copies req+0..48 1:1 into the session struct; the
        // epilogue releases r8+28 AND r8+40 as shared_ptr (bl 0x62530 at
        // 0xD74B8/C0). Leftover words there crash the releases one by one
        // (retval-1, then retval-8 residues; epilogue never completes).
        // NULL both px slots (healthy minimal responses carry no objects
        // here); the cn words (+32/+44) go unread when px is null.
        std::uint32_t z40 = 0;
        std::memcpy(cb->rt_->H(req + 28), &z40, 4);
        std::memcpy(cb->rt_->H(req + 40), &z40, 4);
    }
    // TEMP-DIAG (P1 req-tail): dump POST-fill [req+0..64] to correlate with
    // the sessions-epilogue struct (r8+44=1 crash source hunt).
    {
        static int rd = 0;
        if (rd < 3 && cb->rt_->CheckRange(req, 64, false)) {
            rd++;
            const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(req);
            std::printf("[succ] reqdump:");
            for (int j = 0; j < 64; j++) {
                std::printf(" %02x", p[j]);
            }
            std::printf("\n");
        }
    }
    std::printf("[succ] %s: status=200 blen=%zu -> resume %08x\n", u.c_str() + u.find_last_of('/')
                                                                + 1,
                blen, ret);
    g_succ_done_hack = true;
    cb->rt_->ParkRedirect(ret);
    cb->rt_->stop_step_ = true;
    cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}
void GuestCallbacks::ProbeRedir(std::uint64_t r0, std::uint64_t r1, std::uint64_t lr) {
    // HARNESS_REDIR=pad: throw-to-catch emulation (unwinder stand-in).
    // Fires at the __cxa_throw entry probe: records regs, parks a redirect
    // to the landing pad, halts cleanly. The Run wrapper resumes there.
    // HARNESS_REDIR=pad[,pad2,...]: throw-to-catch emulation chain. Each
    // throw consumes the next pad in order (level 1 -> pad1, level 2 ->
    // pad2, ...). HARNESS_REDIRN caps total (default: list length).
    static std::uint32_t pads[8] = {0};
    static int npads = 0, used = 0;
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* e = std::getenv("HARNESS_REDIR")) {
            char tmp[128];
            std::snprintf(tmp, sizeof(tmp), "%s", e);
            for (char* tok = std::strtok(tmp, ","); tok && npads < 8;
                 tok = std::strtok(nullptr, ",")) {
                pads[npads++] = static_cast<std::uint32_t>(std::strtoul(tok, nullptr, 16));
            }
        }
        if (const char* n = std::getenv("HARNESS_REDIRN")) {
            int cap = std::atoi(n);
            if (cap < npads) {
                npads = cap;
            }
        }
    }
    if (used >= npads && !std::getenv("HARNESS_RETFAIL") && !std::getenv("HARNESS_STACKBT") &&
        !std::getenv("HARNESS_TTFILE") && !std::getenv("HARNESS_TTUNW") &&
        !std::getenv("HARNESS_EXDUMP")) {
        return;
    }
    std::uint32_t pad = (used < npads) ? pads[used++] : 0;
    // HARNESS_STACKBT=1: backtrace at throw via live stack (find words that
    // are return addresses = right after a BL). Maps the propagation chain
    // -> candidate catching frames/pads. Runs BEFORE any chain/retfail
    // gating below (pure observation, never halts).
    if (std::getenv("HARNESS_STACKBT")) {
        GuestCallbacks* cbb = g_probe_cb;
        if (cbb) {
            auto& rgs = cbb->rt_->ActiveJit()->Regs();
            const std::uint32_t sp0 = rgs[13];
            std::printf("[stackbt] throw sp=%08x frames:", sp0);
            int n = 0;
            for (std::uint32_t a = sp0; a < sp0 + 8192 && n < 14; a += 4) {
                if (!cbb->rt_->CheckRange(a, 4, false)) {
                    break;
                }
                std::uint32_t w = 0;
                std::memcpy(&w, cbb->rt_->H(a), 4);
                if (w < 0x1000 || w >= 0x653D1C) {
                    continue;
                }
                if (!cbb->rt_->CheckRange(w - 4, 4, false)) {
                    continue;
                }
                std::uint32_t prev = 0;
                std::memcpy(&prev, cbb->rt_->H(w - 4), 4);
                if ((prev >> 25) == 0x75) {
                    std::printf(" %08x", w);
                    n++;
                }
            }
            std::printf("\n");
        }
    }
    // HARNESS_STACKBT=1 (R7 bisection): regs + frame words at throw ENTRY.
    // Compare with the NESTMAX-halt dump: R7 already stale here => pre-throw
    // clobber class (callbacks? thread seed? game -fomit-frame-pointer?);
    // valid here but stale at Find_exidx #1 => driver/context-init class.
    if (std::getenv("HARNESS_STACKBT")) {
        GuestCallbacks* cbb = g_probe_cb;
        if (cbb) {
            auto& rgs = cbb->rt_->ActiveJit()->Regs();
            const std::uint32_t r7v = rgs[7];
            std::printf("[r7entry] R7=%08x R11=%08x SP=%08x LR=%08x : ", r7v, rgs[11],
                        rgs[13], rgs[14]);
            for (int k = -12; k <= 8; k += 4) {
                std::uint32_t w = 0xDEADBEEFu;
                if (cbb->rt_->CheckRange(r7v + k, 4, false)) {
                    std::memcpy(&w, cbb->rt_->H<const std::uint8_t>(r7v + k), 4);
                }
                std::printf("[%+d]=%08x ", k, w);
            }
            std::printf("\n");
        }
    }
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    auto& regs = cb->rt_->ActiveJit()->Regs();
    // HARNESS_EXDUMP=1: hexdump the exception object (r0, 64B) + ti (r1) at
    // the throw. Layout forensics for catch-handler field reads.
    if (std::getenv("HARNESS_EXDUMP")) {
        std::printf("[exdump] obj=%08x ti=%08x\n", (std::uint32_t)r0, (std::uint32_t)r1);
        std::printf("[exdump] r4-r7=%08x %08x %08x %08x r8-r11=%08x %08x %08x %08x sp=%08x lr=%08x\n",
                    regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10], regs[11],
                    regs[13], regs[14]);
        // Validate the r11 frame chain head (FP-walk depends on it).
        {
            std::uint32_t fp = regs[11], sfp = 0, slr = 0;
            bool ok0 = cb->rt_->CheckRange(fp, 4, false);
            bool ok1 = cb->rt_->CheckRange(fp + 4, 4, false);
            if (ok0) {
                std::memcpy(&sfp, cb->rt_->H(fp), 4);
            }
            if (ok1) {
                std::memcpy(&slr, cb->rt_->H(fp + 4), 4);
            }
            std::printf("[exdump] fp=%08x [%08x]=%08x [%08x]=%08x\n", fp, fp, sfp, fp + 4, slr);
        }
        if (cb->rt_->CheckRange((std::uint32_t)r0, 64, false)) {
            const std::uint8_t* p = cb->rt_->H<const std::uint8_t>((std::uint32_t)r0);
            // HARNESS_EXDUMP=2: 128B (big exception objects carry the key
            // past +64).
            int n = (std::getenv("HARNESS_EXDUMP") && std::getenv("HARNESS_EXDUMP")[0] == '2' &&
                     cb->rt_->CheckRange((std::uint32_t)r0, 128, false))
                        ? 128
                        : 64;
            for (int o = 0; o < n; o += 16) {
                std::printf("[exdump] +%02x:", o);
                for (int j = 0; j < 16; j++) {
                    std::printf(" %02x", p[o + j]);
                }
                std::printf("  |");
                for (int j = 0; j < 16; j++) {
                    std::printf("%c", (p[o + j] >= 32 && p[o + j] < 127) ? (char)p[o + j] : '.');
                }
                std::printf("|\n");
            }
        }
    }
    // HARNESS_TTFILE=path: typeinfo-slot matcher (unwinder discovery). At a
    // throw, resolves every ttype slot (one-hop absolute or pcrel-indirect
    // two-hop) and matches against the thrown type + its live base chain.
    // Prints matches (fn/pad/filter/ti). Halts cleanly after (observe mode).
    if (const char* tf = std::getenv("HARNESS_TTFILE")) {
        static bool loaded = false;
        static std::vector<std::uint32_t> slots;
        static std::vector<std::uint32_t> slotpad;
        static std::vector<int> slotflt;
        if (!loaded) {
            loaded = true;
            if (FILE* f = std::fopen(tf, "r")) {
                char line[128];
                while (std::fgets(line, sizeof(line), f)) {
                    unsigned fn = 0, padoff = 0, se = 0;
                    int fl = 0;
                    if (std::sscanf(line, "%x %x %d %x", &fn, &padoff, &fl, &se) == 4) {
                        slotpad.push_back(fn + padoff);
                        slotflt.push_back(fl);
                        slots.push_back(se);
                    }
                }
                std::fclose(f);
            }
            std::printf("[ttmatch] %zu slots loaded\n", slots.size());
        }
        const std::uint32_t thrown = (std::uint32_t)r1;
        // Live base chain (si_class: base at ti+8), cap 4.
        std::uint32_t chain[5] = {thrown, 0, 0, 0, 0};
        int nc = 1;
        {
            std::uint32_t cur = thrown;
            for (int k = 0; k < 3; k++) {
                std::uint32_t b = 0;
                if (!cb->rt_->CheckRange(cur + 8, 4, false)) {
                    break;
                }
                std::memcpy(&b, cb->rt_->H(cur + 8), 4);
                if (b < 0x600000u || b >= 0x6F0000u) {
                    break;
                }
                chain[nc++] = b;
                cur = b;
            }
        }
        auto isdata = [](std::uint32_t v) { return v >= 0x600000u && v < 0x6F0000u; };
        int hits = 0;
        // HARNESS_TTPAD=pad,filter: redirect to an explicitly chosen match
        // (pad absolute VA, filter = catch-clause switch for r1).
        static std::uint32_t wantpad = 0;
        static std::uint32_t wantflt = 0;
        static bool ttinit = false;
        if (!ttinit) {
            ttinit = true;
            if (const char* e = std::getenv("HARNESS_TTPAD")) {
                unsigned p = 0, fl = 0;
                if (std::sscanf(e, "%x,%u", &p, &fl) == 2) {
                    wantpad = p;
                    wantflt = fl;
                }
            }
        }
        for (std::size_t i = 0; i < slots.size(); i++) {
            std::uint32_t E = slots[i];
            if (!cb->rt_->CheckRange(E, 4, false)) {
                continue;
            }
            std::uint32_t raw = 0;
            std::memcpy(&raw, cb->rt_->H(E), 4);
            std::uint32_t cands[2] = {raw, 0};
            std::int32_t sraw = (std::int32_t)raw;
            std::uint32_t slot2 = E + (std::uint32_t)sraw;
            if (cb->rt_->CheckRange(slot2, 4, false)) {
                std::memcpy(&cands[1], cb->rt_->H(slot2), 4);
            }
            for (int c = 0; c < 2; c++) {
                if (!isdata(cands[c])) {
                    continue;
                }
                for (int k = 0; k < nc; k++) {
                    if (cands[c] == chain[k]) {
                        std::printf("[ttmatch] pad=%08x filter=%d ti=%08x (chain+%d, %s)\n",
                                    slotpad[i], slotflt[i], cands[c], k, c ? "twohop" : "onehop");
                        hits++;
                        // HARNESS_TTREDIR=1: redirect to the FIRST match
                        // (pad + catch-clause switch value).
                        if (hits == 1 && std::getenv("HARNESS_TTREDIR")) {
                            cb->rt_->ParkRedirectEx(slotpad[i], (std::uint32_t)slotflt[i]);
                        }
                        break;
                    }
                }
            }
        }
        // Explicit pad choice wins over first-match.
        if (wantpad) {
            std::printf("[ttmatch] explicit pad=%08x filter=%u\n", wantpad, wantflt);
            cb->rt_->ParkRedirectEx(wantpad, wantflt);
        }
        if (!hits) {
            std::printf("[ttmatch] no match for ti=%08x\n", thrown);
        }
        cb->rt_->stop_step_ = true;
        cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    // HARNESS_TTUNW=path: per-frame unwinder emulation (phone-proven: the
    // same binary catches the DNS-fail exception on device and degrades to
    // error code 6; our Dynarmic unwinder can't reach it). At a throw:
    //  1. base chain of the thrown type (live si_class walk, as TTFILE);
    //  2. live stack return-addr scan -> (ret_pc, stack_slot) frames;
    //  3. exidx binary search (in-C++, prel31) ret_pc -> fn start;
    //  4. map lookup fn -> pads (typed + catch-all/cleanup, incl filter 0);
    //  5. redirect to the innermost untried frame with a live-matching typed
    //     catch, else innermost untried catch-all (r1=filter, 0 for catchall;
    //     r0=exception object kept, sp kept — proven accepted by begin_catch).
    // Rethrows iterate to the next candidate via the tried-pad set.
    if (std::getenv("HARNESS_TTUNW")) {
        UnwNext((std::uint32_t)r1);
        return;
    }
    // (0x28A3DC, throw lr 0x28AE70) instead of throwing: the callers check
    // a stack flag ([sp+8] == 0 -> failure branch) and ignore r0. Uses the
    // LIVE frame: r11 (frame ptr, entry_sp = r11+16) gives flag addr
    // [entry_sp+8] and saved return [entry_sp-4] (validated against the 7
    // known caller rets). Takes precedence over pad redirect.
    if (std::getenv("HARNESS_RETFAIL") && (std::uint32_t)lr == 0x28AE70u) {
        const std::uint32_t r11 = regs[11];
        std::uint32_t entry_sp = r11 + 16;
        std::uint32_t saved_lr = 0;
        bool ok = cb->rt_->CheckRange(entry_sp - 4, 4, false);
        if (ok) {
            std::memcpy(&saved_lr, cb->rt_->H(entry_sp - 4), 4);
        }
        static const std::uint32_t rets[] = {0x28A380u, 0x28B0E4u, 0x28B160u, 0x28B1B8u,
                                             0x28B390u, 0x28B3E8u, 0x28B484u};
        bool known = false;
        for (std::uint32_t r : rets) {
            if (saved_lr == r) {
                known = true;
                break;
            }
        }
        std::printf("[retfail] r11=%08x entry_sp=%08x saved_lr=%08x %s\n", r11, entry_sp, saved_lr,
                    known ? "KNOWN" : "UNKNOWN");
        if (!known) {
            // r11 is stale here: scan the live stack for a return address
            // into one of the 7 caller functions instead. The saved-lr
            // slot +4 gives entry_sp the same way.
            const std::uint32_t sp0 = regs[13];
            for (std::uint32_t a = sp0; a < sp0 + 4096; a += 4) {
                if (!cb->rt_->CheckRange(a, 4, false)) {
                    break;
                }
                std::uint32_t w = 0;
                std::memcpy(&w, cb->rt_->H(a), 4);
                for (std::uint32_t r : rets) {
                    if (w == r) {
                        entry_sp = a + 4;
                        saved_lr = w;
                        known = true;
                        ok = true;
                        break;
                    }
                }
                if (known) {
                    break;
                }
            }
            std::printf("[retfail] stack-scan entry_sp=%08x saved_lr=%08x %s\n", entry_sp,
                        saved_lr, known ? "KNOWN" : "UNKNOWN");
        }
        if (ok && known && cb->rt_->CheckRange(entry_sp + 8, 4, true)) {
            const std::uint32_t z = 0;
            std::memcpy(cb->rt_->H(entry_sp + 8), &z, 4);
            regs[0] = 0;
            regs[13] = entry_sp;
            cb->rt_->ParkRedirect(saved_lr);
            std::printf("[retfail] flag=0, resume at %08x\n", saved_lr);
            cb->rt_->stop_step_ = true;
            cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        std::printf("[retfail] validation failed, falling through\n");
    }
    if (!pad) {
        return;  // RETFAIL-only mode (no pad chain): nothing more to do.
    }
    std::printf("[redir] throw r0=%08x r1=%08x lr=%08x sp=%08x -> pad %08x%s\n", (std::uint32_t)r0,
                (std::uint32_t)r1, (std::uint32_t)lr, regs[13], pad,
                cb->rt_->GuestTid() == 1 ? "" : " (T2+)");
    // HARNESS_REDIROBJ=<hexneedle>: resolve a heap object at redirect time
    // (login record fingerprint) and resume with r0=obj instead of the
    // exception object. Fingerprint: anchor word + [+0x14]==0x11.
    if (const char* hx = std::getenv("HARNESS_REDIROBJ")) {
        std::string hxs(hx);
        std::vector<std::uint8_t> needle;
        for (std::size_t k = 0; k + 1 < hxs.size() && needle.size() < 64; k += 2) {
            auto hv = [](char c) -> int {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f') {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F') {
                    return c - 'A' + 10;
                }
                return -1;
            };
            int hi = hv(hxs[k]), lo = hv(hxs[k + 1]);
            if (hi < 0 || lo < 0) {
                break;
            }
            needle.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        std::uint32_t found = 0;
        if (!needle.empty()) {
            const std::uint32_t lo = 0x10000000u;
            const std::uint32_t hi = cb->rt_->HeapTop();
            const std::size_t nl = needle.size();
            for (std::uint32_t a = lo; a + nl <= hi; a++) {
                if (!cb->rt_->CheckRange(a, nl, false)) {
                    a = (a + 0xFFFu) & ~0xFFFu;
                    a -= 1;
                    continue;
                }
                if (std::memcmp(cb->rt_->H(a), needle.data(), nl) != 0) {
                    continue;
                }
                std::uint32_t w14 = 0;
                if (cb->rt_->CheckRange(a + 0x14, 4, false)) {
                    std::memcpy(&w14, cb->rt_->H(a + 0x14), 4);
                }
                if (w14 == 0x11) {
                    found = a;
                    break;
                }
            }
        }
        if (found != 0) {
            std::printf("[redir] obj=%08x -> pad %08x with r0=obj\n", found, pad);
            cb->rt_->ParkRedirectObj(pad, found);
        } else {
            std::printf("[redir] obj not found, plain redirect\n");
            cb->rt_->ParkRedirect(pad);
        }
    } else {
        cb->rt_->ParkRedirect(pad);
    }
    cb->rt_->stop_step_ = true;
    cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}
void Runtime::ParkRedirect(std::uint32_t pad) {
    pending_redir_ = pad;
    pending_jit_ = ActiveJit();
    pending_r1_ = ~0u;
}
void Runtime::ParkRedirectObj(std::uint32_t pad, std::uint32_t r0val) {
    pending_redir_ = pad;
    pending_jit_ = ActiveJit();
    pending_r1_ = ~0u;
    pending_r0_ = r0val;
}
void Runtime::ParkRedirectEx(std::uint32_t pad, std::uint32_t r1val) {
    pending_redir_ = pad;
    pending_jit_ = ActiveJit();
    pending_r1_ = r1val;
    pending_fp_ = 0;
    pending_flags_ = 0;
}
// Park a redirect with unwind state (applied at resume, see header).
void Runtime::ParkUnwind(std::uint32_t pad, std::uint32_t r1val, std::uint32_t fp,
                         std::uint32_t flags, std::int32_t spoff, std::uint32_t regoff) {
    pending_redir_ = pad;
    pending_jit_ = ActiveJit();
    pending_r1_ = r1val;
    pending_fp_ = fp;
    pending_flags_ = flags;
    pending_spoff_ = spoff;
    pending_regoff_ = regoff;
}
static std::uint32_t g_unw_lastti = 0;

void GuestCallbacks::ResumeTrap(std::uint64_t r0, std::uint64_t, std::uint64_t) {
    // A cleanup pad called _Unwind_Resume (would spiral: our unwinder can't
    // continue it). Recompute the live walk and park the next untried pad.
    // The stack never really unwound, so frames/ti are still valid.
    (void)r0;
    std::printf("[resume] trap exc=%08x, continuing walk for ti=%08x\n", (std::uint32_t)r0,
                g_unw_lastti);
    UnwNext(g_unw_lastti);
}

void GuestCallbacks::UnwNext(std::uint32_t thrown_ti) {
    // Per-frame unwinder emulation (see ProbeRedir HARNESS_TTUNW comment).
    // Shared by the throw probe and the _Unwind_Resume trap: recomputes the
    // live walk (the stack never really unwinds) and parks the next untried
    // pad. Halts in all cases (parked or observe).
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    auto& regs = cb->rt_->ActiveJit()->Regs();
    const char* uf = std::getenv("HARNESS_TTUNW");
    if (!uf) {
        return;
    }
    g_unw_lastti = thrown_ti;
    std::uint32_t thrown = thrown_ti;
    struct PadEnt {
        std::uint32_t fn, pad;
        int flt;
        std::uint32_t slot, st, ln;
    };
    static bool uloaded = false;
    static std::unordered_map<std::uint32_t, std::vector<PadEnt>> umap;
    static std::set<std::uint32_t> tried;
    // HARNESS_TTSKIP=pad[,..]: blacklist pads that crash the walk (halts).
    static std::set<std::uint32_t> skip;
    static bool skipinit = false;
    if (!skipinit) {
        skipinit = true;
        if (const char* e = std::getenv("HARNESS_TTSKIP")) {
            char tmp[256];
            std::snprintf(tmp, sizeof(tmp), "%s", e);
            for (char* tok = std::strtok(tmp, ","); tok; tok = std::strtok(nullptr, ",")) {
                skip.insert(static_cast<std::uint32_t>(std::strtoul(tok, nullptr, 16)));
            }
        }
    }
    if (!uloaded) {
        uloaded = true;
        if (FILE* f = std::fopen(uf, "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) {
                unsigned fn = 0, padoff = 0, se = 0, st = 0, ln = 0;
                int fl = 0;
                int nf = std::sscanf(line, "%x %x %d %x %x %x", &fn, &padoff, &fl, &se, &st,
                                      &ln);
                if (nf >= 4) {
                    // File stores fn-relative pad offsets; redirect needs
                    // absolute VAs (0x208-style offsets alone executed
                    // garbage low memory in ttunw1 — fixed).
                    // st/ln (cols 5-6): call-site range covered, fn-relative;
                    // rows without them (older files) cover everything.
                    umap[fn].push_back({fn, fn + padoff, fl, se, st, ln});
                }
            }
            std::fclose(f);
        }
        std::printf("[ttunw] %zu fns loaded\n", umap.size());
    }
    auto urd = [&](std::uint32_t a, std::uint32_t& v) -> bool {
        if (!cb->rt_->CheckRange(a, 4, false)) {
            return false;
        }
        std::memcpy(&v, cb->rt_->H(a), 4);
        return true;
    };
    auto uprel31 = [&](std::uint32_t place, std::uint32_t& out) -> bool {
        std::uint32_t w = 0;
        if (!urd(place, w)) {
            return false;
        }
        std::int32_t off = (std::int32_t)(w << 1) >> 1;
        out = place + (std::uint32_t)off;
        return true;
    };
    // Frame -> fn via exidx (base 0x59D250, 18184 entries, verified).
    auto ufnof = [&](std::uint32_t pc) -> std::uint32_t {
        const std::uint32_t base = 0x59D250u;
        const int n = 18184;
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            std::uint32_t f = 0;
            if (!uprel31(base + mid * 8, f)) {
                return 0;
            }
            if (f <= pc) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < 1) {
            return 0;
        }
        std::uint32_t f = 0;
        if (!uprel31(base + (lo - 1) * 8, f)) {
            return 0;
        }
        return f;
    };
    std::uint32_t chain[5] = {thrown, 0, 0, 0, 0};
    int nc = 1;
    {
        std::uint32_t cur = thrown;
        for (int k = 0; k < 3; k++) {
            std::uint32_t b = 0;
            if (!urd(cur + 8, b)) {
                break;
            }
            if (b < 0x600000u || b >= 0x6F0000u) {
                break;
            }
            chain[nc++] = b;
            cur = b;
        }
    }
    auto umatch = [&](std::uint32_t E) -> int {
        // Returns chain index or -1. One-hop absolute or pcrel two-hop.
        std::uint32_t raw = 0;
        if (!urd(E, raw)) {
            return -1;
        }
        std::uint32_t cands[2] = {raw, 0};
        std::uint32_t slot2 = E + raw;
        urd(slot2, cands[1]);
        for (int c = 0; c < 2; c++) {
            if (cands[c] < 0x600000u || cands[c] >= 0x6F0000u) {
                continue;
            }
            for (int k = 0; k < nc; k++) {
                if (cands[c] == chain[k]) {
                    return k;
                }
            }
        }
        return -1;
    };
    // Prologue-derived r4 slot: [fp-regoff] for `stmdb sp!,{...}` +
    // `add r11,sp,#N` frames (else default 28). Returns (regoff, count of
    // contiguous saved regs r4..). Packed: regoff | count<<16.
    auto prologue_regoff = [&](std::uint32_t fn) -> std::uint32_t {
        std::uint32_t w0 = 0, w1 = 0;
        if (!urd(fn, w0) || !urd(fn + 4, w1)) {
            return 28 | (7u << 16);
        }
        if ((w0 & 0xFFFF0000u) != 0xE92D0000u) {
            return 28 | (7u << 16);
        }
        if ((w1 & 0xFFFFF000u) != 0xE28DB000u) {
            return 28 | (7u << 16);
        }
        std::uint32_t mask = w0 & 0xFFFFu;
        if (mask & 0x000Fu) {
            return 28 | (7u << 16);
        }
        unsigned cnt = 0;
        while (cnt < 7 && (mask & (0x10u << cnt))) {
            cnt++;
        }
        if (cnt == 0) {
            return 28 | (7u << 16);
        }
        return (w1 & 0xFFu) | (cnt << 16);
    };
    // Live frames: ARM frame-pointer walk (r11 chain: exact frames incl.
    // tail-call-compacted ones the BL heuristic misses) UNION the BL
    // return-addr scan (frames without FP).
    struct Frame {
        std::uint32_t pc, slot;
        bool isfp = false;
    };
    std::vector<Frame> frames;
    {
        std::uint32_t fp = regs[11];
        for (int d = 0; d < 24; d++) {
            std::uint32_t sfp = 0, slr = 0;
            if (!urd(fp, sfp) || !urd(fp + 4, slr)) {
                break;
            }
            if (slr < 0x1000 || slr >= 0x653D1C) {
                break;
            }
            if (sfp != 0 && (sfp <= fp || sfp > fp + 65536)) {
                // Not a sane chain link; still record this frame once.
                frames.push_back({slr & ~1u, fp});
                break;
            }
            frames.push_back({slr & ~1u, fp});
            if (sfp == 0) {
                break;
            }
            fp = sfp;
        }
        const std::uint32_t sp0 = regs[13];
        for (std::uint32_t a = sp0; a < sp0 + 8192 && frames.size() < 40; a += 4) {
            std::uint32_t w = 0;
            if (!urd(a, w)) {
                break;
            }
            if (w < 0x1000 || w >= 0x653D1C) {
                continue;
            }
            std::uint32_t prev = 0;
            if (!urd(w - 4, prev)) {
                continue;
            }
            if ((prev >> 25) == 0x75) {
                std::uint32_t pc = w & ~1u;
                bool dup = false;
                for (const Frame& f : frames) {
                    if (f.pc == pc) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    frames.push_back({pc, a});
                }
            }
        }
    }
    std::printf("[ttunw] throw ti=%08x frames=%zu\n", thrown, frames.size());
    // HARNESS_TTUNW_V=1: verbose per-frame fn + typed-slot resolution
    // (finds false-negative matches the redirect pass skips).
    const bool verbose = (std::getenv("HARNESS_TTUNW_V") != nullptr);
    if (verbose) {
        for (const Frame& fr : frames) {
            std::uint32_t fn = ufnof(fr.pc);
            std::printf("[ttunw-v] pc=%08x fn=%08x", fr.pc, fn);
            auto it = umap.find(fn);
            if (it == umap.end()) {
                std::printf(" (no pads)\n");
                continue;
            }
            std::printf(" pads=%zu:", it->second.size());
            for (const PadEnt& e : it->second) {
                if (e.flt > 0) {
                    std::printf(" [%08x,f%d->%d]", e.pad, e.flt, umatch(e.slot));
                } else {
                    std::printf(" [%08x,all]", e.pad);
                }
            }
            std::printf("\n");
        }
    }
    // HARNESS_TTREG=1: restore callee-saved regs from the pad frame's
    // saved slots + r11=fp (default: keep throw-time regs). Real unwind
    // restores r4-r11 from the frame; keeping the innermost frame's regs
    // feeds handlers garbage (BADCTX-proven: r4/r7/r10 held inner values
    // -> 0x167d string). Assumes prologue push{r4-r11,lr} + fp=sp+28
    // (VERIFIED for 0x19FE80: e92d4ff0/e28db01c): saved r4=[fp-28] ..
    // r10=[fp-4]. Frame sp: TTSP if set, else fp-28 (entry sp).
    const bool restoreregs = (std::getenv("HARNESS_TTREG") != nullptr);
    // HARNESS_TTUNW_PAD=pad: force redirect to one pad (found via its
    // frame in the live walk; its fp anchors sp/regs). Overrides order.
    std::uint32_t forcepad = 0;
    if (const char* e = std::getenv("HARNESS_TTUNW_PAD")) {
        forcepad = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }
    // HARNESS_TTSP=off: resume sp = frame_fp + off (signed bytes,
    // default +8). Landing pads address frame temporaries via sp; the
    // throw sp (possibly KB away) feeds them garbage. The true value is
    // try-site dependent; sweep when a handler derails on wild pointers.
    int ttsp_off = 8;
    const bool forcesp = (std::getenv("HARNESS_TTSP") != nullptr);
    if (const char* so = std::getenv("HARNESS_TTSP")) {
        ttsp_off = std::atoi(so);
    }
    if (forcepad) {
        for (const Frame& fr : frames) {
            std::uint32_t fn = ufnof(fr.pc);
            if (!fn) {
                continue;
            }
            auto it = umap.find(fn);
            if (it == umap.end()) {
                continue;
            }
            for (const PadEnt& e : it->second) {
                if (e.pad != forcepad || tried.count(e.pad) || skip.count(e.pad)) {
                    continue;
                }
                bool iscatch = (e.flt == 0);
                std::printf("[ttunw] forced pad=%08x fn=%08x filter=%d fp=%08x%s%s\n",
                            e.pad, fn, e.flt, fr.slot, forcesp ? " (sp=fp+off)" : "",
                            restoreregs ? " (regs<=frame)" : "");
                tried.insert(e.pad);
                // State applied at resume (ParkUnwind); never touch regs
                // here (park-time writes were observed clobbered). Reg
                // restore only for real-fp frames (BL slots are addrs).
                std::uint32_t flags = ((restoreregs && fr.isfp) ? 1u : 0u) | (forcesp ? 2u : 0u);
                std::uint32_t regoff =
                    ((restoreregs && fr.isfp) ? prologue_regoff(fn) : 28);
                cb->rt_->ParkUnwind(e.pad, (std::uint32_t)(iscatch ? 0 : e.flt), fr.slot,
                                    flags, ttsp_off, regoff);
                cb->rt_->stop_step_ = true;
                cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
                return;
            }
        }
        std::printf("[ttunw] forced pad %08x not on live path\n", forcepad);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (const Frame& fr : frames) {
            std::uint32_t fn = ufnof(fr.pc);
            if (!fn) {
                continue;
            }
            auto it = umap.find(fn);
            if (it == umap.end()) {
                continue;
            }
            for (const PadEnt& e : it->second) {
                bool iscatch = (e.flt == 0);
                if ((pass == 0) == iscatch) {
                    continue;
                }
                if (tried.count(e.pad) || skip.count(e.pad)) {
                    continue;
                }
                // Call-site range: the pad only covers throws within
                // [fn+st, fn+st+ln) (fn-relative). A pad from the right
                // function but wrong site reads wrong locals (the 0x167d
                // saga: 0x19FE80 never covered our site). Rows without a
                // range (older map files) cover everything. Check both the
                // return address and the call itself (ret-4).
                if (e.ln > 0) {
                    std::uint32_t off = fr.pc - fn;
                    if (!((e.st <= off && off < e.st + e.ln) ||
                          (off >= 4 && e.st <= off - 4 && off - 4 < e.st + e.ln))) {
                        continue;
                    }
                }
                if (!iscatch) {
                    if (umatch(e.slot) < 0) {
                        continue;
                    }
                }
                std::printf("[ttunw] %s pad=%08x fn=%08x filter=%d (frame pc=%08x)\n",
                            iscatch ? "catchall" : "typed", e.pad, fn, e.flt, fr.pc);
                tried.insert(e.pad);
                if (tried.size() > 24) {
                    std::printf("[ttunw] tried budget exhausted\n");
                    break;
                }
                // Full unwind state at resume (ParkUnwind): park-time reg
                // writes are clobbered by the halt sync (proven: r4/r6
                // rewritten post-park). Flags from env (TTREG/TTSP).
                {
                    std::uint32_t flags = 0;
                    std::int32_t spoff = 8;
                    std::uint32_t regoff = 28;
                    if (std::getenv("HARNESS_TTREG") && fr.isfp) {
                        flags |= 1u;
                        regoff = prologue_regoff(fn);
                    }
                    if (const char* so = std::getenv("HARNESS_TTSP")) {
                        flags |= 2u;
                        spoff = std::atoi(so);
                    }
                    cb->rt_->ParkUnwind(e.pad, (std::uint32_t)(iscatch ? 0 : e.flt), fr.slot,
                                        flags, spoff, regoff);
                }
                cb->rt_->stop_step_ = true;
                cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
                return;
            }
        }
    }
    std::printf("[ttunw] no candidate for ti=%08x\n", thrown);
    cb->rt_->stop_step_ = true;
    cb->rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return;
}

bool Runtime::ApplyRedirect() {
    if (pending_redir_ == 0 || ActiveJit() != pending_jit_) {
        return false;
    }
    auto* jit = ActiveJit();
    auto& regs = jit->Regs();
    std::uint32_t cpsr = jit->Cpsr();
    cpsr = (cpsr & ~std::uint32_t(0x3F)) | std::uint32_t(0x10);  // USER+ARM
    jit->SetCpsr(cpsr);
    regs[15] = pending_redir_;
    if (pending_r1_ != ~0u) {
        regs[1] = pending_r1_;
    }
    if (pending_r0_ != 0u) {
        regs[0] = pending_r0_;
    }
    // Unwind state applied AT RESUME (see header): restore callee-saved
    // regs from the frame's saved slots ([fp-28..fp-4] for the standard
    // push{r4-r11,lr}+fp=sp+28 prologue), r11=fp, sp per flags/offset.
    if (pending_flags_ & 1u) {
        bool ok = true;
        const std::uint32_t ro = pending_regoff_ & 0xFFFFu;
        const unsigned cnt = (pending_regoff_ >> 16) & 0xFFu;
        for (unsigned k = 0; k < cnt && k < 7; k++) {
            std::uint32_t v = 0;
            if (!CheckRange(pending_fp_ - ro + k * 4, 4, false)) {
                ok = false;
                break;
            }
            std::memcpy(&v, H(pending_fp_ - ro + k * 4), 4);
            regs[4 + k] = v;
        }
        regs[11] = pending_fp_;
        if (!ok) {
            std::printf("[redir] frame-reg restore SHORT\n");
        }
    }
    if (pending_flags_ & 2u) {
        regs[13] = pending_fp_ + (std::uint32_t)pending_spoff_;
    } else if (pending_flags_ & 1u) {
        regs[13] = pending_fp_ - (pending_regoff_ & 0xFFFFu);
    }
    // HARNESS_TTZERO=1: resume sp = freshly zeroed 4KB scratch+2048 (pad
    // temporaries all-zero: SSO-empty path). Last word (overrides fp-28).
    // Decides temporaries-vs-state: fault changes => temporaries (sp);
    // identical fault => regs/object.
    if (std::getenv("HARNESS_TTZERO")) {
        std::uint32_t scr = GuestMalloc(4096);
        if (scr) {
            std::memset(H(scr), 0, 4096);
            regs[13] = scr + 2048;
            std::printf("[redir] sp<=zero-scratch %08x\n", regs[13]);
        }
    }
    stop_step_ = false;
    if (std::getenv("HARNESS_TTUNW")) {
        std::printf("[redir] pre-run r4-r7=%08x %08x %08x %08x r8-r11=%08x %08x %08x %08x sp=%08x\n",
                    regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10], regs[11],
                    regs[13]);
    }
    std::printf("[redir] resume at %08x (r0=%08x r1=%08x sp=%08x)%s\n", pending_redir_, regs[0],
                regs[1], regs[13], GuestTid() == 1 ? "" : " (T2+)");
    pending_redir_ = 0;
    pending_r0_ = 0;
    pending_jit_ = nullptr;
    pending_r1_ = ~0u;
    pending_fp_ = 0;
    pending_flags_ = 0;
    return true;
}
void GuestCallbacks::ProbeDump(std::uint64_t r0, std::uint64_t r1, std::uint64_t lr) {
    static std::uint64_t n = 0;
    static std::int64_t cap = -2;
    if (cap == -2) {
        cap = 8;
        if (const char* ce = std::getenv("HARNESS_PROBE_N")) {
            cap = (std::int64_t)std::strtoul(ce, nullptr, 0);
        }
    }
    if (++n > (std::uint64_t)cap) {
        if (n == (std::uint64_t)cap + 1) {
            std::printf("[probe] ... (quiet henceforth)\n");
        }
        return;
    }
    std::uint32_t len = (r1 > 256) ? 256 : (std::uint32_t)r1;
    std::printf("[probe] #%llu r0=%08x r1=%08x lr=%08x\n", (unsigned long long)n,
                (std::uint32_t)r0, (std::uint32_t)r1, (std::uint32_t)lr);
    GuestCallbacks* cb = g_probe_cb;
    if (!cb) {
        return;
    }
    // Unconditional 16B at [r0] (compare key / object head). Runs before any
    // early-returning branch below.
    if (cb->rt_->CheckRange((std::uint32_t)r0, 16, false)) {
        const std::uint8_t* p0 = cb->rt_->H<const std::uint8_t>((std::uint32_t)r0);
        std::printf("[probe] [r0]:");
        for (int j = 0; j < 16; j++) {
            std::printf(" %02x", p0[j]);
        }
        std::printf("  |");
        for (int j = 0; j < 16; j++) {
            std::printf("%c", (p0[j] >= 32 && p0[j] < 127) ? (char)p0[j] : '.');
        }
        std::printf("|\n");
    }
    // [r0+16] 16B (entry+16 layouts: the handler ptr often sits at +16 of
    // the probed base, e.g. completion-invoker entries). CheckRange-guarded.
    {
        const std::uint32_t r0p16 = (std::uint32_t)r0 + 16;
        if (cb->rt_->CheckRange(r0p16, 16, false)) {
            const std::uint8_t* p016 = cb->rt_->H<const std::uint8_t>(r0p16);
            std::printf("[probe] [r0+16]:");
            for (int j = 0; j < 16; j++) {
                std::printf(" %02x", p016[j]);
            }
            std::printf("  |");
            for (int j = 0; j < 16; j++) {
                std::printf("%c", (p016[j] >= 32 && p016[j] < 127) ? (char)p016[j] : '.');
            }
            std::printf("|\n");
        }
    }
    // [r1] raw 32B (forwarder inputs are objects; the range/longstr
    // interpretation above misfires on them). HARNESS_BIGDUMP=1: 256B.
    if (cb->rt_->CheckRange((std::uint32_t)r1, 32, false)) {
        int nb = std::getenv("HARNESS_BIGDUMP") ? 256 : 32;
        if (!cb->rt_->CheckRange((std::uint32_t)r1, (std::uint32_t)nb, false)) {
            nb = 32;
        }
        const std::uint8_t* p1 = cb->rt_->H<const std::uint8_t>((std::uint32_t)r1);
        std::printf("[probe] [r1]-raw:");
        for (int j = 0; j < nb; j++) {
            std::printf(" %02x", p1[j]);
        }
        std::printf("\n");
    }
    if (cb->rt_->CheckRange((std::uint32_t)r1, 1, false)) {
        std::uint8_t b0 = 0;
        std::memcpy(&b0, cb->rt_->H<const std::uint8_t>((std::uint32_t)r1), 1);
        if ((b0 & 1) == 0) {
            std::uint32_t L = (std::uint32_t)b0 >> 1;
            if (L <= 64 && cb->rt_->CheckRange((std::uint32_t)r1 + 1, L ? L : 1, false)) {
                const std::uint8_t* p1 = cb->rt_->H<const std::uint8_t>((std::uint32_t)r1);
                std::printf("[probe] [r1]-sso(len=%u):", L);
                for (std::uint32_t j = 1; j <= L && j < 65; j++) {
                    std::printf(" %02x", p1[j]);
                }
                std::printf("  |");
                for (std::uint32_t j = 1; j <= L && j < 65; j++) {
                    std::printf("%c", (p1[j] >= 32 && p1[j] < 127) ? (char)p1[j] : '.');
                }
                std::printf("|\n");
            }
        }
    }
    // C++ object form: [rX+0] = vtable -> [-1] = typeinfo -> [+4] = name.
    // Applies to both r0 and r1 (caller decides which holds the object).
    for (int which = 0; which < 2; which++) {
        std::uint32_t rx = (which == 0) ? (std::uint32_t)r0 : (std::uint32_t)r1;
        if (!cb->rt_->CheckRange(rx, 4, false)) {
            continue;
        }
        std::uint32_t vt = 0;
        std::memcpy(&vt, cb->rt_->H<const std::uint8_t>(rx), 4);
        if (cb->rt_->CheckRange(vt - 4, 8, false)) {
            std::uint32_t ti = 0, nm = 0;
            std::memcpy(&ti, cb->rt_->H<const std::uint8_t>(vt - 4), 4);
            if (cb->rt_->CheckRange(ti + 4, 4, false)) {
                std::memcpy(&nm, cb->rt_->H<const std::uint8_t>(ti + 4), 4);
                if (cb->rt_->CheckRange(nm, 1, false)) {
                    char buf[129];
                    std::uint32_t k = 0;
                    for (; k < 128; k++) {
                        char c = 0;
                        std::memcpy(&c, cb->rt_->H<const std::uint8_t>(nm + k), 1);
                        buf[k] = c;
                        if (!c) {
                            break;
                        }
                    }
                    buf[k] = 0;
                    std::printf("[probe] r%u class: %s\n", which, buf);
                }
            }
        }
    }
    // Throw-site form: r1 = std::type_info*. Its [+4] is the mangled name.
    // (Used with HARNESS_PROBE=<__cxa_throw entry> to identify the exception
    // type at throw time; the exception object itself is still being built.)
    if (cb->rt_->CheckRange((std::uint32_t)r1 + 4, 4, false)) {
        std::uint32_t nm = 0;
        std::memcpy(&nm, cb->rt_->H<const std::uint8_t>((std::uint32_t)r1 + 4), 4);
        if (nm && cb->rt_->CheckRange(nm, 1, false)) {
            char buf[129];
            std::uint32_t k = 0;
            for (; k < 128; k++) {
                char c = 0;
                if (!cb->rt_->CheckRange(nm + k, 1, false)) {
                    break;
                }
                std::memcpy(&c, cb->rt_->H<const std::uint8_t>(nm + k), 1);
                buf[k] = c;
                if (!c) {
                    break;
                }
            }
            buf[k] = 0;
            if (k > 0 && k < 128) {
                std::printf("[probe] throw type: %s lr=%08x\n", buf, (std::uint32_t)lr);
            }
            // TEMP-DIAG (unwinder bring-up): measure the exception header by
            // scanning back from the thrown object for the exception-class
            // magic. NDK = libc++ ("CLNGC++"), not libstdc++ ("GNUCC++").
            // 16B... 8B-step guarded (heap-chunk edges fail whole checks).
            {
                const std::uint32_t eo = (std::uint32_t)r0;
                static const char m_clng[8] = {'C', 'L', 'N', 'G', 'C', '+', '+', '\0'};
                static const char m_gnu[8] = {'G', 'N', 'U', 'C', 'C', '+', '+', '\0'};
                for (std::uint32_t back = 8; back <= 256; back += 8) {
                    if (eo < back + 8) {
                        break;
                    }
                    const std::uint32_t a = eo - back;
                    if (!cb->rt_->CheckRange(a, 8, false)) {
                        continue;
                    }
                    char w8[8];
                    std::memcpy(w8, cb->rt_->H<const std::uint8_t>(a), 8);
                    if (std::memcmp(w8, m_clng, 8) == 0 || std::memcmp(w8, m_gnu, 8) == 0) {
                        std::printf("[probe] ue-magic at %08x (obj-%u)\n", a, back);
                        break;
                    }
                }
                // TEMP-DIAG: full backward dump [eo-128, eo] for eyeballing.
                {
                    const std::uint32_t lo = eo > 128 ? eo - 128 : 0;
                    if (lo && cb->rt_->CheckRange(lo, 128, false)) {
                        const std::uint8_t* q = cb->rt_->H<const std::uint8_t>(lo);
                        for (int row = 0; row < 8; row++) {
                            std::printf("[probe] ue-back @%08x:", lo + row * 16);
                            for (int j = 0; j < 16; j++) {
                                std::printf(" %02x", q[row * 16 + j]);
                            }
                            std::printf(" |");
                            for (int j = 0; j < 16; j++) {
                                unsigned ch = q[row * 16 + j];
                                std::printf("%c", (ch >= 32 && ch < 127) ? (char)ch : '.');
                            }
                            std::printf("|\n");
                        }
                    } else {
                        std::printf("[probe] ue-back unreadable\n");
                    }
                }
            }
            {
                const std::uint32_t eo = (std::uint32_t)r0;
                if (cb->rt_->CheckRange(eo, 80, false)) {
                    const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(eo);
                    std::printf("[probe] exc-obj @%08x:", eo);
                    for (int j = 0; j < 80; j++) {
                        std::printf(" %02x", p[j]);
                    }
                    std::printf("\n[probe] exc-ascii: ");
                    for (int j = 0; j < 80; j++) {
                        std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
                    }
                    std::printf("\n");
                    // Follow two likely-string pointers at +16/+64 (URL? stage?).
                    for (int k = 0; k < 2; k++) {
                        std::uint32_t sp = 0;
                        std::memcpy(&sp, p + (k == 0 ? 16 : 64), 4);
                        if (sp >= 0x10000000u && sp < 0x20000000u &&
                            cb->rt_->CheckRange(sp, 64, false)) {
                            const std::uint8_t* q = cb->rt_->H<const std::uint8_t>(sp);
                            std::printf("[probe] exc-str%d @%08x: ", k, sp);
                            for (int j = 0; j < 64; j++) {
                                if (q[j] == 0) {
                                    break;
                                }
                                std::printf("%c", (q[j] >= 32 && q[j] < 127) ? (char)q[j] : '.');
                            }
                            std::printf("\n");
                        }
                    }
                }
            }
        }
    }
    // Indirect form: {begin,end} range, or a libc++ std::string (short:
    // byte0&1==0, data=r1+1, len=byte0>>1; long: [+0]=cap|1, [+4]=len,
    // [+8]=data — matches the 0x2A52F8 wrapper idiom).
    if (cb->rt_->CheckRange((std::uint32_t)r1, 12, false)) {
        const std::uint8_t* q = cb->rt_->H<const std::uint8_t>((std::uint32_t)r1);
        std::uint32_t b = 0, e = 0;
        if (q[0] & 1) {
            std::uint32_t len = (std::uint32_t)q[4] | ((std::uint32_t)q[5] << 8) |
                                ((std::uint32_t)q[6] << 16) | ((std::uint32_t)q[7] << 24);
            b = (std::uint32_t)q[8] | ((std::uint32_t)q[9] << 8) |
                ((std::uint32_t)q[10] << 16) | ((std::uint32_t)q[11] << 24);
            e = b + len;
            std::printf("[probe] [r1]=longstr len=%u\n", len);
        } else {
            b = (std::uint32_t)q[0] | ((std::uint32_t)q[1] << 8) |
                ((std::uint32_t)q[2] << 16) | ((std::uint32_t)q[3] << 24);
            e = (std::uint32_t)q[4] | ((std::uint32_t)q[5] << 8) |
                ((std::uint32_t)q[6] << 16) | ((std::uint32_t)q[7] << 24);
            std::printf("[probe] [r1]={%08x..%08x}\n", b, e);
        }
        if (e >= b && e - b <= 0x10000 && cb->rt_->CheckRange(b, e > b ? e - b : 1, false)) {
            std::uint32_t dl = (e - b > 2048) ? 2048 : e - b;
            const std::uint8_t* p = cb->rt_->H<const std::uint8_t>(b);
            for (std::uint32_t i = 0; i < dl; i += 16) {
                std::printf("[probe] %08x:", b + i);
                for (std::uint32_t j = i; j < i + 16 && j < dl; j++) {
                    std::printf(" %02x", p[j]);
                }
                std::printf("  |");
                for (std::uint32_t j = i; j < i + 16 && j < dl; j++) {
                    std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
                }
                std::printf("|\n");
            }
            if (e - b == 0) {
                std::printf("[probe] (empty range)\n");
            }
            return;
        }
    }
    // Wrapper idiom (io::InputStream stack wrapper): [r0+12]==r0 (self-ref)
    // selects backing = [r0+8]. Dump backing vtable->class + first 32B.
    // (Saw at 0x2B0BD4: wrapper on stack, backing on heap.)
    if (cb->rt_->CheckRange((std::uint32_t)r0, 16, false)) {
        std::uint32_t w8 = 0, w12 = 0;
        std::memcpy(&w8, cb->rt_->H<const std::uint8_t>((std::uint32_t)r0 + 8), 4);
        std::memcpy(&w12, cb->rt_->H<const std::uint8_t>((std::uint32_t)r0 + 12), 4);
        if (w12 == (std::uint32_t)r0 && w8 != 0 &&
            cb->rt_->CheckRange(w8, 36, false)) {
            std::uint32_t bvt = 0;
            std::memcpy(&bvt, cb->rt_->H<const std::uint8_t>(w8), 4);
            std::printf("[probe] backing @%08x vtable=%08x\n", w8, bvt);
            if (cb->rt_->CheckRange(bvt - 4, 8, false)) {
                std::uint32_t bti = 0, bnm = 0;
                std::memcpy(&bti, cb->rt_->H<const std::uint8_t>(bvt - 4), 4);
                if (cb->rt_->CheckRange(bti + 4, 4, false)) {
                    std::memcpy(&bnm, cb->rt_->H<const std::uint8_t>(bti + 4), 4);
                    if (bnm && cb->rt_->CheckRange(bnm, 1, false)) {
                        char buf[129];
                        std::uint32_t k = 0;
                        for (; k < 128; k++) {
                            char c = 0;
                            if (!cb->rt_->CheckRange(bnm + k, 1, false)) {
                                break;
                            }
                            std::memcpy(&c, cb->rt_->H<const std::uint8_t>(bnm + k), 1);
                            buf[k] = c;
                            if (!c) {
                                break;
                            }
                        }
                        buf[k] = 0;
                        if (k > 0 && k < 128) {
                            std::printf("[probe] backing class: %s\n", buf);
                        }
                    }
                }
            }
            const std::uint8_t* bp = cb->rt_->H<const std::uint8_t>(w8);
            for (std::uint32_t i = 0; i < 192; i += 16) {
                if (!cb->rt_->CheckRange(w8 + i, 16, false)) {
                    break;
                }
                bp = cb->rt_->H<const std::uint8_t>(w8);
                std::printf("[probe] %08x:", w8 + i);
                for (std::uint32_t j = i; j < i + 16; j++) {
                    std::printf(" %02x", bp[j]);
                }
                std::printf("  |");
                for (std::uint32_t j = i; j < i + 16; j++) {
                    std::printf("%c", (bp[j] >= 32 && bp[j] < 127) ? (char)bp[j] : '.');
                }
                std::printf("|\n");
            }
        }
    }
    if (!cb->rt_->CheckRange((std::uint32_t)r0, 64, false)) {
        std::printf("[probe] [r0] BAD range\n");
        return;
    }
    {
        std::uint32_t dl = 64;
        const std::uint8_t* p = cb->rt_->H<const std::uint8_t>((std::uint32_t)r0);
        for (std::uint32_t i = 0; i < dl; i += 16) {
            std::printf("[probe] %08x:", (std::uint32_t)r0 + i);
            for (std::uint32_t j = i; j < i + 16 && j < dl; j++) {
                std::printf(" %02x", p[j]);
            }
            std::printf("  |");
            for (std::uint32_t j = i; j < i + 16 && j < dl; j++) {
                std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
            }
            std::printf("|\n");
        }
    }
}
void GuestCallbacks::PreCodeTranslationHook(bool is_thumb, VAddr pc,
                                               Dynarmic::A32::IREmitter& ir) {
    static std::int64_t tl = -2;
    if (tl == -2) {
        tl = -1;
        if (std::getenv("HARNESS_TRANSLOG")) {
            tl = 1;
        }
    }
    // One-time env snapshot (2026-10-17, PLAN.md B11): these were a std::getenv
    // per translated basic block -- a linear environ scan during JIT warmup.
    // Snapshot once (thread-safe magic statics) but keep the individual flags
    // working standalone (a dev may push one via abflags without HARNESS_DIAG).
    static const bool fx_fixpvr = std::getenv("HARNESS_FIXPVR") != nullptr;
    static const bool fx_poke1187 = std::getenv("HARNESS_POKE1187") != nullptr;
    static const bool fx_buildergate = std::getenv("HARNESS_BUILDERGATE") != nullptr;
    static const bool fx_poke180 = std::getenv("HARNESS_POKE180") != nullptr;
    static const bool fx_poke164 = std::getenv("HARNESS_POKE164") != nullptr;
    static const bool fx_diag164 = std::getenv("HARNESS_DIAG164") != nullptr;
    static const bool fx_dumpr9 = std::getenv("HARNESS_DUMPR9") != nullptr;
    // HARNESS_PROBE=<hexpc>[,<hexlen>]: emit an execution-time host call at
    // pc that dumps r0/r1 + min(r1,len,256) bytes at [r0]. Used to capture
    // JIT-time (ptr,len) args (e.g. JSON parser input) that heap-drift and
    // thread racing make invisible post-mortem. Translation-time hook, but
    // the emitted CallHostFunction runs on every execution with live regs.
    {
        static std::int64_t probe = -2;
        static std::uint32_t probe_pc = 0, probe_len = 0, probe_pc2 = 0, probe_pc3 = 0,
                                 probe_pc4 = 0, probe_pc5 = 0;
        if (probe == -2) {
            probe = 0;
            if (const char* pe = std::getenv("HARNESS_PROBE")) {
                probe_pc = static_cast<std::uint32_t>(std::strtoul(pe, nullptr, 16));
                probe_len = 256;
                if (const char* c = std::strchr(pe, ',')) {
                    probe_len = static_cast<std::uint32_t>(std::strtoul(c + 1, nullptr, 16));
                }
                probe = 1;
            }
            if (const char* pe2 = std::getenv("HARNESS_PROBE2")) {
                probe_pc2 = static_cast<std::uint32_t>(std::strtoul(pe2, nullptr, 16));
                probe = 1;
            }
            if (const char* pe3 = std::getenv("HARNESS_PROBE3")) {
                probe_pc3 = static_cast<std::uint32_t>(std::strtoul(pe3, nullptr, 16));
                probe = 1;
            }
            // HARNESS_PROBE4=addr: _Unwind_Resume trap slot (resume-walk).
            if (const char* pe4 = std::getenv("HARNESS_PROBE4")) {
                probe_pc4 = static_cast<std::uint32_t>(std::strtoul(pe4, nullptr, 16));
                probe = 1;
            }
            // HARNESS_PROBE5=addr: extra dump slot (shares ProbeDump triple).
            if (const char* pe5 = std::getenv("HARNESS_PROBE5")) {
                probe_pc5 = static_cast<std::uint32_t>(std::strtoul(pe5, nullptr, 16));
                probe = 1;
            }
        }
        // Standalone IR fixes (independent of probe slots).
        if (fx_fixpvr && pc == 0x2B0C50u) {
            using Reg = Dynarmic::A32::Reg;
            ir.SetRegister(static_cast<Reg>(0), ir.Imm32(11));
        }
        // HARNESS_POKE1187: the loading gate byte [*(r7)+1187] is
        // re-zeroed by an in-update memset AFTER our pre-call poke, so set it
        // right here at 0xC2144 (ldr r0,[r7] just before the gate BL 0xC2148).
        if (fx_poke1187 && pc == 0xC2144u) {
            ir.CallHostFunction(
                &PokeGateByte,
                ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(7))),
                ir.Imm64(0), ir.Imm64(0));
        }
        // HARNESS_BUILDERGATE=1: force the menu-builder frustum gate open
        // (set r0=1 right before `cmp r0,#1` at 0x250C80; menu content is
        // on-screen by definition, and forged rects come out NaN). The body
        // then runs with forged this: faults name requirements, clean run
        // tests graph attachment. Harness-only diag.
        if (fx_buildergate && pc == 0x250C80u) {
            using Reg = Dynarmic::A32::Reg;
            ir.SetRegister(static_cast<Reg>(0), ir.Imm32(1));
        }
        // HARNESS_POKE180=1: render-state advance at 0x263300 (FuncA entry,
        // a block start; r0=this). The 0x263338 site is mid-block (hook never
        // fires there). Sets [this+180] 3->4 when loading-state.
        if (fx_poke180 && pc == 0x263300u) {
            ir.CallHostFunction(
                &PokeShadowState,
                ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                ir.Imm64(0), ir.Imm64(0));
        }
        // HARNESS_POKE164=1: loading-manager flag at 0x283B5C entry (r10=r0
        // there per prologue r10=r0). Forces the menu branch at 0x283C34.
        // HARNESS_DIAG164=1: same hook, diag only (no flip).
        if ((fx_poke164 || fx_diag164) && pc == 0x283B5Cu) {
            ir.CallHostFunction(
                &PokeManagerFlag,
                ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                ir.Imm64(0), ir.Imm64(0));
        }
        // HARNESS_WATCHPC=<hexpc>: regs + backtrace at pc (no slot).
        {
            static bool winit = false;
            static std::uint32_t wpc = 0;
            if (!winit) {
                winit = true;
                if (const char* e = std::getenv("HARNESS_WATCHPC")) {
                    wpc = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
                }
            }
            if (wpc && pc == wpc) {
                wpc_shared = wpc;
                ir.CallHostFunction(&WatchPC,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(1))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))));
                ir.CallHostFunction(&WatchSP, ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(13))), ir.Imm64(0), ir.Imm64(0));
            }
        }
        // HARNESS_DUMPR9=1: log session object at a probe slot
        // (e.g. PROBE2=0xd6c78 entry: r0 = session object, before mov r9,r0).
        if (fx_dumpr9 && pc == probe_pc2 && probe_pc2) {
            ir.CallHostFunction(
                &DumpR9,
                ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                ir.Imm64(0), ir.Imm64(0));
        }
        // HARNESS_PCTRACE=start,len[,cap]: pc-only trace of basic-block
        // entries in range (path forensics without register dumps; much
        // cheaper than TRCALL's ProbeDump triple — safe for warm code).
        {
            static bool pcinit = false;
            static std::uint32_t pcstart = 0, pcend = 0;
            static std::int64_t pccap = 5000, pcn = 0;
            if (!pcinit) {
                pcinit = true;
                if (const char* e = std::getenv("HARNESS_PCTRACE")) {
                    unsigned s = 0, l = 0, c = 5000;
                    if (std::sscanf(e, "%x,%x,%x", &s, &l, &c) >= 2) {
                        pcstart = s;
                        pcend = s + l;
                        pccap = (std::int64_t)c;
                    }
                }
            }
            if (pcend > pcstart && pc >= pcstart && pc < pcend && pcn < pccap) {
                pcn++;
                std::printf("[pctrace] %08x\n", pc);
            }
        }
        {
            static bool trinit = false;
            static std::uint32_t trstart = 0, trend = 0;
            if (!trinit) {
                trinit = true;
                if (const char* e = std::getenv("HARNESS_TRCALL")) {
                    unsigned s = 0, l = 0;
                    if (std::sscanf(e, "%x,%x", &s, &l) == 2) {
                        trstart = s;
                        trend = s + l;
                    }
                }
            }
            if (trend > trstart && pc >= trstart && pc < trend) {
                ir.CallHostFunction(&ProbeDump,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(1))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))));
                ir.CallHostFunction(&ProbeDump2,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(2))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(3))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(13))));
                ir.CallHostFunction(&ProbeDump3,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(4))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(5))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(6))));
            }
        }
        if (probe > 0 && (pc == probe_pc || pc == probe_pc2 || pc == probe_pc3 || pc == probe_pc4 ||
                        pc == probe_pc5)) {
            g_probe_pc = pc;
            std::printf("[probe-emit] %c %08x\n", is_thumb ? 'T' : 'A', pc);
            // HARNESS_FIXPVR=1: at 0x2B0C50 (post image-format lookup) force
            ir.CallHostFunction(&ProbeDump,
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(1))),
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))));
            ir.CallHostFunction(&ProbeDump2,
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(2))),
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(3))),
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(13))));
            ir.CallHostFunction(&ProbeDump3,
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(4))),
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(5))),
                                ir.ZeroExtendWordToLong(
                                    ir.GetRegister(static_cast<Dynarmic::A32::Reg>(6))));
            // HARNESS_PROBE4=_Unwind_Resume entry: resume-trap. A cleanup pad
            // ending in resume would spiral in our broken unwinder; instead
            // continue the walk (next untried pad) via shared UnwNext.
            if (std::getenv("HARNESS_TTUNW") && pc == probe_pc4 && probe_pc4) {
                ir.CallHostFunction(&ResumeTrap, ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))), ir.Imm64(0), ir.Imm64(0));
            }
            // HARNESS_EVENTPOST=1: at PROBE4 (0xC465C, right after the walk's
            // mov r6,r0) decide the pi (mirror natural into pi#1, else synthetic)
            // then steer the walk: r6 <= pi#1 (baked; pi#1 built at entry, which
            // always executes first, so the bake is sound; same addrs forever).
            if (std::getenv("HARNESS_EVENTPOST") && !std::getenv("HARNESS_LASTTHIS") && pc == probe_pc4 && probe_pc4) {
                ir.CallHostFunction(&EventDecide, ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))), ir.Imm64(0), ir.Imm64(0));
                {
                    using Reg = Dynarmic::A32::Reg;
                    ir.SetRegister(static_cast<Reg>(6), ir.Imm32(s_eventpost_pi1));
                }
            }
            // HARNESS_LASTTHIS=1: record loading-builder r0 at PROBE4
            // (mutually exclusive with EVENTPOST on this slot).
            if (std::getenv("HARNESS_LASTTHIS") && !std::getenv("HARNESS_EVENTPOST") && pc == probe_pc4 && probe_pc4) {
                ir.CallHostFunction(&LastThis, ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))), ir.Imm64(0), ir.Imm64(0));
            }
            // HARNESS_RETSUCC=1: success synthesis at raiser entry (PROBE3).
            // HARNESS_EVENTPOST=1: fake the subscriber count at walk entry
            // (PROBE5 = 0xC45FC, r1 = event slot) so the walk reaches the pi
            // acquisition where the pi is acquired (may be NULL).
            if (std::getenv("HARNESS_EVENTPOST") && pc == probe_pc5 && probe_pc5) {
                ir.CallHostFunction(&EventEntry, ir.ZeroExtendWordToLong(ir.GetRegister(static_cast<Dynarmic::A32::Reg>(1))), ir.Imm64(0), ir.Imm64(0));
            }
            if (std::getenv("HARNESS_RETSUCC") && pc == probe_pc3) {
                ir.CallHostFunction(&SuccInject,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))),
                                    ir.Imm64(0));
            }
            // HARNESS_RDUMP=1: deep raiser-entry ABI dump (PROBE3 slot).
            if (std::getenv("HARNESS_RDUMP") && pc == probe_pc3) {
                ir.CallHostFunction(&RaiserDump,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(1))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(2))));
                ir.CallHostFunction(&RaiserDump2,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(3))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(13))));
            }
            // HARNESS_REDIR=pad[,..]: throw-to-catch emulation at the throw
            // entry (PROBE2 slot); HARNESS_RETFAIL=1: failing-return
            // emulation (same site); HARNESS_STACKBT/HARNESS_TTFILE observe.
            // HARNESS_TTUNW=path: per-frame unwinder emulation (same site).
            // Emitted after the dumps.
            if ((std::getenv("HARNESS_REDIR") || std::getenv("HARNESS_RETFAIL") ||
                 std::getenv("HARNESS_STACKBT") || std::getenv("HARNESS_TTFILE") ||
                 std::getenv("HARNESS_TTUNW") || std::getenv("HARNESS_EXDUMP")) &&
                pc == probe_pc2) {
                ir.CallHostFunction(&ProbeRedir,
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(0))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(1))),
                                    ir.ZeroExtendWordToLong(
                                        ir.GetRegister(static_cast<Dynarmic::A32::Reg>(14))));
            }
        }
    }
    if (tl > 0) {
        std::printf("[tr] %c %08x\n", is_thumb ? 'T' : 'A', pc);
    }
}

std::optional<std::uint32_t> GuestCallbacks::MemoryReadCode(VAddr a) {    if (!rt_->CheckAccess(a & ~std::uint32_t(3), 4, false, true)) {
        if (rt_->NoteBad()) {
            std::printf("[mem] BAD exec @ %08x -- halting\n", a);
            rt_->DumpEventRing();
        }
        rt_->stop_step_ = true;
        rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0;
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
    auto& regs = rt_->ActiveJit()->Regs();
    rt_->OnSvc(swi, regs);
}

// inet_* (inet_addr/inet_aton/inet_pton/inet_ntoa/inet_ntop). File scope:
// CallImport's gotos to bad_ptr cannot cross automatic inits, so this lives
// here (same convention as TimeWarp). All paths Ret()+return (handled).
void Runtime::InetThunk(Regs& regs, const std::string& n, std::uint32_t a0, std::uint32_t a1,
                        std::uint32_t a2, std::uint32_t a3) {
    // TEMP-DIAG (ntop failures): log args, capped (ntop has its own cap;
    // pton spam must not eat it).
    if (DiagOn()) {  // PLAN.md A: diagnostic layer
        static int ilog = 0, olog = 0;
        if (n == "inet_ntop" && olog < 12) {
            olog++;
            std::printf("[netsock] %s(%u %08x %08x %08x)%s\n", n.c_str(), a0, a1, a2, a3,
                        olog == 12 ? " (quiet henceforth)" : "");
        } else if (n == "inet_pton" && ilog < 4) {
            ilog++;
            std::printf("[netsock] %s(%u %08x %08x %08x)\n", n.c_str(), a0, a1, a2, a3);
        }
    }
    if (n == "inet_ntoa" || n == "inet_ntop") {
        // inet_ntoa(struct in_addr): addr comes BY VALUE in r0 (NOT a
        // pointer). inet_ntop(af, src, dst, size): dst=a2, size=a3.
        struct in_addr ia {};
        const void* src = nullptr;
        std::uint32_t dst = 0;
        std::uint32_t dstsize = 16;
        if (n == "inet_ntoa") {
            std::uint32_t v = a0;
            std::memcpy(&ia, &v, 4);
            src = &ia;
        } else {
            // AF_INET (16B min) and AF_INET6 (46B min).
            const std::uint32_t need = (a0 == 10 /*AF_INET6*/) ? 46u : 16u;
            if ((a0 != 2 && a0 != 10) || a3 < need || !CheckRange(a1, 4, false) ||
                !CheckRange(a2, need, true)) {
                // TEMP-DIAG: which check fails?
                const std::uint8_t* sp2 =
                    (a1 && CheckRange(a1, 16, false)) ? H<const std::uint8_t>(a1) : nullptr;
                std::printf("[netsock] ntop-FAIL af=%u size=%u src=%08x dst=%08x%s", a0, a3, a1,
                            a2, sp2 ? " bytes:" : " (src unreadable)");
                if (sp2) {
                    for (int q = 0; q < 16; q++) {
                        std::printf(" %02x", sp2[q]);
                    }
                }
                std::printf("\n");
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
        if (n == "inet_ntoa") {
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
    const std::uint32_t sptr = (n == "inet_pton") ? a1 : a0;
    if (!CheckRange(sptr, 1, false)) {
        Ret(regs, (n == "inet_addr") ? 0xFFFFFFFFu : 0u);
        return;
    }
    const char* s = GuestStr(sptr);
    if (n == "inet_addr") {
        Ret(regs, static_cast<std::uint32_t>(::inet_addr(s)));
        return;
    }
    if (n == "inet_aton") {
        struct in_addr ia {};
        const int r = ::inet_aton(s, &ia);
        if (r != 0 && a1 != 0 && CheckRange(a1, 4, true)) {
            std::memcpy(H(a1), &ia, 4);
        }
        Ret(regs, static_cast<std::uint32_t>(r));
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

}  // namespace abr
