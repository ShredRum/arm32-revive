// Smoke test: hand-assembled ARM program through Dynarmic A32 JIT.
//   mov r0, #42; mov r1, #23; add r2, r0, r1
//   mov r3, #0x20; str r2, [r3]; ldr r4, [r3]; svc #0
// Expects r2 == r4 == 65 and a clean halt via CallSVC.
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace {

class Env : public Dynarmic::A32::UserCallbacks {
public:
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(65536, 0);
    Dynarmic::A32::Jit* jit = nullptr;

    template<typename T>
    T Read(Dynarmic::A32::VAddr addr) {
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); i++) {
            value |= T(mem[(addr + i) % mem.size()]) << (8 * i);
        }
        return value;
    }

    template<typename T>
    void Write(Dynarmic::A32::VAddr addr, T value) {
        for (std::size_t i = 0; i < sizeof(T); i++) {
            mem[(addr + i) % mem.size()] = std::uint8_t(value >> (8 * i));
        }
    }

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr addr) override { return Read<std::uint8_t>(addr); }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr addr) override { return Read<std::uint16_t>(addr); }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr addr) override { return Read<std::uint32_t>(addr); }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr addr) override { return Read<std::uint64_t>(addr); }
    void MemoryWrite8(Dynarmic::A32::VAddr addr, std::uint8_t v) override { Write(addr, v); }
    void MemoryWrite16(Dynarmic::A32::VAddr addr, std::uint16_t v) override { Write(addr, v); }
    void MemoryWrite32(Dynarmic::A32::VAddr addr, std::uint32_t v) override { Write(addr, v); }
    void MemoryWrite64(Dynarmic::A32::VAddr addr, std::uint64_t v) override { Write(addr, v); }

    void InterpreterFallback(Dynarmic::A32::VAddr pc, std::size_t n) override {
        std::printf("InterpreterFallback pc=%08x n=%zu (unexpected)\n", pc, n);
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void CallSVC(std::uint32_t swi) override {
        std::printf("CallSVC swi=%u\n", swi);
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }

    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception ex) override {
        std::printf("ExceptionRaised pc=%08x ex=%d\n", pc, static_cast<int>(ex));
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
};

}  // namespace

int main() {
    Env env;
    const std::array<std::uint32_t, 7> prog = {
        0xE3A0002Au,  // mov r0, #42
        0xE3A01017u,  // mov r1, #23
        0xE0802001u,  // add r2, r0, r1
        0xE3A03020u,  // mov r3, #0x20
        0xE5832000u,  // str r2, [r3]
        0xE5934000u,  // ldr r4, [r3]
        0xEF000000u,  // svc #0
    };
    for (std::size_t i = 0; i < prog.size(); i++) {
        env.Write<std::uint32_t>(0x1000 + std::uint32_t(i * 4), prog[i]);
    }

    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = &env;
    cfg.enable_cycle_counting = false;
    Dynarmic::A32::Jit jit(cfg);
    env.jit = &jit;
    jit.Regs()[15] = 0x1000;

    const Dynarmic::HaltReason hr = jit.Run();
    const auto& regs = jit.Regs();
    std::printf("halt=%d r0=%u r1=%u r2=%u r4=%u\n", static_cast<int>(hr), regs[0], regs[1], regs[2],
                regs[4]);

    if (hr != Dynarmic::HaltReason::UserDefined1 || regs[2] != 65 || regs[4] != 65) {
        std::printf("SMOKE FAIL\n");
        return 1;
    }
    std::printf("SMOKE OK\n");
    return 0;
}
