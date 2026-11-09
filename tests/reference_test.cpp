// The software reference renderer (reference.h) on the traces, a third
// witness: every trace under XF_TRACES with Xenia's dump of the same name
// under XF_DUMPS is drawn by the reference and by the Vulkan renderer
// (render::replay, where there is a device), and the three are compared:
// the reference against Xenia's exact-EDRAM pictures (XF_DUMPS/rov/d3d12,
// its Direct3D 12 ROV dump; XF_DUMPS/rov, its Vulkan FSI one), the reference
// against the Vulkan renderer's frame, and the Vulkan frame against Xenia's,
// each as the mean difference over RGB (of 255) and the share of pixels with
// a component off by more than 16, in a table.
//
// It fails on crashes and failed replays, not (yet) on differences: the
// reference is new, and where the three disagree the spec decides
// (docs/renderer.md, "The software reference renderer"). Traces are game
// data and stay outside the repository; without XF_TRACES nothing runs.
// XF_REFERENCE_FILTER=<substring> runs only the traces whose names hold it;
// XF_REFERENCE_OUT=<dir> writes the reference's frames there.

#include "check.h"
#include "xenoforge/reference.h"
#include "xenoforge/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"

namespace {

struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

std::optional<Picture> load(const std::filesystem::path &png) {
    if (!std::filesystem::exists(png)) return std::nullopt;
    int w = 0, h = 0, n = 0;
    unsigned char *data = stbi_load(png.string().c_str(), &w, &h, &n, 4);
    if (!data) return std::nullopt;
    Picture p{uint32_t(w), uint32_t(h), std::vector<unsigned char>(data, data + size_t(w) * h * 4)};
    stbi_image_free(data);
    return p;
}

struct Difference {
    bool same_size = false;
    double mean = 0, share = 0;
};

Difference compare(const Picture &a, const Picture &b) {
    Difference d;
    if (a.width != b.width || a.height != b.height || a.rgba.empty()) return d;
    d.same_size = true;
    uint64_t sum = 0, off = 0;
    const size_t pixels = size_t(a.width) * a.height;
    for (size_t i = 0; i < pixels; i++) {
        int worst = 0;
        for (int c = 0; c < 3; c++) {
            int v = std::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c]));
            sum += v;
            worst = std::max(worst, v);
        }
        off += worst > 16;
    }
    d.mean = double(sum) / (double(pixels) * 3);
    d.share = 100.0 * double(off) / double(pixels);
    return d;
}

std::string cell(const std::optional<Difference> &d) {
    if (!d) return "-";
    if (!d->same_size) return "size";
    char text[48];
    std::snprintf(text, sizeof(text), "%.3f, %.2f%%", d->mean, d->share);
    return text;
}

} // namespace

int main() {
    const char *traces = std::getenv("XF_TRACES"), *dumps = std::getenv("XF_DUMPS");
    if (!traces || !dumps) {
        std::printf("XF_TRACES and XF_DUMPS not set: nothing to draw\n");
        return 0;
    }
    // (a relative path is the working directory's: ctest runs in the build directory)
    if (!std::filesystem::is_directory(traces)) {
        std::printf("XF_TRACES (%s) is not a directory from %s\n", traces,
                    std::filesystem::current_path().string().c_str());
        return 1;
    }
    const char *filter = std::getenv("XF_REFERENCE_FILTER"), *out = std::getenv("XF_REFERENCE_OUT");
    std::string error;
    std::unique_ptr<xf::vk::Device> device = xf::vk::Device::open(error);
    if (!device) std::printf("no Vulkan device (%s): the reference against Xenia alone\n", error.c_str());
    const std::filesystem::path exact = std::filesystem::path(dumps) / "rov";
    std::printf("%-22s | %-17s | %-17s | %-17s | %-17s | %s\n", "frame", "ref : Xenia ROV", "ref : Xenia FSI",
                "ref : ours (Vk)", "ours : Xenia ROV", "reference");
    int drawn = 0;
    double seconds_total = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(traces)) {
        if (entry.path().extension() != ".xtr") continue;
        const std::string stem = entry.path().stem().string(), name = stem + ".png";
        if (filter && stem.find(filter) == std::string::npos) continue;
        std::optional<Picture> rov = load(exact / "d3d12" / name), fsi = load(exact / name);
        if (!rov && !fsi) continue;
        xf::GpuTrace trace;
        CHECK(xf::open_gpu_trace(entry.path(), trace, error));
        // the reference
        xf::reference::Options options;
        xf::reference::Image frame;
        xf::reference::Report report;
        auto start = std::chrono::steady_clock::now();
        bool ok = xf::reference::replay(trace, options, frame, report, error);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        seconds_total += seconds;
        CHECK(ok);
        CHECK(frame.rgba.size() == size_t(frame.width) * frame.height * 4);
        if (!ok || frame.rgba.empty()) {
            std::printf("%-22s | the reference failed: %s\n", stem.c_str(), error.c_str());
            continue;
        }
        drawn++;
        Picture ref{frame.width, frame.height, frame.rgba};
        if (out) {
            std::filesystem::create_directories(out);
            stbi_write_png((std::filesystem::path(out) / name).string().c_str(), int(ref.width), int(ref.height), 4,
                           ref.rgba.data(), int(ref.width) * 4);
        }
        // the Vulkan renderer, on its default path
        std::optional<Picture> ours;
        if (device) {
            xf::render::Image image;
            xf::render::Report vulkan_report;
            xf::render::Options vulkan_options;
            CHECK(xf::render::replay(trace, *device, vulkan_options, image, vulkan_report, error));
            if (!image.rgba.empty()) ours = Picture{image.width, image.height, image.rgba};
        }
        std::optional<Difference> ref_rov, ref_fsi, ref_ours, ours_rov;
        if (rov) ref_rov = compare(ref, *rov);
        if (fsi) ref_fsi = compare(ref, *fsi);
        if (ours) ref_ours = compare(ref, *ours);
        if (ours && rov)
            ours_rov = compare(*ours, *rov);
        else if (ours && fsi)
            ours_rov = compare(*ours, *fsi);
        uint64_t skipped = 0, approximate = 0;
        for (auto &[what, n] : report.skipped) skipped += n;
        for (auto &[what, n] : report.approximate) approximate += n;
        std::printf(
            "%-22s | %-17s | %-17s | %-17s | %-17s | %.1f s, %llu draws, %llu skipped (%zu reasons), %zu approximate\n",
            stem.c_str(), cell(ref_rov).c_str(), cell(ref_fsi).c_str(), cell(ref_ours).c_str(), cell(ours_rov).c_str(),
            seconds, (unsigned long long)report.draws, (unsigned long long)skipped, report.skipped.size(),
            report.approximate.size());
        for (auto &[what, n] : report.skipped)
            std::printf("    skipped %llu: %s\n", (unsigned long long)n, what.c_str());
        for (auto &[what, n] : report.approximate)
            std::printf("    approximate %llu: %s\n", (unsigned long long)n, what.c_str());
        (void)approximate;
    }
    std::printf("%d frames drawn by the reference in %.1f s\n", drawn, seconds_total);
    return finish("reference");
}
