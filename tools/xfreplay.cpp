// xfreplay: a GPU trace replayed through the command processor into the
// renderer (plan B3), the frame it shows written as a PNG and compared with
// Xenia's (its trace dump's PNG of the same trace). --no-edram leaves the
// trace's EDRAM snapshot out, as Xenia's Vulkan trace player does: a frame
// that reads what an earlier frame left in EDRAM differs from Xenia's there.
// --gpu-time prints the GPU's time by kind of work (timestamps), and
// --gpu-time-draws each draw's too, by its shaders; --repeat N
// plays the trace N times through the same renderer, as a game draws a frame
// again, the GPU time then being the fastest of the plays after the first, and
// prints the time a play spent in the command processor and the renderer's
// CPU side (the command thread's work). --profile N samples this thread over
// the repeated plays and prints the N most sampled functions (sampler.h).
// --readback-resolve none|fast|full is Xenia's readback_resolve, --depth-float24-round its
// depth_float24_round, --depth-float24-convert its depth_float24_convert_in_pixel_shader,
// --no-gamma-unorm16 its gamma_render_target_as_unorm16 off (renderer.h). --texture-memory soft,hard[,lifetime] sets
// the texture cache's memory limits (Xenia's texture_cache_memory_limit_*; renderer.h).
// --async-pipelines makes pipelines on worker threads as the player does, --stand-in says what
// draws meanwhile (renderer.h's PipelineStandIn), --linked-pipelines links every pipeline from
// libraries and never makes one whole.
//
//   xfreplay <trace.xtr> [-o frame.png] [--compare xenia.png] [--diff diff.png] [--no-edram]
//            [--dump-shaders <dir>] [--gpu-time] [--gpu-time-draws] [--repeat N] [--profile N] [--async-pipelines]
//            [--stand-in skip|placeholder|library] [--linked-pipelines]
//            [--occlusion-queries] [--whole-extents] [--readback-resolve none|fast|full] [--depth-float24-round]
//            [--depth-float24-convert] [--no-gamma-unorm16]
//            [--texture-memory soft,hard[,lifetime]] [-v]

#include "xenoforge/render_scale.h"
#include "xenoforge/renderer.h"

#include "sampler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"

namespace {

int usage() {
    std::fprintf(stderr, "usage: xfreplay <trace.xtr> [-o frame.png] [--compare xenia.png] [--diff diff.png] "
                         "[--no-edram] [--gpu-time] [--gpu-time-draws] [--repeat N] [--pipeline-cache file] [--async-pipelines] "
                         "[--stand-in skip|placeholder|library] [--linked-pipelines] "
                         "[--occlusion-queries] [--whole-extents] [--readback-resolve none|fast|full] "
                         "[--depth-float24-round] "
                         "[--depth-float24-convert] [--no-gamma-unorm16] "
                         "[--texture-memory soft,hard[,lifetime]] [--scale percent] [--min-scalable pixels] [-v]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    std::string trace_path, out_path, compare_path, diff_path;
    xf::render::Options options;
    bool restore_edram = true;
    uint32_t repeat = 1;
    size_t profile_top = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--compare" && i + 1 < argc) compare_path = argv[++i];
        else if (a == "--diff" && i + 1 < argc) diff_path = argv[++i];
        else if (a == "--no-edram") restore_edram = false;
        else if (a == "--dump-shaders" && i + 1 < argc) options.dump_shaders = argv[++i];
        else if (a == "-v") options.verbose = true;
        else if (a == "--gpu-time") options.gpu_timing = true;
        else if (a == "--gpu-time-draws") options.gpu_timing = options.gpu_timing_draws = true;
        else if (a == "--repeat" && i + 1 < argc) repeat = uint32_t(std::max(1, std::atoi(argv[++i])));
        // the driver's pipeline cache kept in a file between runs (timing runs
        // otherwise spend most of their time making the frame's pipelines)
        else if (a == "--pipeline-cache" && i + 1 < argc) options.pipeline_cache = argv[++i];
        else if (a == "--profile" && i + 1 < argc) profile_top = size_t(std::max(1, std::atoi(argv[++i])));
        // pipelines made on worker threads, their draws skipped meanwhile (as
        // the player plays; a replay otherwise waits for each)
        else if (a == "--async-pipelines") options.async_pipelines = true;
        // the resolution scale in percent (50-800 in steps of 25) and the
        // minimum scalable dimension (docs/renderer.md, "Resolution scale")
        else if (a == "--scale" && i + 1 < argc)
            options.resolution_scale = xf::render::Scale::from_percent(std::atoi(argv[++i])).q4;
        else if (a == "--min-scalable" && i + 1 < argc) options.min_scalable = uint32_t(std::atoi(argv[++i]));
        // what draws while they are made: skip, placeholder or library
        else if (a == "--stand-in" && i + 1 < argc) {
            std::string m = argv[++i];
            using S = xf::render::Options::PipelineStandIn;
            if (m == "skip")
                options.pipeline_stand_in = S::skip;
            else if (m == "placeholder")
                options.pipeline_stand_in = S::placeholder;
            else if (m == "library")
                options.pipeline_stand_in = S::library;
            else
                return usage();
        }
        // every pipeline linked from libraries, never made whole
        else if (a == "--linked-pipelines")
            options.linked_pipelines_only = true;
        // occlusion query reports with the GPU's counts (else the fake ones)
        else if (a == "--occlusion-queries") options.occlusion_queries = xf::gpu::OcclusionQueries::Fast;
        // screen extent reports as Xenia answers them, the whole screen
        else if (a == "--whole-extents")
            options.screen_extents = xf::gpu::ScreenExtents::Whole;
        // Xenia's depth_float24_round
        else if (a == "--depth-float24-round") options.depth_float24_round = true;
        // Xenia's depth_float24_convert_in_pixel_shader
        else if (a == "--depth-float24-convert")
            options.depth_float24_convert_in_pixel_shader = true;
        // Xenia's gamma_render_target_as_unorm16 off
        else if (a == "--no-gamma-unorm16")
            options.gamma_render_target_as_unorm16 = false;
        // the texture cache's memory limits, soft,hard[,soft lifetime] in MB
        // and seconds (0,0 destroys what the GPU is done with each time it
        // finishes work: with --repeat, every texture between the plays)
        else if (a == "--texture-memory" && i + 1 < argc) {
            unsigned soft = 0, hard = 0, lifetime = options.texture_memory_soft_lifetime_s;
            if (std::sscanf(argv[++i], "%u,%u,%u", &soft, &hard, &lifetime) < 2) return usage();
            options.texture_memory_soft_mb = soft, options.texture_memory_hard_mb = hard;
            options.texture_memory_soft_lifetime_s = lifetime;
        } else if (a == "--readback-resolve" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "none") options.readback_resolve = xf::render::ReadbackResolve::none;
            else if (m == "fast") options.readback_resolve = xf::render::ReadbackResolve::fast;
            else if (m == "full") options.readback_resolve = xf::render::ReadbackResolve::full;
            else return usage();
        }
        else if (!a.empty() && a[0] != '-' && trace_path.empty()) trace_path = a;
        else return usage();
    }
    if (trace_path.empty()) return usage();

    std::string error;
    xf::GpuTrace trace;
    if (!xf::open_gpu_trace(trace_path, trace, error)) {
        std::fprintf(stderr, "%s: %s\n", trace_path.c_str(), error.c_str());
        return 1;
    }
    auto device = xf::vk::Device::open(error);
    if (!device) {
        std::fprintf(stderr, "Vulkan: %s\n", error.c_str());
        return 1;
    }
    xf::render::Image frame;
    xf::render::Report report;
    // sampled from the first frame shown on: the plays after the first, with everything made
    bool profiling = false;
    if (profile_top) {
        unsigned long thread = xf::sampler::current_thread();
        // the command processor's work alone, not the trace's walk
        xf::sampler::scope("CommandProcessor::execute_packets");
        options.on_frame = [&profiling, thread](uint64_t, const xf::render::Image &) {
            if (!profiling) profiling = xf::sampler::start(1000, thread);
        };
    }
    double cpu_before = xf::sampler::process_cpu_seconds();
    bool replayed = xf::render::replay(trace, *device, options, frame, report, error, restore_edram, repeat);
    double cpu = xf::sampler::process_cpu_seconds() - cpu_before;
    if (profiling) xf::sampler::stop_and_report(profile_top);
    if (!replayed) {
        std::fprintf(stderr, "replay: %s\n", error.c_str());
        return 1;
    }
    if (repeat > 1 && cpu_before >= 0)
        std::printf("  %.3f ms of CPU (all threads) a play, the trace's walk and the first play included\n",
                    cpu * 1000 / repeat);

    std::printf("%s (title %08X) on %s\n", trace_path.c_str(), trace.title_id, device->name().c_str());
    std::printf("  packets %llu, draws %llu: drawn %llu, resolves %llu, swaps %llu\n", (unsigned long long)report.packets,
                (unsigned long long)report.draws, (unsigned long long)report.drawn, (unsigned long long)report.resolves,
                (unsigned long long)report.swaps);
    std::printf("  made: %llu render targets, %llu textures, %llu pipelines, %llu shaders (%llu samplers' slots "
                "reused)\n",
                (unsigned long long)report.render_targets, (unsigned long long)report.textures,
                (unsigned long long)report.pipelines, (unsigned long long)report.shaders,
                (unsigned long long)report.samplers_reused);
    std::printf("  pipelines: %.3f s making them, %llu libraries, %llu linked, %llu placeholders, %llu draws by a "
                "stand-in\n",
                report.pipeline_seconds, (unsigned long long)report.pipeline_libraries,
                (unsigned long long)report.pipelines_linked, (unsigned long long)report.pipeline_placeholders,
                (unsigned long long)report.stand_in_draws);
    if (report.pipeline_waits)
        std::printf("  %llu pipelines made on the workers, their draws without one for %.1f ms on average, %.1f ms "
                    "at most\n",
                    (unsigned long long)report.pipeline_waits,
                    report.pipeline_wait_seconds * 1000.0 / double(report.pipeline_waits),
                    report.pipeline_wait_max * 1000.0);
    std::printf("  %.3f s in the renderer: %.3f s in submissions (%.3f s waiting for the GPU), %.3f s in resolves, "
                "%.3f s checking textures, %.3f s decoding them, %.3f s on vertex data (%.1f KB uploaded), "
                "%.3f s copying %.1f KB that resolves wrote into memory\n",
                report.seconds, report.submit_seconds, report.wait_seconds, report.resolve_seconds,
                report.check_seconds, report.decode_seconds, report.vertex_seconds, double(report.vertex_bytes) / 1024,
                report.writeback_seconds, double(report.writeback_bytes) / 1024);
    std::printf("  %llu submissions, %llu waits for the GPU\n", (unsigned long long)report.submissions,
                (unsigned long long)report.waits);
    std::printf("  texture cache: %llu textures (%llu views) held in %.1f MB (at most %.1f MB), %llu destroyed by the "
                "memory limits, %llu to make room, %llu loads refused\n",
                (unsigned long long)report.textures_held, (unsigned long long)report.texture_views,
                double(report.texture_bytes) / (1 << 20),
                double(report.texture_bytes_peak) / (1 << 20), (unsigned long long)report.textures_evicted,
                (unsigned long long)report.textures_evicted_for_room, (unsigned long long)report.textures_refused);
    std::printf("  texture loads: %llu new, %llu after the CPU wrote their memory, %llu after a resolve or an export; "
                "%llu resolves wrote the texture they feed\n",
                (unsigned long long)report.texture_loads_new, (unsigned long long)report.texture_loads_cpu_written,
                (unsigned long long)report.texture_loads_gpu_written,
                (unsigned long long)report.resolve_targets_written);
    if (report.textures_scaled)
        std::printf("  %llu textures made the resolution scale's\n", (unsigned long long)report.textures_scaled);
    if (report.zpd_reports)
        std::printf("  occlusion: %llu queries for %llu reports\n", (unsigned long long)report.occlusion_queries,
                    (unsigned long long)report.zpd_reports);
    if (report.viz_queries)
        std::printf("  VIZ queries: %llu, %llu predicating %llu draws on the GPU\n",
                    (unsigned long long)report.viz_queries, (unsigned long long)report.viz_armed,
                    (unsigned long long)report.viz_predicated);
    if (report.extent_reports)
        std::printf("  screen extents: %llu reports, %llu bounded\n", (unsigned long long)report.extent_reports,
                    (unsigned long long)report.extent_bounded);
    if (!report.command_seconds.empty()) {
        // the command thread's work a play: the median and the fastest of the plays after the first
        std::vector<double> plays = report.command_seconds;
        std::sort(plays.begin(), plays.end());
        std::printf("  commands a play: %.3f ms median, %.3f ms fastest, of %zu plays\n",
                    plays[plays.size() / 2] * 1000, plays.front() * 1000, plays.size());
    }
    for (const auto &[why, n] : report.skipped) std::printf("  skipped %6llu  %s\n", (unsigned long long)n, why.c_str());
    for (const auto &[what, n] : report.approximate)
        std::printf("  approx  %6llu  %s\n", (unsigned long long)n, what.c_str());
    for (const auto &[what, n] : report.used) std::printf("  uses    %6llu  %s\n", (unsigned long long)n, what.c_str());
    if (options.gpu_timing) {
        // a play's GPU time: the fastest of the plays after the first, when repeated
        std::vector<std::pair<std::string, xf::render::Report::GpuTime>> kinds(report.gpu_time.begin(),
                                                                               report.gpu_time.end());
        std::sort(kinds.begin(), kinds.end(),
                  [](const auto &a, const auto &b) { return a.second.seconds > b.second.seconds; });
        std::printf("  GPU time a play: %.3f ms%s\n", report.gpu_seconds * 1000,
                    repeat > 1 ? " (the fastest of the plays after the first)" : "");
        for (const auto &[what, t] : kinds)
            std::printf("    %8.3f ms %5.1f%% %6llu  %s\n", t.seconds * 1000,
                        report.gpu_seconds > 0 ? 100.0 * t.seconds / report.gpu_seconds : 0.0,
                        (unsigned long long)t.intervals, what.c_str());
    }
    if (frame.rgba.empty()) {
        std::printf("  no frame (no swap showed one)\n");
        return compare_path.empty() ? 0 : 1;
    }
    std::printf("  frame %ux%u\n", frame.width, frame.height);
    if (!out_path.empty() &&
        !stbi_write_png(out_path.c_str(), int(frame.width), int(frame.height), 4, frame.rgba.data(), int(frame.width) * 4)) {
        std::fprintf(stderr, "could not write %s\n", out_path.c_str());
        return 1;
    }

    if (compare_path.empty()) return 0;
    int w = 0, h = 0, n = 0;
    unsigned char *ref = stbi_load(compare_path.c_str(), &w, &h, &n, 4);
    if (!ref) {
        std::fprintf(stderr, "could not read %s\n", compare_path.c_str());
        return 1;
    }
    if (uint32_t(w) != frame.width || uint32_t(h) != frame.height) {
        // a frame drawn at a resolution scale: compared at the console's size,
        // box filtered down (or nearest up) to the reference's
        if (options.resolution_scale == 4) {
            std::printf("  compare: Xenia's frame is %dx%d, ours %ux%u\n", w, h, frame.width, frame.height);
            stbi_image_free(ref);
            return 1;
        }
        std::printf("  compared at %dx%d, resampled from %ux%u\n", w, h, frame.width, frame.height);
        frame.rgba = xf::render::resample_box(frame.rgba, frame.width, frame.height, uint32_t(w), uint32_t(h));
        frame.width = uint32_t(w), frame.height = uint32_t(h);
    }
    // the color only: both are shown opaque
    uint64_t sum = 0, off = 0;
    std::vector<uint8_t> diff(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        int worst = 0;
        for (int c = 0; c < 3; c++) {
            int d = std::abs(int(frame.rgba[i * 4 + c]) - int(ref[i * 4 + c]));
            sum += d;
            worst = std::max(worst, d);
        }
        if (worst > 16) off++;
        uint8_t v = uint8_t(std::min(255, worst * 4));
        diff[i * 4] = v, diff[i * 4 + 1] = v, diff[i * 4 + 2] = v, diff[i * 4 + 3] = 255;
    }
    stbi_image_free(ref);
    double mean = double(sum) / (double(w) * h * 3), share = 100.0 * double(off) / (double(w) * h);
    std::printf("  compare with %s: mean difference %.3f (of 255), %.3f%% of pixels off by more than 16\n",
                compare_path.c_str(), mean, share);
    if (!diff_path.empty()) stbi_write_png(diff_path.c_str(), w, h, 4, diff.data(), w * 4);
    return 0;
}
