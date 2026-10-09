#include <cstdint>
#include <cstdio>
#include <vector>
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"
struct Env : Dynarmic::A32::UserCallbacks {
    using VAddr = Dynarmic::A32::VAddr;
    using Exception = Dynarmic::A32::Exception;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(65536, 0);
    Dynarmic::A32::Jit* jit = nullptr;
    template<typename T> T Read(Dynarmic::A32::VAddr a){ T v=0; for(std::size_t i=0;i<sizeof(T);i++) v|=T(mem[(a+i)%mem.size()])<<(8*i); return v; }
    template<typename T> void Write(Dynarmic::A32::VAddr a,T v){ for(std::size_t i=0;i<sizeof(T);i++) mem[(a+i)%mem.size()]=std::uint8_t(v>>(8*i)); }
    std::uint8_t MemoryRead8(VAddr a) override { return Read<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(VAddr a) override { return Read<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(VAddr a) override { auto v = Read<std::uint32_t>(a); std::printf("rd32 %08x = %08x\n", a, v); return v; }
    std::uint64_t MemoryRead64(VAddr a) override { return Read<std::uint64_t>(a); }
    void MemoryWrite8(VAddr a,std::uint8_t v) override { Write(a,v); }
    void MemoryWrite16(VAddr a,std::uint16_t v) override { Write(a,v); }
    void MemoryWrite32(VAddr a,std::uint32_t v) override { Write(a,v); }
    void MemoryWrite64(VAddr a,std::uint64_t v) override { Write(a,v); }
    void InterpreterFallback(VAddr pc,std::size_t n) override { std::printf("fallback %08x\n",pc); jit->HaltExecution(Dynarmic::HaltReason::UserDefined2); }
    void CallSVC(std::uint32_t s) override { std::printf("svc %u\n",s); jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void ExceptionRaised(VAddr pc,Exception e) override { std::printf("EX pc=%08x ex=%d\n",pc,(int)e); jit->HaltExecution(Dynarmic::HaltReason::UserDefined2); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
};
int main() {
    Env env;
    for (std::uint32_t a = 0; a < 65536; a += 4) env.Write<std::uint32_t>(a, a);
    env.Write<std::uint32_t>(0x1000, 0xE59FF010u);  // ldr pc, [pc, #0x10]
    env.Write<std::uint32_t>(0x1018, 0x2001u);
    env.Write<std::uint16_t>(0x2000, 0x2001u);  // Thumb: movs r0, #1
    env.Write<std::uint16_t>(0x2002, 0x2102u);  // Thumb: movs r1, #2
    env.Write<std::uint16_t>(0x2004, 0xDF00u);  // Thumb: svc #0
    Dynarmic::A32::UserConfig cfg; cfg.callbacks=&env; cfg.enable_cycle_counting=false;
    Dynarmic::A32::Jit jit(cfg); env.jit=&jit;
    jit.SetCpsr(0x10);
    jit.Regs()[15]=0x1000;
    auto hr=jit.Run();
    auto& r = env.jit->Regs();
    std::printf("hr=%d r0=%08x r1=%08x (want r0=1 r1=2)\n",(int)hr,r[0],r[1]);
    return 0;
}
