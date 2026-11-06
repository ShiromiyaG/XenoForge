// A game library (game_library.h) built by the recompile step from a piece
// of generated code (tests/game_library/code.cpp), loaded, and run beside the
// same code linked into this test:
// - the library takes only the runtime's table and refuses another
//   interface version;
// - it imports nothing (Windows: an empty import directory);
// - the same context comes out of both: the ops, a dispatch, a memcpy the
//   compiler calls, the FPU and VMX ops that reach the C library;
// - an Unwind thrown by the runtime goes through the library's frames to the
//   runtime's catch (resumable), and what that costs, against the static code.
// Needs clang and lld (game_build.h's find_toolchain); without them the
// library half is skipped.

#include "check.h"

#include "xenoforge/game_build.h"
#include "xenoforge/game_library.h"
#include "xenoforge/ppc_context.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace xf;

// the code linked here (tests/game_library/code.cpp)
extern "C" const ppc::GeneratedModule *const xf_modules[];
extern "C" const size_t xf_module_count;

namespace {

// the module whose functions dispatch runs: the static one or the library's
const ppc::GeneratedModule *current = nullptr;
uint64_t dispatched = 0, missed = 0, invalids = 0, thrown = 0;

void (*find(uint32_t address))(ppc::Context &) {
    const ppc::GeneratedFunction *begin = current->functions, *end = begin + current->count;
    auto it = std::lower_bound(begin, end, address,
                               [](const ppc::GeneratedFunction &f, uint32_t a) { return f.address < a; });
    return it != end && it->address == address ? it->fn : nullptr;
}

struct Guest {
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x20000);
    std::unique_ptr<ppc::Context> c = std::make_unique<ppc::Context>();
    ppc::System system;
    Guest() {
        std::memset(c.get(), 0, sizeof(ppc::Context));
        c->base = memory.data();
        c->system = &system;
        c->r[1] = 0x10000;
    }
};

// what code.cpp's xf_00001000 starts from
void setup_arith(Guest &g) {
    ppc::Context &c = *g.c;
    c.r[3] = 5, c.r[4] = 7, c.r[6] = 0x3000;
    const uint8_t word[4] = {0x12, 0x34, 0x56, 0x78};
    std::memcpy(&g.memory[0x3000], word, 4);
    for (int k = 0; k < 300; k++) g.memory[0x4000 + k] = uint8_t(k * 7);
    c.r[7] = 0x5000, c.r[8] = 0x4000, c.r[9] = 300;
    c.f[2] = 2.75;  // fctiw: 3 (to nearest)
    c.f[3] = 1e-30; // fadd. 2.75 + 1e-30: inexact
    c.v[2].f32[0] = 3.0f, c.v[2].f32[1] = -1.5f, c.v[2].f32[2] = 0.25f, c.v[2].f32[3] = 10.0f;
    c.v[3].f32[0] = 8.0f, c.v[3].f32[1] = 0.1f, c.v[3].f32[2] = 1000.0f, c.v[3].f32[3] = 3.0f;
}

// runs the module's function at address, through dispatch as the kernel does
void run(const ppc::GeneratedModule *module, ppc::Context &c, uint32_t address) {
    current = module;
    ppc::dispatch(c, address);
}

// the Unwind's cost: n longjmps from `depth` frames down, nanoseconds each
double unwind_ns(const ppc::GeneratedModule *module, uint32_t depth, int n) {
    Guest g;
    auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < n; k++) {
        g.c->r[5] = depth;
        run(module, *g.c, 0x2000);
    }
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    CHECK(g.c->r[3] == uint64_t(n) && g.c->r[1] == 0x10000 && g.c->r[10] == 0);
    return ns / n;
}

#ifdef _WIN32
// the PE's import directory, read from the file: its size
uint32_t import_directory_size(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        if (at + 4 <= b.size()) std::memcpy(&v, &b[at], 4);
        return v;
    };
    uint32_t pe = u32(0x3C);
    if (u32(pe) != 0x00004550) return ~0u; // "PE\0\0"
    size_t optional = pe + 24;
    // PE32+: the data directories from 112; the imports are the second
    return u32(optional + 112 + 8 + 4);
}
#endif

} // namespace

// the runtime's calls, as the kernel's would be (ppc_runtime.h)
namespace xf::ppc {
void dispatch(Context &c, uint32_t target) {
    if (auto fn = find(target)) {
        dispatched++;
        return fn(c);
    }
    missed++;
}
void invalid(Context &, uint32_t) { invalids++; }
[[noreturn]] void unwind(Context &c) {
    thrown++;
    throw Unwind{uint32_t(c.lr) & ~3u};
}
void switch_stack(Context &, uint32_t, uint32_t) {}
} // namespace xf::ppc

int main() {
    const ppc::GeneratedModule *linked = xf_modules[0];
    CHECK(xf_module_count == 1 && linked->count == 5);

    // the code linked here, first
    Guest a;
    setup_arith(a);
    run(linked, *a.c, 0x1000);
    CHECK(a.c->r[3] == 13 && a.c->r[5] == 0x12345678 && std::memcmp(&a.memory[0x3004], &a.memory[0x3000], 4) == 0);
    CHECK(std::memcmp(&a.memory[0x5000], &a.memory[0x4000], 300) == 0);
    CHECK(int32_t(ppc::lib::bit_cast<uint64_t>(a.c->f[1])) == 3 && a.c->f[4] == 2.75);
    CHECK((a.c->fpscr & (1u << 25)) != 0); // XX: inexact
    CHECK(a.c->v[1].f32[0] == 8.0f && a.c->v[3].f32[0] == 3.0f);
    run(linked, *a.c, 0x1200);
    CHECK(invalids == 1);

    // the library: built into a temporary directory, then loaded
    std::optional<std::filesystem::path> bin = build::find_toolchain();
    if (!bin) {
        std::printf("game_library: no clang and lld, the library half skipped\n");
        return finish("game_library");
    }
    std::error_code ec;
    const std::filesystem::path work = std::filesystem::temp_directory_path(ec) / "xf_game_library_test";
    std::filesystem::remove_all(work, ec);
    const std::filesystem::path dll = work / "test.dll";
    build::Progress progress;
    uint64_t compiled = 0;
    progress.report = [&](build::Stage stage, uint64_t done, uint64_t total, std::string_view detail) {
        if (stage == build::Stage::Compiling && done) compiled = done;
        if (stage == build::Stage::Failed) std::printf("build: %.*s\n", int(detail.size()), detail.data());
        (void)total;
    };
    std::string error;
    auto start = std::chrono::steady_clock::now();
    bool built = build::build_library(XF_GAME_LIBRARY_TEST_DIR, {"code.cpp"}, work / "work", dll, progress, error);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(built && compiled == 2);
    if (!built) return finish("game_library");
    std::printf("built %s in %.1f s with %s\n", dll.string().c_str(), seconds, bin->string().c_str());
#ifdef _WIN32
    CHECK(import_directory_size(dll) == 0);
#endif

    // another interface version is refused, this one taken
    ppc::Runtime runtime = ppc::host_runtime();
    ppc::Runtime other = runtime;
    other.version++;
    CHECK(!ppc::load_game_library(dll, other, error) && error.find("interface version") != std::string::npos);
    const ppc::GameLibrary *library = ppc::load_game_library(dll, runtime, error);
    CHECK(library && library->count == 1);
    if (!library) {
        std::printf("%s\n", error.c_str());
        return finish("game_library");
    }
    const ppc::GeneratedModule *loaded = library->modules[0];
    CHECK(loaded != linked && loaded->count == 5 && loaded->digest == linked->digest && loaded->resume_count == 1);

    // the same context out of both
    Guest b;
    setup_arith(b);
    run(loaded, *b.c, 0x1000);
    CHECK(std::memcmp(a.c.get(), b.c.get(), offsetof(ppc::Context, base)) == 0);
    CHECK(a.memory == b.memory);
    run(loaded, *b.c, 0x1200);
    CHECK(invalids == 2 && missed == 0 && dispatched > 0);

    // the Unwind through the library's frames, and its cost against the
    // static code's: 1 and 16 frames down
    const uint64_t before = thrown;
    for (uint32_t depth : {1u, 16u}) {
        unwind_ns(linked, depth, 200), unwind_ns(loaded, depth, 200); // warm
        double s = unwind_ns(linked, depth, 5000), d = unwind_ns(loaded, depth, 5000);
        std::printf("an Unwind through %2u frames: %6.0f ns linked, %6.0f ns in the library\n", depth + 1, s, d);
    }
    CHECK(thrown == before + 4 * (200 + 5000));
    std::filesystem::remove_all(work, ec);
    return finish("game_library");
}
