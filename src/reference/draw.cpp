/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The reference renderer's draws (reference.h), stage by stage. The source
// of each, the spec first (docs/renderer.md, "The software reference
// renderer"):
//
// - Indices: the guest's, byte-swapped as the draw says, plus VGT_INDX_OFFSET,
//   cut to 24 bits and clamped to VGT_MIN_VTX_INDX and VGT_MAX_VTX_INDX; the
//   reset index (PA_SU_SC_MODE_CNTL's multi_prim_ib_ena) cuts strips, fans,
//   line strips and loops, quad strips and polygons (R6xx's VGT_* registers;
//   Xenia's RemapAndConvertVertexIndices and its primitive processor's list).
// - Primitives: lists, strips, fans, line loops closed by their first index,
//   quads as two triangles split along their 1-3 diagonal, quad strips and polygons as OpenGL's (the
//   triangle strip and fan of their vertices: Mesa's a2xx and r600 give these
//   types to GL_QUAD_STRIP and GL_POLYGON), rectangle lists with the fourth
//   vertex mirrored across the longest edge (Xenia's geometry shader, the
//   spec being silent), points as squares of PA_SU_POINT_SIZE or oPts.x
//   (clamped by PA_SU_POINT_MINMAX) pixels, lines as parallelograms of
//   PA_SU_LINE_CNTL's width.
// - The vertex shader in shader_interpreter.h's machine, its position through
//   PA_CL_VTE_CNTL's formats (W given as 1/W unless vtx_w0_fmt, XY and Z
//   times W with vtx_xy_fmt and vtx_z_fmt), as Xenia's
//   CompleteVertexOrTessEvalShaderInMain.
// - Clipping in homogeneous space (PA_CL_CLIP_CNTL): near and far by
//   dx_clip_space_def (0 <= z <= w, else -w <= z <= w), the guard band
//   (PA_CL_GB_*_CLIP_ADJ: x and y clipped at that many times w; 1 is the
//   viewport, R6xx's register reference), the user clip planes (ucp_ena_0-5,
//   clip or cull only), off with clip_disable (w > 0 kept, z saturated).
// - The viewport (PA_CL_VPORT_*, by PA_CL_VTE_CNTL's enables), the window
//   offset where PA_SU_SC_MODE_CNTL's vtx_window_offset_enable, Direct3D's
//   pixel centers by PA_SU_VTX_CNTL's pix_center; positions snapped to
//   PA_SU_VTX_CNTL's quant_mode (1/16 to 1 pixel) by its round_mode.
// - Faces and culling (PA_SU_SC_MODE_CNTL's face, cull_front, cull_back),
//   polygon offset (the front, back or para scale in 1/16 pixel units times
//   the depth's larger slope, plus the offset in depth units: Direct3D 9's,
//   Xenia's FSI), for polygonal types alone as Xenia's IsPrimitivePolygonal.
// - Rasterization at the samples' positions (R6xx/R7xx's
//   PA_SC_AA_SAMPLE_LOCS, Xenia's kXenosSamplePositions) by the top-left
//   rule, within the window scissor (plus the window offset unless
//   window_offset_disable), the screen scissor and the surface.
// - Interpolation perspective-correct (PERSP_CORR_DIS: linear), flat by
//   SQ_INTERPOLATOR_CNTL's param_shade from the provoking vertex
//   (PROVOKING_VTX_LAST), at the centroid where SQ_CONTEXT_MISC's
//   sc_sample_cntl and SQ_INTERPOLATOR_CNTL's sampling_pattern say (the
//   first covered sample when the pixel is partly covered), the pixel
//   parameters (param_gen) as Xenia's StartFragmentShaderInMain writes them.
// - The pixel shader in quads of 2x2 pixels, helpers included, its texture
//   fetches through sampler.cpp; kills, the alpha test on oC0 where written,
//   alpha to coverage (Xenia's FSI_AlphaToMask thresholds and dither).
// - Depth, stencil and colors as Xenia's ROV/FSI output merger
//   (spirv_shader_translator_rb.cc, dxbc_shader_translator_om.cc; ported in
//   pieces.cpp and below): the depth at each sample from the primitive's
//   plane, 24-bit unorm or 20e4 rounded to the nearest even; the stencil's
//   two faces; blending and packing in the format's precision; write masks.
// - Memory exports (eA, eM0-4) as Xenia's ExportToMemory
//   (shader_translator.cpp's export_to_memory, from
//   dxbc_shader_translator_memexport.cc).
//
// Threads: the vertices in chunks; the pixels by bands of rows, each band
// drawing every primitive in order, so every sample sees its primitives in
// the order the draw gives them.

#include "reference_impl.h"

#include "xenoforge/shader_harness.h"
#include "xenoforge/ucode.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace xf::reference {
namespace {

using namespace xenos::reg;

int32_t sign_extend(uint32_t value, uint32_t bits) { return int32_t(value << (32 - bits)) >> (32 - bits); }
float as_float(uint32_t bits) { return std::bit_cast<float>(bits); }

// a vertex as its shader left it
struct ShadedVertex {
    float pos[4] = {};
    float attr[16][4] = {};
    float point_size = -1.0f;
    bool killed = false;
};

// a vertex being clipped: the position and what is interpolated
struct ClipVertex {
    float pos[4];
    float attr[16][4];
    float pc[2];
};

// a vertex on the screen
struct ScreenVertex {
    double x, y;    // pixels, snapped
    int64_t fx, fy; // 1/256 pixel
    float z;
    float inv_w;
    float attr[16][4];
    float pc[2];
};

// a triangle to rasterize
struct Triangle {
    ScreenVertex v[3];
    bool front = true;
    uint8_t kind = 0;      // 0 polygon, 1 line, 2 point, 3 rectangle
    uint8_t provoking = 0; // for flat interpolators
    int64_t area = 0;      // twice the area in 1/256 pixel squared, positive clockwise on screen
    double dzdx = 0, dzdy = 0;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // pixels it may touch, inclusive-exclusive
};

struct TargetState {
    bool on = false;
    uint32_t format = 0, mask = 0, base = 0, blend = 0;
    float exp_bias = 1.0f;
    bool wide = false;
};

struct DrawState {
    uint32_t pitch = 0, msaa = 0, samples = 1;
    TargetState rt[4];
    bool depth_stencil = false, stencil = false, depth_write = false, float24 = false;
    uint32_t depth_func = 7; // less 1, equal 2, greater 4
    uint32_t depth_base = 0;
    uint32_t stencil_word[2] = {}, stencil_ops[2] = {}; // front, back
    float poly_scale[2] = {}, poly_offset[2] = {};      // front, back (para: both)
    float blend_constant[4] = {};
    bool alpha_test = false;
    uint32_t alpha_func = 7;
    float alpha_ref = 0;
    bool alpha_to_mask = false;
    uint32_t alpha_to_mask_offsets = 0;
    uint32_t centroid = 0, flat = 0;
    bool flat_last = false;
    bool param_gen = false, point_coordinates = false;
    uint32_t param_gen_register = 0;
    bool perspective = true;
    bool pixel_writes_depth = false;
    int32_t scissor[4] = {}; // left, top, right, bottom
};

void swap_endian_128(uint32_t w[4], uint32_t endian) {
    auto swap = [](uint32_t v, uint32_t e) {
        switch (e) {
        case 1: return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
        case 2: return __builtin_bswap32(v);
        case 3: return v >> 16 | v << 16;
        default: return v;
        }
    };
    if (endian == 4) std::swap(w[0], w[1]), std::swap(w[2], w[3]);
    if (endian == 5) std::swap(w[0], w[3]), std::swap(w[1], w[2]);
    if (endian == 4 || endian == 5) endian = 2;
    for (int i = 0; i < 4; i++) w[i] = swap(w[i], endian);
}

// The shaders' surroundings: textures from the draw's table, exports into
// guest memory.
class Env final : public shader::Environment {
  public:
    Env(Renderer::Impl &impl, const std::shared_ptr<const Texture> *textures, const std::string *reasons)
        : impl_(impl), textures_(textures), reasons_(reasons) {}

    bool texture(const shader::TextureFetch &f, float out[4], std::string &why) override {
        uint32_t slot = f.instruction->operands[1].index & 31;
        const Texture *t = textures_[slot].get();
        bool needs = f.instruction->opcode == ucode::FetchOpcode::TextureFetch ||
                     f.instruction->opcode == ucode::FetchOpcode::GetTextureComputedLod ||
                     f.instruction->opcode == ucode::FetchOpcode::GetTextureWeights;
        if (!t && needs) {
            // not decoded: zeros, counted (the draw goes on)
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            impl_.counts.add("texture fetch of a texture not decoded (zeros): " + reasons_[slot]);
            return true;
        }
        bool ok = sample(t, f, out, why, impl_.counts);
        if (debug_here)
            std::fprintf(stderr, "    fetch %u at (%g %g %g): %g %g %g %g (%ux%u, format %u, %zu levels)\n", slot,
                         f.coordinates[0], f.coordinates[1], f.coordinates[2], out[0], out[1], out[2], out[3],
                         t ? t->info.width : 0, t ? t->info.height : 0, t ? t->info.format : 0,
                         t ? t->levels.size() : 0);
        return ok;
    }

    // shader_translator.cpp's export_to_memory
    void memory_export(const float address[4], const float data[5][4], uint32_t written) override {
        uint32_t ea[4];
        std::memcpy(ea, address, 16);
        if (ea[0] >> 30 != 1 || ea[1] >> 23 != 0x96 || ea[2] >> 23 != 0x96 || ea[3] >> 23 != 0x96) return;
        const uint32_t info = ea[2];
        const bool swap_red_blue = (info >> 19) & 1, is_signed = (info >> 16) & 1, is_norm = !((info >> 17) & 1);
        const uint32_t format = (info >> 8) & 63, endian = info & 7;
        struct Case {
            uint32_t size_log2;
            uint8_t widths[4];
            uint8_t kind; // 0 packed integers, 1 16-bit floats (pairs), 2 32-bit floats
            uint8_t pairs;
        } c;
        switch (format) {
        case 2:
        case 8:
        case 9: c = {0, {8, 0, 0, 0}, 0, 0}; break;
        case 3: c = {1, {5, 5, 5, 1}, 0, 0}; break;
        case 4: c = {1, {5, 6, 5, 0}, 0, 0}; break;
        case 5: c = {1, {5, 5, 6, 0}, 0, 0}; break;
        case 6:
        case 14:
        case 50: c = {2, {8, 8, 8, 8}, 0, 0}; break;
        case 7:
        case 54: c = {2, {10, 10, 10, 2}, 0, 0}; break;
        case 10: c = {1, {8, 8, 0, 0}, 0, 0}; break;
        case 15: c = {1, {4, 4, 4, 4}, 0, 0}; break;
        case 16:
        case 55: c = {2, {11, 11, 10, 0}, 0, 0}; break;
        case 17:
        case 56: c = {2, {10, 11, 11, 0}, 0, 0}; break;
        case 24: c = {1, {16, 0, 0, 0}, 0, 0}; break;
        case 25: c = {2, {16, 16, 0, 0}, 0, 0}; break;
        case 26: c = {3, {16, 16, 16, 16}, 0, 0}; break;
        case 30: c = {1, {}, 1, 0}; break;
        case 31: c = {2, {}, 1, 1}; break;
        case 32: c = {3, {}, 1, 2}; break;
        case 36: c = {2, {}, 2, 0}; break;
        case 37: c = {3, {}, 2, 0}; break;
        case 38: c = {4, {}, 2, 0}; break;
        default: impl_.counts.add("memory export format " + std::to_string(format) + " (nothing stored)"); return;
        }
        const uint32_t index = ea[1] & 0x7FFFFF, count = ea[3] & 0x7FFFFF;
        const int32_t left = std::clamp(int32_t(count) - int32_t(index), 0, 5);
        const uint32_t to_store = written & ((1u << left) - 1);
        const uint32_t base = (ea[0] << 2) + (index << c.size_log2);
        for (uint32_t k = 0; k < 5; k++) {
            if (!(to_store & (1u << k))) continue;
            float v[4] = {data[k][0], data[k][1], data[k][2], data[k][3]};
            if (swap_red_blue) std::swap(v[0], v[2]);
            uint32_t w[4] = {};
            if (c.kind == 2) {
                for (int i = 0; i < 4; i++) w[i] = std::bit_cast<uint32_t>(v[i]);
            } else if (c.kind == 1) {
                w[0] = pieces::pack_half2_extended(v[0], c.pairs ? v[1] : 0.0f);
                if (c.pairs == 2) w[1] = pieces::pack_half2_extended(v[2], v[3]);
            } else {
                // as Direct3D converts (NaN as 0, normalized clamped and
                // scaled, integers clamped, rounded half away from zero)
                uint32_t ints[4];
                for (int i = 0; i < 4; i++) {
                    uint32_t width = c.widths[i] ? c.widths[i] : 16;
                    float smax = float((1u << (width - 1)) - 1), smin = -1.0f - smax;
                    float sscale = width > 2 ? smax : 1.0f, umax = float((1ull << width) - 1);
                    float x = std::isnan(v[i]) ? 0.0f : v[i];
                    if (is_signed) {
                        float s = is_norm ? std::clamp(x, -1.0f, 1.0f) * sscale : std::clamp(x, smin, smax);
                        ints[i] = uint32_t(int32_t(s + (s < 0.0f ? -0.5f : 0.5f)));
                    } else {
                        float u = is_norm ? std::clamp(x, 0.0f, 1.0f) * umax : std::clamp(x, 0.0f, umax);
                        ints[i] = uint32_t(u + 0.5f);
                    }
                }
                if (format == 26) {
                    w[0] = (ints[0] & 0xFFFF) | ints[1] << 16;
                    w[1] = (ints[2] & 0xFFFF) | ints[3] << 16;
                } else {
                    uint32_t word = 0;
                    for (uint32_t i = 0, offset = 0; i < 4 && c.widths[i]; offset += c.widths[i], i++) {
                        uint32_t mask = c.widths[i] == 32 ? ~0u : (1u << c.widths[i]) - 1;
                        word |= (ints[i] & mask) << offset;
                    }
                    w[0] = word;
                }
            }
            swap_endian_128(w, endian);
            if (c.size_log2 == 0) {
                uint32_t at = base + k, shift = 8 * (at & 3);
                uint32_t word = w[0] << shift, mask = 0xFFu << shift;
                impl_.export_words(at & ~3u, &word, &mask, 1);
            } else if (c.size_log2 == 1) {
                uint32_t at = (base >> 1) + k, shift = 16 * (at & 1);
                uint32_t word = w[0] << shift, mask = 0xFFFFu << shift;
                impl_.export_words((at >> 1) << 2, &word, &mask, 1);
            } else {
                uint32_t words = 1u << (c.size_log2 - 2);
                uint32_t at = (base >> 2) + words * k;
                const uint32_t masks[4] = {~0u, ~0u, ~0u, ~0u};
                impl_.export_words(at << 2, w, masks, words);
            }
            written_.push_back({(base + (k << c.size_log2)) & ~3u, 16});
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> written_;
    bool debug_here = false;

  private:
    Renderer::Impl &impl_;
    const std::shared_ptr<const Texture> *textures_;
    const std::string *reasons_;
};

// Sutherland-Hodgman in homogeneous space against the planes whose
// distances `distance` gives (inside at or above 0)
template <typename F> void clip_polygon(std::vector<ClipVertex> &poly, uint32_t planes, F distance) {
    std::vector<ClipVertex> out;
    for (uint32_t p = 0; p < planes && !poly.empty(); p++) {
        out.clear();
        size_t n = poly.size();
        for (size_t i = 0; i < n; i++) {
            const ClipVertex &a = poly[i], &b = poly[(i + 1) % n];
            float da = distance(p, a), db = distance(p, b);
            bool ia = da >= 0.0f, ib = db >= 0.0f;
            if (ia) out.push_back(a);
            if (ia != ib) {
                float t = da / (da - db);
                ClipVertex v;
                for (int c = 0; c < 4; c++) v.pos[c] = a.pos[c] + (b.pos[c] - a.pos[c]) * t;
                for (int k = 0; k < 16; k++)
                    for (int c = 0; c < 4; c++) v.attr[k][c] = a.attr[k][c] + (b.attr[k][c] - a.attr[k][c]) * t;
                for (int c = 0; c < 2; c++) v.pc[c] = a.pc[c] + (b.pc[c] - a.pc[c]) * t;
                out.push_back(v);
            }
        }
        std::swap(poly, out);
    }
}

double snap(double v, uint32_t quant, uint32_t round) {
    double steps = std::ldexp(1.0, 4 - int(std::min(quant, 4u)));
    double s = v * steps;
    double r;
    switch (round) {
    case 0: r = std::trunc(s); break;
    case 1: r = std::floor(s + 0.5); break;
    case 2: r = std::nearbyint(s); break; // to even (the host's default rounding)
    default: {
        double f = std::floor(s);
        r = s - f == 0.5 ? (std::fmod(f, 2.0) != 0.0 ? f : f + 1.0) : std::floor(s + 0.5);
        break;
    }
    }
    return r / steps;
}

} // namespace

void Renderer::Impl::draw(const uint32_t *regs, const gpu::Draw &d) {
    report.draws++;
    const uint32_t mode = regs[RB_MODECONTROL] & 7;
    if (mode != 4 && mode != 5) return skip("EDRAM mode " + std::to_string(mode));
    if (!d.count) return;
    const bool color = mode == 4;
    const Shader *vs = vertex_shader;
    const Shader *ps = color ? pixel_shader : nullptr;
    const bool writes_targets = (color && (regs[RB_COLOR_MASK] & 0xFFFF)) || (regs[RB_DEPTHCONTROL] & 3);
    const bool any_exports = (vs && vs->exports) || (ps && ps->exports);
    if (!writes_targets && !any_exports) return;
    if (!vs) return skip("no vertex shader");
    if (d.major_mode_explicit && (regs[r::VGT_OUTPUT_PATH_CNTL] & 3) == 1) return skip("tessellation");
    const uint32_t primitive = d.primitive;
    switch (primitive) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
    case 12:
    case 13:
    case 14:
    case 15: break;
    default: return skip(std::string("primitive ") + xenos::primitive_name(primitive));
    }
    const bool polygonal = (primitive >= 4 && primitive <= 7) || (primitive >= 13 && primitive <= 15);
    const bool points = primitive == 1, rectangles = primitive == 8;
    const bool lines = primitive == 2 || primitive == 3 || primitive == 12;
    const uint32_t surface = regs[RB_SURFACE_INFO], pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
    if (!pitch) return; // Xenia draws nothing then, exports neither
    if (msaa == 3) return skip("MSAA mode 3");

    // a draw that rasterizes nothing (no target, or the vertex shader only
    // exports: SQ_PROGRAM_CNTL's vs_export_mode multipass) runs its vertex
    // shader for its exports, each index once
    bool export_only = !writes_targets || ((regs[SQ_PROGRAM_CNTL] >> 24) & 7) == 7;
    if (export_only && !vs->exports) return;
    if (export_only && ps && ps->exports)
        approximate("pixel shader memory export in a draw that rasterizes nothing (not done)");

    // ---- the render targets' state (Xenia's UpdateSystemConstantValues for its ROV/FSI) ----
    DrawState st;
    st.pitch = pitch, st.msaa = msaa, st.samples = 1u << msaa;
    uint32_t width = 8192, height = 8192;
    auto view_rows = [&](uint32_t base, bool wide) {
        uint32_t samples_x = msaa >= 2 ? 2 : 1, samples_y = msaa >= 1 ? 2 : 1;
        uint32_t pitch_tiles = std::max(1u, (pitch * samples_x + 79) / 80) << (wide ? 1 : 0);
        uint32_t rows = std::max(1u, (EDRAM_TILES - std::min(base, EDRAM_TILES - 1)) / pitch_tiles);
        return std::clamp(rows * (16 / samples_y), 1u, 8192u);
    };
    const uint32_t color_mask = regs[RB_COLOR_MASK];
    bool any_target = false;
    for (uint32_t i = 0; i < 4 && ps && !export_only; i++) {
        uint32_t m = (color_mask >> (4 * i)) & 0xF;
        if (!m || !(ps->colors & (1u << i))) continue;
        uint32_t info = regs[r::COLOR_INFO[i]], format = (info >> 16) & 0xF;
        if (format == 8 || format == 9 || format == 11 || format == 13)
            return skip(std::string("color target format ") + xenos::color_target_format_name(format));
        TargetState &t = st.rt[i];
        t.on = true, t.format = format, t.mask = m, t.base = (info & 0xFFF) % EDRAM_TILES;
        t.blend = regs[r::RB_BLENDCONTROL[i]] & 0x1FFF1FFF;
        t.exp_bias = std::ldexp(1.0f, sign_extend((info >> 20) & 0x3F, 6));
        t.wide = format == 5 || format == 7 || format == 15;
        width = std::min(width, std::clamp(pitch, 1u, 8192u));
        height = std::min(height, view_rows(t.base, t.wide));
        any_target = true;
        if (format == 1) use("draws into 8_8_8_8_GAMMA color targets");
    }
    uint32_t dc = export_only ? 0 : regs[RB_DEPTHCONTROL];
    if (dc & 3) {
        uint32_t info = regs[RB_DEPTH_INFO];
        st.depth_base = (info & 0xFFF) % EDRAM_TILES;
        st.float24 = (info >> 16) & 1;
        width = std::min(width, std::clamp(pitch, 1u, 8192u));
        height = std::min(height, view_rows(st.depth_base, false));
        any_target = true;
        // draw_util.cc's GetNormalizedDepthControl: a depth test that neither
        // writes nor rejects is off
        if ((dc & 2) && !(dc & 4) && ((dc >> 4) & 7) == 7) dc &= ~2u;
        bool depth_stencil = (dc & 3) != 0;
        // off where a color target over the same base writes the bits depth
        // and stencil use (ColorOverlapsDepthStencil)
        for (uint32_t i = 0; i < 4 && depth_stencil; i++) {
            const TargetState &t = st.rt[i];
            if (!t.on || t.base != st.depth_base) continue;
            uint32_t keep[2];
            pieces::keep_masks(t.format, t.mask, keep);
            uint32_t written_bits = t.wide ? ~(keep[0] & keep[1]) : ~keep[0];
            uint32_t used = (dc & 2 ? 0xFFFFFF00u : 0u) | (dc & 1 ? 0xFFu : 0u);
            if (written_bits & used) depth_stencil = false;
        }
        st.depth_stencil = depth_stencil;
        st.depth_func = dc & 2 ? (dc >> 4) & 7 : 7;
        st.depth_write = (dc & 2) && (dc & 4);
        st.stencil = dc & 1;
        uint32_t stencil = regs[r::RB_STENCILREFMASK], stencil_bf = regs[r::RB_STENCILREFMASK_BF];
        st.stencil_word[0] = st.stencil_word[1] = stencil & 0xFFFFFF;
        st.stencil_ops[0] = st.stencil_ops[1] = (dc >> 8) & 0xFFF;
        if (polygonal && (dc >> 7) & 1)
            st.stencil_word[1] = stencil_bf & 0xFFFFFF, st.stencil_ops[1] = (dc >> 20) & 0xFFF;
    }
    if (!any_target && !export_only) {
        if (!vs->exports && !(ps && ps->exports)) return;
        export_only = vs->exports;
        if (!export_only) return;
    }
    {
        const float *offsets = reinterpret_cast<const float *>(&regs[r::PA_SU_POLY_OFFSET_FRONT_SCALE]);
        uint32_t sc_mode = regs[r::PA_SU_SC_MODE_CNTL];
        if (polygonal) {
            if ((sc_mode >> 11) & 1) st.poly_scale[0] = offsets[0], st.poly_offset[0] = offsets[1];
            if ((sc_mode >> 12) & 1) st.poly_scale[1] = offsets[2], st.poly_offset[1] = offsets[3];
        } else if ((sc_mode >> 13) & 1) {
            st.poly_scale[0] = st.poly_scale[1] = offsets[0];
            st.poly_offset[0] = st.poly_offset[1] = offsets[1];
        }
        // the slope's factor in 1/16 pixel units (Xenia's)
        st.poly_scale[0] *= 1.0f / 16.0f, st.poly_scale[1] *= 1.0f / 16.0f;
        std::memcpy(st.blend_constant, &regs[r::RB_BLEND_RED], 16);
        uint32_t cc = regs[r::RB_COLORCONTROL];
        st.alpha_test = (cc >> 3) & 1, st.alpha_func = cc & 7, st.alpha_ref = as_float(regs[r::RB_ALPHA_REF]);
        st.alpha_to_mask = (cc >> 4) & 1, st.alpha_to_mask_offsets = cc >> 24;
        if (st.alpha_to_mask) use("alpha to coverage");
        if (ps && msaa) {
            uint32_t sample_control = (regs[r::SQ_CONTEXT_MISC] >> 2) & 3;
            if (sample_control == 0)
                st.centroid = 0xFFFF;
            else if (sample_control == 2)
                st.centroid = ~(regs[r::SQ_INTERPOLATOR_CNTL] >> 16) & 0xFFFF;
        }
        // SQ_INTERPOLATOR_CNTL's param_shade: no document gives its
        // polarity (a2xx's register database names it alone; Direct3D 9
        // games set it with Gouraud shading), and Xenia ignores it: every
        // interpolator smooth, as Xenia's; counted
        st.flat = 0;
        if (ps && (regs[r::SQ_INTERPOLATOR_CNTL] & 0xFFFF)) {
            char what[96];
            std::snprintf(what, sizeof(what), "SQ_INTERPOLATOR_CNTL param_shade 0x%04X (smooth, as Xenia)",
                          regs[r::SQ_INTERPOLATOR_CNTL] & 0xFFFF);
            use(what);
        }
        st.flat_last = (sc_mode >> 19) & 1;
        st.perspective = !((sc_mode >> 20) & 1);
        if (!st.perspective) use("draws without perspective correction (PERSP_CORR_DIS)");
        uint32_t position = (regs[r::SQ_CONTEXT_MISC] >> 8) & 0xFF;
        st.param_gen = ps && ((regs[SQ_PROGRAM_CNTL] >> 18) & 1) && position < 16;
        st.param_gen_register = position;
        st.point_coordinates = st.param_gen && points;
        st.pixel_writes_depth = ps && ps->depth;
        uint32_t polymode = (sc_mode >> 3) & 3;
        if (polygonal && polymode != 0) approximate("polygon fill modes (filled)");
    }
    // the scissor (draw_util.cc's GetScissor, the window offset added unless
    // window_offset_disable), within the targets
    {
        uint32_t tl = regs[r::PA_SC_WINDOW_SCISSOR_TL], br = regs[r::PA_SC_WINDOW_SCISSOR_BR];
        int32_t tl_x = tl & 0x3FFF, tl_y = (tl >> 16) & 0x3FFF, br_x = br & 0x3FFF, br_y = (br >> 16) & 0x3FFF;
        if (!(tl >> 31)) {
            uint32_t window = regs[r::PA_SC_WINDOW_OFFSET];
            int32_t wx = sign_extend(window & 0x7FFF, 15), wy = sign_extend((window >> 16) & 0x7FFF, 15);
            tl_x += wx, tl_y += wy, br_x += wx, br_y += wy;
        }
        uint32_t stl = regs[r::PA_SC_SCREEN_SCISSOR_TL], sbr = regs[r::PA_SC_SCREEN_SCISSOR_BR];
        tl_x = std::max(tl_x, sign_extend(stl & 0x7FFF, 15));
        tl_y = std::max(tl_y, sign_extend((stl >> 16) & 0x7FFF, 15));
        br_x = std::min(br_x, sign_extend(sbr & 0x7FFF, 15));
        br_y = std::min(br_y, sign_extend((sbr >> 16) & 0x7FFF, 15));
        tl_x = std::max(tl_x, 0), tl_y = std::max(tl_y, 0);
        br_x = std::min(br_x, int32_t(width)), br_y = std::min(br_y, int32_t(height));
        st.scissor[0] = tl_x, st.scissor[1] = tl_y, st.scissor[2] = br_x, st.scissor[3] = br_y;
    }
    if (!export_only && (st.scissor[2] <= st.scissor[0] || st.scissor[3] <= st.scissor[1])) {
        if (!vs->exports) return;
        export_only = true;
    }

    // ---- indices ----
    const bool strip =
        primitive == 3 || primitive == 5 || primitive == 6 || primitive == 12 || primitive == 14 || primitive == 15;
    const bool restart = strip && (regs[r::PA_SU_SC_MODE_CNTL] >> 21) & 1;
    const uint32_t offset = regs[r::VGT_INDX_OFFSET], reset_index = regs[r::VGT_MULTI_PRIM_IB_RESET_INDX];
    const uint32_t min_index = regs[r::VGT_MIN_VTX_INDX] & 0xFFFFFF, max_index = regs[r::VGT_MAX_VTX_INDX] & 0xFFFFFF;
    auto remap = [&](uint32_t index) { return std::min(max_index, std::max(min_index, (index + offset) & 0xFFFFFF)); };
    std::vector<uint32_t> indices;
    indices.reserve(d.count);
    const uint32_t count = d.count;
    if (d.indexed && !d.immediate.empty()) {
        use("draws with immediate indices");
        for (uint32_t i = 0; i < count; i++) {
            uint32_t word = i / (d.index32 ? 1 : 2) < d.immediate.size() ? d.immediate[d.index32 ? i : i / 2] : 0;
            uint32_t index = d.index32 ? word : (i & 1 ? word >> 16 : word & 0xFFFF);
            if (restart && index == (d.index32 ? reset_index : reset_index & 0xFFFF))
                indices.push_back(UINT32_MAX);
            else
                indices.push_back(remap(index));
        }
    } else if (d.indexed) {
        const uint32_t size = d.index32 ? 4 : 2;
        std::vector<uint8_t> raw(size_t(count) * size);
        guest.read(d.index_base, raw);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *b = raw.data() + size_t(i) * size;
            uint32_t v = d.index32 ? uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24
                                   : uint32_t(b[0]) | uint32_t(b[1]) << 8;
            uint32_t index = d.index32 ? gpu::gpu_swap(v, d.index_endian)
                                       : (d.index_endian == gpu::Endian::In16 ? ((v >> 8) | (v << 8)) & 0xFFFF : v);
            if (!d.index32) index &= 0xFFFF;
            if (restart && index == (d.index32 ? reset_index : reset_index & 0xFFFF))
                indices.push_back(UINT32_MAX);
            else
                indices.push_back(remap(index));
        }
    } else {
        for (uint32_t i = 0; i < count; i++) indices.push_back(remap(i));
    }

    // ---- constants and textures ----
    harness::Constants constants[2];
    for (int stage = 0; stage < 2; stage++) {
        harness::Constants &c = constants[stage];
        std::memcpy(c.float_constants, &regs[ALU_CONSTANTS + stage * 1024], 4096);
        std::memcpy(c.bool_constants, &regs[BOOL_CONSTANTS], sizeof(c.bool_constants));
        std::memcpy(c.loop_constants, &regs[LOOP_CONSTANTS], sizeof(c.loop_constants));
        std::memcpy(c.fetch_constants, &regs[FETCH_CONSTANTS], sizeof(c.fetch_constants));
    }
    std::shared_ptr<const Texture> textures_table[32];
    std::string reasons[32];
    {
        std::vector<const Shader *> stages{vs};
        if (ps && !export_only) stages.push_back(ps);
        for (const Shader *s : stages)
            for (const ucode::ControlFlow &cf : s->parsed.control_flow) {
                const auto *e = std::get_if<ucode::Exec>(&cf.instruction);
                if (!e) continue;
                for (const ucode::ExecSlot &slot : e->slots) {
                    const auto *t = std::get_if<ucode::TextureFetchInstruction>(&slot.instruction);
                    if (!t) continue;
                    uint32_t k = t->operands[1].index & 31;
                    if (textures_table[k] || !reasons[k].empty()) continue;
                    const uint32_t *fetch = &regs[FETCH_CONSTANTS + k * 6];
                    if ((fetch[0] & 3) != 2) {
                        reasons[k] = "a texture slot without a texture constant";
                        continue;
                    }
                    textures_table[k] = textures.get(fetch, reasons[k], counts);
                    if (!textures_table[k] && reasons[k].empty()) reasons[k] = "not decoded";
                }
            }
    }
    const std::span<const uint8_t> memory_span(guest.data(), GuestMemory::SIZE);

    // ---- the vertex shader, each vertex once ----
    std::vector<uint32_t> unique;
    unique.reserve(indices.size());
    for (uint32_t i : indices)
        if (i != UINT32_MAX) unique.push_back(i);
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    std::vector<ShadedVertex> shaded(unique.size());
    const uint32_t chunk = 64, chunks = uint32_t((unique.size() + chunk - 1) / chunk);
    std::vector<std::string> failures(chunks);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> exported(chunks);
    const uint32_t vte = regs[r::PA_CL_VTE_CNTL];
    {
        pool.run(chunks, [&](uint32_t c) {
            const uint32_t slot = c;
            Env env(*this, textures_table, reasons);
            for (size_t i = size_t(c) * chunk; i < std::min(unique.size(), size_t(c + 1) * chunk); i++) {
                shader::Invocation inv(vs->parsed, constants[0], memory_span, &env, false);
                inv.registers()[0][0] = float(unique[i]);
                if (inv.run() != shader::Invocation::Status::Done) {
                    if (failures[slot].empty()) failures[slot] = inv.unsupported();
                    shaded[i].killed = true;
                    continue;
                }
                const harness::CaseOutput &o = inv.output();
                ShadedVertex &v = shaded[i];
                // PA_CL_VTE_CNTL's formats: W as 1/W unless vtx_w0_fmt, XY and Z times W
                float w = (vte >> 10) & 1 ? o.position[3] : 1.0f / o.position[3];
                v.pos[0] = (vte >> 8) & 1 ? o.position[0] * w : o.position[0];
                v.pos[1] = (vte >> 8) & 1 ? o.position[1] * w : o.position[1];
                v.pos[2] = (vte >> 9) & 1 ? o.position[2] * w : o.position[2];
                v.pos[3] = w;
                std::memcpy(v.attr, o.interpolators, sizeof(v.attr));
                v.point_size = vs->point_size ? o.point_size[0] : -1.0f;
                v.killed = vs->vertex_kill && (std::bit_cast<uint32_t>(o.point_size[2]) & ~(1u << 31));
            }
            for (auto &x : env.written_) exported[slot].push_back(x);
        });
        report.vertices += unique.size();
    }
    for (const std::string &f : failures)
        if (!f.empty()) {
            skip("vertex shader: " + f);
            return;
        }
    auto invalidate_exports = [&] {
        for (auto &list : exported)
            for (auto [a, n] : list) textures.invalidate(a, n);
    };
    if (export_only) {
        invalidate_exports();
        return;
    }
    report.drawn++;
    auto vertex_of = [&](uint32_t index) -> const ShadedVertex & {
        return shaded[std::lower_bound(unique.begin(), unique.end(), index) - unique.begin()];
    };
    // XF_REF_TRACE_DRAW=n: that draw's shaders, indices and its vertices'
    // clip-space positions, on stderr
    static const char *trace_draw = std::getenv("XF_REF_TRACE_DRAW");
    if (trace_draw && report.draws == std::strtoull(trace_draw, nullptr, 10)) {
        std::fprintf(stderr, "  trace draw %llu: the vertex shader\n%s", (unsigned long long)report.draws,
                     ucode::disassemble(vs->parsed).c_str());
        if (ps)
            std::fprintf(stderr, "  trace draw %llu: the pixel shader\n%s", (unsigned long long)report.draws,
                         ucode::disassemble(ps->parsed).c_str());
        for (int k = 0; k < 32; k++) {
            const uint32_t *f = &regs[FETCH_CONSTANTS + k * 6];
            if ((f[0] & 3) == 2)
                std::fprintf(stderr, "    texture fetch %d: %08X %08X %08X %08X %08X %08X\n", k, f[0], f[1], f[2], f[3],
                             f[4], f[5]);
        }
        for (int stage = 0; stage < 2; stage++)
            for (int k = 0; k < 256; k++) {
                const float *c = constants[stage].float_constants[k];
                if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f)
                    std::fprintf(stderr, "    %s c%d: %.9g %.9g %.9g %.9g\n", stage ? "pixel" : "vertex", k, c[0],
                                 c[1], c[2], c[3]);
            }
        std::fprintf(stderr, "  trace draw %llu: %s, indices", (unsigned long long)report.draws,
                     xenos::primitive_name(primitive));
        for (uint32_t i : indices) std::fprintf(stderr, " %d", int(i));
        std::fprintf(stderr, "\n");
        for (size_t i = 0; i < unique.size(); i++)
            std::fprintf(stderr, "    vertex %u: %g %g %g %g%s\n", unique[i], shaded[i].pos[0], shaded[i].pos[1],
                         shaded[i].pos[2], shaded[i].pos[3], shaded[i].killed ? " (killed)" : "");
    }

    // ---- primitives ----
    // each primitive as the vertices it is made of (3 for triangles and
    // rectangles, 2 for lines, 1 for points), its provoking vertex first or last
    struct Prim {
        uint32_t v[3];
        uint8_t n;
    };
    std::vector<Prim> prims;
    {
        std::vector<std::vector<uint32_t>> runs(1);
        for (uint32_t i : indices) {
            if (i == UINT32_MAX)
                runs.emplace_back();
            else
                runs.back().push_back(i);
        }
        for (const std::vector<uint32_t> &run : runs) {
            size_t n = run.size();
            switch (primitive) {
            case 1:
                for (size_t i = 0; i < n; i++) prims.push_back({{run[i], 0, 0}, 1});
                break;
            case 2:
                for (size_t i = 0; i + 1 < n; i += 2) prims.push_back({{run[i], run[i + 1], 0}, 2});
                break;
            case 3:
            case 12:
                for (size_t i = 0; i + 1 < n; i++) prims.push_back({{run[i], run[i + 1], 0}, 2});
                if (primitive == 12 && n >= 2) prims.push_back({{run[n - 1], run[0], 0}, 2});
                break;
            case 4:
            case 8:
                for (size_t i = 0; i + 2 < n; i += 3) prims.push_back({{run[i], run[i + 1], run[i + 2]}, 3});
                break;
            case 5:
            case 15:
                if (primitive == 15 && n < 3) break;
                for (size_t i = 1; i + 1 < n; i++) prims.push_back({{run[i], run[i + 1], run[0]}, 3});
                break;
            case 6:
            case 14: {
                size_t m = primitive == 14 ? (n >= 4 ? n & ~size_t(1) : 0) : n;
                for (size_t i = 0; i + 2 < m; i++) {
                    if (i & 1)
                        prims.push_back({{run[i + 1], run[i], run[i + 2]}, 3});
                    else
                        prims.push_back({{run[i], run[i + 1], run[i + 2]}, 3});
                }
                break;
            }
            case 13:
                // split along the 1-3 diagonal, as Xenia's quad list
                // geometry shaders (a strip of vertices 0, 1, 3, 2)
                for (size_t i = 0; i + 3 < n; i += 4) {
                    prims.push_back({{run[i], run[i + 1], run[i + 3]}, 3});
                    prims.push_back({{run[i + 1], run[i + 2], run[i + 3]}, 3});
                }
                break;
            }
        }
    }

    // ---- clipping, the viewport, setup ----
    const uint32_t clip = regs[r::PA_CL_CLIP_CNTL], sc_mode = regs[r::PA_SU_SC_MODE_CNTL];
    const uint32_t vtx = regs[r::PA_SU_VTX_CNTL];
    const bool clip_disabled = (clip >> 16) & 1, dx_clip = (clip >> 19) & 1;
    const uint32_t ucp = clip_disabled ? 0u : clip & 0x3F;
    const bool ucp_cull_only = (clip >> 17) & 1;
    float planes[6][4];
    uint32_t plane_count = 0;
    for (uint32_t p = 0; p < 6; p++)
        if (ucp & (1u << p)) std::memcpy(planes[plane_count++], &regs[r::PA_CL_UCP_0_X + p * 4], 16);
    if (ucp) use(ucp_cull_only ? "draws with user clip planes, cull only" : "draws with user clip planes");
    const float *vport = reinterpret_cast<const float *>(&regs[r::PA_CL_VPORT_XSCALE]);
    const float scale[3] = {vte & 1 ? vport[0] : 1.0f, vte & 4 ? vport[2] : 1.0f, vte & 16 ? vport[4] : 1.0f};
    float add[3] = {vte & 2 ? vport[1] : 0.0f, vte & 8 ? vport[3] : 0.0f, vte & 32 ? vport[5] : 0.0f};
    if ((sc_mode >> 16) & 1) {
        uint32_t window = regs[r::PA_SC_WINDOW_OFFSET];
        add[0] += float(sign_extend(window & 0x7FFF, 15));
        add[1] += float(sign_extend((window >> 16) & 0x7FFF, 15));
    }
    // Direct3D's pixel centers at integers: the screen's half pixel
    if (!(vtx & 1)) add[0] += 0.5f, add[1] += 0.5f;
    const uint32_t quant = (vtx >> 3) & 7, round_mode = (vtx >> 1) & 3;
    // the guard band, in multiples of w (1: the viewport)
    auto band = [&](uint32_t reg_index) {
        float v = as_float(regs[reg_index]);
        return std::isfinite(v) && v >= 1.0f ? std::min(v, 1.0e6f) : 1.0f;
    };
    const float gb_x = band(r::PA_CL_GB_HORZ_CLIP_ADJ), gb_y = band(r::PA_CL_GB_VERT_CLIP_ADJ);
    if (!clip_disabled && (gb_x > 1.0f || gb_y > 1.0f)) use("draws clipped at a guard band past the viewport");

    const bool front_ccw = !((sc_mode >> 2) & 1);
    const bool cull_front = sc_mode & 1, cull_back = (sc_mode >> 1) & 1;
    std::vector<Triangle> triangles;
    triangles.reserve(prims.size() * 2);
    auto to_screen = [&](const ClipVertex &c, ScreenVertex &s) -> bool {
        float w = c.pos[3];
        if (!(w > 0.0f) && clip_disabled) return false;
        double x = double(c.pos[0] / w) * scale[0] + add[0];
        double y = double(c.pos[1] / w) * scale[1] + add[1];
        float z = (c.pos[2] / w) * scale[2] + add[2];
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        x = std::clamp(snap(x, quant, round_mode), -1.0e6, 1.0e6);
        y = std::clamp(snap(y, quant, round_mode), -1.0e6, 1.0e6);
        s.fx = int64_t(std::llround(x * 256.0)), s.fy = int64_t(std::llround(y * 256.0));
        s.x = double(s.fx) / 256.0, s.y = double(s.fy) / 256.0;
        s.z = z;
        s.inv_w = 1.0f / w;
        std::memcpy(s.attr, c.attr, sizeof(s.attr));
        s.pc[0] = c.pc[0], s.pc[1] = c.pc[1];
        return true;
    };
    auto setup = [&](Triangle &t) -> bool {
        int64_t area =
            (t.v[1].fx - t.v[0].fx) * (t.v[2].fy - t.v[0].fy) - (t.v[2].fx - t.v[0].fx) * (t.v[1].fy - t.v[0].fy);
        if (!area) return false;
        t.area = area;
        // the depth's plane
        double x1 = t.v[1].x - t.v[0].x, y1 = t.v[1].y - t.v[0].y, x2 = t.v[2].x - t.v[0].x, y2 = t.v[2].y - t.v[0].y;
        double z1 = double(t.v[1].z) - t.v[0].z, z2 = double(t.v[2].z) - t.v[0].z;
        double det = x1 * y2 - x2 * y1;
        t.dzdx = (z1 * y2 - z2 * y1) / det;
        t.dzdy = (x1 * z2 - x2 * z1) / det;
        double lo_x = std::min({t.v[0].x, t.v[1].x, t.v[2].x}), hi_x = std::max({t.v[0].x, t.v[1].x, t.v[2].x});
        double lo_y = std::min({t.v[0].y, t.v[1].y, t.v[2].y}), hi_y = std::max({t.v[0].y, t.v[1].y, t.v[2].y});
        t.x0 = int32_t(std::max(std::floor(lo_x) - 1.0, double(st.scissor[0])));
        t.y0 = int32_t(std::max(std::floor(lo_y) - 1.0, double(st.scissor[1])));
        t.x1 = int32_t(std::min(std::ceil(hi_x) + 1.0, double(st.scissor[2])));
        t.y1 = int32_t(std::min(std::ceil(hi_y) + 1.0, double(st.scissor[3])));
        return t.x0 < t.x1 && t.y0 < t.y1;
    };
    const uint32_t minmax = regs[r::PA_SU_POINT_MINMAX], point_size = regs[r::PA_SU_POINT_SIZE];
    const float point_min = float(minmax & 0xFFFF) * (2.0f / 16.0f), point_max = float(minmax >> 16) * (2.0f / 16.0f);
    const float line_width = float(regs[r::PA_SU_LINE_CNTL] & 0xFFFF) / 8.0f;
    if (lines) approximate("lines rasterized as parallelograms (Vulkan's non-strict lines)");
    const bool vertex_kill_or = (clip >> 21) & 1;
    uint64_t culled = 0;
    for (const Prim &p : prims) {
        const ShadedVertex *sv[3];
        uint32_t killed = 0;
        for (uint32_t k = 0; k < p.n; k++) {
            sv[k] = &vertex_of(p.v[k]);
            killed += sv[k]->killed;
        }
        if (vs->vertex_kill && (vertex_kill_or ? killed > 0 : killed == p.n)) continue;
        // the clip-space polygon
        std::vector<ClipVertex> poly;
        auto put = [&](const float *pos, const float (*attr)[4], float s0, float s1) {
            ClipVertex c;
            std::memcpy(c.pos, pos, 16);
            std::memcpy(c.attr, attr, sizeof(c.attr));
            c.pc[0] = s0, c.pc[1] = s1;
            poly.push_back(c);
        };
        uint8_t provoking = 0;
        if (p.n == 3) {
            if (rectangles) {
                // the fourth corner: the vertex opposite the longest edge
                // (the diagonal) mirrored across it, position and
                // interpolators alike, as Xenia's geometry shader
                auto edge = [&](int a, int b) {
                    float dx = sv[b]->pos[0] - sv[a]->pos[0], dy = sv[b]->pos[1] - sv[a]->pos[1];
                    return dx * dx + dy * dy;
                };
                float e12 = edge(1, 2), e20 = edge(2, 0), e01 = edge(0, 1);
                int a = (e12 > e20 && e12 > e01) ? 0 : (e20 > e01 ? 1 : 2);
                int b = (a + 1) % 3, c = (a + 2) % 3;
                ClipVertex d4;
                for (int k = 0; k < 4; k++) d4.pos[k] = -sv[a]->pos[k] + sv[b]->pos[k] + sv[c]->pos[k];
                for (int i = 0; i < 16; i++)
                    for (int k = 0; k < 4; k++)
                        d4.attr[i][k] = -sv[a]->attr[i][k] + sv[b]->attr[i][k] + sv[c]->attr[i][k];
                d4.pc[0] = d4.pc[1] = 0.0f;
                // a, b, d, c around the rectangle
                put(sv[a]->pos, sv[a]->attr, 0, 0);
                put(sv[b]->pos, sv[b]->attr, 0, 0);
                poly.push_back(d4);
                put(sv[c]->pos, sv[c]->attr, 0, 0);
            } else {
                for (int k = 0; k < 3; k++) put(sv[k]->pos, sv[k]->attr, 0, 0);
                provoking = st.flat_last ? 2 : 0;
            }
        } else if (p.n == 2) {
            for (int k = 0; k < 2; k++) put(sv[k]->pos, sv[k]->attr, 0, 0);
            provoking = st.flat_last ? 1 : 0;
        } else {
            put(sv[0]->pos, sv[0]->attr, 0, 0);
        }
        // user clip planes, cull only: all vertices outside one plane
        if (plane_count && ucp_cull_only) {
            bool out = false;
            for (uint32_t pl = 0; pl < plane_count && !out; pl++) {
                bool all = true;
                for (const ClipVertex &v : poly) {
                    float dist = v.pos[0] * planes[pl][0] + v.pos[1] * planes[pl][1] + v.pos[2] * planes[pl][2] +
                                 v.pos[3] * planes[pl][3];
                    all &= dist < 0.0f;
                }
                out = all;
            }
            if (out) continue;
        }
        if (p.n == 1) {
            // a point: dropped when its center is clipped; else a square on the screen
            const ClipVertex &c = poly[0];
            float w = c.pos[3];
            if (!(w > 0.0f) || std::isnan(c.pos[0]) || std::isnan(c.pos[1])) continue;
            if (!clip_disabled) {
                bool inside = c.pos[2] <= w && (dx_clip ? c.pos[2] >= 0.0f : c.pos[2] >= -w);
                for (uint32_t pl = 0; pl < plane_count && !ucp_cull_only; pl++)
                    inside &= c.pos[0] * planes[pl][0] + c.pos[1] * planes[pl][1] + c.pos[2] * planes[pl][2] +
                                  c.pos[3] * planes[pl][3] >=
                              0.0f;
                if (!inside) continue;
            }
            float dx, dy;
            if (sv[0]->point_size >= 0.0f || vs->point_size) {
                float size = sv[0]->point_size;
                size = std::bit_cast<float>(
                    std::min(std::bit_cast<int32_t>(point_max),
                             std::max(std::bit_cast<int32_t>(point_min), std::bit_cast<int32_t>(size))));
                dx = dy = size;
            } else {
                dx = float(point_size >> 16) * (2.0f / 16.0f), dy = float(point_size & 0xFFFF) * (2.0f / 16.0f);
            }
            if (!(dx > 0.0f && dy > 0.0f)) continue;
            ScreenVertex center;
            if (!to_screen(c, center)) continue;
            ScreenVertex corner[4];
            for (int k = 0; k < 4; k++) {
                corner[k] = center;
                double x = center.x + ((k >> 1) ? 0.5 : -0.5) * dx, y = center.y + ((k & 1) ? 0.5 : -0.5) * dy;
                x = snap(x, quant, round_mode), y = snap(y, quant, round_mode);
                corner[k].fx = std::llround(x * 256.0), corner[k].fy = std::llround(y * 256.0);
                corner[k].x = double(corner[k].fx) / 256.0, corner[k].y = double(corner[k].fy) / 256.0;
                corner[k].pc[0] = float(k >> 1), corner[k].pc[1] = float(k & 1);
            }
            // top-left, top-right, bottom-right; top-left, bottom-right, bottom-left
            const int order[2][3] = {{0, 2, 3}, {0, 3, 1}};
            for (auto &o : order) {
                Triangle t;
                for (int k = 0; k < 3; k++) t.v[k] = corner[o[k]];
                t.kind = 2;
                if (setup(t)) triangles.push_back(t);
            }
            continue;
        }
        // clip
        std::vector<ClipVertex> clipped = poly;
        if (clip_disabled) {
            clip_polygon(clipped, 1, [](uint32_t, const ClipVertex &v) { return v.pos[3] - 1.0e-20f; });
        } else {
            clip_polygon(clipped, 7 + (ucp_cull_only ? 0 : plane_count), [&](uint32_t pl, const ClipVertex &v) {
                const float *q = v.pos;
                switch (pl) {
                case 0: return dx_clip ? q[2] : q[2] + q[3];
                case 1: return q[3] - q[2];
                case 2: return gb_x * q[3] - q[0];
                case 3: return gb_x * q[3] + q[0];
                case 4: return gb_y * q[3] - q[1];
                case 5: return gb_y * q[3] + q[1];
                case 6: return q[3] - 1.0e-20f;
                default: {
                    const float *pp = planes[pl - 7];
                    return q[0] * pp[0] + q[1] * pp[1] + q[2] * pp[2] + q[3] * pp[3];
                }
                }
            });
        }
        if (clipped.size() < 2) continue;
        std::vector<ScreenVertex> screen(clipped.size());
        bool ok = true;
        for (size_t k = 0; k < clipped.size() && ok; k++) ok = to_screen(clipped[k], screen[k]);
        if (!ok) continue;
        if (p.n == 2) {
            // a line: a parallelogram the line's width across its minor axis
            if (screen.size() < 2) continue;
            ScreenVertex a = screen.front(), b = screen.back();
            double dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
            bool x_major = dx >= dy;
            ScreenVertex q[4] = {a, a, b, b};
            double h = line_width * 0.5;
            for (int k = 0; k < 4; k++) {
                double sgn = (k == 0 || k == 3) ? -h : h;
                double x = q[k].x + (x_major ? 0.0 : sgn), y = q[k].y + (x_major ? sgn : 0.0);
                q[k].fx = std::llround(x * 256.0), q[k].fy = std::llround(y * 256.0);
                q[k].x = double(q[k].fx) / 256.0, q[k].y = double(q[k].fy) / 256.0;
            }
            const int order[2][3] = {{0, 1, 2}, {0, 2, 3}};
            for (auto &o : order) {
                Triangle t;
                for (int k = 0; k < 3; k++) t.v[k] = q[o[k]];
                t.kind = 1;
                t.provoking = provoking ? 2 : 0;
                if (setup(t)) triangles.push_back(t);
            }
            continue;
        }
        // a polygon: its fan
        for (size_t k = 1; k + 1 < screen.size(); k++) {
            Triangle t;
            t.v[0] = screen[0], t.v[1] = screen[k], t.v[2] = screen[k + 1];
            t.kind = rectangles ? 3 : 0;
            int64_t area =
                (t.v[1].fx - t.v[0].fx) * (t.v[2].fy - t.v[0].fy) - (t.v[2].fx - t.v[0].fx) * (t.v[1].fy - t.v[0].fy);
            if (!area) continue;
            if (polygonal) {
                // area < 0: counterclockwise on the screen
                t.front = front_ccw ? area < 0 : area > 0;
                if ((t.front && cull_front) || (!t.front && cull_back)) {
                    culled++;
                    continue;
                }
            }
            // the provoking vertex: the original's, where the fan keeps it
            t.provoking = provoking == 0 ? 0 : 2;
            if (setup(t)) triangles.push_back(t);
        }
    }
    report.primitives += triangles.size();
    if (triangles.empty()) {
        invalidate_exports();
        return;
    }

    // ---- rasterization, the pixel shader, the output merger ----
    const uint32_t samples = st.samples;
    // XF_REF_TRACE_PIXEL=x,y: that pixel's quad's inputs and texture
    // fetches, and the pixel's writes to color targets, on stderr
    static const char *trace = std::getenv("XF_REF_TRACE_PIXEL");
    static const int32_t debug_x = trace ? std::atoi(trace) : -1;
    static const int32_t debug_y = trace && std::strchr(trace, ',') ? std::atoi(std::strchr(trace, ',') + 1) : -1;
    int32_t sample_x16[4], sample_y16[4];
    for (uint32_t s = 0; s < 4; s++) pieces::sample_position(msaa, s, sample_x16[s], sample_y16[s]);
    const int32_t top = st.scissor[1] & ~1, bottom = st.scissor[3];
    const int32_t band_rows = 32;
    const uint32_t bands = uint32_t((bottom - top + band_rows - 1) / band_rows);
    std::atomic<uint64_t> quads{0};
    std::vector<std::string> pixel_failures(bands);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> pixel_exports(bands);

    pool.run(bands, [&](uint32_t band_index) {
        const int32_t band_top = top + int32_t(band_index) * band_rows;
        const int32_t band_bottom = std::min(bottom, band_top + band_rows);
        Env env(*this, textures_table, reasons);
        uint64_t local_quads = 0;
        for (const Triangle &t : triangles) {
            if (t.y1 <= band_top || t.y0 >= band_bottom) continue;
            const int32_t qy0 = std::max(t.y0, band_top) & ~1, qy1 = std::min(t.y1, band_bottom);
            const int32_t qx0 = t.x0 & ~1, qx1 = t.x1;
            // clockwise vertex order for the edge functions
            int order[3] = {0, 1, 2};
            if (t.area < 0) std::swap(order[1], order[2]);
            const ScreenVertex *V[3] = {&t.v[order[0]], &t.v[order[1]], &t.v[order[2]]};
            bool top_left[3];
            for (int e = 0; e < 3; e++) {
                const ScreenVertex &a = *V[e], &b = *V[(e + 1) % 3];
                top_left[e] = pieces::top_left(a.fx, a.fy, b.fx, b.fy);
            }
            auto edge = [&](int e, int64_t sx, int64_t sy) {
                const ScreenVertex &a = *V[e], &b = *V[(e + 1) % 3];
                return (b.fx - a.fx) * (sy - a.fy) - (b.fy - a.fy) * (sx - a.fx);
            };
            const double area = double(std::abs(t.area));
            // barycentrics (of t.v[0..2]) at a point in pixels
            auto barycentric = [&](double px, double py, double b[3]) {
                double sx = px * 256.0, sy = py * 256.0;
                double w[3];
                for (int e = 0; e < 3; e++) {
                    const ScreenVertex &a = *V[e], &bb = *V[(e + 1) % 3];
                    w[e] = (double(bb.fx) - double(a.fx)) * (sy - double(a.fy)) -
                           (double(bb.fy) - double(a.fy)) * (sx - double(a.fx));
                }
                // edge e is opposite vertex (e + 2) % 3 of the clockwise order
                double ordered[3];
                for (int e = 0; e < 3; e++) ordered[(e + 2) % 3] = w[e] / area;
                for (int k = 0; k < 3; k++) b[order[k]] = ordered[k];
            };
            const float slope = float(std::max(std::abs(t.dzdx), std::abs(t.dzdy)));
            const int face = t.front ? 0 : 1;
            const float depth_bias = st.poly_scale[face] * slope + st.poly_offset[face];
            for (int32_t qy = qy0; qy < qy1; qy += 2)
                for (int32_t qx = qx0; qx < qx1; qx += 2) {
                    uint32_t cover[4] = {};
                    bool any = false;
                    for (uint32_t lane = 0; lane < 4; lane++) {
                        int32_t px = qx + int32_t(lane & 1), py = qy + int32_t(lane >> 1);
                        if (px < st.scissor[0] || px >= st.scissor[2] || py < band_top || py >= band_bottom ||
                            py < st.scissor[1])
                            continue;
                        for (uint32_t s = 0; s < samples; s++) {
                            int64_t sx = int64_t(px) * 256 + 128 + sample_x16[s] * 16;
                            int64_t sy = int64_t(py) * 256 + 128 + sample_y16[s] * 16;
                            bool inside = true;
                            for (int e = 0; e < 3 && inside; e++) {
                                int64_t w = edge(e, sx, sy);
                                inside = w > 0 || (w == 0 && top_left[e]);
                            }
                            if (inside) cover[lane] |= 1u << s;
                        }
                        any |= cover[lane] != 0;
                    }
                    if (!any) continue;
                    local_quads++;
                    env.debug_here = trace && qx == (debug_x & ~1) && qy == (debug_y & ~1);
                    // the pixel shader, the quad's four lanes
                    std::unique_ptr<shader::Invocation> inv[4];
                    double center_b[4][3];
                    for (uint32_t lane = 0; lane < 4; lane++) {
                        double px = qx + int32_t(lane & 1) + 0.5, py = qy + int32_t(lane >> 1) + 0.5;
                        barycentric(px, py, center_b[lane]);
                    }
                    if (ps) {
                        for (uint32_t lane = 0; lane < 4; lane++) {
                            inv[lane] =
                                std::make_unique<shader::Invocation>(ps->parsed, constants[1], memory_span, &env, true);
                            float (*regs_in)[4] = inv[lane]->registers();
                            double cb[3];
                            const double *b = center_b[lane];
                            // the centroid: the first covered sample where the pixel is partly covered
                            bool partial = cover[lane] && cover[lane] != (1u << samples) - 1;
                            if (partial && st.centroid) {
                                uint32_t s = uint32_t(std::countr_zero(cover[lane]));
                                double px = qx + int32_t(lane & 1) + 0.5 + sample_x16[s] / 16.0;
                                double py = qy + int32_t(lane >> 1) + 0.5 + sample_y16[s] / 16.0;
                                barycentric(px, py, cb);
                            }
                            for (uint32_t k = 0; k < 16; k++) {
                                const double *bk = partial && (st.centroid >> k) & 1 ? cb : b;
                                if ((st.flat >> k) & 1) {
                                    std::memcpy(regs_in[k], t.v[t.provoking].attr[k], 16);
                                    continue;
                                }
                                double q[3], sum = 0;
                                for (int v = 0; v < 3; v++) {
                                    q[v] = st.perspective ? bk[v] * t.v[v].inv_w : bk[v];
                                    sum += q[v];
                                }
                                for (int c = 0; c < 4; c++) {
                                    double a = 0;
                                    for (int v = 0; v < 3; v++) a += q[v] * t.v[v].attr[k][c];
                                    regs_in[k][c] = float(a / sum);
                                }
                            }
                            if (st.param_gen) {
                                float x = float(qx + int32_t(lane & 1)), y = float(qy + int32_t(lane >> 1));
                                if (t.kind == 0 && !t.front) x = -x;
                                if (t.kind == 2) y = -y;
                                float z = t.kind == 1 ? -0.0f : 0.0f, w = 0.0f;
                                if (st.point_coordinates) {
                                    double pc[2] = {};
                                    for (int v = 0; v < 3; v++)
                                        for (int c = 0; c < 2; c++) pc[c] += b[v] * t.v[v].pc[c];
                                    z = std::clamp(float(pc[0]), 0.0f, 1.0f), w = std::clamp(float(pc[1]), 0.0f, 1.0f);
                                }
                                float *g = regs_in[st.param_gen_register];
                                g[0] = x, g[1] = y, g[2] = z, g[3] = w;
                            }
                        }
                        if (env.debug_here) {
                            uint32_t lane = uint32_t(debug_x & 1) | uint32_t(debug_y & 1) << 1;
                            std::fprintf(stderr, "  trace draw %llu: the pixel shader's inputs\n",
                                         (unsigned long long)report.draws);
                            for (int k = 0; k < 16; k++)
                                std::fprintf(stderr, "    r%d %g %g %g %g\n", k, inv[lane]->registers()[k][0],
                                             inv[lane]->registers()[k][1], inv[lane]->registers()[k][2],
                                             inv[lane]->registers()[k][3]);
                        }
                        // the lanes in lockstep at fetches taking derivatives
                        shader::Invocation::Status status[4];
                        for (uint32_t lane = 0; lane < 4; lane++) status[lane] = inv[lane]->run();
                        for (int guard = 0; guard < 100000; guard++) {
                            uint64_t at = UINT64_MAX;
                            for (uint32_t lane = 0; lane < 4; lane++)
                                if (status[lane] == shader::Invocation::Status::Waiting)
                                    at = std::min(at, inv[lane]->waiting_at());
                            if (at == UINT64_MAX) break;
                            float quad[4][4];
                            bool here[4];
                            uint32_t first = 4;
                            for (uint32_t lane = 0; lane < 4; lane++) {
                                here[lane] = status[lane] == shader::Invocation::Status::Waiting &&
                                             inv[lane]->waiting_at() == at;
                                if (here[lane]) {
                                    std::memcpy(quad[lane], inv[lane]->waiting(), 16);
                                    if (first == 4) first = lane;
                                }
                            }
                            for (uint32_t lane = 0; lane < 4; lane++)
                                if (!here[lane]) std::memcpy(quad[lane], quad[first], 16);
                            for (uint32_t lane = 0; lane < 4; lane++)
                                if (here[lane]) {
                                    inv[lane]->resume(quad, lane);
                                    status[lane] = inv[lane]->run();
                                }
                        }
                        bool failed = false;
                        for (uint32_t lane = 0; lane < 4; lane++)
                            if (status[lane] != shader::Invocation::Status::Done) {
                                if (pixel_failures[band_index].empty())
                                    pixel_failures[band_index] = inv[lane]->unsupported().empty()
                                                                     ? "a pixel shader that does not end"
                                                                     : inv[lane]->unsupported();
                                failed = true;
                            }
                        if (failed) continue;
                    }
                    // the output merger, the covered pixels in order
                    for (uint32_t lane = 0; lane < 4; lane++) {
                        uint32_t mask = cover[lane];
                        if (!mask) continue;
                        const uint32_t px = uint32_t(qx + int32_t(lane & 1)), py = uint32_t(qy + int32_t(lane >> 1));
                        const harness::CaseOutput *o = ps ? &inv[lane]->output() : nullptr;
                        uint32_t written = ps ? inv[lane]->colors_written() : 0;
                        if (env.debug_here && int32_t(px) == debug_x && int32_t(py) == debug_y)
                            std::fprintf(stderr,
                                         "  trace draw %llu: covered %x, written %x, killed %d, depth test %d, %s "
                                         "face (depth control %08X, stencil %06X/%06X, mode %08X, clip %08X)\n",
                                         (unsigned long long)report.draws, mask, written,
                                         ps ? int(inv[lane]->killed()) : -1, int(st.depth_stencil),
                                         face ? "back" : "front", regs[RB_DEPTHCONTROL], st.stencil_word[0],
                                         st.stencil_word[1], sc_mode, clip);
                        if (ps && inv[lane]->killed()) continue;
                        if (ps && st.alpha_test && (written & 1)) {
                            float a = o->colors[0][3], ref = st.alpha_ref;
                            bool pass;
                            switch (st.alpha_func) {
                            case 0: pass = false; break;
                            case 1: pass = a < ref; break;
                            case 2: pass = a == ref; break;
                            case 3: pass = a <= ref; break;
                            case 4: pass = a > ref; break;
                            case 5: pass = !(a == ref); break;
                            case 6: pass = a >= ref; break;
                            default: pass = true; break;
                            }
                            if (!pass) continue;
                        }
                        if (ps && st.alpha_to_mask && (written & 1)) {
                            // FSI_AlphaToMask: per sample a threshold less the
                            // pixel's dither offset
                            uint32_t index = (py & 1) | ((px & 1) << 1);
                            float offset = float((st.alpha_to_mask_offsets >> (2 * index)) & 3);
                            float a = o->colors[0][3];
                            uint32_t m = 0xF;
                            auto test = [&](uint32_t s, float base, float step) {
                                if (!(a >= base + offset * -step)) m &= ~(1u << s);
                            };
                            if (msaa == 0)
                                test(0, 1.0f, 0.25f);
                            else if (msaa == 1)
                                test(0, 0.5f, 0.125f), test(1, 1.0f, 0.125f);
                            else
                                test(0, 0.75f, 1.0f / 16), test(1, 0.25f, 1.0f / 16), test(2, 0.5f, 1.0f / 16),
                                    test(3, 1.0f, 1.0f / 16);
                            mask &= m;
                            if (!mask) continue;
                        }
                        // depth and stencil per covered sample (FSI_DepthStencilTest)
                        if (st.depth_stencil) {
                            const uint32_t sw = st.stencil_word[face], so = st.stencil_ops[face];
                            const uint32_t reference = sw & 0xFF, read_mask = (sw >> 8) & 0xFF,
                                           write_mask = (sw >> 16) & 0xFF;
                            for (uint32_t s = 0; s < samples; s++) {
                                if (!(mask & (1u << s))) continue;
                                float z;
                                if (st.pixel_writes_depth) {
                                    z = o->depth[0];
                                } else {
                                    double sxp = px + 0.5 + sample_x16[s] / 16.0, syp = py + 0.5 + sample_y16[s] / 16.0;
                                    double zz = t.v[0].z + t.dzdx * (sxp - t.v[0].x) + t.dzdy * (syp - t.v[0].y);
                                    z = float(zz) + depth_bias;
                                }
                                z = z > 0.0f ? std::min(z, 1.0f) : 0.0f; // saturated, NaN as 0
                                uint32_t z24 = st.float24 ? pieces::float32_to_20e4(z, true)
                                                          : uint32_t(std::nearbyint(z * 16777215.0f));
                                uint32_t at = edram_address(st.depth_base, pitch, msaa, false, true, px, py, s);
                                uint32_t old = edram[at], old_z = old >> 8;
                                bool z_pass = ((st.depth_func & 1) && z24 < old_z) ||
                                              ((st.depth_func & 2) && z24 == old_z) ||
                                              ((st.depth_func & 4) && z24 > old_z);
                                bool s_pass = true;
                                uint32_t result = old;
                                if (st.stencil) {
                                    uint32_t ref_masked = reference & read_mask, old_masked = old & read_mask;
                                    s_pass = ((so & 1) && ref_masked < old_masked) ||
                                             ((so & 2) && ref_masked == old_masked) ||
                                             ((so & 4) && ref_masked > old_masked);
                                    uint32_t op = (so >> (s_pass ? (z_pass ? 6 : 9) : 3)) & 7;
                                    uint32_t v = old;
                                    switch (op) {
                                    case 1: v = 0; break;
                                    case 2: v = reference; break;
                                    case 3: v = std::min(old & 0xFFu, 254u) + 1; break;
                                    case 4: v = std::max(old & 0xFFu, 1u) - 1; break;
                                    case 5: v = ~old; break;
                                    case 6: v = old + 1; break;
                                    case 7: v = old - 1; break;
                                    default: break;
                                    }
                                    result = (old & ~write_mask) | (v & write_mask);
                                }
                                bool pass = z_pass && s_pass;
                                if (env.debug_here && int32_t(px) == debug_x && int32_t(py) == debug_y)
                                    std::fprintf(stderr,
                                                 "    sample %u: depth %06X against %06X (function %u), stencil %02X "
                                                 "against %02X: %s\n",
                                                 s, z24, old_z, st.depth_func, old & 0xFF, reference,
                                                 pass ? "passes" : "fails");
                                if (!pass) mask &= ~(1u << s);
                                if (pass && st.depth_write) result = (result & 0xFFu) | (z24 << 8);
                                edram[at] = result;
                            }
                            if (!mask) continue;
                        }
                        if (!written) continue;
                        for (uint32_t i = 0; i < 4; i++) {
                            const TargetState &rt = st.rt[i];
                            if (!rt.on || !(written & (1u << i))) continue;
                            float c[4];
                            for (int k = 0; k < 4; k++) c[k] = o->colors[i][k] * rt.exp_bias;
                            for (uint32_t s = 0; s < samples; s++) {
                                if (!(mask & (1u << s))) continue;
                                uint32_t at = edram_address(rt.base, pitch, msaa, rt.wide, false, px, py, s);
                                uint32_t dest[2] = {edram[at], rt.wide ? edram[(at + 1) % EDRAM_DWORDS] : 0u};
                                uint32_t out[2];
                                pieces::blend(c, dest, rt.format, rt.blend, st.blend_constant, rt.mask, out);
                                if (env.debug_here && px == uint32_t(debug_x) && py == uint32_t(debug_y) && s == 0)
                                    std::fprintf(stderr,
                                                 "    draw %llu target %u (format %u, base %u): %g %g %g %g, blend "
                                                 "%08X, mask %X: %08X %08X to %08X %08X\n",
                                                 (unsigned long long)report.draws, i, rt.format, rt.base, c[0], c[1],
                                                 c[2], c[3], rt.blend, rt.mask, dest[0], dest[1], out[0], out[1]);
                                edram[at] = out[0];
                                if (rt.wide) edram[(at + 1) % EDRAM_DWORDS] = out[1];
                            }
                        }
                    }
                }
        }
        quads.fetch_add(local_quads, std::memory_order_relaxed);
        pixel_exports[band_index] = std::move(env.written_);
    });
    report.quads += quads.load();
    for (const std::string &f : pixel_failures)
        if (!f.empty()) {
            approximate("pixel shader not run in some quads: " + f);
            break;
        }
    invalidate_exports();
    for (auto &list : pixel_exports)
        for (auto [a, n] : list) textures.invalidate(a, n);
    if (options.verbose)
        std::fprintf(stderr, "  ref draw %llu: %s, %zu vertices, %zu triangles, %llu culled\n",
                     (unsigned long long)report.draws, xenos::primitive_name(primitive), unique.size(),
                     triangles.size(), (unsigned long long)culled);
}

} // namespace xf::reference
