#include "xenoforge/profile_gpd.h"

#include "kernel/gpd.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace xf {

std::filesystem::path profile_gpd_path(const std::filesystem::path &storage, const std::string &xuid,
                                       uint32_t title_id) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X.gpd", title_id);
    return storage / "content" / xuid / "FFFE07D1" / "00010000" / xuid / name;
}

bool read_gpd_achievements(const std::filesystem::path &gpd_file, std::vector<kernel::ProfileAchievement> &out) {
    out.clear();
    std::ifstream in(gpd_file, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    kernel::Gpd gpd;
    if (!gpd.parse(bytes.data(), bytes.size())) return false;
    for (const auto &[key, entry] : gpd.entries) {
        kernel::Achievement a;
        if (key.first != kernel::gpd_section::ACHIEVEMENT || !a.decode(entry)) continue;
        kernel::ProfileAchievement p;
        p.id = a.id, p.gamerscore = a.gamerscore, p.image_id = a.image_id, p.unlocked = a.unlocked();
        p.unlock_time = p.unlocked ? a.unlock_time : 0;
        p.label = a.label, p.description = a.description, p.unachieved = a.unachieved;
        if (const std::vector<uint8_t> *png = gpd.find(kernel::gpd_section::IMAGE, a.image_id)) p.icon = *png;
        out.push_back(std::move(p));
    }
    return true;
}

} // namespace xf
