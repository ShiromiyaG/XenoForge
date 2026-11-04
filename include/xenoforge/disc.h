// The game disc's file system (XDVDFS, the "GDFX" of Xenia), read straight
// from an ISO: a full XGD2/XGD3 dump (the video partition first) or a
// rebuilt image with the game partition alone. Translated from Xenia's
// DiscImageDevice (see LICENSE-xenia).

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xf {

struct DiscEntry {
    std::string name;
    bool directory = false;
    uint64_t offset = 0; // in the image, for files
    uint64_t size = 0;
    std::vector<DiscEntry> children; // directories, in the disc's (sorted) order
};

class Disc {
public:
    static std::unique_ptr<Disc> open(const std::filesystem::path &path, std::string &error);
    ~Disc();

    const DiscEntry &root() const { return root_; }
    uint64_t game_offset() const { return game_offset_; }

    // A path such as "default.xex" or "media/sound.xwb", either slash, any case.
    const DiscEntry *find(std::string_view path) const;

    bool read(const DiscEntry &file, uint64_t offset, void *out, size_t size) const;
    std::vector<uint8_t> read_all(const DiscEntry &file) const;

private:
    Disc() = default;
    bool read_at(uint64_t offset, void *out, size_t size) const;
    bool read_directory(uint32_t sector, uint32_t size, DiscEntry &into, int depth, std::string &error);

    std::FILE *file_ = nullptr;
    uint64_t file_size_ = 0;
    uint64_t game_offset_ = 0;
    DiscEntry root_;
};

} // namespace xf
