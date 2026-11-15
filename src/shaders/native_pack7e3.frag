// The Drawer's resolves (native_draw.cpp): a 2_10_10_10_FLOAT (7e3) target, kept as 16-bit floats, as the
// 2_10_10_10 target EDRAM's bits are when the guest reads the same tiles through that format: each color's 7e3
// bits (xenos.h's Float32To7e3) and the alpha's two bits, written as unorm into an A2B10G10R10 image (exact: a
// 10-bit code n is n / 1023).
#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;

uint to_7e3(float f) {
    if (!(f > 0.0)) return 0u;
    uint u = min(floatBitsToUint(f), 0x41FF0000u);
    uint biased;
    if (u < 0x3E800000u)
        biased = ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23), 24u);
    else
        biased = u + 0xC2000000u;
    return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

void main() {
    vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    color = vec4(to_7e3(c.r) / 1023.0, to_7e3(c.g) / 1023.0, to_7e3(c.b) / 1023.0,
                 round(clamp(c.a, 0.0, 1.0) * 3.0) / 3.0);
}
