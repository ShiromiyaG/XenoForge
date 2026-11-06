// Snappy, from its format description: a varint length, then elements whose
// tag's low two bits say a literal (its length in the tag or in 1-4 bytes
// after it) or a copy (offset in 1, 2 or 4 bytes, length in the tag); a copy
// may overlap what it writes.

#include "xenoforge/snappy.h"

#include <algorithm>
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

namespace {

void put_varint(std::vector<uint8_t> &out, uint32_t value) {
    while (value >= 0x80) {
        out.push_back(uint8_t(value | 0x80));
        value >>= 7;
    }
    out.push_back(uint8_t(value));
}

void put_literal(std::vector<uint8_t> &out, const uint8_t *p, size_t size) {
    if (!size) return;
    size_t n = size - 1;
    if (n < 60) {
        out.push_back(uint8_t(n << 2));
    } else {
        // the length in 1-4 bytes after the tag (tags 60-63)
        uint32_t bytes = n < 0x100 ? 1 : n < 0x10000 ? 2 : n < 0x1000000 ? 3 : 4;
        out.push_back(uint8_t((59 + bytes) << 2));
        for (uint32_t i = 0; i < bytes; i++) out.push_back(uint8_t(n >> (8 * i)));
    }
    out.insert(out.end(), p, p + size);
}

// a copy of up to 64 bytes: the 1-byte offset form where it fits, else the
// 2-byte one
void put_copy_part(std::vector<uint8_t> &out, size_t offset, size_t size) {
    if (size >= 4 && size <= 11 && offset < 2048) {
        out.push_back(uint8_t(1 | (size - 4) << 2 | (offset >> 8) << 5));
        out.push_back(uint8_t(offset));
    } else {
        out.push_back(uint8_t(2 | (size - 1) << 2));
        out.push_back(uint8_t(offset));
        out.push_back(uint8_t(offset >> 8));
    }
}

// longer copies in parts, none left shorter than 4 (google's EmitCopy)
void put_copy(std::vector<uint8_t> &out, size_t offset, size_t size) {
    while (size >= 68) {
        put_copy_part(out, offset, 64);
        size -= 64;
    }
    if (size > 64) {
        put_copy_part(out, offset, 60);
        size -= 60;
    }
    put_copy_part(out, offset, size);
}

uint32_t load32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace

std::vector<uint8_t> snappy_compress(std::span<const uint8_t> in) {
    constexpr size_t BLOCK = 1 << 16;
    constexpr uint32_t TABLE_BITS = 14;
    std::vector<uint8_t> out;
    out.reserve(in.size() / 2 + 16);
    put_varint(out, uint32_t(in.size()));
    std::vector<int32_t> table(size_t(1) << TABLE_BITS);
    for (size_t block = 0; block < in.size(); block += BLOCK) {
        const uint8_t *p = in.data() + block;
        size_t size = std::min(BLOCK, in.size() - block);
        std::fill(table.begin(), table.end(), -1);
        size_t literal = 0, i = 0;
        uint32_t misses = 0;
        while (i + 4 <= size) {
            uint32_t word = load32(p + i);
            uint32_t h = (word * 0x1E35A7BDu) >> (32 - TABLE_BITS);
            int32_t candidate = table[h];
            table[h] = int32_t(i);
            if (candidate < 0 || load32(p + candidate) != word) {
                // incompressible data is skipped faster the longer it lasts
                i += 1 + (misses++ >> 5);
                continue;
            }
            misses = 0;
            size_t length = 4;
            while (i + length < size && p[candidate + length] == p[i + length]) length++;
            put_literal(out, p + literal, i - literal);
            put_copy(out, i - size_t(candidate), length);
            i += length;
            literal = i;
        }
        put_literal(out, p + literal, size - literal);
    }
    return out;
}

} // namespace xf
