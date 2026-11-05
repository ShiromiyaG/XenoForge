// boot: a game run without a window (plan step A4; docs/runtime.md). Loads
// the disc's executable, maps it, binds its imports, sets up the main
// thread as Xenia's XThread does (a stack, the PCR in r13, TLS), runs the
// entry point (generated code where linked, else the interpreter) and
// prints what the kernel saw: every export called and how often, the ones
// with no implementation, where it stopped.
//
//   boot <game.iso | default.xex> [--xex <path>] [--steps N]

#include "xenoforge/disc.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace xf;

namespace {

// for the fault report: the main thread's context and the guest memory
ppc::Context *main_context = nullptr;
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
    if (main_context)
        std::printf(", the main thread last at guest 0x%08X, lr 0x%08X", main_context->pc, uint32_t(main_context->lr));
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
        std::fprintf(stderr, "usage: boot <game.iso | default.xex> [--xex <path>] [--steps N]\n");
        return 2;
    }
    std::string input = argv[1], xex_path = "default.xex";
    uint64_t steps = 2'000'000'000ull;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--steps" && k + 1 < argc) steps = std::strtoull(argv[++k], nullptr, 10);
    }

    std::string error;
    Xex xex;
    if (!load(input, xex_path, xex, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    auto memory = Memory::create(error);
    if (!memory) return std::fprintf(stderr, "memory: %s\n", error.c_str()), 1;
    kernel::Kernel k(*memory);
    if (!k.load(xex, error)) return std::fprintf(stderr, "load: %s\n", error.c_str()), 1;
    k.step_limit = steps;
    std::printf("loaded %s: base 0x%08X, entry 0x%08X, %zu imported functions\n", xex_path.c_str(), xex.base_address,
                k.entry_point(), k.imports().size());

    // the main thread, as Xenia's XThread::Create sets one up (lean): a
    // stack in the 64 KB virtual heap, the PCR in r13, a TLS block
    static ppc::Context c;
    std::memset(&c, 0, sizeof(c));
    k.attach(c);
    Heap *stacks = memory->heap_by_type(false, 64 * 1024);
    uint32_t stack_size = 1u << 20, stack = 0;
    if (!stacks || !stacks->alloc(stack_size, 64 * 1024, ALLOCATE_RESERVE | ALLOCATE_COMMIT, PROTECT_READ_WRITE, true, &stack))
        return std::fprintf(stderr, "no stack\n"), 1;
    uint32_t pcr = memory->system_alloc(0x2D8), tls = memory->system_alloc(0x1000);
    uint32_t stack_top = stack + stack_size;
    k.store32(pcr + 0x00, tls);       // tls_ptr
    k.store64(pcr + 0x30, pcr);       // pcr_ptr
    k.store32(pcr + 0x70, stack_top); // stack_base_ptr (high)
    k.store32(pcr + 0x74, stack);     // stack_end_ptr (low)
    c.r[1] = stack_top - 0x100;
    c.r[13] = pcr;
    c.msr = 0x9030;

    main_context = &c, guest_base = memory->base(), the_kernel = &k;
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_fault);
#endif
    k.call(c, k.entry_point());

    std::printf("%s\n", k.stopped ? ("stopped: " + k.stop_reason).c_str() : "the entry point returned");
    print_report(k);
    return 0;
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
    std::fflush(stdout);
}
} // namespace
