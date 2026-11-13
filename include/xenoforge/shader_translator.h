// Xenos shaders, parsed (ucode_parse.h), translated to SPIR-V for Vulkan 1.3.
// Written anew; what each instruction computes follows Xenia's SPIR-V
// translator (spirv_shader_translator*.cc), the hardware's behavior as Xenia
// knows it (docs/renderer.md, "The translated shader").
//
// One body, three wrappers: the vertex shader, the pixel shader, and a
// compute shader that runs cases for the checks (shader_harness.h).

#pragma once

#include "xenoforge/ucode_parse.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xf::shader {

enum class Stage { Vertex, Pixel, Harness };

struct Translation {
    std::vector<uint32_t> spirv;
    // What the shader uses that the translator does not do yet; a shader
    // with any is not translated (spirv stays empty).
    std::vector<std::string> unsupported;
    // What it does as Xenia does, which is not the console's (counted by the
    // renderer at each draw): getBCF's 0
    std::vector<std::string> approximate;
    bool ok() const { return unsupported.empty() && !spirv.empty(); }
};

// Bindings of the vertex and pixel shaders (the harness's are in
// shader_harness.h):
//   set 0: 0 the constants (uniform: 256 float4, bools, loops, fetch
//          constants, laid out as shader_harness.h's Constants) of a
//          vertex shader, 3 those of a pixel shader, 1 guest
//          memory (storage, words), 2 the system constants (uniform:
//          ndc_scale, ndc_offset, then per fetch slot the image and sampler
//          index, 16 bits each, as uvec4[8], then flags (uvec4; x: the alpha
//          test, bit 3 on and bits 0-2 its function; y: the position's
//          formats, bit 0 W not 1/W, bit 1 XY divided by W, bit 2 Z divided
//          by W; z: a bit per color target written as gamma) and values
//          (vec4; x: the alpha test's reference), then the color targets'
//          scales (vec4, one per target): 208 bytes);
//   set 1: 0 2D images, 1 3D images, 2 cube images, 3 samplers (unsized
//          arrays).
// A vertex shader writes the position and o0-o15 (locations 0-15); a pixel
// shader reads o0-o15 into r0-r15 and writes oC0-oC3 (locations 0-3).
//
// How a vertex shader runs on the host, as Xenia's
// Shader::HostVertexShaderType:
// - Vertex: an invocation per guest vertex, its index (gl_VertexIndex) in r0.x.
// - PointListAsTriangleStrip: four invocations per point, a strip of two
//   triangles (gl_VertexIndex: the guest index shifted left by 2, the corner in
//   bits 0-1: bit 1 right, bit 0 bottom), the sprite made as point_list.geom
//   makes it; only corner 0 exports memory.
// - RectangleListAsTriangleStrip: four invocations per rectangle (the corner
//   in gl_VertexIndex's bits 0-1), the indices of its three guest vertices in
//   a uvec3 at input location 0; the guest shader runs for all three, and the
//   strip is the one rectangle_list.geom makes; guest vertex k exports memory
//   in corner k's invocation only.
// - The domain types: a tessellation evaluation shader, the guest shader run
//   at each point the tessellator makes, with the barycentric or UV
//   coordinates and the control point or patch indices (input location 0, a
//   float per control point of the patch) in r0 and r1 as Xenia's D3D12
//   translator puts them (StartVertexOrDomainShader); integer spacing for
//   discrete tessellation, fractional even otherwise, clockwise.
enum class HostVertex : uint8_t {
    Vertex,
    PointListAsTriangleStrip,
    RectangleListAsTriangleStrip,
    TriangleDomainCPIndexed,
    TriangleDomainPatchIndexed,
    QuadDomainCPIndexed,
    QuadDomainPatchIndexed,
};
// User clip planes (PA_CL_CLIP_CNTL's ucp_ena_0-5 while clipping is on), as
// Xenia's shader modifications user_clip_plane_count and _cull: the enabled
// planes packed in the system constants' clip_planes (renderer.cpp's System),
// each one's distance the dot product of the guest's clip-space position
// (after PA_CL_VTE_CNTL's formats, before the host's viewport transform) with
// the plane, written as a clip distance, or a cull distance with
// ucp_cull_only_ena. Points (PointListAsTriangleStrip) by PA_CL_CLIP_CNTL's
// ps_ucp_mode, as Xenia's TODO describes it from AMD's register guides: the
// center's distances for modes 0 and 1 (the sprite kept or dropped whole), the
// center's as cull distances and each corner's as clip distances for mode 2
// (dropped by the center, then clipped where it crosses a plane), each
// corner's for mode 3 (clipped as a polygon). A rectangle's fourth corner has
// the mirrored distances, as its position.
struct VertexOptions {
    HostVertex host = HostVertex::Vertex;
    bool equal_spacing = false;    // domain types: discrete tessellation
    uint8_t clip_planes = 0;       // 0-6
    bool clip_planes_cull = false; // ucp_cull_only_ena
    uint8_t point_ucp_mode = 0;    // ps_ucp_mode, for points
    // Screen extents (EVENT_WRITE_EXT; renderer.cpp, "Screen extents"): each
    // final position bounds the draw's report, in console pixels through the
    // system constants' extent map (members 10 and 11), with atomic maxima
    // into the storage buffer at set 0, binding 7, four words a report at
    // the report's index: 8192 less the left and top edges, the right and
    // bottom ones plus one (zero: nothing yet). A position not in front of
    // the eye (w not above 0, or not a number) takes the whole viewport.
    bool extents = false;
    // With extents: the four maxima taken over the subgroup first and stored
    // by one invocation of it, where the subgroup's invocations all bound the
    // same report (each its own atomics where not). Needs subgroup votes,
    // arithmetic and election in vertex shaders. One setting for the whole
    // run (the renderer's), so not part of the key.
    bool extents_subgroup = false;
    // Xenia's texture_point_snap (off by default there too): point-sampled
    // 2D fetches with normalized coordinates, where the fetch constant
    // filters by point too (texture::integer_scale_bits' bit 26), sample at
    // their texel's center instead of adding the rounding epsilon. One
    // setting for the whole run (the renderer's), so not part of the key.
    bool texture_point_snap = false;
    // a word naming the variant (the renderer's key for the module)
    uint32_t key() const {
        return uint32_t(host) | (equal_spacing ? 1u << 8 : 0u) | uint32_t(clip_planes) << 9 |
               (clip_planes_cull ? 1u << 12 : 0u) | uint32_t(point_ucp_mode & 3) << 13 | (extents ? 1u << 15 : 0u);
    }
};
// How a pixel shader writes depth (Xenia's DepthStencilMode on the host
// render target path). By default oDepth clamped to 0-1, halved for a D24FS8
// target (system flags' w bit 0), which keeps its 0-2 as 0-1. With
// depth_float24 (Xenia's depth_float24_convert_in_pixel_shader, for draws
// into D24FS8 with the depth test on): every fragment's depth, oDepth or the
// rasterized one taken back to 0-1, rounded to 20e4 (1 towards zero, as
// kFloat24Truncating, with the conservative DepthLess when the shader writes
// no depth; 2 to the nearest even, as kFloat24Rounding), then halved.
//
// centroid: the interpolators (a bit each) read at the centroid of the
// pixel's covered samples rather than at its center, as Xenia's
// interpolators_centroid modification (xenos.h's
// GetInterpolatorSamplingPattern: SQ_CONTEXT_MISC's sc_sample_cntl and
// SQ_INTERPOLATOR_CNTL's sampling_pattern under MSAA). Without it, values a
// triangle's edge pixels take are extrapolated past the triangle.
//
// fsi: the interlock render target path (docs/renderer.md, "Exact EDRAM
// through fragment shader interlock"), 1 under pixel interlock, 2 under
// sample interlock. The shader writes no color, depth or sample mask
// outputs: after the alpha test (a demotion) and alpha to coverage (a mask in
// the guest's sample order), it calls edram_library's xf_fsi (edram_fsi.frag
// compiled as LIBRARY, taken in by spirv::Module::import) inside the
// interlock with oC0-oC3, the targets it wrote on its path (D3D9's rule:
// those it did not write keep EDRAM's), oDepth, the rasterized depth's coarse
// derivatives and the coverage; the library tests depth and stencil, blends
// and packs into EDRAM (binding 5) as the uniform block at binding 4 or the
// pipeline's specialization constants say.
struct PixelOptions {
    uint8_t depth_float24 = 0;
    uint16_t centroid = 0;
    uint8_t fsi = 0; // up to 63
    // a point list's: the pixel parameters' z and w are the sprite's
    // coordinates, read from location 17 (the point expansion's), as
    // Xenia's param_gen_point
    bool point_coordinates = false;
    // Xenia's DepthStencilMode::kEarlyHint: the draw's state has no alpha test
    // and no alpha to coverage, so a shader that neither kills, writes depth
    // nor exports memory has its depth and stencil tested and written before
    // it runs (EarlyFragmentTests), and the alpha test, the alpha to coverage
    // and the coverage output are left out. Ignored for a shader that does any
    // of those, with a float24 conversion and on the interlock path.
    bool early_depth_stencil = false;
    // Host render targets: the source of a minimum or maximum blend times its
    // blend factor, which the console applies and the host's blending
    // ignores, where the factor depends on the source and the blend constant
    // alone; per color target as the system constants say (the renderer's
    // host_blend_control). Not on the interlock path, which blends itself.
    bool blend_premultiply = false;
    bool texture_point_snap = false; // VertexOptions::texture_point_snap's; not part of the key
    // A run at a resolution scale: the interpolators can be read at the
    // console pixel's center, where the system flags' y bit 16 says (a draw
    // into a small target). The run's, not part of the key; at 100% the code
    // is left out.
    bool console_centers = false;
    std::span<const uint32_t> edram_library; // with fsi; not part of the key
    uint32_t key() const {
        // (depth_float24 is 0-2; bits 4-7 are free)
        return depth_float24 | uint32_t(early_depth_stencil) << 2 | uint32_t(blend_premultiply) << 3 |
               uint32_t(centroid) << 8 | uint32_t(fsi) << 24 |
               uint32_t(point_coordinates) << 30;
    }
};
Translation translate(const ucode::ParsedShader &shader, Stage stage, const VertexOptions &vertex = {},
                      const PixelOptions &pixel = {});

} // namespace xf::shader
