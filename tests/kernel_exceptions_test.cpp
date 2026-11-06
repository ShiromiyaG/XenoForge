// The exception dispatch (src/kernel/exceptions.cpp; docs/runtime.md,
// "Structured exceptions") on a guest program (seh_program.h), interpreted
// and then recompiled (seh_gen's output, built with the test): __except and
// its filters, nested __try with __finally run on the unwind, a continued
// exception, a noncontinuable one continued, DbgBreakPoint, SetThreadName's
// exception with no handler, RtlUnwind to its own frame and two frames up,
// and a C++ throw caught two frames up, rethrown from the catch and caught
// again. A child process checks that an exception no handler takes stops the
// title (KeBugCheckEx, KMODE_EXCEPTION_NOT_HANDLED).

#include "check.h"
#include "objects.h"
#include "seh_program.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/ppc_module.h"
#include "xenoforge/ppc_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

extern const xf::ppc::GeneratedFunction xf_seh_functions[];
extern const size_t xf_seh_function_count;
extern const uint32_t xf_seh_resumes[];
extern const size_t xf_seh_resume_count;

using namespace xf;
using namespace xf::kernel;

namespace {

Kernel *K = nullptr;
const seh::Program *P = nullptr;
bool child_unhandled = false;
constexpr uint32_t F_MAIN = 0x82300000;

std::vector<uint32_t> take_log() {
    std::vector<uint32_t> out;
    uint32_t n = std::min(K->load32(seh::LOG), 255u);
    for (uint32_t i = 0; i < n; i++) out.push_back(K->load32(seh::LOG + 4 + 4 * i));
    K->store32(seh::LOG, 0);
    return out;
}

std::string show(const std::vector<uint32_t> &v) {
    std::string s;
    char hex[16];
    for (uint32_t x : v) std::snprintf(hex, sizeof(hex), "%s%X", s.empty() ? "" : " ", x), s += hex;
    return s;
}

#define CHECK_LOG(...)                                                                                                 \
    do {                                                                                                               \
        std::vector<uint32_t> got = take_log(), want = {__VA_ARGS__};                                                  \
        CHECK(got == want);                                                                                            \
        if (got != want) std::printf("  got  %s\n  want %s\n", show(got).c_str(), show(want).c_str());                 \
    } while (0)

// ---- the C++ handler, host code with a guest frame ----
//
// What the games' C++ runtime does with the kernel, cut to the protocol: its
// handler data says the try range of call sites, the type caught (0: any),
// the catch funclet, and a destructor with the range of call sites it covers.
// Searching, a throw (or a rethrow: the type of the one being handled) from
// the try range is caught: the context captured, its IAR made the return
// point, RtlUnwind to the frame with that target (the frame's handler, unwound
// as the target, saves the frame's context and swaps the captured one in, so
// the unwind comes back here), the catch funclet called with the frame in r12,
// then the frame's registers loaded and a return to the continuation it gave.
// A rethrow's search passes the frame whose catch is running (EXCEPTION_NESTED_
// CALL). Unwinding, the destructor runs once.

uint32_t in_catch_frame = 0, current_type = 0;
bool unwind_context_set = false;
std::set<uint32_t> destroyed;

void copy_guest(uint32_t to, uint32_t from, uint32_t size) { std::memmove(K->host(to), K->host(from), size); }

void cxx_handler(ppc::Context &c) {
    const uint32_t h = P->at("seh_cxx_handler");
    // its prologue, as the words at h say: LR at -8, a 0x100-byte frame
    const uint32_t entry = uint32_t(c.r[1]);
    K->store32(entry - 8, uint32_t(c.lr));
    c.r[1] = entry - 0x100;
    K->store32(uint32_t(c.r[1]), entry);
    const uint32_t rec = uint32_t(c.r[3]), frame = uint32_t(c.r[4]), ctx = uint32_t(c.r[5]), dc = uint32_t(c.r[6]);
    const uint32_t pc = K->load32(dc), info = K->load32(K->load32(K->load32(dc + 4)) - 4);
    const uint32_t flags = K->load32(rec + 4), code = K->load32(rec);
    auto field = [&](int i) { return K->load32(info + 4 * i); };
    // calls out return inside its range, past its prologue
    auto call = [&](uint32_t fn, uint32_t ret) {
        c.lr = ret;
        K->dispatch(c, fn);
    };
    if (flags & 0x66) {
        if ((flags & 0x20) && unwind_context_set) {
            copy_guest(seh::FRAME_CTX, ctx, 0xA40);
            copy_guest(ctx, seh::UNWIND_CTX, 0xA40);
        } else if (field(4) && pc >= field(5) && pc < field(6) && destroyed.insert(frame).second) {
            c.r[12] = frame;
            call(field(4), h + 0x10);
        }
    } else if (code == 0xE06D7363) {
        uint32_t type = K->load32(rec + 0x14 + 8);
        if (!type) type = current_type;
        bool passing = (flags & 0x10) && frame == in_catch_frame;
        if (!passing && pc >= field(0) && pc < field(1) && (!field(2) || field(2) == type)) {
            c.r[3] = seh::UNWIND_CTX;
            call(P->at("RtlCaptureContext"), h + 0x10);
            K->store32(seh::UNWIND_CTX + 8, h + 0x14);
            unwind_context_set = true;
            c.r[3] = frame, c.r[4] = h + 0x14, c.r[5] = rec, c.r[6] = 0;
            call(P->at("RtlUnwind"), h + 0x14);
            unwind_context_set = false;
            CHECK(uint32_t(c.r[1]) == entry - 0x100); // back in its own frame
            uint32_t outer = in_catch_frame;
            in_catch_frame = frame, current_type = type;
            c.r[12] = frame;
            call(field(3), h + 0x18);
            uint32_t continuation = uint32_t(c.r[3]);
            // a catch further down (one this catch's rethrow left) is gone
            in_catch_frame = outer > frame ? outer : 0;
            // the CRT's jump to the continuation: the frame's registers
            const uint32_t f = seh::FRAME_CTX;
            c.r[1] = K->load64(f + 0x18 + 8), c.r[2] = K->load64(f + 0x18 + 16);
            for (int r = 14; r < 32; r++) c.r[r] = K->load64(f + 0x18 + 8 * r);
            for (int r = 14; r < 32; r++) {
                uint64_t bits = K->load64(f + 0x128 + 8 * r);
                std::memcpy(&c.f[r], &bits, 8);
            }
            c.set_cr_word(K->load32(f + 0x118));
            c.ctr = K->load64(f + 0x10);
            c.lr = continuation;
            ppc::unwind(c);
        }
    }
    c.r[1] = entry;
    c.lr = K->load32(entry - 8);
    c.r[3] = 1; // ExceptionContinueSearch
}

// ---- the scenarios, on a guest thread ----

void scenario(ppc::Context &c, const char *name) {
    if (std::getenv("XF_SEH_VERBOSE")) std::printf("%s\n", name);
    const uint32_t sp = uint32_t(c.r[1]);
    K->call(c, P->at(name));
    CHECK(uint32_t(c.r[1]) == sp);
    if (K->stopped) std::printf("%s: stopped: %s\n", name, K->stop_reason.c_str());
}

void main_thread(ppc::Context &c) {
    if (child_unhandled) {
        scenario(c, "f_unhandled");
        std::printf("kernel_exceptions: the unhandled exception came back\n");
        std::fflush(stdout);
        std::_Exit(3);
    }
    take_log();
    scenario(c, "f_try1");
    CHECK_LOG(0x10, 0xE0000001, 0x1111, 0x2222, 0x12);
    CHECK(K->load64(seh::FSLOT) == 0x3FF8000000000000ull); // f31 back, 1.5
    scenario(c, "f_nested");
    CHECK_LOG(0x21, 0x31, 0x20, 0x22);
    scenario(c, "f_continue");
    CHECK_LOG(0x40, 0xC0, 0x0BAD, 0x41);
    scenario(c, "f_noncont");
    CHECK_LOG(0x50, 0x50, 0xC0000025, 0xE0000004, 0x52);
    scenario(c, "f_break");
    CHECK_LOG(0x80000003, 0x61);
    scenario(c, "f_threadname");
    CHECK_LOG(0x70);
    CHECK(current_thread() && current_thread()->guest_name == "seh-thread");
    scenario(c, "f_local_unwind");
    CHECK_LOG(0x80, 0x81, 0x55, 0x82);
    scenario(c, "f_lj_outer");
    CHECK_LOG(0x90, 0x91, 0x66, 0x3030, 0x92);
    destroyed.clear();
    scenario(c, "f_cxx_outer");
    CHECK_LOG(0xD1, 0xA1, 0xA5, 0x4444, 0xA9);
    CHECK(in_catch_frame == 0 && !unwind_context_set);
    c.r[3] = 0x600D;
}

// the program at its base in a fresh kernel, run on a guest thread
void run(bool generated) {
    std::string error;
    auto memory = Memory::create(error);
    if (!memory) {
        std::printf("memory: %s\n", error.c_str());
        failures++;
        return;
    }
    Kernel k(*memory);
    K = &k;
    Heap *heap = memory->heap_for(seh::BASE);
    CHECK(heap && heap->alloc_fixed(seh::BASE, seh::SIZE, heap->page_size(), ALLOCATE_RESERVE | ALLOCATE_COMMIT,
                                    PROTECT_READ_WRITE));
    std::memcpy(k.host(seh::BASE), P->image.data(), seh::SIZE);
    for (const seh::Import &i : seh::IMPORTS) k.add_thunk(i.thunk, "xboxkrnl", i.name);
    add_function_table(k, seh::CODE, seh::CODE_END, seh::PDATA, P->pdata_count);

    std::vector<ppc::GeneratedFunction> table = {{F_MAIN, main_thread}, {P->at("seh_cxx_handler"), cxx_handler}};
    if (generated) table.insert(table.end(), xf_seh_functions, xf_seh_functions + xf_seh_function_count);
    std::sort(table.begin(), table.end(), [](const auto &a, const auto &b) { return a.address < b.address; });
    k.set_generated(table.data(), table.size(), generated ? xf_seh_resumes : nullptr,
                    generated ? xf_seh_resume_count : 0);

    Objects &o = objects(k);
    o.set_tls(64, 0, 0, 0);
    Thread::Params params;
    params.stack_size = 256 * 1024;
    params.start_address = F_MAIN;
    uint32_t status;
    Ref<Thread> t = o.create_thread(params, status);
    CHECK(t && status == X_STATUS_SUCCESS);
    if (t) {
        CHECK(o.scheduler->start(*t));
        o.scheduler->join(*t);
        CHECK(t->exit_code == 0x600D);
    }
    CHECK(!k.stopped);
    if (generated) {
        // every frame generated: the unwinds land at resume sites
        CHECK(k.interpreted_entries() == 0);
        CHECK(k.unwinds_by_dispatch == 0 && k.unwinds_thrown > 0);
    } else {
        CHECK(k.interpreted_entries() > 0);
    }
    std::printf("kernel_exceptions (%s): %llu returns to another frame, %llu interpreted entries\n",
                generated ? "generated" : "interpreted", (unsigned long long)k.unwinds_thrown.load(),
                (unsigned long long)k.interpreted_entries());
    std::lock_guard<std::mutex> l(o.lock);
    CHECK(o.threads.empty());
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // what failed shows even if a scenario crashes
    seh::Program program = seh::build_program();
    P = &program;
    if (argc > 1 && std::string(argv[1]) == "unhandled") {
        child_unhandled = true;
        run(false);
        return 3;
    }
    run(false);
    run(true);

    // an exception no handler takes: KeBugCheckEx ends the process (exit 4)
    std::string command = std::string("\"") + argv[0] + "\" unhandled";
    int status = std::system(command.c_str());
#ifndef _WIN32
    status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    CHECK(status == 4);
    return finish("kernel_exceptions");
}
