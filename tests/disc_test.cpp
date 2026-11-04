// XDVDFS on a small image made here: the root's tree walked in order, a
// subdirectory, names in any case, files read, and a tree that loops refused.

#include "check.h"
#include "xenoforge/disc.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xf;

namespace {

constexpr size_t SECTOR = 2048;

void put_le16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}

// one directory entry at a 4-byte slot of a table
void entry(uint8_t *table, uint16_t slot, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
           uint8_t attributes, const char *name) {
    uint8_t *p = table + slot * 4;
    put_le16(p, left);
    put_le16(p + 2, right);
    put_le32(p + 4, sector);
    put_le32(p + 8, size);
    p[12] = attributes;
    p[13] = uint8_t(std::strlen(name));
    std::memcpy(p + 14, name, std::strlen(name));
}

std::vector<uint8_t> make_image(bool looping) {
    std::vector<uint8_t> image(40 * SECTOR, 0);
    uint8_t *volume = image.data() + 32 * SECTOR;
    std::memcpy(volume, "MICROSOFT*XBOX*MEDIA", 20);
    put_le32(volume + 20, 33);   // the root's table
    put_le32(volume + 24, 2048); // and its size

    uint8_t *root = image.data() + 33 * SECTOR;
    std::memset(root, 0xFF, SECTOR);
    // "media" at the top, "default.xex" to its left, "Zeta.bin" to its right;
    // slot 0 means "none", so a loop goes through the others
    entry(root, 0, 5, 12, 34, 2048, 0x10, "media");
    entry(root, 5, looping ? 12 : 0, 0, 36, 8, 0x00, "default.xex");
    entry(root, 12, 0, 0, 37, 0, 0x00, "Zeta.bin");

    uint8_t *media = image.data() + 34 * SECTOR;
    std::memset(media, 0xFF, SECTOR);
    entry(media, 0, 0, 0, 35, 5, 0x00, "a.txt");
    std::memcpy(image.data() + 35 * SECTOR, "hello", 5);
    std::memcpy(image.data() + 36 * SECTOR, "XEX2abcd", 8);
    return image;
}

std::filesystem::path write_temp(const std::vector<uint8_t> &data, const char *name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    return path;
}

void reads_a_disc() {
    auto path = write_temp(make_image(false), "xenoforge_disc_test.iso");
    std::string error;
    auto disc = Disc::open(path, error);
    CHECK(disc != nullptr);
    if (!disc) {
        std::printf("  %s\n", error.c_str());
        return;
    }
    CHECK(disc->game_offset() == 0);
    const auto &root = disc->root().children;
    CHECK(root.size() == 3);
    if (root.size() == 3) {
        CHECK(root[0].name == "default.xex" && !root[0].directory && root[0].size == 8);
        CHECK(root[1].name == "media" && root[1].directory && root[1].children.size() == 1);
        CHECK(root[2].name == "Zeta.bin" && root[2].size == 0);
    }
    const DiscEntry *a = disc->find("MEDIA\\A.TXT");
    CHECK(a && a->size == 5);
    if (a) {
        auto data = disc->read_all(*a);
        CHECK(std::string(data.begin(), data.end()) == "hello");
        char two[2];
        CHECK(disc->read(*a, 3, two, 2) && two[0] == 'l' && two[1] == 'o');
        CHECK(!disc->read(*a, 4, two, 2)); // past the end
    }
    CHECK(disc->find("media/missing") == nullptr);
    CHECK(disc->find("/default.xex") == &root[0]);
    disc.reset();
    std::filesystem::remove(path);
}

void refuses_a_loop() {
    auto path = write_temp(make_image(true), "xenoforge_disc_loop.iso");
    std::string error;
    CHECK(Disc::open(path, error) == nullptr);
    CHECK(error.find("loops") != std::string::npos);
    std::filesystem::remove(path);
}

void refuses_other_files() {
    auto path = write_temp(std::vector<uint8_t>(100 * SECTOR, 0), "xenoforge_disc_blank.iso");
    std::string error;
    CHECK(Disc::open(path, error) == nullptr);
    std::filesystem::remove(path);
}

} // namespace

int main() {
    reads_a_disc();
    refuses_a_loop();
    refuses_other_files();
    return finish("disc");
}
