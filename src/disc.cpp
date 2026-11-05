// XDVDFS read from an ISO. Translated from Xenia's DiscImageDevice
// (src/xenia/vfs/devices/disc_image_device.cc, BSD; see LICENSE-xenia),
// with the directory tree walked without recursion and every entry checked
// against its table.

#include "xenoforge/disc.h"

#include "xenoforge/bytes.h"

#include <cstring>

namespace xf {
namespace {

constexpr uint64_t SECTOR = 2048;

// Where the game partition starts: rebuilt images at 0, then full XGD3, XGD2
// and XGD1 dumps after their video partition. Xenia also tries 0xFB20 and
// 0x20600, kept here.
constexpr uint64_t PARTITION_OFFSETS[] = {0, 0x2080000, 0xFD90000, 0x18300000, 0xFB20, 0x20600};

constexpr char MAGIC[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint8_t ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t MAX_TABLE = 32u << 20;

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

int seek(std::FILE *f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

} // namespace

Disc::~Disc() {
    if (file_) std::fclose(file_);
}

std::unique_ptr<Disc> Disc::open(const std::filesystem::path &path, std::string &error) {
    std::unique_ptr<Disc> disc(new Disc());
#ifdef _WIN32
    disc->file_ = _wfopen(path.c_str(), L"rb");
#else
    disc->file_ = std::fopen(path.c_str(), "rb");
#endif
    if (!disc->file_) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    std::error_code ec;
    disc->file_size_ = std::filesystem::file_size(path, ec);

    uint8_t volume[28] = {};
    bool found = false;
    for (uint64_t offset : PARTITION_OFFSETS) {
        if (offset + 32 * SECTOR + sizeof(volume) > disc->file_size_) continue;
        if (disc->read_at(offset + 32 * SECTOR, volume, sizeof(volume)) && std::memcmp(volume, MAGIC, 20) == 0) {
            disc->game_offset_ = offset;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "no Xbox 360 game partition (XDVDFS) in " + path.string();
        return nullptr;
    }

    uint32_t root_sector = le32(volume + 20);
    uint32_t root_size = le32(volume + 24);
    if (root_size < 13 || root_size > MAX_TABLE) {
        error = "the root directory's size is not plausible";
        return nullptr;
    }
    disc->root_.directory = true;
    if (!disc->read_directory(root_sector, root_size, disc->root_, 0, error)) return nullptr;
    return disc;
}

bool Disc::read_at(uint64_t offset, void *out, size_t size) const {
    if (offset > file_size_ || size > file_size_ - offset) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (seek(file_, offset) != 0) return false;
    return std::fread(out, 1, size, file_) == size;
}

// A directory's table is a binary tree of entries, each at a multiple of 4
// bytes; in order it lists the names sorted.
bool Disc::read_directory(uint32_t sector, uint32_t size, DiscEntry &into, int depth, std::string &error) {
    if (depth > 64) {
        error = "directories nested too deep";
        return false;
    }
    if (size > MAX_TABLE) {
        error = "a directory table is too big";
        return false;
    }
    std::vector<uint8_t> table(size);
    if (!read_at(game_offset_ + uint64_t(sector) * SECTOR, table.data(), size)) {
        error = "a directory table lies outside the image";
        return false;
    }

    struct Node {
        uint32_t slot;
        bool visit;
    };
    std::vector<Node> stack{{0, false}};
    std::vector<bool> seen(size / 4 + 1);
    while (!stack.empty()) {
        Node node = stack.back();
        stack.pop_back();
        size_t pos = size_t(node.slot) * 4;
        if (pos + 14 > size) {
            error = "a directory entry lies outside its table";
            return false;
        }
        const uint8_t *p = table.data() + pos;
        uint16_t left = le16(p), right = le16(p + 2);
        if (!node.visit) {
            // a table padded with 0xFF holds no entries (an empty directory)
            if (left == 0xFFFF && right == 0xFFFF) continue;
            if (seen[node.slot]) {
                error = "a directory's tree loops";
                return false;
            }
            seen[node.slot] = true;
            if (right) stack.push_back({right, false});
            stack.push_back({node.slot, true});
            if (left) stack.push_back({left, false});
            continue;
        }

        uint32_t entry_sector = le32(p + 4);
        uint32_t length = le32(p + 8);
        uint8_t attributes = p[12];
        uint8_t name_length = p[13];
        if (pos + 14 + name_length > size) {
            error = "a file name runs past its directory table";
            return false;
        }
        DiscEntry entry;
        entry.name.assign(reinterpret_cast<const char *>(p + 14), name_length);
        entry.attributes = attributes;
        if (attributes & ATTRIBUTE_DIRECTORY) {
            entry.directory = true;
            if (length && !read_directory(entry_sector, length, entry, depth + 1, error)) return false;
        } else {
            entry.offset = game_offset_ + uint64_t(entry_sector) * SECTOR;
            entry.size = length;
        }
        into.children.push_back(std::move(entry));
    }
    return true;
}

const DiscEntry *Disc::find(std::string_view path) const {
    const DiscEntry *at = &root_;
    while (!path.empty()) {
        size_t cut = path.find_first_of("/\\");
        std::string_view part = path.substr(0, cut);
        path = cut == std::string_view::npos ? std::string_view{} : path.substr(cut + 1);
        if (part.empty()) continue;
        const DiscEntry *next = nullptr;
        for (const DiscEntry &child : at->children) {
            if (same_name(child.name, part)) {
                next = &child;
                break;
            }
        }
        if (!next) return nullptr;
        at = next;
    }
    return at;
}

bool Disc::read(const DiscEntry &file, uint64_t offset, void *out, size_t size) const {
    if (file.directory || offset > file.size || size > file.size - offset) return false;
    return read_at(file.offset + offset, out, size);
}

std::vector<uint8_t> Disc::read_all(const DiscEntry &file) const {
    std::vector<uint8_t> data(file.size);
    if (!read(file, 0, data.data(), data.size())) data.clear();
    return data;
}

} // namespace xf
