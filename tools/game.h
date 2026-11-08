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
#include "xenoforge/port_module.h"
#include "xenoforge/renderer.h"
#include "xenoforge/vk_device.h"
#include "xenoforge/xex.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace xf::game {

struct Options {
    std::string input;                    // a disc image, or a bare default.xex
    std::string xex_path = "default.xex"; // the executable on the disc
    std::filesystem::path storage;        // the storage root (content/, cache*/)
    kernel::SystemSettings system;        // the console's language, country and video the player chose
    uint64_t steps = 2'000'000'000ull;    // the interpreter's instructions per entry
    bool interpret = false;               // the generated code left out (linked, or a game library)
    // Generated code linked into the executable (xfrecomp emit's modules.cpp: a
    // table per module), when there is any; else the game library below.
    const ppc::GeneratedModule *const *modules = nullptr;
    size_t module_count = 0;
    // Built without generated code (the generic boot and player): the game
    // library the recompile step cached for the executable (game_build.h),
    // from this cache (game_build.h's default_cache_dir() when empty), or the
    // one named; with none, the game is interpreted.
    std::filesystem::path cache;
    std::filesystem::path game_library;
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
    uint64_t halt_swap = 0;
    double halt_seconds = 0;
    // Input recordings (xenoforge/input.h; docs/input.md): the answers written
    // to record_input, or given from replay_input; XF_INPUT_RECORD and
    // XF_INPUT_REPLAY when empty.
    std::filesystem::path record_input, replay_input;
};

struct Game {
    // where the kernel logs the addresses it enters the interpreter at, with
    // a cached game library loaded (D3; first, so it outlives the kernel)
    build::EntryLog entry_log;
    Xex xex;
    std::unique_ptr<Memory> memory;
    std::unique_ptr<kernel::Kernel> kernel;
    std::unique_ptr<vk::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<gpu::TraceWriter> trace; // in front of the renderer, when frames are traced
    size_t generated_modules = 0, generated_functions = 0; // the generated code installed (none: interpreted)
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
};
void print_rates(Game &game, Rates &rates, double wall_seconds, const std::string &extra = {});

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
