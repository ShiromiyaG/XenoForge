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
//          constants, laid out as shader_harness.h's Constants), 1 guest
//          memory (storage, words), 2 the system constants (uniform:
//          ndc_scale, ndc_offset, then per fetch slot the image and sampler
//          index, 16 bits each, as uvec4[8]);
//   set 1: 0 2D images, 1 3D images, 2 samplers (unsized arrays).
// A vertex shader writes the position and o0-o15 (locations 0-15); a pixel
// shader reads o0-o15 into r0-r15 and writes oC0-oC3 (locations 0-3).
Translation translate(const ucode::ParsedShader &shader, Stage stage);

} // namespace xf::shader
