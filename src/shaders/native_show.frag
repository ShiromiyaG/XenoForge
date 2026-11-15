// The Drawer's target viewer (native_draw.cpp: present_target, XF_DRAWER_SHOW): a target's color as it is, or
// its first component as grey (depth), times a scale.
#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    float scale;
    int grey;
} pass;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec4 c = texture(source, uv);
    color = pass.grey != 0 ? vec4(vec3(c.r * pass.scale), 1.0) : vec4(c.rgb * pass.scale, 1.0);
}
