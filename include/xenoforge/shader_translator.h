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
#include <string>
#include <vector>

namespace xf::shader {

enum class Stage { Vertex, Pixel, Harness };

struct Translation {
    std::vector<uint32_t> spirv;
    // What the shader uses that the translator does not do yet; a shader
    // with any is not translated (spirv stays empty).
    std::vector<std::string> unsupported;
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
struct VertexOptions {
    HostVertex host = HostVertex::Vertex;
    bool equal_spacing = false; // domain types: discrete tessellation
};
Translation translate(const ucode::ParsedShader &shader, Stage stage, const VertexOptions &vertex = {});

} // namespace xf::shader
