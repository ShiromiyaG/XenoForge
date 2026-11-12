// The recompile step's shader step (shader_step.h).

#include "xenoforge/shader_step.h"

#include "xenoforge/renderer.h"
#include "xenoforge/shader_storage.h"
#include "xenoforge/vk_device.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xf::render {
namespace {

namespace fs = std::filesystem;

// a file by its size and time ("" when it is not there)
std::string file_stamp(const fs::path &p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec) return {};
    const auto time = fs::last_write_time(p, ec);
    if (ec) return {};
    return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}

// the program this runs in (the launcher, xfbuild), by its file: another
// XenoForge (its translator, its pipelines' state) makes other pipelines
std::string program_stamp() {
#ifdef _WIN32
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&program_stamp), &self))
        return {};
    wchar_t path[4096];
    const DWORD n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (!n || n >= std::size(path)) return {};
    return file_stamp(fs::path(std::wstring(path, n)));
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : file_stamp(self);
#endif
}

uint64_t mix(uint64_t key, std::string_view text) {
    for (unsigned char ch : text) key = (key ^ ch) * 0x100000001B3ull;
    return (key ^ 0xFF) * 0x100000001B3ull;
}

// What the step would start from: this program, the title's storage and
// pipeline cache (both written by runs and by the step), the disc's shaders
// and the device and driver. The same as when the step last finished: it
// would make nothing (every pipeline is in the record) and only translate
// the shaders again, so it is skipped. "" when something cannot be told.
std::string step_key(const build::ShaderStep &step, const fs::path &storage, const fs::path &cache,
                     const vk::Device &device) {
    const std::string program = program_stamp(), stored = file_stamp(storage);
    if (program.empty() || stored.empty()) return {};
    uint64_t key = mix(0xCBF29CE484222325ull, "xf shader step 2");
    key = mix(key, program);
    key = mix(key, stored);
    key = mix(key, file_stamp(cache));
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(device.physical(), &p);
    key = mix(key, std::to_string(p.vendorID) + ":" + std::to_string(p.deviceID) + ":" +
                       std::to_string(p.driverVersion));
    key = mix(key, std::string_view(reinterpret_cast<const char *>(p.pipelineCacheUUID), VK_UUID_SIZE));
    key = mix(key, std::to_string(step.disc_variants));
    key = mix(key, std::to_string(step.disc_shaders.size()));
    for (const auto &[type, code] : step.disc_shaders) {
        const uint64_t h = ShaderStorage::hash(type, code);
        key = mix(key, std::string_view(reinterpret_cast<const char *>(&h), sizeof(h)));
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%016llX\n", (unsigned long long)key);
    return text;
}

std::string read_text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

} // namespace

std::vector<std::string> run_shader_step(const build::ShaderStep &step) {
    std::vector<std::string> lines;
    char line[512];
    if (!step.disc_shaders.empty() || step.disc_files) {
        if (step.disc_from_earlier)
            std::snprintf(line, sizeof(line),
                          "the disc: %zu shaders in %zu files (an earlier step's search, of %.1f s, kept)",
                          step.disc_shaders.size(), step.disc_files, step.disc_seconds);
        else
            std::snprintf(line, sizeof(line), "the disc: %zu shaders in %zu files read (%.1f s)",
                          step.disc_shaders.size(), step.disc_files, step.disc_seconds);
        lines.push_back(line);
    }
    const fs::path storage = ShaderStorage::path(step.cache_dir, step.title_id);
    std::error_code ec;
    if (!fs::exists(storage, ec) && step.disc_shaders.empty()) {
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
    // nothing new since the step last finished: nothing to make
    fs::path stamp = storage;
    stamp.replace_extension(".shaderstep");
    if (const std::string key = step_key(step, storage, options.pipeline_cache, *device);
        !key.empty() && read_text(stamp) == key) {
        lines.push_back("unchanged since the last shader step (this XenoForge, the storage, the pipeline cache, "
                        "the disc's shaders and the device the same): nothing to make");
        return lines;
    }
    PrecompileOptions precompile_options;
    precompile_options.extra_shaders = step.disc_shaders;
    precompile_options.cancel = step.cancel;
    precompile_options.extra_variants = step.disc_variants;
    PrecompileReport r;
    bool ok = precompile(*device, options, precompile_options, r, error);
    // runs on the other render target path (the player's setting) described
    // pipelines too: the generic shaders' libraries in their states are made
    // by a renderer on that path, what the first made known from its record
    if (ok && r.descriptions_other_path) {
        Options other = options;
        other.render_target_path = render_target_path(*device, options) == Options::RenderTargetPath::fsi
                                       ? Options::RenderTargetPath::host
                                       : Options::RenderTargetPath::fsi;
        if (render_target_path(*device, other) == other.render_target_path) {
            PrecompileOptions again = precompile_options;
            again.extra_shaders.clear(); // (their libraries: made above)
            PrecompileReport r2;
            ok = precompile(*device, other, again, r2, error);
            r.libraries += r2.libraries, r.libraries_failed += r2.libraries_failed;
            r.pipelines += r2.pipelines, r.pipelines_failed += r2.pipelines_failed;
            r.generic_libraries += r2.generic_libraries, r.generic_libraries_failed += r2.generic_libraries_failed;
            r.seconds += r2.seconds, r.driver_seconds += r2.driver_seconds;
            r.cache_written = r2.cache_written ? r2.cache_written : r.cache_written;
        }
    }
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
    for (const auto &[why, count] : r.descriptions_failed_why) {
        std::snprintf(line, sizeof(line), "  not made here: %llu, %s", (unsigned long long)count, why.c_str());
        lines.push_back(line);
    }
    if (r.generic_libraries || r.generic_libraries_failed) {
        std::snprintf(line, sizeof(line),
                      "the generic shaders: %llu libraries in the descriptions' states (%llu failed), so a shader no "
                      "run met draws at once in them",
                      (unsigned long long)r.generic_libraries, (unsigned long long)r.generic_libraries_failed);
        lines.push_back(line);
    }
    if (r.variant_states_seen) {
        std::snprintf(line, sizeof(line),
                      "the disc's shaders not met in runs: in the %llu commonest of %llu states their parts take, "
                      "%llu libraries made (%llu failed, %llu made before, %llu forms not made for the shader)",
                      (unsigned long long)r.variant_states, (unsigned long long)r.variant_states_seen,
                      (unsigned long long)r.extra_libraries, (unsigned long long)r.extra_libraries_failed,
                      (unsigned long long)r.extra_libraries_done_before, (unsigned long long)r.extra_libraries_unmade);
        lines.push_back(line);
        if (r.variant_check_libraries) {
            std::snprintf(line, sizeof(line),
                          "  checked on the disc's shaders runs met: %llu of their %llu libraries in those states",
                          (unsigned long long)r.variant_check_covered, (unsigned long long)r.variant_check_libraries);
            lines.push_back(line);
        }
    }
    if (!ok) {
        lines.push_back("stopped: " + error);
    } else {
        // the files as the step left them (it wrote the cache and the record)
        const std::string key = step_key(step, storage, options.pipeline_cache, *device);
        if (!key.empty()) std::ofstream(stamp, std::ios::binary | std::ios::trunc) << key;
    }
    return lines;
}

} // namespace xf::render
