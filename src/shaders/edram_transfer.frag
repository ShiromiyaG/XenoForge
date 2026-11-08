// A render target's samples drawn straight into another that takes over its
// EDRAM tiles (renderer.cpp's acquire), in one pass, as Xenia's render target
// cache transfers ownership (vulkan_render_target_cache.cc's
// GetTransferShader, d3d12_render_target_cache.cc's transfer pixel shaders):
// each destination sample finds its dword in EDRAM by the destination's
// layout (edram.glsl's edram_offset, Xenia's XeEdramOffsetBytes), the
// source's pixel and sample holding that dword by the source's, and takes
// the source's value through the guest's bits. What it writes is what a
// store of the source into EDRAM followed by a load into the destination
// writes (edram_store.comp, edram_load.frag), so the two ways give the same
// pictures; EDRAM itself is left as it was.
//
// A source whose depth the host encodes differently (D24FS8, and D24S8 where
// the device has no 24-bit unorm depth) leaves its 32-bit depth beside EDRAM
// as its store does (host_depth_offset), and a destination of such a format
// takes the host's depth back where it still rounds to the guest's bits
// (Xenia's kDepthAndHostDepthToDepth and kColorAndHostDepthToDepth modes;
// here the host depth lives in a buffer rather than in the last depth target).
//
// MS: the source is multisampled. DEPTH: the destination is a depth target,
// STENCIL with the stencil exported; without, the stencil is written a bit
// per pass, the passes for the bits not set discarded (as Xenia does), or
// with STENCIL_COPY written into an R8_UINT image the renderer copies into
// the stencil aspect after the pass (VK_KHR_maintenance8). UINT:
// the destination is a float target written by its bits through its UINT
// view. BSD; see LICENSE-xenia.
#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#ifdef STENCIL
#extension GL_ARB_shader_stencil_export : require
#endif

#define EDRAM_TWO_TARGETS
#include "edram.glsl"

// the source's EDRAM store set (renderer.cpp's RenderTarget::store_set)
#ifdef MS
layout(set = 0, binding = 2) uniform texture2DMS source_image;  // color, or depth
layout(set = 0, binding = 3) uniform utexture2DMS source_uint;  // a depth source's stencil, a float source's bits
#else
layout(set = 0, binding = 2) uniform texture2D source_image;
layout(set = 0, binding = 3) uniform utexture2D source_uint;
#endif
layout(set = 0, binding = 4) buffer HostDepth { uint words[]; } host_depth;

layout(push_constant) uniform Transfer {
    EdramParams dest, source;
} p;

#if defined(STENCIL_COPY)
layout(location = 0) out uint stencil_out;
#elif defined(UINT)
layout(location = 0) out uvec4 color;
#elif !defined(DEPTH)
layout(location = 0) out vec4 color;
#endif

// The host's depth of the sample a depth source holds, for a destination of
// the same format (flags bit 0 on both)
float source_depth = 0.0;
bool source_depth_valid = false;

// The guest's bits of the source's sample that holds EDRAM dword `at`, and
// which of its dwords `at` is (the inverse of edram_offset). `target` is the
// source's while it runs.
uvec2 source_bits(uint at, out uint half_index) {
    uint tile = at / TILE_DWORDS, in_tile = at % TILE_DWORDS;
    uint relative = (tile + 2048u - target.base_tiles) % 2048u;
    uvec2 tile_size = uvec2(80u >> target.ints_log2, 16u);
    uint index = in_tile >> target.ints_log2;
    half_index = in_tile & target.ints_log2;
    uvec2 s = uvec2(index % tile_size.x, index / tile_size.x);
    if (target.is_depth != 0u) {
        uint half_width = tile_size.x >> 1u;
        s.x = s.x >= half_width ? s.x - half_width : s.x + half_width;
    }
    s += uvec2(relative % target.pitch_tiles, relative / target.pitch_tiles) * tile_size;
    uvec2 pixel;
    uint sample_index;
    if (target.msaa >= 2u) {
        uvec2 offset = (s >> 1u) & 1u;
        pixel = ((s >> 2u) << 1u) | (s & 1u);
        sample_index = offset.x | (offset.y << 1u);
    } else if (target.msaa == 1u) {
        sample_index = (s.x >> 1u) & 1u;
        pixel = uvec2((s.x & ~2u) | (s.y & 2u), ((s.y >> 2u) << 1u) | (s.y & 1u));
    } else {
        sample_index = 0u;
        pixel = s;
    }
    int host = int(guest_sample(sample_index));
    ivec2 xy = ivec2(pixel);
#ifdef MS
    vec4 value = texelFetch(source_image, xy, host);
#else
    vec4 value = texelFetch(source_image, xy, 0);
#endif
    if (target.is_depth != 0u) {
#ifdef MS
        uint s8 = texelFetch(source_uint, xy, host).r;
#else
        uint s8 = texelFetch(source_uint, xy, 0).r;
#endif
        // the source's store keeps its host depth beside EDRAM
        if ((target.flags & 1u) != 0u) {
            host_depth.words[host_depth_offset(at)] = floatBitsToUint(value.r);
            source_depth = value.r;
            source_depth_valid = true;
        }
        return uvec2(pack_depth(value.r, s8), 0u);
    }
    if ((target.flags & 8u) != 0u) {
#ifdef MS
        return pack_raw(texelFetch(source_uint, xy, host));
#else
        return pack_raw(texelFetch(source_uint, xy, 0));
#endif
    }
    return pack_color(value);
}

void main() {
    target = p.dest;
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    uint at = edram_offset(pixel, guest_sample(uint(gl_SampleID)));
    bool wide = target.ints_log2 != 0u;
    // the destination's dwords from the source
    target = p.source;
    uint half_index;
    uvec2 bits = source_bits(at, half_index);
    uint low = half_index != 0u ? bits.y : bits.x, high = 0u;
    if (wide) {
        if (target.ints_log2 != 0u) {
            high = bits.y; // the same 64-bit sample (at is its first dword)
        } else {
            uint unused;
            high = source_bits((at + 1u) % EDRAM_DWORDS, unused).x;
        }
    }
    target = p.dest;
#ifdef DEPTH
    float depth = unpack_depth(low);
    // as edram_load.frag: the host's depth where it still rounds to EDRAM's bits
    if ((target.flags & 1u) != 0u && target.stencil_bit >= 8u) {
        float host;
        if (source_depth_valid && p.source.format == target.format)
            host = source_depth;
        else
            host = uintBitsToFloat(host_depth.words[host_depth_offset(at)]);
        if (depth_to_guest(host) == (low >> 8u)) depth = host;
    }
    gl_FragDepth = depth;
#if defined(STENCIL)
    gl_FragStencilRefARB = int(low & 0xFFu);
#elif defined(STENCIL_COPY)
    stencil_out = low & 0xFFu;
#else
    if (target.stencil_bit < 8u && ((low >> target.stencil_bit) & 1u) == 0u) discard;
#endif
#else
#ifdef UINT
    color = unpack_raw(uvec2(low, high));
#else
    color = unpack_color(uvec2(low, high));
#endif
#endif
}
