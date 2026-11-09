// The software reference renderer: the Xenos GPU drawn on the CPU alone,
// deterministic and independent of host GPUs and drivers, as faithful to the
// hardware as its documents allow. A third witness beside the Vulkan
// renderer (renderer.h) and Xenia's pictures: for a trace frame the three
// are compared (tests/reference_test.cpp, tools/xfref.cpp), and where they
// disagree the spec says which is right (docs/renderer.md, "The software
// reference renderer").
//
// It is a gpu::Sink, as the Vulkan renderer is: the command processor gives
// it draws, resolves and swaps with the register file standing. EDRAM is the
// hardware's, 2048 tiles of 80x16 samples of 32 bits, written and read in
// the guest's bits; the shaders run in shader_interpreter.h's machine
// (pixel shaders in quads), textures are read from guest memory by
// texture.h's decode and filtered here, resolves write guest memory as the
// console's do, and a swap's front buffer is the picture, through the
// display's gamma ramp as Xenia's trace dumper outputs it.
//
// What a draw asks for and the reference does not do is counted by reason
// (Report::skipped, approximate) and the frame goes on, never guessed at.

#pragma once

#include "xenoforge/command_processor.h"
#include "xenoforge/gpu_trace.h"
#include "xenoforge/memory.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace xf::reference {

struct Options {
    uint32_t threads = 0; // workers for vertices and rows of pixels; 0: the host's cores
    bool verbose = false; // a line per draw and resolve on stderr
};

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // row-major RGBA8
};

struct Report {
    uint64_t draws = 0, drawn = 0, resolves = 0, swaps = 0, frames = 0;
    uint64_t primitives = 0, quads = 0, vertices = 0; // rasterized primitives, pixel quads shaded, vertices shaded
    uint64_t exports = 0;                             // memory exports done
    std::map<std::string, uint64_t> skipped;          // draws and resolves not done, by reason
    std::map<std::string, uint64_t> approximate;      // done, but not as the hardware would, by reason
    std::map<std::string, uint64_t> used;             // what the frame asked for, counted
};

class Renderer final : public gpu::Sink {
  public:
    Renderer(Memory &memory, gpu::CommandProcessor &processor, const Options &options);
    ~Renderer() override;

    void shader(uint32_t type, uint32_t address, std::span<const uint32_t> code) override;
    void draw(const gpu::RegisterFile &registers, const gpu::Draw &draw) override;
    void resolve(const gpu::RegisterFile &registers, const gpu::Draw &draw) override;
    void swap(const gpu::RegisterFile &registers, const gpu::Swap &swap) override;

    // EDRAM as a trace's snapshot has it (2048 tiles of 5120 bytes)
    void restore_edram(std::span<const uint8_t> bytes);
    // guest memory changed from outside (a trace's memory): decoded textures
    // over it are read again
    void memory_written(uint32_t address, uint32_t bytes) override;

    const Report &report() const;
    std::span<const uint32_t> edram() const;
    // each swap's front buffer, as the display shows it
    std::function<void(uint64_t swap, const Image &frame)> on_frame;

    struct Impl; // src/reference/reference_impl.h

  private:
    std::unique_ptr<Impl> impl_;
};

// A trace played into the reference as render::replay plays it into the
// Vulkan renderer (src/replay.cpp): the last swap's front buffer in frame.
bool replay(const GpuTrace &trace, const Options &options, Image &frame, Report &report, std::string &error);

// The pieces, for the unit tests (tests/reference_units_test.cpp).
namespace pieces {

// xenos.cc's 7e3 (2_10_10_10_FLOAT's colors) and 20e4 (D24FS8's depth)
float float7e3_to_32(uint32_t f10);
uint32_t float32_to_7e3(float f); // clamped to 0-31.875, rounded to nearest even
float float20e4_to_32(uint32_t f24);
uint32_t float32_to_20e4(float f, bool round_to_nearest_even); // clamped to 0-2
// xenos.cc's piecewise linear gamma (8_8_8_8_GAMMA, the PWL textures)
float linear_to_pwl_gamma(float linear); // the gamma code over 255
float pwl_gamma_to_linear(float gamma);
// float16 in the Xenos's extended range (no infinity or NaN: exponent 31 is finite)
uint32_t pack_half2_extended(float x, float y);
void unpack_half2_extended(uint32_t packed, float &x, float &y);
uint16_t float_to_half(float f); // IEEE, rounded to nearest even
float half_to_float(uint16_t h);

// The guest sample positions in 1/16 pixel (R6xx/R7xx's PA_SC_AA_SAMPLE_LOCS
// for 2x and 4x; Xenia's kXenosSamplePositions), y down.
void sample_position(uint32_t msaa, uint32_t sample, int32_t &x16, int32_t &y16);

// The top-left fill rule: whether a point on an edge (its edge function 0)
// is inside, for an edge going from (x0, y0) to (x1, y1) of a triangle
// wound clockwise on screen (y down).
bool top_left(int64_t x0, int64_t y0, int64_t x1, int64_t y1);
// Which samples of a pixel a triangle in 1/256 pixel fixed point covers
// (vertices clockwise or not), by the fill rule.
uint32_t coverage(const int64_t x[3], const int64_t y[3], int32_t px, int32_t py, uint32_t msaa);

// A color target format's guest bits from and to the shader's values
// (edram_fsi.frag's fsi_pack and fsi_unpack), and one sample blended as
// RB_BLENDCONTROL says (blend: its bits & 0x1FFF1FFF) with a destination's
// bits; the bits the write mask keeps (0-15 a component each) kept.
void pack_color(const float c[4], uint32_t format, uint32_t out[2]);
void unpack_color(const uint32_t bits[2], uint32_t format, float out[4]);
void blend(const float source[4], const uint32_t destination[2], uint32_t format, uint32_t blend_control,
           const float constant[4], uint32_t write_mask, uint32_t out[2]);

} // namespace pieces

} // namespace xf::reference
