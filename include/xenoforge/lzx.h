// LZX decompression, for the executables packed with normal compression.
// Written from Microsoft's open specification of the format ([MS-PATCH],
// "LZX DELTA Compression and Decompression"), in the variant the console
// uses: LZX as cabinets have it, which LZX DELTA extends (no reference data,
// no extra length field: a match is 2 to 257 bytes; windows of 2^15 to 2^25,
// LZX DELTA's range).

#pragma once

#include <cstdint>
#include <memory>
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

// A stream decoded a frame at a time, as a cabinet's data blocks are: each
// call is given one frame's compressed bytes (a whole number of 16-bit words,
// the frame ending its word) and the size it decompresses to, at most 32 KB.
// What a frame needs of the ones before it (the block's trees, the repeated
// offsets, the window) is kept here. The stream's first frame starts with the
// header bit that says whether E8 calls are translated.
class LzxStream {
public:
    LzxStream();
    ~LzxStream();
    LzxStream(const LzxStream &) = delete;
    LzxStream &operator=(const LzxStream &) = delete;

    // a window of 2^15 to 2^25 bytes; false with a reason otherwise
    bool start(uint32_t window_size, std::string &error);
    // the frame's bytes; false with a reason for a malformed frame, which leaves
    // the stream as it was (the bytes it would have decoded are not kept)
    bool frame(std::span<const uint8_t> in, std::span<uint8_t> out, std::string &error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xf
