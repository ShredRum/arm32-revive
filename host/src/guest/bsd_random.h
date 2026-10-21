// The BSD random() generator, as bionic implements it.
//
// bionic's rand() IS random() and srand() IS srandom() (libc/bionic/rand.cpp
// in AOSP), and bionic's random() is the FreeBSD/BSD TYPE_3 generator
// (degree 31, trinomial x**31 + x**3 + 1) -- the same core glibc uses, so the
// sequences agree on every platform that has this libc.
//
// The harness used to answer rand()/random() from the drand48 LCG (the
// drand48 constants only describe srand48/lrand48/drand48, NOT rand()).
// The game only needs "some" randomness -- without it every bird jumped in
// lockstep and blinked together -- but the sequence was not a device's.
//
// The core below is a direct port of the Berkeley source
// (@(#)random.c 5.5 (Berkeley) 7/6/88), as reworked by glibc.
#pragma once

#include <cstdint>

namespace abr {

class BsdRandom {
public:
    // The state of a freshly initialised generator (as initstate(1, randtbl,
    // 128) leaves it): the reference table, with the front and rear pointers
    // SEP places apart. This is the state the first random() call uses, before
    // any srandom().
    BsdRandom() { Reset(); }

    void Reset() {
        static const std::int32_t kInitial[kDeg] = {
            -1726662223, 379960547,   1735697613, 1040273694, 1313901226,
            1627687941, -179304937,  -2073333483, 1780058412, -1989503057,
            -615974602, 344556628,    939512070, -1249116260, 1507946756,
            -812545463, 154635395,   1388815473, -1926676823, 525320961,
            -1009028674, 968117788, -123449607,  1284210865, 435012392,
            -2017506339, -911064859, -370259173,  1132637927, 1398500161,
            -205601318,
        };
        for (int i = 0; i < kDeg; i++) {
            state_[i] = kInitial[i];
        }
        fptr_ = kSep;
        rptr_ = 0;
    }

    // srandom(seed): seed the state with a Park-Miller LCG, then cycle the
    // generator 10*deg times to wash out the LCG's initial dependencies.
    void Seed(std::uint32_t seed) {
        if (seed == 0) {
            seed = 1;
        }
        state_[0] = static_cast<std::int32_t>(seed);
        std::int32_t word = static_cast<std::int32_t>(seed);
        for (int i = 1; i < kDeg; i++) {
            // state[i] = (16807 * state[i-1]) % 2147483647, without
            // overflowing 31 bits.
            const long hi = word / 127773;
            const long lo = word % 127773;
            word = static_cast<std::int32_t>(16807 * lo - 2836 * hi);
            if (word < 0) {
                word += 2147483647;
            }
            state_[i] = word;
        }
        fptr_ = kSep;
        rptr_ = 0;
        for (int i = 0; i < kDeg * 10; i++) {
            Next();
        }
    }

    // One 31-bit random number.
    std::int32_t Next() {
        std::uint32_t val = static_cast<std::uint32_t>(state_[fptr_]) +
                            static_cast<std::uint32_t>(state_[rptr_]);
        state_[fptr_] = static_cast<std::int32_t>(val);
        const std::int32_t result = static_cast<std::int32_t>(val >> 1);  // drop the low bit
        if (++fptr_ >= kDeg) {
            fptr_ = 0;
            ++rptr_;
        } else if (++rptr_ >= kDeg) {
            rptr_ = 0;
        }
        return result;
    }

private:
    static constexpr int kDeg = 31;  // DEG_3
    static constexpr int kSep = 3;   // SEP_3
    std::int32_t state_[kDeg] = {0};
    int fptr_ = kSep;
    int rptr_ = 0;
};

}  // namespace abr
