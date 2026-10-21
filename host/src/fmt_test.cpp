// Unit test for the guest printf formatter (Runtime::ExpandPrintf).
//
// The guests are GCC/arm-linux-gnueabi builds (base PCS) on a 32-bit ARM
// target, so the variadic ABI is:
//
//   size_t / unsigned int / int / long / ptrdiff_t  = 4 bytes  -> ONE slot
//   long long / intmax_t                             = 8 bytes  -> an EVEN
//                                                       register pair
//   double                                           = 8 bytes  -> an EVEN
//                                                       register pair
//   va_list (base PCS)                               = ONE WORD (a pointer
//                                                       to a flat area); the
//                                                       consumer reads AT the
//                                                       pointer and advances
//                                                       by 4 (int) or 8
//                                                       (double-word); the
//                                                       CALLER aligns.
//
// The facts below were established by compiling with -mabi=apcs-gnu and
// reading the disassembly, not by assumption.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "guest/dispatch.h"
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

    // --- 8-byte integer argument: TWO consecutive core registers (base PCS:
    //     no even-pair rule -- see abi_basepcs.c). With ai == 1 the pair is
    //     r1:r2; with ai == 3 the registers are gone and the argument is on
    //     the stack, at an 8-byte aligned address.
    {
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0x55667788u;  // r1: low word
        st.gargs[2] = 0x11223344u;  // r2: high word
        st.ai = 1;
        Expect("%jd from r1:r2", rt.ExpandPrintf("%jd", st), "1234605616436508552");
    }
    {
        abr::Runtime::GuestFmtArgs st{};
        const std::uint64_t v = 0x1122334455667788ull;
        std::memcpy(rt.H<std::uint8_t>(kStack), &v, 8);
        st.gargs[0] = 0;
        st.ai = 3;  // r0..r2 taken -> the argument is on the stack
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
        // %lld is 8 bytes too, and %ld is 4.
        abr::Runtime::GuestFmtArgs st{};
        st.gargs[0] = 0;
        st.gargs[1] = 0x55667788u;  // r1: low word
        st.gargs[2] = 0x11223344u;  // r2: high word
        st.ai = 1;
        Expect("%lld from r1:r2", rt.ExpandPrintf("%lld", st), "1234605616436508552");
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

    // --- a double takes the next two free words as well (base PCS).
    {
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
        // One named argument only: the pair is r1:r2 (r1 is NOT skipped).
        abr::Runtime::GuestFmtArgs st{};
        const double d = 2.25;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, 8);
        st.gargs[0] = 0;
        st.gargs[1] = static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);  // r1: low word
        st.gargs[2] = static_cast<std::uint32_t>(bits >> 32);          // r2: high word
        st.ai = 1;
        Expect("%f from r1:r2", rt.ExpandPrintf("%f", st), "2.250000");
    }

    // --- base PCS va_list: the consumer reads AT the pointer and advances
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

    std::printf(g_fail == 0 ? "\nALL OK\n" : "\n%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
