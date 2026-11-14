// The game library and the first tier (game.h's TierSupport): what open,
// poll_tier_up and print_report do for a game with no generated code linked
// in. Only the generic boot and player link this file (their main calls
// enable_tiers); a port's executables do not, and with it go the recompiler's
// discovery, the copy-and-patch tier and the recompile step's cache code.

#include "game.h"

#include "xenoforge/game_build.h"
#include "xenoforge/game_library.h"
#include "xenoforge/recomp.h"
#include "xenoforge/stencil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xf::game {

struct Tiers {
    // where the kernel logs the addresses it enters the interpreter at, with
    // a cached game library loaded (D3)
    build::EntryLog entry_log;
    // the copy-and-patch tier's code (stencil.h): the executable's, its table
    // as the kernel is given it, and the DLLs' made as they load; kept until
    // the kernel is gone, also after the tier-up (frames may still run there)
    std::unique_ptr<stencil::Module> stencil;
    const ppc::GeneratedModule *stencil_table[1] = {};
    std::vector<std::unique_ptr<stencil::Module>> stencil_dlls;
    bool stencil_counting = false;
    // whether the run moves to the game library when the cache gets one (poll)
    bool tier_up = false;
    double tier_up_polled = -1e9;                      // when the cache was last looked at (seconds, steady clock)
    const ppc::GameLibrary *tier_up_library = nullptr; // loaded, waiting for a pause
    // Hot functions first (docs/runtime.md): the tier's heat written for the
    // background build (the hot list), the partial library adopted once the
    // cache has it, and the entries counted from then
    double started = 0;                                // the tier's start (seconds, steady clock)
    double hot_list_written = -1e9;                    // the hot list's last writing
    const ppc::GameLibrary *partial_library = nullptr; // adopted (nullptr: none yet)
    uint64_t partial_tier_entries = 0;                 // the tier's entries when it was adopted
    bool partial_failed = false;                       // it did not load: not tried again
    int tier_up_failures = 0;                          // the library's loads that failed
    // Releasing the first tier (docs/runtime.md): after the tier-up, the code
    // no frame is in released at pauses, a few times
    int release_tries = 0;
    double release_polled = -1e9;
    size_t released_bytes = 0;
};

namespace {

double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The game library the recompile step cached for this executable (or the
// one named), its modules given to the kernel as a linked game's are. False
// when there is none or it does not load (said why).
bool load_game_library(const Options &options, Game &g) {
    Tiers &t = *g.tiers;
    std::filesystem::path library = options.game_library;
    const char *digest = g.cache_key.c_str();
    if (library.empty()) {
        const std::filesystem::path cache = options.cache.empty() ? build::default_cache_dir() : options.cache;
        if (auto cached = build::find_cached(cache, digest)) library = cached->library;
        if (library.empty()) {
            std::printf("generated code: no game library for %s in %s\n", digest, cache.string().c_str());
            return false;
        }
    }
    // the runtime's table, kept for the process's life with the library
    static const ppc::Runtime runtime = ppc::host_runtime();
    std::string error;
    const ppc::GameLibrary *loaded = ppc::load_game_library(library, runtime, error);
    if (!loaded) {
        std::printf("generated code: the game library did not load (%s)\n", error.c_str());
        return false;
    }
    size_t functions = 0;
    for (size_t i = 0; i < loaded->count; i++) functions += loaded->modules[i]->count;
    g.kernel->set_generated_modules(loaded->modules, loaded->count);
    g.generated_modules = loaded->count, g.generated_functions = functions, g.installed_modules = loaded->modules;
    g.tier = Game::Tier::Library;
    std::printf("generated code: %zu modules, %zu functions (the game library %s)\n", loaded->count, functions,
                library.string().c_str());
    // what discovery missed, logged beside the library for its next build
    // (D3): the first entry into the interpreter at each address
    const std::filesystem::path log = build::entry_log_path(library.parent_path(), digest);
    if (t.entry_log.open(log)) {
        g.kernel->set_entry_hook(&build::EntryLog::hook, &t.entry_log);
        std::printf("entry log: %s (%zu addresses)\n", log.string().c_str(), t.entry_log.size());
    }
    return true;
}

// A module's code made by the copy-and-patch tier: its image read, discovered
// (with the entry log's addresses, as the library's build takes them, and the
// project file's starts, tables and hooks) and copied. nullptr, and why.
std::unique_ptr<stencil::Module> make_stencil_module(const Xex &xex, const std::string &name,
                                                     const std::vector<uint32_t> &entries,
                                                     const recomp::ProjectModule *project, bool count,
                                                     stencil::Stats &s, double &discovered, uint64_t &peak_discovery,
                                                     std::string &error) {
    const auto t0 = std::chrono::steady_clock::now();
    recomp::Image image;
    if (!recomp::read_image(xex, image, error)) return nullptr;
    recomp::DiscoverOptions options;
    options.entries = entries;
    if (project) stencil::project_discovery(*project, options);
    const recomp::Discovery d = recomp::discover(image, options);
    stencil::Project hooks;
    if (project) hooks = stencil::project_hooks(*project, d);
    discovered = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    peak_discovery = process_bytes();
    static const ppc::Runtime runtime = ppc::host_runtime();
    stencil::Options o;
    o.count = count;
    // XF_STENCIL_MAP=1: each stencil's copies kept, so a profile names them
    const char *map = std::getenv("XF_STENCIL_MAP");
    o.map = map && *map && *map != '0';
    if (project) o.project = &hooks;
    return stencil::compile(image, d, runtime, name, image_digest(xex.image), o, s, error);
}

// The copy-and-patch tier (stencil.h): the executable's functions discovered
// and made into code now, its table given to the kernel as a game library's
// is, and each DLL's made as it loads. XF_STENCIL_COUNT=1 counts the
// instructions its code runs (the report compares them with the
// interpreter's). False when the tier is not available here.
bool load_stencil_tier(const Options &options, Game &g) {
    Tiers &t = *g.tiers;
    std::string error;
    std::string why;
    if (!stencil::available(&why)) {
        std::printf("first tier (copy-and-patch): not available (%s)\n", why.c_str());
        return false;
    }
    const uint64_t before = process_bytes();
    // the addresses earlier runs entered the interpreter at, as the library's build reads them
    const std::filesystem::path log = build::entry_log_path(g.cache, g.cache_key);
    const std::vector<uint32_t> entries = build::read_entry_log(log);
    static recomp::Project project;
    const recomp::ProjectModule *pm = nullptr;
    if (!options.project.empty()) {
        if (!recomp::load_project(options.project, project, error)) {
            std::printf("first tier (copy-and-patch): the project file: %s\n", error.c_str());
            return false;
        }
        pm = project.find(options.xex_path);
    }
    const bool count = [] {
        const char *c = std::getenv("XF_STENCIL_COUNT");
        return c && *c && *c != '0';
    }();
    stencil::Stats s;
    double discovered = 0;
    uint64_t peak_discovery = 0;
    t.stencil = make_stencil_module(g.xex, options.xex_path, entries, pm, count, s, discovered, peak_discovery, error);
    if (!t.stencil) {
        std::printf("first tier (copy-and-patch): %s\n", error.c_str());
        return false;
    }
    const uint64_t peak = process_bytes();
    t.stencil_counting = count;
    t.started = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    t.stencil_table[0] = t.stencil->table();
    g.kernel->set_generated_modules(t.stencil_table, 1);
    g.generated_modules = 1, g.generated_functions = s.compiled, g.installed_modules = t.stencil_table;
    g.tier = Game::Tier::Stencil;
    std::printf("first tier (copy-and-patch): %zu of %zu functions (%llu of %llu instructions; %zu with checked "
                "returns, %zu with resume sites, %zu hooked, %zu mid-function hooks), %llu stencils copied, %.1f MB of "
                "code (%.1f MB in all, %zu unwind entries); discovery %.3f s (%zu logged entries), the copies %.3f s; "
                "the process's memory %+.1f MB after discovery, %+.1f MB after the copies%s\n",
                s.compiled, s.functions, (unsigned long long)s.compiled_instructions,
                (unsigned long long)s.instructions, s.checked, s.resumable, s.hooked, s.mid_hooks,
                (unsigned long long)s.copies, s.code_bytes / 1048576.0, s.region_bytes / 1048576.0, s.unwind_entries,
                discovered, entries.size(), s.seconds, (double(peak_discovery) - double(before)) / 1048576.0,
                (double(peak) - double(before)) / 1048576.0, count ? " (counting instructions)" : "");
    for (const auto &[reason, n] : s.left_out) std::printf("  left to the interpreter: %zu %s\n", n, reason.c_str());
    // the DLLs it loads, made into code as they load (a library would carry them)
    Tiers *tp = &t;
    g.kernel->set_code_provider([tp](const Xex &xex, std::string &why) -> const ppc::GeneratedModule * {
        stencil::Stats ds;
        double seconds = 0;
        uint64_t peak = 0;
        const std::string name = xex.original_pe_name.empty() ? "a DLL" : xex.original_pe_name;
        auto m = make_stencil_module(xex, name, {}, nullptr, false, ds, seconds, peak, why);
        if (!m) return nullptr;
        std::printf("first tier (copy-and-patch): %s at 0x%08X: %zu of %zu functions, discovery %.3f s, the copies "
                    "%.3f s\n",
                    name.c_str(), xex.base_address, ds.compiled, ds.functions, seconds, ds.seconds);
        std::fflush(stdout);
        tp->stencil_dlls.push_back(std::move(m));
        return tp->stencil_dlls.back()->table();
    });
    // what discovery missed, logged where the library's build reads it
    if (t.entry_log.open(log)) {
        g.kernel->set_entry_hook(&build::EntryLog::hook, &t.entry_log);
        std::printf("entry log: %s (%zu addresses)\n", log.string().c_str(), t.entry_log.size());
    }
    return true;
}

// The tier's modules (the executable's, then the DLLs').
std::vector<stencil::Module *> tier_modules(Game &g) {
    Tiers &t = *g.tiers;
    std::vector<stencil::Module *> out;
    if (t.stencil) out.push_back(t.stencil.get());
    for (auto &d : t.stencil_dlls) out.push_back(d.get());
    return out;
}

// The entries the tier's code took (not the library's), and the calls its
// entries passed to library code (Module::heat).
std::pair<uint64_t, uint64_t> tier_entries(const stencil::Module &m) {
    uint64_t tier = 0, library = 0;
    for (const stencil::FunctionHeat &h : m.heat()) tier += h.entries, library += h.library_calls;
    return {tier, library};
}

// The share of the entries since the partial library was adopted that its
// code served: its calls against the entries the tier's own code took since.
std::string partial_share(Game &g) {
    Tiers &t = *g.tiers;
    if (!t.partial_library || !t.stencil) return "";
    const auto [tier, library] = tier_entries(*t.stencil);
    const uint64_t since = tier - std::min(tier, t.partial_tier_entries);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "%.1f%% of the function entries since the partial library came went to its code (%llu of %llu)",
                  library + since ? 100.0 * double(library) / double(library + since) : 0.0,
                  (unsigned long long)library, (unsigned long long)(library + since));
    return line;
}

// The executable's heat written where the background build reads it.
void write_hot_list(Game &g) {
    Tiers &t = *g.tiers;
    if (!t.stencil || g.cache_key.empty()) return;
    std::vector<build::HotFunction> hot;
    for (const stencil::FunctionHeat &h : t.stencil->heat())
        if (h.entries + h.back_edges && !h.adopted) hot.push_back({h.address, h.entries, h.back_edges, h.instructions});
    if (!hot.empty()) build::write_hot_list(build::hot_list_path(g.cache, g.cache_key), hot);
}

// A partial library (the hot functions, built first) adopted by the tier's
// executable code alone: the kernel keeps the tier's tables, whose entries
// now jump to the partial library's functions. True once adopted.
bool adopt_partial(Game &g) {
    Tiers &t = *g.tiers;
    auto partial = build::find_partial(g.cache, g.cache_key);
    if (!partial) return false;
    static const ppc::Runtime runtime = ppc::host_runtime();
    std::string error;
    const ppc::GameLibrary *lib = ppc::load_game_library(*partial, runtime, error);
    if (!lib) {
        std::printf("tier-up: the partial library %s did not load (%s)\n", partial->string().c_str(), error.c_str());
        t.partial_failed = true;
        return false;
    }
    kernel::Kernel &k = *g.kernel;
    const auto t0 = std::chrono::steady_clock::now();
    if (!k.pause(500)) {
        k.resume();
        std::printf("tier-up: %u guest threads did not park in 0.5 s; trying again\n", k.pause_unparked());
        return false;
    }
    size_t moved = 0;
    for (size_t i = 0; i < lib->count; i++) {
        const ppc::GeneratedModule &m = *lib->modules[i];
        const ppc::GeneratedModule &table = *t.stencil->table();
        if (m.base != table.base || m.digest != table.digest) continue;
        moved += t.stencil->adopt(m, error);
    }
    t.partial_tier_entries = tier_entries(*t.stencil).first;
    k.resume();
    t.partial_library = lib;
    std::printf("tier-up: the partial library %s (the hot functions) runs %zu of the tier's functions now; the "
                "threads paused %.1f ms\n",
                partial->string().c_str(), moved,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000.0);
    std::fflush(stdout);
    return true;
}

// After the tier-up: the tier's code no frame is in released (Module::release),
// at a pause, from the guest threads' host stacks as they wait.
void release_tier(Game &g) {
    Tiers &t = *g.tiers;
    kernel::Kernel &k = *g.kernel;
    const auto t0 = std::chrono::steady_clock::now();
    if (!k.pause(500)) {
        k.resume();
        return;
    }
    size_t bytes = 0, held = 0;
    stencil::ReleaseStats all;
    bool scanned = true;
    std::string error;
    for (stencil::Module *m : tier_modules(g)) {
        const auto [low, high] = m->code_range();
        std::vector<uintptr_t> live;
        if (!k.scan_host_stacks(low, high, live)) {
            scanned = false;
            break;
        }
        stencil::ReleaseStats s;
        bytes += m->release(live, s, error);
        all.released += s.released, all.live += s.live, all.not_adopted += s.not_adopted;
        all.unwind_dropped += s.unwind_dropped;
        held += m->committed_bytes();
    }
    k.resume();
    t.released_bytes += bytes;
    if (!scanned) {
        std::printf("tier release: a host thread runs guest code (its stack is not scanned); later\n");
        return;
    }
    std::printf("tier release: %zu functions' code released (%.1f MB; %.1f MB in all), %zu kept with a frame in them, "
                "%zu the library lacks, %zu unwind entries dropped; the tier holds %.1f MB now; the threads paused "
                "%.1f ms%s%s\n",
                all.released, bytes / 1048576.0, t.released_bytes / 1048576.0, all.live, all.not_adopted,
                all.unwind_dropped, held / 1048576.0,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000.0,
                error.empty() ? "" : "; ", error.c_str());
    std::fflush(stdout);
}

bool poll(Game &g) {
    Tiers &t = *g.tiers;
    const double now = steady_seconds();
    if (now - t.tier_up_polled < 2.0) return false;
    t.tier_up_polled = now;
    kernel::Kernel &k = *g.kernel;
    if (g.tier == Game::Tier::Library && t.stencil && t.release_tries < 10 && now - t.release_polled >= 0) {
        // the tier's code released: three times 2 s apart, then every 30 s
        release_tier(g);
        t.release_polled = now + (++t.release_tries < 3 ? 0.0 : 28.0);
        return false;
    }
    if (g.tier != Game::Tier::Stencil || !t.tier_up) return false;
    // the hot list: after 10 s on the tier, then every 20 s
    if (!t.partial_library && now - t.started >= 10.0 && now - t.hot_list_written >= 20.0) {
        write_hot_list(g);
        t.hot_list_written = now;
    }
    if (!t.tier_up_library) {
        auto cached = build::find_cached(g.cache, g.cache_key);
        if (!cached) {
            if (!t.partial_library && !t.partial_failed) adopt_partial(g);
            return false;
        }
        static const ppc::Runtime runtime = ppc::host_runtime();
        std::string error;
        t.tier_up_library = ppc::load_game_library(cached->library, runtime, error);
        if (!t.tier_up_library) {
            // (tried again twice: a file still being written or scanned is not there for good)
            const bool last = ++t.tier_up_failures >= 3;
            std::printf("tier-up: the game library %s did not load (%s)%s\n", cached->library.string().c_str(),
                        error.c_str(), last ? "; staying on the first tier" : "; trying again");
            if (last) t.tier_up = false;
            return false;
        }
        std::printf("tier-up: the game library %s is ready\n", cached->library.string().c_str());
    }
    const std::string share = partial_share(g);
    // every guest thread parked at a safe point while the entries change
    const auto t0 = std::chrono::steady_clock::now();
    if (!k.pause(500)) {
        k.resume();
        std::printf("tier-up: %u guest threads did not park in 0.5 s; trying again\n", k.pause_unparked());
        return false;
    }
    const ppc::GameLibrary &lib = *t.tier_up_library;
    auto moved = k.adopt_generated_modules(lib.modules, lib.count);
    size_t entries = 0;
    std::string error;
    for (const auto &[from, to] : moved) {
        stencil::Module *m = t.stencil && t.stencil->table() == from ? t.stencil.get() : nullptr;
        for (auto &d : t.stencil_dlls)
            if (d->table() == from) m = d.get();
        if (!m) continue;
        const size_t n = m->adopt(*to, error);
        if (!n && !error.empty()) std::printf("tier-up: %s: %s\n", from->name, error.c_str());
        entries += n;
    }
    k.resume();
    const double paused = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    size_t functions = 0;
    for (size_t i = 0; i < lib.count; i++) functions += lib.modules[i]->count;
    g.generated_modules = lib.count, g.generated_functions = functions, g.installed_modules = lib.modules;
    g.tier = Game::Tier::Library;
    t.tier_up = false;
    t.release_polled = now + 2.0;
    std::printf("tier-up: the game library runs the guest now: %zu of its modules in place of the first tier's, "
                "%zu of the tier's function entries jump to it; the threads paused %.1f ms%s%s\n",
                moved.size(), entries, paused * 1000.0, share.empty() ? "" : "; before it, ", share.c_str());
    std::fflush(stdout);
    return true;
}

// print_report's lines on the first tier (its coverage when counting, its heat) and the entry log
void report(Game &g) {
    kernel::Kernel &k = *g.kernel;
    Tiers &t = *g.tiers;
    if (t.stencil_counting && t.stencil) {
        // the copy-and-patch tier's coverage: the share of the guest's instructions its code ran
        const uint64_t run = t.stencil->executed(), interpreted = k.interpreted_instructions();
        std::printf("copy-and-patch tier: %llu instructions run in its code, %llu in the interpreter (%.2f%% "
                    "covered)\n",
                    (unsigned long long)run, (unsigned long long)interpreted,
                    run + interpreted ? 100.0 * double(run) / double(run + interpreted) : 0.0);
        // the commonest instructions in its code
        std::vector<std::pair<uint64_t, uint32_t>> by_op;
        for (uint32_t op = 0; op < ppc::OPCODE_COUNT; op++)
            if (t.stencil->executed_by_opcode()[op]) by_op.push_back({t.stencil->executed_by_opcode()[op], op});
        std::sort(by_op.rbegin(), by_op.rend());
        std::printf("  the commonest:");
        for (size_t k = 0; k < by_op.size() && k < 40; k++)
            std::printf(" %s %.2f%%", ppc::info(ppc::Opcode(by_op[k].second)).name,
                        100.0 * double(by_op[k].first) / double(run ? run : 1));
        std::printf("\n");
        // the stencils' bytes a guest instruction and the slow paths taken
        // (XF_STENCIL_PIECES=<file>: every stencil's runs written there)
        const char *pieces = std::getenv("XF_STENCIL_PIECES");
        std::printf("%s", t.stencil->count_report(15, pieces ? pieces : "").c_str());
    }
    if (t.stencil) {
        // the hot functions (the heat the background build orders by), and
        // after a tier-up what the library took and what was released
        std::vector<stencil::FunctionHeat> heat = t.stencil->heat();
        uint64_t entries = 0, back = 0, library = 0;
        size_t adopted = 0, released = 0;
        for (const stencil::FunctionHeat &h : heat) {
            entries += h.entries, back += h.back_edges, library += h.library_calls;
            adopted += h.adopted, released += h.released;
        }
        std::printf("first tier: %llu function entries and %llu back edges in its code, %llu calls to library code "
                    "from its entries; %zu of %zu functions adopted, %zu released (%.1f MB)\n",
                    (unsigned long long)entries, (unsigned long long)back, (unsigned long long)library, adopted,
                    heat.size(), released, t.released_bytes / 1048576.0);
        if (const std::string share = partial_share(g); !share.empty())
            std::printf("  hot functions first: %s\n", share.c_str());
    }
    if (t.entry_log.size())
        std::printf("entry log: %zu addresses, %zu new this run\n", t.entry_log.size(), t.entry_log.added());
}

// open()'s part for a game with no code linked in: the cached game library,
// else the first tier (moving to the library when the cache gets it), else
// the interpreter. XF_TIER says otherwise (stencil.h's Choice).
void open_tiers(const Options &options, Game &g) {
    g.tiers = std::make_shared<Tiers>();
    const stencil::Choice choice = stencil::choice();
    if (choice == stencil::Choice::Stencil) {
        load_stencil_tier(options, g);
    } else if (choice == stencil::Choice::Interpret) {
        std::printf("generated code: none (XF_TIER=interpret)\n");
    } else if (!load_game_library(options, g) && choice == stencil::Choice::Default && options.game_library.empty()) {
        g.tiers->tier_up = load_stencil_tier(options, g);
    }
}

bool has_code(const Game &g) { return g.tiers->stencil != nullptr; }

bool describe(Game &g, uint64_t address, std::string &name, uint64_t &displacement) {
    Tiers &t = *g.tiers;
    if (t.stencil && t.stencil->describe(address, name, displacement)) return true;
    for (auto &d : t.stencil_dlls)
        if (d->describe(address, name, displacement)) return true;
    return false;
}

const TierSupport support{open_tiers, poll, report, has_code, describe};

} // namespace

void enable_tiers() { set_tier_support(&support); }

} // namespace xf::game
