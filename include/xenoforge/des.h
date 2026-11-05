// DES (FIPS 46-3), for the kernel's XeCryptDes* and XeCryptDes3* exports.
// Keys and blocks are 64-bit numbers, the standard's bit 1 the most
// significant (the console loads them big-endian).

#pragma once

#include <cstdint>

namespace xf::des {

// The 16 round keys, 48 bits each, the standard's bit 1 at bit 47.
struct Key {
    uint64_t round[16];
};

// The key schedule (the parity bits, every eighth, are not used).
Key expand(uint64_t key);

uint64_t encrypt(const Key &key, uint64_t block);
uint64_t decrypt(const Key &key, uint64_t block);

} // namespace xf::des
