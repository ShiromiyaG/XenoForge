// xfreplay: a GPU trace replayed through the command processor into the
// renderer (plan B3), the frame it shows written as a PNG and compared with
// Xenia's (its trace dump's PNG of the same trace). --no-edram leaves the
// trace's EDRAM snapshot out, as Xenia's Vulkan trace player does: a frame
// that reads what an earlier frame left in EDRAM differs from Xenia's there.
// --gpu-time prints the GPU's time by kind of work (timestamps), and
// --gpu-time-draws each draw's too, by its shaders; --repeat N
// plays the trace N times through the same renderer, as a game draws a frame
// again, the GPU time then being the fastest of the plays after the first.
// --readback-resolve none|fast|full is Xenia's readback_resolve, --depth-float24-round its
// depth_float24_round (renderer.h).
//
//   xfreplay <trace.xtr> [-o frame.png] [--compare xenia.png] [--diff diff.png] [--no-edram]
//            [--dump-shaders <dir>] [--gpu-time] [--gpu-time-draws] [--repeat N] [--async-pipelines]
//            [--occlusion-queries] [--readback-resolve none|fast|full] [--depth-float24-round] [-v]

#include "xenoforge/renderer.h"

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
                         "[--no-edram] [--gpu-time] [--gpu-time-draws] [--repeat N] [--async-pipelines] "
                         "[--occlusion-queries] [--readback-resolve none|fast|full] [--depth-float24-round] [-v]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    std::string trace_path, out_path, compare_path, diff_path;
    xf::render::Options options;
    bool restore_edram = true;
    uint32_t repeat = 1;
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
        // pipelines made on worker threads, their draws skipped meanwhile (as
        // the player plays; a replay otherwise waits for each)
        else if (a == "--async-pipelines") options.async_pipelines = true;
        // occlusion query reports with the GPU's counts (else the fake ones)
        else if (a == "--occlusion-queries") options.occlusion_queries = xf::gpu::OcclusionQueries::Fast;
        // Xenia's depth_float24_round
        else if (a == "--depth-float24-round") options.depth_float24_round = true;
        else if (a == "--readback-resolve" && i + 1 < argc) {
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
    if (!xf::render::replay(trace, *device, options, frame, report, error, restore_edram, repeat)) {
        std::fprintf(stderr, "replay: %s\n", error.c_str());
        return 1;
    }

    std::printf("%s (title %08X) on %s\n", trace_path.c_str(), trace.title_id, device->name().c_str());
    std::printf("  packets %llu, draws %llu: drawn %llu, resolves %llu, swaps %llu\n", (unsigned long long)report.packets,
                (unsigned long long)report.draws, (unsigned long long)report.drawn, (unsigned long long)report.resolves,
                (unsigned long long)report.swaps);
    std::printf("  made: %llu render targets, %llu textures, %llu pipelines, %llu shaders\n",
                (unsigned long long)report.render_targets, (unsigned long long)report.textures,
                (unsigned long long)report.pipelines, (unsigned long long)report.shaders);
    std::printf("  %.3f s in the renderer: %.3f s in submissions (%.3f s waiting for the GPU), %.3f s in resolves, "
                "%.3f s checking textures, %.3f s decoding them, %.3f s on vertex data (%.1f KB uploaded), "
                "%.3f s copying %.1f KB that resolves wrote into memory\n",
                report.seconds, report.submit_seconds, report.wait_seconds, report.resolve_seconds,
                report.check_seconds, report.decode_seconds, report.vertex_seconds, double(report.vertex_bytes) / 1024,
                report.writeback_seconds, double(report.writeback_bytes) / 1024);
    std::printf("  %llu submissions, %llu waits for the GPU\n", (unsigned long long)report.submissions,
                (unsigned long long)report.waits);
    if (report.zpd_reports)
        std::printf("  occlusion: %llu queries for %llu reports\n", (unsigned long long)report.occlusion_queries,
                    (unsigned long long)report.zpd_reports);
    for (const auto &[why, n] : report.skipped) std::printf("  skipped %6llu  %s\n", (unsigned long long)n, why.c_str());
    for (const auto &[what, n] : report.approximate)
        std::printf("  approx  %6llu  %s\n", (unsigned long long)n, what.c_str());
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
        std::printf("  compare: Xenia's frame is %dx%d, ours %ux%u\n", w, h, frame.width, frame.height);
        stbi_image_free(ref);
        return 1;
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
