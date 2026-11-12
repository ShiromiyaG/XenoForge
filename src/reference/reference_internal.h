// The software reference renderer's parts (reference.h): the textures and
// their filtering, the counters shared by the threads.

#pragma once

#include "xenoforge/guest_memory.h"
#include "xenoforge/reference.h"
#include "xenoforge/shader_interpreter.h"
#include "xenoforge/texture.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xf::reference {

namespace pieces {
// render_target_cache.cc's GetPSIColorFormatInfo: the bits of a sample a
// write mask keeps (both all ones when nothing is written)
void keep_masks(uint32_t format, uint32_t write, uint32_t keep[2]);
} // namespace pieces

// What is approximate, counted from any thread.
class Counts {
  public:
    void add(const std::string &what) {
        std::lock_guard lock(mutex_);
        approximate_[what]++;
    }
    void take(std::map<std::string, uint64_t> &into) {
        std::lock_guard lock(mutex_);
        for (auto &[what, n] : approximate_) into[what] += n;
        approximate_.clear();
    }

  private:
    std::mutex mutex_;
    std::map<std::string, uint64_t> approximate_;
};

// A texture as the fetch constant gives it, read out of guest memory
// (texture.h's decode) into floats: per level, per slice or layer, RGBA in
// the guest format's component order, each texel's components already
// through the fetch constant's signs (unsigned, signed, biased, gamma), as
// the hardware converts texels before it filters them.
struct Texture {
    texture::Info info;
    uint32_t mip_min = 0, mip_max = 0; // the levels the sampler clamps to (texture::levels)
    struct Level {
        uint32_t width = 0, height = 0, slices = 0; // slices: 3D depth, or layers
        std::vector<float> texels;                  // RGBA, rows of each slice in turn
        bool empty() const { return texels.empty(); }
    };
    std::vector<Level> levels;
    std::vector<std::pair<uint32_t, uint32_t>> footprint;
    bool is_3d = false; // 3D, else 2D layers (stacked, cube faces)
};

class Textures {
  public:
    explicit Textures(const GuestMemory &memory) : memory_(memory) {}
    // decoded once until the memory under it changes; nullptr with why
    std::shared_ptr<const Texture> get(const uint32_t fetch[6], std::string &why, Counts &counts);
    void invalidate(uint32_t address, uint32_t bytes);
    void clear() { cache_.clear(); }

  private:
    const GuestMemory &memory_;
    std::map<std::array<uint32_t, 6>, std::shared_ptr<const Texture>> cache_;
};

// A texture operation (shader_interpreter.h's TextureFetch) on a texture:
// the sample after the fetch constant's swizzle and exponent adjust; LOD
// queries, weights and gradients. False with why where it is not done.
bool sample(const Texture *texture, const shader::TextureFetch &fetch, float out[4], std::string &why, Counts &counts);

} // namespace xf::reference
