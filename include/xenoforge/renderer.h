// The Xenos renderer, first form (plan B3): a GPU trace replayed on Vulkan,
// draw by draw, as the console's GPU would have drawn it. The PM4 packets
// set registers and constants, load shaders and draw; a draw becomes a
// Vulkan draw with the translated shaders (shader_translator.h) into host
// render targets that stand for EDRAM; a resolve copies a render target
// into guest memory (resolve.h) where later draws read it as a texture
// (texture.h); a swap shows texture fetch constant 0, as Xenia does.
//
// What a draw needs and the renderer does not do yet is counted by reason
// in the report, and the draw is skipped, never guessed at.

#pragma once

#include "xenoforge/gpu_trace.h"
#include "xenoforge/vk_device.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xf::render {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // 8 bits per component, rows top to bottom
};

struct Report {
    uint64_t packets = 0, draws = 0, drawn = 0, resolves = 0, swaps = 0;
    uint64_t render_targets = 0, textures = 0, pipelines = 0, shaders = 0;
    std::map<std::string, uint64_t> skipped;    // draws not done, by reason
    std::map<std::string, uint64_t> approximate; // done, with something not exact yet, by what
};

struct Options {
    bool verbose = false; // a line per draw and resolve on stderr
};

// Replays the trace; the image is what the last swap showed (empty without
// a swap). False only when the trace or the device fails, not for draws
// that were skipped.
bool replay(const GpuTrace &trace, vk::Device &device, const Options &options, Image &frame, Report &report,
            std::string &error);

} // namespace xf::render
