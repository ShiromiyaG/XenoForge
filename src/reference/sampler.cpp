/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The reference renderer's textures: read out of guest memory by
// texture.h's decode (Xenia's untiling and loads; the block-compressed
// formats decompressed, as Xenia's load shaders do where the host has no
// BC), each texel converted to floats through the fetch constant's signs,
// then filtered here, on the CPU.
//
// What the fetch computes is the translator's (src/shader_translator.cpp,
// Translator::sample_texture and texture_signs, from Xenia's
// ProcessTextureFetchInstruction in spirv_shader_translator_fetch.cc): the
// size from the fetch constant, the coordinates offset by the instruction's
// offsets plus Xenia's rounding epsilon (kTextureCoordEpsilon, 1.5/1024 of a
// texel: the hardware's fixed-point coordinates, as Xenia records them) and
// normalized; the LOD from the fetch constant's bias, the register's and the
// instruction's, the gradients (the registers', or the quad's coarse
// derivatives) times 2 to the LOD plus the constant's per-axis exponents;
// cube maps from the face coordinates; stacked textures by layer, two of
// them blended when the volume filter is linear; the result through the
// fetch constant's swizzle, the unsigned components of fixed-point formats
// read as fractions rounded to 16 fractional bits (texture::q16_components),
// times 2 to its exponent adjust. The sampler's
// state is Xenia's texture caches' (GetSamplerParameters): the
// instruction's filters over the constant's, the lowest mip as the lowest
// LOD, a base map at it, the border colors.
//
// Where the reference differs from Xenia, by the documents:
// - signs (signed, biased, gamma) are applied to each texel before
//   filtering (Xenia applies them after the host filters, and says it
//   "likely should be before": dxbc_shader_translator_fetch.cc; R6xx and
//   Direct3D 10 convert texels before filtering);
// - LOD and filtering follow the Vulkan and Direct3D 10 definitions exactly
//   (the larger of the two gradients' lengths in texels, log2), with exact
//   float weights; the host's are implementation's (8 bits of weight on
//   most GPUs);
// - anisotropic filtering (AnisoFilter 1-5, up to 16 taps) is the reference
//   footprint of Vulkan's and Direct3D 10's specifications: taps along the
//   longer gradient as many as its length over the shorter one's, at the LOD
//   of the longer over their count (hosts approximate it each their way);
// - clamp to halfway is clamp to the edge, as Xenia's (counted); cube maps
//   are not seamless (each face clamped to its edges).

#include "reference_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace xf::reference {
namespace {

using texture::HostFormat;

// a texel's components as the host format keeps them: integers of a width,
// or floats
struct Fields {
    uint32_t count = 0;
    uint32_t bits[4] = {};
    uint32_t width[4] = {};
    bool is_float = false;
    float f[4] = {};
};

uint32_t le16(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

bool fields(HostFormat format, const uint8_t *p, Fields &out) {
    auto put = [&](uint32_t i, uint32_t value, uint32_t width) {
        out.bits[i] = value, out.width[i] = width;
        out.count = std::max(out.count, i + 1);
    };
    switch (format) {
    case HostFormat::R8: put(0, p[0], 8); return true;
    case HostFormat::RG8: put(0, p[0], 8), put(1, p[1], 8); return true;
    case HostFormat::RGBA8:
        for (uint32_t i = 0; i < 4; i++) put(i, p[i], 8);
        return true;
    case HostFormat::R5G6B5: {
        uint32_t v = le16(p);
        put(0, v >> 11, 5), put(1, (v >> 5) & 63, 6), put(2, v & 31, 5);
        return true;
    }
    case HostFormat::A1R5G5B5: {
        uint32_t v = le16(p);
        put(0, (v >> 10) & 31, 5), put(1, (v >> 5) & 31, 5), put(2, v & 31, 5), put(3, v >> 15, 1);
        return true;
    }
    case HostFormat::B4G4R4A4: {
        uint32_t v = le16(p);
        put(0, (v >> 4) & 15, 4), put(1, (v >> 8) & 15, 4), put(2, v >> 12, 4), put(3, v & 15, 4);
        return true;
    }
    case HostFormat::RGB10A2: {
        uint32_t v = le32(p);
        put(0, v & 1023, 10), put(1, (v >> 10) & 1023, 10), put(2, (v >> 20) & 1023, 10), put(3, v >> 30, 2);
        return true;
    }
    case HostFormat::R16: put(0, le16(p), 16); return true;
    case HostFormat::RG16: put(0, le16(p), 16), put(1, le16(p + 2), 16); return true;
    case HostFormat::RGBA16:
        for (uint32_t i = 0; i < 4; i++) put(i, le16(p + 2 * i), 16);
        return true;
    case HostFormat::R16F:
    case HostFormat::RG16F:
    case HostFormat::RGBA16F: {
        uint32_t n = format == HostFormat::R16F ? 1 : format == HostFormat::RG16F ? 2 : 4;
        out.is_float = true, out.count = n;
        for (uint32_t i = 0; i < n; i++) out.f[i] = pieces::half_to_float(uint16_t(le16(p + 2 * i)));
        return true;
    }
    case HostFormat::R32F:
    case HostFormat::RG32F:
    case HostFormat::RGBA32F: {
        uint32_t n = format == HostFormat::R32F ? 1 : format == HostFormat::RG32F ? 2 : 4;
        out.is_float = true, out.count = n;
        for (uint32_t i = 0; i < n; i++) out.f[i] = std::bit_cast<float>(le32(p + 4 * i));
        return true;
    }
    default: return false;
    }
}

// a component through its sign (texture_signs): unsigned, signed (snorm,
// no lower than -1), biased (offset binary; no lower than -1 for a
// fixed-point format read as fractions, `clamp`, as the number format then
// takes it in the translator), gamma (the piecewise linear curve back to
// linear)
float signed_component(const Fields &f, uint32_t i, uint32_t sign, bool clamp) {
    if (f.is_float) {
        float v = f.f[i];
        if (sign == 3) return pieces::pwl_gamma_to_linear(v);
        if (sign == 2) return v * 2.0f - 1.0f;
        return v;
    }
    uint32_t w = f.width[i], value = f.bits[i];
    float unorm = float(value) / float((1ull << w) - 1);
    switch (sign) {
    case 1: {
        if (w < 2) return unorm;
        int32_t s = int32_t(value << (32 - w)) >> (32 - w);
        return std::max(-1.0f, float(s) / float((1u << (w - 1)) - 1));
    }
    case 2: {
        float v;
        if (w < 2) {
            v = unorm * 2.0f - 1.0f;
        } else {
            float half = float(1u << (w - 1));
            v = (float(value) - half) / (half - 1.0f);
        }
        return clamp ? std::max(v, -1.0f) : v;
    }
    case 3: return pieces::pwl_gamma_to_linear(unorm);
    default: return unorm;
    }
}

} // namespace

std::shared_ptr<const Texture> Textures::get(const uint32_t fetch[6], std::string &why, Counts &counts) {
    std::array<uint32_t, 6> key;
    std::memcpy(key.data(), fetch, 24);
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    texture::Info info = texture::parse_fetch_constant(fetch);
    // block-compressed formats decompressed (no host BC here), everything
    // else in the guest's bits
    texture::Support support;
    for (HostFormat f : {HostFormat::BC1, HostFormat::BC2, HostFormat::BC3, HostFormat::BC4, HostFormat::BC5})
        support.sampled &= ~(uint64_t(1) << uint32_t(f));
    texture::Plan plan;
    if (!texture::plan(info, plan, why, support)) return nullptr;
    texture::Image image;
    if (!texture::decode(info, memory_, image, why, support)) return nullptr;
    if (!plan.approximate.empty()) counts.add("texture: " + plan.approximate);
    bool any_signed = false;
    for (uint32_t s : info.signs) any_signed |= s == 1;
    if (any_signed && plan.signed_read == texture::Signed::Separate)
        counts.add("signed components of a widened texture format read from its unsigned version");
    if (plan.half != texture::Half::None) counts.add("16-bit fixed texels as halves");
    const bool biased_clamp = !info.num_format && texture::component_bits(info.format)[0] != 0;

    auto t = std::make_shared<Texture>();
    t->info = info;
    texture::Levels levels = texture::levels(info);
    t->mip_min = levels.mip_min, t->mip_max = levels.mip_max;
    t->footprint = image.footprint;
    t->is_3d = info.dimension == 2;
    uint32_t bpt = texture::bytes_per_block(image.format);
    if (texture::is_compressed(image.format) || !bpt) {
        why = std::string("texture host format ") + texture::host_format_name(image.format);
        return nullptr;
    }
    t->levels.resize(image.levels.size());
    for (size_t l = 0; l < image.levels.size(); l++) {
        const std::vector<uint8_t> &data = image.levels[l];
        if (data.empty()) continue;
        Texture::Level &level = t->levels[l];
        level.width = std::max(image.width >> l, 1u);
        level.height = info.dimension == 0 ? 1 : std::max(image.height >> l, 1u);
        size_t count = data.size() / bpt;
        size_t per_slice = size_t(level.width) * level.height;
        level.slices = uint32_t(count / per_slice);
        if (!level.slices) {
            why = "a texture level smaller than its size";
            return nullptr;
        }
        level.texels.resize(size_t(level.slices) * per_slice * 4);
        Fields f;
        for (size_t i = 0; i < size_t(level.slices) * per_slice; i++) {
            f = Fields{};
            if (!fields(image.format, data.data() + i * bpt, f)) {
                why = std::string("texture host format ") + texture::host_format_name(image.format);
                return nullptr;
            }
            float *out = &level.texels[i * 4];
            for (uint32_t g = 0; g < 4; g++) {
                uint32_t h = (plan.host_swizzle >> (3 * g)) & 7;
                if (h < 4 && h < f.count)
                    out[g] = signed_component(f, h, info.signs[g], biased_clamp);
                else
                    out[g] = h == 5 ? 1.0f : 0.0f;
            }
        }
    }
    cache_[key] = t;
    return t;
}

void Textures::invalidate(uint32_t address, uint32_t bytes) {
    uint64_t lo = address & GuestMemory::PHYSICAL_MASK, hi = lo + bytes;
    for (auto it = cache_.begin(); it != cache_.end();) {
        bool hit = false;
        for (auto [a, n] : it->second->footprint) {
            uint64_t s = a & GuestMemory::PHYSICAL_MASK, e = s + n;
            if (s < hi && lo < e) hit = true;
            // a span past the end of physical memory wraps
            if (e > GuestMemory::SIZE && lo < e - GuestMemory::SIZE) hit = true;
        }
        if (hit)
            it = cache_.erase(it);
        else
            ++it;
    }
}

namespace {

using ucode::FetchOpcode;
using ucode::FetchOpDimension;
using ucode::TextureFilter;

// a texel index along an axis of n texels through the clamp mode; false:
// the border
bool address(int64_t i, uint32_t n, uint32_t mode, uint32_t &out) {
    int64_t size = n;
    switch (mode) {
    case 0: // repeat
        i %= size;
        if (i < 0) i += size;
        break;
    case 1: { // mirrored repeat
        int64_t period = 2 * size;
        i %= period;
        if (i < 0) i += period;
        if (i >= size) i = period - 1 - i;
        break;
    }
    case 3:
    case 5: // mirror once, then clamp to the edge (halfway as the edge, as Xenia)
        if (i < 0) i = -1 - i;
        i = std::min(i, size - 1);
        break;
    case 6: // clamp to the border
        if (i < 0 || i >= size) return false;
        break;
    case 7: // mirror once, then the border
        if (i < 0) i = -1 - i;
        if (i >= size) return false;
        break;
    default: // 2 and 4: clamp to the edge (halfway as the edge, as Xenia)
        i = std::clamp<int64_t>(i, 0, size - 1);
        break;
    }
    out = uint32_t(i);
    return true;
}

struct SamplerState {
    uint32_t mag = 0, min = 0, mip = 0, aniso = 0;
    uint32_t vol_mag = 0, vol_min = 0;
    uint32_t clamp[3] = {};
    float border[4] = {};
};

void border_color(const uint32_t fetch[6], float out[4]) {
    uint32_t color = fetch[5] & 3, alpha = (fetch[5] >> 2) & 1;
    switch (color) {
    case 1: out[0] = out[1] = out[2] = out[3] = 1.0f; return;
    case 2: out[0] = 0.5f, out[1] = 0.0f, out[2] = 0.5f; break; // (Cr, Y, Cb) black
    case 3: out[0] = 0.0f, out[1] = 0.5f, out[2] = 0.5f; break; // (Y, Cr, Cb) black
    default: out[0] = out[1] = out[2] = 0.0f; break;
    }
    out[3] = alpha ? 1.0f : 0.0f;
}

// one texel of a level's slice, or the border
void texel(const Texture::Level &level, int64_t x, int64_t y, uint32_t slice, const SamplerState &s, float out[4]) {
    uint32_t ux, uy;
    if (!address(x, level.width, s.clamp[0], ux) || !address(y, level.height, s.clamp[1], uy)) {
        std::memcpy(out, s.border, 16);
        return;
    }
    const float *t = &level.texels[((size_t(slice) * level.height + uy) * level.width + ux) * 4];
    std::memcpy(out, t, 16);
}

// a 2D sample of a level's slice at normalized (u, v)
void sample_2d(const Texture::Level &level, float u, float v, uint32_t slice, bool linear, const SamplerState &s,
               float out[4]) {
    float x = u * float(level.width), y = v * float(level.height);
    if (!linear) {
        texel(level, int64_t(std::floor(x)), int64_t(std::floor(y)), slice, s, out);
        return;
    }
    x -= 0.5f, y -= 0.5f;
    float fx = std::floor(x), fy = std::floor(y);
    float ax = x - fx, ay = y - fy;
    int64_t x0 = int64_t(fx), y0 = int64_t(fy);
    float t[4][4];
    texel(level, x0, y0, slice, s, t[0]);
    texel(level, x0 + 1, y0, slice, s, t[1]);
    texel(level, x0, y0 + 1, slice, s, t[2]);
    texel(level, x0 + 1, y0 + 1, slice, s, t[3]);
    for (int c = 0; c < 4; c++) {
        float top = t[0][c] + (t[1][c] - t[0][c]) * ax;
        float bottom = t[2][c] + (t[3][c] - t[2][c]) * ax;
        out[c] = top + (bottom - top) * ay;
    }
}

// a 3D sample of a level at normalized (u, v, w)
void sample_3d(const Texture::Level &level, float u, float v, float w, bool linear, const SamplerState &s,
               float out[4]) {
    float z = w * float(level.slices);
    auto slice = [&](int64_t k, float r[4]) {
        uint32_t uz;
        if (!address(k, level.slices, s.clamp[2], uz)) {
            std::memcpy(r, s.border, 16);
            return;
        }
        sample_2d(level, u, v, uz, linear, s, r);
    };
    if (!linear) {
        slice(int64_t(std::floor(z)), out);
        return;
    }
    z -= 0.5f;
    float fz = std::floor(z), az = z - fz;
    float a[4], b[4];
    slice(int64_t(fz), a);
    slice(int64_t(fz) + 1, b);
    for (int c = 0; c < 4; c++) out[c] = a[c] + (b[c] - a[c]) * az;
}

float length(const float *v, uint32_t n) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) sum = sum + v[i] * v[i];
    return std::sqrt(sum);
}

} // namespace

bool sample(const Texture *t, const shader::TextureFetch &fetch, float out[4], std::string &why, Counts &counts) {
    const ucode::TextureFetchInstruction &in = *fetch.instruction;
    const uint32_t *k = fetch.fetch_constant;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (in.opcode == FetchOpcode::GetTextureBorderColorFrac) {
        counts.add("getBCF answered 0 (as Xenia)");
        return true;
    }
    const bool gradients_query = in.opcode == FetchOpcode::GetTextureGradients;
    if (gradients_query) {
        // (d/dx c.x, d/dy c.x, d/dx c.y, d/dy c.y), coarse, as the translator
        const float (*q)[4] = fetch.quad;
        if (!fetch.pixel) return true;
        out[0] = q[1][0] - q[0][0], out[1] = q[2][0] - q[0][0];
        out[2] = q[1][1] - q[0][1], out[3] = q[2][1] - q[0][1];
        return true;
    }
    if (!t) {
        why = "a texture fetch without a texture";
        return false;
    }
    const texture::Info &info = t->info;
    const bool cube = in.dimension == FetchOpDimension::Cube;
    const bool volume = in.dimension == FetchOpDimension::D3OrStacked;
    const bool one = in.dimension == FetchOpDimension::D1 && in.operands[0].component_count <= 1;
    // the size by the fetch constant's dimension
    float size[3] = {float(info.width), float(info.dimension == 0 ? 1 : info.height), float(info.depth)};
    if (one) size[1] = 1.0f;
    const float epsilon = 1.5f / 1024.0f;
    const float offsets[3] = {in.offset_x + epsilon, one ? 0.0f : in.offset_y + epsilon,
                              cube     ? in.offset_z
                              : volume ? in.offset_z + epsilon
                                       : 0.0f};

    if (in.opcode == FetchOpcode::GetTextureWeights) {
        uint32_t axes = one ? 1u : volume ? 3u : 2u;
        for (uint32_t a = 0; a < axes; a++) {
            float v = in.unnormalized_coordinates ? fetch.coordinates[a] : fetch.coordinates[a] * size[a];
            float x = v + (offsets[a] - 0.5f);
            out[a] = x - std::floor(x);
        }
        return true;
    }
    if (in.opcode != FetchOpcode::TextureFetch && in.opcode != FetchOpcode::GetTextureComputedLod) {
        why = std::string("texture fetch operation ") + in.opcode_name;
        return false;
    }

    // normalized coordinates of a lane's operand: u, v, and the layer
    // (stacked), z (3D) or face (cube)
    auto normalize = [&](const float *c, float n[3]) {
        float x = c[0], y = one ? 0.0f : c[1], z = (cube || volume) ? c[2] : 0.0f;
        n[0] = in.unnormalized_coordinates ? (x + offsets[0]) / size[0] : x + offsets[0] / size[0];
        n[1] = one ? 0.0f : in.unnormalized_coordinates ? (y + offsets[1]) / size[1] : y + offsets[1] / size[1];
        if (cube) {
            // the cube instruction's face coordinates (1-2) to the face's 0-1
            n[0] = n[0] - 1.0f, n[1] = n[1] - 1.0f;
            float face = offsets[2] != 0.0f ? z + offsets[2] : z;
            n[2] = std::isnan(face) ? 0.0f : std::clamp(face, 0.0f, 5.0f);
        } else if (volume) {
            n[2] = in.unnormalized_coordinates ? z + offsets[2] : z * size[2] + offsets[2]; // in layers or slices
        } else {
            n[2] = 0.0f;
        }
    };
    float c[3];
    normalize(fetch.coordinates, c);

    SamplerState s;
    uint32_t mag = (k[3] >> 19) & 3, min = (k[3] >> 21) & 3, mip = (k[3] >> 23) & 3, aniso = (k[3] >> 25) & 7;
    if (in.mag_filter != TextureFilter::UseFetchConst) mag = uint32_t(in.mag_filter);
    if (in.min_filter != TextureFilter::UseFetchConst) min = uint32_t(in.min_filter);
    if (in.mip_filter != TextureFilter::UseFetchConst) mip = uint32_t(in.mip_filter);
    if (in.aniso_filter != ucode::AnisoFilter::UseFetchConst) aniso = uint32_t(in.aniso_filter);
    for (uint32_t a = 0; a < 3; a++) {
        s.clamp[a] = info.clamp[a];
        if (s.clamp[a] == 4 || s.clamp[a] == 5) counts.add("texture clamp to halfway (as to the edge, as Xenia)");
    }
    border_color(k, s.border);
    // the anisotropy's most taps (AnisoFilter 1-5: 1x to 16x), with linear filtering
    const uint32_t max_aniso = aniso && aniso < 6 && (mag == 1 || min == 1) ? 1u << (aniso - 1) : 1u;

    // the LOD (the translator's): the constant's bias, the register's, the
    // instruction's; with a computed LOD, the gradients times 2 to it plus
    // the per-axis exponents
    const bool computed = in.use_computed_lod && (fetch.pixel || in.use_register_gradients);
    float lod = float(int32_t(k[4] << 10) >> 22) * (1.0f / 32.0f);
    if (in.use_register_lod) lod = fetch.lod + lod;
    if (in.lod_bias != 0.0f) lod = lod + in.lod_bias;
    float exponent_h = float(int32_t(k[4] << 5) >> 27), exponent_v = float(int32_t(k[4]) >> 27);
    const uint32_t axes = volume && t->is_3d ? 3u : 2u;
    float lambda = lod;
    float gh[3] = {}, gv[3] = {};
    uint32_t taps = 1;
    float major[3] = {}; // the anisotropic footprint's long axis, normalized
    if (computed) {
        if (in.use_register_gradients) {
            for (uint32_t a = 0; a < 3; a++) {
                // in texels: the registers' are normalized unless the coordinates are not
                float sz = a < 2 ? size[a] : size[2];
                gh[a] = in.unnormalized_coordinates && !cube ? fetch.gradient_h[a] : fetch.gradient_h[a] * sz;
                gv[a] = in.unnormalized_coordinates && !cube ? fetch.gradient_v[a] : fetch.gradient_v[a] * sz;
            }
        } else {
            float n0[3], n1[3], n2[3];
            normalize(fetch.quad[0], n0);
            normalize(fetch.quad[1], n1);
            normalize(fetch.quad[2], n2);
            for (uint32_t a = 0; a < 2; a++) {
                gh[a] = (n1[a] - n0[a]) * size[a];
                gv[a] = (n2[a] - n0[a]) * size[a];
            }
            if (axes == 3) gh[2] = n1[2] - n0[2], gv[2] = n2[2] - n0[2]; // already in slices
        }
        if (cube && !in.use_register_gradients) {
            float rho = std::max(length(gh, 2), length(gv, 2));
            lambda = std::log2(rho) + (lod + std::max(exponent_h, exponent_v));
        } else {
            float scale_h = std::exp2(lod + exponent_h), scale_v = std::exp2(lod + exponent_v);
            for (uint32_t a = 0; a < 3; a++) gh[a] = gh[a] * scale_h, gv[a] = gv[a] * scale_v;
            float lh = length(gh, axes), lv = length(gv, axes);
            lambda = std::log2(std::max(lh, lv));
            // anisotropic: as many taps along the long axis as its length
            // over the short one's (up to the most), the LOD of the long
            // one over them (the Vulkan and Direct3D 10 reference footprint)
            float rmax = std::max(lh, lv), rmin = std::min(lh, lv);
            if (max_aniso > 1 && axes == 2 && rmin > 0.0f && rmax > rmin) {
                float n = std::min(std::ceil(rmax / rmin), float(max_aniso));
                taps = uint32_t(n);
                lambda = std::log2(rmax / n);
                const float *g = lh >= lv ? gh : gv;
                for (uint32_t a = 0; a < 2; a++) major[a] = g[a] / size[a];
            }
        }
    }
    if (in.opcode == FetchOpcode::GetTextureComputedLod) {
        out[0] = fetch.pixel && in.use_computed_lod && !in.use_register_gradients ? lambda : 0.0f;
        return true;
    }

    // the level: the LOD clamped to the constant's mips (a base map at the
    // lowest), magnifying at or below 0
    float lo = float(t->mip_min), hi = float(t->mip_max);
    float clamped = std::isnan(lambda) ? lo : std::clamp(lambda, lo, mip == 2 ? lo : hi);
    bool linear = clamped <= 0.0f ? mag == 1 : min == 1;
    auto level_sample = [&](uint32_t level, float r[4]) {
        level = std::min<uint32_t>(level, uint32_t(t->levels.size() - 1));
        // a level the constant lacks: the nearest one it has
        while (level > 0 && t->levels[level].empty()) level--;
        while (level + 1 < t->levels.size() && t->levels[level].empty()) level++;
        const Texture::Level &l = t->levels[level];
        if (l.empty()) {
            r[0] = r[1] = r[2] = r[3] = 0.0f;
            return;
        }
        if (volume && t->is_3d) {
            sample_3d(l, c[0], c[1], c[2] / size[2], linear, s, r);
        } else if (cube || (volume && !t->is_3d)) {
            uint32_t layers = l.slices;
            if (cube) {
                sample_2d(l, c[0], c[1], std::min<uint32_t>(uint32_t(c[2]), layers - 1), linear, s, r);
                return;
            }
            // stacked: the layers' filter, the volume filter (the
            // instruction's or the constant's), two layers blended
            uint32_t vmag =
                in.vol_mag_filter == TextureFilter::UseFetchConst ? (k[4] & 1) : uint32_t(in.vol_mag_filter);
            uint32_t vmin =
                in.vol_min_filter == TextureFilter::UseFetchConst ? ((k[4] >> 1) & 1) : uint32_t(in.vol_min_filter);
            bool layer_linear = (clamped <= 0.0f ? vmag : vmin) == 1;
            float layer =
                in.unnormalized_coordinates ? c[2] : std::clamp(std::isnan(c[2]) ? 0.0f : c[2], 0.0f, size[2] - 1.0f);
            float coordinate = layer_linear ? layer - 0.5f : layer;
            float l0 = std::floor(coordinate);
            auto layer_at = [&](float index) {
                int64_t i = int64_t(index);
                return uint32_t(std::clamp<int64_t>(i, 0, int64_t(layers) - 1));
            };
            sample_2d(l, c[0], c[1], layer_at(l0), linear, s, r);
            if (layer_linear) {
                float b[4];
                sample_2d(l, c[0], c[1], layer_at(l0 + 1.0f), linear, s, b);
                float f = coordinate - l0;
                for (int i = 0; i < 4; i++) r[i] = r[i] + (b[i] - r[i]) * f;
            }
        } else {
            sample_2d(l, c[0], c[1], 0, linear, s, r);
        }
    };
    float value[4] = {}, sum[4] = {};
    const float center[2] = {c[0], c[1]};
    for (uint32_t tap = 0; tap < taps; tap++) {
        if (taps > 1) {
            float f = (float(tap) + 0.5f) / float(taps) - 0.5f;
            c[0] = center[0] + major[0] * f, c[1] = center[1] + major[1] * f;
        }
        if (mip == 1 && clamped > 0.0f) {
            // linear between mips
            float l0 = std::floor(clamped);
            float f = clamped - l0;
            level_sample(uint32_t(l0), value);
            if (f > 0.0f) {
                float b[4];
                level_sample(uint32_t(l0) + 1, b);
                for (int i = 0; i < 4; i++) value[i] = value[i] + (b[i] - value[i]) * f;
            }
        } else {
            // the nearest mip (a base map: the lowest)
            uint32_t level = mip == 2 ? t->mip_min : uint32_t(std::floor(clamped + 0.5f));
            if (mip != 2 && clamped > 0.0f && clamped - std::floor(clamped) == 0.5f)
                level = uint32_t(std::floor(clamped));
            level_sample(level, value);
        }
        for (int i = 0; i < 4; i++) sum[i] = tap ? sum[i] + value[i] : value[i];
    }
    if (taps > 1)
        for (int i = 0; i < 4; i++) value[i] = sum[i] / float(taps);
    // the fetch constant's swizzle, its number format (the translator's
    // texture_signs, Xenia's: texture::integer_scale_bits) and the exponent
    // adjust
    float scale = std::ldexp(1.0f, info.exp_adjust);
    const uint32_t bits = texture::integer_scale_bits(info, texture::swizzled_signs(info));
    // (Xenia's AllowsPointSampling: no filter of the instruction's linear)
    const bool point_allowed =
        in.mag_filter != TextureFilter::Linear && in.min_filter != TextureFilter::Linear &&
        in.mip_filter != TextureFilter::Linear &&
        (!in.use_computed_lod || in.aniso_filter == ucode::AnisoFilter::Disabled ||
         in.aniso_filter == ucode::AnisoFilter::UseFetchConst);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t sw = (info.swizzle >> (3 * i)) & 7;
        float v = sw < 4 ? value[sw] : sw == 5 ? 1.0f : 0.0f;
        const uint32_t lane = (bits >> (6 * i)) & 0x3F, width_less_one = lane & 0xF, sign = lane >> 4;
        if (bits & ((1u << 26) - 1)) {
            if (bits & (1u << 24)) {
                // as fractions: 4-6 bit unsigned components point-sampled
                // rebuilt as the console expands them, n (2^w + 1) / 2^2w;
                // unsigned ones to Q16; biased ones no lower than -1
                if (point_allowed && (bits & ((1u << 24) - 1)) && lane - 1 < 15) {
                    const float pow2 = float(2u << width_less_one);
                    const float texel = std::nearbyint(v * (pow2 - 1.0f));
                    v = texel * (pow2 + 1.0f) / (pow2 * pow2);
                }
                if (sign == 0) v = std::nearbyint(v * 65536.0f) / 65536.0f;
                if (sign == 2) v = std::max(v, -1.0f);
            } else {
                // as integers: times 2^w - 1, or 2^(w-1) - 1 signed or biased
                // (a 1-bit biased one times a half, less a half), rounded
                // when the fetch and the constant both sample by point
                const uint32_t shift = width_less_one + 1 - (sign - 1 < 2 ? 1u : 0u);
                const float s = float((1u << shift) - 1);
                v = v * std::max(s, 0.5f) + (std::min(s, 0.5f) - 0.5f);
                if (point_allowed && (bits & (1u << 26))) v = std::nearbyint(v);
            }
        }
        out[i] = v * scale;
    }
    return true;
}

} // namespace xf::reference
