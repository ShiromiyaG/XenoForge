// MD5 as RFC 1321 describes it (section 3.4): four rounds of sixteen steps
// over the block's little-endian words. T is the RFC's table, the integer
// part of 4294967296 * abs(sin(i)) for i = 1 to 64 (generated from that
// definition with 60-digit decimals).

#include "xenoforge/md5.h"

#include "xenoforge/bytes.h"

namespace xf {
namespace {

constexpr uint32_t T[64] = {
    0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE, 0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
    0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE, 0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
    0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA, 0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
    0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED, 0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
    0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C, 0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
    0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05, 0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
    0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039, 0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
    0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1, 0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391,
};

// each round's four rotations, used in turn
constexpr int S[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};

constexpr uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }

} // namespace

const uint32_t md5_initial[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};

void md5_block(uint32_t h[4], const uint8_t block[64]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = le32(block + 4 * i);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        // the round's function F, G, H or I, and the word it takes
        uint32_t f;
        int k;
        switch (i / 16) {
        case 0: f = (b & c) | (~b & d), k = i; break;
        case 1: f = (b & d) | (c & ~d), k = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d, k = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d), k = (7 * i) % 16; break;
        }
        uint32_t next = b + rotl(a + f + x[k] + T[i], S[i / 16][i % 4]);
        a = d;
        d = c;
        c = b;
        b = next;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

} // namespace xf
