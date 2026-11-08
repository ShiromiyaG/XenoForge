// The content packages (package.h) and their devices (kernel/files.h,
// make_package_device), on packages built here: an STFS (read-only and with
// two tables per level, file blocks scattered so the chains are walked, past
// the first hash group) and an SVOD (the four layouts, a directory over
// several sectors, a file across the groups' hash tables and one across two
// data fragments). The layouts are written here from the format's own order
// (tables, then data), not by the formulas the reader uses.

#include "check.h"
#include "kernel_harness.h"

#include "files.h"
#include "xenoforge/package.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace xf;
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

namespace {

void put16(Bytes &b, size_t at, uint32_t v) { b[at] = uint8_t(v >> 8), b[at + 1] = uint8_t(v); }
void put32(Bytes &b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (24 - 8 * i));
}
void put64(Bytes &b, size_t at, uint64_t v) { put32(b, at, uint32_t(v >> 32)), put32(b, at + 4, uint32_t(v)); }
void putle16(Bytes &b, size_t at, uint32_t v) { b[at] = uint8_t(v), b[at + 1] = uint8_t(v >> 8); }
void putle24(Bytes &b, size_t at, uint32_t v) { b[at] = uint8_t(v), b[at + 1] = uint8_t(v >> 8), b[at + 2] = uint8_t(v >> 16); }
void putle32(Bytes &b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (8 * i));
}
void putle64(Bytes &b, size_t at, uint64_t v) { putle32(b, at, uint32_t(v)), putle32(b, at + 4, uint32_t(v >> 32)); }

uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

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
    static std::atomic<int> counter{0};
    std::random_device rd;
    fs::path p = fs::temp_directory_path() / ("xf_package_test_" + std::to_string(rd()) + "_" +
                                              std::to_string(counter++));
    fs::create_directories(p);
    return p;
}

void remove_tree(const fs::path &path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) std::printf("cannot remove %s: %s\n", path.string().c_str(), ec.message().c_str());
}

void write_file(const fs::path &path, const Bytes &b) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
}

// the header of a package: XContentHeader and XContentMetadata, at the
// offsets the format has
Bytes make_header(uint32_t magic, uint32_t volume_type, uint32_t header_size, const Bytes &descriptor,
                  uint32_t data_file_count, uint32_t file_size) {
    Bytes h(std::max<size_t>(file_size, 0x9710), 0);
    put32(h, 0, magic);
    put64(h, 4 + 0x228, 0x1122334455667788ull); // the first license's id
    put32(h, 4 + 0x228 + 8, 0xAABBCCDD);
    put32(h, 0x340, header_size);
    const size_t m = 0x344;
    put32(h, m, 0xB0000);      // content type
    put32(h, m + 4, 2);        // metadata version
    put64(h, m + 8, 0x123456); // content size
    put32(h, m + 0x1C, 0x5846000A); // title id
    std::memcpy(&h[m + 0x35], descriptor.data(), 0x24);
    put32(h, m + 0x59, data_file_count);
    put32(h, m + 0x65, volume_type);
    const char16_t name[] = u"Test Package";
    for (size_t i = 0; name[i]; i++) put16(h, m + 0xCD + 2 * i, name[i]);
    const char16_t ex[] = u"Idioma 10"; // the 10th language, in the V2 extension
    for (size_t i = 0; ex[i]; i++) put16(h, m + 0x50D6 + 2 * i, ex[i]);
    const char16_t publisher[] = u"Pub";
    for (size_t i = 0; publisher[i]; i++) put16(h, m + 0x12CD + 2 * i, publisher[i]);
    put32(h, m + 0x13CE, 6); // thumbnail size
    std::memcpy(&h[m + 0x13D6], "THUMB!", 6);
    return h;
}

struct Item {
    std::string path; // '/'; names as the package holds them (Windows-1252)
    bool dir = false;
    Bytes data;
};

// ---- STFS ----

struct StfsBuilt {
    Bytes bytes;
    uint32_t blocks = 0;
    uint64_t table_offset = 0; // of the file table's first block, in the file
};

StfsBuilt build_stfs(const std::vector<Item> &items, bool read_only, bool root_active, uint32_t header_size) {
    const size_t T = read_only ? 1 : 2;
    const size_t n_table = (items.size() + 63) / 64;
    std::vector<size_t> counts(items.size());
    size_t N = n_table;
    for (size_t i = 0; i < items.size(); i++) N += counts[i] = items[i].dir ? 0 : (items[i].data.size() + 4095) / 4096;
    // the blocks are handed out in a scattered order: every chain jumps
    size_t stride = 37;
    for (auto gcd = [](size_t a, size_t b) {
             while (b) std::swap(a %= b, b);
             return a;
         };
         gcd(stride, N) != 1; stride += 2) {
    }
    size_t cursor = 0;
    auto alloc = [&] { return uint32_t((cursor++ * stride + 5) % N); };
    std::vector<uint32_t> next(N, 0xFFFFFF);
    std::vector<uint32_t> table_blocks;
    for (size_t i = 0; i < n_table; i++) table_blocks.push_back(alloc());
    for (size_t i = 0; i + 1 < n_table; i++) next[table_blocks[i]] = table_blocks[i + 1];
    std::vector<std::vector<uint32_t>> chain(items.size());
    for (size_t i = 0; i < items.size(); i++) {
        for (size_t b = 0; b < counts[i]; b++) chain[i].push_back(alloc());
        for (size_t b = 0; b + 1 < counts[i]; b++) next[chain[i][b]] = chain[i][b + 1];
    }

    // physical places: [L0 table(s)][170 data blocks][L1 table(s)] for the first
    // group, then [L0 table(s)][170 data blocks] for each next
    const size_t G = (N + 169) / 170;
    auto phys_l0 = [&](size_t g) { return g == 0 ? size_t(0) : T + 170 + T + (g - 1) * (170 + T); };
    auto phys_data = [&](size_t d) { return (d / 170 == 0 ? T : phys_l0(d / 170) + T) + d % 170; };
    const size_t phys_l1 = T + 170;
    size_t total = phys_data(N - 1) + 1;
    if (N >= 170) total = std::max(total, phys_l1 + T);

    StfsBuilt out;
    out.blocks = uint32_t(N);
    const size_t base = round_up(header_size, 0x1000);
    Bytes img(base + total * 4096, 0);
    auto block_at = [&](size_t phys) { return &img[base + phys * 4096]; };

    // the file table
    Bytes table(n_table * 4096, 0);
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < items.size(); i++) {
        const Item &it = items[i];
        size_t slash = it.path.rfind('/');
        std::string name = slash == std::string::npos ? it.path : it.path.substr(slash + 1);
        uint8_t *e = &table[i * 0x40];
        std::memcpy(e, name.data(), name.size());
        e[0x28] = uint8_t(name.size()) | (it.dir ? 0x80 : 0) | 0x40;
        putle24(table, i * 0x40 + 0x29, uint32_t(counts[i]));
        putle24(table, i * 0x40 + 0x2C, uint32_t(counts[i]));
        putle24(table, i * 0x40 + 0x2F, counts[i] ? chain[i][0] : 0);
        put16(table, i * 0x40 + 0x32, slash == std::string::npos ? 0xFFFF : uint32_t(index.at(it.path.substr(0, slash))));
        put32(table, i * 0x40 + 0x34, it.dir ? 0 : uint32_t(it.data.size()));
        put16(table, i * 0x40 + 0x38, 15055), put16(table, i * 0x40 + 0x3A, 25558);  // 2009-06-15 12:30:44
        put16(table, i * 0x40 + 0x3C, 10143), put16(table, i * 0x40 + 0x3E, 49021);  // 1999-12-31 23:59:58
        index[it.path] = i;
    }
    for (size_t t = 0; t < n_table; t++) {
        std::memcpy(block_at(phys_data(table_blocks[t])), &table[t * 4096], 4096);
    }
    out.table_offset = base + phys_data(table_blocks[0]) * 4096;
    for (size_t i = 0; i < items.size(); i++)
        for (size_t b = 0; b < counts[i]; b++) {
            size_t n = std::min<size_t>(4096, items[i].data.size() - b * 4096);
            std::memcpy(block_at(phys_data(chain[i][b])), &items[i].data[b * 4096], n);
        }

    // the hash tables; a pair has the active copy marked in the table above
    // (the descriptor's flag for the top one) and garbage in the other
    auto active_copy = [&](size_t g) { return read_only ? size_t(0) : (N >= 170 ? g % 2 : (root_active ? 1 : 0)); };
    for (size_t g = 0; g < G; g++) {
        for (size_t c = 0; c < T; c++) {
            uint8_t *tbl = block_at(phys_l0(g) + c);
            for (size_t i = 0; i < 170; i++) {
                size_t d = g * 170 + i;
                uint32_t info = 0;
                if (c == active_copy(g)) {
                    if (d < N) info = next[d] | (2u << 30);
                } else {
                    info = 0x00ABCDEF | (2u << 30); // the other copy: wrong
                }
                tbl[i * 0x18 + 0x14] = uint8_t(info >> 24), tbl[i * 0x18 + 0x15] = uint8_t(info >> 16);
                tbl[i * 0x18 + 0x16] = uint8_t(info >> 8), tbl[i * 0x18 + 0x17] = uint8_t(info);
            }
        }
    }
    if (N >= 170) {
        for (size_t c = 0; c < T; c++) {
            uint8_t *tbl = block_at(phys_l1 + c);
            for (size_t g = 0; g < G; g++) {
                uint32_t info = (c == (root_active && !read_only ? 1u : 0u)) ? (active_copy(g) ? 0x40000000u : 0u) : 0x40000000u;
                tbl[g * 0x18 + 0x14] = uint8_t(info >> 24), tbl[g * 0x18 + 0x15] = uint8_t(info >> 16);
                tbl[g * 0x18 + 0x16] = uint8_t(info >> 8), tbl[g * 0x18 + 0x17] = uint8_t(info);
            }
        }
    }

    Bytes d(0x24, 0);
    d[0] = 0x24;
    d[2] = uint8_t((read_only ? 1 : 0) | (root_active ? 2 : 0));
    putle16(d, 3, uint32_t(n_table));
    putle24(d, 5, table_blocks[0]);
    put32(d, 0x1C, uint32_t(N));
    put32(d, 0x20, read_only ? 0 : 10);
    Bytes header = make_header(PACKAGE_MAGIC_CON, 0, header_size, d, 0, 0);
    std::memcpy(img.data(), header.data(), std::min(header.size(), base));
    out.bytes = std::move(img);
    return out;
}

std::vector<Item> stfs_items() {
    std::vector<Item> items;
    items.push_back({"a.txt", false, pattern(100, 1)});
    items.push_back({"docs", true, {}});
    items.push_back({"docs/b.bin", false, pattern(10000, 2)});
    items.push_back({"docs/sub", true, {}});
    items.push_back({"docs/sub/c.bin", false, pattern(4096 * 3 + 5, 3)});
    items.push_back({"big.bin", false, pattern(500 * 1024, 4)}); // 125 blocks: past the first hash group
    items.push_back({"empty", false, {}});
    items.push_back({"caf\xE9 \x80.dat", false, pattern(4096, 5)}); // Windows-1252: e acute, euro
    for (int i = 0; i < 70; i++) // a second file table block
        items.push_back({"many" + std::to_string(i), false, pattern(size_t(i) * 37, 100 + i)});
    return items;
}

// the tree as the items say it
void check_items(const Package &p, const std::vector<Item> &items) {
    for (const Item &it : items) {
        std::string path = it.path;
        // the Windows-1252 name as UTF-8
        size_t at;
        while ((at = path.find('\xE9')) != std::string::npos) path.replace(at, 1, "\xC3\xA9");
        while ((at = path.find('\x80')) != std::string::npos) path.replace(at, 1, "\xE2\x82\xAC");
        const PackageNode *n = p.find(path);
        CHECK(n != nullptr);
        if (!n) continue;
        CHECK(n->directory == it.dir);
        if (it.dir) continue;
        CHECK(n->size == it.data.size());
        Bytes got = p.read_all(*n);
        CHECK(got == it.data);
        // reads from the middle, across blocks, past the end
        if (it.data.size() > 5000) {
            Bytes part(6000, 0);
            size_t got_n = p.read(*n, 4090, part.data(), part.size());
            CHECK(got_n == std::min<size_t>(6000, it.data.size() - 4090));
            CHECK(std::equal(part.begin(), part.begin() + long(got_n), it.data.begin() + 4090));
        }
        uint8_t one = 0;
        CHECK(p.read(*n, it.data.size(), &one, 1) == 0);
    }
    // nothing more than the items
    size_t count = 0;
    std::vector<const PackageNode *> stack{&p.root()};
    while (!stack.empty()) {
        const PackageNode *n = stack.back();
        stack.pop_back();
        for (const auto &c : n->children) count++, stack.push_back(c.get());
    }
    CHECK(count == items.size());
}

void stfs_test(bool read_only, bool root_active, uint32_t header_size, const std::vector<Item> &items) {
    fs::path dir = temp_dir();
    StfsBuilt built = build_stfs(items, read_only, root_active, header_size);
    write_file(dir / "pkg", built.bytes);
    std::string error;
    auto p = Package::open(dir / "pkg", error);
    CHECK(p != nullptr);
    if (p) {
        const PackageInfo &i = p->info();
        CHECK(i.magic == PACKAGE_MAGIC_CON);
        CHECK(p->is_stfs());
        CHECK(i.title_id == 0x5846000A && i.content_type == 0xB0000 && i.metadata_version == 2);
        CHECK(i.content_size == 0x123456);
        CHECK(i.licenses[0].licensee_id == 0x1122334455667788ull && i.licenses[0].bits == 0xAABBCCDD);
        CHECK(i.display_name[0] == u"Test Package" && i.display_name[9] == u"Idioma 10" && i.publisher == u"Pub");
        CHECK(i.stfs.total_block_count == built.blocks && i.stfs.read_only_format() == read_only);
        CHECK(i.stfs.root_active_index() == root_active);
        CHECK(p->thumbnail().size() == 6 && std::memcmp(p->thumbnail().data(), "THUMB!", 6) == 0);
        CHECK(p->warnings().empty());
        CHECK(p->read_only() == read_only);
        CHECK(p->total_units() == built.blocks);
        CHECK(p->available_units() == (read_only ? 0u : 4913000u - (built.blocks - 10)));
        check_items(*p, items);
        // names in any case, either slash; times are the FAT ones
        CHECK(p->find("DOCS\\Sub/C.BIN") == p->find("docs/sub/c.bin") && p->find("docs/sub/c.bin"));
        CHECK(p->find("docs/nothing") == nullptr);
        const PackageNode *a = p->find("a.txt");
        CHECK(a && a->create_time == 128895426440000000ull && a->write_time == 125911583980000000ull);
    } else {
        std::printf("stfs: %s\n", error.c_str());
    }
    p.reset(); // closes the file
    remove_tree(dir);
}

// ---- SVOD ----

enum class Layout { Egdf, Xsf, Multi, Single };

struct GNode {
    std::string name;
    bool dir = false;
    Bytes data;
    std::vector<GNode> kids;
    uint64_t forced_sector = 0; // 0: where the allocation is
    uint64_t sector = 0;
    uint64_t length = 0;
};

struct SvodBuilt {
    fs::path header;
    fs::path data;                                           // the ".data" directory
    std::map<std::string, std::pair<uint32_t, uint64_t>> entries; // path -> fragment, address of its header
    std::map<std::string, uint32_t> ordinals;
    uint64_t root_sector = 0;
};

struct DirLayout {
    std::vector<int> order, left, right;
    std::vector<uint32_t> ordinal;
    uint32_t bytes = 0;
};

int bst(const std::vector<GNode> &kids, int lo, int hi, DirLayout &l) {
    if (lo > hi) return -1;
    int mid = (lo + hi) / 2;
    l.order.push_back(mid);
    l.left[mid] = bst(kids, lo, mid - 1, l);
    l.right[mid] = bst(kids, mid + 1, hi, l);
    return mid;
}

DirLayout layout_dir(const std::vector<GNode> &kids) {
    DirLayout l;
    l.left.assign(kids.size(), -1), l.right.assign(kids.size(), -1), l.ordinal.assign(kids.size(), 0);
    bst(kids, 0, int(kids.size()) - 1, l);
    uint32_t off = 0;
    for (int idx : l.order) {
        uint32_t size = (14 + uint32_t(kids[size_t(idx)].name.size()) + 3) & ~3u;
        if (off % 0x800 + size > 0x800) off = uint32_t(round_up(off, 0x800)); // an entry stays in its sector
        l.ordinal[size_t(idx)] = off / 4;
        off += size;
    }
    l.bytes = uint32_t(round_up(off, 0x800));
    return l;
}

SvodBuilt build_svod(GNode root, Layout layout, uint32_t start_data_block, uint32_t fragments, uint32_t header_file_size,
                     const fs::path &dir, const std::string &name) {
    SvodBuilt out;
    const uint32_t k2 = 2 * start_data_block;
    const bool egdf = layout == Layout::Egdf;
    // sectors: the volume's, from after the descriptor's room
    uint64_t cursor = k2 + 40;
    std::sort(root.kids.begin(), root.kids.end(), [](const GNode &a, const GNode &b) { return a.name < b.name; });
    // pass 1: where everything is
    struct Walk {
        uint64_t &cursor;
        void operator()(GNode &n) {
            std::sort(n.kids.begin(), n.kids.end(), [](const GNode &a, const GNode &b) { return a.name < b.name; });
            if (n.dir) {
                DirLayout l = layout_dir(n.kids);
                n.length = n.kids.empty() ? 0 : l.bytes;
                if (n.forced_sector) cursor = n.forced_sector;
                n.sector = cursor;
                cursor += l.bytes / 0x800;
                for (GNode &kid : n.kids) (*this)(kid);
            } else {
                n.length = n.data.size();
                if (n.forced_sector) cursor = n.forced_sector;
                n.sector = cursor;
                cursor += round_up(n.data.size(), 0x800) / 0x800;
            }
        }
    } walk{cursor};
    walk(root);
    out.root_sector = root.sector;

    // the data files: pieces of bytes at addresses, the hash tables left zero
    std::vector<std::vector<std::pair<uint64_t, Bytes>>> regions(fragments);
    auto where = [&](uint64_t sector, uint32_t &frag, uint64_t &addr) {
        uint64_t tb = sector - k2 + (egdf ? 2 : 0);
        frag = uint32_t(tb / 0x14388);
        uint64_t fb = tb % 0x14388;
        addr = 0x2000 + (fb / 0x198) * (0x198 * 0x800 + 0x1000) + (fb % 0x198) * 0x800;
        if (layout == Layout::Single) addr += 0xB000;
    };
    auto put_sector = [&](uint64_t sector, const uint8_t *data, size_t n) {
        uint32_t frag;
        uint64_t addr;
        where(sector, frag, addr);
        regions.at(frag).push_back({addr, Bytes(data, data + n)});
    };
    auto put_bytes = [&](uint64_t first_sector, const Bytes &data) {
        for (size_t at = 0; at < data.size(); at += 0x800)
            put_sector(first_sector + at / 0x800, &data[at], std::min<size_t>(0x800, data.size() - at));
    };
    struct Emit {
        decltype(put_bytes) &put;
        SvodBuilt &built;
        void operator()(const GNode &n, const std::string &path) {
            if (!n.dir) {
                put(n.sector, n.data);
                return;
            }
            if (n.kids.empty()) return;
            DirLayout l = layout_dir(n.kids);
            Bytes dir(l.bytes, 0xFF);
            for (size_t i = 0; i < n.kids.size(); i++) {
                const GNode &kid = n.kids[i];
                size_t at = l.ordinal[i] * 4;
                putle16(dir, at, l.left[i] < 0 ? 0 : l.ordinal[size_t(l.left[i])]);
                putle16(dir, at + 2, l.right[i] < 0 ? 0 : l.ordinal[size_t(l.right[i])]);
                putle32(dir, at + 4, uint32_t(kid.sector));
                putle32(dir, at + 8, uint32_t(kid.dir ? kid.length : kid.data.size()));
                dir[at + 12] = kid.dir ? 0x10 : 0x80;
                dir[at + 13] = uint8_t(kid.name.size());
                std::memcpy(&dir[at + 14], kid.name.data(), kid.name.size());
                std::string kid_path = path.empty() ? kid.name : path + "/" + kid.name;
                built.ordinals[kid_path] = l.ordinal[i];
                // where this entry's header lives
                uint64_t sector = n.sector + (l.ordinal[i] * 4) / 0x800;
                built.entries[kid_path] = {0, sector * 0x800 + (l.ordinal[i] * 4) % 0x800}; // sector for now
            }
            put(n.sector, dir);
            for (const GNode &kid : n.kids) (*this)(kid, path.empty() ? kid.name : path + "/" + kid.name);
        }
    } emit{put_bytes, out};
    emit(root, "");
    // entries' places: sector and offset in it, to a fragment and address
    for (auto &[path, e] : out.entries) {
        uint64_t sector = e.second / 0x800, within = e.second % 0x800;
        uint32_t frag;
        uint64_t addr;
        where(sector, frag, addr);
        e = {frag, addr + within};
    }

    // the media descriptor: the magic, the root's sector and size, its time
    Bytes descriptor(0x800, 0);
    std::memcpy(descriptor.data(), "MICROSOFT*XBOX*MEDIA", 20);
    putle32(descriptor, 0x14, uint32_t(root.sector));
    putle32(descriptor, 0x18, root.length);
    putle64(descriptor, 0x1C, 128895426440000000ull);
    auto put_at = [&](uint64_t address, const Bytes &b) { regions.at(0).push_back({address, b}); };
    switch (layout) {
    case Layout::Egdf: put_at(0x2000, descriptor); break;
    case Layout::Xsf:
        put_at(0x12000, descriptor);
        put_at(0x2000, Bytes{'X', 'S', 'F', 0});
        break;
    case Layout::Multi: put_at(0x2000, descriptor); break;
    case Layout::Single: put_at(0xD000, descriptor); break;
    }

    out.header = dir / name;
    out.data = dir / (name + ".data");
    fs::create_directories(out.data);
    for (uint32_t f = 0; f < fragments; f++) {
        char file_name[16];
        std::snprintf(file_name, sizeof(file_name), "Data%04u", f);
        std::FILE *file = std::fopen((out.data / file_name).string().c_str(), "wb");
        regions[f].push_back({0, Bytes(0x2000 + 0xB000, 0)}); // the tables at the start (and a single file's header)
        std::sort(regions[f].begin(), regions[f].end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (auto &[address, bytes] : regions[f]) {
            std::fseek(file, long(address), SEEK_SET);
            std::fwrite(bytes.data(), 1, bytes.size(), file);
        }
        std::fclose(file);
    }
    Bytes d(0x24, 0);
    d[0] = 0x24;
    d[0x18] = egdf ? 0x40 : 0; // features: enhanced GDF layout
    putle24(d, 0x1C, start_data_block);
    write_file(out.header, make_header(PACKAGE_MAGIC_LIVE, 1, 0xA000, d, fragments, header_file_size));
    return out;
}

GNode file_node(const std::string &name, size_t size, uint32_t seed) {
    GNode n;
    n.name = name;
    n.data = pattern(size, seed);
    return n;
}

GNode svod_tree(bool spanning_fragments) {
    GNode root;
    root.dir = true;
    root.kids.push_back(file_node("a.txt", 300, 11));
    GNode docs;
    docs.name = "docs", docs.dir = true;
    docs.kids.push_back(file_node("b.bin", 5000, 12));
    GNode sub;
    sub.name = "sub", sub.dir = true;
    sub.kids.push_back(file_node("c.bin", 0x800 * 3 + 17, 13));
    docs.kids.push_back(std::move(sub));
    root.kids.push_back(std::move(docs));
    root.kids.push_back(file_node("big.bin", 0x198 * 0x800 * 3 / 2, 14)); // across a group's hash table
    root.kids.push_back(file_node("caf\xE9.dat", 100, 15));
    GNode many;
    many.name = "many", many.dir = true;
    for (int i = 0; i < 100; i++) { // a directory of more than one sector
        char name[16];
        std::snprintf(name, sizeof(name), "f%03d.dat", i);
        many.kids.push_back(file_node(name, 10 + size_t(i), 200 + uint32_t(i)));
    }
    root.kids.push_back(std::move(many));
    root.kids.push_back(GNode{"emptydir", true, {}, {}, 0, 0, 0});
    if (spanning_fragments) {
        GNode span = file_node("span.bin", 0x800 * 20 + 3, 16);
        span.forced_sector = 0x14388 - 8; // k = 0: sectors 0x14380... run from the first data file into the second
        root.kids.push_back(std::move(span));
    }
    return root;
}

void flatten(const GNode &n, const std::string &path, std::vector<Item> &out) {
    for (const GNode &kid : n.kids) {
        std::string p = path.empty() ? kid.name : path + "/" + kid.name;
        out.push_back({p, kid.dir, kid.data});
        if (kid.dir) flatten(kid, p, out);
    }
}

void svod_test(Layout layout, uint32_t start_data_block, uint32_t fragments, bool spanning, const char *layout_name) {
    fs::path dir = temp_dir();
    GNode tree = svod_tree(spanning);
    SvodBuilt built = build_svod(tree, layout, start_data_block, fragments, 0xA000, dir, "pkg");
    std::vector<Item> items;
    // the sorted tree as built
    std::string error;
    auto p = Package::open(built.header, error);
    CHECK(p != nullptr);
    if (!p) {
        std::printf("svod %s: %s\n", layout_name, error.c_str());
        remove_tree(dir);
        return;
    }
    flatten(tree, "", items);
    CHECK(!p->is_stfs());
    CHECK(p->info().magic == PACKAGE_MAGIC_LIVE);
    CHECK(p->info().data_file_count == fragments);
    CHECK(p->read_only());
    CHECK(p->warnings().empty());
    CHECK(p->layout().find(layout_name) != std::string::npos);
    check_items(*p, items);
    // every entry has the root's time, the FILETIME in the descriptor
    const PackageNode *c = p->find("docs/sub/c.bin");
    CHECK(c && c->create_time == 128895426440000000ull && c->write_time == 128895426440000000ull);
    CHECK(p->root().write_time == 128895426440000000ull);
    CHECK(p->find("emptydir") && p->find("emptydir")->children.empty());
    CHECK(p->find("many") && p->find("many")->children.size() == 100);
    p.reset();
    remove_tree(dir);
}

// ---- damaged packages ----

void damage_tests() {
    std::string error;
    fs::path dir = temp_dir();
    std::vector<Item> items = stfs_items();

    // not a package, too small, the header alone
    write_file(dir / "text", Bytes(0x9710, 'x'));
    CHECK(!Package::open(dir / "text", error));
    CHECK(!error.empty());
    write_file(dir / "tiny", Bytes(100, 0));
    CHECK(!Package::open(dir / "tiny", error));
    CHECK(!Package::open(dir / "missing", error));
    StfsBuilt built = build_stfs(items, true, false, 0xAD0E);
    Bytes header_only(built.bytes.begin(), built.bytes.begin() + 0xB000);
    write_file(dir / "header_only", header_only);
    CHECK(!Package::open(dir / "header_only", error));

    // cut short: the file table is there and chains are not, or the open fails; never a crash
    Bytes cut(built.bytes.begin(), built.bytes.begin() + long(built.bytes.size() / 2));
    write_file(dir / "cut", cut);
    {
        auto p = Package::open(dir / "cut", error);
        CHECK(!p || !p->warnings().empty());
        if (p) {
            const PackageNode *big = p->find("big.bin");
            if (big) {
                Bytes got = p->read_all(*big);
                CHECK(got.size() <= big->size);
            }
        }
    }

    // an entry that names a file as its directory
    Bytes bad = built.bytes;
    put16(bad, built.table_offset + 1 * 0x40 + 0x32, 0); // "docs" in directory 0, which is a.txt
    write_file(dir / "bad_parent", bad);
    CHECK(!Package::open(dir / "bad_parent", error));

    // SVOD: no fragments, fewer than the header says, nodes that loop, directories that contain themselves
    {
        fs::path d = temp_dir();
        SvodBuilt s = build_svod(svod_tree(false), Layout::Egdf, 0, 1, 0xA000, d, "pkg");
        CHECK(Package::open(s.header, error));
        // a loop of nodes: a.txt's left node is itself
        Bytes loop(2);
        putle16(loop, 0, s.ordinals.at("a.txt"));
        {
            std::FILE *f = std::fopen((s.data / "Data0000").string().c_str(), "r+b");
            auto [frag, addr] = s.entries.at("a.txt");
            std::fseek(f, long(addr), SEEK_SET);
            std::fwrite(loop.data(), 1, 2, f);
            std::fclose(f);
        }
        CHECK(!Package::open(s.header, error));
        CHECK(error.find("loop") != std::string::npos);
        remove_tree(d);
        d = temp_dir();
        s = build_svod(svod_tree(false), Layout::Egdf, 0, 1, 0xA000, d, "pkg");
        // "docs" has the root's sector as its own: a directory in itself
        Bytes self(4);
        putle32(self, 0, uint32_t(s.root_sector));
        {
            std::FILE *f = std::fopen((s.data / "Data0000").string().c_str(), "r+b");
            auto [frag, addr] = s.entries.at("docs");
            std::fseek(f, long(addr + 4), SEEK_SET);
            std::fwrite(self.data(), 1, 4, f);
            std::fclose(f);
        }
        CHECK(!Package::open(s.header, error));
        CHECK(error.find("deep") != std::string::npos);
        remove_tree(d);
        d = temp_dir();
        s = build_svod(svod_tree(false), Layout::Egdf, 0, 1, 0xA000, d, "pkg");
        remove_tree(s.data / "Data0000");
        CHECK(!Package::open(s.header, error)); // no fragment where the header says one
        remove_tree(s.data);
        CHECK(!Package::open(s.header, error)); // no ".data"
        remove_tree(d);
    }
    remove_tree(dir);
}

// ---- the device ----

void device_test() {
    fs::path dir = temp_dir();
    std::vector<Item> items = stfs_items();
    write_file(dir / "stfs", build_stfs(items, true, false, 0xAD0E).bytes);
    std::string error;
    auto device = kernel::vfs::make_package_device("\\Device\\Content\\1", dir / "stfs", error);
    CHECK(device != nullptr);
    CHECK(!kernel::vfs::make_package_device("\\Device\\Content\\2", dir / "none", error));

    KernelHarness h;
    CHECK(h.init());
    if (!device || !h.k) {
        remove_tree(dir);
        return;
    }
    CHECK(device->name == "STFS" && device->read_only && device->component_name_max_length == 40);
    CHECK(device->sectors_per_unit == 8 && device->bytes_per_sector == 0x200);
    kernel::vfs::FileSystem &files = kernel::file_system(*h.k);
    files.add_device(device);
    CHECK(files.add_link("pkg:", "\\Device\\Content\\1"));

    kernel::vfs::FileSystem::Opened o;
    kernel::vfs::Action action;
    uint32_t status = files.open(nullptr, nullptr, "pkg:\\DOCS\\b.bin", kernel::vfs::Disposition::Open,
                              kernel::vfs::GENERIC_READ, false, true, o, action);
    CHECK(status == kernel::X_STATUS_SUCCESS);
    if (status == kernel::X_STATUS_SUCCESS) {
        CHECK(o.entry->size == 10000 && !o.entry->directory());
        CHECK(o.entry->attributes & kernel::vfs::ATTRIBUTE_READ_ONLY);
        Bytes got(10000);
        size_t done = 0;
        CHECK(o.stream->read(0, got.data(), got.size(), done) == kernel::X_STATUS_SUCCESS && done == 10000);
        CHECK(got == items[2].data);
        CHECK(o.stream->read(10000, got.data(), 1, done) == kernel::X_STATUS_END_OF_FILE);
        uint8_t byte = 1;
        CHECK(o.stream->write(0, &byte, 1, done) == kernel::X_STATUS_ACCESS_DENIED && done == 0);
        CHECK(o.stream->set_length(5) == kernel::X_STATUS_ACCESS_DENIED);
    }
    kernel::vfs::FileSystem::Opened o2;
    CHECK(files.open(nullptr, nullptr, "pkg:\\docs\\none", kernel::vfs::Disposition::Open, kernel::vfs::GENERIC_READ, false,
                  false, o2, action) == kernel::X_STATUS_OBJECT_NAME_NOT_FOUND);
    // a new file cannot be made
    CHECK(files.open(nullptr, nullptr, "pkg:\\new.bin", kernel::vfs::Disposition::Create,
                  kernel::vfs::GENERIC_READ | kernel::vfs::GENERIC_WRITE, false, false, o2,
                  action) != kernel::X_STATUS_SUCCESS);
    kernel::vfs::Entry *docs = files.resolve("pkg:\\docs");
    CHECK(docs && docs->directory() && docs->children.size() == 2);

    // an SVOD through its header file
    fs::path d2 = temp_dir();
    SvodBuilt s = build_svod(svod_tree(false), Layout::Egdf, 3, 1, 0xA000, d2, "svod");
    auto sdev = kernel::vfs::make_package_device("\\Device\\Content\\3", s.header, error);
    CHECK(sdev != nullptr);
    if (sdev) {
        CHECK(sdev->name == "FATX" && sdev->read_only && sdev->component_name_max_length == 255);
        files.add_device(sdev);
        CHECK(files.add_link("svod:", "\\Device\\Content\\3"));
        kernel::vfs::Entry *e = files.resolve("svod:\\docs\\sub\\c.bin");
        CHECK(e && e->size == 0x800 * 3 + 17);
        kernel::vfs::Entry *d = files.resolve("svod:\\docs");
        CHECK(d && d->directory() && (d->attributes & kernel::vfs::ATTRIBUTE_READ_ONLY));
        kernel::vfs::FileSystem::Opened so;
        CHECK(files.open(nullptr, nullptr, "svod:\\big.bin", kernel::vfs::Disposition::Open, kernel::vfs::GENERIC_READ, false,
                      true, so, action) == kernel::X_STATUS_SUCCESS);
        if (so.stream) {
            Bytes got(0x198 * 0x800 * 3 / 2);
            size_t done = 0;
            CHECK(so.stream->read(0, got.data(), got.size(), done) == kernel::X_STATUS_SUCCESS && done == got.size());
            CHECK(got == pattern(got.size(), 14));
        }
    }
    files.remove_link("pkg:");
    files.remove_link("svod:");
    files.remove_device("\\Device\\Content\\1");
    files.remove_device("\\Device\\Content\\3");
    // what holds a device open
    o = {};
    o2 = {};
    device.reset();
    sdev.reset();
    remove_tree(dir);
    remove_tree(d2);
}

// several threads reading one package at once
void thread_test() {
    fs::path dir = temp_dir();
    std::vector<Item> items = stfs_items();
    write_file(dir / "pkg", build_stfs(items, false, true, 0xA000).bytes);
    std::string error;
    auto p = Package::open(dir / "pkg", error);
    CHECK(p != nullptr);
    if (p) {
        std::atomic<int> bad{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; t++)
            threads.emplace_back([&, t] {
                for (int round = 0; round < 20; round++)
                    for (size_t i = size_t(t); i < items.size(); i++) {
                        if (items[i].dir) continue;
                        const PackageNode *n = p->find(items[i].path.find('\xE9') == std::string::npos
                                                           ? items[i].path
                                                           : "caf\xC3\xA9 \xE2\x82\xAC.dat");
                        if (!n || p->read_all(*n) != items[i].data) bad++;
                    }
            });
        for (auto &th : threads) th.join();
        CHECK(bad == 0);
    }
    p.reset();
    remove_tree(dir);
}

} // namespace

int run();

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        return run();
    } catch (const std::exception &e) {
        std::printf("package: exception: %s\n", e.what());
        return 1;
    }
}

int run() {
    std::vector<Item> items = stfs_items();
    // read-only format and two tables per level; the root's table in either copy; a header size that rounds up
    stfs_test(true, false, 0xAD0E, items);
    stfs_test(false, false, 0xA000, items);
    stfs_test(false, true, 0xAD0E, items);
    // a package of fewer than 170 blocks: only the first table
    std::vector<Item> small(items.begin(), items.begin() + 7);
    stfs_test(true, false, 0xA000, small);
    stfs_test(false, true, 0xA000, small);
    stfs_test(false, false, 0xA000, small);

    svod_test(Layout::Egdf, 0, 1, false, "EGDF");
    svod_test(Layout::Egdf, 5, 1, false, "EGDF");
    svod_test(Layout::Xsf, 0, 1, false, "XSF");
    svod_test(Layout::Xsf, 3, 1, false, "XSF");
    svod_test(Layout::Multi, 0, 2, false, "multiple files");
    svod_test(Layout::Single, 0, 1, false, "single file");
    svod_test(Layout::Single, 2, 1, false, "single file");
    svod_test(Layout::Multi, 0, 2, true, "multiple files"); // a file across two data files

    CHECK(package_name_to_utf8("caf\xE9") == "caf\xC3\xA9");
    CHECK(package_name_to_utf8("\x80\x99") == "\xE2\x82\xAC\xE2\x84\xA2");
    CHECK(fat_timestamp(15055, 25558) == 128895426440000000ull);
    CHECK(fat_timestamp(0, 0) == 0);

    damage_tests();
    device_test();
    thread_test();
    return finish("package");
}
