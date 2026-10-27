// guestrun: load any guest ARM32 .so, run its DT_INIT_ARRAY, then call
// exported functions in order. A bring-up probe for new titles (no title
// knowledge, unlike harness.cpp which carries Rovio forensics).
//
//   guestrun <lib.so> [--lib <more.so>]... [--base <addr>] [--files <hostdir>]
//            [--pkg <package>] [--init <lib>] [--call <symbol> [arg...]]...
//
// Libraries load in the order given (dependencies first) into one module,
// the first at --base (default 0), each next one at the next 16 MiB boundary;
// their inits run in the same order; with --init <lib>, only that library's
// (and the guest libraries it needs) at start, the others at the guest's
// dlopen, as on a device.
//
// Arguments of a call (r0..r3, then the stack):
//   123 / 0x7b   an integer
//   s:text       a NUL-terminated copy of text in guest memory
//   $N           the r0 result of call N (0-based)
//   @vm / @env   the runtime's mock JavaVM* / JNIEnv*
//   j:text       a jstring
// Pseudo-calls: @read32 addr, @str addr (print a word / a string), @sleep ms.
// <symbol> may also be a guest address (0x...), e.g. a RegisterNatives target.
// Each call prints "call N sym(...) -> r0 (hr)". The run stops at the first
// call that does not return cleanly.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "guest/dispatch.h"
#include "guest/elf_loader.h"
#include "guest/guest_memory.h"
#include "guest/layout.h"

namespace {

void SetEntry(Dynarmic::A32::Jit& jit, std::uint32_t target) {
    jit.SetCpsr((target & 1u) ? 0x30u : 0x10u);  // Thumb or ARM, user mode
    jit.Regs()[15] = target & ~std::uint32_t(1);
}

struct Call {
    std::string sym;
    std::vector<std::string> args;
};

// Runs guest code at `target` with up to 4 register and N stack words;
// returns true when it came back to the halt trampoline.
bool RunGuest(Dynarmic::A32::Jit& jit, abr::Runtime& rt, const abr::LoadedModule& mod,
              std::uint32_t target, const std::vector<std::uint32_t>& words, std::uint32_t* r0,
              int* hr_out) {
    auto& regs = jit.Regs();
    regs.fill(0);
    std::uint32_t sp = abr::kStackTop - 0x1000u;
    const std::size_t nstack = words.size() > 4 ? words.size() - 4 : 0;
    sp -= static_cast<std::uint32_t>(((nstack * 4) + 7) & ~std::size_t(7));
    for (std::size_t i = 0; i < nstack; i++) {
        std::memcpy(rt.H(sp + static_cast<std::uint32_t>(i * 4)), &words[4 + i], 4);
    }
    for (std::size_t i = 0; i < words.size() && i < 4; i++) {
        regs[i] = words[i];
    }
    regs[13] = sp;
    regs[14] = mod.halt_va;
    SetEntry(jit, target);
    rt.stop_step_ = false;
    const Dynarmic::HaltReason hr = abr::RunJit(jit);
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
    *r0 = regs[0];
    *hr_out = static_cast<int>(hr);
    return hr == Dynarmic::HaltReason::UserDefined1 && !rt.stop_step_;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: guestrun <lib.so> [--files dir] [--pkg name] [--call sym args...]...\n");
        return 2;
    }
    std::string files_dir, pkg = "abr.guestrun";
    std::vector<Call> calls;
    std::vector<std::string> libs = {argv[1]};
    std::uint32_t base = 0;
    std::string init_only;  // --init: only this library's initialisers now
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--lib" && i + 1 < argc && calls.empty()) {
            libs.push_back(argv[++i]);
        } else if (a == "--base" && i + 1 < argc && calls.empty()) {
            base = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--init" && i + 1 < argc && calls.empty()) {
            init_only = argv[++i];
        } else if (a == "--files" && i + 1 < argc) {
            files_dir = argv[++i];
        } else if (a == "--pkg" && i + 1 < argc) {
            pkg = argv[++i];
        } else if (a == "--call" && i + 1 < argc) {
            calls.push_back({argv[++i], {}});
        } else if (!calls.empty()) {
            calls.back().args.push_back(a);
        } else {
            std::printf("unexpected argument %s\n", a.c_str());
            return 2;
        }
    }

    abr::GuestMemory mem;
    if (!mem.ok()) {
        std::printf("guest reservation failed\n");
        return 1;
    }
    abr::ElfLoader loader(mem);
    abr::LoadedModule mod;
    std::string err;
    std::uint32_t at = base;
    for (const std::string& lib : libs) {
        if (!loader.Load(lib.c_str(), mod, err, at)) {
            std::printf("load %s failed: %s\n", lib.c_str(), err.c_str());
            return 1;
        }
        const std::uint32_t hi = mod.more.empty() ? mod.span_hi : mod.more.back().span_hi;
        std::printf("loaded %s at %08x: %zu host imports, %zu init, %zu exports so far\n",
                    lib.c_str(), at, mod.imports.size(), mod.init_array.size(), mod.defined.size());
        at = (hi + 0xFFFFFFu) & ~0xFFFFFFu;
    }

    abr::Runtime rt(mem, mod);
    rt.AddModuleSegments(mod);
    std::random_device rd;
    const std::uint32_t canary = (static_cast<std::uint32_t>(rd()) << 1) | 1u;
    std::memcpy(mem.host(abr::kGuestDataBase + 0), &canary, 4);
    rt.InitDataImports();
    rt.ProtectRelro(mod);
    if (!files_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(files_dir, ec);
        rt.SetFilesDir("/data/data/" + pkg + "/files", files_dir);
        std::printf("files: /data/data/%s/files -> %s\n", pkg.c_str(), files_dir.c_str());
    }

    abr::GuestCallbacks callbacks(&rt);
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = &callbacks;
    cfg.optimizations =
        abr::JitSafeOpts() ? Dynarmic::all_safe_optimizations : Dynarmic::no_optimizations;
    cfg.enable_cycle_counting = abr::kJitCycleCounting;
    if (abr::JitFastmemOn()) {
        cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(rt.mem().base());
    }
    std::size_t cpu = 0;
    rt.AcquireCpuId(&cpu);
    cfg.global_monitor = &rt.Monitor();
    cfg.processor_id = cpu;
    rt.set_callbacks(&callbacks);
    const auto jit_owner = rt.NewJit(cfg);
    Dynarmic::A32::Jit& jit = *jit_owner;
    rt.set_jit(&jit);
    rt.ThreadEnter(&jit, rt.AllocTid());

    if (!init_only.empty()) {
        // As on a device: the library Java loads (and the guest libraries it
        // needs) now, the others when the guest dlopens them.
        const int lib = rt.GuestLibraryByName(init_only);
        const bool ok = lib >= 0 && rt.InitGuestLibrary(lib);
        std::printf("[init] %s -> %s\n", init_only.c_str(), ok ? "ok" : "FAILED");
        if (!ok) {
            return 1;
        }
    }
    for (std::size_t k = 0; init_only.empty() && k < mod.init_array.size(); k++) {
        const std::uint32_t fn = mod.init_array[k];
        if (fn == 0 || fn == 0xFFFFFFFFu) {
            continue;
        }
        std::uint32_t r0 = 0;
        int hr = 0;
        const bool ok = RunGuest(jit, rt, mod, fn, {}, &r0, &hr);
        std::printf("[init] #%zu @%08x -> %s (hr=%d)\n", k, fn, ok ? "ok" : "HALTED", hr);
        if (!ok) {
            return 1;
        }
    }

    for (std::size_t l = 0; init_only.empty() && l < mod.libs.size(); l++) {
        rt.MarkGuestLibraryInitialised(static_cast<int>(l));  // all ran above
    }
    std::vector<std::uint32_t> results;
    for (std::size_t c = 0; c < calls.size(); c++) {
        // Pseudo-calls that read results: @read32 <addr> (prints the word at
        // addr, result = the word), @str <addr> (prints a guest C string);
        // @sleep <ms> waits.
        if (calls[c].sym == "@sleep") {
            // @sleep <ms>: lets guest threads run (result 0).
            const long ms = calls[c].args.empty() ? 0 : std::strtol(calls[c].args[0].c_str(), nullptr, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            std::printf("call %zu @sleep(%ld)\n", c, ms);
            results.push_back(0);
            continue;
        }
        if (calls[c].sym == "@read32" || calls[c].sym == "@str") {
            const std::string& a = calls[c].args.empty() ? std::string("0") : calls[c].args[0];
            std::uint32_t addr = 0;
            if (!a.empty() && a[0] == '$') {
                const std::size_t idx = std::strtoul(a.c_str() + 1, nullptr, 10);
                addr = idx < results.size() ? results[idx] : 0;
            } else {
                addr = static_cast<std::uint32_t>(std::strtoul(a.c_str(), nullptr, 0));
            }
            std::uint32_t w = 0;
            if (calls[c].sym == "@read32") {
                if (rt.CheckRange(addr, 4, false)) {
                    std::memcpy(&w, rt.H(addr), 4);
                }
                std::printf("call %zu @read32(%08x) = %08x (%u)\n", c, addr, w, w);
            } else {
                std::printf("call %zu @str(%08x) = \"%s\"\n", c, addr,
                            rt.CheckRange(addr, 1, false) ? rt.GuestStr(addr) : "<unreadable>");
            }
            results.push_back(w);
            continue;
        }
        std::uint32_t target = 0;
        if (calls[c].sym.rfind("0x", 0) == 0) {
            target = static_cast<std::uint32_t>(std::strtoul(calls[c].sym.c_str(), nullptr, 16));
        } else {
            const auto it = mod.defined.find(calls[c].sym);
            if (it == mod.defined.end()) {
                std::printf("call %zu: no export %s\n", c, calls[c].sym.c_str());
                return 1;
            }
            target = it->second;
        }
        std::vector<std::uint32_t> words;
        std::string shown;
        for (const std::string& a : calls[c].args) {
            std::uint32_t w = 0;
            if (a.rfind("s:", 0) == 0) {
                const std::string s = a.substr(2);
                w = rt.GuestMalloc(static_cast<std::uint32_t>(s.size() + 1));
                std::memcpy(rt.H(w), s.c_str(), s.size() + 1);
            } else if (a == "@vm") {
                w = rt.Jvm();  // the mock JavaVM* (as JNI_OnLoad gets it)
            } else if (a == "@env") {
                w = rt.JniEnv();  // the guest JNIEnv*
            } else if (a.rfind("j:", 0) == 0) {
                w = rt.JniNewString(a.substr(2));  // a jstring
            } else if (!a.empty() && a[0] == '$') {
                const std::size_t idx = std::strtoul(a.c_str() + 1, nullptr, 10);
                w = idx < results.size() ? results[idx] : 0;
            } else {
                w = static_cast<std::uint32_t>(std::strtoul(a.c_str(), nullptr, 0));
            }
            words.push_back(w);
            char b[24];
            std::snprintf(b, sizeof(b), "%s%08x", shown.empty() ? "" : ", ", w);
            shown += b;
        }
        std::uint32_t r0 = 0;
        int hr = 0;
        const bool ok = RunGuest(jit, rt, mod, target, words, &r0, &hr);
        results.push_back(r0);
        std::printf("call %zu %s(%s) -> %08x (%s, hr=%d)\n", c, calls[c].sym.c_str(),
                    shown.c_str(), r0, ok ? "ok" : "HALTED", hr);
        if (!ok) {
            return 1;
        }
    }
    std::printf("guestrun: done\n");
    std::fflush(stdout);
    _exit(0);
}
