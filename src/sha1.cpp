// SHA-1 as FIPS 180-4 describes it: 64-byte blocks, the message padded with
// a 1 bit, zeros and its length in bits (big-endian), 80 rounds a block.

#include "xenoforge/sha1.h"

#include "xenoforge/bytes.h"

#include <cstring>

namespace xf {
namespace {

uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }

} // namespace

void sha1_block(uint32_t h[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int t = 0; t < 16; t++) w[t] = be32(block + 4 * t);
    for (int t = 16; t < 80; t++) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (t < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (t < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t temp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1(std::span<const uint8_t> data, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t full = data.size() & ~size_t(63);
    for (size_t i = 0; i < full; i += 64) sha1_block(h, data.data() + i);

    // the tail, the 1 bit, zeros and the length: one block or two
    uint8_t tail[128] = {};
    size_t rest = data.size() - full;
    if (rest) std::memcpy(tail, data.data() + full, rest);
    tail[rest] = 0x80;
    size_t blocks = rest + 1 + 8 <= 64 ? 1 : 2;
    uint64_t bits = uint64_t(data.size()) * 8;
    put_be32(tail + 64 * blocks - 8, uint32_t(bits >> 32));
    put_be32(tail + 64 * blocks - 4, uint32_t(bits));
    for (size_t i = 0; i < blocks; i++) sha1_block(h, tail + 64 * i);
    for (int i = 0; i < 5; i++) put_be32(digest + 4 * i, h[i]);
}

} // namespace xf
