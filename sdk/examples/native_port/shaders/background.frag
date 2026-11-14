#version 450
// the guest's front buffer (a texture the renderer's cache loaded and
// detiled: Frame::texture), tinted

layout(set = 0, binding = 0) uniform sampler2DArray front;
layout(push_constant) uniform Push { vec4 tint; } pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() { out_color = vec4(texture(front, vec3(uv, 0.0)).rgb * pc.tint.rgb, 1.0); }
