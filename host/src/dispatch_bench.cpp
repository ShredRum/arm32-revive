// Cost of guest branches on arm64: a counted loop, alone, with a call and return
// (the return stack answers) and with an indirect call (the dispatcher answers),
// on two Jits that share a code cache, as the game does.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace {

class Env : public Dynarmic::A32::UserCallbacks {
public:
    std::vector<std::uint32_t> mem = std::vector<std::uint32_t>(0x1000, 0);
    Dynarmic::A32::Jit* jit = nullptr;

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override { return mem[(addr / 4) % mem.size()]; }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { return 0; }
    void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override {}
    void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override {}
    void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override {}
    void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override {}
    void InterpreterFallback(Dynarmic::A32::VAddr, std::size_t) override {
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override {
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
};

double Run(Env& env, Dynarmic::A32::Jit& jit, std::uint32_t pc, std::uint32_t iters) {
    jit.Regs()[0] = iters;
    jit.Regs()[15] = pc;
    env.jit = &jit;
    const auto t0 = std::chrono::steady_clock::now();
    jit.Run();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

}  // namespace

int main() {
    Env env;
    const std::array<std::uint32_t, 3> plain = {
        0xE2500001u,  // loop: subs r0, r0, #1
        0x1AFFFFFDu,  // bne loop
        0xEF000000u,  // svc #0
    };
    const std::array<std::uint32_t, 5> calls = {
        0xEB000002u,  // loop: bl f
        0xE2500001u,  // subs r0, r0, #1
        0x1AFFFFFCu,  // bne loop
        0xEF000000u,  // svc #0
        0xE12FFF1Eu,  // f: bx lr
    };
    const std::array<std::uint32_t, 5> indirect = {
        0xE12FFF31u,  // loop: blx r1
        0xE2500001u,  // subs r0, r0, #1
        0x1AFFFFFCu,  // bne loop
        0xEF000000u,  // svc #0
        0xE12FFF1Eu,  // f: bx lr
    };
    for (std::size_t i = 0; i < plain.size(); i++) {
        env.mem[0x1000 / 4 + i] = plain[i];
    }
    for (std::size_t i = 0; i < calls.size(); i++) {
        env.mem[0x2000 / 4 + i] = calls[i];
    }
    for (std::size_t i = 0; i < indirect.size(); i++) {
        env.mem[0x3000 / 4 + i] = indirect[i];
    }
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = &env;
    cfg.enable_cycle_counting = false;
    Dynarmic::A32::Jit first(cfg);
    Dynarmic::A32::Jit jit(cfg, first.GetSharedCodeCache());
    Run(env, jit, 0x1000, 1000);  // compile
    Run(env, jit, 0x2000, 1000);
    jit.Regs()[1] = 0x3010;
    Run(env, jit, 0x3000, 1000);
    for (int round = 0; round < 3; round++) {
        const double base = Run(env, jit, 0x1000, 400000000u);
        const double call = Run(env, jit, 0x2000, 40000000u);
        jit.Regs()[1] = 0x3010;
        const double ind = Run(env, jit, 0x3000, 4000000u);
        std::printf("loop %.2f ns, call and return +%.2f ns, indirect call and return +%.2f ns\n", base,
                    call - base, ind - base);
    }
    return 0;
}
