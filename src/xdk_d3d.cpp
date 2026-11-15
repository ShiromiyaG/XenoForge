// The guest's Direct3D as the XDK builds it (xenoforge/xdk_d3d.h).

#include "xenoforge/xdk_d3d.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace xf::xdk {

namespace {

// XDK 2.0.21250.0 (D3D9, D3D9LTCG)
constexpr D3DLayout XDK_21250 = [] {
    D3DLayout l;
    l.xdk_build = 21250;
    l.pixel_shader = 0x330C, l.vertex_shader = 0x3310;
    l.fetch_constants = 0x480, l.vs_constants = 0x780, l.ps_constants = 0x1780;
    l.index_buffer = 0x320C, l.viewport = 0x32E0, l.scissor_enabled = 0x3000, l.scissor = 0x32FC;
    l.blocks[0] = {0x2000, 0x10, 10368}, l.blocks[1] = {0x2080, 3, 10432}, l.blocks[2] = {0x2100, 0x15, 10444};
    l.blocks[3] = {0x2180, 5, 10528}, l.blocks[4] = {0x2200, 0x21, 10548}, l.blocks[5] = {0x2280, 0x15, 10596};
    l.blocks[6] = {0x2300, 0x26, 10680}, l.blocks[7] = {0x2380, 8, 10832};
    l.vs_header = 0x368, l.vs_descriptor = 0x380, l.vs_depth_only = 0x388, l.vs_depth_only_flag = 0x20;
    l.vs_data = 0x20;
    l.ps_header = 0x28, l.ps_descriptor = 0x40, l.ps_data = 0x18;
    return l;
}();

// count big-endian words from guest memory, in host order (one copy, then swapped in place)
void read_words(const port::Context &c, uint32_t address, uint32_t *out, size_t count) {
    std::memcpy(out, port::host(c, address), count * 4);
    for (size_t i = 0; i < count; i++) out[i] = __builtin_bswap32(out[i]);
}

} // namespace

const D3DLayout *d3d_layout(uint32_t xdk_build) { return xdk_build == 21250 ? &XDK_21250 : nullptr; }

std::shared_ptr<const ShaderObject> D3DReader::shader(const port::Context &c, uint32_t object, bool vertex,
                                                      bool no_pixel_shader) const {
    if (!object) return nullptr;
    const bool variant = vertex && no_pixel_shader &&
                         (port::load<uint32_t>(c, object + l_.vs_header) & l_.vs_depth_only_flag);
    const uint32_t descriptor =
        vertex ? object + l_.vs_header + port::load<uint32_t>(c, object + (variant ? l_.vs_depth_only : l_.vs_descriptor))
               : object + l_.ps_header + port::load<uint32_t>(c, object + l_.ps_descriptor);
    const uint32_t base = port::load<uint32_t>(c, object + (vertex ? l_.vs_data : l_.ps_data));
    const uint32_t offset = port::load<uint32_t>(c, descriptor), bytes = port::load<uint32_t>(c, descriptor + 4);
    thread_local std::unordered_map<uint64_t, std::shared_ptr<const ShaderObject>> known;
    const uint64_t key =
        (uint64_t(object) << 32 | (base + offset)) ^ (uint64_t(bytes) << 48) ^ uint64_t(vertex) << 47;
    if (auto it = known.find(key); it != known.end()) return it->second;
    auto s = std::make_shared<ShaderObject>();
    if (!base || !bytes || bytes % 12 || bytes > 0x10000) {
        s->bad = true;
    } else {
        s->words.resize(bytes / 4);
        read_words(c, base + offset, s->words.data(), s->words.size());
        const auto stage = vertex ? native::ShaderStage::Vertex : native::ShaderStage::Pixel;
        s->hash = native::shader_hash(s->words.data(), s->words.size(), stage);
        native::shader_uses(s->words.data(), s->words.size(), stage, s->uses);
    }
    known.emplace(key, s);
    return s;
}

void D3DReader::literals(const port::Context &c, uint32_t object, bool vertex, uint32_t vs[1024],
                         uint32_t ps[1024]) const {
    if (!object) return;
    const uint32_t header = object + (vertex ? l_.vs_header : l_.ps_header);
    const uint32_t block = header + port::load<uint32_t>(c, header + 0x14);
    const uint32_t data = port::load<uint32_t>(c, object + (vertex ? l_.vs_data : l_.ps_data));
    const uint32_t end = block + 0x14 + port::load<uint32_t>(c, block + 0x10);
    if (end - block > 0x2000) return; // not a block
    for (uint32_t e = block + 0x14; e + 8 <= end; e += 8) {
        const uint32_t destination = port::load<uint16_t>(c, e), dwords = port::load<uint16_t>(c, e + 2);
        const uint32_t offset = port::load<uint32_t>(c, e + 4);
        if (!dwords) break;
        const uint32_t type = ((destination << 2) >> 16) & 0xFF, first = (destination << 2) & 0x7FF;
        if (type != 0) continue; // fetch, bool and loop constants
        for (uint32_t k = 0; k < dwords && first + k < 2048; k++) {
            const uint32_t at = first + k;
            (at < 1024 ? vs[at] : ps[at - 1024]) = port::load<uint32_t>(c, data + offset + 4 * k);
        }
    }
}

void D3DReader::constants(const port::Context &c, uint32_t dev, uint32_t vs[1024], uint32_t ps[1024]) const {
    read_words(c, dev + l_.vs_constants, vs, 1024);
    read_words(c, dev + l_.ps_constants, ps, 1024);
    literals(c, vertex_shader(c, dev), true, vs, ps);
    literals(c, pixel_shader(c, dev), false, vs, ps);
}

void D3DReader::fetch_constants(const port::Context &c, uint32_t dev, uint32_t out[192]) const {
    read_words(c, dev + l_.fetch_constants, out, 192);
    // a slot whose first word has type 0 is a texture's (vertex fetches have type 3): the D3D sets its 2
    for (uint32_t slot = 0; slot < 32; slot++)
        if ((out[slot * 6] & 3) == 0) out[slot * 6] |= 2;
}

void D3DReader::registers(const port::Context &c, uint32_t dev, uint32_t out[0x400]) const {
    std::memset(out, 0, 0x400 * 4);
    for (const D3DLayout::Block &b : l_.blocks)
        if (b.count) read_words(c, dev + b.offset, out + (b.first - 0x2000), b.count);
}

D3DReader::IndexBuffer D3DReader::index_buffer(const port::Context &c, uint32_t dev) const {
    IndexBuffer out;
    const uint32_t ib = port::load<uint32_t>(c, dev + l_.index_buffer);
    if (!ib) return out;
    out.index32 = port::load<uint32_t>(c, ib) >> 31;
    out.address = port::load<uint32_t>(c, ib + 0x18);
    return out;
}

void D3DReader::clear_area(const port::Context &c, uint32_t dev, uint32_t rect, int32_t out[4]) const {
    const int32_t x = int32_t(port::load<float>(c, dev + l_.viewport));
    const int32_t y = int32_t(port::load<float>(c, dev + l_.viewport + 4));
    out[0] = x, out[1] = y;
    out[2] = x + int32_t(port::load<float>(c, dev + l_.viewport + 8));
    out[3] = y + int32_t(port::load<float>(c, dev + l_.viewport + 12));
    auto within = [&](uint32_t at) {
        int32_t r[4];
        for (int i = 0; i < 4; i++) r[i] = int32_t(port::load<uint32_t>(c, at + 4 * i));
        out[0] = std::max(out[0], r[0]), out[1] = std::max(out[1], r[1]);
        out[2] = std::min(out[2], r[2]), out[3] = std::min(out[3], r[3]);
    };
    if (rect >= 0x10000) within(rect);
    if (port::load<uint32_t>(c, dev + l_.scissor_enabled)) within(dev + l_.scissor);
}

} // namespace xf::xdk
