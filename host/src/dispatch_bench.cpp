// Cost of guest branches on arm64: a counted loop, alone, with a call and return
// (the return stack answers) and with an indirect call (the dispatcher answers),
// on two Jits that share a code cache, as the game does.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <sys/mman.h>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace {

// Guest memory as in the game: the whole 4 GiB reserved, so fastmem works.
class Env : public Dynarmic::A32::UserCallbacks {
public:
    std::uint32_t* mem = static_cast<std::uint32_t*>(
        mmap(nullptr, std::size_t{1} << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    Dynarmic::A32::Jit* jit = nullptr;

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { return 0; }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { return 0; }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override { return mem[addr / 4]; }
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

int main(int argc, char**) {
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
    const std::array<std::uint32_t, 9> fp_memory = {
        0xEE300A20u,  // loop: vadd.f32 s0, s0, s1
        0xE5932000u,  // ldr r2, [r3]
        0xEE211A20u,  // vmul.f32 s2, s2, s1
        0xE5832004u,  // str r2, [r3, #4]
        0xEE300A01u,  // vadd.f32 s0, s0, s2
        0xE5932008u,  // ldr r2, [r3, #8]
        0xE2500001u,  // subs r0, r0, #1
        0x1AFFFFF7u,  // bne loop
        0xEF000000u,  // svc #0
    };
    for (std::size_t i = 0; i < fp_memory.size(); i++) {
        env.mem[0x4000 / 4 + i] = fp_memory[i];
    }
    const std::array<std::uint32_t, 8> conditional_moves = {
        0xEE300A20u,  // loop: vadd.f32 s0, s0, s1
        0xEEB40A41u,  // vcmp.f32 s0, s2
        0xEEF1FA10u,  // vmrs APSR_nzcv, fpscr
        0xCEB00A61u,  // vmovgt.f32 s0, s3
        0xDEB02A40u,  // vmovle.f32 s4, s0
        0xE2500001u,  // subs r0, r0, #1
        0x1AFFFFF8u,  // bne loop
        0xEF000000u,  // svc #0
    };
    for (std::size_t i = 0; i < conditional_moves.size(); i++) {
        env.mem[0x5000 / 4 + i] = conditional_moves[i];
    }
    const std::array<std::uint32_t, 7> branchy = {
        0xE3100001u,  // loop: tst r0, #1
        0x0A000000u,  // beq skip
        0xE2822001u,  // add r2, r2, #1
        0xE2811003u,  // skip: add r1, r1, #3
        0xE2500001u,  // subs r0, r0, #1
        0x1AFFFFF9u,  // bne loop
        0xEF000000u,  // svc #0
    };
    for (std::size_t i = 0; i < branchy.size(); i++) {
        env.mem[0x6000 / 4 + i] = branchy[i];
    }
    const std::array<std::uint32_t, 6> neon_copy = {
        0xF4210A8Fu,  // loop: vld1.32 {d0-d1}, [r1]
        0xF2000D42u,  // vadd.f32 q0, q0, q1
        0xF4020A8Fu,  // vst1.32 {d0-d1}, [r2]
        0xE2500001u,  // subs r0, r0, #1
        0x1AFFFFFAu,  // bne loop
        0xEF000000u,  // svc #0
    };
    for (std::size_t i = 0; i < neon_copy.size(); i++) {
        env.mem[0x7000 / 4 + i] = neon_copy[i];
    }
    Dynarmic::A32::UserConfig cfg;
    cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(env.mem);
    cfg.callbacks = &env;
    cfg.enable_cycle_counting = false;
    if (argc > 1) {  // any argument: the C++ dispatcher only
        cfg.optimizations = Dynarmic::all_safe_optimizations & ~Dynarmic::OptimizationFlag::FastDispatch;
    }
    Dynarmic::A32::Jit first(cfg);
    Dynarmic::A32::Jit jit(cfg, first.GetSharedCodeCache());
    Run(env, jit, 0x1000, 1000);  // compile
    Run(env, jit, 0x2000, 1000);
    jit.Regs()[3] = 0x8000;
    Run(env, jit, 0x4000, 1000);
    jit.Regs()[1] = 0x3010;
    Run(env, jit, 0x3000, 1000);
    for (int round = 0; round < 3; round++) {
        const double base = Run(env, jit, 0x1000, 400000000u);
        const double call = Run(env, jit, 0x2000, 40000000u);
        jit.Regs()[1] = 0x3010;
        const double ind = Run(env, jit, 0x3000, 4000000u);
        jit.Regs()[3] = 0x8000;
        const double fp = Run(env, jit, 0x4000, 100000000u);
        const double cmov = Run(env, jit, 0x5000, 100000000u);
        const double branch = Run(env, jit, 0x6000, 200000000u);
        jit.Regs()[1] = 0x9000;
        jit.Regs()[2] = 0x9100;
        const double neon = Run(env, jit, 0x7000, 100000000u);
        std::printf("loop %.2f ns, call and return +%.2f ns, indirect call and return +%.2f ns, "
                    "fp and memory loop %.2f ns, conditional vmov loop %.2f ns, branchy loop %.2f ns, "
                    "neon load-add-store loop %.2f ns\n",
                    base, call - base, ind - base, fp, cmov, branch, neon);
    }
    return 0;
}
