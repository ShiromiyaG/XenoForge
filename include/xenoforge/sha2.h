// SHA-256, SHA-384 and SHA-512 (FIPS 180-4), for the kernel's
// XeCryptSha256*, XeCryptSha384* and XeCryptSha512* contexts: the block
// functions and the initial hash values. The contexts keep the words, the
// partial block and the count themselves, as SHA-1's do (sha1.h).

#pragma once

#include <cstdint>

namespace xf {

// FIPS 180-4 section 5.3: the first words of each hash
extern const uint32_t sha256_initial[8];
extern const uint64_t sha384_initial[8];
extern const uint64_t sha512_initial[8];

// One block into the eight hash words: 64 bytes for SHA-256, 128 bytes for
// SHA-384 and SHA-512 (the same function, with SHA-384's initial words).
void sha256_block(uint32_t h[8], const uint8_t block[64]);
void sha512_block(uint64_t h[8], const uint8_t block[128]);

} // namespace xf
