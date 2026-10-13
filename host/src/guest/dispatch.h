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
#include <mutex>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"
#include "elf_loader.h"
#include "guest_memory.h"
#include "layout.h"

namespace abr {

class Runtime {
public:
    Runtime(GuestMemory& mem, LoadedModule& mod);
    ~Runtime() = default;

    GuestMemory& mem() { return mem_; }
    LoadedModule& mod() { return mod_; }
    Dynarmic::A32::Jit* jit() { return jit_; }
    void set_jit(Dynarmic::A32::Jit* jit) { jit_ = jit; }

    // Guest heap (bump allocator inside kHeapBase region).
    std::uint32_t GuestMalloc(std::uint32_t size);
    std::uint32_t HeapTop() { return heap_brk_; }
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

    // Validates a guest range before host-side bulk access.
    bool CheckRange(std::uint32_t guest, std::size_t size, bool write);
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
                              std::uint32_t arg2);
    std::uint32_t PthreadKeyCreate(std::uint32_t key_ptr, std::uint32_t destr);
    std::uint32_t PthreadGetSpecific(std::uint32_t key);
    std::uint32_t PthreadSetSpecific(std::uint32_t key, std::uint32_t value);
    // pthread_once via control-flow hijack. Returns true if hijacked (caller
    // must return from the callback immediately without Ret()).
    bool PthreadOnce(std::uint32_t control, std::uint32_t routine,
                     std::array<std::uint32_t, 16>& regs);

    // --- JNI bridge (bring-up): mock JNIEnv table + string handles. ---
    // JniEnv() returns a fake JNIEnv* (guest-data word holding table VA).
    // NewStringUTF() interns a host string, returns opaque jstring handle
    // (== guest VA of its NUL-terminated UTF-8 bytes; guest never derefs it
    // directly, only via GetStringUTFChars which returns the same pointer).
    std::uint32_t JniEnv();
    std::uint32_t NewStringUTF(const char* s);
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
    void SetApkPath(const std::string& path);

    // __gnu_Unwind_Find_exidx: binary search of .ARM.exidx (prel31), returns
    // entry VA or 0. Lets the in-module unwinder do its real job instead of
    // degrading on a stubbed NULL.
    std::uint32_t FindExidx(std::uint32_t pc);

    // Fill 2014-era Bionic ctype tables (_ctype_/_tolower_tab_/_toupper_tab_)
    // and resolve their data imports. BSD layout (bionic copied it): char
    // classes U=1 L=2 N=4 S=8 P=16 C=32 B=64 X=128, indexed [c], EOF slot [-1].
    // Game indexes bytes (ldrb) — verified at the 0x45C17E ctype lookup.
    void InitDataImports();

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
    std::uint32_t PthreadJoin(std::uint32_t tid, std::uint32_t retval_out);
    std::uint32_t PthreadDetach(std::uint32_t tid);
    std::uint32_t ErrnoSlot();
    void set_callbacks(Dynarmic::A32::UserCallbacks* cb) { cb_ = cb; }

    std::atomic<std::uint64_t> svc_count{0};
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

private:
    using Regs = std::array<std::uint32_t, 16>;
    void CallImport(std::uint32_t idx, Regs& regs);
    void CallJni(std::uint32_t fn, Regs& regs);
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
    std::uint32_t heap_brk_ = kHeapBase;
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
    std::unordered_map<std::uint32_t, std::uint32_t> heap_sizes_;  // guest addr -> size

    // Worker thread states (guest tid -> host). Main thread is tid 1.
    struct ThreadState {
        std::thread th;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::uint32_t retval = 0;
        bool detached = false;
        bool joined = false;
        std::string last_svc = "?";  // HARNESS_LASTSVC triage: parked import
        std::string svc_hist;        // ring of recent imports (path shape)
    };
    std::unordered_map<std::uint32_t, std::shared_ptr<ThreadState>> threads_;
    std::uint32_t next_tid_ = 2;
    std::unordered_map<std::uint32_t, std::uint32_t> errno_slots_;  // tid -> guest slot
    std::uint32_t next_errno_slot_ = 0;  // bump from kGuestDataBase+0x104

    // pthread_once hijack state: run the init routine inline by redirecting
    // guest control flow (no nested Run). See PthreadOnce().
    struct OnceHijack {
        bool active = false;
        std::uint32_t control = 0;
        std::array<std::uint32_t, 16> saved_regs{};
    };
    OnceHijack once_hijack_;

    // JNI bridge state.
    std::uint32_t jni_table_ = 0;   // guest VA of 256-entry function table
    std::uint32_t jni_env_ = 0;     // guest VA of JNIEnv* (word -> jni_table_)
    std::uint32_t jni_str_next_ = 0;  // bump pointer for interned UTF-8 bytes
    std::uint32_t jvm_table_ = 0;   // guest VA of 8-entry JavaVM table
    std::uint32_t jvm_ = 0;         // guest VA of JavaVM* (word -> jvm_table_)
    std::uint32_t jni_obj_next_ = 0;  // bump pointer for class/method handles
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

    // File-bridge state.
    std::string files_prefix_ = "/data/data/com.rovio.angrybirdsrio/files";
    std::string files_hostbase_;  // set by harness (SetFilesDir)
    struct HostFile {
        FILE* f = nullptr;
    };
    std::unordered_map<std::uint32_t, HostFile> file_map_;  // guest FILE* -> host
    std::unordered_map<std::uint32_t, int> fd_map_;         // guest fd -> host fd
    std::uint32_t next_guest_fd_ = 100;
    // Leaf lock for the three maps above (+next_guest_fd_): brief scoped
    // holds around lookups/inserts/erases ONLY — never across syscalls
    // (a worker blocked in ::read under the old whole-bridge mutex_ froze
    // every other thread; 2026-10-10). No nesting: never call GuestMalloc
    // or other mutex_-taking code while holding it.
    std::mutex io_mutex_;
    struct HostDir {
        DIR* d = nullptr;
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
    std::unordered_map<std::uint32_t, std::uint32_t> asset_mgr_map_;  // mgr -> 1
    std::unordered_map<std::uint32_t, std::uint32_t> jni_array_lens_;  // handle -> len

    struct Mapping {
        std::uint32_t start, end;
        int prot;
    };
    std::vector<Mapping> mappings_;
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
