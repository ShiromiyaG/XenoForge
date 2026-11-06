// The pixel shader a draw is drawn with while its own pipeline is made on a
// worker thread (Options::pipeline_stand_in placeholder): transparent black,
// as Xenia's placeholder.ps.glsl (its Vulkan backend's pipeline hot-swap).
// Xenia's writes oC0 alone; this one writes all four color outputs, the
// guest's pixel shaders' array, so no bound target gets undefined values.
// BSD; see LICENSE-xenia.
#version 450

layout(location = 0) out vec4 colors[4];

void main() {
    for (int i = 0; i < 4; i++) colors[i] = vec4(0.0);
}
