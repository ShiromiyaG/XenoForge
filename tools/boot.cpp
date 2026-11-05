// boot: a game run without a window (plan step A4; docs/runtime.md). Loads
// the disc's executable, maps it, binds its imports, starts the main thread
// the kernel makes (Kernel::launch: stack, PCR, KTHREAD, TLS, as Xenia's
// XThread), runs the entry point (generated code where linked, else the
// interpreter) and prints what the kernel saw: the modules loaded (the
// executable, the DLLs it loaded or unloaded), every export called and how
// often, the ones with no implementation, the threads and where it stopped,
// and what the GPU did (packets, draws, resolves, swaps, interrupts).
// The disc is the game's file system (game:, d:), or a bare executable's
// directory is; --content names the writable storage root (Xenia's layout:
// content/ for saves, cache*/), a temporary directory's xenoforge by default.
//
// Built as boot_<game> (CMake's XF_GAMES), it links that game's generated
// code (xfrecomp emit) and dispatch runs it; --interpret leaves it out.
// --render draws what the GPU is sent (the renderer as the command
// processor's Sink, on a Vulkan device without a window); off by default,
// so the GPU stays headless. --shots <dir> writes the front buffer of chosen
// swaps there as <title>_<swap>.png, the swaps numbered from 0 as Xenia's
// --trace_gpu_frames numbers a trace's frame (frame N: what the swap after
// the first N shows): --shot-swaps 300,301,890-910, or --shot-every N. The
// report adds the renderer's counts.
//
//   boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] [--content <dir>] [--interpret]
//        [--render [--shots <dir> [--shot-swaps <list>] [--shot-every N]]]

#include "xenoforge/command_processor.h"
#include "xenoforge/disc.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/renderer.h"
#include "xenoforge/vk_device.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace xf;

#ifdef XF_GENERATED
// the game's generated functions by address (xfrecomp emit's table.cpp, C
// linkage; the same layout as Generated)
extern "C" const kernel::Generated xf_functions[];
extern "C" const size_t xf_function_count;
#endif

namespace {

// for the fault report: the guest memory
uint8_t *guest_base = nullptr;
kernel::Kernel *the_kernel = nullptr;
render::Renderer *the_renderer = nullptr;

void print_report(kernel::Kernel &k, bool waiting);

// "300,301,890-910": swap numbers and ranges of them
bool parse_swaps(const std::string &list, std::set<uint64_t> &out) {
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

#ifdef _WIN32
// An access fault in the guest's code: where, in guest terms, then the report.
LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t at = e->ExceptionRecord->ExceptionInformation[1];
    uint64_t offset = at - uint64_t(guest_base);
    std::printf("fault: %s host 0x%llX", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)at);
    if (offset < (1ull << 33)) std::printf(" (guest 0x%08llX)", (unsigned long long)offset);
    // the faulting thread's own context (the handler runs on it)
    if (ppc::Context *c = kernel::Kernel::current_context())
        std::printf(", its guest thread last at 0x%08X, lr 0x%08X", c->pc, uint32_t(c->lr));
    std::printf("\n");
    // the renderer's report only if no call of its is under way (this may be its thread)
    if (the_kernel) print_report(*the_kernel, false);
    std::fflush(stdout);
    ExitProcess(3);
}
#endif

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

} // namespace

int main(int argc, char **argv) {
    const char *usage = "usage: boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] "
                        "[--content <dir>] [--interpret] [--render [--shots <dir> [--shot-swaps <list>] "
                        "[--shot-every N]]]\n";
    if (argc < 2) return std::fprintf(stderr, "%s", usage), 2;
    std::string input = argv[1], xex_path = "default.xex";
    std::error_code ec;
    std::filesystem::path storage = std::filesystem::temp_directory_path(ec) / "xenoforge";
    uint64_t steps = 2'000'000'000ull;
    uint32_t seconds = 30;
    bool interpret = false; // the generated code left out, where it is linked
    bool rendering = false;
    std::filesystem::path shots;
    std::set<uint64_t> shot_swaps;
    uint64_t shot_every = 0;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--steps" && k + 1 < argc) steps = std::strtoull(argv[++k], nullptr, 10);
        else if (a == "--seconds" && k + 1 < argc) seconds = uint32_t(std::strtoul(argv[++k], nullptr, 10));
        else if (a == "--content" && k + 1 < argc) storage = argv[++k];
        else if (a == "--interpret") interpret = true;
        else if (a == "--render") rendering = true;
        else if (a == "--shots" && k + 1 < argc) shots = argv[++k];
        else if (a == "--shot-swaps" && k + 1 < argc) {
            if (!parse_swaps(argv[++k], shot_swaps)) return std::fprintf(stderr, "bad --shot-swaps\n%s", usage), 2;
        } else if (a == "--shot-every" && k + 1 < argc) shot_every = std::strtoull(argv[++k], nullptr, 10);
        else std::fprintf(stderr, "boot: %s ignored\n", a.c_str());
    }
    if (!shots.empty() && !rendering) return std::fprintf(stderr, "--shots needs --render\n"), 2;

    std::string error;
    Xex xex;
    if (!load(input, xex_path, xex, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    auto memory = Memory::create(error);
    if (!memory) return std::fprintf(stderr, "memory: %s\n", error.c_str()), 1;
    kernel::Kernel k(*memory);
    bool bare = input.ends_with(".xex") || input.ends_with(".XEX");
    bool mounted = bare ? kernel::mount_directory(k, std::filesystem::path(input).parent_path(), error)
                        : kernel::mount_disc(k, input, error);
    if (!mounted || !kernel::mount_storage(k, storage, error))
        return std::fprintf(stderr, "mount: %s\n", error.c_str()), 1;
    std::printf("storage: %s\n", storage.string().c_str());
    // its module's path, which the DLLs it loads by a bare name are next to
    kernel::set_executable_path(k, "game:\\" + (bare ? std::filesystem::path(input).filename().string() : xex_path));
    if (!k.load(xex, error)) return std::fprintf(stderr, "load: %s\n", error.c_str()), 1;
    k.step_limit = steps;
#ifdef XF_GENERATED
    // dispatch looks addresses up by binary search
    if (!std::is_sorted(xf_functions, xf_functions + xf_function_count,
                        [](const kernel::Generated &a, const kernel::Generated &b) { return a.address < b.address; }))
        return std::fprintf(stderr, "the generated functions are not in address order\n"), 1;
    if (!interpret) k.set_generated(xf_functions, xf_function_count);
    std::printf("generated code: %zu functions%s\n", size_t(xf_function_count), interpret ? " (left out: --interpret)" : "");
#else
    (void)interpret;
#endif
    std::printf("loaded %s: base 0x%08X, entry 0x%08X, %zu imported functions\n", xex_path.c_str(), xex.base_address,
                k.entry_point(), k.imports().size());
    std::printf("stack 0x%X; TLS: ", xex.stack_size);
    if (xex.has_tls)
        std::printf("%u slots, %u bytes of data (0x%X from 0x%08X)\n", xex.tls.slot_count, xex.tls.data_size,
                    xex.tls.raw_data_size, xex.tls.raw_data_address);
    else std::printf("none\n");

    // the renderer, as the command processor's Sink before its thread runs
    std::unique_ptr<vk::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    if (rendering) {
        device = vk::Device::open(error);
        if (!device) return std::fprintf(stderr, "Vulkan: %s\n", error.c_str()), 1;
        render::Options options;
        if (!shots.empty()) {
            std::filesystem::create_directories(shots, ec);
            stbi_write_png_compression_level = 1; // fast: the shots are written on the GPU's thread
            char title[16];
            std::snprintf(title, sizeof(title), "%08X", xex.execution.title_id);
            options.frame_wanted = [&shot_swaps, shot_every](uint64_t swap) {
                return shot_swaps.count(swap) || (shot_every && swap % shot_every == 0);
            };
            options.on_frame = [dir = shots, prefix = std::string(title)](uint64_t swap, const render::Image &frame) {
                std::filesystem::path path = dir / (prefix + "_" + std::to_string(swap) + ".png");
                if (!stbi_write_png(path.string().c_str(), int(frame.width), int(frame.height), 4, frame.rgba.data(),
                                    int(frame.width) * 4))
                    std::fprintf(stderr, "could not write %s\n", path.string().c_str());
            };
        }
        gpu::CommandProcessor &processor = kernel::gpu_command_processor(k);
        renderer = render::Renderer::create(*device, processor, options, error);
        if (!renderer) return std::fprintf(stderr, "renderer: %s\n", error.c_str()), 1;
        processor.set_sink(renderer.get());
        the_renderer = renderer.get();
        std::printf("rendering on %s%s%s\n", device->name().c_str(), shots.empty() ? "" : ", shots in ",
                    shots.string().c_str());
    }

    guest_base = memory->base(), the_kernel = &k;
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_fault);
#endif
    if (!k.launch(error)) return std::fprintf(stderr, "launch: %s\n", error.c_str()), 1;
    // until the main thread ends, the code hits what cannot run, or time is up
    uint32_t exit_code = 0;
    bool ended = false;
    for (uint32_t ms = 0; ms < seconds * 1000 && !k.stopped; ms += 100)
        if ((ended = k.wait_main(100, &exit_code))) break;

    if (k.stopped) std::printf("stopped: %s\n", k.stop_reason.c_str());
    else if (ended) std::printf("the main thread ended (exit code 0x%08X)\n", exit_code);
    else std::printf("still running after %u s\n", seconds);
    std::printf("threads:\n%s", k.thread_report().c_str());
    print_report(k, true);
    // other threads may still run: no destructors under them
    std::fflush(stdout);
    std::_Exit(0);
}

namespace {
// the renderer's counts: draws drawn and skipped by reason, what is
// approximate, resolves, swaps, the time it took
void print_renderer_report(const render::Report &r) {
    std::printf("renderer: %llu draws (%llu drawn), %llu resolves, %llu swaps (%llu front buffers written)\n",
                (unsigned long long)r.draws, (unsigned long long)r.drawn, (unsigned long long)r.resolves,
                (unsigned long long)r.swaps, (unsigned long long)r.frames);
    std::printf("renderer: %.1f s spent: %.1f s in submissions, %.1f s in resolves, %.1f s checking textures, "
                "%.1f s decoding them, %.1f s copying %.1f MB of vertex data\n",
                r.seconds, r.submit_seconds, r.resolve_seconds, r.check_seconds, r.decode_seconds, r.vertex_seconds,
                double(r.vertex_bytes) / (1 << 20));
    std::printf("renderer: made %llu render targets, %llu textures, %llu pipelines, %llu shaders\n",
                (unsigned long long)r.render_targets, (unsigned long long)r.textures,
                (unsigned long long)r.pipelines, (unsigned long long)r.shaders);
    for (const auto &[why, n] : r.skipped) std::printf("  skipped %8llu  %s\n", (unsigned long long)n, why.c_str());
    for (const auto &[what, n] : r.approximate)
        std::printf("  approx  %8llu  %s\n", (unsigned long long)n, what.c_str());
}

void print_report(kernel::Kernel &k, bool waiting) {
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
        std::printf("  %10llu  0x%08X\n", (unsigned long long)entered[i].first, entered[i].second);
    std::vector<std::pair<uint64_t, std::string>> calls;
    for (const auto &[name, n] : k.calls()) calls.push_back({n, name});
    std::sort(calls.rbegin(), calls.rend());
    std::printf("kernel calls (%zu exports):\n", calls.size());
    auto missing = k.unimplemented();
    for (const auto &[n, name] : calls)
        std::printf("  %8llu  %s%s\n", (unsigned long long)n, name.c_str(), missing.count(name) ? "  (not implemented)" : "");
    // what the GPU did: packets, draws, resolves, swaps, interrupts
    std::printf("%s", kernel::video_report(k).c_str());
    // and the APU: render driver frames, XMA contexts
    std::printf("%s", kernel::audio_report(k).c_str());
    if (the_renderer) {
        render::Report r;
        if (waiting) r = the_renderer->report();
        if (waiting || the_renderer->try_report(r)) {
            print_renderer_report(r);
            std::string failure;
            if (the_renderer->failed(failure)) std::printf("renderer: failed: %s\n", failure.c_str());
        } else {
            std::printf("renderer: busy (the fault came during one of its calls)\n");
        }
    }
    std::fflush(stdout);
}
} // namespace
