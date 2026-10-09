// Minimal ARM32 ET_DYN loader: maps PT_LOADs at linked addresses inside the
// 4GB reservation, applies REL/JMPREL relocations, emits SVC trampolines for
// every undefined import.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "guest_memory.h"

namespace abr {

struct Import {
    std::string name;
    bool is_function;  // false => data object (needs a guest address, not a call trampoline)
    std::uint32_t tramp_va = 0;  // guest VA of `svc` trampoline (functions) / data slot
};

struct PendingData {
    std::uint32_t loc_va = 0;  // relocation target to patch
    std::uint32_t import_idx = 0;
    std::uint32_t addend = 0;  // original *loc value
};

struct LoadSegment {
    std::uint32_t start = 0, size = 0;
    int prot = 0;
};

struct LoadedModule {
    std::string path;
    std::uint32_t bias = 0;  // load bias (0: mapped at linked addresses)
    std::uint32_t halt_va = 0;  // `svc #0` trampoline: LR target for calls into guest
    std::vector<LoadSegment> segments;  // mapped LOAD ranges (for access tracking)
    std::vector<std::uint32_t> init_array;
    std::vector<std::uint32_t> fini_array;
    std::vector<Import> imports;  // index = trampoline id (swi = id+1)
    std::vector<PendingData> pending_data;  // ABS32 data refs to patch later
    std::uint32_t jni_tramp_base = 0;  // 256 `svc` trampolines: swi = kSwiJniBase+i
    std::uint32_t jvm_tramp_base = 0;  // 8 `svc` trampolines: swi = kSwiJniBase+0x100+i
    std::uint32_t exidx_va = 0;        // .ARM.exidx guest VA (0 if absent)
    std::uint32_t exidx_count = 0;     // number of 8-byte entries
};

class ElfLoader {
public:
    explicit ElfLoader(GuestMemory& mem) : mem_(mem) {}

    // Loads file into guest memory. Returns false + error string on failure.
    bool Load(const char* path, LoadedModule& out, std::string& err);

    // Patches all pending ABS32 refs of a data import to addend+guest_addr.
    bool ResolveData(LoadedModule& mod, std::uint32_t import_idx, std::uint32_t guest_addr);

private:
    GuestMemory& mem_;
};

}  // namespace abr
