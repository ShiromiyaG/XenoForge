// Xenia's PPC instruction tests (src/xenia/cpu/ppc/testing, BSD), assembled
// by tools/assemble_ppc_tests.sh into XF_PPC_TESTS, run through the
// interpreter as Xenia's ppc_testing_main.cc runs them (docs/recompiler.md,
// "Checks"): each test case is a label, run with fresh memory and zeroed
// registers from its REGISTER_IN and MEMORY_IN until it returns to the
// sentinel LR, then its REGISTER_OUT and MEMORY_OUT are compared. The values
// are read as Xenia's string_util reads them. Xenia's skip.txt is honored.
//
//   XF_PPC_TESTS=../external/ppc_tests build/ppc_test [name filters...] [-v]
//
// Without XF_PPC_TESTS there is nothing to run. Built with
// XF_PPC_RECOMPILED (ppc_recompiled_test), each case calls the function
// xfrecomp wrote for it instead of interpreting it, and dispatch() runs a
// function of the same suite or the interpreter.

#include "xenoforge/guest_space.h"
#include "xenoforge/ppc_interp.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace xf::ppc;

#ifdef XF_PPC_RECOMPILED
#include "xenoforge/ppc_generated.h"
struct XfTestFunction {
    const char *suite;
    uint32_t address;
    void (*fn)(Context &);
};
extern const XfTestFunction xf_test_functions[];
extern const size_t xf_test_function_count;
namespace {
std::map<std::pair<std::string, uint32_t>, void (*)(Context &)> functions;
std::string current_suite;
bool missing = false;
Opcode missing_op = Opcode::Invalid;
} // namespace
namespace xf::ppc {
void dispatch(Context &c, uint32_t target) {
    auto it = functions.find({current_suite, target});
    if (it != functions.end()) return it->second(c);
    Stopped s = interpret(c, target, uint32_t(c.lr), 1'000'000);
    if (s.reason != Stopped::Returned) missing = true, missing_op = s.op;
}
void invalid(Context &, uint32_t) { missing = true, missing_op = Opcode::Invalid; }
} // namespace xf::ppc
#endif

namespace {

constexpr uint32_t START = 0x80000000;
constexpr uint32_t SENTINEL = 0xBCBCBCBC;

struct TestCase {
    std::string name;
    uint32_t address = 0;
    std::vector<std::pair<std::string, std::string>> annotations;
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// string_util's ifs<T>: "-", "0x" and a trailing "h" understood
uint64_t parse_u64(std::string_view v, bool force_hex = false) {
    bool hex = force_hex, negative = false;
    if (v.starts_with("-")) negative = true, v.remove_prefix(1);
    if (v.starts_with("0x")) hex = true, v.remove_prefix(2);
    if (v.ends_with("h")) hex = true, v.remove_suffix(1);
    uint64_t result = 0;
    std::from_chars(v.data(), v.data() + v.size(), result, hex ? 16 : 10);
    return negative ? uint64_t(-int64_t(result)) : result;
}
// fpfs<double>: hex as the bits
double parse_double(std::string_view v) {
    bool hex = false, negative = false;
    if (v.starts_with("-")) negative = true, v.remove_prefix(1);
    if (v.starts_with("0x")) hex = true, v.remove_prefix(2);
    if (v.ends_with("h")) hex = true, v.remove_suffix(1);
    double result;
    if (hex) {
        uint64_t bits = parse_u64(v, true);
        if (negative) bits = uint64_t(-int64_t(bits));
        std::memcpy(&result, &bits, 8);
    } else {
        result = std::strtod(std::string(v).c_str(), nullptr);
        if (negative) result = -result;
    }
    return result;
}
// from_string<vec128_t>: [hex words] or (floats)
Vec128 parse_vec(const std::string &v) {
    Vec128 out{};
    const char *p = v.c_str(), *end = p + v.size();
    bool hex = true;
    if (p != end && *p == '[') ++p;
    else if (p != end && *p == '(') hex = false, ++p;
    for (int k = 0; k < 4; k++) {
        while (p != end && (*p == ' ' || *p == ',')) ++p;
        if (p == end) break;
        if (hex) {
            auto r = std::from_chars(p, end, out.u32[k], 16);
            p = r.ptr;
        } else {
            char *next;
            out.f32[k] = std::strtof(p, &next);
            p = next;
        }
    }
    return out;
}

std::vector<uint8_t> parse_bytes(const std::string &s) {
    std::vector<uint8_t> bytes;
    for (size_t k = 0; k < s.size();) {
        while (k < s.size() && s[k] == ' ') k++;
        if (k >= s.size()) break;
        bytes.push_back(uint8_t(std::strtoul(s.substr(k, 2).c_str(), nullptr, 16)));
        k += 2;
    }
    return bytes;
}

bool load_suite(const fs::path &source, std::vector<TestCase> &cases) {
    fs::path map = fs::path(source).replace_extension(".map");
    std::ifstream m(map);
    if (!m) return false;
    std::string line;
    while (std::getline(m, line)) {
        size_t at = line.find(" t test_");
        if (at == std::string::npos) continue;
        cases.push_back({trim(line.substr(at + 8)), START + uint32_t(std::stoul(line.substr(0, at), nullptr, 16)), {}});
    }
    std::ifstream s(source);
    TestCase *current = nullptr;
    while (std::getline(s, line)) {
        size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        std::string rest = line.substr(start);
        if (rest.starts_with("test_")) {
            std::string label = rest.substr(5, rest.find(':') - 5);
            current = nullptr;
            for (TestCase &t : cases)
                if (t.name == label) current = &t;
        } else if (rest.size() > 3 && rest[0] == '#' && rest[1] == '_') {
            size_t space = rest.find(' ', 3);
            if (space == std::string::npos || !current) continue;
            current->annotations.push_back({rest.substr(3, space - 3), trim(rest.substr(space + 1))});
        }
    }
    return true;
}

struct Totals {
    uint64_t passed = 0, failed = 0, skipped = 0, unimplemented = 0;
    std::map<std::string, uint64_t> missing; // opcode -> tests it stopped
};

} // namespace

int main(int argc, char **argv) {
    const char *dir = std::getenv("XF_PPC_TESTS");
    if (!dir) {
        std::printf("XF_PPC_TESTS not set: nothing to run\n");
        return 0;
    }
    std::vector<std::string> filters;
    bool verbose = false;
    for (int k = 1; k < argc; k++) {
        if (std::string(argv[k]) == "-v") verbose = true;
        else filters.push_back(argv[k]);
    }
    std::set<std::string> skip;
    {
        std::ifstream f(fs::path(dir) / "skip.txt");
        std::string line;
        while (std::getline(f, line))
            if (!trim(line).empty() && line[0] != '#') skip.insert(trim(line));
    }
    std::vector<fs::path> sources;
    for (const auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".s" && fs::exists(fs::path(e.path()).replace_extension(".bin")))
            sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());

    xf::GuestSpace space;
    // the tests' memory: low addresses, Xenia's dummy heap, the code
    const std::pair<uint32_t, uint32_t> regions[] = {{0, 0x200000}, {0x10000000, 0x10010000}, {START, START + 0x100000}};
    for (auto [a, b] : regions)
        if (!space.commit(a, b - a)) return std::printf("could not commit guest memory\n"), 1;

    // the time base counts up, one per read
    static uint64_t ticks = 0;
    System system;
    system.time_base = [](Context &) { return ++ticks; };
#ifdef XF_PPC_RECOMPILED
    for (size_t k = 0; k < xf_test_function_count; k++)
        functions[{xf_test_functions[k].suite, xf_test_functions[k].address}] = xf_test_functions[k].fn;
    system.unimplemented = [](Context &, Opcode op, uint32_t) {
        if (!missing) missing = true, missing_op = op;
    };
#endif

    Totals totals;
    for (const fs::path &source : sources) {
        std::string suite = source.stem().string();
        if (!filters.empty() && std::none_of(filters.begin(), filters.end(),
                                             [&](const std::string &f) { return suite.find(f) != std::string::npos; }))
            continue;
        std::vector<TestCase> cases;
        if (!load_suite(source, cases)) continue;
        std::ifstream b(fs::path(source).replace_extension(".bin"), std::ios::binary);
        std::vector<uint8_t> code((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
        uint64_t suite_failed = 0, suite_missing = 0;
        for (TestCase &t : cases) {
            if (skip.count(t.name)) {
                totals.skipped++;
                continue;
            }
            // fresh memory and registers, as Xenia's Setup per test
            // (the tests touch the first 64 KB, 0x100000 on, and Xenia's heap)
            std::memset(space.base(), 0, 0x10000);
            std::memset(space.base() + 0x100000, 0, 0x10000);
            std::memset(space.base() + 0x10000000, 0, 0x10000);
            std::memcpy(space.base() + START, code.data(), code.size());
            static Context c;
            std::memset(&c, 0, sizeof(c));
            c.base = space.base();
            c.lr = SENTINEL;
            c.msr = 0x9030; // Xenia's ThreadState, "dumped from a real 360"
            c.system = &system;
            for (auto &[key, value] : t.annotations) {
                size_t space_at = value.find(' ');
                std::string reg = value.substr(0, space_at), v = space_at == std::string::npos ? "" : value.substr(space_at + 1);
                int n;
                if (key == "REGISTER_IN") {
                    if (std::sscanf(reg.c_str(), "r%d", &n) == 1) c.r[n] = parse_u64(v);
                    else if (std::sscanf(reg.c_str(), "f%d", &n) == 1) c.f[n] = parse_double(v);
                    else if (std::sscanf(reg.c_str(), "v%d", &n) == 1) c.v[n] = parse_vec(v);
                    else if (reg == "cr") c.set_cr_word(uint32_t(parse_u64(v)));
                } else if (key == "MEMORY_IN") {
                    uint32_t address = uint32_t(std::strtoul(reg.c_str(), nullptr, 16));
                    std::vector<uint8_t> bytes = parse_bytes(v);
                    std::memcpy(space.base() + address, bytes.data(), bytes.size());
                }
            }
#ifdef XF_PPC_RECOMPILED
            current_suite = suite, missing = false;
            Stopped stop;
            auto fn = functions.find({suite, t.address});
            if (fn != functions.end()) {
                fn->second(c);
                if (missing) stop = {Stopped::Unimplemented, t.address, missing_op};
            } else {
                stop = interpret(c, t.address, SENTINEL, 1'000'000);
            }
#else
            Stopped stop = interpret(c, t.address, SENTINEL, 1'000'000);
#endif
            if (stop.reason == Stopped::Unimplemented || stop.reason == Stopped::Invalid) {
                totals.unimplemented++, suite_missing++;
                totals.missing[stop.reason == Stopped::Invalid ? "(invalid)" : info(stop.op).name]++;
                continue;
            }
            std::string failures;
            if (stop.reason == Stopped::Steps) failures += " (did not return)";
            for (auto &[key, value] : t.annotations) {
                size_t space_at = value.find(' ');
                std::string reg = value.substr(0, space_at), v = space_at == std::string::npos ? "" : value.substr(space_at + 1);
                char actual[96] = {};
                bool ok = true;
                int n;
                if (key == "REGISTER_OUT") {
                    if (std::sscanf(reg.c_str(), "r%d", &n) == 1) {
                        ok = c.r[n] == parse_u64(v);
                        std::snprintf(actual, sizeof(actual), "%016llX", (unsigned long long)c.r[n]);
                    } else if (std::sscanf(reg.c_str(), "f%d", &n) == 1) {
                        uint64_t bits;
                        std::memcpy(&bits, &c.f[n], 8);
                        if (v.find("0x") != std::string::npos) ok = bits == parse_u64(v, true);
                        else ok = c.f[n] == parse_double(v);
                        std::snprintf(actual, sizeof(actual), "%016llX", (unsigned long long)bits);
                    } else if (std::sscanf(reg.c_str(), "v%d", &n) == 1) {
                        Vec128 e = parse_vec(v);
                        ok = std::memcmp(e.u32, c.v[n].u32, 16) == 0;
                        std::snprintf(actual, sizeof(actual), "[%08X, %08X, %08X, %08X]", c.v[n].u32[0], c.v[n].u32[1],
                                      c.v[n].u32[2], c.v[n].u32[3]);
                    } else if (reg == "cr") {
                        ok = c.cr_word() == parse_u64(v);
                        std::snprintf(actual, sizeof(actual), "%016llX", (unsigned long long)c.cr_word());
                    }
                } else if (key == "MEMORY_OUT") {
                    uint32_t address = uint32_t(std::strtoul(reg.c_str(), nullptr, 16));
                    std::vector<uint8_t> bytes = parse_bytes(v);
                    ok = std::memcmp(space.base() + address, bytes.data(), bytes.size()) == 0;
                    std::snprintf(actual, sizeof(actual), "(memory differs)");
                } else {
                    continue;
                }
                if (!ok) failures += "\n      " + reg + ": expected " + v + ", got " + actual;
            }
            if (failures.empty()) {
                totals.passed++;
            } else {
                totals.failed++, suite_failed++;
                if (verbose) std::printf("  %s:%s\n", t.name.c_str(), failures.c_str());
            }
        }
        if (suite_failed || suite_missing)
            std::printf("%-40s %4zu cases: %llu failed, %llu not run\n", suite.c_str(), cases.size(),
                        (unsigned long long)suite_failed, (unsigned long long)suite_missing);
    }
    std::printf("passed %llu, failed %llu, not run %llu (instructions missing), skipped %llu\n",
                (unsigned long long)totals.passed, (unsigned long long)totals.failed,
                (unsigned long long)totals.unimplemented, (unsigned long long)totals.skipped);
    if (!totals.missing.empty()) {
        std::vector<std::pair<uint64_t, std::string>> missing;
        for (auto &[name, n] : totals.missing) missing.push_back({n, name});
        std::sort(missing.rbegin(), missing.rend());
        std::printf("missing:");
        for (auto &[n, name] : missing) std::printf(" %s %llu", name.c_str(), (unsigned long long)n);
        std::printf("\n");
    }
    return totals.failed || totals.unimplemented ? 1 : 0;
}
