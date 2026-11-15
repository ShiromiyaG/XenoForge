// A game's own shaders for a port's renderer (docs/sdk.md, "A port that
// replaces the guest's Direct3D"): Xenos microcode translated to SPIR-V by
// the translator the emulated path uses, so a port draws a game's draws with
// the game's shaders, without writing them again, and without XenoForge's
// command processor in between.
//
//   #include <xenoforge/native_shaders.h>
//   // the guest's D3D shader object holds the microcode as big-endian words
//   std::vector<uint32_t> words(count);
//   for (uint32_t i = 0; i < count; i++) words[i] = xf::port::load<uint32_t>(c, microcode + 4 * i);
//   xf::native::TranslatedShader vs = xf::native::translate_shader(words.data(), words.size(),
//                                                                  xf::native::ShaderStage::Vertex);
//   if (!vs.ok()) log(vs.why);                      // what the translator does not do yet
//   // vkCreateShaderModule(vs.spirv), entry point "main"; cache it by vs.hash
//
// The translated shaders' interface (the emulated path's own; shader_translator.h):
//
//   set 0, binding 0: uniform ShaderConstants of the vertex shader
//          binding 3: uniform ShaderConstants of the pixel shader
//          binding 1: storage buffer, guest memory as 32-bit words (big-endian as stored): the vertex
//                     fetches read it at the fetch constants' addresses, swapped in the shader
//                     (Frame::guest_memory gives the buffer and keeps it current)
//          binding 2: uniform SystemConstants
//   set 1, binding 0: 2D array images, 1: 3D images, 2: cube images, 3: samplers (unsized arrays,
//          indexed by SystemConstants::slots)
//   a vertex shader writes the position and o0-o15 (locations 0-15); a pixel shader reads
//   o0-o15 (locations 0-15) and writes oC0-oC3 (color locations 0-3)
//   one invocation per guest vertex, its index (gl_VertexIndex) in r0.x: index buffers are the
//   port's (the guest's, swapped), vertex input bindings none
//   VertexForm::RectangleList: a rectangle list's vertex shader (primitive type 8), drawn as a
//   triangle strip of four vertices a rectangle (the corner in gl_VertexIndex's bits 0-1), the
//   indices of its three guest vertices a uvec3 at input location 0 (VK_FORMAT_R32G32B32_UINT, the
//   same for the four); the guest shader runs for the three and the fourth corner is made from them
//   VertexForm::PointList: a point list's (primitive type 1), each point a triangle strip of four
//   vertices, gl_VertexIndex the guest index shifted left by 2 with the corner in bits 0-1 (bit 1
//   right, bit 0 bottom): the sprite made from the point size (SystemConstants::point)
//   A draw's other forms (ShaderVariant): user clip planes, a D24FS8 depth converted in the pixel
//   shader, centroid interpolators, a point list's sprite coordinates, early depth and stencil, and
//   minimum and maximum blends with factors multiplied in; native_draw.h's draw_setup says which
//
// The two blocks below are the layouts the shaders read, checked against the
// renderer's at XenoForge's build (a change to either is a change to this
// header, so to the SDK's version).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xf::native {

enum class ShaderStage { Vertex, Pixel };
// how a vertex shader runs on the host (shader_translator.h's HostVertex): a
// guest vertex an invocation, or a rectangle list's rectangle four
enum class VertexForm { Vertices, RectangleList, PointList };

// A shader's form for a draw beyond its stage (the emulated path's
// shader::VertexOptions and PixelOptions, as its draws pick them): the
// default is the plain form, the one the bake makes. native_draw.h's
// draw_setup gives the variant each of a draw's shaders takes.
struct ShaderVariant {
    // a vertex shader's
    VertexForm form = VertexForm::Vertices;
    uint8_t clip_planes = 0;       // the user clip planes enabled (0-6), packed in SystemConstants::clip_planes
    bool clip_planes_cull = false; // as cull distances (PA_CL_CLIP_CNTL's ucp_cull_only_ena)
    uint8_t point_ucp_mode = 0;    // a point list's ps_ucp_mode (0-3)
    // a pixel shader's
    uint8_t depth_float24 = 0;       // 1: a D24FS8 depth converted towards zero, 2: to the nearest even
    uint16_t centroid = 0;           // the interpolators read at the centroid (multisampled), a bit each
    bool point_coordinates = false;  // a point list's pixel parameters take the sprite's coordinates
    bool early_depth_stencil = false; // depth and stencil tested before the shader runs
    bool blend_premultiply = false;  // SystemConstants::blend_premultiply's factors multiplied in
    // a word for each (0: the plain form); a cache keys the forms by it
    uint32_t vertex_key() const {
        return uint32_t(form) | uint32_t(clip_planes) << 4 | uint32_t(clip_planes_cull) << 7 |
               uint32_t(point_ucp_mode & 3) << 8;
    }
    uint32_t pixel_key() const {
        return uint32_t(depth_float24 & 3) | uint32_t(point_coordinates) << 2 | uint32_t(early_depth_stencil) << 3 |
               uint32_t(blend_premultiply) << 4 | uint32_t(centroid) << 16;
    }
    uint32_t key(bool vertex) const { return vertex ? vertex_key() : pixel_key(); }
};

struct TranslatedShader {
    std::vector<uint32_t> spirv; // empty when not translated (why)
    std::string why;             // what the shader uses that the translator does not do yet
    // what it does as Xenia does, which is not quite the console's (for a
    // port's report)
    std::vector<std::string> approximate;
    // the microcode's hash: the one the emulated path keys the shader by,
    // PassInfo's vertex_shader and pixel_shader, and shader dumps' names
    uint64_t hash = 0;
    // the stage's float constants (c0-c255) the shader reads, a bit each
    // (used_float_constants): the ones a draw needs uploaded
    uint64_t float_constants[4] = {};
    bool ok() const { return why.empty() && !spirv.empty(); }
};

// The microcode's words in host order (a guest shader object stores them
// big-endian: swap each). Thread-safe; a port caches the result by hash.
TranslatedShader translate_shader(const uint32_t *microcode, size_t words, ShaderStage stage,
                                  VertexForm form = VertexForm::Vertices);
// The same in a draw's form (only the stage's half of the variant is read).
TranslatedShader translate_shader(const uint32_t *microcode, size_t words, ShaderStage stage,
                                  const ShaderVariant &variant);

// The hash alone (translate_shader's): a shader known already is not
// translated again.
uint64_t shader_hash(const uint32_t *microcode, size_t words, ShaderStage stage);

// The stage's float constants the shader reads (its ALU operands), a bit per
// constant c0-c255 in out[0..3] (c0 = bit 0 of out[0]); a constant read
// relative to a0 or aL sets them all. What a draw needs of the constants: a
// native renderer uploads only these, and a port comparing its draws with
// the GPU's compares only these (the GPU's other constants hold whatever an
// earlier shader left there). False when the microcode does not parse.
bool used_float_constants(const uint32_t *microcode, size_t words, ShaderStage stage, uint64_t out[4]);

// What else a shader reads of a draw's state: the texture fetch constants
// (slots 0-31, a bit each) its texture fetches and texture queries name, and
// the vertex fetch constants (0-95, a bit each in vertex_fetches[0..2]) its
// vertex fetches name, with its float constants (used_float_constants'). A
// native renderer binds only these textures and streams. vertex_strides: each
// vertex fetch constant's stride in dwords (its full fetches' largest; 0 for
// one it does not name), so a port copying a draw's vertices copies indices
// first to last + 1 times it. And what a draw's setup takes of it
// (native_draw.h): the color targets a pixel shader writes, whether it
// writes depth, kills pixels or exports memory, and each texture slot's
// filters its first fetch gives over the fetch constant's.
struct ShaderUses {
    uint64_t float_constants[4] = {};
    uint32_t texture_slots = 0;
    uint32_t vertex_fetches[3] = {};
    uint8_t vertex_strides[96] = {};
    uint8_t colors_written = 0; // oC0-oC3, a bit each
    bool writes_depth = false;  // oDepth
    bool kills = false;         // a kill instruction
    bool exports = false;       // memexport (eA, eM#)
    // per slot (filter_slots: a bit each) the first texture fetch's filters:
    // mag, min, mip (0 point, 1 linear, 2 base map; 3 the fetch constant's)
    // and anisotropy (0-5; 7 the fetch constant's)
    uint32_t filter_slots = 0;
    uint8_t slot_filters[32][4] = {};
};
bool shader_uses(const uint32_t *microcode, size_t words, ShaderStage stage, ShaderUses &out);

// The translator's version: a digest of its sources (the parser, the
// translator, the shader headers), so SPIR-V kept on disk is never used by
// a translator that would make other SPIR-V.
const char *translator_version();

// A port's shaders by hash: in memory, and in a directory, under a folder
// of the translator's version, as <hash, 16 hex digits>.vs.spv / .ps.spv (a shader that would not translate as
// <hash>.vs.txt / .ps.txt, its why), so a shader is translated once, ever.
// bake_shaders fills the directory before a run; a shader the bake missed is
// translated when first asked for and written there. Thread-safe.
class ShaderCache {
  public:
    explicit ShaderCache(std::filesystem::path directory = {}); // empty: in memory only
    ~ShaderCache();
    ShaderCache(const ShaderCache &) = delete;
    ShaderCache &operator=(const ShaderCache &) = delete;
    // the shader for this microcode (host order), translated or read when not
    // met yet; the reference stays valid as long as the cache
    // (form: a vertex shader's; the rectangle list form is kept apart, and
    // not baked: translated when first met)
    const TranslatedShader &get(const uint32_t *microcode, size_t words, ShaderStage stage,
                                VertexForm form = VertexForm::Vertices);
    // a draw's form (ShaderVariant), kept on disk as <hash>.vs.<key, 8 hex digits>.spv (.ps. for a pixel
    // shader's) beside the plain ones; translated when first met
    const TranslatedShader &get(const uint32_t *microcode, size_t words, ShaderStage stage,
                                const ShaderVariant &variant);
    struct Stats {
        uint64_t read = 0;       // from the directory (the bake's, or an earlier run's)
        uint64_t translated = 0; // translated now (what a bake would have saved)
        uint64_t failed = 0;     // would not translate
    };
    Stats stats() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// The bake: every shader a run of the game recorded (XenoForge's shader
// storage, <cache>/<title id>.xfshaders, which the generic player and a
// port's emulated frames write as they meet shaders) translated into a
// ShaderCache directory. The count written (translated or not), or -1 and
// why. (xfshader bake <storage> <directory> from a port's build.)
long bake_shaders(const std::filesystem::path &storage, const std::filesystem::path &directory, std::string &why);

// set 0's binding 0 (vertex) and 3 (pixel): a stage's constants, as the
// GPU's registers hold them (the D3DDevice's shadow copies them in this
// order: 256 float4, then the bools, the loops and the 32 fetch constants
// of 6 words), host order.
struct ShaderConstants {
    float float_constants[256][4]; // c0-c255 of the stage (the pixel shader's: the registers' 256-511)
    uint32_t bool_constants[8];    // b0-b255, a bit each
    uint32_t loop_constants[32];   // i0-i31
    uint32_t fetch_constants[32 * 6];
};
static_assert(sizeof(ShaderConstants) == 4096 + 32 + 128 + 768);

// set 0's binding 2: what the draw's fixed function gives the shaders.
struct SystemConstants {
    float ndc_scale[4]; // the guest's viewport to the host's NDC: x, y, z (and w unused)
    float ndc_offset[4];
    uint32_t slots[32]; // per fetch slot: image index (bits 0-13), kind (14-15: 2D array, 3D, cube), sampler (16-31)
    uint32_t flags[4];  // x: alpha test (bit 3 on, bits 0-2 its function); y: the position's formats (bit 0 W not
                        // 1/W, bit 1 XY divided by W, bit 2 Z divided by W); z: a bit per color target written as
                        // gamma; w: D24FS8 depth
    float values[4];    // x: the alpha test's reference; y, z: the point diameter's bounds
    float color_scales[4];
    float point[4];            // the constant point diameter, and guest pixels to NDC
    uint32_t signs[32];        // per slot, the texture's signs on the result's components (2 bits each)
    uint32_t signed_slots[32]; // per slot, the texture's signed view (as slots); 0 none
    float clip_planes[6][4];   // the user clip planes enabled, packed
    float extent_map[4];       // host NDC to console pixels (screen extents; 0 when not asked for)
    uint32_t extent_report[4];
    uint32_t integer_scale[32]; // per slot, how its result goes through the constant's number format
    uint32_t blend_premultiply[4];
    float blend_constant[4];
    uint32_t generic[4]; // (the emulated path's generic shaders; 0)
};
static_assert(sizeof(SystemConstants) == 784);

} // namespace xf::native
