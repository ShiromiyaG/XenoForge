/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Content packages (package.h). Translated from Xenia (BSD; see
// LICENSE-xenia): src/xenia/vfs/devices/xcontent_container_device.cc,
// xcontent_devices/stfs_container_device.cc and svod_container_device.cc,
// xcontent_container_file.cc, stfs_xbox.h, and the header's layout from
// src/xenia/kernel/xam/xcontent/xcontent.h; checked against them. Where it
// differs, and why:
//   - the host files are read with positioned reads under a lock, not mapped
//     (Xenia maps the whole package); every read is bounds-checked, so a
//     damaged package fails to open or reads short instead of reading past
//     a mapping;
//   - a file's blocks are kept as runs (a block following the last one in the
//     host file extends it), STFS's as well as SVOD's, and a read finds its run
//     by search (Xenia walks the list);
//   - an STFS entry's parent index and a chain that leaves the package fail
//     the open or end the file short with a warning (Xenia asserts);
//   - an SVOD directory is walked without recursion along its nodes, and an
//     SVOD whose root directory is empty has an empty root;
//   - the SVOD root's time is read as the FILETIME it is (the volume
//     descriptor's 64 bits at 0x1C); Xenia reads the two halves as a FAT date
//     and time, which gives a time that means nothing. Every entry has it, as
//     in Xenia;
//   - a FAT time that is no date (month 0, day 0) is 0; Xenia lets mktime
//     carry it into the month before.

#include "xenoforge/package.h"

#include "xenoforge/bytes.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace xf {
namespace {

constexpr size_t HEADER_BYTES = 0x344 + 0x93D6; // XContentHeader + XContentMetadata
constexpr size_t META = 0x344;
constexpr uint64_t BLOCK = 0x1000; // XContentContainerDevice::kBlockSize

uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint64_t le64(const uint8_t *p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }
uint32_t le24(const uint8_t *p) { return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; } // load_uint24_le

uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

std::u16string u16(const uint8_t *p, size_t max) {
    std::u16string s;
    for (size_t i = 0; i < max; i++) {
        char16_t c = char16_t(be16(p + 2 * i));
        if (!c) break;
        s += c;
    }
    return s;
}

bool equal_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
           });
}

std::string join(const std::string &parent, const std::string &name) {
    return parent.empty() ? name : parent + "/" + name;
}

// STFS: hash levels and blocks per table
constexpr uint32_t BLOCKS_PER_HASH_LEVEL[3] = {170, 28900, 4913000};
constexpr uint32_t END_OF_CHAIN = 0xFFFFFF;
// SVOD
constexpr uint32_t LAYOUT_UNKNOWN = 0, LAYOUT_EGDF = 1, LAYOUT_XSF = 2, LAYOUT_SINGLE = 4, LAYOUT_MULTIPLE = 8;
constexpr char MEDIA_MAGIC[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t MEDIA_MAGIC_SIZE = 20;
constexpr uint32_t BAD_FILE = 0xFFFFFFFF;

// appends a piece to a file's runs, extending the last when it follows it
void add_run(PackageNode &node, uint32_t file, uint64_t offset, uint64_t length) {
    if (!node.runs.empty()) {
        PackageRun &last = node.runs.back();
        if (last.file == file && last.offset + last.length == offset) {
            last.length += length;
            return;
        }
    }
    uint64_t start = node.runs.empty() ? 0 : node.runs.back().start + node.runs.back().length;
    node.runs.push_back({file, offset, length, start});
}

} // namespace

std::string package_name_to_utf8(std::string_view ansi) {
    // Windows-1252: 0xA0-0xFF are Latin-1, 0x80-0x9F as below (the five it
    // leaves out stay the control character of that value, as Windows maps them)
    static const uint16_t high[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                      0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    std::string out;
    for (unsigned char c : ansi) {
        uint32_t cp = (c >= 0x80 && c < 0xA0) ? high[c - 0x80] : c;
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | cp >> 6);
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xE0 | cp >> 12);
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// decode_fat_timestamp (stfs_xbox.h): date = year-1980 (7 bits), month (4),
// day (5); time = hour (5), minute (6), second / 2 (5)
uint64_t fat_timestamp(uint32_t date, uint32_t time) {
    int64_t year = int64_t((date & 0xFE00) >> 9) + 1980;
    int64_t month = (date & 0x01E0) >> 5, day = date & 0x1F;
    if (month < 1 || month > 12 || day < 1) return 0;
    // days from 1970-01-01 (the civil-calendar algorithm)
    int64_t y = month <= 2 ? year - 1 : year;
    int64_t era = y / 400, yoe = y - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    int64_t seconds = days * 86400 + int64_t((time & 0xF800) >> 11) * 3600 + int64_t((time & 0x07E0) >> 5) * 60 +
                      int64_t(time & 0x1F) * 2;
    return uint64_t(seconds + 11644473600ll) * 10000000ull;
}

Package::~Package() {
    for (auto &f : files_)
        if (f && f->file) std::fclose(f->file);
}

// ---- host files ----

namespace {
int seek64(std::FILE *f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::unique_ptr<std::FILE, int (*)(std::FILE *)> open_host(const std::filesystem::path &path, uint64_t &size) {
#ifdef _WIN32
    std::FILE *f = _wfopen(path.c_str(), L"rb");
#else
    std::FILE *f = std::fopen(path.c_str(), "rb");
#endif
    size = 0;
    if (f) {
        seek64(f, 0);
        std::fseek(f, 0, SEEK_END);
#ifdef _WIN32
        size = uint64_t(_ftelli64(f));
#else
        size = uint64_t(ftello(f));
#endif
    }
    return {f, &std::fclose};
}
} // namespace

size_t Package::read_some(uint32_t file, uint64_t offset, void *out, size_t length) const {
    if (file >= files_.size() || offset >= files_[file]->size) return 0;
    const HostFile &f = *files_[file];
    length = size_t(std::min<uint64_t>(length, f.size - offset));
    std::lock_guard<std::mutex> l(f.mutex);
    if (seek64(f.file, offset) != 0) return 0;
    return std::fread(out, 1, length, f.file);
}

bool Package::read_at(uint32_t file, uint64_t offset, void *out, size_t length) const {
    return read_some(file, offset, out, length) == length;
}

// ---- opening ----

std::unique_ptr<Package> Package::open(const std::filesystem::path &path, std::string &error) {
    std::unique_ptr<Package> p(new Package());
    uint64_t size = 0;
    auto host = open_host(path, size);
    if (!host) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    auto file = std::make_unique<HostFile>();
    file->file = host.release();
    file->size = size;
    p->files_.push_back(std::move(file));
    if (!p->read_header(error)) return nullptr;
    if (p->info_.volume_type == PackageVolume::Stfs) {
        if (!p->read_stfs(error)) return nullptr;
    } else if (p->info_.volume_type == PackageVolume::Svod) {
        if (!p->read_svod(path, error)) return nullptr;
    } else {
        error = "unknown volume type " + std::to_string(uint32_t(p->info_.volume_type));
        return nullptr;
    }
    return p;
}

bool Package::read_header(std::string &error) {
    std::vector<uint8_t> h(HEADER_BYTES);
    if (!read_at(0, 0, h.data(), h.size())) {
        error = "too small for a package header";
        return false;
    }
    PackageInfo &i = info_;
    i.magic = be32(h.data());
    if (i.magic != PACKAGE_MAGIC_CON && i.magic != PACKAGE_MAGIC_LIVE && i.magic != PACKAGE_MAGIC_PIRS) {
        error = "not a package (no CON, LIVE or PIRS magic)";
        return false;
    }
    // XContentHeader: magic, signature (0x228), licenses (16 of 0x10), content id, header size
    for (size_t n = 0; n < 16; n++) {
        const uint8_t *l = h.data() + 4 + 0x228 + 0x10 * n;
        i.licenses[n] = {be64(l), be32(l + 8), be32(l + 12)};
    }
    std::memcpy(i.content_id.data(), h.data() + 0x32C, 0x14);
    i.header_size = be32(h.data() + 0x340);

    const uint8_t *m = h.data() + META;
    i.content_type = be32(m);
    i.metadata_version = be32(m + 4);
    i.content_size = be64(m + 8);
    i.media_id = be32(m + 0x10), i.version = be32(m + 0x14), i.base_version = be32(m + 0x18);
    i.title_id = be32(m + 0x1C);
    i.platform = m[0x20], i.executable_type = m[0x21], i.disc_number = m[0x22], i.disc_count = m[0x23];
    i.save_game_id = be32(m + 0x24);
    std::memcpy(i.console_id.data(), m + 0x28, 5);
    i.profile_id = be64(m + 0x2D);
    // the volume descriptor, at 0x35
    const uint8_t *v = m + 0x35;
    i.volume_type = PackageVolume(be32(m + 0x65));
    i.stfs.descriptor_length = v[0], i.stfs.version = v[1], i.stfs.flags = v[2];
    i.stfs.file_table_block_count = le16(v + 3); // little-endian, unlike the rest
    i.stfs.file_table_block_number = le24(v + 5);
    std::memcpy(i.stfs.top_hash_table_hash.data(), v + 8, 0x14);
    i.stfs.total_block_count = be32(v + 0x1C), i.stfs.free_block_count = be32(v + 0x20);
    i.svod.descriptor_length = v[0], i.svod.block_cache_element_count = v[1];
    i.svod.worker_thread_processor = v[2], i.svod.worker_thread_priority = v[3];
    std::memcpy(i.svod.first_fragment_hash_entry.data(), v + 4, 0x14);
    i.svod.features = v[0x18];
    i.svod.num_data_blocks = le24(v + 0x19), i.svod.start_data_block = le24(v + 0x1C);
    i.data_file_count = be32(m + 0x59);
    i.data_file_size = be64(m + 0x5D);
    i.online_creator = be64(m + 0x69);
    i.category = be32(m + 0x71);
    std::memcpy(i.device_id.data(), m + 0xB9, 0x14);
    for (size_t n = 0; n < 12; n++) {
        if (n < 9) {
            i.display_name[n] = u16(m + 0xCD + 0x100 * n, 128);
            i.description[n] = u16(m + 0x9CD + 0x100 * n, 128);
        } else if (i.metadata_version >= 2) {
            i.display_name[n] = u16(m + 0x50D6 + 0x100 * (n - 9), 128);
            i.description[n] = u16(m + 0x90D6 + 0x100 * (n - 9), 128);
        }
    }
    i.publisher = u16(m + 0x12CD, 64);
    i.title_name = u16(m + 0x134D, 64);
    i.flags = m[0x13CD];
    i.thumbnail_size = be32(m + 0x13CE);
    i.title_thumbnail_size = be32(m + 0x13D2);
    // the room a thumbnail has: version 1's, and version 2's (three more
    // languages' names take the rest)
    uint32_t room = i.metadata_version == 1 ? 0x4000 : 0x3D00;
    thumbnail_.assign(m + 0x13D6, m + 0x13D6 + std::min(i.thumbnail_size, room));
    title_thumbnail_.assign(m + 0x53D6, m + 0x53D6 + std::min(i.title_thumbnail_size, room));
    return true;
}

// ---- STFS ----

// StfsContainerDevice::BlockToOffset: data blocks have a hash table (two
// unless the package is read-only) before every 170, and tables of tables
uint64_t Package::stfs_block_offset(uint64_t index) const {
    uint64_t block = index;
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t level_base = BLOCKS_PER_HASH_LEVEL[i];
        block += ((index + level_base) / level_base) * blocks_per_hash_table_;
        if (index < level_base) break;
    }
    return block << 12;
}

uint32_t Package::stfs_hash_block_number(uint32_t block_index, uint32_t level) const {
    if (level == 2) return block_step_[1];
    if (block_index < BLOCKS_PER_HASH_LEVEL[level]) return level == 0 ? 0 : block_step_[level - 1];
    uint32_t block = (block_index / BLOCKS_PER_HASH_LEVEL[level]) * block_step_[level];
    if (level == 0) {
        block += ((block_index / BLOCKS_PER_HASH_LEVEL[1]) + 1) * blocks_per_hash_table_;
        if (block_index < BLOCKS_PER_HASH_LEVEL[1]) return block;
    }
    return block + blocks_per_hash_table_;
}

// UpdateCachedHashTable: reads the level's table (kept by its offset) and
// the record for the block, whose active-index bit says where the next lower
// table of the pair is
bool Package::stfs_hash_table(uint32_t block, uint32_t level, uint32_t &secondary) {
    const uint64_t offset = uint64_t(stfs_hash_block_number(block, level)) << 12;
    auto it = hash_tables_.find(offset);
    if (it == hash_tables_.end()) {
        std::vector<uint8_t> table(0x1000);
        if (!read_at(0, data_base_ + offset + secondary, table.data(), table.size())) return false;
        it = hash_tables_.emplace(offset, std::move(table)).first;
    }
    uint32_t record = block % BLOCKS_PER_HASH_LEVEL[0];
    if (level >= 1) record = (block / BLOCKS_PER_HASH_LEVEL[level - 1]) % BLOCKS_PER_HASH_LEVEL[0];
    // StfsHashEntry: sha1 (0x14), then info: bit 30 the active index
    uint32_t info = be32(it->second.data() + record * 0x18 + 0x14);
    secondary = (info & 0x40000000) ? uint32_t(BLOCK) : 0;
    return true;
}

// GetBlockHash: the level-0 record of the block (its next block in the chain
// in the low 24 bits), after the tables above it
bool Package::stfs_block_hash(uint32_t block_index, uint32_t &info) {
    uint32_t secondary = info_.stfs.root_active_index() ? uint32_t(BLOCK) : 0;
    // GetAmountOfHashLevelsToCheck
    uint32_t levels = 0;
    for (uint32_t level = 0; level < 3; level++) {
        if (info_.stfs.total_block_count < BLOCKS_PER_HASH_LEVEL[level]) {
            levels = level;
            break;
        }
        if (level == 2) levels = 0; // more blocks than the format has: Xenia checks no level
    }
    if (info_.stfs.read_only_format()) {
        // one table per level and only the first checked
        secondary = 0;
        levels = 0;
    }
    for (int level = int(levels); level >= 0; level--)
        if (!stfs_hash_table(block_index, uint32_t(level), secondary)) return false;
    const uint64_t offset = uint64_t(stfs_hash_block_number(block_index, 0)) << 12;
    const uint32_t record = block_index % BLOCKS_PER_HASH_LEVEL[0];
    info = be32(hash_tables_[offset].data() + record * 0x18 + 0x14);
    return true;
}

bool Package::read_stfs(std::string &error) {
    const PackageInfo::Stfs &s = info_.stfs;
    if (s.descriptor_length != 0x24) warnings_.push_back("the STFS volume descriptor is not 0x24 bytes long");
    if (!info_.header_size) {
        error = "the header size is 0";
        return false;
    }
    data_base_ = round_up(info_.header_size, BLOCK);
    blocks_per_hash_table_ = s.read_only_format() ? 1 : 2;
    block_step_[0] = BLOCKS_PER_HASH_LEVEL[0] + blocks_per_hash_table_;
    block_step_[1] = BLOCKS_PER_HASH_LEVEL[1] + (BLOCKS_PER_HASH_LEVEL[0] + 1) * blocks_per_hash_table_;
    read_only_ = s.read_only_format();
    total_units_ = s.total_block_count;
    available_units_ = read_only_ ? 0 : BLOCKS_PER_HASH_LEVEL[2] - (s.total_block_count - s.free_block_count);
    layout_ = "STFS";

    root_.directory = true;
    std::vector<PackageNode *> all;
    uint32_t table_block = s.file_table_block_number;
    uint32_t read_blocks = 0;
    std::vector<uint8_t> block(BLOCK);
    for (uint32_t n = 0; n < s.file_table_block_count; n++) {
        if (!read_at(0, data_base_ + stfs_block_offset(table_block), block.data(), block.size())) {
            error = "the file table is outside the package";
            return false;
        }
        read_blocks++;
        for (size_t m = 0; m < BLOCK / 0x40; m++) {
            const uint8_t *e = block.data() + m * 0x40; // StfsDirectoryEntry
            if (e[0] == 0) break;
            const uint8_t flags = e[0x28]; // name length (6 bits), contiguous, directory
            const uint16_t directory_index = be16(e + 0x32);
            PackageNode *parent = &root_;
            if (directory_index != 0xFFFF) {
                if (directory_index >= all.size() || !all[directory_index]->directory) {
                    error = "a file table entry's directory is not an earlier directory";
                    return false;
                }
                parent = all[directory_index];
            }
            auto node = std::make_unique<PackageNode>();
            node->name = package_name_to_utf8(std::string_view(reinterpret_cast<const char *>(e), flags & 0x3F));
            node->path = join(parent->path, node->name);
            node->directory = flags & 0x80;
            node->size = be32(e + 0x34);
            node->create_time = fat_timestamp(be16(e + 0x38), be16(e + 0x3A));
            node->write_time = fat_timestamp(be16(e + 0x3C), be16(e + 0x3E));
            node->block = le24(e + 0x2F);
            if (!node->directory) {
                // the chain of blocks, from the first through the hash tables
                uint32_t index = node->block;
                uint64_t remaining = node->size;
                uint32_t counted = 0;
                while (remaining && index != END_OF_CHAIN) {
                    uint64_t length = std::min<uint64_t>(BLOCK, remaining);
                    add_run(*node, 0, data_base_ + stfs_block_offset(index), length);
                    remaining -= length;
                    counted++;
                    uint32_t info = 0;
                    if (!stfs_block_hash(index, info)) {
                        warnings_.push_back(node->path + ": a hash table is outside the package");
                        break;
                    }
                    index = info & 0xFFFFFF;
                }
                if (remaining)
                    warnings_.push_back(node->path + ": the chain has " + std::to_string(remaining) +
                                        " bytes fewer than the file");
                if (!remaining && counted != le24(e + 0x2C))
                    warnings_.push_back(node->path + ": the chain has " + std::to_string(counted) +
                                        " blocks, the entry says " + std::to_string(le24(e + 0x2C)));
            }
            all.push_back(node.get());
            parent->children.push_back(std::move(node));
        }
        uint32_t info = 0;
        if (!stfs_block_hash(table_block, info)) {
            error = "the file table's hash table is outside the package";
            return false;
        }
        table_block = info & 0xFFFFFF;
        if (table_block == END_OF_CHAIN) break;
    }
    if (read_blocks != s.file_table_block_count)
        warnings_.push_back("the file table has " + std::to_string(read_blocks) + " blocks, the header says " +
                            std::to_string(s.file_table_block_count));
    return true;
}

// ---- SVOD ----

// SvodContainerDevice::BlockToOffset: a sector of the volume (0x800 bytes)
// as an offset in a data file. The files hold 0x14388 sectors in groups of
// 0x198 under a level-0 hash table of 0x1000 bytes, and a level-1 table at
// the start; a file ends at 0xA290000 bytes.
void Package::svod_block_offset(uint64_t sector, uint64_t &address, uint32_t &file) const {
    constexpr uint64_t BLOCK_SIZE = 0x800, HASH_BLOCK_SIZE = 0x1000, BLOCKS_PER_L0_HASH = 0x198,
                       HASHES_PER_L1_HASH = 0xA1C4, BLOCKS_PER_FILE = 0x14388, MAX_FILE_SIZE = 0xA290000;
    const uint64_t first = uint64_t(info_.svod.start_data_block) * 2;
    if (sector < first) { // a sector before the data: no such place
        address = 0;
        file = BAD_FILE;
        return;
    }
    uint64_t true_block = sector - first;
    if (svod_layout_ == LAYOUT_EGDF) true_block += 2; // the 0x1000 bytes before the descriptor
    uint64_t file_block = true_block % BLOCKS_PER_FILE;
    uint64_t file_index = true_block / BLOCKS_PER_FILE;
    uint64_t offset = 0;
    uint64_t level0_table_count = (file_block / BLOCKS_PER_L0_HASH) + 1;
    offset += level0_table_count * HASH_BLOCK_SIZE;
    uint64_t level1_table_count = (level0_table_count / HASHES_PER_L1_HASH) + 1;
    offset += level1_table_count * HASH_BLOCK_SIZE;
    if (svod_layout_ == LAYOUT_SINGLE) offset += svod_base_offset_; // the header of the single file
    uint64_t block_address = file_block * BLOCK_SIZE + offset;
    if (block_address >= MAX_FILE_SIZE) { // the offset overran the file: into the next
        file_index += 1;
        block_address %= MAX_FILE_SIZE;
        block_address += 0x2000;
    }
    address = block_address;
    file = file_index < files_.size() ? uint32_t(file_index) : BAD_FILE;
}

// SvodContainerDevice::ReadEntry: the directory whose first sector is
// `sector`, a binary tree of entries in sectors of 0x800 bytes, nodes named
// by their ordinal (4-byte units from the first sector, the root's 0), walked
// left, entry, right. A node met twice or directories nested too deep are a
// damaged tree (Xenia would not stop).
bool Package::svod_entry(uint32_t sector, PackageNode &parent, int &budget, int depth, std::string &error) {
    if (depth > 32) {
        error = "the directories are nested too deep or loop";
        return false;
    }
    std::vector<bool> seen(0x10000);
    struct Header {
        uint16_t node_l, node_r;
        uint32_t data_block, length;
        uint8_t attributes, name_length;
        std::string name;
    };
    auto read_header = [&](uint32_t ord, Header &h) {
        if (--budget < 0) {
            error = "the directory tree is too big";
            return false;
        }
        if (ord > 0xFFFF || seen[ord]) {
            error = "a directory's nodes loop";
            return false;
        }
        seen[ord] = true;
        // for directories with many files the ordinal passes into the next sector
        uint64_t ordinal_offset = uint64_t(ord) * 4;
        uint64_t address;
        uint32_t file;
        svod_block_offset(sector + ordinal_offset / 0x800, address, file);
        address += ordinal_offset % 0x800;
        uint8_t raw[0xE];
        if (file == BAD_FILE || !read_at(file, address, raw, sizeof(raw))) {
            error = "cannot read the directory entry in file " + std::to_string(file) + " at offset " +
                    std::to_string(address) + " (ordinal " + std::to_string(ord) + ")";
            return false;
        }
        h.node_l = le16(raw), h.node_r = le16(raw + 2);
        h.data_block = le32(raw + 4), h.length = le32(raw + 8);
        h.attributes = raw[12], h.name_length = raw[13];
        std::string name(h.name_length, '\0');
        if (h.name_length && !read_at(file, address + sizeof(raw), name.data(), name.size())) {
            error = "cannot read a directory entry's name";
            return false;
        }
        h.name = package_name_to_utf8(name);
        return true;
    };

    std::vector<std::pair<uint32_t, Header>> stack;
    uint32_t current = 0;
    bool descend = true;
    while (true) {
        while (descend) {
            Header h;
            if (!read_header(current, h)) return false;
            uint16_t left = h.node_l;
            stack.emplace_back(current, std::move(h));
            if (left) current = left;
            else descend = false;
        }
        if (stack.empty()) return true;
        Header h = std::move(stack.back().second);
        stack.pop_back();

        auto node = std::make_unique<PackageNode>();
        node->name = h.name;
        node->path = join(parent.path, h.name);
        node->block = h.data_block;
        // SVOD entries have no times of their own: the root's, as Xenia
        node->create_time = node->write_time = root_.create_time;
        if (h.attributes & 0x10) {
            node->directory = true;
            if (h.length && !svod_entry(h.data_block, *node, budget, depth + 1, error)) return false;
        } else {
            node->size = h.length;
            // the sectors, one by one; the run list joins those that follow
            uint64_t remaining = round_up(h.length, 0x800);
            for (uint64_t sec = h.data_block; remaining; sec++, remaining -= 0x800) {
                uint64_t address;
                uint32_t file;
                svod_block_offset(sec, address, file);
                if (file == BAD_FILE) {
                    warnings_.push_back(node->path + ": a sector is outside the data files");
                    break;
                }
                add_run(*node, file, address, 0x800);
            }
        }
        parent.children.push_back(std::move(node));
        if (h.node_r) {
            current = h.node_r;
            descend = true;
        }
    }
}

bool Package::read_svod(const std::filesystem::path &path, std::string &error) {
    // the fragments: Data0000, Data0001... in "<path>.data", sorted by name
    std::filesystem::path directory = path;
    directory += ".data";
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        error = "the package is multi-file but " + directory.string() + " does not exist";
        return false;
    }
    std::vector<std::filesystem::path> fragments;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) fragments.push_back(it->path());
    std::sort(fragments.begin(), fragments.end(),
              [](const auto &a, const auto &b) { return a.filename().string() < b.filename().string(); });
    if (fragments.size() != info_.data_file_count || fragments.empty()) {
        error = "the SVOD expects " + std::to_string(info_.data_file_count) + " data fragments, but " +
                std::to_string(fragments.size()) + " are present";
        return false;
    }
    // the header file is not read from again: the fragments replace it
    for (auto &f : files_)
        if (f->file) std::fclose(f->file);
    files_.clear();
    for (const auto &fragment : fragments) {
        uint64_t size = 0;
        auto host = open_host(fragment, size);
        if (!host) {
            error = "cannot open " + fragment.string();
            return false;
        }
        auto file = std::make_unique<HostFile>();
        file->file = host.release();
        file->size = size;
        total_units_ += size;
        files_.push_back(std::move(file));
    }
    total_units_ /= 8 * 0x200; // sectors_per_allocation_unit * bytes_per_sector
    available_units_ = 0;
    read_only_ = true;

    if (files_[0]->size < HEADER_BYTES) {
        error = "the first data file is too small";
        return false;
    }
    // the layouts: where the media descriptor's magic is, and where the
    // volume starts
    uint8_t magic[MEDIA_MAGIC_SIZE];
    auto has_magic = [&](uint64_t at) {
        return read_at(0, at, magic, sizeof(magic)) && std::memcmp(magic, MEDIA_MAGIC, MEDIA_MAGIC_SIZE) == 0;
    };
    uint64_t magic_offset = 0;
    if (info_.svod.enhanced_gdf_layout()) {
        if (!has_magic(0x2000)) {
            error = "the SVOD uses an EGDF layout, but the magic block was not found";
            return false;
        }
        svod_base_offset_ = 0, magic_offset = 0x2000, svod_layout_ = LAYOUT_EGDF;
        layout_ = "EGDF";
    } else if (has_magic(0x12000)) {
        svod_base_offset_ = 0x10000, magic_offset = 0x12000;
        uint8_t xsf[3] = {};
        read_at(0, 0x2000, xsf, 3);
        if (std::memcmp(xsf, "XSF", 3) == 0) {
            svod_layout_ = LAYOUT_XSF;
            layout_ = "XSF (converted by a third-party tool)";
        } else {
            svod_layout_ = LAYOUT_UNKNOWN;
            layout_ = "XSF without its header";
        }
    } else {
        if (!has_magic(info_.data_file_count == 1 ? 0xD000 : 0x2000)) {
            error = "could not locate the SVOD magic block";
            return false;
        }
        // at 0xD000 it is a single file: the STFS header (0xB000) and 0x2000
        // of hash tables come before the volume
        svod_base_offset_ = 0xB000, magic_offset = 0xD000;
        if (files_.size() == 1) {
            svod_layout_ = LAYOUT_SINGLE;
            layout_ = "single file";
        } else {
            svod_base_offset_ = 0, magic_offset = 0x2000, svod_layout_ = LAYOUT_MULTIPLE;
            layout_ = "multiple files";
        }
    }

    // the volume descriptor: root sector and size (4 bytes each), its time
    uint8_t root[0x10];
    if (!read_at(0, magic_offset + 0x14, root, sizeof(root))) {
        error = "cannot read the SVOD's root directory";
        return false;
    }
    const uint32_t root_sector = le32(root), root_size = le32(root + 4);
    root_.directory = true;
    root_.create_time = root_.write_time = le64(root + 8);
    if (!root_size) return true; // as the disc's: nothing there
    int budget = 1 << 20;
    return svod_entry(root_sector, root_, budget, 0, error);
}

// ---- reading ----

const PackageNode *Package::find(std::string_view path) const {
    const PackageNode *node = &root_;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find_first_of("/\\", i);
        if (j == std::string_view::npos) j = path.size();
        std::string_view part = path.substr(i, j - i);
        i = j + 1;
        if (part.empty()) continue;
        const PackageNode *next = nullptr;
        for (const auto &c : node->children)
            if (equal_case(c->name, part)) {
                next = c.get();
                break;
            }
        if (!next) return nullptr;
        node = next;
    }
    return node;
}

// XContentContainerFile::ReadSync
size_t Package::read(const PackageNode &file, uint64_t offset, void *out, size_t length) const {
    if (file.directory || offset >= file.size) return 0;
    uint64_t remaining = std::min<uint64_t>(length, file.size - offset);
    // the run holding the offset: the last that starts at or before it
    auto it = std::upper_bound(file.runs.begin(), file.runs.end(), offset,
                               [](uint64_t o, const PackageRun &r) { return o < r.start; });
    if (it == file.runs.begin()) return 0;
    uint8_t *p = static_cast<uint8_t *>(out);
    size_t total = 0;
    for (--it; it != file.runs.end() && remaining; ++it) {
        uint64_t within = offset - it->start;
        if (within >= it->length) break; // a gap: the runs end short
        uint64_t n = std::min(it->length - within, remaining);
        size_t got = read_some(it->file, it->offset + within, p, size_t(n));
        total += got;
        if (got < n) break;
        p += got;
        offset += got;
        remaining -= got;
    }
    return total;
}

std::vector<uint8_t> Package::read_all(const PackageNode &file) const {
    std::vector<uint8_t> out(size_t(file.size));
    out.resize(read(file, 0, out.data(), out.size()));
    return out;
}

} // namespace xf
