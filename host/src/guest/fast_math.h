// Fast-path math imports: the pure value functions of libm, resolved to a
// table index ONCE at load time (Import::fast_math = index + 1), so a call is
// one indexed jump instead of a walk down the import-name chain in
// Runtime::CallImport.
//
// Why: Seasons' boot sorted with a comparator that called sqrt millions of
// times, and Minecraft: Story Mode calls sinf ~50000 times a second; through
// the name chain each call cost ~2.5 us on the sandbox instead of ~20 ns.
//
// Values and the softfp convention match math_thunks.inc (the chain keeps
// the functions with pointers or stack arguments: modf, frexp, fma, remquo,
// ...): a float in r0, a double in r0:r1 (low word first), the second double
// in r2:r3; the result in r0 (float) or r0:r1 (double). nearbyint(f) is
// rint(f), as there (the default rounding mode is the only one guests use).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <math.h>

namespace abr {

enum class MathKind : std::uint8_t { F_F, D_D, FF_F, DD_D };

struct FastMathEntry {
    const char* name;
    MathKind kind;
    float (*f_f)(float);
    double (*d_d)(double);
    float (*ff_f)(float, float);
    double (*dd_d)(double, double);
};

#define ABR_FF(n, f) {n, MathKind::F_F, static_cast<float (*)(float)>(&::f), nullptr, nullptr, nullptr}
#define ABR_DD(n, f) {n, MathKind::D_D, nullptr, static_cast<double (*)(double)>(&::f), nullptr, nullptr}
#define ABR_FF2(n, f) \
    {n, MathKind::FF_F, nullptr, nullptr, static_cast<float (*)(float, float)>(&::f), nullptr}
#define ABR_DD2(n, f) \
    {n, MathKind::DD_D, nullptr, nullptr, nullptr, static_cast<double (*)(double, double)>(&::f)}

inline const FastMathEntry kFastMath[] = {
    // float -> float
    ABR_FF("acosf", acosf), ABR_FF("asinf", asinf), ABR_FF("atanf", atanf),
    ABR_FF("cbrtf", cbrtf), ABR_FF("ceilf", ceilf), ABR_FF("cosf", cosf),
    ABR_FF("coshf", coshf), ABR_FF("erff", erff), ABR_FF("erfcf", erfcf),
    ABR_FF("expf", expf), ABR_FF("exp2f", exp2f), ABR_FF("expm1f", expm1f),
    ABR_FF("floorf", floorf), ABR_FF("logf", logf), ABR_FF("log10f", log10f),
    ABR_FF("log2f", log2f), ABR_FF("rintf", rintf), ABR_FF("nearbyintf", rintf),
    ABR_FF("roundf", roundf), ABR_FF("truncf", truncf), ABR_FF("sinf", sinf),
    ABR_FF("sinhf", sinhf), ABR_FF("sqrtf", sqrtf), ABR_FF("tanf", tanf),
    ABR_FF("tanhf", tanhf), ABR_FF("tgammaf", tgammaf), ABR_FF("lgammaf", lgammaf),
    // double -> double
    ABR_DD("acos", acos), ABR_DD("asin", asin), ABR_DD("atan", atan), ABR_DD("cbrt", cbrt),
    ABR_DD("ceil", ceil), ABR_DD("cos", cos), ABR_DD("cosh", cosh), ABR_DD("erf", erf),
    ABR_DD("erfc", erfc), ABR_DD("exp", exp), ABR_DD("exp2", exp2), ABR_DD("expm1", expm1),
    ABR_DD("floor", floor), ABR_DD("log", log), ABR_DD("log10", log10), ABR_DD("log2", log2),
    ABR_DD("log1p", log1p), ABR_DD("rint", rint), ABR_DD("nearbyint", rint),
    ABR_DD("round", round), ABR_DD("trunc", trunc), ABR_DD("sin", sin), ABR_DD("sinh", sinh),
    ABR_DD("sqrt", sqrt), ABR_DD("tan", tan), ABR_DD("tanh", tanh), ABR_DD("tgamma", tgamma),
    ABR_DD("lgamma", lgamma), ABR_DD("acosh", acosh), ABR_DD("asinh", asinh),
    ABR_DD("atanh", atanh),
    // (float, float) -> float
    ABR_FF2("atan2f", atan2f), ABR_FF2("powf", powf), ABR_FF2("fmodf", fmodf),
    ABR_FF2("hypotf", hypotf), ABR_FF2("copysignf", copysignf), ABR_FF2("fminf", fminf),
    ABR_FF2("fmaxf", fmaxf), ABR_FF2("fdimf", fdimf), ABR_FF2("remainderf", remainderf),
    // (double, double) -> double
    ABR_DD2("atan2", atan2), ABR_DD2("pow", pow), ABR_DD2("fmod", fmod), ABR_DD2("hypot", hypot),
    ABR_DD2("copysign", copysign), ABR_DD2("fmin", fmin), ABR_DD2("fmax", fmax),
    ABR_DD2("fdim", fdim), ABR_DD2("remainder", remainder),
};

#undef ABR_FF
#undef ABR_DD
#undef ABR_FF2
#undef ABR_DD2

constexpr std::size_t kFastMathCount = sizeof(kFastMath) / sizeof(kFastMath[0]);
static_assert(kFastMathCount < 255, "fast_math ids must fit one byte");

// The load-time id: table index + 1, or 0 for a name not in the table.
inline std::uint8_t FastMathForName(const char* name) {
    for (std::size_t i = 0; i < kFastMathCount; i++) {
        if (std::strcmp(kFastMath[i].name, name) == 0) {
            return static_cast<std::uint8_t>(i + 1);
        }
    }
    return 0;
}

}  // namespace abr
