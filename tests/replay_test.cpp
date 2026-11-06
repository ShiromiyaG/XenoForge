// The renderer against Xenia's pictures: every trace under XF_TRACES that
// has Xenia's trace dump PNG of the same name under XF_DUMPS is replayed
// and compared. Traces are game data and stay outside the repository, so
// without XF_TRACES there is nothing to run.
//
// The picture to match is Xenia's Direct3D 12 trace dump, in XF_DUMPS/d3d12
// (docs/plan.md, "References for the GPU"); its Vulkan dump, in XF_DUMPS
// itself, is the second picture, used alone where there is no D3D12 one.
// A frame passes within a mean difference of 1 (of 255) and with at most
// 0.1% of pixels off by more than 16. Where Xenia's two backends disagree
// with each other by more than that, matching the Vulkan picture passes too,
// and the line says so: what differs between them is open (renderer.md).
// With XF_LINKED_PIPELINES=1 every draw is drawn by a pipeline linked from
// libraries (VK_EXT_graphics_pipeline_library), which must draw the same.

#include "check.h"
#include "xenoforge/renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace {

struct Picture {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

std::optional<Picture> load(const std::filesystem::path &png) {
    if (!std::filesystem::exists(png)) return std::nullopt;
    Picture p;
    int n = 0;
    unsigned char *data = stbi_load(png.string().c_str(), &p.width, &p.height, &n, 4);
    if (!data) return std::nullopt;
    p.rgba.assign(data, data + size_t(p.width) * p.height * 4);
    stbi_image_free(data);
    return p;
}

// the mean difference over RGB (of 255) and the share of pixels with a
// component off by more than 16, in percent; false when the sizes differ
struct Difference {
    bool same_size = false;
    double mean = 0, share = 0;
    bool passes() const { return same_size && mean <= 1.0 && share <= 0.1; }
};

Difference compare(const unsigned char *a, uint32_t width, uint32_t height, const Picture &b) {
    Difference d;
    if (uint32_t(b.width) != width || uint32_t(b.height) != height) return d;
    d.same_size = true;
    uint64_t sum = 0, off = 0;
    for (size_t i = 0; i < size_t(width) * height; i++) {
        int worst = 0;
        for (int c = 0; c < 3; c++) {
            int v = std::abs(int(a[i * 4 + c]) - int(b.rgba[i * 4 + c]));
            sum += v;
            worst = std::max(worst, v);
        }
        off += worst > 16;
    }
    d.mean = double(sum) / (double(width) * height * 3);
    d.share = 100.0 * double(off) / (double(width) * height);
    return d;
}

} // namespace

int main() {
    const char *traces = std::getenv("XF_TRACES"), *dumps = std::getenv("XF_DUMPS");
    if (!traces || !dumps) {
        std::printf("XF_TRACES and XF_DUMPS not set: nothing to replay\n");
        return 0;
    }
    std::string error;
    auto device = xf::vk::Device::open(error);
    if (!device) {
        std::printf("no Vulkan device (%s): nothing to replay\n", error.c_str());
        return 0;
    }
    // XF_LINKED_PIPELINES=1: every pipeline linked from libraries, never made
    // whole (Options::linked_pipelines_only), where the device has them
    xf::render::Options options;
    if (const char *linked = std::getenv("XF_LINKED_PIPELINES"); linked && linked[0] == '1') {
        if (device->pipeline_library())
            options.linked_pipelines_only = true;
        else
            std::printf("XF_LINKED_PIPELINES: the device has no graphics pipeline libraries; whole pipelines\n");
    }
    int compared = 0, vulkan_only = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(traces)) {
        if (entry.path().extension() != ".xtr") continue;
        std::string name = entry.path().stem().string() + ".png";
        std::optional<Picture> d3d12 = load(std::filesystem::path(dumps) / "d3d12" / name);
        std::optional<Picture> vulkan = load(std::filesystem::path(dumps) / name);
        if (!d3d12 && !vulkan) continue;
        xf::GpuTrace trace;
        CHECK(xf::open_gpu_trace(entry.path(), trace, error));
        xf::render::Image frame;
        xf::render::Report report;
        CHECK(xf::render::replay(trace, *device, options, frame, report, error));
        CHECK(frame.rgba.size() == size_t(frame.width) * frame.height * 4);
        if (frame.rgba.size() != size_t(frame.width) * frame.height * 4) continue;
        std::optional<Difference> to_d3d12, to_vulkan;
        if (d3d12) to_d3d12 = compare(frame.rgba.data(), frame.width, frame.height, *d3d12);
        if (vulkan) to_vulkan = compare(frame.rgba.data(), frame.width, frame.height, *vulkan);
        std::printf("%-20s", entry.path().stem().string().c_str());
        if (to_d3d12) std::printf(" d3d12 mean %.3f, %.3f%% off", to_d3d12->mean, to_d3d12->share);
        if (to_vulkan)
            std::printf("%s vulkan mean %.3f, %.3f%% off", to_d3d12 ? ";" : "", to_vulkan->mean, to_vulkan->share);
        bool passes = false;
        if (to_d3d12) {
            passes = to_d3d12->passes();
            if (!passes && to_vulkan && to_vulkan->passes()) {
                // Xenia's backends against each other
                Difference backends =
                    compare(d3d12->rgba.data(), uint32_t(d3d12->width), uint32_t(d3d12->height), *vulkan);
                if (!backends.passes()) {
                    std::printf(" (matches Vulkan's picture only: Xenia's two differ by %.3f, %.3f%%)", backends.mean,
                                backends.share);
                    passes = true;
                    vulkan_only++;
                }
            }
        } else {
            passes = to_vulkan->passes();
        }
        std::printf("\n");
        CHECK(passes);
        compared++;
    }
    std::printf("%d frames compared", compared);
    if (vulkan_only) std::printf(", %d matching Vulkan's picture only", vulkan_only);
    std::printf("\n");
    return finish("replay");
}
