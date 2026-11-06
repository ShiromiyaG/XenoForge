// texture_load.comp against texture.cpp's decode: every format the renderer
// draws, as the host holds it and through every fallback (what a device
// without BC, 16-bit unorm and snorm or the packed 16-bit formats takes) and
// every separate signed version, laid out in guest memory at random (tiled
// and linear, mips and the packed tail, 3D, cube and stacked, odd sizes, each
// byte swap), loaded on the GPU level by level with the constants the
// renderer passes (texture::gpu_levels) and compared byte for byte with the
// CPU's decode. Without a Vulkan device there is nothing to run.

#include "xenoforge/texture.h"
#include "xenoforge/vk_compute.h"
#include "xenoforge/xenos.h"

#include "check.h"

#include <cstdio>
#include <cstring>

using namespace xf;

namespace {

const uint32_t TEXTURE_LOAD[] = {
#include "texture_load_test.comp.inc"
};

constexpr uint32_t MEMORY_BYTES = 4u << 20; // the textures lie in the first 4 MB

uint32_t hash(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352D, x ^= x >> 15, x *= 0x846CA68B, x ^= x >> 16;
    return x;
}

struct Fetch {
    uint32_t format = 6, dimension = 1, width = 1, height = 1, depth = 1;
    bool tiled = true, stacked = false, packed = false;
    uint32_t pitch = 0, endian = 0, base = 0x10000, mip_base = 0, mip_max = 0;
    uint32_t signs = 0; // two bits a component

    texture::Info info() const {
        uint32_t d[6] = {};
        d[0] = 2 | (signs << 2) | ((pitch >> 5) << 22) | (uint32_t(tiled) << 31);
        d[1] = format | (endian << 6) | (uint32_t(stacked) << 10) | (base & 0xFFFFF000);
        if (dimension == 0) d[2] = width - 1;
        else if (dimension == 2) d[2] = (width - 1) | ((height - 1) << 11) | ((depth - 1) << 22);
        else d[2] = (width - 1) | ((height - 1) << 13) | ((dimension == 3 ? 5 : depth - 1) << 26);
        d[3] = 0x688 << 1;
        d[4] = mip_max << 6;
        d[5] = (dimension << 9) | (uint32_t(packed) << 11) | (mip_base & 0xFFFFF000);
        return texture::parse_fetch_constant(d);
    }
};

struct Gpu {
    std::unique_ptr<vk::Compute> compute;
    std::vector<uint8_t> memory; // the first bytes of guest memory, as the GPU's mirror holds them
    GuestMemory guest;
    uint32_t runs = 0, compared = 0;
};

// One texture's version on both sides; false (and why) where they differ.
bool compare(Gpu &gpu, const Fetch &fetch, const texture::Support &support, bool signed_version, std::string &why) {
    texture::Info info = fetch.info();
    texture::Plan plan;
    texture::Image image;
    if (!texture::plan(info, plan, why, support, signed_version)) return false;
    if (!texture::decode(info, gpu.guest, image, why, support, signed_version)) return false;
    std::vector<texture::GpuLevel> levels;
    uint64_t total = 0;
    texture::gpu_levels(plan, levels, total);
    std::vector<uint32_t> level_of;
    for (size_t i = 0; i < plan.slices.size(); i++)
        if (!i || plan.slices[i].level != plan.slices[i - 1].level) level_of.push_back(plan.slices[i].level);
    for (size_t i = 0; i < levels.size(); i++) {
        texture::GpuLevel g = levels[i];
        uint64_t bytes = texture::level_bytes(plan, level_of[i]);
        if (!g.words) continue;
        uint32_t out_word = g.out_word;
        g.out_word = 0;
        std::vector<uint8_t> mem = gpu.memory, out(size_t(g.words) * 4, 0xCD), params(sizeof(g));
        std::memcpy(params.data(), &g, sizeof(g));
        std::vector<uint8_t> *buffers[] = {&mem, &out, &params};
        std::string error;
        if (!gpu.compute->run(TEXTURE_LOAD, buffers, g.row_words / 64, error)) {
            why = "run: " + error;
            return false;
        }
        gpu.runs++;
        const std::vector<uint8_t> &cpu = image.levels[level_of[i]];
        if (cpu.size() != bytes) {
            why = "level " + std::to_string(level_of[i]) + ": the CPU's is " + std::to_string(cpu.size()) +
                  " bytes, not " + std::to_string(bytes);
            return false;
        }
        for (size_t b = 0; b < bytes; b++)
            if (out[b] != cpu[b]) {
                char text[160];
                std::snprintf(text, sizeof(text), "level %u (word %u): byte %zu is %02X on the GPU, %02X on the CPU",
                              level_of[i], out_word, b, out[b], cpu[b]);
                why = text;
                return false;
            }
        gpu.compared++;
    }
    return true;
}

// The layouts each format is loaded in.
std::vector<Fetch> layouts(uint32_t format) {
    std::vector<Fetch> out;
    texture::Block block = texture::format_block(format);
    uint32_t endian = block.bytes == 2 || (block.width == 2) ? 1 : 2; // 8in16 for 16-bit texels, else 8in32
    Fetch f;
    f.format = format, f.endian = endian;
    // 2D tiled, odd sized, every level down to the packed tail
    f.width = 37, f.height = 19, f.pitch = 64, f.mip_base = 0x200000, f.mip_max = 5, f.packed = true;
    out.push_back(f);
    // 2D linear, another byte swap
    f = Fetch{};
    f.format = format, f.tiled = false, f.width = 40, f.height = 13, f.pitch = 64, f.endian = endian == 1 ? 0 : 3;
    out.push_back(f);
    // 3D tiled, with mips
    f = Fetch{};
    f.format = format, f.dimension = 2, f.width = 20, f.height = 12, f.depth = 5, f.pitch = 32, f.endian = endian;
    f.mip_base = 0x200000, f.mip_max = 2;
    out.push_back(f);
    // a cube, and 3 stacked layers
    f = Fetch{};
    f.format = format, f.dimension = 3, f.width = 16, f.height = 16, f.pitch = 32, f.endian = endian;
    out.push_back(f);
    f.dimension = 1, f.stacked = true, f.depth = 3, f.width = 24, f.height = 10;
    out.push_back(f);
    // 1D, linear
    f = Fetch{};
    f.format = format, f.dimension = 0, f.tiled = false, f.width = 300, f.endian = endian;
    out.push_back(f);
    return out;
}

void loads_match(Gpu &gpu) {
    // what a device without the optional formats has (XF_TEXTURE_FALLBACKS)
    using H = texture::HostFormat;
    texture::Support fallbacks;
    for (H h : {H::BC1, H::BC2, H::BC3, H::BC4, H::BC5, H::R16, H::RG16, H::RGBA16, H::R5G6B5, H::A1R5G5B5,
                H::B4G4R4A4})
        fallbacks.sampled &= ~(uint64_t(1) << uint32_t(h));
    fallbacks.snorm = (uint64_t(1) << uint32_t(H::R8)) | (uint64_t(1) << uint32_t(H::RG8)) |
                      (uint64_t(1) << uint32_t(H::RGBA8));
    const texture::Support supports[2] = {texture::Support{}, fallbacks};
    uint32_t formats = 0, failed = 0;
    for (uint32_t format = 0; format < 64; format++) {
        texture::Plan probe;
        std::string why;
        Fetch f;
        f.format = format, f.width = 8, f.height = 8, f.pitch = 32;
        if (!texture::plan(f.info(), probe, why)) continue; // not drawn
        formats++;
        for (const texture::Support &support : supports)
            for (const Fetch &layout : layouts(format))
                for (bool signed_version : {false, true}) {
                    Fetch l = layout;
                    l.signs = signed_version ? 0x55 : 0;
                    texture::Plan p;
                    if (!texture::plan(l.info(), p, why, support)) {
                        std::printf("  %s: %s\n", xenos::format_name(format), why.c_str());
                        failed++;
                        continue;
                    }
                    if (signed_version && p.signed_read != texture::Signed::Separate) continue;
                    if (!compare(gpu, l, support, signed_version, why)) {
                        std::printf("  %s (%s%s, dimension %u, %s): %s\n", xenos::format_name(format),
                                    texture::host_format_name(p.format), signed_version ? ", signed" : "",
                                    l.dimension, l.tiled ? "tiled" : "linear", why.c_str());
                        failed++;
                    }
                }
    }
    std::printf("  %u formats, %u levels compared in %u runs\n", formats, gpu.compared, gpu.runs);
    CHECK(failed == 0);
    CHECK(formats == 45); // the 64 less the 19 Xenia has no host format for
}

} // namespace

int main() {
    std::string error;
    Gpu gpu;
    gpu.compute = vk::Compute::open(error);
    if (!gpu.compute) {
        std::printf("no Vulkan device (%s): nothing to run\n", error.c_str());
        return 0;
    }
    std::printf("on %s\n", gpu.compute->device_name().c_str());
    gpu.memory.resize(MEMORY_BYTES);
    for (uint32_t i = 0; i < MEMORY_BYTES; i += 4) {
        uint32_t v = hash(i * 2654435761u + 7);
        std::memcpy(&gpu.memory[i], &v, 4);
    }
    gpu.guest.write(0, gpu.memory);
    loads_match(gpu);
    return finish("texture_gpu");
}
