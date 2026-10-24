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
