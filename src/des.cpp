// DES as FIPS 46-3 describes it: the initial permutation, sixteen Feistel
// rounds (expansion, the round key, the eight S-boxes, the permutation P)
// and the inverse permutation; the key schedule through PC-1, the left
// rotations and PC-2. The tables are the standard's, positions counted from
// 1 at the most significant bit; the inverse of IP and the expansion E
// (each S-box's six bits: its four, and one more from each neighbour) are
// computed from their definitions.

#include "xenoforge/des.h"

#include <array>

namespace xf::des {
namespace {

// clang-format off
constexpr uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10,  2,
    60, 52, 44, 36, 28, 20, 12,  4,
    62, 54, 46, 38, 30, 22, 14,  6,
    64, 56, 48, 40, 32, 24, 16,  8,
    57, 49, 41, 33, 25, 17,  9,  1,
    59, 51, 43, 35, 27, 19, 11,  3,
    61, 53, 45, 37, 29, 21, 13,  5,
    63, 55, 47, 39, 31, 23, 15,  7,
};

constexpr uint8_t P[32] = {
    16,  7, 20, 21,
    29, 12, 28, 17,
     1, 15, 23, 26,
     5, 18, 31, 10,
     2,  8, 24, 14,
    32, 27,  3,  9,
    19, 13, 30,  6,
    22, 11,  4, 25,
};

constexpr uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17,  9,
     1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27,
    19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
     7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29,
    21, 13,  5, 28, 20, 12,  4,
};

constexpr uint8_t PC2[48] = {
    14, 17, 11, 24,  1,  5,
     3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8,
    16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32,
};

constexpr uint8_t SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

// S1 to S8, each four rows of sixteen
constexpr uint8_t S[8][64] = {
    {
        14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,
         0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,
         4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0,
        15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13,
    },
    {
        15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,
         3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,
         0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15,
        13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9,
    },
    {
        10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,
        13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,
        13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
         1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12,
    },
    {
         7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,
        13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,
        10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
         3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14,
    },
    {
         2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9,
        14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6,
         4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14,
        11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3,
    },
    {
        12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
        10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
         9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
         4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13,
    },
    {
         4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
        13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
         1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
         6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12,
    },
    {
        13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
         1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
         7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
         2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11,
    },
};
// clang-format on

// FP = IP^-1: the bit IP takes from position IP[i] goes back there
constexpr std::array<uint8_t, 64> make_fp() {
    std::array<uint8_t, 64> fp{};
    for (int i = 0; i < 64; i++) fp[IP[i] - 1] = uint8_t(i + 1);
    return fp;
}
constexpr std::array<uint8_t, 64> FP = make_fp();

// E: for S-box i, bits 4i to 4i + 5 of R (1-based, wrapping around)
constexpr std::array<uint8_t, 48> make_e() {
    std::array<uint8_t, 48> e{};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 6; j++) e[6 * i + j] = uint8_t((4 * i + j + 31) % 32 + 1);
    return e;
}
constexpr std::array<uint8_t, 48> E = make_e();

// out's bit i (from the left) is in's bit table[i] (from the left, 1-based)
uint64_t permute(uint64_t in, int in_bits, const uint8_t *table, int out_bits) {
    uint64_t out = 0;
    for (int i = 0; i < out_bits; i++) out = out << 1 | (in >> (in_bits - table[i]) & 1);
    return out;
}

// the cipher function f(R, K)
uint32_t f(uint32_t r, uint64_t k) {
    uint64_t x = permute(r, 32, E.data(), 48) ^ k;
    uint32_t s = 0;
    for (int i = 0; i < 8; i++) {
        // six bits: the outer two pick the row, the inner four the column
        uint32_t b = uint32_t(x >> (42 - 6 * i)) & 63;
        s = s << 4 | S[i][((b >> 4 & 2) | (b & 1)) * 16 + (b >> 1 & 15)];
    }
    return uint32_t(permute(s, 32, P, 32));
}

uint64_t crypt(const Key &key, uint64_t block, bool decrypt) {
    block = permute(block, 64, IP, 64);
    uint32_t l = uint32_t(block >> 32), r = uint32_t(block);
    for (int i = 0; i < 16; i++) {
        uint32_t next = l ^ f(r, key.round[decrypt ? 15 - i : i]);
        l = r;
        r = next;
    }
    // the halves swapped back before the last permutation
    return permute(uint64_t(r) << 32 | l, 64, FP.data(), 64);
}

} // namespace

Key expand(uint64_t key) {
    Key k{};
    uint64_t cd = permute(key, 64, PC1, 56);
    uint32_t c = uint32_t(cd >> 28), d = uint32_t(cd & 0x0FFFFFFF);
    for (int i = 0; i < 16; i++) {
        for (int s = 0; s < SHIFTS[i]; s++) {
            c = (c << 1 | c >> 27) & 0x0FFFFFFF;
            d = (d << 1 | d >> 27) & 0x0FFFFFFF;
        }
        k.round[i] = permute(uint64_t(c) << 28 | d, 56, PC2, 48);
    }
    return k;
}

uint64_t encrypt(const Key &key, uint64_t block) { return crypt(key, block, false); }
uint64_t decrypt(const Key &key, uint64_t block) { return crypt(key, block, true); }

} // namespace xf::des
