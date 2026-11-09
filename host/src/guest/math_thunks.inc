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
    // The float, double, (float, float) and (double, double) functions of
    // fast_math.h never reach this chain: CallImport serves them by their
    // load-time id (CallFastMath). Only the three-argument fma ones are here.
    if (id == I::fmaf_) {
        Ret(regs, b32(fmaf(x, y, f32(regs[2]))));
        return;
    }
    if (id == I::fma_) {
        // AAPCS32: only r0-r3 carry arguments. x and y fill r0:r1 and
        // r2:r3, so the third double goes on the stack at [sp], 8-byte
        // aligned (sp is 8-aligned at a public call).
        if (!CheckRange(regs[13], 8, false)) {
            goto bad_ptr;
        }
        std::uint32_t z[2] = {0, 0};
        std::memcpy(z, H(regs[13]), 8);
        ret64(fma(xd, yd, f64(z[0], z[1])));
        return;
    }
    // int-returning classifiers
    if (id == I::__fpclassifyd_ || id == I::__fpclassifyf_) {
        int c = (id == I::__fpclassifyd_) ? std::fpclassify(xd) : std::fpclassify(x);
        Ret(regs, static_cast<std::uint32_t>(c));
        return;
    }
    if (id == I::__isfinite_ || id == I::__isfinitef_ || id == I::__isnan_ || id == I::isnan_ || id == I::__isnanf_ ||
        id == I::__isnormal_ || id == I::__isnormalf_) {
        int c = 0;
        if (id == I::__isfinite_) {
            c = std::isfinite(xd);
        } else if (id == I::__isfinitef_) {
            c = std::isfinite(x);
        } else if (id == I::__isnan_ || id == I::isnan_) {  // bionic exports both (double)
            c = std::isnan(xd);
        } else if (id == I::__isnanf_) {
            c = std::isnan(x);
        } else if (id == I::__isnormal_) {
            c = std::isnormal(xd);
        } else {
            c = std::isnormal(x);
        }
        Ret(regs, static_cast<std::uint32_t>(c));
        return;
    }
    if (id == I::__signbit_ || id == I::__signbitf_ || id == I::__signbitl_) {
        int c = (id == I::__signbit_) ? std::signbit(xd) : std::signbit(x);
        Ret(regs, static_cast<std::uint32_t>(c));
        return;
    }
    if (id == I::lrint_ || id == I::llrint_ || id == I::lrintf_ || id == I::llrintf_ || id == I::lround_ ||
        id == I::llround_ || id == I::lroundf_ || id == I::llroundf_) {
        bool use_x = (id == I::lrintf_ || id == I::llrintf_ || id == I::lroundf_ || id == I::llroundf_);
        bool is_ll = (id == I::llrint_ || id == I::llrintf_ || id == I::llround_ || id == I::llroundf_);
        bool is_round = (id == I::lround_ || id == I::llround_ || id == I::lroundf_ || id == I::llroundf_);
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
    if (id == I::ilogb_ || id == I::ilogbf_ || id == I::logb_ || id == I::logbf_) {
        if (id == I::ilogb_ || id == I::ilogbf_) {
            int v = (id == I::ilogb_) ? ilogb(xd) : ilogbf(x);
            Ret(regs, static_cast<std::uint32_t>(v));
        } else if (id == I::logb_) {
            ret64(logb(xd));
        } else {
            Ret(regs, b32(logbf(x)));
        }
        return;
    }
    // double,int -> double
    if (id == I::ldexp_ || id == I::scalbn_ || id == I::scalbln_ || id == I::scalbnl_) {
        // scalbnl (long double variant) is the same on ARM32: long double ==
        // double. It was missing and silently returned 0.0 (Rio imports it).
        ret64(ldexp(xd, static_cast<int>(a2)));
        return;
    }
    if (id == I::ldexpf_ || id == I::scalbnf_ || id == I::scalblnf_) {
        Ret(regs, b32(ldexpf(x, static_cast<int>(a1))));
        return;
    }
    // modf/frexp with out-pointers
    if (id == I::modf_ || id == I::modff_) {
        if (id == I::modf_) {
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
    if (id == I::frexp_ || id == I::frexpf_) {
        if (id == I::frexp_) {
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
    if (id == I::nan_ || id == I::nanf_ || id == I::nanl_) {
        if (id == I::nanf_ || id == I::nanl_) {
            Ret(regs, b32(nanf(GuestStr(a0))));
        } else {
            ret64(nan(GuestStr(a0)));
        }
        return;
    }
    if (id == I::nextafter_ || id == I::nextafterf_ || id == I::nexttoward_ || id == I::nexttowardf_) {
        if (id == I::nextafter_) {
            ret64(nextafter(xd, yd));
        } else if (id == I::nextafterf_) {
            Ret(regs, b32(nextafterf(x, y)));
        } else if (id == I::nexttoward_) {
            ret64(nexttoward(xd, yd));
        } else {
            Ret(regs, b32(nexttowardf(x, yd)));
        }
        return;
    }
    if (id == I::remquo_ || id == I::remquof_) {
        // AAPCS32: remquo(double, double, int*) fills r0-r3 with the doubles,
        // so quo is on the stack at [sp]; remquof(float, float, int*) passes
        // quo in r2.
        std::uint32_t qp = a2;
        if (id == I::remquo_) {
            if (!CheckRange(regs[13], 4, false)) {
                goto bad_ptr;
            }
            std::memcpy(&qp, H(regs[13]), 4);
        }
        if (id == I::remquo_) {
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
