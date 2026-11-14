// What boot and the player share (docs/runtime.md, "boot" and "The
// player"): a game opened (the disc or a bare executable read, the memory,
// the kernel, the file system and the storage root, the executable loaded,
// its generated code linked in), the renderer installed as the command
// processor's Sink, the main thread launched with the fault report in place,
// the report of what the kernel, the GPU, the sound and the input did, and
// the scripted presses (--press) both take.
//
// A port or game linked with its generated code gives the tables (one per
// module) in Options; the generic boot and player load the game library
// cached for the disc (game_library.h) instead.

#pragma once

#include "xenoforge/game_build.h"
#include "xenoforge/input.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/patches.h"
#include "xenoforge/port_module.h"
#include "xenoforge/renderer.h"
#include "xenoforge/stencil.h"
#include "xenoforge/vk_device.h"
#include "xenoforge/xex.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace xf::ppc {
struct GameLibrary;
}

namespace xf::game {

struct Options {
    std::string input;                    // a disc image, or a bare default.xex
    std::string xex_path = "default.xex"; // the executable on the disc
    std::filesystem::path storage;        // the storage root (content/, cache*/)
    kernel::SystemSettings system;        // the console's language, country and video the player chose
    kernel::ContentFormat content_format = kernel::ContentFormat::Stfs; // how new saves are kept
    uint64_t steps = 2'000'000'000ull;    // the interpreter's instructions per entry
    bool interpret = false;               // the generated code left out (linked, or a game library)
    // Generated code linked into the executable (xfrecomp emit's modules.cpp: a
    // table per module), when there is any; else the game library below.
    const ppc::GeneratedModule *const *modules = nullptr;
    size_t module_count = 0;
    // Built without generated code (the generic boot and player): the game
    // library the recompile step cached for the executable (game_build.h),
    // from this cache (game_build.h's default_cache_dir() when empty), or the
    // one named; with none (or one for another interface or set of patches),
    // the copy-and-patch tier (stencil.h; docs/runtime.md, "The first tier"),
    // moving to the library when one appears (poll_tier_up), and the
    // interpreter where the tier is not available. XF_TIER says otherwise
    // (stencil.h's Choice).
    std::filesystem::path cache;
    std::filesystem::path game_library;
    // A port's project file (recomp.h, "The project file"), for the tier: its
    // starts, tables, hook slots and mid-function hooks in the executable's
    // code, as a library emitted with it has them (boot --project).
    std::filesystem::path project;
    // The lockstep check (xenoforge/lockstep.h): the interpreter checks the
    // generated code of the calls to these functions and of one call in
    // lockstep_sample to any; XF_LOCKSTEP and XF_LOCKSTEP_SAMPLE when neither
    // is given (the player's way). The report gives the counts.
    std::vector<uint32_t> lockstep;
    uint32_t lockstep_sample = 0;
    // The deterministic mode (xenoforge/kernel.h, Deterministic; docs/runtime.md):
    // 0 off, 1 as fast as the host goes, 2 paced to the host's clock. When 0,
    // XF_DETERMINISTIC says (1, or "paced"). It halts at the first hand-over
    // after swap halt_swap, or after halt_seconds of its clock (0: never).
    int deterministic = 0;
    // Deterministic::spin_idle (boot --spin-idle): a long spin with nothing
    // else ready waits as an idle thread does, for the generated code's
    // benchmark
    bool spin_idle = false;
    uint64_t halt_swap = 0;
    double halt_seconds = 0;
    // Input recordings (xenoforge/input.h; docs/input.md): the answers written
    // to record_input, or given from replay_input; XF_INPUT_RECORD and
    // XF_INPUT_REPLAY when empty.
    std::filesystem::path record_input, replay_input;
    // Game patches (patches.h): the folder read and what the user turned on or off; code patches
    // are written into the executable's image before anything else reads it, data patches into
    // the guest's memory once it is loaded.
    patches::Config patches;
    // the choices for the executable's title, asked once its title ID is known (the player reads the
    // game's own settings here); they replace patches.enabled when given
    std::function<std::map<std::string, bool>(uint32_t title_id)> patch_choices;
};

struct Game {
    // where the kernel logs the addresses it enters the interpreter at, with
    // a cached game library loaded (D3; first, so it outlives the kernel)
    build::EntryLog entry_log;
    // the copy-and-patch tier's code (stencil.h): the executable's, its table
    // as the kernel is given it, and the DLLs' made as they load; kept until
    // the kernel is gone, also after the tier-up (frames may still run there)
    std::unique_ptr<stencil::Module> stencil;
    const ppc::GeneratedModule *stencil_table[1] = {};
    std::vector<std::unique_ptr<stencil::Module>> stencil_dlls;
    bool stencil_counting = false;
    // which code runs the guest (tier_name), and whether the run moves to the
    // game library when the cache gets one (poll_tier_up)
    enum class Tier { Interpreter, Linked, Library, Stencil } tier = Tier::Interpreter;
    bool tier_up = false;
    double tier_up_polled = -1e9; // when the cache was last looked at (seconds, steady clock)
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
    Xex xex;
    // the executable's image digest before any patch, and the game library's cache key: the digest
    // alone, or with the enabled code patches' key (patches::cache_key)
    uint64_t base_digest = 0;
    std::string cache_key;
    patches::Plan patch_plan; // the enabled patches that apply to the executable
    std::unique_ptr<Memory> memory;
    std::unique_ptr<kernel::Kernel> kernel;
    std::unique_ptr<vk::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<gpu::TraceWriter> trace; // in front of the renderer, when frames are traced
    size_t generated_modules = 0, generated_functions = 0; // the generated code installed (none: interpreted)
    const ppc::GeneratedModule *const *installed_modules = nullptr; // its tables (linked, or the game library's)
    // the cache (Options::cache, else default_cache_dir()): the title's
    // pipeline cache and shader storage are kept there by default
    std::filesystem::path cache;
    // the port's hooks as a module loaded at run time (port_module.h), when it has one
    std::unique_ptr<kernel::PortModule> port_module;
};

// The game read, its memory and kernel made, the disc (or the executable's
// directory) and the storage root mounted, the executable loaded and its
// generated code installed; prints what boot always printed of it (the
// storage, the generated functions, the executable's base, entry, stack and
// TLS). False, and why, when any of it fails.
bool open(const Options &options, Game &game, std::string &error);

// The renderer on a Vulkan device without a window (or on game.device when
// it is opened already: the player's, with its window), installed as the
// command processor's Sink, before the game runs. With XF_TRACE_FRAMES set
// (a list as parse_numbers reads it), those frames are traced as well, into
// XF_TRACE_DIR (./traces by default).
bool start_renderer(Game &game, const render::Options &options, std::string &error);

// Frames traced (gpu_trace_writer.h): a trace writer put in front of the
// renderer, writing <title id>_<frame>.xtr into the directory; frames
// numbered as the shots are (frame N: what the swap after the first N shows).
bool start_trace(Game &game, const std::set<uint64_t> &frames, const std::filesystem::path &directory,
                 std::string &error);

// The main thread started, with the fault report (an access fault in the
// guest's code: where, in guest terms, then the report) in place.
bool launch(Game &game, std::string &error);

// The port's hooks built as a module (xenoforge_add_port's HOT_RELOAD;
// port_module.h): loaded from `path` (a bare name: beside the executable)
// before the game runs. False, and why, when it cannot be.
bool load_port_module(Game &game, const std::filesystem::path &path, std::string &error);
// The module loaded again while the game runs when its file changed (and
// settled), or when `force`: the line saying so printed. False when it was
// not (unchanged, none, or the new file unusable: said on stderr).
bool poll_port_module(Game &game, bool force);

// Which code runs the guest: "the game library", "the first tier
// (copy-and-patch)", "linked generated code", "the interpreter".
const char *tier_name(const Game &game);
// The tier-up (docs/runtime.md, "The first tier"): on the first tier, every
// few seconds, whether the cache has the game's library now (the launcher
// builds it in the background); when it has, the library is loaded and, with
// every guest thread parked for a moment, the kernel's tables and the tier's
// function entries are moved to it. True when it happened (the line saying
// so printed). Call it from the loop that waits for the game.
bool poll_tier_up(Game &game);

// The modules, the interpreter's entries, the kernel calls, what the GPU,
// the sound and the input did, the port's hook calls (--hook-stats), and the
// renderer's counts (waiting: for a call of its under way; else skipped if
// one is).
void print_report(Game &game, bool waiting);

// One machine-readable line for the batch compatibility run (xfcompat),
// printed last: "xf-summary " and a JSON object with the title, the image
// digest, the generated code installed, the interpreter's entries, the
// swaps, the exports called without an implementation (the most called
// first), the GPU's gaps (draws skipped, approximate) and `state` (ok,
// stall, stopped, ended, crash) with `detail` (the stop reason, the fault),
// `stack` (the guest stack of a crash or stop, one line, as functions) and
// `report` (the crash report's file).
void print_summary(Game &game, const std::string &state, const std::string &detail = {},
                   const std::string &stack = {}, const std::string &report_file = {});

// Where crash and stop reports are written (crash-<time>.txt; the current
// directory by default, XF_CRASH_DIR, or boot's --crash-dir).
void set_crash_directory(const std::filesystem::path &directory);
// A stop's report (the kernel's stop_report) written there and printed, with
// the path; the path, or empty.
std::string write_stop_report(Game &game);

// --rates: one line a second of the run (the swaps, the scheduler's clock,
// the renderer's time), so runs of boot and the player can be set side by
// side over time. `extra` is appended to the line (the player's own counts).
struct Rates {
    double last_wall = 0, last_busy = 0, last_busy_wall = 0;
    uint64_t last_swaps = 0, last_draws = 0, last_resolves = 0;
    uint64_t last_texture_loads = 0; // the renderer's, at its last report
    uint64_t last_run_ns = 0, last_wait_ns = 0; // the command processor's thread
};
void print_rates(Game &game, Rates &rates, double wall_seconds, const std::string &extra = {});

// A path the guest launches (kernel::Kernel::title_exit.path) as the path on the disc --xex takes: the device
// prefix (game:, d:, \Device\Cdrom0) dropped, slashes forward; "" for the dashboard or a path that is not on the disc.
std::string disc_path(std::string path);
// The command line of the process that runs the title this one launched (boot --follow-launches, the player's
// follow_launches): `args` as this process got them (argv[0] first) without the options named in `replace` (each
// with its value), then those with a value appended with it (an empty value only drops the option). What decides which title follows is
// disc_path; how it is started (a process here, in the launcher's process on Android) is the caller's.
std::vector<std::string> relaunch_args(const std::vector<std::string> &args,
                                       const std::vector<std::pair<std::string, std::string>> &replace);

// "300,301,890-910": numbers and ranges of them
bool parse_numbers(const std::string &list, std::set<uint64_t> &out);

// Scripted presses: "start@40" or "start@40s" (START held 0.3 s from 40 s
// on the scheduler's clock, user 0), "a+b@12.5:1" (A and B held 1 s),
// "start@#1100" (from the GPU's 1100th swap: what the game shows, whatever
// the run's speed); buttons a b x y start back guide lb rb ls rs up down
// left right lt rt.
struct Press {
    input::Gamepad pad;
    uint64_t from_ms = 0, hold_ms = 300;
    uint64_t from_swap = UINT64_MAX; // set: from that swap on, not from_ms
};
bool parse_press(const std::string &text, Press &out, std::string &error);
// what the presses hold at a time, as a driver's overlay takes it (user 0);
// swaps counts the GPU's swaps, for the presses at a swap
input::Overlay presses_overlay(std::vector<Press> presses, std::function<uint64_t()> swaps = nullptr);

} // namespace xf::game
