// A GPU trace in Xenia's format made here: the register snapshot, a pixel
// shader in snappy-compressed memory loaded with IM_LOAD, a vertex shader in
// the packet, a constant, a draw with a texture, a resolve with a clear, a
// swap, a bad packet; then counted. Cut short, it is refused.

#include "check.h"
#include "xenoforge/bytes.h"
#include "xenoforge/trace_stats.h"
#include "xenoforge/xenos.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace xf;

namespace {

struct Writer {
    std::vector<uint8_t> out;
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i)));
    }
    void bytes(const std::vector<uint8_t> &b) { out.insert(out.end(), b.begin(), b.end()); }
    // what the packet read (memory commands) goes between its start and end
    void packet(std::vector<uint32_t> words, const std::vector<uint8_t> &inside = {}) {
        u32(4); // PacketStart
        u32(0x1000);
        u32(uint32_t(words.size()));
        for (uint32_t w : words) {
            uint8_t b[4];
            put_be32(b, w);
            out.insert(out.end(), b, b + 4);
        }
        bytes(inside);
        u32(5); // PacketEnd
    }
};

uint32_t type0(uint32_t reg) { return reg; } // one register
uint32_t type3(uint32_t opcode, uint32_t count) { return 3u << 30 | (count - 1) << 16 | opcode << 8; }

// a pixel shader: alloc of memory exports, then one tfetch from constant 7
std::vector<uint32_t> pixel_shader() {
    uint32_t exec_d0 = 1 | 1u << 12 | 1u << 16, exec_d1 = 2u << 12; // exec_end, address 1, one fetch
    uint32_t alloc_d1 = 3u << 9 | 12u << 12;
    return {0, alloc_d1 | exec_d0 << 16, exec_d0 >> 16 | exec_d1 << 16, 1u | 7u << 20, 0, 1u << 14};
}

std::vector<uint8_t> make_trace() {
    using namespace xenos;
    Writer w;
    w.u32(1); // version
    for (int i = 0; i < 40; i++) w.out.push_back('0' + i % 10);
    w.u32(0x58460001);

    // the register snapshot
    std::vector<uint32_t> regs(REGISTER_COUNT);
    regs[reg::RB_COLOR_INFO] = 5u << 16;   // 16_16_16_16
    regs[reg::RB_DEPTHCONTROL] = 2;        // z enabled
    regs[reg::RB_SURFACE_INFO] = 2u << 16; // 4x MSAA
    uint32_t *tex = &regs[reg::FETCH_CONSTANTS + 7 * 6];
    tex[0] = 2;                       // a texture
    tex[1] = 18 | 0x12345000;         // DXT1 at 0x12345000
    tex[2] = 255 | 127u << 13;        // 256x128
    tex[5] = 1u << 9;                 // 2D
    w.u32(10);
    w.u32(0);
    w.u32(REGISTER_COUNT);
    w.u32(0); // execute_callbacks, padded
    w.u32(0); // not compressed
    w.u32(REGISTER_COUNT * 4);
    for (uint32_t r : regs) w.u32(r);

    // the pixel shader in memory, compressed: one snappy literal
    auto ps = pixel_shader();
    std::vector<uint8_t> raw;
    for (uint32_t d : ps) {
        uint8_t b[4];
        put_be32(b, d);
        raw.insert(raw.end(), b, b + 4);
    }
    std::vector<uint8_t> snappy = {uint8_t(raw.size()), uint8_t((raw.size() - 1) << 2)};
    snappy.insert(snappy.end(), raw.begin(), raw.end());
    Writer read; // a MemoryRead, recorded inside the IM_LOAD that made it
    read.u32(6);
    read.u32(0x00100000);
    read.u32(1); // snappy
    read.u32(uint32_t(snappy.size()));
    read.u32(uint32_t(raw.size()));
    read.bytes(snappy);

    w.u32(0); // PrimaryBufferStart
    w.u32(0x1000);
    w.u32(0);
    w.packet({type0(reg::RB_MODECONTROL), 4});                               // color and depth
    w.packet({type3(pm4::IM_LOAD, 2), 0x00100000 | 1, uint32_t(ps.size())}, read.out); // the pixel shader
    w.packet({type3(pm4::IM_LOAD_IMMEDIATE, 5), 0, 3, 1, 0x2000, 0});        // an empty vertex shader
    w.packet({type3(pm4::SET_CONSTANT, 2), 4u << 16 | 0x104, 0xF});          // RB_COLOR_MASK: target 0
    // the draw inside an indirect buffer: the buffer's own packet ends after it
    w.u32(4); // PacketStart of INDIRECT_BUFFER, two words
    w.u32(0x1000);
    w.u32(2);
    for (uint32_t word : {type3(pm4::INDIRECT_BUFFER, 2), 0x2000u}) {
        uint8_t b[4];
        put_be32(b, word);
        w.out.insert(w.out.end(), b, b + 4);
    }
    w.u32(2); // IndirectBufferStart
    w.u32(0x2000);
    w.u32(0);
    w.packet({type3(pm4::DRAW_INDX_2, 1), 4 | 2u << 6 | 3u << 16}); // a triangle, auto index
    w.u32(3); // IndirectBufferEnd
    w.u32(5); // the indirect buffer packet's end
    w.packet({type0(reg::RB_MODECONTROL), 6});                               // copy
    w.packet({type0(reg::RB_COPY_CONTROL), 1u << 8});                        // color 0, cleared
    w.packet({type0(reg::RB_COPY_DEST_INFO), 6u << 7});                      // to 8_8_8_8
    w.packet({type3(pm4::DRAW_INDX, 2), 0, 8 | 2u << 6 | 3u << 16});          // the resolve's rectangle
    w.packet({type3(pm4::XE_SWAP, 1), 0});
    w.packet({0x0BADF00D});
    w.packet({0x80000000}); // type 2
    w.u32(1);               // PrimaryBufferEnd
    w.u32(9);               // Event: swap
    w.u32(0);
    return w.out;
}

std::filesystem::path write_temp(const std::vector<uint8_t> &data, const char *name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    return path;
}

void counts_a_trace() {
    auto path = write_temp(make_trace(), "xenoforge_trace_test.xtr");
    GpuTrace trace;
    TraceStats s;
    std::string error;
    CHECK(open_gpu_trace(path, trace, error));
    CHECK(trace.title_id == 0x58460001 && trace.commit.size() == 40);
    CHECK(count_gpu_trace(trace, s, error));
    if (!error.empty()) std::printf("  %s\n", error.c_str());
    std::filesystem::remove(path);

    CHECK(s.packets[0] == 4 && s.packets[2] == 1 && s.packets[3] == 6 && s.bad_packets == 1);
    CHECK(s.type3.at(xenos::pm4::IM_LOAD) == 1 && s.type3.at(xenos::pm4::DRAW_INDX) == 1);
    CHECK(!s.type3.count(xenos::pm4::INDIRECT_BUFFER)); // its packet gave way to the ones inside
    CHECK(s.memory_reads == 1 && s.swaps == 1);
    CHECK(s.shaders.size() == 2);
    for (const auto &[hash, shader] : s.shaders)
        if (shader.type == 1) CHECK(shader.summary.memexport && shader.summary.texture_slots.count(7)); // read after the memory came
    CHECK(s.draws == 2 && s.resolves == 1);
    CHECK(s.primitives.at(4) == 1 && s.primitives.at(8) == 1 && s.sources.at(2) == 2);
    CHECK(s.edram_modes.at(4) == 1 && s.edram_modes.at(6) == 1);
    CHECK(s.color_formats.size() == 1 && s.color_formats.at(5) == 1);
    CHECK(s.depth_formats.at(0) == 1 && s.msaa.at(2) == 1);
    CHECK(s.resolve_color_formats.at(6) == 1 && s.resolve_clears == 1 && s.resolve_depth == 0);
    CHECK(s.shaders.size() == 2 && s.draws_without_shader == 0 && s.draws_with_memexport == 1);
    CHECK(s.textures.size() == 1 && s.texture_bindings == 1 && s.missing_texture_constants == 0);
    if (s.textures.size() == 1) {
        auto [base, format, width, height, depth, dimension] = *s.textures.begin();
        CHECK(base == 0x12345000 && format == 18 && width == 256 && height == 128 && depth == 1 && dimension == 1);
    }
}

void refuses_a_cut_trace() {
    auto data = make_trace();
    for (size_t cut : {size_t(20), size_t(60), data.size() - 3}) {
        auto path = write_temp(std::vector<uint8_t>(data.begin(), data.begin() + cut), "xenoforge_trace_cut.xtr");
        GpuTrace trace;
        TraceStats s;
        std::string error;
        CHECK(!open_gpu_trace(path, trace, error) || !count_gpu_trace(trace, s, error));
        std::filesystem::remove(path);
    }
}

} // namespace

int main() {
    counts_a_trace();
    refuses_a_cut_trace();
    return finish("trace");
}
