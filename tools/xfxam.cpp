// xfxam: the console's own XAM as the oracle for ours (docs/runtime.md, "XAM's
// oracle"). The file is the system update's $flash_xam.xex from the player's
// disc (xfinspect --package extracts it); it is read from the path given and
// nothing of it is kept.
//
//   xfxam <xam.xex> imports                what its imports bind to here
//   xfxam <xam.xex> init                   its entry run on our kernel: the
//                                          calls it made, where it stopped
//   xfxam <xam.xex> call <Export> [arg..]  one export, ours and the console's,
//                                          each from a fresh kernel: the
//                                          results, the arguments' memory and
//                                          the kernel calls side by side
//   xfxam <xam.xex> survey [--out <file>]  every export both have, sorted into
//                                          matches, differs, needs state, faults
//
// An argument is a number (the register's value), or memory made for it, at
// the same address on both sides: buf:<size>[:<hex bytes>], fill:<size>:<byte>,
// u32:<value>, u64:<value>, str:<text>, wstr:<text>. Each side runs in a
// process of its own (`side`, which `call` and `survey` start), so a fault or
// a hang on one side costs only that side's answer.

#include "files.h"
#include "objects.h"
#include "xam.h"
#include "xenoforge/exports.h"
#include "xenoforge/kernel.h"
#include "xenoforge/memory.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

using namespace xf;
using kernel::Kernel;

namespace {

// where the arguments' memory goes, on both sides (a range nothing else in
// the kernel or XAM's start takes), and the thunk ours is called through
constexpr uint32_t ARGS_BASE = 0x5F000000, ARGS_SIZE = 0x100000;
constexpr uint32_t THUNK = ARGS_BASE + ARGS_SIZE - 0x1000;
// the console's XAM, as its system loads it
constexpr const char *XAM_PATH = "\\SystemRoot\\xam.xex";
constexpr uint32_t PROCESS_ATTACH = 1; // DllMain's reason

bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool parse_number(const std::string &s, uint64_t &out) {
    if (s.empty()) return false;
    char *end = nullptr;
    out = s[0] == '-' ? uint64_t(std::strtoll(s.c_str(), &end, 0)) : std::strtoull(s.c_str(), &end, 0);
    return end && *end == 0;
}

std::string hex(const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789ABCDEF";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) s[2 * i] = digits[p[i] >> 4], s[2 * i + 1] = digits[p[i] & 15];
    return s;
}

std::string format(const char *f, ...) {
    char text[512];
    va_list list;
    va_start(list, f);
    std::vsnprintf(text, sizeof(text), f, list);
    va_end(list);
    return text;
}

std::vector<uint8_t> unhex(const std::string &s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

// ---- the synthetic title ----

// A title of the tool's own, for XAM to have one running: an unencrypted,
// uncompressed XEX2 with an execution info (a made-up title ID), an image of
// one page at 0x82000000 and an entry point that returns at once. No game's
// data: only the fields XAM and the kernel read about a title.
constexpr uint32_t TITLE_BASE = 0x82000000, TITLE_ID = 0x58450001, TITLE_HEADER = 0x1000, TITLE_IMAGE = 0x1000;
constexpr const char *TITLE_FILE = "default.xex";
constexpr const char *PARTITION1 = "\\Device\\Harddisk0\\Partition1";

void put32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

std::vector<uint8_t> make_title_xex() {
    std::vector<uint8_t> f(TITLE_HEADER + TITLE_IMAGE, 0);
    uint8_t *p = f.data();
    std::memcpy(p, "XEX2", 4);
    put32(p + 4, 1); // a title
    put32(p + 8, TITLE_HEADER);
    put32(p + 0x10, 0x100); // the security info
    const uint32_t headers[][2] = {
        {xex_key::FILE_FORMAT_INFO, 0x300},         {xex_key::IMAGE_BASE_ADDRESS, TITLE_BASE},
        {xex_key::ENTRY_POINT, TITLE_BASE + 0x100}, {xex_key::EXECUTION_INFO, 0x340},
        {xex_key::DEFAULT_STACK_SIZE, 0x40000},
    };
    put32(p + 0x14, uint32_t(std::size(headers)));
    for (size_t i = 0; i < std::size(headers); i++) {
        put32(p + 0x18 + 8 * i, headers[i][0]);
        put32(p + 0x1C + 8 * i, headers[i][1]);
    }
    uint8_t *s = p + 0x100;
    put32(s + 4, TITLE_IMAGE);
    put32(s + 0x110, TITLE_BASE);
    put32(s + 0x180, 1);
    put32(s + 0x184, (1u << 4) | 1);
    put32(p + 0x300, 8); // file format: not encrypted, not compressed
    uint8_t *exec = p + 0x340;
    put32(exec + 4, 0x10000100); // version 1.0.1.0
    put32(exec + 8, 0x10000100); // base version
    put32(exec + 0xC, TITLE_ID);
    exec[0x12] = 1, exec[0x13] = 1; // disc 1 of 1
    uint8_t *image = p + TITLE_HEADER;
    image[0] = 'M', image[1] = 'Z';
    put32(image + 0x100, 0x38600000); // li r3, 0
    put32(image + 0x104, 0x4E800020); // blr
    return f;
}

// ---- the arguments ----

struct Arg {
    std::string text;           // as given
    bool memory = false;        // memory made for it: its address is the value
    uint64_t value = 0;         // the register's value
    std::vector<uint8_t> bytes; // the memory's starting contents
};

bool parse_arg(const std::string &text, Arg &a, std::string &error) {
    a.text = text;
    size_t colon = text.find(':');
    std::string kind = colon == std::string::npos ? "" : text.substr(0, colon);
    std::string rest = colon == std::string::npos ? "" : text.substr(colon + 1);
    uint64_t v = 0;
    if (kind.empty()) {
        if (!parse_number(text, a.value)) return error = "not a number: " + text, false;
        return true;
    }
    a.memory = true;
    if (kind == "buf") {
        size_t second = rest.find(':');
        if (!parse_number(rest.substr(0, second), v) || v > 0x10000) return error = "bad size: " + text, false;
        a.bytes.assign(size_t(v), 0);
        if (second != std::string::npos) {
            std::vector<uint8_t> init = unhex(rest.substr(second + 1));
            std::copy_n(init.begin(), std::min(init.size(), a.bytes.size()), a.bytes.begin());
        }
    } else if (kind == "fill") {
        size_t second = rest.find(':');
        uint64_t byte = 0;
        if (second == std::string::npos || !parse_number(rest.substr(0, second), v) || v > 0x10000 ||
            !parse_number(rest.substr(second + 1), byte))
            return error = "bad fill: " + text, false;
        a.bytes.assign(size_t(v), uint8_t(byte));
    } else if (kind == "u32" || kind == "u64") {
        if (!parse_number(rest, v)) return error = "not a number: " + text, false;
        int n = kind == "u32" ? 4 : 8;
        for (int i = 0; i < n; i++) a.bytes.push_back(uint8_t(v >> (8 * (n - 1 - i))));
    } else if (kind == "str") {
        a.bytes.assign(rest.begin(), rest.end());
        a.bytes.push_back(0);
    } else if (kind == "wstr") {
        for (char ch : rest) a.bytes.push_back(0), a.bytes.push_back(uint8_t(ch)); // big-endian UTF-16, ASCII
        a.bytes.push_back(0), a.bytes.push_back(0);
    } else {
        return error = "unknown argument kind: " + text, false;
    }
    return true;
}

// Each argument's memory in the range, in order, 16-byte aligned with a gap
// of 16 bytes after each (a write past one shows); the range's used size.
uint32_t place_args(std::vector<Arg> &args) {
    uint32_t at = ARGS_BASE;
    for (Arg &a : args) {
        if (!a.memory) continue;
        a.value = at;
        at += (uint32_t(a.bytes.size()) + 16 + 15) & ~15u;
    }
    return at - ARGS_BASE;
}

// ---- the kernel calls each side makes ----

struct Call {
    uint32_t thread = 0;
    int phase = 0; // 0 the start (XAM's entry), 1 the export's call
    std::string name;
    uint32_t args[4] = {};
    uint32_t result = 0;
    bool returned = false;
};

struct Recorder {
    std::mutex mutex;
    std::vector<Call> calls;
    std::atomic<int> phase{0};
    Kernel *k = nullptr;
};
thread_local std::vector<size_t> open_calls;
bool g_trace = false; // --trace: each call printed as it is made

void record(void *user, const kernel::Import &import, ppc::Context &c, bool returned) {
    Recorder &r = *static_cast<Recorder *>(user);
    std::lock_guard lock(r.mutex);
    if (g_trace) {
        kernel::Thread *t = kernel::current_thread();
        if (!returned)
            std::fprintf(stderr, "[%u] %s!%s(0x%X, 0x%X, 0x%X, 0x%X) from 0x%08X\n", t ? t->id : 0,
                         import.library.c_str(), import.name.c_str(), uint32_t(c.r[3]), uint32_t(c.r[4]),
                         uint32_t(c.r[5]), uint32_t(c.r[6]), uint32_t(c.lr));
        else
            std::fprintf(stderr, "[%u]   %s -> 0x%08X\n", t ? t->id : 0, import.name.c_str(), uint32_t(c.r[3]));
    }
    if (!returned && g_trace && r.k && import.name == "XexLoadExecutable" && uint32_t(c.r[3]) >= 0x1000)
        std::fprintf(stderr, "    (name \"%.100s\")\n", reinterpret_cast<const char *>(r.k->host(uint32_t(c.r[3]))));
    if (!returned) {
        Call call;
        kernel::Thread *t = kernel::current_thread();
        call.thread = t ? t->id : 0;
        call.phase = r.phase;
        call.name = import.library + "!" + import.name;
        for (int i = 0; i < 4; i++) call.args[i] = uint32_t(c.r[3 + i]);
        open_calls.push_back(r.calls.size());
        r.calls.push_back(std::move(call));
    } else if (!open_calls.empty()) {
        Call &call = r.calls[open_calls.back()];
        open_calls.pop_back();
        call.result = uint32_t(c.r[3]), call.returned = true;
    }
}

// ---- one side: a kernel, and guest code run on its threads ----

struct Side {
    std::unique_ptr<Memory> memory;
    std::unique_ptr<Kernel> k;
    Recorder recorder;
    Xex xex;
    uint32_t xam = 0; // the console's XAM's HMODULE
    // the title running (the synthetic one): its file and what the kernel
    // keeps of it (ours loads it; the console's XAM launches it)
    std::vector<uint8_t> title_file;
    Xex title;
};

bool make_side(Side &s, std::string &error) {
    s.memory = Memory::create(error);
    if (!s.memory) return false;
    s.k = std::make_unique<Kernel>(*s.memory);
    s.recorder.k = s.k.get();
    s.k->set_import_hook(record, &s.recorder);
    Heap *heap = s.memory->heap_for(ARGS_BASE);
    if (!heap || !heap->alloc_fixed(ARGS_BASE, ARGS_SIZE, heap->page_size(), ALLOCATE_RESERVE | ALLOCATE_COMMIT,
                                    PROTECT_READ_WRITE))
        return error = "the arguments' range is taken", false;
    return true;
}

bool load_xam(Side &s, const std::vector<uint8_t> &file, std::string &error) {
    if (!parse_xex(file, s.xex, error) || !load_xex_image(file, s.xex, error)) return false;
    s.xam = kernel::load_module(*s.k, XAM_PATH, file, error);
    return s.xam != 0;
}

// A guest function on c's thread: r3-r10, the rest on the stack where a
// caller stores them (the doubleword at r1 + 0x50 + 8 * (n - 8), the word
// at 0x54 a 32-bit argument), its r3; everything but memory restored.
uint64_t guest_call(Kernel &k, ppc::Context &c, uint32_t function, const std::vector<uint64_t> &args) {
    auto saved = std::make_unique<ppc::Context>(c);
    uint32_t extra = args.size() > 8 ? uint32_t(8 * (args.size() - 8)) : 0;
    c.r[1] = (c.r[1] - (64 + 112) - ((extra + 15) & ~15u)) & ~uint64_t(15);
    for (size_t n = 0; n < args.size(); n++) {
        if (n < 8)
            c.r[3 + n] = args[n];
        else
            k.store64(uint32_t(c.r[1]) + 0x50 + uint32_t(8 * (n - 8)), args[n]);
    }
    for (size_t n = args.size(); n < 8; n++) c.r[3 + n] = 0;
    k.call(c, function);
    uint64_t r = c.r[3];
    c = *saved;
    return r;
}

// Host code on a new guest thread of the process (the system's for XAM's
// start, the title's for a call, as a game makes it), waited for: false when
// it has not ended in time (it stays as it is).
struct Job {
    std::function<void(kernel::Thread &)> body;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false, exited = false;
};

bool run_job(Side &s, uint32_t process, const char *name, Job &job, double seconds, std::string &error) {
    kernel::Objects &o = kernel::objects(*s.k);
    kernel::Thread::Params p;
    p.stack_size = 256 * 1024;
    p.process = process;
    p.name = name;
    p.host_arg = &job;
    p.host = [](kernel::Thread &self, void *arg) {
        Job &j = *static_cast<Job *>(arg);
        auto finish = [&](bool exited) {
            std::lock_guard lock(j.mutex);
            j.done = true, j.exited = exited;
            j.cv.notify_all();
        };
        try {
            j.body(self);
        } catch (const kernel::ThreadExit &) {
            finish(true);
            throw;
        }
        finish(false);
    };
    uint32_t status = 0;
    kernel::Ref<kernel::Thread> t = o.create_thread(p, status);
    if (!t) {
        char text[64];
        std::snprintf(text, sizeof(text), "no thread (0x%08X)", status);
        return error = text, false;
    }
    if (!o.scheduler->start(*t)) return error = "no host thread", false;
    std::unique_lock lock(job.mutex);
    return job.cv.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return job.done; });
}

// XAM's start: its entry, DllMain(hmodule, DLL_PROCESS_ATTACH, 0), on a
// system thread, as the console's kernel starts it
struct Init {
    std::string status; // "returned 0x...", "did not return in N s", "stopped: ...", "exited"
    uint32_t result = 0;
};

Init run_init(Side &s, double seconds) {
    Init init;
    s.recorder.phase = 0;
    Job job;
    uint32_t entry = s.xex.entry_point, hmodule = s.xam;
    job.body = [&, entry, hmodule](kernel::Thread &self) {
        init.result = uint32_t(guest_call(*s.k, *self.context, entry, {hmodule, PROCESS_ATTACH, 0}));
    };
    std::string error;
    char text[128];
    if (!run_job(s, kernel::system_process(*s.k), "XAM start", job, seconds, error)) {
        if (!error.empty()) return init.status = error, init;
        std::snprintf(text, sizeof(text), "did not return in %g s", seconds);
        init.status = text;
    } else if (s.k->stopped) {
        init.status = "stopped: " + s.k->stop_reason;
    } else if (job.exited) {
        init.status = "the thread exited";
    } else {
        std::snprintf(text, sizeof(text), "returned 0x%08X", init.result);
        init.status = text;
    }
    return init;
}

// the xam exports this kernel implements, by name: their ordinals
std::map<std::string, uint16_t> our_xam_exports() {
    std::map<std::string, uint16_t> out;
    for (uint32_t ordinal = 0; ordinal < 0x10000; ordinal++)
        if (const Export *e = find_export("xam.xex", uint16_t(ordinal)))
            if (e->kind == ExportKind::Function && Kernel::implemented("xam", e->name)) out[e->name] = e->ordinal;
    return out;
}

uint16_t xam_ordinal(const std::string &name) {
    for (uint32_t ordinal = 0; ordinal < 0x10000; ordinal++)
        if (const Export *e = find_export("xam.xex", uint16_t(ordinal)); e && name == e->name) return e->ordinal;
    return 0;
}

// ---- a side's answer, written to a file for `call` and `survey` ----

struct Answer {
    std::string init;   // the real side's start
    std::string status; // returned, timeout, stopped ..., fault ...
    uint64_t r3 = 0;
    std::vector<uint8_t> region; // the arguments' memory after
    std::vector<Call> calls;     // on the call's thread, while it ran
    std::string log;             // the side's own output
};

std::string g_result_path; // where the side writes its answer (the fault handler too)
Side *g_side = nullptr;

void write_status_and_exit(const std::string &status, int code) {
    if (FILE *f = std::fopen(g_result_path.c_str(), "ab")) {
        std::fprintf(f, "status %s\n", status.c_str());
        std::fclose(f);
    }
    std::fflush(stdout);
    std::_Exit(code);
}

// a fault in guest code (or ours): where, in guest terms when it can tell
void report_fault(uintptr_t host_address, const char *what) {
    char text[256];
    std::string where;
    if (g_side && g_side->memory) {
        uintptr_t base = uintptr_t(g_side->memory->base());
        if (host_address >= base && host_address - base < 0x100000000ull + 0x1000) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " guest 0x%08X", uint32_t(host_address - base));
            where = buf;
        }
    }
    std::string guest;
    std::string chain;
    if (ppc::Context *c = Kernel::current_context()) {
        char buf[64];
        kernel::Thread *t = kernel::current_thread();
        std::snprintf(buf, sizeof(buf), " at pc 0x%08X lr 0x%08X (thread %u)", c->pc, uint32_t(c->lr), t ? t->id : 0);
        guest = buf;
        // the callers, by the stack's back chain (each frame's LR is saved 8
        // bytes below its caller's frame)
        if (g_side && g_side->k) {
            Kernel &k = *g_side->k;
            uint32_t sp = uint32_t(c->r[1]);
            for (int i = 0; i < 16 && sp >= 0x70000000 && sp < 0x7F000000; i++) {
                uint32_t up = k.load32(sp);
                if (up <= sp || up >= 0x7F000000) break;
                std::snprintf(buf, sizeof(buf), " 0x%08X", k.load32(up - 8));
                chain += buf;
                sp = up;
            }
        }
    }
    std::snprintf(text, sizeof(text), "fault %s (host %p%s)%s", what, reinterpret_cast<void *>(host_address),
                  where.c_str(), guest.c_str());
    std::fprintf(stderr, "xfxam: %s\n", text);
    if (!chain.empty()) std::fprintf(stderr, "xfxam: the callers' return addresses:%s\n", chain.c_str());
    write_status_and_exit(text, 4);
}

#ifdef _WIN32
LONG WINAPI unhandled(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        report_fault(uintptr_t(r->ExceptionInformation[1]), r->ExceptionInformation[0] ? "writing" : "reading");
    char what[48];
    std::snprintf(what, sizeof(what), "exception 0x%08lX", r->ExceptionCode);
    report_fault(uintptr_t(r->ExceptionAddress), what);
    return EXCEPTION_EXECUTE_HANDLER;
}
void install_fault_handler() { SetUnhandledExceptionFilter(unhandled); }
#else
void on_signal(int signal, siginfo_t *info, void *) {
    report_fault(uintptr_t(info->si_addr), signal == SIGBUS ? "(bus)" : "accessing");
}
void install_fault_handler() {
    struct sigaction action = {};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}
#endif

void write_calls(FILE *f, const std::vector<Call> &calls) {
    for (const Call &c : calls)
        std::fprintf(f, "call %s 0x%08X 0x%08X 0x%08X 0x%08X %s0x%08X\n", c.name.c_str(), c.args[0], c.args[1],
                     c.args[2], c.args[3], c.returned ? "" : "unreturned ", c.result);
}

bool read_answer(const std::string &path, Answer &a) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string key;
        in >> key;
        std::string rest = line.size() > key.size() + 1 ? line.substr(key.size() + 1) : "";
        if (key == "init")
            a.init = rest;
        else if (key == "status")
            a.status = rest;
        else if (key == "r3")
            a.r3 = std::strtoull(rest.c_str(), nullptr, 16);
        else if (key == "region")
            a.region = unhex(rest);
        else if (key == "call") {
            Call c;
            std::string word;
            in >> c.name;
            for (uint32_t &v : c.args) in >> word, v = uint32_t(std::strtoul(word.c_str(), nullptr, 16));
            in >> word;
            if (word == "unreturned")
                in >> word;
            else
                c.returned = true;
            c.result = uint32_t(std::strtoul(word.c_str(), nullptr, 16));
            a.calls.push_back(std::move(c));
        }
    }
    return true;
}

struct Options {
    double seconds = 10, init_seconds = 30;
    bool init = true;
    bool title = true;                // a title running when the call is made (--no-title: none)
    std::vector<std::string> prepare; // --prepare: XAM functions to call after its start
    bool launch = false;              // --launch: XAM's own launch of the title (XamLoaderLaunchTitleEx)
    bool user = false;   // --user: ours keeps its signed-in user (the console's XAM has none to compare with)
    bool system = false; // the call on a system thread (else a title's, as a game makes it)
    uint64_t steps = UINT64_MAX;
};

// ---- a title running ----

// The hard drive: a directory of the host as the console's
// \Device\Harddisk0\Partition1, writable, holding the title's file. The
// console's XAM looks for its content there; ours reads the same files as the
// title's directory and keeps what it writes in a storage root of its own.
bool prepare_hdd(Side &s, const std::filesystem::path &dir, std::string &error) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    s.title_file = make_title_xex();
    std::ofstream f(dir / TITLE_FILE, std::ios::binary);
    f.write(reinterpret_cast<const char *>(s.title_file.data()), std::streamsize(s.title_file.size()));
    if (!f) return error = "cannot write " + (dir / TITLE_FILE).string(), false;
    return parse_xex(s.title_file, s.title, error) && load_xex_image(s.title_file, s.title, error);
}

bool mount_hdd_real(Side &s, const std::filesystem::path &dir, bool launch) {
    auto device = kernel::vfs::make_host_device(PARTITION1, dir, false);
    if (!device) return false;
    kernel::file_system(*s.k).add_device(device);
    if (launch) { // XAM's launch loads the dashboard's name: the synthetic title answers to it
        std::error_code ec;
        std::filesystem::path root = dir.parent_path() / "sysroot";
        std::filesystem::create_directories(root, ec);
        std::filesystem::copy_file(dir / TITLE_FILE, root / "dash.xex",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        kernel::file_system(*s.k).add_device(kernel::vfs::make_host_device("\\SystemRoot", root, false));
    }
    return true;
}

// Waits until the kernel calls have stopped coming for a while (the threads
// XAM woke have gone back to waiting): what a launch leaves is settled then.
void wait_quiet(Side &s, double seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    size_t last = SIZE_MAX;
    int stable = 0;
    while (std::chrono::steady_clock::now() < end && stable < 6) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        size_t now;
        {
            std::lock_guard lock(s.recorder.mutex);
            now = s.recorder.calls.size();
        }
        stable = now == last ? stable + 1 : 0;
        last = now;
    }
}

// The console's launch: XamLoaderLaunchTitle(path, 0) as the dashboard calls
// it. XAM's loader thread unloads the title modules, loads the executable and
// starts it through the kernel (XexLoadExecutable, XexStartExecutable), and
// makes the state it keeps for a running title. What it returns is whether the
// kernel's calls were made.
// Functions of the console's XAM called after its start, on a system thread,
// with the arguments given (--prepare <address>[:arg[:arg...]], hex): the
// state XAM makes itself when a title runs, for what the launch above cannot
// make here.
std::string run_prepare(Side &s, const std::vector<std::string> &calls, double seconds) {
    std::string out;
    for (const std::string &text : calls) {
        std::vector<uint64_t> values;
        std::stringstream in(text);
        for (std::string part; std::getline(in, part, ':');) values.push_back(std::strtoull(part.c_str(), nullptr, 16));
        if (values.empty()) continue;
        uint32_t function = uint32_t(values[0]);
        values.erase(values.begin());
        uint64_t r3 = 0;
        Job job;
        job.body = [&](kernel::Thread &self) { r3 = guest_call(*s.k, *self.context, function, values); };
        std::string error;
        bool done = run_job(s, kernel::system_process(*s.k), "prepare", job, seconds, error);
        out += format(" 0x%08X -> %s0x%08X;", function, done ? "" : "(no return) ", uint32_t(r3));
    }
    return out;
}

// What the console's XAM makes for a running title that a launch cannot make
// here, by the build it was found in (identified by its entry point). The
// launch itself (XamLoaderLaunchTitleEx, --launch) goes as far as asking the
// kernel to end the previous title (ExTerminateTitleProcess), which this
// kernel does not model (a process per title), so the
// state is made by the functions of XAM's own that the launch calls, found by
// reading its code (2.0.16202's, no names):
//   0x816E5118  makes the title's heap (heap 0 of its table: the one XamAlloc
//               and every call that allocates on a title's thread use; the
//               launch's state machine makes it as it reaches "title started");
//   0x816DF2F8  stores the current title's ID (what XamGetCurrentTitleId and
//               XNetLogonGetTitleID answer; its start stores 0). The title's
//               execution info XAM reads live from the kernel's executable
//               module (RtlImageXexHeaderField), which the kernel holds already.
// {title} stands for the synthetic title's ID.
struct Build {
    uint32_t entry;
    const char *version;
    std::vector<std::string> prepare; // address[:arg...], hex, in order
};
const Build builds[] = {
    {0x81695360, "2.0.16202", {"816E5118", "816DF2F8:{title}"}},
};

std::vector<std::string> title_state_for(const Side &s) {
    for (const Build &b : builds) {
        if (b.entry != s.xex.entry_point) continue;
        std::vector<std::string> out = b.prepare;
        for (std::string &call : out)
            if (size_t at = call.find("{title}"); at != std::string::npos) call.replace(at, 7, format("%X", TITLE_ID));
        return out;
    }
    return {};
}

std::string launch_title_real(Side &s, double seconds) {
    s.recorder.phase = 0;
    const uint32_t name_at = ARGS_BASE + ARGS_SIZE - 0x2000;
    std::string path = std::string(PARTITION1) + "\\" + TITLE_FILE;
    std::memcpy(s.k->host(name_at), path.c_str(), path.size() + 1);
    uint16_t ordinal = xam_ordinal("XamLoaderLaunchTitleEx");
    uint32_t function = kernel::module_export(*s.k, s.xam, ordinal);
    if (!function) return "XamLoaderLaunchTitleEx is not exported";
    Job job;
    job.body = [&](kernel::Thread &self) { guest_call(*s.k, *self.context, function, {name_at, 0, 0, 0}); };
    std::string error;
    if (!run_job(s, kernel::system_process(*s.k), "the launch", job, seconds, error))
        return error.empty() ? "XamLoaderLaunchTitleEx did not return" : error;
    wait_quiet(s, seconds);
    size_t started = 0;
    {
        std::lock_guard lock(s.recorder.mutex);
        for (const Call &c : s.recorder.calls)
            if (c.name == "xboxkrnl!XexStartExecutable") started++;
    }
    return started ? "started" : "not started (no XexStartExecutable)";
}

// Ours: the title the kernel loads and starts, as boot does
// (the console's side too has the kernel hold the title, as the console's
// kernel does for whatever XAM launched: `mounts` false there, the hard drive
// being the writable Partition1 already)
std::string launch_title_hle(Side &s, const std::filesystem::path &dir, const std::filesystem::path &storage,
                             bool mounts) {
    std::string error;
    if (mounts && (!kernel::mount_directory(*s.k, dir, error) || !kernel::mount_storage(*s.k, storage, error)))
        return "mount: " + error;
    kernel::set_executable_path(*s.k, std::string("game:\\") + TITLE_FILE);
    if (!s.k->load(s.title, error) || !s.k->launch(error)) return error;
    return "started";
}

// `side <hle|real> <result> <Export> [args]`: one side's answer, in its file
int run_side(const std::vector<uint8_t> &file, bool real, const std::string &result, const std::string &name,
             std::vector<Arg> args, const Options &o) {
    g_result_path = result;
    std::remove(result.c_str());
    install_fault_handler();
    static Side s; // (left as it is at _Exit)
    g_side = &s;
    std::string error;
    if (!make_side(s, error)) {
        std::fprintf(stderr, "xfxam: %s\n", error.c_str());
        write_status_and_exit("setup " + error, 2);
    }
    s.k->step_limit = o.steps;
    uint32_t used = place_args(args);
    for (const Arg &a : args)
        if (a.memory) std::memcpy(s.k->host(uint32_t(a.value)), a.bytes.data(), a.bytes.size());
    uint32_t function = 0;
    std::string init_status = "none";
    if (real) {
        if (!load_xam(s, file, error)) write_status_and_exit("setup " + error, 2);
        uint16_t ordinal = xam_ordinal(name);
        function = ordinal ? kernel::module_export(*s.k, s.xam, ordinal) : 0;
        if (!function) write_status_and_exit("setup the console's XAM does not export " + name, 2);
        std::filesystem::path hdd = std::filesystem::path(result).replace_extension(".hdd");
        if (o.title && (!prepare_hdd(s, hdd, error) || !mount_hdd_real(s, hdd, o.launch)))
            write_status_and_exit("setup " + error, 2);
        if (o.init) init_status = run_init(s, o.init_seconds).status;
        if (o.init && o.title && !o.launch)
            init_status += "; the kernel's title: " + launch_title_hle(s, hdd, "", false);
        if (o.init && o.launch) init_status += "; the launch: " + launch_title_real(s, o.init_seconds);
        std::vector<std::string> prepare;
        if (o.init && o.title && !o.launch) prepare = title_state_for(s);
        prepare.insert(prepare.end(), o.prepare.begin(), o.prepare.end());
        if (o.init && !prepare.empty()) init_status += "; prepared:" + run_prepare(s, prepare, o.init_seconds);
        s.k->stopped = false; // (a start that stopped leaves the call its own chance)
    } else {
        std::filesystem::path hdd = std::filesystem::path(result).replace_extension(".hdd");
        if (o.title) {
            std::string status;
            if (!prepare_hdd(s, hdd, error)) write_status_and_exit("setup " + error, 2);
            status = launch_title_hle(s, hdd, std::filesystem::path(result).replace_extension(".storage"), true);
            if (status != "started") write_status_and_exit("setup the title: " + status, 2);
            // the console's XAM has no user (a profile is a package on a content
            // device it has not been given): ours is left with none either
            if (!o.user) kernel::set_signed_in(*s.k, false);
        }
        s.k->add_thunk(THUNK, "xam", name);
        function = THUNK;
    }
    if (FILE *f = std::fopen(result.c_str(), "wb")) {
        std::fprintf(f, "init %s\n", init_status.c_str());
        std::fclose(f);
    }
    std::vector<uint64_t> values;
    for (const Arg &a : args) values.push_back(a.value);
    s.recorder.phase = 1;
    uint32_t call_thread = 0;
    uint64_t r3 = 0;
    Job job;
    job.body = [&](kernel::Thread &self) {
        call_thread = self.id;
        r3 = guest_call(*s.k, *self.context, function, values);
    };
    uint32_t process = o.system ? kernel::system_process(*s.k) : kernel::title_process(*s.k);
    bool done = run_job(s, process, "the call", job, o.seconds, error);
    std::string status = !error.empty() ? "setup " + error
                         : !done        ? "timeout"
                         : s.k->stopped ? "stopped " + s.k->stop_reason
                         : job.exited   ? "exited"
                                        : "returned";
    FILE *f = std::fopen(result.c_str(), "ab");
    if (!f) return 2;
    std::fprintf(f, "r3 %016llX\n", static_cast<unsigned long long>(r3));
    std::fprintf(f, "region %s\n", hex(s.k->host(ARGS_BASE), used).c_str());
    {
        std::lock_guard lock(s.recorder.mutex);
        std::vector<Call> mine;
        for (const Call &c : s.recorder.calls)
            if (c.phase == 1 && c.thread == call_thread) mine.push_back(c);
        write_calls(f, mine);
    }
    std::fclose(f);
    write_status_and_exit(status, 0);
    return 0;
}

// ---- starting a side in a process of its own ----

std::string quote(const std::string &s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char ch : s) out += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    return out + "'";
#endif
}

std::string g_self;

int run_child(const std::string &command) {
#ifdef _WIN32
    // cmd /c strips the outer quotes of a line that starts with one
    int r = std::system(("\"" + command + "\"").c_str());
    return r;
#else
    int r = std::system(command.c_str());
    return WIFEXITED(r) ? WEXITSTATUS(r) : 128 + (WIFSIGNALED(r) ? WTERMSIG(r) : 0);
#endif
}

std::string read_text(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

Answer ask_side(const std::string &xex, bool real, const std::string &name, const std::vector<std::string> &args,
                const Options &o, const std::filesystem::path &work) {
    std::string tag = name + (real ? ".real" : ".hle");
    std::string result = (work / (tag + ".answer")).string(), log = (work / (tag + ".log")).string();
    std::string command =
        quote(g_self) + " " + quote(xex) + " side " + (real ? "real " : "hle ") + quote(result) + " " + name;
    for (const std::string &a : args) command += " " + quote(a);
    command += format(" --seconds %g --init-seconds %g --steps %llu%s%s%s%s", o.seconds, o.init_seconds,
                      static_cast<unsigned long long>(o.steps), o.init ? "" : " --no-init",
                      o.title ? "" : " --no-title", o.system ? " --system" : "", g_trace ? " --trace" : "");
    if (o.launch) command += " --launch";
    if (o.user) command += " --user";
    if (!o.prepare.empty()) {
        std::string joined;
        for (const std::string &p : o.prepare) joined += (joined.empty() ? "" : ",") + p;
        command += " --prepare " + quote(joined);
    }
    command += " > " + quote(log) + " 2>&1";
    int code = run_child(command);
    Answer a;
    if (!read_answer(result, a) || a.status.empty()) {
        char text[64];
        std::snprintf(text, sizeof(text), "fault (the process ended with %d, no answer)", code);
        a.status = text;
    }
    a.log = read_text(log);
    // our kernel ends a run when the title ends (exits, launches another, asks
    // for a disc): the thread that called did not come back, as a call that
    // ends its thread on the console's side
    if (a.status.rfind("stopped the title", 0) == 0) a.status = "exited";
    return a;
}

// ---- comparing the answers ----

std::string describe_calls(const std::vector<Call> &calls, size_t limit) {
    std::string out;
    for (size_t i = 0; i < calls.size() && i < limit; i++) {
        char line[160];
        std::snprintf(line, sizeof(line), "      %s(0x%X, 0x%X, 0x%X, 0x%X) -> %s0x%08X\n", calls[i].name.c_str(),
                      calls[i].args[0], calls[i].args[1], calls[i].args[2], calls[i].args[3],
                      calls[i].returned ? "" : "(no return) ", calls[i].result);
        out += line;
    }
    if (calls.size() > limit) out += "      ... " + std::to_string(calls.size() - limit) + " more\n";
    return out;
}

bool returned(const Answer &a) { return a.status == "returned"; }

// which argument's memory holds the offset in the range, and where in it
std::string where_in_args(const std::vector<Arg> &args, uint32_t offset) {
    for (size_t i = 0; i < args.size(); i++) {
        if (!args[i].memory) continue;
        uint32_t start = uint32_t(args[i].value) - ARGS_BASE;
        if (offset >= start && offset < start + args[i].bytes.size() + 16) {
            char text[48];
            std::snprintf(text, sizeof(text), "arg %zu +0x%X", i, offset - start);
            return text;
        }
    }
    return "between the arguments";
}

// the bytes that differ, as runs (at most a few, each at most 32 bytes shown)
std::string memory_diff(const std::vector<Arg> &args, const std::vector<uint8_t> &ours,
                        const std::vector<uint8_t> &real, size_t &runs) {
    std::string out;
    runs = 0;
    size_t n = std::min(ours.size(), real.size());
    for (size_t i = 0; i < n;) {
        if (ours[i] == real[i]) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && (ours[j] != real[j] || (j + 1 < n && ours[j + 1] != real[j + 1]))) j++;
        if (runs++ < 6) {
            size_t shown = std::min<size_t>(j - i, 32);
            const char *more = j - i > shown ? " ..." : "";
            out += "      " + where_in_args(args, uint32_t(i)) + ", " + std::to_string(j - i) + " bytes\n";
            out += "        ours " + hex(&ours[i], shown) + more + "\n";
            out += "        real " + hex(&real[i], shown) + more + "\n";
        }
        i = j;
    }
    if (runs > 6) out += "      ... " + std::to_string(runs - 6) + " more runs\n";
    return out;
}

enum class Verdict { Matches, Differs, NeedsState, Faults };
const char *verdict_name(Verdict v) {
    switch (v) {
    case Verdict::Matches: return "matches";
    case Verdict::Differs: return "differs";
    case Verdict::NeedsState: return "needs state";
    case Verdict::Faults: return "faults";
    }
    return "?";
}

// a handle (Xenia's numbering, which the kernel's follows): two answers that
// are both handles are the same answer, whatever the numbers (XAM's start
// made objects of its own before the call); and a pointer into the heaps the
// kernel and XAM allocate from (0x30000000-0x4FFFFFFF: pool, XAM's own heaps),
// which two sides with another history of allocations place elsewhere
bool is_handle(uint32_t v) { return (v & 0xFFF00000) == 0xF8000000 || (v >= 0x30000000 && v < 0x50000000); }

// the arguments' memory with the words that are a handle on both sides made
// equal
void normalize_handles(std::vector<uint8_t> &a, std::vector<uint8_t> &b) {
    auto word = [](const std::vector<uint8_t> &m, size_t i) {
        return uint32_t(m[i]) << 24 | uint32_t(m[i + 1]) << 16 | uint32_t(m[i + 2]) << 8 | m[i + 3];
    };
    for (size_t i = 0; i + 4 <= std::min(a.size(), b.size()); i += 4)
        if (is_handle(word(a, i)) && is_handle(word(b, i))) std::copy_n(&a[i], 4, &b[i]);
}

// The answers that say XAM lacks what its start (DllMain alone, no title
// launched through it, no profile, no storage) did not make: no user signed
// in (ERROR_NO_SUCH_USER, as a code or an HRESULT), and no memory for the
// call (ERROR_OUTOFMEMORY, E_OUTOFMEMORY, ERROR_NO_SYSTEM_RESOURCES,
// STATUS_NO_MEMORY, STATUS_INSUFFICIENT_RESOURCES: the heap XAM makes for a
// title as it launches one).
bool missing_state(uint32_t r) {
    switch (r) {
    case 0x00000525:
    case 0x80070525:
    case 0x0000000E:
    case 0x8007000E:
    case 0x000005AA:
    case 0xC0000017:
    case 0xC000009A: return true;
    default: return false;
    }
}

// how the two answers compare (`side`: whose fault, for a fault), and the
// text that says so
Verdict compare(const std::string &name, const std::vector<Arg> &args, const Answer &ours, const Answer &real,
                std::string &text, std::string &side) {
    std::string arg_text;
    for (const Arg &a : args) arg_text += (arg_text.empty() ? "" : ", ") + a.text;
    text = name + "(" + arg_text + ")\n";
    text += format("  ours: %s, r3 0x%08X\n", ours.status.c_str(), uint32_t(ours.r3));
    text +=
        format("  real: %s, r3 0x%08X (its start: %s)\n", real.status.c_str(), uint32_t(real.r3), real.init.c_str());
    Verdict v;
    side.clear();
    bool real_failed = !returned(real) && real.status != "timeout" && real.status != "exited";
    bool ours_failed = !returned(ours) && ours.status != "exited";
    if (real.status == "exited" && ours.status == "exited") {
        v = Verdict::Matches; // both ended the calling thread
    } else if (real_failed || ours_failed) {
        v = Verdict::Faults;
        side = real_failed && ours_failed ? "both" : real_failed ? "real" : "ours";
    } else if (real.status == "timeout") {
        v = Verdict::NeedsState;
    } else if (returned(ours) != returned(real)) {
        v = Verdict::Differs; // one ended its thread, the other returned
    } else {
        std::vector<uint8_t> a = ours.region, b = real.region;
        normalize_handles(a, b);
        size_t runs = 0;
        std::string diff = memory_diff(args, a, b, runs);
        uint32_t r_ours = uint32_t(ours.r3), r_real = uint32_t(real.r3);
        bool same_result = r_ours == r_real || (is_handle(r_ours) && is_handle(r_real));
        // a thread's ID is a count of the threads made before it: XAM's start
        // made some on one side only
        if (name == "GetCurrentThreadId" || name == "GetCurrentProcessId") same_result = true;
        v = same_result && runs == 0 ? Verdict::Matches : Verdict::Differs;
        if (!same_result && missing_state(r_real)) {
            v = Verdict::NeedsState;
            text += "  the real one's answer says what its start did not set up (no user, no memory)\n";
        }
        if (!same_result) text += "  the results differ\n";
        if (runs) text += "  the arguments' memory differs:\n" + diff;
    }
    text += "  kernel calls: ours " + std::to_string(ours.calls.size()) + ", real " +
            std::to_string(real.calls.size()) + "\n";
    text += "    ours:\n" + describe_calls(ours.calls, 12);
    text += "    real:\n" + describe_calls(real.calls, 40);
    return v;
}

// ---- the survey's arguments, from each export's parameters (Xenia's) ----

struct Signature {
    const char *name;
    int mark; // Xenia's: 0 implemented, 1 sketchy, 2 stub
    const char *params;
};
#define XF_XAM_SIGNATURE(name, mark, params) {#name, mark, params},
const Signature signatures[] = {
#include "xam_signatures.inc"
};
#undef XF_XAM_SIGNATURE

const Signature *find_signature(const std::string &name) {
    for (const Signature &s : signatures)
        if (name == s.name) return &s;
    return nullptr;
}

bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

// A value for each parameter, as a game would pass one: user 0, sizes 0x100,
// counts 1, a zeroed 4 KB buffer for each pointer (a dword named as one
// too), no overlapped (the call completes before it returns), 0 otherwise.
std::vector<std::string> survey_args(const Signature *sig) {
    std::vector<std::string> out;
    if (!sig) return out;
    std::stringstream in(sig->params);
    std::string param;
    while (std::getline(in, param, ',')) {
        size_t first = param.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        param = param.substr(first);
        size_t space = param.find_last_of(' ');
        std::string type = space == std::string::npos ? param : param.substr(0, space);
        std::string name = space == std::string::npos ? "" : param.substr(space + 1);
        std::string lower;
        for (char ch : name) lower += char(std::tolower(uint8_t(ch)));
        bool pointer = type.starts_with("lp") || type.starts_with("pointer_t");
        if (has(type, "OVERLAPPED") || has(lower, "overlapped"))
            out.push_back("0");
        else if (pointer)
            out.push_back("buf:0x1000");
        else if (has(lower, "user_index") || lower == "user" || has(lower, "user_idx"))
            out.push_back("0");
        else if (has(lower, "size") || has(lower, "length") || has(lower, "len") || has(lower, "cch") ||
                 lower.starts_with("cb") || has(lower, "bytes"))
            out.push_back("0x100");
        else if (has(lower, "count"))
            out.push_back("1");
        else if (lower.ends_with("ptr") || has(lower, "buffer") || has(lower, "address") || has(lower, "addr"))
            out.push_back("buf:0x1000");
        else
            out.push_back("0");
    }
    return out;
}

// The exports the generic arguments say nothing about (a user count of 0, a
// zeroed XUID or content structure that every check turns away before the
// state is looked at), run again with the arguments a title passes: the
// synthetic title's ID (0x58450001), user 0, a count of 1, a saved game's
// content structure. The survey runs each as a case of its own after the
// generic ones and tallies them apart. Words are split at spaces.
struct Variant {
    const char *name;
    std::string args;
};

// X_CONTENT_DATA (0x134 bytes) of a saved game on device 1: the device, the
// type (1), the display name zeroed, the file name "SaveFile"
std::string saved_game() { return "buf:0x134:0000000100000001" + std::string(512, '0') + "5361766546696C65"; }

const std::vector<Variant> variants = {
    {"XamShowSigninUI", "1 0"},
    {"XamShowSigninUIp", "0 1 0"},
    {"XamUserCreateAchievementEnumerator", "0x58450001 0 0 0 0 1 buf:16 buf:16"},
    {"XamUserCreateTitlesPlayedEnumerator", "0x58450001 0 0 0 1 buf:16 buf:16"},
    {"XamUserAreUsersFriends", "0 buf:16:0009000000000001 1 buf:16 0"},
    {"XamUserCheckPrivilege", "0 0x2 buf:16"},
    {"XamUserLogon", "buf:16:0009000000000001 0 0"},
    {"XamContentCreateEnumerator", "0xFF 1 1 0 1 buf:16 buf:16"},
    {"XamContentCreateDeviceEnumerator", "1 0 1 buf:16 buf:16"},
    {"XamContentGetDeviceData", "1 buf:0x100"},
    {"XamContentGetDeviceState", "1 0"},
    {"XamContentCreateEx", "0 str:save " + saved_game() + " 1 buf:16 buf:16 0 0x10000 0"},
    {"XamVoiceCreate", "0 0xF buf:16"},
    {"XamAlloc", "0 0x100 buf:16"},
};

// ---- the commands ----

int cmd_imports(const std::vector<uint8_t> &file, bool all) {
    Xex xex;
    std::string error;
    if (!parse_xex(file, xex, error) || !load_xex_image(file, xex, error)) {
        std::fprintf(stderr, "xfxam: %s\n", error.c_str());
        return 1;
    }
    size_t functions = 0, variables = 0, ours = 0;
    std::vector<std::string> missing, unnamed, every;
    for (const XexImport &im : list_imports(xex)) {
        // every import has a descriptor record, a function a thunk too
        if (im.thunk) continue;
        const std::string &library = xex.import_libraries[im.library].name;
        const Export *e = find_export(library, im.ordinal);
        if (!e) {
            char text[64];
            std::snprintf(text, sizeof(text), "%s ordinal 0x%03X", library.c_str(), im.ordinal);
            unnamed.push_back(text);
            continue;
        }
        if (e->kind == ExportKind::Variable) {
            variables++;
            continue;
        }
        functions++;
        std::string lib = library.substr(0, library.find('.'));
        if (Kernel::implemented(lib, e->name))
            ours++;
        else
            missing.push_back(lib + "!" + e->name);
        if (all) every.push_back(lib + "!" + e->name);
    }
    std::printf("imports: %zu functions (%zu implemented here), %zu variables, %zu no table names\n", functions, ours,
                variables, unnamed.size());
    std::printf("not implemented here (%zu):\n", missing.size());
    for (const std::string &m : missing) std::printf("  %s\n", m.c_str());
    for (const std::string &u : unnamed) std::printf("  %s (no name)\n", u.c_str());
    if (all) {
        std::printf("every function it imports (%zu):\n", every.size());
        for (const std::string &m : every) std::printf("  %s\n", m.c_str());
    }
    return 0;
}

int cmd_init(const std::vector<uint8_t> &file, const Options &o, const std::filesystem::path &work) {
    install_fault_handler();
    static Side s;
    g_side = &s;
    g_result_path = (work / "init.answer").string();
    std::string error;
    if (!make_side(s, error) || !load_xam(s, file, error)) {
        std::fprintf(stderr, "xfxam: %s\n", error.c_str());
        return 1;
    }
    s.k->step_limit = o.steps;
    std::filesystem::path hdd = work / "init.hdd";
    if (o.title && (!prepare_hdd(s, hdd, error) || !mount_hdd_real(s, hdd, o.launch))) {
        std::fprintf(stderr, "xfxam: %s\n", error.c_str());
        return 1;
    }
    auto start = std::chrono::steady_clock::now();
    Init init = run_init(s, o.init_seconds);
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("XAM's start: %s (%.2f s)\n", init.status.c_str(), wall);
    if (o.launch) std::printf("the launch: %s\n", launch_title_real(s, o.init_seconds).c_str());
    if (o.title && !o.launch) std::printf("the kernel's title: %s\n", launch_title_hle(s, hdd, "", false).c_str());
    std::vector<std::string> prepare;
    if (o.title && !o.launch) prepare = title_state_for(s);
    prepare.insert(prepare.end(), o.prepare.begin(), o.prepare.end());
    if (!prepare.empty()) std::printf("prepared:%s\n", run_prepare(s, prepare, o.init_seconds).c_str());
    if (s.k->stopped) std::printf("stopped: %s\n", s.k->stop_reason.c_str());
    {
        std::lock_guard lock(s.recorder.mutex);
        std::map<uint32_t, size_t> per_thread;
        for (const Call &c : s.recorder.calls) per_thread[c.thread]++;
        std::printf("kernel calls: %zu on %zu threads\n", s.recorder.calls.size(), per_thread.size());
        // the start's own thread, in order (the last ones say where it is)
        uint32_t first = s.recorder.calls.empty() ? 0 : s.recorder.calls.front().thread;
        std::vector<Call> mine;
        for (const Call &c : s.recorder.calls)
            if (c.thread == first) mine.push_back(c);
        std::printf("the start's thread (%u), %zu calls; the first 60 and the last 40:\n", first, mine.size());
        for (size_t i = 0; i < mine.size(); i++)
            if (i < 60 || i + 40 >= mine.size())
                std::printf("%s", describe_calls({mine[i]}, 1).c_str());
            else if (i == 60)
                std::printf("      ...\n");
    }
    std::printf("calls by export:\n");
    for (const auto &[name, count] : s.k->calls())
        std::printf("  %-48s %llu%s\n", name.c_str(), static_cast<unsigned long long>(count),
                    s.k->unimplemented().count(name) ? "  (not implemented)" : "");
    std::printf("%s", s.k->thread_report().c_str());
    std::printf("%s", kernel::modules_report(*s.k).c_str());
    std::printf("interpreted entries: %llu\n", static_cast<unsigned long long>(s.k->interpreted_entries()));
    std::fflush(stdout);
    std::_Exit(0);
}

int cmd_call(const std::string &xex, const std::string &name, const std::vector<std::string> &arg_texts,
             const Options &o, const std::filesystem::path &work) {
    std::vector<Arg> args(arg_texts.size());
    std::string error;
    for (size_t i = 0; i < args.size(); i++)
        if (!parse_arg(arg_texts[i], args[i], error)) {
            std::fprintf(stderr, "xfxam: %s\n", error.c_str());
            return 1;
        }
    place_args(args);
    Answer ours = ask_side(xex, false, name, arg_texts, o, work);
    Answer real = ask_side(xex, true, name, arg_texts, o, work);
    std::string text, side;
    Verdict v = compare(name, args, ours, real, text, side);
    std::printf("%s  verdict: %s%s\n", text.c_str(), verdict_name(v), side.empty() ? "" : (" (" + side + ")").c_str());
    return v == Verdict::Matches ? 0 : 3;
}

int cmd_survey(const std::vector<uint8_t> &file, const std::string &xex, const Options &o,
               const std::filesystem::path &work, const std::string &out_path, const std::set<std::string> &only) {
    // the exports both have: ours, and those the console's module exports
    Side s;
    std::string error;
    if (!make_side(s, error) || !load_xam(s, file, error)) {
        std::fprintf(stderr, "xfxam: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> names;
    size_t ours_only = 0;
    for (const auto &[name, ordinal] : our_xam_exports()) {
        if (!only.empty() && !only.count(name)) continue;
        if (kernel::module_export(*s.k, s.xam, ordinal))
            names.push_back(name);
        else
            ours_only++;
    }
    std::printf("%zu exports both have (%zu of ours the console's XAM does not export)\n", names.size(), ours_only);
    FILE *out = out_path.empty() ? stdout : std::fopen(out_path.c_str(), "wb");
    if (!out) return 1;
    std::map<Verdict, std::vector<std::string>> by, by_variant;
    std::map<std::string, size_t> faults_by;
    // one case: the export with the arguments, both sides, compared, written
    auto run_case = [&](const std::string &name, const std::string &label, const std::vector<std::string> &texts,
                        std::map<Verdict, std::vector<std::string>> &tally) {
        const Signature *sig = find_signature(name);
        std::vector<Arg> args(texts.size());
        std::string parse_error;
        for (size_t i = 0; i < args.size(); i++) parse_arg(texts[i], args[i], parse_error);
        place_args(args);
        Answer ours = ask_side(xex, false, name, texts, o, work);
        Answer real = ask_side(xex, true, name, texts, o, work);
        std::string text, side;
        Verdict v = compare(name, args, ours, real, text, side);
        tally[v].push_back(label);
        if (&tally == &by && !side.empty()) faults_by[side]++;
        std::string verdict = verdict_name(v) + (side.empty() ? "" : " (" + side + ")");
        std::fprintf(out, "%s  verdict: %s%s\n\n", text.c_str(), verdict.c_str(),
                     sig ? (sig->mark == 2   ? "; Xenia's is a stub"
                            : sig->mark == 1 ? "; Xenia's is sketchy"
                                             : "")
                         : "; no signature in Xenia");
        std::fflush(out);
        std::printf("%-44s %-15s ours %-9s 0x%08X  real %-9s 0x%08X\n", label.c_str(), verdict.c_str(),
                    ours.status.substr(0, ours.status.find(' ')).c_str(), uint32_t(ours.r3),
                    real.status.substr(0, real.status.find(' ')).c_str(), uint32_t(real.r3));
        std::fflush(stdout);
    };
    for (const std::string &name : names) run_case(name, name, survey_args(find_signature(name)), by);
    // the exports the generic arguments say nothing about, with arguments a
    // title would pass (their own tally: the generic one above stays the
    // survey's count)
    std::map<std::string, int> variant_count;
    for (const Variant &variant : variants) {
        if (!std::count(names.begin(), names.end(), variant.name)) continue;
        std::string label = std::string(variant.name) + "#" + std::to_string(++variant_count[variant.name]);
        std::vector<std::string> texts;
        std::stringstream in(variant.args);
        for (std::string word; in >> word;) texts.push_back(word);
        run_case(variant.name, label, texts, by_variant);
    }
    std::fprintf(out, "summary:");
    for (Verdict v : {Verdict::Matches, Verdict::Differs, Verdict::NeedsState, Verdict::Faults})
        std::fprintf(out, " %s %zu,", verdict_name(v), by[v].size());
    std::fprintf(out, " of %zu\n", names.size());
    std::fprintf(out, "variants:");
    for (Verdict v : {Verdict::Matches, Verdict::Differs, Verdict::NeedsState, Verdict::Faults})
        std::fprintf(out, " %s %zu,", verdict_name(v), by_variant[v].size());
    std::fprintf(out, "\n");
    if (out != stdout) std::fclose(out);
    for (Verdict v : {Verdict::Matches, Verdict::Differs, Verdict::NeedsState, Verdict::Faults})
        std::printf("%s: %zu\n", verdict_name(v), by[v].size());
    for (const auto &[side, count] : faults_by) std::printf("  faults (%s): %zu\n", side.c_str(), count);
    std::printf("variants (arguments a title would pass, %zu cases):", variants.size());
    for (Verdict v : {Verdict::Matches, Verdict::Differs, Verdict::NeedsState, Verdict::Faults})
        std::printf(" %s %zu,", verdict_name(v), by_variant[v].size());
    std::printf("\n");
    std::fflush(stdout);
    std::_Exit(0);
}

void usage() {
    std::printf("usage: xfxam <xam.xex> imports\n"
                "       xfxam <xam.xex> init [--init-seconds N]\n"
                "       xfxam <xam.xex> call <Export> [arg...] [--seconds N] [--no-init]\n"
                "       xfxam <xam.xex> survey [--out <file>] [--only A,B] [--seconds N]\n"
                "  an argument: a number, or memory at the same address on both sides:\n"
                "  buf:<size>[:<hex>], fill:<size>:<byte>, u32:<v>, u64:<v>, str:<text>, wstr:<text>\n"
                "  --work <dir>   where the sides' answers and logs go (the temporary directory)\n"
                "  --system       the call on a system thread (else a title's, as a game makes it)\n"
                "  --trace        every kernel call printed as it is made (to the side's log)\n"
                "  --steps N      the interpreter's instructions per entry\n"
                "  --no-title     no title running (default: the synthetic one, Partition1 a host directory)\n"
                "  --user         ours keeps its signed-in user (default: signed out, as the console's XAM is)\n"
                "  --prepare A[:x],B  XAM functions (hex addresses) called after its start, as a title's state\n"
                "  --launch       XAM's own XamLoaderLaunchTitleEx instead of that state (it stops at the\n"
                "                 old title's end, ExTerminateTitleProcess; docs/runtime.md, \"A title running\")\n"
                "  --only A,B     survey: these exports only (or @file, one per line); imports --all lists them\n");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 1;
    }
    g_self = std::filesystem::absolute(argv[0]).string();
    std::string xex = argv[1], command = argv[2];
    Options o;
    std::filesystem::path work = std::filesystem::temp_directory_path() / "xfxam";
    std::string out_path;
    std::set<std::string> only;
    std::vector<std::string> rest;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc)
            o.seconds = std::atof(argv[++i]);
        else if (a == "--init-seconds" && i + 1 < argc)
            o.init_seconds = std::atof(argv[++i]);
        else if (a == "--no-init")
            o.init = false;
        else if (a == "--no-title")
            o.title = false;
        else if (a == "--launch")
            o.launch = true;
        else if (a == "--user")
            o.user = true;
        else if (a == "--prepare" && i + 1 < argc) {
            std::stringstream in(argv[++i]);
            for (std::string n; std::getline(in, n, ',');) o.prepare.push_back(n);
        } else if (a == "--trace")
            g_trace = true;
        else if (a == "--system")
            o.system = true;
        else if (a == "--steps" && i + 1 < argc)
            o.steps = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--work" && i + 1 < argc)
            work = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            out_path = argv[++i];
        else if (a == "--only" && i + 1 < argc) {
            // names separated by commas, or @file with them (also one per line)
            std::string list = argv[++i];
            if (list[0] == '@') list = read_text(list.substr(1));
            for (char &ch : list)
                if (ch == '\n' || ch == '\r') ch = ',';
            std::stringstream in(list);
            for (std::string n; std::getline(in, n, ',');)
                if (!n.empty()) only.insert(n);
        } else
            rest.push_back(a);
    }
    std::vector<uint8_t> file;
    if (!read_file(xex, file)) {
        std::fprintf(stderr, "xfxam: cannot read %s\n", xex.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(work, ec);
    if (command == "imports") return cmd_imports(file, !rest.empty() && rest[0] == "--all");
    if (command == "init") return cmd_init(file, o, work);
    if (command == "call" && !rest.empty())
        return cmd_call(xex, rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()), o, work);
    if (command == "survey") return cmd_survey(file, xex, o, work, out_path, only);
    if (command == "side" && rest.size() >= 3 && (rest[0] == "hle" || rest[0] == "real")) {
        std::vector<Arg> args(rest.size() - 3);
        std::string error;
        for (size_t i = 0; i < args.size(); i++)
            if (!parse_arg(rest[3 + i], args[i], error)) {
                std::fprintf(stderr, "xfxam: %s\n", error.c_str());
                return 1;
            }
        return run_side(file, rest[0] == "real", rest[1], rest[2], args, o);
    }
    usage();
    return 1;
}
