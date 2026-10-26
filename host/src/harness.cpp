// Bring-up harness (native): loads the real ARM32 game .so, relocates it,
// then runs DT_INIT_ARRAY through Dynarmic with logging-stub SVC dispatch.
// Usage: harness <path-to-libAngryBirdsRio.so>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/halt_reason.h"
#include "guest/dispatch.h"
#include "guest/elf_loader.h"
#include "guest/guest_memory.h"
#include "guest/layout.h"
#include "guest/realgl.h"

namespace {
// Seconds before the watchdog aborts a stuck run; armed with alarm() all over
// this file. The old hardcoded message claimed 300s while alarm(kWatchdogSeconds) was used.
constexpr unsigned kWatchdogSeconds = 60;
volatile int g_current_init = -1;
volatile bool g_running = false;
std::uint32_t g_snap_region[64] = {};
// Load-time value of the literal-pool word the init loop watches. The old
// check compared against a Rio-specific constant (0x2200990d), which made
// every non-Rio title print "[corrupt]" after every init even though the
// image was intact. Baseline must come from the loaded image itself.
std::uint32_t g_snap_4de210 = 0;

void Watchdog(int) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "\n[watchdog] STUCK at init (%us) -- aborting\n",
                     kWatchdogSeconds);
    if (n > 0) {
        (void)write(STDOUT_FILENO, buf, static_cast<std::size_t>(n));
    }
    _exit(42);
}

// Heartbeat: samples guest PC every 5s so slow-vs-stuck is visible.
void Heartbeat(Dynarmic::A32::Jit* jit, abr::Runtime* rt) {
    std::uint32_t last_pc = 0;
    while (g_running) {
        sleep(5);
        if (!g_running) {
            break;
        }
        auto& regs = jit->Regs();
        std::printf("[beat] init #%d pc=%08x lr=%08x sp=%08x svc=%llu%s\n", g_current_init, regs[15],
                    regs[14], regs[13], (unsigned long long)rt->svc_count,
                    regs[15] == last_pc ? " (SAME AS LAST)" : "");
        std::fflush(stdout);
        last_pc = regs[15];
    }
}

// Calls into guest code with correct ARM/Thumb state (see cbnz_test saga:
// Dynarmic takes state from CPSR.T via SetCpsr, PC must be even).
void CallGuest(Dynarmic::A32::Jit& jit, std::uint32_t target) {
    if (target & 1u) {
        jit.SetCpsr(0x30);  // Thumb, user mode
        jit.Regs()[15] = target & ~std::uint32_t(1);
    } else {
        jit.SetCpsr(0x10);  // ARM, user mode
        jit.Regs()[15] = target;
    }
}

// Single-step runner with a last-PC ring (for JNI-death forensics; the init
// loop has its own inline copy). Returns the halt reason.
Dynarmic::HaltReason StepRun(Dynarmic::A32::Jit& jit, abr::Runtime& rt, abr::GuestMemory& mem,
                             std::uint32_t halt_va, int max_steps, bool full_trace) {
    std::uint32_t ring_pc[32] = {};
    std::uint32_t ring_r0[32] = {};
    unsigned rpos = 0;
    // NOTE: default is NOT-clean; only reaching halt_va reports UserDefined1.
    // (Cap-exhaustion previously faked clean passes.)
    Dynarmic::HaltReason hr = Dynarmic::HaltReason::UserDefined2;
    std::uint32_t prev_pc = 0xFFFFFFFFu;
    unsigned stall = 0;
    rt.stop_step_ = false;
    for (int s = 0; s < max_steps; s++) {
        if (rt.stop_step_) {
            hr = Dynarmic::HaltReason::UserDefined2;  // abnormal callback halt
            break;
        }
        const std::uint32_t pc = jit.Regs()[15];
        ring_pc[rpos % 32] = pc;
        ring_r0[rpos % 32] = jit.Regs()[0];
        rpos++;
        if (pc == halt_va) {
            hr = Dynarmic::HaltReason::UserDefined1;  // clean return
            break;
        }
        if (!rt.IsCodeAddress(pc & ~std::uint32_t(1))) {
            hr = Dynarmic::HaltReason::UserDefined2;
            break;
        }
        // Stall detector: after a callback-side halt (BAD mem, LOUD stub) the
        // JIT no longer advances; further Step()s risk backend asserts, so stop.
        if (pc == prev_pc) {
            if (++stall >= 4) {
                hr = Dynarmic::HaltReason::UserDefined2;
                break;
            }
        } else {
            stall = 0;
        }
        prev_pc = pc;
        if (full_trace) {
            std::uint32_t w = 0xDEADDEADu;
            if (rt.CheckRange(pc & ~std::uint32_t(3), 4, false)) {
                std::memcpy(&w, rt.H(pc & ~std::uint32_t(3)), 4);
            }
            std::printf("[jstep] pc=%08x mem=%08x r0=%08x r1=%08x r2=%08x lr=%08x\n", pc, w,
                        jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[14]);
        }
        // NOTE: Step() return intentionally ignored (mirrors the proven init
        // loop): single-steps report HaltReason::Step routinely; real halts
        // surface via pc checks below and callback-side HaltExecution.
        jit.Step();
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined3);
    }
    std::printf("jni step-trace last PCs:");
    for (unsigned i = 0; i < 32; i++) {
        std::printf(" %08x(r0=%08x)", ring_pc[(rpos + i) % 32], ring_r0[(rpos + i) % 32]);
    }
    std::printf("\n");
    return hr;
}

// Runs one guest Java_* export: regs=(env, 0, r2, r3) + stack words, lr=HALT.
Dynarmic::HaltReason RunOneJni(Dynarmic::A32::Jit& jit, abr::Runtime& rt, abr::GuestMemory& mem,
                               abr::LoadedModule& mod, std::uint32_t fn, std::uint32_t r2,
                               std::uint32_t r3, const std::vector<std::uint32_t>& stack_args = {}) {
    auto& regs = jit.Regs();
    regs.fill(0);
    regs[13] = abr::kStackTop - 0x100;
    regs[14] = mod.halt_va;
    regs[0] = rt.JniEnv();
    regs[1] = 0;
    regs[2] = r2;
    regs[3] = r3;
    // HARNESS_JSTK: caller passes resolved words (numbers, or byte-array
    // handles created via HARNESS_JBARRAY); pushed so stack[0] = first.
    if (!stack_args.empty()) {
        std::uint32_t n = (std::uint32_t)stack_args.size();
        if (n & 1) {
            n++;  // keep SP 8-aligned
        }
        regs[13] -= 4 * n;
        for (std::size_t i = 0; i < stack_args.size(); i++) {
            std::uint32_t w = stack_args[i];
            std::memcpy(mem.host(regs[13] + i * 4), &w, 4);
        }
        std::printf("[jni] +%zu stack args @%08x\n", stack_args.size(), regs[13]);
    }
    CallGuest(jit, fn);
    std::printf("[jni] calling @%08x env=%08x r2=%08x r3=%08x ... ", fn, regs[0], r2, r3);
    std::fflush(stdout);
    alarm(kWatchdogSeconds);
    Dynarmic::HaltReason hr;
    // HARNESS_JNISTEP (empty/1): step this call. HARNESS_JNISTEP=N: step only
    // calls with sequence index >= N (fast-forward earlier calls with Run()).
    static int jni_seq = 0;
    bool do_step = false;
    if (const char* se = std::getenv("HARNESS_JNISTEP")) {
        int from = 0;
        if (*se) {
            char* end = nullptr;
            long v = std::strtol(se, &end, 10);
            from = (end != se) ? (int)v : 0;
        }
        do_step = (jni_seq >= from);
    }
    jni_seq++;
    if (do_step) {
        int cap = 50000;
        if (const char* ce = std::getenv("HARNESS_STEPCAP")) {
            cap = std::atoi(ce);
        }
        hr = StepRun(jit, rt, mem, mod.halt_va, cap,
                     std::getenv("HARNESS_FULLTRACE") != nullptr);
    } else {
        hr = jit.Run();
    }
    alarm(0);
    std::printf("hr=%d r0=%08x (%u) svc=%llu\n", static_cast<int>(hr), regs[0], regs[0],
                (unsigned long long)rt.svc_count);
    if (std::getenv("HARNESS_REGS")) {
        std::printf("[regs] r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x "
                    "r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x sp=%08x lr=%08x\n",
                    regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
                    regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14]);
    }
    rt.SvcHistDump();
    // HARNESS_HEXDUMPFILE=<hexaddr>,<hexlen>,<path>: raw binary guest dump
    // to a host file (for extracting decrypted Lua sources etc.; no 1KB cap,
    // single post-mortem dump after the last JNI call... actually per call
    // like HEXDUMP; use the last one).
    if (const char* hf = std::getenv("HARNESS_HEXDUMPFILE")) {
        std::string s(hf);
        auto c1 = s.find(',');
        auto c2 = (c1 == std::string::npos) ? std::string::npos : s.find(',', c1 + 1);
        if (c1 != std::string::npos && c2 != std::string::npos) {
            std::uint32_t da =
                static_cast<std::uint32_t>(std::strtoul(s.substr(0, c1).c_str(), nullptr, 16));
            std::uint32_t dl = static_cast<std::uint32_t>(
                std::strtoul(s.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16));
            std::string path = s.substr(c2 + 1);
            if (dl > 0x100000) {
                dl = 0x100000;
            }
            if (rt.CheckRange(da, dl, false)) {
                FILE* f = std::fopen(path.c_str(), "wb");
                if (f) {
                    std::fwrite(rt.H<std::uint8_t>(da), 1, dl, f);
                    std::fclose(f);
                    std::printf("[hexdumpfile] %08x+%u -> %s\n", da, dl, path.c_str());
                }
            } else {
                std::printf("[hexdumpfile] BAD range %08x+%u\n", da, dl);
            }
        }
    }
    if (const char* hd = std::getenv("HARNESS_HEXDUMP")) {
        std::uint32_t da = static_cast<std::uint32_t>(std::strtoul(hd, nullptr, 16));
        std::uint32_t dl = 64;
        if (const char* c = std::strchr(hd, ',')) {
            dl = static_cast<std::uint32_t>(std::strtoul(c + 1, nullptr, 16));
        }
        if (dl > 1024) {
            dl = 1024;
        }
        if (rt.CheckRange(da, dl, false)) {
            const std::uint8_t* p = rt.H<std::uint8_t>(da);
            std::printf("[hexdump] %08x (%u bytes):\n", da, dl);
            for (std::uint32_t i = 0; i < dl; i += 16) {
                std::printf("[hexdump] %08x:", da + i);
                for (std::uint32_t j = i; j < i + 16 && j < dl; j++) {
                    std::printf(" %02x", p[j]);
                }
                std::printf("  |");
                for (std::uint32_t j = i; j < i + 16 && j < dl; j++) {
                    std::printf("%c", (p[j] >= 32 && p[j] < 127) ? (char)p[j] : '.');
                }
                std::printf("|\n");
            }
        } else {
            std::printf("[hexdump] BAD range %08x+%u\n", da, dl);
        }
    }
    // HARNESS_HEXFIND=<text>: scan the guest heap for an ASCII needle, print
    // up to 5 contexts. Finds formatted exception messages etc. post-mortem
    // (heap is bump, no reuse, so all iterations persist).
    if (const char* hf = std::getenv("HARNESS_HEXFIND")) {
        std::uint32_t lo = 0x10000000u, shown = 0;
        const std::uint32_t hi = rt.HeapTop();
        const std::size_t nl = std::strlen(hf);
        for (std::uint32_t a = lo; a + nl <= hi && shown < 5; a++) {
            if (!rt.CheckRange(a, nl, false)) {
                // Heap has mapping gaps (per-allocation commits); skip the
                // page instead of aborting the whole scan (the old `break`
                // silently missed everything above the first gap — e.g. the
                // image-format table at 0x1230xxxx).
                a = (a + 0xFFFu) & ~0xFFFu;
                a -= 1;  // the loop's a++ lands on the page start
                continue;
            }
            if (std::memcmp(rt.H(a), hf, nl) != 0) {
                continue;
            }
            shown++;
            std::printf("[hexfind] @%08x:\n", a);
            for (int k = -64; k < 128; k += 16) {
                if ((std::int64_t)a + k < (std::int64_t)lo) {
                    continue;
                }
                const std::uint32_t b = (std::uint32_t)((std::int64_t)a + k);
                if (!rt.CheckRange(b, 16, false)) {
                    break;
                }
                const std::uint8_t* p = rt.H<std::uint8_t>(b);
                std::printf("[hexfind] %08x:", b);
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
        if (!shown) {
            std::printf("[hexfind] \"%s\": no hits in heap range\n", hf);
        }
    }
    // HARNESS_HEXFINDHEX=<hexbytes>: same heap scan with a hex needle
    // (for pointer values containing NULs, e.g. vtable addrs like 0x65d560
    // as "60d56500"). Prints up to 5 contexts.
    if (const char* hx = std::getenv("HARNESS_HEXFINDHEX")) {
        std::string hxs(hx);
        std::vector<std::uint8_t> needle;
        for (std::size_t i = 0; i + 1 < hxs.size() && needle.size() < 64; i += 2) {
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
            int hi = hv(hxs[i]), lo = hv(hxs[i + 1]);
            if (hi < 0 || lo < 0) {
                break;
            }
            needle.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        if (!needle.empty()) {
            std::uint32_t lo = 0x10000000u, shown = 0;
            const std::uint32_t hi = rt.HeapTop();
            const std::size_t nl = needle.size();
            for (std::uint32_t a = lo; a + nl <= hi && shown < 5; a++) {
                if (!rt.CheckRange(a, nl, false)) {
                    a = (a + 0xFFFu) & ~0xFFFu;
                    a -= 1;
                    continue;
                }
                if (std::memcmp(rt.H(a), needle.data(), nl) != 0) {
                    continue;
                }
                shown++;
                std::printf("[hexfindhex] @%08x:\n", a);
                for (int k = -32; k < 64; k += 16) {
                    if ((std::int64_t)a + k < (std::int64_t)lo) {
                        continue;
                    }
                    const std::uint32_t b = (std::uint32_t)((std::int64_t)a + k);
                    if (!rt.CheckRange(b, 16, false)) {
                        break;
                    }
                    const std::uint8_t* p = rt.H<std::uint8_t>(b);
                    std::printf("[hexfindhex] %08x:", b);
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
            if (!shown) {
                std::printf("[hexfindhex] \"%s\": no hits in heap range\n", hx);
            }
        }
    }
    // HARNESS_JNIBT=1: guest-stack backtrace. Buckets code pointers per 64KB.
    if (std::getenv("HARNESS_JNIBT")) {
        std::uint32_t sp = regs[13] & ~std::uint32_t(3);
        if (sp < abr::kStackBase) {
            sp = abr::kStackBase;
        }
        std::uint64_t buckets[112] = {};
        std::uint32_t n = 0;
        for (std::uint32_t a = sp; a < abr::kStackTop; a += 4) {
            if (!rt.CheckRange(a, 4, false)) {
                break;
            }
            std::uint32_t v = 0;
            std::memcpy(&v, mem.host(a), 4);
            const std::uint32_t base = v & ~std::uint32_t(1);
            if (base >= 0x1000 && base < 0x653d1c && (base & 3u) == 0) {
                buckets[base >> 16]++;
                n++;
                if (n <= 40) {
                    std::printf("[jnibt] %08x%s\n", v, (v & 1u) ? " T" : " A");
                }
            }
        }
        std::printf("[jnibt] %u code ptrs; top buckets:", n);
        for (int i = 0; i < 112; i++) {
            if (buckets[i] > n / 20) {
                std::printf(" %05x:%llu", i, (unsigned long long)buckets[i]);
            }
        }
        std::printf("\n");
        // Distinct addresses inside the dominant bucket (cycle identification).
        for (int bi = 0; bi < 112; bi++) {
            if (buckets[bi] == 0 || buckets[bi] <= n / 20) {
                continue;
            }
            std::uint32_t seen_v[16] = {};
            std::uint64_t seen[16] = {};
            int nseen = 0;
            for (std::uint32_t a = sp; a < abr::kStackTop; a += 4) {
                if (!rt.CheckRange(a, 4, false)) {
                    break;
                }
                std::uint32_t v = 0;
                std::memcpy(&v, mem.host(a), 4);
                if ((v & ~std::uint32_t(1)) >= (std::uint32_t)bi * 0x10000 &&
                    (v & ~std::uint32_t(1)) < (std::uint32_t)(bi + 1) * 0x10000) {
                    int k = 0;
                    for (; k < nseen; k++) {
                        if (seen_v[k] == v) {
                            break;
                        }
                    }
                    if (k == nseen && nseen < 16) {
                        seen_v[nseen] = v;
                        seen[nseen] = 1;
                        nseen++;
                    } else if (k < nseen) {
                        seen[k]++;
                    }
                }
            }
            for (int k = 0; k < nseen; k++) {
                std::printf("[jnibt] distinct %08x x%llu\n", seen_v[k],
                            (unsigned long long)seen[k]);
            }
        }
    }
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
    return hr;
}
}  // namespace

// D2 self-test (2026-10-18): differential test of abr::LiveHeapIndex against the
// std::map it replaced -- the exact semantics the live consumers depend on
// (free-of-unknown whitelist, realloc guard, audio-pool liveness, JNI bound).
// Run with HARNESS_LIVEIDX_TEST=1; needs no guest .so. Returns 0 on success.
int LiveIdxSelfTest() {
    int fails = 0;
    std::printf("[liveidx] differential test vs std::map ...\n");
    {
        std::mt19937 rng(12345);
        abr::LiveHeapIndex idx;
        std::map<std::uint32_t, std::uint32_t> ref;
        for (int round = 0; round < 400000; ++round) {
            const int op = (int)(rng() % 100);
            const std::uint32_t key = 0x10000000u + (rng() % 4096) * 8u;
            if (op < 55) {
                const std::uint32_t sz = 8 + rng() % 4096;
                idx.Insert(key, sz);
                ref[key] = sz;
            } else if (op < 85) {
                const std::uint32_t a = idx.Erase(key);
                auto it = ref.find(key);
                const std::uint32_t b = (it == ref.end()) ? 0 : it->second;
                if (a != b) {
                    ++fails;
                    std::printf("FAIL erase key=%08x new=%u old=%u\n", key, a, b);
                }
                if (it != ref.end()) {
                    ref.erase(it);
                }
            } else {
                const std::uint32_t a = idx.Find(key);
                auto it = ref.find(key);
                const std::uint32_t b = (it == ref.end()) ? 0 : it->second;
                if (a != b) {
                    ++fails;
                    std::printf("FAIL find key=%08x new=%u old=%u\n", key, a, b);
                }
            }
            if (idx.Size() != ref.size()) {
                ++fails;
                std::printf("FAIL size new=%zu old=%zu\n", idx.Size(), ref.size());
            }
        }
        std::printf("[liveidx] 400k ops: live=%zu cap=%zu grown=%u fails=%d\n", idx.Size(),
                    idx.Capacity(), idx.Grown(), fails);
    }
    std::printf("[liveidx] grow/shrink stress ...\n");
    {
        abr::LiveHeapIndex idx;
        const std::uint32_t n = 200000;
        for (std::uint32_t i = 0; i < n; ++i) {
            idx.Insert(0x10000000u + i * 8u, 24 + (i % 512));
        }
        if (idx.Size() != n) {
            ++fails;
            std::printf("FAIL stress size=%zu want=%u\n", idx.Size(), n);
        }
        const std::size_t big = idx.Capacity();
        for (std::uint32_t i = 0; i < n; ++i) {
            if (idx.Erase(0x10000000u + i * 8u) != 24 + (i % 512)) {
                ++fails;
                std::printf("FAIL stress erase %u\n", i);
            }
        }
        if (idx.Size() != 0) {
            ++fails;
            std::printf("FAIL stress not empty: %zu\n", idx.Size());
        }
        idx.Insert(0x50000000u, 32);  // one insert triggers the idle shrink
        if (idx.Capacity() >= big) {
            ++fails;
            std::printf("FAIL shrink: cap %zu not below %zu\n", idx.Capacity(), big);
        }
        if (idx.Find(0x50000000u) != 32 || idx.Size() != 1) {
            ++fails;
            std::printf("FAIL shrink lost state (find=%u size=%zu)\n", idx.Find(0x50000000u),
                        idx.Size());
        }
        std::printf("[liveidx] 200k insert/erase: cap %zu -> %zu, survives (fails=%d)\n", big,
                    idx.Capacity(), fails);
    }
    std::printf("[liveidx] tombstone churn + update/resurrect ...\n");
    {
        abr::LiveHeapIndex idx;
        const std::size_t cap0 = idx.Capacity();
        for (int r = 0; r < 200000; ++r) {
            const std::uint32_t k = 0x20000000u + (r % 1000) * 8u;
            idx.Insert(k, 32);
            if (idx.Erase(k) != 32) {
                ++fails;
            }
        }
        if (idx.Size() != 0 || idx.Capacity() != cap0) {
            ++fails;
            std::printf("FAIL churn: size=%zu cap %zu -> %zu\n", idx.Size(), cap0, idx.Capacity());
        }
        idx.Insert(0x30000000u, 100);
        idx.Insert(0x30000000u, 200);  // update in place
        if (idx.Find(0x30000000u) != 200 || idx.Size() != 1) {
            ++fails;
            std::printf("FAIL update: find=%u size=%zu\n", idx.Find(0x30000000u), idx.Size());
        }
        if (idx.Erase(0x30000000u) != 200 || idx.Erase(0x30000000u) != 0) {
            ++fails;
            std::printf("FAIL erase/double-erase\n");
        }
        idx.Insert(0x30000000u, 50);  // resurrect over a tombstone
        if (idx.Find(0x30000000u) != 50 || idx.Size() != 1) {
            ++fails;
            std::printf("FAIL resurrect: find=%u size=%zu\n", idx.Find(0x30000000u), idx.Size());
        }
        std::printf("[liveidx] churn/update/resurrect done (fails=%d)\n", fails);
    }
    std::printf("[liveidx] %s (fails=%d)\n", fails == 0 ? "SELFTEST PASS" : "SELFTEST FAIL",
                fails);
    return fails == 0 ? 0 : 1;
}

// Screenshot directory, resolved in main() from the guest .so path (the
// filmstrip code below has no access to argv).
std::string g_shotdir = "work";

// Find "<root>/games/<id>" from a path that contains "/games/<id>/", e.g. the
// guest .so at <root>/games/rio/work/apk_prod/assets/guestlib/armeabi-v7a/x.so.
// Returns "" when the path has no games/<id> component (caller falls back).
std::string GameDirFrom(const std::string& path) {
    static const std::string ksep = "/games/";
    std::size_t id_start = std::string::npos;
    const std::size_t at = path.rfind(ksep);
    if (at != std::string::npos) {
        id_start = at + ksep.size();
    } else if (path.rfind("games/", 0) == 0) {  // relative path from the repo root
        id_start = 6;
    } else {
        return {};
    }
    const std::size_t id_end = path.find('/', id_start);
    if (id_end == std::string::npos) {
        return {};
    }
    return path.substr(0, id_end);  // <root>/games/<id>
}

// First *.apk in a directory (the pristine Rovio APK of a game).
std::string FirstApkIn(const std::string& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return {};
    }
    for (const auto& e : it) {
        if (e.path().extension() == ".apk") {
            return e.path().string();
        }
    }
    return {};
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // logs must survive _exit()
    // Run fingerprint (relthrow-class lesson: lost envs waste runs): log the
    // full HARNESS_* env so any log is reproducible from its head.
    {
        extern char** environ;
        std::printf("[env]");
        for (char** e = environ; *e; e++) {
            std::string s(*e);
            if (s.rfind("HARNESS_", 0) == 0) {
                std::printf(" %s", s.c_str());
            }
        }
        std::printf("\n");
    }
    // TEMP-DIAG: host memcmp sanity (wild memcmp3 values investigation).
    {
        int t1 = std::memcmp("min", "for", 3);
        int t2 = std::memcmp("PVR", "PVR", 3);
        int t3 = std::memcmp("PVR", "BMP", 3);
        std::printf("[memselftest] %d %d %d (want 7 0 14)\n", t1, t2, t3);
    }
    // D2 self-test: no guest .so needed, so it runs before argv is checked.
    if (std::getenv("HARNESS_LIVEIDX_TEST")) {
        return LiveIdxSelfTest();
    }
    if (argc < 2) {
        std::printf("usage: harness <libAngryBirdsRio.so>\n");
        return 2;
    }

    abr::GuestMemory mem;
    if (!mem.ok()) {
        std::printf("guest reservation failed\n");
        return 1;
    }
    abr::ElfLoader loader(mem);
    abr::LoadedModule mod;
    std::string err;
    if (!loader.Load(argv[1], mod, err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("loaded %s: %zu imports (%zu pending data), %zu init, %zu fini, halt=%08x\n",
                mod.path.c_str(), mod.imports.size(), mod.pending_data.size(), mod.init_array.size(),
                mod.fini_array.size(), mod.halt_va);
    abr::CheckGuestBuildId(mod);
    // Temporary GOT audit: JUMP_SLOT region must not retain file values.
    for (std::uint32_t a : {0x671000u, 0x671010u, 0x671018u, 0x671020u}) {
        std::uint32_t v = 0;
        std::memcpy(&v, mem.host(a), 4);
        std::printf("[got] %08x = %08x\n", a, v);
    }
    for (const auto& pd : mod.pending_data) {
        std::printf("[pending] loc=%08x idx=%u (%s) addend=%08x\n", pd.loc_va, pd.import_idx,
                    mod.imports[pd.import_idx].name.c_str(), pd.addend);
    }
    // HARNESS_TRAMPMAP=1: dump import index -> name -> trampoline VA (for
    // attributing raw trampoline PCs from TRANSLOG backtraces).
    if (std::getenv("HARNESS_TRAMPMAP")) {
        for (std::size_t i = 0; i < mod.imports.size(); i++) {
            std::printf("[trampmap] %zu %s tramp=%08x\n", i, mod.imports[i].name.c_str(),
                        mod.imports[i].tramp_va);
        }
    }
    // Snapshot of the mystery region: what is at VA 0x672010 right after load?
    {
        std::uint32_t v = 0;
        std::memcpy(&v, mem.host(0x672010), 4);
        std::printf("[snap] VA 00672010 after load = %08x (base=%p)\n", v, (void*)mem.base());
        std::uint32_t w = 0;
        std::memcpy(&w, mem.host(0x4de210), 4);
        g_snap_4de210 = w;
        std::printf("[snap] VA 004de210 after load = %08x\n", w);
        for (int i = 0; i < 64; i++) {
            std::memcpy(&g_snap_region[i], mem.host(0x672000 + i * 4), 4);
        }
    }

    abr::Runtime rt(mem, mod);
    rt.AddModuleSegments(mod);
    // Canary + errno slot for the main thread (before InitDataImports: the
    // guard variable cell reuses this canary value).
    std::random_device rd;
    const std::uint32_t canary = (static_cast<std::uint32_t>(rd()) << 1) | 1u;
    std::memcpy(mem.host(abr::kGuestDataBase + 0), &canary, 4);
    rt.InitDataImports();  // ctype tables + data-import slots (needs mappings)
    rt.ProtectRelro(mod);

    // File bridge root: HARNESS_FILES=<hostdir>, else <root>/games/<id>/work/files
    // derived from the guest .so path.
    const std::string game_dir = GameDirFrom(argv[1]);
    {
        std::string hostdir;
        if (const char* e = std::getenv("HARNESS_FILES")) {
            hostdir = e;
        } else if (!game_dir.empty()) {
            hostdir = game_dir + "/work/files";
        } else {
            std::string p = argv[1];
            for (int i = 0; i < 3; i++) {
                const std::size_t s = p.rfind('/');
                if (s == std::string::npos) {
                    break;
                }
                p = p.substr(0, s);
            }
            hostdir = p + "/files";
        }
        std::error_code ec;
        std::filesystem::create_directories(hostdir, ec);
        if (ec) {
            std::printf("[files] WARNING: cannot create %s (%s)\n", hostdir.c_str(),
                        ec.message().c_str());
        }
        rt.SetFilesDir("/data/data/com.rovio.angrybirdsrio/files", hostdir);
    }
    g_shotdir = game_dir.empty() ? "work" : game_dir + "/work";

    // APK path for the AAssetManager bridge: HARNESS_APK, else the pristine APK
    // at <root>/games/<id>/original/.
    {
        std::string apk;
        if (const char* e = std::getenv("HARNESS_APK")) {
            apk = e;
        } else if (!game_dir.empty()) {
            apk = FirstApkIn(game_dir + "/original");
        }
        if (apk.empty()) {
            std::printf("[apk] WARNING: no APK found (set HARNESS_APK)\n");
        }
        rt.SetApkPath(apk);
    }

    // (Canary written above, before InitDataImports.)
    Dynarmic::A32::UserCallbacks* cb = nullptr;
    (void)cb;
    abr::GuestCallbacks callbacks(&rt);
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = &callbacks;
    // JIT mode: shipping default is all_safe_optimizations (measured ~17.5%
    // less CPU, PLAN.md C1); env NOOPT/SAFEOPTS is handled by JitSafeOpts().
    cfg.optimizations =
        abr::JitSafeOpts() ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    cfg.enable_cycle_counting = abr::kJitCycleCounting;
    // Fastmem prototype (HARNESS_FASTMEM=1): direct host access to the 1:1
    // 4GB reservation; faults fall back to callbacks (BAD halting
    // preserved via slow path). Diags that sniff callbacks (WP/ScanWP/
    // HEXFIND/memcmp3) go blind under fastmem. Default off.
    if (abr::JitFastmemOn()) {
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(rt.mem().base());
    }
    std::size_t main_cpu = 0;
    rt.AcquireCpuId(&main_cpu);
    cfg.global_monitor = &rt.Monitor();
    cfg.processor_id = main_cpu;
    Dynarmic::A32::Jit jit(cfg);
    rt.set_jit(&jit);
    rt.set_callbacks(&callbacks);  // worker Jits share these callbacks

    // Run init functions one by one, LR = HALT trampoline.
    std::signal(SIGALRM, Watchdog);
    g_running = true;
    std::thread beat(Heartbeat, &jit, &rt);
    beat.detach();
    std::size_t ok_count = 0;
    // HARNESS_SKIP=num[,num...]: skip init indices (bring-up isolation).
    // HARNESS_DEFER=num: run that init LAST (dependency probe).
    bool skip[128] = {};
    int defer = -1;
    if (const char* e = std::getenv("HARNESS_SKIP")) {
        const char* p = e;
        while (*p) {
            int v = std::atoi(p);
            if (v >= 0 && v < 128) {
                skip[v] = true;
            }
            while (*p && *p != ',') {
                p++;
            }
            if (*p == ',') {
                p++;
            }
        }
    }
    if (const char* e = std::getenv("HARNESS_DEFER")) {
        defer = std::atoi(e);
    }
    // Execution order: file order, with `defer` moved last (dependency probe).
    std::vector<std::size_t> order;
    for (std::size_t k = 0; k < mod.init_array.size(); k++) {
        if ((int)k != defer) {
            order.push_back(k);
        }
    }
    if (defer >= 0 && (std::size_t)defer < mod.init_array.size()) {
        order.push_back((std::size_t)defer);
    }
    for (std::size_t k : order) {
        if (mod.init_array[k] == 0) {
            // A NULL slot of the guest's DT_INIT_ARRAY: kept in the vector so
            // the indices match the guest's, but there is nothing to run.
            continue;
        }
        if (skip[k]) {
            std::printf("[init] #%zu @%08x SKIPPED\n", k, mod.init_array[k]);
            ok_count++;
            continue;
        }
        auto& regs = jit.Regs();
        regs.fill(0);
        regs[13] = abr::kStackTop - std::uint32_t(k * 16);  // keep SP 8-aligned
        regs[14] = mod.halt_va;
        CallGuest(jit, mod.init_array[k]);
        const std::uint64_t svc_before = rt.svc_count;
        std::printf("[init] #%zu @%08x ... ", k, mod.init_array[k]);
        std::fflush(stdout);
        g_current_init = static_cast<int>(k);
        alarm(300);
        Dynarmic::HaltReason hr;
        // Single-step trace mode (HARNESS_STEP=<init# or 'all'>): records last
        // PCs to catch wild jumps.
        const char* step_env = std::getenv("HARNESS_STEP");
        const bool do_step =
            step_env && (std::string(step_env) == "all" || std::atoi(step_env) == (int)k);
        if (do_step) {
            std::uint32_t ring_pc[32] = {};
            std::uint32_t ring_r0[32] = {};
            std::uint32_t ring_r2[32] = {};
            std::uint32_t ring_r12[32] = {};
            unsigned rpos = 0;
            hr = Dynarmic::HaltReason::UserDefined2;  // cap-exhaustion is NOT clean
            const bool full_trace = std::getenv("HARNESS_FULLTRACE") != nullptr;
            rt.stop_step_ = false;
            for (int s = 0; s < 50000; s++) {
                const std::uint32_t pc = jit.Regs()[15];
                ring_pc[rpos % 32] = pc;
                ring_r0[rpos % 32] = jit.Regs()[0];
                ring_r2[rpos % 32] = jit.Regs()[2];
                ring_r12[rpos % 32] = jit.Regs()[12];
                rpos++;
                if (pc == mod.halt_va) {
                    hr = Dynarmic::HaltReason::UserDefined1;  // clean return
                    break;
                }
                if (!rt.IsCodeAddress(pc & ~std::uint32_t(1))) {
                    hr = Dynarmic::HaltReason::UserDefined2;
                    break;
                }
                if (full_trace) {
                    std::uint32_t w = 0xDEADDEADu;
                    if (rt.CheckRange(pc & ~std::uint32_t(3), 4, false)) {
                        std::memcpy(&w, rt.H(pc & ~std::uint32_t(3)), 4);
                    }
                    std::printf("[step] pc=%08x mem=%08x r0=%08x r1=%08x r2=%08x r12=%08x lr=%08x\n",
                                pc, w, jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                                jit.Regs()[12], jit.Regs()[14]);
                }
                jit.Step();
                if (rt.stop_step_) {
                    hr = Dynarmic::HaltReason::UserDefined2;
                    break;
                }
                jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
                jit.ClearHalt(Dynarmic::HaltReason::UserDefined3);
            }
            std::printf("step-trace last PCs:");
            for (unsigned i = 0; i < 32; i++) {
                std::printf(" %08x(r0=%08x r2=%08x r12=%08x)", ring_pc[(rpos + i) % 32],
                            ring_r0[(rpos + i) % 32], ring_r2[(rpos + i) % 32],
                            ring_r12[(rpos + i) % 32]);
            }
            std::printf("\n");
        } else {
            hr = jit.Run();
        }
        alarm(0);
        g_current_init = -1;
        std::printf("hr=%d (+%llu svc)\n", static_cast<int>(hr),
                    (unsigned long long)(rt.svc_count - svc_before));
        std::fflush(stdout);
        // Corruption watch: literal pool word must survive all inits.
        {
            std::uint32_t w = 0;
            std::memcpy(&w, mem.host(0x4de210), 4);
            if (w != g_snap_4de210) {
                std::printf("[corrupt] VA 004de210 = %08x after init #%zu (load=%08x)\n", w, k,
                            g_snap_4de210);
            }
        }
        if (hr == Dynarmic::HaltReason::UserDefined1) {
            ok_count++;
        } else {
            std::printf("[init] #%zu @%08x HALTED hr=%d regs: r0=%08x lr=%08x\n", k,
                        mod.init_array[k], static_cast<int>(hr), regs[0], regs[14]);
            // Death-state snapshot: key words that feed the fatal path.
            for (std::uint32_t a : {0x4de210u, 0x672010u, 0x1aed88u, 0x66ffccu, 0x66e668u}) {
                std::uint32_t v = 0xDEADDEADu;
                if (rt.CheckRange(a, 4, false)) {
                    std::memcpy(&v, mem.host(a), 4);
                }
                std::printf("[death] *(%08x) = %08x\n", a, v);
            }
            break;
        }
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
    }
    std::printf("init done: %zu/%zu clean, svc=%llu halts=%llu\n", ok_count, mod.init_array.size(),
                (unsigned long long)rt.svc_count, (unsigned long long)rt.halt_count);
    if (ok_count != mod.init_array.size()) {
        // Diff the mystery region vs post-load snapshot: did any filler run?
        int ndiff = 0;
        for (int i = 0; i < 64; i++) {
            std::uint32_t v = 0;
            std::memcpy(&v, mem.host(0x672000 + i * 4), 4);
            if (v != g_snap_region[i]) {
                if (ndiff < 10) {
                    std::printf("[regdiff] %08x: load=%08x now=%08x\n", 0x672000 + i * 4,
                                g_snap_region[i], v);
                }
                ndiff++;
            }
        }
        std::printf("[regdiff] total changed words: %d\n", ndiff);
        auto& regs = jit.Regs();
        std::printf("[bt] sp=%08x r11=%08x lr=%08x\n", regs[13], regs[11], regs[14]);
        std::printf("[bt] r4-r10:");
        for (int r = 4; r <= 10; r++) {
            std::printf(" %08x", regs[r]);
        }
        std::printf("\n[bt] stack scan:");
        const std::uint32_t sp = regs[13] & ~std::uint32_t(3);
        int found = 0;
        for (std::uint32_t a = sp; a < abr::kStackTop && found < 24; a += 4) {
            if (!rt.CheckRange(a, 4, false)) {
                break;
            }
            std::uint32_t v = 0;
            std::memcpy(&v, mem.host(a), 4);
            const std::uint32_t base = v & ~std::uint32_t(1);
            if (base >= 0x1000 && base < 0x653d1c && (v & 1u) == 1u) {
                std::printf(" %08x", v);
                found++;
            }
        }
        std::printf("\n");
    }
    // JNI smoke test: HARNESS_JNI=<hexaddr[,hexaddr...]> calls guest Java_*
    // exports in order after inits (app startup: config, init, resize, update,
    // render). HARNESS_JNI2=<hexaddr> appends one more (backward compat).
    // Args: call[0] r2 = HARNESS_JSTR jstring; calls[1..] r2/r3 = JA2/JA3
    // (single call without JSTR keeps legacy JA2/JA3 behavior).
    // HARNESS_ONLOAD=<hexaddr> (default real JNI_OnLoad) runs first with
    // r0=mock JavaVM* (sole writer of JavaVM slot 0x6747D0); NOONLOAD=1 skips.
    if (const char* e = std::getenv("HARNESS_JNI")) {
        // Call list: comma-separated addrs + HARNESS_JNI2 appended.
        std::vector<std::uint32_t> fns;
        {
            const char* p = e;
            while (*p) {
                while (*p == ' ' || *p == ',') {
                    p++;
                }
                if (!*p) {
                    break;
                }
                fns.push_back(static_cast<std::uint32_t>(std::strtoul(p, nullptr, 16)));
                while (*p && *p != ',') {
                    p++;
                }
            }
        }
        if (const char* e2 = std::getenv("HARNESS_JNI2")) {
            fns.push_back(static_cast<std::uint32_t>(std::strtoul(e2, nullptr, 16)));
        }
        if (fns.empty()) {
            std::printf("[jni] empty call list\n");
            _exit(1);
        }
        for (std::uint32_t f : fns) {
            if (!rt.IsCodeAddress(f & ~std::uint32_t(1))) {
                std::printf("[jni] BAD addr %08x\n", f);
                _exit(1);
            }
        }
        if (!std::getenv("HARNESS_NOONLOAD")) {
            std::uint32_t onload = 0xc2db4;  // real JNI_OnLoad (dynsym-verified)
            if (const char* o = std::getenv("HARNESS_ONLOAD")) {
                onload = static_cast<std::uint32_t>(std::strtoul(o, nullptr, 16));
            }
            if (!rt.IsCodeAddress(onload & ~std::uint32_t(1))) {
                std::printf("[jni] BAD onload addr %08x\n", onload);
                _exit(1);
            }
            auto& regs = jit.Regs();
            regs.fill(0);
            regs[13] = abr::kStackTop - 0x100;
            regs[14] = mod.halt_va;
            regs[0] = rt.Jvm();
            regs[1] = 0;
            CallGuest(jit, onload);
            std::printf("[jni] JNI_OnLoad @%08x jvm=%08x ... ", onload, regs[0]);
            std::fflush(stdout);
            alarm(kWatchdogSeconds);
            Dynarmic::HaltReason ohr = jit.Run();
            alarm(0);
            std::printf("hr=%d r0=%08x svc=%llu\n", static_cast<int>(ohr), regs[0],
                        (unsigned long long)rt.svc_count);
            jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            if (ohr != Dynarmic::HaltReason::UserDefined1) {
                std::printf("[jni] OnLoad failed, stopping\n");
                _exit(1);
            }
        }
        std::uint32_t ja2 = 0, ja3 = 0;
        if (const char* v = std::getenv("HARNESS_JA2")) {
            ja2 = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 16));
        }
        if (const char* v = std::getenv("HARNESS_JA3")) {
            ja3 = static_cast<std::uint32_t>(std::strtoul(v, nullptr, 16));
        }
        std::uint32_t jstr = 0;
        if (const char* s = std::getenv("HARNESS_JSTR")) {
            jstr = rt.NewStringUTF(s);
            std::printf("[jni] jstring \"%s\" -> %08x\n", s, jstr);
        }
        // HARNESS_JBARRAY=<len>: guest byte[] for stack args; HARNESS_JSTK is a
        // comma list of hex words with "A" = that array (e.g. mixdata: "A,1000").
        std::uint32_t jbarray = 0;
        if (const char* bl = std::getenv("HARNESS_JBARRAY")) {
            jbarray = rt.NewByteArray(static_cast<std::uint32_t>(std::strtoul(bl, nullptr, 16)));
            std::printf("[jni] byte array -> %08x\n", jbarray);
        }
        std::vector<std::uint32_t> jstk;
        if (const char* st = std::getenv("HARNESS_JSTK")) {
            std::string cur;
            auto flush = [&]() {
                if (cur.empty()) {
                    return;
                }
                if (cur == "A" || cur == "a") {
                    jstk.push_back(jbarray);
                } else {
                    jstk.push_back(static_cast<std::uint32_t>(std::strtoul(cur.c_str(), nullptr, 16)));
                }
                cur.clear();
            };
            for (const char* p = st; *p; p++) {
                if (*p == ',') {
                    flush();
                } else if (*p != ' ') {
                    cur += *p;
                }
            }
            flush();
        }
        bool ok = true;
        for (std::size_t i = 0; i < fns.size() && ok; i++) {
            // Call[0]: JSTR jstring (or legacy JA2/JA3 when alone without JSTR).
            // Calls[1..]: JA2/JA3 (dims for init/resize).
            std::uint32_t r2 = 0, r3 = 0;
            if (i == 0) {
                r2 = (jstr != 0) ? jstr : ja2;
                r3 = (jstr != 0) ? 0 : ja3;
            } else {
                r2 = ja2;
                r3 = ja3;
            }
            Dynarmic::HaltReason hr = RunOneJni(jit, rt, mem, mod, fns[i], r2, r3, jstk);
            ok = (hr == Dynarmic::HaltReason::UserDefined1);
            // HARNESS_PACEMS=N: sleep N ms after each JNI call (deterministic
            // pacing so concurrent workers reach completion before main exits
            // via _exit; sprinting main otherwise murders them mid-flight).
            {
                static int pacems = -1;
                if (pacems < 0) {
                    pacems = 0;
                    if (const char* e = std::getenv("HARNESS_PACEMS")) {
                        pacems = std::atoi(e);
                    }
                }
                if (pacems > 0 && pacems < 10000) {
                    ::usleep((useconds_t)(pacems * 1000));
                }
            }
            // HARNESS_TLOG=1: host monotonic ms at each JNI call boundary
            // (frame pacing diagnostics: what dt does the game observe?).
            if (std::getenv("HARNESS_TLOG")) {
                struct timespec ts {};
                ::clock_gettime(CLOCK_MONOTONIC, &ts);
                std::printf("[tlog] fn=%08x t=%lld.%03lld r0=%08x\n", fns[i],
                            (long long)ts.tv_sec, (long long)(ts.tv_nsec / 1000000),
                            jit.Regs()[0]);
            }
            // HARNESS_SHOT_EVERY=N: filmstrip screenshot every Nth call.
            {
                static int every = -1;
                if (every < 0) {
                    every = 0;
                    if (const char* se = std::getenv("HARNESS_SHOT_EVERY")) {
                        every = std::atoi(se);
                    }
                }
                if (every > 0 && (i + 1) % (std::size_t)every == 0) {
                    char path[256];
                    const std::string p = g_shotdir + "/shot";
                    std::snprintf(path, sizeof(path), "%s%03zu.ppm", p.c_str(), i + 1);
                    rt.RealGLBackend()->Screenshot(path, 800, 480);
                    std::printf("[realgl] filmstrip -> %s\n", path);
                }
            }
        }
        // HARNESS_SCREENSHOT=1: capture the real-GL drawable (needs
        // HARNESS_REALGL=1) to <games/<id>/work/shot.ppm after the JNI sequence.
        if (std::getenv("HARNESS_SCREENSHOT")) {
            const std::string shot = g_shotdir + "/shot.ppm";
            rt.RealGLBackend()->Screenshot(shot.c_str(), 800, 480);
        }
        rt.ThreadCensus();
        {
            // TEMP-DIAG (heap pressure): bump top at exit (leak-only free;
            // ceiling was 256MB (now 1GB) -> NULL mallocs -> null+off faults.
            std::printf("[heap] top=%08x (base %08x, ceiling %08x)\n", rt.HeapTop(),
                        abr::kHeapBase, abr::kHeapBase + abr::kHeapSize);
        }
        // HARNESS_SHOW=N: keep the live GL window open N seconds (viewing).
        if (const char* sh = std::getenv("HARNESS_SHOW")) {
            int secs = std::atoi(sh);
            if (secs > 0 && secs < 600) {
                std::printf("[harness] window open %ds\n", secs);
                std::fflush(stdout);
                sleep((unsigned)secs);
            }
        }
        _exit(ok ? 0 : 1);
    }
    _exit(ok_count == mod.init_array.size() ? 0 : 1);
}
