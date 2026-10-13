// Bring-up harness (native): loads the real ARM32 game .so, relocates it,
// then runs DT_INIT_ARRAY through Dynarmic with logging-stub SVC dispatch.
// Usage: harness <path-to-libAngryBirdsRio.so>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <filesystem>
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
volatile int g_current_init = -1;
volatile bool g_running = false;
std::uint32_t g_snap_region[64] = {};
// HARNESS_LOGINAT1: forged account shared from the LOGINAT block (same ctx
// twice, device order: initiation, then r1=1 registration).
std::uint32_t g_login_acct = 0;

void Watchdog(int) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "\n[watchdog] STUCK at init (300s) -- aborting\n");
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
    // HARNESS_POKE1187=1 (loading->menu gate): the gate is byte
    // [*(0x672e98)+1187]==1 (reader 0xBE714 from update 0xC2148; sole writer
    // 0xBE704 never fires). Poke it before every call (BSS valid post-init).
    if (std::getenv("HARNESS_POKE1187")) {
        const std::uint32_t slot = 0x672e98u;
        if (rt.CheckRange(slot, 4, false)) {
            std::uint32_t obj = 0;
            std::memcpy(&obj, mem.host(slot), 4);
            if (obj && rt.CheckRange(obj + 1187, 1, true)) {
                std::uint8_t old = 0, one = 1;
                std::memcpy(&old, mem.host(obj + 1187), 1);
                std::memcpy(mem.host(obj + 1187), &one, 1);
                static int pk = 0;
                if (pk < 12) {
                    pk++;
                    std::printf("[poke1187] fn=%08x obj=%08x was=%u\n", fn, obj, old);
                }
            } else {
                static int pk0 = 0;
                if (pk0 < 3) {
                    pk0++;
                    std::printf("[poke1187] fn=%08x obj=%08x INVALID\n", fn, obj);
                }
            }
        }
    }
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
    alarm(60);
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
        // Throw-to-catch redirect (worker throws land here too via shared
        // Runtime; ApplyRedirect is per-Jit so cross-talk is impossible).
        int redirs = 0;
        while (hr == Dynarmic::HaltReason::UserDefined1 && redirs++ < 4 && rt.ApplyRedirect()) {
            alarm(60);
            hr = jit.Run();
        }
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
        std::printf("[snap] VA 004de210 after load = %08x\n", w);
        for (int i = 0; i < 64; i++) {
            std::memcpy(&g_snap_region[i], mem.host(0x672000 + i * 4), 4);
        }
    }

    abr::Runtime rt(mem, mod);
    for (const auto& seg : mod.segments) {
        rt.AddMapping(seg.start, seg.size, seg.prot);
    }
    // Canary + errno slot for the main thread (before InitDataImports: the
    // guard variable cell reuses this canary value).
    std::random_device rd;
    const std::uint32_t canary = (static_cast<std::uint32_t>(rd()) << 1) | 1u;
    std::memcpy(mem.host(abr::kGuestDataBase + 0), &canary, 4);
    rt.InitDataImports();  // ctype tables + data-import slots (needs mappings)

    // File bridge root: HARNESS_FILES=<hostdir>, else <project>/work/files
    // derived from the .so path (ROOT/work/lib/armeabi-v7a/<file>).
    {
        std::string hostdir;
        if (const char* e = std::getenv("HARNESS_FILES")) {
            hostdir = e;
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

    // APK path for the AAssetManager bridge: HARNESS_APK or derived
    // (<project>/com.rovio.angrybirdsrio.2676.apk alongside work/).
    {
        std::string apk;
        if (const char* e = std::getenv("HARNESS_APK")) {
            apk = e;
        } else {
            std::string p = argv[1];
            for (int i = 0; i < 3; i++) {
                const std::size_t s = p.rfind('/');
                if (s == std::string::npos) {
                    break;
                }
                p = p.substr(0, s);
            }
            apk = p + "/../com.rovio.angrybirdsrio.2676.apk";
        }
        rt.SetApkPath(apk);
    }

    // (Canary written above, before InitDataImports.)
    Dynarmic::A32::UserCallbacks* cb = nullptr;
    (void)cb;
    abr::GuestCallbacks callbacks(&rt);
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = &callbacks;
    // A/B: HARNESS_SAFEOPTS=1 -> all_safe_optimizations (default lib
    // mode); unset -> no_optimizations (bring-up mode). MCSM ships fast;
    // we kept bring-up mode for debuggability. Compare baselines first.
    cfg.optimizations = std::getenv("HARNESS_SAFEOPTS") ? Dynarmic::all_safe_optimizations
                                                        : Dynarmic::no_optimizations;
    Dynarmic::ExclusiveMonitor monitor(1);
    cfg.global_monitor = &monitor;
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
    // #22 is PARKED by default (dead-by-construction, AGENTS.md sect.5);
    // HARNESS_FORCE22=1 overrides for experiments.
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
    if (!std::getenv("HARNESS_FORCE22")) {
        skip[22] = true;
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
        if (skip[k]) {
            const bool parked = (k == 22 && !std::getenv("HARNESS_FORCE22"));
            std::printf("[init] #%zu @%08x %s\n", k, mod.init_array[k],
                        parked ? "PARKED (dead-by-construction, see AGENTS.md sect.5)" : "SKIPPED");
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
            if (w != 0x2200990du) {
                std::printf("[corrupt] VA 004de210 = %08x after init #%zu\n", w, k);
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
    // HARNESS_RAW=<hexaddr>: micro-call for isolated forensics (wall #3):
    // builds two SSO-short "PVR" buffers in guest-data scratch, calls addr
    // with (r0=buf1, r1=0, r2=-1, r3=buf2, stack=[3]) and prints r0. Used to
    // test 0x69CDC (string compare) outside the lookup loop.
    if (const char* rawe = std::getenv("HARNESS_RAW")) {
        std::uint32_t fn = static_cast<std::uint32_t>(std::strtoul(rawe, nullptr, 16));
        const std::uint32_t b1 = abr::kGuestDataBase + 0x100;
        const std::uint32_t b2 = abr::kGuestDataBase + 0x120;
        const std::uint8_t s1[12] = {0x06, 'P', 'V', 'R', 0, 0, 0, 0, 0, 0, 0, 0};
        const std::uint8_t s2[12] = {0x06, 'P', 'V', 'R', 0, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(mem.host(b1), s1, 12);
        std::memcpy(mem.host(b2), s2, 12);
        // HARNESS_RAWDOT=1: first buffer becomes ".PVR" (dotted key).
        if (std::getenv("HARNESS_RAWDOT")) {
            const std::uint8_t sd[12] = {0x08, '.', 'P', 'V', 'R', 0, 0, 0, 0, 0, 0, 0};
            std::memcpy(mem.host(b1), sd, 12);
        }
        auto& regs = jit.Regs();
        regs.fill(0);
        regs[13] = abr::kStackTop - 0x100;
        regs[14] = mod.halt_va;
        regs[0] = b1;
        regs[1] = 0;
        regs[2] = 0xFFFFFFFFu;
        // HARNESS_RAW2=<hexaddr>: r3 override for the raw call (default:
        // second SSO buffer). Lets the compare run against static entries.
        regs[3] = b2;
        if (const char* r2e = std::getenv("HARNESS_RAW2")) {
            regs[3] = static_cast<std::uint32_t>(std::strtoul(r2e, nullptr, 16));
        }
        regs[13] -= 8;
        std::uint32_t three = 3;
        std::memcpy(mem.host(regs[13]), &three, 4);
        std::uint32_t z2 = 0;
        std::memcpy(mem.host(regs[13] + 4), &z2, 4);
        CallGuest(jit, fn);
        std::printf("[raw] calling @%08x ... ", fn);
        std::fflush(stdout);
        alarm(60);
        Dynarmic::HaltReason hr = jit.Run();
        alarm(0);
        std::printf("hr=%d r0=%08x (%u) r1=%08x svc=%llu\n", static_cast<int>(hr), regs[0], regs[0],
                    regs[1], (unsigned long long)rt.svc_count);
        _exit(hr == Dynarmic::HaltReason::UserDefined1 ? 0 : 1);
    }
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
            alarm(60);
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
        // HARNESS_JLAST="r2,r3" (hex or A=array): overrides r2/r3 for the
        // LAST call only (e.g. nativeMixData needs r3=array, others need dims).
        std::uint32_t jlast2 = 0, jlast3 = 0;
        bool have_jlast = false;
        if (const char* jl = std::getenv("HARNESS_JLAST")) {
            std::string s(jl), a, b;
            auto c = s.find(',');
            a = (c == std::string::npos) ? s : s.substr(0, c);
            b = (c == std::string::npos) ? "" : s.substr(c + 1);
            auto word = [&](const std::string& t) -> std::uint32_t {
                if (t == "A" || t == "a") {
                    return jbarray;
                }
                return static_cast<std::uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
            };
            jlast2 = word(a);
            jlast3 = word(b);
            // "J" = recorded AudioOutput handle (occupies r2+r3 as jlong).
            if (a == "J" || a == "j" || b == "J" || b == "j") {
                jlast2 = rt.AudioJLo();
                jlast3 = rt.AudioJHi();
            }
            have_jlast = true;
        }
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
            if (have_jlast && i + 1 == fns.size()) {
                r2 = jlast2;
                r3 = jlast3;
            }
            // HARNESS_JOVR="idx=r2,r3[,s0,s1...]" (; separated): per-call
            // overrides for input injection etc. (A = byte array). Stack
            // override replaces jstk for that call only.
            std::vector<std::uint32_t> stk = jstk;
            if (const char* jo = std::getenv("HARNESS_JOVR")) {
                std::string spec(jo), cur;
                auto apply = [&](const std::string& s) {
                    auto eq = s.find('=');
                    if (eq == std::string::npos) {
                        return;
                    }
                    std::size_t idx =
                        static_cast<std::size_t>(std::strtoul(s.substr(0, eq).c_str(), nullptr, 10));
                    if (idx != i) {
                        return;
                    }
                    std::string rest = s.substr(eq + 1), tok;
                    std::vector<std::string> parts;
                    for (char ch : rest) {
                        if (ch == ',') {
                            parts.push_back(tok);
                            tok.clear();
                        } else if (ch != ' ') {
                            tok += ch;
                        }
                    }
                    parts.push_back(tok);
                    auto word = [&](const std::string& t) -> std::uint32_t {
                        if (t == "A" || t == "a") {
                            return jbarray;
                        }
                        if (t.size() > 2 && t[0] == 'f') {
                            float f = std::strtof(t.c_str() + 1, nullptr);
                            std::uint32_t w = 0;
                            std::memcpy(&w, &f, 4);
                            return w;
                        }
                        return static_cast<std::uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
                    };
                    if (parts.size() > 0 && !parts[0].empty()) {
                        r2 = word(parts[0]);
                    }
                    if (parts.size() > 1 && !parts[1].empty()) {
                        r3 = word(parts[1]);
                    }
                    // "J" = recorded AudioOutput handle (jlong in r2+r3).
                    if ((parts.size() > 0 && (parts[0] == "J" || parts[0] == "j")) ||
                        (parts.size() > 1 && (parts[1] == "J" || parts[1] == "j"))) {
                        r2 = rt.AudioJLo();
                        r3 = rt.AudioJHi();
                    }
                    if (parts.size() > 2) {
                        stk.clear();
                        for (std::size_t k = 2; k < parts.size(); k++) {
                            stk.push_back(word(parts[k]));
                        }
                    }
                };
                for (char ch : spec) {
                    if (ch == ';') {
                        apply(cur);
                        cur.clear();
                    } else {
                        cur += ch;
                    }
                }
                apply(cur);
            }
            Dynarmic::HaltReason hr = RunOneJni(jit, rt, mem, mod, fns[i], r2, r3, stk);
            ok = (hr == Dynarmic::HaltReason::UserDefined1);
            // HARNESS_SCENEAT=N (JNI call index): direct 7-arg BL 0x283A3C            // (mgr,1,0,zerobuf + zero stack) scene-liveness proof (harness-only
            // diag, never ships). mgr from the render hook (never hardcoded);
            // gated on +164==2 and empty B4 vector; expects WHITE when content
            // was never built (flip without builders).
            if (const char* se = std::getenv("HARNESS_SCENEAT")) {
                static bool sdone = false;
                if (!sdone && i == (std::size_t)std::atoi(se)) {
                    sdone = true;
                    const std::uint32_t mgr = rt.last_mgr_;
                    std::uint32_t m164 = 0xFFFFFFFFu, b4b = 0, b4e = 0;
                    const bool mgrad = (mgr != 0 && rt.CheckRange(mgr + 0xB8, 4, false));
                    if (mgrad) {
                        std::memcpy(&m164, rt.H(mgr + 0xA4), 4);
                        std::memcpy(&b4b, rt.H(mgr + 0xB4), 4);
                        std::memcpy(&b4e, rt.H(mgr + 0xB8), 4);
                    }
                    std::printf("[scene7] mgr=%08x +164=%08x b4=[%08x..%08x]\n", mgr,
                                m164, b4b, b4e);
                    if (mgrad && m164 == 2 && b4b == b4e && rt.IsCodeAddress(0x283A3Cu)) {
                        const std::uint32_t zb = rt.GuestCalloc(1, 128);
                        if (zb) {
                            auto& rgs = jit.Regs();
                            rgs.fill(0);
                            rgs[13] = abr::kStackTop - 0x200;
                            rgs[13] -= 16;
                            const std::uint32_t zw = 0;
                            for (int k = 0; k < 4; k++) {
                                std::memcpy(mem.host(rgs[13] + k * 4), &zw, 4);
                            }
                            rgs[14] = mod.halt_va;
                            rgs[0] = mgr;
                            rgs[1] = 1;
                            rgs[2] = 0;
                            rgs[3] = zb;
                            CallGuest(jit, 0x283A3Cu);
                            std::printf("[scene7] calling @000283a3c ... ");
                            std::fflush(stdout);
                            alarm(60);
                            Dynarmic::HaltReason hr2 = jit.Run();
                            alarm(0);
                            std::uint32_t post = 0xFFFFFFFFu;
                            if (rt.CheckRange(mgr + 0xA4, 4, false)) {
                                std::memcpy(&post, rt.H(mgr + 0xA4), 4);
                            }
                            std::printf("hr=%d r0=%08x +164: %08x -> %08x\n",
                                        static_cast<int>(hr2), rgs[0], m164, post);
                            jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                        }
                    } else {
                        std::printf("[scene7] preconditions unmet, skipping\n");
                    }
                }
            }
            // HARNESS_MGRAT=N (JNI call index): direct BL 0x283B5C manager
            // pass (harness-only diag, never ships). Single arg r0=mgr (head
            // ignores rest); gates (+216/+172/+176/+164==2) all true live.
            // One-shot (clears +216). Binary: runs (post +216==0 proves it)
            // vs bails; downstream loop/nodes/builders/film observed.
            // POLL MODE: from call N on, fires on the FIRST satisfied gate
            // (+172/+176 pulse and drain with loading progress; a fixed index
            // can miss it).
            if (const char* se = std::getenv("HARNESS_MGRAT")) {
                static bool sdone = false;
                if (!sdone && i >= (std::size_t)std::atoi(se)) {
                    const std::uint32_t mgr = rt.last_mgr_;
                    std::uint32_t g216 = 0, g172 = 0, g176 = 0, g164 = 0;
                    const bool mgrad =
                        (mgr != 0 && rt.CheckRange(mgr + 220, 4, false));
                    if (mgrad) {
                        std::memcpy(&g216, rt.H(mgr + 216), 1);
                        std::memcpy(&g172, rt.H(mgr + 172), 4);
                        std::memcpy(&g176, rt.H(mgr + 176), 4);
                        std::memcpy(&g164, rt.H(mgr + 164), 4);
                    }
                    static int mno = 0;
                    const bool fire =
                        (mgrad && g216 != 0 && g172 != 0 && g164 == 2 &&
                         rt.IsCodeAddress(0x283B5Cu));
                    if (fire || mno < 3) {
                        mno++;
                        std::printf("[mgrat] mgr=%08x +216=%u +172=%08x +176=%08x "
                                    "+164=%08x\n",
                                    mgr, g216, g172, g176, g164);
                    }
                    // HARNESS_MGRFORGE=1: satisfy the +172/+176 presence gates
                    // with zeroed heap blocks when they are null (loading
                    // progress normally fills them; timing-jittered). If the
                    // pass derefs them expecting objects, the fault names the
                    // requirement (bounded). Else the pass runs: observe
                    // loop/nodes/builders/film.
                    bool forged = false;
                    if (std::getenv("HARNESS_MGRFORGE") && mgrad && g172 == 0 &&
                        g176 == 0) {
                        const std::uint32_t zb = rt.GuestCalloc(1, 16);
                        if (zb && rt.CheckRange(mgr + 180, 4, true)) {
                            std::memcpy(rt.H(mgr + 172), &zb, 4);
                            std::memcpy(rt.H(mgr + 176), &zb, 4);
                            g172 = zb;
                            g176 = zb;
                            forged = true;
                            std::printf("[mgrat] forged +172/+176=%08x\n", zb);
                        }
                    }
                    if (fire || forged) {
                        sdone = true;
                        auto& rgs = jit.Regs();
                        rgs.fill(0);
                        rgs[13] = abr::kStackTop - 0x200;
                        rgs[13] -= 16;
                        const std::uint32_t zw = 0;
                        for (int k = 0; k < 4; k++) {
                            std::memcpy(mem.host(rgs[13] + k * 4), &zw, 4);
                        }
                        rgs[14] = mod.halt_va;
                        rgs[0] = mgr;
                        rgs[1] = 0;
                        rgs[2] = 0;
                        rgs[3] = 0;
                        CallGuest(jit, 0x283B5Cu);
                        std::printf("[mgrat] calling @000283b5c ... ");
                        std::fflush(stdout);
                        alarm(60);
                        Dynarmic::HaltReason hr2 = jit.Run();
                        alarm(0);
                        std::uint32_t post = 0;
                        if (rt.CheckRange(mgr + 216, 1, false)) {
                            std::uint8_t b = 0;
                            std::memcpy(&b, rt.H(mgr + 216), 1);
                            post = b;
                        }
                        std::printf("hr=%d r0=%08x +216: %u -> %u\n",
                                    static_cast<int>(hr2), rgs[0], g216, post);
                        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    } else {
                        std::printf("[mgrat] preconditions unmet, skipping\n");
                    }
                }
            }
            // HARNESS_BUILDER=N (JNI call index): direct BL 0x250AB8 menu
            // builder (harness-only diag, never ships). this = live scene
            // object from the loading-builder hook (vtable-gated to 0x669620,
            // the scene class), r1 = device-observed scene key 0x101929.
            // Binary: builds content (film!) vs faults (pc+addr = requirement).
            if (const char* se = std::getenv("HARNESS_BUILDER")) {
                static bool sdone = false;
                if (!sdone && i == (std::size_t)std::atoi(se)) {
                    sdone = true;
                    const std::uint32_t tho = rt.last_this_;
                    std::uint32_t vt = 0xFFFFFFFFu;
                    const bool tgrad =
                        (tho != 0 && rt.CheckRange(tho, 4, false));
                    if (tgrad) {
                        std::memcpy(&vt, rt.H(tho), 4);
                    }
                    std::printf("[builder] this=%08x vtable=%08x\n", tho, vt);
                    // HARNESS_BUILDERFORGE=1: forge this instead (scene vtable
                    // VA 0x669428 + zeros; +100 slot = 0x251308 valid code).
                    // Graph attachment unknown: expect faults (each names the
                    // next requirement) or invisible builds, not menu pixels.
                    std::uint32_t bthis = tho;
                    if (std::getenv("HARNESS_BUILDERFORGE")) {
                        bthis = rt.GuestCalloc(1, 512);
                        if (bthis) {
                            const std::uint32_t svt = 0x669428u;
                            std::memcpy(rt.H(bthis), &svt, 4);
                            // Borrow the live render sub-object so the
                            // builder-called writer (r1=1) proceeds past its
                            // [r0+0xA8]->vtable+0x60 getter (else wild read).
                            // If builders self-register via globals, content
                            // may still reach the live graph (film decides).
                            if (rt.last_mgr_ != 0 &&
                                rt.CheckRange(rt.last_mgr_ + 0xAC, 4, false)) {
                                std::uint32_t robj = 0;
                                std::memcpy(&robj, rt.H(rt.last_mgr_ + 0xA8), 4);
                                if (robj && rt.CheckRange(robj, 4, false)) {
                                    std::memcpy(rt.H(bthis + 0xA8), &robj, 4);
                                    std::printf("[builder] borrowed render=%08x\n",
                                                robj);
                                }
                            }
                            std::printf("[builder] forged this=%08x\n", bthis);
                        }
                    }
                    // HARNESS_BUILDERGL=N: same direct builder call but with a
                    // LIVE GL_Context this (heap scan for vtable VA 0x669404,
                    // device-proven [this]; our surf object qualifies) and no
                    // forge/borrow. Device arg shape (r1=color, r2=r3=0).
                    // Harness-only diag. Binary: clean+content (film!) vs
                    // fault (pc+addr = requirement).
                    if (std::getenv("HARNESS_BUILDERGL")) {
                        std::uint32_t glthis = 0;
                        const std::uint32_t want = 0x669404u;
                        // Bound: GL_Context allocates at nativeInit (low heap).
                        // Full-heap scan stalls the single core for seconds and
                        // trips pool watchdogs (FATAL abort); keep it under ~0.2s.
                        const std::uint32_t scan_end =
                            rt.HeapTop() < 0x11000000u ? rt.HeapTop() : 0x11000000u;
                        const auto t0 = std::chrono::steady_clock::now();
                        for (std::uint32_t a = 0x10000000u; a + 4 <= scan_end; a += 4) {
                            if (!rt.CheckRange(a, 4, false)) {
                                a = (a + 0xFFFu) & ~0xFFFu;
                                a -= 1;
                                continue;
                            }
                            std::uint32_t w = 0;
                            std::memcpy(&w, rt.H(a), 4);
                            if (w == want) {
                                glthis = a;
                                break;
                            }
                        }
                        std::printf("[builder] live GL_Context this=%08x scan=%.1fs\n", glthis,
                                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                                        .count());
                        if (glthis && rt.IsCodeAddress(0x250AB8u)) {
                            // Device burst shape: 6 rects alternating colors
                            // (0x101929 / 0x7F000000) in ~100ms. Same XY.
                            const std::uint32_t cols[2] = {0x101929u, 0x7F000000u};
                            for (int bi = 0; bi < 6; bi++) {
                            auto& rgs = jit.Regs();
                            rgs.fill(0);
                            rgs[13] = abr::kStackTop - 0x200;
                            rgs[13] -= 16;
                            // HARNESS_BUILDERXY="X,Y" (ints, vcvt->float): the
                            // only forged rect inputs ([sp+0]=X, [sp+4]=Y);
                            // rest derives from live this (this+228 floats,
                            // [this]/[this+8] widths). Device candidates:
                            // 488,44 (d1b) or 400,240 (viewport center).
                            std::uint32_t xy[2] = {0, 0};
                            if (const char* xye = std::getenv("HARNESS_BUILDERXY")) {
                                std::string xs(xye);
                                const auto cm = xs.find(',');
                                if (cm != std::string::npos) {
                                    xy[0] = static_cast<std::uint32_t>(
                                        std::strtoul(xs.substr(0, cm).c_str(), nullptr, 0));
                                    xy[1] = static_cast<std::uint32_t>(
                                        std::strtoul(xs.substr(cm + 1).c_str(), nullptr, 0));
                                }
                            }
                            std::memcpy(mem.host(rgs[13] + 0), &xy[0], 4);
                            std::memcpy(mem.host(rgs[13] + 4), &xy[1], 4);
                            const std::uint32_t zw = 0;
                            for (int k = 2; k < 4; k++) {
                                std::memcpy(mem.host(rgs[13] + k * 4), &zw, 4);
                            }
                            rgs[14] = mod.halt_va;
                            rgs[0] = glthis;
                            rgs[1] = cols[bi & 1];
                            rgs[2] = 0;
                            rgs[3] = 0;
                            CallGuest(jit, 0x250AB8u);
                            std::printf("[builder] calling @000250ab8 live #%d ... ", bi);
                            std::fflush(stdout);
                            alarm(60);
                            Dynarmic::HaltReason hr2 = jit.Run();
                            alarm(0);
                            std::printf("hr=%d r0=%08x\n", static_cast<int>(hr2), rgs[0]);
                            jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                            }  // burst x6
                        }
                    }
                    if (((std::getenv("HARNESS_BUILDERFORGE") && bthis) ||
                         (tgrad && vt == 0x669620u)) &&
                        rt.IsCodeAddress(0x250AB8u)) {
                        auto& rgs = jit.Regs();
                        rgs.fill(0);
                        rgs[13] = abr::kStackTop - 0x200;
                        rgs[13] -= 16;
                        const std::uint32_t zw = 0;
                        for (int k = 0; k < 4; k++) {
                            std::memcpy(mem.host(rgs[13] + k * 4), &zw, 4);
                        }
                        rgs[14] = mod.halt_va;
                        rgs[0] = bthis;
                        rgs[1] = 0x101929u;
                        rgs[2] = 0;
                        rgs[3] = 0;
                        CallGuest(jit, 0x250AB8u);
                        std::printf("[builder] calling @000250ab8 ... ");
                        std::fflush(stdout);
                        alarm(60);
                        Dynarmic::HaltReason hr2 = jit.Run();
                        alarm(0);
                        std::printf("hr=%d r0=%08x\n", static_cast<int>(hr2), rgs[0]);
                        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    } else {
                        std::printf("[builder] preconditions unmet, skipping\n");
                    }
                }
            }
            // HARNESS_LOGINAT=N (JNI call index): login initiation trigger —
            // direct BL 0x5CE50 (FUN_0006CE50) with a forged RovioAccount*
            // (harness-only diag, never ships). Account: calloc 0x2C,
            // [+0]=0x65E504, rest zero (=> +25==0 gate passes, +32==0).
            // Binary: login flow starts (functors/logs/events/builders/film)
            // vs clean no-op vs fault (pc+addr = requirement).
            if (const char* se = std::getenv("HARNESS_LOGINAT")) {
                static bool sdone = false;
                if (!sdone && i == (std::size_t)std::atoi(se)) {
                    sdone = true;
                    // Faithful mode (HARNESS_LOGINAT_FAITHFUL=1): copy the live
                    // account bytes snapshotted at the natural 0x5CE50 hit
                    // (WATCHPCSNAP) instead of zeros. Same-run pointers stay
                    // valid; heap drift between runs cannot poison it.
                    bool faithful = false;
                    if (std::getenv("HARNESS_LOGINAT_FAITHFUL") &&
                        rt.loginat_acct_len_ >= 44) {
                        faithful = true;
                    }
                    const std::uint32_t acct =
                        rt.GuestCalloc(1, faithful ? 0x40 : 0x2C);
                    if (acct) {
                        if (faithful) {
                            std::memcpy(rt.H(acct), rt.loginat_acct_snap_,
                                        rt.loginat_acct_len_);
                            std::printf("[loginat] faithful %uB from live @%08x"
                                        " -> copy @%08x\n",
                                        rt.loginat_acct_len_, rt.loginat_acct_,
                                        acct);
                        } else {
                        const std::uint32_t av = 0x65E504u;
                        std::memcpy(rt.H(acct), &av, 4);
                        // +32 must be a writable block (flow stores +0x50
                        // through it); zeros are scratch until proven content.
                        // Prefer the live session-ish object stashed from
                        // D5994 entries (updates pass the real one there).
                        const std::uint32_t ab = rt.GuestCalloc(1, 0x60);
                        if (ab) {
                            std::memcpy(rt.H(acct + 32), &ab, 4);
                        }
                        if (rt.loginat_session_ != 0) {
                            // Holder fix (+72 verdict): [account+32] must be
                            // a 0x10B holder with session at [+8] (NOT the
                            // session directly — wrong level faults via
                            // [mutex+72]). Alloc zeroed holder, link both.
                            const std::uint32_t hh = rt.GuestCalloc(1, 0x10B);
                            if (hh) {
                                std::memcpy(rt.H(hh + 8), &rt.loginat_session_, 4);
                                std::memcpy(rt.H(acct + 32), &hh, 4);
                                std::printf("[loginat] +32 holder=%08x session=%08x\n",
                                            hh, rt.loginat_session_);
                            } else {
                                std::memcpy(rt.H(acct + 32), &rt.loginat_session_, 4);
                                std::printf("[loginat] +32 linked live session=%08x\n",
                                            rt.loginat_session_);
                            }
                        }
                        }  // end zeros+holder path (faithful skips it)
                        std::printf("[loginat] forged account=%08x\n", acct);
                        g_login_acct = acct;
                        auto& rgs = jit.Regs();
                        rgs.fill(0);
                        rgs[13] = abr::kStackTop - 0x200;
                        rgs[13] -= 16;
                        const std::uint32_t zw = 0;
                        for (int k = 0; k < 4; k++) {
                            std::memcpy(mem.host(rgs[13] + k * 4), &zw, 4);
                        }
                        rgs[14] = mod.halt_va;
                        rgs[0] = acct;
                        rgs[1] = 0;
                        rgs[2] = 0;
                        rgs[3] = 0;
                        CallGuest(jit, 0x5CE50u);
                        std::printf("[loginat] calling @0005ce50 ... ");
                        std::fflush(stdout);
                        alarm(60);
                        Dynarmic::HaltReason hr2 = jit.Run();
                        alarm(0);
                        std::printf("hr=%d r0=%08x\n", static_cast<int>(hr2), rgs[0]);
                        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    }
                }
            }
            // HARNESS_LOGINAT1=N (JNI call index): loginFinished with r1=1 on
            // the LOGINAT-forged account (same ctx twice, device order).
            // r1=1 executes the +32 block + virtual +72 observer registration
            // that r1=0 skips (PROVEN). Binary: registers (functors/events/
            // builders/film follow?) vs faults (pc+addr = requirement).
            if (const char* se = std::getenv("HARNESS_LOGINAT1")) {
                static bool sdone = false;
                if (!sdone && i == (std::size_t)std::atoi(se)) {
                    sdone = true;
                    std::uint8_t pre25 = 0xFF;
                    if (g_login_acct != 0 &&
                        rt.CheckRange(g_login_acct + 25, 1, false)) {
                        std::memcpy(&pre25, rt.H(g_login_acct + 25), 1);
                    }
                    std::printf("[loginat1] acct=%08x pre25=%u\n", g_login_acct,
                                pre25);
                    if (g_login_acct != 0 && rt.IsCodeAddress(0x5D2F0u)) {
                        auto& rgs = jit.Regs();
                        rgs.fill(0);
                        rgs[13] = abr::kStackTop - 0x200;
                        rgs[13] -= 16;
                        const std::uint32_t zw = 0;
                        for (int k = 0; k < 4; k++) {
                            std::memcpy(mem.host(rgs[13] + k * 4), &zw, 4);
                        }
                        rgs[14] = mod.halt_va;
                        rgs[0] = g_login_acct;
                        rgs[1] = 1;
                        rgs[2] = 0;
                        rgs[3] = 0;
                        CallGuest(jit, 0x5D2F0u);
                        std::printf("[loginat1] calling @0005d2f0 r1=1 ... ");
                        std::fflush(stdout);
                        alarm(60);
                        Dynarmic::HaltReason hr2 = jit.Run();
                        alarm(0);
                        std::uint8_t post25 = 0xFF;
                        if (rt.CheckRange(g_login_acct + 25, 1, false)) {
                            std::memcpy(&post25, rt.H(g_login_acct + 25), 1);
                        }
                        std::printf("hr=%d r0=%08x +25: %u -> %u\n",
                                    static_cast<int>(hr2), rgs[0], pre25, post25);
                        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    }
                }
            }
            // HARNESS_BC158=N (JNI call index): event-consumer mirror —
            // direct BL 0xBC158 (0xBC0AC's worker) with a forged event
            // record: C={bx-lr,0} (even path, no deref), D=3 zero words
            // (copied then cleared by the body), B=0 (flows only into the
            // benign call's r0). Device fires this 4x at +11s (Java->…->
            // B004C->BC0AC) feeding menu construction; in-harness the poster
            // never runs. Harness-only diag, never ships. Binary: clean run
            // (+ drawRect/builder/film downstream?) vs fault (pc+addr =
            // requirement: which word must be live?).
            if (const char* se = std::getenv("HARNESS_BC158")) {
                static bool sdone = false;
                if (!sdone && i == (std::size_t)std::atoi(se)) {
                    sdone = true;
                    const std::uint32_t evc = rt.GuestCalloc(1, 8);
                    const std::uint32_t evd = rt.GuestCalloc(1, 12);
                    if (evc && evd) {
                        // HARNESS_BC158LIVE=1: live record shape from D1c
                        // (C={0xBEA58 tag-dispatcher,0}, D={0x11,0xd,0}).
                        // Default (J1): sterile C={bx-lr,0} + zero D.
                        std::uint32_t c0 = 0x251260u;
                        std::uint32_t d0 = 0, d1 = 0, d2 = 0;
                        if (std::getenv("HARNESS_BC158LIVE")) {
                            c0 = 0xBEA58u;
                            d0 = 0x11;
                            d1 = 0xd;
                            d2 = 0;
                        }
                        const std::uint32_t z = 0;
                        std::memcpy(rt.H(evc), &c0, 4);
                        std::memcpy(rt.H(evc + 4), &z, 4);
                        std::memcpy(rt.H(evd), &d0, 4);
                        std::memcpy(rt.H(evd + 4), &d1, 4);
                        std::memcpy(rt.H(evd + 8), &d2, 4);
                        std::printf("[bc158] C=%08x {%08x,0} D=%08x {%x,%x,%x}\n",
                                    evc, c0, evd, d0, d1, d2);
                        auto& rgs = jit.Regs();
                        rgs.fill(0);
                        rgs[13] = abr::kStackTop - 0x200;
                        rgs[13] -= 16;
                        for (int k = 0; k < 4; k++) {
                            std::memcpy(mem.host(rgs[13] + k * 4), &z, 4);
                        }
                        rgs[14] = mod.halt_va;
                        rgs[0] = 0;
                        rgs[1] = 0;
                        rgs[2] = evc;
                        rgs[3] = evd;
                        CallGuest(jit, 0xBC158u);
                        std::printf("[bc158] calling @000bc158 ... ");
                        std::fflush(stdout);
                        alarm(60);
                        Dynarmic::HaltReason hr2 = jit.Run();
                        alarm(0);
                        std::printf("hr=%d r0=%08x\n", static_cast<int>(hr2),
                                    rgs[0]);
                        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    }
                }
            }
            // HARNESS_BPATCH=1: overwrite +16 of live queue-B completion
            // entries (MAGIC 0xFB048F99 at +8, RET 0x28DC18 at +24, +12==0)
            // with our free-thunk callable (vt+0x18). Binary: [eventpost]
            // INVOKE in log <=> process drains B and invokes entry functors.
            // Harness-only diag, never ships. Runs after updates.
            if (std::getenv("HARNESS_BPATCH") && fns[i] == 0xC2014u) {
                static bool bbuilt = false;
                static std::uint32_t bcall = 0;
                static int bpatched = 0;
                if (!bbuilt) {
                    bbuilt = true;
                    std::uint32_t cb = rt.GuestCalloc(1, 8);
                    std::uint32_t vt = rt.GuestCalloc(1, 0x20);
                    std::uint32_t efree = 0;
                    for (const auto& im : mod.imports) {
                        if (im.name == "free") {
                            efree = im.tramp_va;
                            break;
                        }
                    }
                    if (cb && vt && efree) {
                        std::memcpy(rt.H(cb), &vt, 4);
                        std::memcpy(rt.H(vt + 0x18), &efree, 4);
                        bcall = cb;
                        rt.eventpost_callable_ = cb;
                        std::printf("[bpatch] callable=%08x free=%08x\n", cb, efree);
                    }
                }
                if (bcall && bpatched < 12) {
                    static const std::uint8_t magic[4] = {0x99, 0x8F, 0x04, 0xFB};
                    static const std::uint32_t retc = 0x28DC18u;
                    const std::uint32_t lo = 0x10000000u;
                    const std::uint32_t hi = rt.HeapTop();
                    for (std::uint32_t a = lo; a + 4 <= hi && bpatched < 12; a++) {
                        if (!rt.CheckRange(a, 4, false)) {
                            a = (a + 0xFFFu) & ~0xFFFu;
                            a -= 1;
                            continue;
                        }
                        if (std::memcmp(rt.H(a), magic, 4) != 0) {
                            continue;
                        }
                        const std::uint32_t base = a - 8;
                        if (!rt.CheckRange(base, 32, true)) {
                            continue;
                        }
                        std::uint32_t w24 = 0, w12 = 0, w16 = 0;
                        std::memcpy(&w24, rt.H(base + 24), 4);
                        std::memcpy(&w12, rt.H(base + 12), 4);
                        std::memcpy(&w16, rt.H(base + 16), 4);
                        if (w24 != retc || w12 != 0 || w16 == bcall) {
                            continue;
                        }
                        std::memcpy(rt.H(base + 16), &bcall, 4);
                        bpatched++;
                        std::printf("[bpatch] entry=%08x orig16=%08x\n", base, w16);
                    }
                }
            }
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
            // HARNESS_RAWPOST=<fnhex> + HARNESS_RAWAT=<n> + HARNESS_RAWOBJ=<hexneedle>:
            // after JNI call index N, scan guest heap for needle, call fn with
            // r0=found (or HARNESS_RAW0), r1=r2=r3=0, then continue sequence.
            // (loginFinished-style completion pump; skips cleanly if missing.)
            {
                static int rawat = -2;
                static std::uint32_t rawfn = 0;
                static std::vector<std::uint8_t> rawneedle;
                static bool rawdone = false;
                if (rawat == -2) {
                    rawat = -1;
                    if (const char* e = std::getenv("HARNESS_RAWPOST")) {
                        rawfn = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
                    }
                    if (const char* e = std::getenv("HARNESS_RAWAT")) {
                        rawat = std::atoi(e);
                    }
                    if (const char* hx = std::getenv("HARNESS_RAWOBJ")) {
                        std::string hxs(hx);
                        for (std::size_t k = 0; k + 1 < hxs.size() && rawneedle.size() < 64;
                             k += 2) {
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
                            rawneedle.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
                        }
                    }
                }
                if (!rawdone && rawfn != 0 && rawat >= 0 && i == (std::size_t)rawat) {
                    rawdone = true;
                    std::uint32_t found = 0;
                    if (const char* r0e = std::getenv("HARNESS_RAW0")) {
                        found = static_cast<std::uint32_t>(std::strtoul(r0e, nullptr, 16));
                    } else if (std::getenv("HARNESS_RAWSTASH")) {
                        // r0 = live-stashed session (e.g. D5994 traffic) for
                        // direct calls needing real session args (r1 via
                        // HARNESS_RAW1).
                        found = rt.loginat_session_;
                        if (found) {
                            std::printf("[rawpost] stashed session=%08x\n", found);
                        }
                    } else if (std::getenv("HARNESS_RAWBUILD")) {                        // Forge a login-functor ctx (for direct 0x5FE70 calls):
                        // [+0]=functor vtable (unread, realistic), [+4]=zeroed
                        // account-sized block, [+8]=0 (=> loginFinished skips
                        // +40/+16 reads), [+0xC]=empty SSO (all zero).
                        const std::uint32_t zb = rt.GuestCalloc(1, 0x2C);
                        const std::uint32_t cx = rt.GuestCalloc(1, 0x20);
                        if (zb && cx) {
                            const std::uint32_t fv = 0x65E650u;
                            std::memcpy(rt.H(cx), &fv, 4);
                            std::memcpy(rt.H(cx + 4), &zb, 4);
                            found = cx;
                            std::printf("[rawpost] forged ctx=%08x acct=%08x\n", cx, zb);
                        }
                    } else if (!rawneedle.empty()) {
                        const std::uint32_t lo = 0x10000000u;
                        const std::uint32_t hi = rt.HeapTop();
                        const std::size_t nl = rawneedle.size();
                        // Fingerprint: anchor word AND [+0x14]==0x11 (login
                        // record skeleton, device ctxdump). Lookalikes sharing
                        // the anchor alone are skipped.
                        for (std::uint32_t a = lo; a + nl <= hi; a++) {
                            if (!rt.CheckRange(a, nl, false)) {
                                a = (a + 0xFFFu) & ~0xFFFu;
                                a -= 1;
                                continue;
                            }
                            if (std::memcmp(rt.H(a), rawneedle.data(), nl) != 0) {
                                continue;
                            }
                            std::uint32_t w14 = 0;
                            if (rt.CheckRange(a + 0x14, 4, false)) {
                                std::memcpy(&w14, rt.H(a + 0x14), 4);
                            }
                            std::printf("[rawpost] candidate %08x [+0x14]=%08x %s\n", a, w14,
                                        (w14 == 0x11) ? "MATCH" : "skip");
                            // HARNESS_RAWANY=1: accept the first needle hit
                            // without the login-record fingerprint (e.g. the
                            // RovioAccount singleton by its vtable needle).
                            static bool rawany = false;
                            static bool rawany_init = false;
                            if (!rawany_init) {
                                rawany_init = true;
                                rawany = (std::getenv("HARNESS_RAWANY") != nullptr);
                            }
                            if (rawany || w14 == 0x11) {
                                found = a;
                                break;
                            }
                        }
                    }
                    if (found == 0) {
                        std::printf("[rawpost] obj not found, skipping\n");
                    } else {
                        auto& rgs = jit.Regs();
                        rgs.fill(0);
                        rgs[13] = abr::kStackTop - 0x100;
                        rgs[14] = mod.halt_va;
                        rgs[0] = found;
                        rgs[1] = 0;
                        rgs[2] = 0;
                        rgs[3] = 0;
                        // HARNESS_RAW1=<hex>: r1 override for the post call
                        // (e.g. writer-select r1=1).
                        if (const char* r1e = std::getenv("HARNESS_RAW1")) {
                            rgs[1] = static_cast<std::uint32_t>(std::strtoul(r1e, nullptr, 16));
                        }
                        // Pre/post snapshot around +0x14 (skeleton re-check).
                        std::uint32_t pre[4] = {};
                        if (rt.CheckRange(found, 0x20, false)) {
                            std::memcpy(pre, rt.H(found), 0x20);
                        }
                        std::printf("[rawpost] pre obj=%08x [+0]=%08x [+4]=%08x [+8]=%08x "
                                    "[+0xC]=%08x [+0x10]=%08x [+0x14]=%08x [+0x18]=%08x "
                                    "[+0x1C]=%08x\n",
                                    found, pre[0], pre[1], pre[2], pre[3], pre[4], pre[5],
                                    pre[6], pre[7]);
                        CallGuest(jit, rawfn);
                        std::printf("[rawpost] calling @%08x obj=%08x ... ", rawfn, found);
                        std::fflush(stdout);
                        alarm(60);
                        Dynarmic::HaltReason hr2 = jit.Run();
                        alarm(0);
                        std::printf("hr=%d r0=%08x\n", static_cast<int>(hr2), rgs[0]);
                        std::uint32_t post = 0;
                        if (rt.CheckRange(found + 0x18, 4, false)) {
                            std::memcpy(&post, rt.H(found + 0x18), 4);
                        }
                        std::printf("[rawpost] post [+0x18]=%08x\n", post);
                    }
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
                    char path[64];
                    std::snprintf(path, sizeof(path), "work/shot%03zu.ppm", i + 1);
                    rt.RealGLBackend()->Screenshot(path, 800, 480);
                    std::printf("[realgl] filmstrip -> %s\n", path);
                }
            }
        }
        // HARNESS_SCREENSHOT=1: capture the real-GL drawable (needs
        // HARNESS_REALGL=1) to work/shot.ppm after the JNI sequence.
        if (std::getenv("HARNESS_SCREENSHOT")) {
            rt.RealGLBackend()->Screenshot("work/shot.ppm", 800, 480);
        }
        rt.ThreadCensus();
        // TEMP-DIAG (sessions handoff): session-flag value at exit.
        {
            if (abr::g_sess_watch != 0 && rt.CheckRange(abr::g_sess_watch, 1, false)) {
                std::uint8_t fl = 0;
                std::memcpy(&fl, rt.H(abr::g_sess_watch), 1);
                std::printf("[sesswatch] exit flag=%u\n", fl);
            }
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
