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

#include "xenoforge/game_interface.h" // XF_GAME_INTERFACE_VERSION, made by the build
#include "xenoforge/disc.h"
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
#include <intrin.h>
#include <windows.h>
#else
#include <cpuid.h>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace xf::build {
namespace {

namespace fs = std::filesystem;

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

// What each file is compiled with: the statically linked games' flags
// (CMakeLists.txt: -O2 -g0 -w -mfma -mavx2; never -ffast-math), freestanding
// (no header but the ones written beside the code, no C library: math
// without errno), and the game library's side of ppc_runtime.h.
std::vector<std::string> compile_flags() {
    char version[64];
    std::snprintf(version, sizeof(version), "-DXF_GAME_INTERFACE_VERSION=0x%08Xu", XF_GAME_INTERFACE_VERSION);
    std::vector<std::string> flags = {
        "-std=c++20",        "-O2",  "-g0", "-w", "-mfma", "-mavx2", "-fno-math-errno", "-nostdinc", "-nostdinc++",
        "-DXF_GAME_LIBRARY", version};
#ifndef _WIN32
    // position independent, only the entry exported, and no stack
    // protector (some distributions' clang turns it on; it needs the C
    // library's __stack_chk_fail)
    for (const char *f : {"-fPIC", "-fvisibility=hidden", "-fno-stack-protector"}) flags.push_back(f);
#endif
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

fs::path library_path(const fs::path &cache, const std::string &digest) {
    return cache / (digest + "-" + hex_version() + LIBRARY_EXTENSION);
}
// what the build left out and took in, beside the library
fs::path report_path(const fs::path &cache, const std::string &digest) {
    return cache / (digest + "-" + hex_version() + ".build.txt");
}
constexpr const char *REPORT_EXTENSION = ".build.txt";

// a digest however it is written (16 hex digits, either case), as the cache
// names it; empty when it is not one
std::string normal_digest(std::string_view digest) {
    std::string text(digest);
    if (text.size() != 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return {};
    return hex_digest(std::strtoull(text.c_str(), nullptr, 16));
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
        if (trimmed.starts_with("XF_FN(") && !trimmed.ends_with(";")) {
            size_t close = trimmed.find(')');
            if (close == std::string::npos || close < 6 + 8) return false;
            const std::string digits = trimmed.substr(close - 8, 8);
            if (digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
            if (!seen) out.head = pending, seen = true;
            pending.clear();
            out.chunks.push_back({uint32_t(std::strtoul(digits.c_str(), nullptr, 16)), line});
            if (!trimmed.ends_with("}")) current = &out.chunks.back().text;
            continue;
        }
        pending += line;
    }
    if (current || !seen) return false;
    out.tail = pending;
    return true;
}

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
};

bool start(const fs::path &program, const std::vector<std::string> &args, const fs::path &log, Process &p,
           std::string &error) {
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
                              CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT, nullptr,
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

// Waits up to `ms` for one of the processes to end: its index and exit code,
// or -1 when none did.
int wait_any(std::vector<Process> &running, int ms, int &exit_code) {
#ifdef _WIN32
    std::vector<HANDLE> handles;
    for (const Process &p : running) handles.push_back(p.handle);
    DWORD r = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, DWORD(ms));
    if (r >= WAIT_OBJECT_0 + handles.size()) return -1;
    int k = int(r - WAIT_OBJECT_0);
    DWORD code = 1;
    GetExitCodeProcess(running[size_t(k)].handle, &code);
    CloseHandle(running[size_t(k)].handle);
    running[size_t(k)].handle = nullptr;
    exit_code = int(code);
    return k;
#else
    for (int waited = 0;; waited += 20) {
        for (size_t k = 0; k < running.size(); k++) {
            int status = 0;
            if (waitpid(running[k].pid, &status, WNOHANG) == running[k].pid) {
                running[k].pid = -1;
                exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
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
        std::unique_ptr<Disc> image = Disc::open(disc, why);
        if (image) {
            std::vector<const DiscEntry *> pending{&image->root()};
            while (!pending.empty()) {
                const DiscEntry *e = pending.back();
                pending.pop_back();
                for (const DiscEntry &child : e->children) {
                    if (child.directory)
                        pending.push_back(&child);
                    else if (wanted(child.name, child.size))
                        take(image->read_all(child));
                }
            }
        }
    }
    step.disc_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

class Build {
  public:
    Build(Progress &progress, const BuildOptions &options) : progress_(progress), options_(options) {}

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

    Progress &progress_;
    const BuildOptions &options_;
    std::map<std::string, std::string> module_paths_;
};

bool Build::compile_and_link(const fs::path &src_root, const std::vector<std::string> &sources, const fs::path &work,
                             const fs::path &out, std::string &error) {
    std::string why;
    if (!cpu_supported(why)) return fail(error, why);
    std::optional<fs::path> bin = find_toolchain(options_.toolchain);
    if (!bin)
        return fail(error, "no clang and lld found (toolchain/bin beside the launcher, XF_TOOLCHAIN, an installed "
                           "LLVM or the PATH)");
    // the headers, as this XenoForge has them, and the library's entry
    for (const Header &h : HEADERS)
        if (!write_if_changed(work / "include" / h.name, h.text)) return fail(error, "cannot write the headers");
    const fs::path entry = work / "xf_library_entry.cpp";
    if (!write_if_changed(entry, "// Written by the recompile step: the game library's entry (game_library.h).\n"
                                 "#define XF_GAME_LIBRARY_ENTRY\n#include \"xenoforge/game_library.h\"\n"))
        return fail(error, "cannot write " + utf8(entry));
    std::error_code ec;
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
            const std::string head = "\nXF_FN(" + recomp::function_name("xf_", a) + ") {";
            size_t at = text.find(head);
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
    if (!cpu_supported(why)) return fail(error, why);
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
    const std::string digest = hex_digest(image_digest(xex.image));
    const fs::path library = library_path(cache, digest);
    std::error_code ec;
    if (options_.shaders_only && !fs::exists(library, ec)) {
        BuildReport found;
        if (options_.shader_step) shaders(disc, cache, xex, digest, found);
        if (options_.report) *options_.report = found;
        report(Stage::Done, 1, 1, "");
        return true;
    }
    if ((options_.shaders_only || !options_.force) && fs::exists(library, ec)) {
        BuildReport found = read_build_report(cache, digest).value_or(BuildReport{});
        if (options_.shader_step) {
            shaders(disc, cache, xex, digest, found);
            write_report(report_path(cache, digest), library, found);
        }
        if (options_.report) *options_.report = found;
        report(Stage::Done, 1, 1, utf8(library));
        return true;
    }
    if (cancelled()) return fail(error, "cancelled");
    // the addresses earlier runs entered the interpreter at (D3), and the
    // ones given: discovery's extra entries
    BuildReport result;
    std::vector<uint32_t> entries;
    if (options_.use_entry_log) entries = read_entry_log(entry_log_path(cache, digest));
    result.entries_taken = entries.size();
    entries.insert(entries.end(), options_.entries.begin(), options_.entries.end());
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    result.entries = entries.size();

    // the emitted code, in a work directory of its own (a build stopped
    // before goes on from the objects it left)
    const fs::path work = cache / "work" / (digest + "-" + hex_version());
    const fs::path src = work / "src";
    fs::create_directories(src, ec);
    if (ec) return fail(error, "cannot make " + utf8(src) + ": " + ec.message());
    recomp::EmitOptions emit;
    emit.discover.entries = std::move(entries);
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
    if (options_.shader_step) shaders(disc, cache, xex, digest, result);
    if (!write_report(report_path(cache, digest), library, result))
        return fail(error, "cannot write " + utf8(report_path(cache, digest)));
    if (options_.report) *options_.report = result;
    for (const auto &entry : fs::directory_iterator(cache, ec)) {
        const std::string name = utf8(entry.path().filename());
        if (name.starts_with(digest + "-") && entry.path() != library && entry.path() != report_path(cache, digest) &&
            (name.ends_with(LIBRARY_EXTENSION) || name.ends_with(REPORT_EXTENSION)))
            fs::remove(entry.path(), ec);
    }
    if (!options_.keep_work) {
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
    if (options_.disc_shaders) {
        report(Stage::Linking, 0, 0, "shaders: the disc's");
        find_disc_shaders(disc, step, progress_.cancel);
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
    // what a stopped build compiled already
    std::vector<size_t> todo;
    uint64_t done = 0;
    for (size_t k : batch) {
        auto object = fs::last_write_time(jobs[k].object, ec);
        if (!ec && object >= fs::last_write_time(jobs[k].source, ec) && !ec)
            done++;
        else
            todo.push_back(k);
    }
    const uint64_t total = batch.size();
    report(Stage::Compiling, done, total, done ? std::to_string(done) + " compiled before" : "");
    const std::vector<std::string> flags = compile_flags();
    const std::string include = "-I" + utf8(work / "include");
    std::vector<Process> running;
    size_t next = 0;
    std::string failure;
    while (next < todo.size() || !running.empty()) {
        if (cancelled() || !failure.empty()) {
            for (Process &p : running) end(p);
            running.clear();
            break;
        }
        while (next < todo.size() && running.size() < std::max(1u, options_.jobs)) {
            const Job &j = jobs[todo[next]];
            fs::create_directories(j.object.parent_path(), ec);
            fs::remove(j.object, ec); // (one clang rejects leaves none behind)
            std::vector<std::string> args = flags;
            args.push_back(include);
            args.push_back("-c");
            args.push_back(utf8(j.source));
            args.push_back("-o");
            args.push_back(utf8(j.object));
            Process p;
            p.job = todo[next++];
            if (!start(clang, args, j.log, p, failure)) break;
            running.push_back(p);
        }
        if (running.empty()) continue;
        int code = 0;
        int k = wait_any(running, 100, code);
        if (k < 0) continue;
        const size_t index = running[size_t(k)].job;
        Job &j = jobs[index];
        running.erase(running.begin() + k);
        j.code = code;
        const std::string name = utf8(fs::relative(j.object, work / "obj", ec).replace_extension(".cpp"));
        if (code != 0) {
            failed.push_back(index);
            fs::remove(j.object, ec);
            report(Stage::Compiling, ++done, total, "clang rejected " + name);
            continue;
        }
        fs::remove(j.log, ec);
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
            const uint32_t a = o.file.chunks[c].address;
            text += recomp::left_out_stub("xf_", a, "clang rejected it: " + reason) + "\n";
            left_.push_back({module, a, "clang rejected it: " + reason});
            tables[(origin.source.parent_path() / "table.cpp").lexically_normal()].insert(a);
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
    // no entry point, no default libraries: the library imports nothing
    fs::path lld = bin / "lld-link.exe";
    std::vector<std::string> args;
    if (!fs::exists(lld, ec)) lld = bin / "lld.exe", args = {"-flavor", "link"};
    // (/Brepro: no time stamp, the same bytes from the same objects)
    for (const char *a : {"/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/opt:ref", "/Brepro"})
        args.push_back(a);
    args.push_back("/implib:" + utf8(work / "game.lib"));
#else
    fs::path lld = bin / "ld.lld";
    std::vector<std::string> args;
    if (!fs::exists(lld, ec)) lld = bin / "lld", args = {"-flavor", "gnu"};
    for (const char *a : {"-shared", "--eh-frame-hdr", "-z", "noexecstack", "--gc-sections"}) args.push_back(a);
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
    if (!start(lld, args, log, running[0], why)) return fail(error, why);
    int code = 0;
    while (wait_any(running, 100, code) < 0) {
        if (cancelled()) {
            end(running[0]);
            return fail(error, "cancelled");
        }
    }
    if (code != 0 || !fs::exists(out, ec))
        return fail(error, "lld failed (" + std::to_string(code) + "):\n" + read_head(log, 30));
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
    const fs::path library = library_path(cache_dir, normal);
    std::error_code ec;
    if (!fs::is_regular_file(library, ec)) return std::nullopt;
    return CachedGame{library, normal, XF_GAME_INTERFACE_VERSION};
}

std::optional<BuildReport> read_build_report(const fs::path &cache_dir, std::string_view digest) {
    const std::string normal = normal_digest(digest);
    if (normal.empty()) return std::nullopt;
    std::ifstream f(report_path(cache_dir, normal), std::ios::binary);
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
        }
    }
    return r;
}

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
        if (options.report) *options.report = BuildReport{build.left_, 0, 0};
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
    // FMA (CPUID 1, ECX bit 12) and AVX2 (CPUID 7, EBX bit 5), with the OS
    // saving the YMM registers (OSXSAVE, then XCR0's bits 1 and 2)
    unsigned regs[4] = {};
#ifdef _WIN32
    int r[4];
    __cpuid(r, 1);
    std::memcpy(regs, r, sizeof(r));
#else
    __get_cpuid(1, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
    const bool fma = regs[2] & (1u << 12), osxsave = regs[2] & (1u << 27);
#ifdef _WIN32
    __cpuidex(r, 7, 0);
    std::memcpy(regs, r, sizeof(r));
#else
    __get_cpuid_count(7, 0, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
    const bool avx2 = regs[1] & (1u << 5);
    bool ymm = false;
    if (osxsave) {
        unsigned lo, hi;
        __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        ymm = (lo & 6) == 6;
    }
    if (fma && avx2 && ymm) return true;
    why = "this CPU lacks AVX2 or FMA, which the generated code is built for";
    return false;
}

} // namespace xf::build
