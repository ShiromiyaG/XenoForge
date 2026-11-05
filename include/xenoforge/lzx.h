// LZX decompression, for the executables packed with normal compression.
// Written from Microsoft's open specification of the format ([MS-PATCH],
// "LZX DELTA Compression and Decompression"), in the variant the console
// uses: LZX as cabinets have it, which LZX DELTA extends (no reference data,
// no extra length field: a match is 2 to 257 bytes; windows of 2^15 to 2^21).

#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace xf {

// Decompresses exactly out.size() bytes from the bitstream in: its 32 KB
// chunks joined, without the sizes that prefix them in a file. Every read is
// checked; a stream that is malformed, ends early or copies from before its
// start is refused with a reason in error.
bool lzx_decompress(std::span<const uint8_t> in, std::span<uint8_t> out, uint32_t window_size, std::string &error);

} // namespace xf
