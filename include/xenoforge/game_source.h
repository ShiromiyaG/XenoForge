// Where a game's files are: a disc image (disc.h) or a content package
// (package.h: an arcade game, a game on demand, a demo), read the same way
// by what looks for the executables and the data before the kernel mounts
// either as the game's device.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xf {

class Disc;
class Package;
struct DiscEntry;
struct PackageNode;

class GameSource {
public:
    struct File {
        std::string path; // from the root, '\' between names
        uint64_t size = 0;
        const DiscEntry *disc = nullptr;
        const PackageNode *package = nullptr;
    };

    // A package where the file begins as one ("CON ", "LIVE", "PIRS"), a disc
    // image otherwise. On failure: null and the reason.
    static std::unique_ptr<GameSource> open(const std::filesystem::path &path, std::string &error);
    // whether a file begins as a package (its first four bytes)
    static bool is_package(const std::filesystem::path &path);
    ~GameSource();

    bool package() const { return package_ != nullptr; }
    const Package *package_file() const { return package_.get(); }
    // every file, directories first to last as the source lists them
    const std::vector<File> &files() const { return files_; }
    // a path such as "default.xex" or "media/sound.xwb", either slash, any case
    const File *find(std::string_view path) const;
    size_t read(const File &file, uint64_t offset, void *out, size_t size) const;
    std::vector<uint8_t> read_all(const File &file) const;

private:
    GameSource();
    std::unique_ptr<Disc> disc_;
    std::unique_ptr<Package> package_;
    std::vector<File> files_;
};

} // namespace xf
