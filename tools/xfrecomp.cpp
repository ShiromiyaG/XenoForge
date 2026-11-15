// xfrecomp: the recompiler's command (plan step A2).
//
//   xfrecomp analyze <game.iso | default.xex> [--xex <path on the disc>]
//            [--compare <rexglue's *_init.cpp>] [--functions <out.tsv>]
//            [--tables <out.tsv>] [--unresolved <out.tsv>] [--no-pointers]
//            [--project <file.toml>] [--check-functions <file.toml>] [--calls [<counts>]]
//
// prints what discovery found by kind; --compare sets the function starts
// against rexglue's PPCFuncMappings for the same executable; --functions
// writes every function (address, size, source, name, whether it switches or
// saves the stack, its resume sites), --tables the jump
// tables, --unresolved the branches out of a function to no start (function,
// branch, target); --no-pointers leaves the data pointer scan out.
// --project discovers with a project file's starts and jump tables for the
// executable (docs/recompiler.md, "The project file"); --check-functions
// says how many of a project file's starts (ours, or rexglue's function list)
// discovery finds without them, and where the others fall. --calls counts the
// functions by how they keep the calling convention and what their calls cost
// in the context (report_calls), weighted by a file of counted calls if given.
//
//   xfrecomp tests <assembled tests dir> <out dir> [--files N]
//
// recompiles Xenia's instruction tests (tools/assemble_ppc_tests.sh) into
// out/ppc_tests_0..N-1.cpp and ppc_tests_table.cpp for ppc_recompiled_test:
// every test label a function as long as the code up to the next label.
//
//   xfrecomp emit <game.iso | file.xex> <out dir> [--xex <path>]... [--files N] [--no-pointers] [--listing]
//                 [--project <file.toml>]
//
// writes the generated code of every executable on the disc (each file that
// is an XEX2 executable, but title updates and the system update's), or of
// those --xex names, or of the bare executable: one directory each, named
// after its path ("default_xex", "waves_L360_dll"), holding its functions as
// C++ in a namespace of its own (functions_K.cpp: 64 KB of the image each, or
// about N files with --files; each declares what it calls) and table.cpp
// with its table (ppc_module.h: the path, base, size and digest of the image
// it was made from, the functions by address); and modules.cpp listing every
// module's table for the game library (xf_modules). A file is written only
// when its text changes, so the build after a discovery fix compiles the
// files that fix touched. Game code: the out dir stays outside the
// repository. --listing writes functions_K.ppc beside each functions_K.cpp
// (each instruction's address, word and disassembly) and #line directives
// in the C++ pointing at it, so a debugger steps PPC instructions
// (docs/recompiler.md, "PPC in the host debugger"). --project applies a
// port's project file: its starts, names, jump tables, functions left to the
// interpreter, and the hook slots of what the port hooks; without one (or
// with an empty one) the emission is the same as ever.

#include "calling_convention.h"
#include "xenoforge/disc.h"
#include "xenoforge/game_build.h"
#include "xenoforge/ppc.h"
#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace xf;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: xfrecomp analyze <game.iso | default.xex> [--xex <path>] [--compare <init.cpp>] "
                 "[--functions <out.tsv>] [--tables <out.tsv>] [--unresolved <out.tsv>] [--no-pointers]\n"
                 "                [--project <file.toml>] [--check-functions <file.toml>] [--calls [<counts>]]\n"
                 "       xfrecomp tests <assembled tests dir> <out dir> [--files N]\n"
                 "       xfrecomp emit <game.iso | file.xex> <out dir> [--xex <path>]... [--files N] "
                 "[--no-pointers] [--listing] [--project <file.toml>]\n"
                 "       xfrecomp jobs [N]\n"
                 "       xfrecomp signatures <reference game> --project <file.toml> --out <file> [--xex <path>] "
                 "[--prefix P]...\n"
                 "       xfrecomp identify <game> --signatures <file>... [--xex <path>] [--names-out <file.toml>] "
                 "[--check <file.toml>]\n");
    return 2;
}

// an executable on the host (XEX2 first), not a disc image
bool is_executable(const std::string &input) {
    std::ifstream f(input, std::ios::binary);
    char magic[4] = {};
    f.read(magic, 4);
    return f && std::string_view(magic, 4) == "XEX2";
}

bool load(const std::string &input, const std::string &xex_path, Xex &xex, std::string &error) {
    std::vector<uint8_t> file;
    if (is_executable(input)) {
        std::ifstream f(input, std::ios::binary);
        if (!f) return error = "cannot read " + input, false;
        file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } else {
        auto disc = Disc::open(input, error);
        if (!disc) return false;
        const DiscEntry *entry = disc->find(xex_path);
        if (!entry) return error = "no " + xex_path + " on the disc", false;
        file = disc->read_all(*entry);
        if (file.empty()) return error = "cannot read " + xex_path, false;
    }
    return parse_xex(file, xex, error) && load_xex_image(file, xex, error);
}

std::vector<uint8_t> read_all(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int tests(const std::filesystem::path &dir, const std::filesystem::path &out, int files) {
    namespace fs = std::filesystem;
    constexpr uint32_t START = 0x80000000;
    std::vector<fs::path> sources;
    for (const auto &e : fs::directory_iterator(dir))
        if (e.path().extension() == ".s" && fs::exists(fs::path(e.path()).replace_extension(".bin")))
            sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());
    fs::create_directories(out);
    std::vector<std::string> bodies(files), declarations(files);
    std::string table;
    uint64_t functions = 0;
    for (size_t k = 0; k < sources.size(); k++) {
        std::string suite = sources[k].stem().string(), prefix = "t" + std::to_string(k) + "_";
        recomp::Image image;
        image.base = START;
        image.bytes = read_all(fs::path(sources[k]).replace_extension(".bin"));
        image.sections.push_back({".text", START, uint32_t(image.bytes.size()), true, false});
        std::vector<uint32_t> labels;
        std::ifstream m(fs::path(sources[k]).replace_extension(".map"));
        std::string line;
        while (std::getline(m, line)) {
            size_t at = line.find(" t test_");
            if (at != std::string::npos) labels.push_back(START + uint32_t(std::stoul(line.substr(0, at), nullptr, 16)));
        }
        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
        for (size_t l = 0; l < labels.size(); l++) {
            uint32_t end = l + 1 < labels.size() ? labels[l + 1] : START + uint32_t(image.bytes.size());
            image.pdata.push_back({labels[l], end - labels[l], 0, false});
        }
        // every label is a function already; the tests hold no pointers to scan
        recomp::Discovery d = recomp::discover(image, {.scan_pointers = false});
        std::string &body = bodies[k % files], &decl = declarations[k % files];
        body += "\n// " + suite + "\n";
        for (const auto &[a, f] : d.functions) {
            decl += "XF_FN(" + recomp::function_name(prefix, a) + ");\n";
            body += recomp::emit_function(image, d, f, prefix);
            table += "    {\"" + suite + "\", 0x" + recomp::function_name("", a) + "u, " + recomp::function_name(prefix, a) + "},\n";
            functions++;
        }
    }
    for (int n = 0; n < files; n++) {
        std::ofstream f(out / ("ppc_tests_" + std::to_string(n) + ".cpp"));
        f << "// Written by xfrecomp tests. Xenia's PPC instruction tests, recompiled.\n"
             "#include \"xenoforge/ppc_generated.h\"\n\n"
          << declarations[n] << bodies[n];
    }
    std::string all_declarations;
    for (const std::string &d : declarations) all_declarations += d;
    std::ofstream f(out / "ppc_tests_table.cpp");
    f << "// Written by xfrecomp tests: every recompiled function of the tests, by suite and address.\n"
         "#include \"xenoforge/ppc_generated.h\"\n#include <cstddef>\n\n"
      << all_declarations
      << "\nstruct XfTestFunction { const char *suite; uint32_t address; void (*fn)(xf::ppc::Context &); };\n"
         "extern const XfTestFunction xf_test_functions[] = {\n"
      << table << "};\nextern const size_t xf_test_function_count = " << functions << ";\n";
    std::printf("%zu suites, %llu functions\n", sources.size(), (unsigned long long)functions);
    return 0;
}
} // namespace

// The PPC listing of a generated file (--listing; docs/recompiler.md, "PPC in
// the host debugger"): functions_K.ppc beside functions_K.cpp, a line per
// instruction (address, word, disassembly) under a line per function, and
// in the C++ a #line before each instruction's code pointing at its line
// there, so a debugger steps the generated code by PPC instructions. The
// emitter's own comment before each instruction ("// 822D28E4 lwz ...") is
// what is listed, so the emitter, and the game library's interface, stay as
// they are. Written only when the text changes.
bool add_listing(const std::filesystem::path &cpp, const std::function<uint32_t(uint32_t)> &word) {
    std::ifstream in(cpp, std::ios::binary);
    if (!in) return false;
    std::filesystem::path ppc = cpp;
    ppc.replace_extension(".ppc");
    std::string ppc_name = std::filesystem::absolute(ppc).generic_string();
    static const std::regex fn_line(R"(^XF_FN\((\w+)\) \{(.*)$)");
    static const std::regex instr_line(R"(^\s*// ([0-9A-F]{8}) (.*)$)");
    std::string code, listing, line;
    uint32_t listed = 0; // lines in the listing so far
    std::smatch m;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("#line ")) continue; // an earlier listing's
        if (std::regex_match(line, m, fn_line) && !line.ends_with("}")) {
            if (listed) listing += "\n", listed++;
            listing += m[1].str() + ":" + m[2].str() + "\n";
            code += "#line " + std::to_string(++listed) + " \"" + ppc_name + "\"\n" + line + "\n";
            continue;
        }
        if (std::regex_match(line, m, instr_line)) {
            uint32_t address = uint32_t(std::stoul(m[1].str(), nullptr, 16));
            char text[32];
            std::snprintf(text, sizeof(text), "    %08X  %08X  ", address, word(address));
            listing += text + m[2].str() + "\n";
            code += line + "\n#line " + std::to_string(++listed) + " \"" + ppc_name + "\"\n";
            continue;
        }
        code += line + "\n";
    }
    in.close();
    auto write = [](const std::filesystem::path &path, const std::string &text) {
        std::ifstream old(path, std::ios::binary);
        std::string now((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        if (old && now == text) return;
        old.close();
        std::ofstream(path, std::ios::binary) << text;
    };
    write(ppc, listing);
    write(cpp, code);
    return true;
}

// the disc's modules through recomp::emit_modules (recomp_module.cpp), a line each
int emit(const std::string &input, const std::vector<std::string> &only, const std::filesystem::path &out, int files,
         const recomp::DiscoverOptions &options, bool listing, const recomp::Project *project) {
    std::string error;
    std::vector<recomp::ModuleFile> modules;
    if (!recomp::find_modules(input, only, modules, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    recomp::EmitOptions o;
    o.files = files;
    o.discover = options;
    o.project = project;
    std::vector<recomp::EmittedModule> emitted;
    if (!recomp::emit_modules(modules, out, o, emitted, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    for (const recomp::EmittedModule &m : emitted)
        std::printf("%-28s %7zu functions, %9llu instructions, %4d files, %4d written (%s)\n", m.path.c_str(),
                    m.functions, (unsigned long long)m.instructions, m.files, m.written, m.directory.c_str());
    for (const recomp::EmittedModule &m : emitted) {
        if (m.hooks || m.mid_hooks || m.names)
            std::printf("%-28s %7zu hook slots, %zu mid-function, %zu names\n", m.path.c_str(), m.hooks, m.mid_hooks,
                        m.names);
        for (const std::string &w : m.warnings) std::printf("%-28s project: %s\n", m.path.c_str(), w.c_str());
    }
    if (listing) {
        // the words from each module's image, as the emitter read them
        for (size_t k = 0; k < emitted.size(); k++) {
            const recomp::EmittedModule &m = emitted[k];
            auto module = std::find_if(modules.begin(), modules.end(),
                                       [&](const recomp::ModuleFile &f) { return f.path == m.path; });
            Xex xex;
            recomp::Image image;
            std::string why;
            bool loaded = module != modules.end() && parse_xex(module->file, xex, why) &&
                          load_xex_image(module->file, xex, why) && recomp::read_image(xex, image, why);
            auto word = [&](uint32_t address) { return loaded ? image.word(address) : 0u; };
            size_t listed = 0;
            for (const std::string &source : m.sources)
                if (std::filesystem::path(source).filename().string().starts_with("functions_"))
                    listed += add_listing(out / source, word);
            std::printf("%-28s %7zu files listed (.ppc beside each)%s\n", m.path.c_str(), listed,
                        loaded ? "" : ", without the words");
        }
    }
    return 0;
}

// a project file, its warnings printed (nullptr, and why, when it cannot be read)
std::unique_ptr<recomp::Project> load_project(const char *path) {
    auto p = std::make_unique<recomp::Project>();
    std::string error;
    if (!recomp::load_project(path, *p, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return nullptr;
    }
    for (const std::string &w : p->warnings) std::fprintf(stderr, "project: %s\n", w.c_str());
    return p;
}

// signatures: a reference game's named functions (its project file's names, those starting with a --prefix:
// all without one) written as signatures. identify: a game's functions named from signature files, written
// as a project file's [names] (docs/recompiler.md, "The XDK by signature"); --check scores them against a
// project file's names.
int signatures_command(int argc, char **argv) {
    const bool make = std::string(argv[1]) == "signatures";
    const std::string input = argv[2];
    std::string xex_path = "default.xex", out, names_out;
    std::vector<std::string> prefixes, signature_files;
    std::unique_ptr<recomp::Project> project, checked;
    for (int k = 3; k < argc; k++) {
        const std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--project" && k + 1 < argc) {
            if (!(project = load_project(argv[++k]))) return 1;
        } else if (a == "--out" && k + 1 < argc) out = argv[++k];
        else if (a == "--prefix" && k + 1 < argc) prefixes.push_back(argv[++k]);
        else if (a == "--signatures" && k + 1 < argc) signature_files.push_back(argv[++k]);
        else if (a == "--names-out" && k + 1 < argc) names_out = argv[++k];
        else if (a == "--check" && k + 1 < argc) {
            if (!(checked = load_project(argv[++k]))) return 1;
        } else
            return usage();
    }
    if (make ? (!project || out.empty()) : signature_files.empty()) return usage();
    std::string error;
    Xex xex;
    recomp::Image image;
    if (!load(input, xex_path, xex, error) || !recomp::read_image(xex, image, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    recomp::DiscoverOptions options;
    if (const recomp::ProjectModule *pm = project ? project->find(xex_path) : nullptr)
        for (const auto &[a, end] : pm->starts) options.functions.emplace(a, end);
    const recomp::Discovery d = recomp::discover(image, options);
    if (make) {
        const recomp::ProjectModule *pm = project->find(xex_path);
        std::vector<recomp::Signature> signatures;
        for (const auto &[a, name] : pm ? pm->names : std::map<uint32_t, std::string>{}) {
            bool wanted = prefixes.empty();
            for (const std::string &p : prefixes) wanted |= name.rfind(p, 0) == 0;
            if (!wanted) continue;
            auto it = d.functions.find(a);
            if (it == d.functions.end()) {
                std::fprintf(stderr, "%s at %08X: no function starts there\n", name.c_str(), a);
                continue;
            }
            recomp::Signature s = recomp::signature_of(image, a, it->second.size);
            s.name = name;
            signatures.push_back(std::move(s));
        }
        std::ofstream f(out, std::ios::binary);
        f << recomp::write_signatures(signatures);
        std::printf("%zu signatures written to %s\n", signatures.size(), out.c_str());
        return f ? 0 : 1;
    }
    std::vector<recomp::Signature> reference;
    for (const std::string &file : signature_files) {
        std::ifstream f(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f && text.empty()) return std::fprintf(stderr, "cannot read %s\n", file.c_str()), 1;
        if (!recomp::read_signatures(text, reference, error))
            return std::fprintf(stderr, "%s: %s\n", file.c_str(), error.c_str()), 1;
    }
    const std::vector<recomp::SignatureMatch> found = recomp::identify(image, d, reference);
    std::map<std::string, uint32_t> truth;
    if (const recomp::ProjectModule *pm = checked ? checked->find(xex_path) : nullptr)
        for (const auto &[a, name] : pm->names) truth[name] = a;
    uint64_t sure = 0, unsure = 0, missing = 0, right = 0, wrong = 0;
    std::string toml = "# found by xfrecomp identify (docs/recompiler.md, \"The XDK by signature\")\n[names]\n";
    for (const recomp::SignatureMatch &m : found) {
        const char *verdict = "";
        if (auto t = truth.find(m.name); t != truth.end()) {
            const bool ok = t->second == m.address;
            (ok ? right : wrong)++;
            verdict = ok ? "  right" : "  WRONG";
        }
        if (!m.address) {
            missing++;
            std::printf("  %-40s not found%s\n", m.name.c_str(), verdict);
            continue;
        }
        (m.sure ? sure : unsure)++;
        std::printf("  %-40s %08X  %s %.3f (next %.3f)%s\n", m.name.c_str(), m.address,
                    m.exact ? "exact" : m.sure ? "sure " : "maybe", m.score, m.runner_up, verdict);
        char line[160];
        std::snprintf(line, sizeof(line), "%s\"0x%08X\" = \"%s\"%s\n", m.sure ? "" : "# ", m.address, m.name.c_str(),
                      m.sure ? "" : "  # not sure");
        toml += line;
    }
    std::printf("%zu names: %llu sure, %llu not sure, %llu not found", found.size(), (unsigned long long)sure,
                (unsigned long long)unsure, (unsigned long long)missing);
    if (right + wrong) std::printf("; against the project file: %llu right, %llu wrong", (unsigned long long)right,
                                   (unsigned long long)wrong);
    std::printf("\n");
    if (!names_out.empty()) {
        std::ofstream f(names_out, std::ios::binary);
        f << toml;
        if (!f) return std::fprintf(stderr, "cannot write %s\n", names_out.c_str()), 1;
    }
    return 0;
}

// --calls: the functions by how they keep the calling convention, and what
// trusting it could save at their calls (docs/recompiler.md, "Trusting the
// calling convention, weighed"). With a file of counted calls (lines "site target
// count", hex addresses), each call site weighted by how often it ran.
void report_calls(const recomp::Image &image, const recomp::Discovery &d, const std::string &counts_file) {
    using recomp::Convention;
    const auto conv = recomp::analyze_calling_convention(image, d);
    std::map<Convention, std::pair<uint64_t, uint64_t>> by_kind; // functions, instructions
    for (const auto &[a, cc] : conv) {
        by_kind[cc.kind].first++;
        by_kind[cc.kind].second += cc.instructions;
    }
    std::printf("functions by the calling convention:\n");
    for (const auto &[k, n] : by_kind) {
        std::printf("  %-60s %7llu functions %9llu instructions", recomp::convention_name(k),
                    (unsigned long long)n.first, (unsigned long long)n.second);
        // a few of those that do not keep it, for a look
        int shown = 0;
        if (k != Convention::Keeps && k != Convention::NoNonVolatile && k != Convention::Skipped)
            for (const auto &[a, cc] : conv)
                if (cc.kind == k && shown++ < 6) std::printf(" %08X", a);
        std::printf("\n");
    }

    // counted calls: by site, and by target
    std::map<uint32_t, uint64_t> site_count, target_count;
    uint64_t counted = 0;
    if (!counts_file.empty()) {
        std::ifstream f(counts_file);
        uint32_t site, target;
        unsigned long long n;
        std::string a, b;
        while (f >> a >> b >> n) {
            site = uint32_t(std::stoul(a, nullptr, 16)), target = uint32_t(std::stoul(b, nullptr, 16));
            site_count[site] += n, target_count[target] += n, counted += n;
        }
        std::printf("counted calls: %llu at %zu sites\n", (unsigned long long)counted, site_count.size());
    }
    auto keeps = [&](uint32_t a) {
        auto it = conv.find(a);
        return it != conv.end() &&
               (it->second.kind == Convention::Keeps || it->second.kind == Convention::NoNonVolatile);
    };
    auto helper = [&](uint32_t a) {
        auto it = d.functions.find(a);
        return it != d.functions.end() && it->second.source == recomp::Source::Helper;
    };
    // Per call site in a function that keeps it: the non-volatile registers
    // named since the call before it (in address order; what the code may
    // store to the context before it today) and until the next (what it may
    // load again after it): an upper bound on the context's loads and stores
    // locals would save there. By what the call reaches.
    enum Reach { ToKeeping, ToOther, Dispatch, Reaches };
    const char *reach_names[] = {"to a function that keeps it", "to another function (synced)",
                                 "through dispatch (synced)"};
    uint64_t sites[Reaches] = {}, ops[Reaches] = {}, dyn_sites[Reaches] = {}, dyn_ops[Reaches] = {};
    // per entry: the restores that would go (helpers' loads and stores, the
    // inline restores' stores), when nothing in it syncs
    uint64_t entries = 0, synced_entries = 0, restore_ops = 0, helper_restores = 0, functions_unsynced = 0;
    for (const auto &[address, cc] : conv) {
        if (cc.kind != Convention::Keeps) continue;
        const recomp::Function &f = d.functions.at(address);
        const std::set<uint32_t> code = recomp::function_instructions(image, d, f);
        const std::vector<uint32_t> pcs(code.begin(), code.end());
        const std::vector<recomp::NonVolatileSet> named = recomp::named_non_volatile(image, pcs);
        struct Site {
            size_t index;
            Reach reach;
        };
        std::vector<Site> calls;
        uint64_t restored_by_helper = 0;
        for (size_t k = 0; k < pcs.size(); k++) {
            ppc::Instr i{image.word(pcs[k])};
            ppc::Opcode op = ppc::decode(i.code);
            bool lk = false, direct = false;
            uint32_t t = 0;
            if (op == ppc::Opcode::bx) {
                lk = i.I.LK, direct = true;
                t = uint32_t(int32_t(i.I.LI << 8) >> 6) + (i.I.AA ? 0 : pcs[k]);
            } else if (op == ppc::Opcode::bcx) {
                lk = i.B.LK, direct = true;
                t = uint32_t(int32_t(int16_t(i.B.BD << 2))) + (i.B.AA ? 0 : pcs[k]);
            } else if (op == ppc::Opcode::bclrx || op == ppc::Opcode::bcctrx) {
                lk = i.XL.LK;
            }
            if (direct && helper(t)) {
                const std::string &name = d.functions.at(t).name;
                if (name.rfind("__rest", 0) == 0) {
                    bool g = name.rfind("__restgprlr_", 0) == 0;
                    uint32_t first = uint32_t(std::stoul(name.substr(name.rfind('_') + 1)));
                    restored_by_helper += (first >= 64 ? 128 : 32) - first;
                    (void)g;
                }
                continue;
            }
            if (!lk) continue;
            Reach r = !direct ? Dispatch : keeps(t) ? ToKeeping : d.functions.count(t) ? ToOther : Dispatch;
            auto it = d.functions.find(t);
            if (direct && it != d.functions.end() && it->second.source == recomp::Source::Import) r = Dispatch;
            calls.push_back({k, r});
        }
        bool syncs = false;
        for (size_t c = 0; c < calls.size(); c++) {
            const size_t from = c ? calls[c - 1].index + 1 : 0;
            const size_t to = c + 1 < calls.size() ? calls[c + 1].index : pcs.size();
            recomp::NonVolatileSet before, after;
            for (size_t k = from; k < calls[c].index; k++) before.add(named[k]);
            for (size_t k = calls[c].index + 1; k < to; k++) after.add(named[k]);
            const uint64_t n = before.count() + after.count();
            const Reach r = calls[c].reach;
            syncs |= r != ToKeeping;
            sites[r]++, ops[r] += n;
            auto sc = site_count.find(pcs[calls[c].index]);
            if (sc != site_count.end()) dyn_sites[r] += sc->second, dyn_ops[r] += sc->second * n;
        }
        // the inline restores' stores to the context, the helpers' loads and stores
        const uint64_t restored = cc.restored.count();
        const uint64_t per_entry =
            (restored > restored_by_helper ? restored - restored_by_helper : 0) + 2 * restored_by_helper;
        const uint64_t e = target_count.count(address) ? target_count.at(address) : 0;
        entries += e;
        if (syncs)
            synced_entries += e;
        else {
            functions_unsynced++;
            restore_ops += e * per_entry;
            if (restored_by_helper) helper_restores += e;
        }
    }
    std::printf("call sites in the functions that keep it (registers named around each: an upper bound on the "
                "context's loads and stores saved):\n");
    for (int r = 0; r < Reaches; r++)
        std::printf("  %-34s %8llu sites, %9llu loads and stores; counted %12llu calls, %14llu loads and stores\n",
                    reach_names[r], (unsigned long long)sites[r], (unsigned long long)ops[r],
                    (unsigned long long)dyn_sites[r], (unsigned long long)dyn_ops[r]);
    std::printf("entries of functions that keep it: %llu counted, %llu of them in functions that sync (%llu functions "
                "never sync); restores that would go in the others: %llu loads and stores, %llu helper calls\n",
                (unsigned long long)entries, (unsigned long long)synced_entries, (unsigned long long)functions_unsynced,
                (unsigned long long)restore_ops, (unsigned long long)helper_restores);
}

int main(int argc, char **argv) {
    // how many compilers the generated code's build runs at once (xenoforge_add_port's): the
    // recompile step's policy (game_build.h's job_policy: the cores and the memory free now), or N;
    // the number on stdout, why on stderr
    if (argc >= 2 && std::string(argv[1]) == "jobs") {
        const build::JobPolicy p = build::job_policy(argc >= 3 ? unsigned(std::max(0, std::atoi(argv[2]))) : 0u);
        std::printf("%u\n", p.limit);
        std::fprintf(stderr, "%s\n", p.why.c_str());
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "emit") {
        std::vector<std::string> only;
        int files = 0; // by each module's size
        bool listing = false;
        recomp::DiscoverOptions options;
        std::unique_ptr<recomp::Project> project;
        for (int k = 4; k < argc; k++) {
            std::string a = argv[k];
            if (a == "--project" && k + 1 < argc) {
                if (!(project = load_project(argv[++k]))) return 1;
            } else if (a == "--files" && k + 1 < argc) files = std::max(1, std::atoi(argv[++k]));
            else if (a == "--xex" && k + 1 < argc) only.push_back(argv[++k]);
            else if (a == "--no-pointers") options.scan_pointers = false;
            else if (a == "--listing") listing = true;
            else return usage();
        }
        return emit(argv[2], only, argv[3], files, options, listing, project.get());
    }
    if (argc >= 4 && std::string(argv[1]) == "tests") {
        int files = 16;
        for (int k = 4; k + 1 < argc; k++)
            if (std::string(argv[k]) == "--files") files = std::max(1, std::atoi(argv[k + 1]));
        return tests(argv[2], argv[3], files);
    }
    if (argc >= 3 && (std::string(argv[1]) == "signatures" || std::string(argv[1]) == "identify"))
        return signatures_command(argc, argv);
    if (argc < 3 || std::string(argv[1]) != "analyze") return usage();
    std::string input = argv[2], xex_path = "default.xex", compare, functions_out, tables_out, unresolved_out;
    std::string call_counts;
    bool calls = false;
    recomp::DiscoverOptions options;
    std::unique_ptr<recomp::Project> project, checked;
    for (int k = 3; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--compare" && k + 1 < argc) compare = argv[++k];
        else if (a == "--functions" && k + 1 < argc) functions_out = argv[++k];
        else if (a == "--tables" && k + 1 < argc) tables_out = argv[++k];
        else if (a == "--unresolved" && k + 1 < argc) unresolved_out = argv[++k];
        else if (a == "--no-pointers") options.scan_pointers = false;
        else if (a == "--calls") {
            calls = true;
            if (k + 1 < argc && argv[k + 1][0] != '-') call_counts = argv[++k];
        } else if (a == "--project" && k + 1 < argc) {
            if (!(project = load_project(argv[++k]))) return 1;
        } else if (a == "--check-functions" && k + 1 < argc) {
            if (!(checked = load_project(argv[++k]))) return 1;
        } else
            return usage();
    }
    std::string error;
    Xex xex;
    recomp::Image image;
    if (!load(input, xex_path, xex, error) || !recomp::read_image(xex, image, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("image         0x%08X, %zu bytes, entry 0x%08X\n", image.base, image.bytes.size(), image.entry);
    for (const recomp::Section &s : image.sections)
        if (s.code) std::printf("code          %-10s 0x%08X-0x%08X\n", s.name.c_str(), s.address, s.address + s.size);
    std::printf(".pdata        %zu entries; %zu imported functions\n", image.pdata.size(), image.imports.size());

    if (const recomp::ProjectModule *pm = project ? project->find(xex_path) : nullptr) {
        for (const auto &[a, end] : pm->starts) options.functions.emplace(a, end);
        for (uint32_t a : pm->hooks) options.functions.emplace(a, 0);
        for (const auto &[bctr, targets] : pm->jump_tables) options.jump_tables[bctr] = targets;
    }
    recomp::Discovery d = recomp::discover(image, options);
    for (const auto &[what, n] : d.counts) std::printf("  %-36s %llu\n", what.c_str(), (unsigned long long)n);
    std::map<int, uint64_t> kinds;
    for (const auto &[at, t] : d.tables) kinds[t.kind]++;
    const char *kind_names[] = {"absolute", "byte offsets", "shifted byte offsets", "halfword offsets"};
    for (const auto &[k, n] : kinds) std::printf("  jump tables, %-22s %llu\n", kind_names[k], (unsigned long long)n);

    if (!functions_out.empty()) {
        std::ofstream f(functions_out);
        for (const auto &[a, fn] : d.functions) {
            f << std::hex << a << '\t' << std::dec << fn.size << '\t' << recomp::source_name(fn.source) << '\t' << fn.name
              << (fn.switches_stack ? "\tswitches the stack" : "") << (fn.saves_stack ? "\tsaves the stack" : "");
            if (!fn.resumes.empty()) {
                f << "\tresumes at";
                for (uint32_t r : fn.resumes) f << ' ' << std::hex << r;
            }
            f << '\n';
        }
    }

    if (!unresolved_out.empty()) {
        // function, branch, target
        std::ofstream f(unresolved_out);
        for (const recomp::Unresolved &u : d.unresolved)
            f << std::hex << u.function << '\t' << u.site << '\t' << u.target << '\n';
    }

    if (!tables_out.empty()) {
        // bctr, kind, table, index register, targets
        std::ofstream f(tables_out);
        for (const auto &[at, t] : d.tables) {
            f << std::hex << at << '\t' << int(t.kind) << '\t' << t.table << '\t' << std::dec << t.index;
            for (uint32_t target : t.targets) f << '\t' << std::hex << target;
            f << '\n';
        }
    }

    if (const recomp::ProjectModule *pm = checked ? checked->find(xex_path) : nullptr) {
        // the list's starts against this discovery (made without them)
        uint64_t found = 0, inside = 0, labels = 0, outside = 0, not_code = 0;
        std::vector<uint32_t> examples;
        for (const auto &[a, end] : pm->starts) {
            if (d.functions.count(a)) {
                found++;
                continue;
            }
            if (!image.is_code(a)) {
                not_code++;
                continue;
            }
            auto it = d.functions.upper_bound(a);
            bool in = it != d.functions.begin() && a < std::prev(it)->first + std::prev(it)->second.size;
            (in ? inside : outside)++;
            if (in && std::prev(it)->second.blocks.count(a)) labels++;
            if (examples.size() < 12) examples.push_back(a);
        }
        std::printf("the project file's %zu starts: %llu found here, %llu inside a function here (%llu of them "
                    "branch targets there), %llu in no function, %llu not code\n",
                    pm->starts.size(), (unsigned long long)found, (unsigned long long)inside,
                    (unsigned long long)labels, (unsigned long long)outside, (unsigned long long)not_code);
        if (!examples.empty()) {
            std::printf("  not found:");
            for (uint32_t a : examples) std::printf(" %08X", a);
            std::printf("\n");
        }
    }

    if (calls) report_calls(image, d, call_counts);

    if (!compare.empty()) {
        std::ifstream f(compare);
        if (!f) return std::fprintf(stderr, "cannot read %s\n", compare.c_str()), 1;
        std::set<uint32_t> theirs;
        std::string line;
        std::regex mapping(R"(\{\s*0x([0-9A-Fa-f]{8}),\s*\w+\s*\})");
        while (std::getline(f, line)) {
            std::smatch m;
            if (std::regex_search(line, m, mapping)) theirs.insert(uint32_t(std::stoul(m[1].str(), nullptr, 16)));
        }
        uint64_t both = 0;
        std::map<std::string, uint64_t> only_ours;
        std::vector<uint32_t> only_theirs;
        for (const auto &[a, fn] : d.functions)
            if (theirs.count(a)) both++;
            else only_ours[recomp::source_name(fn.source)]++;
        for (uint32_t a : theirs)
            if (!d.functions.count(a)) only_theirs.push_back(a);
        std::printf("compared with %s: %zu functions there\n", compare.c_str(), theirs.size());
        std::printf("  in both                              %llu\n", (unsigned long long)both);
        for (const auto &[src, n] : only_ours) std::printf("  only here, from %-20s %llu\n", src.c_str(), (unsigned long long)n);
        std::printf("  only there                           %zu", only_theirs.size());
        for (size_t k = 0; k < only_theirs.size() && k < 12; k++) std::printf(" %08X", only_theirs[k]);
        std::printf("\n");
        // where the ones only there sit in ours
        uint64_t inside = 0;
        for (uint32_t a : only_theirs) {
            auto it = d.functions.upper_bound(a);
            if (it != d.functions.begin() && a < std::prev(it)->first + std::prev(it)->second.size) inside++;
        }
        std::printf("  of those, inside a function here     %llu\n", (unsigned long long)inside);
    }
    return 0;
}
