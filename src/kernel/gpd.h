// XDBF files (a game's SPA, a profile's GPDs): read and written whole.
// Written from Free60's XDBF and GPD pages and checked against Xenia's
// xdbf_io.h, gpd_info.cc and gpd_info_title.cc (BSD; see LICENSE-xenia): the
// header (magic, version 0x10000, entry and free-table counts), the entry
// table (section, id, offset, size), the free table and the data, all
// big-endian. Entries are kept sorted by section and id; a written file is
// laid out compactly, its entry table padded to a multiple of 512 entries as
// Xenia's, with the one free entry that marks the end.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xf::kernel {

// the sections of a GPD (Xenia's GpdSection)
namespace gpd_section {
constexpr uint16_t ACHIEVEMENT = 1, IMAGE = 2, SETTING = 3, TITLE = 4, STRING = 5;
}
// a SPA's metadata section and its achievement table's id ("XACH")
constexpr uint16_t SPA_METADATA = 1;
constexpr uint64_t SPA_ID_XACH = 0x58414348, SPA_ID_XSTC = 0x58535443;
constexpr uint16_t SPA_STRINGS = 3;

struct Gpd {
    std::map<std::pair<uint16_t, uint64_t>, std::vector<uint8_t>> entries; // (section, id)

    // false when the bytes are not an XDBF file (the entries read so far are kept)
    bool parse(const uint8_t *data, size_t size);
    std::vector<uint8_t> serialize() const;
    const std::vector<uint8_t> *find(uint16_t section, uint64_t id) const;
};

// a GPD achievement entry (X_XDBF_GPD_ACHIEVEMENT: 0x1C bytes, then the
// label, description and unachieved description as UTF-16)
constexpr uint32_t ACHIEVEMENT_ACHIEVED = 0x20000; // AchievementFlags::kAchieved
struct Achievement {
    uint32_t id = 0, image_id = 0, gamerscore = 0, flags = 0;
    uint64_t unlock_time = 0; // FILETIME
    std::u16string label, description, unachieved;

    std::vector<uint8_t> encode() const;
    bool decode(const std::vector<uint8_t> &bytes);
    bool unlocked() const { return flags & ACHIEVEMENT_ACHIEVED; }
};

// a SPA's achievements (its XACH table: 36 bytes each, the strings by id in
// the language asked, else the file's default language, else English)
std::vector<Achievement> spa_achievements(const Gpd &spa, uint32_t language);

// the entry of a profile setting (X_XDBF_GPD_SETTING_HEADER, 0x18 bytes:
// id, 0, type, 7 bytes, the 8-byte value; strings and binaries follow)
std::vector<uint8_t> encode_setting(uint32_t id, uint8_t type, const uint8_t value[8],
                                    const std::vector<uint8_t> &data);
bool decode_setting(const std::vector<uint8_t> &bytes, uint32_t &id, uint8_t &type, uint8_t value[8],
                    std::vector<uint8_t> &data);

} // namespace xf::kernel
