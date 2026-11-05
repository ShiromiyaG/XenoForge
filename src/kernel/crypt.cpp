/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The crypto family (plan step A4; docs/runtime.md): the XeCrypt* hashes,
// HMACs, ciphers and big-number functions, and the XeKeys* ones over the
// console's key vault. Translated from Xenia's
// src/xenia/kernel/xboxkrnl/xboxkrnl_crypt.cc and xecrypt_rsa.h (BSD; see
// LICENSE-xenia); checked against them. The algorithms are the project's
// own, written from their standards (sha1.h, sha2.h, md5.h, aes.h, des.h,
// bignum.h), where Xenia takes TinySHA1, Stephan Brumme's SHA-256, FFmpeg's
// MD5 and SHA-512, a DES library and aes_128.
//
// The games keep the contexts in their own memory, so they have Xenia's
// layouts, big-endian:
//   XECRYPT_SHA_STATE (0x58)      count (bytes, 32 bits), 5 words, buffer[64]
//   XECRYPT_HMACSHA_STATE (0xB0)  the inner SHA state, then the outer one
//   XECRYPT_SHA256_STATE (0x64)   count (32 bits), 8 words, buffer[64]
//   XECRYPT_SHA512_STATE (0xC8)   count (64 bits), 8 words, buffer[128]; SHA-384's too
//   XECRYPT_MD5_STATE (0x54)      count (32 bits), A to D, buffer[64]
//   XECRYPT_RC4_STATE (0x102)     S[256], i, j
//   XECRYPT_AES_STATE (0x160)     11 encryption round keys, then the 11 of the
//                                 equivalent inverse cipher
//   XECRYPT_DES_STATE (0x80)      16 round keys as Xenia's DES library keeps
//                                 them: 48 bits in little-endian 64-bit words
//   XECRYPT_DES3_STATE (0x180)    three of them
// The buffer keeps the partial block at its start; what the rest holds, and
// what each Final leaves, is what Xenia leaves (each export says).
//
// Leaner than, or beyond, Xenia: SHA-384 (Init, Update, Final) and the
// one-shot SHA-384 and SHA-512, which Xenia lacks, follow its SHA-512;
// XeKeysConsoleSignatureVerification, which Xenia lacks too, accepts every
// signature (see there). SHA-1's length field takes the whole 64-bit bit
// count where TinySHA1 keeps 32 bits (messages from 512 MB up).
// XeCryptBnQwNeRsaPubCrypt fails for an even modulus or one past 4096 bits,
// which Xenia's generic arithmetic would take.

#include "xenoforge/aes.h"
#include "xenoforge/bignum.h"
#include "xenoforge/bytes.h"
#include "xenoforge/des.h"
#include "xenoforge/kernel.h"
#include "xenoforge/md5.h"
#include "xenoforge/sha1.h"
#include "xenoforge/sha2.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;

void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, uint32_t(v >> 32));
    put_be32(p + 4, uint32_t(v));
}

uint64_t le64(const uint8_t *p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }

void put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i));
}

// Copies as much of a digest as the output takes.
void output(Kernel &k, uint32_t out, uint32_t out_size, const uint8_t *digest, size_t size) {
    if (out && out_size) std::memcpy(k.host(out), digest, std::min<size_t>(size, out_size));
}

// ---- the hashes' running state ----

// A hash's state as the contexts hold it: the words, the byte count, and
// the block buffer with the partial block at its start.
template <class W, size_t N, size_t B, void (*Compress)(W *, const uint8_t *)> struct Hash {
    using Word = W;
    static constexpr size_t words = N, block = B;
    W h[N] = {};
    uint64_t count = 0;
    uint8_t buffer[B] = {};

    // A pending partial block is completed and compressed, whole blocks are
    // compressed straight from the input, the rest goes to the buffer's
    // start. stage_first copies a first whole block through the buffer even
    // when none is pending, as FFmpeg's SHA-512 does (Xenia's SHA-512
    // contexts keep its buffer).
    void update(const uint8_t *data, size_t size, bool stage_first = false) {
        if (!size) return;
        size_t used = size_t(count % B);
        count += size;
        if (used || (stage_first && size >= B)) {
            size_t take = std::min(size, B - used);
            std::memcpy(buffer + used, data, take);
            data += take, size -= take;
            if (used + take < B) return;
            Compress(h, buffer);
        }
        for (; size >= B; data += B, size -= B) Compress(h, data);
        if (size) std::memcpy(buffer, data, size);
    }

    // The padding, in the buffer: a 1 bit, zeros, and the length in bits in
    // the last length_size bytes (big-endian, MD5's little-endian); the
    // buffer keeps the last block compressed. The count stays.
    void pad(size_t length_size, bool little_endian = false) {
        size_t used = size_t(count % B);
        buffer[used++] = 0x80;
        if (used > B - length_size) {
            std::memset(buffer + used, 0, B - used);
            Compress(h, buffer);
            used = 0;
        }
        std::memset(buffer + used, 0, B - used);
        uint64_t bits = count * 8;
        if (little_endian)
            put_le64(buffer + B - length_size, bits);
        else
            put_be64(buffer + B - 8, bits);
        Compress(h, buffer);
    }

    // the words as bytes, as many as the digest has
    void digest(uint8_t *out, size_t size, bool little_endian = false) const {
        uint8_t all[N * sizeof(W)];
        for (size_t i = 0; i < N; i++)
            for (size_t j = 0; j < sizeof(W); j++) {
                size_t shift = little_endian ? 8 * j : 8 * (sizeof(W) - 1 - j);
                all[i * sizeof(W) + j] = uint8_t(h[i] >> shift);
            }
        std::memcpy(out, all, std::min(size, sizeof(all)));
    }
};

using Sha1 = Hash<uint32_t, 5, 64, sha1_block>;
using Sha256 = Hash<uint32_t, 8, 64, sha256_block>;
using Sha512 = Hash<uint64_t, 8, 128, sha512_block>; // and SHA-384
using Md5 = Hash<uint32_t, 4, 64, md5_block>;

template <class H> H start(const typename H::Word (&initial)[H::words]) {
    H s;
    std::copy_n(initial, H::words, s.h);
    return s;
}

constexpr uint32_t sha1_initial[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

// A context in guest memory: the count (count_size bytes), the words, the
// buffer, one after the other, big-endian.
template <class H> struct Context {
    Kernel &k;
    uint32_t p, count_size;

    uint32_t words_at() const { return p + count_size; }
    uint32_t buffer_at() const { return p + count_size + uint32_t(H::words * sizeof(typename H::Word)); }

    H load() const {
        H s;
        s.count = count_size == 8 ? k.load64(p) : k.load32(p);
        for (size_t i = 0; i < H::words; i++) {
            uint32_t at = words_at() + uint32_t(i * sizeof(typename H::Word));
            if constexpr (sizeof(typename H::Word) == 8)
                s.h[i] = k.load64(at);
            else
                s.h[i] = k.load32(at);
        }
        std::memcpy(s.buffer, k.host(buffer_at()), H::block);
        return s;
    }
    void store_count(const H &s) const {
        if (count_size == 8)
            k.store64(p, s.count);
        else
            k.store32(p, uint32_t(s.count));
    }
    void store_words(const H &s) const {
        for (size_t i = 0; i < H::words; i++) {
            uint32_t at = words_at() + uint32_t(i * sizeof(typename H::Word));
            if constexpr (sizeof(typename H::Word) == 8)
                k.store64(at, s.h[i]);
            else
                k.store32(at, s.h[i]);
        }
    }
    void store_buffer(const H &s, size_t bytes) const { std::memcpy(k.host(buffer_at()), s.buffer, bytes); }
    // Init: the context zeroed, then the initial words
    void init(const typename H::Word (&initial)[H::words]) const {
        std::memset(k.host(p), 0, buffer_at() + H::block - p);
        store_words(start<H>(initial));
    }
};

Context<Sha1> sha1_context(Kernel &k, uint32_t p) { return {k, p, 4}; }
Context<Sha256> sha256_context(Kernel &k, uint32_t p) { return {k, p, 4}; }
Context<Sha512> sha512_context(Kernel &k, uint32_t p) { return {k, p, 8}; }
Context<Md5> md5_context(Kernel &k, uint32_t p) { return {k, p, 4}; }

// The three (input, size) pairs of the one-shot hashes, from argument first.
template <class H> void update3(Kernel &k, ppc::Context &c, int first, H &s, bool stage_first = false) {
    for (int i = 0; i < 3; i++) {
        uint32_t input = arg(k, c, first + 2 * i), size = arg(k, c, first + 2 * i + 1);
        if (input && size) s.update(k.host(input), size, stage_first);
    }
}

// ---- SHA-1 ----

XF_EXPORT(xboxkrnl, XeCryptShaInit) { sha1_context(k, arg(k, c, 0)).init(sha1_initial); }

// SHA-1 contexts as TinySHA1 leaves them in Xenia: an update writes back
// only the partial block; a final leaves the digest in the words and the
// last block it compressed in the buffer (the console pads in place, so a
// title finalizing the same context twice sees that block, not the message
// tail, when the padding took a second block), and the count as it was.
void sha_update(Kernel &k, uint32_t p, const uint8_t *data, size_t size) {
    auto ctx = sha1_context(k, p);
    Sha1 s = ctx.load();
    s.update(data, size);
    ctx.store_count(s);
    ctx.store_words(s);
    ctx.store_buffer(s, size_t(s.count % 64));
}

void sha_final(Kernel &k, uint32_t p, uint32_t out, uint32_t out_size) {
    auto ctx = sha1_context(k, p);
    Sha1 s = ctx.load();
    s.pad(8);
    uint8_t digest[20];
    s.digest(digest, sizeof(digest));
    output(k, out, out_size, digest, sizeof(digest));
    ctx.store_words(s);
    ctx.store_buffer(s, 64);
}

XF_EXPORT(xboxkrnl, XeCryptShaUpdate) {
    uint32_t input = arg(k, c, 1), size = arg(k, c, 2);
    sha_update(k, arg(k, c, 0), size ? k.host(input) : nullptr, size);
}

XF_EXPORT(xboxkrnl, XeCryptShaFinal) { sha_final(k, arg(k, c, 0), arg(k, c, 1), arg(k, c, 2)); }

// one-shot SHA-1 on the host
void sha1_digest(Sha1 &s, uint8_t digest[20]) {
    s.pad(8);
    s.digest(digest, 20);
}

XF_EXPORT(xboxkrnl, XeCryptSha) {
    // (input1, size1, input2, size2, input3, size3, output, output_size)
    Sha1 s = start<Sha1>(sha1_initial);
    update3(k, c, 0, s);
    uint8_t digest[20];
    sha1_digest(s, digest);
    output(k, arg(k, c, 6), arg(k, c, 7), digest, sizeof(digest));
}

// ---- HMAC-SHA (RFC 2104) ----

// Two SHA-1 states, inner and outer, after the key's pads. A key past the
// block is cut to its first 64 bytes, as the console does, not hashed.
struct HmacSha {
    Sha1 inner = start<Sha1>(sha1_initial), outer = start<Sha1>(sha1_initial);

    HmacSha(const uint8_t *key, size_t key_size) {
        uint8_t ipad[64] = {}, opad[64] = {};
        for (size_t i = 0; i < std::min<size_t>(key_size, 64); i++) ipad[i] = opad[i] = key[i];
        // the pads cover the whole block, not just the key
        for (int i = 0; i < 64; i++) {
            ipad[i] ^= 0x36;
            opad[i] ^= 0x5C;
        }
        inner.update(ipad, 64);
        outer.update(opad, 64);
    }
    HmacSha(const Sha1 &i, const Sha1 &o) : inner(i), outer(o) {}

    // the inner hash finished, its digest through the outer one
    void finish(uint8_t digest[20]) {
        uint8_t inner_digest[20];
        sha1_digest(inner, inner_digest);
        outer.update(inner_digest, 20);
        sha1_digest(outer, digest);
    }
};

XF_EXPORT(xboxkrnl, XeCryptHmacShaInit) {
    // (XECRYPT_HMACSHA_STATE *, key, key_size): zeroed, both halves started
    // with their pads (whole blocks: the buffers stay zero)
    uint32_t p = arg(k, c, 0), key = arg(k, c, 1), key_size = arg(k, c, 2);
    HmacSha s(key_size ? k.host(key) : nullptr, key_size);
    std::memset(k.host(p), 0, 0xB0);
    auto store = [&](uint32_t at, const Sha1 &half) {
        auto ctx = sha1_context(k, at);
        ctx.store_count(half);
        ctx.store_words(half);
    };
    store(p, s.inner);
    store(p + 0x58, s.outer);
}

XF_EXPORT(xboxkrnl, XeCryptHmacShaUpdate) {
    uint32_t input = arg(k, c, 1), size = arg(k, c, 2);
    sha_update(k, arg(k, c, 0), size ? k.host(input) : nullptr, size);
}

XF_EXPORT(xboxkrnl, XeCryptHmacShaFinal) {
    // the inner state finished in place (its digest in its words), its
    // digest through the outer one, which is finished into the output
    uint32_t p = arg(k, c, 0);
    sha_final(k, p, 0, 0);
    uint8_t inner_digest[20];
    std::memcpy(inner_digest, k.host(p + 4), sizeof(inner_digest));
    sha_update(k, p + 0x58, inner_digest, sizeof(inner_digest));
    sha_final(k, p + 0x58, arg(k, c, 1), arg(k, c, 2));
}

// (key, key_size, input1, size1, input2, size2, input3, size3): the HMAC
void hmac_sha3(Kernel &k, ppc::Context &c, uint8_t digest[20]) {
    uint32_t key = arg(k, c, 0), key_size = arg(k, c, 1);
    HmacSha s(key_size ? k.host(key) : nullptr, key_size);
    update3(k, c, 2, s.inner);
    s.finish(digest);
}

XF_EXPORT(xboxkrnl, XeCryptHmacSha) {
    // (..., output, output_size)
    uint8_t digest[20];
    hmac_sha3(k, c, digest);
    output(k, arg(k, c, 8), arg(k, c, 9), digest, sizeof(digest));
}

XF_EXPORT(xboxkrnl, XeCryptHmacShaVerify) {
    // (..., expected, expected_size): as much of the HMAC as is given
    uint8_t digest[20];
    hmac_sha3(k, c, digest);
    uint32_t expected = arg(k, c, 8), expected_size = arg(k, c, 9);
    size_t size = std::min<size_t>(sizeof(digest), expected_size);
    result(c, !size || std::memcmp(digest, k.host(expected), size) == 0);
}

// ---- SHA-256, SHA-384, SHA-512 ----

// The contexts as Xenia leaves them: an update writes back the whole buffer
// (the rest of it as Brumme's SHA-256 and FFmpeg's SHA-512 leave it); a
// final writes only the digest, to the buffer's start, leaving the words
// and the count as they were.

XF_EXPORT(xboxkrnl, XeCryptSha256Init) { sha256_context(k, arg(k, c, 0)).init(sha256_initial); }

XF_EXPORT(xboxkrnl, XeCryptSha256Update) {
    auto ctx = sha256_context(k, arg(k, c, 0));
    uint32_t input = arg(k, c, 1), size = arg(k, c, 2);
    Sha256 s = ctx.load();
    if (size) s.update(k.host(input), size);
    ctx.store_count(s);
    ctx.store_words(s);
    ctx.store_buffer(s, 64);
}

XF_EXPORT(xboxkrnl, XeCryptSha256Final) {
    auto ctx = sha256_context(k, arg(k, c, 0));
    Sha256 s = ctx.load();
    s.pad(8);
    uint8_t digest[32];
    s.digest(digest, sizeof(digest));
    output(k, arg(k, c, 1), arg(k, c, 2), digest, sizeof(digest));
    std::memcpy(k.host(ctx.buffer_at()), digest, sizeof(digest));
}

XF_EXPORT(xboxkrnl, XeCryptSha256) {
    Sha256 s = start<Sha256>(sha256_initial);
    update3(k, c, 0, s);
    s.pad(8);
    uint8_t digest[32];
    s.digest(digest, sizeof(digest));
    output(k, arg(k, c, 6), arg(k, c, 7), digest, sizeof(digest));
}

void sha512_update(Kernel &k, ppc::Context &c) {
    auto ctx = sha512_context(k, arg(k, c, 0));
    uint32_t input = arg(k, c, 1), size = arg(k, c, 2);
    Sha512 s = ctx.load();
    if (size) s.update(k.host(input), size, true);
    ctx.store_count(s);
    ctx.store_words(s);
    ctx.store_buffer(s, 128);
}

// (state, output, output_size); SHA-384 is SHA-512's first 48 bytes
void sha512_final(Kernel &k, ppc::Context &c, size_t digest_size) {
    auto ctx = sha512_context(k, arg(k, c, 0));
    Sha512 s = ctx.load();
    s.pad(16);
    uint8_t digest[64];
    s.digest(digest, digest_size);
    output(k, arg(k, c, 1), arg(k, c, 2), digest, digest_size);
    std::memcpy(k.host(ctx.buffer_at()), digest, digest_size);
}

void sha512_oneshot(Kernel &k, ppc::Context &c, const uint64_t (&initial)[8], size_t digest_size) {
    Sha512 s = start<Sha512>(initial);
    update3(k, c, 0, s, true);
    s.pad(16);
    uint8_t digest[64];
    s.digest(digest, digest_size);
    output(k, arg(k, c, 6), arg(k, c, 7), digest, digest_size);
}

XF_EXPORT(xboxkrnl, XeCryptSha384Init) { sha512_context(k, arg(k, c, 0)).init(sha384_initial); }
XF_EXPORT(xboxkrnl, XeCryptSha384Update) { sha512_update(k, c); }
XF_EXPORT(xboxkrnl, XeCryptSha384Final) { sha512_final(k, c, 48); }
XF_EXPORT(xboxkrnl, XeCryptSha384) { sha512_oneshot(k, c, sha384_initial, 48); }

XF_EXPORT(xboxkrnl, XeCryptSha512Init) { sha512_context(k, arg(k, c, 0)).init(sha512_initial); }
XF_EXPORT(xboxkrnl, XeCryptSha512Update) { sha512_update(k, c); }
XF_EXPORT(xboxkrnl, XeCryptSha512Final) { sha512_final(k, c, 64); }
XF_EXPORT(xboxkrnl, XeCryptSha512) { sha512_oneshot(k, c, sha512_initial, 64); }

// ---- MD5 (RFC 1321) and HMAC-MD5 ----

XF_EXPORT(xboxkrnl, XeCryptMd5Init) { md5_context(k, arg(k, c, 0)).init(md5_initial); }

XF_EXPORT(xboxkrnl, XeCryptMd5Update) {
    // as FFmpeg's MD5 leaves the context in Xenia: the whole buffer back
    auto ctx = md5_context(k, arg(k, c, 0));
    uint32_t input = arg(k, c, 1), size = arg(k, c, 2);
    Md5 s = ctx.load();
    if (size) s.update(k.host(input), size);
    ctx.store_count(s);
    ctx.store_words(s);
    ctx.store_buffer(s, 64);
}

XF_EXPORT(xboxkrnl, XeCryptMd5Final) {
    // the digest is left in the state words, as bytes; the count and the
    // buffer stay
    auto ctx = md5_context(k, arg(k, c, 0));
    Md5 s = ctx.load();
    s.pad(8, true);
    uint8_t digest[16];
    s.digest(digest, sizeof(digest), true);
    output(k, arg(k, c, 1), arg(k, c, 2), digest, sizeof(digest));
    std::memcpy(k.host(ctx.words_at()), digest, sizeof(digest));
}

XF_EXPORT(xboxkrnl, XeCryptMd5) {
    Md5 s = start<Md5>(md5_initial);
    update3(k, c, 0, s);
    s.pad(8, true);
    uint8_t digest[16];
    s.digest(digest, sizeof(digest), true);
    output(k, arg(k, c, 6), arg(k, c, 7), digest, sizeof(digest));
}

XF_EXPORT(xboxkrnl, XeCryptHmacMd5) {
    // (key, key_size, input1, size1, input2, size2, input3, size3, output,
    // output_size): a long key cut to the block, as XeCryptHmacSha does
    uint32_t key = arg(k, c, 0), key_size = std::min<uint32_t>(arg(k, c, 1), 64);
    uint8_t ipad[64], opad[64];
    std::memset(ipad, 0x36, 64);
    std::memset(opad, 0x5C, 64);
    for (uint32_t i = 0; i < key_size; i++) ipad[i] ^= k.host(key)[i], opad[i] ^= k.host(key)[i];
    Md5 inner = start<Md5>(md5_initial), outer = start<Md5>(md5_initial);
    inner.update(ipad, 64);
    update3(k, c, 2, inner);
    inner.pad(8, true);
    uint8_t digest[16];
    inner.digest(digest, sizeof(digest), true);
    outer.update(opad, 64);
    outer.update(digest, sizeof(digest));
    outer.pad(8, true);
    outer.digest(digest, sizeof(digest), true);
    output(k, arg(k, c, 8), arg(k, c, 9), digest, sizeof(digest));
}

// ---- XeCryptRotSumSha ----

// Adds and subtracts each big-endian 64-bit word into a 256-bit rotating
// checksum (Xenia's, after ExCrypt); a tail short of a word is not counted.
void rot_sum(uint64_t sum[4], const uint8_t *data, uint32_t size) {
    for (uint32_t i = 0; i < size / 8; i++) {
        uint64_t value = uint64_t(be32(data + 8 * i)) << 32 | be32(data + 8 * i + 4);
        sum[1] += value;
        sum[3] -= value;
        if (sum[1] < value) sum[0]++;
        if (sum[3] > value) sum[2]--;
        sum[1] = std::rotl(sum[1], 29);
        sum[3] = std::rotl(sum[3], 31);
    }
}

XF_EXPORT(xboxkrnl, XeCryptRotSumSha) {
    // (input1, size1, input2, size2, output, output_size): the SHA-1 of the
    // sum twice, the inputs, the sum's complement twice
    uint32_t input1 = arg(k, c, 0), size1 = arg(k, c, 1), input2 = arg(k, c, 2), size2 = arg(k, c, 3);
    const uint8_t *data1 = size1 ? k.host(input1) : nullptr, *data2 = size2 ? k.host(input2) : nullptr;
    uint64_t sum[4] = {};
    rot_sum(sum, data1, size1);
    rot_sum(sum, data2, size2);
    uint8_t bytes[32];
    for (int i = 0; i < 4; i++) put_be64(bytes + 8 * i, sum[i]);
    Sha1 s = start<Sha1>(sha1_initial);
    s.update(bytes, 32);
    s.update(bytes, 32);
    s.update(data1, size1);
    s.update(data2, size2);
    for (uint8_t &b : bytes) b = uint8_t(~b);
    s.update(bytes, 32);
    s.update(bytes, 32);
    uint8_t digest[20];
    sha1_digest(s, digest);
    output(k, arg(k, c, 4), arg(k, c, 5), digest, sizeof(digest));
}

// ---- RC4 ----

// XECRYPT_RC4_STATE: S at 0, i at 0x100, j at 0x101
void rc4_key(uint8_t *state, const uint8_t *key, uint32_t key_size) {
    state[0x100] = state[0x101] = 0;
    for (int x = 0; x < 256; x++) state[x] = uint8_t(x);
    if (!key_size) return; // (Xenia divides by it)
    uint8_t j = 0;
    for (uint32_t x = 0; x < 256; x++) {
        j = uint8_t(j + state[x] + key[x % key_size]);
        std::swap(state[x], state[j]);
    }
}

void rc4_crypt(uint8_t *state, uint8_t *data, uint32_t size) {
    uint8_t &i = state[0x100], &j = state[0x101];
    for (uint32_t n = 0; n < size; n++) {
        i = uint8_t(i + 1);
        j = uint8_t(j + state[i]);
        std::swap(state[i], state[j]);
        data[n] ^= state[uint8_t(state[i] + state[j])];
    }
}

XF_EXPORT(xboxkrnl, XeCryptRc4Key) {
    // (XECRYPT_RC4_STATE *, key, key_size)
    uint32_t key_size = arg(k, c, 2);
    rc4_key(k.host(arg(k, c, 0)), key_size ? k.host(arg(k, c, 1)) : nullptr, key_size);
}

XF_EXPORT(xboxkrnl, XeCryptRc4Ecb) {
    // (XECRYPT_RC4_STATE *, data, size): in place
    uint32_t size = arg(k, c, 2);
    if (size) rc4_crypt(k.host(arg(k, c, 0)), k.host(arg(k, c, 1)), size);
}

XF_EXPORT(xboxkrnl, XeCryptRc4) {
    // (key, key_size, data, size)
    uint8_t state[0x102];
    uint32_t key_size = arg(k, c, 1), size = arg(k, c, 3);
    rc4_key(state, key_size ? k.host(arg(k, c, 0)) : nullptr, key_size);
    if (size) rc4_crypt(state, k.host(arg(k, c, 2)), size);
}

// ---- AES-128 ----

// the encryption round keys from an XECRYPT_AES_STATE (Xenia ciphers both
// ways with them)
aes::Key aes_key(Kernel &k, uint32_t state) {
    aes::Key key;
    std::memcpy(key.round, k.host(state), sizeof(key.round));
    return key;
}

// CBC over whole blocks (a partial last block is left alone), the feed
// updated to the last ciphertext block; in and out may be the same.
void aes_cbc(const aes::Key &key, const uint8_t *in, uint32_t size, uint8_t *out, uint8_t feed[16], bool encrypt) {
    for (uint32_t n = 0; n < (size & ~15u); n += 16, in += 16, out += 16) {
        if (encrypt) {
            for (int j = 0; j < 16; j++) feed[j] ^= in[j];
            aes::encrypt_block(key, feed, feed);
            std::memcpy(out, feed, 16);
        } else {
            uint8_t block[16];
            std::memcpy(block, in, 16);
            aes::decrypt_block(key, block, out);
            for (int j = 0; j < 16; j++) out[j] ^= feed[j];
            std::memcpy(feed, block, 16);
        }
    }
}

XF_EXPORT(xboxkrnl, XeCryptAesKey) {
    // (XECRYPT_AES_STATE *, key[16])
    uint32_t state = arg(k, c, 0);
    aes::Key key = aes::expand(k.host(arg(k, c, 1)));
    std::memcpy(k.host(state), key.round, sizeof(key.round));
    aes::equivalent_inverse_keys(key, k.host(state + 0xB0));
}

XF_EXPORT(xboxkrnl, XeCryptAesEcb) {
    // (XECRYPT_AES_STATE *, input[16], output[16], encrypt)
    aes::Key key = aes_key(k, arg(k, c, 0));
    const uint8_t *in = k.host(arg(k, c, 1));
    uint8_t *out = k.host(arg(k, c, 2));
    if (arg(k, c, 3))
        aes::encrypt_block(key, in, out);
    else
        aes::decrypt_block(key, in, out);
}

XF_EXPORT(xboxkrnl, XeCryptAesCbc) {
    // (XECRYPT_AES_STATE *, input, size, output, feed[16], encrypt)
    uint32_t size = arg(k, c, 2);
    if (size < 16) return;
    aes_cbc(aes_key(k, arg(k, c, 0)), k.host(arg(k, c, 1)), size, k.host(arg(k, c, 3)), k.host(arg(k, c, 4)),
            arg(k, c, 5) != 0);
}

// ---- DES and triple DES ----

// an XECRYPT_DES_STATE's round keys, Xenia's DES library's 64-bit words
// copied from the host (little-endian)
des::Key des_key(const uint8_t *state) {
    des::Key key;
    for (int i = 0; i < 16; i++) key.round[i] = le64(state + 8 * i);
    return key;
}

void store_des_key(uint8_t *state, const des::Key &key) {
    for (int i = 0; i < 16; i++) put_le64(state + 8 * i, key.round[i]);
}

// one block through DES (keys 1) or triple DES, encrypt-decrypt-encrypt
// (keys 3)
uint64_t des_crypt(const des::Key *keys, int count, uint64_t block, bool encrypt) {
    if (count == 1) return encrypt ? des::encrypt(keys[0], block) : des::decrypt(keys[0], block);
    if (encrypt) return des::encrypt(keys[2], des::decrypt(keys[1], des::encrypt(keys[0], block)));
    return des::decrypt(keys[0], des::encrypt(keys[1], des::decrypt(keys[2], block)));
}

// (state, input, output, encrypt): one block, big-endian
void des_ecb(Kernel &k, ppc::Context &c, int count) {
    des::Key keys[3];
    uint8_t *state = k.host(arg(k, c, 0));
    for (int i = 0; i < count; i++) keys[i] = des_key(state + 0x80 * i);
    k.store64(arg(k, c, 2), des_crypt(keys, count, k.load64(arg(k, c, 1)), arg(k, c, 3) != 0));
}

// (state, input, size, output, feed, encrypt): CBC over whole 8-byte
// blocks, the feed updated to the last ciphertext block
void des_cbc(Kernel &k, ppc::Context &c, int count) {
    des::Key keys[3];
    uint8_t *state = k.host(arg(k, c, 0));
    for (int i = 0; i < count; i++) keys[i] = des_key(state + 0x80 * i);
    uint32_t in = arg(k, c, 1), size = arg(k, c, 2), out = arg(k, c, 3), feed = arg(k, c, 4);
    bool encrypt = arg(k, c, 5) != 0;
    uint64_t last = k.load64(feed);
    for (uint32_t i = 0; i < size / 8; i++) {
        uint64_t block = k.load64(in + 8 * i);
        if (encrypt) {
            last = des_crypt(keys, count, block ^ last, true);
            k.store64(out + 8 * i, last);
        } else {
            k.store64(out + 8 * i, des_crypt(keys, count, block, false) ^ last);
            last = block;
        }
    }
    k.store64(feed, last);
}

XF_EXPORT(xboxkrnl, XeCryptDesKey) {
    // (XECRYPT_DES_STATE *, key)
    store_des_key(k.host(arg(k, c, 0)), des::expand(k.load64(arg(k, c, 1))));
}

XF_EXPORT(xboxkrnl, XeCryptDesEcb) { des_ecb(k, c, 1); }
XF_EXPORT(xboxkrnl, XeCryptDesCbc) { des_cbc(k, c, 1); }

XF_EXPORT(xboxkrnl, XeCryptDes3Key) {
    // (XECRYPT_DES3_STATE *, keys[3])
    uint8_t *state = k.host(arg(k, c, 0));
    uint32_t keys = arg(k, c, 1);
    for (uint32_t i = 0; i < 3; i++) store_des_key(state + 0x80 * i, des::expand(k.load64(keys + 8 * i)));
}

XF_EXPORT(xboxkrnl, XeCryptDes3Ecb) { des_ecb(k, c, 3); }
XF_EXPORT(xboxkrnl, XeCryptDes3Cbc) { des_cbc(k, c, 3); }

XF_EXPORT(xboxkrnl, XeCryptDesParity) {
    // (input, size, output): each byte's low bit set to make its parity odd
    uint32_t in = arg(k, c, 0), size = arg(k, c, 1), out = arg(k, c, 2);
    for (uint32_t i = 0; i < size; i++) {
        uint8_t b = k.host(in + i)[0] & 0xFE;
        k.host(out + i)[0] = uint8_t(b | (~std::popcount(b) & 1));
    }
}

// ---- big numbers ----

XF_EXPORT(xboxkrnl, XeCryptBnQw_SwapDwQwLeBe) {
    // (input, output, count): each 64-bit word byte-swapped
    uint32_t in = arg(k, c, 0), out = arg(k, c, 1), count = arg(k, c, 2);
    for (uint32_t i = 0; i < count; i++) k.store64(out + 8 * i, __builtin_bswap64(k.load64(in + 8 * i)));
}

XF_EXPORT(xboxkrnl, XeCryptBnQw_Copy) {
    // (source, destination, count)
    uint32_t count = arg(k, c, 2);
    if (count) std::memmove(k.host(arg(k, c, 1)), k.host(arg(k, c, 0)), size_t(count) * 8);
}

XF_EXPORT(xboxkrnl, XeCryptBnQwNeRsaPubCrypt) {
    // (a, b, XECRYPT_RSA *): b = a ^ exponent mod modulus. XECRYPT_RSA: the
    // modulus's size in 64-bit words, the public exponent, 8 bytes, then
    // the modulus; the numbers are big-endian words, least significant
    // first. Keys under 512 bits fail, as in Xenia; so do keys over 4096
    // bits and even moduli (Montgomery's method).
    uint32_t a = arg(k, c, 0), b = arg(k, c, 1), rsa = arg(k, c, 2);
    uint32_t words = k.load32(rsa), exponent = k.load32(rsa + 4);
    if (words < 8 || words > 64) return result(c, 0);
    std::vector<uint64_t> base(words), modulus(words), out(words);
    for (uint32_t i = 0; i < words; i++) {
        base[i] = k.load64(a + 8 * i);
        modulus[i] = k.load64(rsa + 0x10 + 8 * i);
    }
    if (!bignum::mod_exp(base, exponent, modulus, out)) return result(c, 0);
    for (uint32_t i = 0; i < words; i++) k.store64(b + 8 * i, out[i]);
    result(c, 1);
}

XF_EXPORT(xboxkrnl, XeCryptBnQwBeSigFormat) {
    // (XECRYPT_SIG *, hash[20], salt[10]): the block XeCryptBnQwBeSigVerify
    // compares against, built as a big-endian number (zeros, 1, the salt, a
    // hash of the inputs, 0xBC; everything before the hash encrypted with
    // it by RC4, the top bit cleared), stored with its 64-bit words in
    // reverse (after ExCrypt, as Xenia)
    uint32_t sig = arg(k, c, 0);
    const uint8_t *hash = k.host(arg(k, c, 1)), *salt = k.host(arg(k, c, 2));
    uint8_t block[0x100] = {};
    block[0xE0] = 1;
    std::memcpy(block + 0xE1, salt, 0xA);
    block[0xFF] = 0xBC;
    Sha1 s = start<Sha1>(sha1_initial);
    s.update(block, 8);
    s.update(hash, 0x14);
    s.update(salt, 0xA);
    sha1_digest(s, block + 0xEB);
    uint8_t rc4[0x102];
    rc4_key(rc4, block + 0xEB, 0x14);
    rc4_crypt(rc4, block, 0xEB);
    block[0] &= 0x7F;
    for (int i = 0; i < 32; i++) std::memcpy(k.host(sig + 8 * i), block + 0x100 - 8 * (i + 1), 8);
}

XF_EXPORT(xboxkrnl, XeCryptBnQwBeSigVerify) {
    // (XECRYPT_SIG *, hash, salt, XECRYPT_RSA *): Xenia's stub, valid (the
    // hash comes from XeCryptRotSumSha; the known salts are "XBOX360SVC",
    // "XBOX360XTT", "TDBXBOX36O")
    result(c, 1);
}

XF_EXPORT(xboxkrnl, XeCryptBnQwNeModInv) {
    // (n): n^-1 mod 2^64 for an odd n, from five correct bits by Newton's
    // steps (Xenia's arithmetic); a 64-bit result
    uint64_t input = arg64(k, c, 0);
    uint64_t result = (input * 3) ^ 2;
    uint64_t intermediate = 1 - result * input;
    for (uint32_t i = 5; i < 32; i <<= 1) {
        result = result * (intermediate + 1);
        intermediate = intermediate * intermediate;
    }
    c.r[3] = result * (intermediate + 1);
}

XF_EXPORT(xboxkrnl, XeCryptBnQwNeModMul) {
    // (a, b, out, inverse, modulus, size): Montgomery's multiplication,
    // out = a * b / 2^(64 * size) mod modulus, inverse being
    // XeCryptBnQwNeModInv(modulus[0]). As the kernel (and Xenia) do it: the
    // sums of a * b and m * modulus kept apart and subtracted at the end,
    // the modulus added back unless the first is larger. The inputs are only
    // read before out is written, so out may be one of them.
    uint32_t a = arg(k, c, 0), b = arg(k, c, 1), out = arg(k, c, 2), modulus = arg(k, c, 4);
    uint64_t inverse = arg64(k, c, 3);
    uint32_t n = arg(k, c, 5);
    if (!n) return;
    using u128 = unsigned __int128;
    std::vector<uint64_t> ab(n), mn(n), m_words(n);
    uint64_t a0 = k.load64(a);
    for (uint32_t j = 0; j < n; j++) m_words[j] = k.load64(modulus + 8 * j);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t bi = k.load64(b + 8 * i);
        // makes the low words of both sums equal, so they drop out of the shift
        uint64_t m = (ab[0] - mn[0] + bi * a0) * inverse;
        uint64_t ab_carry = 0, mn_carry = 0;
        for (uint32_t j = 0; j < n; j++) {
            u128 t = u128(bi) * k.load64(a + 8 * j) + ab[j] + ab_carry;
            u128 u = u128(m) * m_words[j] + mn[j] + mn_carry;
            if (j) {
                ab[j - 1] = uint64_t(t);
                mn[j - 1] = uint64_t(u);
            }
            ab_carry = uint64_t(t >> 64);
            mn_carry = uint64_t(u >> 64);
        }
        ab[n - 1] = ab_carry;
        mn[n - 1] = mn_carry;
    }
    bool greater = false;
    for (uint32_t i = n; i-- > 0;)
        if (ab[i] != mn[i]) {
            greater = ab[i] > mn[i];
            break;
        }
    uint64_t carry = 0, borrow = 0;
    for (uint32_t i = 0; i < n; i++) {
        u128 sum = u128(ab[i]) + (greater ? 0 : m_words[i]) + carry;
        carry = uint64_t(sum >> 64);
        uint64_t lo = uint64_t(sum), diff = lo - mn[i];
        uint64_t next_borrow = lo < mn[i] || diff < borrow;
        k.store64(out + 8 * i, diff - borrow);
        borrow = next_borrow;
    }
}

// A PKCS #1 v1.5 signature block for a SHA-1 hash, as a little-endian
// number: format 0 is the standard DigestInfo, 1 leaves out its NULL
// parameters, 2 leaves it out (after ExCrypt, as Xenia). Sizes outside 39
// to 512 get only the 0xFF fill.
void pkcs1_format(const uint8_t *hash, uint32_t format, uint8_t *sig, uint32_t size) {
    // the DigestInfo prefixes reversed, each ending with the 0x00 separator
    static constexpr uint8_t digest_info0[] = {0x14, 0x04, 0x00, 0x05, 0x1A, 0x02, 0x03, 0x0E,
                                               0x2B, 0x05, 0x06, 0x09, 0x30, 0x21, 0x30, 0x00};
    static constexpr uint8_t digest_info1[] = {0x14, 0x04, 0x1A, 0x02, 0x03, 0x0E, 0x2B,
                                               0x05, 0x06, 0x07, 0x30, 0x1F, 0x30, 0x00};
    std::memset(sig, 0xFF, size);
    if (size < 39 || size > 512) return;
    sig[size - 1] = 0;
    sig[size - 2] = 1;
    std::reverse_copy(hash, hash + 0x14, sig);
    if (format == 0)
        std::memcpy(sig + 0x14, digest_info0, sizeof(digest_info0));
    else if (format == 1)
        std::memcpy(sig + 0x14, digest_info1, sizeof(digest_info1));
    else if (format == 2)
        sig[0x14] = 0;
}

XF_EXPORT(xboxkrnl, XeCryptBnDwLePkcs1Format) {
    // (hash[20], format, signature, size)
    uint32_t size = arg(k, c, 3);
    if (size) pkcs1_format(k.host(arg(k, c, 0)), arg(k, c, 1), k.host(arg(k, c, 2)), size);
}

XF_EXPORT(xboxkrnl, XeCryptBnDwLePkcs1Verify) {
    // (hash[20], signature, size): the block rebuilt in the format its byte
    // 0x16 tells (0x00 in format 0, 0x1A in format 1) and compared
    uint32_t size = arg(k, c, 2);
    if (size < 39 || size > 512) return result(c, 0);
    const uint8_t *sig = k.host(arg(k, c, 1));
    uint32_t format = sig[0x16] == 0 ? 0 : sig[0x16] == 0x1A ? 1 : 2;
    std::vector<uint8_t> expected(size);
    pkcs1_format(k.host(arg(k, c, 0)), format, expected.data(), size);
    result(c, std::memcmp(expected.data(), sig, size) == 0);
}

// ---- random ----

XF_EXPORT(xboxkrnl, XeCryptRandom) {
    // (buffer, size): random bytes, from a generator per thread seeded by
    // the host, as Xenia
    thread_local std::mt19937_64 generator(std::random_device{}());
    uint32_t buffer = arg(k, c, 0), size = arg(k, c, 1);
    for (uint32_t i = 0; i < size; i++) k.host(buffer + i)[0] = uint8_t(generator());
}

// ---- the key vault (XeKeys*) ----

// The keys Xenia has. The serial numbers are made up; 0x19 is the retail
// roamable obfuscation key; 0x39 is the retail PIRS public key
// (XECRYPT_RSAPUB_2048: 32 words, exponent 3, then the modulus).
constexpr uint8_t serial_number[12] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '1', '0'};
constexpr uint8_t key19[16] = {0xE1, 0xBC, 0x15, 0x9C, 0x73, 0xB1, 0xEA, 0xE9,
                               0xAB, 0x31, 0x70, 0xF3, 0xAD, 0x47, 0xEB, 0xF3};
constexpr uint8_t key39[0x110] = {
    0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE6, 0x3B, 0x32,
    0xB2, 0x8D, 0x9E, 0x9E, 0xE7, 0x9D, 0xFC, 0x5C, 0x72, 0x41, 0x94, 0x58, 0x47, 0xDE, 0x0D, 0x18, 0x40, 0x72, 0xD6,
    0xE3, 0x46, 0x8E, 0xBA, 0x8E, 0xBC, 0x1A, 0x90, 0xAC, 0x20, 0xBA, 0x03, 0x85, 0xB5, 0x1A, 0x3E, 0x25, 0xF9, 0xA6,
    0x58, 0xEB, 0xB6, 0xA3, 0xC4, 0xA3, 0xEE, 0xB2, 0xB0, 0xAE, 0x97, 0x69, 0xEB, 0xFE, 0x71, 0xFC, 0x02, 0xAB, 0x77,
    0xBA, 0xC8, 0xE6, 0x74, 0xE6, 0x7C, 0x63, 0x0E, 0xAF, 0x4C, 0xF7, 0xE7, 0x11, 0x4A, 0x80, 0x24, 0x72, 0x05, 0x7A,
    0x63, 0xD0, 0xF8, 0x91, 0x02, 0xA6, 0xE7, 0x7D, 0x77, 0xC5, 0xA7, 0x9B, 0x08, 0x11, 0x2E, 0xA0, 0x64, 0x45, 0x60,
    0x46, 0xBC, 0x36, 0xE1, 0x17, 0x71, 0xBE, 0x66, 0x49, 0x2F, 0xAE, 0x20, 0xA4, 0x76, 0x9C, 0x27, 0x51, 0xCF, 0x4B,
    0x34, 0x7A, 0x35, 0xBC, 0xA4, 0xAA, 0x1C, 0x47, 0x4B, 0xF4, 0x97, 0x22, 0x4E, 0x13, 0x24, 0xD3, 0xC1, 0x57, 0xDF,
    0x4D, 0x84, 0xB9, 0x18, 0x97, 0x99, 0xAC, 0x00, 0xB3, 0x3D, 0x03, 0x25, 0x60, 0xC8, 0x7A, 0x59, 0xFE, 0x48, 0xFF,
    0x28, 0x3D, 0x10, 0xBB, 0x9E, 0x09, 0x06, 0x2A, 0x61, 0x20, 0x2C, 0xF8, 0x72, 0xEB, 0x87, 0xE6, 0xD1, 0xFB, 0xB3,
    0x66, 0xFC, 0x4A, 0x02, 0xAE, 0xD4, 0xD8, 0x37, 0xCF, 0xA6, 0x32, 0x25, 0x79, 0x36, 0x0E, 0xF4, 0xED, 0x19, 0xA2,
    0x10, 0x27, 0x96, 0x2F, 0x9F, 0xA9, 0x3D, 0xA4, 0x37, 0x30, 0x11, 0x51, 0x83, 0xBD, 0xF7, 0xC7, 0xE5, 0xCE, 0xAA,
    0xEC, 0xDE, 0x48, 0xA0, 0x84, 0xF7, 0xB0, 0xF6, 0x4B, 0x8E, 0xF0, 0x89, 0xBD, 0x47, 0x7C, 0x90, 0xDD, 0x88, 0x12,
    0x17, 0x40, 0xD2, 0x4E, 0xA6, 0xC6, 0x11, 0x04, 0x1B, 0x57, 0xA8, 0x68, 0xB4, 0x61, 0xF4, 0x1B, 0xC6, 0x8B, 0xE8,
    0xD9, 0x20, 0xF2, 0x05, 0xE0, 0x70};

constexpr uint32_t CONSOLE_PRIVATE_KEY = 0x33;
constexpr uint32_t SECURED_DATA_LIMIT = 0x2000;

std::span<const uint8_t> vault_key(uint32_t key) {
    switch (key) {
    case 0x14: // the console's serial number
    case 0x15: // the motherboard's
        return serial_number;
    case 0x19: return key19;
    case 0x39: return key39;
    default: return {};
    }
}

// Each key's size in the key vault (Xenia's, after ExCrypt), 0 for those it
// does not list.
uint32_t key_size(uint32_t key) {
    static constexpr uint16_t sizes[0x39] = {
        0x1,  0x1,  0x1,  0x1,  0x2,  0x2,  0x4,   0x4,   0x4,   0x4,   0x8,    0x8,    0x8,  0x8,  0x10,
        0x10, 0x10, 0x10, 0x10, 0x10, 0xC,  0xC,   0x2,   0x10,  0x10,  0x10,   0x10,   0x18, 0x10, 0x10,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,  0x10,  0x10,  0x10,  0x10,   0x10,   0x10, 0x10, 0x10,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1D0, 0x390, 0x1D0, 0x1A8, 0x1288, 0x2108,
    };
    if (key < std::size(sizes)) return sizes[key];
    return key == 0x44 ? 0x100 : 0; // the key vault's signature
}

XF_EXPORT(xboxkrnl, XeKeysGetKeyProperties) {
    // (key): its size
    uint32_t key = arg(k, c, 0), size = key_size(key);
    if (!size) std::fprintf(stderr, "kernel: XeKeysGetKeyProperties: key 0x%04X not implemented\n", key & 0xFFFF);
    result(c, size);
}

XF_EXPORT(xboxkrnl, XeKeysGetKey) {
    // (key (16 bits), buffer, length *): the console's private key cannot
    // be read, keys past the vault fail, and a key Xenia has goes only to a
    // buffer of exactly its size (the status codes are not verified);
    // others succeed untouched
    uint32_t key = arg(k, c, 0) & 0xFFFF, buffer = arg(k, c, 1), length = arg(k, c, 2);
    if (key == CONSOLE_PRIVATE_KEY) return result(c, X_STATUS_ACCESS_DENIED);
    if (key >= SECURED_DATA_LIMIT) return result(c, X_STATUS_INVALID_PARAMETER);
    std::span<const uint8_t> data = vault_key(key);
    if (data.empty()) {
        std::fprintf(stderr, "kernel: XeKeysGetKey: key 0x%04X not implemented\n", key);
        return result(c, X_STATUS_SUCCESS);
    }
    if (k.load32(length) != data.size()) return result(c, X_STATUS_BUFFER_TOO_SMALL);
    std::memcpy(k.host(buffer), data.data(), data.size());
    k.store32(length, uint32_t(data.size()));
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XeKeysHmacSha) {
    // (key, input1, size1, input2, size2, input3, size3, output,
    // output_size): the HMAC under a vault key; only 0x19 is there
    if (arg(k, c, 0) != 0x19) return result(c, X_STATUS_UNSUCCESSFUL);
    HmacSha s(key19, sizeof(key19));
    update3(k, c, 1, s.inner);
    uint8_t digest[20];
    s.finish(digest);
    output(k, arg(k, c, 7), arg(k, c, 8), digest, sizeof(digest));
    result(c, X_STATUS_SUCCESS);
}

// The console's KEY_OBFUSCATION_KEY (0x18) is per console; Xenia's is zero.
// Keys are obscured by encrypting them with it (HvxKeysObscureKey).
aes::Key obfuscation_key() {
    static const uint8_t zero[16] = {};
    return aes::expand(zero);
}

void unobscure(Kernel &k, uint32_t obscured, uint8_t key[16]) {
    aes::decrypt_block(obfuscation_key(), k.host(obscured), key);
}

XF_EXPORT(xboxkrnl, XeKeysObscureKey) {
    // (input[16], output[16])
    aes::encrypt_block(obfuscation_key(), k.host(arg(k, c, 0)), k.host(arg(k, c, 1)));
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XeKeysAesCbcUsingKey) {
    // (obscured key[16], input, size, output, feed[16], encrypt)
    uint8_t key[16];
    unobscure(k, arg(k, c, 0), key);
    uint32_t size = arg(k, c, 2);
    if (size >= 16)
        aes_cbc(aes::expand(key), k.host(arg(k, c, 1)), size, k.host(arg(k, c, 3)), k.host(arg(k, c, 4)),
                arg(k, c, 5) != 0);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XeKeysHmacShaUsingKey) {
    // (obscured key[16], input1, size1, input2, size2, input3, size3,
    // output, output_size)
    uint32_t obscured = arg(k, c, 0);
    if (!obscured) return result(c, X_STATUS_INVALID_PARAMETER);
    uint8_t key[16];
    unobscure(k, obscured, key);
    HmacSha s(key, sizeof(key));
    update3(k, c, 1, s.inner);
    uint8_t digest[20];
    s.finish(digest);
    output(k, arg(k, c, 7), arg(k, c, 8), digest, sizeof(digest));
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XeKeysGetConsoleType) {
    // (type *): retail
    k.store32(arg(k, c, 0), 2);
    result(c, X_STATUS_SUCCESS);
}

// Xenia's made-up console id (XE_CONSOLE_ID's bit fields as MSVC lays them
// out: refurb 3 and month 9 in the first byte, then year 1, MAC index bytes
// 0x40, 0x66, 0x7E and CRC 0 in a little-endian word)
constexpr uint8_t console_id[5] = {0x93, 0x01, 0x64, 0xE6, 0x07};

XF_EXPORT(xboxkrnl, XeKeysGetConsoleID) {
    // (XE_CONSOLE_ID *raw, hex string): the id, and Xenia's text for it (12
    // characters, no terminator)
    uint32_t raw = arg(k, c, 0), text = arg(k, c, 1);
    if (raw) std::memcpy(k.host(raw), console_id, sizeof(console_id));
    if (text) std::memcpy(k.host(text), "245149100000", 12);
    result(c, X_STATUS_SUCCESS);
}

XF_EXPORT(xboxkrnl, XeKeysConsolePrivateKeySign) {
    // (hash, XE_CONSOLE_CERTIFICATE *output): no signature, Xenia's made-up
    // retail certificate
    uint32_t hash = arg(k, c, 0), output = arg(k, c, 1);
    if (!hash || !output) return result(c, 0);
    std::memset(k.host(output), 0, 0x1A8);
    std::memcpy(k.host(output + 0x02), console_id, sizeof(console_id));
    k.store32(output + 0x18, 2); // console type: retail
    static constexpr uint8_t manufacture_date[8] = {2, 0, 0, 5, 1, 1, 2, 2};
    std::memcpy(k.host(output + 0x1C), manufacture_date, sizeof(manufacture_date));
    result(c, 1);
}

XF_EXPORT(xboxkrnl, XeKeysConsoleSignatureVerification) {
    // (hash[20], XE_CONSOLE_SIGNATURE *, LONG *compare_result): Xenia has
    // none, so there an import of it answers 0. The console checks the
    // signature's certificate (XE_CONSOLE_CERTIFICATE) against Microsoft's
    // key and the signature over the hash against the certificate's public
    // key; neither key is here, and XeKeysConsolePrivateKeySign signs
    // nothing. Every signature is taken as valid and as this console's own,
    // which is what the data a game signed itself through
    // XeKeysConsolePrivateKeySign is: TRUE, and a nonzero comparison
    // result. The XDK code seen calling it (on a block read from the hard
    // disk, magic "Josh") zeroes the LONG first and trusts the data only
    // when both are nonzero, rebuilding it from defaults otherwise; this
    // answer takes the console's own path instead of the rebuild Xenia's 0
    // forces.
    uint32_t compare_result = arg(k, c, 2);
    if (compare_result) k.store32(compare_result, 1);
    result(c, 1);
}

} // namespace
} // namespace xf::kernel
