// Snappy, for Xenia's GPU traces (their memory, registers and EDRAM
// snapshots are snappy-compressed): read, and written by the trace writer.
// Written from the format's description (google/snappy,
// format_description.txt).

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

// A stream any snappy decoder reads (Xenia's included): the input in blocks
// of 64 KB, each matched against itself only, as google's compressor does,
// so every copy's offset fits in two bytes.
std::vector<uint8_t> snappy_compress(std::span<const uint8_t> in);

} // namespace xf
