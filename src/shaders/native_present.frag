// The Drawer's present (native_draw.cpp): a 2D array's first layer (Frame::texture's view of the guest's front
// buffer, or a target's) over the output image.
#version 450
layout(set = 0, binding = 0) uniform sampler2DArray source;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = texture(source, vec3(uv, 0.0)); }
