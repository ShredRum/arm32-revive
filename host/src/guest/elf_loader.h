// Minimal ARM32 ET_DYN loader: maps PT_LOADs at linked addresses inside the
// 4GB reservation, applies REL/JMPREL relocations, emits SVC trampolines for
// every undefined import.
#pragma once

#include "fast_math.h"
#include "gl_ids.h"
#include "import_ids.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_memory.h"

namespace abr {

// Fast-path math imports: fast_math.h (FastMathForName, the load-time id).

// Imports that wait (for another thread, time or I/O) without moving guest
// pointers around: while a thread sits in one, a guest signal can be taken
// for it by a proxy (a GC may then treat it as stopped). Every other import
// is short and may write the guest heap (memcpy, realloc...), so its thread
// is stopped when it returns to guest code instead.
inline bool BlockingImportName(const std::string& n) {
    static const char* const kNames[] = {
        "sem_wait", "sem_timedwait", "pthread_cond_wait", "pthread_cond_timedwait",
        "pthread_cond_timedwait_monotonic", "pthread_cond_timedwait_monotonic_np",
        "pthread_mutex_lock", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_join",
        "nanosleep", "usleep", "sleep", "poll", "select", "epoll_wait", "accept", "connect",
        "recv", "recvfrom", "recvmsg", "read", "ALooper_pollAll", "ALooper_pollOnce",
        "sigsuspend", "eglSwapBuffers"};
    for (const char* k : kNames) {
        if (n == k) {
            return true;
        }
    }
    return false;
}

// Imports the guests call in tight loops, dispatched by a load-time id ahead
// of the import-name chain (Runtime::CallImport), like FastMath.
enum class HotImport : std::uint8_t {
    None, TlsGet, TlsSet, MutexLock, MutexUnlock, MemCopy, MemSet, MemSetAeabi, MemClr,
    MemCmp, StrLen, StrCmp
};
inline HotImport HotImportForName(const std::string& n) {
    if (n == "pthread_getspecific") return HotImport::TlsGet;
    if (n == "pthread_setspecific") return HotImport::TlsSet;
    if (n == "pthread_mutex_lock") return HotImport::MutexLock;
    if (n == "pthread_mutex_unlock") return HotImport::MutexUnlock;
    // memcpy with overlap stays a memmove: guests rely on that on bionic.
    if (n == "memcpy" || n == "__aeabi_memcpy" || n == "__aeabi_memcpy4" ||
        n == "__aeabi_memcpy8" || n == "memmove" || n == "__aeabi_memmove" ||
        n == "__aeabi_memmove4" || n == "__aeabi_memmove8") {
        return HotImport::MemCopy;
    }
    // Three argument orders: memset is (dest, c, n), __aeabi_memset* is
    // (dest, n, c) and __aeabi_memclr* is (dest, n).
    if (n == "memset") return HotImport::MemSet;
    if (n == "__aeabi_memset" || n == "__aeabi_memset4" || n == "__aeabi_memset8") {
        return HotImport::MemSetAeabi;
    }
    if (n == "__aeabi_memclr" || n == "__aeabi_memclr4" || n == "__aeabi_memclr8") {
        return HotImport::MemClr;
    }
    // The three most called imports of a Rovio frame (docs/minimum.md).
    if (n == "memcmp" || n == "bcmp") return HotImport::MemCmp;
    if (n == "strlen") return HotImport::StrLen;
    if (n == "strcmp") return HotImport::StrCmp;
    return HotImport::None;
}

// pthread_getspecific answered inside the JIT (assembled with the NDK clang,
// not hand-encoded). r1 = this thread's block (Jit::SetTpidruro), 0 until the
// host call has bound it; that host call also answers keys out of range:
//   e3500b01  cmp   r0, #0x400          ; kTlsKeys
//   2a000004  bhs   slow
//   ee1d1f70  mrc   p15, 0, r1, c13, c0, 3
//   e3510000  cmp   r1, #0
//   0a000001  beq   slow
//   e7910100  ldr   r0, [r1, r0, lsl #2]
//   e12fff1e  bx    lr
//   ea......  slow: b <the import's svc trampoline>
static_assert(kTlsKeys == 0x400);
inline constexpr std::uint32_t kTlsGetThunk[7] = {
    0xe3500b01u, 0x2a000004u, 0xee1d1f70u, 0xe3510000u,
    0x0a000001u, 0xe7910100u, 0xe12fff1eu,
};
// The word after the thunk at `va`: b `tramp_va`.
inline std::uint32_t TlsGetThunkSlowWord(std::uint32_t va, std::uint32_t tramp_va) {
    const std::uint32_t at = va + sizeof(kTlsGetThunk);
    return 0xEA000000u | (((tramp_va - (at + 8)) >> 2) & 0xFFFFFFu);
}

struct Import {
    std::string name;
    bool is_function;  // false => data object (needs a guest address, not a call trampoline)
    std::uint32_t tramp_va = 0;  // guest VA of `svc` trampoline (functions) / data slot
    std::uint32_t call_va = 0;   // guest VA the guest actually branches to (== tramp_va
                                  // unless the import runs guest-side VFP code)
    std::uint8_t fast_math = 0;  // kFastMath index + 1 (0 = not a fast-math import)
    GlId gl_id = GlId::None;     // GlIdForName: the id RealGL::Handle switches on
    ImportId import_id = ImportId::None;  // ImportIdForName: CallImport's handler id
    bool blocking = false;       // BlockingImportName
    HotImport hot = HotImport::None;  // HotImportForName
};

struct PendingData {
    std::uint32_t loc_va = 0;  // relocation target to patch
    std::uint32_t import_idx = 0;
    std::uint32_t addend = 0;  // original *loc value
};

struct LoadSegment {
    std::uint32_t start = 0, size = 0;
    int prot = 0;
};

struct LoadedModule {
    std::string path;
    std::uint32_t bias = 0;  // load bias (0: mapped at linked addresses)
    std::uint32_t halt_va = 0;  // `svc #0` trampoline: LR target for calls into guest
    std::uint32_t once_va = 0;  // `svc #kSwiOnce`: LR target for a pthread_once init routine
    std::vector<LoadSegment> segments;  // mapped LOAD ranges (for access tracking)
    std::vector<std::uint32_t> init_array;
    std::vector<std::uint32_t> fini_array;
    std::vector<Import> imports;  // index = trampoline id (swi = id+1)
    std::vector<PendingData> pending_data;  // ABS32 data refs to patch later
    std::uint32_t jni_tramp_base = 0;  // 256 `svc` trampolines: swi = kSwiJniBase+i
    std::uint32_t jvm_tramp_base = 0;  // 8 `svc` trampolines: swi = kSwiJniBase+0x100+i
    std::uint32_t exidx_va = 0;        // .ARM.exidx guest VA (0 if absent)
    std::uint32_t exidx_count = 0;     // number of 8-byte entries
    // PT_GNU_RELRO as the real linker protects it (glibc page rounding). Holds
    // .got/.dynamic/init_array: read-only after relocation, never written at
    // runtime. The host refuses guest writes there (see Runtime::AddWriteGuard).
    std::uint32_t relro_start = 0;
    std::uint32_t relro_size = 0;
    std::vector<std::uint8_t> build_id;  // GNU build-id (NOTE desc; empty if absent)
    // DEFINED sym name -> guest address (st_value + bias), over every loaded
    // module; the first definition wins, as in the linker's search order.
    std::unordered_map<std::string, std::uint32_t> defined;
    // Each library's own definitions (index 0: the first library, i: more[i-1]),
    // for dlsym on a library handle.
    std::vector<std::unordered_map<std::string, std::uint32_t>> lib_defined;
    // Further guest libraries loaded into this module (Load with append):
    // the fields above describe the first one.
    std::uint32_t span_lo = 0, span_hi = 0;  // first library's mapped address range
    struct Extra {
        std::string path;
        std::uint32_t bias = 0;
        std::uint32_t span_lo = 0, span_hi = 0;
        std::uint32_t exidx_va = 0, exidx_count = 0;
        std::uint32_t relro_start = 0, relro_size = 0;
    };
    std::vector<Extra> more;
    // Per library (index 0: the first, i: more[i-1]): its own entries of
    // init_array [init_begin, init_end) and its DT_NEEDED names, so a host
    // can run a library's initialisers when the guest dlopens it, after the
    // ones of the guest libraries it needs (as the linker does).
    struct LibInfo {
        std::uint32_t init_begin = 0, init_end = 0;
        std::vector<std::string> needed;
    };
    std::vector<LibInfo> libs;
    // Trampoline page cursors, kept for later loads.
    std::uint32_t next_tramp = 0;
    std::uint32_t next_vfp_thunk = 0;
};
// The read-only LOAD of the module that holds the word at va, or nullptr.
const LoadSegment* ReadOnlySegmentAt(const LoadedModule& mod, std::uint32_t va);
// Store one data-import relocation. A slot in a read-only LOAD (a text
// relocation, DT_TEXTREL: Rio 1.0.0's 2011 library has them) is written
// through a short RW window over its pages, the way Load applies RELATIVE
// relocations there; any other slot must be writable -- the caller checks.
void WriteDataSlot(GuestMemory& mem, const LoadedModule& mod, std::uint32_t loc_va,
                   std::uint32_t value);
// Bind a data import to a guest address (see the definition in
// elf_loader.cpp). Free function so the runtime can use it too.
bool ResolveDataImport(GuestMemory& mem, LoadedModule& mod, std::uint32_t import_idx,
                       std::uint32_t guest_addr);


class ElfLoader {
public:
    explicit ElfLoader(GuestMemory& mem) : mem_(mem) {}

    // Loads file into guest memory at link address + bias. Returns false +
    // error string on failure. When `out` already holds a library, this one
    // is added to it: its undefined symbols bind first to the libraries
    // loaded before (load dependencies first, like the linker's search order),
    // then to host imports, which share one trampoline table.
    bool Load(const char* path, LoadedModule& out, std::string& err, std::uint32_t bias = 0);

    // Patches all pending ABS32 refs of a data import to addend+guest_addr.

private:
    GuestMemory& mem_;
};

}  // namespace abr
