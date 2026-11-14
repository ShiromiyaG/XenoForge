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
// function of the same suite or the interpreter. Built with XF_PPC_STENCIL
// (ppc_stencil_test), each suite's cases are made into code by the
// copy-and-patch tier (stencil.h) and run there, and each case is run through
// the interpreter too: the context and the memory they leave must be the
// same, besides the expected values.

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

#ifdef XF_PPC_STENCIL
#include "xenoforge/game_library.h"
#include "xenoforge/recomp.h"
#include "xenoforge/stencil.h"
namespace {
std::map<uint32_t, void (*)(Context &)> functions; // the suite's, by address
bool missing = false;
Opcode missing_op = Opcode::Invalid;
} // namespace
namespace xf::ppc {
constexpr uint32_t THROWS = 0x80001000; // a call there throws (unwind_check)
void dispatch(Context &c, uint32_t target) {
    if (target == THROWS) throw Unwind{target};
    auto it = functions.find(target);
    if (it != functions.end()) return it->second(c);
    Stopped s = interpret(c, target, uint32_t(c.lr), 1'000'000);
    if (s.reason != Stopped::Returned) missing = true, missing_op = s.op;
}
void invalid(Context &, uint32_t) { missing = true, missing_op = Opcode::Invalid; }
} // namespace xf::ppc
namespace {
// A C++ exception thrown from a dispatch the tier's code made, two calls
// deep in it (a stencil with a frame, then the call's), caught outside: as an
// Unwind (ppc_module.h) crosses it, through the unwind information the tier
// registers for its copies.
bool unwind_check(uint8_t *base) {
    const uint32_t start = 0x80000000;
    const uint32_t words[] = {
        0x48000011, // 80000000 bl 80000010
        0x4E800020, // 80000004 blr
        0x60000000, 0x60000000,
        0x80640000, // 80000010 lwz r3, 0(r4)
        0x48000FED, // 80000014 bl 80001000 (no function: dispatch, which throws)
        0x4E800020, // 80000018 blr
    };
    xf::recomp::Image image;
    image.base = image.entry = start;
    for (uint32_t w : words)
        for (int k = 3; k >= 0; k--) image.bytes.push_back(uint8_t(w >> (8 * k)));
    image.sections.push_back({".text", start, uint32_t(image.bytes.size()), true, false});
    xf::recomp::Discovery d;
    for (auto [a, size] : {std::pair<uint32_t, uint32_t>{start, 8}, {start + 0x10, 12}}) {
        xf::recomp::Function f;
        f.address = a, f.size = size, f.source = xf::recomp::Source::Pdata;
        d.functions[a] = f;
    }
    Runtime runtime = core_runtime();
    runtime.dispatch = &dispatch;
    runtime.invalid = &invalid;
    xf::stencil::Stats stats;
    std::string error;
    auto module = xf::stencil::compile(image, d, runtime, "unwind", 0, {}, stats, error);
    if (!module || module->table()->count != 2) return false;
    static Context c;
    std::memset(&c, 0, sizeof(c));
    c.base = base;
    c.r[4] = 0x100;
    volatile int after = 0;
    try {
        module->table()->functions[0].fn(c);
    } catch (const Unwind &u) {
        after = u.target == THROWS ? 1 : 2;
    }
    return after == 1 && c.lr == start + 0x18;
}

// The control flow the emitter has beside the instructions, in the tier's
// code: checked returns, stack switches, resume sites, the port's hooks, and
// the tier-up's jump to a library's function. Each program is a few words at
// 0x80000000 with its functions given; the runtime's calls are the test's.
namespace control {
constexpr uint32_t BASE = 0x80000000, THUNK = 0x80003000, THROW_TO = 0x80002000;
uint32_t switch_thunk = 0, switch_resume = 0, hooked_calls = 0, mid_answer = 0;
uint32_t throw_target = 0, throw_sp = 0;
void dispatch(Context &c, uint32_t target) {
    if (target == THROW_TO) {
        // an Unwind to a resume site, with r1 inside the frame that takes it
        c.r[1] = throw_sp;
        throw Unwind{throw_target};
    }
    xf::ppc::dispatch(c, target);
}
void switch_stack(Context &c, uint32_t thunk, uint32_t resume) {
    // as if the stack came back where it was left: LR is the resume address
    switch_thunk = thunk, switch_resume = resume;
    c.lr = resume;
}
[[noreturn]] void unwind(Context &c) { throw Unwind{uint32_t(c.lr) & ~3u}; }
void hooked(Context &c, HookSlot &slot) {
    hooked_calls++;
    slot.original(c);
}
uint32_t mid_hook(Context &, HookSlot &) { return mid_answer; }

struct Program {
    xf::recomp::Image image;
    xf::recomp::Discovery d;
    std::unique_ptr<xf::stencil::Module> module;
    std::string error;
    Program(std::initializer_list<uint32_t> words) {
        image.base = image.entry = BASE;
        for (uint32_t w : words)
            for (int k = 3; k >= 0; k--) image.bytes.push_back(uint8_t(w >> (8 * k)));
        image.sections.push_back({".text", BASE, uint32_t(image.bytes.size()), true, false});
    }
    xf::recomp::Function &function(uint32_t address, uint32_t size) {
        xf::recomp::Function &f = d.functions[address];
        f.address = address, f.size = size, f.source = xf::recomp::Source::Pdata;
        return f;
    }
    bool compile(const xf::stencil::Project *project = nullptr) {
        Runtime runtime = core_runtime();
        runtime.dispatch = &dispatch;
        runtime.invalid = &xf::ppc::invalid;
        runtime.switch_stack = &switch_stack;
        runtime.unwind = &unwind;
        runtime.hooked = &hooked;
        runtime.mid_hook = &mid_hook;
        xf::stencil::Stats stats;
        xf::stencil::Options options;
        options.project = project;
        module = xf::stencil::compile(image, d, runtime, "control", 0, options, stats, error);
        return module != nullptr;
    }
    void (*fn(uint32_t address) const)(Context &) {
        const GeneratedModule &m = *module->table();
        for (size_t k = 0; k < m.count; k++)
            if (m.functions[k].address == address) return m.functions[k].fn;
        return nullptr;
    }
};

int run_checks(uint8_t *base) {
    int failed = 0;
    auto check = [&](bool ok, const char *what) {
        std::printf("copy-and-patch: %s: %s\n", what, ok ? "ok" : "FAILED");
        failed += !ok;
    };
    static Context c;
    auto fresh = [&] {
        std::memset(&c, 0, sizeof(c));
        c.base = base;
        c.lr = 0x12345678, c.r[1] = 0x70000000;
    };
    {
        // a function that switches the stack: a return with LR and r1 as at its
        // entry goes back to its caller; one with r1 moved is thrown to LR
        Program p({0x38630001,   // 80000000 addi r3, r3, 1
                   0x4E800020,   // 80000004 blr
                   0x38210010,   // 80000008 addi r1, r1, 16
                   0x4E800020}); // 8000000C blr
        p.function(BASE, 8).switches_stack = true;
        p.function(BASE + 8, 8).switches_stack = true;
        bool ok = p.compile() && p.fn(BASE) && p.fn(BASE + 8);
        if (ok) {
            fresh();
            p.fn(BASE)(c);
            ok = c.r[3] == 1;
            fresh();
            uint32_t to = 0;
            try {
                p.fn(BASE + 8)(c);
            } catch (const Unwind &u) {
                to = u.target;
            }
            ok = ok && to == 0x12345678;
        }
        check(ok, "checked returns");
    }
    {
        // a branch to the stack switch's thunk goes on, when switched back to,
        // where the entry's LR says when the returns are checked, where LR
        // says when not; a call to it, after the call
        Program p({0x3C001111,   // 80000000 lis r0, 0x1111
                   0x7C0803A6,   // 80000004 mtlr r0
                   0x48002FF8,   // 80000008 b 80003000 (the thunk)
                   0x60000000,   // 8000000C
                   0x3C001111,   // 80000010 lis r0, 0x1111
                   0x7C0803A6,   // 80000014 mtlr r0
                   0x48002FE8,   // 80000018 b 80003000
                   0x60000000,   // 8000001C
                   0x48002FE1,   // 80000020 bl 80003000
                   0x38630001,   // 80000024 addi r3, r3, 1
                   0x4E800020}); // 80000028 blr
        p.function(BASE, 12).switches_stack = true;
        p.function(BASE + 0x10, 12);
        p.function(BASE + 0x20, 12);
        p.d.stack_thunks.insert(THUNK);
        bool ok = p.compile();
        if (ok) {
            fresh();
            p.fn(BASE)(c);
            ok = switch_thunk == THUNK && switch_resume == 0x12345678;
            fresh();
            p.fn(BASE + 0x10)(c);
            ok = ok && switch_resume == 0x11110000;
            fresh();
            p.fn(BASE + 0x20)(c);
            ok = ok && switch_resume == BASE + 0x24 && c.r[3] == 1;
        }
        check(ok, "stack switches");
    }
    {
        // an Unwind to a resume site, with r1 inside the frame, goes on there
        Program p({0x38630001,   // 80000000 addi r3, r3, 1
                   0x48001FFD,   // 80000004 bl 80002000 (throws to 80000008)
                   0x38630064,   // 80000008 addi r3, r3, 100
                   0x4E800020}); // 8000000C blr
        p.function(BASE, 16).resumes = {BASE + 8};
        bool ok = p.compile() && p.module->table()->resume_count == 1 && p.module->table()->resumes[0] == BASE + 8;
        if (ok) {
            fresh();
            throw_target = BASE + 8, throw_sp = 0x70000000 - 0x20;
            try {
                p.fn(BASE)(c);
            } catch (const Unwind &) {
                ok = false;
            }
            ok = ok && c.r[3] == 101;
        }
        check(ok, "resume sites");
    }
    {
        // a hooked function's slot, and a mid-function hook's answers
        Program p({0x38630001,   // 80000000 addi r3, r3, 1
                   0x3863000A,   // 80000004 addi r3, r3, 10 (the mid hook, may go on at 80000008)
                   0x38630064,   // 80000008 addi r3, r3, 100
                   0x4E800020}); // 8000000C blr
        p.function(BASE, 16);
        xf::stencil::Project project;
        project.hooks[BASE].slot = true;
        project.hooks[BASE].mid[BASE + 4] = {BASE + 8};
        bool ok = p.compile(&project) && p.module->table()->hook_count == 2;
        HookSlot *function = nullptr, *mid = nullptr;
        for (size_t k = 0; ok && k < p.module->table()->hook_count; k++)
            (p.module->table()->hooks[k]->original ? function : mid) = p.module->table()->hooks[k];
        ok = ok && function && mid && function->address == BASE && mid->address == BASE + 4;
        static int anything;
        auto run = [&](uint32_t answer) {
            fresh();
            mid_answer = answer;
            p.fn(BASE)(c);
            return uint32_t(c.r[3]);
        };
        if (ok) {
            ok = run(0) == 111 && hooked_calls == 0; // nothing hooked: past both
            mid->hooks = &anything;
            ok = ok && run(0) == 111 && run(BASE + 8) == 101 && run(1) == 1;
            function->hooks = &anything;
            ok = ok && run(0) == 111 && hooked_calls == 1;
        }
        check(ok, "the port's hooks");
    }
    {
        // the tier-up: a call from the tier's code to a function the library
        // has goes to the library's code once adopted
        Program p({0x48000009,   // 80000000 bl 80000008
                   0x4E800020,   // 80000004 blr
                   0x38630001,   // 80000008 addi r3, r3, 1
                   0x4E800020}); // 8000000C blr
        p.function(BASE, 8);
        p.function(BASE + 8, 8);
        bool ok = p.compile();
        if (ok) {
            fresh();
            p.fn(BASE)(c);
            ok = c.r[3] == 1;
            static const GeneratedFunction library_functions[] = {{BASE + 8, [](Context &c) { c.r[3] += 1000; }}};
            GeneratedModule library = *p.module->table();
            library.functions = library_functions, library.count = 1;
            std::string error;
            ok = ok && p.module->adopt(library, error) == 1;
            fresh();
            p.fn(BASE)(c);
            ok = ok && c.r[3] == 1000;
        }
        check(ok, "the tier-up's jump to the library");
    }
    return failed;
}
} // namespace control
} // namespace
#endif

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
    bool verbose = false, all = false;
    for (int k = 1; k < argc; k++) {
        if (std::string(argv[k]) == "-v") verbose = true;
        else if (std::string(argv[k]) == "--all") all = true; // Xenia's skipped cases too
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
#ifdef XF_PPC_STENCIL
    system.unimplemented = [](Context &, Opcode op, uint32_t) {
        if (!missing) missing = true, missing_op = op;
    };
#endif
#ifdef XF_PPC_RECOMPILED
    for (size_t k = 0; k < xf_test_function_count; k++)
        functions[{xf_test_functions[k].suite, xf_test_functions[k].address}] = xf_test_functions[k].fn;
    system.unimplemented = [](Context &, Opcode op, uint32_t) {
        if (!missing) missing = true, missing_op = op;
    };
#endif

#ifdef XF_PPC_STENCIL
    if (!unwind_check(space.base()))
        return std::printf("copy-and-patch: an exception did not cross the tier's code\n"), 1;
    std::printf("copy-and-patch: an exception crossed the tier's code\n");
    if (control::run_checks(space.base())) return 1;
#endif
    Totals totals;
    [[maybe_unused]] uint64_t stencil_cases = 0, differ = 0;
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
#ifdef XF_PPC_STENCIL
        // the suite's cases as functions (each up to the next), made into code
        std::unique_ptr<xf::stencil::Module> module;
        functions.clear();
        {
            xf::recomp::Image image;
            image.base = image.entry = START;
            image.bytes = code;
            image.sections.push_back({".text", START, uint32_t(code.size()), true, false});
            std::vector<uint32_t> starts;
            for (const TestCase &t : cases) starts.push_back(t.address);
            std::sort(starts.begin(), starts.end());
            starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
            xf::recomp::Discovery d;
            for (size_t k = 0; k < starts.size(); k++) {
                xf::recomp::Function f;
                f.address = starts[k];
                f.size = (k + 1 < starts.size() ? starts[k + 1] : START + uint32_t(code.size())) - starts[k];
                f.source = xf::recomp::Source::Pdata;
                d.functions[f.address] = f;
            }
            Runtime runtime = core_runtime();
            runtime.dispatch = &dispatch;
            runtime.invalid = &invalid;
            runtime.unwind = [](Context &) { std::abort(); };
            runtime.switch_stack = [](Context &, uint32_t, uint32_t) { std::abort(); };
            xf::stencil::Stats stats;
            std::string error;
            module = xf::stencil::compile(image, d, runtime, suite, 0, {}, stats, error);
            if (!module) return std::printf("the copy-and-patch tier: %s\n", error.c_str()), 1;
            const GeneratedModule *table = module->table();
            for (size_t k = 0; k < table->count; k++) functions[table->functions[k].address] = table->functions[k].fn;
        }
#endif
        for (TestCase &t : cases) {
            if (!all && skip.count(t.name)) {
                totals.skipped++;
                continue;
            }
            static Context c;
            auto setup = [&] {
                // fresh memory and registers, as Xenia's Setup per test
                // (the tests touch the first 64 KB, 0x100000 on, and Xenia's heap)
                std::memset(space.base(), 0, 0x10000);
                std::memset(space.base() + 0x100000, 0, 0x10000);
                std::memset(space.base() + 0x10000000, 0, 0x10000);
                std::memcpy(space.base() + START, code.data(), code.size());
                std::memset(&c, 0, sizeof(c));
                c.base = space.base();
                c.lr = SENTINEL;
                c.msr = 0x9030; // Xenia's ThreadState, "dumped from a real 360"
                c.system = &system;
                for (auto &[key, value] : t.annotations) {
                    size_t space_at = value.find(' ');
                    std::string reg = value.substr(0, space_at),
                                v = space_at == std::string::npos ? "" : value.substr(space_at + 1);
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
            };
            setup();
#ifdef XF_PPC_STENCIL
            // the interpreter's run first, what it leaves kept to compare
            const uint64_t ticks_before = ticks; // mftb's, the same for both runs
            const Stopped expected = interpret(c, t.address, SENTINEL, 1'000'000);
            static Context interpreted;
            std::memcpy(&interpreted, &c, sizeof(c));
            // the memory the tests use (setup's), the code's included
            const std::pair<uint32_t, uint32_t> used[] = {{0, 0x10000},
                                                          {0x100000, 0x110000},
                                                          {0x10000000, 0x10010000},
                                                          {START, START + uint32_t(code.size())}};
            static std::vector<uint8_t> memory_after;
            memory_after.clear();
            for (auto [a, b] : used) memory_after.insert(memory_after.end(), space.base() + a, space.base() + b);
            setup();
            ticks = ticks_before;
            missing = false;
            Stopped stop;
            if (auto fn = functions.find(t.address); fn != functions.end()) {
                fn->second(c);
                if (missing) stop = {Stopped::Unimplemented, t.address, missing_op};
                stencil_cases++;
            } else {
                stop = interpret(c, t.address, SENTINEL, 1'000'000);
            }
            if (expected.reason == Stopped::Returned && stop.reason == Stopped::Returned) {
                // the same context (but pc, which the interpreter sets at every
                // instruction) and the same memory
                interpreted.pc = c.pc;
                bool same = std::memcmp(&interpreted, &c, sizeof(c)) == 0;
                size_t at = 0;
                for (auto [a, b] : used) {
                    same = same && std::memcmp(space.base() + a, memory_after.data() + at, b - a) == 0;
                    at += b - a;
                }
                if (!same) {
                    differ++;
                    std::printf("  %s: differs from the interpreter\n", t.name.c_str());
                }
            }
#endif
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
#elif !defined(XF_PPC_STENCIL)
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
#ifdef XF_PPC_STENCIL
    std::printf("copy-and-patch: %llu cases run in its code, %llu differ from the interpreter\n",
                (unsigned long long)stencil_cases, (unsigned long long)differ);
    if (differ || (!stencil_cases && totals.passed)) return 1;
#endif
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
