// xffuzz: the differential fuzzer's command line (docs/recompiler.md, "The
// differential fuzzer"; xenoforge/fuzz.h).
//
//   xffuzz gen <cases.xfz> [--seed N] [--per-op N] [--single P] [--only op,op,...]
//   xffuzz run <cases.xfz> <out.xfs>                 the interpreter's states
//   xffuzz emit <cases.xfz> <dir> [--files N]        the cases recompiled (tests/ppc_fuzz_test.cpp)
//   xffuzz compare <cases.xfz> <a.xfs> <b.xfs> [--names A,B] [--examples N] [--ignore field,...]
//                  [--only op,...] [--singles] [--known]
//
// compare groups the cases that ran on both sides by their focus instruction
// and counts the fields that differ ("fpscr.FI", "f", "cr", "memory"...),
// with examples: the case's code, the differing fields before and after on
// both sides, and the registers its instructions name.

#include "xenoforge/fuzz.h"
#include "xenoforge/recomp.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>

using namespace xf::ppc;
using namespace xf::ppc::fuzz;
namespace recomp = xf::recomp;

namespace {

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        if (comma > start) out.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

int usage() {
    std::fprintf(stderr, "usage: xffuzz gen|run|emit|compare ... (see tools/xffuzz.cpp)\n");
    return 2;
}

int emit(const std::vector<Case> &cases, const std::filesystem::path &out, int files) {
    recomp::Image image;
    image.base = CODE_BASE;
    image.bytes = code_image(cases);
    image.sections.push_back({".text", CODE_BASE, uint32_t(image.bytes.size()), true, false});
    for (size_t k = 0; k < cases.size(); k++) image.pdata.push_back({case_address(k), CASE_STRIDE, 0, false});
    recomp::Discovery d = recomp::discover(image, {.scan_pointers = false});
    std::filesystem::create_directories(out);
    std::vector<std::string> bodies(files), declarations(files);
    std::string table = "extern const XfFuzzFunction xf_fuzz_functions[] = {\n", all;
    size_t n = 0;
    for (const auto &[a, f] : d.functions) {
        size_t file = n++ % files;
        std::string name = recomp::function_name("z_", a);
        declarations[file] += "XF_FN(" + name + ");\n";
        bodies[file] += recomp::emit_function(image, d, f, "z_");
        all += "XF_FN(" + name + ");\n";
        table += "    {0x" + recomp::function_name("", a) + "u, " + name + "},\n";
    }
    auto write = [](const std::filesystem::path &p, const std::string &text) {
        // only when the text changes, so the build does not redo what is the same
        std::ifstream in(p, std::ios::binary);
        std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (old == text) return;
        std::ofstream(p, std::ios::binary) << text;
    };
    for (int k = 0; k < files; k++)
        write(out / ("fuzz_" + std::to_string(k) + ".cpp"),
              "// Written by xffuzz emit: the fuzzer's cases, recompiled.\n#include \"xenoforge/ppc_generated.h\"\n\n" +
                  declarations[k] + bodies[k]);
    write(out / "fuzz_table.cpp",
          "// Written by xffuzz emit: every recompiled case by address.\n#include \"xenoforge/ppc_generated.h\"\n"
          "#include <cstddef>\n\n" +
              all + "\nstruct XfFuzzFunction { uint32_t address; void (*fn)(xf::ppc::Context &); };\n" + table +
              "};\nextern const size_t xf_fuzz_function_count = " + std::to_string(n) + ";\n");
    std::printf("%zu cases, %zu functions\n", cases.size(), n);
    return 0;
}

std::string field_class(const std::string &f) {
    if (f.starts_with("fpscr") || f.starts_with("xer") || f.starts_with("vscr")) return f;
    std::string c = f;
    while (!c.empty() && std::isdigit(uint8_t(c.back()))) c.pop_back();
    return c;
}

// --known (a = XenoForge, b = Xenia): the departures from Xenia that
// docs/recompiler.md ("Semantics, where it departs from Xenia") makes on
// purpose are taken out of Xenia's state, so what is left is unexplained.
// False: the case holds an instruction Xenia has no semantics for (it
// compiles them as nothing) or whose difference is the whole result.
bool known_xenia(const Case &cs, const State &ours, State &xenia) {
    static const std::set<Opcode> lacking = {
        // Xenia has none of these: the AltiVec PEM's
        Opcode::vmsummbm, Opcode::vmsumshm, Opcode::vmsumshs, Opcode::vmsumubm, Opcode::vmsumuhm, Opcode::vmsumuhs,
        Opcode::vmulesb, Opcode::vmulesh, Opcode::vmuleub, Opcode::vmuleuh, Opcode::vmulosb, Opcode::vmulosh,
        Opcode::vmuloub, Opcode::vmulouh, Opcode::vsum2sws, Opcode::vsum4sbs, Opcode::vsum4shs, Opcode::vsum4ubs,
        Opcode::vsumsws, Opcode::vmhaddshs, Opcode::vmhraddshs, Opcode::vmladduhm, Opcode::vupkhpx, Opcode::vupklpx,
        // the string instructions (the manual's, tests/ppc)
        Opcode::lswi, Opcode::lswx, Opcode::stswi, Opcode::stswx,
        // the console's per-byte shift counts (Xenia uses byte 15's)
        Opcode::vsl, Opcode::vsr,
    };
    for (uint32_t w : cs.code)
        if (lacking.count(decode(w))) return false;
    // OE (XER[OV], SO) and CR's SO copied from XER[SO]: Xenia models neither
    xenia.xer_ov = ours.xer_ov, xenia.xer_so = ours.xer_so;
    uint32_t so_bits = 0x11111111;
    xenia.cr = (xenia.cr & ~so_bits) | (ours.cr & so_bits);
    // CA set by the OE forms of addc, subfc, subfe, addme too
    for (uint32_t w : cs.code)
        if (info(decode(w)).form == Form::XO && (w & 0x400)) xenia.xer_ca = ours.xer_ca;
    // VSCR[SAT]: set by saturation and sticky (Xenia's backend never sets it,
    // and its mtvscr leaves it alone)
    if (ours.vscr_sat && !xenia.vscr_sat) xenia.vscr_sat = 1;
    for (uint32_t w : cs.code) {
        Opcode op = decode(w);
        // mcrxr's field from XER's SO and OV too
        if (op == Opcode::mcrxr) {
            uint32_t shift = 28 - 4 * ((w >> 23) & 7);
            if (((xenia.cr >> shift) & 3) == ((ours.cr >> shift) & 3))
                xenia.cr = (xenia.cr & ~(15u << shift)) | (ours.cr & (15u << shift));
        }
        // mtvscr sets SAT (Xenia's sets only its copy of VSCR)
        if (op == Opcode::mtvscr) xenia.vscr_sat = ours.vscr_sat;
        // sraw/srad: CA from the sign past 31/63 bits
        if (op == Opcode::srawx || op == Opcode::sradx) xenia.xer_ca = ours.xer_ca;
    }
    // the FPSCR left alone by arithmetic without Rc (Xenia clears FEX and VX on every one)
    if (ours.fpscr == cs.in.fpscr) xenia.fpscr = ours.fpscr;
    // mfxer reads OV and SO too
    for (uint32_t w : cs.code)
        if (decode(w) == Opcode::mfspr && ((w >> 16) & 31) == 1) {
            uint32_t rt = (w >> 21) & 31;
            if ((xenia.r[rt] & ~0xC0000000ull) == (ours.r[rt] & ~0xC0000000ull)) xenia.r[rt] = ours.r[rt];
        }
    return true;
}

int compare(const std::vector<Case> &cases, const std::vector<Result> &a, const std::vector<Result> &b,
            const std::vector<std::string> &names, size_t examples, const std::set<std::string> &ignore,
            const std::set<std::string> &only, bool singles, bool known) {
    if (a.size() != cases.size() || b.size() != cases.size()) {
        std::fprintf(stderr, "the states do not match the cases\n");
        return 1;
    }
    struct Op {
        size_t compared = 0, diverged = 0, shown = 0;
        std::map<std::string, size_t> fields;
    };
    std::map<std::string, Op> ops;
    size_t compared = 0, diverged = 0, skipped = 0;
    std::map<std::string, size_t> unsupported;
    static const std::regex reg_token(R"(\b(vr|fr|r|f|v)(\d+)\b)");
    for (size_t k = 0; k < cases.size(); k++) {
        const Case &cs = cases[k];
        std::string op = info(focus(cs)).name;
        if (!only.empty() && !only.count(op)) continue;
        if (singles && cs.code.size() != 1) continue;
        if (a[k].status != RAN || b[k].status != RAN) {
            skipped++;
            if (a[k].status == UNSUPPORTED || b[k].status == UNSUPPORTED)
                unsupported[op + (a[k].status == UNSUPPORTED ? " (" + names[0] + ")" : " (" + names[1] + ")")]++;
            else
                unsupported[op + " (stopped)"]++;
            continue;
        }
        State theirs = b[k].out;
        if (known && !known_xenia(cs, a[k].out, theirs)) {
            skipped++, unsupported[op + " (known)"]++;
            continue;
        }
        std::vector<std::string> d = differences(a[k].out, theirs);
        std::erase_if(d, [&](const std::string &f) { return ignore.count(f) || ignore.count(field_class(f)) || ignore.count(f.substr(0, f.find('.'))); });
        Op &o = ops[op];
        o.compared++, compared++;
        if (d.empty()) continue;
        o.diverged++, diverged++;
        std::set<std::string> classes;
        for (const std::string &f : d) classes.insert(field_class(f));
        for (const std::string &c : classes) o.fields[c]++;
        if (o.shown >= examples) continue;
        o.shown++;
        std::printf("-- %s, case %zu:\n", op.c_str(), k);
        std::set<std::string> named;
        for (size_t w = 0; w < cs.code.size(); w++) {
            std::string text = disassemble(case_address(k) + uint32_t(4 * w), cs.code[w]);
            std::printf("   %08X  %s\n", cs.code[w], text.c_str());
            for (std::sregex_iterator it(text.begin(), text.end(), reg_token), end; it != end; ++it)
                named.insert(std::string(1, (*it)[1].str().front() == 'r' ? 'r' : (*it)[1].str().front()) + (*it)[2].str());
        }
        for (const std::string &f : named)
            if (std::find(d.begin(), d.end(), f) == d.end() && !f.starts_with("cr"))
                std::printf("   %-10s in %s\n", f.c_str(), field_value(cs.in, f).c_str());
        std::set<std::string> printed;
        for (const std::string &f : d) {
            std::string key = f.starts_with("fpscr") ? "fpscr" : f.starts_with("cr") ? "cr" : f;
            if (!printed.insert(key).second) continue;
            if (key == "memory") {
                for (uint32_t m = 0; m < WINDOW_SIZE; m++)
                    if (a[k].out.memory[m] != theirs.memory[m]) {
                        std::printf("   memory     first differs at %08X: in %02X, %s %02X, %s %02X\n", WINDOW + m,
                                    cs.in.memory[m], names[0].c_str(), a[k].out.memory[m], names[1].c_str(),
                                    theirs.memory[m]);
                        break;
                    }
                continue;
            }
            std::printf("   %-10s in %s, %s %s, %s %s\n", key.c_str(), field_value(cs.in, f).c_str(), names[0].c_str(),
                        field_value(a[k].out, f).c_str(), names[1].c_str(), field_value(theirs, f).c_str());
        }
        if (printed.count("fpscr")) {
            std::string bits;
            for (const std::string &f : d)
                if (f.starts_with("fpscr.")) bits += " " + f.substr(6);
            std::printf("   fpscr bits:%s\n", bits.c_str());
        }
    }
    std::printf("\n%-14s %8s %8s  fields\n", "instruction", "compared", "diverged");
    for (const auto &[name, o] : ops) {
        if (!o.diverged) continue;
        std::string f;
        for (const auto &[c, n] : o.fields) f += " " + c + " " + std::to_string(n);
        std::printf("%-14s %8zu %8zu %s\n", name.c_str(), o.compared, o.diverged, f.c_str());
    }
    std::printf("\n%zu cases compared, %zu diverged in %zu instructions; %zu not run on both sides\n", compared,
                diverged, size_t(std::count_if(ops.begin(), ops.end(), [](auto &o) { return o.second.diverged > 0; })),
                skipped);
    if (!unsupported.empty()) {
        std::printf("not run:");
        for (const auto &[op, n] : unsupported) std::printf(" %s %zu", op.c_str(), n);
        std::printf("\n");
    }
    return diverged ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    std::string command = argv[1];
    std::vector<std::string> args;
    std::map<std::string, std::string> flags;
    for (int k = 2; k < argc; k++) {
        std::string s = argv[k];
        if (s.starts_with("--")) {
            bool valued = s != "--singles" && s != "--known";
            flags[s] = valued && k + 1 < argc ? argv[++k] : "";
        } else {
            args.push_back(s);
        }
    }
    if (command == "gen") {
        Options o;
        if (flags.count("--seed")) o.seed = std::stoull(flags["--seed"]);
        if (flags.count("--per-op")) o.per_op = std::stoul(flags["--per-op"]);
        if (flags.count("--single")) o.single_percent = std::stoul(flags["--single"]);
        if (flags.count("--only")) o.only = split(flags["--only"]);
        std::vector<Case> cases = generate(o);
        if (!write_cases(args[0].c_str(), cases)) return std::fprintf(stderr, "cannot write %s\n", args[0].c_str()), 1;
        std::printf("%zu cases\n", cases.size());
        return 0;
    }
    std::vector<Case> cases;
    if (!read_cases(args[0].c_str(), cases)) return std::fprintf(stderr, "cannot read %s\n", args[0].c_str()), 1;
    if (command == "run" && args.size() == 2) {
        Runner runner(cases);
        if (!runner.ok()) return std::fprintf(stderr, "no guest memory\n"), 1;
        std::vector<Result> results(cases.size());
        size_t unsupported = 0, stopped = 0;
        for (size_t k = 0; k < cases.size(); k++) {
            results[k] = runner.run(k);
            unsupported += results[k].status == UNSUPPORTED, stopped += results[k].status == STOPPED;
        }
        if (!write_results(args[1].c_str(), results)) return std::fprintf(stderr, "cannot write %s\n", args[1].c_str()), 1;
        std::printf("%zu cases: %zu unsupported, %zu stopped\n", cases.size(), unsupported, stopped);
        return 0;
    }
    if (command == "emit" && args.size() == 2)
        return emit(cases, args[1], flags.count("--files") ? std::stoi(flags["--files"]) : 8);
    if (command == "compare" && args.size() == 3) {
        std::vector<Result> a, b;
        if (!read_results(args[1].c_str(), a) || !read_results(args[2].c_str(), b))
            return std::fprintf(stderr, "cannot read the states\n"), 1;
        std::vector<std::string> names = flags.count("--names") ? split(flags["--names"]) : std::vector<std::string>{};
        if (names.size() != 2) names = {"a", "b"};
        std::set<std::string> ignore, only;
        for (const std::string &s : split(flags["--ignore"])) ignore.insert(s);
        for (const std::string &s : split(flags["--only"])) only.insert(s);
        size_t examples = flags.count("--examples") ? std::stoul(flags["--examples"]) : 2;
        return compare(cases, a, b, names, examples, ignore, only, flags.count("--singles") > 0, flags.count("--known") > 0);
    }
    return usage();
}
