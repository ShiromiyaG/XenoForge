// What boot and the player share (game.h).

#include "game.h"

#include "xenoforge/command_processor.h"
#include "xenoforge/disc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef XF_GENERATED
// the game library's tables, one per module (xfrecomp emit's modules.cpp)
extern "C" const xf::ppc::GeneratedModule *const xf_modules[];
extern "C" const size_t xf_module_count;
#endif

namespace xf::game {
namespace {

// for the fault report
Game *the_game = nullptr;

bool load(const std::string &input, const std::string &xex_path, Xex &xex, std::string &error) {
    std::vector<uint8_t> file;
    if (input.ends_with(".xex") || input.ends_with(".XEX")) {
        std::ifstream f(input, std::ios::binary);
        if (!f) return error = "cannot read " + input, false;
        file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } else {
        auto disc = Disc::open(input, error);
        if (!disc) return false;
        const DiscEntry *entry = disc->find(xex_path);
        if (!entry) return error = "no " + xex_path + " on the disc", false;
        file = disc->read_all(*entry);
    }
    return parse_xex(file, xex, error) && load_xex_image(file, xex, error);
}

#ifdef _WIN32
// An access fault in the guest's code: where, in guest terms, then the report.
LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t at = e->ExceptionRecord->ExceptionInformation[1];
    uint64_t offset = the_game ? at - uint64_t(the_game->memory->base()) : ~0ull;
    std::printf("fault: %s host 0x%llX", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)at);
    if (offset < (1ull << 33)) std::printf(" (guest 0x%08llX)", (unsigned long long)offset);
    // the faulting thread's own context (the handler runs on it)
    if (ppc::Context *c = kernel::Kernel::current_context())
        std::printf(", its guest thread last at 0x%08X, lr 0x%08X", c->pc, uint32_t(c->lr));
    std::printf("\n");
    // the renderer's report only if no call of its is under way (this may be its thread)
    if (the_game) print_report(*the_game, false);
    std::fflush(stdout);
    ExitProcess(3);
}
#endif

// the renderer's counts: draws drawn and skipped by reason, what is
// approximate, resolves, swaps, the time it took
void print_renderer_report(const render::Report &r) {
    std::printf("renderer: %llu draws (%llu drawn), %llu resolves, %llu swaps (%llu front buffers written)\n",
                (unsigned long long)r.draws, (unsigned long long)r.drawn, (unsigned long long)r.resolves,
                (unsigned long long)r.swaps, (unsigned long long)r.frames);
    std::printf("renderer: %.1f s spent: %.1f s in submissions (%.1f s waiting for the GPU), %.1f s in resolves, "
                "%.1f s checking textures, %.1f s decoding them, %.1f s copying %.1f MB of vertex data, %.1f s "
                "copying %.1f MB that resolves wrote into memory\n",
                r.seconds, r.submit_seconds, r.wait_seconds, r.resolve_seconds, r.check_seconds, r.decode_seconds,
                r.vertex_seconds, double(r.vertex_bytes) / (1 << 20), r.writeback_seconds,
                double(r.writeback_bytes) / (1 << 20));
    std::printf("renderer: %llu submissions, %llu waits for the GPU\n", (unsigned long long)r.submissions,
                (unsigned long long)r.waits);
    std::printf("renderer: made %llu render targets, %llu textures, %llu pipelines, %llu shaders\n",
                (unsigned long long)r.render_targets, (unsigned long long)r.textures, (unsigned long long)r.pipelines,
                (unsigned long long)r.shaders);
    std::printf("renderer: %.2f s making pipelines; the pipeline cache: %.1f KB read at the start, %.1f KB written\n",
                r.pipeline_seconds, r.pipeline_cache_read / 1024.0, r.pipeline_cache_written / 1024.0);
    for (const auto &[why, n] : r.skipped) {
        std::printf("  skipped %8llu  %s", (unsigned long long)n, why.c_str());
        auto at = r.skipped_at.find(why);
        if (at != r.skipped_at.end() && !at->second.empty()) {
            std::printf(" (frames");
            for (uint64_t swap : at->second) std::printf(" %llu", (unsigned long long)swap);
            std::printf("%s)", n > at->second.size() ? " ..." : "");
        }
        std::printf("\n");
    }
    for (const auto &[what, n] : r.approximate)
        std::printf("  approx  %8llu  %s\n", (unsigned long long)n, what.c_str());
}

} // namespace

bool open(const Options &options, Game &g, std::string &error) {
    const std::string &input = options.input;
    if (!load(input, options.xex_path, g.xex, error)) return false;
    g.memory = Memory::create(error);
    if (!g.memory) return error = "memory: " + error, false;
    g.kernel = std::make_unique<kernel::Kernel>(*g.memory);
    kernel::Kernel &k = *g.kernel;
    bool bare = input.ends_with(".xex") || input.ends_with(".XEX");
    bool mounted = bare ? kernel::mount_directory(k, std::filesystem::path(input).parent_path(), error)
                        : kernel::mount_disc(k, input, error);
    if (!mounted || !kernel::mount_storage(k, options.storage, error)) return error = "mount: " + error, false;
    std::printf("storage: %s\n", options.storage.string().c_str());
    // its module's path, which the DLLs it loads by a bare name are next to
    kernel::set_executable_path(k, "game:\\" +
                                       (bare ? std::filesystem::path(input).filename().string() : options.xex_path));
#ifdef XF_GENERATED
    // each module's code runs once an image it was made from is loaded
    // (the executable's in load(), a DLL's as XexLoadImage loads it)
    size_t functions = 0;
    for (size_t i = 0; i < xf_module_count; i++) functions += xf_modules[i]->count;
    if (!options.interpret) k.set_generated_modules(xf_modules, xf_module_count);
    std::printf("generated code: %zu modules, %zu functions%s\n", size_t(xf_module_count), functions,
                options.interpret ? " (left out: --interpret)" : "");
#endif
    if (!k.load(g.xex, error)) return error = "load: " + error, false;
    k.step_limit = options.steps;
    std::printf("loaded %s: base 0x%08X, entry 0x%08X, %zu imported functions\n", options.xex_path.c_str(),
                g.xex.base_address, k.entry_point(), k.imports().size());
    std::printf("stack 0x%X; TLS: ", g.xex.stack_size);
    if (g.xex.has_tls)
        std::printf("%u slots, %u bytes of data (0x%X from 0x%08X)\n", g.xex.tls.slot_count, g.xex.tls.data_size,
                    g.xex.tls.raw_data_size, g.xex.tls.raw_data_address);
    else
        std::printf("none\n");
    return true;
}

bool start_renderer(Game &g, const render::Options &options, std::string &error) {
    if (!g.device) g.device = vk::Device::open(error);
    if (!g.device) return error = "Vulkan: " + error, false;
    gpu::CommandProcessor &processor = kernel::gpu_command_processor(*g.kernel);
    // the game's pipeline cache: a temporary directory's xenoforge-cache
    // unless the caller names one (the player: beside its config.toml)
    render::Options o = options;
    if (o.pipeline_cache.empty()) {
        std::error_code ec;
        char name[32];
        std::snprintf(name, sizeof(name), "%08X.vkpipelines", g.xex.execution.title_id);
        o.pipeline_cache = std::filesystem::temp_directory_path(ec) / "xenoforge-cache" / name;
    }
    g.renderer = render::Renderer::create(*g.device, processor, o, error);
    if (!g.renderer) return error = "renderer: " + error, false;
    processor.set_sink(g.renderer.get());
    // frames traced as XF_TRACE_FRAMES and XF_TRACE_DIR say (the player's
    // way to them; boot has --trace-frames)
    if (const char *frames = std::getenv("XF_TRACE_FRAMES")) {
        std::set<uint64_t> numbers;
        if (!parse_numbers(frames, numbers)) return error = "XF_TRACE_FRAMES: not a list of frames", false;
        const char *dir = std::getenv("XF_TRACE_DIR");
        if (!start_trace(g, numbers, dir ? dir : "traces", error)) return false;
    }
    return true;
}

bool start_trace(Game &g, const std::set<uint64_t> &frames, const std::filesystem::path &directory,
                 std::string &error) {
    if (!g.renderer) return error = "tracing frames needs the renderer", false;
    gpu::CommandProcessor &processor = kernel::gpu_command_processor(*g.kernel);
    gpu::TraceOptions options;
    options.directory = directory;
    options.frames = frames;
    options.title_id = g.xex.execution.title_id;
    g.trace = std::make_unique<gpu::TraceWriter>(processor, g.renderer.get(), g.renderer.get(), options);
    processor.set_sink(g.trace.get());
    std::printf("tracing %zu frames into %s\n", frames.size(), directory.string().c_str());
    return true;
}

bool launch(Game &g, std::string &error) {
    the_game = &g;
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_fault);
#endif
    return g.kernel->launch(error);
}

void print_report(Game &g, bool waiting) {
    kernel::Kernel &k = *g.kernel;
    std::printf("%s", kernel::modules_report(k).c_str());
    // where the interpreter was entered: with generated code, what
    // discovery missed (the most entered first)
    auto targets = k.interpreted_targets();
    std::vector<std::pair<uint64_t, uint32_t>> entered;
    for (const auto &[address, n] : targets) entered.push_back({n, address});
    std::sort(entered.rbegin(), entered.rend());
    std::printf("interpreted entries: %llu, at %zu addresses\n", (unsigned long long)k.interpreted_entries(),
                entered.size());
    for (size_t i = 0; i < entered.size() && i < 20; i++)
        std::printf("  %10llu  0x%08X  %s\n", (unsigned long long)entered[i].first, entered[i].second,
                    kernel::module_at(k, entered[i].second).c_str());
    // fiber switches, longjmps, catches: what took them
    std::printf("%s", kernel::stacks_report(k).c_str());
    std::vector<std::pair<uint64_t, std::string>> calls;
    for (const auto &[name, n] : k.calls()) calls.push_back({n, name});
    std::sort(calls.rbegin(), calls.rend());
    std::printf("kernel calls (%zu exports):\n", calls.size());
    auto missing = k.unimplemented();
    for (const auto &[n, name] : calls)
        std::printf("  %8llu  %s%s\n", (unsigned long long)n, name.c_str(),
                    missing.count(name) ? "  (not implemented)" : "");
    // what the GPU did: packets, draws, resolves, swaps, interrupts
    std::printf("%s", kernel::video_report(k).c_str());
    // the APU: render driver frames, XMA contexts
    std::printf("%s", kernel::audio_report(k).c_str());
    // what the input exports answered
    std::printf("%s", kernel::input_report(k).c_str());
    if (g.renderer) {
        render::Report r;
        if (waiting) {
            g.renderer->drain(); // the last front buffers asked for, to on_frame
            r = g.renderer->report();
        }
        if (waiting || g.renderer->try_report(r)) {
            print_renderer_report(r);
            std::string failure;
            if (g.renderer->failed(failure)) std::printf("renderer: failed: %s\n", failure.c_str());
        } else {
            std::printf("renderer: busy (the fault came during one of its calls)\n");
        }
    }
    if (g.trace) {
        for (const auto &path : g.trace->finished()) std::printf("trace: %s\n", path.string().c_str());
        for (const auto &e : g.trace->errors()) std::printf("trace: %s\n", e.c_str());
    }
    std::fflush(stdout);
}

void print_rates(Game &g, Rates &r, double wall, const std::string &extra) {
    const gpu::Counts &counts = kernel::gpu_command_processor(*g.kernel).counts();
    const uint64_t swaps = counts.swaps.load(), draws = counts.draws.load(), resolves = counts.resolves.load();
    const double dt = wall - r.last_wall;
    if (dt <= 0) return;
    // the renderer's time without waiting for a call of its: over the time
    // since the last report it gave (a call under way when this one asks
    // shows as "-")
    render::Report report;
    double busy = -1;
    if (g.renderer && g.renderer->try_report(report)) {
        busy = (report.seconds - r.last_busy) / (wall - r.last_busy_wall);
        r.last_busy = report.seconds, r.last_busy_wall = wall;
    }
    char renderer[48] = "";
    if (g.renderer)
        std::snprintf(renderer, sizeof(renderer), busy < 0 ? ", renderer busy -" : ", renderer busy %.0f%%",
                      busy * 100);
    std::printf("rates: %6.1f s (guest %6.1f s): %6llu swaps, %5.1f/s, %6.0f draws/s, %4.0f resolves/s%s%s\n", wall,
                g.kernel->uptime_ms() / 1000.0, (unsigned long long)swaps, (swaps - r.last_swaps) / dt,
                (draws - r.last_draws) / dt, (resolves - r.last_resolves) / dt, renderer, extra.c_str());
    std::fflush(stdout);
    r.last_wall = wall, r.last_swaps = swaps, r.last_draws = draws, r.last_resolves = resolves;
}

bool parse_numbers(const std::string &list, std::set<uint64_t> &out) {
    size_t at = 0;
    while (at < list.size()) {
        size_t comma = list.find(',', at);
        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? list.size() : comma + 1;
        if (item.empty()) continue;
        char *end = nullptr;
        uint64_t first = std::strtoull(item.c_str(), &end, 10), last = first;
        if (*end == '-') last = std::strtoull(end + 1, &end, 10);
        if (*end || last < first || last - first > 100000) return false;
        for (uint64_t n = first; n <= last; n++) out.insert(n);
    }
    return true;
}

bool parse_press(const std::string &text, Press &out, std::string &error) {
    static const std::map<std::string, uint16_t> buttons = {
        {"a", input::A},
        {"b", input::B},
        {"x", input::X},
        {"y", input::Y},
        {"start", input::START},
        {"back", input::BACK},
        {"guide", input::GUIDE},
        {"lb", input::LEFT_SHOULDER},
        {"rb", input::RIGHT_SHOULDER},
        {"ls", input::LEFT_THUMB},
        {"rs", input::RIGHT_THUMB},
        {"up", input::DPAD_UP},
        {"down", input::DPAD_DOWN},
        {"left", input::DPAD_LEFT},
        {"right", input::DPAD_RIGHT},
    };
    out = Press{};
    size_t at = text.find('@');
    if (at == std::string::npos) return error = "--press " + text + ": no @<seconds>", false;
    std::string names = text.substr(0, at), when = text.substr(at + 1);
    for (size_t from = 0; from <= names.size();) {
        size_t plus = names.find('+', from);
        std::string name = names.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
        for (char &ch : name) ch = char(std::tolower(uint8_t(ch)));
        if (name == "lt")
            out.pad.left_trigger = 0xFF;
        else if (name == "rt")
            out.pad.right_trigger = 0xFF;
        else if (auto it = buttons.find(name); it != buttons.end())
            out.pad.buttons |= it->second;
        else
            return error = "--press " + text + ": no button " + name, false;
        if (plus == std::string::npos) break;
        from = plus + 1;
    }
    double seconds = 0, hold = 0.3;
    char *end = nullptr;
    const bool at_swap = !when.empty() && when[0] == '#';
    const char *start = when.c_str() + (at_swap ? 1 : 0);
    seconds = std::strtod(start, &end);
    if (end == start || seconds < 0) return error = "--press " + text + ": no time", false;
    if (at_swap)
        out.from_swap = uint64_t(seconds);
    else if (*end == 's')
        end++;
    if (*end == ':') {
        const char *h = end + 1;
        hold = std::strtod(h, &end);
        if (end == h || hold <= 0) return error = "--press " + text + ": bad hold", false;
        if (*end == 's') end++;
    }
    if (*end) return error = "--press " + text + ": trailing " + end, false;
    if (!at_swap) out.from_ms = uint64_t(std::llround(seconds * 1000));
    out.hold_ms = uint64_t(std::llround(hold * 1000));
    return true;
}

input::Overlay presses_overlay(std::vector<Press> presses, std::function<uint64_t()> swaps) {
    if (presses.empty()) return nullptr;
    // a press at a swap starts when the swap is first seen reached
    struct State {
        std::mutex mutex;
        std::vector<Press> presses;
        std::function<uint64_t()> swaps;
    };
    auto state = std::make_shared<State>();
    state->presses = std::move(presses), state->swaps = std::move(swaps);
    return [state](uint32_t user, uint64_t now_ms) {
        input::Gamepad g;
        if (user != 0) return g;
        std::lock_guard<std::mutex> lock(state->mutex);
        const uint64_t swaps = state->swaps ? state->swaps() : 0;
        for (Press &p : state->presses) {
            if (p.from_swap != UINT64_MAX) {
                if (swaps < p.from_swap) continue;
                p.from_ms = now_ms, p.from_swap = UINT64_MAX;
            }
            if (now_ms >= p.from_ms && now_ms < p.from_ms + p.hold_ms) g = input::combine(g, p.pad);
        }
        return g;
    };
}

} // namespace xf::game
