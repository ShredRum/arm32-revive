// Minimal ARM32 ET_DYN loader: maps PT_LOADs at linked addresses inside the
// 4GB reservation, applies REL/JMPREL relocations, emits SVC trampolines for
// every undefined import.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_memory.h"

namespace abr {

// Fast-path identity for the math imports a guest can call in a tight loop.
// Seasons' boot spent tens of seconds in a guest-side sort whose comparator
// called sqrt millions of times; every one of those calls walked the ~200
// name comparisons in Runtime::CallImport, which cost far more than the libm
// call itself. The identity is resolved ONCE at load time, so the per-call
// dispatch is a switch.
enum class FastMath : std::uint8_t {
    None = 0,
    Sqrt = 1,
    Atan = 2,
    Sin = 3,
    Cos = 4,
    Floor = 5,
    Fmod = 6,
    Pow = 7,
    Tan = 8,
};

inline FastMath FastMathForName(const std::string& name) {
    if (name == "sqrt") return FastMath::Sqrt;
    if (name == "atan") return FastMath::Atan;
    if (name == "sin") return FastMath::Sin;
    if (name == "cos") return FastMath::Cos;
    if (name == "floor") return FastMath::Floor;
    if (name == "fmod") return FastMath::Fmod;
    if (name == "pow") return FastMath::Pow;
    if (name == "tan") return FastMath::Tan;
    return FastMath::None;
}

struct Import {
    std::string name;
    bool is_function;  // false => data object (needs a guest address, not a call trampoline)
    std::uint32_t tramp_va = 0;  // guest VA of `svc` trampoline (functions) / data slot
    std::uint32_t call_va = 0;   // guest VA the guest actually branches to (== tramp_va
                                  // unless the import runs guest-side VFP code)
    std::uint8_t fast_math = 0;  // FastMath id (0 = not a fast-math import)
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
    std::uint32_t once_va = 0;  // `svc #kSwiOnce`: LR target for a pthread_once init routine
    std::vector<LoadSegment> segments;  // mapped LOAD ranges (for access tracking)
    std::vector<std::uint32_t> init_array;
    std::vector<std::uint32_t> fini_array;
    std::vector<Import> imports;  // index = trampoline id (swi = id+1)
    std::vector<PendingData> pending_data;  // ABS32 data refs to patch later
    std::uint32_t jni_tramp_base = 0;  // 256 `svc` trampolines: swi = kSwiJniBase+i
    std::uint32_t jvm_tramp_base = 0;  // 8 `svc` trampolines: swi = kSwiJniBase+0x100+i
    std::uint32_t exidx_va = 0;        // .ARM.exidx guest VA (0 if absent)
    std::uint32_t exidx_count = 0;     // number of 8-byte entries
    // PT_GNU_RELRO as the real linker protects it (glibc page rounding). Holds
    // .got/.dynamic/init_array: read-only after relocation, never written at
    // runtime. The host refuses guest writes there (see Runtime::AddWriteGuard).
    std::uint32_t relro_start = 0;
    std::uint32_t relro_size = 0;
    std::vector<std::uint8_t> build_id;  // GNU build-id (NOTE desc; empty if absent)
    std::unordered_map<std::string, std::uint32_t> defined;  // DEFINED sym name -> st_value (for apphost Java_* lookup)
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

// G3.4 (2026-10-18): the pinned guest image. Every hardcoded guest VA, patch
// site and behavioural hook in the host assumes exactly this .so
// (AngryBirdsRio 2.6.13, sha256 980fad194cc6aefa56b4860965cefcf1de83dbb84c4d06a56c219327fe06486b).
// The GNU build-id is a 20-byte fingerprint of that image, checkable at load.
inline const std::vector<std::uint8_t>& PinnedGuestBuildId() {
    static const std::vector<std::uint8_t> id = {
        0x2b, 0x1e, 0x20, 0x7c, 0xc3, 0x0a, 0xd6, 0x4f, 0xac, 0x6b,
        0x1f, 0x23, 0x4c, 0x8a, 0x4c, 0x0f, 0xbb, 0xdb, 0x3e, 0x36,
    };
    return id;
}

// True when the loaded module is the pinned image (build-id matches). The
// host's engine VAs and behavioural crutches belong to that one image; a
// different guest must run without them.
inline bool IsPinnedGuest(const LoadedModule& mod) {
    return !mod.build_id.empty() && mod.build_id == PinnedGuestBuildId();
}

// One LOUD line if the loaded module is not the pinned image (absent, empty or
// different build-id). Never fatal: the host still runs, but every pinned VA is
// suspect, so the caller must know.
//
// identity_verified: the caller independently confirmed the image against the
// SHA-256 the packaging step put in the APK (see AppHost). Then the image IS
// the expected one for this title and the missing/different build-id note is
// a property of that image, not an anomaly -- so it is reported plainly
// instead of LOUD.
inline void CheckGuestBuildId(const LoadedModule& mod, bool identity_verified = false) {
    const std::vector<std::uint8_t>& pin = PinnedGuestBuildId();
    if (IsPinnedGuest(mod)) {
        return;
    }
    if (identity_verified) {
        std::printf("[guest] image fingerprint verified; this image has no matching GNU build-id note "
                    "(host pins VAs/hooks for the Rio image, which do not apply here)\n");
        return;
    }
    std::printf("[guest] LOUD build-id mismatch -- host pins VAs/hooks for the known image\n");
    std::printf("[guest] LOUD   loaded (%zu B):", mod.build_id.size());
    for (std::uint8_t b : mod.build_id) {
        std::printf(" %02x", b);
    }
    std::printf("\n[guest] LOUD   pinned (%zu B):", pin.size());
    for (std::uint8_t b : pin) {
        std::printf(" %02x", b);
    }
    std::printf("\n");
}

}  // namespace abr
