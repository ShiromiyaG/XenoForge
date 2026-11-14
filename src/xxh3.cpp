// XXH3 64-bit with seed 0 and the default secret (xxh3.h), from the algorithm's
// description in xxHash (https://github.com/Cyan4973/xxHash, BSD-2-Clause:
// Copyright (C) 2012-2023 Yann Collet). The 192-byte default secret below is
// the algorithm's published constant; the rest is written here.

#include "xenoforge/xxh3.h"

namespace xf {
namespace {

constexpr uint8_t SECRET[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4, 0x6d, 0xe9,
    0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78,
    0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21, 0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6,
    0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8, 0xa8, 0xfa, 0x76, 0x3f,
    0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31,
    0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff,
    0xfa, 0x13, 0x63, 0xeb, 0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f,
    0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

constexpr uint64_t P32_1 = 0x9E3779B1u, P32_2 = 0x85EBCA77u, P32_3 = 0xC2B2AE3Du;
constexpr uint64_t P64_1 = 0x9E3779B185EBCA87ull, P64_2 = 0xC2B2AE3D27D4EB4Full, P64_3 = 0x165667B19E3779F9ull,
                   P64_4 = 0x85EBCA77C2B2AE63ull, P64_5 = 0x27D4EB2F165667C5ull;
constexpr uint64_t MX1 = 0x165667919E3779F9ull, MX2 = 0x9FB21C651E98DF25ull;

// the hash reads little-endian words, whatever the host
uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
uint64_t swap64(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r = (r << 8) | ((v >> (8 * i)) & 0xFF);
    return r;
}

// the 128-bit product of two 64-bit numbers, its halves xored
uint64_t mul_fold(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xFFFFFFFF, a1 = a >> 32, b0 = b & 0xFFFFFFFF, b1 = b >> 32;
    const uint64_t ll = a0 * b0, lh = a0 * b1, hl = a1 * b0, hh = a1 * b1;
    const uint64_t cross = (ll >> 32) + (hl & 0xFFFFFFFF) + lh;
    const uint64_t high = (hl >> 32) + (cross >> 32) + hh;
    const uint64_t low = (cross << 32) | (ll & 0xFFFFFFFF);
    return low ^ high;
}

uint64_t avalanche64(uint64_t h) {
    h ^= h >> 33, h *= P64_2, h ^= h >> 29, h *= P64_3;
    return h ^ (h >> 32);
}
uint64_t avalanche(uint64_t h) {
    h ^= h >> 37, h *= MX1;
    return h ^ (h >> 32);
}

uint64_t mix16(const uint8_t *in, const uint8_t *secret) {
    return mul_fold(le64(in) ^ le64(secret), le64(in + 8) ^ le64(secret + 8));
}

uint64_t short_hash(const uint8_t *in, size_t len) {
    if (len == 0) return avalanche64(le64(SECRET + 56) ^ le64(SECRET + 64));
    if (len <= 3) {
        const uint32_t combined =
            uint32_t(in[0]) << 16 | uint32_t(in[len >> 1]) << 24 | uint32_t(in[len - 1]) | uint32_t(len) << 8;
        const uint64_t flip = uint64_t(le32(SECRET) ^ le32(SECRET + 4));
        return avalanche64(uint64_t(combined) ^ flip);
    }
    if (len <= 8) {
        const uint64_t input = uint64_t(le32(in + len - 4)) + (uint64_t(le32(in)) << 32);
        uint64_t h = input ^ (le64(SECRET + 8) ^ le64(SECRET + 16));
        h ^= rotl(h, 49) ^ rotl(h, 24);
        h *= MX2;
        h ^= (h >> 35) + len;
        h *= MX2;
        return h ^ (h >> 28);
    }
    if (len <= 16) {
        const uint64_t lo = le64(in) ^ (le64(SECRET + 24) ^ le64(SECRET + 32));
        const uint64_t hi = le64(in + len - 8) ^ (le64(SECRET + 40) ^ le64(SECRET + 48));
        return avalanche(len + swap64(lo) + hi + mul_fold(lo, hi));
    }
    uint64_t acc = len * P64_1;
    if (len <= 128) {
        if (len > 32) {
            if (len > 64) {
                if (len > 96) acc += mix16(in + 48, SECRET + 96) + mix16(in + len - 64, SECRET + 112);
                acc += mix16(in + 32, SECRET + 64) + mix16(in + len - 48, SECRET + 80);
            }
            acc += mix16(in + 16, SECRET + 32) + mix16(in + len - 32, SECRET + 48);
        }
        acc += mix16(in, SECRET) + mix16(in + len - 16, SECRET + 16);
        return avalanche(acc);
    }
    for (int i = 0; i < 8; i++) acc += mix16(in + 16 * i, SECRET + 16 * i);
    acc = avalanche(acc);
    for (size_t i = 8; i < len / 16; i++) acc += mix16(in + 16 * i, SECRET + 16 * (i - 8) + 3);
    acc += mix16(in + len - 16, SECRET + 136 - 17);
    return avalanche(acc);
}

void accumulate_stripe(uint64_t acc[8], const uint8_t *in, const uint8_t *secret) {
    for (int i = 0; i < 8; i++) {
        const uint64_t data = le64(in + 8 * i), key = data ^ le64(secret + 8 * i);
        acc[i ^ 1] += data;
        acc[i] += (key & 0xFFFFFFFF) * (key >> 32);
    }
}

void scramble(uint64_t acc[8], const uint8_t *secret) {
    for (int i = 0; i < 8; i++) {
        uint64_t a = acc[i];
        a ^= a >> 47;
        a ^= le64(secret + 8 * i);
        acc[i] = a * P32_1;
    }
}

uint64_t long_hash(const uint8_t *in, size_t len) {
    uint64_t acc[8] = {P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1};
    constexpr size_t STRIPE = 64, SECRET_SIZE = sizeof(SECRET), PER_BLOCK = (SECRET_SIZE - STRIPE) / 8,
                     BLOCK = STRIPE * PER_BLOCK;
    const size_t blocks = (len - 1) / BLOCK;
    for (size_t b = 0; b < blocks; b++) {
        for (size_t s = 0; s < PER_BLOCK; s++) accumulate_stripe(acc, in + b * BLOCK + s * STRIPE, SECRET + s * 8);
        scramble(acc, SECRET + SECRET_SIZE - STRIPE);
    }
    const size_t stripes = ((len - 1) - BLOCK * blocks) / STRIPE;
    for (size_t s = 0; s < stripes; s++) accumulate_stripe(acc, in + blocks * BLOCK + s * STRIPE, SECRET + s * 8);
    accumulate_stripe(acc, in + len - STRIPE, SECRET + SECRET_SIZE - STRIPE - 7);
    uint64_t result = len * P64_1;
    for (int i = 0; i < 4; i++)
        result += mul_fold(acc[2 * i] ^ le64(SECRET + 11 + 16 * i), acc[2 * i + 1] ^ le64(SECRET + 11 + 16 * i + 8));
    return avalanche(result);
}

} // namespace

uint64_t xxh3_64(const void *data, size_t size) {
    const uint8_t *in = static_cast<const uint8_t *>(data);
    return size <= 240 ? short_hash(in, size) : long_hash(in, size);
}

} // namespace xf
