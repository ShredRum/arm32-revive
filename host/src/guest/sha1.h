// SHA-1 (FIPS 180-4) over a guest SHA1_CTX, for bionic's legacy SHA1Init /
// SHA1Update / SHA1Final exports (NetBSD sha1.c, which libunity imports).
//
// The guest owns the context, so the state lives in ITS layout, exactly as
// NetBSD keeps it: uint32_t state[5] @0, uint32_t count[2] @20 (the message
// length in bits, count[0] the low word), uint8_t buffer[64] @28; 92 bytes.
// Keeping the same fields means a guest that copies or inspects a context
// sees what bionic would have left there.
#pragma once
#include <cstdint>
#include <cstring>

namespace abr {

struct GuestSha1Ctx {
    std::uint32_t state[5];
    std::uint32_t count[2];
    std::uint8_t buffer[64];
};
static_assert(sizeof(GuestSha1Ctx) == 92, "NetBSD SHA1_CTX on arm32");

inline void Sha1Transform(std::uint32_t st[5], const std::uint8_t blk[64]) {
    auto rol = [](std::uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };
    std::uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (std::uint32_t(blk[4 * i]) << 24) | (std::uint32_t(blk[4 * i + 1]) << 16) |
               (std::uint32_t(blk[4 * i + 2]) << 8) | std::uint32_t(blk[4 * i + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        std::uint32_t f = 0, k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}

inline void Sha1Init(GuestSha1Ctx* c) {
    c->state[0] = 0x67452301u;
    c->state[1] = 0xEFCDAB89u;
    c->state[2] = 0x98BADCFEu;
    c->state[3] = 0x10325476u;
    c->state[4] = 0xC3D2E1F0u;
    c->count[0] = c->count[1] = 0;
}

inline void Sha1Update(GuestSha1Ctx* c, const std::uint8_t* data, std::uint32_t len) {
    std::uint32_t j = c->count[0];
    if ((c->count[0] += len << 3) < j) {
        c->count[1] += (len >> 29) + 1;
    }
    j = (j >> 3) & 63;
    std::uint32_t i = 0;
    if (j + len > 63) {
        i = 64 - j;
        std::memcpy(&c->buffer[j], data, i);
        Sha1Transform(c->state, c->buffer);
        for (; i + 63 < len; i += 64) {
            Sha1Transform(c->state, data + i);
        }
        j = 0;
    }
    std::memcpy(&c->buffer[j], data + i, len - i);
}

inline void Sha1Final(std::uint8_t digest[20], GuestSha1Ctx* c) {
    std::uint8_t finalcount[8];
    for (int i = 0; i < 8; i++) {
        finalcount[i] = static_cast<std::uint8_t>(c->count[i >= 4 ? 0 : 1] >> ((3 - (i & 3)) * 8));
    }
    const std::uint8_t pad80 = 0x80, pad0 = 0;
    Sha1Update(c, &pad80, 1);
    while ((c->count[0] & 504) != 448) {
        Sha1Update(c, &pad0, 1);
    }
    Sha1Update(c, finalcount, 8);
    for (int i = 0; i < 20; i++) {
        digest[i] = static_cast<std::uint8_t>(c->state[i >> 2] >> ((3 - (i & 3)) * 8));
    }
    std::memset(c, 0, sizeof(*c));  // NetBSD wipes the context
}

}  // namespace abr
