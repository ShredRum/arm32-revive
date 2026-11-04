// Unit test for the guest printf formatter (Runtime::ExpandPrintf).
//
// The guests are GCC/arm-linux-gnueabi builds on a 32-bit ARM target. The
// variadic ABI is AAPCS32:
//
//   size_t / unsigned int / int / long / ptrdiff_t  = 4 bytes  -> ONE slot
//   long long / intmax_t                             = 8 bytes  -> an EVEN
//                                                       core-register pair,
//                                                       else an 8-byte
//                                                       aligned stack slot
//   double                                           = 8 bytes  -> an EVEN
//                                                       core-register pair,
//                                                       else an 8-byte
//                                                       aligned stack slot
//   va_list                                          = ONE WORD (a pointer
//                                                       to a flat area); the
//                                                       consumer reads AT the
//                                                       pointer and advances
//                                                       by 4 (int) or 8
//                                                       (double-word); the
//                                                       CALLER aligns.
//
// That is the VARIADIC export path (ExpandPrintf). The JNI native ENTRY path
// uses the same even-pair rule, pinned by the marshalling test near the end
// and by the guest's own Utils.handleReferrerValue entry.
//
// Proven from the guest code, not from a compiler guess: the WebP Xing
// parser in Rio/Space/Star Wars calls
//   fprintf(fp, "Note: Xing stream size %lu differs by %f%% ...", lu, d);
// with the %lu in r2 and the double wholly at [sp] (`vstr d8,[sp]`), r3
// unused. Base PCS would put the low word in r3. See the ABI tests below.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <zlib.h>

#include "jni_call_words.h"
#include <set>
#include <thread>
#include <atomic>
#include <sys/mman.h>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "guest/dispatch.h"
#include "guest/bsd_random.h"
#include "guest/elf_loader.h"
#include "guest/guest_memory.h"

// Scratch directory for file tests: $TMPDIR (Termux has no /tmp), else /tmp.
static std::string TestTmp() {
    const char* t = std::getenv("TMPDIR");
    return (t && *t) ? std::string(t) : std::string("/tmp");
}

namespace abr {
// Test-only bridge to the private import dispatcher (see dispatch.h).
struct RuntimeTestAccess {
    using RegsView = std::array<std::uint32_t, 16>;
    // Call the dispatcher with the given argument registers; returns r0.
    static void SetSignalsInUse(Runtime& rt, bool on) { rt.signals_in_use_ = on; }
    static std::uint32_t Call(Runtime& rt, std::uint32_t idx, const std::uint32_t args[4]) {
        Runtime::Regs regs{};
        regs[0] = args[0];
        regs[1] = args[1];
        regs[2] = args[2];
        regs[3] = args[3];
        rt.CallImport(idx, regs);
        return regs[0];
    }
    // The raw EABI syscall path (svc #0, the number in r7): what the guest's
    // own libc-free code (the bundled breakpad) uses. Reports r0 and whether
    // the guest thread was stopped.
    static std::uint32_t GuestSyscall(Runtime& rt, std::uint32_t nr, std::uint32_t r0,
                                      std::uint32_t r1, std::uint32_t r2, bool* stopped) {
        Runtime::Regs regs{};
        regs[7] = nr;
        regs[0] = r0;
        regs[1] = r1;
        regs[2] = r2;
        rt.OnGuestSyscall(regs);
        if (stopped) {
            *stopped = rt.stop_step_;
        }
        return regs[0];
    }
    // Same, with the caller's register file in and the result out.
    static std::uint32_t Call(Runtime& rt, std::uint32_t idx, const std::uint32_t args[4],
                              const RegsView& in, RegsView* out) {
        Runtime::Regs regs = in;
        regs[0] = args[0];
        regs[1] = args[1];
        regs[2] = args[2];
        regs[3] = args[3];
        rt.CallImport(idx, regs);
        if (out) {
            *out = regs;
        }
        return regs[0];
    }
    // File-bridge state, test-only (single-threaded fmt_test).
    static std::size_t FdMapSize(Runtime& rt) { return rt.fd_map_.size(); }
    static std::size_t FileMapSize(Runtime& rt) { return rt.file_map_.size(); }
    static std::size_t FreeGuestFdCount(Runtime& rt) { return rt.free_guest_fds_.size(); }
    static std::uint32_t NextGuestFd(Runtime& rt) { return rt.next_guest_fd_; }
    static std::uint32_t GuestLiveSize(Runtime& rt, std::uint32_t h) { return rt.LiveIdxFind(h); }
    static std::uint32_t Jni(Runtime& rt, std::uint32_t fn, const std::uint32_t args[4]) {
        Runtime::Regs regs{};
        regs[0] = args[0];
        regs[1] = args[1];
        regs[2] = args[2];
        regs[3] = args[3];
        rt.CallJni(fn, regs);
        return regs[0];
    }
    static void JniRegion(Runtime& rt, std::uint32_t fn, std::uint32_t str, std::uint32_t start,
                          std::uint32_t len, std::uint32_t sp) {
        Runtime::Regs regs{};
        regs[1] = str;
        regs[2] = start;
        regs[3] = len;
        regs[13] = sp;
        rt.CallJni(fn, regs);
    }
    static std::size_t JniStringCount(Runtime& rt) { return rt.jni_strings_.size(); }
    static std::size_t UtfCharCount(Runtime& rt) { return rt.jni_utf_chars_.size(); }
    static void TrackUtfChars(Runtime& rt, std::uint32_t g) { rt.jni_utf_chars_.insert(g); }
    static std::size_t AssetMapSize(Runtime& rt) { return rt.asset_map_.size(); }
    static std::size_t LiveCount(Runtime& rt) { return rt.live_idx_.Size(); }
    static void SeedHostThreadTls(Runtime& rt, std::uint32_t tid, std::uint32_t slot,
                                  std::uint32_t key, std::uint32_t value) {
        rt.errno_slots_[tid] = slot;
        const std::uint32_t saved = rt.GuestTid();
        rt.ThreadEnter(nullptr, tid);
        rt.PthreadSetSpecific(key, value);
        rt.ThreadEnter(nullptr, saved);
    }
    static std::uint32_t ImportIndex(Runtime& rt, const std::string& name) {
        for (std::uint32_t i = 0; i < rt.mod_.imports.size(); i++) {
            if (rt.mod_.imports[i].name == name) {
                return i;
            }
        }
        return 0xFFFFFFFFu;
    }
    static void SetTls(Runtime& rt, std::uint32_t key, std::uint32_t value) {
        rt.PthreadSetSpecific(key, value);
    }
    static std::uint32_t TlsBlock(Runtime& rt, std::uint32_t tid) { return rt.TlsFor(tid); }
    static bool HostThreadHasErrno(Runtime& rt, std::uint32_t tid) {
        return rt.errno_slots_.find(tid) != rt.errno_slots_.end();
    }
    static bool HostThreadHasTls(Runtime& rt, std::uint32_t tid, std::uint32_t key) {
        const auto it = rt.tls_blocks_.find(tid);
        return it != rt.tls_blocks_.end() && *rt.H<std::uint32_t>(it->second + key * 4) != 0;
    }
    static std::size_t FreeErrnoCount(Runtime& rt) { return rt.errno_free_.size(); }
    static bool InMapping(Runtime& rt, std::uint32_t a, std::size_t n) { return rt.CheckRange(a, n, false); }
    static void MapRwx(Runtime& rt, std::uint32_t a, std::uint32_t n) {
        rt.mem().Map(a, n, PROT_READ | PROT_WRITE | PROT_EXEC);
        rt.AddMapping(a, n, PROT_READ | PROT_WRITE | PROT_EXEC);
    }
    static bool TakeStopped(Runtime& rt) {
        const bool s = rt.stop_step_;
        rt.stop_step_ = false;
        return s;
    }
};
}  // namespace abr

namespace {

int g_fail = 0;

void Expect(const std::string& what, const std::string& got, const std::string& want) {
    if (got == want) {
        std::printf("ok   %-28s -> \"%s\"\n", what.c_str(), got.c_str());
        return;
    }
    std::printf("FAIL %-28s -> \"%s\" (want \"%s\")\n", what.c_str(), got.c_str(), want.c_str());
    g_fail++;
}

void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xff));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}

void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; i++) {
        v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xff));
    }
}

// A one-entry ZIP whose central directory says "deflated, 64 bytes", but the
// payload is not a deflate stream. AAssetManager_open must fail, free the
// already-allocated guest buffer, and leave no live block behind.
std::vector<std::uint8_t> MakeBadAssetZip() {
    const std::string name = "assets/bad.bin";
    const std::vector<std::uint8_t> data = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03};
    std::vector<std::uint8_t> v;
    Put32(v, 0x04034b50u);  // local file header
    Put16(v, 20);
    Put16(v, 0);
    Put16(v, 8);  // method 8 = deflate
    Put16(v, 0);
    Put16(v, 0);
    Put32(v, 0);
    Put32(v, static_cast<std::uint32_t>(data.size()));
    Put32(v, 64);  // declared uncompressed size
    Put16(v, static_cast<std::uint16_t>(name.size()));
    Put16(v, 0);
    v.insert(v.end(), name.begin(), name.end());
    v.insert(v.end(), data.begin(), data.end());
    const std::uint32_t cd_off = static_cast<std::uint32_t>(v.size());
    Put32(v, 0x02014b50u);  // central directory
    Put16(v, 20);
    Put16(v, 20);
    Put16(v, 0);
    Put16(v, 8);
    Put16(v, 0);
    Put16(v, 0);
    Put32(v, 0);
    Put32(v, static_cast<std::uint32_t>(data.size()));
    Put32(v, 64);
    Put16(v, static_cast<std::uint16_t>(name.size()));
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 0);
    Put32(v, 0);
    Put32(v, 0);  // local header offset
    v.insert(v.end(), name.begin(), name.end());
    const std::uint32_t cd_size = static_cast<std::uint32_t>(v.size()) - cd_off;
    Put32(v, 0x06054b50u);  // EOCD
    Put16(v, 0);
    Put16(v, 0);
    Put16(v, 1);
    Put16(v, 1);
    Put32(v, cd_size);
    Put32(v, cd_off);
    Put16(v, 0);
    return v;
}

constexpr std::uint32_t kScratch = 0x10001000;  // strings
constexpr std::uint32_t kStack = 0x10002000;    // fake variadic stack area
constexpr std::uint32_t kOut = 0x10003000;      // %n destination

}  // namespace

// Reads the Java arguments of a fake native call into guest words, the way
// AppHost_Call (promoted varargs) and the RegisterNatives stubs (raw) do.
struct JniArgsProbe {
    static inline const char* sig = "()V";
    static inline std::vector<std::uint32_t> words;
    static std::uint32_t Wrap(void* o) {
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(o));
    }
    static void Promoted(void*, void*, ...);
    static void Raw(void*, void*, ...);
    static std::string Shown() {
        std::string out = std::to_string(words.size()) + ":";
        for (std::size_t i = 0; i < words.size(); i++) {
            char b[12];
            std::snprintf(b, sizeof b, "%s%08x", i ? " " : "", words[i]);
            out += b;
        }
        return out;
    }
};
void JniArgsProbe::Promoted(void*, void* thiz, ...) {
    va_list ap;
    va_start(ap, thiz);
    abr::AppendJniArgs(words, sig, ap, abr::JniFloatArgs::kPromoted, Wrap);
    va_end(ap);
}
void JniArgsProbe::Raw(void*, void* thiz, ...) {
    va_list ap;
    va_start(ap, thiz);
    abr::AppendJniArgs(words, sig, ap, abr::JniFloatArgs::kRaw, Wrap);
    va_end(ap);
}

int main() {
    // The quarantine is off by default (REVIEW R-013); its test at the end
    // needs it on. Set before the first free: EnvOn caches its answer.
    setenv("HARNESS_QUARANTINE", "1", 1);
    abr::GuestMemory mem;
    abr::LoadedModule mod;
    // Imports so the dispatcher can be reached for the libc calls below.
    mod.imports.push_back(abr::Import{"sscanf", true, 0, 0, 0});    // 0
    mod.imports.push_back(abr::Import{"strcoll", true, 0, 0, 0});  // 1
    mod.imports.push_back(abr::Import{"memmem", true, 0, 0, 0});    // 2
    mod.imports.push_back(abr::Import{"sysconf", true, 0, 0, 0});   // 3
    mod.imports.push_back(abr::Import{"getpid", true, 0, 0, 0});    // 4
    mod.imports.push_back(abr::Import{"syscall", true, 0, 0, 0});   // 5
    mod.imports.push_back(abr::Import{"setjmp", true, 0, 0, 0});   // 6
    mod.imports.push_back(abr::Import{"longjmp", true, 0, 0, 0});  // 7
    mod.imports.push_back(abr::Import{"pthread_mutex_init", true, 0, 0, 0});     // 8
    mod.imports.push_back(abr::Import{"pthread_mutex_lock", true, 0, 0, 0});     // 9
    mod.imports.push_back(abr::Import{"pthread_mutex_destroy", true, 0, 0, 0});  // 10
    mod.imports.push_back(abr::Import{"pthread_mutex_unlock", true, 0, 0, 0});   // 11
    mod.imports.push_back(abr::Import{"gethostname", true, 0, 0, 0});         // 12
    mod.imports.push_back(abr::Import{"srand", true, 0, 0, 0});            // 13
    mod.imports.push_back(abr::Import{"rand", true, 0, 0, 0});            // 14
    mod.imports.push_back(abr::Import{"snprintf", true, 0, 0, 0});         // 15
    mod.imports.push_back(abr::Import{"btowc", true, 0, 0, 0});            // 16
    mod.imports.push_back(abr::Import{"wctob", true, 0, 0, 0});            // 17
    mod.imports.push_back(abr::Import{"wctype", true, 0, 0, 0});            // 18
    mod.imports.push_back(abr::Import{"getauxval", true, 0, 0, 0});        // 19
    mod.imports.push_back(abr::Import{"iswctype", true, 0, 0, 0});         // 20
    mod.imports.push_back(abr::Import{"fopen", true, 0, 0, 0});           // 21
    mod.imports.push_back(abr::Import{"fclose", true, 0, 0, 0});          // 22
    mod.imports.push_back(abr::Import{"gmtime", true, 0, 0, 0});          // 23
    mod.imports.push_back(abr::Import{"AAssetManager_fromJava", true, 0, 0, 0});  // 24
    mod.imports.push_back(abr::Import{"AAssetManager_open", true, 0, 0, 0});      // 25
    mod.imports.push_back(abr::Import{"opendir", true, 0, 0, 0});         // 26
    mod.imports.push_back(abr::Import{"readdir_r", true, 0, 0, 0});       // 27
    mod.imports.push_back(abr::Import{"closedir", true, 0, 0, 0});        // 28
    mod.imports.push_back(abr::Import{"stat", true, 0, 0, 0});            // 29
    mod.imports.push_back(abr::Import{"fseek", true, 0, 0, 0});           // 30
    mod.imports.push_back(abr::Import{"ftell", true, 0, 0, 0});           // 31
    mod.imports.push_back(abr::Import{"fwrite", true, 0, 0, 0});          // 32
    mod.imports.push_back(abr::Import{"socket", true, 0, 0, 0});          // 33
    mod.imports.push_back(abr::Import{"setsockopt", true, 0, 0, 0});      // 34
    mod.imports.push_back(abr::Import{"getsockopt", true, 0, 0, 0});      // 35
    mod.imports.push_back(abr::Import{"close", true, 0, 0, 0});           // 36
    mod.imports.push_back(abr::Import{"statfs", true, 0, 0, 0});          // 37
    mod.imports.push_back(abr::Import{"poll", true, 0, 0, 0});            // 38
    mod.imports.push_back(abr::Import{"pthread_cond_timedwait_monotonic", true, 0, 0, 0});  // 39
    mod.imports.push_back(abr::Import{"pthread_cond_timedwait", true, 0, 0, 0});  // 40
    mod.imports.push_back(abr::Import{"strtol", true, 0, 0, 0});          // 41
    mod.imports.push_back(abr::Import{"strtoul", true, 0, 0, 0});         // 42
    mod.imports.push_back(abr::Import{"atoi", true, 0, 0, 0});            // 43
    mod.imports.push_back(abr::Import{"nanosleep", true, 0, 0, 0});       // 44
    mod.imports.push_back(abr::Import{"strlen", true, 0, 0, 0});          // 45
    mod.imports.push_back(abr::Import{"strncpy", true, 0, 0, 0});         // 46
    mod.imports.push_back(abr::Import{"strncat", true, 0, 0, 0});         // 47
    mod.imports.push_back(abr::Import{"strcmp", true, 0, 0, 0});          // 48
    mod.imports.push_back(abr::Import{"malloc", true, 0, 0, 0});          // 49
    mod.imports.push_back(abr::Import{"calloc", true, 0, 0, 0});          // 50
    mod.imports.push_back(abr::Import{"realloc", true, 0, 0, 0});         // 51
    mod.imports.push_back(abr::Import{"mmap", true, 0, 0, 0});            // 52
    mod.imports.push_back(abr::Import{"munmap", true, 0, 0, 0});          // 53
    mod.imports.push_back(abr::Import{"open", true, 0, 0, 0});            // 54
    mod.imports.push_back(abr::Import{"fma", true, 0, 0, 0});             // 55
    mod.imports.push_back(abr::Import{"remquo", true, 0, 0, 0});          // 56
    mod.imports.push_back(abr::Import{"remquof", true, 0, 0, 0});         // 57
    mod.imports.push_back(abr::Import{"dup", true, 0, 0, 0});             // 58
    mod.imports.push_back(abr::Import{"mkdir", true, 0, 0, 0});           // 59
    mod.imports.push_back(abr::Import{"read", true, 0, 0, 0});            // 60
    mod.imports.push_back(abr::Import{"mkstemp", true, 0, 0, 0});         // 61
    mod.imports.push_back(abr::Import{"__system_property_get", true, 0, 0, 0});  // 62
    mod.imports.push_back(abr::Import{"access", true, 0, 0, 0});          // 63
    mod.imports.push_back(abr::Import{"pthread_mutexattr_init", true, 0, 0, 0});     // 64
    mod.imports.push_back(abr::Import{"pthread_mutexattr_settype", true, 0, 0, 0});  // 65
    mod.imports.push_back(abr::Import{"pthread_mutex_trylock", true, 0, 0, 0});      // 66
    mod.imports.push_back(abr::Import{"pthread_cond_destroy", true, 0, 0, 0});       // 67
    mod.imports.push_back(abr::Import{"vasprintf", true, 0, 0, 0});       // 68
    mod.imports.push_back(abr::Import{"fnmatch", true, 0, 0, 0});         // 69
    mod.imports.push_back(abr::Import{"basename", true, 0, 0, 0});        // 70
    mod.imports.push_back(abr::Import{"wcslen", true, 0, 0, 0});          // 71
    mod.imports.push_back(abr::Import{"__cxa_guard_acquire", true, 0, 0, 0});  // 72
    mod.imports.push_back(abr::Import{"__cxa_guard_release", true, 0, 0, 0});  // 73
    mod.imports.push_back(abr::Import{"__cxa_guard_abort", true, 0, 0, 0});    // 74
    mod.imports.push_back(abr::Import{"isxdigit", true, 0, 0, 0});  // 75
    mod.imports.push_back(abr::Import{"isspace", true, 0, 0, 0});  // 76
    mod.imports.push_back(abr::Import{"strsep", true, 0, 0, 0});  // 77
    mod.imports.push_back(abr::Import{"strxfrm", true, 0, 0, 0});  // 78
    mod.imports.push_back(abr::Import{"div", true, 0, 0, 0});  // 79
    mod.imports.push_back(abr::Import{"getuid", true, 0, 0, 0});  // 80
    mod.imports.push_back(abr::Import{"getpwuid", true, 0, 0, 0});  // 81
    mod.imports.push_back(abr::Import{"getgrgid", true, 0, 0, 0});  // 82
    mod.imports.push_back(abr::Import{"sched_get_priority_max", true, 0, 0, 0});  // 83
    mod.imports.push_back(abr::Import{"readdir", true, 0, 0, 0});  // 84
    mod.imports.push_back(abr::Import{"chdir", true, 0, 0, 0});  // 85
    mod.imports.push_back(abr::Import{"getcwd", true, 0, 0, 0});  // 86
    mod.imports.push_back(abr::Import{"getenv", true, 0, 0, 0});  // 87
    mod.imports.push_back(abr::Import{"setenv", true, 0, 0, 0});  // 88
    mod.imports.push_back(abr::Import{"unsetenv", true, 0, 0, 0});  // 89
    mod.imports.push_back(abr::Import{"SHA1Init", true, 0, 0, 0});  // 90
    mod.imports.push_back(abr::Import{"SHA1Update", true, 0, 0, 0});  // 91
    mod.imports.push_back(abr::Import{"SHA1Final", true, 0, 0, 0});  // 92
    mod.imports.push_back(abr::Import{"crc32", true, 0, 0, 0});  // 93
    mod.imports.push_back(abr::Import{"inflateInit_", true, 0, 0, 0});  // 94
    mod.imports.push_back(abr::Import{"inflate", true, 0, 0, 0});  // 95
    mod.imports.push_back(abr::Import{"inflateEnd", true, 0, 0, 0});  // 96
    mod.imports.push_back(abr::Import{"mbrtowc", true, 0, 0, 0});  // 97
    mod.imports.push_back(abr::Import{"wcrtomb", true, 0, 0, 0});  // 98
    mod.imports.push_back(abr::Import{"fscanf", true, 0, 0, 0});  // 99
    mod.imports.push_back(abr::Import{"getaddrinfo", true, 0, 0, 0});  // 100
    mod.imports.push_back(abr::Import{"freeaddrinfo", true, 0, 0, 0});  // 101
    mod.imports.push_back(abr::Import{"gai_strerror", true, 0, 0, 0});  // 102
    mod.imports.push_back(abr::Import{"inet_aton", true, 0, 0, 0});  // 103
    mod.imports.push_back(abr::Import{"mmap", true, 0, 0, 0});  // 104
    mod.imports.push_back(abr::Import{"mremap", true, 0, 0, 0});  // 105
    mod.imports.push_back(abr::Import{"pthread_cond_signal", true, 0, 0, 0});  // 106
    mod.imports.push_back(abr::Import{"pthread_cond_wait", true, 0, 0, 0});    // 107
    mod.imports.push_back(abr::Import{"pthread_key_create", true, 0, 0, 0});   // 108
    mod.imports.push_back(abr::Import{"pthread_setspecific", true, 0, 0, 0});  // 109
    mod.imports.push_back(abr::Import{"pthread_getspecific", true, 0, 0, 0});  // 110
    mod.imports.push_back(abr::Import{"pthread_key_delete", true, 0, 0, 0});   // 111
    mod.imports.push_back(abr::Import{"pthread_attr_init", true, 0, 0, 0});        // 112
    mod.imports.push_back(abr::Import{"pthread_attr_setstacksize", true, 0, 0, 0});  // 113
    // Every fast-math function twice: through the name chain (fast_math 0)
    // and through the load-time table (fast_math = index + 1).
    const std::uint32_t kMathBase = static_cast<std::uint32_t>(mod.imports.size());
    for (std::size_t i = 0; i < abr::kFastMathCount; i++) {
        mod.imports.push_back(abr::Import{abr::kFastMath[i].name, true, 0, 0, 0});
        mod.imports.push_back(abr::Import{abr::kFastMath[i].name, true, 0, 0,
                                          static_cast<std::uint8_t>(i + 1)});
    }
    for (abr::Import& imp : mod.imports) {
        imp.hot = abr::HotImportForName(imp.name);  // as the loader does
        imp.gl_id = abr::GlIdForName(imp.name.c_str());
    }
    abr::Runtime rt(mem, mod);

    auto put = [&](std::uint32_t addr, const std::string& s) {
        std::memcpy(rt.H<std::uint8_t>(addr), s.data(), s.size() + 1);
    };
    put(kScratch, "hello");
    put(kScratch + 0x40, "world");

    // --- fast-math table: bit-identical to the name chain for every entry
    //     (r0 and r1 compared: a double result is r0:r1).
    {
        const float fx[] = {0.5f, -1.25f, 3.0f, 100.0f, 0.0f};
        const float fy[] = {2.0f, -0.75f, 0.5f, 3.0f, 1.0f};
        const double dx[] = {0.5, -1.25, 3.0, 100.0, 0.0};
        const double dy[] = {2.0, -0.75, 0.5, 3.0, 1.0};
        int bad = 0;
        std::string first_bad;
        for (std::size_t i = 0; i < abr::kFastMathCount; i++) {
            for (int k = 0; k < 5; k++) {
                std::uint32_t args[4] = {0, 0, 0, 0};
                switch (abr::kFastMath[i].kind) {
                case abr::MathKind::F_F:
                case abr::MathKind::FF_F:
                    std::memcpy(&args[0], &fx[k], 4);
                    std::memcpy(&args[1], &fy[k], 4);
                    break;
                case abr::MathKind::D_D:
                case abr::MathKind::DD_D:
                    std::memcpy(&args[0], &dx[k], 8);
                    std::memcpy(&args[2], &dy[k], 8);
                    break;
                }
                abr::RuntimeTestAccess::RegsView chain{}, fast{};
                const abr::RuntimeTestAccess::RegsView in{};
                abr::RuntimeTestAccess::Call(rt, kMathBase + 2 * i, args, in, &chain);
                abr::RuntimeTestAccess::Call(rt, kMathBase + 2 * i + 1, args, in, &fast);
                if (chain[0] != fast[0] || chain[1] != fast[1]) {
                    if (bad++ == 0) {
                        first_bad = std::string(abr::kFastMath[i].name) + " #" + std::to_string(k);
                    }
                }
            }
        }
        Expect("fast math = name chain, all " + std::to_string(abr::kFastMathCount) + " functions",
               bad ? first_bad : std::string("same"), "same");
    }

    // --- 4-byte integer argument: one register slot, the next argument
    //     must survive (%zu used to read 8 bytes and eat the next slot).
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;  // fmt
        st.gargs[1] = 12345;  // size_t
        st.gargs[2] = kScratch;  // const char*
        st.ai = 1;
        Expect("%zu then %s", rt.ExpandPrintf("%zu %s", st), "12345 hello");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0xFFFFFFFFu;  // (size_t)-1
        st.gargs[2] = kScratch;
        st.ai = 1;
        Expect("%zu of -1", rt.ExpandPrintf("%zu", st), "4294967295");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0xFFFFFFF0u;  // ptrdiff_t
        st.ai = 1;
        Expect("%td negative", rt.ExpandPrintf("%td", st), "-16");
    }

    // --- 8-byte integer argument: AAPCS32 EVEN core-register pair. With
    //     ai == 1, r1 is skipped and the value is in r2:r3; with ai == 3
    //     there is no complete pair, so the whole value is on the stack at
    //     an 8-byte aligned address.
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0xDEADBEEFu;  // r1: skipped
        st.gargs[2] = 0x55667788u;  // r2: low word
        st.gargs[3] = 0x11223344u;  // r3: high word
        st.ai = 1;
        Expect("%jd skips r1 -> r2:r3", rt.ExpandPrintf("%jd", st),
               "1234605616436508552");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        const std::uint64_t v = 0x1122334455667788ull;
        std::memcpy(rt.H<std::uint8_t>(kStack), &v, 8);
        st.gargs[0] = 0;
        st.gargs[1] = 0xDEADBEEFu;  // r1: skipped
        st.gargs[2] = 0xDEADBEEFu;  // r2
        st.gargs[3] = 0xDEADBEEFu;  // r3: no complete pair left
        st.ai = 3;
        st.stack = kStack;
        Expect("%jd from the stack", rt.ExpandPrintf("%jd", st), "1234605616436508552");
    }
    {
        // Same value, but now r0 and r1 are taken, so the pair is r2:r3.
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;         // named arg 1
        st.gargs[1] = 0xBBBBBBBBu;  // named arg 2
        st.gargs[2] = 0x55667788u;  // r2: low word
        st.gargs[3] = 0x11223344u;  // r3: high word
        st.ai = 2;
        Expect("%jd from r2:r3", rt.ExpandPrintf("%jd", st), "1234605616436508552");
    }
    {
        // %lld is 8 bytes too; ai == 1 skips r1 exactly like %jd.
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0xDEADBEEFu;  // r1: skipped
        st.gargs[2] = 0x55667788u;  // r2: low word
        st.gargs[3] = 0x11223344u;  // r3: high word
        st.ai = 1;
        Expect("%lld skips r1 -> r2:r3", rt.ExpandPrintf("%lld", st),
               "1234605616436508552");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0xFFFFFFF0u;
        st.ai = 1;
        Expect("%ld 32-bit", rt.ExpandPrintf("%ld", st), "-16");
    }

    // --- %h / %hh narrow the PRINTED value (the argument itself is an int:
    //     the guest promotes short/char before the call).
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0x12345;  // 0x2345 as a short
        st.ai = 1;
        Expect("%hu narrowed", rt.ExpandPrintf("%hu", st), "9029");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0x123;  // 0x23 as a char
        st.ai = 1;
        Expect("%hhu narrowed", rt.ExpandPrintf("%hhu", st), "35");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0xFFFF8000u;  // -32768 as a short
        st.ai = 1;
        Expect("%hd narrowed", rt.ExpandPrintf("%hd", st), "-32768");
    }

    // --- a double follows the same AAPCS32 even-pair rule.
    {
        // r0 and r1 are taken, so the pair is r2:r3.
        abr::Runtime::GuestFmtArgs st{};
        const double d = 1.5;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, 8);
        st.gargs[0] = 0;         // named arg 1
        st.gargs[1] = 0xBBBBBBBBu;  // named arg 2
        st.gargs[2] = static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);  // r2: low word
        st.gargs[3] = static_cast<std::uint32_t>(bits >> 32);          // r3: high word
        st.ai = 2;
        Expect("%f from r2:r3", rt.ExpandPrintf("%f", st), "1.500000");
    }
    {
        // printf("%f", d): the first variadic argument is the double, so
        // r1 is skipped and the value is in r2:r3. This is the test that
        // fails under the old base-PCS rule (which read r1:r2).
        abr::Runtime::GuestFmtArgs st{};
        const double d = 2.25;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, 8);
        st.gargs[0] = 0;         // fmt
        st.gargs[1] = 0xDEADBEEFu;  // r1: skipped
        st.gargs[2] = static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);  // r2: low word
        st.gargs[3] = static_cast<std::uint32_t>(bits >> 32);          // r3: high word
        st.ai = 1;
        Expect("%f skips r1 -> r2:r3", rt.ExpandPrintf("%f", st), "2.250000");
    }
    {
        // printf("%d %f", i, d): %d takes r1, so the double is in r2:r3.
        abr::Runtime::GuestFmtArgs st{};
        const double d = 2.25;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, 8);
        st.gargs[0] = 0;         // fmt
        st.gargs[1] = 42;        // %d: r1
        st.gargs[2] = static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);  // r2: low word
        st.gargs[3] = static_cast<std::uint32_t>(bits >> 32);          // r3: high word
        st.ai = 1;
        Expect("%d %f from r2:r3", rt.ExpandPrintf("%d %f", st), "42 2.250000");
    }
    {
        // sprintf(dst, "%d %f", i, d): dst/fmt take r0/r1, %d takes r2,
        // and the double has no complete even pair left -> [sp].
        abr::Runtime::GuestFmtArgs st{};
        const double d = 1.5;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, 8);
        std::memcpy(rt.H<std::uint8_t>(kStack), &bits, 8);
        st.gargs[0] = 0;         // dst
        st.gargs[1] = 0;         // fmt
        st.gargs[2] = 42;        // %d: r2
        st.gargs[3] = 0xDEADBEEFu;  // r3: skipped
        st.ai = 2;
        st.stack = kStack;
        Expect("%d %f from the stack", rt.ExpandPrintf("%d %f", st), "42 1.500000");
    }
    {
        // The real guest shape: Rio/Space/Star Wars WebP
        //   fprintf(fp, "Note: Xing stream size %lu differs by %f%% ...",
        //           lu, d)
        // has the %lu in r2 and the double wholly at [sp]; r3 is not the
        // low word. This is the call site that proves AAPCS32.
        abr::Runtime::GuestFmtArgs st{};
        const double d = 1.5;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, 8);
        std::memcpy(rt.H<std::uint8_t>(kStack), &bits, 8);
        st.gargs[0] = 0;         // FILE*
        st.gargs[1] = 0;         // fmt
        st.gargs[2] = 4660;      // %lu: r2
        st.gargs[3] = 0xDEADBEEFu;  // r3: stale, not the double
        st.ai = 2;
        st.stack = kStack;
        Expect("guest %lu %f from the stack", rt.ExpandPrintf("%lu %f", st),
               "4660 1.500000");
    }

    // --- AAPCS32 va_list: the consumer reads AT the pointer and advances
    //     by 4 / 8; the CALLER aligns the 8-byte slots (a compiler built
    //     area puts them at 8-byte aligned addresses), so an 8-byte read
    //     rounds the cursor up to the next 8 first.
    {
        const std::uint32_t area = kScratch + 0x80;  // 8-byte aligned
        const std::uint32_t v1 = 0x11223344u;
        const std::uint64_t v2 = 0x5566778899AABBCCull;
        std::memcpy(rt.H<std::uint8_t>(area), &v1, 4);
        std::memcpy(rt.H<std::uint8_t>(area + 8), &v2, 8);  // the next aligned slot
        abr::Runtime::GuestFmtArgs st{};
        st.via_va = true;
        st.stack = area;
        Expect("va: %u then %lld", rt.ExpandPrintf("%u %lld", st),
               "287454020 6153737369425722316");
    }

    // --- %n writes the number of characters written so far; the width
    //     follows the length modifier.
    {
        std::memset(rt.H<std::uint8_t>(kOut), 0xAA, 16);
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = kOut;
        st.ai = 1;
        rt.ExpandPrintf("abc%n", st);
        std::uint32_t n = 0;
        std::memcpy(&n, rt.H<std::uint8_t>(kOut), 4);
        Expect("%n count", std::to_string(n), "3");
    }
    {
        std::memset(rt.H<std::uint8_t>(kOut), 0xAA, 16);
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = kOut;
        st.ai = 1;
        rt.ExpandPrintf("abcd%hn", st);
        std::uint16_t n = 0;
        std::memcpy(&n, rt.H<std::uint8_t>(kOut), 2);
        Expect("%hn count", std::to_string(n), "4");
    }

    // --- sscanf: the DESTINATION width follows the length modifier
    //     (C99 7.21.6.2): hh 1, h 2, none/l/z/t 4, ll/j 8; %f float (4),
    //     %lf/%Lf double (8).
    {
        const std::uint32_t kIn = 0x10005000;
        const std::uint32_t kFmt = 0x10005100;
        const std::uint32_t kDst = 0x10005200;
        auto scanf = [&](const char* in, const char* fmt, std::uint32_t dst, std::uint32_t dst2) {
            put(kIn, in);
            put(kFmt, fmt);
            std::memset(rt.H<std::uint8_t>(dst), 0xAA, 32);
            const std::uint32_t args[4] = {kIn, kFmt, dst, dst2};
            return abr::RuntimeTestAccess::Call(rt, 0, args);
        };
        {
            const std::uint32_t n = scanf("123 -456", "%d %d", kDst, kDst + 8);
            std::int32_t a = 0, b = 0;
            std::memcpy(&a, rt.H<std::uint8_t>(kDst), 4);
            std::memcpy(&b, rt.H<std::uint8_t>(kDst + 8), 4);
            Expect("sscanf %d %d", std::to_string(a) + " " + std::to_string(b) + " n=" +
                                        std::to_string(n),
                   "123 -456 n=2");
        }
        {
            scanf("1234567890123", "%lld", kDst, 0);
            std::int64_t v = 0;
            std::memcpy(&v, rt.H<std::uint8_t>(kDst), 8);
            Expect("sscanf %lld (8 bytes)", std::to_string(v), "1234567890123");
        }
        {
            scanf("-1234567890123", "%jd", kDst, 0);
            std::int64_t v = 0;
            std::memcpy(&v, rt.H<std::uint8_t>(kDst), 8);
            Expect("sscanf %jd (8 bytes)", std::to_string(v), "-1234567890123");
        }
        {
            scanf("-70000", "%hd", kDst, 0);
            std::int16_t v = 0;
            std::memcpy(&v, rt.H<std::uint8_t>(kDst), 2);
            const std::uint8_t tail = rt.H<std::uint8_t>(kDst)[2];
            Expect("sscanf %hd (2 bytes)", std::to_string(v) + " tail=" + std::to_string(tail),
                   "-4464 tail=170");
        }
        {
            scanf("300", "%hhu", kDst, 0);
            std::uint8_t v = 0;
            std::memcpy(&v, rt.H<std::uint8_t>(kDst), 1);
            Expect("sscanf %hhu (1 byte)", std::to_string((int)v), "44");
        }
        {
            scanf("4294967295", "%zu", kDst, 0);
            std::uint32_t v = 0;
            std::memcpy(&v, rt.H<std::uint8_t>(kDst), 4);
            Expect("sscanf %zu (4 bytes)", std::to_string(v), "4294967295");
        }
        {
            scanf("1.5", "%f", kDst, 0);
            float f = 0;
            std::memcpy(&f, rt.H<std::uint8_t>(kDst), 4);
            Expect("sscanf %f (float, 4 bytes)", std::to_string(f), "1.500000");
        }
        {
            scanf("1.5", "%lf", kDst, 0);
            double d = 0;
            std::memcpy(&d, rt.H<std::uint8_t>(kDst), 8);
            Expect("sscanf %lf (double, 8 bytes)", std::to_string(d), "1.500000");
        }
    }

    // --- the libc calls the guests actually make (they used to fall into
    //     the generic tail, which answers 0 for everything).
    {
        const std::uint32_t kS1 = 0x10006000;
        const std::uint32_t kS2 = 0x10006100;
        const std::uint32_t kHay = 0x10006200;
        const std::uint32_t kNdl = 0x10006300;
        put(kS1, "abc");
        put(kS2, "abd");
        auto call = [&](std::uint32_t idx, const std::uint32_t args[4]) {
            return abr::RuntimeTestAccess::Call(rt, idx, args);
        };
        {
            const std::uint32_t args[4] = {kS1, kS2, 0, 0};
            const std::int32_t r = (std::int32_t)call(1, args);
            Expect("strcoll abc<abd", std::to_string(r < 0 ? -1 : (r > 0 ? 1 : 0)), "-1");
        }
        {
            const std::uint32_t args[4] = {kS1, kS1, 0, 0};
            const std::int32_t r = (std::int32_t)call(1, args);
            Expect("strcoll abc==abc", std::to_string(r), "0");
        }
        {
            const std::uint32_t args[4] = {kS2, kS1, 0, 0};
            const std::int32_t r = (std::int32_t)call(1, args);
            Expect("strcoll abd>abc", std::to_string(r > 0 ? 1 : (r < 0 ? -1 : 0)), "1");
        }
        {
            put(kHay, "0123456789abcdef");
            put(kNdl, "456");
            const std::uint32_t args[4] = {kHay, 16, kNdl, 3};
            const std::uint32_t r = call(2, args);
            Expect("memmem found", std::to_string(r - kHay), "4");
        }
        {
            put(kNdl, "xyz");
            const std::uint32_t args[4] = {kHay, 16, kNdl, 3};
            Expect("memmem missing", std::to_string(call(2, args)), "0");
        }
        {
            // _SC_PAGESIZE is 30 in Linux.
            const std::uint32_t args[4] = {30, 0, 0, 0};
            const std::uint32_t r = call(3, args);
            Expect("sysconf _SC_PAGESIZE", std::to_string(r != 0 ? 1 : 0), "1");
        }
        {
            const std::uint32_t args[4] = {0, 0, 0, 0};
            Expect("getpid nonzero", std::to_string(call(4, args) != 0 ? 1 : 0), "1");
        }
        {
            // syscall(224) = gettid.
            const std::uint32_t args[4] = {224, 0, 0, 0};
            Expect("syscall gettid", std::to_string(call(5, args)), "1");
        }
        {
            // syscall(268) = tgkill: the guest signals itself; report success.
            const std::uint32_t args[4] = {268, 0, 0, 0};
            Expect("syscall tgkill", std::to_string(call(5, args)), "0");
        }
        {
            // An unknown number is refused instead of silently answering 0.
            const std::uint32_t args[4] = {4242, 0, 0, 0};
            const std::int32_t r = (std::int32_t)call(5, args);
            Expect("syscall unknown -> -ENOSYS", std::to_string(r), "-38");
        }
    }

    // --- setjmp / longjmp: the jmp_buf is the guest's, the layout is ours.
    {
        const std::uint32_t kBuf = 0x10006400;
        auto hex = [](std::uint32_t v) {
            char b[16];
            std::snprintf(b, sizeof(b), "0x%08x", v);
            return std::string(b);
        };
        abr::RuntimeTestAccess::RegsView in{};
        in[4] = 0x11111111u;
        in[5] = 0x22222222u;
        in[13] = 0x33333333u;  // sp
        in[14] = 0x44444445u;  // lr (odd => the Thumb bit)
        abr::RuntimeTestAccess::RegsView out{};
        const std::uint32_t args[4] = {kBuf, 0, 0, 0};
        const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, 6, args, in, &out);
        Expect("setjmp returns 0", std::to_string(r), "0");
        {
            std::uint32_t saved[10];
            std::memcpy(saved, rt.H<std::uint8_t>(kBuf), 40);
            Expect("setjmp saved r4", hex(saved[0]), "0x11111111");
            Expect("setjmp saved r5", hex(saved[1]), "0x22222222");
            Expect("setjmp saved sp", hex(saved[8]), "0x33333333");
            Expect("setjmp saved lr", hex(saved[9]), "0x44444445");
        }
        {
            // longjmp restores the callee-saved registers, the stack pointer
            // and makes the guest continue at the saved return address.
            in[4] = 0;
            in[13] = 0;
            in[14] = 0;
            const std::uint32_t args2[4] = {kBuf, 1, 0, 0};
            const std::uint32_t r2 = abr::RuntimeTestAccess::Call(rt, 7, args2, in, &out);
            Expect("longjmp value", std::to_string(r2), "1");
            Expect("longjmp restores r4", hex(out[4]), "0x11111111");
            Expect("longjmp restores r5", hex(out[5]), "0x22222222");
            Expect("longjmp restores sp", hex(out[13]), "0x33333333");
            Expect("longjmp resumes at the saved lr", hex(out[15]), "0x44444444");
        }
        {
            // C99: longjmp(env, 0) must still make setjmp return 1.
            const std::uint32_t args3[4] = {kBuf, 0, 0, 0};
            Expect("longjmp 0 -> 1", std::to_string(abr::RuntimeTestAccess::Call(rt, 7, args3, in, &out)),
                   "1");
        }
    }

    // --- a mutex that is destroyed while held must not be freed: the owner
    //     map is filled only AFTER a lock succeeds, so a destroy in that
    //     window would leave the holder with a dangling futex.
    {
        const std::uint32_t kMutex = 0x10006500;
        const std::uint32_t init[4] = {kMutex, 0, 0, 0};
        const std::uint32_t lock[4] = {kMutex, 0, 0, 0};
        const std::uint32_t destroy[4] = {kMutex, 0, 0, 0};
        const std::uint32_t unlock[4] = {kMutex, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 8, init);
        Expect("mutex lock", std::to_string(abr::RuntimeTestAccess::Call(rt, 9, lock)), "0");
        abr::RuntimeTestAccess::Call(rt, 10, destroy);  // must be skipped
        Expect("mutex unlock after a skipped destroy",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 11, unlock)), "0");
        Expect("mutex lock after a skipped destroy",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 9, lock)), "0");
    }

    // --- pthread_mutex_destroy in the window between the lock succeeding and
    //     the owner being recorded: the destroy must still not free the
    //     mutex, and the mutex must stay usable afterwards.
    {
        const std::uint32_t kM = 0x10006800;
        const std::uint32_t init[4] = {kM, 0, 0, 0};
        const std::uint32_t lock[4] = {kM, 0, 0, 0};
        const std::uint32_t destroy[4] = {kM, 0, 0, 0};
        const std::uint32_t unlock[4] = {kM, 0, 0, 0};
        rt.test_skip_mutex_owner_ = true;  // the owner is NOT recorded
        abr::RuntimeTestAccess::Call(rt, 8, init);
        abr::RuntimeTestAccess::Call(rt, 9, lock);
        const unsigned before = rt.mutex_destroy_skipped_;
        abr::RuntimeTestAccess::Call(rt, 10, destroy);  // must be refused
        Expect("the raced destroy was refused",
               std::to_string(rt.mutex_destroy_skipped_ > before ? 1 : 0), "1");
        // The mutex is a NORMAL one (bionic default): release the hold the
        // test took, then it must lock and unlock again.
        Expect("mutex unlock after a raced destroy",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 11, unlock)), "0");
        Expect("mutex usable after a raced destroy",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 9, lock)), "0");
        Expect("mutex unlock again",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 11, unlock)), "0");
        rt.test_skip_mutex_owner_ = false;
    }

    // --- the stuck-update scan must not read past the mapped region: the
    //     guest address space is one PROT_NONE 4GB reservation, so a read
    //     past the end of the heap would SIGSEGV the host.
    {
        const std::uint32_t sp = abr::kHeapBase + abr::kHeapSize - 8;
        abr::Runtime::StackHit hits[8];
        const abr::Runtime::StackScan sc = rt.ScanStackForReturnAddresses(sp, 0x4000, hits, 8);
        Expect("stack scan reads only mapped words",
               std::to_string(sc.words_read <= 2 ? 1 : 0), "1");
    }

    // --- gethostname: POSIX semantics -- 0 on success, the NAME in the
    //     buffer. The guest uses the return value as a pointer, so a
    //     non-zero answer makes it dereference a small integer.
    {
        const std::uint32_t kHost = 0x10006600;
        std::memset(rt.H<std::uint8_t>(kHost), 0, 64);
        const std::uint32_t args[4] = {kHost, 64, 0, 0};
        const std::uint32_t n = abr::RuntimeTestAccess::Call(rt, 12, args);
        const char* s = reinterpret_cast<const char*>(rt.H<std::uint8_t>(kHost));
        Expect("gethostname returns 0", std::to_string(n), "0");
        Expect("gethostname nul-terminated", std::to_string(s[0] != '\0' ? 1 : 0), "1");
    }

    // --- the BSD random() generator must agree with the host's random()
    //     (same core as bionic's: bionic's rand() IS random()).
    {
        abr::BsdRandom rng;
        ::srandom(1);
        bool same = true;
        for (int i = 0; i < 32; i++) {
            if (rng.Next() != static_cast<std::int32_t>(::random())) {
                same = false;
                break;
            }
        }
        Expect("bsd random matches the host's random()", same ? "same" : "different", "same");
    }
    {
        // And the dispatcher must answer rand()/srand() from it.
        const std::uint32_t seed[4] = {12345, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 13, seed);  // srand(12345)
        const std::uint32_t args[4] = {0, 0, 0, 0};
        const std::uint32_t a = abr::RuntimeTestAccess::Call(rt, 14, args);  // rand()
        const std::uint32_t b = abr::RuntimeTestAccess::Call(rt, 14, args);  // rand()
        Expect("rand() is in range", std::to_string(a < 0x80000000u ? 1 : 0), "1");
        Expect("rand() advances", std::to_string(a != b ? 1 : 0), "1");
    }

    // --- snprintf must return the C99 length (the length the expansion WOULD
    //     have), and write at most cap-1 characters plus the NUL.
    {
        const std::uint32_t kDst = 0x10006700;
        const std::uint32_t kLong = 0x10006800;
        std::string big(600, 'x');
        put(kLong, big.c_str());
        const std::uint32_t args[4] = {kDst, 16, kLong, 0};  // snprintf(dst, 16, "%s", big)
        const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, 15, args);
        Expect("snprintf returns the C99 length", std::to_string(r), std::to_string(big.size()));
        std::string written(reinterpret_cast<const char*>(rt.H<std::uint8_t>(kDst)));
        Expect("snprintf wrote cap-1 chars", std::to_string(written.size()), "15");
    }

    // --- sscanf: a value that does not fit must saturate, not abort the scan.
    {
        const std::uint32_t kIn = 0x10006900;
        const std::uint32_t kFmt = 0x10006a00;
        const std::uint32_t kDst = 0x10006b00;
        put(kIn, "99999999999999999999");
        put(kFmt, "%lld");
        const std::uint32_t args[4] = {kIn, kFmt, kDst, 0};
        const std::uint32_t n = abr::RuntimeTestAccess::Call(rt, 0, args);  // sscanf
        std::int64_t v = 0;
        std::memcpy(&v, rt.H<std::uint8_t>(kDst), 8);
        Expect("sscanf of an out-of-range number still converts", std::to_string(n), "1");
        Expect("sscanf saturated to LLONG_MAX",
               std::to_string(v == 9223372036854775807LL ? 1 : 0), "1");
    }

    // --- an unknown EABI syscall must answer -ENOSYS like a kernel does and
    //     must NOT stop the guest thread (the old code halted it, which turned
    //     a missing syscall into a dead game).
    {
        bool stopped = false;
        const std::uint32_t r =
            abr::RuntimeTestAccess::GuestSyscall(rt, /*nr=*/999, 0, 0, 0, &stopped);
        Expect("unknown syscall answers -ENOSYS",
               std::to_string(r == static_cast<std::uint32_t>(-ENOSYS) ? 1 : 0), "1");
        Expect("unknown syscall does not stop the guest",
               std::to_string(stopped ? 0 : 1), "1");
    }
    // Positive control: a syscall the harness implements still answers.
    {
        bool stopped = false;
        const std::uint32_t r =
            abr::RuntimeTestAccess::GuestSyscall(rt, /*nr=*/20 /*getpid*/, 0, 0, 0, &stopped);
        Expect("known syscall (getpid) still answers", std::to_string(r > 0 ? 1 : 0), "1");
        Expect("known syscall does not stop the guest",
               std::to_string(stopped ? 0 : 1), "1");
    }

    // --- the wide-character ctype: Bionic's wctype takes a property NAME and
    //     returns the enum 1..12; iswctype takes that enum plus a wide char.
    //     The guests build their OWN classification tables from these answers
    //     at init (measured in the Star Wars guest: a 256-entry btowc table
    //     and a 16-entry wctype table), so a wrong shape poisons those tables
    //     silently. The strings are written into guest memory: a host pointer
    //     would make the test pass through a path the guest never uses.
    {
        const std::uint32_t a[4] = {'A', 0, 0, 0};
        Expect("btowc of a printable byte", std::to_string(abr::RuntimeTestAccess::Call(rt, 16, a)), "65");
        const std::uint32_t b[4] = {200, 0, 0, 0};
        Expect("btowc(200) is WEOF",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 16, b)), "4294967295");
        const std::uint32_t c[4] = {'x', 0, 0, 0};
        Expect("wctob of a printable wide char",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 17, c)), "120");
        const std::uint32_t d[4] = {300, 0, 0, 0};
        Expect("wctob(300) is EOF",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 17, d)), "4294967295");

        // wctype() returns Bionic's WC_TYPE_* enum, not a ctype bit mask.
        const char* const names[12] = {
            "alnum", "alpha", "blank", "cntrl", "digit", "graph",
            "lower", "print", "punct", "space", "upper", "xdigit"};
        for (std::uint32_t i = 0; i < 12; ++i) {
            const std::uint32_t addr = kScratch + 0x100 + i * 0x20;
            put(addr, names[i]);
            const std::uint32_t args[4] = {addr, 0, 0, 0};
            Expect(std::string("wctype(\"") + names[i] + "\")",
                   std::to_string(abr::RuntimeTestAccess::Call(rt, 18, args)),
                   std::to_string(i + 1));
        }
        const std::uint32_t unknown = kScratch + 0x100 + 12 * 0x20;
        put(unknown, "no-such-class");
        {
            const std::uint32_t args[4] = {unknown, 0, 0, 0};
            Expect("wctype(unknown) is 0",
                   std::to_string(abr::RuntimeTestAccess::Call(rt, 18, args)), "0");
        }
        {
            const std::uint32_t args[4] = {0, 0, 0, 0};
            Expect("wctype(NULL) is 0 after LOUD",
                   std::to_string(abr::RuntimeTestAccess::Call(rt, 18, args)), "0");
        }
        {
            const std::uint32_t args[4] = {0xFFFFFFF0u, 0, 0, 0};
            Expect("wctype(unmapped) is 0 after LOUD",
                   std::to_string(abr::RuntimeTestAccess::Call(rt, 18, args)), "0");
        }

        // iswctype(wc, type): Bionic 7.1.2 returns the classifier's class
        // MASK (only blank is the explicit 0/1 test). Pinning the exact
        // values also pins the X=0x40/B=0x80 order from ctype.h.
        auto isw = [&](std::uint32_t wc, std::uint32_t type) {
            const std::uint32_t args[4] = {wc, type, 0, 0};
            return abr::RuntimeTestAccess::Call(rt, 20, args);
        };
        auto check = [&](const char* what, std::uint32_t wc, std::uint32_t type,
                         std::uint32_t want) {
            Expect(what, std::to_string(isw(wc, type)), std::to_string(want));
        };
        check("iswctype A/alnum", 'A', 1, 1);
        check("iswctype 5/alnum", '5', 1, 4);
        check("iswctype !/alnum", '!', 1, 0);
        check("iswctype a/alpha", 'a', 2, 2);
        check("iswctype 5/alpha", '5', 2, 0);
        check("iswctype space/blank", ' ', 3, 1);
        check("iswctype tab/blank", '\t', 3, 1);
        check("iswctype newline/blank", '\n', 3, 0);
        check("iswctype newline/cntrl", '\n', 4, 32);
        check("iswctype A/cntrl", 'A', 4, 0);
        check("iswctype 5/digit", '5', 5, 4);
        check("iswctype !/graph", '!', 6, 16);
        check("iswctype space/graph", ' ', 6, 0);
        check("iswctype a/lower", 'a', 7, 2);
        check("iswctype A/lower", 'A', 7, 0);
        check("iswctype space/print", ' ', 8, 128);
        check("iswctype newline/print", '\n', 8, 0);
        check("iswctype !/punct", '!', 9, 16);
        check("iswctype A/punct", 'A', 9, 0);
        check("iswctype space/space", ' ', 10, 8);
        check("iswctype tab/space", '\t', 10, 8);
        check("iswctype A/space", 'A', 10, 0);
        check("iswctype A/upper", 'A', 11, 1);
        check("iswctype a/upper", 'a', 11, 0);
        check("iswctype F/xdigit", 'F', 12, 64);
        check("iswctype 5/xdigit", '5', 12, 4);
        check("iswctype G/xdigit", 'G', 12, 0);
        check("iswctype 0x80/alpha", 0x80, 2, 0);
        check("iswctype A/type 0", 'A', 0, 0);
        check("iswctype A/type 13", 'A', 13, 0);
    }

    // --- JNI guest-data regions: strings [kJniStringStart,kJniStringLimit),
    //     object handles [kJniObjectStart,kJniObjectLimit). The allocator must
    //     reject the first byte past a region before any guest write, and the
    //     32-bit alignment arithmetic must not wrap at UINT32_MAX.
    {
        std::uint32_t at = 0;
        const std::uint32_t obj_last = abr::kJniObjectLimit - 4;
        Expect("JNI object last slot",
               std::to_string(abr::JniObjectHandleFits(obj_last, &at)), "1");
        Expect("JNI object last addr", std::to_string(at), std::to_string(obj_last));
        Expect("JNI object first invalid",
               std::to_string(abr::JniObjectHandleFits(obj_last + 1, &at)), "0");
        Expect("JNI object at limit",
               std::to_string(abr::JniObjectHandleFits(abr::kJniObjectLimit, &at)), "0");
        Expect("JNI object UINT32_MAX",
               std::to_string(abr::JniObjectHandleFits(0xFFFFFFFFu, &at)), "0");

        const std::uint32_t str_last = abr::kJniStringLimit - 4;
        Expect("JNI string last slot",
               std::to_string(abr::JniStringFits(str_last, 4, &at)), "1");
        Expect("JNI string last addr", std::to_string(at), std::to_string(str_last));
        Expect("JNI string first invalid",
               std::to_string(abr::JniStringFits(str_last + 1, 1, &at)), "0");
        Expect("JNI string cannot enter objects",
               std::to_string(abr::JniStringFits(abr::kJniStringLimit, 1, &at)), "0");
        Expect("JNI string UINT32_MAX next",
               std::to_string(abr::JniStringFits(0xFFFFFFFFu, 1, &at)), "0");
        Expect("JNI string UINT32_MAX len",
               std::to_string(abr::JniStringFits(abr::kJniStringStart, 0xFFFFFFFFu, &at)), "0");
        Expect("JNI string len overflow",
               std::to_string(abr::JniStringFits(abr::kJniStringStart, 0x100000000ull, &at)), "0");
    }

    // --- getauxval: the measured Star Wars consumer at 0x6708e4..0x6708f0
    //     tests HWCAP bit 12. On ARM32 that bit is NEON, not FAST_MULT; the
    //     old test encoded the arm64 label and also mislabelled VFP. Pin the
    //     real armv7-a mask and the bits that must stay absent.
    {
        constexpr std::uint32_t kAtHwcap = 16;
        constexpr std::uint32_t kAtPagesz = 6;
        constexpr std::uint32_t kAtHwcap2 = 26;
        const std::uint32_t hw[4] = {kAtHwcap, 0, 0, 0};
        const std::uint32_t hwcap = abr::RuntimeTestAccess::Call(rt, 19, hw);
        Expect("getauxval(AT_HWCAP) is the armv7-a mask",
               std::to_string(hwcap), std::to_string(abr::kArmV7Hwcap));
        Expect("getauxval(AT_HWCAP) has NEON (bit 12)",
               std::to_string(hwcap & abr::kHwcapNeon),
               std::to_string(abr::kHwcapNeon));
        Expect("getauxval(AT_HWCAP) has FAST_MULT (bit 4)",
               std::to_string(hwcap & abr::kHwcapFastMult),
               std::to_string(abr::kHwcapFastMult));
        Expect("getauxval(AT_HWCAP) has VFP (bit 6)",
               std::to_string(hwcap & abr::kHwcapVfp),
               std::to_string(abr::kHwcapVfp));
        Expect("getauxval(AT_HWCAP) has TLS (bit 15)",
               std::to_string(hwcap & abr::kHwcapTls),
               std::to_string(abr::kHwcapTls));
        Expect("getauxval(AT_HWCAP) excludes FPA",
               std::to_string(hwcap & abr::kHwcapFpa), "0");
        Expect("getauxval(AT_HWCAP) excludes LPAE",
               std::to_string(hwcap & abr::kHwcapLpae), "0");
        Expect("getauxval(AT_HWCAP) excludes VFPv4",
               std::to_string(hwcap & abr::kHwcapVfpv4), "0");
        const std::uint32_t ps[4] = {kAtPagesz, 0, 0, 0};
        Expect("getauxval(AT_PAGESZ) is the page size",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 19, ps) > 0 ? 1 : 0), "1");
        const std::uint32_t h2[4] = {kAtHwcap2, 0, 0, 0};
        Expect("getauxval(AT_HWCAP2) is zero",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 19, h2)), "0");
    }

    // --- JNI native-entry marshalling: a 64-bit argument takes an
    //     EVEN-numbered core register pair (AAPCS32), or an 8-byte aligned
    //     stack slot. Pinned by the guest's own
    //     Utils.handleReferrerValue(Ljava/lang/String;JJ)V, which reads its
    //     two longs at entry_sp+0 and entry_sp+8; without the pad the guest
    //     would read the HIGH words there (usually zero). Words 0..3 go to
    //     r0..r3, the rest to the stack at (index - 4) * 4.
    {
        std::vector<std::uint32_t> w = {0x10002000u, 0x20003000u, 0x30004000u};
        abr::AppendJniWord64(w, 0x1122334455667788ull);
        abr::AppendJniWord64(w, 0x99AABBCCDDEEFF00ull);
        char got[128];
        std::snprintf(got, sizeof got, "%zu:%08x %08x %08x %08x %08x %08x %08x %08x",
                      w.size(), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        Expect("JNI (String;JJ) word stream", got,
               "8:10002000 20003000 30004000 00000000 55667788 11223344 ddeeff00 99aabbcc");
    }
    {
        // A 64-bit argument that comes first needs no pad: it lands in r2:r3.
        std::vector<std::uint32_t> w = {0x10002000u, 0x20003000u};
        abr::AppendJniWord64(w, 0x1122334455667788ull);
        char got[64];
        std::snprintf(got, sizeof got, "%zu:%08x %08x %08x %08x",
                      w.size(), w[0], w[1], w[2], w[3]);
        Expect("JNI (J)V word stream", got,
               "4:10002000 20003000 55667788 11223344");
    }

    // --- C++ static-local guard: the first acquire constructs (1), a later
    //     one after release sees the object done (0); abort lets the next
    //     acquire construct again.
    {
        const std::uint32_t g = kScratch + 0x800;
        std::memset(mem.host(g), 0, 4);
        const std::uint32_t ga[4] = {g, 0, 0, 0};
        std::string got = std::to_string(abr::RuntimeTestAccess::Call(rt, 72, ga));
        abr::RuntimeTestAccess::Call(rt, 74, ga);  // abort
        got += std::to_string(abr::RuntimeTestAccess::Call(rt, 72, ga));
        abr::RuntimeTestAccess::Call(rt, 73, ga);  // release
        got += std::to_string(abr::RuntimeTestAccess::Call(rt, 72, ga));
        std::uint32_t w = 0;
        std::memcpy(&w, mem.host(g), 4);
        Expect("__cxa_guard acquire/abort/acquire/release/acquire", got + " bit0=" +
               std::to_string(w & 1u), "110 bit0=1");
    }

    // --- small libc: ctype masks as bionic's (isxdigit = _N|_X), strsep
    //     cuts at the first delimiter, strxfrm = strlcpy in the C locale,
    //     div returns through the hidden result pointer in r0.
    {
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0,
                        std::uint32_t z = 0) {
            const std::uint32_t a[4] = {x, y, z, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, a);
        };
        std::string got;
        for (int c : {int('a'), int('G'), int('5'), int(' '), int('\t'), -1}) {
            got += call(75, static_cast<std::uint32_t>(c)) ? 'x' : '-';
            got += call(76, static_cast<std::uint32_t>(c)) ? 's' : '-';
        }
        Expect("isxdigit/isspace on a G 5 space tab EOF", got, "x---x--s-s--");
        const std::uint32_t str = kScratch + 0x900, sp = kScratch + 0x940, dl = kScratch + 0x950;
        put(str, "ab,cd");
        put(dl, ",;");
        std::memcpy(mem.host(sp), &str, 4);
        const std::uint32_t t1 = call(77, sp, dl);
        std::uint32_t rest = 0;
        std::memcpy(&rest, mem.host(sp), 4);
        const std::uint32_t t2 = call(77, sp, dl);
        std::uint32_t end = 1;
        std::memcpy(&end, mem.host(sp), 4);
        Expect("strsep tokens", std::string(rt.H<const char>(t1)) + "|" +
               rt.H<const char>(t2) + "|" + std::to_string(rest - str) + "|" + std::to_string(end),
               "ab|cd|3|0");
        const std::uint32_t dst = kScratch + 0x960;
        const std::uint32_t xl = call(78, dst, dl, 2);
        Expect("strxfrm(n=2) copies 1 + NUL, returns strlen",
               std::to_string(xl) + rt.H<const char>(dst), "2,");
        const std::uint32_t q = kScratch + 0x980;
        call(79, q, static_cast<std::uint32_t>(-7), 2);
        std::int32_t qr[2] = {0, 0};
        std::memcpy(qr, mem.host(q), 8);
        Expect("div(-7, 2)", std::to_string(qr[0]) + " " + std::to_string(qr[1]), "-3 -1");
    }

    // --- user database in the guest's LP32 structs: passwd uid at +8 and a
    //     name string at +0; group gid at +8, gr_mem NULL-terminated at +12.
    {
        const std::uint32_t none[4] = {0, 0, 0, 0};
        const std::uint32_t uid = abr::RuntimeTestAccess::Call(rt, 80, none);
        const std::uint32_t ua[4] = {uid, 0, 0, 0};
        const std::uint32_t pw = abr::RuntimeTestAccess::Call(rt, 81, ua);
        std::uint32_t w[4] = {0, 0, 0, 0};
        if (pw) {
            std::memcpy(w, mem.host(pw), 12);
        }
        Expect("getpwuid(getuid()): uid, name", pw && w[2] == uid && *rt.H<const char>(w[0]) ? "1" : "0",
               "1");
        const std::uint32_t ga[4] = {static_cast<std::uint32_t>(::getgid()), 0, 0, 0};
        const std::uint32_t gr = abr::RuntimeTestAccess::Call(rt, 82, ga);
        bool term = false;
        if (gr) {
            std::memcpy(w, mem.host(gr), 16);
            for (std::uint32_t i = 0; i < 64 && !term; i++) {
                std::uint32_t m = 1;
                std::memcpy(&m, mem.host(w[3] + i * 4), 4);
                term = (m == 0);
            }
        }
        Expect("getgrgid(getgid()): gid, gr_mem ends", gr && w[2] == ga[0] && term ? "1" : "0", "1");
        const std::uint32_t fifo[4] = {1, 0, 0, 0};
        Expect("sched_get_priority_max(SCHED_FIFO)",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 83, fifo)),
               "99");
    }

    // --- guest environment: setenv/getenv/unsetenv share one table and the
    //     environ array follows it ("K=V" strings, NULL-terminated).
    {
        const std::uint32_t kVar = kScratch + 0xA00, kK = kScratch + 0xA10, kV1 = kScratch + 0xA20,
                            kV2 = kScratch + 0xA30;
        std::memset(mem.host(kVar), 0, 4);
        rt.environ_var_ = kVar;
        put(kK, "ABR_K");
        put(kV1, "one");
        put(kV2, "two");
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0, std::uint32_t z = 0) {
            const std::uint32_t a[4] = {x, y, z, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, a);
        };
        auto env0 = [&]() -> std::string {
            std::uint32_t arr = 0, e0 = 0;
            std::memcpy(&arr, mem.host(kVar), 4);
            if (!arr) {
                return "noarr";
            }
            std::memcpy(&e0, mem.host(arr), 4);
            return e0 ? rt.H<const char>(e0) : "NULL";
        };
        std::string got = std::to_string(call(87, kK)) + " ";
        call(88, kK, kV1, 1);
        call(88, kK, kV2, 0);  // no overwrite
        const std::uint32_t v = call(87, kK);
        got += (v ? rt.H<const char>(v) : "NULL") + std::string(" ") + env0() + " ";
        call(89, kK);
        got += std::to_string(call(87, kK)) + " " + env0();
        Expect("getenv/setenv/unsetenv/environ", got, "0 one ABR_K=one 0 NULL");
        rt.environ_var_ = 0;
    }

    // --- SHA1 over a guest SHA1_CTX (FIPS 180 "abc" vector), crc32, and a
    //     guest z_stream inflating what host zlib deflated: output, then
    //     next_in/avail_in/next_out/avail_out/total_out as zlib leaves them.
    {
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0, std::uint32_t z = 0) {
            const std::uint32_t a[4] = {x, y, z, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, a);
        };
        const std::uint32_t ctx = kScratch + 0xB00, msg = kScratch + 0xB80, dg = kScratch + 0xBA0;
        put(msg, "abc");
        call(90, ctx);
        call(91, ctx, msg, 3);
        call(92, dg, ctx);
        std::string hex;
        for (int i = 0; i < 20; i++) {
            char b[3];
            std::snprintf(b, sizeof b, "%02x", *rt.H<std::uint8_t>(dg + static_cast<std::uint32_t>(i)));
            hex += b;
        }
        Expect("SHA1(abc)", hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
        char crc[12];
        std::snprintf(crc, sizeof crc, "%08x", call(93, 0, msg, 3));
        Expect("crc32(abc)", crc, "352441c2");
        const std::string plain(3000, 'z');
        std::vector<Bytef> packed(compressBound(static_cast<uLong>(plain.size())));
        uLongf plen = static_cast<uLongf>(packed.size());
        ::compress(packed.data(), &plen, reinterpret_cast<const Bytef*>(plain.data()),
                   static_cast<uLong>(plain.size()));
        const std::uint32_t zs = kScratch + 0xC00, zin = 0x10008000, zout = 0x10009000,
                            zver = kScratch + 0xC80;
        std::memcpy(mem.host(zin), packed.data(), plen);
        put(zver, ZLIB_VERSION);
        std::memset(mem.host(zs), 0, 56);
        const std::uint32_t r_init = call(94, zs, zver, 56);
        std::uint32_t f[14];
        std::memcpy(f, mem.host(zs), 56);
        f[0] = zin;
        f[1] = static_cast<std::uint32_t>(plen);
        f[3] = zout;
        f[4] = 4096;
        std::memcpy(mem.host(zs), f, 56);
        const std::uint32_t r_inf = call(95, zs, Z_FINISH);
        std::memcpy(f, mem.host(zs), 56);
        const bool same = std::memcmp(mem.host(zout), plain.data(), plain.size()) == 0;
        const std::uint32_t r_end = call(96, zs);
        char got[160];
        std::snprintf(got, sizeof got, "%u %u %u same=%d in+%u avail_in=%u out+%u avail_out=%u total=%u",
                      r_init, r_inf, r_end, same ? 1 : 0, f[0] - zin, f[1], f[3] - zout, f[4], f[5]);
        char want[160];
        std::snprintf(want, sizeof want, "0 1 0 same=1 in+%u avail_in=0 out+3000 avail_out=1096 total=3000",
                      static_cast<unsigned>(plen));
        Expect("guest z_stream inflate", got, want);
    }

    // --- name resolution offline (bionic EAI numbers; a numeric host
    //     resolves into the guest's LP32 addrinfo), inet_aton, and mremap
    //     growing a mapping by moving it with its contents.
    {
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0, std::uint32_t z = 0,
                        std::uint32_t w = 0) {
            const std::uint32_t a[4] = {x, y, z, w};
            return abr::RuntimeTestAccess::Call(rt, idx, a);
        };
        const std::uint32_t kN = kScratch + 0xD00, kSv = kScratch + 0xD40, kRes = kScratch + 0xD50,
                            kIa = kScratch + 0xD60;
        put(kN, "example.com");
        std::string got = std::to_string(call(100, kN, 0, 0, kRes)) + " ";
        put(kN, "127.0.0.1");
        put(kSv, "80");
        const std::uint32_t r = call(100, kN, kSv, 0, kRes);
        std::uint32_t ai = 0;
        std::memcpy(&ai, mem.host(kRes), 4);
        std::uint32_t f[8] = {0};
        if (r == 0 && ai) {
            std::memcpy(f, mem.host(ai), 32);
        }
        const std::uint8_t* sa = f[6] ? mem.host(f[6]) : nullptr;
        got += std::to_string(r) + " fam=" + std::to_string(f[1]) + " len=" + std::to_string(f[4]) +
               " port=" + std::to_string(sa ? (sa[2] << 8 | sa[3]) : 0) + " ip0=" +
               std::to_string(sa ? sa[4] : 0);
        if (ai) {
            call(101, ai);
        }
        got += std::string(" '") + rt.H<const char>(call(102, 7)) + "'";
        put(kN, "10.1.2.3");
        got += " aton=" + std::to_string(call(103, kN, kIa));
        char ip[16];
        std::snprintf(ip, sizeof ip, ":%u.%u.%u.%u", *mem.host(kIa), *mem.host(kIa + 1),
                      *mem.host(kIa + 2), *mem.host(kIa + 3));
        got += ip;
        Expect("getaddrinfo offline / numeric, gai_strerror, inet_aton", got,
               "7 0 fam=2 len=16 port=80 ip0=127 'No address associated with hostname' aton=1:10.1.2.3");
        abr::RuntimeTestAccess::RegsView in{};
        in[13] = kScratch + 0xE00;  // stack: fd -1, offset 0
        const std::uint32_t stk[2] = {0xFFFFFFFFu, 0};
        std::memcpy(mem.host(in[13]), stk, 8);
        auto mmap_at = [&](std::uint32_t at, std::uint32_t len, int prot, int fixed) {
            const std::uint32_t margs[4] = {at, len, static_cast<std::uint32_t>(prot),
                                            static_cast<std::uint32_t>(MAP_PRIVATE | MAP_ANONYMOUS | fixed)};
            return abr::RuntimeTestAccess::Call(rt, 104, margs, in, nullptr);
        };
        const std::uint32_t m = mmap_at(0, 4096, PROT_READ | PROT_WRITE, 0);
        std::string mr = "mmap-failed";
        if (m != 0xFFFFFFFFu) {
            std::memcpy(mem.host(m), "kept", 5);
            mmap_at(m + 4096, 4096, PROT_READ, MAP_FIXED);  // the next page is taken
            const std::int32_t stay = static_cast<std::int32_t>(call(105, m, 4096, 3 * 4096, 0));
            const std::uint32_t moved = call(105, m, 4096, 3 * 4096, 1);
            mr = std::to_string(stay) + " " + (moved != m && moved != 0xFFFFFFFFu ? rt.H<const char>(moved) : "fail") +
                 " old=" + (abr::RuntimeTestAccess::InMapping(rt, m, 1) ? "mapped" : "gone");
        }
        Expect("mremap: in place refused when taken, moves with MAYMOVE", mr, "-1 kept old=gone");
        // Boehm's GC_unmap shape: PROT_NONE MAP_FIXED over the middle page of
        // a three-page mapping, then a partial munmap of the first page.
        const std::uint32_t t = mmap_at(0, 3 * 4096, PROT_READ | PROT_WRITE, 0);
        std::string pm = "mmap-failed";
        if (t != 0xFFFFFFFFu) {
            const std::uint32_t r1 = mmap_at(t + 4096, 4096, PROT_NONE, MAP_FIXED);
            auto rd = [&](std::uint32_t a) { return abr::RuntimeTestAccess::InMapping(rt, a, 1) ? 'r' : '-'; };
            pm = std::string(r1 == t + 4096 ? "fixed " : "fixed-failed ") + rd(t) + rd(t + 4096) + rd(t + 8192);
            const std::uint32_t ua[4] = {t, 4096, 0, 0};
            const std::uint32_t r2 = abr::RuntimeTestAccess::Call(rt, 53, ua);
            pm += " " + std::to_string(r2) + " " + rd(t) + rd(t + 8192);
        }
        Expect("MAP_FIXED over part of a mapping, partial munmap", pm, "fixed r-r 0 -r");
    }

    // --- JNI argument words from a signature (AppendJniArgs). Both read
    //     modes give the same stream: narrow types extended as AAPCS32 wants,
    //     J/D paired and aligned, F as bits, references through the wrapper.
    //     Raw mode is how the RegisterNatives stubs read a fixed-argument call
    //     from ART; it holds on AArch64 only, so it is tested there (run
    //     fmt_test on the sandbox). Enough arguments to spill past x7 / v7.
    {
        // Words 0, 1 = env, thiz (they set the parity of the J/D pairs).
        const char* want =
            "24:000000e0 00000070 00000007 3fc00000 55667788 11223344 00000001 000000a1 "
            "9999999a 3ff19999 c0200000 ffffff80 ffff8000 0000ffff fffffffd 00000000 "
            "41000000 41100000 41200000 41300000 41400000 41500000 41600000 41700000";
        JniArgsProbe::sig = "(IFJZLjava/lang/String;DFBSCI[IFFFFFFFF)V";
        JniArgsProbe::words = {0xE0u, 0x70u};
        JniArgsProbe::Promoted(nullptr, nullptr, 7, 1.5f, 0x1122334455667788ll,
                               (unsigned char)1, (void*)0xA1, 1.1, -2.5f, (signed char)-128,
                               (short)-32768, (unsigned short)0xFFFF, -3, (void*)0, 8.0f, 9.0f,
                               10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f);
        Expect("JNI args (promoted varargs)", JniArgsProbe::Shown(), want);
#if defined(__aarch64__) && !defined(__APPLE__)
        using Fixed = void (*)(void*, void*, int, float, long long, unsigned char, void*, double,
                               float, signed char, short, unsigned short, int, void*, float, float,
                               float, float, float, float, float, float);
        JniArgsProbe::words = {0xE0u, 0x70u};
        reinterpret_cast<Fixed>(&JniArgsProbe::Raw)(
            nullptr, nullptr, 7, 1.5f, 0x1122334455667788ll, 1, (void*)0xA1, 1.1, -2.5f, -128,
            -32768, 0xFFFF, -3, nullptr, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f);
        Expect("JNI args (fixed call read raw, AAPCS64)", JniArgsProbe::Shown(), want);
#endif
    }

    // --- file bridge: fopen/fclose must free the 96-byte guest FILE, erase
    //     the fd mapping, and recycle the guest fd. A loop that reuses one
    //     slot keeps fd_map_/file_map_/next_guest_fd_ flat.
    {
        const std::uint32_t kFdPath = 0x10006000;
        const std::uint32_t kFdMode = 0x10006100;
        const std::string host_dir = TestTmp() + "/opencode/abr-fdtest";
        const std::string host_file = host_dir + "/fd-loop.txt";
        std::filesystem::create_directories(host_dir);
        std::filesystem::remove(host_file);
        rt.SetFilesDir("/data/data/abr.test/files", host_dir);
        put(kFdPath, "/data/data/abr.test/files/fd-loop.txt");
        put(kFdMode, "w+");
        const std::uint32_t open_args[4] = {kFdPath, kFdMode, 0, 0};

        const std::size_t fd_before = abr::RuntimeTestAccess::FdMapSize(rt);
        const std::size_t file_before = abr::RuntimeTestAccess::FileMapSize(rt);
        const std::uint32_t h = abr::RuntimeTestAccess::Call(rt, 21, open_args);
        Expect("fopen returns a FILE*", h ? "1" : "0", "1");
        if (h) {
            const std::uint32_t cargs[4] = {h, 0, 0, 0};
            const std::uint32_t cr = abr::RuntimeTestAccess::Call(rt, 22, cargs);
            Expect("fclose returns 0", std::to_string(static_cast<std::int32_t>(cr)), "0");
            Expect("guest FILE freed", std::to_string(abr::RuntimeTestAccess::GuestLiveSize(rt, h)),
                   "0");
        }
        const std::size_t fd_after_one = abr::RuntimeTestAccess::FdMapSize(rt);
        const std::size_t file_after_one = abr::RuntimeTestAccess::FileMapSize(rt);
        const std::uint32_t next_after_one = abr::RuntimeTestAccess::NextGuestFd(rt);
        Expect("fd_map_ restored after fclose", std::to_string(fd_after_one),
               std::to_string(fd_before));
        Expect("file_map_ restored after fclose", std::to_string(file_after_one),
               std::to_string(file_before));

        bool loop_ok = true;
        for (int i = 0; i < 64; i++) {
            const std::uint32_t lh = abr::RuntimeTestAccess::Call(rt, 21, open_args);
            if (!lh) {
                loop_ok = false;
                break;
            }
            const std::uint32_t cargs[4] = {lh, 0, 0, 0};
            const std::uint32_t cr = abr::RuntimeTestAccess::Call(rt, 22, cargs);
            if (cr != 0) {
                loop_ok = false;
                break;
            }
        }
        Expect("fopen/fclose loop succeeds", loop_ok ? "1" : "0", "1");
        Expect("fd_map_ flat after loop", std::to_string(abr::RuntimeTestAccess::FdMapSize(rt)),
               std::to_string(fd_after_one));
        Expect("file_map_ flat after loop", std::to_string(abr::RuntimeTestAccess::FileMapSize(rt)),
               std::to_string(file_after_one));
        Expect("guest fd number reused", std::to_string(abr::RuntimeTestAccess::NextGuestFd(rt)),
               std::to_string(next_after_one));
        Expect("one reusable guest fd", std::to_string(abr::RuntimeTestAccess::FreeGuestFdCount(rt)),
               "1");
        std::filesystem::remove(host_file);
    }

    // --- localtime/gmtime: the non-_r form owns one per-thread guest
    //     buffer. Repeated calls must return the same pointer and fill the
    //     Bionic ARM struct tm layout.
    {
        const std::uint32_t kTime = 0x10006200;
        const std::int32_t zero = 0;
        std::memcpy(rt.H(kTime), &zero, 4);
        const std::uint32_t args[4] = {kTime, 0, 0, 0};
        const std::uint32_t p1 = abr::RuntimeTestAccess::Call(rt, 23, args);
        const std::uint32_t p2 = abr::RuntimeTestAccess::Call(rt, 23, args);
        Expect("gmtime returns a static buffer", p1 ? "1" : "0", "1");
        Expect("gmtime reuses the same pointer", std::to_string(p2), std::to_string(p1));
        if (p1) {
            std::int32_t f[9] = {};
            for (int i = 0; i < 9; i++) {
                std::memcpy(&f[i], rt.H(p1 + (std::uint32_t)(i * 4)), 4);
            }
            std::int32_t off = 0;
            std::uint32_t zone = 1;
            std::memcpy(&off, rt.H(p1 + 36), 4);
            std::memcpy(&zone, rt.H(p1 + 40), 4);
            Expect("gmtime epoch sec/min/hour", std::to_string(f[0]) + "/" + std::to_string(f[1]) +
                                                    "/" + std::to_string(f[2]),
                   "0/0/0");
            Expect("gmtime epoch date", std::to_string(f[3]) + "/" + std::to_string(f[4]) + "/" +
                                             std::to_string(f[5]),
                   "1/0/70");
            Expect("gmtime epoch weekday/yday", std::to_string(f[6]) + "/" + std::to_string(f[7]),
                   "4/0");
            Expect("gmtime gmtoff/zone", std::to_string(off) + "/" + std::to_string(zone), "0/0");
        }
    }

    // --- host thread exit: ReapHostThread drops the exiting tid's errno and
    //     TLS state (HostThreadRelease calls it before freeing the stack/JIT).
    {
        const std::uint32_t fake_tid = 4242;
        const std::uint32_t key = 7;
        const std::uint32_t slot = 0xF1000300u;
        const std::size_t free_before = abr::RuntimeTestAccess::FreeErrnoCount(rt);
        abr::RuntimeTestAccess::SeedHostThreadTls(rt, fake_tid, slot, key, 0x12345678u);
        Expect("host thread state seeded",
               (abr::RuntimeTestAccess::HostThreadHasErrno(rt, fake_tid) &&
                abr::RuntimeTestAccess::HostThreadHasTls(rt, fake_tid, key))
                   ? "1"
                   : "0",
               "1");
        rt.ReapHostThread(fake_tid);
        Expect("host thread errno reaped",
               abr::RuntimeTestAccess::HostThreadHasErrno(rt, fake_tid) ? "0" : "1", "1");
        Expect("host thread tls reaped",
               abr::RuntimeTestAccess::HostThreadHasTls(rt, fake_tid, key) ? "0" : "1", "1");
        Expect("host thread errno slot recycled",
               std::to_string(abr::RuntimeTestAccess::FreeErrnoCount(rt)),
               std::to_string(free_before + 1));
    }

    // --- AAssetManager_open failure: a bad deflate entry must free the
    //     guest buffer it allocated before reporting NULL.
    {
        const std::string bad_zip = TestTmp() + "/opencode/abr-asset-bad.zip";
        const std::vector<std::uint8_t> zip = MakeBadAssetZip();
        FILE* zf = std::fopen(bad_zip.c_str(), "wb");
        Expect("bad asset zip written", zf ? "1" : "0", "1");
        if (zf) {
            std::fwrite(zip.data(), 1, zip.size(), zf);
            std::fclose(zf);
            rt.SetApkPath(bad_zip);
            const std::uint32_t mgr_args[4] = {0, 0, 0, 0};
            const std::uint32_t mgr = abr::RuntimeTestAccess::Call(rt, 24, mgr_args);
            Expect("bad asset manager indexed", mgr ? "1" : "0", "1");
            const std::uint32_t kAssetPath = 0x10006300;
            put(kAssetPath, "assets/bad.bin");
            const std::uint32_t open_args[4] = {mgr, kAssetPath, 0, 0};
            const std::size_t live_before = abr::RuntimeTestAccess::LiveCount(rt);
            const std::uint32_t asset = abr::RuntimeTestAccess::Call(rt, 25, open_args);
            Expect("bad asset open returns NULL", asset ? "0" : "1", "1");
            Expect("bad asset not registered", std::to_string(abr::RuntimeTestAccess::AssetMapSize(rt)),
                   "0");
            Expect("bad asset guest buffer freed",
                   std::to_string(abr::RuntimeTestAccess::LiveCount(rt)),
                   std::to_string(live_before));
            std::filesystem::remove(bad_zip);
        }
    }

    // --- GetStringUTFChars: a mock jstring returns its own handle and
    //     ReleaseStringUTFChars must not remove it from jni_strings_. A
    //     tracked real-wrapped guest copy is freed and erased on Release.
    {
        const std::size_t before = abr::RuntimeTestAccess::JniStringCount(rt);
        const std::uint32_t js = rt.JniNewString("mock-utf");
        const std::uint32_t get_args[4] = {0, js, 0, 0};
        const std::uint32_t chars = abr::RuntimeTestAccess::Jni(rt, 169, get_args);
        Expect("mock GetStringUTFChars returns handle",
               chars == js ? "1" : "0", "1");
        Expect("mock jstring registered", std::to_string(abr::RuntimeTestAccess::JniStringCount(rt)),
               std::to_string(before + 1));
        const std::uint32_t rel_args[4] = {0, js, chars, 0};
        abr::RuntimeTestAccess::Jni(rt, 170, rel_args);
        Expect("mock Release keeps registry",
               std::to_string(abr::RuntimeTestAccess::JniStringCount(rt)),
               std::to_string(before + 1));

        const std::uint32_t copy = rt.GuestMalloc(8);
        std::memcpy(rt.H(copy), "copy", 5);
        abr::RuntimeTestAccess::TrackUtfChars(rt, copy);
        Expect("tracked UTF copy present", std::to_string(abr::RuntimeTestAccess::UtfCharCount(rt)),
               "1");
        const std::uint32_t rel_copy[4] = {0, 0, copy, 0};
        abr::RuntimeTestAccess::Jni(rt, 170, rel_copy);
        Expect("tracked UTF copy freed",
               std::to_string(abr::RuntimeTestAccess::GuestLiveSize(rt, copy)), "0");
        Expect("tracked UTF map cleared", std::to_string(abr::RuntimeTestAccess::UtfCharCount(rt)),
               "0");
    }

    // --- readdir_r: bionic arm32 dirent (name @19, type @18, d_reclen
    //     bytes copied, nothing past the record).
    {
        const std::string host_dir = TestTmp() + "/opencode/abr-dirtest";
        std::filesystem::remove_all(host_dir);
        std::filesystem::create_directories(host_dir);
        std::FILE* f = std::fopen((host_dir + "/abc").c_str(), "w");
        if (f) {
            std::fclose(f);
        }
        rt.SetFilesDir("/data/data/abr.dir/files", host_dir);
        const std::uint32_t kPath = 0x10007000, kEnt = 0x10007100, kRes = 0x10007300;
        put(kPath, "/data/data/abr.dir/files");
        const std::uint32_t oargs[4] = {kPath, 0, 0, 0};
        const std::uint32_t d = abr::RuntimeTestAccess::Call(rt, 26, oargs);
        Expect("opendir returns a DIR*", d ? "1" : "0", "1");
        bool found = false, clean_tail = true, type_ok = false, reclen_ok = false;
        for (int i = 0; i < 8 && d; i++) {
            std::memset(rt.H<std::uint8_t>(kEnt), 0xAB, 0x200);
            const std::uint32_t rargs[4] = {d, kEnt, kRes, 0};
            const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, 27, rargs);
            std::uint32_t res = 0;
            std::memcpy(&res, rt.H(kRes), 4);
            if (r != 0 || res == 0) {
                Expect("readdir_r end returns 0 and NULL", std::to_string(r) + "/" + std::to_string(res),
                       "0/0");
                break;
            }
            const std::uint8_t* e = rt.H<std::uint8_t>(kEnt);
            std::uint16_t rl = 0;
            std::memcpy(&rl, e + 16, 2);
            for (std::uint32_t k = rl; k < 0x200; k++) {
                if (e[k] != 0xAB) {
                    clean_tail = false;
                }
            }
            if (std::strcmp(reinterpret_cast<const char*>(e + 19), "abc") == 0) {
                found = true;
                type_ok = (e[18] == DT_REG);
                reclen_ok = (rl == 24);
            }
        }
        Expect("readdir_r name at +19", found ? "1" : "0", "1");
        Expect("readdir_r d_type at +18", type_ok ? "1" : "0", "1");
        Expect("readdir_r d_reclen = align8(19+len+1)", reclen_ok ? "1" : "0", "1");
        Expect("readdir_r writes nothing past d_reclen", clean_tail ? "1" : "0", "1");
        if (d) {
            const std::uint32_t cargs[4] = {d, 0, 0, 0};
            abr::RuntimeTestAccess::Call(rt, 28, cargs);
        }
    }

    // --- chdir moves the guest's current directory (getcwd, relative
    //     names follow it); readdir returns the DIR's own entry; ".." back
    //     to the files dir restores the default; a missing dir fails.
    {
        const std::string host_dir = TestTmp() + "/opencode/abr-dirtest";
        std::filesystem::create_directories(host_dir + "/sub");
        if (std::FILE* f = std::fopen((host_dir + "/sub/xyz").c_str(), "w")) {
            std::fclose(f);
        }
        rt.SetFilesDir("/data/data/abr.dir/files", host_dir);
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0) {
            const std::uint32_t a[4] = {x, y, 0, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, a);
        };
        const std::uint32_t kP = 0x10007400, kBuf = 0x10007500;
        std::string got;
        put(kP, "sub");
        got += std::to_string(call(85, kP)) + " ";
        got += call(86, kBuf, 256) ? rt.H<const char>(kBuf) : "NULL";
        put(kP, ".");
        const std::uint32_t d = call(26, kP);
        bool found = false;
        for (int i = 0; i < 8 && d; i++) {
            const std::uint32_t e = call(84, d);
            if (!e) {
                break;
            }
            found = found || std::strcmp(rt.H<const char>(e + 19), "xyz") == 0;
        }
        if (d) {
            call(28, d);
        }
        got += found ? " xyz" : " -";
        put(kP, "..");
        got += " " + std::to_string(call(85, kP)) + " ";
        got += call(86, kBuf, 256) ? rt.H<const char>(kBuf) : "NULL";
        put(kP, "nope");
        got += " " + std::to_string(static_cast<std::int32_t>(call(85, kP)));
        Expect("chdir/getcwd/readdir", got,
               "0 /data/data/abr.dir/files/sub xyz 0 /data/data/abr.dir/files -1");
    }

    // --- UTF-8 as bionic 7: mbrtowc whole and split across calls (the
    //     mbstate keeps the first byte: -2, then 1), wcrtomb of U+20AC; and
    //     fscanf leaves the stream right after what it consumed.
    {
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0, std::uint32_t z = 0,
                        std::uint32_t w = 0) {
            const std::uint32_t a[4] = {x, y, z, w};
            return abr::RuntimeTestAccess::Call(rt, idx, a);
        };
        const std::uint32_t kS = 0x10007600, kW = 0x10007610, kSt = 0x10007620, kB = 0x10007630;
        std::memcpy(mem.host(kS), "\xc3\xa9", 3);
        std::memset(mem.host(kSt), 0, 4);
        std::string got = std::to_string(call(97, kW, kS, 2, kSt)) + ":";
        std::uint32_t wc = 0;
        std::memcpy(&wc, mem.host(kW), 4);
        got += std::to_string(wc) + " ";
        got += std::to_string(static_cast<std::int32_t>(call(97, kW, kS, 1, kSt))) + " ";
        got += std::to_string(call(97, kW, kS + 1, 1, kSt)) + ":";
        std::memcpy(&wc, mem.host(kW), 4);
        got += std::to_string(wc) + " ";
        const std::uint32_t k = call(98, kB, 0x20AC, 0);
        char hx[16];
        std::snprintf(hx, sizeof hx, "%u:%02x%02x%02x", k, *mem.host(kB), *mem.host(kB + 1),
                      *mem.host(kB + 2));
        got += hx;
        Expect("mbrtowc/wcrtomb UTF-8", got, "2:233 -2 1:233 3:e282ac");

        const std::string host_dir = TestTmp() + "/opencode/abr-dirtest";
        if (std::FILE* f = std::fopen((host_dir + "/scan").c_str(), "w")) {
            std::fputs("12 abc\n34", f);
            std::fclose(f);
        }
        rt.SetFilesDir("/data/data/abr.dir/files", host_dir);
        const std::uint32_t kPath = 0x10007700, kMode = 0x10007740, kFmt1 = 0x10007750,
                            kFmt2 = 0x10007760, kI = 0x10007770, kStr = 0x10007780;
        put(kPath, "/data/data/abr.dir/files/scan");
        put(kMode, "r");
        put(kFmt1, "%d %s");
        put(kFmt2, "%d");
        const std::uint32_t fp = call(21, kPath, kMode);
        const std::uint32_t r1 = call(99, fp, kFmt1, kI, kStr);
        std::int32_t i1 = 0;
        std::memcpy(&i1, mem.host(kI), 4);
        const std::string s1 = rt.H<const char>(kStr);
        const std::uint32_t pos = call(31, fp);
        const std::uint32_t r2 = call(99, fp, kFmt2, kI);
        std::int32_t i2 = 0;
        std::memcpy(&i2, mem.host(kI), 4);
        const std::int32_t r3 = static_cast<std::int32_t>(call(99, fp, kFmt2, kI));
        call(22, fp);
        Expect("fscanf twice, then EOF",
               std::to_string(r1) + " " + std::to_string(i1) + " " + s1 + " @" + std::to_string(pos) +
                   " " + std::to_string(r2) + " " + std::to_string(i2) + " " + std::to_string(r3),
               "2 12 abc @6 1 34 -1");
    }

    // --- stat: bionic arm32 struct stat is 104 bytes, st_size at +48.
    {
        const std::string host_dir = TestTmp() + "/opencode/abr-stattest";
        std::filesystem::create_directories(host_dir);
        {
            std::FILE* f = std::fopen((host_dir + "/sz").c_str(), "wb");
            const std::string blob(12345, 'x');
            if (f) {
                std::fwrite(blob.data(), 1, blob.size(), f);
                std::fclose(f);
            }
        }
        rt.SetFilesDir("/data/data/abr.stat/files", host_dir);
        const std::uint32_t kPath = 0x10007400, kBuf = 0x10007500;
        put(kPath, "/data/data/abr.stat/files/sz");
        std::memset(rt.H<std::uint8_t>(kBuf), 0xAB, 0x100);
        const std::uint32_t sargs[4] = {kPath, kBuf, 0, 0};
        const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, 29, sargs);
        const std::uint8_t* b = rt.H<std::uint8_t>(kBuf);
        std::uint64_t size = 0;
        std::uint32_t mode = 0;
        std::memcpy(&size, b + 48, 8);
        std::memcpy(&mode, b + 16, 4);
        bool tail = true;
        for (int k = 104; k < 0x100; k++) {
            if (b[k] != 0xAB) {
                tail = false;
            }
        }
        Expect("stat returns 0", std::to_string(r), "0");
        Expect("stat st_size at +48", std::to_string(size), "12345");
        Expect("stat st_mode is a regular file", S_ISREG(mode) ? "1" : "0", "1");
        Expect("stat writes exactly 104 bytes", tail ? "1" : "0", "1");
        // The app's external dir (Unity's persistentDataPath with the storage
        // permission) lands in the app's data dir; other apps' dirs do not.
        const std::string ext = std::filesystem::path(host_dir).parent_path().string() + "/ext";
        Expect("external app dir maps to <data>/ext",
               rt.MapGuestPathString("/storage/emulated/0/Android/data/abr.stat/files/s"),
               ext + "/files/s");
        Expect("external app dir exists",
               std::filesystem::is_directory(ext) ? "1" : "0", "1");
        Expect("another app's external dir stays unmapped",
               rt.MapGuestPathString("/storage/emulated/0/Android/data/abr.statx/files"), "");
    }

    // --- fseek: the guest long is signed 32-bit; a negative offset must
    //     move backwards (it used to be zero-extended to ~+4 GB).
    {
        const std::string host_dir = TestTmp() + "/opencode/abr-seektest";
        std::filesystem::create_directories(host_dir);
        rt.SetFilesDir("/data/data/abr.seek/files", host_dir);
        const std::uint32_t kPath = 0x10007600, kMode = 0x10007700, kData = 0x10007800;
        put(kPath, "/data/data/abr.seek/files/s.bin");
        put(kMode, "w+");
        put(kData, "0123456789");
        const std::uint32_t oargs[4] = {kPath, kMode, 0, 0};
        const std::uint32_t f = abr::RuntimeTestAccess::Call(rt, 21, oargs);
        Expect("seek test fopen", f ? "1" : "0", "1");
        if (f) {
            const std::uint32_t wargs[4] = {kData, 1, 10, f};
            Expect("fwrite 10 bytes", std::to_string(abr::RuntimeTestAccess::Call(rt, 32, wargs)), "10");
            const std::uint32_t sargs[4] = {f, static_cast<std::uint32_t>(-3), SEEK_END, 0};
            Expect("fseek(-3, SEEK_END) returns 0",
                   std::to_string(abr::RuntimeTestAccess::Call(rt, 30, sargs)), "0");
            const std::uint32_t targs[4] = {f, 0, 0, 0};
            Expect("ftell after fseek(-3, SEEK_END)",
                   std::to_string(abr::RuntimeTestAccess::Call(rt, 31, targs)), "7");
            const std::uint32_t cargs[4] = {f, 0, 0, 0};
            abr::RuntimeTestAccess::Call(rt, 22, cargs);
        }
    }

    // --- setsockopt passes optlen BY VALUE at [sp]; getsockopt passes a
    //     pointer there. SO_RCVTIMEO's struct timeval is 8 bytes on the guest.
    {
        const std::uint32_t sargs[4] = {AF_INET, SOCK_DGRAM, 0, 0};
        const std::uint32_t fd = abr::RuntimeTestAccess::Call(rt, 33, sargs);
        Expect("socket() returns a guest fd", static_cast<std::int32_t>(fd) >= 0 ? "1" : "0", "1");
        const std::uint32_t kVal = 0x10007900, kLen = 0x10007910, kSp = 0x10007A00;
        abr::RuntimeTestAccess::RegsView in{};
        in[13] = kSp;
        auto st32 = [&](std::uint32_t a, std::uint32_t v) { std::memcpy(rt.H(a), &v, 4); };
        auto ld32 = [&](std::uint32_t a) {
            std::uint32_t v = 0;
            std::memcpy(&v, rt.H(a), 4);
            return v;
        };
        st32(kVal, 1);
        st32(kSp, 4);  // optlen by value
        const std::uint32_t so[4] = {fd, SOL_SOCKET, SO_REUSEADDR, kVal};
        Expect("setsockopt SO_REUSEADDR (len by value)",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 34, so, in, nullptr)), "0");
        st32(kVal, 0);
        st32(kLen, 4);
        st32(kSp, kLen);  // optlen pointer
        Expect("getsockopt SO_REUSEADDR returns 0",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 35, so, in, nullptr)), "0");
        Expect("getsockopt SO_REUSEADDR value", std::to_string(ld32(kVal) != 0), "1");
        st32(kVal, 2);
        st32(kVal + 4, 0);
        st32(kSp, 8);
        const std::uint32_t tvo[4] = {fd, SOL_SOCKET, SO_RCVTIMEO, kVal};
        Expect("setsockopt SO_RCVTIMEO (8-byte timeval)",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 34, tvo, in, nullptr)), "0");
        st32(kVal, 0);
        st32(kLen, 8);
        st32(kSp, kLen);
        abr::RuntimeTestAccess::Call(rt, 35, tvo, in, nullptr);
        Expect("getsockopt SO_RCVTIMEO sec", std::to_string(ld32(kVal)), "2");
        Expect("getsockopt SO_RCVTIMEO len", std::to_string(ld32(kLen)), "8");
        const std::uint32_t cargs[4] = {fd, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 36, cargs);
    }

    // --- statfs: reachable (it used to fall into the 0 stub) and fills the
    //     88-byte bionic arm32 struct statfs with 64-bit counts.
    {
        rt.SetFilesDir("/data/data/abr.sfs/files", TestTmp() + "/opencode/abr-stattest");
        const std::uint32_t kPath = 0x10007B00, kBuf = 0x10007C00;
        put(kPath, "/data/data/abr.sfs/files");
        std::memset(rt.H<std::uint8_t>(kBuf), 0xAB, 0x100);
        const std::uint32_t args[4] = {kPath, kBuf, 0, 0};
        const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, 37, args);
        const std::uint8_t* b = rt.H<std::uint8_t>(kBuf);
        std::uint32_t bsize = 0, namelen = 0;
        std::uint64_t blocks = 0;
        std::memcpy(&bsize, b + 4, 4);
        std::memcpy(&blocks, b + 8, 8);
        std::memcpy(&namelen, b + 56, 4);
        bool tail = true;
        for (int k = 88; k < 0x100; k++) {
            if (b[k] != 0xAB) {
                tail = false;
            }
        }
        Expect("statfs returns 0", std::to_string(r), "0");
        Expect("statfs f_bsize > 0", bsize > 0 ? "1" : "0", "1");
        Expect("statfs f_blocks (u64 @8) > 0", blocks > 0 ? "1" : "0", "1");
        // 255 on most filesystems, 256 on some: the field, not one value.
        Expect("statfs f_namelen @56", namelen >= 255 && namelen < 1024 ? "1" : "0", "1");
        Expect("statfs writes exactly 88 bytes", tail ? "1" : "0", "1");
    }

    // --- poll: fd < 0 -> revents 0 and not counted; an fd that is not open
    //     -> POLLNVAL, counted, no wait; a real socket is reported normally.
    {
        const std::uint32_t kP = 0x10007D00;
        auto setpfd = [&](std::uint32_t i, std::int32_t fd, std::int16_t ev) {
            std::memcpy(rt.H(kP + i * 8), &fd, 4);
            std::memcpy(rt.H(kP + i * 8 + 4), &ev, 2);
            const std::int16_t z = 0x5A5A;
            std::memcpy(rt.H(kP + i * 8 + 6), &z, 2);
        };
        auto rev = [&](std::uint32_t i) {
            std::int16_t v = 0;
            std::memcpy(&v, rt.H(kP + i * 8 + 6), 2);
            return static_cast<int>(v);
        };
        setpfd(0, -1, POLLIN);
        const std::uint32_t a_neg[4] = {kP, 1, 0, 0};
        Expect("poll fd<0 returns 0", std::to_string(abr::RuntimeTestAccess::Call(rt, 38, a_neg)), "0");
        Expect("poll fd<0 revents 0", std::to_string(rev(0)), "0");
        setpfd(0, 9999, POLLIN);
        const std::uint32_t a_bad[4] = {kP, 1, static_cast<std::uint32_t>(-1), 0};
        Expect("poll unknown fd returns 1 at once",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 38, a_bad)), "1");
        Expect("poll unknown fd revents POLLNVAL", std::to_string(rev(0)), std::to_string(POLLNVAL));
        const std::uint32_t sargs[4] = {AF_INET, SOCK_DGRAM, 0, 0};
        const std::uint32_t sfd = abr::RuntimeTestAccess::Call(rt, 33, sargs);
        setpfd(0, static_cast<std::int32_t>(sfd), POLLOUT);
        setpfd(1, -1, POLLIN);
        const std::uint32_t a_ok[4] = {kP, 2, 1000, 0};
        Expect("poll writable socket returns 1",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 38, a_ok)), "1");
        Expect("poll socket revents POLLOUT", std::to_string(rev(0) & POLLOUT), std::to_string(POLLOUT));
        Expect("poll ignored entry revents 0", std::to_string(rev(1)), "0");
        const std::uint32_t cargs[4] = {sfd, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 36, cargs);
    }

    // --- timed cond waits honour the deadline's clock: the _monotonic
    //     variant takes a CLOCK_MONOTONIC deadline (it used to be compared
    //     against CLOCK_REALTIME and time out at once), the plain one a
    //     CLOCK_REALTIME deadline.
    {
        const std::uint32_t kM = 0x10007E00, kC = 0x10007E40, kTs = 0x10007E80;
        const std::uint32_t minit[4] = {kM, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 8, minit);
        abr::RuntimeTestAccess::Call(rt, 9, minit);  // lock
        auto wait_ms = [&](std::uint32_t idx, clockid_t clk) {
            struct timespec now {};
            clock_gettime(clk, &now);
            std::int64_t ns = static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec +
                              100000000LL;
            const std::uint32_t sec = static_cast<std::uint32_t>(ns / 1000000000LL);
            const std::uint32_t nsec = static_cast<std::uint32_t>(ns % 1000000000LL);
            std::memcpy(rt.H(kTs), &sec, 4);
            std::memcpy(rt.H(kTs + 4), &nsec, 4);
            const std::uint32_t args[4] = {kC, kM, kTs, 0};
            const auto t0 = std::chrono::steady_clock::now();
            const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, idx, args);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            return std::make_pair(r, static_cast<long long>(ms));
        };
        const auto mono = wait_ms(39, CLOCK_MONOTONIC);
        Expect("timedwait_monotonic returns ETIMEDOUT", std::to_string(mono.first),
               std::to_string(ETIMEDOUT));
        Expect("timedwait_monotonic waits ~100 ms", (mono.second >= 80 && mono.second < 1000) ? "1" : "0",
               "1");
        const auto real = wait_ms(40, CLOCK_REALTIME);
        Expect("timedwait (realtime) returns ETIMEDOUT", std::to_string(real.first),
               std::to_string(ETIMEDOUT));
        Expect("timedwait (realtime) waits ~100 ms", (real.second >= 80 && real.second < 1000) ? "1" : "0",
               "1");
        abr::RuntimeTestAccess::Call(rt, 11, minit);  // unlock
    }

    // --- strtol/strtoul/atoi saturate like a 32-bit libc and set errno.
    {
        const std::uint32_t kStr = 0x10007F00, kEnd = 0x10007F80;
        auto guest_errno = [&]() {
            std::uint32_t e = 0;
            std::memcpy(&e, rt.H(rt.ErrnoSlot()), 4);
            return e;
        };
        auto conv = [&](std::uint32_t idx, const char* txt, std::uint32_t* err) {
            put(kStr, txt);
            const std::uint32_t z = 0;
            std::memcpy(rt.H(rt.ErrnoSlot()), &z, 4);
            const std::uint32_t args[4] = {kStr, kEnd, 10, 0};
            const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, idx, args);
            *err = guest_errno();
            return r;
        };
        std::uint32_t e = 0;
        Expect("strtol(2147483648) = LONG_MAX", std::to_string(conv(41, "2147483648", &e)), "2147483647");
        Expect("strtol overflow errno ERANGE", std::to_string(e), std::to_string(ERANGE));
        Expect("strtol(-2147483649) = LONG_MIN",
               std::to_string(static_cast<std::int32_t>(conv(41, "-2147483649", &e))), "-2147483648");
        Expect("strtol(-5) = -5", std::to_string(static_cast<std::int32_t>(conv(41, "-5", &e))), "-5");
        Expect("strtol(-5) errno 0", std::to_string(e), "0");
        Expect("strtoul(4294967296) = ULONG_MAX", std::to_string(conv(42, "4294967296", &e)), "4294967295");
        Expect("strtoul overflow errno ERANGE", std::to_string(e), std::to_string(ERANGE));
        Expect("strtoul(4294967295) fits", std::to_string(conv(42, "4294967295", &e)), "4294967295");
        Expect("strtoul(4294967295) errno 0", std::to_string(e), "0");
        Expect("strtoul(-1) = ULONG_MAX, no error", std::to_string(conv(42, "-1", &e)) + "/" + std::to_string(e),
               "4294967295/0");
        conv(41, "123abc", &e);
        std::uint32_t endp = 0;
        std::memcpy(&endp, rt.H(kEnd), 4);
        Expect("strtol endptr after digits", std::to_string(endp - kStr), "3");
        Expect("atoi(99999999999) saturates", std::to_string(conv(43, "99999999999", &e)), "2147483647");
    }

    // --- TAB is blank but not printable: no _B bit (only SPACE has it).
    {
        const std::uint32_t t_print[4] = {'\t', 8, 0, 0};
        const std::uint32_t s_print[4] = {' ', 8, 0, 0};
        const std::uint32_t t_blank[4] = {'\t', 3, 0, 0};
        Expect("iswctype(TAB, print) = 0", std::to_string(abr::RuntimeTestAccess::Call(rt, 20, t_print)), "0");
        Expect("iswctype(SPACE, print) = _B",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 20, s_print)), "128");
        Expect("iswctype(TAB, blank) = 1", std::to_string(abr::RuntimeTestAccess::Call(rt, 20, t_blank)), "1");
    }

    // --- nanosleep: NULL req -> EFAULT, bad nsec -> EINVAL, valid -> 0.
    {
        const std::uint32_t kTs = 0x10008000;
        const std::uint32_t a_null[4] = {0, 0, 0, 0};
        Expect("nanosleep(NULL) = -1", std::to_string(static_cast<std::int32_t>(
                                           abr::RuntimeTestAccess::Call(rt, 44, a_null))), "-1");
        std::uint32_t ts[2] = {0, 1000000000u};
        std::memcpy(rt.H(kTs), ts, 8);
        const std::uint32_t a_bad[4] = {kTs, 0, 0, 0};
        Expect("nanosleep(nsec=1e9) = -1", std::to_string(static_cast<std::int32_t>(
                                               abr::RuntimeTestAccess::Call(rt, 44, a_bad))), "-1");
        ts[1] = 1000000;  // 1 ms
        std::memcpy(rt.H(kTs), ts, 8);
        const std::uint32_t a_ok[4] = {kTs, kTs, 0, 0};
        Expect("nanosleep(1 ms) = 0", std::to_string(abr::RuntimeTestAccess::Call(rt, 44, a_ok)), "0");
    }

    // --- string family works on guest memory: no 64 KiB cap, unreadable
    //     memory halts the guest, only the touched bytes are validated.
    {
        const std::uint32_t kBig = 0x10100000;
        std::memset(rt.H(kBig), 'a', 100000);
        *rt.H<char>(kBig + 100000) = 0;
        const std::uint32_t a_big[4] = {kBig, 0, 0, 0};
        Expect("strlen of 100000 bytes", std::to_string(abr::RuntimeTestAccess::Call(rt, 45, a_big)),
               "100000");
        Expect("strlen ok does not halt", abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0", "0");
        const std::uint32_t a_bad[4] = {0xE0000000u, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 45, a_bad);
        Expect("strlen(unmapped) halts the guest", abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0",
               "1");
        // "ab" in the last 3 bytes of the heap mapping: strncpy(dst, src, 8)
        // reads only up to the NUL, so it must not halt.
        const std::uint32_t kTail = 0x50000000u - 3;
        if (abr::RuntimeTestAccess::InMapping(rt, kTail, 3) &&
            !abr::RuntimeTestAccess::InMapping(rt, kTail, 4)) {
            put(kTail, "ab");
            const std::uint32_t kDst = 0x10101000;
            std::memset(rt.H(kDst), 0x55, 16);
            const std::uint32_t a_ncpy[4] = {kDst, kTail, 8, 0};
            abr::RuntimeTestAccess::Call(rt, 46, a_ncpy);
            Expect("strncpy src at mapping end does not halt",
                   abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0", "0");
            const std::uint8_t* d = rt.H<std::uint8_t>(kDst);
            const bool padded = d[0] == 'a' && d[1] == 'b' && d[2] == 0 && d[7] == 0 && d[8] == 0x55;
            Expect("strncpy copies and zero-pads exactly n", padded ? "1" : "0", "1");
        } else {
            Expect("heap mapping ends at 0x50000000 (test premise)", "0", "1");
        }
        const std::uint32_t kD = 0x10102000, kS = 0x10102100;
        put(kD, "x");
        put(kS, "yz");
        const std::uint32_t a_ncat[4] = {kD, kS, 0xFFFFFFFFu, 0};
        abr::RuntimeTestAccess::Call(rt, 47, a_ncat);
        Expect("strncat(n=(size_t)-1)", std::string(rt.H<const char>(kD)), "xyz");
        Expect("strncat does not halt", abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0", "0");
        put(kS, "xyy");
        const std::uint32_t a_cmp[4] = {kD, kS, 0, 0};
        Expect("strcmp(xyz, xyy) > 0",
               static_cast<std::int32_t>(abr::RuntimeTestAccess::Call(rt, 48, a_cmp)) > 0 ? "1" : "0", "1");
    }

    // --- LDREX/STREX are atomic across threads: 4 Jits on the shared monitor,
    //     each with its own processor id, increment one counter 200000 times
    //     (ldrex/add/strex/bne loop, assembled with the NDK clang). A lost
    //     update makes the total come out short. The Jits come from NewJit,
    //     so on arm64 they also run from one shared code cache.
    {
        const std::uint32_t kCode = 0x60000000u, kCounter = 0x60000800u;
        abr::RuntimeTestAccess::MapRwx(rt, kCode, 0x1000);
        const std::uint32_t prog[] = {0xe1901f9f, 0xe2811001, 0xe1802f91, 0xe3520000,
                                      0x1afffffa, 0xe2533001, 0x1afffff8, 0xef000000};
        std::memcpy(rt.H(kCode), prog, sizeof(prog));
        std::memset(rt.H(kCounter), 0, 4);
        abr::GuestCallbacks cb(&rt);
        constexpr int kThreads = 4;
        constexpr std::uint32_t kIters = 200000;
        std::vector<std::thread> ths;
        std::atomic<int> halted_ok{0};
        std::shared_ptr<Dynarmic::A32::SharedCodeCache> caches[kThreads];
        for (int t = 0; t < kThreads; t++) {
            ths.emplace_back([&, t]() {
                std::size_t id = 0;
                if (!rt.AcquireCpuId(&id)) {
                    return;
                }
                Dynarmic::A32::UserConfig cfg;
                cfg.callbacks = &cb;
                cfg.global_monitor = &rt.Monitor();
                cfg.processor_id = id;
                const auto jit_owner = rt.NewJit(cfg);
                Dynarmic::A32::Jit& jit = *jit_owner;
                caches[t] = jit.GetSharedCodeCache();
                rt.ThreadEnter(&jit, 500 + t);
                jit.Regs()[0] = kCounter;
                jit.Regs()[3] = kIters;
                jit.Regs()[15] = kCode;
                jit.SetCpsr(0x10);
                const auto hr = jit.Run();
                if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1)) {
                    halted_ok++;
                }
                rt.ReleaseCpuId(id);
            });
        }
        for (auto& th : ths) {
            th.join();
        }
        std::uint32_t total = 0;
        std::memcpy(&total, rt.H(kCounter), 4);
        Expect("4 threads x 200000 LDREX/STREX increments", std::to_string(total),
               std::to_string(kThreads * kIters));
        Expect("all increment threads halted cleanly", std::to_string(halted_ok.load()),
               std::to_string(kThreads));
        // x64 cannot share (nullptr everywhere); arm64 must use one cache.
        bool one_cache = true;
        for (int t = 1; t < kThreads; t++) {
            one_cache = one_cache && caches[t] == caches[0];
        }
        Expect("the increment Jits use one code cache (or none can share)",
               one_cache ? "1" : "0", "1");
        abr::RuntimeTestAccess::TakeStopped(rt);
    }

    // --- code that runs off the end of its mapping faults at the first
    //     unreadable instruction: the instructions before it execute, the
    //     thread halts (F-23: a zero word used to be translated and run).
    {
        const std::uint32_t kPage = 0x61000000u;
        abr::RuntimeTestAccess::MapRwx(rt, kPage, 0x1000);
        const std::uint32_t tail[] = {0xe3a00001, 0xe3a01002};  // mov r0,#1; mov r1,#2
        std::memcpy(rt.H(kPage + 0xFF8), tail, sizeof(tail));
        abr::GuestCallbacks cb3(&rt);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &cb3;
        std::size_t id = 0;
        rt.AcquireCpuId(&id);
        cfg.global_monitor = &rt.Monitor();
        cfg.processor_id = id;
        Dynarmic::A32::Jit jit(cfg);
        rt.ThreadEnter(&jit, 701);
        jit.Regs()[15] = kPage + 0xFF8;
        jit.SetCpsr(0x10);
        jit.Run();
        Expect("code before the unmapped word ran",
               std::to_string(jit.Regs()[0]) + "," + std::to_string(jit.Regs()[1]), "1,2");
        // Dynarmic leaves pc after the faulting word; ExceptionRaised gets its address.
        Expect("halted at the unmapped word",
               std::string(abr::RuntimeTestAccess::TakeStopped(rt) ? "stopped" : "ran on") + "," +
                   std::to_string(jit.Regs()[15] - kPage),
               "stopped,4100");
        rt.ThreadEnter(nullptr, 0);
        rt.ReleaseCpuId(id);
    }

    // --- coprocessors: none is attached, so an MCR for one is UNDEFINED and
    //     halts the guest (it used to terminate the process while compiling);
    //     the MCR p15 barriers are barriers. ARM, then Thumb.
    {
        const std::uint32_t kPage = 0x61010000u;
        abr::RuntimeTestAccess::MapRwx(rt, kPage, 0x1000);
        // dmb, dsb, isb as mcr p15; mov r0,#7; mcr p3,0,r0,c1,c0,0; mov r1,#9
        const std::uint32_t arm[] = {0xee070fba, 0xee070f9a, 0xee070f95,
                                     0xe3a00007, 0xee010310, 0xe3a01009};
        std::memcpy(rt.H(kPage), arm, sizeof(arm));
        // mcr p15 dmb; movs r0,#7; mcr p3,0,r0,c1,c0,0; movs r1,#9
        const std::uint16_t thumb[] = {0xee07, 0x0fba, 0x2007, 0xee01, 0x0310, 0x2109};
        std::memcpy(rt.H(kPage + 0x100), thumb, sizeof(thumb));
        abr::GuestCallbacks cb4(&rt);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &cb4;
        std::size_t id = 0;
        rt.AcquireCpuId(&id);
        cfg.global_monitor = &rt.Monitor();
        cfg.processor_id = id;
        Dynarmic::A32::Jit jit(cfg);
        rt.ThreadEnter(&jit, 702);
        const auto run = [&](std::uint32_t pc, std::uint32_t cpsr) {
            jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
            jit.Regs().fill(0);
            jit.Regs()[15] = pc;
            jit.SetCpsr(cpsr);
            jit.Run();
            return std::to_string(jit.Regs()[0]) + "," + std::to_string(jit.Regs()[1]) + "," +
                   (abr::RuntimeTestAccess::TakeStopped(rt) ? "stopped" : "ran on");
        };
        Expect("ARM: barriers run, an unattached coprocessor halts", run(kPage, 0x10), "7,0,stopped");
        Expect("Thumb: the same", run(kPage + 0x100, 0x30), "7,0,stopped");
        rt.ThreadEnter(nullptr, 0);
        rt.ReleaseCpuId(id);
    }

    // --- rewritten code: an indirect call must not reach the old block from
    //     the branch table of a Jit that did not flush. On arm64 the two Jits
    //     share a cache, so the second one's flush is the only one needed.
    {
        struct Env : Dynarmic::A32::UserCallbacks {
            std::array<std::uint32_t, 0x1000> mem{};
            Dynarmic::A32::Jit* jit = nullptr;
            std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
            std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
            std::uint32_t MemoryRead32(Dynarmic::A32::VAddr a) override { return mem[a / 4]; }
            std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { return 0; }
            void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override {}
            void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override {}
            void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override {}
            void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override {}
            void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {}
            void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
            void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            void AddTicks(std::uint64_t) override {}
            std::uint64_t GetTicksRemaining() override { return 1000000; }
        } env;
        env.mem[0x100 / 4] = 0xe12fff31;  // blx r1
        env.mem[0x104 / 4] = 0xef000000;  // svc #0
        env.mem[0x200 / 4] = 0xe3a00001;  // f: mov r0,#1
        env.mem[0x204 / 4] = 0xe12fff1e;  // bx lr
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &env;
        cfg.enable_cycle_counting = false;
        Dynarmic::A32::Jit a(cfg);
        Dynarmic::A32::Jit b(cfg, a.GetSharedCodeCache());
        const auto call = [&](Dynarmic::A32::Jit& j) {
            env.jit = &j;
            j.Regs()[0] = 0;
            j.Regs()[1] = 0x200;
            j.Regs()[15] = 0x100;
            j.Run();
            j.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            return std::to_string(j.Regs()[0]);
        };
        Expect("indirect call, first", call(a), "1");
        Expect("indirect call, from the table", call(a), "1");
        Expect("the other Jit, first", call(b), "1");
        env.mem[0x200 / 4] = 0xe3a00002;  // mov r0,#2
        b.InvalidateCacheRange(0x200, 8);
        if (!a.GetSharedCodeCache()) {
            a.InvalidateCacheRange(0x200, 8);  // x64: private caches
        }
        Expect("the flushing Jit sees the new code", call(b), "2");
        Expect("so does the Jit with the old table entry", call(a), "2");
    }

    // --- R-007: a return must not reach a block another Jit invalidated. Jit
    //     a calls f; inside f, Jit b (same cache) sees the code after the call
    //     rewritten and flushes it; a then returns through its return stack.
    {
        struct Env : Dynarmic::A32::UserCallbacks {
            std::array<std::uint32_t, 0x1000> mem{};
            Dynarmic::A32::Jit* a = nullptr;
            Dynarmic::A32::Jit* b = nullptr;
            Dynarmic::A32::Jit* running = nullptr;
            bool rewrite = false;
            std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
            std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
            std::uint32_t MemoryRead32(Dynarmic::A32::VAddr v) override { return mem[v / 4]; }
            std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { return 0; }
            void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override {}
            void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override {}
            void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override {}
            void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override {}
            void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {}
            void CallSVC(std::uint32_t swi) override {
                if (swi == 2) {  // inside f, on Jit a
                    if (rewrite) {
                        mem[0x104 / 4] = 0xe3a00002;  // mov r0,#2
                        b->InvalidateCacheRange(0x104, 4);
                        b->Regs()[15] = 0x300;
                        running = b;
                        b->Run();  // applies the flush to the shared cache
                        b->ClearHalt(Dynarmic::HaltReason::UserDefined1);
                        running = a;
                    }
                    return;
                }
                running->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            }
            void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
                running->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            void AddTicks(std::uint64_t) override {}
            std::uint64_t GetTicksRemaining() override { return 1000000; }
        } env;
        env.mem[0x100 / 4] = 0xeb00003e;  // bl 0x200
        env.mem[0x104 / 4] = 0xe3a00001;  // mov r0,#1
        env.mem[0x108 / 4] = 0xef000001;  // svc #1: done
        env.mem[0x200 / 4] = 0xef000002;  // f: svc #2
        env.mem[0x204 / 4] = 0xe12fff1e;  // bx lr
        env.mem[0x300 / 4] = 0xef000003;  // b: svc #3: done
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &env;
        cfg.enable_cycle_counting = false;
        Dynarmic::A32::Jit a(cfg);
        Dynarmic::A32::Jit b(cfg, a.GetSharedCodeCache());
        env.a = &a;
        env.b = &b;
        const auto run_a = [&]() {
            env.running = &a;
            a.Regs()[0] = 0;
            a.Regs()[15] = 0x100;
            a.Run();
            a.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            return std::to_string(a.Regs()[0]);
        };
        Expect("R-007: the call returns", run_a(), "1");
        if (a.GetSharedCodeCache()) {
            env.rewrite = true;
            Expect("R-007: a return after another Jit's flush runs the new code", run_a(), "2");
        }
    }

    // --- R-007: a block translated from the old bytes must not be emitted
    //     after another Jit flushed them. Jit b reads the code to translate
    //     it; at that moment the code is rewritten and Jit a flushes it.
    {
        struct Env : Dynarmic::A32::UserCallbacks {
            std::array<std::uint32_t, 0x1000> mem{};
            Dynarmic::A32::Jit* a = nullptr;
            Dynarmic::A32::Jit* running = nullptr;
            bool rewrite = false;
            std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
            std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
            std::uint32_t MemoryRead32(Dynarmic::A32::VAddr v) override {
                const std::uint32_t word = mem[v / 4];
                if (v == 0x400 && rewrite) {
                    rewrite = false;
                    mem[0x400 / 4] = 0xe3a00002;  // mov r0,#2
                    a->InvalidateCacheRange(0x400, 4);
                    Dynarmic::A32::Jit* outer = running;
                    running = a;
                    a->Regs()[15] = 0x300;
                    a->Run();  // applies the flush while b translates
                    a->ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    running = outer;
                }
                return word;
            }
            std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { return 0; }
            void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override {}
            void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override {}
            void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override {}
            void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override {}
            void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {}
            void CallSVC(std::uint32_t) override { running->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
            void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
                running->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            void AddTicks(std::uint64_t) override {}
            std::uint64_t GetTicksRemaining() override { return 1000000; }
        } env;
        env.mem[0x300 / 4] = 0xef000003;  // a: svc #3
        env.mem[0x400 / 4] = 0xe3a00001;  // mov r0,#1
        env.mem[0x404 / 4] = 0xef000001;  // svc #1
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &env;
        cfg.enable_cycle_counting = false;
        Dynarmic::A32::Jit a(cfg);
        Dynarmic::A32::Jit b(cfg, a.GetSharedCodeCache());
        env.a = &a;
        if (a.GetSharedCodeCache()) {
            env.rewrite = true;
            env.running = &b;
            b.Regs()[15] = 0x400;
            b.Run();
            Expect("R-007: a block flushed during its translation is translated again",
                   std::to_string(b.Regs()[0]), "2");
        }
    }

#ifdef DYNARMIC_DISPATCH_COUNTERS
    // --- R-007 diagnostics: a translated block whose bytes change with no
    //     flush is reported on the second look, not the first.
    {
        const std::uint32_t kPage = 0x61020000u;
        abr::RuntimeTestAccess::MapRwx(rt, kPage, 0x1000);
        const std::uint32_t tail[] = {0xe3a00001, 0xe3a01002};  // mov r0,#1; mov r1,#2
        std::memcpy(rt.H(kPage + 0xFF8), tail, sizeof(tail));
        abr::GuestCallbacks cb5(&rt);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &cb5;
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(rt.H(0));
        std::size_t id = 0;
        rt.AcquireCpuId(&id);
        cfg.global_monitor = &rt.Monitor();
        cfg.processor_id = id;
        Dynarmic::A32::Jit jit(cfg);
        rt.ThreadEnter(&jit, 703);
        jit.Regs()[15] = kPage + 0xFF8;
        jit.SetCpsr(0x10);
        jit.Run();
        abr::RuntimeTestAccess::TakeStopped(rt);
        std::string ranges;
        const std::size_t clean = jit.DiagChangedCode(ranges);
        *rt.H<std::uint32_t>(kPage + 0xFF8) = 0xe3a00003;  // mov r0,#3, no flush
        const std::size_t first = jit.DiagChangedCode(ranges);
        const std::size_t second = jit.DiagChangedCode(ranges);
        Expect("codewatch: unchanged, first look, second look",
               std::to_string(clean) + "," + std::to_string(first) + "," + std::to_string(second), "0,0,1");
        jit.InvalidateCacheRange(kPage + 0xFF8, 4);
        jit.Run();  // applies the flush
        abr::RuntimeTestAccess::TakeStopped(rt);
        jit.DiagChangedCode(ranges);
        Expect("codewatch: nothing after a flush", std::to_string(jit.DiagChangedCode(ranges)), "0");
        rt.ThreadEnter(nullptr, 0);
        rt.ReleaseCpuId(id);
    }
#endif

#if defined(__aarch64__)
    // --- FPSR across a fastmem fallback: a guest division by zero survives a
    //     memory access that takes the callback, and the host's own FP flags
    //     from inside that callback do not reach the guest. arm64 only: the
    //     x64 backend (PC builds) lets the callback's inexact flag through.
    {
        struct Env : Dynarmic::A32::UserCallbacks {
            std::uint8_t* base = nullptr;
            Dynarmic::A32::Jit* jit = nullptr;
            std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
            std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
            std::uint32_t MemoryRead32(Dynarmic::A32::VAddr v) override {
                if (v < 0x10000) {
                    std::uint32_t w = 0;
                    std::memcpy(&w, base + v, 4);
                    return w;
                }
                volatile float a = 1.0f, b = 3.0f;
                volatile float c = a / b;  // host FPSR: inexact
                (void)c;
                return 7;
            }
            std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { return 0; }
            void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override {}
            void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override {}
            void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override {}
            void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override {}
            void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {}
            void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
            void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            void AddTicks(std::uint64_t) override {}
            std::uint64_t GetTicksRemaining() override { return 1000000; }
        } env;
        // 4 GiB reserved, only the code page accessible: the load faults.
        void* m = mmap(nullptr, std::size_t{1} << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        env.base = static_cast<std::uint8_t*>(m);
        mprotect(env.base, 0x10000, PROT_READ | PROT_WRITE);
        const std::uint32_t prog[] = {0xee800a81,   // vdiv.f32 s0, s1, s2
                                      0xe5932000,   // ldr r2, [r3]
                                      0xeef14a10,   // vmrs r4, fpscr
                                      0xef000000};  // svc #0
        std::memcpy(env.base + 0x1000, prog, sizeof(prog));
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &env;
        cfg.enable_cycle_counting = false;
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(env.base);
        Dynarmic::A32::Jit jit(cfg);
        env.jit = &jit;
        jit.ExtRegs()[1] = 0x3f800000;  // s1 = 1.0f
        jit.ExtRegs()[2] = 0;           // s2 = 0.0f
        jit.Regs()[3] = 0x80000;
        jit.Regs()[15] = 0x1000;
        jit.Run();
        Expect("FPSR across a fastmem fallback: r2, divide-by-zero, inexact",
               std::to_string(jit.Regs()[2]) + "," + std::to_string((jit.Regs()[4] >> 1) & 1) + "," +
                   std::to_string((jit.Regs()[4] >> 4) & 1),
               "7,1,0");
        munmap(m, std::size_t{1} << 32);
    }
#endif

    // --- conditional VMOV (a select in IR, not a block split): each of a
    //     pair of opposite conditions moves only when its flags say so.
    {
        struct Env : Dynarmic::A32::UserCallbacks {
            std::array<std::uint32_t, 0x400> mem{};
            Dynarmic::A32::Jit* jit = nullptr;
            std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
            std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
            std::uint32_t MemoryRead32(Dynarmic::A32::VAddr v) override { return mem[v / 4]; }
            std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { return 0; }
            void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override {}
            void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override {}
            void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override {}
            void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override {}
            void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {}
            void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
            void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            void AddTicks(std::uint64_t) override {}
            std::uint64_t GetTicksRemaining() override { return 1000000; }
        } env;
        env.mem[0x100 / 4] = 0xeeb40a41;  // vcmp.f32 s0, s2
        env.mem[0x104 / 4] = 0xeef1fa10;  // vmrs APSR_nzcv, fpscr
        env.mem[0x108 / 4] = 0xceb00a61;  // vmovgt.f32 s0, s3
        env.mem[0x10c / 4] = 0xdeb02a40;  // vmovle.f32 s4, s0
        env.mem[0x110 / 4] = 0xef000000;  // svc #0
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &env;
        cfg.enable_cycle_counting = false;
        Dynarmic::A32::Jit jit(cfg);
        env.jit = &jit;
        const auto run = [&](std::uint32_t s0) {
            jit.ExtRegs()[0] = s0;
            jit.ExtRegs()[2] = 0x3f800000;  // s2 = 1.0
            jit.ExtRegs()[3] = 0x40a00000;  // s3 = 5.0
            jit.ExtRegs()[4] = 0x41100000;  // s4 = 9.0
            jit.Regs()[15] = 0x100;
            jit.Run();
            jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            char out[32];
            std::snprintf(out, sizeof(out), "%08x,%08x", jit.ExtRegs()[0], jit.ExtRegs()[4]);
            return std::string(out);
        };
        Expect("vmovgt taken, vmovle not (s0 = 2.0)", run(0x40000000), "40a00000,41100000");
        Expect("vmovgt not, vmovle taken (s0 = 0.0)", run(0), "00000000,00000000");
        // A conditional branch in the middle of a loop (fused into its block).
        const std::uint32_t branchy[] = {0xe3100001, 0x0a000000, 0xe2822001, 0xe2811003,
                                         0xe2500001, 0x1afffff9, 0xef000000};
        std::memcpy(&env.mem[0x200 / 4], branchy, sizeof(branchy));
        jit.Regs()[0] = 11;
        jit.Regs()[1] = 0;
        jit.Regs()[2] = 0;
        jit.Regs()[15] = 0x200;
        jit.Run();
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
        Expect("beq in a loop: odd counts, all counts",
               std::to_string(jit.Regs()[2]) + "," + std::to_string(jit.Regs()[1]), "6,33");
        // NEON math runs in the standard FPSCR mode (denormals flushed), a VFP
        // scalar op between such ops in the guest's mode (here: kept).
        const std::uint32_t mixed[] = {0xf2000d42,   // vadd.f32 q0, q0, q1
                                       0xf3004d52,   // vmul.f32 q2, q0, q1
                                       0xee3aaa8b,   // vadd.f32 s20, s21, s22
                                       0xf2046d42,   // vadd.f32 q3, q2, q1
                                       0xef000000};  // svc #0
        std::memcpy(&env.mem[0x300 / 4], mixed, sizeof(mixed));
        const std::uint32_t q0[] = {0x3f800000, 0x00000001, 0x40000000, 0x40400000};  // 1, denormal, 2, 3
        const std::uint32_t q1[] = {0x3f800000, 0x00000000, 0x3f800000, 0x3f800000};  // 1, 0, 1, 1
        for (int i = 0; i < 4; i++) {
            jit.ExtRegs()[i] = q0[i];
            jit.ExtRegs()[4 + i] = q1[i];
        }
        jit.ExtRegs()[21] = 0x00000001;  // denormal
        jit.ExtRegs()[22] = 0;
        jit.SetFpscr(0);
        jit.Regs()[15] = 0x300;
        jit.Run();
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
        char lanes[96];
        std::snprintf(lanes, sizeof(lanes), "%08x,%08x,%08x %08x %08x %08x", jit.ExtRegs()[1], jit.ExtRegs()[20],
                      jit.ExtRegs()[12], jit.ExtRegs()[13], jit.ExtRegs()[14], jit.ExtRegs()[15]);
        Expect("NEON flushes a denormal, VFP between keeps it, q3 = q2 + q1", lanes,
               "00000000,00000001,40400000 00000000 40800000 40a00000");
        // A loop of a forward link and a backward one still sees a halt from
        // another thread (only backward links poll the flag).
        const std::uint32_t spin[] = {0xe2800001,   // loop: add r0, r0, #1
                                      0xea000000,   // b next (forward link)
                                      0x00000000,
                                      0xe2811001,   // next: add r1, r1, #1
                                      0xeafffffa};  // b loop (backward link)
        std::memcpy(&env.mem[0x380 / 4], spin, sizeof(spin));
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
        jit.Regs()[15] = 0x380;
        std::thread stopper([&jit]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined3);
        });
        const Dynarmic::HaltReason why = jit.Run();
        stopper.join();
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined3);
        Expect("a spinning loop halts from another thread",
               (why == Dynarmic::HaltReason::UserDefined3 && jit.Regs()[0] > 0 && jit.Regs()[1] > 0) ? "1" : "0", "1");
    }

    // --- VLD1/VST1 (one element per structure) move whole registers; the
    //     bytes land as the element-wise form put them. VLD2 still interleaves.
    {
        struct Env : Dynarmic::A32::UserCallbacks {
            std::array<std::uint8_t, 0x1000> mem{};
            Dynarmic::A32::Jit* jit = nullptr;
            std::uint64_t Get(Dynarmic::A32::VAddr v, std::size_t n) {
                std::uint64_t x = 0;
                std::memcpy(&x, &mem[v], n);
                return x;
            }
            void Put(Dynarmic::A32::VAddr v, std::uint64_t x, std::size_t n) { std::memcpy(&mem[v], &x, n); }
            std::uint8_t MemoryRead8(Dynarmic::A32::VAddr v) override { return static_cast<std::uint8_t>(Get(v, 1)); }
            std::uint16_t MemoryRead16(Dynarmic::A32::VAddr v) override { return static_cast<std::uint16_t>(Get(v, 2)); }
            std::uint32_t MemoryRead32(Dynarmic::A32::VAddr v) override { return static_cast<std::uint32_t>(Get(v, 4)); }
            std::uint64_t MemoryRead64(Dynarmic::A32::VAddr v) override { return Get(v, 8); }
            void MemoryWrite8(Dynarmic::A32::VAddr v, std::uint8_t x) override { Put(v, x, 1); }
            void MemoryWrite16(Dynarmic::A32::VAddr v, std::uint16_t x) override { Put(v, x, 2); }
            void MemoryWrite32(Dynarmic::A32::VAddr v, std::uint32_t x) override { Put(v, x, 4); }
            void MemoryWrite64(Dynarmic::A32::VAddr v, std::uint64_t x) override { Put(v, x, 8); }
            void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {}
            void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
            void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            }
            void AddTicks(std::uint64_t) override {}
            std::uint64_t GetTicksRemaining() override { return 1000000; }
        } env;
        const std::uint32_t prog[] = {0xf4210a0f,   // vld1.8 {d0-d1}, [r1]
                                      0xf4020a4f,   // vst1.16 {d0-d1}, [r2]
                                      0xf421278d,   // vld1.32 {d2}, [r1]!
                                      0xf423480f,   // vld2.8 {d4-d5}, [r3]
                                      0xef000000};  // svc #0
        std::memcpy(&env.mem[0x100], prog, sizeof(prog));
        for (int i = 0; i < 16; i++) {
            env.mem[0x400 + i] = static_cast<std::uint8_t>(0x10 + i);
        }
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &env;
        cfg.enable_cycle_counting = false;
        Dynarmic::A32::Jit jit(cfg);
        env.jit = &jit;
        jit.Regs()[1] = 0x400;
        jit.Regs()[2] = 0x500;
        jit.Regs()[3] = 0x400;
        jit.Regs()[15] = 0x100;
        jit.Run();
        char out[96];
        std::snprintf(out, sizeof(out), "%d,%08x,%08x%08x,%08x", std::memcmp(&env.mem[0x400], &env.mem[0x500], 16),
                      jit.Regs()[1], jit.ExtRegs()[5], jit.ExtRegs()[4], jit.ExtRegs()[8]);
        Expect("vld1.8/vst1.16 copy, vld1.32 writeback, d2, vld2.8 deinterleaves", out,
               "0,00000408,1716151413121110,16141210");
    }

    // --- allocator edges: huge sizes fail with NULL+ENOMEM (malloc used to
    //     wrap to an 8-byte block), calloc(0, x) returns a unique block,
    //     a calloc product that overflows 32 bits fails.
    {
        auto guest_errno = [&]() {
            std::uint32_t e = 0;
            std::memcpy(&e, rt.H(rt.ErrnoSlot()), 4);
            return e;
        };
        const std::uint32_t m_huge[4] = {0xFFFFFFFCu, 0, 0, 0};
        Expect("malloc(0xFFFFFFFC) = NULL", std::to_string(abr::RuntimeTestAccess::Call(rt, 49, m_huge)), "0");
        Expect("malloc failure errno ENOMEM", std::to_string(guest_errno()), std::to_string(ENOMEM));
        const std::uint32_t c_zero[4] = {0, 16, 0, 0};
        const std::uint32_t z1 = abr::RuntimeTestAccess::Call(rt, 50, c_zero);
        const std::uint32_t z2 = abr::RuntimeTestAccess::Call(rt, 50, c_zero);
        Expect("calloc(0,16) returns a unique block", (z1 && z2 && z1 != z2) ? "1" : "0", "1");
        const std::uint32_t c_ovf[4] = {0x10000u, 0x10001u, 0, 0};
        Expect("calloc overflowing 32 bits = NULL",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 50, c_ovf)), "0");
        const std::uint32_t r_huge[4] = {z1, 0xFFFFFFF0u, 0, 0};
        Expect("realloc(p, huge) = NULL", std::to_string(abr::RuntimeTestAccess::Call(rt, 51, r_huge)), "0");
        Expect("realloc failure keeps the block", abr::RuntimeTestAccess::GuestLiveSize(rt, z1) ? "1" : "0",
               "1");
    }

    // --- mmap: a file mapping shows the file (fd/offset/flags were dropped,
    //     returning zero pages); munmap revokes the range; MAP_FIXED outside
    //     the arena is refused.
    {
        const std::string host_dir = TestTmp() + "/opencode/abr-mmaptest";
        std::filesystem::create_directories(host_dir);
        {
            std::FILE* f = std::fopen((host_dir + "/m.bin").c_str(), "wb");
            std::string blob(8192, 'x');
            blob.replace(4096, 5, "HELLO");
            if (f) {
                std::fwrite(blob.data(), 1, blob.size(), f);
                std::fclose(f);
            }
        }
        rt.SetFilesDir("/data/data/abr.mmap/files", host_dir);
        const std::uint32_t kPath = 0x10008100, kSp = 0x10008200;
        put(kPath, "/data/data/abr.mmap/files/m.bin");
        const std::uint32_t oargs[4] = {kPath, O_RDONLY, 0, 0};
        const std::uint32_t gfd = abr::RuntimeTestAccess::Call(rt, 54, oargs);
        Expect("open for mmap", static_cast<std::int32_t>(gfd) >= 0 ? "1" : "0", "1");
        abr::RuntimeTestAccess::RegsView in{};
        in[13] = kSp;
        const std::uint32_t stk[2] = {gfd, 4096};  // fd, offset
        std::memcpy(rt.H(kSp), stk, 8);
        const std::uint32_t margs[4] = {0, 4096, PROT_READ, MAP_PRIVATE};
        const std::uint32_t m = abr::RuntimeTestAccess::Call(rt, 52, margs, in, nullptr);
        Expect("file mmap succeeds", (m != 0xFFFFFFFFu && m >= 0x70000000u) ? "1" : "0", "1");
        if (m != 0xFFFFFFFFu) {
            Expect("file mmap shows the file at the offset", std::string(rt.H<const char>(m), 5), "HELLO");
            Expect("mapped range is readable", abr::RuntimeTestAccess::InMapping(rt, m, 4096) ? "1" : "0", "1");
            const std::uint32_t uargs[4] = {m, 4096, 0, 0};
            Expect("munmap returns 0", std::to_string(abr::RuntimeTestAccess::Call(rt, 53, uargs)), "0");
            Expect("munmapped range is no longer readable",
                   abr::RuntimeTestAccess::InMapping(rt, m, 4) ? "1" : "0", "0");
        }
        const std::uint32_t anon_stk[2] = {0xFFFFFFFFu, 0};
        std::memcpy(rt.H(kSp), anon_stk, 8);
        const std::uint32_t aargs[4] = {0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS};
        const std::uint32_t am = abr::RuntimeTestAccess::Call(rt, 52, aargs, in, nullptr);
        Expect("anonymous mmap is zeroed and writable",
               (am != 0xFFFFFFFFu && *rt.H<std::uint8_t>(am + 100) == 0 &&
                abr::RuntimeTestAccess::InMapping(rt, am, 8192))
                   ? "1"
                   : "0",
               "1");
        const std::uint32_t fargs[4] = {0x10000000u, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED};
        Expect("MAP_FIXED over the heap is refused",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 52, fargs, in, nullptr)), "4294967295");
    }

    // --- setjmp/longjmp preserve the VFP callee-saved d8-d15 and FPSCR.
    {
        abr::GuestCallbacks cb2(&rt);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &cb2;
        std::size_t id = 0;
        rt.AcquireCpuId(&id);
        cfg.global_monitor = &rt.Monitor();
        cfg.processor_id = id;
        Dynarmic::A32::Jit jit(cfg);
        rt.ThreadEnter(&jit, 700);
        for (int i = 0; i < 16; i++) {
            jit.ExtRegs()[16 + i] = 0xA0000000u + static_cast<std::uint32_t>(i);
        }
        jit.SetFpscr(0x03000000u);  // RMode = round towards zero
        const std::uint32_t kJb = 0x10008400;
        const std::uint32_t sj[4] = {kJb, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 6, sj);
        for (int i = 0; i < 16; i++) {
            jit.ExtRegs()[16 + i] = 0;
        }
        jit.SetFpscr(0);
        const std::uint32_t lj[4] = {kJb, 7, 0, 0};
        const std::uint32_t r = abr::RuntimeTestAccess::Call(rt, 7, lj);
        bool vfp_ok = true;
        for (int i = 0; i < 16; i++) {
            if (jit.ExtRegs()[16 + i] != 0xA0000000u + static_cast<std::uint32_t>(i)) {
                vfp_ok = false;
            }
        }
        Expect("longjmp returns its value", std::to_string(r), "7");
        Expect("longjmp restores d8-d15", vfp_ok ? "1" : "0", "1");
        Expect("longjmp restores FPSCR", std::to_string(jit.Fpscr() & 0x03000000u), std::to_string(0x03000000u));
        rt.ThreadEnter(nullptr, 0);
        rt.ReleaseCpuId(id);
    }

    // --- AAPCS32 math args: fma's third double and remquo's quo pointer are
    //     on the stack; remquof's quo is in r2 (r4 was read before).
    {
        auto dw = [](double d, std::uint32_t* lo, std::uint32_t* hi) {
            std::uint64_t b = 0;
            std::memcpy(&b, &d, 8);
            *lo = static_cast<std::uint32_t>(b);
            *hi = static_cast<std::uint32_t>(b >> 32);
        };
        auto fromw = [](std::uint32_t lo, std::uint32_t hi) {
            const std::uint64_t b = (std::uint64_t(hi) << 32) | lo;
            double d = 0;
            std::memcpy(&d, &b, 8);
            return d;
        };
        const std::uint32_t kSp = 0x10008600, kQ = 0x10008700;
        abr::RuntimeTestAccess::RegsView in{}, out{};
        in[4] = 0xDEADBEEF;
        in[5] = 0xDEADBEEF;
        in[13] = kSp;
        std::uint32_t a[4], z[2];
        dw(2.0, &a[0], &a[1]);
        dw(3.0, &a[2], &a[3]);
        dw(10.0, &z[0], &z[1]);
        std::memcpy(rt.H(kSp), z, 8);
        abr::RuntimeTestAccess::Call(rt, 55, a, in, &out);
        Expect("fma(2,3,10) third arg from [sp]", std::to_string(fromw(out[0], out[1])), "16.000000");
        dw(7.0, &a[0], &a[1]);
        dw(2.0, &a[2], &a[3]);
        std::memcpy(rt.H(kSp), &kQ, 4);
        std::memset(rt.H(kQ), 0, 4);
        abr::RuntimeTestAccess::Call(rt, 56, a, in, &out);
        std::int32_t q = 0;
        std::memcpy(&q, rt.H(kQ), 4);
        Expect("remquo(7,2) remainder", std::to_string(fromw(out[0], out[1])), "-1.000000");
        Expect("remquo quo via [sp]", std::to_string(q & 7), "4");
        float fx = 7.0f, fy = 2.0f;
        std::uint32_t fa[4] = {0, 0, kQ, 0};
        std::memcpy(&fa[0], &fx, 4);
        std::memcpy(&fa[1], &fy, 4);
        std::memset(rt.H(kQ), 0, 4);
        abr::RuntimeTestAccess::Call(rt, 57, fa, in, &out);
        std::memcpy(&q, rt.H(kQ), 4);
        Expect("remquof quo via r2", std::to_string(q & 7), "4");
    }

    // --- file bridge errno and missing calls (dup, mkdir, read, unmapped
    //     paths, mkstemp, __system_property_get).
    {
        auto guest_errno = [&]() {
            std::uint32_t e = 0;
            std::memcpy(&e, rt.H(rt.ErrnoSlot()), 4);
            return e;
        };
        const std::string host_dir = TestTmp() + "/opencode/abr-errnotest";
        std::filesystem::remove_all(host_dir);
        std::filesystem::create_directories(host_dir);
        rt.SetFilesDir("/data/data/abr.errno/files", host_dir);
        const std::uint32_t kP = 0x10008800, kBuf = 0x10008900;
        // dup
        const std::uint32_t sargs[4] = {AF_INET, SOCK_DGRAM, 0, 0};
        const std::uint32_t sfd = abr::RuntimeTestAccess::Call(rt, 33, sargs);
        const std::uint32_t dargs[4] = {sfd, 0, 0, 0};
        const std::uint32_t dfd = abr::RuntimeTestAccess::Call(rt, 58, dargs);
        Expect("dup returns a new guest fd (not stdin)",
               (static_cast<std::int32_t>(dfd) > 2 && dfd != sfd) ? "1" : "0", "1");
        // mkdir
        put(kP, "/data/data/abr.errno/files/d");
        const std::uint32_t margs[4] = {kP, 0755, 0, 0};
        Expect("mkdir new dir = 0", std::to_string(abr::RuntimeTestAccess::Call(rt, 59, margs)), "0");
        Expect("mkdir existing dir = -1",
               std::to_string(static_cast<std::int32_t>(abr::RuntimeTestAccess::Call(rt, 59, margs))), "-1");
        Expect("mkdir existing dir errno EEXIST", std::to_string(guest_errno()), std::to_string(EEXIST));
        put(kP, "/data/data/abr.errno/files/no/such");
        abr::RuntimeTestAccess::Call(rt, 59, margs);
        Expect("mkdir with missing parent errno ENOENT", std::to_string(guest_errno()), std::to_string(ENOENT));
        // read on an unknown fd
        const std::uint32_t rargs[4] = {9999, kBuf, 4, 0};
        Expect("read(unknown fd) = -1",
               std::to_string(static_cast<std::int32_t>(abr::RuntimeTestAccess::Call(rt, 60, rargs))), "-1");
        Expect("read(unknown fd) errno EBADF", std::to_string(guest_errno()), std::to_string(EBADF));
        // unmapped path: ENOENT, no halt
        put(kP, "/sdcard/nothing");
        const std::uint32_t aargs[4] = {kP, F_OK, 0, 0};
        Expect("access(unmapped path) = -1",
               std::to_string(static_cast<std::int32_t>(abr::RuntimeTestAccess::Call(rt, 63, aargs))), "-1");
        Expect("access(unmapped path) errno ENOENT", std::to_string(guest_errno()), std::to_string(ENOENT));
        Expect("unmapped path does not halt", abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0", "0");
        // mkstemp writes the suffix at the end of the GUEST template
        const std::string tmpl = "/data/data/abr.errno/files/tXXXXXX";
        put(kP, tmpl);
        const std::uint32_t tlen = static_cast<std::uint32_t>(tmpl.size());
        *rt.H<char>(kP + tlen + 1) = 'Z';  // byte after the NUL must stay
        const std::uint32_t kargs[4] = {kP, 0, 0, 0};
        const std::uint32_t tfd = abr::RuntimeTestAccess::Call(rt, 61, kargs);
        const std::string tname(rt.H<const char>(kP));
        Expect("mkstemp returns a fd", static_cast<std::int32_t>(tfd) > 2 ? "1" : "0", "1");
        Expect("mkstemp keeps the guest length", std::to_string(tname.size()), std::to_string(tlen));
        Expect("mkstemp replaced the X's", tname.find("XXXXXX") == std::string::npos ? "1" : "0", "1");
        Expect("mkstemp wrote nothing past the NUL", std::string(1, *rt.H<char>(kP + tlen + 1)), "Z");
        // unknown property -> "" and 0
        put(kP, "abr.no.such.property");
        std::memset(rt.H(kBuf), 'q', 92);
        const std::uint32_t pargs[4] = {kP, kBuf, 0, 0};
        Expect("property_get(unknown) = 0", std::to_string(abr::RuntimeTestAccess::Call(rt, 62, pargs)), "0");
        Expect("property_get(unknown) writes \"\"", std::to_string(*rt.H<std::uint8_t>(kBuf)), "0");
    }

    // --- guest mutex types follow bionic: NORMAL (default) can be unlocked
    //     by another thread and trylock on a held one is EBUSY; RECURSIVE /
    //     ERRORCHECK come from the attribute or the static initializer word.
    {
        const std::uint32_t kMn = 0x10008A00, kMr = 0x10008A40, kMe = 0x10008A80, kMs = 0x10008AC0,
                            kAt = 0x10008B00;
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0) {
            const std::uint32_t args[4] = {x, y, 0, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, args);
        };
        call(8, kMn, 0);  // init NORMAL (attr NULL)
        Expect("normal: lock", std::to_string(call(9, kMn)), "0");
        Expect("normal: trylock while held = EBUSY", std::to_string(call(66, kMn)), std::to_string(EBUSY));
        std::uint32_t foreign = 1;
        // Another guest thread (its own tid: a bare std::thread would be
        // guest tid 1, the same as this one).
        std::thread([&] {
            rt.ThreadEnter(nullptr, 77);
            foreign = call(11, kMn);
            rt.ThreadEnter(nullptr, 0);
        }).join();
        Expect("normal: unlock from another thread = 0", std::to_string(foreign), "0");
        Expect("normal: free again after foreign unlock", std::to_string(call(66, kMn)), "0");
        call(11, kMn);
        {
            // The foreign unlock released it: the books must say so, or the
            // destroy guard keeps a free mutex as "held" and leaks it.
            call(9, kMn);
            std::thread([&] {
                rt.ThreadEnter(nullptr, 77);
                call(11, kMn);
                rt.ThreadEnter(nullptr, 0);
            }).join();
            const unsigned skipped = rt.mutex_destroy_skipped_;
            call(10, kMn);
            Expect("normal: destroy after a foreign unlock is not refused",
                   std::to_string(rt.mutex_destroy_skipped_ - skipped), "0");
            call(8, kMn, 0);  // init again for the next use
        }
        call(64, kAt);
        call(65, kAt, 1);  // RECURSIVE
        call(8, kMr, kAt);
        Expect("recursive: lock", std::to_string(call(9, kMr)), "0");
        Expect("recursive: relock", std::to_string(call(9, kMr)), "0");
        call(11, kMr);
        call(11, kMr);
        call(65, kAt, 2);  // ERRORCHECK
        call(8, kMe, kAt);
        call(9, kMe);
        Expect("errorcheck: relock = EDEADLK", std::to_string(call(9, kMe)), std::to_string(EDEADLK));
        call(11, kMe);
        {
            // The same answers on the sliced path (signals in use), which
            // first tries the lock at once and slices only when it is held.
            abr::RuntimeTestAccess::SetSignalsInUse(rt, true);
            call(9, kMr);
            Expect("sliced: recursive relock", std::to_string(call(9, kMr)), "0");
            call(11, kMr);
            call(11, kMr);
            call(9, kMe);
            Expect("sliced: errorcheck relock = EDEADLK", std::to_string(call(9, kMe)),
                   std::to_string(EDEADLK));
            call(11, kMe);
            call(9, kMn);  // held here; another thread must wait for it
            std::atomic<bool> got{false};
            std::thread t([&] {
                rt.ThreadEnter(nullptr, 78);
                call(9, kMn);
                got = true;
                call(11, kMn);
                rt.ThreadEnter(nullptr, 0);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const bool early = got.load();
            call(11, kMn);
            t.join();
            Expect("sliced: a held normal mutex waits, then is taken",
                   std::string(early ? "early" : "waited") + (got.load() ? ",taken" : ",never"),
                   "waited,taken");
            abr::RuntimeTestAccess::SetSignalsInUse(rt, false);
        }
        const std::uint32_t rec_init = 0x4000;  // PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
        std::memcpy(rt.H(kMs), &rec_init, 4);
        call(9, kMs);
        Expect("static recursive initializer: relock", std::to_string(call(9, kMs)), "0");
        call(11, kMs);
        call(11, kMs);
    }

    // --- destroying a cond while another thread waits in it must not free
    //     the host object under the waiter (it now holds a reference).
    {
        const std::uint32_t kM2 = 0x10008C00, kC2 = 0x10008C40, kTs2 = 0x10008C80;
        std::uint32_t wr = 0;
        std::atomic<bool> waiting{false};
        std::thread waiter([&] {
            const std::uint32_t mi[4] = {kM2, 0, 0, 0};
            abr::RuntimeTestAccess::Call(rt, 8, mi);
            abr::RuntimeTestAccess::Call(rt, 9, mi);
            struct timespec now {};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const std::int64_t ns = static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec +
                                    300000000LL;
            const std::uint32_t ts[2] = {static_cast<std::uint32_t>(ns / 1000000000LL),
                                         static_cast<std::uint32_t>(ns % 1000000000LL)};
            std::memcpy(rt.H(kTs2), ts, 8);
            const std::uint32_t w[4] = {kC2, kM2, kTs2, 0};
            waiting = true;
            wr = abr::RuntimeTestAccess::Call(rt, 39, w);
            abr::RuntimeTestAccess::Call(rt, 11, mi);
        });
        while (!waiting) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::uint32_t d[4] = {kC2, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 67, d);  // destroy under the waiter
        waiter.join();
        Expect("waiter survives a concurrent cond destroy (ETIMEDOUT)", std::to_string(wr),
               std::to_string(ETIMEDOUT));
    }

    // --- pthread_*specific: each guest thread its own value; a key delete
    //     clears it for every thread.
    {
        const std::uint32_t kKeyAt = 0x10008E00;
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0) {
            const std::uint32_t args[4] = {x, y, 0, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, args);
        };
        call(108, kKeyAt, 0);
        std::uint32_t key = 0;
        std::memcpy(&key, rt.H(kKeyAt), 4);
        call(109, key, 5);
        std::uint32_t other_before = 1, other_after = 0;
        std::thread([&] {
            rt.ThreadEnter(nullptr, 77);
            other_before = call(110, key);
            call(109, key, 7);
            other_after = call(110, key);
        }).join();
        Expect("tls: another thread starts at NULL", std::to_string(other_before), "0");
        Expect("tls: another thread keeps its own", std::to_string(other_after), "7");
        Expect("tls: this thread keeps its own", std::to_string(call(110, key)), "5");
        call(111, key);
        Expect("tls: a deleted key reads NULL", std::to_string(call(110, key)), "0");
    }

    // --- pthread_attr_setstacksize: the LP32 PTHREAD_STACK_MIN is 8192
    //     (FMOD's stream threads ask for exactly that).
    {
        const std::uint32_t kAttr = 0x10008F00;
        const std::uint32_t init[4] = {kAttr, 0, 0, 0};
        abr::RuntimeTestAccess::Call(rt, 112, init);
        const std::uint32_t ok[4] = {kAttr, 8192, 0, 0};
        const std::uint32_t small[4] = {kAttr, 8188, 0, 0};
        Expect("attr: stack size 8192 accepted", std::to_string(abr::RuntimeTestAccess::Call(rt, 113, ok)), "0");
        Expect("attr: below 8192 refused", std::to_string(abr::RuntimeTestAccess::Call(rt, 113, small)),
               std::to_string(EINVAL));
    }

    // --- cond wakes: a signal with nobody waiting is not kept, and a signal
    //     after the waiter went to sleep always reaches it, also in the
    //     sliced waits used while guest signals are in use (a slice timing
    //     out across the wake used to lose it: Unity hung for good).
    {
        const std::uint32_t kM3 = 0x10008D00, kC3 = 0x10008D40, kTs3 = 0x10008D80;
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0,
                        std::uint32_t z = 0) {
            const std::uint32_t args[4] = {x, y, z, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, args);
        };
        auto deadline_in = [&](std::int64_t ms) {
            struct timespec now {};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const std::int64_t ns = static_cast<std::int64_t>(now.tv_sec) * 1000000000LL +
                                    now.tv_nsec + ms * 1000000LL;
            const std::uint32_t ts[2] = {static_cast<std::uint32_t>(ns / 1000000000LL),
                                         static_cast<std::uint32_t>(ns % 1000000000LL)};
            std::memcpy(rt.H(kTs3), ts, 8);
        };
        call(8, kM3);
        call(106, kC3);  // nobody waits
        call(9, kM3);
        deadline_in(50);
        Expect("cond: a signal before the wait is not kept",
               std::to_string(call(39, kC3, kM3, kTs3)), std::to_string(ETIMEDOUT));
        call(11, kM3);
        int lost = 0;
        for (int sliced = 0; sliced < 2; sliced++) {
            abr::RuntimeTestAccess::SetSignalsInUse(rt, sliced == 1);
            for (int i = 0; i < 150; i++) {
                std::atomic<bool> in{false};
                std::uint32_t r = 1;
                std::thread w([&] {
                    call(9, kM3);
                    in = true;
                    r = call(107, kC3, kM3);
                    call(11, kM3);
                });
                while (!in) {
                    std::this_thread::yield();
                }
                call(9, kM3);  // the waiter has let it go: it sleeps
                if (i % 2) {
                    call(11, kM3);
                    call(106, kC3);  // signal outside the mutex
                } else {
                    call(106, kC3);
                    call(11, kM3);
                }
                w.join();
                lost += r != 0;
            }
        }
        abr::RuntimeTestAccess::SetSignalsInUse(rt, false);
        Expect("cond: 300 signals after the wait, none lost", std::to_string(lost), "0");
    }

    // --- imports that used to answer 0 from the generic stub (G-02).
    {
        const std::uint32_t kP = 0x10008D00, kF = 0x10008D40, kVa = 0x10008D80, kS = 0x10008E00;
        auto call = [&](std::uint32_t idx, std::uint32_t x, std::uint32_t y = 0, std::uint32_t z = 0) {
            const std::uint32_t args[4] = {x, y, z, 0};
            return abr::RuntimeTestAccess::Call(rt, idx, args);
        };
        put(kF, "x=%d");
        const std::uint32_t v42 = 42;
        std::memcpy(rt.H(kVa), &v42, 4);
        std::memset(rt.H(kP), 0, 4);
        Expect("vasprintf returns the length", std::to_string(call(68, kP, kF, kVa)), "4");
        std::uint32_t sp = 0;
        std::memcpy(&sp, rt.H(kP), 4);
        Expect("vasprintf allocates the string", sp ? std::string(rt.H<const char>(sp)) : "NULL", "x=42");
        put(kF, "*.png");
        put(kS, "a/b.png");
        Expect("fnmatch without PATHNAME matches across /", std::to_string(call(69, kF, kS, 0)), "0");
        Expect("fnmatch with PATHNAME (bionic 0x2) does not", std::to_string(call(69, kF, kS, 2)), "1");
        put(kS, "/data/files/save.lua/");
        const std::uint32_t b = call(70, kS);
        Expect("basename strips the trailing slash", b ? std::string(rt.H<const char>(b)) : "NULL", "save.lua");
        const std::uint32_t w[4] = {'a', 'b', 'c', 0};
        std::memcpy(rt.H(kS), w, sizeof(w));
        Expect("wcslen counts 4-byte units", std::to_string(call(71, kS)), "3");
    }

    // --- Mock jstrings past the 3840-byte arena go to the guest heap instead
    //     of halting (Rio filled the arena in 160 s of play, 2026-10-25).
    {
        bool all_ok = true;
        std::uint32_t last = 0;
        for (int i = 0; i < 400; i++) {  // 400 x 20 B > 3840 B
            const std::string s = "jstring-overflow-" + std::to_string(i % 10);
            last = rt.JniNewString(s);
            if (last == 0 || std::strcmp(reinterpret_cast<const char*>(rt.H(last)), s.c_str()) != 0) {
                all_ok = false;
                break;
            }
        }
        Expect("jstrings past the arena are created and readable", all_ok ? "1" : "0", "1");
        Expect("overflow jstring lives outside the arena",
               (last < abr::kJniStringStart || last >= abr::kJniStringLimit) ? "1" : "0", "1");
        Expect("overflow jstring is a live heap block",
               abr::RuntimeTestAccess::GuestLiveSize(rt, last) != 0 ? "1" : "0", "1");
        const std::uint32_t len_args[4] = {0, last, 0, 0};
        Expect("overflow jstring GetStringUTFLength",
               std::to_string(abr::RuntimeTestAccess::Jni(rt, 168, len_args)),
               std::to_string(std::strlen("jstring-overflow-9")));
    }

#ifdef ABR_DIAG_COUNTERS
    // Stage D smoke: the dump must be safe after JNI arena traffic and must
    // emit exactly one parseable line. The diag APK soak is the real test;
    // this catches a crash or a format/argument mismatch in the native build.
    rt.JniNewString("stage-d-counters");
    rt.DumpDiagCounters();
#endif

    // --- jstrings are counted in UTF-16 units, whatever their bytes: a guest
    //     sizes GetStringChars reads and region buffers from these numbers.
    {
        const std::uint32_t kSp = 0x60004000u, kBuf = kSp + 0x100;
        abr::RuntimeTestAccess::MapRwx(rt, kSp, 0x1000);
        std::memcpy(rt.H(kSp), &kBuf, 4);
        const std::uint32_t h = rt.JniNewString("a\xD0\xAF\xE2\x82\xAC\xF0\x9F\x98\x80z");
        const std::uint32_t a[4] = {0, h, 0, 0};
        Expect("GetStringLength: 1 + 1 + 1 + 2 + 1 units",
               std::to_string(abr::RuntimeTestAccess::Jni(rt, 164, a)), "6");
        abr::RuntimeTestAccess::JniRegion(rt, 220, h, 1, 4, kSp);
        Expect("GetStringRegion is UTF-16",
               std::to_string(*rt.H<std::uint16_t>(kBuf)) + "," +
                   std::to_string(*rt.H<std::uint16_t>(kBuf + 2)) + "," +
                   std::to_string(*rt.H<std::uint16_t>(kBuf + 4)) + "," +
                   std::to_string(*rt.H<std::uint16_t>(kBuf + 6)),
               "1071,8364,55357,56832");
        abr::RuntimeTestAccess::JniRegion(rt, 221, h, 1, 2, kSp);
        Expect("GetStringUTFRegion counts units, copies bytes",
               std::string(rt.H<const char>(kBuf), 5), "\xD0\xAF\xE2\x82\xAC");
        // Half a surrogate pair: U+1F600 = D83D DE00, three bytes each.
        std::memset(rt.H<char>(kBuf), 0x55, 8);
        abr::RuntimeTestAccess::JniRegion(rt, 221, h, 3, 1, kSp);
        Expect("GetStringUTFRegion: first half of a pair",
               std::string(rt.H<const char>(kBuf), 4), "\xED\xA0\xBD\x55");
        abr::RuntimeTestAccess::JniRegion(rt, 221, h, 4, 2, kSp);
        Expect("GetStringUTFRegion: second half of a pair, then z",
               std::string(rt.H<const char>(kBuf), 4), "\xED\xB8\x80z");
        // Broken and overlong text: one count for length, chars and region.
        const std::uint32_t bad = rt.JniNewString("caf\xE9 x\xF0\x8F\xBF\xBF!");
        const std::uint32_t ba[4] = {0, bad, 0, 0};
        Expect("GetStringLength of broken text: 6 bytes + 1 overlong + 1",
               std::to_string(abr::RuntimeTestAccess::Jni(rt, 164, ba)), "8");
        const std::uint32_t chars = abr::RuntimeTestAccess::Jni(rt, 165, ba);
        Expect("GetStringChars gives what GetStringLength said",
               std::to_string(*rt.H<std::uint16_t>(chars + 3 * 2)) + "," +
                   std::to_string(*rt.H<std::uint16_t>(chars + 6 * 2)) + "," +
                   std::to_string(*rt.H<std::uint16_t>(chars + 7 * 2)) + "," +
                   std::to_string(*rt.H<std::uint16_t>(chars + 8 * 2)),
               "233,65535,33,0");
        std::memset(rt.H<char>(kBuf), 0, 8);
        abr::RuntimeTestAccess::JniRegion(rt, 221, bad, 6, 2, kSp);
        Expect("GetStringUTFRegion counts an overlong character as one unit",
               std::string(rt.H<const char>(kBuf), 6), std::string("\xF0\x8F\xBF\xBF!\0", 6));
        abr::GuestCallbacks cb(&rt);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &cb;
        std::size_t id = 0;
        rt.AcquireCpuId(&id);
        cfg.global_monitor = &rt.Monitor();
        cfg.processor_id = id;
        const auto jit = rt.NewJit(cfg);
        rt.ThreadEnter(jit.get(), 900);  // the LOUD path halts the running Jit
        abr::RuntimeTestAccess::JniRegion(rt, 221, h, 5, 2, kSp);
        Expect("a region past the units halts", abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0",
               "1");
        rt.ThreadEnter(nullptr, 0);
        rt.ReleaseCpuId(id);
    }

    // --- TLS blocks sit apart from handles, which must stay unmapped. A thread
    //     past the last block stops instead of reading another's values.
    {
        Expect("PtrHandle numbers are not mapped",
               abr::RuntimeTestAccess::InMapping(rt, 0xF2000000u, 4) ? "1" : "0", "0");
        std::uint32_t last = 0;
        for (std::uint32_t i = 0; i < abr::kTlsBlocks; i++) {
            last = abr::RuntimeTestAccess::TlsBlock(rt, 2000 + i);
        }
        Expect("the first thread past the pool stops",
               abr::RuntimeTestAccess::TakeStopped(rt) ? "1" : "0", "1");
        Expect("...on a block no thread has", std::to_string(last),
               std::to_string(abr::kTlsBase + (abr::kTlsBlocks - 1) * abr::kTlsBlockSize));
        for (std::uint32_t i = 0; i < abr::kTlsBlocks; i++) {
            rt.ReapHostThread(2000 + i);
        }
    }

    // --- pthread_getspecific thunk: each Jit reads its own thread's block,
    //     inside guest code once the first call has bound it. The Jits come
    //     from NewJit: on arm64 they share one code cache, which must not fix
    //     the block of the thread that compiled the thunk.
    {
        const std::uint32_t kCode = 0x60003000u, kTramp = kCode + 0x40, kHalt = kCode + 0x100;
        const std::uint32_t idx = abr::RuntimeTestAccess::ImportIndex(rt, "pthread_getspecific");
        abr::RuntimeTestAccess::MapRwx(rt, kCode, 0x1000);
        std::memcpy(rt.H(kCode), abr::kTlsGetThunk, sizeof(abr::kTlsGetThunk));
        const std::uint32_t slow = abr::TlsGetThunkSlowWord(kCode, kTramp);
        std::memcpy(rt.H(kCode + sizeof(abr::kTlsGetThunk)), &slow, 4);
        const std::uint32_t svc_get = 0xEF000000u | (idx + 1), svc_halt = 0xEF000000u;
        std::memcpy(rt.H(kTramp), &svc_get, 4);
        std::memcpy(rt.H(kHalt), &svc_halt, 4);
        abr::GuestCallbacks cb(&rt);
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &cb;
        std::size_t id = 0;
        rt.AcquireCpuId(&id);
        cfg.global_monitor = &rt.Monitor();
        cfg.processor_id = id;
        const auto ja = rt.NewJit(cfg), jb = rt.NewJit(cfg);
        auto get = [&](Dynarmic::A32::Jit& j, std::uint32_t key) {
            j.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            j.Regs()[0] = key;
            j.Regs()[14] = kHalt;
            j.Regs()[15] = kCode;
            j.SetCpsr(0x10);
            j.Run();
            return std::to_string(j.Regs()[0]);
        };
        rt.ThreadEnter(ja.get(), 801);
        abr::RuntimeTestAccess::SetTls(rt, 5, 11);
        rt.ThreadEnter(jb.get(), 802);
        abr::RuntimeTestAccess::SetTls(rt, 5, 22);
        rt.ThreadEnter(ja.get(), 801);
        auto host_calls = [&](auto fn) {
            const std::uint64_t n0 = rt.svc_count.load();
            fn();
            return std::to_string(rt.svc_count.load() - n0);
        };
        // Each run ends in one svc: the halt.
        Expect("first read goes to the host and binds",
               host_calls([&] { Expect("thread 801, first read", get(*ja, 5), "11"); }), "2");
        Expect("next read stays in guest code",
               host_calls([&] { Expect("thread 801, bound read", get(*ja, 5), "11"); }), "1");
        rt.ThreadEnter(jb.get(), 802);
        Expect("another Jit reads its own thread's value", get(*jb, 5), "22");
        Expect("...also once bound", get(*jb, 5), "22");
        rt.ThreadEnter(ja.get(), 802);
        Expect("a Jit that changes threads does not keep the old block", get(*ja, 5), "22");
        Expect("key past the block is NULL", get(*ja, abr::kTlsKeys + 7), "0");
        Expect("key at the end of the range is NULL", get(*ja, abr::kTlsKeys), "0");
        Expect("key never set is NULL", get(*ja, abr::kTlsKeys - 1), "0");
        const std::uint32_t b802 = abr::RuntimeTestAccess::TlsBlock(rt, 802);
        Expect("the block lies outside the heap",
               b802 >= abr::kTlsBase && b802 < abr::kTlsBase + abr::kTlsBlocks * abr::kTlsBlockSize
                   ? "1"
                   : "0",
               "1");
        rt.ThreadEnter(nullptr, 0);
        rt.ReapHostThread(801);
        rt.ReapHostThread(802);
        Expect("a new thread gets a cleared block",
               std::to_string(*rt.H<std::uint32_t>(abr::RuntimeTestAccess::TlsBlock(rt, 803) + 5 * 4)),
               "0");
        rt.ReapHostThread(803);
        rt.ReleaseCpuId(id);
    }

    // --- quarantine: the last 64 freed blocks stay out of reuse; a double
    //     free of one of them is refused (LOUD) and the heap goes on. LAST on
    //     purpose: the heap starts below the fixed scratch addresses the
    //     tests above write (kScratch 0x10001000...), and ~9 KB of blocks
    //     reach them.
    {
        std::vector<std::uint32_t> blocks;
        for (int i = 0; i < 100; i++) {
            blocks.push_back(rt.GuestMalloc(48));
        }
        for (const std::uint32_t b : blocks) {
            rt.GuestFree(b);
        }
        std::set<std::uint32_t> held(blocks.end() - 64, blocks.end());
        int reused = 0;
        std::vector<std::uint32_t> again;
        for (int i = 0; i < 64; i++) {
            const std::uint32_t b = rt.GuestMalloc(48);
            again.push_back(b);
            reused += held.count(b) ? 1 : 0;
        }
        Expect("quarantine: the last 64 frees are not handed out again", std::to_string(reused), "0");
        rt.GuestFree(blocks.back());  // still quarantined: a double free
        const std::uint32_t after = rt.GuestMalloc(48);
        Expect("quarantine: malloc works after a refused double free",
               std::to_string(after != 0 && !held.count(after)), "1");
        rt.GuestFree(after);
        for (const std::uint32_t b : again) {
            rt.GuestFree(b);
        }
    }

    std::printf(g_fail == 0 ? "\nALL OK\n" : "\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
