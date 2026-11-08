// A title's shader and pipeline storage (shader_storage.h).

#include "xenoforge/shader_storage.h"

#include <cstring>

namespace xf {
namespace {

std::FILE *open_file(const std::filesystem::path &path, const char *mode) {
#ifdef _WIN32
    wchar_t wide[8] = {};
    for (size_t i = 0; mode[i] && i < 7; i++) wide[i] = wchar_t(mode[i]);
    return _wfopen(path.c_str(), wide);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::vector<uint32_t> read_words(const std::filesystem::path &path) {
    std::vector<uint32_t> words;
    std::FILE *f = open_file(path, "rb");
    if (!f) return words;
    uint32_t chunk[16384];
    for (size_t n; (n = std::fread(chunk, 4, std::size(chunk), f)) > 0;) words.insert(words.end(), chunk, chunk + n);
    std::fclose(f);
    return words;
}

void hex8(char *out, uint32_t v) { std::snprintf(out, 9, "%08X", v); }

} // namespace

ShaderStorage::~ShaderStorage() {
    if (file_) std::fclose(file_);
}

std::filesystem::path ShaderStorage::path(const std::filesystem::path &cache_dir, uint32_t title_id) {
    char name[16];
    hex8(name, title_id);
    return cache_dir / (std::string(name) + ".xfshaders");
}

std::filesystem::path ShaderStorage::pipeline_cache_path(const std::filesystem::path &cache_dir, uint32_t title_id) {
    char name[16];
    hex8(name, title_id);
    return cache_dir / (std::string(name) + ".vkpipelines");
}

uint64_t ShaderStorage::hash(uint32_t seed, std::span<const uint32_t> words) {
    uint64_t h = 0xcbf29ce484222325ull ^ seed; // FNV-1a
    for (uint32_t d : words)
        for (int i = 0; i < 4; i++) h = (h ^ ((d >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    return h;
}

bool ShaderStorage::open(const std::filesystem::path &file, bool append) {
    std::lock_guard lock(mutex_);
    if (file_) std::fclose(file_), file_ = nullptr;
    shaders_.clear(), pipelines_.clear(), built_.clear();
    shader_hashes_.clear(), pipeline_hashes_.clear();
    const std::vector<uint32_t> words = read_words(file);
    // the records up to the first one cut short
    size_t valid = 0;
    if (words.size() >= 2 && words[0] == MAGIC && words[1] == VERSION) {
        size_t at = valid = 2;
        while (at + 2 <= words.size()) {
            const uint32_t kind = words[at], count = words[at + 1];
            if (count > words.size() - at - 2) break;
            std::span<const uint32_t> w(words.data() + at + 2, count);
            if (kind == SHADER && count >= 3) {
                Shader s{w[0], uint64_t(w[1]) | uint64_t(w[2]) << 32, std::vector<uint32_t>(w.begin() + 3, w.end())};
                if (shader_hashes_.insert(s.hash).second) shaders_.push_back(std::move(s));
            } else if (kind == PIPELINE) {
                if (pipeline_hashes_.insert(hash(0, w)).second) pipelines_.emplace_back(w.begin(), w.end());
            } else if (kind == BUILT && count >= 9) {
                Built b{w[0], w[1], w[2]};
                std::memcpy(b.uuid.data(), &w[3], 16);
                const uint32_t libraries = w[7], pipelines = w[8];
                if (uint64_t(libraries + uint64_t(pipelines)) * 2 + 9 <= count) {
                    for (uint32_t i = 0; i < libraries; i++)
                        b.libraries.push_back(uint64_t(w[9 + 2 * i]) | uint64_t(w[10 + 2 * i]) << 32);
                    for (uint32_t i = 0; i < pipelines; i++) {
                        const size_t k = 9 + 2 * size_t(libraries) + 2 * i;
                        b.pipelines.push_back(uint64_t(w[k]) | uint64_t(w[k + 1]) << 32);
                    }
                    built_.push_back(std::move(b));
                }
            }
            at += 2 + count;
            valid = at;
        }
    }
    if (!append) return valid != 0 || words.empty();
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    if (valid == words.size() && valid) {
        file_ = open_file(file, "ab");
    } else {
        // new, of another version, or cut short: written again from what
        // was whole
        file_ = open_file(file, "wb");
        if (!file_) return false;
        const uint32_t header[2] = {MAGIC, VERSION};
        if (valid)
            std::fwrite(words.data(), 4, valid, file_);
        else
            std::fwrite(header, 4, 2, file_);
        std::fflush(file_);
    }
    return file_ != nullptr;
}

void ShaderStorage::append(uint32_t kind, std::span<const uint32_t> words) {
    if (!file_) return;
    const uint32_t head[2] = {kind, uint32_t(words.size())};
    std::fwrite(head, 4, 2, file_);
    if (!words.empty()) std::fwrite(words.data(), 4, words.size(), file_);
    std::fflush(file_);
}

bool ShaderStorage::add_shader(uint32_t type, uint64_t hash, std::span<const uint32_t> code) {
    std::lock_guard lock(mutex_);
    if (!shader_hashes_.insert(hash).second) return false;
    shaders_.push_back({type, hash, std::vector<uint32_t>(code.begin(), code.end())});
    std::vector<uint32_t> words{type, uint32_t(hash), uint32_t(hash >> 32)};
    words.insert(words.end(), code.begin(), code.end());
    append(SHADER, words);
    return true;
}

bool ShaderStorage::add_pipeline(std::span<const uint32_t> description) {
    std::lock_guard lock(mutex_);
    if (!pipeline_hashes_.insert(hash(0, description)).second) return false;
    pipelines_.emplace_back(description.begin(), description.end());
    append(PIPELINE, description);
    return true;
}

void ShaderStorage::add_built(const Built &b) {
    std::lock_guard lock(mutex_);
    built_.push_back(b);
    std::vector<uint32_t> words{b.vendor, b.device, b.driver, 0, 0, 0, 0, uint32_t(b.libraries.size()),
                                uint32_t(b.pipelines.size())};
    std::memcpy(&words[3], b.uuid.data(), 16);
    for (const auto *list : {&b.libraries, &b.pipelines})
        for (uint64_t h : *list) words.insert(words.end(), {uint32_t(h), uint32_t(h >> 32)});
    append(BUILT, words);
}

std::vector<ShaderStorage::Shader> ShaderStorage::shaders() const {
    std::lock_guard lock(mutex_);
    return shaders_;
}

std::vector<std::vector<uint32_t>> ShaderStorage::pipelines() const {
    std::lock_guard lock(mutex_);
    return pipelines_;
}

std::vector<ShaderStorage::Built> ShaderStorage::built() const {
    std::lock_guard lock(mutex_);
    return built_;
}

bool ShaderStorage::has_shader(uint64_t hash) const {
    std::lock_guard lock(mutex_);
    return shader_hashes_.count(hash) != 0;
}

} // namespace xf
