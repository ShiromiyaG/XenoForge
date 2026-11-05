// boot: a game run without a window (plan step A4; docs/runtime.md). Loads
// the disc's executable, maps it, binds its imports, starts the main thread
// the kernel makes (Kernel::launch: stack, PCR, KTHREAD, TLS, as Xenia's
// XThread), runs the entry point (generated code where linked, else the
// interpreter) and prints what the kernel saw: every export called and how
// often, the ones with no implementation, the threads and where it stopped,
// and what the GPU did (packets, draws, resolves, swaps, interrupts).
// The disc is the game's file system (game:, d:), or a bare executable's
// directory is; --content names the writable storage root (Xenia's layout:
// content/ for saves, cache*/), a temporary directory's xenoforge by default.
//
//   boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] [--content <dir>]

#include "xenoforge/disc.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace xf;

namespace {

// for the fault report: the guest memory
uint8_t *guest_base = nullptr;
kernel::Kernel *the_kernel = nullptr;

void print_report(kernel::Kernel &k);

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
    if (the_kernel) print_report(*the_kernel);
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
    if (argc < 2) {
        std::fprintf(stderr, "usage: boot <game.iso | default.xex> [--xex <path>] [--steps N] [--seconds N] "
                             "[--content <dir>]\n");
        return 2;
    }
    std::string input = argv[1], xex_path = "default.xex";
    std::error_code ec;
    std::filesystem::path storage = std::filesystem::temp_directory_path(ec) / "xenoforge";
    uint64_t steps = 2'000'000'000ull;
    uint32_t seconds = 30;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--steps" && k + 1 < argc) steps = std::strtoull(argv[++k], nullptr, 10);
        else if (a == "--seconds" && k + 1 < argc) seconds = uint32_t(std::strtoul(argv[++k], nullptr, 10));
        else if (a == "--content" && k + 1 < argc) storage = argv[++k];
    }

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
    if (!k.load(xex, error)) return std::fprintf(stderr, "load: %s\n", error.c_str()), 1;
    k.step_limit = steps;
    std::printf("loaded %s: base 0x%08X, entry 0x%08X, %zu imported functions\n", xex_path.c_str(), xex.base_address,
                k.entry_point(), k.imports().size());
    std::printf("stack 0x%X; TLS: ", xex.stack_size);
    if (xex.has_tls)
        std::printf("%u slots, %u bytes of data (0x%X from 0x%08X)\n", xex.tls.slot_count, xex.tls.data_size,
                    xex.tls.raw_data_size, xex.tls.raw_data_address);
    else std::printf("none\n");

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
    print_report(k);
    // other threads may still run: no destructors under them
    std::fflush(stdout);
    std::_Exit(0);
}

namespace {
void print_report(kernel::Kernel &k) {
    std::printf("interpreted entries: %llu\n", (unsigned long long)k.interpreted_entries());
    std::vector<std::pair<uint64_t, std::string>> calls;
    for (const auto &[name, n] : k.calls()) calls.push_back({n, name});
    std::sort(calls.rbegin(), calls.rend());
    std::printf("kernel calls (%zu exports):\n", calls.size());
    auto missing = k.unimplemented();
    for (const auto &[n, name] : calls)
        std::printf("  %8llu  %s%s\n", (unsigned long long)n, name.c_str(), missing.count(name) ? "  (not implemented)" : "");
    // what the GPU did: packets, draws, resolves, swaps, interrupts
    std::printf("%s", kernel::video_report(k).c_str());
    std::fflush(stdout);
}
} // namespace
