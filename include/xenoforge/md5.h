// MD5 (RFC 1321), for the kernel's XeCryptMd5* contexts and HMAC-MD5: the
// block function and the initial words. The contexts keep the words, the
// partial block and the count themselves, as SHA-1's do (sha1.h).

#pragma once

#include <cstdint>

namespace xf {

// RFC 1321 section 3.3: A, B, C, D
extern const uint32_t md5_initial[4];

// One 64-byte block (little-endian words) into A, B, C, D.
void md5_block(uint32_t h[4], const uint8_t block[64]);

} // namespace xf
