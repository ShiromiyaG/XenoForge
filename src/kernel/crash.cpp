// Crash and stop reports (crash.h; docs/runtime.md, "Crash and stop reports").
// Written anew; Xenia has no counterpart (it logs the faulting PC and the
// registers of the thread). The report names, for the faulting thread: the
// guest function (the generated code's table gives the function a host
// address or a guest address falls in), the guest stack by the r1 back chain
// (stacks.cpp's guest_backtrace), and the last kernel calls from a ring each
// thread keeps. It runs in a fault handler, so it takes no lock that the
// faulting code may hold: the kernel's lock is only tried, and a thread's
// stack is read inside its own range.

#include "xenoforge/crash.h"

#include "objects.h"
#include "stacks.h"
#include "xenoforge/memory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace xf::kernel {
namespace {

// A thread's last export calls: a ring read racily by the report (a torn
// entry shows as a wrong name at worst).
constexpr uint32_t RING = 16;
struct Ring {
    std::atomic<const Import *> call[RING] = {};
    std::atomic<uint32_t> from[RING] = {};
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> thread{0};
};

struct Registry {
    std::mutex mutex;
    std::vector<Ring *> rings;
};

// (never destroyed: a thread may end after main returns)
Registry &registry() {
    static Registry *r = new Registry;
    return *r;
}

struct Holder {
    Ring ring;
    Holder() {
        std::lock_guard l(registry().mutex);
        registry().rings.push_back(&ring);
    }
    ~Holder() {
        std::lock_guard l(registry().mutex);
        auto &v = registry().rings;
        v.erase(std::remove(v.begin(), v.end(), &ring), v.end());
    }
};

// "0x82001234 default.xex+0x1234, function 0x82001200+0x34"
std::string describe(Kernel &k, uint32_t address) {
    char text[64];
    std::snprintf(text, sizeof(text), "0x%08X", address);
    std::string out = text;
    const char *module = nullptr;
    uint32_t start = 0;
    bool function = k.guest_function(address, module, start);
    std::string name;
    uint32_t base = 0;
    if (module) {
        name = module;
        // (the table's module is the loaded one: its base is its range's)
        std::string loaded;
        if (module_at_try(k, address, loaded, base)) name = loaded;
        else base = 0;
    } else if (!module_at_try(k, address, name, base)) {
        name.clear();
    }
    if (!name.empty() && base) {
        std::snprintf(text, sizeof(text), " %s+0x%X", name.c_str(), address - base);
        out += text;
    } else if (!name.empty()) {
        out += " " + name;
    }
    if (function) {
        std::snprintf(text, sizeof(text), ", function 0x%08X+0x%X", start, address - start);
        out += text;
    }
    return out;
}

// "0x82001234 (0x82001200+0x34)": the one-line form
std::string brief(Kernel &k, uint32_t address) {
    char text[64];
    std::snprintf(text, sizeof(text), "0x%08X", address);
    const char *module;
    uint32_t start;
    if (!k.guest_function(address, module, start)) return text;
    std::snprintf(text, sizeof(text), "0x%08X (0x%08X+0x%X)", address, start, address - start);
    return text;
}

void append_calls(std::string &out, const Ring &r, uint32_t most) {
    uint32_t n = r.count.load(std::memory_order_relaxed);
    uint32_t shown = std::min({n, RING, most});
    for (uint32_t i = n - shown; i != n; i++) {
        const Import *import = r.call[i % RING].load(std::memory_order_relaxed);
        if (!import) continue;
        char line[64];
        std::snprintf(line, sizeof(line), "  (from 0x%08X)%s\n", r.from[i % RING].load(std::memory_order_relaxed),
                      import->fn ? "" : "  not implemented");
        out += "    " + import->library + "!" + import->name + line;
    }
}

} // namespace

void note_kernel_call(const Import &import, uint32_t lr) {
    thread_local Holder holder;
    Ring &r = holder.ring;
    if (Thread *t = current_thread()) r.thread.store(t->id, std::memory_order_relaxed);
    uint32_t n = r.count.load(std::memory_order_relaxed);
    r.call[n % RING].store(&import, std::memory_order_relaxed);
    r.from[n % RING].store(lr, std::memory_order_relaxed);
    r.count.store(n + 1, std::memory_order_release);
}

CrashReport make_crash_report(Kernel &k, const std::string &what, const void *host_pc) {
    CrashReport report;
    report.what = what;
    std::string &out = report.text;
    char line[256];
    out = "crash report: " + what + "\n";

    Thread *t = current_thread();
    ppc::Context *c = t ? t->context.get() : nullptr;
    if (!t || !c) {
        out += "thread: not a guest thread\n";
    } else {
        const char *name = t->params.name ? t->params.name : t->guest_name.c_str();
        std::snprintf(line, sizeof(line), "thread: %u%s%s%s (handle 0x%08X, start 0x%08X, stack 0x%08X-0x%08X)\n",
                      t->id, t->main ? " (main)" : "", name[0] ? ", " : "", name, t->self_handle,
                      t->params.start_address, t->stack_limit, t->stack_base);
        out += line;

        report.pc = c->pc, report.lr = uint32_t(c->lr);
        // a fault in generated code: its function, from the host address
        uint32_t function = 0;
        if (host_pc && k.guest_function_of_host(host_pc, function)) {
            std::snprintf(line, sizeof(line), "faulted in: generated function 0x%08X (host code %p)\n", function,
                          host_pc);
            out += line;
            out += "  " + describe(k, function) + "\n";
            report.pc = function;
        }
        out += "context pc: " + describe(k, c->pc) + (function ? " (the last call's or store's, not the fault's)" : "") +
               "\n";
        out += "lr: " + describe(k, uint32_t(c->lr)) + "\n";
        const uint32_t sp = uint32_t(c->r[1]);
        std::snprintf(line, sizeof(line), "r1: 0x%08X, r3: 0x%08X, r4: 0x%08X, r13: 0x%08X\n", sp, uint32_t(c->r[3]),
                      uint32_t(c->r[4]), uint32_t(c->r[13]));
        out += line;

        // the stack, inside the thread's own range when r1 is in it
        const bool own = sp >= t->stack_limit && sp < t->stack_base;
        std::vector<uint32_t> frames =
            guest_backtrace(k, sp, own ? t->stack_limit : 0, own ? t->stack_base : 0, 24);
        out += "stack (return addresses by the r1 back chain):\n";
        report.stack = brief(k, report.pc);
        if (function) report.stack += " <- " + brief(k, c->pc);
        report.stack += " <- lr " + brief(k, uint32_t(c->lr));
        for (size_t i = 0; i < frames.size(); i++) {
            std::snprintf(line, sizeof(line), "  #%zu ", i);
            out += line + describe(k, frames[i]) + "\n";
            if (i < 12) report.stack += " <- " + brief(k, frames[i]);
        }
        if (frames.empty()) out += "  (none readable)\n";
    }

    // the last kernel calls: this thread's, then the others' (if the lock on
    // the rings is free)
    {
        std::unique_lock l(registry().mutex, std::try_to_lock);
        out += "last kernel calls, oldest first:\n";
        if (l) {
            // this thread's first
            for (int pass = 0; pass < 2; pass++)
                for (Ring *r : registry().rings) {
                    const bool mine = t && r->thread.load() == t->id;
                    if (mine != (pass == 0) || r->count.load() == 0) continue;
                    std::snprintf(line, sizeof(line), "  thread %u%s:\n", r->thread.load(),
                                  mine ? " (this one)" : "");
                    out += line;
                    append_calls(out, *r, mine ? RING : 6);
                }
        } else {
            out += "  (the rings are being changed)\n";
        }
    }

    // the other threads, if the kernel's lock is free
    {
        Objects &o = k.objects();
        std::unique_lock l(o.lock, std::try_to_lock);
        if (l) {
            out += "threads:\n";
            for (Thread *other : o.threads) {
                const char *state = other->blocked == Thread::Blocked::Wait        ? "waiting"
                                    : other->blocked == Thread::Blocked::Suspended ? "suspended"
                                                                                   : "running";
                const char *name = other->params.name ? other->params.name : other->guest_name.c_str();
                std::snprintf(line, sizeof(line), "  thread %u%s%s%s: %s, pc 0x%08X, lr 0x%08X\n", other->id,
                              other->main ? " (main)" : "", name[0] ? ", " : "", name, state, other->context->pc,
                              uint32_t(other->context->lr));
                out += line;
            }
        } else {
            out += "threads: (the kernel's lock is held)\n";
        }
    }
    return report;
}

bool write_crash_report(CrashReport &report, const std::filesystem::path &directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    for (int n = 0; n < 100; n++) {
        std::filesystem::path path = directory / (std::string("crash-") + stamp + (n ? "-" + std::to_string(n) : "") + ".txt");
        if (std::filesystem::exists(path, ec)) continue;
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f << report.text;
        f.flush();
        if (!f) return false;
        report.file = path;
        return true;
    }
    return false;
}

namespace {

Kernel *handler_kernel = nullptr;
std::filesystem::path handler_directory;
CrashHook handler_hook = nullptr;
void *handler_user = nullptr;

// The report, written and shown, then the process ends (the guest cannot go
// on after a host fault).
[[noreturn]] void crash(const std::string &what, const void *host_pc) {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) std::_Exit(3); // a fault in the handler
    CrashReport report = make_crash_report(*handler_kernel, what, host_pc);
    if (!handler_directory.empty()) write_crash_report(report, handler_directory);
    if (handler_hook) {
        handler_hook(*handler_kernel, report, handler_user);
    } else {
        std::fprintf(stderr, "%s", report.text.c_str());
        if (!report.file.empty()) std::fprintf(stderr, "report written to %s\n", report.file.string().c_str());
    }
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

// "reading host 0x10 (guest 0x00000010)"
std::string access_text(const char *kind, uint64_t host) {
    char text[96];
    uint64_t offset = host - uint64_t(handler_kernel->host(0));
    if (offset < (1ull << 32))
        std::snprintf(text, sizeof(text), "%s host 0x%llX (guest 0x%08llX)", kind, (unsigned long long)host,
                      (unsigned long long)offset);
    else
        std::snprintf(text, sizeof(text), "%s host 0x%llX", kind, (unsigned long long)host);
    return text;
}

#ifdef _WIN32
LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: {
        uint64_t kind = r->NumberParameters >= 2 ? r->ExceptionInformation[0] : 0;
        crash(access_text(kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
                          r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0),
              r->ExceptionAddress);
    }
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        crash("illegal host instruction", r->ExceptionAddress);
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        crash("host integer division by zero", r->ExceptionAddress);
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
#else
void on_signal(int signal, siginfo_t *info, void *) {
    // TODO: the faulting host pc from the ucontext (REG_RIP, ARM64's pc), to name a generated function
    const char *kind = signal == SIGSEGV   ? "memory fault at"
                       : signal == SIGBUS  ? "bus error at"
                       : signal == SIGILL  ? "illegal host instruction at"
                                           : "host arithmetic fault at";
    crash(access_text(kind, uint64_t(uintptr_t(info->si_addr))), nullptr);
}
#endif

} // namespace

void install_crash_handler(Kernel &k, const std::filesystem::path &directory, CrashHook hook, void *user) {
    handler_kernel = &k, handler_directory = directory, handler_hook = hook, handler_user = user;
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_exception);
#else
    struct sigaction action = {};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(s, &action, nullptr);
#endif
}

} // namespace xf::kernel
