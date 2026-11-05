// SHA-1 (FIPS 180-4), for the hashes that chain an executable's compressed
// blocks.

#pragma once

#include <cstdint>
#include <span>

namespace xf {

void sha1(std::span<const uint8_t> data, uint8_t digest[20]);

} // namespace xf
