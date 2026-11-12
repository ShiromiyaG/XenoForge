/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Writing STFS packages (package.h). Xenia has no STFS writer: its content manager keeps a save as a host
// directory and only reads packages. The layout written here is the one its reader (package.cpp, translated from
// stfs_container_device.cc and stfs_xbox.h) and the console read, taken from them and from the Free60 wiki's
// STFS page (docs/System-Software/Formats/STFS.md, in refs/):
//   - the header, then the volume from the header's size rounded up to 0x1000: blocks of 0x1000 bytes, a hash
//     table pair (primary, secondary) before every 170 data blocks, one more pair at each level above (170 level-0
//     tables under a level-1 table, 170 of those under the one level-2 table);
//   - a hash entry is the SHA-1 of the block below it, a status byte (0x80 in use) and the next block of the file
//     as 24 bits (0xFFFFFF at the end); the entries above level 0 carry bit 0x40 of the status to say the
//     secondary table is the active one, which is never set here;
//   - block 0 starts the file table (0x40-byte entries, directories before what they hold), the files follow,
//     each in consecutive blocks; the volume descriptor holds the table's place and the SHA-1 of the top table.
// Where it differs from a console's package: the CON signature is left zero (the console's private key is not
// ours to have), and no blocks are left free (a console leaves room to grow; this volume is rewritten whole at
// every close, so it never grows in place).

#include "xenoforge/package.h"

#include "xenoforge/bytes.h"
#include "xenoforge/sha1.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace xf {
namespace {

constexpr size_t HEADER_BYTES = 0x344 + 0x93D6;
constexpr size_t META = 0x344;
constexpr uint64_t BLOCK = 0x1000;
constexpr uint32_t PER_L0 = 170, PER_L1 = 28900, PER_L2 = 4913000;
constexpr uint32_t STEP0 = PER_L0 + 2, STEP1 = PER_L1 + (PER_L0 + 1) * 2; // the read-write format: two tables each
constexpr uint32_t END = 0xFFFFFF;
constexpr size_t NAME_MAX_BYTES = 0x28;

void put_be16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, uint32_t(v >> 32));
    put_be32(p + 4, uint32_t(v));
}
void put_le24(uint8_t *p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16); }
void put_le16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }

// Physical block of a data block, and of the hash table pair at a level for the block that starts it (the
// formulas of StfsContainerDevice::BlockToOffset and ComputeBlockHashBlockNumber for two tables per level)
uint64_t physical_block(uint64_t index) {
    const uint32_t bases[3] = {PER_L0, PER_L1, PER_L2};
    uint64_t block = index;
    for (uint32_t base : bases) {
        block += ((index + base) / base) * 2;
        if (index < base) break;
    }
    return block;
}

uint64_t hash_table_block(uint32_t index, uint32_t level) {
    const uint32_t per[2] = {PER_L0, PER_L1}, step[2] = {STEP0, STEP1};
    if (level == 2) return step[1];
    if (index < per[level]) return level == 0 ? 0 : step[level - 1];
    uint64_t block = uint64_t(index / per[level]) * step[level];
    if (level == 0) {
        block += uint64_t(index / PER_L1 + 1) * 2;
        if (index < PER_L1) return block;
    }
    return block + 2;
}

// FILETIME -> the FAT date and time an entry holds
void fat_from_filetime(uint64_t filetime, uint8_t *date, uint8_t *time) {
    int64_t seconds = int64_t(filetime / 10000000ull) - 11644473600ll;
    if (seconds < 315532800) seconds = 315532800; // 1980-01-01, the FAT epoch
    int64_t days = seconds / 86400, rest = seconds % 86400;
    // civil from days (the inverse of fat_timestamp's calendar)
    int64_t z = days + 719468;
    int64_t era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = yoe + era * 400 + (month <= 2);
    year = std::clamp<int64_t>(year, 1980, 2107);
    put_be16(date, uint32_t((year - 1980) << 9 | month << 5 | day));
    put_be16(time, uint32_t((rest / 3600) << 11 | ((rest / 60) % 60) << 5 | (rest % 60) / 2));
}

uint64_t filetime_of(const std::filesystem::path &path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    return uint64_t(seconds + 11644473600ll) * 10000000ull;
}

// A name as the table keeps it: Windows-1252, 1 to 40 bytes
bool table_name(const std::filesystem::path &name, std::string &out) {
    std::u8string u8 = name.u8string();
    out.clear();
    for (size_t i = 0; i < u8.size();) {
        uint32_t c = u8[i], n = 0;
        if (c >= 0xF0)
            c &= 7, n = 3;
        else if (c >= 0xE0)
            c &= 15, n = 2;
        else if (c >= 0xC0)
            c &= 31, n = 1;
        i++;
        for (; n && i < u8.size(); n--, i++) c = c << 6 | (u8[i] & 0x3F);
        out += (c < 0x100) ? char(c) : '_'; // Latin-1 as Windows-1252 has it for 0xA0-0xFF; the rest has no byte
    }
    return !out.empty() && out.size() <= NAME_MAX_BYTES;
}

struct Item {
    std::string name;
    std::filesystem::path host;
    bool directory = false;
    uint16_t parent = 0xFFFF;
    uint64_t size = 0, create = 0, write = 0;
    uint32_t first = 0, blocks = 0;
};

bool collect(const std::filesystem::path &dir, uint16_t parent, std::vector<Item> &items, std::string &error) {
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(*it);
    if (ec) return error = "cannot list " + dir.string(), false;
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.path() < b.path(); });
    for (const auto &e : entries) {
        const bool is_dir = e.is_directory(ec), is_file = e.is_regular_file(ec);
        if (!is_dir && !is_file) continue;
        Item item;
        if (!table_name(e.path().filename(), item.name)) {
            error = "the name " + e.path().filename().string() + " does not fit an STFS entry (1 to 40 bytes)";
            return false;
        }
        item.host = e.path();
        item.directory = is_dir;
        item.parent = parent;
        item.size = is_dir ? 0 : e.file_size(ec);
        item.write = filetime_of(e.path());
        item.create = item.write;
        if (items.size() >= 0xFFFE) return error = "too many files for one package", false;
        const uint16_t index = uint16_t(items.size());
        items.push_back(std::move(item));
        if (is_dir && !collect(e.path(), index, items, error)) return false;
    }
    return true;
}

int seek64(std::FILE *f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::FILE *open_for_write(const std::filesystem::path &path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

std::FILE *open_for_read(const std::filesystem::path &path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

struct Hash {
    uint8_t sha1[20];
};

} // namespace

bool write_stfs_package(const std::filesystem::path &out, const std::vector<uint8_t> &header_in,
                        const std::filesystem::path &directory, std::string &error) {
    if (header_in.size() < HEADER_BYTES) return error = "the header is too short", false;
    std::vector<Item> items;
    if (!collect(directory, 0xFFFF, items, error)) return false;

    // the layout: the file table's blocks first, then each file's, consecutive
    const uint32_t table_blocks = uint32_t(std::max<size_t>(1, (items.size() * 0x40 + BLOCK - 1) / BLOCK));
    uint64_t next = table_blocks;
    for (Item &item : items) {
        if (item.directory) continue;
        item.blocks = uint32_t((item.size + BLOCK - 1) / BLOCK);
        item.first = item.blocks ? uint32_t(next) : 0;
        next += item.blocks;
    }
    if (next >= PER_L2) return error = "the package would hold more blocks than STFS has hash levels for", false;
    const uint32_t total = uint32_t(next);
    const uint32_t groups = (total + PER_L0 - 1) / PER_L0;
    const uint32_t levels = total < PER_L0 ? 0 : total < PER_L1 ? 1 : 2; // what the reader checks

    std::vector<uint8_t> header = header_in;
    const uint32_t header_size = be32(header.data() + 0x340) ? be32(header.data() + 0x340) : uint32_t(HEADER_BYTES);
    const uint64_t data_base = (uint64_t(header_size) + BLOCK - 1) / BLOCK * BLOCK;

    std::filesystem::path temp = out;
    temp += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::FILE *f = open_for_write(temp);
    if (!f) return error = "cannot create " + temp.string(), false;
    uint64_t end_of_file = data_base;
    bool ok = true;
    auto put_block = [&](uint64_t physical, const uint8_t *data) {
        if (!ok) return;
        uint64_t at = data_base + physical * BLOCK;
        if (seek64(f, at) != 0 || std::fwrite(data, 1, BLOCK, f) != BLOCK) ok = false;
        end_of_file = std::max(end_of_file, at + BLOCK);
    };

    // level 0's entries for every block, kept until their tables are written
    std::vector<uint8_t> level0(size_t(groups) * BLOCK, 0);
    auto entry0 = [&](uint32_t index) { return level0.data() + size_t(index / PER_L0) * BLOCK + (index % PER_L0) * 0x18; };
    auto record = [&](uint32_t index, const uint8_t *block, uint32_t next_block) {
        uint8_t *e = entry0(index);
        sha1({block, size_t(BLOCK)}, e);
        e[0x14] = 0x80;
        e[0x15] = uint8_t(next_block >> 16), e[0x16] = uint8_t(next_block >> 8), e[0x17] = uint8_t(next_block);
        put_block(physical_block(index), block);
    };

    // the file table
    std::vector<uint8_t> table(size_t(table_blocks) * BLOCK, 0);
    for (size_t i = 0; i < items.size(); i++) {
        const Item &item = items[i];
        uint8_t *e = table.data() + i * 0x40;
        std::memcpy(e, item.name.data(), item.name.size());
        e[0x28] = uint8_t(item.name.size() | (item.directory ? 0x80 : 0) | (!item.directory && item.blocks ? 0x40 : 0));
        put_le24(e + 0x29, item.blocks);
        put_le24(e + 0x2C, item.blocks);
        put_le24(e + 0x2F, item.first);
        put_be16(e + 0x32, item.parent);
        put_be32(e + 0x34, uint32_t(item.size));
        fat_from_filetime(item.create, e + 0x38, e + 0x3A);
        fat_from_filetime(item.write, e + 0x3C, e + 0x3E);
    }
    for (uint32_t b = 0; b < table_blocks; b++) record(b, table.data() + size_t(b) * BLOCK, b + 1 < table_blocks ? b + 1 : END);

    // the files
    std::vector<uint8_t> block(BLOCK);
    for (const Item &item : items) {
        if (item.directory || !item.blocks) continue;
        std::FILE *in = open_for_read(item.host);
        if (!in) {
            error = "cannot read " + item.host.string();
            ok = false;
            break;
        }
        for (uint32_t b = 0; b < item.blocks && ok; b++) {
            std::fill(block.begin(), block.end(), 0);
            size_t want = size_t(std::min<uint64_t>(BLOCK, item.size - uint64_t(b) * BLOCK));
            if (std::fread(block.data(), 1, want, in) != want) {
                error = item.host.string() + " changed while it was written";
                ok = false;
                break;
            }
            record(item.first + b, block.data(), b + 1 < item.blocks ? item.first + b + 1 : END);
        }
        std::fclose(in);
        if (!ok) break;
    }

    // the tables: level 0 for every group, then the levels above, each entry the hash of the table below
    std::vector<uint8_t> zero(BLOCK, 0);
    Hash top{};
    if (ok) {
        for (uint32_t g = 0; g < groups; g++) {
            uint64_t at = hash_table_block(g * PER_L0, 0);
            put_block(at, level0.data() + size_t(g) * BLOCK);
            put_block(at + 1, zero.data());
        }
        if (levels == 0) {
            sha1({level0.data(), size_t(BLOCK)}, top.sha1);
        } else {
            const uint32_t l1_count = (groups + PER_L0 - 1) / PER_L0;
            std::vector<uint8_t> level1(size_t(l1_count) * BLOCK, 0);
            for (uint32_t g = 0; g < groups; g++)
                sha1({level0.data() + size_t(g) * BLOCK, size_t(BLOCK)},
                     level1.data() + size_t(g / PER_L0) * BLOCK + (g % PER_L0) * 0x18);
            for (uint32_t t = 0; t < l1_count; t++) {
                uint64_t at = hash_table_block(t * PER_L1, 1);
                put_block(at, level1.data() + size_t(t) * BLOCK);
                put_block(at + 1, zero.data());
            }
            if (levels == 1) {
                sha1({level1.data(), size_t(BLOCK)}, top.sha1);
            } else {
                std::vector<uint8_t> level2(BLOCK, 0);
                for (uint32_t t = 0; t < l1_count; t++)
                    sha1({level1.data() + size_t(t) * BLOCK, size_t(BLOCK)}, level2.data() + t * 0x18);
                uint64_t at = hash_table_block(0, 2);
                put_block(at, level2.data());
                put_block(at + 1, zero.data());
                sha1({level2.data(), size_t(BLOCK)}, top.sha1);
            }
        }
    }

    // the header, with the volume descriptor and the sizes filled in
    if (ok) {
        uint8_t *m = header.data() + META;
        const uint64_t content_size = uint64_t(total) * BLOCK;
        put_be32(header.data(), PACKAGE_MAGIC_CON);
        put_be32(header.data() + 0x340, header_size);
        put_be64(m + 8, content_size);
        uint8_t *v = m + 0x35;
        std::memset(v, 0, 0x24);
        v[0] = 0x24;
        v[1] = 0;
        v[2] = 0; // flags: read-write format (two tables per level), the primary table active
        put_le16(v + 3, table_blocks);
        put_le24(v + 5, 0);
        std::memcpy(v + 8, top.sha1, 20);
        put_be32(v + 0x1C, total);
        put_be32(v + 0x20, 0);
        put_be32(m + 0x59, 1); // data file count
        put_be64(m + 0x5D, content_size);
        put_be32(m + 0x65, 0); // STFS
        // the content ID: SHA-1 of the metadata, from 0x344 to the header's size
        sha1({header.data() + META, size_t(header_size) - META}, header.data() + 0x32C);
        std::vector<uint8_t> padded(size_t(data_base), 0);
        std::memcpy(padded.data(), header.data(), std::min<size_t>(header_size, padded.size()));
        if (seek64(f, 0) != 0 || std::fwrite(padded.data(), 1, padded.size(), f) != padded.size()) ok = false;
    }
    if (std::fclose(f) != 0) ok = false;
    if (ok) {
        std::filesystem::resize_file(temp, end_of_file, ec); // the last table pairs end the file when no data follows
        std::filesystem::rename(temp, out, ec);
        if (ec) {
            error = "cannot rename " + temp.string() + ": " + ec.message();
            ok = false;
        }
    } else if (error.empty()) {
        error = "cannot write " + temp.string();
    }
    if (!ok) std::filesystem::remove(temp, ec);
    return ok;
}

bool extract_package(const Package &package, const std::filesystem::path &directory, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    struct Walk {
        const Package &p;
        std::string &error;
        bool run(const PackageNode &dir, const std::filesystem::path &at) {
            std::error_code ec;
            for (const auto &child : dir.children) {
                std::filesystem::path path = at / std::filesystem::path(std::u8string(child->name.begin(), child->name.end()));
                if (child->directory) {
                    std::filesystem::create_directories(path, ec);
                    if (!run(*child, path)) return false;
                    continue;
                }
                std::FILE *f = open_for_write(path);
                if (!f) return error = "cannot create " + path.string(), false;
                std::vector<uint8_t> buffer(1 << 16);
                bool good = true;
                for (uint64_t at_byte = 0; at_byte < child->size && good;) {
                    size_t want = size_t(std::min<uint64_t>(buffer.size(), child->size - at_byte));
                    size_t got = p.read(*child, at_byte, buffer.data(), want);
                    if (!got) break; // a chain the package does not hold: the file ends short, as a read of it does
                    good = std::fwrite(buffer.data(), 1, got, f) == got;
                    at_byte += got;
                }
                if (std::fclose(f) != 0) good = false;
                if (!good) return error = "cannot write " + path.string(), false;
            }
            return true;
        }
    } walk{package, error};
    return walk.run(package.root(), directory);
}

} // namespace xf
