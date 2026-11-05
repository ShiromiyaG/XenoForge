// xfrecomp: the recompiler's command (plan step A2).
//
//   xfrecomp analyze <game.iso | default.xex> [--xex <path on the disc>]
//            [--compare <rexglue's *_init.cpp>] [--functions <out.tsv>]
//
// prints what discovery found by kind; --compare sets the function starts
// against rexglue's PPCFuncMappings for the same executable; --functions
// writes every function (address, size, source, name).
//
//   xfrecomp tests <assembled tests dir> <out dir> [--files N]
//
// recompiles Xenia's instruction tests (tools/assemble_ppc_tests.sh) into
// out/ppc_tests_0..N-1.cpp and ppc_tests_table.cpp for ppc_recompiled_test:
// every test label a function as long as the code up to the next label.
//
//   xfrecomp emit <game.iso | default.xex> <out dir> [--xex <path>] [--files N]
//
// writes the game's functions as C++ (functions_0..N-1.cpp, by address),
// functions.h declaring them all, and table.cpp listing them by address
// for dispatch. Game code: the out dir stays outside the repository.

#include "xenoforge/disc.h"
#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace xf;

namespace {

int usage() {
    std::fprintf(stderr, "usage: xfrecomp analyze <game.iso | default.xex> [--xex <path>] [--compare <init.cpp>] "
                         "[--functions <out.tsv>]\n"
                         "       xfrecomp tests <assembled tests dir> <out dir> [--files N]\n"
                         "       xfrecomp emit <game.iso | default.xex> <out dir> [--xex <path>] [--files N]\n");
    return 2;
}

bool load(const std::string &input, const std::string &xex_path, Xex &xex, std::string &error) {
    std::vector<uint8_t> file;
    bool is_xex = input.size() > 4 && (input.ends_with(".xex") || input.ends_with(".XEX"));
    if (is_xex) {
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
        recomp::Discovery d = recomp::discover(image);
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

int emit(const std::string &input, const std::string &xex_path, const std::filesystem::path &out, int files) {
    std::string error;
    Xex xex;
    recomp::Image image;
    if (!load(input, xex_path, xex, error) || !recomp::read_image(xex, image, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    recomp::Discovery d = recomp::discover(image);
    std::filesystem::create_directories(out);
    const std::string prefix = "xf_";
    // the functions with code, split by instruction count
    std::vector<const recomp::Function *> list;
    uint64_t total = 0;
    for (const auto &[a, f] : d.functions)
        if (f.source != recomp::Source::Import) list.push_back(&f), total += f.size / 4;
    {
        std::ofstream h(out / "functions.h");
        h << "// Written by xfrecomp emit. Game code: not for the repository.\n#pragma once\n"
             "#include \"xenoforge/ppc_generated.h\"\n\n";
        for (const recomp::Function *f : list) h << "XF_FN(" << recomp::function_name(prefix, f->address) << ");\n";
    }
    uint64_t per_file = total / files + 1, in_file = 0;
    int n = 0;
    std::ofstream f(out / ("functions_0.cpp"));
    f << "#include \"functions.h\"\n\n";
    for (const recomp::Function *fn : list) {
        if (in_file >= per_file && n + 1 < files) {
            f.close();
            f.open(out / ("functions_" + std::to_string(++n) + ".cpp"));
            f << "#include \"functions.h\"\n\n";
            in_file = 0;
        }
        f << recomp::emit_function(image, d, *fn, prefix) << "\n";
        in_file += fn->size / 4;
    }
    f.close();
    for (int k = n + 1; k < files; k++) std::ofstream(out / ("functions_" + std::to_string(k) + ".cpp")) << "\n";
    std::ofstream t(out / "table.cpp");
    t << "// Written by xfrecomp emit: the functions by address, for dispatch.\n#include \"functions.h\"\n"
         "#include <cstddef>\n\nstruct XfFunction { uint32_t address; void (*fn)(xf::ppc::Context &); };\n"
         "extern const XfFunction xf_functions[] = {\n";
    for (const recomp::Function *fn : list)
        t << "    {0x" << recomp::function_name("", fn->address) << "u, " << recomp::function_name(prefix, fn->address) << "},\n";
    t << "};\nextern const size_t xf_function_count = " << list.size() << ";\n";
    std::printf("%zu functions, %llu instructions, %d files\n", list.size(), (unsigned long long)total, files);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && std::string(argv[1]) == "emit") {
        std::string xex_path = "default.xex";
        int files = 64;
        for (int k = 4; k + 1 < argc; k++) {
            if (std::string(argv[k]) == "--files") files = std::max(1, std::atoi(argv[k + 1]));
            if (std::string(argv[k]) == "--xex") xex_path = argv[k + 1];
        }
        return emit(argv[2], xex_path, argv[3], files);
    }
    if (argc >= 4 && std::string(argv[1]) == "tests") {
        int files = 16;
        for (int k = 4; k + 1 < argc; k++)
            if (std::string(argv[k]) == "--files") files = std::max(1, std::atoi(argv[k + 1]));
        return tests(argv[2], argv[3], files);
    }
    if (argc < 3 || std::string(argv[1]) != "analyze") return usage();
    std::string input = argv[2], xex_path = "default.xex", compare, functions_out, tables_out;
    for (int k = 3; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--compare" && k + 1 < argc) compare = argv[++k];
        else if (a == "--functions" && k + 1 < argc) functions_out = argv[++k];
        else if (a == "--tables" && k + 1 < argc) tables_out = argv[++k];
        else return usage();
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

    recomp::Discovery d = recomp::discover(image);
    for (const auto &[what, n] : d.counts) std::printf("  %-36s %llu\n", what.c_str(), (unsigned long long)n);
    std::map<int, uint64_t> kinds;
    for (const auto &[at, t] : d.tables) kinds[t.kind]++;
    const char *kind_names[] = {"absolute", "byte offsets", "shifted byte offsets", "halfword offsets"};
    for (const auto &[k, n] : kinds) std::printf("  jump tables, %-22s %llu\n", kind_names[k], (unsigned long long)n);

    if (!functions_out.empty()) {
        std::ofstream f(functions_out);
        for (const auto &[a, fn] : d.functions)
            f << std::hex << a << '\t' << std::dec << fn.size << '\t' << recomp::source_name(fn.source) << '\t' << fn.name
              << '\n';
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
