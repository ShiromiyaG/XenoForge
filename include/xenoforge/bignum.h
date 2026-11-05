// Modular exponentiation, for the RSA public operation of the kernel's
// XeCryptBnQwNeRsaPubCrypt. Numbers are arrays of 64-bit words, least
// significant first, as the console's XeCryptBnQw* functions hold them (the
// kernel loads each word from guest memory big-endian).

#pragma once

#include <cstdint>
#include <span>

namespace xf::bignum {

// out = base ^ exponent mod modulus, every array modulus.size() words (out
// may be base). False, and out untouched, for an empty or even modulus:
// Montgomery's method needs an odd one, as every RSA modulus is.
bool mod_exp(std::span<const uint64_t> base, uint32_t exponent, std::span<const uint64_t> modulus,
             std::span<uint64_t> out);

} // namespace xf::bignum
