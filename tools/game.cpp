// What boot and the player share (game.h).

#include "game.h"

#include "xenoforge/command_processor.h"
#include "xenoforge/crash.h"
#include "xenoforge/disc.h"
#include "xenoforge/game_build.h"
#include "xenoforge/game_library.h"
#include "xenoforge/hooks.h"
#include "xenoforge/lockstep.h"
#include "xenoforge/native_render.h"
#include "xenoforge/shader_storage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
// after windows.h
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace xf::game {
namespace {

// for the fault report
Game *the_game = nullptr;

bool load(const std::string &input, const std::string &xex_path, Xex &xex, std::string &error) {
    std::vector<uint8_t> file;
    if (input.ends_with(".xex") || input.ends_with(".XEX")) {
        std::ifstream f(input, std::ios::binary);
        if (!f) return error = "cannot read " + input, false;
        file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } else {
        auto disc = Disc::open(input, error);
        if (!disc) return false;
        const DiscEntry *entry = disc->find(xex_path);
        if (!entry) return error = "no " + xex_path + " on the disc", false;
        file = disc->read_all(*entry);
    }
    return parse_xex(file, xex, error) && load_xex_image(file, xex, error);
}

std::filesystem::path crash_directory = ".";

// What boot and the player do after the kernel's crash handler wrote the
// report (kernel/crash.h): print it, the run's report, and the summary.
void on_crash(kernel::Kernel &, kernel::CrashReport &report, void *) {
    std::printf("fault: %s\n%s", report.what.c_str(), report.text.c_str());
    if (!report.file.empty()) std::printf("crash report written to %s\n", report.file.string().c_str());
    // the renderer's report only if no call of its is under way (this may be its thread)
    if (the_game) {
        print_report(*the_game, false);
        print_summary(*the_game, "crash", report.what, report.stack, report.file.string());
    }
    std::fflush(stdout);
}

// the renderer's counts: draws drawn and skipped by reason, what is
// approximate, resolves, swaps, the time it took
void print_renderer_report(const render::Report &r) {
    std::printf("renderer: %llu draws (%llu drawn), %llu resolves, %llu swaps (%llu front buffers written)\n",
                (unsigned long long)r.draws, (unsigned long long)r.drawn, (unsigned long long)r.resolves,
                (unsigned long long)r.swaps, (unsigned long long)r.frames);
    // a port's own frames (native_render.h), when it drew or yielded any
    if (const native::Counters n = native::counters(); n.frames_native || n.frames_yielded)
        std::printf("renderer: the port drew %llu frames, yielded %llu (%llu held); skipped %llu draws and %llu "
                    "resolves, kept %llu and %llu by its filter, %llu exporting memory; %llu occlusion reports faked\n",
                    (unsigned long long)n.frames_native, (unsigned long long)n.frames_yielded,
                    (unsigned long long)n.frames_held, (unsigned long long)n.draws_skipped,
                    (unsigned long long)n.resolves_skipped, (unsigned long long)n.draws_kept,
                    (unsigned long long)n.resolves_kept, (unsigned long long)n.memexport_kept,
                    (unsigned long long)n.occlusion_faked);
    std::printf("renderer: %.1f s spent: %.1f s in submissions (%.1f s waiting for the GPU), %.1f s in resolves, "
                "%.1f s checking textures, %.1f s decoding them, %.1f s on vertex data (%.1f MB uploaded), %.1f s "
                "copying %.1f MB that resolves wrote into memory\n",
                r.seconds, r.submit_seconds, r.wait_seconds, r.resolve_seconds, r.check_seconds, r.decode_seconds,
                r.vertex_seconds, double(r.vertex_bytes) / (1 << 20), r.writeback_seconds,
                double(r.writeback_bytes) / (1 << 20));
    std::printf("renderer: %llu submissions, %llu waits for the GPU\n", (unsigned long long)r.submissions,
                (unsigned long long)r.waits);
    std::printf("renderer: made %llu render targets, %llu textures, %llu pipelines, %llu shaders (%llu shader loads "
                "from unwritten words, nothing copied)\n",
                (unsigned long long)r.render_targets, (unsigned long long)r.textures, (unsigned long long)r.pipelines,
                (unsigned long long)r.shaders, (unsigned long long)r.shader_loads_again);
    std::printf("renderer: texture loads: %llu new, %llu again after the CPU wrote their memory, %llu after a resolve "
                "or a memory export did; %llu resolves wrote the texture they feed (%llu table places taken back)\n",
                (unsigned long long)r.texture_loads_new, (unsigned long long)r.texture_loads_cpu_written,
                (unsigned long long)r.texture_loads_gpu_written, (unsigned long long)r.resolve_targets_written,
                (unsigned long long)r.resolve_targets_reclaimed);
    std::printf(
        "renderer: texture cache: %llu textures (%llu views) held in %.1f MB (at most %.1f MB), %llu destroyed by the "
        "memory limits, %llu to make room in a bindless table, %llu loads refused for want of it\n",
        (unsigned long long)r.textures_held, (unsigned long long)r.texture_views, double(r.texture_bytes) / (1 << 20),
        double(r.texture_bytes_peak) / (1 << 20), (unsigned long long)r.textures_evicted,
        (unsigned long long)r.textures_evicted_for_room, (unsigned long long)r.textures_refused);
    std::printf("renderer: %.2f s making pipelines; the pipeline cache: %.1f KB read at the start, %.1f KB written\n",
                r.pipeline_seconds, r.pipeline_cache_read / 1024.0, r.pipeline_cache_written / 1024.0);
    std::printf("renderer: generic shaders: %llu programs, %llu stand-ins linked, %llu draws drawn by them; %llu shader "
                "forms translated on the workers (%.2f s there)\n",
                (unsigned long long)r.generic_programs, (unsigned long long)r.generic_pipelines,
                (unsigned long long)r.generic_draws, (unsigned long long)r.translations_async, r.translate_async_seconds);
    for (const auto &[why, n] : r.generic_refused)
        std::printf("renderer:   %llu draws waiting not drawn by them: %s\n", (unsigned long long)n, why.c_str());
    std::printf("renderer: %.3f s translating %llu shader forms on its thread (%.2f ms each, at most %.2f ms; at most "
                "%.1f ms in one frame)\n",
                r.translate_seconds, (unsigned long long)r.translations,
                r.translations ? r.translate_seconds * 1000.0 / double(r.translations) : 0.0, r.translate_max * 1000.0,
                r.translate_frame_max * 1000.0);
    std::printf("renderer: shader storage: %llu shaders and %llu pipelines added; made at once as the build step made "
                "them: %llu pipelines, %llu libraries (%llu and %llu of them not in the pipeline cache, left to the "
                "workers)\n",
                (unsigned long long)r.shaders_stored, (unsigned long long)r.pipelines_stored,
                (unsigned long long)r.pipelines_prebuilt, (unsigned long long)r.pipeline_libraries_prebuilt,
                (unsigned long long)r.pipelines_prebuilt_missed, (unsigned long long)r.pipeline_libraries_prebuilt_missed);
    std::printf(
        "renderer: %llu pipeline libraries, %llu pipelines linked, %llu placeholders, %llu draws by a stand-in; "
        "hitches: %llu frames with more than 4 ms making pipelines (at most %.1f ms), %llu swaps more than 50 ms "
        "apart, %llu more than 100 ms (at most %.0f ms)\n",
        (unsigned long long)r.pipeline_libraries, (unsigned long long)r.pipelines_linked,
        (unsigned long long)r.pipeline_placeholders, (unsigned long long)r.stand_in_draws,
        (unsigned long long)r.pipeline_hitches, r.pipeline_frame_max * 1000.0, (unsigned long long)r.swap_gaps_50ms,
        (unsigned long long)r.swap_gaps_100ms, r.swap_gap_max * 1000.0);
    if (r.pipeline_waits)
        std::printf(
            "renderer: %llu pipelines made on the workers, their draws without one for %.0f ms on average, %.0f "
            "ms at most\n",
            (unsigned long long)r.pipeline_waits, r.pipeline_wait_seconds * 1000.0 / double(r.pipeline_waits),
            r.pipeline_wait_max * 1000.0);
    if (!r.gpu_time.empty()) {
        // XF_GPU_TIME: the GPU's time by kind of work, the largest first
        std::vector<std::pair<std::string, render::Report::GpuTime>> kinds(r.gpu_time.begin(), r.gpu_time.end());
        std::sort(kinds.begin(), kinds.end(),
                  [](const auto &a, const auto &b) { return a.second.seconds > b.second.seconds; });
        std::printf("renderer: %.2f s of GPU time\n", r.gpu_seconds);
        for (size_t i = 0; i < kinds.size() && i < 24; i++)
            std::printf("  GPU %8.3f s %5.1f%%  %s\n", kinds[i].second.seconds,
                        r.gpu_seconds > 0 ? 100.0 * kinds[i].second.seconds / r.gpu_seconds : 0.0,
                        kinds[i].first.c_str());
    }
    for (const auto &[why, n] : r.skipped) {
        std::printf("  skipped %8llu  %s", (unsigned long long)n, why.c_str());
        auto at = r.skipped_at.find(why);
        if (at != r.skipped_at.end() && !at->second.empty()) {
            std::printf(" (frames");
            for (uint64_t swap : at->second) std::printf(" %llu", (unsigned long long)swap);
            std::printf("%s)", n > at->second.size() ? " ..." : "");
        }
        std::printf("\n");
    }
    for (const auto &[what, n] : r.approximate)
        std::printf("  approx  %8llu  %s\n", (unsigned long long)n, what.c_str());
    for (const auto &[what, n] : r.used) std::printf("  uses    %8llu  %s\n", (unsigned long long)n, what.c_str());
}

// The game library the recompile step cached for this executable (or the one
// named), its modules given to the kernel as a linked game's are; with none,
// or one that does not load, the game is interpreted.
void load_game_library(const Options &options, Game &g) {
    std::filesystem::path library = options.game_library;
    char digest[24];
    std::snprintf(digest, sizeof(digest), "%016llx", (unsigned long long)image_digest(g.xex.image));
    if (library.empty()) {
        const std::filesystem::path cache = options.cache.empty() ? build::default_cache_dir() : options.cache;
        if (auto cached = build::find_cached(cache, digest)) library = cached->library;
        if (library.empty()) {
            std::printf("generated code: none (no game library for %s in %s)\n", digest, cache.string().c_str());
            return;
        }
    }
    // the runtime's table, kept for the process's life with the library
    static const ppc::Runtime runtime = ppc::host_runtime();
    std::string error;
    const ppc::GameLibrary *loaded = ppc::load_game_library(library, runtime, error);
    if (!loaded) {
        std::printf("generated code: none (%s)\n", error.c_str());
        return;
    }
    size_t functions = 0;
    for (size_t i = 0; i < loaded->count; i++) functions += loaded->modules[i]->count;
    g.kernel->set_generated_modules(loaded->modules, loaded->count);
    g.generated_modules = loaded->count, g.generated_functions = functions, g.installed_modules = loaded->modules;
    std::printf("generated code: %zu modules, %zu functions (the game library %s)\n", loaded->count, functions,
                library.string().c_str());
    // what discovery missed, logged beside the library for its next build
    // (D3): the first entry into the interpreter at each address
    const std::filesystem::path log = build::entry_log_path(library.parent_path(), digest);
    if (g.entry_log.open(log)) {
        g.kernel->set_entry_hook(&build::EntryLog::hook, &g.entry_log);
        std::printf("entry log: %s (%zu addresses)\n", log.string().c_str(), g.entry_log.size());
    }
}

} // namespace

bool open(const Options &options, Game &g, std::string &error) {
    const std::string &input = options.input;
    if (!load(input, options.xex_path, g.xex, error)) return false;
    g.cache = options.cache.empty() ? build::default_cache_dir() : options.cache;
    g.memory = Memory::create(error);
    if (!g.memory) return error = "memory: " + error, false;
    g.kernel = std::make_unique<kernel::Kernel>(*g.memory);
    kernel::Kernel &k = *g.kernel;
    // the deterministic mode and input recordings, before anything runs
    int deterministic = options.deterministic;
    if (const char *d = std::getenv("XF_DETERMINISTIC"); !deterministic && d && *d && *d != '0')
        deterministic = std::strcmp(d, "paced") == 0 ? 2 : 1;
    if (deterministic) {
        kernel::Deterministic d;
        d.paced = deterministic == 2;
        if (const char *ms = std::getenv("XF_DETERMINISTIC_FORCE_MS"); ms && *ms)
            d.force_after_ms = uint32_t(std::strtoul(ms, nullptr, 10));
        if (options.halt_swap || options.halt_seconds > 0) {
            kernel::Kernel *kp = &k;
            uint64_t swap = options.halt_swap, clock = uint64_t(options.halt_seconds * 1e7);
            d.halt = [kp, swap, clock] {
                return (swap && kernel::gpu_command_processor(*kp).counts().swaps.load() >= swap) ||
                       (clock && kp->interrupt_time() >= clock);
            };
        }
        k.make_deterministic(d);
        std::printf("deterministic mode (%s)\n", d.paced ? "paced" : "as fast as the host goes");
    }
    std::filesystem::path record = options.record_input, replay = options.replay_input;
    if (const char *r = std::getenv("XF_INPUT_RECORD"); record.empty() && r && *r) record = r;
    if (const char *r = std::getenv("XF_INPUT_REPLAY"); replay.empty() && r && *r) replay = r;
    if (!replay.empty() && !kernel::replay_input(k, replay, error)) return error = "input: " + error, false;
    if (!record.empty() && !kernel::record_input(k, record, error)) return error = "input: " + error, false;
    bool bare = input.ends_with(".xex") || input.ends_with(".XEX");
    bool mounted = bare ? kernel::mount_directory(k, std::filesystem::path(input).parent_path(), error)
                        : kernel::mount_disc(k, input, error);
    if (!mounted || !kernel::mount_storage(k, options.storage, error)) return error = "mount: " + error, false;
    std::printf("storage: %s\n", options.storage.string().c_str());
    kernel::set_content_format(k, options.content_format);
    // the console's settings: what the player chose, and the settings titles wrote in earlier runs
    // (kept in the storage root, except in the deterministic mode, which starts from the defaults)
    {
        kernel::SystemSettings system = options.system;
        if (system.file.empty() && !options.deterministic && !options.storage.empty())
            system.file = options.storage / "xconfig.bin";
        kernel::apply_system_settings(k, system);
    }
    // its module's path, which the DLLs it loads by a bare name are next to
    kernel::set_executable_path(k, "game:\\" +
                                       (bare ? std::filesystem::path(input).filename().string() : options.xex_path));
    if (options.module_count) {
        // generated code linked in: each module's code runs once an image it was
        // made from is loaded (the executable's in load(), a DLL's as XexLoadImage loads it)
        size_t functions = 0;
        for (size_t i = 0; i < options.module_count; i++) functions += options.modules[i]->count;
        if (!options.interpret) k.set_generated_modules(options.modules, options.module_count);
        if (!options.interpret)
            g.generated_modules = options.module_count, g.generated_functions = functions,
            g.installed_modules = options.modules;
        std::printf("generated code: %zu modules, %zu functions%s\n", options.module_count, functions,
                    options.interpret ? " (left out: --interpret)" : "");
    } else if (!options.interpret) {
        load_game_library(options, g);
    }
    if (!k.load(g.xex, error)) return error = "load: " + error, false;
    k.step_limit = options.steps;
    {
        kernel::LockstepOptions lockstep;
        lockstep.functions = options.lockstep, lockstep.sample = options.lockstep_sample;
        std::string why;
        bool wanted =
            !lockstep.functions.empty() || lockstep.sample || kernel::lockstep_options_from_env(lockstep, why);
        if (!why.empty()) return error = why, false;
        if (wanted && g.generated_functions) {
            if (!k.enable_lockstep(lockstep, error)) return error = "lockstep: " + error, false;
            std::printf("lockstep: %zu functions chosen, one call in %u sampled\n", lockstep.functions.size(),
                        lockstep.sample);
        } else if (wanted) {
            std::printf("lockstep: no generated code to check\n");
        }
    }
    std::printf("loaded %s: base 0x%08X, entry 0x%08X, %zu imported functions\n", options.xex_path.c_str(),
                g.xex.base_address, k.entry_point(), k.imports().size());
    // a DLL's entry point is its DllMain, which returns at once: another
    // executable loads it (a launcher's game and splash DLLs, which its
    // default.xex loads one after the other and calls by their export)
    if (g.xex.module_flags & 0x8)
        std::printf("note: %s is a DLL (module flags 0x%X, %s): run as the executable, its DllMain returns at once; "
                    "the disc's default.xex loads it\n",
                    options.xex_path.c_str(), g.xex.module_flags, g.xex.original_pe_name.c_str());
    std::printf("stack 0x%X; TLS: ", g.xex.stack_size);
    if (g.xex.has_tls)
        std::printf("%u slots, %u bytes of data (0x%X from 0x%08X)\n", g.xex.tls.slot_count, g.xex.tls.data_size,
                    g.xex.tls.raw_data_size, g.xex.tls.raw_data_address);
    else
        std::printf("none\n");
    return true;
}

bool start_renderer(Game &g, const render::Options &options, std::string &error) {
    if (!g.device) g.device = vk::Device::open(error);
    if (!g.device) return error = "Vulkan: " + error, false;
    gpu::CommandProcessor &processor = kernel::gpu_command_processor(*g.kernel);
    // the game's pipeline cache and shader and pipeline storage, beside its
    // game library in the cache (<title id>.vkpipelines, .xfshaders), where
    // the launcher's build step makes the pipelines earlier runs met
    // (render::precompile), unless the caller names them
    render::Options o = options;
    const uint32_t title = g.xex.execution.title_id;
    if (o.pipeline_cache.empty()) o.pipeline_cache = ShaderStorage::pipeline_cache_path(g.cache, title);
    if (o.shader_storage.empty()) o.shader_storage = ShaderStorage::path(g.cache, title);
    // XF_PIPELINE_CACHE=<file>: the pipeline cache elsewhere (an empty one
    // for a cold start); XF_SHADER_STORAGE=<file>, or =none for no storage
    if (const char *cache = std::getenv("XF_PIPELINE_CACHE")) o.pipeline_cache = cache;
    if (const char *storage = std::getenv("XF_SHADER_STORAGE"))
        o.shader_storage = std::string(storage) == "none" ? std::filesystem::path() : std::filesystem::path(storage);
    // XF_PIPELINES=wait|skip|placeholder|library: each pipeline waited for
    // (boot's default), or made on worker threads with the draws meanwhile
    // skipped, drawn by a placeholder or by a linked pipeline (renderer.h,
    // Options::pipeline_stand_in; the player's default is the option's)
    if (const char *mode = std::getenv("XF_PIPELINES")) {
        std::string m = mode;
        o.async_pipelines = m != "wait";
        if (m == "skip") o.pipeline_stand_in = render::Options::PipelineStandIn::skip;
        if (m == "placeholder") o.pipeline_stand_in = render::Options::PipelineStandIn::placeholder;
        if (m == "library") o.pipeline_stand_in = render::Options::PipelineStandIn::library;
    }
    // the GPU's time by kind of work, in the report (XF_GPU_TIME=1; =draws
    // each draw's too)
    if (const char *timing = std::getenv("XF_GPU_TIME")) {
        o.gpu_timing = timing[0] && timing[0] != '0';
        o.gpu_timing_draws = std::string(timing) == "draws";
    }
    // occlusion queries: the GPU's counts, as Xenia's default "fast" mode;
    // XF_OCCLUSION_QUERIES=fake keeps the processor's made-up ones
    const char *queries = std::getenv("XF_OCCLUSION_QUERIES");
    processor.set_occlusion_queries(queries && std::string(queries) == "fake" ? gpu::OcclusionQueries::Fake
                                                                              : gpu::OcclusionQueries::Fast);
    // screen extents: the whole screen, as Xenia answers (the default);
    // XF_SCREEN_EXTENTS=measured, the bounds of what the draws rasterized
    // (beyond Xenia). The answer changes no picture, only how many of
    // predicated tiling's tiles a draw runs in, and measured bounds have the
    // command processor wait for the GPU at each interrupt or wait the
    // guest's extents may be read at: a game with ~120 reports a frame fell
    // from 30 to 20 frames a second (docs/renderer.md, "Screen extents
    // live").
    const char *extents = std::getenv("XF_SCREEN_EXTENTS");
    processor.set_screen_extents(extents && std::string(extents) == "measured" ? gpu::ScreenExtents::Measured
                                                                               : gpu::ScreenExtents::Whole);
    // when the CPU sees what resolves wrote (Xenia's readback_resolve):
    // XF_READBACK_RESOLVE=none (the default, as Xenia's), fast or full
    if (const char *mode = std::getenv("XF_READBACK_RESOLVE")) {
        std::string m = mode;
        o.readback_resolve = m == "full"   ? render::ReadbackResolve::full
                             : m == "fast" ? render::ReadbackResolve::fast
                                           : render::ReadbackResolve::none;
    }
    // Xenia's depth_float24_round: XF_DEPTH_FLOAT24_ROUND=1
    if (const char *round = std::getenv("XF_DEPTH_FLOAT24_ROUND")) o.depth_float24_round = round[0] && round[0] != '0';
    // the texture cache's memory limits (Xenia's texture_cache_memory_limit_*):
    // XF_TEXTURE_MEMORY=soft,hard[,soft lifetime] in MB and seconds
    if (const char *limits = std::getenv("XF_TEXTURE_MEMORY")) {
        unsigned soft = o.texture_memory_soft_mb, hard = o.texture_memory_hard_mb;
        unsigned lifetime = o.texture_memory_soft_lifetime_s;
        if (std::sscanf(limits, "%u,%u,%u", &soft, &hard, &lifetime) < 2)
            return error = "XF_TEXTURE_MEMORY: not soft,hard[,lifetime]", false;
        o.texture_memory_soft_mb = soft, o.texture_memory_hard_mb = hard, o.texture_memory_soft_lifetime_s = lifetime;
    }
    g.renderer = render::Renderer::create(*g.device, processor, o, error);
    if (!g.renderer) return error = "renderer: " + error, false;
    processor.set_sink(g.renderer.get());
    // frames traced as XF_TRACE_FRAMES and XF_TRACE_DIR say (the player's
    // way to them; boot has --trace-frames); XF_TRACE_DIR alone, none until
    // the player's F4 asks for the next
    const char *frames = std::getenv("XF_TRACE_FRAMES"), *dir = std::getenv("XF_TRACE_DIR");
    if (frames || dir) {
        std::set<uint64_t> numbers;
        if (frames && !parse_numbers(frames, numbers)) return error = "XF_TRACE_FRAMES: not a list of frames", false;
        if (!start_trace(g, numbers, dir ? dir : "traces", error)) return false;
    }
    return true;
}

bool start_trace(Game &g, const std::set<uint64_t> &frames, const std::filesystem::path &directory,
                 std::string &error) {
    if (!g.renderer) return error = "tracing frames needs the renderer", false;
    gpu::CommandProcessor &processor = kernel::gpu_command_processor(*g.kernel);
    gpu::TraceOptions options;
    options.directory = directory;
    options.frames = frames;
    options.title_id = g.xex.execution.title_id;
    g.trace = std::make_unique<gpu::TraceWriter>(processor, g.renderer.get(), g.renderer.get(), options);
    processor.set_sink(g.trace.get());
    std::printf("tracing %zu frames into %s\n", frames.size(), directory.string().c_str());
    return true;
}

bool launch(Game &g, std::string &error) {
    the_game = &g;
    // before any write watch, so memory's fault handler comes first in the chain
    if (const char *dir = std::getenv("XF_CRASH_DIR")) crash_directory = dir;
    kernel::install_crash_handler(*g.kernel, crash_directory, on_crash, nullptr);
    return g.kernel->launch(error);
}

bool load_port_module(Game &g, const std::filesystem::path &path, std::string &error) {
    std::filesystem::path file = path;
    if (!file.has_parent_path()) {
        // beside the executable
#ifdef _WIN32
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (n && n < MAX_PATH) file = std::filesystem::path(self).parent_path() / path;
#else
        std::error_code ec;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) file = self.parent_path() / path;
#endif
    }
    auto m = std::make_unique<kernel::PortModule>();
    if (!m->load(file, error)) return false;
    std::printf("port: hooks from %s (reloaded when it changes)\n", file.string().c_str());
    g.port_module = std::move(m);
    return true;
}

bool poll_port_module(Game &g, bool force) {
    if (!g.port_module) return false;
    if (!force && !g.port_module->changed()) {
        g.port_module->collect();
        return false;
    }
    std::string error, line;
    if (!g.port_module->reload(*g.kernel, error, &line)) {
        std::fprintf(stderr, "port: not reloaded: %s\n", error.c_str());
        return false;
    }
    std::printf("%s", line.c_str());
    std::fflush(stdout);
    return true;
}

void print_report(Game &g, bool waiting) {
    kernel::Kernel &k = *g.kernel;
    std::printf("%s", kernel::modules_report(k).c_str());
    if (k.lockstep_enabled()) std::printf("%s", k.lockstep_report().c_str());
    if (hooks::stats_on()) std::printf("%s", hooks::stats_report().c_str());
    if (g.port_module) std::printf("port module: %u reloads\n", g.port_module->reloads());
    // where the interpreter was entered: with generated code, what
    // discovery missed (the most entered first)
    auto targets = k.interpreted_targets();
    std::vector<std::pair<uint64_t, uint32_t>> entered;
    for (const auto &[address, n] : targets) entered.push_back({n, address});
    std::sort(entered.rbegin(), entered.rend());
    std::printf("interpreted entries: %llu, at %zu addresses\n", (unsigned long long)k.interpreted_entries(),
                entered.size());
    if (g.entry_log.size())
        std::printf("entry log: %zu addresses, %zu new this run\n", g.entry_log.size(), g.entry_log.added());
    for (size_t i = 0; i < entered.size() && i < 20; i++)
        std::printf("  %10llu  0x%08X  %s\n", (unsigned long long)entered[i].first, entered[i].second,
                    kernel::module_at(k, entered[i].second).c_str());
    // fiber switches, longjmps, catches: what took them
    std::printf("%s", kernel::stacks_report(k).c_str());
    std::vector<std::pair<uint64_t, std::string>> calls;
    for (const auto &[name, n] : k.calls()) calls.push_back({n, name});
    std::sort(calls.rbegin(), calls.rend());
    std::printf("kernel calls (%zu exports):\n", calls.size());
    auto missing = k.unimplemented();
    for (const auto &[n, name] : calls)
        std::printf("  %8llu  %s%s\n", (unsigned long long)n, name.c_str(),
                    missing.count(name) ? "  (not implemented)" : "");
    // what the GPU did: packets, draws, resolves, swaps, interrupts
    std::printf("%s", kernel::video_report(k).c_str());
    // the APU: render driver frames, XMA contexts
    std::printf("%s", kernel::audio_report(k).c_str());
    // what the input exports answered
    std::printf("%s", kernel::input_report(k).c_str());
    if (g.renderer) {
        render::Report r;
        if (waiting) {
            g.renderer->drain(); // the last front buffers asked for, to on_frame
            r = g.renderer->report();
        }
        if (waiting || g.renderer->try_report(r)) {
            print_renderer_report(r);
            if (g.device) {
                vk::MemoryCounts m = g.device->memory_counts();
                std::printf("device memory: %llu allocations (at most %llu, %llu made); images pooled: %llu in %llu "
                            "blocks (%.1f of %.1f MB), %llu of their own (%.1f MB)\n",
                            (unsigned long long)m.allocations, (unsigned long long)m.allocations_peak,
                            (unsigned long long)m.allocations_made, (unsigned long long)m.pooled_images,
                            (unsigned long long)m.blocks, double(m.pooled_bytes) / (1 << 20),
                            double(m.block_bytes) / (1 << 20), (unsigned long long)m.own_images,
                            double(m.own_bytes) / (1 << 20));
            }
            Memory::WatchCounts w = g.memory->watch_counts();
            std::printf("memory's write watches: %llu pages watched, %llu write faults (%llu let go already), %llu "
                        "pages written\n",
                        (unsigned long long)w.watched, (unsigned long long)w.faults, (unsigned long long)w.retried,
                        (unsigned long long)w.written);
            std::string failure;
            if (g.renderer->failed(failure)) std::printf("renderer: failed: %s\n", failure.c_str());
        } else {
            std::printf("renderer: busy (the fault came during one of its calls)\n");
        }
    }
    if (g.trace) {
        for (const auto &path : g.trace->finished()) std::printf("trace: %s\n", path.string().c_str());
        for (const auto &e : g.trace->errors()) std::printf("trace: %s\n", e.c_str());
    }
    std::fflush(stdout);
}

// The process's resident memory in bytes (its working set on Windows); 0
// where it is not known.
uint64_t resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
    return 0;
#else
    unsigned long long pages = 0, resident = 0;
    FILE *statm = std::fopen("/proc/self/statm", "r");
    if (!statm) return 0;
    if (std::fscanf(statm, "%llu %llu", &pages, &resident) != 2) resident = 0;
    std::fclose(statm);
    return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
}

namespace {
// a JSON string's body
std::string json_escape(const std::string &text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20 || c == 0x7F) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04x", c);
            out += hex;
        } else
            out += char(c);
    }
    return out;
}
} // namespace

void set_crash_directory(const std::filesystem::path &directory) { crash_directory = directory; }

std::string write_stop_report(Game &g) {
    kernel::Kernel &k = *g.kernel;
    if (k.stop_report.empty()) return {};
    kernel::CrashReport report;
    report.text = k.stop_report;
    kernel::write_crash_report(report, crash_directory);
    std::printf("%s", report.text.c_str());
    if (!report.file.empty()) std::printf("crash report written to %s\n", report.file.string().c_str());
    return report.file.string();
}

void print_summary(Game &g, const std::string &state, const std::string &detail, const std::string &stack,
                   const std::string &report_file) {
    kernel::Kernel &k = *g.kernel;
    char text[256];
    std::string out = "xf-summary {";
    std::snprintf(text, sizeof(text), "\"title_id\":\"%08X\",\"digest\":\"%016llx\",", g.xex.execution.title_id,
                  (unsigned long long)image_digest(g.xex.image));
    out += text;
    out += "\"state\":\"" + json_escape(state) + "\",\"detail\":\"" + json_escape(detail) + "\",";
    out += "\"stack\":\"" + json_escape(stack) + "\",\"report\":\"" + json_escape(report_file) + "\",";
    std::snprintf(text, sizeof(text),
                  "\"modules\":%zu,\"functions\":%zu,\"interpreted_entries\":%llu,\"interpreted_addresses\":%zu,"
                  "\"swaps\":%llu,\"draws\":%llu,",
                  g.generated_modules, g.generated_functions, (unsigned long long)k.interpreted_entries(),
                  k.interpreted_targets().size(),
                  (unsigned long long)kernel::gpu_command_processor(k).counts().swaps.load(),
                  (unsigned long long)kernel::gpu_command_processor(k).counts().draws.load());
    out += text;
    // the exports called without an implementation, the most called first
    std::vector<std::pair<uint64_t, std::string>> missing;
    auto absent = k.unimplemented();
    for (const auto &[name, n] : k.calls())
        if (absent.count(name)) missing.push_back({n, name});
    std::sort(missing.rbegin(), missing.rend());
    out += "\"unimplemented\":[";
    for (size_t i = 0; i < missing.size() && i < 8; i++)
        out += (i ? ",\"" : "\"") + json_escape(missing[i].second) + "\"";
    out += "],\"gpu\":[";
    // the GPU's gaps: draws skipped and done approximately, by what (the renderer's, when it runs)
    size_t listed = 0;
    render::Report r;
    if (g.renderer && g.renderer->try_report(r)) {
        auto list = [&](const char *kind, const std::map<std::string, uint64_t> &counts) {
            std::vector<std::pair<uint64_t, std::string>> by;
            for (const auto &[what, n] : counts) by.push_back({n, what});
            std::sort(by.rbegin(), by.rend());
            for (size_t i = 0; i < by.size() && i < 4; i++, listed++)
                out += std::string(listed ? ",\"" : "\"") + kind + ": " + json_escape(by[i].second) + "\"";
        };
        list("skipped", r.skipped);
        list("approx", r.approximate);
    }
    out += "]}";
    std::printf("%s\n", out.c_str());
    std::fflush(stdout);
}

void print_rates(Game &g, Rates &r, double wall, const std::string &extra) {
    const gpu::Counts &counts = kernel::gpu_command_processor(*g.kernel).counts();
    const uint64_t swaps = counts.swaps.load(), draws = counts.draws.load(), resolves = counts.resolves.load();
    const double dt = wall - r.last_wall;
    if (dt <= 0) return;
    // the renderer's time without waiting for a call of its: over the time
    // since the last report it gave (a call under way when this one asks
    // shows as "-")
    render::Report report;
    double busy = -1, loads = 0;
    if (g.renderer && g.renderer->try_report(report)) {
        const double since = wall - r.last_busy_wall;
        busy = (report.seconds - r.last_busy) / since;
        loads = double(report.textures - r.last_texture_loads) / since;
        r.last_busy = report.seconds, r.last_busy_wall = wall, r.last_texture_loads = report.textures;
    }
    char renderer[288] = "";
    if (g.renderer && busy < 0) {
        std::snprintf(renderer, sizeof(renderer), ", renderer busy -");
    } else if (g.renderer) {
        // with the texture cache's size, for seeing that memory stays flat,
        // and its loads a second; and the pipelines so far: made, the draws
        // skipped waiting for one and those drawn by a stand-in
        auto waiting = report.skipped.find("pipeline not ready (being made)");
        std::snprintf(renderer, sizeof(renderer),
                      ", renderer busy %.0f%%, textures %llu (%.0f MB, %.0f loads/s), pipelines %llu (%llu draws "
                      "skipped, %llu by a stand-in; %llu libraries, %llu and %llu as built)",
                      busy * 100, (unsigned long long)report.textures_held, double(report.texture_bytes) / (1 << 20),
                      loads, (unsigned long long)report.pipelines,
                      (unsigned long long)(waiting == report.skipped.end() ? 0 : waiting->second),
                      (unsigned long long)report.stand_in_draws, (unsigned long long)report.pipeline_libraries,
                      (unsigned long long)report.pipelines_prebuilt,
                      (unsigned long long)report.pipeline_libraries_prebuilt);
    }
    char resident[96] = "";
    if (uint64_t bytes = resident_bytes())
        std::snprintf(resident, sizeof(resident), ", resident %.0f MB", double(bytes) / (1 << 20));
    if (g.device) {
        // the device memory allocations (the image pool's blocks among them)
        const size_t used = std::strlen(resident);
        std::snprintf(resident + used, sizeof(resident) - used, ", %llu allocations",
                      (unsigned long long)g.device->memory_counts().allocations);
    }
    std::printf("rates: %6.1f s (guest %6.1f s): %6llu swaps, %5.1f/s, %6.0f draws/s, %4.0f resolves/s%s%s%s\n", wall,
                g.kernel->uptime_ms() / 1000.0, (unsigned long long)swaps, (swaps - r.last_swaps) / dt,
                (draws - r.last_draws) / dt, (resolves - r.last_resolves) / dt, renderer, resident, extra.c_str());
    std::fflush(stdout);
    r.last_wall = wall, r.last_swaps = swaps, r.last_draws = draws, r.last_resolves = resolves;
}

bool parse_numbers(const std::string &list, std::set<uint64_t> &out) {
    size_t at = 0;
    while (at < list.size()) {
        size_t comma = list.find(',', at);
        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? list.size() : comma + 1;
        if (item.empty()) continue;
        char *end = nullptr;
        uint64_t first = std::strtoull(item.c_str(), &end, 10), last = first;
        if (*end == '-') last = std::strtoull(end + 1, &end, 10);
        if (*end || last < first || last - first > 100000) return false;
        for (uint64_t n = first; n <= last; n++) out.insert(n);
    }
    return true;
}

std::string disc_path(std::string path) {
    std::string lower = path;
    for (char &ch : lower) ch = char(std::tolower((unsigned char)ch));
    for (const char *prefix : {"game:", "d:", "\\device\\cdrom0", "\\device\\harddisk0\\cdrom0"}) {
        size_t n = std::strlen(prefix);
        if (lower.compare(0, n, prefix) != 0) continue;
        path = path.substr(n);
        while (!path.empty() && (path[0] == '\\' || path[0] == '/')) path.erase(0, 1);
        for (char &ch : path)
            if (ch == '\\') ch = '/';
        return path;
    }
    return {};
}

std::vector<std::string> relaunch_args(const std::vector<std::string> &args,
                                       const std::vector<std::pair<std::string, std::string>> &replace) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); i++) {
        bool dropped = false;
        for (const auto &r : replace)
            if (i > 0 && args[i] == r.first) {
                dropped = true;
                if (i + 1 < args.size()) i++;
                break;
            }
        if (!dropped) out.push_back(args[i]);
    }
    for (const auto &r : replace)
        if (!r.second.empty()) out.insert(out.end(), {r.first, r.second});
    return out;
}

bool parse_press(const std::string &text, Press &out, std::string &error) {
    static const std::map<std::string, uint16_t> buttons = {
        {"a", input::A},
        {"b", input::B},
        {"x", input::X},
        {"y", input::Y},
        {"start", input::START},
        {"back", input::BACK},
        {"guide", input::GUIDE},
        {"lb", input::LEFT_SHOULDER},
        {"rb", input::RIGHT_SHOULDER},
        {"ls", input::LEFT_THUMB},
        {"rs", input::RIGHT_THUMB},
        {"up", input::DPAD_UP},
        {"down", input::DPAD_DOWN},
        {"left", input::DPAD_LEFT},
        {"right", input::DPAD_RIGHT},
    };
    out = Press{};
    size_t at = text.find('@');
    if (at == std::string::npos) return error = "--press " + text + ": no @<seconds>", false;
    std::string names = text.substr(0, at), when = text.substr(at + 1);
    for (size_t from = 0; from <= names.size();) {
        size_t plus = names.find('+', from);
        std::string name = names.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
        for (char &ch : name) ch = char(std::tolower(uint8_t(ch)));
        if (name == "lt")
            out.pad.left_trigger = 0xFF;
        else if (name == "rt")
            out.pad.right_trigger = 0xFF;
        else if (auto it = buttons.find(name); it != buttons.end())
            out.pad.buttons |= it->second;
        else
            return error = "--press " + text + ": no button " + name, false;
        if (plus == std::string::npos) break;
        from = plus + 1;
    }
    double seconds = 0, hold = 0.3;
    char *end = nullptr;
    const bool at_swap = !when.empty() && when[0] == '#';
    const char *start = when.c_str() + (at_swap ? 1 : 0);
    seconds = std::strtod(start, &end);
    if (end == start || seconds < 0) return error = "--press " + text + ": no time", false;
    if (at_swap)
        out.from_swap = uint64_t(seconds);
    else if (*end == 's')
        end++;
    if (*end == ':') {
        const char *h = end + 1;
        hold = std::strtod(h, &end);
        if (end == h || hold <= 0) return error = "--press " + text + ": bad hold", false;
        if (*end == 's') end++;
    }
    if (*end) return error = "--press " + text + ": trailing " + end, false;
    if (!at_swap) out.from_ms = uint64_t(std::llround(seconds * 1000));
    out.hold_ms = uint64_t(std::llround(hold * 1000));
    return true;
}

input::Overlay presses_overlay(std::vector<Press> presses, std::function<uint64_t()> swaps) {
    if (presses.empty()) return nullptr;
    // a press at a swap starts when the swap is first seen reached
    struct State {
        std::mutex mutex;
        std::vector<Press> presses;
        std::function<uint64_t()> swaps;
    };
    auto state = std::make_shared<State>();
    state->presses = std::move(presses), state->swaps = std::move(swaps);
    return [state](uint32_t user, uint64_t now_ms) {
        input::Gamepad g;
        if (user != 0) return g;
        std::lock_guard<std::mutex> lock(state->mutex);
        const uint64_t swaps = state->swaps ? state->swaps() : 0;
        for (Press &p : state->presses) {
            if (p.from_swap != UINT64_MAX) {
                if (swaps < p.from_swap) continue;
                p.from_ms = now_ms, p.from_swap = UINT64_MAX;
            }
            if (now_ms >= p.from_ms && now_ms < p.from_ms + p.hold_ms) g = input::combine(g, p.pad);
        }
        return g;
    };
}

} // namespace xf::game
