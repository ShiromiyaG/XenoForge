// A PM4 front end that only counts: registers kept from the trace's
// snapshot and the packets, memory from the trace's reads, shaders taken
// from IM_LOAD. The packet layouts follow Xenia's
// src/xenia/gpu/pm4_command_processor_implement.h (BSD; see LICENSE-xenia).

#include "xenoforge/trace_stats.h"

#include "xenoforge/bytes.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace xf {
namespace {

using namespace xenos;

constexpr uint32_t CHUNK = 1u << 16;
constexpr uint32_t PHYSICAL_MASK = 0x1FFFFFFF;

// The physical memory the trace recorded, in 64 KB chunks, zero elsewhere.
class Memory {
public:
    void write(uint32_t address, std::span<const uint8_t> bytes) {
        for (size_t i = 0; i < bytes.size();) {
            uint32_t a = (address + uint32_t(i)) & PHYSICAL_MASK;
            auto &chunk = chunks_[a / CHUNK];
            if (!chunk) chunk = std::make_unique<uint8_t[]>(CHUNK);
            size_t n = std::min<size_t>(CHUNK - a % CHUNK, bytes.size() - i);
            std::memcpy(chunk.get() + a % CHUNK, bytes.data() + i, n);
            i += n;
        }
    }
    uint32_t be32_at(uint32_t address) const {
        uint8_t b[4];
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t a = (address + i) & PHYSICAL_MASK;
            auto it = chunks_.find(a / CHUNK);
            b[i] = it == chunks_.end() ? 0 : it->second[a % CHUNK];
        }
        return be32(b);
    }

private:
    std::unordered_map<uint32_t, std::unique_ptr<uint8_t[]>> chunks_;
};

uint64_t hash_shader(uint32_t type, std::span<const uint32_t> dwords) {
    uint64_t h = 0xcbf29ce484222325ull ^ type; // FNV-1a
    for (uint32_t d : dwords)
        for (int i = 0; i < 4; i++) h = (h ^ ((d >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    return h;
}

class Counter : public TraceVisitor {
public:
    explicit Counter(TraceStats &s) : s_(s), regs_(REGISTER_COUNT) {}

    void registers(uint32_t first, std::span<const uint32_t> values) override {
        for (size_t i = 0; i < values.size() && first + i < regs_.size(); i++) regs_[first + i] = values[i];
    }
    void memory_read(uint32_t address, std::span<const uint8_t> bytes) override {
        s_.memory_reads++;
        s_.memory_read_bytes += bytes.size();
        memory_.write(address, bytes);
    }
    void memory_write(uint32_t address, std::span<const uint8_t> bytes) override {
        s_.memory_writes++;
        memory_.write(address, bytes);
    }
    void edram_snapshot(std::span<const uint8_t>) override { s_.edram_snapshots++; }

    void packet(uint32_t, std::span<const uint8_t> words) override {
        if (words.size() < 4) return;
        d_.resize(words.size() / 4);
        for (size_t i = 0; i < d_.size(); i++) d_[i] = be32(words.data() + 4 * i);
        execute();
    }

private:
    void write(uint32_t index, uint32_t value) {
        if (index < regs_.size()) regs_[index] = value;
    }
    uint32_t arg(size_t i) const { return i < d_.size() ? d_[i] : 0; }

    void execute() {
        uint32_t h = d_[0];
        if (h == 0 || h == 0x0BADF00D) {
            s_.bad_packets++;
            return;
        }
        uint32_t type = h >> 30;
        s_.packets[type]++;
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
        s_.type3[opcode]++;
        switch (opcode) {
        case pm4::SET_CONSTANT: {
            static const uint32_t bases[] = {reg::ALU_CONSTANTS, reg::FETCH_CONSTANTS, reg::BOOL_CONSTANTS,
                                             reg::LOOP_CONSTANTS, 0x2000};
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
            static const uint32_t bases[] = {reg::ALU_CONSTANTS, reg::FETCH_CONSTANTS, reg::BOOL_CONSTANTS,
                                             reg::LOOP_CONSTANTS, 0x2000};
            uint32_t address = arg(1) & 0x3FFFFFFF, kind = (arg(2) >> 16) & 0xFF, index = arg(2) & 0x7FF;
            uint32_t size = arg(3) & 0xFFF;
            if (kind > 4) break;
            for (uint32_t i = 0; i < size; i++) write(bases[kind] + index + i, memory_.be32_at(address + 4 * i));
            break;
        }
        case pm4::IM_LOAD: {
            uint32_t shader_type = arg(1) & 3, address = arg(1) & ~3u, size = arg(2) & 0xFFFF;
            std::vector<uint32_t> code(size);
            for (uint32_t i = 0; i < size; i++) code[i] = memory_.be32_at(address + 4 * i);
            load_shader(shader_type, code);
            break;
        }
        case pm4::IM_LOAD_IMMEDIATE: {
            uint32_t shader_type = arg(1), size = arg(2) & 0xFFFF;
            if (3 + size > d_.size()) break;
            load_shader(shader_type, {d_.data() + 3, size});
            break;
        }
        case pm4::DRAW_INDX: draw(arg(2)); break; // after the viz query token
        case pm4::DRAW_INDX_2: draw(arg(1)); break;
        case pm4::XE_SWAP: s_.swaps++; break;
        }
    }

    void load_shader(uint32_t type, std::span<const uint32_t> code) {
        if (type > 1) return;
        uint64_t hash = hash_shader(type, code);
        if (!s_.shaders.count(hash)) s_.shaders[hash] = {type, xf::ucode::analyze(code)};
        (type == 0 ? vertex_ : pixel_) = hash;
        (type == 0 ? have_vertex_ : have_pixel_) = true;
    }

    void draw(uint32_t initiator) {
        write(reg::VGT_DRAW_INITIATOR, initiator);
        s_.draws++;
        s_.primitives[initiator & 0x3F]++;
        s_.sources[(initiator >> 6) & 3]++;
        uint32_t mode = regs_[reg::RB_MODECONTROL] & 7;
        s_.edram_modes[mode]++;

        if (EdramMode(mode) == EdramMode::Copy) {
            s_.resolves++;
            uint32_t control = regs_[reg::RB_COPY_CONTROL];
            if ((control >> 8) & 3) s_.resolve_clears++;
            if ((control & 7) == 4) s_.resolve_depth++;
            else s_.resolve_color_formats[(regs_[reg::RB_COPY_DEST_INFO] >> 7) & 0x3F]++;
            return;
        }
        if (EdramMode(mode) != EdramMode::ColorDepth && EdramMode(mode) != EdramMode::DepthOnly) return;

        s_.msaa[(regs_[reg::RB_SURFACE_INFO] >> 16) & 3]++;
        if (regs_[reg::RB_DEPTHCONTROL] & 3) s_.depth_formats[(regs_[reg::RB_DEPTH_INFO] >> 16) & 1]++;
        bool color = EdramMode(mode) == EdramMode::ColorDepth;
        if (color) {
            static const uint32_t infos[] = {reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO,
                                             reg::RB_COLOR3_INFO};
            for (int rt = 0; rt < 4; rt++)
                if ((regs_[reg::RB_COLOR_MASK] >> (4 * rt)) & 0xF) s_.color_formats[(regs_[infos[rt]] >> 16) & 0xF]++;
        }

        if (!have_vertex_ || (color && !have_pixel_)) s_.draws_without_shader++;
        bool memexport = false;
        if (have_vertex_) memexport |= bind(s_.shaders[vertex_].summary);
        if (color && have_pixel_) memexport |= bind(s_.shaders[pixel_].summary);
        if (memexport) s_.draws_with_memexport++;
    }

    // the fetch constants a shader reads, as they stand at this draw
    bool bind(const xf::ucode::ShaderSummary &shader) {
        for (uint32_t slot : shader.texture_slots) {
            const uint32_t *f = &regs_[reg::FETCH_CONSTANTS + slot * 6];
            s_.texture_bindings++;
            if (FetchConstantType(f[0] & 3) != FetchConstantType::Texture) {
                s_.missing_texture_constants++;
                continue;
            }
            uint32_t format = f[1] & 0x3F, dimension = (f[5] >> 9) & 3, base = f[1] & 0xFFFFF000;
            uint32_t w = 0, h = 1, d = 1;
            switch (dimension) {
            case 0: w = (f[2] & 0xFFFFFF) + 1; break;
            case 2: w = (f[2] & 0x7FF) + 1, h = ((f[2] >> 11) & 0x7FF) + 1, d = ((f[2] >> 22) & 0x3FF) + 1; break;
            default: w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1, d = ((f[2] >> 26) & 0x3F) + 1; break;
            }
            s_.textures.insert({base, format, w, h, d, dimension});
        }
        for (uint32_t slot : shader.vertex_slots) {
            const uint32_t *f = &regs_[reg::FETCH_CONSTANTS + slot * 2];
            if (FetchConstantType(f[0] & 3) != FetchConstantType::Vertex) s_.missing_vertex_constants++;
        }
        return shader.memexport;
    }

    TraceStats &s_;
    std::vector<uint32_t> regs_;
    Memory memory_;
    std::vector<uint32_t> d_;
    uint64_t vertex_ = 0, pixel_ = 0;
    bool have_vertex_ = false, have_pixel_ = false;
};

void add(Counts &into, const Counts &from) {
    for (const auto &[k, v] : from) into[k] += v;
}

} // namespace

void TraceStats::merge(const TraceStats &o) {
    traces += o.traces;
    bytes += o.bytes;
    for (int i = 0; i < 4; i++) packets[i] += o.packets[i];
    bad_packets += o.bad_packets;
    add(type3, o.type3);
    memory_reads += o.memory_reads, memory_read_bytes += o.memory_read_bytes, memory_writes += o.memory_writes;
    edram_snapshots += o.edram_snapshots, swaps += o.swaps;
    draws += o.draws;
    add(primitives, o.primitives), add(sources, o.sources), add(edram_modes, o.edram_modes);
    add(color_formats, o.color_formats), add(depth_formats, o.depth_formats), add(msaa, o.msaa);
    resolves += o.resolves, resolve_clears += o.resolve_clears, resolve_depth += o.resolve_depth;
    add(resolve_color_formats, o.resolve_color_formats);
    shaders.insert(o.shaders.begin(), o.shaders.end());
    draws_without_shader += o.draws_without_shader, draws_with_memexport += o.draws_with_memexport;
    textures.insert(o.textures.begin(), o.textures.end());
    texture_bindings += o.texture_bindings, missing_texture_constants += o.missing_texture_constants;
    missing_vertex_constants += o.missing_vertex_constants;
}

bool count_gpu_trace(const GpuTrace &trace, TraceStats &stats, std::string &error) {
    stats.traces++;
    stats.bytes += trace.data.size();
    Counter counter(stats);
    return walk_gpu_trace(trace, counter, error);
}

} // namespace xf
