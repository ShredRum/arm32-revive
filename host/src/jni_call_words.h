// JNI native-entry argument marshalling for the guest (32-bit ARM).
//
// A 64-bit argument (jlong, jdouble) takes an EVEN-numbered core register
// pair, or, when no pair is left, an 8-byte aligned stack slot. This is the
// AAPCS32 rule, and it is what the guests' own native entry points expect:
// Utils.handleReferrerValue(Ljava/lang/String;JJ)V reads its two longs at
// entry_sp+0 and entry_sp+8 (measured by disassembling both Star Wars II
// builds). A compiler check with the default armv7a-linux-gnueabi ABI emits
// the same two loads, while -mabi=apcs-gnu would put the first long in r3.
//
// The word stream is laid out as [env, thiz, args...]; words 0..3 go to
// r0..r3 and the rest to the stack at spbase + (i - 4) * 4. spbase is
// 8-byte aligned, so an even index means an 8-byte aligned stack slot as
// well. Keep this helper shared with fmt_test: the padding is easy to
// "clean up" and silently break the one method it applies to.
#pragma once

#include <cstdint>
#include <vector>

namespace abr {

inline void AppendJniWord64(std::vector<std::uint32_t>& words, std::uint64_t v) {
    if (words.size() & 1u) {
        words.push_back(0);
    }
    words.push_back(static_cast<std::uint32_t>(v));
    words.push_back(static_cast<std::uint32_t>(v >> 32));
}

}  // namespace abr

#include <cstdarg>
#include <cstring>

namespace abr {

// How a float argument reaches the va_list a word reader consumes.
enum class JniFloatArgs {
    // A C variadic call (the generated Java_* forwarders pass their named
    // parameters on to AppHost_Call's `...`): a jfloat was promoted to double.
    kPromoted,
    // A stub the JVM called with fixed parameters and that reads them through
    // its own `...` (the RegisterNatives stubs). Exact on AArch64 Linux only
    // (AAPCS64; not Apple's variant, not x86-64 SysV, whose variadic callee
    // needs %al): unnamed arguments travel exactly like named ones, and a
    // float sits unpromoted in the low 4 bytes of a vector register or of an
    // 8-byte stack slot, so it is read as a double's bit pattern and its low
    // 32 bits are the float.
    kRaw,
};

// Appends the guest words of one native call's Java arguments (the part of
// `sig` between '(' and ')', after env and thiz) read from `ap`: I and the
// narrow types extended to a 32-bit word as AAPCS32 wants them (Z, C zero-
// extended; B, S sign-extended), J and D as an aligned register pair or
// stack slot (AppendJniWord64), F as its bits, references through `wrap`
// (void* -> guest handle). Stops at a malformed signature.
template <class Wrap>
void AppendJniArgs(std::vector<std::uint32_t>& words, const char* sig, va_list ap,
                   JniFloatArgs floats, Wrap wrap) {
    const char* p = sig;
    if (*p == '(') {
        p++;
    }
    for (; *p && *p != ')'; p++) {
        switch (*p) {
            case 'J':
                AppendJniWord64(words, static_cast<std::uint64_t>(va_arg(ap, long long)));
                break;
            case 'D': {
                const double d = va_arg(ap, double);
                std::uint64_t u = 0;
                std::memcpy(&u, &d, 8);
                AppendJniWord64(words, u);
                break;
            }
            case 'F': {
                const double d = va_arg(ap, double);
                std::uint32_t w = 0;
                if (floats == JniFloatArgs::kPromoted) {
                    const float f = static_cast<float>(d);
                    std::memcpy(&w, &f, 4);
                } else {
                    std::memcpy(&w, &d, 4);  // little-endian: the low half
                }
                words.push_back(w);
                break;
            }
            case 'Z':
                words.push_back(static_cast<std::uint8_t>(va_arg(ap, int)));
                break;
            case 'C':
                words.push_back(static_cast<std::uint16_t>(va_arg(ap, int)));
                break;
            case 'B':
                words.push_back(static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(static_cast<std::int8_t>(va_arg(ap, int)))));
                break;
            case 'S':
                words.push_back(static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(static_cast<std::int16_t>(va_arg(ap, int)))));
                break;
            case 'I':
                words.push_back(static_cast<std::uint32_t>(va_arg(ap, int)));
                break;
            case 'L':
            case '[': {
                words.push_back(wrap(va_arg(ap, void*)));
                while (*p == '[') {
                    p++;
                }
                if (*p == 'L') {
                    while (*p && *p != ';') {
                        p++;
                    }
                }
                if (!*p) {
                    return;
                }
                break;
            }
            default:
                return;  // not a JNI type: malformed signature
        }
    }
}

}  // namespace abr
