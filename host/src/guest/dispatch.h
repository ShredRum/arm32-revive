// SVC dispatch + Dynarmic memory callbacks + guest heap.
// swi == 0            -> HALT (stop Run cleanly)
// 1 .. imports.size() -> imported function trampoline
// 0x100000 + i        -> JNI function (fatal until the JNI bridge lands)
#pragma once
#include <array>
#include <atomic>
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

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"
#include "appid.h"
#include "arm_hwcap.h"
#include "diag_counters.h"
#include "elf_loader.h"
#include "guest_memory.h"
#include "layout.h"
#include "live_heap.h"

namespace abr {

// Cached env flag (perf): getenv() is a linear environ scan; hot paths use
// this instead. Defined in dispatch.cpp; read-once (all HARNESS_* setenv
// happen once pre-guest, nothing unsets).
bool EnvOn(const char* name);

// Master switch for the diagnostic layer: pc probes, the WATCHPC hook, the
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

// G1.1 (2026-10-18, PLAN.md): JIT cycle counting is pure overhead here. The
// A32 callbacks make AddTicks a no-op and GetTicksRemaining return a constant
// 1e9 (dispatch.h), so the per-block/per-SVC cycle accounting can never affect
// control flow. Kept as one constant so all four UserConfig sites stay in sync.
inline constexpr bool kJitCycleCounting = false;

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

class Runtime {
public:
    Runtime(GuestMemory& mem, LoadedModule& mod);
    ~Runtime() = default;

    GuestMemory& mem() { return mem_; }
    LoadedModule& mod() { return mod_; }
    Dynarmic::A32::Jit* jit() { return jit_; }
    void set_jit(Dynarmic::A32::Jit* jit) { jit_ = jit; }

    // Guest heap (dlmalloc mspace inside the heap arena; quarantine on free).
    std::uint32_t GuestMalloc(std::uint32_t size);
    std::uint32_t HeapTop() { return heap_base_ + kHeapSize; }
    std::uint32_t GuestCalloc(std::uint32_t n, std::uint32_t size);
    std::uint32_t GuestRealloc(std::uint32_t addr, std::uint32_t size);
    void GuestFree(std::uint32_t addr);

    // Guest mmap arena.
    std::uint32_t GuestMmap(std::uint32_t addr, std::uint32_t size, int prot, int flags, int fd,
                            std::uint32_t offset);
    int GuestMunmap(std::uint32_t addr, std::uint32_t size);

    // Host pointer from guest address (no checks; callbacks validate first).
    template <typename T = std::uint8_t>
    T* H(std::uint32_t guest) {
        return reinterpret_cast<T*>(mem_.host(guest));
    }

    const char* GuestStr(std::uint32_t guest);

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

    std::uint32_t PthreadMutex(const std::string& n, std::uint32_t obj);
    // Real rwlock map (MCSM-compare gap: guest imports 5 rwlock symbols).
    std::uint32_t PthreadRwlock(const std::string& n, std::uint32_t obj);
    std::uint32_t PthreadCond(const std::string& n, std::uint32_t cond, std::uint32_t arg1,
                              std::uint32_t arg2, bool monotonic = false);
    std::uint32_t PthreadKeyCreate(std::uint32_t key_ptr, std::uint32_t destr);
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
    // Product host: real-ART <-> mock-handle map (pointer-width airtight).
    std::uint32_t WrapObject(void* real);
    void* UnwrapObject(std::uint32_t mock);
    // Throw-to-catch emulation (unwinder stand-in, harness-only): consume a
    // pending redirect (set by ProbeRedir) by resuming ActiveJit at the pad
    // in ARM state. Returns true if redirected (caller must Run() again).
    bool ApplyRedirect();
    // Park a redirect from a probe (captures calling Jit for per-thread match).
    void ParkRedirect(std::uint32_t pad);
    // Park a redirect with r0 override (e.g. resume in a functor with a
    // runtime-found record instead of the exception object); 0 = keep regs.
    void ParkRedirectObj(std::uint32_t pad, std::uint32_t r0val);
    // TTFILE matcher parks pad + filter here (r1 override for the pad's
    // catch-clause switch); ApplyRedirect applies them.
    void ParkRedirectEx(std::uint32_t pad, std::uint32_t r1val);
    // Park a redirect with unwind state (frame fp + restore flags + sp
    // offset), applied AT RESUME (register writes at park time were
    // observed clobbered before resume).
    void ParkUnwind(std::uint32_t pad, std::uint32_t r1val, std::uint32_t fp, std::uint32_t flags,
                    std::int32_t spoff, std::uint32_t regoff = 28);

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
    bool CheckAccess(std::uint32_t addr, std::size_t size, bool write, bool exec);

    // --- file bridge (bring-up): guest paths under files-prefix map to a host
    // directory (harness: <project>/work/files). FILE*/DIR* are opaque guest
    // handles (guest-heap blocks); fds are small ints >= 100.
    void SetFilesDir(const std::string& guest_prefix, const std::string& host_base);
    // Map a guest path to the host path (the file bridge's rule).
    std::string MapGuestPath(std::uint32_t guest_path);

    // How many pthread_mutex_destroy calls were REFUSED because the mutex
    // was held (or a lock was in flight). Diagnostic + test hook.
    unsigned mutex_destroy_skipped_ = 0;

    // TEST ONLY: do not record the owner after a successful lock. It
    // simulates the window between the lock succeeding and the owner being
    // recorded -- the window pthread_mutex_destroy has to survive. False in
    // every real run.
    bool test_skip_mutex_owner_ = false;

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

    // __gnu_Unwind_Find_exidx: binary search of .ARM.exidx (prel31), returns
    // entry VA or 0. Lets the in-module unwinder do its real job instead of
    // degrading on a stubbed NULL.
    std::uint32_t FindExidx(std::uint32_t pc);

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
    // Product host (apphost): bind a caller-created Jit + tid to this thread
    // (Java-created threads have no bridge ThreadState; same routing).
    void ThreadEnter(Dynarmic::A32::Jit* jit, std::uint32_t tid);
    std::uint32_t PthreadCreate(std::uint32_t tid_out, std::uint32_t attr, std::uint32_t start,
                                std::uint32_t arg);
    // End-of-run census: tid/done/retval for every worker (detached threads
    // can't be joined; _exit would silence them).
    void ThreadCensus();
#ifdef ABR_DIAG_COUNTERS
    // Stage D: one compact leak-counter line. Called by the product watchdog
    // every 30 s. No-op and no symbol in the shipping build.
    void DumpDiagCounters();
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
    std::atomic<std::uint64_t> svc_count{0};
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
    // Pending (blocked-in) mutex lock: obj+tid set BEFORE the host lock
    // call, cleared after. Names WHAT a stuck thread wants.
    std::atomic<std::uint32_t> pend_lock_obj_{0};
    std::atomic<std::uint32_t> pend_lock_tid_{0};
    std::atomic<std::uint64_t> halt_count{0};
    std::atomic<std::uint64_t> bad_count{0};

    // Set on every abnormal callback halt (all UserDefined2/3 sites). Makes
    // StepRun stop instead of clearing a real halt and stepping into garbage
    // (which once faked a clean render pass). Ignored by Run().
    bool stop_step_ = false;

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
    // inet_* helpers live here (not inside CallImport): gotos to bad_ptr
    // cannot cross their automatic inits (2026-10-10).
    void InetThunk(Regs& regs, const std::string& n, std::uint32_t a0, std::uint32_t a1,
                   std::uint32_t a2, std::uint32_t a3);
    // Throw-to-catch emulation (unwinder stand-in): ProbeRedir halts at the
    // throw with pending_redir_ set; ApplyRedirect() resumes there.
    // Per-Jit: a halted thread must never redirect a different thread.
    // Unwind state (frame fp + flags) is applied AT RESUME (not at park):
    // anything guest-side between park and resume would clobber parked
    // register values (observed: r4/r6 rewritten post-park).
    std::uint32_t pending_redir_ = 0;
    Dynarmic::A32::Jit* pending_jit_ = nullptr;
    // r1 override for the pad (catch-clause switch value); ~0u = keep regs.
    std::uint32_t pending_r1_ = ~0u;
    // r0 override for the pad (functor record etc.); 0 = keep regs.
    std::uint32_t pending_r0_ = 0;
    // Frame fp for reg/sp restore; restore-mode flags (bit0: regs, bit1: sp).
    std::uint32_t pending_fp_ = 0;
    std::uint32_t pending_flags_ = 0;
    std::int32_t pending_spoff_ = 8;
    // r4 slot = [fp-regoff] (from the frame's prologue push pattern).
    std::uint32_t pending_regoff_ = 28;

    GuestMemory& mem_;
    LoadedModule& mod_;
    Dynarmic::A32::Jit* jit_ = nullptr;
    Dynarmic::A32::UserCallbacks* cb_ = nullptr;  // shared callbacks for worker Jits
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
    std::vector<QEnt> quarantine_;
    std::unordered_set<std::uint32_t> quarantine_set_;  // O(1) double-free check (vector scan was O(n^2))
    std::uint32_t mmap_next_ = 0x70000000u;
    std::recursive_mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> stub_hits_;

    // Host-side pthread object maps (guest addr -> host object).
    std::unordered_map<std::uint32_t, pthread_mutex_t*> mutex_map_;
    std::unordered_map<std::uint32_t, pthread_rwlock_t*> rwlock_map_;
    std::unordered_map<std::uint32_t, pthread_cond_t*> cond_map_;
    std::unordered_map<std::uint32_t, pthread_key_t> key_map_;
    std::uint32_t next_key_ = 1;
    std::unordered_map<std::uint64_t, std::uint32_t> tls_map_;  // (key<<32)|tid -> value
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
    std::uint32_t next_tid_ = 2;
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
        std::unique_ptr<Dynarmic::A32::Jit> jit;
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
#ifdef __ANDROID__
    // Real-call delegation (product host; see dispatch.cpp).
    bool RealCallObject(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
                        std::uint32_t a3, Regs& regs, const std::string& mdesc);
    // Real NewObject (audio + future real objects): same resolution as
    // RealCallObject, V-form marshal, NewObjectA, Wrap. False = mock path.
    bool RealNewObject(std::uint32_t fn, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3,
                       Regs& regs, const std::string& mdesc);
    std::unordered_map<std::string, void*> real_classes_;
    std::unordered_map<std::uint32_t, void*> real_methods_;
    // Real field IDs per mock GetFieldID cookie (audio buffer fields).
    std::unordered_map<std::uint32_t, void*> real_fields_;
    // Pinned real byte arrays: guest mock handle -> {real array, host
    // elems ptr, guest copy, len}. Release copies back per mode.
    struct ArrayPin {
        void* real_arr = nullptr;
        void* host_elems = nullptr;
        std::uint32_t guest_buf = 0;
        std::uint32_t len = 0;
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
    // Recorded AudioOutput native handle (J) from <init>(JIIII)V, for
    // mixdata arg synthesis (HARNESS_JLAST/JOVR "J").
    std::uint32_t audio_j_lo_ = 0, audio_j_hi_ = 0;
  public:
    std::uint32_t AudioJLo() const { return audio_j_lo_; }
    std::uint32_t AudioJHi() const { return audio_j_hi_; }
    std::unordered_map<std::uint32_t, std::string> jni_classes_;  // handle -> name
    std::unordered_map<std::uint32_t, std::string> jni_methods_;  // cookie -> desc
    std::unordered_map<std::uint32_t, std::string> jni_objects_;  // handle -> desc
    std::unordered_map<std::uint32_t, std::uint32_t> jni_objclass_;  // obj -> class
    std::unordered_map<std::uint32_t, std::string> jni_strings_;  // jstring -> UTF-8

    // File-bridge state. The guest-visible prefix is derived from our own
    // package name (appid.h); SetFilesDir overrides it with what the caller
    // knows, and the file bridge also matches the package tail either way.
    std::string files_prefix_ = GuestFilesPrefix();
    std::string files_hostbase_;  // set by harness (SetFilesDir)
    struct HostFile {
        FILE* f = nullptr;
    };
    std::unordered_map<std::uint32_t, HostFile> file_map_;  // guest FILE* -> host
    std::unordered_map<std::uint32_t, int> fd_map_;         // guest fd -> host fd
    std::uint32_t next_guest_fd_ = 100;
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
    // Guest-mutex owner tracking (deadlock triage): obj -> (holder
    // guest-tid, recursion depth). Maintained on successful lock/unlock
    // under io_mutex_ (never across the host lock call). Dumped by
    // MutexOwnersDump() when the stuck-update watchdog fires.
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, int>> mutex_owner_;
    void MutexNoteLocked(std::uint32_t obj, std::uint32_t tid) {
        std::lock_guard<std::mutex> io(io_mutex_);
        auto& e = mutex_owner_[obj];
        // Diagnostic layer (PLAN.md A): the owner map itself stays maintained
        // (MutexOwnersDump needs it for the watchdog); only the print is gated.
        if (DiagOn() && e.second > 0 && e.first != tid) {
            std::printf("[mutex] OWNER-CHANGE obj=%08x %u->%u (was depth %d)\n", obj, e.first,
                        tid, e.second);
        }
        e.first = tid;
        e.second++;
    }
    void MutexNoteUnlocked(std::uint32_t obj, std::uint32_t tid) {
        std::lock_guard<std::mutex> io(io_mutex_);
        auto it = mutex_owner_.find(obj);
        if (it == mutex_owner_.end()) {
            return;
        }
        if (it->second.first != tid) {
            std::printf("[mutex] UNLOCK-FOREIGN obj=%08x by=%u owner=%u\n", obj, tid,
                        it->second.first);
            return;
        }
        if (--(it->second.second) <= 0) {
            mutex_owner_.erase(it);
        }
    }
    void MutexOwnersDump() {
        // Watchdog robustness: never block here (a leaked guard would wedge
        // the watchdog itself, hiding the very deadlock it reports).
        std::unique_lock<std::mutex> io(io_mutex_, std::try_to_lock);
        if (!io.owns_lock()) {
            std::printf("[mutex] owners: SKIPPED (io guard contested)\n");
            return;
        }
        if (mutex_owner_.empty()) {
            std::printf("[mutex] owners: none held\n");
            return;
        }
        for (const auto& kv : mutex_owner_) {
            std::printf("[mutex] obj=%08x holder=%u depth=%d\n", kv.first, kv.second.first,
                        kv.second.second);
        }
    }
    struct HostDir {
        DIR* d = nullptr;
        // CPU-topology listing (/sys/devices/system/cpu): the game's only
        // consumer filters cpuN itself, but Bionic readdir_r writes a 280B
        // struct dirent while the guest passes a 24B buffer (game bug,
        // faithfully reproduced = heap corruption of neighbors: Fox BAD
        // 0x78616D63 + crash3 0x73344545, both entry-name bytes at buf+24).
        // Mitigation: serve only short cpuN/dot entries (a kernel without
        // long nodes; the game's own filter accepts nothing else).
        bool cpu_topo = false;
        std::string path;
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

    struct Mapping {
        std::uint32_t start, end;
        int prot;
    };
    std::vector<Mapping> mappings_;
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
    // HARNESS_EVENTPOST/FILL (synthetic-event intervention, harness-only):
    // pi captured from the walk (r6 after BL 0xC4654); callable recorded for
    // the free-thunk invocation marker. Guest VAs; 0 = none.
    std::uint32_t eventpost_pi_ = 0;
    std::uint32_t eventpost_callable_ = 0;
    // Loading-manager object last seen by the render hook (PokeManagerFlag);
    // HARNESS_SCENEAT reads it from here (never hardcoded). Guest VA.
    std::uint32_t last_mgr_ = 0;
    // Its +164 at last sight (2=loading per DIAG164). TAPBEAT governor
    // reads it (slow beat while loading cooks). 0xFFFFFFFF = unseen.
    std::uint32_t last_mgr164_ = 0xFFFFFFFFu;
    // Scene object last seen by the loading-builder hook (HARNESS_LASTTHIS);
    // HARNESS_BUILDER calls the menu builder with it. Guest VA.
    std::uint32_t last_this_ = 0;
    // Session-ish object last seen at D5994 entry (HARNESS_WATCHPC); the
    // login trigger links its forged account+32 to it (updates pass the real
    // one, the trigger forged zeros). Guest VA, 0 = none seen.
    std::uint32_t loginat_session_ = 0;
    // Live object last seen at a WATCHPC hit (HARNESS_WATCHPCSNAP=<hexlen>):
    // guest VA + host-side byte snapshot (first hit wins). Lets later code
    // forge a faithful copy without hardcoding heap addrs (which drift
    // ±0x290 run to run even with identical configs — retry PROVEN).
    std::uint32_t loginat_acct_ = 0;
    std::uint32_t loginat_acct_len_ = 0;
    std::uint8_t loginat_acct_snap_[64] = {};
  public:
    class RealGL* RealGLBackend();
};

class GuestCallbacks : public Dynarmic::A32::UserCallbacks {
public:
    using VAddr = Dynarmic::A32::VAddr;
    using Exception = Dynarmic::A32::Exception;
    explicit GuestCallbacks(Runtime* rt) : rt_(rt) { g_probe_cb = this; }
    // Execution-time probe target for HARNESS_PROBE (single callback set;
    // harness uses one Runtime per process for the main thread's Jit; worker
    // Jits share the same callbacks object... see ActiveJit routing).
    static GuestCallbacks* g_probe_cb;
    // Product host (apphost): real-Java delegation hook. Returns the calling
    // host thread's real JNIEnv*, or null (harness: always null -> mocks).
    // Set once by apphost at OnLoad; never in harness builds.
    static void* (*g_real_env_hook)();
    static std::uint32_t g_probe_pc;  // pc being probed (fill gates on it)
    static void ProbeDump(std::uint64_t r0, std::uint64_t r1, std::uint64_t lr);
    static void ProbeDump2(std::uint64_t r2, std::uint64_t r3, std::uint64_t sp);
    static void ProbeDump3(std::uint64_t r4, std::uint64_t r5, std::uint64_t r6);
    static void ProbeRedir(std::uint64_t r0, std::uint64_t r1, std::uint64_t lr);
    // Per-frame unwinder step (throw probe + resume trap share it).
    static void UnwNext(std::uint32_t thrown_ti);
    // _Unwind_Resume trap: continue the pad walk instead of spiraling.
    static void ResumeTrap(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_RDUMP=1: deep dump of the HTTP raiser entry ABI (PROBE3).
    static void RaiserDump(std::uint64_t r0, std::uint64_t r1, std::uint64_t r2);
    static void RaiserDump2(std::uint64_t r3, std::uint64_t lr, std::uint64_t sp);
    // HARNESS_RETSUCC=1: synthesize HTTP success at the raiser entry
    // (PROBE3) for the .../time endpoint (canned timestamp + test sig).
    static void SuccInject(std::uint64_t r0, std::uint64_t lr, std::uint64_t);
    // HARNESS_POKE1187 mid-update gate poke (r7 = gate slot address).
    static void PokeGateByte(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_POKE180=1: loading render-state advance. At 0x263338
    // (ldr r0,[r4,#180] in ShadowingSurfaceData draw), if [r4+180]==3 write 4
    // (flips the 0x263340 bne + switch mode). r4 is live — no hardcode.
    static void PokeShadowState(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_POKE164=1: [loading-manager+164]=0 at 0x283B5C entry (forces
    // the state-1/menu branch at the 0x283C34 fork).
    static void PokeManagerFlag(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_EVENTPOST=1: at 0xC465C (PROBE4 slot, right after the walk's
    // mov r6,r0), decide the pi the walk will use: mirror the natural one
    // into pi#1 when usable, else synthetic content. Then r6<=pi#1 (baked).
    static void EventDecide(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_EVENTPOST=1: at event-walk entry (0xC45FC, PROBE5 slot), fake a
    // nonzero subscriber count while natural is 0 (see EventDecide).
    static void EventEntry(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_LASTTHIS=1: record loading-builder r0 (scene object) at PROBE4.
    static void LastThis(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_WATCHPC=<hexpc>: regs + stack code-pointer backtrace at pc
    // (ladder forensics: who calls with null-derived args). Capped, no slot.
    static void WatchPC(std::uint64_t r0, std::uint64_t r1, std::uint64_t lr);
    static void WatchSP(std::uint64_t, std::uint64_t, std::uint64_t);
    // HARNESS_WATCHPC companion: stashes r4/r6/sp at the watched pc (the binder
    // checkers keep the context in r4 and the arg position in r6; probing only
    // r0/r1/lr hid both). Dev-only, emitted only for the watched pc.
    static void WatchRegs(std::uint64_t a, std::uint64_t b, std::uint64_t c);
    // HARNESS_DUMPR9=1: log session object (r9) + flag at a probe slot
    // (e.g. PROBE2=0xd726c, the [r9+120] check). Finds the object main
    // must observe for the sessions->menu handoff.
    static void DumpR9(std::uint64_t, std::uint64_t, std::uint64_t);

    std::uint8_t MemoryRead8(VAddr addr) override;
    std::uint16_t MemoryRead16(VAddr addr) override;
    std::uint32_t MemoryRead32(VAddr addr) override;
    std::uint64_t MemoryRead64(VAddr addr) override;
    std::optional<std::uint32_t> MemoryReadCode(VAddr addr) override;
    // Translation log (HARNESS_TRANSLOG=1): every translated instruction address.
    void PreCodeTranslationHook(bool is_thumb, VAddr pc,
                                Dynarmic::A32::IREmitter& ir) override;
    void MemoryWrite8(VAddr addr, std::uint8_t v) override;
    void MemoryWrite16(VAddr addr, std::uint16_t v) override;
    void MemoryWrite32(VAddr addr, std::uint32_t v) override;
    void MemoryWrite64(VAddr addr, std::uint64_t v) override;

    // LDREX/STREX support. Single-threaded bring-up: compare-and-swap against
    // the normal path is exact while no other thread writes concurrently.
    // (Multithreaded guests will need monitor-backed exclusives later.)
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
        if (rt_->WriteGuardHit(addr, sizeof(T), &gwhat)) {
            if (rt_->NoteWriteGuard()) {
                std::printf("[mem] %s write REFUSED (exclusive) @ %08x size %zu guest-pc=%08x\n",
                            gwhat, addr, sizeof(T), rt_->ActiveJit()->Regs()[15]);
            }
            return false;
        }
        if (!rt_->CheckAccess(addr, sizeof(T), true, false)) {
            if (rt_->NoteBad()) {
                std::printf("[mem] BAD exclusive write @ %08x size %zu -- halting\n", addr,
                            sizeof(T));
            }
            rt_->jit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return false;
        }
        T cur = 0;
        const std::uint8_t* p = rt_->H<const std::uint8_t>(addr);
        for (std::size_t i = 0; i < sizeof(T); i++) {
            cur |= T(p[i]) << (8 * i);
        }
        if (cur != expected) {
            return false;
        }
        std::uint8_t* w = rt_->H<std::uint8_t>(addr);
        for (std::size_t i = 0; i < sizeof(T); i++) {
            w[i] = std::uint8_t(v >> (8 * i));
        }
        return true;
    }
    Runtime* rt_;
};

// Session-flag watch address (DumpR9-armed; read by harness census).
extern std::uint32_t g_sess_watch;

}  // namespace abr
