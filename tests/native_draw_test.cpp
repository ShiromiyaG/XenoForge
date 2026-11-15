// A port's draw setup from registers (xenoforge/native_draw.h), without a GPU:
// - draw_setup picks the targets the pixel shader writes with a channel on,
//   the depth target a draw tests, depth and stencil from RB_DEPTHCONTROL
//   (the back face's own only for polygons), culling, blending (a maximum
//   with factors becomes the pixel shader's premultiply), the shaders' forms
//   (rectangle and point lists, user clip planes, early depth), the alpha
//   test and the textures' filter overrides, and refuses what it cannot draw;
// - place_draw gives the polygon offset in the depth format's units and the
//   targets' scales;
// - convert_indices makes quads, rectangles, points, line loops and the
//   reset index's cuts as the emulated path does;
// - clear_value keeps formats as Frame::target_format's targets hold them.

#include "check.h"

#include "xenoforge/native_draw.h"
#include "xenoforge/xenos.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace xf;
using namespace xf::native;
namespace reg = xenos::reg;

namespace {

constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E;
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_CL_CLIP_CNTL = 0x2204;
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206, PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380, PA_CL_UCP_0_X = 0x2388;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;

// a 1280x720 frame's state: color target 0 (8_8_8_8 at tile 0) and D24S8 at tile 0x300, the viewport over it
std::vector<uint32_t> frame_registers() {
    std::vector<uint32_t> r(xenos::REGISTER_COUNT, 0);
    r[reg::RB_MODECONTROL] = 4; // color and depth
    r[reg::RB_SURFACE_INFO] = 1280;
    r[reg::RB_COLOR_INFO] = 0;
    r[reg::RB_DEPTH_INFO] = 0x300;
    r[reg::RB_COLOR_MASK] = 0xF;
    r[RB_BLENDCONTROL0] = 0x00010001; // ONE, ADD, ZERO
    r[reg::PA_SC_WINDOW_SCISSOR_TL] = 0x80000000u;
    r[reg::PA_SC_WINDOW_SCISSOR_BR] = 720u << 16 | 1280u;
    r[reg::PA_SC_SCREEN_SCISSOR_BR] = 0x20002000u; // 8192 (15-bit signed)
    r[PA_CL_VTE_CNTL] = 0x3F; // the viewport's scale and offset on
    const float vport[6] = {640.0f, 640.0f, -360.0f, 360.0f, 1.0f, 0.0f};
    std::memcpy(&r[PA_CL_VPORT_XSCALE], vport, sizeof(vport));
    return r;
}

ShaderUses pixel_writing(uint8_t colors) {
    ShaderUses u;
    u.colors_written = colors;
    return u;
}

} // namespace

int main() {
    const ShaderUses vs;
    // a plain draw: color target 0, depth tested less and written, back faces culled
    {
        std::vector<uint32_t> r = frame_registers();
        r[reg::RB_DEPTHCONTROL] = 2 | 4 | 1u << 4; // z enable, z write, LESS
        r[reg::PA_SU_SC_MODE_CNTL] = 2;            // cull back
        const ShaderUses ps = pixel_writing(1);
        DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.why.empty() && !s.empty);
        CHECK(s.pitch == 1280 && s.msaa == 0);
        CHECK(s.color[0].used && s.color[0].format == 0 && !s.color[1].used);
        CHECK(s.depth.used && s.depth.edram_base == 0x300 && s.depth.format == 0);
        CHECK(s.depth_test && s.depth_write && !s.stencil_test && s.depth_compare == VK_COMPARE_OP_LESS);
        CHECK(s.cull == VK_CULL_MODE_BACK_BIT && s.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        CHECK(!s.blend[0].blendEnable && s.blend[0].colorWriteMask == 0xF);
        CHECK(s.pixel_shader && s.pixel.early_depth_stencil); // no kill, no oDepth, no alpha test
        CHECK(s.vertex.vertex_key() == 0 && s.indices == IndexForm::AsGiven);
        place_draw(s, r.data(), 1280, 720, VK_FORMAT_D24_UNORM_S8_UINT);
        CHECK(s.viewport.width == 1280.0f && std::fabs(s.viewport.height) == 720.0f);
        CHECK(s.scissor.extent.width == 1280 && s.scissor.extent.height == 720 && !s.empty);
        CHECK(s.system.color_scales[0] == 1.0f);
        // the same with another cull: another pipeline
        r[reg::PA_SU_SC_MODE_CNTL] = 1;
        DrawSetup t = draw_setup(r.data(), 4, vs, &ps);
        CHECK(t.cull == VK_CULL_MODE_FRONT_BIT && t.pipeline_key != s.pipeline_key);
    }
    // a target the shader does not write, a channel mask of 0: not a target
    {
        std::vector<uint32_t> r = frame_registers();
        r[reg::RB_COLOR_MASK] = 0xF0; // target 1's channels only
        const ShaderUses ps = pixel_writing(1);
        const DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.why.empty() && s.empty); // nothing written: no depth either
        r[reg::RB_COLOR_INFO + 2] = 0x00050020; // RB_COLOR1_INFO: 16_16_16_16 at tile 0x20
        const ShaderUses ps2 = pixel_writing(3);
        const DrawSetup t = draw_setup(r.data(), 4, vs, &ps2);
        CHECK(!t.color[0].used && t.color[1].used && t.color[1].edram_base == 0x20 && t.color[1].format == 5);
    }
    // depth only (EDRAM mode 5): no pixel shader, the depth target alone
    {
        std::vector<uint32_t> r = frame_registers();
        r[reg::RB_MODECONTROL] = 5;
        r[reg::RB_DEPTHCONTROL] = 2 | 4 | 7u << 4;
        const ShaderUses ps = pixel_writing(1);
        const DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.why.empty() && !s.pixel_shader && !s.color[0].used && s.depth.used);
    }
    // stencil: the back face's own for polygons, the front's for a rectangle list
    {
        std::vector<uint32_t> r = frame_registers();
        // stencil on, front: ALWAYS, REPLACE on pass; back faces on (bit 7): EQUAL, KEEP
        r[reg::RB_DEPTHCONTROL] = 1 | 1u << 7 | 7u << 8 | 2u << 14 | 2u << 20;
        r[RB_STENCILREFMASK] = 0x00FF0F05, r[RB_STENCILREFMASK_BF] = 0x00110F07;
        const ShaderUses ps = pixel_writing(1);
        DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.stencil_test && s.stencil_front.compareOp == VK_COMPARE_OP_ALWAYS);
        CHECK(s.stencil_front.passOp == VK_STENCIL_OP_REPLACE && s.stencil_back.compareOp == VK_COMPARE_OP_EQUAL);
        CHECK(s.stencil_reference[0] == 5 && s.stencil_reference[1] == 7 && s.stencil_write_mask[1] == 0x11);
        CHECK(s.stencil_compare_mask[0] == 0x0F);
        s = draw_setup(r.data(), 8, vs, &ps); // a rectangle list: no facing
        CHECK(s.stencil_back.compareOp == VK_COMPARE_OP_ALWAYS && s.stencil_reference[1] == 5);
        CHECK(s.vertex.form == VertexForm::RectangleList && s.indices == IndexForm::Rectangles && s.restart);
        CHECK(s.cull == VK_CULL_MODE_NONE);
    }
    // a maximum with a source factor of the source's alpha: multiplied in by the pixel shader
    {
        std::vector<uint32_t> r = frame_registers();
        r[RB_BLENDCONTROL0] = 6u | 3u << 5 | 0u << 8 | (1u | 0u << 5 | 0u << 8) << 16; // SRC_ALPHA, MAX, ZERO
        const ShaderUses ps = pixel_writing(1);
        const DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.pixel.blend_premultiply && s.system.blend_premultiply[0] != 0);
        CHECK(s.blend[0].colorBlendOp == VK_BLEND_OP_ADD); // the shader takes the maximum with zero
    }
    // the alpha test and alpha to coverage into the system constants, and no early depth with them
    {
        std::vector<uint32_t> r = frame_registers();
        r[RB_COLORCONTROL] = 8 | 4 | 16; // alpha test GREATER, alpha to coverage
        const float ref = 0.5f;
        std::memcpy(&r[RB_ALPHA_REF], &ref, 4);
        const ShaderUses ps = pixel_writing(1);
        const DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK((s.system.flags[0] & 0xF) == 0xC && (s.system.flags[0] >> 16 & 1) && s.system.values[0] == 0.5f);
        CHECK(!s.pixel.early_depth_stencil);
    }
    // user clip planes: packed, the vertex shader's form with their count
    {
        std::vector<uint32_t> r = frame_registers();
        r[PA_CL_CLIP_CNTL] = 0x5; // planes 0 and 2
        const float plane2[4] = {0, 0, 1, -0.5f};
        std::memcpy(&r[PA_CL_UCP_0_X + 8], plane2, 16);
        const ShaderUses ps = pixel_writing(1);
        const DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.vertex.clip_planes == 2 && !s.vertex.clip_planes_cull);
        CHECK(s.system.clip_planes[1][2] == 1.0f && s.system.clip_planes[1][3] == -0.5f);
    }
    // a texture slot's filters from the shader's first fetch, the vertex shader's first
    {
        std::vector<uint32_t> r = frame_registers();
        ShaderUses ps = pixel_writing(1), v;
        ps.texture_slots = 1u << 3, ps.filter_slots = 1u << 3;
        ps.slot_filters[3][0] = 0, ps.slot_filters[3][1] = 1, ps.slot_filters[3][2] = 3, ps.slot_filters[3][3] = 7;
        v.texture_slots = 1u << 5;
        const DrawSetup s = draw_setup(r.data(), 4, v, &ps);
        CHECK(s.texture_slots == (1u << 3 | 1u << 5));
        CHECK(s.slot_filters[3][0] == 0 && s.slot_filters[3][1] == 1 && s.slot_filters[3][2] == 3);
        CHECK(s.slot_filters[5][0] == 3 && s.slot_filters[5][3] == 7); // the constant's
    }
    // what it cannot draw
    {
        std::vector<uint32_t> r = frame_registers();
        const ShaderUses ps = pixel_writing(1);
        CHECK(!draw_setup(r.data(), 7, vs, &ps).why.empty()); // wireframe triangle lists
        r[reg::RB_MODECONTROL] = 6;
        CHECK(!draw_setup(r.data(), 4, vs, &ps).why.empty()); // a copy mode
    }
    // the polygon offset: D24S8 in 2^-24 units rounded up, D24FS8 kept in a float target halved
    {
        std::vector<uint32_t> r = frame_registers();
        r[reg::RB_DEPTHCONTROL] = 2 | 4 | 1u << 4;
        r[reg::PA_SU_SC_MODE_CNTL] = 1u << 11; // the front's offset on
        const float offsets[2] = {16.0f, 0.0001f};
        std::memcpy(&r[PA_SU_POLY_OFFSET_FRONT_SCALE], offsets, 8);
        const ShaderUses ps = pixel_writing(1);
        DrawSetup s = draw_setup(r.data(), 4, vs, &ps);
        CHECK(s.poly_offset[0] == 16.0f && s.poly_offset[1] == 0.0001f);
        place_draw(s, r.data(), 1280, 720, VK_FORMAT_D24_UNORM_S8_UINT);
        CHECK(s.depth_bias_slope == 1.0f); // per 1/16 pixel
        CHECK(s.depth_bias_constant == std::ceil(0.0001f * float((1u << 24) - 1)));
        r[reg::RB_DEPTH_INFO] = 0x300 | 1u << 16; // D24FS8, a float target
        s = draw_setup(r.data(), 4, vs, &ps);
        place_draw(s, r.data(), 1280, 720, VK_FORMAT_D32_SFLOAT_S8_UINT);
        CHECK(s.depth_bias_constant == 0.0001f * float(1u << 24) * 0.5f && s.depth_bias_representation == 2);
        CHECK(s.system.flags[3] & 1); // the depth halved
    }
    // indices
    {
        std::vector<uint32_t> r = frame_registers();
        const ShaderUses ps = pixel_writing(1);
        std::vector<uint32_t> out;
        DrawSetup quads = draw_setup(r.data(), 13, vs, &ps);
        const uint32_t q[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        convert_indices(quads, q, 8, 0, out);
        CHECK((out == std::vector<uint32_t>{0, 1, 3, 1, 2, 3, 4, 5, 7, 5, 6, 7}));
        convert_indices(quads, nullptr, 4, 10, out); // non-indexed from vertex 10
        CHECK((out == std::vector<uint32_t>{10, 11, 13, 11, 12, 13}));
        DrawSetup rects = draw_setup(r.data(), 8, vs, &ps);
        convert_indices(rects, nullptr, 3, 0, out);
        CHECK(out.size() == 12 && out[0] == 0 && out[2] == 2 && out[9] == 0 && out[11] == 2);
        DrawSetup points = draw_setup(r.data(), 1, vs, &ps);
        CHECK(points.vertex.form == VertexForm::PointList && points.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
        convert_indices(points, nullptr, 2, 5, out);
        CHECK((out == std::vector<uint32_t>{20, 21, 22, 23, UINT32_MAX, 24, 25, 26, 27}));
        // a line loop closed by its first index; the reset index cuts strips only with multi_prim_ib_ena
        r[VGT_MULTI_PRIM_IB_RESET_INDX] = 0xFFFF;
        DrawSetup loop = draw_setup(r.data(), 12, vs, &ps);
        const uint32_t l[7] = {0, 1, 2, 0xFFFF, 3, 4, 5};
        convert_indices(loop, l, 7, 0, out);
        CHECK(!loop.reset && out.size() == 8 && out[7] == 0); // (one loop through 0xFFFF)
        r[reg::PA_SU_SC_MODE_CNTL] = 1u << 21;
        loop = draw_setup(r.data(), 12, vs, &ps);
        convert_indices(loop, l, 7, 0, out);
        CHECK((out == std::vector<uint32_t>{0, 1, 2, 0, UINT32_MAX, 3, 4, 5, 3}));
        DrawSetup strip = draw_setup(r.data(), 6, vs, &ps);
        convert_indices(strip, l, 7, 0, out);
        CHECK(strip.restart && out[3] == UINT32_MAX && out.size() == 7);
    }
    // clear values as the port's targets keep them
    {
        const uint32_t white[2] = {0xFFFFFFFFu, 0};
        VkClearValue v = clear_value(false, 0, white);
        CHECK(v.color.float32[0] == 1.0f && v.color.float32[3] == 1.0f);
        const uint32_t depth_one[2] = {0xFFFFFF00u | 0x42, 0};
        v = clear_value(true, 0, depth_one);
        CHECK(v.depthStencil.depth == 1.0f && v.depthStencil.stencil == 0x42);
        const float color[4] = {0.5f, 0, 0, 1};
        v = clear_value(false, 5, color, 0, 0); // 16_16_16_16 as snorm of -32..32
        CHECK(std::fabs(v.color.float32[0] - 0.5f * 1024.0f / 32767.0f) < 1e-7f);
        v = clear_value(true, 1, nullptr, 1.0f, 3); // D24FS8 halved
        CHECK(v.depthStencil.depth == 0.5f && v.depthStencil.stencil == 3);
        const uint32_t seven_e3[2] = {0x3FFu | 0x3u << 30, 0}; // the largest 7e3 red, alpha 1
        v = clear_value(false, 3, seven_e3);
        CHECK(v.color.float32[0] > 30.0f && v.color.float32[3] == 1.0f);
    }
    return finish("native_draw");
}
