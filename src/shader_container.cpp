// Shader containers found in data. Layout from XenosRecomp's shader.h and
// shader_recompiler.cpp (hedge-dev, MIT): flags, virtual size, physical
// size, a word, the constant table's offset, the definitions' offset, the
// shader header's offset, two zero words; the header starts with the
// microcode's offset into the physical part and its size in bytes. The
// definitions, after five words of their own header: float constants as
// (register, count, offset of the values in the physical part) until a zero
// word, then loop constants as (register, count, the values) until another;
// a pixel shader's float registers count from 256, loop registers from 8992
// four apart. Every field is checked against the data before it is used.

#include "xenoforge/shader_container.h"

#include "xenoforge/bytes.h"

namespace xf {
namespace {

void read_definitions(const uint8_t *c, uint64_t virtual_size, uint64_t total, uint32_t table, bool vertex,
                      ShaderContainer &s) {
    if (!table || table + 20 > virtual_size) return;
    uint64_t at = table + 20;
    auto word = [&](uint64_t offset, uint32_t &out) {
        if (offset + 4 > virtual_size) return false;
        out = be32(c + offset);
        return true;
    };
    uint32_t w = 0;
    // float constants
    while (word(at, w) && w != 0) {
        uint32_t reg = w >> 16, count = w & 0xFFFF, offset = 0;
        if (!word(at + 4, offset)) return;
        uint64_t values = virtual_size + offset;
        for (uint32_t k = 0; k < (count + 3) / 4; k++) {
            if (values + 16 > total) return;
            ShaderContainer::FloatConstant f;
            f.index = reg + k - (vertex ? 0 : 256);
            for (int j = 0; j < 4; j++) f.words[j] = be32(c + values + 4 * j);
            if (f.index < 256) s.float_constants.push_back(f);
            values += 16;
        }
        at += 8;
    }
    at += 4;
    // loop constants
    while (word(at, w) && w != 0) {
        uint32_t reg = w >> 16, count = w & 0xFFFF;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t value = 0;
            if (!word(at + 4 + 4 * k, value)) return;
            if (reg >= 8992) {
                uint32_t index = (reg - 8992) / 4 + k;
                if (index < 32) s.loop_constants.emplace_back(index, value);
            }
        }
        at += 4 + 4 * uint64_t(count);
    }
}

} // namespace

std::vector<ShaderContainer> find_shader_containers(std::span<const uint8_t> data) {
    std::vector<ShaderContainer> found;
    constexpr size_t HEADER = 36;
    for (size_t at = 0; at + HEADER <= data.size();) {
        const uint8_t *c = data.data() + at;
        uint32_t flags = be32(c);
        uint64_t virtual_size = be32(c + 4), physical_size = be32(c + 8);
        uint64_t total = virtual_size + physical_size;
        uint32_t shader_offset = be32(c + 24);
        bool looks = (flags & 0xFFFFFF00u) == 0x102A1100u && be32(c + 28) == 0 && be32(c + 32) == 0 &&
                     total <= data.size() - at && shader_offset + 8 <= virtual_size;
        if (looks) {
            uint64_t code_offset = virtual_size + be32(c + shader_offset);
            uint64_t code_size = be32(c + shader_offset + 4);
            if (code_size % 4 == 0 && code_size && code_offset + code_size <= total) {
                ShaderContainer s;
                s.offset = at;
                s.vertex = flags & 1;
                s.flags = flags;
                for (uint64_t k = 0; k < code_size; k += 4) s.microcode.push_back(be32(c + code_offset + k));
                read_definitions(c, virtual_size, total, be32(c + 20), s.vertex, s);
                found.push_back(std::move(s));
                at += total;
                continue;
            }
        }
        at += 4;
    }
    return found;
}

} // namespace xf
