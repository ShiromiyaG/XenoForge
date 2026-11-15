// The Drawer's resolves (native_draw.cpp): a single-sampled target read as a multisampled view of the same
// EDRAM tiles (the console's downsample: a resolve averaging a 2x or 4x view's samples): each pixel of the view
// from its samples' pixels (4x: sample s at (s & 1, s >> 1) of the 2x2 block, 2x: at (0, s) of the 1x2), the
// ones the resolve selects, averaged.
#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    int samples_x, samples_y; // the view's samples across and down (2 2: 4x; 1 2: 2x)
    int first, count;         // the samples taken
} pass;
layout(location = 0) out vec4 color;

void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * ivec2(pass.samples_x, pass.samples_y);
    vec4 sum = vec4(0.0);
    for (int s = pass.first; s < pass.first + pass.count; s++) {
        ivec2 at = pass.samples_x == 2 ? ivec2(s & 1, s >> 1) : ivec2(0, s & 1);
        sum += texelFetch(source, base + at, 0);
    }
    color = sum / float(pass.count);
}
