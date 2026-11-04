// AES-128, as the console's executables are encrypted: the file key decrypted
// with ECB, then the image with CBC from a zero IV. Written from FIPS-197.

#pragma once

#include <cstddef>
#include <cstdint>

namespace xf::aes {

// The 11 round keys of AES-128, expanded once per key.
struct Key {
    uint8_t round[176];
};

Key expand(const uint8_t key[16]);

void encrypt_block(const Key &key, const uint8_t in[16], uint8_t out[16]);
void decrypt_block(const Key &key, const uint8_t in[16], uint8_t out[16]);

// In place; size is a multiple of 16.
void decrypt_ecb(const Key &key, uint8_t *data, size_t size);
void decrypt_cbc(const Key &key, const uint8_t iv[16], uint8_t *data, size_t size);
void encrypt_cbc(const Key &key, const uint8_t iv[16], uint8_t *data, size_t size);

} // namespace xf::aes
