// The deterministic regression check (docs/runtime.md, "The deterministic
// regression check"): runs discs' boots in the deterministic mode to a halt
// and compares what each run leaves (the guest memory hash, the clock, the
// scheduler's counts) with expected values kept outside the repository, in
// the TOML file XF_DETERMINISTIC_EXPECTED names (no disc, no game data here).
// A run marked `interpret = true` runs a second time with --interpret, and the
// two must agree with each other as well. Skipped (77) without the variable.
//
//   deterministic_regression_test [--update] [--list] [name...]
//
// --update writes what the runs left into the file as the new expected
// values (a change meant to alter the runs; only runs that halted cleanly,
// and agreed when interpreted too); names pick runs. The file:
//
//   logs = "<dir>"            # each run's output (default: logs/ beside the file)
//   [[run]]
//   name = "b-6000"           # how reports and filters call it
//   exe = "<boot_<game>>"     # optional: a static boot; else this build's boot
//   args = ["<iso>", "--cache", "<dir>", "--deterministic", "--until-swap", "6000"]
//   interpret = false         # true: also with --interpret, the two compared
//   hash = "..."              # what --update writes; a run without them only
//   runs = 0                  #   reports what it left
//   clock = "..."
//   counts = "..."
//
// Every run gets `--steps 18446744073709551615 --seconds 100000` before its own
// arguments (it must end at its halt, not at boot's default limits). A run
// without --interpret must have run generated code (the summary's modules), so
// a game library missing from the cache, which boot would quietly interpret,
// is a failure that says so; a forced hand-over makes the run inexact and fails.
// The watchdog that forces them waits 10 s of the host's time here, not 100 ms
// (XF_DETERMINISTIC_FORCE_MS, unless the environment sets it): on a loaded
// machine the host can stop a guest thread for longer than 100 ms, and a run
// so cut is no longer exact; a guest thread that truly never hands over is
// still caught.

#include "xenoforge/toml.h"

#include "check.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Spec {
    std::string name, exe;
    std::vector<std::string> args;
    bool interpret = false;
    std::map<std::string, std::string> expected; // hash, runs, clock, counts (as text)
};

// what a run left
struct Result {
    bool halted = false, generated = false;
    int exit_code = -1;
    double seconds = 0;
    std::map<std::string, std::string> values;
    std::string error;
};

std::string value_of(const Result &r, const std::string &key) {
    const auto v = r.values.find(key);
    return v == r.values.end() ? "-" : v->second;
}

std::string quote(const std::string &s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
#endif
}

std::string after(const std::string &line, const std::string &key) {
    const size_t at = line.find(key);
    return at == std::string::npos ? std::string() : line.substr(at + key.size());
}

// a JSON number field of the summary line ("modules":1)
long long json_number(const std::string &line, const std::string &key) {
    const std::string field = after(line, "\"" + key + "\":");
    return field.empty() ? -1 : std::strtoll(field.c_str(), nullptr, 10);
}

Result run(const std::string &exe, const Spec &spec, bool interpret, const fs::path &log) {
    Result r;
    std::string command = quote(exe);
    for (size_t k = 0; k < spec.args.size(); k++) {
        command += " " + quote(spec.args[k]);
        if (k == 0) command += " --steps 18446744073709551615 --seconds 100000";
    }
    if (interpret) command += " --interpret";
    command += " > " + quote(log.string()) + " 2>&1";
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd /c strips the outer quotes
#endif
    const auto start = std::chrono::steady_clock::now();
    const int status = std::system(command.c_str());
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
#ifdef _WIN32
    r.exit_code = status;
#else
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    std::ifstream in(log);
    if (!in) {
        r.error = "no output";
        return r;
    }
    std::string line;
    long long forced = -1;
    while (std::getline(in, line)) {
        if (line.starts_with("guest memory hash "))
            r.values["hash"] = after(line, "guest memory hash ");
        else if (line.starts_with("deterministic: clock ")) {
            // "deterministic: clock C s in H s on the host (...); <counts>"
            std::string clock = after(line, "deterministic: clock ");
            r.values["clock"] = clock.substr(0, clock.find(' '));
            std::string counts = after(line, "); ");
            r.values["counts"] = counts;
            const size_t runs_end = counts.find(" runs");
            const size_t runs_start = counts.rfind(' ', runs_end - 1);
            if (runs_end != std::string::npos && runs_start != std::string::npos)
                r.values["runs"] = counts.substr(runs_start + 1, runs_end - runs_start - 1);
            const std::string before_forced = counts.substr(0, counts.find(" hand-overs forced"));
            forced = std::strtoll(before_forced.substr(before_forced.rfind(' ') + 1).c_str(), nullptr, 10);
            r.halted = counts.ends_with("; halted");
        } else if (line.starts_with("xf-summary ")) {
            r.generated = json_number(line, "modules") > 0;
        }
    }
    if (!r.halted)
        r.error = "did not halt (no \"; halted\" in the scheduler's report)";
    else if (!r.values.count("hash"))
        r.error = "no guest memory hash";
    else if (forced != 0)
        r.error = "hand-overs forced: the run is not exact";
    else if (!interpret && !r.generated)
        r.error =
            "no generated code ran (a game library missing from the cache, or out of date: xfbuild <iso> <cache>)";
    return r;
}

// The file with each [[run]] block's values set to what `results` holds
// (by the blocks' order), the rest of its text kept.
std::string updated(const std::string &text, const std::vector<std::map<std::string, std::string>> &results) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    std::vector<size_t> starts;
    for (size_t k = 0; k < lines.size(); k++)
        if (lines[k].find("[[run]]") == 0) starts.push_back(k);
    // from the last block back, so earlier blocks' line numbers stay put
    for (size_t b = starts.size(); b-- > 0;) {
        if (b >= results.size() || results[b].empty()) continue;
        size_t end = b + 1 < starts.size() ? starts[b + 1] : lines.size();
        for (size_t k = starts[b] + 1; k < end; k++)
            if (!lines[k].empty() && lines[k][0] == '[') end = k; // another table ends it
        size_t last = starts[b];
        for (size_t k = starts[b] + 1; k < end; k++)
            if (lines[k].find_first_not_of(" \t") != std::string::npos && lines[k][0] != '#') last = k;
        for (const char *key : {"hash", "runs", "clock", "counts"}) {
            const auto value = results[b].find(key);
            if (value == results[b].end()) continue;
            const std::string written =
                std::string(key) + " = " + (std::string(key) == "runs" ? value->second : "\"" + value->second + "\"");
            bool replaced = false;
            for (size_t k = starts[b] + 1; k <= last; k++) {
                const std::string &l = lines[k];
                const size_t eq = l.find('=');
                if (eq == std::string::npos) continue;
                std::string name = l.substr(0, eq);
                name.erase(name.find_last_not_of(" \t") + 1);
                if (name == key) {
                    lines[k] = written;
                    replaced = true;
                }
            }
            if (!replaced) lines.insert(lines.begin() + ptrdiff_t(++last), written);
        }
    }
    std::string out;
    for (const std::string &l : lines) out += l + "\n";
    return out;
}

} // namespace

int main(int argc, char **argv) {
    const char *path = std::getenv("XF_DETERMINISTIC_EXPECTED");
    if (!path || !*path) {
        std::printf("XF_DETERMINISTIC_EXPECTED not set: skipped\n");
        return 77;
    }
    bool update = false, list = false;
    std::vector<std::string> names;
    for (int k = 1; k < argc; k++) {
        const std::string a = argv[k];
        if (a == "--update")
            update = true;
        else if (a == "--list")
            list = true;
        else
            names.push_back(a);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("%s: cannot be read\n", path);
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    xf::toml::Value root;
    std::string error;
    if (!xf::toml::parse(text, root, error)) {
        std::printf("%s: %s\n", path, error.c_str());
        return 1;
    }
    fs::path logs = fs::path(path).parent_path() / "logs";
    if (const xf::toml::Value *v = root.find("logs"); v && v->is_string()) logs = v->string;
    std::vector<Spec> specs;
    if (const xf::toml::Value *runs = root.find("run"); runs && runs->is_array()) {
        for (const xf::toml::Value &t : runs->array) {
            Spec s;
            if (const auto *v = t.find("name"); v && v->is_string()) s.name = v->string;
            s.exe = XF_BOOT_EXE;
            if (const auto *v = t.find("exe"); v && v->is_string()) s.exe = v->string;
            if (const auto *v = t.find("args"); v && v->is_array())
                for (const auto &a : v->array)
                    if (a.is_string()) s.args.push_back(a.string);
            if (const auto *v = t.find("interpret"); v && v->is_bool()) s.interpret = v->boolean;
            for (const char *key : {"hash", "clock", "counts"})
                if (const auto *v = t.find(key); v && v->is_string()) s.expected[key] = v->string;
            if (const auto *v = t.find("runs"); v && v->is_integer()) s.expected["runs"] = std::to_string(v->integer);
            if (s.name.empty()) s.name = "run " + std::to_string(specs.size() + 1);
            specs.push_back(s);
        }
    }
    if (specs.empty()) {
        std::printf("%s: no [[run]]\n", path);
        return 1;
    }
    if (list) {
        for (const Spec &s : specs)
            std::printf("%s%s: %zu arguments, %s\n", s.name.c_str(), s.interpret ? " (and interpreted)" : "",
                        s.args.size(), s.expected.count("hash") ? s.expected.at("hash").c_str() : "no expected values");
        return 0;
    }
    if (const char *force = std::getenv("XF_DETERMINISTIC_FORCE_MS"); !force || !*force) {
#ifdef _WIN32
        _putenv_s("XF_DETERMINISTIC_FORCE_MS", "10000");
#else
        setenv("XF_DETERMINISTIC_FORCE_MS", "10000", 1);
#endif
    }
    std::error_code ec;
    fs::create_directories(logs, ec);

    std::vector<std::map<std::string, std::string>> results(specs.size());
    for (size_t k = 0; k < specs.size(); k++) {
        const Spec &s = specs[k];
        if (!names.empty() && std::find(names.begin(), names.end(), s.name) == names.end()) continue;
        if (s.args.empty()) {
            std::printf("%s: no args\n", s.name.c_str());
            failures++;
            continue;
        }
        std::string file = s.name;
        for (char &c : file)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        const Result generated = run(s.exe, s, false, logs / (file + ".log"));
        std::printf("%s: %.1f s, hash %s, %s runs, clock %s s%s%s\n", s.name.c_str(), generated.seconds,
                    value_of(generated, "hash").c_str(), value_of(generated, "runs").c_str(),
                    value_of(generated, "clock").c_str(), generated.error.empty() ? "" : ": ", generated.error.c_str());
        bool ok = generated.error.empty();
        if (ok && s.interpret) {
            const Result interpreted = run(s.exe, s, true, logs / (file + ".interpreted.log"));
            std::printf("%s, interpreted: %.1f s, hash %s%s%s\n", s.name.c_str(), interpreted.seconds,
                        value_of(interpreted, "hash").c_str(), interpreted.error.empty() ? "" : ": ",
                        interpreted.error.c_str());
            if (!interpreted.error.empty())
                ok = false;
            else
                for (const char *key : {"hash", "runs", "clock", "counts"})
                    if (value_of(generated, key) != value_of(interpreted, key)) {
                        std::printf("%s: the interpreted run's %s differs: %s, generated %s\n", s.name.c_str(), key,
                                    value_of(interpreted, key).c_str(), value_of(generated, key).c_str());
                        ok = false;
                    }
        }
        if (!ok) {
            failures++;
            std::printf("%s: see %s\n", s.name.c_str(), (logs / (file + ".log")).string().c_str());
            continue;
        }
        if (update) {
            results[k] = generated.values;
            continue;
        }
        if (s.expected.empty()) {
            std::printf("%s: no expected values (--update writes them)\n", s.name.c_str());
            continue;
        }
        for (const auto &[key, value] : s.expected) {
            const auto got = generated.values.find(key);
            if (got == generated.values.end() || got->second != value) {
                std::printf("%s: %s %s, expected %s\n", s.name.c_str(), key.c_str(),
                            got == generated.values.end() ? "missing" : got->second.c_str(), value.c_str());
                failures++;
            }
        }
    }
    if (update) {
        std::ofstream out(path, std::ios::binary);
        out << updated(text, results);
        std::printf("%s: expected values written\n", path);
    }
    return finish("deterministic_regression");
}
