// LZX decompression, for the executables packed with normal compression.
// Written from Microsoft's open specification of the format ([MS-PATCH],
// "LZX DELTA Compression and Decompression"), in the variant the console
// uses: LZX as cabinets have it, which LZX DELTA extends (no reference data,
// no extra length field: a match is 2 to 257 bytes; windows of 2^15 to 2^25,
// LZX DELTA's range).

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

// The same with reference data, as a patch over known bytes uses it (a title
// update's delta): the window starts holding reference (its last window_size
// bytes at most, zeros before it if it is shorter), and a match may reach
// back into it before the first output byte. The stream is the cabinet's
// form, as Xenia decodes patches (it presets the window's data and leaves
// the delta extensions off).
bool lzx_decompress_delta(std::span<const uint8_t> in, std::span<const uint8_t> reference, std::span<uint8_t> out,
                          uint32_t window_size, std::string &error);

} // namespace xf
