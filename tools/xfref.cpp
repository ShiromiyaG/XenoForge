// xfref: a trace's frame drawn by the software reference renderer
// (reference.h), on the CPU alone.
//
//   xfref <trace.xtr> -o frame.png [--compare <png>] [--diff diff.png] [--threads N] [-v]
//
// --compare prints the mean difference over RGB (of 255) and the share of
// pixels with a component off by more than 16, as replay_test does; --diff
// writes the per-pixel difference (amplified 4x) where the sizes match. The
// report lists what was skipped and what is approximate, by reason.

#include "xenoforge/reference.h"

#include <algorithm>
#include <chrono>
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

int main(int argc, char **argv) {
    const char *trace_path = nullptr, *out = nullptr, *compare = nullptr, *diff = nullptr;
    xf::reference::Options options;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!std::strcmp(argv[i], "--compare") && i + 1 < argc)
            compare = argv[++i];
        else if (!std::strcmp(argv[i], "--diff") && i + 1 < argc)
            diff = argv[++i];
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc)
            options.threads = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "-v"))
            options.verbose = true;
        else if (!trace_path)
            trace_path = argv[i];
        else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!trace_path) {
        std::fprintf(stderr,
                     "usage: xfref <trace.xtr> -o frame.png [--compare <png>] [--diff d.png] [--threads N] [-v]\n");
        return 2;
    }
    std::string error;
    xf::GpuTrace trace;
    if (!xf::open_gpu_trace(trace_path, trace, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    xf::reference::Image frame;
    xf::reference::Report report;
    auto start = std::chrono::steady_clock::now();
    bool ok = xf::reference::replay(trace, options, frame, report, error);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!ok) std::fprintf(stderr, "replay: %s\n", error.c_str());
    std::printf("%llu draws (%llu drawn), %llu resolves, %llu swaps; %llu vertices, %llu primitives, %llu quads, "
                "%llu exports; %.1f s\n",
                (unsigned long long)report.draws, (unsigned long long)report.drawn, (unsigned long long)report.resolves,
                (unsigned long long)report.swaps, (unsigned long long)report.vertices,
                (unsigned long long)report.primitives, (unsigned long long)report.quads,
                (unsigned long long)report.exports, seconds);
    for (auto &[what, n] : report.skipped) std::printf("  skipped %llu: %s\n", (unsigned long long)n, what.c_str());
    for (auto &[what, n] : report.approximate)
        std::printf("  approximate %llu: %s\n", (unsigned long long)n, what.c_str());
    if (options.verbose)
        for (auto &[what, n] : report.used) std::printf("  used %llu: %s\n", (unsigned long long)n, what.c_str());
    if (frame.rgba.empty()) {
        std::fprintf(stderr, "no frame\n");
        return 1;
    }
    if (out && !stbi_write_png(out, int(frame.width), int(frame.height), 4, frame.rgba.data(), int(frame.width) * 4)) {
        std::fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    if (compare) {
        int w = 0, h = 0, n = 0;
        unsigned char *b = stbi_load(compare, &w, &h, &n, 4);
        if (!b) {
            std::fprintf(stderr, "cannot read %s\n", compare);
            return 1;
        }
        if (uint32_t(w) != frame.width || uint32_t(h) != frame.height) {
            std::printf("sizes differ: %ux%u against %dx%d\n", frame.width, frame.height, w, h);
        } else {
            uint64_t sum = 0, off = 0;
            std::vector<uint8_t> d(size_t(w) * h * 4, 255);
            for (size_t i = 0; i < size_t(w) * h; i++) {
                int worst = 0;
                for (int c = 0; c < 3; c++) {
                    int v = std::abs(int(frame.rgba[i * 4 + c]) - int(b[i * 4 + c]));
                    sum += v;
                    worst = std::max(worst, v);
                    d[i * 4 + c] = uint8_t(std::min(255, v * 4));
                }
                off += worst > 16;
            }
            std::printf("mean %.3f, %.3f%% off\n", double(sum) / (double(w) * h * 3),
                        100.0 * double(off) / (double(w) * h));
            if (diff) stbi_write_png(diff, w, h, 4, d.data(), w * 4);
        }
        stbi_image_free(b);
    }
    return ok ? 0 : 1;
}
