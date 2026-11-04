// AES-128 from FIPS-197. The S-box is computed at compile time (the field
// walked by multiplying by 3 and dividing by 3, then the affine map), not
// typed in, and the tests check the standard's vectors.

#include "xenoforge/aes.h"

#include <cstring>

namespace xf::aes {
namespace {

constexpr uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

constexpr uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

constexpr uint8_t rotl8(uint8_t x, int s) { return uint8_t((x << s) | (x >> (8 - s))); }

struct Tables {
    uint8_t sbox[256];
    uint8_t inv_sbox[256];
    uint8_t mul9[256], mul11[256], mul13[256], mul14[256];
};

constexpr Tables make_tables() {
    Tables t{};
    // p goes through every nonzero element (times 3 each step) and q stays
    // its inverse (divided by 3 each step)
    uint8_t p = 1, q = 1;
    do {
        p = uint8_t(p ^ xtime(p));
        q = uint8_t(q ^ (q << 1));
        q = uint8_t(q ^ (q << 2));
        q = uint8_t(q ^ (q << 4));
        if (q & 0x80) q ^= 0x09;
        t.sbox[p] = uint8_t(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    t.sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) {
        t.inv_sbox[t.sbox[i]] = uint8_t(i);
        t.mul9[i] = mul(uint8_t(i), 9);
        t.mul11[i] = mul(uint8_t(i), 11);
        t.mul13[i] = mul(uint8_t(i), 13);
        t.mul14[i] = mul(uint8_t(i), 14);
    }
    return t;
}

constexpr Tables T = make_tables();

// The state is column-major: byte r + 4c is row r of column c.
void add_round_key(uint8_t s[16], const uint8_t *k) {
    for (int i = 0; i < 16; i++) s[i] ^= k[i];
}

void sub_bytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] = T.sbox[s[i]];
}

void inv_sub_bytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] = T.inv_sbox[s[i]];
}

void shift_rows(uint8_t s[16]) {
    uint8_t o[16];
    std::memcpy(o, s, 16);
    for (int r = 1; r < 4; r++)
        for (int c = 0; c < 4; c++) s[r + 4 * c] = o[r + 4 * ((c + r) % 4)];
}

void inv_shift_rows(uint8_t s[16]) {
    uint8_t o[16];
    std::memcpy(o, s, 16);
    for (int r = 1; r < 4; r++)
        for (int c = 0; c < 4; c++) s[r + 4 * c] = o[r + 4 * ((c + 4 - r) % 4)];
}

void mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *a = s + 4 * c;
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        a[0] = uint8_t(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        a[1] = uint8_t(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        a[2] = uint8_t(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        a[3] = uint8_t((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

void inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *a = s + 4 * c;
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        a[0] = uint8_t(T.mul14[a0] ^ T.mul11[a1] ^ T.mul13[a2] ^ T.mul9[a3]);
        a[1] = uint8_t(T.mul9[a0] ^ T.mul14[a1] ^ T.mul11[a2] ^ T.mul13[a3]);
        a[2] = uint8_t(T.mul13[a0] ^ T.mul9[a1] ^ T.mul14[a2] ^ T.mul11[a3]);
        a[3] = uint8_t(T.mul11[a0] ^ T.mul13[a1] ^ T.mul9[a2] ^ T.mul14[a3]);
    }
}

} // namespace

Key expand(const uint8_t key[16]) {
    Key k{};
    std::memcpy(k.round, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {k.round[i - 4], k.round[i - 3], k.round[i - 2], k.round[i - 1]};
        if (i % 16 == 0) {
            uint8_t first = t[0];
            t[0] = uint8_t(T.sbox[t[1]] ^ rcon);
            t[1] = T.sbox[t[2]];
            t[2] = T.sbox[t[3]];
            t[3] = T.sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) k.round[i + j] = uint8_t(k.round[i - 16 + j] ^ t[j]);
    }
    return k;
}

void encrypt_block(const Key &key, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, key.round);
    for (int round = 1; round < 10; round++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, key.round + 16 * round);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, key.round + 160);
    std::memcpy(out, s, 16);
}

void decrypt_block(const Key &key, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, key.round + 160);
    for (int round = 9; round > 0; round--) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, key.round + 16 * round);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, key.round);
    std::memcpy(out, s, 16);
}

void decrypt_ecb(const Key &key, uint8_t *data, size_t size) {
    for (size_t off = 0; off + 16 <= size; off += 16) decrypt_block(key, data + off, data + off);
}

void decrypt_cbc(const Key &key, const uint8_t iv[16], uint8_t *data, size_t size) {
    uint8_t prev[16], ct[16];
    std::memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= size; off += 16) {
        std::memcpy(ct, data + off, 16);
        decrypt_block(key, ct, data + off);
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        std::memcpy(prev, ct, 16);
    }
}

void encrypt_cbc(const Key &key, const uint8_t iv[16], uint8_t *data, size_t size) {
    const uint8_t *prev = iv;
    for (size_t off = 0; off + 16 <= size; off += 16) {
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        encrypt_block(key, data + off, data + off);
        prev = data + off;
    }
}

} // namespace xf::aes
