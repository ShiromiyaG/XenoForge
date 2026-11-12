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
//
// The pictures are those of the render target path the renderer takes
// (render::render_target_path; XF_RENDER_TARGET_PATH=fsi or host forces
// one): on the interlock path Xenia's own exact ones, XF_DUMPS/rov (its
// Direct3D 12 ROV dump in rov/d3d12, its Vulkan FSI dump in rov itself:
// `--render_target_path_d3d12=rov`, `--render_target_path_vulkan=fsi`), where
// they are; on host render targets Xenia's default ones, as above, and its
// ROV one where those differ (host render targets at the Xenos's sample
// positions, Options::xenos_sample_positions, draw antialiased edges as
// Xenia's exact paths do, not as its host paths by default).
//
// With XF_REPLAY_SCALES=<percent>,<percent>... each trace is replayed again at
// those resolution scales (docs/renderer.md, "Resolution scale"). Where
// XF_SCALED_DUMPS holds Xenia's Direct3D 12 dump at that scale
// (<dir>/rov<n>x on the interlock path, <dir>/<n>x on host render targets,
// made with --draw_resolution_scale_x/y=<n>), the frame is compared with it
// at its size, and one whose 100% picture matches Xenia's passes within the
// SCALED bound below. Otherwise (and for the scales Xenia has not, such as
// 150%) it is box filtered to the console's size and compared with our own
// 100% frame, printed only: a scaled frame is not the console's picture
// (blurs made by bilinear taps between texels blur less over finer
// texels, as in Xenia's scaled pictures), so no bound holds it there.

#include "check.h"
#include "xenoforge/render_scale.h"
#include "xenoforge/renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// a scaled frame against Xenia's at the same scale: the mean difference (of
// 255) and the share of pixels off by more than 16 (percent)
constexpr double SCALED_MEAN = 4.0, SCALED_SHARE = 5.0;

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
    // (a relative path is the working directory's: ctest runs in the build directory)
    if (!std::filesystem::is_directory(traces)) {
        std::printf("XF_TRACES (%s) is not a directory from %s\n", traces,
                    std::filesystem::current_path().string().c_str());
        return 1;
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
    std::filesystem::path pictures = dumps;
    if (xf::render::render_target_path(*device, options) == xf::render::Options::RenderTargetPath::fsi &&
        std::filesystem::is_directory(pictures / "rov")) {
        pictures /= "rov";
        std::printf("the interlock render target path: Xenia's ROV and FSI pictures (%s)\n", pictures.string().c_str());
    }
    int compared = 0, vulkan_only = 0, rov_only = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(traces)) {
        if (entry.path().extension() != ".xtr") continue;
        std::string name = entry.path().stem().string() + ".png";
        std::optional<Picture> d3d12 = load(pictures / "d3d12" / name);
        std::optional<Picture> vulkan = load(pictures / name);
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
        if (!passes && pictures == dumps) {
            // Host render targets take the Xenos's sample positions where
            // the device places samples (Options::xenos_sample_positions),
            // as Xenia's exact paths always do and its host paths do not by
            // default: an antialiased frame's edges then match Xenia's ROV
            // picture rather than its default one.
            std::optional<Picture> rov = load(pictures / "rov" / "d3d12" / name);
            if (rov) {
                Difference to_rov = compare(frame.rgba.data(), frame.width, frame.height, *rov);
                std::printf("; ROV mean %.3f, %.3f%% off", to_rov.mean, to_rov.share);
                if (to_rov.passes()) {
                    std::printf(" (matches Xenia's picture at the Xenos's sample positions)");
                    passes = true;
                    rov_only++;
                }
            }
        }
        std::printf("\n");
        CHECK(passes);
        compared++;
        // the resolution scales asked for, against this 100% frame
        const bool exact = to_d3d12 && to_d3d12->passes();
        const char *scales = std::getenv("XF_REPLAY_SCALES");
        for (const char *at = scales; at && *at;) {
            const int percent = std::atoi(at);
            at = std::strchr(at, ',');
            if (at) at++;
            if (percent == 100 || percent <= 0) continue;
            xf::render::Options scaled_options = options;
            scaled_options.resolution_scale = xf::render::Scale::from_percent(percent).q4;
            xf::render::Image scaled;
            xf::render::Report scaled_report;
            CHECK(xf::render::replay(trace, *device, scaled_options, scaled, scaled_report, error));
            if (scaled.rgba.empty()) continue;
            Picture ours{int(frame.width), int(frame.height), frame.rgba};
            std::vector<uint8_t> down =
                xf::render::resample_box(scaled.rgba, scaled.width, scaled.height, frame.width, frame.height);
            Difference d = compare(down.data(), frame.width, frame.height, ours);
            std::printf("%-20s at %d%% (%ux%u, %llu textures scaled): mean %.3f, %.3f%% off its 100%% frame",
                        entry.path().stem().string().c_str(), percent, scaled.width, scaled.height,
                        (unsigned long long)scaled_report.textures_scaled, d.mean, d.share);
            const char *scaled_dumps = std::getenv("XF_SCALED_DUMPS");
            std::optional<Picture> xenia_scaled;
            if (scaled_dumps && percent % 100 == 0)
                xenia_scaled = load(std::filesystem::path(scaled_dumps) /
                                    ((pictures != dumps ? "rov" : "") + std::to_string(percent / 100) + "x") / name);
            if (xenia_scaled) {
                Difference x = compare(scaled.rgba.data(), scaled.width, scaled.height, *xenia_scaled);
                const bool close = x.same_size && x.mean <= SCALED_MEAN && x.share <= SCALED_SHARE;
                std::printf("; Xenia's at %d%%: mean %.3f, %.3f%% off%s", percent, x.mean, x.share,
                            close ? "" : exact ? " (past the bound)" : " (past the bound; its 100% frame is not Xenia's)");
                if (exact) CHECK(close);
            }
            std::printf("\n");
        }
    }
    std::printf("%d frames compared", compared);
    if (vulkan_only) std::printf(", %d matching Vulkan's picture only", vulkan_only);
    if (rov_only) std::printf(", %d matching the ROV picture only", rov_only);
    std::printf("\n");
    return finish("replay");
}
