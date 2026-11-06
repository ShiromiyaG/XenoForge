// The pixel shader of a draw without one (depth only) into a D24FS8 target
// whose depth the pixel shaders convert (Options::depth_float24_convert_in_
// pixel_shader): the rasterized depth, from the host's 0-0.5 back to 0-1,
// rounded to 20e4 and halved again, so the target holds what EDRAM's 24 bits
// can, as the guest's pixel shaders write it (shader_translator.h's
// PixelOptions). Xenia's float24_truncate and float24_round fragment shaders
// (vulkan_pipeline_cache.cc, spirv_shader_translator_rb.cc's depth-only
// shaders); ROUND: to the nearest even, else towards zero. BSD; see
// LICENSE-xenia.
#version 450

#ifndef ROUND
layout(depth_less) out float gl_FragDepth;
#endif

void main() {
    float depth = clamp(gl_FragCoord.z * 2.0, 0.0, 1.0);
    uint bits = floatBitsToUint(depth);
#ifdef ROUND
    // xenos::Float32To20e4 rounding to the nearest even, then Float20e4To32
    uint f24 = 0u;
    if (depth > 0.0) {
        f24 = bits < 0x38800000u ? ((bits & 0x7FFFFFu) | 0x800000u) >> min(113u - (bits >> 23u), 24u)
                                 : bits + 0xC8000000u;
        f24 += 3u + ((f24 >> 3u) & 1u);
        f24 = (f24 >> 3u) & 0xFFFFFFu;
    }
    float result = 0.0;
    if (f24 != 0u) {
        uint mantissa = f24 & 0xFFFFFu;
        int exponent = int(f24 >> 20u);
        if (exponent == 0) {
            int lz = 20 - findMSB(mantissa);
            exponent = 1 - lz;
            mantissa = (mantissa << uint(lz)) & 0xFFFFFu;
        }
        result = uintBitsToFloat((uint(exponent + 112) << 23u) | (mantissa << 3u)) * 0.5;
    }
    gl_FragDepth = result;
#else
    // towards zero: the mantissa's low bits dropped (3 for a normal 20e4, up
    // to 23 at the exponent -34), 0 below 2^-34
    float result = 0.0;
    if (bits >= 0x2E800000u) {
        int drop = max(116 - int((bits >> 23u) & 0xFFu), 3);
        result = uintBitsToFloat(bitfieldInsert(bits, 0u, 0, drop)) * 0.5;
    }
    gl_FragDepth = result;
#endif
}
