#include "dispatch.h"

#include <dynarmic/frontend/A32/a32_ir_emitter.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include <cstring>

namespace abr {

// TEMP-DIAG (registry ParseError): registry FILE* set + dump counter.
static std::set<FILE*> g_regfiles;
static int g_regdump_n = 0;

Runtime::Runtime(GuestMemory& mem, LoadedModule& mod) : mem_(mem), mod_(mod) {
    // HARNESS_HEAPBASE=hex: relocate heap start (address-sensitivity probe).
    if (const char* e = std::getenv("HARNESS_HEAPBASE")) {
        heap_brk_ = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }
    // Heap + stacks + trampolines + tables + guest data are all committable.
    mem_.Map(heap_brk_, kHeapSize, PROT_READ | PROT_WRITE);
    AddMapping(heap_brk_, kHeapSize, PROT_READ | PROT_WRITE);
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
}

void Runtime::AddMapping(std::uint32_t start, std::uint32_t size, int prot) {
    mappings_.push_back({start, start + size, prot});
}

bool Runtime::CheckAccess(std::uint32_t addr, std::size_t size, bool write, bool exec) {
    const std::uint64_t end = std::uint64_t(addr) + size;
    for (const auto& m : mappings_) {
        if (addr >= m.start && end <= m.end) {
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

std::uint32_t Runtime::GuestMalloc(std::uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size = (size + 7) & ~std::uint32_t(7);
    if (size == 0) {
        size = 8;
    }
    if (heap_brk_ + size > kHeapBase + kHeapSize) {
        return 0;
    }
    const std::uint32_t out = heap_brk_;
    heap_brk_ += size;
    heap_sizes_[out] = size;
    return out;
}

std::uint32_t Runtime::GuestCalloc(std::uint32_t n, std::uint32_t size) {
    const std::uint64_t total = std::uint64_t(n) * size;
    if (total > 0x10000000u) {
        return 0;
    }
    const std::uint32_t out = GuestMalloc(static_cast<std::uint32_t>(total));
    if (out) {
        std::memset(H(out), 0, static_cast<std::size_t>(total));
    }
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
    const std::uint32_t out = GuestMalloc(size);
    if (out && addr) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = heap_sizes_.find(addr);
        std::uint32_t old = (it == heap_sizes_.end()) ? size : it->second;
        std::memcpy(H(out), H<const void>(addr), old < size ? old : size);
    }
    return out;
}

void Runtime::GuestFree(std::uint32_t /*addr*/) {
    // Bump allocator: intentionally a no-op for bring-up.
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
    for (const auto& m : mappings_) {
        if (guest >= m.start && end <= m.end) {
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
    svc_count++;
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
    {
        // Locked: worker threads share stub_hits_/call_total with main.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::uint64_t& h = stub_hits_["#trace:" + mod_.imports[idx].name];
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
        static bool threadlog = std::getenv("HARNESS_THREADLOG") != nullptr;
        const bool lineal = (GuestTid() == 1) || threadlog;
        if ((++h <= 3 || call_total < callmax) && lineal) {
            std::printf("[call]%s %s(%08x %08x %08x %08x) lr=%08x\n",
                        GuestTid() == 1 ? "" : "(T2+)", mod_.imports[idx].name.c_str(), regs[0],
                        regs[1], regs[2], regs[3], regs[14]);
        }
        call_total++;
    }
    CallImport(idx, regs);
}

void Runtime::CallImport(std::uint32_t idx, Regs& regs) {
    const Import& imp = mod_.imports[idx];
    const std::string& n = imp.name;
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];

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
        std::memmove(H(a0), H<const void>(a1), a2);
        ScanWP(this, a0, a2, n.c_str());
        Ret(regs, a0);
        return;
    }
    if (n == "memmove" || n == "__aeabi_memmove" || n == "__aeabi_memmove4" ||
        n == "__aeabi_memmove8") {
        if (!CheckRange(a0, a2, true) || !CheckRange(a1, a2, false)) {
            goto bad_ptr;
        }
        std::memmove(H(a0), H<const void>(a1), a2);
        ScanWP(this, a0, a2, n.c_str());
        Ret(regs, a0);
        return;
    }
    if (n == "memset" || n == "__aeabi_memset" || n == "__aeabi_memset4" || n == "__aeabi_memset8" ||
        n == "__aeabi_memclr" || n == "__aeabi_memclr4" || n == "__aeabi_memclr8") {
        const bool is_clr = n.size() >= 14 && n.compare(0, 14, "__aeabi_memclr") == 0;
        const std::uint32_t len = is_clr ? a1 : a2;
        if (!CheckRange(a0, len, true)) {
            goto bad_ptr;
        }
        if (is_clr) {
            std::memset(H(a0), 0, a1);
        } else {
            std::memset(H(a0), a1 & 0xFF, a2);
        }
        Ret(regs, a0);
        return;
    }
    if (n == "memcmp" || n == "bcmp") {
        if (!CheckRange(a0, a2, false) || !CheckRange(a1, a2, false)) {
            goto bad_ptr;
        }
        // TEMP-DIAG (font wall): log len-3 compares to catch the PVR one.
        if (a2 == 3) {
            const std::uint8_t* p0 = H<const std::uint8_t>(a0);
            const std::uint8_t* p1 = H<const std::uint8_t>(a1);
            std::printf("[memcmp3] %08x: %02x %02x %02x vs %08x: %02x %02x %02x -> %d\n",
                        a0, p0[0], p0[1], p0[2], a1, p1[0], p1[1], p1[2],
                        std::memcmp(p0, p1, 3));
        }
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
            if (n == "strcpy") {
                std::strcpy(reinterpret_cast<char*>(H(a0)), GuestStr(a1));
            } else {
                std::strcat(reinterpret_cast<char*>(H(a0)), GuestStr(a1));
            }
        } else {
            if (!CheckRange(a0, a2 + (n == "strncpy" ? 0 : 1), true) ||
                !CheckRange(a1, a2, false)) {
                goto bad_ptr;
            }
            if (n == "strncpy") {
                std::strncpy(reinterpret_cast<char*>(H(a0)), GuestStr(a1), a2);
            } else {
                std::strncat(reinterpret_cast<char*>(H(a0)), GuestStr(a1), a2);
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
        static std::uint32_t strtok_state = 0;  // legacy strtok (single-threaded)
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
    if (n == "clock_gettime") {
        struct timespec ts {};
        const int r = clock_gettime(a0, &ts);
        if (r == 0) {
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
        Ret(regs, static_cast<std::uint32_t>(t & 0xFFFFFFFFu),
            static_cast<std::uint32_t>((std::uint64_t(t) >> 32) & 0xFFFFFFFFu));
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
    if (n == "pthread_cond_init" || n == "pthread_cond_destroy" || n == "pthread_cond_signal" ||
        n == "pthread_cond_broadcast" || n == "pthread_cond_wait" ||
        n == "pthread_cond_timedwait" || n == "pthread_cond_timedwait_monotonic") {
        std::string base = (n == "pthread_cond_timedwait_monotonic") ? "pthread_cond_timedwait" : n;
        Ret(regs, PthreadCond(base, a0, a1, a2));
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
        if (std::getenv("HARNESS_NOWORKER")) {
            std::printf("[thread] NOWORKER: skip create start=%08x\n", a2);
            if (a0 && CheckRange(a0, 4, true)) {
                std::uint32_t zero = 0;
                std::memcpy(H(a0), &zero, 4);
            }
            Ret(regs, 0);
            return;
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
        {
            static std::uint64_t poll_hits = 0;
            if (++poll_hits <= 5) {
                std::string ds;
                for (std::size_t i = 0; i < pfds.size() && i < 4; i++) {
                    char b[32];
                    std::snprintf(b, sizeof(b), "%d ", pfds[i].fd);
                    ds += b;
                }
                std::printf("[poll] nfds=%u to=%d fds=[%s]%s\n", a1, (int)a2, ds.c_str(),
                            poll_hits == 5 ? " (quiet henceforth)" : "");
            }
        }
        int r = ::poll(pfds.data(), (nfds_t)pfds.size(), to);
        {
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
            stop_step_ = true;
            ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        Ret(regs, FindExidx(a0));
        return;
    }
    // --- file bridge (bring-up) ---
    // Guest paths under files_prefix_ map to files_hostbase_ (harness sets it
    // to <project>/work/files). /dev/null|zero|urandom pass to host same.
    // Anything else fails LOUD (discovery: see what the engine wants).
    // NOTE: guest struct stat uses the NDK-r27 ARM layout (88B+... see below);
    // 2014-bionic compat verified only by game behavior (st_size/st_mode reads).
    // Whole-block lock: file/dir maps are shared with worker threads
    // (recursive_mutex, so nested GuestMalloc etc. stay safe).
    {
        std::lock_guard<std::recursive_mutex> file_lock(mutex_);
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
            if (g.compare(0, 5, "/sys/") == 0 || g.compare(0, 6, "/proc/") == 0) {
                return g;
            }
            if (!files_prefix_.empty() && g.compare(0, files_prefix_.size(), files_prefix_) == 0 &&
                !files_hostbase_.empty()) {
                return files_hostbase_ + g.substr(files_prefix_.size());
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
            const auto it = file_map_.find(h);
            return it == file_map_.end() ? nullptr : it->second.f;
        };
        auto new_file_handle = [&]() -> std::uint32_t {
            const std::uint32_t h = GuestMalloc(96);  // opaque, Bionic-compatible size
            if (!h) {
                return 0;
            }
            std::memset(H(h), 0, 96);
            file_map_[h] = HostFile{nullptr};
            return h;
        };
        // Bind a host FILE*: allocate a guest fd, expose it at Bionic __sFile
        // offset +14 (_file, ldrsh-loaded inline by guest code for fsync etc.)
        // and in fd_map_ for fd-based thunks. Returns false on exhaustion.
        auto bind_file = [&](std::uint32_t h, FILE* f) -> bool {
            const std::uint32_t g = next_guest_fd_++;
            if (g >= 0x8000) {
                return false;
            }
            fd_map_[g] = fileno(f);
            file_map_[h].f = f;
            const std::uint16_t g16 = static_cast<std::uint16_t>(g);
            std::memcpy(H(h + 14), &g16, 2);
            return true;
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
            std::printf("[files] fopen(\"%s\",\"%s\") -> %08x\n", hp.c_str(), mode.c_str(), h);
            // TEMP-DIAG (registry ParseError): track registry handles to dump
            // their exact bytes on fread/fwrite below.
            if (hp.find("registry") != std::string::npos) {
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
            std::size_t got = std::fread(H(a0), a1, a2, f);
            // TEMP-DIAG (registry ParseError): dump exact bytes read.
            if (g_regfiles.count(f) && g_regdump_n < 60) {
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
            if (g_regfiles.count(f) && g_regdump_n < 60) {
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
                char* r = std::fgets(reinterpret_cast<char*>(H(a0)), (int)a1, f);
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
            const auto it = fd_map_.find(a0);
            if (it == fd_map_.end()) {
                Ret(regs, 0);
                return;
            }
            FILE* f = ::fdopen(it->second, GuestStr(a1));
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
            const std::uint32_t g = next_guest_fd_++;
            fd_map_[g] = fd;
            std::printf("[files] open(\"%s\",%o) -> fd %u\n", hp.c_str(), a1, g);
            Ret(regs, g);
            return;
        }
        if (n == "close" || n == "fsync") {
            const auto it = fd_map_.find(a0);
            if (it == fd_map_.end()) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            int r = (n == "close") ? ::close(it->second) : ::fsync(it->second);
            if (n == "close") {
                fd_map_.erase(it);
            }
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
            const std::uint32_t g0 = next_guest_fd_++;
            const std::uint32_t g1 = next_guest_fd_++;
            fd_map_[g0] = hfds[0];
            fd_map_[g1] = hfds[1];
            std::memcpy(H(a0), &g0, 4);
            std::memcpy(H(a0 + 4), &g1, 4);
            std::printf("[files] pipe() -> [%u %u]\n", g0, g1);
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
            const auto it = fd_map_.find(a0);
            if (it == fd_map_.end()) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            if (!CheckRange(a1, a2, n == "read")) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            ssize_t r = (n == "read") ? ::read(it->second, H(a1), a2) : ::write(it->second, H(a1), a2);
            Ret(regs, static_cast<std::uint32_t>(r < 0 ? -1 : r));
            return;
        }
        if (n == "lseek") {
            const auto it = fd_map_.find(a0);
            if (it == fd_map_.end()) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            off_t r = ::lseek(it->second, (off_t)a1, (int)a2);
            Ret(regs, static_cast<std::uint32_t>(r));
            return;
        }
        if (n == "writev") {
            const auto it = fd_map_.find(a0);
            if (it == fd_map_.end() || !CheckRange(a1, (std::size_t)a2 * 8, false)) {
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
                ssize_t w = ::write(it->second, H(base), len);
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
            const auto it = fd_map_.find(a0);
            if (it == fd_map_.end()) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            struct stat st {};
            if (::fstat(it->second, &st) != 0 || !fill_stat(a1, st)) {
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
            dir_map_[h] = HostDir{d};
            Ret(regs, h);
            return;
        }
        if (n == "readdir_r") {
            // args: DIR*(a0), dirent*(a1), dirent**(a2). Guest dirent (bionic):
            // u64 ino@0, i64 off@8, u16 reclen@16, u8 type@17, name[256]@18.
            const auto it = dir_map_.find(a0);
            if (it == dir_map_.end() || !CheckRange(a1, 280, true) || !CheckRange(a2, 4, true)) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            errno = 0;
            struct dirent* de = ::readdir(it->second.d);
            if (!de) {
                std::uint32_t z = 0;
                std::memcpy(H(a2), &z, 4);
                Ret(regs, errno ? errno : 0);
                return;
            }
            std::uint8_t* p = H<std::uint8_t>(a1);
            std::memset(p, 0, 280);
            std::uint64_t ino = de->d_ino;
            std::memcpy(p, &ino, 8);
            std::int64_t off = de->d_off;
            std::memcpy(p + 8, &off, 8);
            std::uint16_t rl = de->d_reclen;
            std::memcpy(p + 16, &rl, 2);
            p[17] = de->d_type;
            std::strncpy(reinterpret_cast<char*>(p + 18), de->d_name, 255);
            std::memcpy(H(a2), &a1, 4);
            Ret(regs, 0);
            return;
        }
        if (n == "closedir") {
            const auto it = dir_map_.find(a0);
            if (it == dir_map_.end()) {
                Ret(regs, static_cast<std::uint32_t>(-1));
                return;
            }
            int r = ::closedir(it->second.d);
            dir_map_.erase(it);
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
            std::printf("[asset] open(\"%s\") -> %08x (%llu bytes, %s)\n", want.c_str(), gbuf,
                        (unsigned long long)found->usize,
                        found->method == 0 ? "stored" : "deflated");
            Ret(regs, gbuf);  // handle IS the guest buffer (getBuffer model)
            return;
        }
        if (n == "AAsset_close") {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (asset_map_.erase(a0) == 0) {
                Ret(regs, 0);
                return;
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
            std::printf("[gl] %s accepted (fake)\n", n.c_str());
            Ret(regs, 0);
            return;
        }
        if (n == "glGetError") {
            Ret(regs, 0);  // GL_NO_ERROR
            return;
        }
    }
    {
        // Locked: worker threads share stub_hits_ with main.
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
    std::printf("[svc] mappings:\n");
    for (const auto& m : mappings_) {
        std::printf("    [%08x-%08x] prot=%d\n", m.start, m.end, m.prot);
    }
    stop_step_ = true;
    ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

std::uint32_t Runtime::PthreadMutex(const std::string& n, std::uint32_t obj) {
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
            pthread_mutex_destroy(it->second);
            delete it->second;
            mutex_map_.erase(it);
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
    if (n == "pthread_mutex_lock") {
        return static_cast<std::uint32_t>(pthread_mutex_lock(it->second));
    }
    if (n == "pthread_mutex_unlock") {
        return static_cast<std::uint32_t>(pthread_mutex_unlock(it->second));
    }
    if (n == "pthread_mutex_trylock") {
        return static_cast<std::uint32_t>(pthread_mutex_trylock(it->second));
    }
    return 22;
}

std::uint32_t Runtime::PthreadCond(const std::string& n, std::uint32_t cond, std::uint32_t arg1,
                                   std::uint32_t arg2) {
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
        mutex_.unlock();
        const int r = pthread_cond_wait(it->second, mit->second);
        mutex_.lock();
        return static_cast<std::uint32_t>(r);
    }
    if (n == "pthread_cond_timedwait") {
        struct timespec ts {};
        std::memcpy(&ts, H(arg2), sizeof(ts));
        // Cap absurd waits so missing signals surface instead of hanging forever.
        struct timespec now {};
        clock_gettime(CLOCK_REALTIME, &now);
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
    const std::uint32_t s = kGuestDataBase + 0x104 + next_errno_slot_ * 4;
    next_errno_slot_++;
    if (std::uint64_t(s) + 4 > std::uint64_t(kGuestDataBase) + 0x2000) {
        return kGuestDataBase + 0x100;  // arena full: alias main (bring-up)
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
    // 2MB guest stack from the heap (bump; leak accepted in bring-up).
    const std::uint32_t stack = GuestMalloc(0x200000u);
    if (!stack) {
        return 12;  // ENOMEM
    }
    auto st = std::make_shared<ThreadState>();
    std::uint32_t tid = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        tid = next_tid_++;
        threads_[tid] = st;
    }
    if (!CheckRange(tid_out, 4, true)) {
        std::printf("[thread] LOUD bad tid_out %08x -- halting\n", tid_out);
        ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return 0xFFFFFFFFu;
    }
    std::memcpy(H(tid_out), &tid, 4);
    std::printf("[thread] create tid=%u start=%08x arg=%08x stack=%08x\n", tid, start, arg, stack);
    st->th = std::thread([this, st, tid, start, arg, stack]() {
        t_tid = tid;
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = cb_;
        cfg.optimizations = Dynarmic::no_optimizations;
        static Dynarmic::ExclusiveMonitor shared_mon(8);
        cfg.global_monitor = &shared_mon;
        static std::atomic<unsigned> next_cpu{1};
        cfg.processor_id = next_cpu.fetch_add(1) % 8;
        if (cfg.processor_id == 0) {
            cfg.processor_id = 1;
        }
        Dynarmic::A32::Jit jit(cfg);
        t_active_jit = &jit;
        auto& regs = jit.Regs();
        regs.fill(0);
        regs[13] = stack + 0x200000u - 8;  // 8-aligned top
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
        (void)hr;
        {
            std::lock_guard<std::mutex> lock(st->m);
            st->retval = regs[0];
            st->done = true;
        }
        std::printf("[thread] tid=%u done retval=%08x\n", tid, regs[0]);
        st->cv.notify_all();
        t_active_jit = nullptr;
    });
    st->th.detach();
    return 0;
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
        std::unique_lock<std::mutex> lock(st->m);
        st->cv.wait(lock, [&] { return st->done; });
    }
    if (retval_out) {
        if (!CheckRange(retval_out, 4, true)) {
            return 14;  // EFAULT
        }
        std::memcpy(H(retval_out), &st->retval, 4);
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
        threads_.erase(it);
    }
    return 0;
}

// ---- JNI bridge (bring-up) ----

std::uint32_t Runtime::JniEnv() {
    return jni_env_;
}

std::uint32_t Runtime::Jvm() {
    return jvm_;
}

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
    {
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
    case 221:  // GetStringRegion(env, str, start, len, buf) -> void (jchar units)
    case 222:  // GetStringUTFRegion(env, str, start, len, buf) -> void (bytes)
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
        if (fn == 221) {
            if (!CheckRange(buf, a3 * 2, true)) {
                std::printf("[jni] region BAD buf %08x -- halting\n", buf);
                stop_step_ = true;
                ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
                return;
            }
            for (std::uint32_t i = 0; i < a3; i++) {
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
            std::memcpy(H(buf), s.data() + a2, a3);
        }
        Ret(regs, 0);
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
    case 23:   // DeleteLocalRef(env, obj) -> void: mock handles need no GC
    case 22:   // DeleteGlobalRef(env, obj) -> void (same)
    case 228:  // DeleteWeakGlobalRef(env, ref) -> void (same)
        // PROVEN load-bearing: caller @0x267FD0 treats r0!=0 as error and
        // throws "Java method threw an exception". Zero it like Dalvik does.
        Ret(regs, 0);
        return;
    case 25:   // NewLocalRef(env, ref) -> same handle (no GC in mock world)
    case 21:   // NewGlobalRef(env, obj) -> same handle
    case 227:  // NewWeakGlobalRef(env, obj) -> same handle
        Ret(regs, a1);
        return;
    case 17:  // ExceptionClear(env) -> void (mock never raises)
        Ret(regs, 0);
        return;
    case 15:  // ExceptionOccurred(env) -> NULL (mock never raises)
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
        const char* vtag = (fn == 28) ? "" : ((fn == 29) ? "V" : "A");
        std::printf("[jni] NewObject%s %s -> %08x\n", vtag, mdesc.c_str(), o);
        Ret(regs, o);
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
        std::printf("[jni] CallVoidMethod%s %s\n", vtag, mdesc.c_str());
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
            const std::uint32_t js = JniNewString(content);
            std::printf("[jni] Call%sObject%s %s -> str %08x \"%s\"\n", is_static ? "Static" : "",
                        vtag, mdesc.c_str(), js, content.c_str());
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
        std::printf("[jni] Call%sObject%s %s -> %08x\n", is_static ? "Static" : "", vtag,
                    mdesc.c_str(), o);
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
            std::printf("[jni] GetStaticObjectField %s -> str %08x\n", fdesc.c_str(), js);
            Ret(regs, js);
            return;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        jni_obj_next_ = (jni_obj_next_ + 3) & ~std::uint32_t(3);
        const std::uint32_t o = jni_obj_next_;
        jni_obj_next_ += 4;
        std::memcpy(H(o), &o, 4);
        jni_objects_[o] = "field<" + fdesc + ">";
        std::printf("[jni] GetStaticObjectField %s -> %08x\n", fdesc.c_str(), o);
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
        std::printf("[jni] GetStaticField#%u %s -> 0\n", fn, fdesc.c_str());
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
        std::printf("[jni] CallStaticBooleanMethod%s %s -> 0\n", vtag, mdesc.c_str());
        Ret(regs, 0);
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
        }
        const char* vtag = (fn == 129) ? "" : ((fn == 130) ? "V" : "A");
        std::printf("[jni] CallStaticIntMethod%s %s -> %u\n", vtag, mdesc.c_str(), v);
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
        std::printf("[jni] CallLongMethod%s %s -> 0\n", vtag, mdesc.c_str());
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
        std::printf("[jni] CallStatic%s %s -> 0\n",
                    fn == 140 ? "DoubleMethodA" : "VoidMethod", mdesc.c_str());
        Ret(regs, 0, 0);
        return;
    }
    case 184: {  // GetByteArrayElements(env, array, isCopy) -> jbyte*
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
        Ret(regs, 0);  // direct pointer: nothing to copy back or free
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
        std::printf("[jni] FindClass \"%s\" -> %08x\n", name.c_str(), h);
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
        std::printf("[jni] Get%s%sID %s -> %08x\n", (fn == 33 || fn == 94) ? "" : "Static",
                    (fn == 33 || fn == 113) ? "Method" : "Field", jni_methods_[c].c_str(), c);
        Ret(regs, c);
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
            std::printf("[mem] BAD %s @ %08x size %zu guest-pc=%08x -- halting\n", exec ? "exec" : "read",
                        addr, sizeof(T), rt_->ActiveJit()->Regs()[15]);
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
    }
    return v;
}

template <typename T>
void GuestCallbacks::Write(VAddr addr, T v) {
    if (!rt_->CheckAccess(addr, sizeof(T), true, false)) {
        if (rt_->NoteBad()) {
            std::printf("[mem] BAD write @ %08x size %zu guest-pc=%08x -- halting\n", addr,
                        sizeof(T), rt_->ActiveJit()->Regs()[15]);
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
    // [r1] raw 32B (forwarder inputs are objects; the range/longstr
    // interpretation above misfires on them).
    if (cb->rt_->CheckRange((std::uint32_t)r1, 32, false)) {
        const std::uint8_t* p1 = cb->rt_->H<const std::uint8_t>((std::uint32_t)r1);
        std::printf("[probe] [r1]-raw:");
        for (int j = 0; j < 32; j++) {
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
                std::printf("[probe] throw type: %s\n", buf);
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
    // HARNESS_PROBE=<hexpc>[,<hexlen>]: emit an execution-time host call at
    // pc that dumps r0/r1 + min(r1,len,256) bytes at [r0]. Used to capture
    // JIT-time (ptr,len) args (e.g. JSON parser input) that heap-drift and
    // thread racing make invisible post-mortem. Translation-time hook, but
    // the emitted CallHostFunction runs on every execution with live regs.
    {
        static std::int64_t probe = -2;
        static std::uint32_t probe_pc = 0, probe_len = 0, probe_pc2 = 0, probe_pc3 = 0;
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
        }
        // Standalone IR fixes (independent of probe slots).
        if (std::getenv("HARNESS_FIXPVR") && pc == 0x2B0C50u) {
            using Reg = Dynarmic::A32::Reg;
            ir.SetRegister(static_cast<Reg>(0), ir.Imm32(11));
        }
        if (probe > 0 && (pc == probe_pc || pc == probe_pc2 || pc == probe_pc3)) {
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
        }
    }
    if (tl > 0) {
        std::printf("[tr] %c %08x\n", is_thumb ? 'T' : 'A', pc);
    }
}

std::optional<std::uint32_t> GuestCallbacks::MemoryReadCode(VAddr a) {    if (!rt_->CheckAccess(a & ~std::uint32_t(3), 4, false, true)) {
        if (rt_->NoteBad()) {
            std::printf("[mem] BAD exec @ %08x -- halting\n", a);
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
    rt_->stop_step_ = true;
    rt_->ActiveJit()->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

}  // namespace abr
