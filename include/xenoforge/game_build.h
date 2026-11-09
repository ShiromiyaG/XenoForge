// The emulator's recompile step (docs/plan.md, D2; docs/runtime.md, "The
// game library"): a disc's game library built on the player's machine into a
// cache, and found again by its executable's digest. The launcher calls it
// on a worker thread and shows the progress; boot and the generic player
// find the cached library (game_library.h).
//
// This header is the contract between the launcher's view and the build:
// discovery and emission in process into a work directory under the cache,
// then clang and lld (the toolchain beside the launcher, else an installed
// LLVM) compiling the files in parallel and linking the library, which is
// kept as <cache>/<digest>-<interface version>.dll (.so).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xf::build {

// the stages a build goes through, in order
enum class Stage { Reading, Discovering, Emitting, Compiling, Linking, Done, Failed };

// what the build reports as it goes, from its worker thread: the stage,
// how far it is in it (done of total units, 0 of 0 when unknown) and a
// line to show (a module's name, a file, the error)
struct Progress {
    std::function<void(Stage stage, uint64_t done, uint64_t total, std::string_view detail)> report;
    // set by the caller to stop the build; it stops at the next unit (the
    // compilers under way are ended)
    std::atomic<bool> *cancel = nullptr;
};

// a game library in the cache
struct CachedGame {
    std::filesystem::path library; // the DLL (.so) to load
    std::string digest;            // the executable's image digest, as the launcher's library shows it
    uint32_t interface_version = 0;
};

// where the cache lives by default (%LOCALAPPDATA%\xenoforge\cache, or
// $XDG_CACHE_HOME/xenoforge)
std::filesystem::path default_cache_dir();

// the cached library for an executable's digest (16 hex digits, either
// case), if one was built for this XenoForge's interface version
std::optional<CachedGame> find_cached(const std::filesystem::path &cache_dir, std::string_view digest);

// builds the disc's game library into the cache; blocks, so it runs on a
// worker thread. false with the error when it fails or is cancelled.
bool prepare_game(const std::filesystem::path &disc, const std::filesystem::path &cache_dir, Progress &progress,
                  std::string &error);

// ---- what a build left out, and what the runtime found it missed ----
//
// A build never fails whole (docs/plan.md, D2): a function the emitter cannot
// emit, or that clang rejects (its file is split again until the culprits are
// found), is left out of the library and runs in the interpreter. Only a disc
// that cannot be read, a disc without default.xex, a missing toolchain or a
// fault in XenoForge's own files fails. What was left out, and how many of
// the entry log's lines the build took in, are kept beside the library:
// <cache>/<digest>-<interface version>.build.txt.
//
// The runtime logs each address it enters the interpreter at as a function
// (D3) in <cache>/<digest>.entries.txt (EntryLog: one address in hex per
// line, appended, each address once; kept across XenoForge versions). The
// next prepare_game takes them in as discovery's extra entries, so the
// library gets better at each run with nothing done by hand.

struct LeftOutFunction {
    std::string module;   // the executable's path on the disc ("default.xex")
    uint32_t address = 0; // 0: the whole module was left out
    std::string reason;
};
struct BuildReport {
    std::vector<LeftOutFunction> left_out;
    // the entry log's addresses the build read (the log's first lines: it
    // only grows), and the extra entries it was given in all
    size_t entries_taken = 0, entries = 0;
    // what the shader step (BuildOptions::shader_step) made or why it made
    // nothing, a line each ("shaders <TAB> line" in the report)
    std::vector<std::string> shaders;
    // the project file the library was emitted with (BuildOptions::project):
    // a digest of what it says, empty without one ("project <digest>")
    std::string project;
};

// the build report of the library cached for this digest at this interface
// version (nothing when there is none, or no report beside it)
std::optional<BuildReport> read_build_report(const std::filesystem::path &cache_dir, std::string_view digest);

// The runtime's entry log for an executable's digest, and its addresses
// (distinct, in the order they were logged; none when there is no log).
std::filesystem::path entry_log_path(const std::filesystem::path &cache_dir, std::string_view digest);
std::vector<uint32_t> read_entry_log(const std::filesystem::path &file);

// For the launcher's rebuild offer: how many addresses the runtime logged
// since the cached library for this digest was built, not counting the
// functions that build left out (they run in the interpreter by design). 0
// when there is no cached library for this XenoForge's interface version, or
// nothing new: then rebuilding gains nothing. A rebuild is prepare_game with
// BuildOptions::force.
size_t new_entries(const std::filesystem::path &cache_dir, std::string_view digest);
inline bool has_new_entries(const std::filesystem::path &cache_dir, std::string_view digest) {
    return new_entries(cache_dir, digest) != 0;
}

// The runtime's side: open() loads what the log holds, add() appends an
// address the first time it is seen (later calls cost a lock and a lookup;
// the file is written and flushed only for new ones). hook() has the kernel's
// entry hook's signature (Kernel::set_entry_hook(&EntryLog::hook, &log)).
class EntryLog {
  public:
    EntryLog();
    EntryLog(const EntryLog &) = delete;
    EntryLog &operator=(const EntryLog &) = delete;
    ~EntryLog();
    bool open(const std::filesystem::path &file);
    void add(uint32_t address);
    size_t size() const;   // the distinct addresses, logged before and now
    size_t added() const;  // the ones this run added
    static void hook(void *log, uint32_t address) { static_cast<EntryLog *>(log)->add(address); }

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// ---- extensions (the launcher may leave them alone) ----

// The shader step (docs/renderer.md, "Shaders and pipelines at the
// launcher's build step"): once the library is built, or found in the cache,
// the title's shader and pipeline storage (shader_storage.h, beside the
// library) and the shaders found on the disc are made into the title's
// pipeline cache by the caller's function, as it needs a Vulkan device and
// this library links none (the launcher and xfbuild give
// render::run_shader_step, shader_step.h). It never fails the build: what it
// made, or why it made nothing, is the report's `shaders` lines.
struct ShaderStep {
    std::filesystem::path cache_dir;
    uint32_t title_id = 0;
    std::string digest;
    // the shader containers in the disc's files (shader_container.h): type
    // (0 vertex, 1 pixel) and microcode, each once; and how many files were
    // read for them, in how long
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> disc_shaders;
    size_t disc_files = 0;
    double disc_seconds = 0;
    bool disc_from_earlier = false; // what an earlier step's search found (<digest>.discshaders), not read again
    const std::atomic<bool> *cancel = nullptr;
};

struct BuildOptions {
    unsigned jobs = 4;               // compilers at once (more ran out of memory on 16 GB)
    std::filesystem::path toolchain; // where clang and lld are; empty: find_toolchain's search
    bool keep_work = false;          // keep the emitted code and objects (they are hundreds of MB)
    // keep the objects (<cache>/work/<digest>/obj, about 0.7 MB a file of
    // generated code), each beside its source's key: the next build of the
    // disc (another XenoForge's, the entry log's) compiles again only the
    // files whose text, the headers' code, the flags or the compiler changed
    bool keep_objects = true;
    bool force = false;              // build even when the cache has the library
    std::vector<uint32_t> entries;   // extra function entries, beside the entry log's
    bool use_entry_log = true;       // take the entry log's addresses in
    // what the build left out and took in, when given (also on a cache hit,
    // from the report beside the library)
    BuildReport *report = nullptr;
    // the shader step, run after the library is built or found; the disc's
    // files searched for shader containers for it first (files up to 64 MB,
    // media left out)
    std::function<std::vector<std::string>(const ShaderStep &step)> shader_step;
    bool disc_shaders = true;
    // the shader step alone, the library neither built nor needed (its
    // report updated when there is one)
    bool shaders_only = false;
    // For the tests: functions (by address, in any module) made to fail the
    // emission, or to fail clang (a line that does not compile put in their
    // body), so the build's recovery is exercised.
    std::vector<uint32_t> fail_emit, fail_compile;
    // A port's project file (docs/recompiler.md, "The project file"): its
    // hooks' slots, names, starts and tables emitted into the library. The
    // library cached for the executable is built again when it was made
    // with another project file, or none (BuildReport::project).
    std::filesystem::path project;
};
bool prepare_game(const std::filesystem::path &disc, const std::filesystem::path &cache_dir, Progress &progress,
                  std::string &error, const BuildOptions &options);

// The compile and link alone: `sources` (paths relative to src_root: what
// xfrecomp emit wrote, its modules.cpp among them) built into a game library
// at `library`, with the headers, objects and logs in `work`. What
// prepare_game does after the emission; for the tests and ports' tools. A
// functions file clang rejects is split and its culprits left out as there
// (options.report lists them; entries are 0).
bool build_library(const std::filesystem::path &src_root, const std::vector<std::string> &sources,
                   const std::filesystem::path &work, const std::filesystem::path &library, Progress &progress,
                   std::string &error, const BuildOptions &options = {});

// The directory holding clang and lld: `hint` (or its bin/), XF_TOOLCHAIN
// (or its bin/), toolchain/bin beside the executable (what the launcher
// ships), then an installed LLVM (%ProgramFiles%\LLVM\bin) and the PATH.
std::optional<std::filesystem::path> find_toolchain(const std::filesystem::path &hint = {});

// whether this CPU runs what the build makes (AVX2 and FMA, as the
// statically linked games are built); why not when it does not
bool cpu_supported(std::string &why);

} // namespace xf::build
