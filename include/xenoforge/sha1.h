// SHA-1 (FIPS 180-4), for the hashes that chain an executable's compressed
// blocks and for the kernel's XeCryptSha* contexts.

#pragma once

#include <cstdint>
#include <span>

namespace xf {

void sha1(std::span<const uint8_t> data, uint8_t digest[20]);

// One 64-byte block into the five hash words (the kernel's contexts keep the
// words, the partial block and the count themselves).
void sha1_block(uint32_t h[5], const uint8_t block[64]);

// HMAC-SHA-1 (RFC 2104) of data under key; a key past the 64-byte block is
// hashed first, as the RFC says (the kernel's XeCryptHmacSha cuts it instead).
void hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data, uint8_t digest[20]);

} // namespace xf
