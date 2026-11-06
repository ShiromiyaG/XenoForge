// The recompile step (xenoforge/game_build.h; docs/runtime.md, "The game
// library"): a disc's executables discovered and emitted in process into a
// work directory under the cache, compiled by clang a file per process (4 at
// once by default) against the generated code's headers as this XenoForge
// has them (written beside the code), linked by lld into a library that
// imports nothing, and kept in the cache by the executable's digest and the
// interface version.

#include "xenoforge/game_build.h"

#include "xenoforge/game_interface.h" // XF_GAME_INTERFACE_VERSION, made by the build
#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
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
};

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
    // the headers and the entry written into work, then sources (relative
    // to src_root) compiled and linked into out
    bool compile_and_link(const fs::path &src_root, const std::vector<std::string> &sources, const fs::path &work,
                          const fs::path &out, std::string &error);

  private:
    bool compile(const fs::path &clang, const fs::path &work, std::vector<Job> &jobs, std::string &error);
    bool link(const fs::path &bin, const fs::path &work, const std::vector<Job> &jobs, const fs::path &out,
              std::string &error);

    Progress &progress_;
    const BuildOptions &options_;
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
    if (!compile(*bin / (std::string("clang") + EXE), work, jobs, error)) return false;
    return link(*bin, work, jobs, out, error);
}

bool Build::run(const fs::path &disc, const fs::path &cache, std::string &error) {
    std::string why;
    if (!cpu_supported(why)) return fail(error, why);
    report(Stage::Reading, 0, 0, utf8(disc.filename()));
    if (!find_toolchain(options_.toolchain))
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
    Xex xex;
    if (!parse_xex(executable->file, xex, why) || !load_xex_image(executable->file, xex, why))
        return fail(error, executable->path + ": " + why);
    const std::string digest = hex_digest(image_digest(xex.image));
    const fs::path library = library_path(cache, digest);
    std::error_code ec;
    if (!options_.force && fs::exists(library, ec)) {
        report(Stage::Done, 1, 1, utf8(library));
        return true;
    }
    if (cancelled()) return fail(error, "cancelled");

    // the emitted code, in a work directory of its own (a build stopped
    // before goes on from the objects it left)
    const fs::path work = cache / "work" / (digest + "-" + hex_version());
    const fs::path src = work / "src";
    fs::create_directories(src, ec);
    if (ec) return fail(error, "cannot make " + utf8(src) + ": " + ec.message());
    recomp::EmitOptions emit;
    emit.progress = [&](recomp::EmitStage stage, size_t done, size_t total, const std::string &detail) {
        report(stage == recomp::EmitStage::Discovering ? Stage::Discovering : Stage::Emitting, done, total, detail);
        return !cancelled();
    };
    std::vector<recomp::EmittedModule> emitted;
    if (!recomp::emit_modules(modules, src, emit, emitted, why)) return fail(error, why);
    modules.clear(); // the files' bytes, no longer needed

    std::vector<std::string> sources;
    for (const recomp::EmittedModule &m : emitted) sources.insert(sources.end(), m.sources.begin(), m.sources.end());
    sources.push_back("modules.cpp");
    const fs::path built = work / (std::string("game") + LIBRARY_EXTENSION);
    if (!compile_and_link(src, sources, work, built, error)) return false;
    // into the cache, replacing what an older XenoForge built for this disc
    fs::create_directories(cache, ec);
    fs::rename(built, library, ec);
    if (ec) return fail(error, "cannot move the library into " + utf8(library) + ": " + ec.message());
    for (const auto &entry : fs::directory_iterator(cache, ec)) {
        const std::string name = utf8(entry.path().filename());
        if (name.starts_with(digest + "-") && name.ends_with(LIBRARY_EXTENSION) && entry.path() != library)
            fs::remove(entry.path(), ec);
    }
    if (!options_.keep_work) {
        fs::remove_all(work, ec);
        if (fs::is_empty(cache / "work", ec)) fs::remove(cache / "work", ec);
    }
    report(Stage::Done, 1, 1, utf8(library));
    return true;
}

bool Build::compile(const fs::path &clang, const fs::path &work, std::vector<Job> &jobs, std::string &error) {
    std::error_code ec;
    // the largest first, so the last ones to finish are short
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) { return a.size > b.size; });
    // what a stopped build compiled already
    std::vector<size_t> todo;
    uint64_t done = 0;
    for (size_t k = 0; k < jobs.size(); k++) {
        auto object = fs::last_write_time(jobs[k].object, ec);
        if (!ec && object >= fs::last_write_time(jobs[k].source, ec) && !ec)
            done++;
        else
            todo.push_back(k);
    }
    const uint64_t total = jobs.size();
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
        const Job &j = jobs[running[size_t(k)].job];
        running.erase(running.begin() + k);
        if (code != 0) {
            failure = "clang failed on " + utf8(j.source.filename()) + " (" + std::to_string(code) + "):\n" +
                      read_head(j.log, 20);
            continue;
        }
        fs::remove(j.log, ec);
        report(Stage::Compiling, ++done, total,
               utf8(fs::relative(j.object, work / "obj", ec).replace_extension(".cpp")));
    }
    if (cancelled()) return fail(error, "cancelled");
    if (!failure.empty()) return fail(error, failure);
    return true;
}

bool Build::link(const fs::path &bin, const fs::path &work, const std::vector<Job> &jobs, const fs::path &out,
                 std::string &error) {
    report(Stage::Linking, 0, 1, utf8(out.filename()));
    std::error_code ec;
    // the objects in a response file (hundreds of them)
    std::string response;
    for (const Job &j : jobs) response += quote(utf8(j.object)) + "\n";
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
    // the digest however it is written: 16 hex digits
    std::string text(digest);
    if (text.size() != 16 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return std::nullopt;
    const std::string normal = hex_digest(std::strtoull(text.c_str(), nullptr, 16));
    const fs::path library = library_path(cache_dir, normal);
    std::error_code ec;
    if (!fs::is_regular_file(library, ec)) return std::nullopt;
    return CachedGame{library, normal, XF_GAME_INTERFACE_VERSION};
}

bool build_library(const fs::path &src_root, const std::vector<std::string> &sources, const fs::path &work,
                   const fs::path &library, Progress &progress, std::string &error, const BuildOptions &options) {
    Build build(progress, options);
    try {
        if (!build.compile_and_link(src_root, sources, work, library, error)) return false;
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
