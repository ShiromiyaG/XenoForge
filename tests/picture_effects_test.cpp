// The window's picture effects (frontend/present_effects.h) on synthetic
// images, on the GPU without a window: area sampling equals the box average of
// the texels each pixel covers, at whole and fractional ratios; the cubics and
// sharp bilinear keep a flat picture flat, enlarging and reducing; SMAA softens
// a hard aliased diagonal edge and leaves a flat picture and a straight
// horizontal line as they are. With XF_EFFECTS_TIMING=1 it also prints each
// pass's GPU time at 1080p and 4K (timestamps; once, not a benchmark).

#include "check.h"

#include "../frontend/present_effects.h"

#include <volk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace xf;
using frontend::Antialiasing;
using frontend::EffectOptions;
using frontend::Resampling;

namespace {

struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    uint8_t at(uint32_t x, uint32_t y, int c) const { return rgba[(size_t(y) * width + x) * 4 + c]; }
};

Picture make(uint32_t w, uint32_t h, auto &&color) {
    Picture p{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *o = &p.rgba[(size_t(y) * w + x) * 4];
            color(x, y, o);
            o[3] = 255;
        }
    return p;
}

class Runner {
  public:
    explicit Runner(vk::Device &device) : device_(device) {}
    bool open(std::string &error) {
        VkDevice dev = device_.device();
        return effects_.open(device_, [dev] { vkDeviceWaitIdle(dev); }, error);
    }
    // the effects from `in` to dest_width x dest_height; the result read back (empty when no pass ran)
    Picture run(const EffectOptions &options, const Picture &in, uint32_t dest_width, uint32_t dest_height,
                double *pass_ms = nullptr, uint32_t *passes = nullptr) {
        Picture out;
        vk::Image source = device_.create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {in.width, in.height, 1},
                                                1, 1, VK_SAMPLE_COUNT_1_BIT,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = source.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_.device(), &vi, nullptr, &view);
        vk::Buffer up = device_.create_buffer(in.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(up.mapped, in.rgba.data(), in.rgba.size());
        const VkDeviceSize back_bytes = VkDeviceSize(dest_width) * dest_height * 4;
        vk::Buffer back = device_.create_buffer(back_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        VkQueryPool queries = VK_NULL_HANDLE;
        if (pass_ms) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 9;
            vkCreateQueryPool(device_.device(), &qi, nullptr, &queries);
            effects_.timestamps(queries, 9);
        }
        frontend::PictureEffects::Output result;
        bool ran = false;
        std::string error;
        device_.run(
            [&](VkCommandBuffer cmd) {
                layout(cmd, source.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {in.width, in.height, 1}};
                vkCmdCopyBufferToImage(cmd, up.buffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                layout(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                ran = effects_.record(cmd, 0, options, view, in.width, in.height, dest_width, dest_height, result);
                if (!ran || result.width != dest_width || result.height != dest_height) return;
                VkBufferImageCopy out_copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {dest_width, dest_height, 1}};
                vkCmdCopyImageToBuffer(cmd, result.image, VK_IMAGE_LAYOUT_GENERAL, back.buffer, 1, &out_copy);
            },
            error);
        if (ran && result.width == dest_width && result.height == dest_height) {
            out.width = dest_width, out.height = dest_height;
            out.rgba.assign(static_cast<uint8_t *>(back.mapped), static_cast<uint8_t *>(back.mapped) + back_bytes);
        }
        if (passes) *passes = effects_.passes();
        if (queries) {
            const uint32_t n = effects_.passes() + 1;
            uint64_t stamps[9] = {};
            vkGetQueryPoolResults(device_.device(), queries, 0, n, sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device_.physical(), &props);
            for (uint32_t i = 1; i < n; i++)
                pass_ms[i - 1] = double(stamps[i] - stamps[i - 1]) * props.limits.timestampPeriod * 1e-6;
            effects_.timestamps(VK_NULL_HANDLE, 0);
            vkDestroyQueryPool(device_.device(), queries, nullptr);
        }
        vkDestroyImageView(device_.device(), view, nullptr);
        device_.destroy(source);
        device_.destroy(up);
        device_.destroy(back);
        return out;
    }

  private:
    static void layout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = from, b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    vk::Device &device_;
    frontend::PictureEffects effects_;
};

// the largest difference of any channel between two pictures of one size (256 for sizes that differ)
int largest_difference(const Picture &a, const Picture &b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) return 256;
    int most = 0;
    for (size_t i = 0; i < a.rgba.size(); i++) most = std::max(most, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    return most;
}

// each destination pixel's exact box average of the source texels it covers
Picture box_average(const Picture &in, uint32_t dw, uint32_t dh) {
    const double rx = double(in.width) / dw, ry = double(in.height) / dh;
    return make(dw, dh, [&](uint32_t x, uint32_t y, uint8_t *o) {
        const double x0 = x * rx, x1 = x0 + rx, y0 = y * ry, y1 = y0 + ry;
        double sum[3] = {}, total = 0;
        for (uint32_t sy = uint32_t(y0); sy < std::min<double>(std::ceil(y1), in.height); sy++)
            for (uint32_t sx = uint32_t(x0); sx < std::min<double>(std::ceil(x1), in.width); sx++) {
                const double w = (std::min<double>(x1, sx + 1) - std::max<double>(x0, sx)) *
                                 (std::min<double>(y1, sy + 1) - std::max<double>(y0, sy));
                for (int c = 0; c < 3; c++) sum[c] += w * in.at(sx, sy, c);
                total += w;
            }
        for (int c = 0; c < 3; c++) o[c] = uint8_t(std::lround(sum[c] / total));
    });
}

// pixels of the green channel strictly between black and white
int softened(const Picture &p) {
    int n = 0;
    for (uint32_t y = 0; y < p.height; y++)
        for (uint32_t x = 0; x < p.width; x++) n += p.at(x, y, 1) > 8 && p.at(x, y, 1) < 247;
    return n;
}

} // namespace

int main() {
    std::string error;
    auto device = vk::Device::open(error);
    if (!device) {
        std::printf("no Vulkan device (%s): nothing to run\n", error.c_str());
        return finish("picture_effects");
    }
    Runner runner(*device);
    if (!runner.open(error)) {
        std::printf("the effects could not be made: %s\n", error.c_str());
        CHECK(false);
        return finish("picture_effects");
    }

    // which filter runs: automatic is area only where the picture is reduced; nearest, FSR and CAS take none
    {
        EffectOptions o;
        CHECK(frontend::resampling_pass(o, 3840, 2160, 1920, 1080) == Resampling::area);
        CHECK(frontend::resampling_pass(o, 3840, 2160, 3840, 2160) == Resampling::bilinear);
        CHECK(frontend::resampling_pass(o, 1280, 720, 1920, 1080) == Resampling::bilinear);
        CHECK(!frontend::effects_apply(o, 1280, 720, 1920, 1080) && frontend::effects_apply(o, 2560, 1440, 1920, 1080));
        o.smooth = false;
        CHECK(!frontend::effects_apply(o, 3840, 2160, 1920, 1080)); // nearest: the blit
        o.smooth = true, o.upscaler = 2;
        CHECK(frontend::resampling_pass(o, 3840, 2160, 1920, 1080) == Resampling::bilinear);
        o.upscaler = 0, o.resampling = Resampling::catmull_rom;
        CHECK(frontend::resampling_pass(o, 1280, 720, 1920, 1080) == Resampling::catmull_rom);
        CHECK(frontend::antialiasing_from("smaa_ultra") == Antialiasing::smaa_ultra &&
              frontend::resampling_from("sharp_bilinear") == Resampling::sharp_bilinear &&
              frontend::resampling_from("x") == Resampling::automatic);
    }

    // area sampling equals the box average, at a whole ratio, fractional ones and one per axis
    {
        const Picture noise = make(300, 200, [](uint32_t x, uint32_t y, uint8_t *o) {
            const uint32_t h = (x * 2654435761u) ^ (y * 2246822519u);
            o[0] = uint8_t(h >> 3), o[1] = uint8_t((x * 7 + y * 13) & 255), o[2] = uint8_t((x ^ y) * 5);
        });
        EffectOptions o;
        o.resampling = Resampling::area;
        const uint32_t sizes[][2] = {{100, 100}, {120, 80}, {127, 61}, {300, 77}, {7, 5}};
        for (auto [dw, dh] : sizes) {
            const Picture got = runner.run(o, noise, dw, dh);
            const int diff = largest_difference(got, box_average(noise, dw, dh));
            if (diff > 1) std::printf("area %ux%u -> %ux%u: off by %d\n", noise.width, noise.height, dw, dh, diff);
            CHECK(diff <= 1);
        }
        // automatic takes area when reducing
        o.resampling = Resampling::automatic;
        CHECK(largest_difference(runner.run(o, noise, 120, 80), box_average(noise, 120, 80)) <= 1);
    }

    // the cubics and sharp bilinear keep a flat picture flat, enlarging and reducing
    {
        const Picture flat = make(64, 48, [](uint32_t, uint32_t, uint8_t *o) { o[0] = 77, o[1] = 140, o[2] = 200; });
        const Resampling filters[] = {Resampling::bspline, Resampling::mitchell, Resampling::catmull_rom,
                                      Resampling::sharp_bilinear};
        const uint32_t sizes[][2] = {{160, 120}, {100, 75}, {25, 19}, {9, 7}};
        for (Resampling f : filters)
            for (auto [dw, dh] : sizes) {
                EffectOptions o;
                o.resampling = f;
                const Picture want = make(dw, dh, [](uint32_t, uint32_t, uint8_t *c) { c[0] = 77, c[1] = 140, c[2] = 200; });
                const int diff = largest_difference(runner.run(o, flat, dw, dh), want);
                if (diff > 1) std::printf("filter %u to %ux%u: off by %d\n", uint32_t(f), dw, dh, diff);
                CHECK(diff <= 1);
            }
        // and a cubic is not a box: it differs from area on a detailed picture reduced
        const Picture stripes = make(64, 64, [](uint32_t x, uint32_t, uint8_t *o) { o[0] = o[1] = o[2] = x % 3 ? 0 : 255; });
        EffectOptions a, b;
        a.resampling = Resampling::area, b.resampling = Resampling::catmull_rom;
        CHECK(largest_difference(runner.run(a, stripes, 40, 40), runner.run(b, stripes, 40, 40)) > 2);
    }

    // SMAA: a hard aliased diagonal (a slope of 1 in 4) softened; a flat picture and a straight horizontal line kept
    {
        const Picture diagonal = make(64, 64, [](uint32_t x, uint32_t y, uint8_t *o) { o[0] = o[1] = o[2] = 4 * y < x ? 255 : 0; });
        const Picture flat = make(64, 64, [](uint32_t, uint32_t, uint8_t *o) { o[0] = 30, o[1] = 120, o[2] = 220; });
        const Picture line = make(64, 64, [](uint32_t, uint32_t y, uint8_t *o) { o[0] = o[1] = o[2] = y == 31 ? 255 : 0; });
        const Picture half = make(64, 64, [](uint32_t, uint32_t y, uint8_t *o) { o[0] = o[1] = o[2] = y >= 32 ? 255 : 0; });
        CHECK(softened(diagonal) == 0);
        for (Antialiasing a : {Antialiasing::smaa_medium, Antialiasing::smaa_high, Antialiasing::smaa_ultra}) {
            EffectOptions o;
            o.antialiasing = a;
            const Picture soft = runner.run(o, diagonal, 64, 64);
            const int n = softened(soft);
            std::printf("SMAA preset %u: %d pixels of the diagonal softened\n", uint32_t(a), n);
            CHECK(n >= 32);
            // only along the edge: far from it the picture is as it was
            bool kept = !soft.rgba.empty();
            for (uint32_t y = 0; y < 64 && kept; y++)
                for (uint32_t x = 0; x < 64; x++)
                    if (std::abs(int(x) - int(4 * y)) > 8 && soft.at(x, y, 1) != diagonal.at(x, y, 1)) kept = false;
            CHECK(kept);
            CHECK(largest_difference(runner.run(o, flat, 64, 64), flat) == 0);
            CHECK(largest_difference(runner.run(o, line, 64, 64), line) == 0);
            CHECK(largest_difference(runner.run(o, half, 64, 64), half) == 0);
        }
    }

    // each pass's GPU time, once, at 1080p and 4K
    if (const char *t = std::getenv("XF_EFFECTS_TIMING"); t && std::string(t) == "1") {
        auto scene = [](uint32_t w, uint32_t h) {
            return make(w, h, [](uint32_t x, uint32_t y, uint8_t *o) {
                // shapes with aliased edges at several slopes, over a gradient
                const bool in = ((x / 7 + y / 3) & 8) != 0 || ((x * 3 + y * 5) / 41) % 5 == 0;
                o[0] = uint8_t(in ? 230 : x * 255 / 3840), o[1] = uint8_t(in ? 200 : y * 255 / 2160), o[2] = in ? 40 : 90;
            });
        };
        const Picture hd = scene(1920, 1080), uhd = scene(3840, 2160);
        struct Case {
            const char *name;
            const Picture *in;
            uint32_t w, h;
            EffectOptions o;
        };
        auto with = [](Antialiasing a, Resampling r) {
            EffectOptions o;
            o.antialiasing = a, o.resampling = r;
            return o;
        };
        const Picture big = scene(4800, 2700); // 250% of 1080p
        const Case cases[] = {
            {"SMAA high 1080p (edges, weights, blend)", &hd, 1920, 1080, with(Antialiasing::smaa_high, Resampling::bilinear)},
            {"SMAA ultra 1080p (edges, weights, blend)", &hd, 1920, 1080, with(Antialiasing::smaa_ultra, Resampling::bilinear)},
            {"SMAA high 4K (edges, weights, blend)", &uhd, 3840, 2160, with(Antialiasing::smaa_high, Resampling::bilinear)},
            {"SMAA ultra 4K (edges, weights, blend)", &uhd, 3840, 2160, with(Antialiasing::smaa_ultra, Resampling::bilinear)},
            {"FXAA 4K", &uhd, 3840, 2160, with(Antialiasing::fxaa, Resampling::bilinear)},
            {"area 4K to 1080p", &uhd, 1920, 1080, with(Antialiasing::none, Resampling::area)},
            {"area 4800x2700 to 1080p", &big, 1920, 1080, with(Antialiasing::none, Resampling::area)},
            {"area 4800x2700 to 4K", &big, 3840, 2160, with(Antialiasing::none, Resampling::area)},
            {"Mitchell 4K to 1080p", &uhd, 1920, 1080, with(Antialiasing::none, Resampling::mitchell)},
            {"Mitchell 1080p to 4K", &hd, 3840, 2160, with(Antialiasing::none, Resampling::mitchell)},
            {"sharp bilinear 1080p to 4K", &hd, 3840, 2160, with(Antialiasing::none, Resampling::sharp_bilinear)},
        };
        for (const Case &c : cases) {
            double ms[8] = {};
            uint32_t passes = 0;
            runner.run(c.o, *c.in, c.w, c.h, ms, &passes); // warm
            runner.run(c.o, *c.in, c.w, c.h, ms, &passes);
            std::printf("%-42s", c.name);
            for (uint32_t i = 0; i < passes; i++) std::printf(" %.3f", ms[i]);
            std::printf(" ms\n");
        }
    }
    return finish("picture_effects");
}
