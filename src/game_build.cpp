// The recompile step (xenoforge/game_build.h; docs/runtime.md, "The game
// library"): a disc's executables discovered and emitted in process into a
// work directory under the cache, compiled by clang a file per process (4 at
// once by default) against the generated code's headers as this XenoForge
// has them (written beside the code), linked by lld into a library that
// imports nothing, and kept in the cache by the executable's digest and the
// interface version.
//
// It never fails whole on the game's code (docs/plan.md, D2): a function the
// emitter cannot emit is a stub that dispatches to the interpreter
// (recomp.h's emit_modules), and a file clang rejects is split into parts,
// compiled again, and the failing parts split again until the functions
// clang rejects are found (bounded: 8 parts a round, 4 rounds, 256 extra
// compiles in all, past which a failing part's functions are all left out);
// those become stubs too, and their module's table holds no code for them.
// The addresses the runtime logged entering the interpreter (EntryLog, D3)
// are discovery's extra entries.

#include "xenoforge/game_build.h"

#include "game_build_flags.h"

#include "xenoforge/game_interface.h" // XF_GAME_INTERFACE_VERSION, made by the build
#include "xenoforge/disc.h"
#include "xenoforge/game_source.h"
#include "xenoforge/recomp.h"
#include "xenoforge/shader_container.h"
#include "xenoforge/shader_storage.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// (after windows.h) K32GetProcessMemoryInfo, in kernel32
#include <psapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace xf::build {
namespace {

namespace fs = std::filesystem;

// The library has no C runtime, and on Windows the compiler calls the
// runtime's stack probe in a function whose frame is above a page (a guest
// function with a large frame of locals, seldom): the frame's size in rax,
// each page down to the new stack pointer touched so the guard page moves,
// every register but r10 and r11 kept. Defined in the entry's file.
const std::string STACK_PROBE = R"xf(
#if defined(_WIN32) && defined(__x86_64__)
__asm__(".globl __chkstk\n"
        "__chkstk:\n"
        "    movq %rsp, %r10\n"
        "    movq %rax, %r11\n"
        "1:  cmpq $0x1000, %r11\n"
        "    jbe 2f\n"
        "    subq $0x1000, %r10\n"
        "    testq %r10, (%r10)\n"
        "    subq $0x1000, %r11\n"
        "    jmp 1b\n"
        "2:  subq %r11, %r10\n"
        "    testq %r10, (%r10)\n"
        "    ret\n");
#endif
)xf";

// the headers the generated code compiles against, as this build has them
// (cmake/game_interface.cmake)
struct Header {
    const char *name;
    const char *text;
};
const Header HEADERS[] = {
#include "game_headers.inc"
};

#ifdef _WIN32
constexpr const char *LIBRARY_EXTENSION = ".dll", *OBJECT_EXTENSION = ".obj", *EXE = ".exe";
#else
constexpr const char *LIBRARY_EXTENSION = ".so", *OBJECT_EXTENSION = ".o", *EXE = "";
#endif

// What each file is compiled with (game_build_flags.h) and the interface
// version
std::vector<std::string> compile_flags(CpuLevel level) {
    char version[64];
    std::snprintf(version, sizeof(version), "-DXF_GAME_INTERFACE_VERSION=0x%08Xu", XF_GAME_INTERFACE_VERSION);
    std::vector<std::string> flags;
    for (const char *f : base_compile_flags(level)) flags.push_back(f);
    flags.push_back(version);
    return flags;
}

std::string utf8(const fs::path &p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string hex_digest(uint64_t digest) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)digest);
    return b;
}
std::string hex_version() {
    char b[16];
    std::snprintf(b, sizeof(b), "%08x", XF_GAME_INTERFACE_VERSION);
    return b;
}

// <digest>-<interface version>-<CPU level>: a library is found only by a
// XenoForge of its interface, on a CPU of its level (cpu_level.h)
std::string library_stem(const std::string &digest, CpuLevel level) {
    return digest + "-" + hex_version() + "-" + cpu_level_name(level);
}
fs::path library_path(const fs::path &cache, const std::string &digest, CpuLevel level) {
    return cache / (library_stem(digest, level) + LIBRARY_EXTENSION);
}
// what the build left out and took in, beside the library
fs::path report_path(const fs::path &cache, const std::string &digest, CpuLevel level) {
    return cache / (library_stem(digest, level) + ".build.txt");
}
// a build report file (read_build_report)
std::optional<BuildReport> read_report(const fs::path &file);
// the level a build is for: the one asked for, else game_cpu_level()
CpuLevel build_level(const BuildOptions &options) {
    return options.cpu_level != CpuLevel::None ? options.cpu_level : game_cpu_level();
}
// why a build for `level` cannot be used here (empty when it can)
std::string level_refused(CpuLevel level) {
    if (cpu_has_level(level)) return "";
    if (host_cpu_level() == CpuLevel::None)
        return "this CPU lacks x86-64-v2 (SSE4.2, POPCNT), the oldest level the generated code is built for";
    return std::string("this CPU lacks the CPU level ") + cpu_level_name(level) + " asked for (it has " +
           cpu_level_name(host_cpu_level()) + ")";
}
constexpr const char *REPORT_EXTENSION = ".build.txt";

// a digest however it is written (16 hex digits, either case), as the cache
// names it; empty when it is not one
std::string normal_digest(std::string_view digest) {
    std::string text(digest);
    // the image's digest, and after a "+" the key of the code patches the library was built with
    std::string key;
    if (const size_t plus = text.find('+'); plus != std::string::npos) key = text.substr(plus + 1), text.resize(plus);
    auto valid = [](const std::string &s) {
        return s.size() == 16 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    };
    if (!valid(text) || (!key.empty() && !valid(key))) return {};
    std::string normal = hex_digest(std::strtoull(text.c_str(), nullptr, 16));
    if (!key.empty()) normal += "+" + hex_digest(std::strtoull(key.c_str(), nullptr, 16));
    return normal;
}

std::string read_file(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

bool write_report(const fs::path &path, const fs::path &library, const BuildReport &r) {
    std::string t = "# Written by the recompile step (game_build.h): what the game library " +
                    utf8(library.filename()) + " left out and took in.\n";
    t += "entries_taken " + std::to_string(r.entries_taken) + "\nentries " + std::to_string(r.entries) + "\n";
    for (const LeftOutFunction &f : r.left_out) {
        std::string reason;
        for (char ch : f.reason) reason += ch == '\n' || ch == '\r' || ch == '\t' ? ' ' : ch;
        t += "left\t" + f.module + "\t" + hex8(f.address) + "\t" + reason + "\n";
    }
    if (!r.project.empty()) t += "project " + r.project + "\n";
    for (const std::string &line : r.shaders) {
        std::string clean;
        for (char ch : line) clean += ch == '\n' || ch == '\r' ? ' ' : ch;
        t += "shaders\t" + clean + "\n";
    }
    std::ofstream f(path, std::ios::binary);
    f << t;
    return bool(f);
}

// ---- the generated files split by function (recomp.h's emit_modules gives the layout) ----

struct Chunk {
    uint32_t address = 0;
    std::string text; // its lines, from `XF_FN(...) {` to `}`
    // A function the port hooks (recomp.h's EmitHooks::slot): its body is
    // xfo_<address>, and the one-line wrapper xf_<address> that follows it
    // (XF_HOOKED) is kept in the same chunk, as one needs the other.
    bool hooked = false;
    std::string wrapper;
};
struct SplitFile {
    std::string head, tail; // up to the first function (the declarations); after the last
    std::vector<Chunk> chunks;
};

// false when the text is not a functions file as the emitter writes it
bool split_source(const std::string &text, SplitFile &out) {
    out = {};
    std::string pending, *current = nullptr;
    bool seen = false;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end + 1;
        const std::string line = text.substr(at, end - at);
        at = end;
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
            trimmed.pop_back();
        if (current) {
            *current += line;
            if (trimmed == "}") current = nullptr;
            continue;
        }
        // a hooked function's wrapper, right after its body: the body's chunk
        if (trimmed.starts_with("XF_FN(xf_") && trimmed.find("{ XF_HOOKED(") != std::string::npos &&
            !out.chunks.empty() && out.chunks.back().hooked && out.chunks.back().wrapper.empty() &&
            trimmed.find("xfo_" + hex8(out.chunks.back().address) + ")") != std::string::npos) {
            out.chunks.back().text += line;
            out.chunks.back().wrapper = line;
            continue;
        }
        // a project name's alias after a function (recomp_module.cpp's
        // alias_line): its chunk's
        if (!out.chunks.empty() && (trimmed == "#ifdef _MSC_VER" || trimmed == "#else" || trimmed == "#endif" ||
                                    (trimmed.starts_with("XF_FN(") && trimmed.find("__attribute__((alias(") !=
                                                                          std::string::npos))) {
            out.chunks.back().text += line;
            continue;
        }
        if (trimmed.starts_with("XF_FN(") && !trimmed.ends_with(";")) {
            size_t close = trimmed.find(')');
            if (close == std::string::npos || close < 6 + 8) return false;
            const std::string digits = trimmed.substr(close - 8, 8);
            if (digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
            if (!seen) out.head = pending, seen = true;
            pending.clear();
            out.chunks.push_back({uint32_t(std::strtoul(digits.c_str(), nullptr, 16)), line});
            out.chunks.back().hooked = trimmed.starts_with("XF_FN(xfo_");
            if (!trimmed.ends_with("}")) current = &out.chunks.back().text;
            continue;
        }
        pending += line;
    }
    if (current || !seen) return false;
    // (a hooked body without its wrapper: not the emitter's layout)
    for (const Chunk &c : out.chunks)
        if (c.hooked && c.wrapper.empty()) return false;
    out.tail = pending;
    return true;
}

// A project's digest: what it says (each module's starts, names, hooks,
// mid hooks, skips and jump tables), FNV-1a over a text of it, so the same
// project read from other files (includes, imported names) has the same one.
std::string digest_of(const recomp::Project &p) {
    std::string t;
    auto words = [&](const char *what, const auto &list) {
        t += what;
        for (uint32_t a : list) t += " " + hex8(a);
        t += "\n";
    };
    for (const recomp::ProjectModule &m : p.modules) {
        t += "module " + m.path + "\n";
        for (const auto &[a, end] : m.starts) t += "start " + hex8(a) + " " + hex8(end) + "\n";
        for (const auto &[a, name] : m.names) t += "name " + hex8(a) + " " + name + "\n";
        words("hooks", m.hooks);
        words("skip", m.skip);
        for (const auto &[a, jumps] : m.mid_hooks) words(("mid " + hex8(a)).c_str(), jumps);
        for (const auto &[a, targets] : m.jump_tables) words(("table " + hex8(a)).c_str(), targets);
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char ch : t) h = (h ^ ch) * 0x100000001B3ull;
    return hex_digest(h);
}

// the functions files a build must have left to compile for the generated
// code's header to be precompiled for them (Build::compile)
constexpr size_t PCH_MINIMUM_FILES = 4;

// The jobs policy's numbers (job_memory, job_policy): a compiler's memory
// by its source's size, the size of the largest ordinary functions file
// (64 KB of the image), and the compilers at once where the memory free
// cannot be read. Measured on three discs (docs/recompiler.md, "The
// recompile step's memory"): a functions file peaked at 128 MB plus 26 MB
// a megabyte of source (the median), 152 at most, so 160 MB and 192 bound
// every one with room.
constexpr uint64_t JOB_FIXED_BYTES = 160ull << 20;
constexpr uint64_t JOB_BYTES_PER_SOURCE_BYTE = 192;
constexpr uint64_t JOB_LARGEST_SOURCE_BYTES = 2ull << 20;
constexpr unsigned UNKNOWN_MEMORY_JOBS = 4;

bool is_functions_file(const fs::path &source) { return utf8(source.filename()).starts_with("functions_"); }

// clang's first error in its log, else its exit code
std::string first_error(const fs::path &log, int code) {
    std::ifstream f(log);
    std::string line;
    while (std::getline(f, line)) {
        size_t at = line.find("error: ");
        if (at == std::string::npos) continue;
        line = line.substr(at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 200) line = line.substr(0, 200) + "...";
        return line;
    }
    char b[64];
    std::snprintf(b, sizeof(b), "clang exited with code 0x%X", unsigned(code));
    return b;
}

bool write_if_changed(const fs::path &path, const std::string &text) {
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::string old((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (old == text) return true;
        }
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f << text;
    return bool(f);
}

std::string read_head(const fs::path &path, size_t lines) {
    std::ifstream f(path);
    std::string out, line;
    while (lines-- && std::getline(f, line)) out += line + "\n";
    return out;
}

fs::path executable_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    if (!n || n == std::size(buffer)) return {};
    return fs::path(std::wstring(buffer, n)).parent_path();
#else
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : self.parent_path();
#endif
}

// a directory with clang and lld in it
bool has_toolchain(const fs::path &dir) {
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir / (std::string("clang") + EXE), ec)) return false;
#ifdef _WIN32
    return fs::exists(dir / "lld-link.exe", ec) || fs::exists(dir / "lld.exe", ec);
#else
    return fs::exists(dir / "ld.lld", ec) || fs::exists(dir / "lld", ec);
#endif
}

// ---- processes: the compilers and the linker ----

#ifdef _WIN32
std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
// one argument as CommandLineToArgvW reads it back
std::string quote(const std::string &arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t slashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            slashes++;
            continue;
        }
        out.append(ch == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        out += ch;
    }
    out.append(slashes * 2, '\\');
    return out + "\"";
}
#else
std::string quote(const std::string &arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"'\\") == std::string::npos) return arg;
    std::string out = "\"";
    for (char ch : arg) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + "\"";
}
#endif

// A process with its output and errors in a log file, at a lower priority
// than the launcher's.
struct Process {
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    pid_t pid = -1;
#endif
    size_t job = 0;
    uint64_t expected = 0; // the memory it is expected to need at most (job_memory)
};

// what an ended process took: its CPU time and its peak working set
// (resident set)
struct ProcessStats {
    double cpu_seconds = 0;
    uint64_t peak_bytes = 0;
};

// the memory a running process holds now (its working set, resident set)
uint64_t memory_of(const Process &p) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!p.handle || !K32GetProcessMemoryInfo(p.handle, &counters, sizeof(counters))) return 0;
    return counters.WorkingSetSize;
#else
    if (p.pid < 0) return 0;
    std::ifstream f("/proc/" + std::to_string(p.pid) + "/statm");
    uint64_t size = 0, resident = 0;
    if (!(f >> size >> resident)) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? resident * uint64_t(page) : 0;
#endif
}

bool start(const fs::path &program, const std::vector<std::string> &args, const fs::path &log, Process &p,
           std::string &error, bool lowest = false) {
#ifdef _WIN32
    std::string line = quote(utf8(program));
    for (const std::string &a : args) line += " " + quote(a);
    std::wstring command = wide(line);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return error = "cannot write " + utf8(log), false;
    // only the log is inherited (another thread of the launcher may be
    // starting a process with handles of its own)
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attributes(size);
    auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    bool ok =
        InitializeProcThreadAttributeList(list, 1, 0, &size) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof(out), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = out;
    si.lpAttributeList = list;
    PROCESS_INFORMATION pi{};
    ok = ok && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | (lowest ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS) | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                              nullptr, &si.StartupInfo, &pi);
    DWORD why = GetLastError();
    if (size) DeleteProcThreadAttributeList(list);
    CloseHandle(out);
    if (!ok) return error = "cannot run " + utf8(program) + " (error " + std::to_string(why) + ")", false;
    CloseHandle(pi.hThread);
    p.handle = pi.hProcess;
    return true;
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    std::string log_path = log.string();
    posix_spawn_file_actions_addopen(&actions, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    std::string program_path = program.string();
    std::vector<std::string> all = {program_path};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (std::string &a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    int r = posix_spawn(&p.pid, program_path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (r != 0) return error = "cannot run " + program_path + ": " + std::strerror(r), false;
    if (lowest) setpriority(PRIO_PROCESS, id_t(p.pid), 19); // the lowest: a game plays meanwhile
    return true;
#endif
}

void end(Process &p) {
#ifdef _WIN32
    if (!p.handle) return;
    TerminateProcess(p.handle, 1);
    WaitForSingleObject(p.handle, 5000);
    CloseHandle(p.handle);
    p.handle = nullptr;
#else
    if (p.pid < 0) return;
    kill(p.pid, SIGKILL);
    int status = 0;
    waitpid(p.pid, &status, 0);
    p.pid = -1;
#endif
}

// Waits up to `ms` for one of the processes to end: its index, exit code and
// what it took, or -1 when none did.
int wait_any(std::vector<Process> &running, int ms, int &exit_code, ProcessStats *stats = nullptr) {
#ifdef _WIN32
    std::vector<HANDLE> handles;
    for (const Process &p : running) handles.push_back(p.handle);
    DWORD r = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, DWORD(ms));
    if (r >= WAIT_OBJECT_0 + handles.size()) return -1;
    int k = int(r - WAIT_OBJECT_0);
    DWORD code = 1;
    GetExitCodeProcess(running[size_t(k)].handle, &code);
    if (stats) {
        FILETIME created, exited, kernel, user;
        auto ticks = [](const FILETIME &t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        if (GetProcessTimes(running[size_t(k)].handle, &created, &exited, &kernel, &user))
            stats->cpu_seconds = double(ticks(kernel) + ticks(user)) * 1e-7;
        PROCESS_MEMORY_COUNTERS counters{};
        if (K32GetProcessMemoryInfo(running[size_t(k)].handle, &counters, sizeof(counters)))
            stats->peak_bytes = counters.PeakWorkingSetSize;
    }
    CloseHandle(running[size_t(k)].handle);
    running[size_t(k)].handle = nullptr;
    exit_code = int(code);
    return k;
#else
    for (int waited = 0;; waited += 20) {
        for (size_t k = 0; k < running.size(); k++) {
            int status = 0;
            struct rusage usage{};
            if (wait4(running[k].pid, &status, WNOHANG, &usage) == running[k].pid) {
                running[k].pid = -1;
                exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                if (stats) {
                    stats->cpu_seconds = double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                                         double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
                    stats->peak_bytes = uint64_t(usage.ru_maxrss) * 1024; // kilobytes on Linux
                }
                return int(k);
            }
        }
        if (waited >= ms) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
}

// ---- the build ----

struct Job {
    fs::path source, object, log;
    uintmax_t size = 0;
    int code = 0;        // clang's exit code
    bool linked = true;  // its object goes into the library (not a file split, or rejected)
};

// FNV-1a over some text (the objects' keys; not a defence against anyone)
uint64_t text_key(std::string_view text, uint64_t key = 0xCBF29CE484222325ull) {
    for (unsigned char ch : text) key = (key ^ ch) * 0x100000001B3ull;
    return (key ^ 0xFF) * 0x100000001B3ull;
}

// A header's code without its comments, blank lines and the spaces around
// lines, for the objects' key: an edit of the comments alone keeps the
// objects (no generated-code header uses __LINE__, so the lines it moves
// change nothing compiled). String and character literals are kept whole.
std::string code_of(std::string_view text) {
    std::string out, line;
    auto end_line = [&] {
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.append(line, a, b - a + 1).push_back('\n');
        line.clear();
    };
    for (size_t i = 0; i < text.size(); i++) {
        const char ch = text[i];
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') i++;
            end_line();
        } else if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            size_t close = text.find("*/", i + 2);
            i = close == std::string_view::npos ? text.size() : close + 1;
            line.push_back(' ');
        } else if (ch == '"' || ch == '\'') {
            line.push_back(ch);
            for (i++; i < text.size() && text[i] != ch && text[i] != '\n'; i++) {
                line.push_back(text[i]);
                if (text[i] == '\\' && i + 1 < text.size()) line.push_back(text[++i]);
            }
            if (i < text.size() && text[i] == ch) line.push_back(ch);
            else if (i < text.size()) end_line();
        } else if (ch == '\n') {
            end_line();
        } else {
            line.push_back(ch);
        }
    }
    end_line();
    return out;
}

// Beside each object, the key of the source it was compiled from: an object
// is reused when its source's text is the same (compile), whenever the
// source was written
fs::path source_key_path(const Job &j) {
    fs::path p = j.object;
    return p.replace_extension(".source");
}

// a file beside a job's, its object and log beside the job's
Job beside(const Job &j, const std::string &stem) {
    Job d;
    d.source = j.source.parent_path() / (stem + ".cpp");
    d.object = j.object.parent_path() / (stem + OBJECT_EXTENSION);
    d.log = j.object.parent_path() / (stem + ".log");
    return d;
}

// how a file clang rejects is split (recover)
constexpr size_t SPLIT_PARTS = 8, SPLIT_ROUNDS = 4, SPLIT_COMPILES = 256;

// The shader containers in a disc's files (shader_container.h's search, as
// xfeffect's), for the shader step: each file up to 64 MB but the media
// (video, sound), on a disc image or in a directory; each shader once.
void find_disc_shaders(const fs::path &disc, ShaderStep &step, const std::atomic<bool> *cancel) {
    const auto started = std::chrono::steady_clock::now();
    constexpr uint64_t LIMIT = 64ull << 20;
    std::unordered_set<uint64_t> seen;
    auto wanted = [&](const std::string &name, uint64_t size) {
        if (size < 64 || size > LIMIT || (cancel && cancel->load())) return false;
        std::string lower = name;
        for (char &ch : lower) ch = char(std::tolower(uint8_t(ch)));
        for (const char *media : {".xmv", ".wmv", ".bik", ".xma", ".wav", ".xwm", ".mp3", ".wma", ".xwb", ".xsb"})
            if (lower.ends_with(media)) return false;
        return true;
    };
    auto take = [&](std::span<const uint8_t> data) {
        step.disc_files++;
        for (ShaderContainer &c : find_shader_containers(data)) {
            const uint32_t type = c.vertex ? 0 : 1;
            if (seen.insert(ShaderStorage::hash(type, c.microcode)).second)
                step.disc_shaders.emplace_back(type, std::move(c.microcode));
        }
    };
    std::error_code ec;
    if (fs::is_directory(disc, ec)) {
        for (auto it = fs::recursive_directory_iterator(disc, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec) || !wanted(utf8(it->path().filename()), it->file_size(ec))) continue;
            const std::string bytes = read_file(it->path());
            take(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
        }
    } else {
        std::string why;
        if (std::unique_ptr<GameSource> image = GameSource::open(disc, why))
            for (const GameSource::File &file : image->files())
                if (wanted(file.path, file.size)) take(image->read_all(file));
    }
    step.disc_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// What find_disc_shaders found on a disc image, kept beside the library
// (<cache>/<digest>.discshaders) so a later build step reads it instead of
// the disc again (tens of seconds): a magic and version, the image's size
// and time, the files and seconds the search took, then each shader's type,
// length and microcode. Another image (size or time) searches again; a
// directory is searched each time.
constexpr uint32_t DISC_SHADERS_MAGIC = 0x53444658; // "XFDS"
constexpr uint32_t DISC_SHADERS_VERSION = 1;

fs::path disc_shaders_path(const fs::path &cache, const std::string &digest) {
    return cache / (digest + ".discshaders");
}

std::string disc_stamp(const fs::path &disc) {
    std::error_code ec;
    if (!fs::is_regular_file(disc, ec)) return {};
    const auto size = fs::file_size(disc, ec);
    if (ec) return {};
    const auto time = fs::last_write_time(disc, ec);
    if (ec) return {};
    return std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
}

bool read_disc_shaders(const fs::path &path, const std::string &stamp, ShaderStep &step) {
    if (stamp.empty()) return false;
    const std::string bytes = read_file(path);
    size_t at = 0;
    auto take = [&](void *out, size_t n) {
        if (bytes.size() - at < n) return false;
        std::memcpy(out, bytes.data() + at, n);
        at += n;
        return true;
    };
    uint32_t magic = 0, version = 0, stamp_size = 0, count = 0;
    uint64_t files = 0;
    double seconds = 0;
    if (!take(&magic, 4) || !take(&version, 4) || magic != DISC_SHADERS_MAGIC || version != DISC_SHADERS_VERSION ||
        !take(&stamp_size, 4) || bytes.size() - at < stamp_size || bytes.compare(at, stamp_size, stamp) != 0 ||
        stamp_size != stamp.size())
        return false;
    at += stamp_size;
    if (!take(&files, 8) || !take(&seconds, 8) || !take(&count, 4)) return false;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> shaders;
    shaders.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = 0, words = 0;
        if (!take(&type, 4) || !take(&words, 4) || (bytes.size() - at) / 4 < words) return false;
        std::vector<uint32_t> code(words);
        if (!take(code.data(), size_t(words) * 4)) return false;
        shaders.emplace_back(type, std::move(code));
    }
    if (at != bytes.size()) return false;
    step.disc_shaders = std::move(shaders);
    step.disc_files = size_t(files);
    step.disc_seconds = seconds;
    step.disc_from_earlier = true;
    return true;
}

void write_disc_shaders(const fs::path &path, const std::string &stamp, const ShaderStep &step) {
    if (stamp.empty()) return;
    std::string out;
    auto put = [&](const void *p, size_t n) { out.append(static_cast<const char *>(p), n); };
    const uint32_t magic = DISC_SHADERS_MAGIC, version = DISC_SHADERS_VERSION, stamp_size = uint32_t(stamp.size()),
                   count = uint32_t(step.disc_shaders.size());
    const uint64_t files = step.disc_files;
    put(&magic, 4), put(&version, 4), put(&stamp_size, 4), put(stamp.data(), stamp.size());
    put(&files, 8), put(&step.disc_seconds, 8), put(&count, 4);
    for (const auto &[type, code] : step.disc_shaders) {
        const uint32_t words = uint32_t(code.size());
        put(&type, 4), put(&words, 4), put(code.data(), code.size() * 4);
    }
    // whole or not at all: written beside, then put in place
    fs::path part = path;
    part += ".part";
    std::error_code ec;
    bool written;
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        written = bool(f.write(out.data(), std::streamsize(out.size())));
    }
    if (written) fs::rename(part, path, ec);
    if (!written || ec) fs::remove(part, ec);
}

class Build {
  public:
    Build(Progress &progress, const BuildOptions &options)
        : progress_(progress), options_(options), level_(build_level(options)) {}

    bool cancelled() const { return progress_.cancel && progress_.cancel->load(); }
    void report(Stage stage, uint64_t done, uint64_t total, std::string_view detail) {
        if (progress_.report) progress_.report(stage, done, total, detail);
    }
    bool fail(std::string &error, const std::string &why) {
        error = cancelled() ? "cancelled" : why;
        report(Stage::Failed, 0, 0, error);
        return false;
    }

    bool run(const fs::path &disc, const fs::path &cache, std::string &error);
    // the shader step (BuildOptions::shader_step), its lines into the report
    void shaders(const fs::path &disc, const fs::path &cache, const Xex &xex, const std::string &digest,
                 BuildReport &result);
    // the headers and the entry written into work, then sources (relative
    // to src_root) compiled and linked into out
    bool compile_and_link(const fs::path &src_root, const std::vector<std::string> &sources, const fs::path &work,
                          const fs::path &out, std::string &error);

    // what the build left out (the emission's and clang's), in order
    std::vector<LeftOutFunction> left_;
    // what each compiler took, and the jobs policy's line (BuildReport)
    std::vector<BuildReport::Compiled> compiled_;
    BuildReport::Compiled linked_; // the linker's (its file the library)
    JobPolicy policy_;

  private:
    // the jobs of `batch` compiled (what a stopped build compiled already
    // skipped); the ones clang rejects in `failed`. False only when cancelled
    // or a compiler cannot be started.
    bool compile(const fs::path &clang, const fs::path &work, std::vector<Job> &jobs, std::vector<size_t> batch,
                 std::vector<size_t> &failed, std::string &error);
    // the files clang rejected split until the functions it rejects are
    // found, those left out
    bool recover(const fs::path &clang, const fs::path &work, std::vector<Job> &jobs,
                 const std::vector<size_t> &failed, std::string &error);
    bool link(const fs::path &bin, const fs::path &work, const std::vector<Job> &jobs, const fs::path &out,
              std::string &error);
    // a module's path on the disc, by its directory under the emitted code
    std::string module_of(const fs::path &source) const {
        const std::string dir = utf8(source.parent_path().filename());
        auto it = module_paths_.find(dir);
        return it != module_paths_.end() ? it->second : dir;
    }
    // (BuildOptions::fail_compile) a line clang rejects put in those functions
    void force_failures(const std::vector<Job> &jobs);
    // whether another compiler, on `next`, starts beside the running ones
    // (JobPolicy): one always runs; past it the policy's limit, and unless
    // the jobs were given, the memory free now less what the running ones
    // are still expected to grow by (re-checked as they end and every tenth
    // of a second while one waits)
    bool has_room(const std::vector<Process> &running, const Job &next) const {
        if (running.empty()) return true;
        if (running.size() >= policy_.limit) return false;
        if (options_.jobs) return true;
        const uint64_t available = available_memory();
        if (!available) return true; // unknown: the limit alone
        uint64_t growth = 0;
        for (const Process &p : running) {
            const uint64_t now = memory_of(p);
            growth += p.expected > now ? p.expected - now : 0;
        }
        return available > growth + policy_.reserve_bytes + job_memory(next.size);
    }

    Progress &progress_;
    const BuildOptions &options_;
    const CpuLevel level_; // what the library is built for
    std::map<std::string, std::string> module_paths_;
};

bool Build::compile_and_link(const fs::path &src_root, const std::vector<std::string> &sources, const fs::path &work,
                             const fs::path &out, std::string &error) {
    std::string why;
    if (std::string refused = level_refused(level_); !refused.empty()) return fail(error, refused);
    std::optional<fs::path> bin = find_toolchain(options_.toolchain);
    if (!bin)
        return fail(error, "no clang and lld found (toolchain/bin beside the launcher, XF_TOOLCHAIN, an installed "
                           "LLVM or the PATH)");
    // the headers, as this XenoForge has them, and the library's entry
    for (const Header &h : HEADERS)
        if (!write_if_changed(work / "include" / h.name, h.text)) return fail(error, "cannot write the headers");
    // The objects a build left (kept with BuildOptions::keep_objects) are
    // reused when their source's text is the one they were compiled from (a
    // recompiler change rewrites only the files whose text it changes), by
    // the same compiler, against the same headers' code and flags: their
    // key is kept beside them, and another one discards them all. The
    // interface version is in the entry's text, the one file that uses it,
    // so that file alone is recompiled when only the version moved (a
    // header's comments edited, say).
    std::error_code ec;
    {
        uint64_t key = 0xCBF29CE484222325ull;
        auto mix = [&key](std::string_view s) { key = text_key(s, key); };
        for (const Header &h : HEADERS) mix(h.name), mix(code_of(h.text));
        for (const std::string &f : compile_flags(level_))
            if (!f.starts_with("-DXF_GAME_INTERFACE_VERSION=")) mix(f);
        // the compiler, by its size and time: another clang makes another
        // object of the same text
        const fs::path clang = *bin / (std::string("clang") + EXE);
        mix(std::to_string(fs::file_size(clang, ec)));
        mix(std::to_string(fs::last_write_time(clang, ec).time_since_epoch().count()));
        const fs::path stamp = work / "objects.key";
        const std::string text = hex_digest(key) + "\n";
        if (read_file(stamp) != text) {
            fs::remove_all(work / "obj", ec);
            if (!write_if_changed(stamp, text)) return fail(error, "cannot write " + utf8(stamp));
        }
    }
    const fs::path entry = work / "xf_library_entry.cpp";
    if (!write_if_changed(entry, "// Written by the recompile step: the game library's entry (game_library.h), for "
                                 "interface version " +
                                     hex_version() +
                                     ".\n#define XF_GAME_LIBRARY_ENTRY\n#include \"xenoforge/game_library.h\"\n" +
                                     STACK_PROBE))
        return fail(error, "cannot write " + utf8(entry));
    std::vector<Job> jobs;
    auto add = [&](const fs::path &source, const fs::path &relative) {
        Job j;
        j.source = source;
        j.object = work / "obj" / relative;
        j.object.replace_extension(OBJECT_EXTENSION);
        j.log = j.object;
        j.log.replace_extension(".log");
        j.size = fs::file_size(j.source, ec);
        if (ec) return false;
        jobs.push_back(j);
        return true;
    };
    for (const std::string &s : sources)
        if (!add(src_root / s, s)) return fail(error, "no " + utf8(src_root / s));
    add(entry, "xf_library_entry.cpp");
    if (!options_.fail_compile.empty()) force_failures(jobs);
    const fs::path clang = *bin / (std::string("clang") + EXE);
    policy_ = job_policy(options_.jobs);
    if (options_.background) {
        // a game plays meanwhile on the first tier: half the compilers, at the lowest priority
        policy_.limit = std::max(1u, policy_.limit / 2);
        policy_.why += "; halved to " + std::to_string(policy_.limit) + " while a game plays (at the lowest priority)";
    }
    std::vector<size_t> all(jobs.size()), failed;
    std::iota(all.begin(), all.end(), size_t(0));
    if (!compile(clang, work, jobs, all, failed, error)) return false;
    if (!failed.empty() && !recover(clang, work, jobs, failed, error)) return false;
    return link(*bin, work, jobs, out, error);
}

void Build::force_failures(const std::vector<Job> &jobs) {
    for (const Job &j : jobs) {
        if (!is_functions_file(j.source)) continue;
        std::string text = read_file(j.source);
        bool changed = false;
        for (uint32_t a : options_.fail_compile) {
            std::string head = "\nXF_FN(" + recomp::function_name("xfo_", a) + ") {"; // a hooked one's body
            size_t at = text.find(head);
            if (at == std::string::npos) at = text.find(head = "\nXF_FN(" + recomp::function_name("xf_", a) + ") {");
            if (at == std::string::npos) continue;
            size_t end = text.find('\n', at + 1);
            if (end == std::string::npos || text.compare(end - 1, 1, "}") == 0) continue; // a stub: one line
            const std::string bad = "    xf_forced_compile_failure; // BuildOptions::fail_compile\n";
            if (text.compare(end + 1, bad.size(), bad) == 0) continue;
            text.insert(end + 1, bad);
            changed = true;
        }
        if (changed) write_if_changed(j.source, text);
    }
}

bool Build::run(const fs::path &disc, const fs::path &cache, std::string &error) {
    std::string why;
    if (std::string refused = level_refused(level_); !refused.empty()) return fail(error, refused);
    report(Stage::Reading, 0, 0, utf8(disc.filename()));
    if (!options_.shaders_only && !find_toolchain(options_.toolchain))
        return fail(error, "no clang and lld found (toolchain/bin beside the launcher, XF_TOOLCHAIN, an installed "
                           "LLVM or the PATH)");
    std::vector<recomp::ModuleFile> modules;
    if (!recomp::find_modules(disc, {}, modules, why)) return fail(error, utf8(disc) + ": " + why);
    // the executable: default.xex on a disc, the file itself when bare
    const recomp::ModuleFile *executable = modules.size() == 1 ? &modules[0] : nullptr;
    for (const recomp::ModuleFile &m : modules) {
        std::string lower = m.path;
        for (char &ch : lower) ch = char(std::tolower(uint8_t(ch)));
        if (lower == "default.xex") executable = &m;
    }
    if (!executable) return fail(error, utf8(disc) + ": no default.xex");
    const std::string executable_path = executable->path;
    Xex xex;
    if (!parse_xex(executable->file, xex, why) || !load_xex_image(executable->file, xex, why))
        return fail(error, executable->path + ": " + why);
    // the enabled code patches: the library is cached by the unpatched image's digest and their
    // key, and they are written into each executable's file (patches::patched_file) before discovery
    const patches::Plan plan = patches::make_plan(patches::module_info(xex), options_.patches);
    for (const std::string &line : plan.log) report(Stage::Reading, 0, 0, line);
    const std::string digest = patches::cache_key(hex_digest(image_digest(xex.image)), plan);
    const fs::path library = library_path(cache, digest, level_);
    std::error_code ec;
    // the port's project file, read first: what it says is part of what the
    // cached library must have been made with
    recomp::Project project;
    std::string project_digest;
    if (!options_.project.empty()) {
        if (!recomp::load_project(options_.project, project, why)) return fail(error, why);
        for (const std::string &w : project.warnings) report(Stage::Reading, 0, 0, "project file: " + w);
        project_digest = digest_of(project);
    }
    if (options_.shaders_only && !fs::exists(library, ec)) {
        BuildReport found;
        if (options_.shader_step) shaders(disc, cache, xex, digest, found);
        if (options_.report) *options_.report = found;
        report(Stage::Done, 1, 1, "");
        return true;
    }
    const bool same_project = read_report(report_path(cache, digest, level_)).value_or(BuildReport{}).project == project_digest;
    if ((options_.shaders_only || (!options_.force && same_project)) && fs::exists(library, ec)) {
        BuildReport found = read_report(report_path(cache, digest, level_)).value_or(BuildReport{});
        if (options_.shader_step) {
            shaders(disc, cache, xex, digest, found);
            write_report(report_path(cache, digest, level_), library, found);
        }
        if (options_.report) *options_.report = found;
        report(Stage::Done, 1, 1, utf8(library));
        return true;
    }
    if (cancelled()) return fail(error, "cancelled");
    // the addresses earlier runs entered the interpreter at (D3), and the
    // ones given: discovery's extra entries
    BuildReport result;
    result.project = project_digest;
    std::vector<uint32_t> entries;
    if (options_.use_entry_log) entries = read_entry_log(entry_log_path(cache, digest));
    result.entries_taken = entries.size();
    entries.insert(entries.end(), options_.entries.begin(), options_.entries.end());
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    result.entries = entries.size();

    // the emitted code, in a work directory of its own (a build stopped
    // before goes on from the objects it left, and with keep_work another
    // XenoForge's build recompiles only the files whose text or headers
    // changed: compile_and_link)
    const fs::path work = cache / "work" / digest;
    const fs::path src = work / "src";
    fs::create_directories(src, ec);
    if (ec) return fail(error, "cannot make " + utf8(src) + ": " + ec.message());
    recomp::EmitOptions emit;
    emit.discover.entries = std::move(entries);
    if (!options_.project.empty()) emit.project = &project;
    emit.keep_going = true; // an executable that cannot be read is interpreted
    emit.progress = [&](recomp::EmitStage stage, size_t done, size_t total, const std::string &detail) {
        report(stage == recomp::EmitStage::Discovering ? Stage::Discovering : Stage::Emitting, done, total, detail);
        return !cancelled();
    };
    if (!options_.fail_emit.empty())
        emit.leave_out = [this](const std::string &, uint32_t address) {
            const auto &list = options_.fail_emit;
            return std::find(list.begin(), list.end(), address) != list.end()
                       ? std::string("the emission was made to fail (BuildOptions::fail_emit)")
                       : std::string();
        };
    std::vector<recomp::EmittedModule> emitted;
    // the executables the enabled code patches change are emitted from a copy of the file with the
    // patched image (the recompiler knows nothing of patches)
    for (recomp::ModuleFile &m : modules) {
        Xex x;
        if (!parse_xex(m.file, x, why) || !load_xex_image(m.file, x, why)) continue;
        const patches::Plan p = patches::make_plan(patches::module_info(x), options_.patches);
        if (!p.any_code()) continue;
        patches::apply_code(p, x.image, x.base_address);
        std::vector<uint8_t> patched;
        if (!patches::patched_file(m.file, x, patched))
            return fail(error, m.path + ": cannot apply the code patches to this executable's file");
        m.file = std::move(patched);
    }
    if (!recomp::emit_modules(modules, src, emit, emitted, why)) return fail(error, why);
    modules.clear(); // the files' bytes, no longer needed

    std::vector<std::string> sources;
    for (const recomp::EmittedModule &m : emitted) {
        for (const recomp::LeftOut &l : m.left_out) {
            // the executable itself unreadable: nothing worth a library
            if (l.address == 0 && m.path == executable_path)
                return fail(error, l.module + ": " + l.reason);
            left_.push_back({l.module, l.address, l.reason});
        }
        sources.insert(sources.end(), m.sources.begin(), m.sources.end());
        if (!m.directory.empty()) module_paths_[m.directory] = m.path;
    }
    if (!left_.empty()) report(Stage::Emitting, 1, 1, std::to_string(left_.size()) + " functions left out");
    sources.push_back("modules.cpp");
    const fs::path built = work / (std::string("game") + LIBRARY_EXTENSION);
    if (!compile_and_link(src, sources, work, built, error)) return false;
    // into the cache, replacing what an older XenoForge built for this disc
    fs::create_directories(cache, ec);
    fs::rename(built, library, ec);
    if (ec) return fail(error, "cannot move the library into " + utf8(library) + ": " + ec.message());
    result.left_out = left_;
    result.compiled = compiled_;
    result.linked = linked_;
    result.jobs = policy_.why;
    if (options_.shader_step) shaders(disc, cache, xex, digest, result);
    if (!write_report(report_path(cache, digest, level_), library, result))
        return fail(error, "cannot write " + utf8(report_path(cache, digest, level_)));
    if (options_.report) *options_.report = result;
    for (const auto &entry : fs::directory_iterator(cache, ec)) {
        const std::string name = utf8(entry.path().filename());
        if (name.starts_with(digest + "-") && entry.path() != library && entry.path() != report_path(cache, digest, level_) &&
            (name.ends_with(LIBRARY_EXTENSION) || name.ends_with(REPORT_EXTENSION)))
            fs::remove(entry.path(), ec);
    }
    if (options_.keep_work) {
        // all of it
    } else if (options_.keep_objects) {
        // the objects and their keys, for the next build (compile_and_link);
        // the emitted code, which the next build writes again, goes
        std::vector<fs::path> emitted;
        for (const auto &entry : fs::directory_iterator(work, ec))
            if (entry.path().filename() != "obj" && entry.path().filename() != "objects.key")
                emitted.push_back(entry.path());
        for (const fs::path &p : emitted) fs::remove_all(p, ec);
    } else {
        fs::remove_all(work, ec);
        if (fs::is_empty(cache / "work", ec)) fs::remove(cache / "work", ec);
    }
    report(Stage::Done, 1, 1, utf8(library));
    return true;
}

void Build::shaders(const fs::path &disc, const fs::path &cache, const Xex &xex, const std::string &digest,
                    BuildReport &result) {
    ShaderStep step;
    step.cache_dir = cache;
    step.title_id = xex.execution.title_id;
    step.digest = digest;
    step.cancel = progress_.cancel;
    step.disc_variants = options_.disc_variants;
    if (options_.disc_shaders) {
        // what an earlier step found on this image, or a search of it
        const fs::path kept = disc_shaders_path(cache, digest);
        const std::string stamp = disc_stamp(disc);
        if (!read_disc_shaders(kept, stamp, step)) {
            report(Stage::Linking, 0, 0, "shaders: the disc's");
            find_disc_shaders(disc, step, progress_.cancel);
            if (!cancelled()) write_disc_shaders(kept, stamp, step);
        }
    }
    report(Stage::Linking, 0, 0, "shaders: their pipelines");
    try {
        result.shaders = options_.shader_step(step);
    } catch (const std::exception &e) {
        result.shaders = {std::string("skipped: ") + e.what()};
    }
}

bool Build::compile(const fs::path &clang, const fs::path &work, std::vector<Job> &jobs, std::vector<size_t> batch,
                    std::vector<size_t> &failed, std::string &error) {
    std::error_code ec;
    // the largest first, so the last ones to finish are short
    std::stable_sort(batch.begin(), batch.end(), [&](size_t a, size_t b) { return jobs[a].size > jobs[b].size; });
    // what an earlier build (or a stopped one) compiled from the same text
    std::vector<size_t> todo;
    std::map<size_t, std::string> source_keys;
    uint64_t done = 0;
    for (size_t k : batch) {
        const std::string key = hex_digest(text_key(read_file(jobs[k].source))) + "\n";
        if (fs::exists(jobs[k].object, ec) && read_file(source_key_path(jobs[k])) == key) {
            done++;
        } else {
            todo.push_back(k);
            source_keys[k] = key;
        }
    }
    const uint64_t total = batch.size();
    // (the first report says what the jobs policy chose, and why)
    report(Stage::Compiling, done, total,
           (done ? std::to_string(done) + " compiled before; " : std::string()) + policy_.why);
    const std::vector<std::string> flags = compile_flags(level_);
    const std::string include = "-I" + utf8(work / "include");
    // The generated code's header precompiled once for the functions files
    // (each parsed it again: 0.55 s of clang a file, a tenth of an ordinary
    // file's time; the code comes out the same). Made anew by each build
    // with enough files left to pay for it (0.7 s), as the headers were
    // written again; a build that cannot make it goes without.
    std::string pch;
    const size_t functions_todo =
        size_t(std::count_if(todo.begin(), todo.end(), [&](size_t k) { return is_functions_file(jobs[k].source); }));
    if (functions_todo >= PCH_MINIMUM_FILES && !cancelled()) {
        const fs::path file = work / "obj" / "ppc_generated.pch";
        fs::create_directories(file.parent_path(), ec);
        fs::remove(file, ec);
        std::vector<std::string> args = flags;
        args.push_back(include);
        for (const char *a : {"-x", "c++-header"}) args.push_back(a);
        args.push_back(utf8(work / "include" / "xenoforge" / "ppc_generated.h"));
        args.push_back("-o");
        args.push_back(utf8(file));
        std::vector<Process> one(1);
        std::string ignored;
        fs::path log = file;
        log.replace_extension(".pch.log");
        if (start(clang, args, log, one[0], ignored, options_.background)) {
            int code = 1;
            while (!one.empty() && !cancelled())
                if (wait_any(one, 100, code) == 0) one.clear();
            for (Process &p : one) end(p), code = 1;
            if (code == 0 && fs::exists(file, ec)) pch = utf8(file), fs::remove(log, ec);
        }
    }
    std::vector<Process> running;
    size_t next = 0;
    std::string failure;
    while (next < todo.size() || !running.empty()) {
        if (cancelled() || !failure.empty()) {
            for (Process &p : running) end(p);
            running.clear();
            break;
        }
        while (next < todo.size() && has_room(running, jobs[todo[next]])) {
            const Job &j = jobs[todo[next]];
            fs::create_directories(j.object.parent_path(), ec);
            fs::remove(j.object, ec); // (one clang rejects leaves none behind)
            fs::remove(source_key_path(j), ec);
            std::vector<std::string> args = flags;
            args.push_back(include);
            if (!pch.empty() && is_functions_file(j.source)) {
                args.push_back("-include-pch");
                args.push_back(pch);
            }
            args.push_back("-c");
            args.push_back(utf8(j.source));
            args.push_back("-o");
            args.push_back(utf8(j.object));
            Process p;
            p.job = todo[next++];
            p.expected = job_memory(j.size);
            if (!start(clang, args, j.log, p, failure, options_.background)) break;
            running.push_back(p);
        }
        if (running.empty()) continue;
        int code = 0;
        ProcessStats stats;
        int k = wait_any(running, 100, code, &stats);
        if (k < 0) continue;
        const size_t index = running[size_t(k)].job;
        Job &j = jobs[index];
        running.erase(running.begin() + k);
        j.code = code;
        const std::string name = utf8(fs::relative(j.object, work / "obj", ec).replace_extension(".cpp"));
        compiled_.push_back({name, stats.cpu_seconds, stats.peak_bytes});
        if (code != 0) {
            failed.push_back(index);
            fs::remove(j.object, ec);
            report(Stage::Compiling, ++done, total, "clang rejected " + name);
            continue;
        }
        fs::remove(j.log, ec);
        write_if_changed(source_key_path(j), source_keys[index]);
        report(Stage::Compiling, ++done, total, name);
    }
    if (cancelled()) return fail(error, "cancelled");
    if (!failure.empty()) return fail(error, failure);
    return true;
}

bool Build::recover(const fs::path &clang, const fs::path &work, std::vector<Job> &jobs,
                    const std::vector<size_t> &failed, std::string &error) {
    std::error_code ec;
    // a file of XenoForge's own (a module's table, the module list, the
    // entry), or every file at once: a fault of this XenoForge, not the game's
    size_t functions_files = 0;
    for (const Job &j : jobs) functions_files += is_functions_file(j.source);
    for (size_t k : failed)
        if (!is_functions_file(jobs[k].source))
            return fail(error, "clang failed on " + utf8(jobs[k].source.filename()) + " (" +
                                   std::to_string(jobs[k].code) + "):\n" + read_head(jobs[k].log, 20));
    if (failed.size() == functions_files && functions_files > 2)
        return fail(error, "clang failed on every file of the generated code; the first, " +
                               utf8(jobs[failed[0]].source.filename()) + ":\n" + read_head(jobs[failed[0]].log, 20));

    // Each rejected file split into parts, compiled again; a part rejected is
    // split again, until single functions: those are the culprits. Past the
    // rounds or compiles allowed, a rejected part's functions are all left
    // out. A file whose parts all compile (clang ran out of memory on it) is
    // replaced by them.
    struct Origin {
        size_t job;
        SplitFile file;
        std::vector<std::pair<size_t, std::string>> culprits; // chunk, why
    };
    struct Piece {
        size_t origin;
        std::vector<size_t> chunks;
        std::string stem, reason;
        size_t round;
    };
    std::vector<Origin> origins;
    std::vector<Piece> pending;
    for (size_t k : failed) {
        Origin o{k, {}, {}};
        if (!split_source(read_file(jobs[k].source), o.file) || o.file.chunks.empty())
            return fail(error, "clang failed on " + utf8(jobs[k].source.filename()) + ", which cannot be split:\n" +
                                   read_head(jobs[k].log, 20));
        jobs[k].linked = false;
        Piece p{origins.size(), std::vector<size_t>(o.file.chunks.size()), utf8(jobs[k].source.stem()),
                first_error(jobs[k].log, jobs[k].code), 0};
        std::iota(p.chunks.begin(), p.chunks.end(), size_t(0));
        report(Stage::Compiling, 0, 0,
               "clang rejected " + utf8(jobs[k].source.filename()) + " (" + p.reason + "): splitting it");
        origins.push_back(std::move(o));
        pending.push_back(std::move(p));
    }
    size_t compiles = 0;
    while (!pending.empty()) {
        std::vector<Piece> parts;
        std::vector<size_t> batch;
        for (Piece &p : pending) {
            Origin &o = origins[p.origin];
            if (p.chunks.size() == 1) {
                o.culprits.push_back({p.chunks[0], p.reason});
                continue;
            }
            const size_t n = std::min(SPLIT_PARTS, p.chunks.size());
            if (p.round >= SPLIT_ROUNDS || compiles + n > SPLIT_COMPILES) {
                for (size_t c : p.chunks)
                    o.culprits.push_back({c, "in a part of " + std::to_string(p.chunks.size()) +
                                                 " functions clang rejected: " + p.reason});
                continue;
            }
            for (size_t i = 0; i < n; i++) {
                Piece part{p.origin, {}, p.stem + "_" + std::to_string(i), {}, p.round + 1};
                for (size_t c = p.chunks.size() * i / n; c < p.chunks.size() * (i + 1) / n; c++)
                    part.chunks.push_back(p.chunks[c]);
                std::string text = o.file.head;
                for (size_t c : part.chunks) text += o.file.chunks[c].text + "\n";
                text += o.file.tail;
                Job j = beside(jobs[o.job], part.stem);
                if (!write_if_changed(j.source, text)) return fail(error, "cannot write " + utf8(j.source));
                j.size = text.size();
                batch.push_back(jobs.size());
                jobs.push_back(std::move(j));
                parts.push_back(std::move(part));
                compiles++;
            }
        }
        pending.clear();
        if (batch.empty()) break;
        std::vector<size_t> rejected;
        if (!compile(clang, work, jobs, batch, rejected, error)) return false;
        for (size_t i = 0; i < batch.size(); i++) {
            Job &j = jobs[batch[i]];
            if (std::find(rejected.begin(), rejected.end(), batch[i]) == rejected.end()) continue;
            j.linked = false;
            parts[i].reason = first_error(j.log, j.code);
            pending.push_back(std::move(parts[i]));
        }
    }

    // the culprits: stubs in a file of their own beside each rejected file,
    // and no code for them in their module's table
    std::vector<size_t> batch;
    std::map<fs::path, std::set<uint32_t>> tables;
    for (Origin &o : origins) {
        if (o.culprits.empty()) continue;
        std::sort(o.culprits.begin(), o.culprits.end());
        const Job &origin = jobs[o.job];
        const std::string module = module_of(origin.source);
        std::string text = o.file.head;
        for (const auto &[c, reason] : o.culprits) {
            const Chunk &chunk = o.file.chunks[c];
            const uint32_t a = chunk.address;
            if (chunk.hooked) {
                // A hooked function's body left out: the body a stub that runs
                // the original in the interpreter, the wrapper (the hooks) kept,
                // and so the table's entry (it is the wrapper).
                text += recomp::left_out_original_stub("xfo_", a, "clang rejected it: " + reason) + chunk.wrapper +
                        "\n";
            } else {
                text += recomp::left_out_stub("xf_", a, "clang rejected it: " + reason) + "\n";
                tables[(origin.source.parent_path() / "table.cpp").lexically_normal()].insert(a);
            }
            left_.push_back({module, a, "clang rejected it: " + reason});
            report(Stage::Compiling, 0, 0, "left out " + module + " 0x" + hex8(a) + ": " + reason);
        }
        text += o.file.tail;
        Job j = beside(origin, utf8(origin.source.stem()) + "_left");
        if (!write_if_changed(j.source, text)) return fail(error, "cannot write " + utf8(j.source));
        j.size = text.size();
        batch.push_back(jobs.size());
        jobs.push_back(std::move(j));
    }
    for (const auto &[table, addresses] : tables) {
        std::string text = read_file(table);
        for (uint32_t a : addresses) {
            const std::string from = "{0x" + hex8(a) + "u, " + recomp::function_name("xf_", a) + "},";
            size_t at = text.find(from);
            if (at == std::string::npos)
                return fail(error, "no entry for 0x" + hex8(a) + " in " + utf8(table) + " to leave it out");
            text.replace(at, from.size(), "{0x" + hex8(a) + "u, nullptr},");
        }
        if (!write_if_changed(table, text)) return fail(error, "cannot write " + utf8(table));
        bool found = false;
        for (size_t k = 0; k < jobs.size(); k++)
            if (jobs[k].source.lexically_normal() == table) {
                fs::remove(jobs[k].object, ec); // compiled again
                batch.push_back(k);
                found = true;
            }
        if (!found) return fail(error, utf8(table) + " is not among the files built");
    }
    if (batch.empty()) return true;
    std::vector<size_t> rejected;
    if (!compile(clang, work, jobs, batch, rejected, error)) return false;
    if (!rejected.empty())
        return fail(error, "clang failed on " + utf8(jobs[rejected[0]].source.filename()) +
                               " after the functions it rejected were left out:\n" +
                               read_head(jobs[rejected[0]].log, 20));
    return true;
}

bool Build::link(const fs::path &bin, const fs::path &work, const std::vector<Job> &jobs, const fs::path &out,
                 std::string &error) {
    report(Stage::Linking, 0, 1, utf8(out.filename()));
    std::error_code ec;
    // the objects in a response file (hundreds of them)
    std::string response;
    for (const Job &j : jobs)
        if (j.linked) response += quote(utf8(j.object)) + "\n";
    const fs::path rsp = work / "link.rsp";
    if (!write_if_changed(rsp, response)) return fail(error, "cannot write " + utf8(rsp));
    fs::remove(out, ec);
#ifdef _WIN32
    fs::path lld = bin / "lld-link.exe";
    std::vector<std::string> args;
    if (!fs::exists(lld, ec)) lld = bin / "lld.exe", args = {"-flavor", "link"};
    for (const char *a : link_flags()) args.push_back(a);
    args.push_back("/implib:" + utf8(work / "game.lib"));
#else
    fs::path lld = bin / "ld.lld";
    std::vector<std::string> args;
    if (!fs::exists(lld, ec)) lld = bin / "lld", args = {"-flavor", "gnu"};
    for (const char *a : link_flags()) args.push_back(a);
#endif
    args.push_back(
#ifdef _WIN32
        "/out:" + utf8(out)
#else
        "-o" + utf8(out)
#endif
    );
    args.push_back("@" + utf8(rsp));
    const fs::path log = work / "link.log";
    std::vector<Process> running(1);
    std::string why;
    if (!start(lld, args, log, running[0], why, options_.background)) return fail(error, why);
    int code = 0;
    ProcessStats stats;
    while (wait_any(running, 100, code, &stats) < 0) {
        if (cancelled()) {
            end(running[0]);
            return fail(error, "cancelled");
        }
    }
    if (code != 0 || !fs::exists(out, ec))
        return fail(error, "lld failed (" + std::to_string(code) + "):\n" + read_head(log, 30));
    linked_ = {utf8(out.filename()), stats.cpu_seconds, stats.peak_bytes};
    report(Stage::Linking, 1, 1, utf8(out.filename()));
    return true;
}

} // namespace

fs::path default_cache_dir() {
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA")) return fs::path(local) / "xenoforge" / "cache";
#else
    if (const char *xdg = std::getenv("XDG_CACHE_HOME")) return fs::path(xdg) / "xenoforge";
    if (const char *home = std::getenv("HOME")) return fs::path(home) / ".cache" / "xenoforge";
#endif
    return fs::temp_directory_path() / "xenoforge-cache";
}

std::optional<CachedGame> find_cached(const fs::path &cache_dir, std::string_view digest) {
    const std::string normal = normal_digest(digest);
    if (normal.empty()) return std::nullopt;
    const CpuLevel level = game_cpu_level();
    const fs::path library = library_path(cache_dir, normal, level);
    std::error_code ec;
    if (!fs::is_regular_file(library, ec)) return std::nullopt;
    return CachedGame{library, normal, XF_GAME_INTERFACE_VERSION, level};
}

std::optional<BuildReport> read_build_report(const fs::path &cache_dir, std::string_view digest) {
    const std::string normal = normal_digest(digest);
    if (normal.empty()) return std::nullopt;
    return read_report(report_path(cache_dir, normal, game_cpu_level()));
}

namespace {

std::optional<BuildReport> read_report(const fs::path &file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::nullopt;
    BuildReport r;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("entries_taken ")) {
            r.entries_taken = size_t(std::strtoull(line.c_str() + 14, nullptr, 10));
        } else if (line.starts_with("entries ")) {
            r.entries = size_t(std::strtoull(line.c_str() + 8, nullptr, 10));
        } else if (line.starts_with("left\t")) {
            // left <TAB> module <TAB> address <TAB> reason
            size_t a = line.find('\t', 5), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            r.left_out.push_back({line.substr(5, a - 5),
                                  uint32_t(std::strtoul(line.substr(a + 1, b - a - 1).c_str(), nullptr, 16)),
                                  line.substr(b + 1)});
        } else if (line.starts_with("shaders\t")) {
            r.shaders.push_back(line.substr(8));
        } else if (line.starts_with("project ")) {
            r.project = line.substr(8);
        }
    }
    return r;
}

} // namespace

fs::path entry_log_path(const fs::path &cache_dir, std::string_view digest) {
    std::string normal = normal_digest(digest);
    return cache_dir / ((normal.empty() ? std::string(digest) : normal) + ".entries.txt");
}

std::vector<uint32_t> read_entry_log(const fs::path &file) {
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    std::ifstream f(file, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        char *end = nullptr;
        unsigned long v = std::strtoul(line.c_str(), &end, 16);
        if (end == line.c_str() || v == 0 || v > 0xFFFFFFFFul) continue;
        if (seen.insert(uint32_t(v)).second) out.push_back(uint32_t(v));
    }
    return out;
}

size_t new_entries(const fs::path &cache_dir, std::string_view digest) {
    if (!find_cached(cache_dir, digest)) return 0;
    const BuildReport r = read_build_report(cache_dir, digest).value_or(BuildReport{});
    std::unordered_set<uint32_t> left;
    for (const LeftOutFunction &l : r.left_out) left.insert(l.address);
    const std::vector<uint32_t> logged = read_entry_log(entry_log_path(cache_dir, digest));
    size_t count = 0;
    for (size_t k = r.entries_taken; k < logged.size(); k++) count += !left.count(logged[k]);
    return count;
}

struct EntryLog::State {
    mutable std::mutex mutex;
    std::unordered_set<uint32_t> seen;
    std::ofstream file;
    size_t added = 0;
};

EntryLog::EntryLog() = default;
EntryLog::~EntryLog() = default;

bool EntryLog::open(const fs::path &file) {
    if (!state_) state_ = std::make_unique<State>();
    std::lock_guard lock(state_->mutex);
    for (uint32_t a : read_entry_log(file)) state_->seen.insert(a);
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    if (state_->file.is_open()) state_->file.close();
    state_->file.open(file, std::ios::binary | std::ios::app);
    return state_->file.is_open();
}

void EntryLog::add(uint32_t address) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (!state_->seen.insert(address).second) return;
    state_->added++;
    if (!state_->file.is_open()) return;
    state_->file << hex8(address) << '\n';
    state_->file.flush();
}

size_t EntryLog::size() const {
    if (!state_) return 0;
    std::lock_guard lock(state_->mutex);
    return state_->seen.size();
}

size_t EntryLog::added() const {
    if (!state_) return 0;
    std::lock_guard lock(state_->mutex);
    return state_->added;
}

bool build_library(const fs::path &src_root, const std::vector<std::string> &sources, const fs::path &work,
                   const fs::path &library, Progress &progress, std::string &error, const BuildOptions &options) {
    Build build(progress, options);
    try {
        if (!build.compile_and_link(src_root, sources, work, library, error)) return false;
        if (options.report) {
            *options.report = BuildReport{build.left_, 0, 0};
            options.report->compiled = build.compiled_;
            options.report->linked = build.linked_;
            options.report->jobs = build.policy_.why;
        }
        build.report(Stage::Done, 1, 1, utf8(library));
        return true;
    } catch (const std::exception &e) {
        return build.fail(error, std::string("the build failed: ") + e.what());
    }
}

bool prepare_game(const fs::path &disc, const fs::path &cache_dir, Progress &progress, std::string &error) {
    return prepare_game(disc, cache_dir, progress, error, BuildOptions{});
}

bool prepare_game(const fs::path &disc, const fs::path &cache_dir, Progress &progress, std::string &error,
                  const BuildOptions &options) {
    Build build(progress, options);
    try {
        return build.run(disc, cache_dir, error);
    } catch (const std::exception &e) {
        return build.fail(error, std::string("the build failed: ") + e.what());
    }
}

namespace {
// /proc/meminfo's line for `key` ("MemAvailable"), in bytes; 0 without it
[[maybe_unused]] uint64_t meminfo(std::string_view key) {
    std::ifstream f("/proc/meminfo");
    std::string name, unit;
    uint64_t kilobytes = 0;
    while (f >> name >> kilobytes) {
        std::getline(f, unit);
        if (name.size() == key.size() + 1 && name.starts_with(key) && name.back() == ':') return kilobytes * 1024;
    }
    return 0;
}

std::string gigabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / double(1ull << 30));
    return text;
}
} // namespace

uint64_t available_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#else
    return meminfo("MemAvailable"); // Linux since 3.14, and Android
#endif
}

uint64_t total_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#else
    return meminfo("MemTotal");
#endif
}

uint64_t job_memory(uint64_t source_bytes) {
    // measured (docs/recompiler.md, "The recompile step's memory"): clang's
    // peak on a functions file is about a fixed part (the process, the
    // precompiled header) and a part growing with the file
    return JOB_FIXED_BYTES + source_bytes * JOB_BYTES_PER_SOURCE_BYTE;
}

JobPolicy job_policy(unsigned explicit_jobs, unsigned cores, uint64_t total_bytes, uint64_t available_bytes) {
    JobPolicy p;
    p.cores = std::max(1u, cores);
    p.total_bytes = total_bytes;
    p.available_bytes = available_bytes;
    // the rest of the machine keeps an eighth of it, at least a gigabyte
    p.reserve_bytes = std::max<uint64_t>(1ull << 30, total_bytes / 8);
    p.job_bytes = job_memory(JOB_LARGEST_SOURCE_BYTES);
    if (explicit_jobs) {
        p.limit = explicit_jobs;
        p.why = std::to_string(p.limit) + " compilers at once (given)";
        return p;
    }
    if (!available_bytes) {
        p.limit = std::min(p.cores, UNKNOWN_MEMORY_JOBS);
        p.why = std::to_string(p.limit) + " compilers at once: the memory free is unknown, " +
                std::to_string(p.cores) + " cores";
        return p;
    }
    const uint64_t room = available_bytes > p.reserve_bytes ? available_bytes - p.reserve_bytes : 0;
    const unsigned by_memory = unsigned(std::max<uint64_t>(1, room / p.job_bytes));
    p.limit = std::min(p.cores, by_memory);
    p.why = "up to " + std::to_string(p.limit) + " compilers at once: " + gigabytes(available_bytes) + " of " +
            gigabytes(total_bytes) + " free, " + gigabytes(p.reserve_bytes) + " kept for the rest, up to " +
            gigabytes(p.job_bytes) + " a compiler (" + std::to_string(by_memory) + "), " + std::to_string(p.cores) +
            " cores; another starts only while the memory allows";
    return p;
}

JobPolicy job_policy(unsigned explicit_jobs) {
    unsigned cores = std::thread::hardware_concurrency();
    return job_policy(explicit_jobs, cores ? cores : UNKNOWN_MEMORY_JOBS, total_memory(), available_memory());
}

std::optional<fs::path> find_toolchain(const fs::path &hint) {
    std::vector<fs::path> candidates;
    auto both = [&](const fs::path &dir) {
        if (dir.empty()) return;
        candidates.push_back(dir);
        candidates.push_back(dir / "bin");
    };
    both(hint);
    if (const char *env = std::getenv("XF_TOOLCHAIN")) both(env);
    if (fs::path self = executable_dir(); !self.empty()) both(self / "toolchain");
#ifdef _WIN32
    if (const char *programs = std::getenv("ProgramFiles")) candidates.push_back(fs::path(programs) / "LLVM" / "bin");
    const char separator = ';';
#else
    const char separator = ':';
#endif
    if (const char *path = std::getenv("PATH")) {
        std::string list = path;
        for (size_t at = 0; at <= list.size();) {
            size_t end = list.find(separator, at);
            if (end == std::string::npos) end = list.size();
            if (end > at) candidates.push_back(list.substr(at, end - at));
            at = end + 1;
        }
    }
    for (const fs::path &dir : candidates)
        if (has_toolchain(dir)) return dir;
    return std::nullopt;
}

bool cpu_supported(std::string &why) {
    why = level_refused(game_cpu_level());
    return why.empty();
}

} // namespace xf::build
