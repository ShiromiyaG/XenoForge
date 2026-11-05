// xftexture: the textures a GPU trace's draws read, decoded as the renderer
// will (texture.h) and written out as PNGs, plus the front buffer at each
// swap (fetch constant 0, as VdSwap sets it). Each texture once, by its
// fetch constant; level 0, its first slice, with the constant's swizzle
// applied, as a shader would see it. The resolves on the way are read
// (resolve.h) and listed. What could not be decoded is listed with the
// reason.
//
//   xftexture <trace.xtr> -o <folder>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "xenoforge/bytes.h"
#include "xenoforge/gpu_trace.h"
#include "xenoforge/resolve.h"
#include "xenoforge/texture.h"
#include "xenoforge/ucode.h"
#include "xenoforge/xenos.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::xenos;

namespace {

const char *host_name(texture::HostFormat f) {
    static const char *names[] = {"Unsupported", "R8", "RG8", "RGBA8", "B5G6R5", "A1R5G5B5", "A4R4G4B4", "RGB10A2",
                                  "R16", "RG16", "RGBA16", "R16F", "RG16F", "RGBA16F", "R32", "RG32", "RGBA32",
                                  "R32F", "RG32F", "RGBA32F", "BC1", "BC2", "BC3", "BC4", "BC5"};
    return names[size_t(f)];
}

// The registers and memory a trace's packets set, Xenia's PM4 front end as
// far as textures and resolves need it (trace_stats.cpp counts the same way).
class Replay : public TraceVisitor {
public:
    Replay(std::filesystem::path out, std::string stem) : out_(std::move(out)), stem_(std::move(stem)) {
        regs_.resize(REGISTER_COUNT);
    }

    void registers(uint32_t first, std::span<const uint32_t> values) override {
        for (size_t i = 0; i < values.size() && first + i < regs_.size(); i++) regs_[first + i] = values[i];
    }
    void memory_read(uint32_t address, std::span<const uint8_t> bytes) override { memory_.write(address, bytes); }
    void memory_write(uint32_t address, std::span<const uint8_t> bytes) override { memory_.write(address, bytes); }
    void packet(uint32_t, std::span<const uint8_t> words) override {
        if (words.size() < 4) return;
        d_.resize(words.size() / 4);
        for (size_t i = 0; i < d_.size(); i++) d_[i] = be32(words.data() + 4 * i);
        execute();
    }

    int decoded = 0, failed = 0, draws = 0, resolves = 0, resolve_failures = 0, swaps = 0;

private:
    void write(uint32_t index, uint32_t value) {
        if (index < regs_.size()) regs_[index] = value;
    }
    uint32_t arg(size_t i) const { return i < d_.size() ? d_[i] : 0; }

    void execute() {
        uint32_t h = d_[0];
        if (h == 0 || h == 0x0BADF00D) return;
        uint32_t type = h >> 30;
        if (type == 0) {
            uint32_t count = ((h >> 16) & 0x3FFF) + 1, base = h & 0x7FFF;
            bool one = (h >> 15) & 1;
            for (uint32_t i = 0; i < count && 1 + i < d_.size(); i++) write(one ? base : base + i, d_[1 + i]);
        } else if (type == 1) {
            write(h & 0x7FF, arg(1));
            write((h >> 11) & 0x7FF, arg(2));
        } else if (type == 3) {
            type3(h);
        }
    }

    void type3(uint32_t h) {
        uint32_t opcode = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
        static const uint32_t bases[] = {reg::ALU_CONSTANTS, reg::FETCH_CONSTANTS, reg::BOOL_CONSTANTS,
                                         reg::LOOP_CONSTANTS, 0x2000};
        // Predicated packets (predicated tiling) run only for the bins
        // selected; a predicated swap never (pm4_command_processor_implement.h).
        if ((h & 1) && (!(bin_select_ & bin_mask_) || opcode == pm4::XE_SWAP)) return;
        switch (opcode) {
        case 0x50: bin_mask_ = uint64_t(arg(1)) << 32 | arg(2); break;   // SET_BIN_MASK
        case 0x51: bin_select_ = uint64_t(arg(1)) << 32 | arg(2); break; // SET_BIN_SELECT
        case 0x60: bin_mask_ = (bin_mask_ & ~0xFFFFFFFFull) | arg(1); break;
        case 0x61: bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | uint64_t(arg(1)) << 32; break;
        case 0x62: bin_select_ = (bin_select_ & ~0xFFFFFFFFull) | arg(1); break;
        case 0x63: bin_select_ = (bin_select_ & 0xFFFFFFFFull) | uint64_t(arg(1)) << 32; break;
        case pm4::SET_CONSTANT: {
            uint32_t kind = (arg(1) >> 16) & 0xFF, index = arg(1) & 0x7FF;
            if (kind > 4) break;
            for (uint32_t i = 0; i + 1 < count && 2 + i < d_.size(); i++) write(bases[kind] + index + i, d_[2 + i]);
            break;
        }
        case pm4::SET_CONSTANT2:
        case pm4::SET_SHADER_CONSTANTS: {
            uint32_t index = arg(1) & 0xFFFF;
            for (uint32_t i = 0; i + 1 < count && 2 + i < d_.size(); i++) write(index + i, d_[2 + i]);
            break;
        }
        case pm4::LOAD_ALU_CONSTANT: {
            uint32_t address = arg(1) & 0x3FFFFFFF, kind = (arg(2) >> 16) & 0xFF, index = arg(2) & 0x7FF;
            uint32_t size = arg(3) & 0xFFF;
            if (kind > 4) break;
            for (uint32_t i = 0; i < size; i++) write(bases[kind] + index + i, memory_.be32(address + 4 * i));
            break;
        }
        case pm4::IM_LOAD: {
            uint32_t shader_type = arg(1) & 3, address = arg(1) & ~3u, size = arg(2) & 0xFFFF;
            std::vector<uint32_t> code(size);
            for (uint32_t i = 0; i < size; i++) code[i] = memory_.be32(address + 4 * i);
            load_shader(shader_type, code);
            break;
        }
        case pm4::IM_LOAD_IMMEDIATE: {
            uint32_t shader_type = arg(1), size = arg(2) & 0xFFFF;
            if (3 + size > d_.size()) break;
            load_shader(shader_type, {d_.data() + 3, size});
            break;
        }
        case pm4::DRAW_INDX: draw(); break;
        case pm4::DRAW_INDX_2: draw(); break;
        case pm4::XE_SWAP:
            swaps++;
            texture_at(0, "swap" + std::to_string(swaps));
            break;
        }
    }

    void load_shader(uint32_t type, std::span<const uint32_t> code) {
        if (type > 1) return;
        (type == 0 ? vertex_ : pixel_) = xf::ucode::analyze(code).texture_slots;
    }

    void draw() {
        draws++;
        uint32_t mode = regs_[reg::RB_MODECONTROL] & 7;
        if (EdramMode(mode) == EdramMode::Copy) {
            resolves++;
            resolve::Info info;
            std::string error;
            if (!resolve::get_info(regs_, memory_, info, error)) {
                resolve_failures++;
                std::printf("resolve %d: %s\n", resolves, error.c_str());
                return;
            }
            std::printf("resolve %d: %s %ux%u at %u,%u (EDRAM tile %u, pitch %u, %ux MSAA) -> %s at 0x%08X+%u,%u "
                        "pitch %u%s%s%s%s\n",
                        resolves,
                        info.depth ? depth_target_format_name(info.source_format)
                                   : color_target_format_name(info.source_format),
                        info.width, info.height, info.x, info.y, info.edram_base, info.surface_pitch, 1u << info.msaa,
                        format_name(info.dest_format), info.dest_base, info.dest_x, info.dest_y, info.dest_pitch,
                        info.copy ? "" : " (no copy)", info.clear_color ? ", clear color" : "",
                        info.clear_depth ? ", clear depth" : "", error.empty() ? "" : (" - " + error).c_str());
            return;
        }
        if (EdramMode(mode) != EdramMode::ColorDepth && EdramMode(mode) != EdramMode::DepthOnly) return;
        std::set<uint32_t> slots = vertex_;
        if (EdramMode(mode) == EdramMode::ColorDepth) slots.insert(pixel_.begin(), pixel_.end());
        for (uint32_t slot : slots) texture_at(slot, "");
    }

    // The texture of fetch constant slot, once per constant.
    void texture_at(uint32_t slot, const std::string &label) {
        std::array<uint32_t, 6> f;
        for (int i = 0; i < 6; i++) f[i] = regs_[reg::FETCH_CONSTANTS + slot * 6 + i];
        if (FetchConstantType(f[0] & 3) != FetchConstantType::Texture) return;
        if (label.empty() && !seen_.insert(f).second) return;
        texture::Info info = texture::parse_fetch_constant(f.data());
        int index = int(seen_.size());
        std::string name = label.empty() ? std::to_string(index) : label;
        std::printf("%-7s %-20s %-7s %s %ux%ux%u%s pitch %u endian %u at 0x%08X", name.c_str(),
                    format_name(info.format), host_name(info.host), dimension_name(info.dimension), info.width,
                    info.height, info.depth, info.tiled ? " tiled" : " linear", info.pitch, info.endian,
                    info.base_address);
        if (info.mip_max) std::printf(" mips %u-%u at 0x%08X%s", info.mip_min, info.mip_max, info.mip_address,
                                      info.packed_mips ? " packed" : "");
        texture::Image image;
        std::string why;
        if (!texture::decode(info, memory_, image, why)) {
            std::printf(": not decoded, %s\n", why.c_str());
            failed++;
            unsupported_[why]++;
            return;
        }
        std::vector<uint8_t> rgba = texture::to_rgba8(image, image.levels[0].empty() ? 1 : 0);
        uint32_t level = image.levels[0].empty() ? 1 : 0;
        uint32_t w = std::max(image.width >> level, 1u), h = std::max(image.height >> level, 1u);
        // the swizzle: per component x y z w 0 1
        for (size_t i = 0; i < rgba.size(); i += 4) {
            uint8_t c[4] = {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
            for (int k = 0; k < 4; k++) {
                uint32_t s = (info.swizzle >> (3 * k)) & 7;
                rgba[i + k] = s < 4 ? c[s] : s == 4 ? 0 : 255;
            }
        }
        char file[160];
        std::snprintf(file, sizeof file, "%s_%s_%08X_%s_%ux%u.png", stem_.c_str(), name.c_str(), info.base_address,
                      format_name(info.format), w, h);
        std::filesystem::path path = out_ / file;
        if (rgba.empty() || !stbi_write_png(path.string().c_str(), int(w), int(h), 4, rgba.data(), int(w * 4))) {
            std::printf(": decoded, PNG not written\n");
            failed++;
            return;
        }
        std::printf(": %zu levels -> %s\n", image.levels.size(), file);
        decoded++;
    }

    std::filesystem::path out_;
    std::string stem_;
    std::vector<uint32_t> regs_;
    GuestMemory memory_;
    std::vector<uint32_t> d_;
    std::set<uint32_t> vertex_, pixel_;
    std::set<std::array<uint32_t, 6>> seen_;
    uint64_t bin_mask_ = 0xFFFFFFFF, bin_select_ = 0xFFFFFFFF;

public:
    std::map<std::string, int> unsupported_;
};

} // namespace

int main(int argc, char **argv) {
    std::filesystem::path trace_path, out;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else trace_path = a;
    }
    if (trace_path.empty() || out.empty()) {
        std::fprintf(stderr, "usage: xftexture <trace.xtr> -o <folder>\n");
        return 2;
    }
    GpuTrace trace;
    std::string error;
    if (!open_gpu_trace(trace_path, trace, error)) {
        std::fprintf(stderr, "%s: %s\n", trace_path.string().c_str(), error.c_str());
        return 1;
    }
    std::filesystem::create_directories(out);
    Replay replay(out, trace_path.stem().string());
    if (!walk_gpu_trace(trace, replay, error)) {
        std::fprintf(stderr, "%s: %s\n", trace_path.string().c_str(), error.c_str());
        return 1;
    }
    std::printf("%s: %d draws, %d resolves (%d not read), %d swaps; %d textures decoded, %d not\n",
                trace_path.filename().string().c_str(), replay.draws, replay.resolves, replay.resolve_failures,
                replay.swaps, replay.decoded, replay.failed);
    for (const auto &[why, n] : replay.unsupported_) std::printf("  %4d %s\n", n, why.c_str());
    return 0;
}
