// The recompile step's shader step (game_build.h's BuildOptions::shader_step;
// docs/renderer.md, "Shaders and pipelines at the launcher's build step"):
// render::precompile on a Vulkan device of its own, for the title's storage
// and pipeline cache in the cache directory, with the disc's shaders. Never
// fails: without a device, or when the step cannot run, its lines say why.
#pragma once

#include "xenoforge/game_build.h"

#include <string>
#include <vector>

namespace xf::render {

std::vector<std::string> run_shader_step(const build::ShaderStep &step);

} // namespace xf::render
