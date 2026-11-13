// Exact EDRAM through fragment shader interlock: the pixel shader reads and
// writes EDRAM itself, as Xenia's interlock render target path does it
// (render_target_path_vulkan=fsi: spirv_shader_translator_rb.cc's
// FSI_LoadSampleMask, FSI_LoadEdramOffsets, FSI_DepthStencilTest,
// FSI_ClampAndPackColor, FSI_UnpackColor and the blending functions, and
// CompleteFragmentShaderInMain's color writes; the D3D12 ROV path,
// dxbc_shader_translator_om.cc, computes the same), with the render target
// state of vulkan_command_processor.cc's UpdateSystemConstantValues and
// render_target_cache.cc's GetPSIColorFormatInfo. BSD; see LICENSE-xenia.
//
// Built two ways:
// - LIBRARY: xf_fsi, called by the translated pixel shader inside its
//   interlock (shader_translator.cpp; spirv.h's Module::import takes this
//   module's functions and globals). The main here only keeps xf_fsi in the
//   module.
// - DEPTH_ONLY (with SAMPLE_INTERLOCK or not): a whole fragment shader for the
//   draws without a pixel shader that test or write depth or stencil.
// - DEPTH_ONLY with UNORDERED: the same without the interlock, for the
//   renderer's unordered depth passes (xf_fsi_depth_unordered).
//
// The render targets' state is the draw's uniform block (binding 4), or, with
// the specialization constants set (SPECIALIZED), the formats, write masks,
// blending and depth state baked into the pipeline, so the driver drops the
// branches that do not apply (Xenia's TODO: its interlock path is slow mainly
// because all of it is dynamic).

#version 460
#if defined(DEPTH_ONLY) && !defined(UNORDERED)
#extension GL_ARB_fragment_shader_interlock : require
#endif

#include "edram_fsi.glsl"

#if defined(DEPTH_ONLY) && defined(UNORDERED)
// no interlock: the renderer's unordered depth passes (xf_fsi_depth_unordered)
void main() {
    vec2 dxy = vec2(dFdxCoarse(gl_FragCoord.z), dFdyCoarse(gl_FragCoord.z));
    if (gl_HelperInvocation) return;
    xf_fsi_depth_unordered(gl_FragCoord, dxy, uint(gl_SampleMaskIn[0]), gl_FrontFacing);
}
#elif defined(DEPTH_ONLY)
#ifdef SAMPLE_INTERLOCK
layout(sample_interlock_ordered) in;
#else
layout(pixel_interlock_ordered) in;
#endif
void main() {
    vec2 dxy = vec2(dFdxCoarse(gl_FragCoord.z), dFdyCoarse(gl_FragCoord.z));
    bool alive = !gl_HelperInvocation;
    // (the early rejection: a fragment that fails leaves the interlock at once)
    alive = alive && !xf_fsi_rejects(gl_FragCoord, dxy, uint(gl_SampleMaskIn[0]), gl_FrontFacing, alive);
    beginInvocationInterlockARB();
    xf_fsi(vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), 0u, 0.0, false, gl_FragCoord, dxy, uint(gl_SampleMaskIn[0]),
           gl_FrontFacing, 0xFu, alive);
    endInvocationInterlockARB();
}
#else
// keeps xf_fsi and xf_fsi_rejects in the module; never run
void main() {
    if (xf_fsi_rejects(vec4(0.0), vec2(0.0), 0u, false, false)) return;
    xf_fsi(vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), 0u, 0.0, false, vec4(0.0), vec2(0.0), 0u, false, 0u, false);
}
#endif
