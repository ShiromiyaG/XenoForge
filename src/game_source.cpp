#include "xenoforge/game_source.h"

#include "xenoforge/disc.h"
#include "xenoforge/package.h"

#include <cctype>
#include <cstring>
#include <fstream>

namespace xf {
namespace {

void list(const DiscEntry &dir, const std::string &prefix, std::vector<GameSource::File> &out) {
    for (const DiscEntry &e : dir.children) {
        std::string path = prefix.empty() ? e.name : prefix + "\\" + e.name;
        if (e.directory)
            list(e, path, out);
        else
            out.push_back({std::move(path), e.size, &e, nullptr});
    }
}

void list(const PackageNode &dir, const std::string &prefix, std::vector<GameSource::File> &out) {
    for (const std::unique_ptr<PackageNode> &e : dir.children) {
        std::string path = prefix.empty() ? e->name : prefix + "\\" + e->name;
        if (e->directory)
            list(*e, path, out);
        else
            out.push_back({std::move(path), e->size, nullptr, e.get()});
    }
}

bool same_path(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        const char x = a[i] == '/' ? '\\' : char(std::tolower(uint8_t(a[i])));
        const char y = b[i] == '/' ? '\\' : char(std::tolower(uint8_t(b[i])));
        if (x != y) return false;
    }
    return true;
}

} // namespace

GameSource::GameSource() = default;
GameSource::~GameSource() = default;

bool GameSource::is_package(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    char magic[4] = {};
    if (!f.read(magic, 4)) return false;
    return !std::memcmp(magic, "CON ", 4) || !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4);
}

std::unique_ptr<GameSource> GameSource::open(const std::filesystem::path &path, std::string &error) {
    std::unique_ptr<GameSource> source(new GameSource);
    if (is_package(path)) {
        source->package_ = Package::open(path, error);
        if (!source->package_) return nullptr;
        list(source->package_->root(), "", source->files_);
    } else {
        source->disc_ = Disc::open(path, error);
        if (!source->disc_) return nullptr;
        list(source->disc_->root(), "", source->files_);
    }
    return source;
}

const GameSource::File *GameSource::find(std::string_view path) const {
    while (!path.empty() && (path.front() == '\\' || path.front() == '/')) path.remove_prefix(1);
    for (const File &f : files_)
        if (same_path(f.path, path)) return &f;
    return nullptr;
}

size_t GameSource::read(const File &file, uint64_t offset, void *out, size_t size) const {
    if (offset >= file.size) return 0;
    if (size > file.size - offset) size = size_t(file.size - offset);
    if (file.package) return package_->read(*file.package, offset, out, size);
    return disc_->read(*file.disc, offset, out, size) ? size : 0;
}

std::vector<uint8_t> GameSource::read_all(const File &file) const {
    return file.package ? package_->read_all(*file.package) : disc_->read_all(*file.disc);
}

} // namespace xf
