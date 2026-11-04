// Snappy, from its format description: a varint length, then elements whose
// tag's low two bits say a literal (its length in the tag or in 1-4 bytes
// after it) or a copy (offset in 1, 2 or 4 bytes, length in the tag); a copy
// may overlap what it writes.

#include "xenoforge/snappy.h"

#include <cstring>

namespace xf {
namespace {

bool read_varint(std::span<const uint8_t> in, size_t &pos, uint32_t &value) {
    value = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        if (pos >= in.size()) return false;
        uint8_t b = in[pos++];
        value |= uint32_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

} // namespace

bool snappy_length(std::span<const uint8_t> in, uint32_t &length) {
    size_t pos = 0;
    return read_varint(in, pos, length);
}

bool snappy_decompress(std::span<const uint8_t> in, std::vector<uint8_t> &out) {
    size_t pos = 0;
    uint32_t length = 0;
    if (!read_varint(in, pos, length)) return false;
    out.resize(length);
    size_t at = 0;
    while (pos < in.size()) {
        uint8_t tag = in[pos++];
        size_t size = 0, offset = 0;
        switch (tag & 3) {
        case 0: { // literal
            size = (tag >> 2) + 1;
            if (size > 60) {
                size_t bytes = size - 60;
                if (pos + bytes > in.size()) return false;
                size = 0;
                for (size_t i = 0; i < bytes; i++) size |= size_t(in[pos + i]) << (8 * i);
                size += 1;
                pos += bytes;
            }
            if (size > in.size() - pos || size > out.size() - at) return false;
            std::memcpy(out.data() + at, in.data() + pos, size);
            pos += size;
            at += size;
            continue;
        }
        case 1: // copy, 1-byte offset
            if (pos >= in.size()) return false;
            size = 4 + ((tag >> 2) & 7);
            offset = size_t(tag >> 5) << 8 | in[pos++];
            break;
        case 2: // copy, 2-byte offset
            if (pos + 2 > in.size()) return false;
            size = (tag >> 2) + 1;
            offset = size_t(in[pos]) | size_t(in[pos + 1]) << 8;
            pos += 2;
            break;
        case 3: // copy, 4-byte offset
            if (pos + 4 > in.size()) return false;
            size = (tag >> 2) + 1;
            offset = size_t(in[pos]) | size_t(in[pos + 1]) << 8 | size_t(in[pos + 2]) << 16 |
                     size_t(in[pos + 3]) << 24;
            pos += 4;
            break;
        }
        if (offset == 0 || offset > at || size > out.size() - at) return false;
        // byte by byte: the source may run into what this copy writes
        for (size_t i = 0; i < size; i++, at++) out[at] = out[at - offset];
    }
    return at == out.size();
}

} // namespace xf
