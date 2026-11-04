// Snappy decompression, for Xenia's GPU traces (their memory, registers and
// EDRAM snapshots are snappy-compressed). Written from the format's
// description (google/snappy, format_description.txt).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace xf {

// The decompressed length the stream announces, or false if it is malformed.
bool snappy_length(std::span<const uint8_t> in, uint32_t &length);

// Every byte checked: a stream that reads or copies out of bounds, or whose
// output differs from the length it announces, is refused.
bool snappy_decompress(std::span<const uint8_t> in, std::vector<uint8_t> &out);

} // namespace xf
