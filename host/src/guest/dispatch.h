// SVC dispatch + Dynarmic memory callbacks + guest heap.
// swi == 0            -> HALT (stop Run cleanly)
// 1 .. imports.size() -> imported function trampoline
// 0x100000 + i        -> JNI function (fatal until the JNI bridge lands)
#pragma once
#include <array>
#include <bitset>
#include <atomic>
#include <shared_mutex>
#include <semaphore.h>
#include <zlib.h>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mapping_table.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"
#include "appid.h"
#include "arm_hwcap.h"
#include "diag_counters.h"
#include "import_ids.h"
#include "apk_vfs.h"
#include "elf_loader.h"
#include "guest_memory.h"
#include "layout.h"
#include "live_heap.h"

namespace abr {

// Cached env flag (perf): getenv() is a linear environ scan; hot paths use
// this instead. Defined in dispatch.cpp; read-once (all HARNESS_* setenv
// happen once pre-guest, nothing unsets).
bool EnvOn(const char* name);

// Drops this thread's copy of its GL buffer bindings (realgl.cpp): at every
// Java->native entry, guest JNI call into Java, eglMakeCurrent and
// eglReleaseThread -- every point where its GL context can change.
void ForgetGlBindings();
#ifdef ABR_DIAG_COUNTERS
void RealGlDiagDump();  // [counters] lines: GL state calls that repeat the set state
#endif

// Master switch for the diagnostic layer: the
// per-call / per-frame tracing and the hot-path logs. Backed by HARNESS_DIAG,
// default OFF for shipping (see PLAN.md batch A/B). Explicit values "0",
// "off", "false", "-" and "" mean OFF; any other value means ON.
bool DiagOn();

// JIT optimization level (2026-10-17, PLAN.md C1). The shipping default is now
// all_safe_optimizations: measured ~17.5% LESS CPU than no_optimizations on the
// same title-screen workload (CPU ticks over a fixed 90 s window, diag off;
// see AGENTS.md 134). HARNESS_NOOPT=1 restores the old bring-up mode.
// HARNESS_SAFEOPTS is no longer read (it is a legacy alias for "safe", which is
// now the default). Keep all four UserConfig sites + the `flags:` print on this
// one helper so they can never drift apart.
inline bool JitSafeOpts() { return !EnvOn("HARNESS_NOOPT"); }
// HARNESS_NOFASTDISPATCH=1: every indirect branch goes through the C++ dispatcher.
inline bool FastDispatchOn() { return !EnvOn("HARNESS_NOFASTDISPATCH"); }
inline Dynarmic::OptimizationFlag JitOptimizations() {
    if (!JitSafeOpts()) {
        return Dynarmic::no_optimizations;
    }
    return FastDispatchOn() ? Dynarmic::all_safe_optimizations
                            : Dynarmic::all_safe_optimizations & ~Dynarmic::OptimizationFlag::FastDispatch;
}
// A guest signal is pending for the thread whose Jit stops with this reason.
inline constexpr Dynarmic::HaltReason kSignalHalt = Dynarmic::HaltReason::UserDefined4;
// Runs the guest handlers of this thread's pending, unblocked signals (no-op
// when there are none or no Runtime is bound to the thread).
void DeliverGuestSignalsHere();
// While guest signals are in use, a thread tells the runtime when it starts
// and stops executing guest code (see Runtime::SigThread): entering waits
// until no handler is running on its behalf; resuming does the same without
// counting a new run level.
void GuestSigEnterJit();
void GuestSigResumeJit();
void GuestSigLeaveJit();

// Jit::Run, resumed after a code-cache invalidation or a guest signal. A
// guest that writes code (a JIT such as Mono's) calls cacheflush; the Jit
// then stops at the next block boundary with CacheInvalidation, drops the
// stale blocks, and expects to be run again. A signal sent to this thread
// stops it with kSignalHalt; the handler runs, then the thread continues.
// Neither stop is the guest's halt.
inline Dynarmic::HaltReason RunJit(Dynarmic::A32::Jit& jit) {
    constexpr auto kResume = static_cast<std::uint32_t>(Dynarmic::HaltReason::CacheInvalidation) |
                             static_cast<std::uint32_t>(kSignalHalt);
    GuestSigEnterJit();
    for (;;) {
        const auto raw = static_cast<std::uint32_t>(jit.Run());
        if (raw & static_cast<std::uint32_t>(kSignalHalt)) {
            DeliverGuestSignalsHere();
            GuestSigResumeJit();
        }
        const std::uint32_t hr = raw & ~kResume;
        if (hr != 0) {
            GuestSigLeaveJit();
            return static_cast<Dynarmic::HaltReason>(hr);
        }
    }
}
// Fastmem (direct host access for guest loads/stores): always on in the
// product, where a boot without it is ~10x slower; opt-in on the PC harness.
inline bool JitFastmemOn() {
#ifdef __ANDROID__
    return true;
#else
    return std::getenv("HARNESS_FASTMEM") != nullptr;
#endif
}
// One code cache for all Jits (Runtime::NewJit); HARNESS_NOJITSHARE=1 gives
// every Jit its own cache again.
inline bool JitShareOn() { return !EnvOn("HARNESS_NOJITSHARE"); }
// pthread_getspecific answered in guest code (Runtime::BindTls); with
// HARNESS_NOTLSTHUNK=1 no Jit gets a block, so every call is a host call.
inline bool TlsThunkOn() { return !EnvOn("HARNESS_NOTLSTHUNK"); }

// G1.1 (2026-10-18, PLAN.md): JIT cycle counting is pure overhead here. The
// A32 callbacks make AddTicks a no-op and GetTicksRemaining return a constant
// 1e9 (dispatch.h), so the per-block/per-SVC cycle accounting can never affect
// control flow. Kept as one constant so all four UserConfig sites stay in sync.
inline constexpr bool kJitCycleCounting = false;

// Bionic arm32 struct dirent: d_name starts after u64 d_ino, s64 d_off,
// u16 d_reclen and u8 d_type (NDK dirent.h; offsetof checked with the NDK
// clang for armv7a). Guest code reads names at +19 (Rio 0x2c67b8, 0x508ed0).
inline constexpr std::uint32_t kGuestDirentNameOff = 19;
// Bionic arm32 sizeof(struct stat) (NDK sys/stat.h, checked with sizeof).
inline constexpr std::uint32_t kGuestStatSize = 104;
// Every Jit in the process shares one exclusive monitor (Dynarmic requires a
// single monitor for all Jits that touch the same memory, and a unique
// processor id per Jit: exclusive_monitor.h). This is the id pool size; the
// monitor's CheckAndClear is O(N) under a spin lock, so keep it modest. Live
// Jits at once: Java threads that entered the guest + running guest workers +
// up to 8 qsort comparator slots.
inline constexpr std::size_t kMaxGuestCpus = 64;
// Bytes of the guest jmp_buf our setjmp/longjmp use: r4-r11, sp, lr, s16-s31
// (d8-d15), FPSCR. Bionic's arm jmp_buf is 64 words, so this always fits.
inline constexpr std::uint32_t kGuestJmpBufBytes = 27 * 4;
// PROP_VALUE_MAX of the guest's bionic (sys/system_properties.h): value
// buffers passed to __system_property_get are this many bytes.
inline constexpr std::uint32_t kGuestPropValueMax = 92;
// poll() nfds above this is rejected with EINVAL (the kernel's limit is
// RLIMIT_NOFILE; the guests poll one to a few fds). Bounds the host vector.
inline constexpr std::uint32_t kGuestPollMaxFds = 4096;
// Bionic arm32 sizeof(struct statfs) (NDK sys/vfs.h, checked with sizeof).
inline constexpr std::uint32_t kGuestStatfsSize = 88;

// The JNI guest-data arena has two disjoint bump regions. Strings must never
// grow into the handle table: the old check only looked at kGuestDataSize and
// let a long enough string overwrite jni_obj_next_'s region.
inline constexpr std::uint32_t kJniStringStart = kGuestDataBase + 0x2100u;
inline constexpr std::uint32_t kJniStringLimit = kGuestDataBase + 0x3000u;
inline constexpr std::uint32_t kJniObjectStart = kJniStringLimit;
inline constexpr std::uint32_t kJniObjectLimit = kGuestDataBase + kGuestDataSize;

// Pure bounds predicates for the two regions. Exposed for fmt_test: a wrong
// alignment/overflow rule must fail at the last valid and first invalid slot
// before any guest write is attempted. `out` receives the aligned address on
// success and is untouched on failure.
bool JniStringFits(std::uint32_t next, std::size_t len, std::uint32_t* out);
bool JniObjectHandleFits(std::uint32_t next, std::uint32_t* out);

// D1 (2026-10-17, PLAN.md): the dlmalloc overflow firewall is OFF by default.
// Default OFF means no recursive_mutex and no live-block index walk on the
// memcpy/memmove/memset/strcpy/strcat/fgets/fread paths -- the write proceeds at
// its natural length. HARNESS_CLAMPWRITE=1 re-arms it.
// The two JNI GetString*Region calls pass force=true: there the return value
// is a FUNCTIONAL destination bound, not a firewall (an unbounded UTF-16 widen
// overran a 24 B block into the next chunk head), so it stays on regardless.
inline bool ClampWriteOn() { return EnvOn("HARNESS_CLAMPWRITE"); }

class Runtime;
// Deleter of a Jit from Runtime::NewJit: takes it off the runtime's record.
struct JitDrop {
    JitDrop() noexcept = default;
    explicit JitDrop(Runtime* r) noexcept : rt(r) {}
    void operator()(Dynarmic::A32::Jit* j) const;
    Runtime* rt = nullptr;
};

class Runtime {
public:
    Runtime(GuestMemory& mem, LoadedModule& mod);
    ~Runtime() = default;

    GuestMemory& mem() { return mem_; }
    LoadedModule& mod() { return mod_; }
    Dynarmic::A32::Jit* jit() { return jit_; }
    void set_jit(Dynarmic::A32::Jit* jit) { jit_ = jit; }
    std::uint32_t AllocTid() { return next_tid_.fetch_add(1, std::memory_order_relaxed); }
    // The process-wide exclusive monitor and its processor ids. Every Jit
    // factory acquires an id and passes Monitor() as global_monitor; the id is
    // released (and its reservation cleared) once that Jit no longer runs.
    Dynarmic::ExclusiveMonitor& Monitor() { return monitor_; }
    bool AcquireCpuId(std::size_t* id);
    void ReleaseCpuId(std::size_t id);
    // Every guest Jit comes from here. Jits whose config differs only in
    // processor_id share one code cache, so a block is compiled once per
    // process: a new thread runs code other threads already ran at full
    // speed instead of compiling it again (60-160 ms per thread measured,
    // which turned a benign engine race into a self-join abort; R-005).
    // Every live Jit is on record (a guest's cacheflush reaches all of them
    // when they do not share a cache); the deleter takes it off.
    using JitPtr = std::unique_ptr<Dynarmic::A32::Jit, JitDrop>;
    JitPtr NewJit(const Dynarmic::A32::UserConfig& cfg);
    // Deletes a Jit that left its JitPtr (release()).
    void ReleaseJit(Dynarmic::A32::Jit* j);

    // Guest heap (dlmalloc mspace inside the heap arena; quarantine on free).
    std::uint32_t GuestMalloc(std::uint32_t size);
    std::uint32_t GuestMemalign(std::uint32_t align, std::uint32_t size);
    // dlopen of a guest library already in the module (by file name, any
    // directory): a stable non-NULL handle, else 0.
    std::uint32_t GuestLibraryHandle(const std::string& name);
    int GuestLibraryIndex(std::uint32_t h) const;  // -1: not a library handle
    // Runs guest library `lib`'s initialisers once (first the guest libraries
    // it needs), on this thread, like the linker at dlopen time. A host calls
    // it for the library Java loads; the guest's dlopen for the others.
    // False when an initialiser did not return cleanly.
    bool InitGuestLibrary(int lib);
    // The host ran these initialisers itself (its own init loop).
    void MarkGuestLibraryInitialised(int lib);
    // The guest library with this file name (any directory), or -1.
    int GuestLibraryByName(const std::string& name) const;
    std::mutex lib_init_mu_;
    std::condition_variable lib_init_cv_;
    std::vector<std::uint32_t> lib_init_state_;  // 0 none, 1 running (by tid+1 << 2), 2 done
    std::uint32_t HeapTop() { return heap_base_ + kHeapSize; }
    std::uint32_t GuestCalloc(std::uint32_t n, std::uint32_t size);
    std::uint32_t GuestRealloc(std::uint32_t addr, std::uint32_t size);
    void GuestFree(std::uint32_t addr);

    // Guest mmap/munmap inside the mmap arena [kMmapArenaBase, kTrampBase).
    // host_fd < 0 = anonymous. Returns the guest address, or 0xFFFFFFFF
    // (MAP_FAILED) with the guest errno set.
    std::uint32_t GuestMmap(std::uint32_t addr, std::uint32_t size, int prot, int flags,
                            int host_fd, std::uint64_t offset);
    int GuestMunmap(std::uint32_t addr, std::uint32_t size);
    int HostMapArena(std::uint32_t addr, std::uint64_t len, int hprot, int share, bool anon,
                     int host_fd, std::uint64_t offset);
    int GuestMprotect(std::uint32_t addr, std::uint32_t size, int prot);

    // --- Guest signals (arm32 numbering; sigset_t = 32 bits, signals 1-32).
    // Off until a guest sends one (tkill/pthread_kill/kill): the Rovio
    // titles never do, so their waits, host calls and run loops are
    // unchanged; a thread already blocked when the first one comes is served
    // by a proxy (below).
    // sigaction/sigprocmask keep the guest's dispositions and masks; a sent
    // signal is pending on the target thread. A thread executing guest code
    // stops at its next block boundary (kSignalHalt) and runs the handler; a
    // thread in an interruptible wait (2 ms slices) runs it between slices;
    // any other thread outside guest code gets a proxy (below). The handler
    // runs on a callback Jit, on the thread's own guest stack, seeded with
    // its callee-saved registers (a GC that scans the stack sees them).
    //
    // A thread that is not executing guest code -- inside a host call, or
    // back in the host altogether (a Java thread between two native calls)
    // -- cannot stop for a signal itself. Such a thread is `outside`; a
    // signal for it is delivered by a proxy host thread that runs the
    // handler as that guest thread (its tid, TLS, errno, mask), on its guest
    // stack: below the sp it had when it made the host call, with its r4-r11
    // (`snap`), or from its stack top when it has no guest frames (`parked`).
    // `token` is held by whoever runs a handler for the thread; the thread
    // takes it before it executes guest code again, so it stays stopped while
    // a handler runs for it (a GC's stop-the-world needs exactly that).
    struct SigThread {
        std::atomic<std::uint32_t> pending{0};
        std::atomic<std::uint32_t> blocked{0};
        std::atomic<Dynarmic::A32::Jit*> jit{nullptr};
        std::atomic<int> host_tid{0};  // the kernel tid of the host thread
        std::recursive_mutex token;
        std::atomic<bool> outside{true};
        std::atomic<bool> parked{true};
        std::atomic<bool> proxy{false};  // a proxy is scheduled or running
        std::array<std::uint32_t, 16> snap{};
        std::uint32_t snap_cpsr = 0x10;
        // snap holds this host call's registers (taken only while signals
        // are in use); otherwise the thread's Jit registers are its state.
        std::atomic<bool> snap_valid{false};
        std::uint32_t stack_top = 0;
        // The HostCond this thread sleeps on in a guest cond wait (dump).
        std::atomic<const void*> wait_cv{nullptr};
        std::atomic<std::uint64_t> wait_wakes{0};  // the cond's wakes handed out when it slept
    };
    // CallSVC: the thread leaves guest code for a host call / comes back.
    void SigEnterHostCall(const std::array<std::uint32_t, 16>& regs, std::uint32_t cpsr,
                          bool blocking);
    void SigLeaveHostCall(bool blocking);
    // An import's or JNI function's name for a svc number (diagnostics).
    std::string SvcName(std::uint32_t swi) const;
    // A blocking host call: an import marked blocking, or a JNI call (Java).
    bool SvcBlocks(std::uint32_t swi) const {
        if (swi >= kSwiKuserBase) {
            return false;
        }
        if (swi >= kSwiJniBase) {
            return true;
        }
        return swi >= 1 && swi - 1 < mod_.imports.size() && mod_.imports[swi - 1].blocking;
    }
    // Runs the deliverable signals of an outside thread on its behalf.
    void ProxyDeliver(std::shared_ptr<SigThread> st, std::uint32_t tid);
    // The kernel tid running guest thread `tid`, or 0 when it is unknown.
    int HostTidOf(std::uint32_t tid);
    void RegisterSigThread(std::uint32_t tid, Dynarmic::A32::Jit* jit);
    // The thread's Jit is being deleted: signals must no longer touch it.
    void ForgetSigThreadJit(std::uint32_t tid);
    bool SignalsInUse() const { return signals_in_use_.load(std::memory_order_acquire); }
    // True when this thread has a pending signal it may take now.
    bool SignalDeliverable() const;
    // Delivers this thread's deliverable signals; false if a handler halted
    // the guest (the thread must stop).
    bool DeliverPendingSignals();
    std::uint32_t GuestSigaction(std::uint32_t sig, std::uint32_t act, std::uint32_t old);
    std::uint32_t GuestSigmask(std::uint32_t how, std::uint32_t set, std::uint32_t old);
    // Returns 0 or an errno value.
    std::uint32_t GuestSendSignal(std::uint32_t tid, std::uint32_t sig);
    std::uint32_t GuestSigsuspend(std::uint32_t mask_ptr);
    // Slice length of an interruptible wait while signals are in use.
    static constexpr long kSigSliceNs = 2000000;
    // This thread's guest stack (pthread_getattr_np); default: the main stack.
    static void SetThreadStack(std::uint32_t lo, std::uint32_t size);
    std::uint32_t PthreadAttr(ImportId id, const std::string& n, std::uint32_t a0, std::uint32_t a1,
                              std::uint32_t a2);
    // C++ static-local guards (__cxa_guard_acquire/release/abort, ARM C++
    // ABI: a 32-bit guard, bit 0 set once the object is constructed).
    // Returns acquire's result (1 = the caller constructs), 0 for release and
    // abort, or kSemBadPtr for a wild guard pointer.
    std::uint32_t CxaGuard(ImportId id, const std::string& n, std::uint32_t guard);
    bool EnsureApkIndex();
    // NDK objects the guest only holds opaquely (ALooper*, AInputQueue*,
    // AInputEvent*, AConfiguration*, ANativeWindow*, sensor objects): a
    // 32-bit handle in an unmapped guest range (a dereference halts cleanly)
    // for each live host pointer.
    std::uint32_t PtrHandle(void* p);
    void* PtrOf(std::uint32_t h);
    void DropHandle(std::uint32_t h);
    std::mutex ptr_mu_;
    std::unordered_map<void*, std::uint32_t> ptr_fwd_;
    std::unordered_map<std::uint32_t, void*> ptr_rev_;
    std::vector<std::uint32_t> ptr_free_;
    std::uint32_t ptr_next_ = 0xF2000000u;
    // The real AAssetManager behind a guest asset manager handle.
    std::unordered_map<std::uint32_t, void*> real_asset_mgrs_;
    void SetRealAssetManager(std::uint32_t guest, void* real) {
        std::lock_guard<std::mutex> l(ptr_mu_);
        real_asset_mgrs_[guest] = real;
    }
    // The NDK imports of a NativeActivity title (ALooper_*, AInputQueue_*,
    // AInputEvent_*, AMotionEvent_*, AKeyEvent_*, AConfiguration_*,
    // ANativeWindow_*, ASensor*); false if `n` is not one of them.
    bool NdkCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs);
    // True for an NDK function NdkCall serves (dlsym of a system library).
    static bool NdkServes(const std::string& n);
    // A trampoline for a host import made at run time (dlsym of a function a
    // system library would export and the host serves); its guest address,
    // or 0 when the trampoline page or the reserved table is full.
    std::uint32_t RuntimeImport(const std::string& name);
    std::uint32_t sys_dl_handle_ = 0;  // dlopen of a system library
    // EGL for a guest that drives its own context (Unity): forwarded to the
    // real EGL with handles for displays, configs, contexts and surfaces.
    // False when `n` is not an EGL import; a wild pointer halts inside.
    bool EglCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs);
    std::atomic<std::uint64_t> egl_swaps_{0};  // frames a guest presented
    // A guest AAssetManager over our APK (what AAssetManager_fromJava gives),
    // for a host that hands one to the guest itself (ANativeActivity); 0 when
    // the APK cannot be indexed.
    std::uint32_t NewGuestAssetManager();
    // zlib (inflate*/deflate*/crc32) over guest z_streams and bionic's
    // SHA1Init/Update/Final over a guest SHA1_CTX. False: a wild guest
    // pointer (the caller halts).
    bool ZlibCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs);
    bool Sha1Call(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs);
    // Wide characters (bionic 7: UTF-8 in every locale, wchar_t 32 bits,
    // mbstate_t 4 bytes): mbrtowc wcrtomb wcscoll wcsxfrm wcsftime getwc
    // putwc ungetwc. False: a wild guest pointer.
    bool WideCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs);
    // Name resolution in the guest's LP32 structs (getaddrinfo, freeaddrinfo,
    // gai_strerror, gethostbyname, gethostbyaddr, __get_h_errno), with
    // bionic's EAI_/AI_/h_errno numbers. Offline titles resolve only NULL,
    // numeric hosts and localhost; any other name gets EAI_NODATA /
    // HOST_NOT_FOUND, as a phone without network answers. False: a wild
    // guest pointer.
    bool NetDbCall(ImportId id, const std::string& n, std::array<std::uint32_t, 16>& regs);
    std::unordered_map<int, std::uint32_t> gai_msgs_;  // bionic EAI code -> guest text
    std::uint8_t mbrtowc_state_[4] = {0, 0, 0, 0};  // for ps == NULL, as bionic's static
    std::mutex zlib_mu_;
    std::unordered_map<std::uint32_t, std::unique_ptr<z_stream>> zstreams_;
    std::unordered_map<std::string, std::uint32_t> zlib_msgs_;  // host msg -> guest copy
    std::mutex guard_mu_;
    std::condition_variable guard_cv_;
    // Guest semaphores; kSemBadPtr asks the caller to halt on a wild pointer.
    static constexpr std::uint32_t kSemBadPtr = 0xFFFFFFFEu;
    std::uint32_t GuestSem(ImportId id, const std::string& n, std::uint32_t sem, std::uint32_t a1,
                           std::uint32_t a2);

    // Host pointer from guest address (no checks; callbacks validate first).
    template <typename T = std::uint8_t>
    T* H(std::uint32_t guest) {
        return reinterpret_cast<T*>(mem_.host(guest));
    }

    const char* GuestStr(std::uint32_t guest);
    // strnlen over guest memory without a size cap: scans mapping by mapping
    // with memchr. Returns false if a byte before the NUL (or before max) is
    // not readable -- the caller must then halt the guest, as a real libc
    // would fault. On success *len = min(strlen, max).
    bool GuestStrnlen(std::uint32_t s, std::uint32_t max, std::uint32_t* len);

    // Guest printf vararg cursor. Two shapes share it: inline APCS words then
    // stack (ai starts at the first free core reg; stack = regs[13] at entry),
    // or a guest va_list read as a flat word array (via_va; stack = va ptr).
    struct GuestFmtArgs {
        std::uint32_t gargs[4];
        int ai;
        std::uint32_t stack;
        bool via_va;
    };
    // Expand a guest printf/format string (C99 subset; unknown specs stay
    // literal). Shared by __android_log_print and the snprintf family so the
    // engine's own text is never silently empty. total_out (optional)
    // receives the length the expansion WOULD have -- C99's snprintf
    // semantics, which the guests use to size their own buffers.
    std::string ExpandPrintf(const std::string& fmt, GuestFmtArgs& st,
                             std::size_t* total_out = nullptr);

    // dlmalloc corruption tripwire count + guest pc at first trip (see
    // AbrHeapCorruptionHook). Polled by the watchdog.
    unsigned HeapCorruptionCount();
    std::uint32_t HeapCorruptionPc();
    // HUNT watch (HARNESS_HEAPWATCH): true = write lands outside every live
    // heap allocation (overflow); quarantined = UAF write. LOUD capped.
    // Runs on EVERY guest store when fastmem is off: hunt-only cost.
    bool HeapWatchViolation(std::uint32_t addr, std::size_t size);
    // Overflow firewall (dlmalloc era): bulk writes past a live chunk smash
    // dlmalloc headers -> host SIGSEGV with no witness. Exact-base writes
    // are clamped to the mapped size + LOUD (capped); interior-pointer
    // overflows stay visible (future work).
    std::uint32_t ClampHeapWrite(std::uint32_t dst, std::uint32_t len, const char* n,
                                 bool force = false);

    // D2 (2026-10-18): the live-block index surface. Insert/erase/find keep the
    // exact base semantics the old std::map had; Containing resolves an interior
    // pointer EXACTLY, by two mechanisms: an ordered side-index of the (few)
    // blocks larger than the probe's reach -- the only blocks whose interiors a
    // hash cannot reach -- and a bounded back-probe for everything smaller.
    // The two-tier shape is not taste: a real session showed every in-arena
    // containment query is either an exact base or an interior farther than
    // 512 B back (contain_ok=0), so a probe alone silently LOST the sscanf field
    // clamp on 1242 Boot-path destinations. All four are called with mutex_
    // already held; under DiagOn() they cross-check the (temporary, full) std::map
    // oracle, so exactness is measured, not asserted.
    void LiveIdxInsert(std::uint32_t base, std::uint32_t size);
    std::uint32_t LiveIdxErase(std::uint32_t base);       // 0 = was not live
    std::uint32_t LiveIdxFind(std::uint32_t base);        // 0 = not live
    bool LiveIdxContaining(std::uint32_t addr, std::uint32_t* base, std::uint32_t* size);
    // One-line index + consumer-counter report (diag only; see apphost.cpp).
    void PrintHeapIdx();
    // Validates a guest range before host-side bulk access.
    bool CheckRange(std::uint32_t guest, std::size_t size, bool write);
    // Refuse guest writes into [start,start+size) (RELRO after relocation).
    // Host-side writes (H()/SetData) bypass this by design.
    void AddWriteGuard(std::uint32_t start, std::uint32_t size, const char* what);
    // True when [addr,addr+size) lies inside a guard; *what names it.
    bool WriteGuardHit(std::uint32_t addr, std::size_t size, const char** what);
    bool NoteWriteGuard() { return ++write_guard_hits_ <= 32; }
    // Valid code addresses: module RX segments, trampoline page, HALT slot.
    bool IsCodeAddress(std::uint32_t va);

    void OnSvc(std::uint32_t swi, std::array<std::uint32_t, 16>& regs);

    // SVC histogram (HARNESS_SVCHIST=1): per-import call counts, dumped on demand.
    // Counts ride on the existing stub_hits_ map ("#trace:name" for imports,
    // "#jni:N" for JNI) — no per-call overhead added here.
    void SvcHistDump();

    std::uint32_t PthreadMutex(ImportId id, const std::string& n, std::uint32_t obj, std::uint32_t attr = 0);
    // Real rwlock map (MCSM-compare gap: guest imports 5 rwlock symbols).
    std::uint32_t PthreadRwlock(ImportId id, const std::string& n, std::uint32_t obj);
    std::uint32_t PthreadCond(ImportId id, const std::string& n, std::uint32_t cond, std::uint32_t arg1,
                              std::uint32_t arg2, bool monotonic = false);
    std::uint32_t PthreadKeyCreate(std::uint32_t key_ptr, std::uint32_t destr);
    std::uint32_t PthreadKeyDelete(std::uint32_t key);
    // At a guest thread's clean exit (POSIX): each key with a destructor and
    // a non-NULL value is reset to NULL and its destructor called with the
    // old value, in rounds, up to PTHREAD_DESTRUCTOR_ITERATIONS (4 on bionic).
    void RunKeyDestructors();
    // pthread_exit from a worker's own code: cleanup handlers run, then the
    // thread stops with kThreadExitHalt and exits with this value.
    static constexpr Dynarmic::HaltReason kThreadExitHalt = Dynarmic::HaltReason::UserDefined5;
    std::uint32_t PthreadGetSpecific(std::uint32_t key);
    std::uint32_t PthreadSetSpecific(std::uint32_t key, std::uint32_t value);
    // pthread_once via control-flow hijack. Returns true if hijacked (caller
    // must return from the callback immediately without Ret()).
    bool PthreadOnce(std::uint32_t control, std::uint32_t routine,
                     std::array<std::uint32_t, 16>& regs);
    // A guest thread can stop (fatal guest error, or its start routine
    // returning from inside an init routine) while it still owns pthread_once
    // controls that read "in progress". Release them: the init never completed,
    // so a later call must run it again -- otherwise a caller that waited would
    // wait forever. Runs on the dying thread, from its thread_local once stack.
    void ReleaseOnceControlsOnExit(std::uint32_t tid);

    // --- JNI bridge (bring-up): mock JNIEnv table + string handles. ---
    // JniEnv() returns a fake JNIEnv* (guest-data word holding table VA).
    // NewStringUTF() interns a host string, returns opaque jstring handle
    // (== guest VA of its NUL-terminated UTF-8 bytes; guest never derefs it
    // directly, only via GetStringUTFChars which returns the same pointer).
    std::uint32_t JniEnv();
    std::uint32_t NewStringUTF(const char* s);
    // Product host: a guest handle for a real Java object (a global ref of
    // ours behind it). As in JNI, a handle is a reference, not the object:
    // one made during a Java -> native call is that call's local reference
    // and goes when the call returns (or at DeleteLocalRef / PopLocalFrame);
    // a `permanent` one (the guest's NewGlobalRef, the host's own) lives
    // until DeleteGlobalRef. One made on a guest worker thread outside any call
    // lives until DeleteLocalRef, DetachCurrentThread or the thread's end, as
    // on a native-attached thread.
    std::uint32_t WrapObject(void* real, bool permanent = false);
    void* RealString(std::uint32_t handle);
#ifdef __ANDROID__
    void* StringMonitorObject(std::uint32_t handle);
    // Scope of the Java copies JavaObjectFor makes of guest strings: every
    // caller has one (CallJni does); they are deleted when it ends.
    class JavaCopies {
    public:
        JavaCopies();
        ~JavaCopies();

    private:
        std::size_t from_;
    };
#endif
    bool JniStringText(std::uint32_t handle, std::string& out);
    // The local frame of one Java -> native call (the host pushes one per
    // call it runs, the guest with PushLocalFrame).
    static void PushLocalFrame();
    void PopLocalFrame();
    // How many real objects the guest holds handles for; -1 when the runtime
    // lock is busy (a watchdog never waits for it).
    long WrappedObjectCount();
    bool JniStringStats(std::size_t* count, std::size_t* bytes);
    // Strings kept for good because a Java method returned them.
    std::atomic<std::uint64_t> jni_string_results_{0};
    void* UnwrapObject(std::uint32_t mock);
    // A guest reference as a real Java reference (a native's return value):
    // a wrapped object, a mock jstring as a new String, a mock jclass as its
    // real class. Null for 0 or an unknown handle. Product host only.
    void* JavaObjectFor(std::uint32_t handle);
    std::string JniClassName(std::uint32_t handle);
    // The real jfieldID behind a GetFieldID / GetStaticFieldID cookie
    // ("field Cls.name:sig"), cached; null if the field does not resolve.
    void* RealFieldId(std::uint32_t cookie, bool is_static);
    // A method/field cookie (as GetMethodID/GetFieldID make) for a real
    // java.lang.reflect.Method/Constructor/Field object; 0 if it cannot be
    // described.
    std::uint32_t CookieForReflected(std::uint32_t member, bool field);
    // Element size of a real primitive array from its class ("[I" -> 4), 0
    // for anything else.
    std::uint32_t JniArrayElemSize(std::uint32_t array_handle);
    std::unordered_map<std::uint32_t, void*> real_static_fields_;
    std::unordered_map<std::uint32_t, std::uint32_t> jni_char_copies_;  // guest copy -> 1
    std::unordered_map<std::uint32_t, void*> jni_string_monitors_;  // see StringMonitorObject
    // Direct ByteBuffers (GetDirectBufferAddress): their memory is the
    // host's, outside the guest. Each gets a guest shadow of its capacity,
    // filled from the buffer when the guest asks for the address; whatever
    // the guest wrote goes back when the Java -> native call that handed the
    // buffer over returns (FlushDirectBuffers, called by the host then).
    void FlushDirectBuffers();
    struct DirectBuf {
        std::uint32_t guest = 0, size = 0;
    };
    std::unordered_map<void*, DirectBuf> direct_bufs_;
    // NewByteArray(len): guest-heap buffer (zeroed), opaque jbyteArray handle
    // (== buffer VA, same convention as strings). Used by harness tests and,
    // later, by Java-side array delivery for nativeMixData.
    std::uint32_t NewByteArray(std::uint32_t len);
    // Jvm(): mock JavaVM* (guest-data word holding the JavaVM table VA).
    // Needed by JNI_OnLoad and the GetJNIEnv() helper (slot 0x6747D0).
    std::uint32_t Jvm();
    // JniNewString: internals backing for NewStringUTF + String-returning
    // Java methods. Registers content for Length/Region thunks.
    std::uint32_t JniNewString(const std::string& s);

    // Mapped-range tracking for safe callbacks.
    void AddMapping(std::uint32_t start, std::uint32_t size, int prot);
    // Records the module's LOAD segments in the mapping table, splitting the
    // segment that holds PT_GNU_RELRO at the (page-aligned) RELRO end so the
    // RELRO pages can later become read-only on their own.
    void AddModuleSegments(const LoadedModule& mod);
    // After relocation (InitDataImports): RELRO pages become read-only on the
    // host and in the mapping table, as bionic's linker does. A guest write
    // there is then a clean halt in every mode (fastmem stores fault too).
    void ProtectRelro(const LoadedModule& mod);
    bool CheckAccess(std::uint32_t addr, std::size_t size, bool write, bool exec);

    // --- file bridge (bring-up): guest paths under files-prefix map to a host
    // directory (harness: <project>/work/files). FILE*/DIR* are opaque guest
    // handles (guest-heap blocks); fds are small ints >= 100.
    void SetFilesDir(const std::string& guest_prefix, const std::string& host_base);
    void SetPrivateSdcard(bool on) { private_sdcard_ = on; }
    // Map a guest path to the host path (the file bridge's rule).
    std::string MapGuestPath(std::uint32_t guest_path);
    std::string MapGuestPathString(const std::string& g);
    // A 32-bit task's /proc/cpuinfo or /proc/self/auxv as a host file path.
    std::string Proc32File(const std::string& g);
    // The guest's environment: getenv/setenv/unsetenv and the `environ`
    // array (NULL-terminated "K=V" guest strings; its variable is
    // environ_var_). SetGuestEnv lets the host seed it before the guest runs.
    void SetGuestEnv(const std::string& key, const std::string& value);
    std::uint32_t GuestGetenv(const std::string& key);
    std::uint32_t GuestSetenv(const std::string& key, const std::string* value, bool overwrite);
    void RebuildEnvironLocked();
    std::mutex env_mu_;
    std::vector<std::pair<std::string, std::uint32_t>> env_;  // key, guest "K=V"
    std::uint32_t environ_var_ = 0;
    std::uint32_t environ_array_ = 0;
    // The guest's current directory (chdir/getcwd), guest spelling. Empty =
    // the default, the files dir, where bare relative names go (R-006).
    std::string cwd_;
    std::mutex cwd_mu_;
    std::string GuestCwd();

    // How many pthread_mutex_destroy calls were refused (EBUSY) because the
    // mutex was held. Diagnostic + test hook; under mutex_map_mu_.
    unsigned mutex_destroy_skipped_ = 0;

    // A guest mutex's host side: the host mutex of its type and its holder
    // book (holder guest tid, depth), written only by the thread that owns
    // the lock (no lock needed), read for the stall dump.
    struct HostMutex {
        explicit HostMutex(int t);
        ~HostMutex();
        HostMutex(const HostMutex&) = delete;
        HostMutex& operator=(const HostMutex&) = delete;
        pthread_mutex_t m;
        int type;  // 0 normal, 1 recursive, 2 errorcheck
        std::atomic<std::uint32_t> holder{0};
        std::atomic<std::int32_t> depth{0};
    };

    // A return address found on the guest stack.
    struct StackHit {
        std::uint32_t off = 0;
        std::uint32_t value = 0;
    };
    struct StackScan {
        unsigned words_read = 0;  // how many words were actually read
        unsigned hits = 0;        // return addresses found
    };
    // Scan [sp, sp+max_bytes) of the guest stack for return addresses.
    // Only words inside the MAPPED region are read: the guest address space
    // is one PROT_NONE 4GB reservation with the parts mapped on demand, so a
    // read past the end of the stack (or into a hole of the heap arena)
    // would take the HOST down with SIGSEGV. The scan stops at the first
    // unmapped word.
    StackScan ScanStackForReturnAddresses(std::uint32_t sp, std::uint32_t max_bytes,
                                         StackHit* hits, unsigned hit_cap);
    void SetApkPath(const std::string& path);
    // Packed files (apk_vfs.h): indexes assets/abport/ of base.apk and its
    // splits once SetFilesDir, SetApkPath and SetPrivateSdcard are done.
    // Returns the summary line for the boot log.
    std::string InitPackedFiles();
    // Per-title network policy from the packaging (default offline, R-005).
    void SetNetworkAllowed(bool on) { network_allowed_ = on; }
    bool NetworkAllowed() const { return network_allowed_; }
    // The original app's targetSdkVersion (0 = unknown: current behaviour).
    void SetGuestTargetSdk(int sdk) { guest_target_sdk_ = sdk; }

    // __gnu_Unwind_Find_exidx: binary search of .ARM.exidx (prel31), returns
    // entry VA or 0. Lets the in-module unwinder do its real job instead of
    // degrading on a stubbed NULL.
    std::uint32_t FindExidx(std::uint32_t pc);
    // .ARM.exidx table {base, count} of the loaded library that holds pc.
    void ExidxFor(std::uint32_t pc, std::uint32_t* base, std::uint32_t* count) const;
    void ProtectOneRelro(std::uint32_t relro_start, std::uint32_t relro_size);

    // Fill 2014-era Bionic ctype tables (_ctype_/_tolower_tab_/_toupper_tab_)
    // and resolve their data imports. Bionic 7.1.2 layout (libc/include/ctype.h):
    // U=1 L=2 N=4 S=8 P=16 C=32 X=0x40 B=0x80, indexed [c], EOF slot [-1].
    // Game indexes bytes (ldrb) — verified at the 0x45C17E ctype lookup.
    // X/B are NOT in alphabetical/bit order: swapping them breaks isxdigit
    // and isblank while leaving isalpha working, so a quick smoke test misses it.
    void InitDataImports();

    // ARM32 kuser helper page: mapped RX at 0xffff0000 with svc trampolines
    // for the four kernel helpers and a LOUD trap everywhere else. Called once
    // from the Runtime ctor; the page lives outside every other arena.
    void InitUserHelpers();

    // Real guest threads (pthread_create milestone): each worker gets its own
    // Dynarmic Jit sharing GuestMemory + this Runtime. ActiveJit() routes all
    // callback-side jit_ uses to the calling thread's Jit (main falls back to
    // jit_). GuestTid() is 1 on main, worker ids otherwise.
    Dynarmic::A32::Jit* ActiveJit();
    std::uint32_t GuestTid();
    // The host side of a guest cond (PthreadCond); the dump shows its wakes
    // and the last sender.
    struct HostCond {
        // The sleepers, oldest first, under m. A signal hands its wake to the
        // oldest one (a broadcast to all) and wakes exactly that one through
        // its own cv: no wake is lost and no sleeper wakes for another's.
        struct Waiter {
            pthread_cond_t cv;  // CLOCK_MONOTONIC, used with m
            bool woken = false;
            Waiter* next = nullptr;
            Waiter* prev = nullptr;
        };
        pthread_mutex_t m;
        Waiter* head = nullptr;
        Waiter* tail = nullptr;
        std::uint64_t wakes = 0;  // wakes handed out (dump)
        std::atomic<std::uint32_t> last_tid{0};
    };
    // Product host (apphost): bind a caller-created Jit + tid to this thread
    // (Java-created threads have no bridge ThreadState; same routing).
    void ThreadEnter(Dynarmic::A32::Jit* jit, std::uint32_t tid);
    // Product host thread exit: drop this tid's errno/TLS state before the
    // caller frees its guest stack and deletes its JIT.
    void ReapHostThread(std::uint32_t tid);
    std::uint32_t PthreadCreate(std::uint32_t tid_out, std::uint32_t attr, std::uint32_t start,
                                std::uint32_t arg);
    // End-of-run census: tid/done/retval for every worker (detached threads
    // can't be joined; _exit would silence them).
    void ThreadCensus();
    // Every guest thread the signal registry knows: where it is (running
    // guest code, inside a host call -- which import --, or back in the
    // host) and its pc/lr/sp. Racy reads, for a stall report only.
    void DumpGuestThreads();
#ifdef ABR_DIAG_COUNTERS
    // Stage D: one compact leak-counter line. Called by the product watchdog
    // every 30 s. No-op and no symbol in the shipping build.
    void DumpDiagCounters();
    // Translations whose guest bytes changed without a cacheflush (R-007).
    void DiagCheckCode();
#endif
    std::uint32_t PthreadJoin(std::uint32_t tid, std::uint32_t retval_out);
    std::uint32_t PthreadDetach(std::uint32_t tid);
    std::uint32_t ErrnoSlot();
    // Publish the host errno into the calling guest thread's errno cell. Guest
    // code that branches on errno (e.g. Space's "file missing = fresh install"
    // path) must not see a stale 0 after a failed libc call.
    void SetGuestErrno(int e);
    void set_callbacks(Dynarmic::A32::UserCallbacks* cb) { cb_ = cb; }

    // G1.2: monotonic diag counter; only ever read in diag/harness dumps, so
    // relaxed is enough (never an ordering dependency).
    std::atomic<std::uint64_t> svc_count{0};  // all threads; diag builds only
    // Guest->host calls made by the calling thread (no shared cache line).
    static std::uint64_t ThreadSvcCount();
    // Kuser entries 0..4 (barrier/cmpxchg/cmpxchg64/get_tls/bad). Relaxed:
    // only used to print the first hit of each entry under DiagOn().
    std::atomic<std::uint64_t> kuser_hits_[5] = {};
    // Raw-syscall numbers seen (bit i for nr i, bit 63 = "other/none").
    // Diagnostic only: under DiagOn() the first hit prints once.
    std::atomic<std::uint64_t> syscall_seen_{0};
    // Last import entered (any thread): stuck-update watchdog reads these
    // (no strings here - index resolved against mod_.imports at dump time).
    std::atomic<std::uint32_t> last_svc_idx_{0xFFFFFFFFu};
    std::atomic<std::uint32_t> last_svc_tid_{0};
    std::atomic<std::uint64_t> halt_count{0};
    std::atomic<std::uint64_t> bad_count{0};

    // Set on every abnormal callback halt (all UserDefined2/3 sites). Makes
    // StepRun stop instead of clearing a real halt and stepping into garbage
    // (which once faked a clean render pass). Ignored by Run().
    bool stop_step_ = false;

    // Stops the guest thread that made the current call once it returns (a
    // wild pointer or an impossible request): the clean-halt half of
    // "LOUD + HaltExecution" (AGENTS §6).
    void HaltGuest() {
        stop_step_ = true;
        if (Dynarmic::A32::Jit* j = ActiveJit()) {
            j->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
    }

    // True for the first few bad accesses (log), false once flooding.
    bool NoteBad() {
        bad_count++;
        return bad_count <= 10;
    }

    // Event ring (HARNESS_MALLOCLOG=1): last 200k malloc/free lines in RAM,
    // zero steady-state I/O; DumpEventRing() prints them on demand at
    // crash/hang sites. Own leaf mutex (never nested, never across calls).
    static constexpr std::uint32_t kRingN = 200000;
    char* ring_buf_ = nullptr;
    std::uint32_t ring_head_ = 0;
    std::uint64_t ring_total_ = 0;
    std::mutex ring_mu_;
    void RingPush(const char* line) {
        std::lock_guard<std::mutex> l(ring_mu_);
        if (!ring_buf_) {
            ring_buf_ = new (std::nothrow) char[(std::size_t)kRingN * 64];
            if (!ring_buf_) {
                return;
            }
        }
        char* slot = ring_buf_ + (std::size_t)(ring_head_ % kRingN) * 64;
        std::strncpy(slot, line, 63);
        slot[63] = 0;
        ring_head_++;
        ring_total_++;
    }
    void DumpEventRing() {
        std::lock_guard<std::mutex> l(ring_mu_);
        if (!ring_buf_ || ring_total_ == 0) {
            return;
        }
        const std::uint32_t n =
            ring_total_ > kRingN ? kRingN : (std::uint32_t)ring_total_;
        std::printf("[ring] dump %llu events (last %u kept)\n",
                    (unsigned long long)ring_total_, n);
        const std::uint32_t start = ring_head_ - n;
        for (std::uint32_t i = 0; i < n; i++) {
            std::printf("%s\n", ring_buf_ + (std::size_t)((start + i) % kRingN) * 64);
        }
        std::printf("[ring] end\n");
    }

private:
    using Regs = std::array<std::uint32_t, 16>;
    // Test-only bridge to the private import dispatcher (host/src/fmt_test.cpp).
    friend struct RuntimeTestAccess;
    void CallImport(std::uint32_t idx, Regs& regs);
    // Fast path for the math imports the guest calls in tight loops; the id is
    // resolved at load time (Import::fast_math) so the per-call dispatch costs
    // one switch instead of the import-name chain.
    void CallFastMath(std::uint8_t id, Regs& regs, std::uint32_t a0, std::uint32_t a1,
                      std::uint32_t a2, std::uint32_t a3);
    void CallJni(std::uint32_t fn, Regs& regs);
    void OnKuser(std::uint32_t idx, Regs& regs);
    void KuserSetFlags(bool ok);
    // Raw guest `svc #0` (ARM EABI, r7 = syscall number). The bundled LSS in
    // breakpad issues these directly; they must not be mistaken for kSwiHalt.
    void OnGuestSyscall(Regs& regs);
  public:
    // Public for the RealGL backend (same-module forwarding).
    void Ret(Regs& regs, std::uint32_t r0, std::uint32_t r1 = 0);
  private:
    void RetVoid(Regs& regs) { Ret(regs, regs[0], regs[1]); }
    void GuestMemCopy(ImportId id, Regs& regs, const std::string& n, std::uint32_t dst, std::uint32_t src,
                      std::uint32_t len);
    void GuestMemSet(ImportId id, Regs& regs, const std::string& n, std::uint32_t dst, int fill,
                     std::uint32_t len);
    // inet_* helpers live here (not inside CallImport): gotos to bad_ptr
    // cannot cross their automatic inits (2026-10-10).
    void InetThunk(ImportId id, Regs& regs, const std::string& n, std::uint32_t a0, std::uint32_t a1,
                   std::uint32_t a2, std::uint32_t a3);
    GuestMemory& mem_;
    LoadedModule& mod_;
    Dynarmic::A32::Jit* jit_ = nullptr;
    Dynarmic::A32::UserCallbacks* cb_ = nullptr;  // shared callbacks for worker Jits
    Dynarmic::ExclusiveMonitor monitor_{kMaxGuestCpus};
    std::mutex cpu_ids_mu_;
    std::bitset<kMaxGuestCpus> cpu_ids_used_;
    std::shared_mutex sem_map_mu_;  // guards sem_map_ (readers: post/wait)
    std::unordered_map<std::uint32_t, std::shared_ptr<sem_t>> sem_map_;  // GuestSem
    std::vector<std::uint32_t> lib_handles_;  // GuestLibraryHandle, per library
    std::uint32_t global_dl_handle_ = 0;      // dlopen(NULL)
    struct GuestSigAct {
        std::uint32_t handler = 0, mask = 0, flags = 0;
    };
    std::array<GuestSigAct, 33> sig_acts_{};
    std::mutex sig_mu_;  // sig_acts_, sig_threads_
    std::unordered_map<std::uint32_t, std::shared_ptr<SigThread>> sig_threads_;
    std::atomic<bool> signals_in_use_{false};
    // Runs entry(r0..r2) on a callback Jit with the given guest sp and
    // callee-saved registers r4-r11; false if it did not return cleanly.
    bool RunNestedOnStack(std::uint32_t entry, std::uint32_t r0, std::uint32_t r1,
                          std::uint32_t r2, std::uint32_t sp, const std::uint32_t* r4_r11);
    bool RunNested(std::uint32_t entry, std::uint32_t r0, std::uint32_t r1, std::uint32_t r2,
                   std::uint32_t sp, const std::uint32_t* r4_r11, std::int32_t& out);
    bool network_allowed_ = false;  // SetNetworkAllowed
    int guest_target_sdk_ = 0;      // SetGuestTargetSdk
    // One shared code cache per Jit configuration (NewJit).
    struct CodeCacheEntry {
        Dynarmic::A32::UserConfig cfg;
        std::shared_ptr<Dynarmic::A32::SharedCodeCache> cache;
    };
    std::mutex code_cache_mu_;
    std::mutex live_jits_mu_;
    std::unordered_set<Dynarmic::A32::Jit*> live_jits_;
    std::vector<CodeCacheEntry> code_caches_;
    void* heap_ms_ = nullptr;           // dlmalloc mspace over the heap arena
    std::uint32_t heap_base_ = kHeapBase;  // arena start (HARNESS_HEAPBASE)
    std::uint64_t heap_used_ = 0;          // live bytes telemetry (OOM log)
#ifdef ABR_DIAG_COUNTERS
    DiagCounters diag_;
#endif
    // Quarantine: recently-freed blocks held out of reuse (UAF oracle).
    // Poisoned on free, verified on release to dlmalloc; a clobbered block
    // names a live use-after-free LOUD instead of corrupting silently.
    struct QEnt {
        std::uint32_t addr = 0;
        std::uint32_t size = 0;
    };
    // HARNESS_NORELEASE=1 (triage): every freed block is held for good, so
    // this list grows without bound and needs the set for its check.
    std::vector<QEnt> norelease_held_;
    std::unordered_set<std::uint32_t> norelease_set_;
    std::uint32_t mmap_next_ = kMmapArenaBase;
    std::recursive_mutex mutex_;
    // The guest heap's own lock: dlmalloc's mspace, the live-block index,
    // heap_used_, the quarantine/norelease lists and the audio array pool.
    // Not the runtime's mutex_ (malloc/free are among the hottest imports).
    // Order: mutex_ -> heap_mu_; heap code never takes mutex_.
    std::recursive_mutex heap_mu_;
    // A lock held for a scope (mutex_ by default, or the heap's); diag builds
    // also add the wait to rt_wait[site].
    struct RtLock {
        std::recursive_mutex& m;
        RtLock(Runtime& rt, int site, std::recursive_mutex* which = nullptr)
            : m(which ? *which : rt.mutex_) {
#ifdef ABR_DIAG_COUNTERS
            const std::uint64_t t0 = DiagTicks();
            m.lock();
            rt.diag_.rt_wait[site].fetch_add(DiagTicks() - t0, std::memory_order_relaxed);
            rt.diag_.rt_wait_n[site].fetch_add(1, std::memory_order_relaxed);
#else
            (void)site;
            m.lock();
#endif
        }
        ~RtLock() { m.unlock(); }
        RtLock(const RtLock&) = delete;
        RtLock& operator=(const RtLock&) = delete;
    };
    std::unordered_map<std::string, std::uint64_t> stub_hits_;

    // Host-side pthread object maps (guest addr -> host object).
    // Host sync objects behind guest mutex/rwlock/cond addresses. Shared
    // ownership: every operation takes its own reference under mutex_ before
    // it blocks, so a guest *_destroy (which only drops the map entry) can
    // never free an object another thread is still waiting in. The deleter
    // runs pthread_*_destroy when the last user lets go.
    // Guest mutex -> host mutex. Its own reader/writer lock (mutex_map_mu_),
    // never the runtime's mutex_: lock/unlock only read it. Order: mutex_
    // (PthreadCond) -> mutex_map_mu_.
    std::shared_mutex mutex_map_mu_;
    std::unordered_map<std::uint32_t, std::shared_ptr<HostMutex>> mutex_map_;
    void NoteLocked(HostMutex& hm);
    void NoteUnlocking(HostMutex& hm, std::uint32_t obj);
    std::atomic<unsigned long> mutex_foreign_unlocks_{0};
    std::unordered_map<std::uint32_t, std::shared_ptr<pthread_rwlock_t>> rwlock_map_;
    std::unordered_map<std::uint32_t, std::shared_ptr<HostCond>> cond_map_;
    std::unordered_map<std::uint32_t, std::uint32_t> key_map_;  // key -> guest destructor (0: none)
    std::uint32_t next_key_ = 1;
    // Guest thread -> guest address of its pthread_*specific values, kTlsKeys
    // words read and written without a lock, also by a proxy standing in for
    // the thread or by a key delete.
    std::unordered_map<std::uint32_t, std::uint32_t> tls_blocks_;
    std::vector<std::uint32_t> tls_free_;  // blocks of ended threads
    std::uint32_t next_tls_block_ = 0;
    std::atomic<std::uint32_t> tls_reaps_{0};  // a block may have changed hands
    std::uint32_t TlsFor(std::uint32_t tid);
    // Lets the running Jit's pthread_getspecific thunk read the caller's block.
    void BindTls();
    // D2 (2026-10-18): flat index of live heap blocks (base -> usable bytes)
    // replaces the std::map that used to sit here. The map allocated a host
    // node per live guest block and freed it on every guest free -- a hidden
    // second allocator round-trip plus O(log n) pointer chasing on the hottest
    // path in the emulator -- while its main consumer (the overflow firewall)
    // has been OFF by default since D1. See live_heap.h for the shape and the
    // semantics kept; harness.cpp has the differential self-test.
    abr::LiveHeapIndex live_idx_;
    // D2 exactness, second tier (2026-10-18): an ORDERED index of the live
    // blocks the bounded back-probe cannot reach, i.e. usable size >
    // kProbeReach. Ordering is what "which block contains this interior pointer"
    // needs and a hash cannot provide; keeping it for the large blocks only is
    // enough because an interior offset > reach is only possible inside a block
    // larger than the reach. Those blocks are the long-lived buffers, not the
    // allocation churn, so the host node per entry here is bounded (measured:
    // printed as big= / bigscan= next to the flat index it shadows) -- while the
    // hot small-block path stays allocation-free. INVARIANT: LiveIdxInsert is
    // only ever called for a base that is NOT live (fresh malloc/calloc, or
    // realloc after its erase), so a stale entry can never linger here.
    // Membership maintenance counts (big_ins_n_/big_del_n_) price this tier.
    static constexpr std::uint32_t kProbeReach = 512;  // 64 x 8 B probe steps
    std::map<std::uint32_t, std::uint32_t> big_live_;
    // D2 measurement counters (plain members: every call site already holds
    // mutex_). They answer "what did the old structure actually cost / did its
    // odd paths ever fire", instead of asserting it. Printed by PrintHeapIdx
    // under HARNESS_DIAG only.
    unsigned long long hx_contain_ok_ = 0;     // interior found by the back-probe
    unsigned long long hx_contain_big_ = 0;    // interior resolved by big_live_
    unsigned long long hx_contain_miss_ = 0;   // in-arena, not inside a live block
    unsigned long long hx_free_unknown_ = 0;   // free of a pointer we never handed out
    unsigned long long hx_realloc_unknown_ = 0;
    unsigned long long hx_pool_stale_ = 0;     // pooled array whose block was freed
    unsigned long long big_ins_n_ = 0;         // big-block inserts (side-index cost)
    unsigned long long big_del_n_ = 0;         // big-block erases

    // Worker thread states (guest tid -> host). Main thread is tid 1.
    struct ThreadState {
        std::thread th;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::uint32_t retval = 0;
        bool detached = false;
        bool joined = false;
        // Guest stack base (8MB) owned by this thread. Until 2026-10-18 nothing
        // ever freed it ("leak accepted in bring-up", while free was leak-only
        // anyway): the VideoRewards/time-sync retry loop creates a thread per
        // iteration and exhausted the whole 1GB arena (~123 x 8MB = 985MB used
        // -> OOM -> bad free -> USAGE_ERROR abort). Reaped exactly once by
        // ReapThreadStack (thread exit, or join/detach, whichever comes first).
        std::uint32_t stack = 0;
        std::string last_svc = "?";  // HARNESS_LASTSVC triage: parked import
        std::string svc_hist;        // ring of recent imports (path shape)
    };
    std::unordered_map<std::uint32_t, std::shared_ptr<ThreadState>> threads_;
    // One counter for every guest-visible thread id (guest workers AND Java
    // threads entering the guest): they key errno slots and TLS, so two
    // threads must never share one. It used to be two counters (workers from
    // 2, Java threads from 1000) that collided after 998 pthread_create.
    std::atomic<std::uint32_t> next_tid_{2};
    // Live PthreadCreate stacks (counted for the OOM line: thread churn is the
    // first thing that ever filled the arena, so name it there).
    unsigned thr_stacks_ = 0;
    unsigned long long thr_stack_bytes_ = 0;
    // Hand a finished thread's guest stack back to the heap (idempotent).
    void ReapThreadStack(std::uint32_t tid);
    // G2.6 (2026-10-18): drop a finished thread's per-tid state (errno slot
    // back to the free list, TLS entries removed). Called from ReapThreadStack.
    void ReapThreadTls(std::uint32_t tid);
    // Callback-JIT pool (PvZ re-enter pattern): host thunks run inside the
    // outer Jit's Run(), where nested Run() asserts. Guest callbacks (qsort
    // comparators) run on SEPARATE Jit instances (own stack, lr=halt slot,
    // loop till halt) with t_active_jit swapped for the duration. 8 slots
    // (cb_slots_.resize(8) on first use); exhaustion (pathological nesting)
    // fails the sort LOUD, array untouched.
    struct CbSlot {
        JitPtr jit;
        std::uint32_t stack = 0;
        bool busy = false;
    };
    std::vector<CbSlot> cb_slots_;
    // Last nested-run halt reason (diagnoses comparator failures).
    int cb_last_hr_ = -1;
    // Runs guest compar(pa,pb)->int32 on a callback JIT. Atomic from the
    // caller's view: false = do not commit anything.
    bool RunComparator(std::uint32_t compar, std::uint32_t pa, std::uint32_t pb,
                       std::int32_t& out);
    std::unordered_map<std::uint32_t, std::uint32_t> errno_slots_;  // tid -> guest slot
    std::uint32_t next_errno_slot_ = 0;  // bump from kGuestDataBase+0x104
    // G2.6: slots of finished threads, handed out again before bumping (the
    // pool is finite: kGuestDataBase+0x2000 => ~1979 slots).
    std::vector<std::uint32_t> errno_free_;

    // pthread_once hijack state: run the init routine inline by redirecting
    // guest control flow (no nested Run). See PthreadOnce(). A *stack* is
    // required: an init routine may legitimately call pthread_once on a
    // different control variable (observed in Space init #9). Only the same
    // control reappearing while in progress is real (pathological) recursion.
    struct OnceHijack {
        std::uint32_t control = 0;
        std::uint32_t owner_tid = 0;  // guest thread that ran the init routine
        std::array<std::uint32_t, 16> saved_regs{};
    };
    static constexpr int kOnceMaxDepth = 16;
    // Per-thread, NOT shared: the stack records "this thread is inside these
    // init routines". A shared stack let a bystander thread's return pop
    // another thread's slot (see games/space/JOURNAL.md, kSwiOnce).
    static thread_local std::array<OnceHijack, kOnceMaxDepth> once_stack_;
    static thread_local int once_depth_;

    // JNI bridge state.
    std::uint32_t jni_table_ = 0;   // guest VA of 256-entry function table
    std::uint32_t jni_env_ = 0;     // guest VA of JNIEnv* (word -> jni_table_)
    // Product host: real-ART <-> mock-handle object map (pointer-width
    // airtight: guest only ever sees 32-bit mock handles from 0xF1100000+;
    // real 64-bit pointers never enter guest regs/fields).
    std::uint32_t next_obj_ = 0xF1100000u;
    std::unordered_map<void*, std::uint32_t> obj_fwd_;
    std::unordered_map<std::uint32_t, void*> obj_rev_;
    std::unordered_set<std::uint32_t> obj_permanent_;  // WrapObject(permanent)
    std::vector<std::uint32_t> obj_free_;  // released handles, reused once the range is used up
    // Drops a real handle: its global ref and every map entry keyed by it.
    void ReleaseHandleLocked(std::uint32_t h);
    // A local handle of this thread goes now (DeleteLocalRef): out of its
    // frame, then released.
    void DropLocalLocked(std::uint32_t h);
#ifdef __ANDROID__
    // Real-call delegation (product host; see dispatch.cpp).
    bool RealCallObject(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
                        std::uint32_t a3, Regs& regs, const std::string& mdesc);
    // Real NewObject (audio + future real objects): same resolution as
    // RealCallObject, V-form marshal, NewObjectA, Wrap. False = mock path.
    bool RealNewObject(std::uint32_t fn, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3,
                       Regs& regs, const std::string& mdesc);
    std::unordered_map<std::string, void*> real_classes_;
    // The real class named `cls` (a cached global ref), or null with no
    // exception pending.
    void* RealClass(void* real_env, const std::string& cls);
public:
    // The app's ClassLoader (global ref): RealClass falls back to it when
    // FindClass fails on a thread attached from native code.
    void SetAppClassLoader(void* loader) { app_class_loader_.store(loader); }
    void* AppClassLoader() const { return app_class_loader_.load(); }
private:
    std::atomic<void*> app_class_loader_{nullptr};
    std::unordered_map<std::uint32_t, void*> real_methods_;
    // Real field IDs per mock GetFieldID cookie (audio buffer fields).
    std::unordered_map<std::uint32_t, void*> real_fields_;
    // Pinned real byte arrays: guest mock handle -> {real array, host
    // elems ptr, guest copy, len}. Release copies back per mode.
    struct ArrayPin {
        void* real_arr = nullptr;
        void* host_elems = nullptr;
        std::uint32_t guest_buf = 0;
        std::uint32_t len = 0;   // bytes
        int type = 1;            // 0 boolean .. 7 double (JNI table order); 1 = byte
    };
    std::unordered_map<std::uint32_t, ArrayPin> array_pins_;
#endif
    // Recycled guest copy-buffers (addr,len), exact-len match only. Declared
    // outside the ANDROID gate (used by GuestFree/Realloc coherence scans on
    // all targets; stays empty where real arrays never occur).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> array_pool_;
    std::uint32_t jni_str_next_ = 0;  // bump in [kJniStringStart,kJniStringLimit)
    std::uint32_t jvm_table_ = 0;   // guest VA of 8-entry JavaVM table
    std::uint32_t jvm_ = 0;         // guest VA of JavaVM* (word -> jvm_table_)
    std::uint32_t jni_obj_next_ = 0;  // bump in [kJniObjectStart,kJniObjectLimit)
    // Allocate one aligned 4-byte object/class/method handle. Caller holds
    // mutex_. Returns false after LOUD-halting; callers must not write then.
    bool JniAllocObjectHandle(std::uint32_t* out);
  public:
    std::unordered_map<std::uint32_t, std::string> jni_classes_;  // handle -> name
    std::unordered_map<std::uint32_t, std::string> jni_methods_;  // cookie -> desc
    std::unordered_map<std::uint32_t, std::string> jni_objects_;  // handle -> desc
    std::unordered_map<std::uint32_t, std::uint32_t> jni_objclass_;  // obj -> class
    std::unordered_map<std::uint32_t, std::string> jni_strings_;  // jstring -> UTF-8
    // Guest copies returned by GetStringUTFChars for a real-wrapped ART
    // string. They are not jstrings, so they must not live in jni_strings_;
    // ReleaseStringUTFChars frees them. Guarded by mutex_.
    std::unordered_set<std::uint32_t> jni_utf_chars_;

    // File-bridge state. The guest-visible prefix is derived from our own
    // package name (appid.h); SetFilesDir overrides it with what the caller
    // knows, and the file bridge also matches the package tail either way.
    std::string files_prefix_ = GuestFilesPrefix();
    std::string files_hostbase_;  // set by harness (SetFilesDir)
    // The app's own external dir, "<storage root>/Android/data/<pkg>": the
    // guest's spelling tail and its host home inside the app's data dir
    // (SetFilesDir), created on first use.
    std::string ext_tail_;
    std::string ext_hostbase_;
    std::once_flag ext_made_;
    // host.private_sdcard: all of shared storage is the app's own
    // "<files>/sdcard" (a title that keeps its data there, MCSM).
    bool private_sdcard_ = false;
    std::once_flag sdcard_made_;
    std::once_flag storage_parent_made_;
    struct HostFile {
        FILE* f = nullptr;
        std::uint32_t gfd = 0xFFFFFFFFu;  // guest fd bound to this FILE*
    };
    std::unordered_map<std::uint32_t, HostFile> file_map_;  // guest FILE* -> host
    std::unordered_map<std::uint32_t, int> fd_map_;         // guest fd -> host fd
    std::uint32_t next_guest_fd_ = 100;
    // Guest fd numbers released by close/fclose, reusable only after the
    // host fd has actually been closed. Kept under io_mutex_.
    std::vector<std::uint32_t> free_guest_fds_;
    // Guest fds opened on /dev/alarm (mapped to /dev/null host-side):
    // their ioctls are served from host clocks (tick source!). Same
    // leaf-lock discipline as fd_map_.
    std::unordered_set<std::uint32_t> alarm_fds_;
    void NoteAlarmFd(std::uint32_t g) {
        std::lock_guard<std::mutex> io(io_mutex_);
        alarm_fds_.insert(g);
    }
    bool IsAlarmFd(std::uint32_t g) {
        std::lock_guard<std::mutex> io(io_mutex_);
        return alarm_fds_.count(g) != 0;
    }
    void ForgetAlarmFd(std::uint32_t g) {
        std::lock_guard<std::mutex> io(io_mutex_);
        alarm_fds_.erase(g);
    }
    // Leaf lock for the three maps above (+next_guest_fd_): brief scoped
    // holds around lookups/inserts/erases ONLY — never across syscalls
    // (a worker blocked in ::read under the old whole-bridge mutex_ froze
    // every other thread; 2026-10-10). No nesting: never call GuestMalloc
    // or other mutex_-taking code while holding it.
    std::mutex io_mutex_;
    // Packed files (apk_vfs.h). vfs_ is built at boot, then only read (its
    // deletion marks have their own lock). fd_slice_: guest fds that read a
    // packed file through their host fd of the APK; every place that drops
    // or rebinds a guest fd number goes through SetFdSliceLocked, so a
    // reused number never inherits a slice. fd_slices_ = its size, read
    // without the lock so titles with no packed files pay one load.
    abvfs::Pack vfs_;
    bool vfs_on_ = false;
    std::unordered_map<std::uint32_t, std::shared_ptr<abvfs::Slice>> fd_slice_;
    std::atomic<std::size_t> fd_slices_{0};
    void SetFdSliceLocked(std::uint32_t g, std::shared_ptr<abvfs::Slice> s) {
        if (s) {
            fd_slice_[g] = std::move(s);
        } else {
            fd_slice_.erase(g);
        }
        fd_slices_.store(fd_slice_.size(), std::memory_order_relaxed);
    }
    std::shared_ptr<abvfs::Slice> FdSlice(std::uint32_t g) {
        if (fd_slices_.load(std::memory_order_relaxed) == 0) {
            return nullptr;
        }
        std::lock_guard<std::mutex> io(io_mutex_);
        const auto it = fd_slice_.find(g);
        return it == fd_slice_.end() ? nullptr : it->second;
    }
    // The held guest mutexes and their holders, for the stall dump. Never
    // blocks (the watchdog must not wedge on the deadlock it reports).
    void MutexOwnersDump() {
        std::shared_lock<std::shared_mutex> map_lock(mutex_map_mu_, std::try_to_lock);
        if (!map_lock.owns_lock()) {
            std::printf("[mutex] owners: SKIPPED (map lock contested)\n");
            return;
        }
        std::printf("[mutex] foreign unlocks so far: %lu\n",
                    mutex_foreign_unlocks_.load(std::memory_order_relaxed));
        int held = 0;
        for (const auto& kv : mutex_map_) {
            const std::int32_t d = kv.second->depth.load(std::memory_order_relaxed);
            if (d > 0) {
                held++;
                std::printf("[mutex] obj=%08x holder=%u depth=%d\n", kv.first,
                            kv.second->holder.load(std::memory_order_relaxed), d);
            }
        }
        if (held == 0) {
            std::printf("[mutex] owners: none held\n");
        }
    }
    struct HostDir {
        DIR* d = nullptr;
        std::string path;
        std::uint32_t ent = 0;  // readdir's guest struct dirent (280 bytes), lazily
        // Packed entries with no real file of the same name (apk_vfs.h),
        // listed after the real ones: name, is a directory.
        std::vector<std::pair<std::string, bool>> packed;
        std::size_t packed_next = 0;
    };
    std::unordered_map<std::uint32_t, HostDir> dir_map_;  // guest DIR* -> host

    // APK asset bridge (AAssetManager_*): central-directory index over the
    // APK file; opened assets live as guest-heap buffers (getBuffer model).
    struct ApkEntry {
        std::string name;
        std::uint32_t method = 0;  // 0 = stored, 8 = deflated
        std::uint64_t csize = 0, usize = 0;
        std::uint64_t data_off = 0;  // file offset of entry data
    };
    struct HostAsset {
        std::uint32_t guest_ptr = 0;
        std::uint64_t size = 0;
    };
    std::unordered_map<std::string, ApkEntry> apk_index_;
    std::string apk_path_;
    bool apk_indexed_ = false;
    std::unordered_map<std::uint32_t, HostAsset> asset_map_;  // guest AAsset* -> buf
    std::uint32_t next_asset_mgr_ = 1;
    // getauxval(AT_PLATFORM) answers a POINTER, so the string lives in the
    // guest's memory (allocated once, cached here).
    std::uint32_t platform_va_ = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> asset_mgr_map_;  // mgr -> 1
    std::unordered_map<std::uint32_t, std::uint32_t> jni_array_lens_;  // handle -> len

    MappingTable mappings_;  // thread-safe, see mapping_table.h
    // RELRO-style write guard: guest writes into these ranges are refused (and
    // named, capped) instead of silently corrupting a function pointer. The
    // loader registers the module's PT_GNU_RELRO here once relocations are done.
    struct WriteGuard {
        std::uint32_t start, end;
        const char* what;
    };
    std::vector<WriteGuard> write_guards_;
    std::atomic<unsigned> write_guard_hits_{0};
    // Real GLES backend (HARNESS_REALGL=1); null until first gl* call.
    // Harness-only bring-up; same forwarding serves the device build later.
    class RealGL* realgl_ = nullptr;
  public:
    class RealGL* RealGLBackend();
};

class GuestCallbacks : public Dynarmic::A32::UserCallbacks {
public:
    using VAddr = Dynarmic::A32::VAddr;
    using Exception = Dynarmic::A32::Exception;
    explicit GuestCallbacks(Runtime* rt) : rt_(rt) {}
    // Product host (apphost): real-Java delegation hook. Returns the calling
    // host thread's real JNIEnv*, or null (harness: always null -> mocks).
    // Set once by apphost at OnLoad; never in harness builds.
    static void* (*g_real_env_hook)();
    // Product host: the host function the real JVM is to call for a guest
    // native registered through RegisterNatives (class, method, signature,
    // guest entry), or null when none is left. Set by apphost; null in PC
    // tools, where RegisterNatives only logs.
    static void* (*g_native_stub_hook)(const char* cls, const char* name, const char* sig,
                                       std::uint32_t fn);

    std::uint8_t MemoryRead8(VAddr addr) override;
    std::uint16_t MemoryRead16(VAddr addr) override;
    std::uint32_t MemoryRead32(VAddr addr) override;
    std::uint64_t MemoryRead64(VAddr addr) override;
    std::optional<std::uint32_t> MemoryReadCode(VAddr addr) override;
    // Translation-time diagnostics (HARNESS_TRANSLOG, HARNESS_PCTRACE); emits no IR.
    void PreCodeTranslationHook(bool is_thumb, VAddr pc,
                                Dynarmic::A32::IREmitter& ir) override;
    void MemoryWrite8(VAddr addr, std::uint8_t v) override;
    void MemoryWrite16(VAddr addr, std::uint16_t v) override;
    void MemoryWrite32(VAddr addr, std::uint32_t v) override;
    void MemoryWrite64(VAddr addr, std::uint64_t v) override;

    // STREX: Dynarmic's monitor checks the reservation, then calls these with
    // the value LDREX saw (`e`); the store must succeed only if memory still
    // holds it, atomically against every other thread (a real CAS). The arm64
    // backend never uses fastmem for exclusives, so all of them come here.
    bool MemoryWriteExclusive8(VAddr addr, std::uint8_t v, std::uint8_t e) override {
        return ExclusiveWrite(addr, v, e);
    }
    bool MemoryWriteExclusive16(VAddr addr, std::uint16_t v, std::uint16_t e) override {
        return ExclusiveWrite(addr, v, e);
    }
    bool MemoryWriteExclusive32(VAddr addr, std::uint32_t v, std::uint32_t e) override {
        return ExclusiveWrite(addr, v, e);
    }
    bool MemoryWriteExclusive64(VAddr addr, std::uint64_t v, std::uint64_t e) override {
        return ExclusiveWrite(addr, v, e);
    }

    void InterpreterFallback(VAddr pc, std::size_t n) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(VAddr pc, Exception ex) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return UINT64_C(1000000000); }

private:
    template <typename T>
    T Read(VAddr addr, bool exec);
    template <typename T>
    void Write(VAddr addr, T v);
    template <typename T>
    bool ExclusiveWrite(VAddr addr, T v, T expected) {
        const char* gwhat = nullptr;
        Dynarmic::A32::Jit* self = rt_->ActiveJit();  // the thread doing the STREX
        if ((addr & (sizeof(T) - 1)) != 0) {
            // ARMv7: an unaligned exclusive access is an alignment fault.
            if (rt_->NoteBad()) {
                std::printf("[mem] BAD unaligned exclusive write @ %08x size %zu -- halting\n",
                            addr, sizeof(T));
            }
            rt_->stop_step_ = true;
            if (self) {
                self->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            return false;
        }
        if (rt_->WriteGuardHit(addr, sizeof(T), &gwhat) ||
            !rt_->CheckAccess(addr, sizeof(T), true, false)) {
            if (rt_->NoteBad()) {
                std::printf("[mem] BAD exclusive write @ %08x size %zu%s%s -- halting\n", addr,
                            sizeof(T), gwhat ? " guard=" : "", gwhat ? gwhat : "");
            }
            rt_->stop_step_ = true;
            if (self) {
                self->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            return false;
        }
        return __atomic_compare_exchange_n(rt_->H<T>(addr), &expected, v, false,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    Runtime* rt_;
};

}  // namespace abr
