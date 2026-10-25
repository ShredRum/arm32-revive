// libm thunks: pure value functions forwarded to host libm (softfp ABI:
// float bits in rN, double bits in rN:rN+1 little-endian, same on return).
// Included inside Runtime::CallImport where (n, a0-a3, regs) are in scope.
{
    auto f32 = [&](std::uint32_t b) -> float {
        float v = 0;
        std::memcpy(&v, &b, 4);
        return v;
    };
    auto b32 = [&](float v) -> std::uint32_t {
        std::uint32_t b = 0;
        std::memcpy(&b, &v, 4);
        return b;
    };
    auto f64 = [&](std::uint32_t lo, std::uint32_t hi) -> double {
        double v = 0;
        std::uint32_t w[2] = {lo, hi};
        std::memcpy(&v, w, 8);
        return v;
    };
    auto ret64 = [&](double v) {
        std::uint32_t w[2] = {0, 0};
        std::memcpy(w, &v, 8);
        Ret(regs, w[0], w[1]);
    };
    float x = f32(a0), y = f32(a1);
    double xd = f64(a0, a1), yd = f64(a2, a3);
    // float -> float
    if (n == "acosf") {
        Ret(regs, b32(acosf(x)));
        return;
    }
    if (n == "asinf") {
        Ret(regs, b32(asinf(x)));
        return;
    }
    if (n == "atanf") {
        Ret(regs, b32(atanf(x)));
        return;
    }
    if (n == "cbrtf") {
        Ret(regs, b32(cbrtf(x)));
        return;
    }
    if (n == "ceilf") {
        Ret(regs, b32(ceilf(x)));
        return;
    }
    if (n == "cosf") {
        Ret(regs, b32(cosf(x)));
        return;
    }
    if (n == "coshf") {
        Ret(regs, b32(coshf(x)));
        return;
    }
    if (n == "erff") {
        Ret(regs, b32(erff(x)));
        return;
    }
    if (n == "erfcf") {
        Ret(regs, b32(erfcf(x)));
        return;
    }
    if (n == "expf") {
        Ret(regs, b32(expf(x)));
        return;
    }
    if (n == "exp2f") {
        Ret(regs, b32(exp2f(x)));
        return;
    }
    if (n == "expm1f") {
        Ret(regs, b32(expm1f(x)));
        return;
    }
    if (n == "floorf") {
        Ret(regs, b32(floorf(x)));
        return;
    }
    if (n == "logf") {
        Ret(regs, b32(logf(x)));
        return;
    }
    if (n == "log10f") {
        Ret(regs, b32(log10f(x)));
        return;
    }
    if (n == "log2f") {
        Ret(regs, b32(log2f(x)));
        return;
    }
    if (n == "rintf" || n == "nearbyintf" || n == "roundf" || n == "truncf") {
        float r = (n == "rintf" || n == "nearbyintf")
                      ? rintf(x)
                      : (n == "roundf" ? roundf(x) : truncf(x));
        Ret(regs, b32(r));
        return;
    }
    if (n == "sinf") {
        Ret(regs, b32(sinf(x)));
        return;
    }
    if (n == "sinhf") {
        Ret(regs, b32(sinhf(x)));
        return;
    }
    if (n == "sqrtf") {
        Ret(regs, b32(sqrtf(x)));
        return;
    }
    if (n == "tanf") {
        Ret(regs, b32(tanf(x)));
        return;
    }
    if (n == "tanhf") {
        Ret(regs, b32(tanhf(x)));
        return;
    }
    if (n == "tgammaf" || n == "lgammaf") {
        Ret(regs, b32(n == "tgammaf" ? tgammaf(x) : lgammaf(x)));
        return;
    }
    // double -> double
    if (n == "acos" || n == "asin" || n == "atan" || n == "cbrt" || n == "ceil" || n == "cos" ||
        n == "cosh" || n == "erf" || n == "erfc" || n == "exp" || n == "exp2" || n == "expm1" ||
        n == "floor" || n == "log" || n == "log10" || n == "log2" || n == "log1p" || n == "rint" ||
        n == "nearbyint" || n == "round" || n == "trunc" || n == "sin" || n == "sinh" ||
        n == "sqrt" || n == "tan" || n == "tanh" || n == "tgamma" || n == "lgamma" ||
        n == "acosh" || n == "asinh" || n == "atanh") {
        double r = 0;
        if (n == "acos") {
            r = acos(xd);
        } else if (n == "asin") {
            r = asin(xd);
        } else if (n == "atan") {
            r = atan(xd);
        } else if (n == "cbrt") {
            r = cbrt(xd);
        } else if (n == "ceil") {
            r = ceil(xd);
        } else if (n == "cos") {
            r = cos(xd);
        } else if (n == "cosh") {
            r = cosh(xd);
        } else if (n == "erf") {
            r = erf(xd);
        } else if (n == "erfc") {
            r = erfc(xd);
        } else if (n == "exp") {
            r = exp(xd);
        } else if (n == "exp2") {
            r = exp2(xd);
        } else if (n == "expm1") {
            r = expm1(xd);
        } else if (n == "floor") {
            r = floor(xd);
        } else if (n == "log") {
            r = log(xd);
        } else if (n == "log10") {
            r = log10(xd);
        } else if (n == "log2") {
            r = log2(xd);
        } else if (n == "log1p") {
            // Was missing: the import fell through to the generic tail and
            // silently returned 0.0 instead of log(1+x) (Rio imports it).
            r = log1p(xd);
        } else if (n == "rint" || n == "nearbyint") {
            r = rint(xd);
        } else if (n == "round") {
            r = round(xd);
        } else if (n == "trunc") {
            r = trunc(xd);
        } else if (n == "sin") {
            r = sin(xd);
        } else if (n == "sinh") {
            r = sinh(xd);
        } else if (n == "sqrt") {
            r = sqrt(xd);
        } else if (n == "tan") {
            r = tan(xd);
        } else if (n == "tanh") {
            r = tanh(xd);
        } else if (n == "tgamma") {
            r = tgamma(xd);
        } else if (n == "lgamma") {
            r = lgamma(xd);
        } else if (n == "acosh") {
            r = acosh(xd);
        } else if (n == "asinh") {
            r = asinh(xd);
        } else {
            r = atanh(xd);
        }
        ret64(r);
        return;
    }
    // (float,float) -> float
    if (n == "atan2f" || n == "powf" || n == "fmodf" || n == "hypotf" || n == "copysignf" ||
        n == "fminf" || n == "fmaxf" || n == "fdimf" || n == "remainderf" || n == "fmaf") {
        float r = 0;
        if (n == "atan2f") {
            r = atan2f(x, y);
        } else if (n == "powf") {
            r = powf(x, y);
        } else if (n == "fmodf") {
            r = fmodf(x, y);
        } else if (n == "hypotf") {
            r = hypotf(x, y);
        } else if (n == "copysignf") {
            r = copysignf(x, y);
        } else if (n == "fminf") {
            r = fminf(x, y);
        } else if (n == "fmaxf") {
            r = fmaxf(x, y);
        } else if (n == "fdimf") {
            r = fdimf(x, y);
        } else if (n == "remainderf") {
            r = remainderf(x, y);
        } else {
            r = fmaf(x, y, f32(regs[2]));
        }
        Ret(regs, b32(r));
        return;
    }
    // (double,double) -> double
    if (n == "atan2" || n == "pow" || n == "fmod" || n == "hypot" || n == "copysign" ||
        n == "fmin" || n == "fmax" || n == "fdim" || n == "remainder" || n == "fma") {
        double r = 0;
        if (n == "atan2") {
            r = atan2(xd, yd);
        } else if (n == "pow") {
            r = pow(xd, yd);
        } else if (n == "fmod") {
            r = fmod(xd, yd);
        } else if (n == "hypot") {
            r = hypot(xd, yd);
        } else if (n == "copysign") {
            r = copysign(xd, yd);
        } else if (n == "fmin") {
            r = fmin(xd, yd);
        } else if (n == "fmax") {
            r = fmax(xd, yd);
        } else if (n == "fdim") {
            r = fdim(xd, yd);
        } else if (n == "remainder") {
            r = remainder(xd, yd);
        } else {
            // AAPCS32: only r0-r3 carry arguments. x and y fill r0:r1 and
            // r2:r3, so the third double goes on the stack at [sp], 8-byte
            // aligned (sp is 8-aligned at a public call).
            if (!CheckRange(regs[13], 8, false)) {
                goto bad_ptr;
            }
            std::uint32_t z[2] = {0, 0};
            std::memcpy(z, H(regs[13]), 8);
            r = fma(xd, yd, f64(z[0], z[1]));
        }
        ret64(r);
        return;
    }
    // int-returning classifiers
    if (n == "__fpclassifyd" || n == "__fpclassifyf") {
        int c = (n == "__fpclassifyd") ? std::fpclassify(xd) : std::fpclassify(x);
        Ret(regs, static_cast<std::uint32_t>(c));
        return;
    }
    if (n == "__isfinite" || n == "__isfinitef" || n == "__isnan" || n == "__isnanf" ||
        n == "__isnormal" || n == "__isnormalf") {
        int c = 0;
        if (n == "__isfinite") {
            c = std::isfinite(xd);
        } else if (n == "__isfinitef") {
            c = std::isfinite(x);
        } else if (n == "__isnan") {
            c = std::isnan(xd);
        } else if (n == "__isnanf") {
            c = std::isnan(x);
        } else if (n == "__isnormal") {
            c = std::isnormal(xd);
        } else {
            c = std::isnormal(x);
        }
        Ret(regs, static_cast<std::uint32_t>(c));
        return;
    }
    if (n == "__signbit" || n == "__signbitf" || n == "__signbitl") {
        int c = (n == "__signbit") ? std::signbit(xd) : std::signbit(x);
        Ret(regs, static_cast<std::uint32_t>(c));
        return;
    }
    if (n == "lrint" || n == "llrint" || n == "lrintf" || n == "llrintf" || n == "lround" ||
        n == "llround" || n == "lroundf" || n == "llroundf") {
        bool use_x = (n == "lrintf" || n == "llrintf" || n == "lroundf" || n == "llroundf");
        bool is_ll = (n == "llrint" || n == "llrintf" || n == "llround" || n == "llroundf");
        bool is_round = (n == "lround" || n == "llround" || n == "lroundf" || n == "llroundf");
        long long v = 0;
        if (use_x) {
            v = is_round ? (is_ll ? llroundf(x) : lroundf(x)) : (is_ll ? llrintf(x) : lrintf(x));
        } else {
            v = is_round ? (is_ll ? llround(xd) : lround(xd)) : (is_ll ? llrint(xd) : lrint(xd));
        }
        Ret(regs, static_cast<std::uint32_t>(v & 0xFFFFFFFFu),
            static_cast<std::uint32_t>((std::uint64_t(v) >> 32) & 0xFFFFFFFFu));
        return;
    }
    if (n == "ilogb" || n == "ilogbf" || n == "logb" || n == "logbf") {
        if (n == "ilogb" || n == "ilogbf") {
            int v = (n == "ilogb") ? ilogb(xd) : ilogbf(x);
            Ret(regs, static_cast<std::uint32_t>(v));
        } else if (n == "logb") {
            ret64(logb(xd));
        } else {
            Ret(regs, b32(logbf(x)));
        }
        return;
    }
    // double,int -> double
    if (n == "ldexp" || n == "scalbn" || n == "scalbln" || n == "scalbnl") {
        // scalbnl (long double variant) is the same on ARM32: long double ==
        // double. It was missing and silently returned 0.0 (Rio imports it).
        ret64(ldexp(xd, static_cast<int>(a2)));
        return;
    }
    if (n == "ldexpf" || n == "scalbnf" || n == "scalblnf") {
        Ret(regs, b32(ldexpf(x, static_cast<int>(a1))));
        return;
    }
    // modf/frexp with out-pointers
    if (n == "modf" || n == "modff") {
        if (n == "modf") {
            if (!CheckRange(a2, 8, true)) {
                goto bad_ptr;
            }
            double ip = 0;
            double r = modf(xd, &ip);
            std::uint32_t w[2] = {0, 0};
            std::memcpy(w, &ip, 8);
            std::memcpy(H(a2), w, 8);
            ret64(r);
        } else {
            if (!CheckRange(a1, 4, true)) {
                goto bad_ptr;
            }
            float ip = 0;
            float r = modff(x, &ip);
            std::memcpy(H(a1), &ip, 4);
            Ret(regs, b32(r));
        }
        return;
    }
    if (n == "frexp" || n == "frexpf") {
        if (n == "frexp") {
            if (!CheckRange(a2, 4, true)) {
                goto bad_ptr;
            }
            int e = 0;
            double r = frexp(xd, &e);
            std::uint32_t eu = static_cast<std::uint32_t>(e);
            std::memcpy(H(a2), &eu, 4);
            ret64(r);
        } else {
            if (!CheckRange(a1, 4, true)) {
                goto bad_ptr;
            }
            int e = 0;
            float r = frexpf(x, &e);
            std::uint32_t eu = static_cast<std::uint32_t>(e);
            std::memcpy(H(a1), &eu, 4);
            Ret(regs, b32(r));
        }
        return;
    }
    if (n == "nan" || n == "nanf" || n == "nanl") {
        if (n == "nanf" || n == "nanl") {
            Ret(regs, b32(nanf(GuestStr(a0))));
        } else {
            ret64(nan(GuestStr(a0)));
        }
        return;
    }
    if (n == "nextafter" || n == "nextafterf" || n == "nexttoward" || n == "nexttowardf") {
        if (n == "nextafter") {
            ret64(nextafter(xd, yd));
        } else if (n == "nextafterf") {
            Ret(regs, b32(nextafterf(x, y)));
        } else if (n == "nexttoward") {
            ret64(nexttoward(xd, yd));
        } else {
            Ret(regs, b32(nexttowardf(x, yd)));
        }
        return;
    }
    if (n == "remquo" || n == "remquof") {
        // AAPCS32: remquo(double, double, int*) fills r0-r3 with the doubles,
        // so quo is on the stack at [sp]; remquof(float, float, int*) passes
        // quo in r2.
        std::uint32_t qp = a2;
        if (n == "remquo") {
            if (!CheckRange(regs[13], 4, false)) {
                goto bad_ptr;
            }
            std::memcpy(&qp, H(regs[13]), 4);
        }
        if (n == "remquo") {
            if (!CheckRange(qp, 4, true)) {
                goto bad_ptr;
            }
            int q = 0;
            double r = remquo(xd, yd, &q);
            std::uint32_t qu = static_cast<std::uint32_t>(q);
            std::memcpy(H(qp), &qu, 4);
            ret64(r);
        } else {
            if (!CheckRange(qp, 4, true)) {
                goto bad_ptr;
            }
            int q = 0;
            float r = remquof(x, y, &q);
            std::uint32_t qu = static_cast<std::uint32_t>(q);
            std::memcpy(H(qp), &qu, 4);
            Ret(regs, b32(r));
        }
        return;
    }
}
