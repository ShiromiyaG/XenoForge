// The console's content packages (STFS and SVOD, "XContent"): a CON, LIVE or
// PIRS header, its metadata, and a read-only file system after it. DLC, title
// updates, saves, XBLA and Games on Demand titles, and the system update on
// every retail disc are packages. Translated from Xenia (BSD; see
// LICENSE-xenia): src/xenia/vfs/devices/xcontent_container_device.*,
// xcontent_devices/stfs_container_*, svod_container_*, stfs_xbox.h and
// src/xenia/kernel/xam/xcontent/xcontent.h.
//
// A Package reads the header and builds the whole tree of files up front, as
// Xenia's devices do; reads then come from the host files (the package's own,
// or the SVOD's data fragments beside it). The kernel mounts one as a device
// (kernel/files.h, make_package_device); xfinspect --package lists and
// extracts one.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xf {

constexpr uint32_t PACKAGE_MAGIC_CON = 0x434F4E20; // "CON " (signed by a console)
constexpr uint32_t PACKAGE_MAGIC_LIVE = 0x4C495645; // "LIVE" (signed by Microsoft, from Live)
constexpr uint32_t PACKAGE_MAGIC_PIRS = 0x50495253; // "PIRS" (signed by Microsoft, other media)

enum class PackageVolume : uint32_t { Stfs = 0, Svod = 1 };

// XContentHeader (0x344 bytes at the start) and XContentMetadata (0x93D6
// bytes after it), the fields a title or a tool reads.
struct PackageInfo {
    uint32_t magic = 0;
    std::array<uint8_t, 0x14> content_id{};
    uint32_t header_size = 0; // the package's data starts at this, rounded up to 0x1000
    struct License {
        uint64_t licensee_id = 0;
        uint32_t bits = 0, flags = 0;
    };
    std::array<License, 16> licenses{};

    uint32_t content_type = 0; // XContentType: 1 saved game, 2 marketplace, 0x10000 profile...
    uint32_t metadata_version = 0;
    uint64_t content_size = 0;
    // the execution info (the executable's, as in its XEX)
    uint32_t media_id = 0, version = 0, base_version = 0, title_id = 0, save_game_id = 0;
    uint8_t platform = 0, executable_type = 0, disc_number = 0, disc_count = 0;
    std::array<uint8_t, 5> console_id{};
    uint64_t profile_id = 0;
    PackageVolume volume_type = PackageVolume::Stfs;
    uint32_t data_file_count = 0; // SVOD's fragments
    uint64_t data_file_size = 0;
    uint64_t online_creator = 0;
    uint32_t category = 0;
    std::array<uint8_t, 0x14> device_id{};
    std::u16string display_name[12], description[12]; // by language, English first
    std::u16string publisher, title_name;
    uint8_t flags = 0;
    uint32_t thumbnail_size = 0, title_thumbnail_size = 0;

    // the STFS volume descriptor (0x24 bytes)
    struct Stfs {
        uint8_t descriptor_length = 0, version = 0, flags = 0;
        uint32_t file_table_block_count = 0, file_table_block_number = 0;
        std::array<uint8_t, 0x14> top_hash_table_hash{};
        uint32_t total_block_count = 0, free_block_count = 0;
        bool read_only_format() const { return flags & 1; } // one backing block per hash table
        bool root_active_index() const { return flags & 2; }
    } stfs;
    // the SVOD device descriptor (0x24 bytes)
    struct Svod {
        uint8_t descriptor_length = 0, block_cache_element_count = 0, worker_thread_processor = 0,
                worker_thread_priority = 0;
        std::array<uint8_t, 0x14> first_fragment_hash_entry{};
        uint8_t features = 0;
        uint32_t num_data_blocks = 0, start_data_block = 0;
        bool enhanced_gdf_layout() const { return features & 0x40; }
    } svod;
};

// A piece of a file: bytes at an offset of one of the package's host files.
struct PackageRun {
    uint32_t file = 0; // index in the package's host files (STFS: 0)
    uint64_t offset = 0, length = 0;
    uint64_t start = 0; // where it begins in the file's own bytes
};

// A file or directory of the package.
struct PackageNode {
    std::string name; // UTF-8
    std::string path; // from the root, '/' between names ("" the root)
    bool directory = false;
    uint64_t size = 0;
    uint64_t create_time = 0, write_time = 0; // FILETIMEs (0: unknown)
    std::vector<PackageRun> runs;
    std::vector<std::unique_ptr<PackageNode>> children;
    uint32_t block = 0; // the first block (SVOD's sector)
};

class Package {
public:
    // Reads a package file; an SVOD's header file has its fragments in
    // "<path>.data". On failure: null and the reason.
    static std::unique_ptr<Package> open(const std::filesystem::path &path, std::string &error);

    const PackageInfo &info() const { return info_; }
    const PackageNode &root() const { return root_; }
    bool is_stfs() const { return info_.volume_type == PackageVolume::Stfs; }
    // what the volume reports (NtQueryVolumeInformationFile's numbers)
    uint64_t total_units() const { return total_units_; }
    uint64_t available_units() const { return available_units_; }
    bool read_only() const { return read_only_; }
    // what was found odd and went on (a chain shorter than its file, a file
    // table of another length...)
    const std::vector<std::string> &warnings() const { return warnings_; }
    // how the SVOD is laid out ("EGDF", "XSF", "single file", "multiple files", "no XSF header")
    const std::string &layout() const { return layout_; }

    // a path such as "dir/file.bin", either slash, any case
    const PackageNode *find(std::string_view path) const;
    // Reads bytes of a file (XContentContainerFile::ReadSync): up to `length`
    // from `offset`; returns the count (fewer at the end of the file or where
    // the package is short). Reads may come from several threads at once.
    size_t read(const PackageNode &file, uint64_t offset, void *out, size_t length) const;
    std::vector<uint8_t> read_all(const PackageNode &file) const;
    // the thumbnail and the title's thumbnail (PNG), from the header
    const std::vector<uint8_t> &thumbnail() const { return thumbnail_; }
    const std::vector<uint8_t> &title_thumbnail() const { return title_thumbnail_; }

    ~Package();

private:
    Package() = default;

    struct HostFile {
        std::FILE *file = nullptr;
        uint64_t size = 0;
        mutable std::mutex mutex; // the file's position
    };
    bool read_at(uint32_t file, uint64_t offset, void *out, size_t length) const;
    size_t read_some(uint32_t file, uint64_t offset, void *out, size_t length) const;

    bool read_header(std::string &error);
    bool read_stfs(std::string &error);
    bool read_svod(const std::filesystem::path &path, std::string &error);
    // STFS
    uint64_t stfs_block_offset(uint64_t block) const;
    uint32_t stfs_hash_block_number(uint32_t block, uint32_t level) const;
    bool stfs_block_hash(uint32_t block, uint32_t &info);
    bool stfs_hash_table(uint32_t block, uint32_t level, uint32_t &secondary);
    // SVOD
    void svod_block_offset(uint64_t sector, uint64_t &address, uint32_t &file) const;
    bool svod_entry(uint32_t sector, PackageNode &parent, int &budget, int depth, std::string &error);

    PackageInfo info_;
    PackageNode root_;
    std::vector<std::unique_ptr<HostFile>> files_;
    std::vector<uint8_t> thumbnail_, title_thumbnail_;
    std::vector<std::string> warnings_;
    std::string layout_;
    uint64_t data_base_ = 0;   // STFS: where the volume starts in the file
    uint64_t total_units_ = 0, available_units_ = 0;
    bool read_only_ = true;
    uint64_t svod_base_offset_ = 0;
    int svod_layout_ = 0;
    uint32_t blocks_per_hash_table_ = 1;
    uint32_t block_step_[2] = {0, 0};
    std::map<uint64_t, std::vector<uint8_t>> hash_tables_; // by the table's offset in the volume
};

// A Windows-1252 name (a package's file names) as UTF-8
std::string package_name_to_utf8(std::string_view ansi);
// A FAT date and time (the STFS entries') as a FILETIME; 0 when the date is not one
uint64_t fat_timestamp(uint32_t date, uint32_t time);

} // namespace xf
