// A title's shader and pipeline storage (docs/renderer.md, "Shaders and
// pipelines at the launcher's build step"): what the renderer met while a
// game ran, kept beside its game library in the cache so that the launcher's
// build step can translate the shaders and make their pipeline libraries
// into the game's Vulkan pipeline cache before the next run.
//
// As Xenia's per-title shader and pipeline storage (its pipeline caches'
// InitializeShaderStorage: <title id>.xsh, the guest shaders' microcode, and
// <title id>.<backend>.xpso, the pipelines' descriptions, both appended to as
// the game runs, a header of a magic and a version first): here one file,
// <cache>/<title id>.xfshaders, a header (magic, version) and then records,
// each a kind, its length in words and its words, appended and flushed as
// they come, so a run cut short keeps what it met. A tail cut in the middle
// of a record is left out when read.
//
// The records: a shader's microcode (its type, the renderer's hash of it,
// the words as IM_LOAD gives them); a pipeline's description, as the
// renderer writes it (opaque here: the renderer's format, versioned by its
// first word); and what a build step made into the pipeline cache for a
// device and driver (the hashes of the libraries' and pipelines' keys), so
// that the renderer knows which draws find their pipelines made.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_set>
#include <vector>

namespace xf {

class ShaderStorage {
public:
    struct Shader {
        uint32_t type = 0; // 0 vertex, 1 pixel (IM_LOAD's)
        uint64_t hash = 0;
        std::vector<uint32_t> code;
    };
    // what a build step made, for the device and driver it made it on (the
    // pipeline cache's header: vendor, device, pipelineCacheUUID; and the
    // driver's version)
    struct Built {
        uint32_t vendor = 0, device = 0, driver = 0;
        std::array<uint8_t, 16> uuid{};
        std::vector<uint64_t> libraries, pipelines; // hashes of their keys
    };

    ShaderStorage() = default;
    ShaderStorage(const ShaderStorage &) = delete;
    ShaderStorage &operator=(const ShaderStorage &) = delete;
    ~ShaderStorage();

    // The storage of a title in a cache directory, and its pipeline cache
    // beside it (vk: the renderer's Options::pipeline_cache)
    static std::filesystem::path path(const std::filesystem::path &cache_dir, uint32_t title_id);
    static std::filesystem::path pipeline_cache_path(const std::filesystem::path &cache_dir, uint32_t title_id);

    // reads what the file holds (nothing when it is not there or not a
    // storage of this version) and, with append, keeps it open to add to; a
    // file of another version is started again then. False when it cannot
    // be read nor made.
    bool open(const std::filesystem::path &file, bool append = true);
    // true when it was not there (and is appended, when open for it)
    bool add_shader(uint32_t type, uint64_t hash, std::span<const uint32_t> code);
    bool add_pipeline(std::span<const uint32_t> description);
    void add_built(const Built &built);

    std::vector<Shader> shaders() const;
    std::vector<std::vector<uint32_t>> pipelines() const;
    std::vector<Built> built() const;
    bool has_shader(uint64_t hash) const;

    // FNV-1a over the words' bytes, from the seed (the renderer's hash of a
    // shader: seeded with its type)
    static uint64_t hash(uint32_t seed, std::span<const uint32_t> words);

    static constexpr uint32_t MAGIC = 0x53534658; // "XFSS"
    static constexpr uint32_t VERSION = 1;
    enum Kind : uint32_t { SHADER = 1, PIPELINE = 2, BUILT = 3 };

private:
    void append(uint32_t kind, std::span<const uint32_t> words);
    mutable std::mutex mutex_;
    std::FILE *file_ = nullptr;
    std::vector<Shader> shaders_;
    std::vector<std::vector<uint32_t>> pipelines_;
    std::vector<Built> built_;
    std::unordered_set<uint64_t> shader_hashes_, pipeline_hashes_;
};

} // namespace xf
