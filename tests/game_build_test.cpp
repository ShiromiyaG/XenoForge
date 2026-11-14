// The recompile step (game_build.h) on an executable the test makes, a build
// that never fails whole (docs/plan.md, D2) and the entry log's feedback (D3):
// - a function made to fail the emission (BuildOptions::fail_emit) and one
//   made to fail clang (fail_compile, in a file of 18 functions, split twice
//   to find it) are left out, listed with why in the report beside the
//   library, and the library is made: its table has no code for them, and
//   their callers reach dispatch through their stubs;
// - an address logged by EntryLog (once, however often it is added) inside a
//   function .pdata sizes is no function without the log, and one after the
//   next build, which takes the log in; new_entries counts what was logged
//   since the build, but the functions it left out;
// - the cache's report read back when the library is cached already.
// Needs clang and lld (find_toolchain); without them the test is skipped.

#include "check.h"

#include "xenoforge/game_build.h"
#include "xenoforge/game_library.h"
#include "xenoforge/patches.h"
#include "xenoforge/ppc_context.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

using namespace xf;

namespace {

constexpr uint32_t BASE = 0x82000000, IMAGE_SIZE = 0x3000;
constexpr uint32_t E = BASE + 0x1000;     // the entry: calls the 16 leaves
constexpr uint32_t LEAVES = BASE + 0x1100; // leaf k at LEAVES + 0x10 * k: addi r3, r3, k + 1; blr
constexpr uint32_t P = BASE + 0x1200;     // .pdata's: li r3, 0; blr; li r3, 42; blr
constexpr uint32_t X = P + 8;             // inside P: only the entry log makes it a function
constexpr uint32_t PDATA = 0x2000;        // (an offset in the image)
constexpr uint32_t BAD_EMIT = LEAVES + 0x10 * 9, BAD_COMPILE = LEAVES + 0x10 * 5;

const ppc::GeneratedModule *current = nullptr;
uint64_t missed = 0;

void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    v[at] = uint8_t(x >> 24), v[at + 1] = uint8_t(x >> 16), v[at + 2] = uint8_t(x >> 8), v[at + 3] = uint8_t(x);
}
void put_le16(std::vector<uint8_t> &v, size_t at, uint16_t x) { v[at] = uint8_t(x), v[at + 1] = uint8_t(x >> 8); }
void put_le32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    for (int k = 0; k < 4; k++) v[at + k] = uint8_t(x >> (8 * k));
}

uint32_t addi(uint32_t d, uint32_t a, int16_t v) { return 14u << 26 | d << 21 | a << 16 | uint16_t(v); }
uint32_t bl(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC) | 1; }
constexpr uint32_t BLR = 0x4E800020, MFLR_R12 = 0x7D8802A6, MTLR_R12 = 0x7D8803A6;

// The image: MZ and PE headers with one code section (.text at 0x1000) and
// the exception directory (.pdata, holding P); then the code.
std::vector<uint8_t> make_image() {
    std::vector<uint8_t> image(IMAGE_SIZE, 0);
    image[0] = 'M', image[1] = 'Z';
    put_le32(image, 0x3C, 0x80);
    std::memcpy(&image[0x80], "PE\0\0", 4);
    put_le16(image, 0x80 + 6, 1);     // sections
    put_le16(image, 0x80 + 20, 0xE0); // the optional header's size
    const uint32_t optional = 0x80 + 24;
    put_le16(image, optional, 0x10B);
    put_le32(image, optional + 92, 16); // data directories
    put_le32(image, optional + 96 + 8 * 3, PDATA);
    put_le32(image, optional + 96 + 8 * 3 + 4, 8);
    const uint32_t section = optional + 0xE0;
    std::memcpy(&image[section], ".text", 5);
    put_le32(image, section + 8, 0x1000);
    put_le32(image, section + 12, 0x1000);
    put_le32(image, section + 36, 0x60000020);
    put32(image, PDATA, P);
    put32(image, PDATA + 4, 4u << 8); // 4 instructions
    auto code = [&](uint32_t at, std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) put32(image, at - BASE, w), at += 4;
    };
    code(E, {MFLR_R12});
    for (uint32_t k = 0; k < 16; k++) code(E + 4 + 4 * k, {bl(E + 4 + 4 * k, LEAVES + 0x10 * k)});
    code(E + 4 + 64, {MTLR_R12, BLR});
    for (uint32_t k = 0; k < 16; k++) code(LEAVES + 0x10 * k, {addi(3, 3, int16_t(k + 1)), BLR});
    code(P, {addi(3, 0, 0), BLR, addi(3, 0, 42), BLR});
    return image;
}

// the executable: an XEX2 file neither encrypted nor compressed (as
// kernel_modules_test makes its DLLs), a title, not a DLL
std::vector<uint8_t> make_xex() {
    std::vector<uint8_t> f(0x1000, 0);
    std::memcpy(f.data(), "XEX2", 4);
    put32(f, 0x04, 0x1);
    put32(f, 0x08, 0x1000);
    put32(f, 0x10, 0x400);
    const std::pair<uint32_t, uint32_t> headers[] = {
        {xex_key::ENTRY_POINT, E},
        {xex_key::IMAGE_BASE_ADDRESS, BASE},
        {xex_key::FILE_FORMAT_INFO, 0x600},
        {xex_key::EXECUTION_INFO, 0x700}};
    put32(f, 0x14, uint32_t(std::size(headers)));
    for (size_t i = 0; i < std::size(headers); i++) {
        put32(f, 0x18 + 8 * i, headers[i].first);
        put32(f, 0x1C + 8 * i, headers[i].second);
    }
    put32(f, 0x400 + 0x004, IMAGE_SIZE);
    put32(f, 0x400 + 0x110, BASE);
    put32(f, 0x600, 8); // no encryption, no compression
    put32(f, 0x700 + 0xC, 0x58460001);   // the execution info: a made-up title ID
    put32(f, 0x400 + 0x10C, 0x10000000); // 4 KB pages...
    put32(f, 0x400 + 0x180, 1);          // ...in one run of three, all code
    put32(f, 0x400 + 0x184, (3u << 4) | uint32_t(XexSection::Code));
    std::vector<uint8_t> image = make_image();
    f.insert(f.end(), image.begin(), image.end());
    return f;
}

const ppc::GeneratedFunction *entry_of(const ppc::GeneratedModule *m, uint32_t address) {
    for (size_t i = 0; i < m->count; i++)
        if (m->functions[i].address == address) return &m->functions[i];
    return nullptr;
}

bool has(const build::BuildReport &r, uint32_t address, const char *why) {
    for (const build::LeftOutFunction &f : r.left_out)
        if (f.address == address && f.module == "default.xex" && f.reason.find(why) != std::string::npos) return true;
    return false;
}

// builds into the cache; the library, loaded
const ppc::GameLibrary *build_and_load(const std::filesystem::path &xex, const std::filesystem::path &cache,
                                       build::BuildReport &report) {
    build::Progress progress;
    progress.report = [](build::Stage stage, uint64_t, uint64_t, std::string_view detail) {
        if (stage == build::Stage::Failed || detail.starts_with("left out") || detail.starts_with("clang rejected "))
            std::printf("  %.*s\n", int(detail.size()), detail.data());
    };
    build::BuildOptions options;
    options.force = true;
    options.fail_emit = {BAD_EMIT};
    options.fail_compile = {BAD_COMPILE};
    options.report = &report;
    std::string error;
    bool built = build::prepare_game(xex, cache, progress, error, options);
    CHECK(built);
    if (!built) return std::printf("%s\n", error.c_str()), nullptr;
    const ppc::GameLibrary *library = nullptr;
    static const ppc::Runtime runtime = ppc::host_runtime();
    for (const auto &entry : std::filesystem::directory_iterator(cache))
        if (entry.path().extension() == ".dll" || entry.path().extension() == ".so")
            library = ppc::load_game_library(entry.path(), runtime, error);
    CHECK(library && library->count == 1);
    if (!library) std::printf("%s\n", error.c_str());
    return library;
}

// E through the library: the leaves' sum but the two left out, which reach
// dispatch (the interpreter, in the kernel)
void run_entry(const ppc::GeneratedModule *m) {
    auto c = std::make_unique<ppc::Context>();
    std::memset(c.get(), 0, sizeof(ppc::Context));
    current = m;
    missed = 0;
    ppc::dispatch(*c, E);
    CHECK(c->r[3] == 136 - (5 + 1) - (9 + 1) && missed == 2);
}

} // namespace

// the runtime's calls, as the kernel's would be (ppc_runtime.h): the
// module's table, a function without code counted as the interpreter's
namespace xf::ppc {
void dispatch(Context &c, uint32_t target) {
    const GeneratedFunction *f = current ? entry_of(current, target) : nullptr;
    if (f && f->fn) return f->fn(c);
    missed++;
}
void invalid(Context &, uint32_t) {}
[[noreturn]] void unwind(Context &c) { throw Unwind{uint32_t(c.lr) & ~3u}; }
void switch_stack(Context &, uint32_t, uint32_t) {}
} // namespace xf::ppc

// The jobs policy (game_build.h's JobPolicy): the cores and the memory free
// less the reserve, a given count winning, the cores alone where the memory
// is unknown, and at least one compiler.
void job_policy() {
    const uint64_t GB = 1ull << 30;
    const uint64_t job = build::job_memory(2ull << 20);
    CHECK(job > 256ull << 20 && job < GB);
    build::JobPolicy p = build::job_policy(0, 16, 16 * GB, 2 * GB + 5 * job + job / 2);
    CHECK(p.reserve_bytes == 2 * GB && p.limit == 5 && !p.why.empty());
    CHECK(build::job_policy(0, 4, 16 * GB, 14 * GB).limit == 4); // the cores
    CHECK(build::job_policy(0, 8, 8 * GB, GB / 2).limit == 1);   // too little: still one
    CHECK(build::job_policy(3, 16, 16 * GB, GB / 2).limit == 3); // given
    CHECK(build::job_policy(0, 16, 0, 0).limit == 4);            // unknown memory
    CHECK(build::available_memory() <= build::total_memory());
}

int main() {
    job_policy();
    if (!build::find_toolchain()) {
        std::printf("game_build: no clang and lld, skipped\n");
        return finish("game_build");
    }
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "xf_game_build_test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path xex_path = root / "default.xex", first = root / "cache1", second = root / "cache2";
    const std::vector<uint8_t> file = make_xex();
    std::ofstream(xex_path, std::ios::binary).write(reinterpret_cast<const char *>(file.data()), file.size());
    Xex xex;
    std::string error;
    CHECK(parse_xex(file, xex, error) && load_xex_image(file, xex, error));
    char digest[24];
    std::snprintf(digest, sizeof(digest), "%016llx", (unsigned long long)image_digest(xex.image));

    // D2: the build without the log, two functions failing
    build::BuildReport report;
    const ppc::GameLibrary *library = build_and_load(xex_path, first, report);
    CHECK(report.left_out.size() == 2 && report.entries_taken == 0 && report.entries == 0);
    CHECK(has(report, BAD_EMIT, "BuildOptions::fail_emit") && has(report, BAD_COMPILE, "clang rejected it"));
    if (library) {
        const ppc::GeneratedModule *m = library->modules[0];
        const ppc::GeneratedFunction *bad_emit = entry_of(m, BAD_EMIT), *bad_compile = entry_of(m, BAD_COMPILE);
        CHECK(bad_emit && !bad_emit->fn && bad_compile && !bad_compile->fn);
        CHECK(entry_of(m, E) && entry_of(m, E)->fn && entry_of(m, LEAVES)->fn && entry_of(m, P)->fn);
        CHECK(!entry_of(m, X));
        run_entry(m);
    }
    auto read_back = build::read_build_report(first, digest);
    CHECK(read_back && read_back->left_out.size() == 2 && has(*read_back, BAD_COMPILE, "clang rejected it"));

    // D3: the runtime logs X (and a function left out, and X again)
    {
        build::EntryLog log;
        CHECK(log.open(build::entry_log_path(first, digest)));
        log.add(X), log.add(BAD_EMIT), log.add(X);
        CHECK(log.size() == 2 && log.added() == 2);
    }
    CHECK(build::read_entry_log(build::entry_log_path(first, digest)) == std::vector<uint32_t>({X, BAD_EMIT}));
    CHECK(build::new_entries(first, digest) == 1 && build::has_new_entries(first, digest));
    CHECK(build::new_entries(second, digest) == 0); // nothing cached there
    {
        build::EntryLog again; // what is logged already is not appended again
        CHECK(again.open(build::entry_log_path(first, digest)));
        again.add(X);
        CHECK(again.size() == 2 && again.added() == 0);
    }

    // the next build takes the log in (in a second cache: the first's library
    // is loaded, so it cannot be replaced)
    std::filesystem::create_directories(second, ec);
    std::filesystem::copy_file(build::entry_log_path(first, digest), build::entry_log_path(second, digest), ec);
    library = build_and_load(xex_path, second, report);
    CHECK(report.entries_taken == 2 && report.entries == 2 && report.left_out.size() == 2);
    CHECK(build::new_entries(second, digest) == 0);
    if (library) {
        const ppc::GeneratedModule *m = library->modules[0];
        const ppc::GeneratedFunction *x = entry_of(m, X);
        CHECK(x && x->fn);
        if (x && x->fn) {
            auto c = std::make_unique<ppc::Context>();
            std::memset(c.get(), 0, sizeof(ppc::Context));
            current = m;
            ppc::dispatch(*c, X);
            CHECK(c->r[3] == 42);
        }
        run_entry(m);
    }

    // the library cached: no build, the report read back
    build::Progress quiet;
    build::BuildOptions cached;
    build::BuildReport cached_report;
    cached.report = &cached_report;
    CHECK(build::prepare_game(xex_path, second, quiet, error, cached));
    CHECK(cached_report.entries_taken == 2 && cached_report.left_out.size() == 2);

    // the shader step on the cached library: given the cache and the digest,
    // its lines kept in the report beside the library and read back
    build::BuildOptions shaders;
    build::BuildReport shaders_report;
    shaders.report = &shaders_report;
    shaders.disc_shaders = false;
    std::string step_digest;
    shaders.shader_step = [&](const build::ShaderStep &step) {
        step_digest = step.digest;
        CHECK(step.cache_dir == second);
        return std::vector<std::string>{"made nothing", "a second line"};
    };
    CHECK(build::prepare_game(xex_path, second, quiet, error, shaders));
    CHECK(shaders_report.shaders.size() == 2 && shaders_report.left_out.size() == 2);
    auto reread = build::read_build_report(second, step_digest);
    CHECK(reread && reread->shaders.size() == 2 && reread->shaders[1] == "a second line" &&
          reread->entries_taken == 2 && reread->left_out.size() == 2);

    // E2/E3: a project file through the build. Two leaves hooked (their
    // bodies xfo_, their wrappers the table's), one of them made to fail
    // clang: its body left out as a stub that asks the system for the
    // original, its wrapper (the hooks) kept in the table. The library is
    // built again when the project changes.
    {
        const std::filesystem::path third = root / "cache3", project = root / "port.toml";
        std::ofstream(project) << "hooks = [0x" << std::hex << LEAVES + 0x30 << ", 0x" << BAD_COMPILE
                               << "]\n[names]\n\"0x" << LEAVES + 0x30 << "\" = \"leaf_three\"\n";
        uint64_t links = 0;
        build::Progress counting;
        counting.report = [&](build::Stage stage, uint64_t done, uint64_t total, std::string_view detail) {
            if (stage == build::Stage::Linking && done == 0 && total == 1) links++; // a link begun
            if (stage == build::Stage::Failed) std::printf("  %.*s\n", int(detail.size()), detail.data());
        };
        build::BuildOptions options;
        options.project = project;
        options.fail_compile = {BAD_COMPILE};
        build::BuildReport with;
        options.report = &with;
        CHECK(build::prepare_game(xex_path, third, counting, error, options));
        CHECK(with.left_out.size() == 1 && has(with, BAD_COMPILE, "clang rejected it") && !with.project.empty());
        static const ppc::Runtime runtime = ppc::host_runtime();
        const ppc::GameLibrary *hooked = nullptr;
        for (const auto &entry : std::filesystem::directory_iterator(third))
            if (entry.path().extension() == ".dll" || entry.path().extension() == ".so") {
                // (a copy loaded: the cache's file is built again below)
                const std::filesystem::path copy = root / ("hooked" + entry.path().extension().string());
                std::filesystem::copy_file(entry.path(), copy, std::filesystem::copy_options::overwrite_existing, ec);
                hooked = ppc::load_game_library(copy, runtime, error);
            }
        CHECK(hooked && hooked->count == 1);
        if (hooked && hooked->count == 1) {
            const ppc::GeneratedModule *m = hooked->modules[0];
            CHECK(m->hook_count == 2 && m->name_count == 1);
            CHECK(entry_of(m, BAD_COMPILE) && entry_of(m, BAD_COMPILE)->fn); // the wrapper
            // the leaves' sum, the one left out asked of the system (the interpreter)
            auto c = std::make_unique<ppc::Context>();
            std::memset(c.get(), 0, sizeof(ppc::Context));
            ppc::System system;
            system.run_original = [](ppc::Context &, uint32_t address) { missed += address == BAD_COMPILE; };
            c->system = &system;
            current = m;
            missed = 0;
            ppc::dispatch(*c, E);
            CHECK(c->r[3] == 136 - (5 + 1) && missed == 1);
        }
        // the same project: the cached library; none: built again
        const uint64_t built = links;
        CHECK(build::prepare_game(xex_path, third, counting, error, options) && links == built);
        build::BuildOptions plain;
        build::BuildReport without;
        plain.report = &without;
        CHECK(build::prepare_game(xex_path, third, counting, error, plain) && links == built + 1);
        CHECK(without.project.empty() && without.left_out.empty());
    }

    // The objects kept (the default), a build again recompiles nothing whose
    // text and headers are the same (each object's source key beside it, and
    // the objects' key), and everything once the objects' key differs (other
    // headers' code, flags or compiler); the emitted code itself goes. The
    // work kept whole (keep_work) reuses them the same way. Without either,
    // nothing is left.
    {
        const std::filesystem::path fourth = root / "cache4";
        uint64_t reused = 0, total = 0;
        bool seen = false; // the compile stage's first report: what was compiled before
        build::Progress counting;
        counting.report = [&](build::Stage stage, uint64_t done, uint64_t all, std::string_view) {
            if (stage == build::Stage::Compiling && !seen) seen = true, reused = done, total = all;
        };
        build::BuildOptions kept;
        kept.force = true;
        auto build_once = [&] {
            reused = total = 0, seen = false;
            return build::prepare_game(xex_path, fourth, counting, error, kept);
        };
        CHECK(build_once() && reused == 0 && total > 0);
        std::filesystem::path key, work;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(fourth / "work"))
            if (entry.path().filename() == "objects.key") key = entry.path(), work = entry.path().parent_path();
        CHECK(!key.empty() && std::filesystem::is_directory(work / "obj") && !std::filesystem::exists(work / "src"));
        CHECK(build_once() && reused == total && total > 0);
        kept.keep_work = true;
        CHECK(build_once() && reused == total && total > 0 && std::filesystem::is_directory(work / "src"));
        std::ofstream(key) << "0\n";
        CHECK(build_once() && reused == 0 && total > 0);
        kept.keep_work = false, kept.keep_objects = false;
        CHECK(build_once() && !std::filesystem::exists(work));
    }

    // game patches (patches.h): a code patch enabled for a made-up title (leaf 0 adds 5 instead of 1) is
    // written into the image before discovery, and the library is kept under the digest and the patches'
    // key; the plain name has no library there
    {
        const std::filesystem::path folder = root / "patches", patched = root / "cache_patched";
        std::filesystem::create_directories(folder, ec);
        const patches::ModuleInfo module = patches::module_info(xex);
        CHECK(module.title_id == 0x58460001 && module.code.size() == 1 && module.hash != 0);
        char text[512];
        std::snprintf(text, sizeof(text),
                      "title_name = \"made up\"\ntitle_id = \"58460001\"\nhash = \"0x%016llX\"\n"
                      "[[patch]]\nname = \"Leaf\"\n[[patch.be32]]\naddress = 0x%08X\nvalue = 0x38630005\n",
                      (unsigned long long)module.hash, LEAVES);
        std::ofstream(folder / "58460001 - made up.patch.toml") << text;
        build::Progress quiet2;
        build::BuildOptions options;
        options.patches.directory = folder;
        options.patches.off = false;
        options.patches.enabled = {{"Leaf", true}};
        CHECK(build::prepare_game(xex_path, patched, quiet2, error, options));
        const patches::Plan plan = patches::make_plan(module, options.patches);
        CHECK(plan.any_code());
        const std::string key = patches::cache_key(digest, plan);
        auto cached_patched = build::find_cached(patched, key);
        CHECK(cached_patched && !build::find_cached(patched, digest));
        CHECK(!build::find_cached(patched, digest + std::string("+nothex")));
        if (cached_patched) {
            CHECK(cached_patched->library.filename().string().starts_with(key + "-"));
            static const ppc::Runtime runtime = ppc::host_runtime();
            const ppc::GameLibrary *lib = ppc::load_game_library(cached_patched->library, runtime, error);
            CHECK(lib && lib->count == 1);
            if (lib) {
                auto c = std::make_unique<ppc::Context>();
                std::memset(c.get(), 0, sizeof(ppc::Context));
                current = lib->modules[0];
                ppc::dispatch(*c, LEAVES);
                CHECK(c->r[3] == 5); // the plain library's leaf 0 gives 1
            }
        }
        // the same patch turned off: no code patches, no key
        options.patches.enabled = {{"Leaf", false}};
        CHECK(!patches::make_plan(module, options.patches).any_code());
    }

    // the libraries stay loaded: the directory is left for the next run's
    // remove_all
    return finish("game_build");
}
