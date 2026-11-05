// The renderer against Xenia's pictures: every trace under XF_TRACES that
// has Xenia's trace dump PNG of the same name under XF_DUMPS is replayed
// and compared. Traces are game data and stay outside the repository, so
// without XF_TRACES there is nothing to run.
//
// A frame passes within a mean difference of 1 (of 255) and with at most
// 0.1% of pixels off by more than 16.

#include "check.h"
#include "xenoforge/renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

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
    int compared = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(traces)) {
        if (entry.path().extension() != ".xtr") continue;
        std::filesystem::path png = std::filesystem::path(dumps) / (entry.path().stem().string() + ".png");
        if (!std::filesystem::exists(png)) continue;
        xf::GpuTrace trace;
        CHECK(xf::open_gpu_trace(entry.path(), trace, error));
        xf::render::Image frame;
        xf::render::Report report;
        CHECK(xf::render::replay(trace, *device, {}, frame, report, error));
        int w = 0, h = 0, n = 0;
        unsigned char *ref = stbi_load(png.string().c_str(), &w, &h, &n, 4);
        CHECK(ref != nullptr);
        CHECK(uint32_t(w) == frame.width && uint32_t(h) == frame.height);
        uint64_t sum = 0, off = 0;
        for (size_t i = 0; ref && i < size_t(w) * h && frame.rgba.size() == size_t(w) * h * 4; i++) {
            int worst = 0;
            for (int c = 0; c < 3; c++) {
                int d = std::abs(int(frame.rgba[i * 4 + c]) - int(ref[i * 4 + c]));
                sum += d;
                worst = std::max(worst, d);
            }
            off += worst > 16;
        }
        stbi_image_free(ref);
        double mean = double(sum) / (double(w) * h * 3), share = 100.0 * double(off) / (double(w) * h);
        std::printf("%-20s mean %.3f, %.3f%% off\n", entry.path().stem().string().c_str(), mean, share);
        CHECK(mean <= 1.0);
        CHECK(share <= 0.1);
        compared++;
    }
    std::printf("%d frames compared\n", compared);
    return finish("replay");
}
