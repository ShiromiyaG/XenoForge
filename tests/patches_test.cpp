// Game patches (patches.h) on made-up files: every field type read, the hash
// matching and its refusal, the split of code and data by the module's
// sections, the code written into an image and the cache key it changes, data
// written into guest memory, and XXH3.

#include "check.h"

#include "xenoforge/memory.h"
#include "xenoforge/patches.h"
#include "xenoforge/xxh3.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace xf;

namespace {

constexpr uint32_t BASE = 0x82000000, PAGE = 0x10000;

// an executable of three 64 KB pages: code, data, read-only data
Xex make_xex() {
    Xex x;
    x.base_address = BASE;
    x.execution.title_id = 0x58460001;
    x.image.resize(3 * PAGE);
    for (size_t i = 0; i < x.image.size(); i++) x.image[i] = uint8_t(i * 7 + (i >> 8));
    x.pages = {{uint8_t(XexSection::Code), 1}, {uint8_t(XexSection::Data), 1}, {uint8_t(XexSection::ReadOnly), 1}};
    return x;
}

std::string hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%016llX", (unsigned long long)v);
    return b;
}

std::string patch_text(uint64_t hash) {
    return "title_name = \"A made-up title\"\n"
           "title_id = \"58460001\"\n"
           "hash = [\"" + hex16(hash) + "\", \"\"]\n"
           "[[patch]]\n"
           "name = \"Everything\"\ndesc = \"d\"\nauthor = \"me\"\nis_enabled = true\n"
           "  [[patch.be8]]\n  address = 0x82010000\n  value = 0x1AB\n"
           "  [[patch.be16]]\n  address = 0x82010002\n  value = 0x1234\n"
           "  [[patch.be32]]\n  address = 0x82010004\n  value = 0xDEADBEEF\n"
           "  [[patch.be64]]\n  address = 0x82010008\n  value = 0x0102030405060708\n"
           "  [[patch.f32]]\n  address = 0x82010010\n  value = 1.5\n"
           "  [[patch.f64]]\n  address = 0x82010018\n  value = -2\n"
           "  [[patch.string]]\n  address = 0x82010020\n  value = \"Hi\"\n"
           "  [[patch.u16string]]\n  address = 0x82010030\n  value = \"A\xC3\xA9\"\n"
           "  [[patch.array]]\n  address = 0x82010040\n  value = \"0xAABBCC1\"\n"
           "[[patch]]\n"
           "name = \"Code\"\n"
           "  [[patch.be32]]\n  address = 0x82000100\n  value = 0x60000000\n"
           "[[patch]]\n"
           "name = \"Across\"\nis_enabled = false\n"
           "  [[patch.array]]\n  address = 0x8200FFFE\n  value = \"11223344\"\n"
           "[[patch]]\n"
           "name = \"Broken\"\nis_enabled = true\n"
           "  [[patch.be32]]\n  address = 0x82010100\n  value = \"text\"\n";
}

void parsing(uint64_t hash) {
    patches::File f;
    std::string why;
    CHECK(patches::parse(patch_text(hash), f, why));
    CHECK(f.title_id == 0x58460001 && f.title_name == "A made-up title");
    CHECK(f.hashes.size() == 1 && f.hashes[0] == hash);
    CHECK(f.patches.size() == 3); // the broken one is left out
    CHECK(!f.warnings.empty() && f.warnings[0].find("Broken") != std::string::npos);
    const patches::Patch &all = f.patches[0];
    CHECK(all.name == "Everything" && all.description == "d" && all.author == "me" && all.enabled);
    CHECK(all.entries.size() == 9);
    auto bytes = [&](uint32_t address) -> const std::vector<uint8_t> & {
        for (const patches::Entry &e : all.entries)
            if (e.address == address) return e.bytes;
        static const std::vector<uint8_t> none;
        failures++;
        return none;
    };
    using V = std::vector<uint8_t>;
    // the entries are read in Xenia's type order (string, u16string, array, f64, f32, be64...)
    CHECK(bytes(0x82010000) == V({0xAB}));
    CHECK(bytes(0x82010002) == V({0x12, 0x34}));
    CHECK(bytes(0x82010004) == V({0xDE, 0xAD, 0xBE, 0xEF}));
    CHECK(bytes(0x82010008) == V({1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(bytes(0x82010010) == V({0x3F, 0xC0, 0, 0}));
    CHECK(bytes(0x82010018) == V({0xC0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(bytes(0x82010020) == V({'H', 'i'}));
    CHECK(bytes(0x82010030) == V({0, 'A', 0, 0xE9})); // UTF-16, big-endian
    CHECK(bytes(0x82010040) == V({0xAA, 0xBB, 0xCC, 0x01})); // an odd digit last is read alone
    CHECK(!f.patches[2].enabled);

    // refused: no hash, no title, not TOML
    patches::File g;
    CHECK(!patches::parse("title_name = \"x\"\ntitle_id = \"58460001\"\n", g, why));
    CHECK(!patches::parse("hash = \"0x1\"\ntitle_id = \"58460001\"\n", g, why));
    CHECK(!patches::parse("title_name = = 3", g, why));
    // a single hash as a string
    CHECK(patches::parse("title_name = \"x\"\ntitle_id = \"58460001\"\nhash = \"0xAB\"\n", g, why));
    CHECK(g.hashes.size() == 1 && g.hashes[0] == 0xAB && g.patches.empty());

    CHECK(patches::is_patch_file_name("58460001 - Some name.patch.toml"));
    CHECK(patches::is_patch_file_name("58460001.patch.toml"));
    CHECK(!patches::is_patch_file_name("5846001.patch.toml"));
    CHECK(!patches::is_patch_file_name("58460001 - Some name.toml"));
    CHECK(!patches::is_patch_file_name("Some name - 58460001.patch.toml"));
}

void planning(const Xex &xex, const patches::ModuleInfo &info) {
    CHECK(info.title_id == 0x58460001 && info.base == BASE && info.size == 3 * PAGE);
    CHECK(info.code.size() == 1 && info.code[0].first == BASE && info.code[0].second == BASE + PAGE);
    CHECK(info.hash == xxh3_64(xex.image.data(), PAGE));
    CHECK(info.is_code(BASE) && info.is_code(BASE + PAGE - 1) && !info.is_code(BASE + PAGE));

    patches::File f;
    std::string why;
    CHECK(patches::parse(patch_text(info.hash), f, why));
    f.path = "58460001 - made up.patch.toml";
    const std::vector<patches::File> files{f};

    // the file's own is_enabled: "Everything" and "Code"? ("Code" has none: off)
    patches::Plan plan = patches::make_plan(info, files, {});
    CHECK(!plan.any_code()); // only the data patch is on
    CHECK(plan.items.size() == 9);
    for (const patches::Item &i : plan.items) CHECK(!i.code && i.name == "Everything");

    // the config turns patches on and off over the file
    plan = patches::make_plan(info, files, {{"Code", true}, {"Everything", false}});
    CHECK(plan.items.size() == 1 && plan.items[0].code && plan.items[0].entry.address == BASE + 0x100);
    CHECK(plan.any_code() && plan.code_key.size() == 16);
    const std::string key_code = plan.code_key;
    CHECK(patches::cache_key("0123456789abcdef", plan) == "0123456789abcdef+" + key_code);
    CHECK(patches::cache_key("0123456789abcdef", patches::Plan{}) == "0123456789abcdef");

    // a patch across the code page's end is cut in two
    plan = patches::make_plan(info, files, {{"Code", true}, {"Across", true}, {"Everything", false}});
    CHECK(plan.items.size() == 3);
    CHECK(plan.items[1].code && plan.items[1].entry.address == 0x8200FFFE && plan.items[1].entry.bytes.size() == 2);
    CHECK(!plan.items[2].code && plan.items[2].entry.address == 0x82010000 && plan.items[2].entry.bytes.size() == 2);
    CHECK(plan.code_key != key_code); // another set of code patches, another key

    // the same set of code patches, the same key; a data-only change leaves it
    patches::Plan again = patches::make_plan(info, files, {{"Code", true}, {"Across", true}, {"Everything", true}});
    CHECK(again.code_key == plan.code_key);

    // applied to the image: the code bytes only
    std::vector<uint8_t> image = xex.image;
    CHECK(patches::apply_code(plan, image, BASE) == 6);
    CHECK(image[0x100] == 0x60 && image[0x101] == 0 && image[0xFFFE] == 0x11 && image[0xFFFF] == 0x22);
    CHECK(image[0x10000] == xex.image[0x10000]); // the data half waits for the memory
    // the rest of the image is as it was
    image[0x100] = xex.image[0x100], image[0x101] = xex.image[0x101], image[0x102] = xex.image[0x102];
    image[0x103] = xex.image[0x103], image[0xFFFE] = xex.image[0xFFFE], image[0xFFFF] = xex.image[0xFFFF];
    CHECK(image == xex.image);

    // another version of the module: the hash is refused
    patches::ModuleInfo other = info;
    other.hash ^= 1;
    plan = patches::make_plan(other, files, {{"Code", true}});
    CHECK(plan.items.empty() && !plan.any_code());
    CHECK(!plan.log.empty() && plan.log[0].find("refused") != std::string::npos);
    // another title: nothing, not even a log line
    other = info;
    other.title_id = 0x58460002;
    plan = patches::make_plan(other, files, {{"Code", true}});
    CHECK(plan.items.empty() && plan.log.empty());
}

void directory(const patches::ModuleInfo &info) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "xf_patches_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto write = [&](const char *name, const std::string &text) { std::ofstream(dir / name, std::ios::binary) << text; };
    write("58460001 - made up.patch.toml", patch_text(info.hash));
    write("readme.txt", "not a patch");
    write("58460001 - bad.patch.toml", "this is not toml = = =");
    std::vector<std::string> problems;
    std::vector<patches::File> files = patches::load_directory(dir, &problems);
    CHECK(files.size() == 1 && files[0].path == "58460001 - made up.patch.toml");
    CHECK(problems.size() >= 3); // the name, the bad file, the broken patch's warning
    patches::Config config;
    config.directory = dir;
    config.enabled = {{"Code", true}};
    patches::Plan plan = patches::make_plan(info, config);
    CHECK(plan.any_code() && plan.items.size() == 10);
    config.off = true;
    CHECK(patches::make_plan(info, config).items.empty());
    CHECK(patches::load_directory(dir / "missing").empty());
    fs::remove_all(dir, ec);
}

void data_in_memory(const patches::ModuleInfo &info) {
    std::string error;
    auto memory = Memory::create(error);
    CHECK(memory != nullptr);
    if (!memory) return;
    Heap *heap = memory->heap_for(BASE);
    CHECK(heap->alloc_fixed(BASE, 3 * PAGE, PAGE, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ));
    patches::File f;
    std::string why;
    CHECK(patches::parse(patch_text(info.hash), f, why));
    patches::Plan plan = patches::make_plan(info, {f}, {{"Code", true}, {"Across", true}});
    // one data item lies in no heap's committed memory
    patches::Item stray;
    stray.name = "stray";
    stray.entry = {0x82200000, {1, 2, 3, 4}};
    plan.items.push_back(stray);
    const std::vector<std::string> log = patches::apply_data(plan, *memory);
    const uint8_t *p = memory->translate(BASE + PAGE);
    CHECK(p[0] == 0x33 && p[1] == 0x44 && p[2] == 0x12 && p[3] == 0x34); // across's data half, then be16
    CHECK(memory->translate(BASE + 0x10004)[0] == 0xDE && memory->translate(BASE + 0x10007)[0] == 0xEF);
    CHECK(std::memcmp(memory->translate(BASE + 0x10020), "Hi", 2) == 0);
    CHECK(memory->translate(BASE + 0x100)[0] == 0);   // code items are not the memory's here
    RegionInfo region;
    CHECK(heap->query(BASE + PAGE, region) && region.protect == PROTECT_READ); // the protection is back
    bool said = false;
    for (const std::string &l : log) said |= l.find("stray") != std::string::npos && l.find("left out") != std::string::npos;
    CHECK(said);
}

// XXH3-64: the empty input's known value, and every branch's sizes hashed to different values (the whole
// function was compared with the reference library over 0..5000 bytes and three large sizes when written)
void xxh3_values() {
    CHECK(xxh3_64("", 0) == 0x2D06800538D394C2ull);
    std::vector<uint8_t> d(5000);
    for (size_t i = 0; i < d.size(); i++) d[i] = uint8_t(i * 31 + 7);
    // the sizes agree with themselves when called twice and the branches differ from each other
    uint64_t seen[16];
    int n = 0;
    for (size_t size : {1, 3, 4, 8, 9, 16, 17, 128, 129, 240, 241, 1024, 1025, 5000}) {
        const uint64_t h = xxh3_64(d.data(), size);
        CHECK(h == xxh3_64(d.data(), size));
        for (int k = 0; k < n; k++) CHECK(seen[k] != h);
        seen[n++] = h;
    }
}

} // namespace

int main() {
    Xex xex = make_xex();
    const patches::ModuleInfo info = patches::module_info(xex);
    xxh3_values();
    parsing(info.hash);
    planning(xex, info);
    directory(info);
    data_in_memory(info);
    return finish("patches_test");
}
