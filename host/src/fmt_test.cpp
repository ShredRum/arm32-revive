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

#include "jni_call_words.h"
#include <dirent.h>

#include "guest/dispatch.h"
#include "guest/bsd_random.h"
#include "guest/elf_loader.h"
#include "guest/guest_memory.h"

namespace abr {
// Test-only bridge to the private import dispatcher (see dispatch.h).
struct RuntimeTestAccess {
    using RegsView = std::array<std::uint32_t, 16>;
    // Call the dispatcher with the given argument registers; returns r0.
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
    static std::size_t JniStringCount(Runtime& rt) { return rt.jni_strings_.size(); }
    static std::size_t UtfCharCount(Runtime& rt) { return rt.jni_utf_chars_.size(); }
    static void TrackUtfChars(Runtime& rt, std::uint32_t g) { rt.jni_utf_chars_.insert(g); }
    static std::size_t AssetMapSize(Runtime& rt) { return rt.asset_map_.size(); }
    static std::size_t LiveCount(Runtime& rt) { return rt.live_idx_.Size(); }
    static void SeedHostThreadTls(Runtime& rt, std::uint32_t tid, std::uint32_t slot,
                                  std::uint32_t key, std::uint32_t value) {
        rt.errno_slots_[tid] = slot;
        rt.tls_map_[(static_cast<std::uint64_t>(key) << 32) | tid] = value;
    }
    static bool HostThreadHasErrno(Runtime& rt, std::uint32_t tid) {
        return rt.errno_slots_.find(tid) != rt.errno_slots_.end();
    }
    static bool HostThreadHasTls(Runtime& rt, std::uint32_t tid, std::uint32_t key) {
        return rt.tls_map_.find((static_cast<std::uint64_t>(key) << 32) | tid) != rt.tls_map_.end();
    }
    static std::size_t FreeErrnoCount(Runtime& rt) { return rt.errno_free_.size(); }
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

int main() {
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
    abr::Runtime rt(mem, mod);

    auto put = [&](std::uint32_t addr, const std::string& s) {
        std::memcpy(rt.H<std::uint8_t>(addr), s.data(), s.size() + 1);
    };
    put(kScratch, "hello");
    put(kScratch + 0x40, "world");

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
        Expect("mutex usable after a raced destroy",
               std::to_string(abr::RuntimeTestAccess::Call(rt, 9, lock)), "0");
        Expect("mutex unlock after a raced destroy",
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

    // --- file bridge: fopen/fclose must free the 96-byte guest FILE, erase
    //     the fd mapping, and recycle the guest fd. A loop that reuses one
    //     slot keeps fd_map_/file_map_/next_guest_fd_ flat.
    {
        const std::uint32_t kFdPath = 0x10006000;
        const std::uint32_t kFdMode = 0x10006100;
        const std::string host_dir = "/tmp/opencode/abr-fdtest";
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
        const std::string bad_zip = "/tmp/opencode/abr-asset-bad.zip";
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
        const std::string host_dir = "/tmp/opencode/abr-dirtest";
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

    std::printf(g_fail == 0 ? "\nALL OK\n" : "\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
