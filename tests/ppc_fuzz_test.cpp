// The differential fuzzer's cases (docs/recompiler.md, "The differential
// fuzzer"), written by `xffuzz gen` with a fixed seed and recompiled by
// `xffuzz emit` at build time, run through the interpreter and through their
// generated code: every register, flag and byte of memory must agree. The
// generated code is built as the games' is (-O2), so what the emitter or the
// compiler does differently from the interpreter shows here.
//
//   build/ppc_fuzz_test [cases.xfz] [-v]

#include "xenoforge/fuzz.h"
#include "xenoforge/ppc_generated.h"
#include "xenoforge/ppc_interp.h"

#include <cstdio>
#include <map>
#include <string>

using namespace xf::ppc;
using namespace xf::ppc::fuzz;

struct XfFuzzFunction {
    uint32_t address;
    void (*fn)(Context &);
};
extern const XfFuzzFunction xf_fuzz_functions[];
extern const size_t xf_fuzz_function_count;

namespace xf::ppc {
// the cases make no calls; a stray one is interpreted
void dispatch(Context &c, uint32_t target) { interpret(c, target, uint32_t(c.lr), 10'000); }
void invalid(Context &, uint32_t) {}
} // namespace xf::ppc

int main(int argc, char **argv) {
    std::string path = XF_FUZZ_CASES;
    bool verbose = false;
    for (int k = 1; k < argc; k++) {
        std::string a = argv[k];
        if (a == "-v") verbose = true;
        else path = a;
    }
    std::vector<Case> cases;
    if (!read_cases(path.c_str(), cases)) return std::printf("cannot read %s\n", path.c_str()), 1;
    std::map<uint32_t, void (*)(Context &)> functions;
    for (size_t k = 0; k < xf_fuzz_function_count; k++) functions[xf_fuzz_functions[k].address] = xf_fuzz_functions[k].fn;
    Runner runner(cases);
    if (!runner.ok()) return std::printf("no guest memory\n"), 1;
    size_t compared = 0, failed = 0, missing = 0, unsupported = 0;
    std::map<std::string, size_t> by_op;
    for (size_t k = 0; k < cases.size(); k++) {
        auto fn = functions.find(case_address(k));
        if (fn == functions.end()) {
            missing++;
            continue;
        }
        Result interpreted = runner.run(k);
        Result generated = runner.run(k, fn->second);
        if (interpreted.status != RAN || generated.status != RAN) {
            unsupported++;
            continue;
        }
        compared++;
        std::vector<std::string> d = differences(interpreted.out, generated.out);
        if (d.empty()) continue;
        failed++;
        std::string op = info(focus(cases[k])).name;
        if (by_op[op]++ < 2 || verbose) {
            std::printf("case %zu (%s):", k, op.c_str());
            for (uint32_t w : cases[k].code) std::printf(" %08X", w);
            std::printf("\n");
            for (const std::string &f : d)
                std::printf("  %-12s interpreter %s, generated %s\n", f.c_str(), field_value(interpreted.out, f).c_str(),
                            field_value(generated.out, f).c_str());
        }
    }
    for (const auto &[op, n] : by_op) std::printf("%-14s %zu\n", op.c_str(), n);
    std::printf("%zu cases: %zu compared, %zu differ, %zu without generated code, %zu not run\n", cases.size(), compared,
                failed, missing, unsupported);
    return failed || missing ? 1 : 0;
}
