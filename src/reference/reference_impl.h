// The reference renderer's state (reference.h), shared by its files: the
// sink and its resolves and swaps (renderer.cpp), the draw path (draw.cpp).

#pragma once

#include "reference_internal.h"

#include "xenoforge/command_processor.h"
#include "xenoforge/ucode_parse.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

namespace xf::reference {

constexpr uint32_t EDRAM_TILES = 2048, TILE_DWORDS = 80 * 16, EDRAM_DWORDS = EDRAM_TILES * TILE_DWORDS;

// registers xenos.h does not name (Xenia's register_table.inc)
namespace r {
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100, VGT_MIN_VTX_INDX = 0x2101, VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t RB_BLEND_RED = 0x2105;
constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;
constexpr uint32_t RB_BLENDCONTROL[4] = {0x2201, 0x2209, 0x220A, 0x220B};
constexpr uint32_t RB_COLORCONTROL = 0x2202;
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204, PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206;
constexpr uint32_t SQ_CONTEXT_MISC = 0x2181, SQ_INTERPOLATOR_CNTL = 0x2182;
constexpr uint32_t PA_SU_POINT_SIZE = 0x2280, PA_SU_POINT_MINMAX = 0x2281, PA_SU_LINE_CNTL = 0x2282;
constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x2284;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;
constexpr uint32_t PA_CL_GB_VERT_CLIP_ADJ = 0x2303, PA_CL_GB_HORZ_CLIP_ADJ = 0x2305;
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_COLOR_CLEAR_LO = 0x231F;
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380;
constexpr uint32_t PA_CL_UCP_0_X = 0x2388;
constexpr uint32_t DC_LUTA_CONTROL = 0x1930;
constexpr uint32_t COLOR_INFO[4] = {0x2001, 0x2003, 0x2004, 0x2005};
} // namespace r

// Workers that run a job's indices; the caller works too.
class Pool {
  public:
    explicit Pool(uint32_t threads);
    ~Pool();
    void run(uint32_t count, const std::function<void(uint32_t)> &job);
    uint32_t size() const { return uint32_t(workers_.size()) + 1; }

  private:
    void work();
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    const std::function<void(uint32_t)> *job_ = nullptr;
    uint32_t count_ = 0, pending_ = 0;
    uint64_t generation_ = 0;
    std::atomic<uint32_t> next_{0};
    bool stop_ = false;
};

// A shader loaded, and what its microcode writes (by its results' targets).
struct Shader {
    ucode::ParsedShader parsed;
    uint32_t colors = 0; // oC# written somewhere, a bit each
    bool depth = false, point_size = false, vertex_kill = false, exports = false;
};

struct Renderer::Impl {
    Impl(Memory &memory, gpu::CommandProcessor &processor, const Options &options);

    Memory &memory;
    GuestMemory guest;
    gpu::CommandProcessor &processor;
    Options options;
    std::vector<uint32_t> edram;
    Textures textures;
    Counts counts;
    Report report;
    Pool pool;
    std::mutex memory_mutex; // memory exports from the workers
    std::map<uint64_t, std::unique_ptr<Shader>> shaders;
    const Shader *vertex_shader = nullptr, *pixel_shader = nullptr;
    std::function<void(uint64_t, const Image &)> *on_frame = nullptr;

    void skip(const std::string &why) { report.skipped[why]++; }
    void approximate(const std::string &why) { counts.add(why); }
    void use(const std::string &what) { report.used[what]++; }

    void load_shader(uint32_t type, std::span<const uint32_t> code);
    void draw(const uint32_t *regs, const gpu::Draw &draw);
    void resolve(const uint32_t *regs);
    void swap(const uint32_t *regs);
    // a memory export's words, written (from any thread)
    void export_words(uint32_t address, const uint32_t *words, const uint32_t *masks, uint32_t count);
};

// The dword in EDRAM of a pixel's sample (Xenia's ROV and FSI addressing,
// edram_fsi.frag's fsi_offsets and sample_offset at resolution scale 1):
// base in tiles, pitch the surface's in pixels, msaa 0-2, wide 64-bit
// samples, depth with its tiles' halves swapped. The second dword of a
// 64-bit sample is the next one.
uint32_t edram_address(uint32_t base, uint32_t pitch, uint32_t msaa, bool wide, bool depth, uint32_t x, uint32_t y,
                       uint32_t sample);

} // namespace xf::reference
