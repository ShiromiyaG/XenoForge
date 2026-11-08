// Crash and stop reports (include/xenoforge/crash.h): the report of a guest
// thread (its function, stack by the back chain, last kernel calls), a stop
// by the interpreter, and real host faults in a child process, in generated
// code (a host function in the table) and in interpreted code: the handler
// writes crash-<time>.txt and the process ends with exit code 3.

#include "check.h"

#include "objects.h"
#include "xenoforge/crash.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
uint32_t interpreted_code = 0; // guest memory with PPC code
std::string report_text, report_stack;

enum : uint32_t { F_FAULT = 0x82000000, F_B = 0x82000100, F_C = 0x82000200, F_REPORT = 0x82000300 };

// two frames on the stack as the XDK's prologues leave them: r1 back chains
// and the LRs saved 8 below each frame's top, returning into F_B and F_C
void fake_frames(ppc::Context &c) {
    c.r[1] -= 0x100;
    uint32_t sp = uint32_t(c.r[1]);
    K->store32(sp, sp + 0x40);
    K->store32(sp + 0x40 - 8, F_B + 0x14);
    K->store32(sp + 0x40, sp + 0x80);
    K->store32(sp + 0x80 - 8, F_C + 0x28);
    K->store32(sp + 0x80, sp + 0xC0);
    c.lr = F_B + 0x14;
}

// two exports, called through their thunks as generated code calls them
uint32_t thunk_a = 0, thunk_b = 0;

void make_thunks(Kernel &k, Memory &memory) {
    thunk_a = memory.system_alloc(0x10), thunk_b = thunk_a + 8;
    k.add_thunk(thunk_a, "xboxkrnl", "KeGetCurrentProcessType");
    k.add_thunk(thunk_b, "xboxkrnl", "KeQueryPerformanceFrequency");
}

void call_exports(ppc::Context &c) {
    c.lr = F_B + 0x14;
    K->dispatch(c, thunk_a);
    c.lr = F_B + 0x14;
    K->dispatch(c, thunk_b);
}

// a host function that faults: a write to guest page 0, which nothing maps
void fault(ppc::Context &c) {
    fake_frames(c);
    call_exports(c);
    *reinterpret_cast<volatile uint32_t *>(c.base + 0x10) = 1;
}

void nop(ppc::Context &) {}

// the report, taken as a fault handler would, without a fault
void report(ppc::Context &c) {
    fake_frames(c);
    call_exports(c);
    CrashReport r = make_crash_report(*K, "a test");
    report_text = r.text, report_stack = r.stack;
}

const Generated generated[] = {
    {F_FAULT, fault}, {F_B, nop}, {F_C, nop}, {F_REPORT, report},
};

// runs a guest thread from start to its end
void run(Kernel &k, uint32_t start) {
    Objects &o = k.objects();
    Thread::Params p;
    p.stack_size = 256 * 1024;
    p.start_address = start;
    p.name = "crasher";
    uint32_t status;
    Ref<Thread> t = o.create_thread(p, status);
    CHECK(t && status == X_STATUS_SUCCESS);
    if (!t) return;
    CHECK(o.scheduler->start(*t));
    o.scheduler->join(*t);
}

void put_code(Kernel &k, uint32_t address, std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) k.store32(address, w), address += 4;
}

// the child: a kernel with the handler, then the fault
int child(const std::string &mode, const char *directory) {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return 1;
    Kernel k(*memory);
    K = &k;
    k.set_generated(generated, std::size(generated));
    make_thunks(k, *memory);
    install_crash_handler(k, directory);
    k.objects().set_tls(64, 0, 0, 0);
    uint32_t code = memory->system_alloc(0x1000);
    // li r4, 0x10; stw r3, 0(r4); blr
    put_code(k, code, {0x38800010, 0x90640000, 0x4E800020});
    run(k, mode == "generated" ? F_FAULT : code);
    return 0; // the handler ends the process before this
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// runs this program as the child; its exit code
int run_child(const char *self, const char *mode, const std::filesystem::path &directory) {
    std::string command = std::string("\"") + self + "\" --child " + mode + " \"" + directory.string() + "\" > \"" +
                          (directory / "out.txt").string() + "\" 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\"";
    return std::system(command.c_str());
#else
    int status = std::system(command.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--child") return child(argv[2], argv[3]);

    std::string error;
    auto memory = Memory::create(error);
    if (!memory) return std::printf("memory: %s\n", error.c_str()), 1;
    {
        Kernel k(*memory);
        K = &k;
        k.set_generated(generated, std::size(generated));
        k.objects().set_tls(64, 0, 0, 0);
        make_thunks(k, *memory);

        // the report of a guest thread: where, the stack as functions, the calls
        run(k, F_REPORT);
        CHECK(has(report_text, "crash report: a test"));
        CHECK(has(report_text, "thread: ") && has(report_text, "crasher"));
        CHECK(has(report_text, "function 0x82000100+0x14"));
        CHECK(has(report_text, "#0 0x82000114"));
        CHECK(has(report_text, "#1 0x82000228") && has(report_text, "function 0x82000200+0x28"));
        CHECK(has(report_text, "xboxkrnl!KeGetCurrentProcessType"));
        CHECK(has(report_text, "xboxkrnl!KeQueryPerformanceFrequency"));
        CHECK(report_text.find("KeGetCurrentProcessType") < report_text.find("KeQueryPerformanceFrequency"));
        CHECK(has(report_stack, "0x82000114 (0x82000100+0x14) <- 0x82000228 (0x82000200+0x28)"));

        // the interpreter stops on an instruction it cannot run: the stop
        // carries the report taken on the stopping thread
        uint32_t code = memory->system_alloc(0x1000);
        put_code(k, code, {0x38600001, 0x00000000});
        run(k, code);
        CHECK(k.stopped);
        CHECK(has(k.stop_reason, "invalid instruction"));
        CHECK(has(k.stop_report, "crash report: the interpreter stopped") && has(k.stop_report, "thread: "));
        CHECK(!k.stop_stack.empty());
    }

    // real faults, each in a process of its own
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("xf_crash_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (const char *mode : {"generated", "interpreted"}) {
        std::filesystem::path dir = root / mode;
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        int code = run_child(argv[0], mode, dir);
        CHECK(code == 3);
        std::filesystem::path file;
        for (auto &e : std::filesystem::directory_iterator(dir))
            if (e.path().filename().string().rfind("crash-", 0) == 0) file = e.path();
        CHECK(!file.empty());
        std::string text = read_file(file);
        CHECK(has(text, "crash report: writing host 0x") && has(text, "(guest 0x00000010)"));
        if (std::string(mode) == "generated") {
            CHECK(has(text, "faulted in: generated function 0x82000000"));
            CHECK(has(text, "#0 0x82000114"));
            CHECK(has(text, "xboxkrnl!KeQueryPerformanceFrequency"));
        } else {
            // the interpreter's pc is the store's: the second instruction of a page
            size_t at = text.find("context pc: 0x");
            CHECK(at != std::string::npos);
            if (at != std::string::npos) CHECK((std::strtoul(text.c_str() + at + 12, nullptr, 16) & 0xFFF) == 4);
            CHECK(!has(text, "faulted in:"));
        }
        if (failures) std::printf("%s's report:\n%s\nits output:\n%s\n", mode, text.c_str(), read_file(dir / "out.txt").c_str());
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return finish("kernel_crash");
}
