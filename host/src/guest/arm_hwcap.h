// ARM32 Linux HWCAP bits, from the NDK
// arm-linux-androideabi/asm/hwcap.h. The arm64 HWCAP layout is different:
// do not reuse these names for the host's own getauxval.
#pragma once
#include <cstdint>

namespace abr {

inline constexpr std::uint32_t kHwcapSwp = 1u << 0;
inline constexpr std::uint32_t kHwcapHalf = 1u << 1;
inline constexpr std::uint32_t kHwcapThumb = 1u << 2;
inline constexpr std::uint32_t kHwcap26Bit = 1u << 3;
inline constexpr std::uint32_t kHwcapFastMult = 1u << 4;
inline constexpr std::uint32_t kHwcapFpa = 1u << 5;
inline constexpr std::uint32_t kHwcapVfp = 1u << 6;
inline constexpr std::uint32_t kHwcapEdsp = 1u << 7;
inline constexpr std::uint32_t kHwcapJava = 1u << 8;
inline constexpr std::uint32_t kHwcapIwmmxt = 1u << 9;
inline constexpr std::uint32_t kHwcapCrunch = 1u << 10;
inline constexpr std::uint32_t kHwcapThumbee = 1u << 11;
inline constexpr std::uint32_t kHwcapNeon = 1u << 12;
inline constexpr std::uint32_t kHwcapVfpv3 = 1u << 13;
inline constexpr std::uint32_t kHwcapVfpv3D16 = 1u << 14;
inline constexpr std::uint32_t kHwcapTls = 1u << 15;
inline constexpr std::uint32_t kHwcapVfpv4 = 1u << 16;
inline constexpr std::uint32_t kHwcapIdiva = 1u << 17;
inline constexpr std::uint32_t kHwcapIdivt = 1u << 18;
inline constexpr std::uint32_t kHwcapIdiv = kHwcapIdiva | kHwcapIdivt;
inline constexpr std::uint32_t kHwcapVfpd32 = 1u << 19;
inline constexpr std::uint32_t kHwcapLpae = 1u << 20;
inline constexpr std::uint32_t kHwcapEvtstrm = 1u << 21;
inline constexpr std::uint32_t kHwcapFphp = 1u << 22;
inline constexpr std::uint32_t kHwcapAsimdhp = 1u << 23;
inline constexpr std::uint32_t kHwcapAsimddp = 1u << 24;
inline constexpr std::uint32_t kHwcapAsimdfhm = 1u << 25;
inline constexpr std::uint32_t kHwcapAsimdbf16 = 1u << 26;
inline constexpr std::uint32_t kHwcapI8mm = 1u << 27;

// The armv7-a feature set this process advertises. Dynarmic implements the
// ARMv7-A integer, VFPv3 and ASIMD/NEON instructions; the kuser page supplies
// TLS. VFPv3D16 is deliberately conservative (at least 16 D registers); the
// NEON path uses the ASIMD register file. SWP, FPA, JAVA, iWMMXt, Crunch,
// ThumbEE, 26-bit, LPAE, VFPv4, IDIV and FPHP are not advertised.
inline constexpr std::uint32_t kArmV7Hwcap =
    kHwcapHalf | kHwcapThumb | kHwcapFastMult | kHwcapVfp | kHwcapEdsp |
    kHwcapNeon | kHwcapVfpv3 | kHwcapVfpv3D16 | kHwcapTls;

// Pin the bits the guest consumer and the old false comments disagreed about.
static_assert(kHwcapFastMult == (1u << 4));
static_assert(kHwcapVfp == (1u << 6));
static_assert(kHwcapNeon == (1u << 12));
static_assert(kHwcapTls == (1u << 15));
static_assert((kArmV7Hwcap & kHwcapNeon) != 0);

}  // namespace abr
