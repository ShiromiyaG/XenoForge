// The recompile step's shader step (shader_step.h).

#include "xenoforge/shader_step.h"

#include "xenoforge/renderer.h"
#include "xenoforge/shader_storage.h"
#include "xenoforge/vk_device.h"

#include <cstdio>
#include <filesystem>

namespace xf::render {

std::vector<std::string> run_shader_step(const build::ShaderStep &step) {
    std::vector<std::string> lines;
    char line[512];
    if (!step.disc_shaders.empty() || step.disc_files) {
        std::snprintf(line, sizeof(line), "the disc: %zu shaders in %zu files read (%.1f s)", step.disc_shaders.size(),
                      step.disc_files, step.disc_seconds);
        lines.push_back(line);
    }
    const std::filesystem::path storage = ShaderStorage::path(step.cache_dir, step.title_id);
    std::error_code ec;
    if (!std::filesystem::exists(storage, ec) && step.disc_shaders.empty()) {
        lines.push_back("skipped: no shader storage yet (" + storage.filename().string() +
                        ": a run of the game writes it)");
        return lines;
    }
    std::string error;
    std::unique_ptr<vk::Device> device = vk::Device::open(error);
    if (!device) {
        lines.push_back("skipped: no Vulkan device (" + error + ")");
        return lines;
    }
    // the renderer's options as a run takes them (the player's and boot's
    // defaults), with the title's storage and pipeline cache
    Options options;
    options.shader_storage = storage;
    options.pipeline_cache = ShaderStorage::pipeline_cache_path(step.cache_dir, step.title_id);
    PrecompileOptions precompile_options;
    precompile_options.extra_shaders = step.disc_shaders;
    precompile_options.cancel = step.cancel;
    PrecompileReport r;
    const bool ok = precompile(*device, options, precompile_options, r, error);
    std::snprintf(line, sizeof(line), "device: %s; pipeline cache %s (%.1f KB read, %.1f KB written)",
                  device->name().c_str(),
                  r.cache_matched ? "this device's and driver's" : r.cache_read ? "another's, made again" : "new",
                  r.cache_read / 1024.0, r.cache_written / 1024.0);
    lines.push_back(line);
    std::snprintf(line, sizeof(line),
                  "shaders: %llu from runs (%llu not translated), %llu from the disc (%llu not met in runs, %llu "
                  "vertex shaders awaiting a vertex declaration, %llu not translated)",
                  (unsigned long long)r.shaders, (unsigned long long)r.shaders_failed,
                  (unsigned long long)r.extra_shaders, (unsigned long long)r.extra_new,
                  (unsigned long long)r.extra_undeclared, (unsigned long long)r.extra_failed);
    lines.push_back(line);
    std::snprintf(line, sizeof(line),
                  "pipelines: %llu described by runs (%llu made before, %llu not made here); made %llu libraries "
                  "(%llu failed) and %llu whole pipelines (%llu failed) in %.1f s (%.1f s of the driver's on the "
                  "workers)",
                  (unsigned long long)r.descriptions, (unsigned long long)r.descriptions_done_before,
                  (unsigned long long)r.descriptions_failed, (unsigned long long)r.libraries,
                  (unsigned long long)r.libraries_failed, (unsigned long long)r.pipelines,
                  (unsigned long long)r.pipelines_failed, r.seconds, r.driver_seconds);
    lines.push_back(line);
    if (!ok) lines.push_back("stopped: " + error);
    return lines;
}

} // namespace xf::render
