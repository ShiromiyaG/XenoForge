#include "xenoforge/title_update.h"

#include "xenoforge/game_source.h"
#include "xenoforge/package.h"

#include <cstdio>

namespace xf {

TitleUpdate::TitleUpdate() = default;
TitleUpdate::~TitleUpdate() = default;

std::unique_ptr<TitleUpdate> TitleUpdate::open(const std::filesystem::path &package, std::string &error) {
    std::unique_ptr<TitleUpdate> update(new TitleUpdate);
    update->path_ = package;
    update->package_ = Package::open(package, error);
    if (!update->package_) return nullptr;
    if (update->package_->info().content_type != CONTENT_TYPE) {
        error = "not a title update (another content type)";
        return nullptr;
    }
    return update;
}

uint32_t TitleUpdate::title_id() const { return package_->info().title_id; }
uint32_t TitleUpdate::version() const { return package_->info().version; }

std::filesystem::path TitleUpdate::find(uint32_t title_id, std::span<const std::filesystem::path> roots) {
    char title[9], type[9];
    std::snprintf(title, sizeof(title), "%08X", title_id);
    std::snprintf(type, sizeof(type), "%08X", CONTENT_TYPE);
    std::filesystem::path best;
    uint32_t best_version = 0;
    for (const std::filesystem::path &root : roots) {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(root / title / type, ec)) {
            if (!entry.is_regular_file(ec) || !GameSource::is_package(entry.path())) continue;
            std::string why;
            std::unique_ptr<TitleUpdate> update = open(entry.path(), why);
            if (!update || update->title_id() != title_id) continue;
            if (best.empty() || update->version() > best_version) best = entry.path(), best_version = update->version();
        }
    }
    return best;
}

std::vector<uint8_t> TitleUpdate::patch(std::string_view module_path) const {
    while (!module_path.empty() && (module_path.front() == '\\' || module_path.front() == '/'))
        module_path.remove_prefix(1);
    std::string path(module_path);
    path += 'p';
    const PackageNode *node = package_->find(path);
    if (!node || node->directory) return {};
    return package_->read_all(*node);
}

} // namespace xf
