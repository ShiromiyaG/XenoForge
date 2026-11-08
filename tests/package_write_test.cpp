// Writing STFS packages (package.h, write_stfs_package) and the way back (extract_package): directories of files
// packed, read by Package, checked against the layout the format says (raw positions of the tables and blocks, the
// SHA-1 chain up to the descriptor's top hash), unpacked and compared. Sizes cross the hash levels: under 170
// blocks, past 170 (a level-1 table), and past 28,900 (the level-2 table).

#include "check.h"

#include "xenoforge/bytes.h"
#include "xenoforge/package.h"
#include "xenoforge/sha1.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace xf;
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

namespace {

Bytes pattern(size_t n, uint32_t seed) {
    Bytes out(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto &b : out) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        b = uint8_t(x >> 11);
    }
    return out;
}

fs::path temp_dir() {
    static int counter = 0;
    std::random_device rd;
    fs::path p = fs::temp_directory_path() / ("xf_stfs_write_" + std::to_string(rd()) + "_" + std::to_string(counter++));
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path &path, const Bytes &b) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
}

Bytes read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void put32(Bytes &b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (24 - 8 * i));
}

// what a title's package starts with: the type, version, title and a name
Bytes make_header() {
    Bytes h(0x344 + 0x93D6, 0);
    put32(h, 0, PACKAGE_MAGIC_CON);
    put32(h, 0x340, 0x344 + 0x93D6);
    const size_t m = 0x344;
    put32(h, m, 1); // saved game
    put32(h, m + 4, 2);
    put32(h, m + 0x1C, 0x5846000A);
    const char16_t name[] = u"Slot one";
    for (size_t i = 0; i < 8; i++) {
        h[m + 0xCD + 2 * i] = uint8_t(name[i] >> 8);
        h[m + 0xCD + 2 * i + 1] = uint8_t(name[i]);
    }
    return h;
}

struct Tree {
    std::vector<std::pair<std::string, Bytes>> files;
    std::vector<std::string> directories; // empty ones included
};

void make_tree(const fs::path &root, const Tree &t) {
    for (const auto &d : t.directories) fs::create_directories(root / d);
    for (const auto &[name, data] : t.files) write_file(root / name, data);
}

void check_package_matches(const Package &p, const Tree &t) {
    for (const auto &[name, data] : t.files) {
        const PackageNode *n = p.find(name);
        CHECK(n && !n->directory);
        if (!n) continue;
        CHECK(n->size == data.size());
        CHECK(p.read_all(*n) == data);
    }
    for (const auto &d : t.directories) {
        const PackageNode *n = p.find(d);
        CHECK(n && n->directory);
    }
    CHECK(p.warnings().empty());
}

// the layout from the format's own order, by raw positions: block index -> physical block for fewer than
// 28,900 blocks (a pair of tables before each 170, a level-1 pair after the first group)
uint64_t raw_block(uint64_t i) { return i + 2 + 2 * (i / 170) + (i >= 170 ? 2 : 0); }

void check_raw(const fs::path &file, uint32_t blocks) {
    Bytes f = read_file(file);
    const uint64_t base = 0xA000;
    auto block_at = [&](uint64_t physical) { return f.data() + base + physical * 0x1000; };
    CHECK(f.size() >= base + raw_block(blocks - 1) * 0x1000 + 0x1000);
    if (f.size() < base + raw_block(blocks - 1) * 0x1000 + 0x1000) return;
    // every level-0 entry holds the hash of its block and the right next-block link
    for (uint32_t i = 0; i < blocks; i++) {
        // group 0's pair is first; group g >= 1's follows the level-1 pair, after g full groups
        uint64_t table = i < 170 ? 0 : uint64_t(i / 170) * 172 + 2;
        const uint8_t *e = block_at(table) + (i % 170) * 0x18;
        uint8_t digest[20];
        sha1({block_at(raw_block(i)), 0x1000}, digest);
        CHECK(std::memcmp(e, digest, 20) == 0);
        CHECK(e[0x14] == 0x80);
        uint32_t next = uint32_t(e[0x15]) << 16 | uint32_t(e[0x16]) << 8 | e[0x17];
        // files here are consecutive, so a block's link is the next block or the end of its file
        CHECK(next == i + 1 || next == 0xFFFFFF);
    }
    // the top table's hash is in the descriptor (offset 0x379 + 8)
    const uint8_t *top = blocks < 170 ? block_at(0) : block_at(172);
    uint8_t digest[20];
    sha1({top, 0x1000}, digest);
    CHECK(std::memcmp(f.data() + 0x379 + 8, digest, 20) == 0);
    if (blocks >= 170) {
        // level 1: the entry of group g holds the hash of that group's table
        for (uint32_t g = 0; g < (blocks + 169) / 170; g++) {
            uint64_t table = g == 0 ? 0 : uint64_t(g) * 172 + 2;
            sha1({block_at(table), 0x1000}, digest);
            CHECK(std::memcmp(top + g * 0x18, digest, 20) == 0);
        }
    }
    // the header: the content ID is the SHA-1 of the metadata
    sha1({f.data() + 0x344, 0x93D6}, digest);
    CHECK(std::memcmp(f.data() + 0x32C, digest, 20) == 0);
    CHECK(std::memcmp(f.data(), "CON ", 4) == 0);
}

void round_trip(const Tree &t, uint32_t expect_blocks_at_least, bool raw) {
    fs::path dir = temp_dir(), src = dir / "src", out = dir / "pkg", back = dir / "back";
    fs::create_directories(src);
    make_tree(src, t);
    std::string error;
    CHECK(write_stfs_package(out, make_header(), src, error));
    CHECK(error.empty());
    CHECK(!fs::exists(fs::path(out.string() + ".tmp")));
    std::unique_ptr<Package> p = Package::open(out, error);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->is_stfs());
        CHECK(p->info().magic == PACKAGE_MAGIC_CON);
        CHECK(p->info().content_type == 1);
        CHECK(p->info().title_id == 0x5846000A);
        CHECK(p->info().display_name[0] == u"Slot one");
        CHECK(p->info().stfs.total_block_count >= expect_blocks_at_least);
        CHECK(!p->read_only());
        check_package_matches(*p, t);
        if (raw) check_raw(out, p->info().stfs.total_block_count);
        // and back to a directory
        CHECK(extract_package(*p, back, error));
        for (const auto &[name, data] : t.files) CHECK(read_file(back / name) == data);
        for (const auto &d : t.directories) CHECK(fs::is_directory(back / d));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        // a few files in directories: empty, one byte, one block, a block and a byte, an empty directory
        Tree t;
        t.directories = {"empty", "a/b/c"};
        t.files = {{"zero.bin", {}},        {"one.bin", pattern(1, 1)},       {"block.bin", pattern(4096, 2)},
                   {"over.bin", pattern(4097, 3)}, {"a/inner.dat", pattern(10000, 4)},
                   {"a/b/c/deep.dat", pattern(70000, 5)}, {"Save.bin", pattern(300000, 6)}};
        round_trip(t, 10, true);
    }
    {
        // an empty directory makes an empty package: the file table only
        Tree t;
        round_trip(t, 1, true);
    }
    {
        // past 170 blocks: a level-1 table over several level-0 tables
        Tree t;
        t.files = {{"big1.bin", pattern(1000 * 4096 + 123, 7)}, {"big2.bin", pattern(97 * 4096, 8)}, {"s.bin", pattern(5, 9)}};
        round_trip(t, 1000, true);
    }
    {
        // past 28,900 blocks: the level-2 table (read back through the reader's own walk)
        Tree t;
        t.files = {{"huge.bin", pattern(29100 * 4096ull + 77, 10)}, {"tail.bin", pattern(4096 * 3, 11)}};
        round_trip(t, 29100, false);
    }
    {
        // one more write over a package: the file is replaced whole, never half there
        fs::path dir = temp_dir(), src = dir / "src", out = dir / "pkg";
        fs::create_directories(src);
        write_file(src / "a.bin", pattern(9000, 12));
        std::string error;
        CHECK(write_stfs_package(out, make_header(), src, error));
        write_file(src / "a.bin", pattern(20000, 13));
        write_file(src / "b.bin", pattern(10, 14));
        CHECK(write_stfs_package(out, make_header(), src, error));
        auto p = Package::open(out, error);
        CHECK(p != nullptr);
        if (p) {
            CHECK(p->read_all(*p->find("a.bin")) == pattern(20000, 13));
            CHECK(p->read_all(*p->find("b.bin")) == pattern(10, 14));
        }
        p.reset();
        // a name past what an entry holds is refused, and nothing is left behind
        write_file(src / std::string(41, 'x'), pattern(3, 15));
        fs::path out2 = dir / "pkg2";
        CHECK(!write_stfs_package(out2, make_header(), src, error));
        CHECK(!error.empty());
        CHECK(!fs::exists(out2));
        CHECK(!fs::exists(fs::path(out2.string() + ".tmp")));
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    {
        // names in Latin-1 come back as the UTF-8 they went in as
        fs::path dir = temp_dir(), src = dir / "src", out = dir / "pkg";
        fs::create_directories(src);
        std::string name = "caf\xC3\xA9.sav";
        write_file(src / fs::path(std::u8string(name.begin(), name.end())), pattern(50, 16));
        std::string error;
        CHECK(write_stfs_package(out, make_header(), src, error));
        auto p = Package::open(out, error);
        CHECK(p && p->find(name) != nullptr);
        p.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    return finish("package_write");
}
