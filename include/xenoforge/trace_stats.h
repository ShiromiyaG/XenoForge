// What GPU traces use, counted: the packets, the draws and their primitives,
// the EDRAM modes and render target formats, the resolves, the shaders and
// what their microcode does, the textures and vertex formats. It decides
// what the renderer does first (docs/renderer.md, "counted").

#pragma once

#include "xenoforge/gpu_trace.h"
#include "xenoforge/ucode.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace xf {

using Counts = std::map<uint32_t, uint64_t>;

struct TraceStats {
    uint32_t traces = 0;
    uint64_t bytes = 0;
    uint64_t packets[4] = {}, bad_packets = 0;
    Counts type3; // by opcode
    uint64_t memory_reads = 0, memory_read_bytes = 0, memory_writes = 0, edram_snapshots = 0, swaps = 0;

    uint64_t draws = 0;
    Counts primitives, sources, edram_modes;
    Counts color_formats, depth_formats, msaa; // per draw that writes them
    uint64_t resolves = 0, resolve_clears = 0;
    Counts resolve_color_formats; // destination format of color resolves
    uint64_t resolve_depth = 0;

    struct Shader {
        uint32_t type; // 0 vertex, 1 pixel
        ucode::ShaderSummary summary;
    };
    std::map<uint64_t, Shader> shaders; // by hash of type and microcode
    uint64_t draws_without_shader = 0, draws_with_memexport = 0;

    // a texture is its address, format, size and dimension
    std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> textures;
    uint64_t texture_bindings = 0, missing_texture_constants = 0, missing_vertex_constants = 0;

    void merge(const TraceStats &other);
};

// Walks the trace through a PM4 front end that keeps the registers and the
// memory the trace recorded, counting as it goes.
bool count_gpu_trace(const GpuTrace &trace, TraceStats &stats, std::string &error);

} // namespace xf
