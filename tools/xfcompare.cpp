// xfcompare: a picture against Xenia's, by the measure xfreplay and
// replay_test use (the mean difference of the color, of 255, and the share
// of pixels off by more than 16 in any component). Given a directory, the
// reference is compared with every PNG in it and the closest named, for
// finding which of boot's shots shows the scene a Xenia dump shows when
// the two count swaps differently.
//
//   xfcompare <xenia.png> <ours.png | dir> [--diff diff.png]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"

namespace {

struct Picture {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
    bool load(const std::string &path) {
        int n = 0;
        unsigned char *data = stbi_load(path.c_str(), &width, &height, &n, 4);
        if (!data) return false;
        rgba.assign(data, data + size_t(width) * height * 4);
        stbi_image_free(data);
        return true;
    }
};

struct Difference {
    bool same_size = false;
    double mean = 0, share = 0; // of 255; percent of pixels off by more than 16
};

Difference compare(const Picture &a, const Picture &b, std::vector<unsigned char> *diff) {
    Difference d;
    if (a.width != b.width || a.height != b.height) return d;
    d.same_size = true;
    uint64_t sum = 0, off = 0;
    size_t pixels = size_t(a.width) * a.height;
    if (diff) diff->assign(pixels * 4, 255);
    for (size_t i = 0; i < pixels; i++) {
        int worst = 0;
        for (int c = 0; c < 3; c++) {
            int v = std::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c]));
            sum += v;
            worst = std::max(worst, v);
        }
        off += worst > 16;
        if (diff) (*diff)[i * 4] = (*diff)[i * 4 + 1] = (*diff)[i * 4 + 2] = (unsigned char)std::min(255, worst * 4);
    }
    d.mean = double(sum) / (double(pixels) * 3);
    d.share = 100.0 * double(off) / double(pixels);
    return d;
}

} // namespace

int main(int argc, char **argv) {
    std::string reference_path, other, diff_path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--diff" && i + 1 < argc)
            diff_path = argv[++i];
        else if (reference_path.empty())
            reference_path = a;
        else if (other.empty())
            other = a;
        else
            return std::fprintf(stderr, "usage: xfcompare <xenia.png> <ours.png | dir> [--diff diff.png]\n"), 2;
    }
    if (other.empty())
        return std::fprintf(stderr, "usage: xfcompare <xenia.png> <ours.png | dir> [--diff diff.png]\n"), 2;
    Picture reference;
    if (!reference.load(reference_path)) return std::fprintf(stderr, "could not read %s\n", reference_path.c_str()), 1;

    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_directory(other)) {
        for (const auto &entry : std::filesystem::directory_iterator(other))
            if (entry.path().extension() == ".png") candidates.push_back(entry.path());
        std::sort(candidates.begin(), candidates.end());
    } else {
        candidates.push_back(other);
    }
    std::filesystem::path best;
    Difference best_difference;
    for (const auto &path : candidates) {
        Picture picture;
        if (!picture.load(path.string())) {
            std::fprintf(stderr, "could not read %s\n", path.string().c_str());
            continue;
        }
        Difference d = compare(reference, picture, nullptr);
        if (!d.same_size) {
            std::printf("%s: %dx%d, Xenia's %dx%d\n", path.string().c_str(), picture.width, picture.height,
                        reference.width, reference.height);
            continue;
        }
        std::printf("%s: mean difference %.3f (of 255), %.3f%% of pixels off by more than 16\n", path.string().c_str(),
                    d.mean, d.share);
        if (best.empty() || d.mean < best_difference.mean) best = path, best_difference = d;
    }
    if (best.empty()) return 1;
    if (candidates.size() > 1)
        std::printf("closest: %s (mean %.3f, %.3f%% off)\n", best.string().c_str(), best_difference.mean,
                    best_difference.share);
    if (!diff_path.empty()) {
        Picture picture;
        picture.load(best.string());
        std::vector<unsigned char> diff;
        compare(reference, picture, &diff);
        stbi_write_png(diff_path.c_str(), reference.width, reference.height, 4, diff.data(), reference.width * 4);
    }
    return 0;
}
