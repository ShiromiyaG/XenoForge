// The Drawer's target viewer (native_draw.cpp, XF_DRAWER_SHOW): a target's first component, scaled, as grey.
#version 450
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    float scale;
} pass;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = vec4(vec3(texture(source, uv).r * pass.scale), 1.0); }
