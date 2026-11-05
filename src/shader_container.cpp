// Shader containers found in data. Layout from XenosRecomp's shader.h
// (hedge-dev, MIT): flags, virtual size, physical size, a word, the constant
// table's offset, the definitions' offset, the shader header's offset, two
// zero words; the header starts with the microcode's offset into the
// physical part and its size in bytes. Every field is checked against the
// data before it is used.

#include "xenoforge/shader_container.h"

#include "xenoforge/bytes.h"

namespace xf {

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
