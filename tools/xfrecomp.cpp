// xfrecomp: the recompiler's command (plan step A2).
//
//   xfrecomp analyze <game.iso | default.xex> [--xex <path on the disc>]
//            [--compare <rexglue's *_init.cpp>] [--functions <out.tsv>]
//            [--tables <out.tsv>] [--unresolved <out.tsv>] [--no-pointers]
//
// prints what discovery found by kind; --compare sets the function starts
// against rexglue's PPCFuncMappings for the same executable; --functions
// writes every function (address, size, source, name), --tables the jump
// tables, --unresolved the branches out of a function to no start (function,
// branch, target); --no-pointers leaves the data pointer scan out.
//
//   xfrecomp tests <assembled tests dir> <out dir> [--files N]
//
// recompiles Xenia's instruction tests (tools/assemble_ppc_tests.sh) into
// out/ppc_tests_0..N-1.cpp and ppc_tests_table.cpp for ppc_recompiled_test:
// every test label a function as long as the code up to the next label.
//
//   xfrecomp emit <game.iso | file.xex> <out dir> [--xex <path>]... [--files N] [--no-pointers]
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
// repository.

#include "xenoforge/disc.h"
#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace xf;

namespace {

int usage() {
    std::fprintf(stderr, "usage: xfrecomp analyze <game.iso | default.xex> [--xex <path>] [--compare <init.cpp>] "
                         "[--functions <out.tsv>] [--tables <out.tsv>] [--unresolved <out.tsv>] [--no-pointers]\n"
                         "       xfrecomp tests <assembled tests dir> <out dir> [--files N]\n"
                         "       xfrecomp emit <game.iso | file.xex> <out dir> [--xex <path>]... [--files N] "
                         "[--no-pointers]\n");
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

// An executable to emit: its path on the disc (or the host file's name) and
// the file.
struct Executable {
    std::string path;
    std::vector<uint8_t> file;
};

bool is_xex_file(const std::vector<uint8_t> &file) {
    return file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0;
}

// every XEX2 executable under dir, but the system update's
void find_executables(const Disc &disc, const DiscEntry &dir, const std::string &prefix, std::vector<Executable> &out) {
    for (const DiscEntry &e : dir.children) {
        std::string path = prefix.empty() ? e.name : prefix + "\\" + e.name;
        if (e.directory) {
            std::string lower = e.name;
            for (char &ch : lower) ch = char(std::tolower(uint8_t(ch)));
            if (prefix.empty() && lower == "$systemupdate") continue;
            find_executables(disc, e, path, out);
            continue;
        }
        char magic[4] = {};
        if (e.size < 0x18 || !disc.read(e, 0, magic, 4) || std::memcmp(magic, "XEX2", 4) != 0) continue;
        out.push_back({path, disc.read_all(e)});
    }
}

// what a path becomes as a directory and a C++ name: letters, digits and _
std::string identifier(const std::string &path) {
    std::string s;
    for (char ch : path) s += std::isalnum(uint8_t(ch)) ? ch : '_';
    return s;
}

std::string hex(uint64_t v, int digits) {
    char b[24];
    std::snprintf(b, sizeof(b), "%0*llX", digits, (unsigned long long)v);
    return b;
}

// Writes a file unless it holds that already, so a build after an emission
// compiles only what changed; whether it wrote.
bool write_if_changed(const std::filesystem::path &path, const std::string &text) {
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::string old((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (old == text) return false;
        }
    }
    std::ofstream(path, std::ios::binary) << text;
    return true;
}

// One module's directory (see the top): false, the reason printed, when the
// executable cannot be read.
struct Emitted {
    std::string ns;
    size_t functions = 0;
    uint64_t instructions = 0;
    int files = 0, written = 0;
};
bool emit_module(const Executable &e, const std::filesystem::path &dir, const std::string &ns, int files,
                 const recomp::DiscoverOptions &options, Emitted &out) {
    std::string error;
    Xex xex;
    recomp::Image image;
    if (!parse_xex(e.file, xex, error) || !load_xex_image(e.file, xex, error) ||
        !recomp::read_image(xex, image, error)) {
        std::fprintf(stderr, "%s: %s\n", e.path.c_str(), error.c_str());
        return false;
    }
    recomp::Discovery d = recomp::discover(image, options);
    std::filesystem::create_directories(dir);
    const std::string prefix = "xf_";
    // the functions with code: not the imports, nor those that switch the
    // stack (the interpreter runs them; recomp.h)
    std::vector<const recomp::Function *> list;
    uint64_t total = 0;
    for (const auto &[a, f] : d.functions)
        if (f.source != recomp::Source::Import && !f.switches_stack) list.push_back(&f), total += f.size / 4;
    // Files by address: 64 KB of the image each (about 16,000 instructions,
    // a file the build compiles in seconds), or what --files asks; empty
    // ones not written. A function found or lost changes its own file and its
    // callers' (each file declares what it calls), not every file after it.
    uint32_t bucket = 0x10000;
    if (files > 0 && !list.empty()) {
        uint32_t span = list.back()->address + list.back()->size - list.front()->address;
        bucket = std::max<uint32_t>(0x1000, (span / uint32_t(files) + 0xFFF) & ~0xFFFu);
    }
    std::map<uint32_t, std::vector<const recomp::Function *>> by_file;
    for (const recomp::Function *fn : list) by_file[(fn->address - image.base) / bucket].push_back(fn);
    const std::string open = "namespace " + ns + " {\n\n", close = "\n} // namespace " + ns + "\n";
    const std::string head = "// Written by xfrecomp emit from " + e.path +
                             ". Game code: not for the repository.\n#include \"xenoforge/ppc_generated.h\"\n\n";
    std::set<std::string> current;
    for (const auto &[k, functions] : by_file) {
        std::string bodies;
        std::set<uint32_t> calls;
        for (const recomp::Function *fn : functions) {
            bodies += recomp::emit_function(image, d, *fn, prefix, &calls) + "\n";
            calls.insert(fn->address);
        }
        std::string text = head + open;
        for (uint32_t a : calls) text += "XF_FN(" + recomp::function_name(prefix, a) + ");\n";
        text += "\n" + bodies + close;
        std::string name = "functions_" + std::to_string(k) + ".cpp";
        current.insert(name);
        out.written += write_if_changed(dir / name, text);
    }
    // files of an earlier emission that hold nothing now: left empty
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.starts_with("functions_") && name.ends_with(".cpp") && !current.count(name))
            out.written += write_if_changed(entry.path(), "\n");
    }
    // an earlier layout's header of every function
    std::error_code ec;
    std::filesystem::remove(dir / "functions.h", ec);
    // the path as a C++ string
    std::string literal;
    for (char ch : e.path) literal += ch == '\\' ? std::string("\\\\") : std::string(1, ch);
    std::string t = "// Written by xfrecomp emit: " + e.path + "'s functions by address, for dispatch.\n" +
                    "#include \"xenoforge/ppc_generated.h\"\n\n" + open;
    for (const recomp::Function *fn : list) t += "XF_FN(" + recomp::function_name(prefix, fn->address) + ");\n";
    t += "\nconst ::xf::ppc::GeneratedFunction functions[] = {\n";
    for (const recomp::Function *fn : list)
        t += "    {0x" + recomp::function_name("", fn->address) + "u, " + recomp::function_name(prefix, fn->address) +
             "},\n";
    if (list.empty()) t += "    {0, nullptr}, // (none: the count says 0)\n";
    t += "};\n\n// the image it was made from: path, base, size, digest (xf::image_digest)\n"
         "extern const ::xf::ppc::GeneratedModule module = {\"" +
         literal + "\", 0x" + hex(xex.base_address, 8) + "u, 0x" + hex(xex.image.size(), 8) + "u, 0x" +
         hex(image_digest(xex.image), 16) + "ull, functions, " + std::to_string(list.size()) + "};\n" + close;
    out.written += write_if_changed(dir / "table.cpp", t);
    out.ns = ns, out.functions = list.size(), out.instructions = total, out.files = int(by_file.size());
    return true;
}

} // namespace

int emit(const std::string &input, const std::vector<std::string> &only, const std::filesystem::path &out, int files,
         const recomp::DiscoverOptions &options) {
    std::string error;
    std::vector<Executable> executables;
    if (is_executable(input)) {
        executables.push_back({std::filesystem::path(input).filename().string(), read_all(input)});
    } else {
        auto disc = Disc::open(input, error);
        if (!disc) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
        if (only.empty()) find_executables(*disc, disc->root(), "", executables);
        for (const std::string &path : only) {
            const DiscEntry *entry = disc->find(path);
            if (!entry || entry->directory) return std::fprintf(stderr, "no %s on the disc\n", path.c_str()), 1;
            executables.push_back({path, disc->read_all(*entry)});
        }
    }
    // title updates are patches to apply, not modules
    std::erase_if(executables, [](const Executable &e) {
        Xex x;
        std::string ignored;
        return !is_xex_file(e.file) || (parse_xex(e.file, x, ignored) && x.is_patch());
    });
    if (executables.empty()) return std::fprintf(stderr, "no executable to emit\n"), 1;
    std::filesystem::create_directories(out);
    std::vector<Emitted> emitted;
    std::set<std::string> names;
    for (const Executable &e : executables) {
        std::string name = identifier(e.path);
        for (int k = 2; !names.insert(name).second; k++) name = identifier(e.path) + "_" + std::to_string(k);
        Emitted m;
        if (!emit_module(e, out / name, "xfm_" + name, files, options, m)) return 1;
        std::printf("%-28s %7zu functions, %9llu instructions, %4d files, %4d written (%s)\n", e.path.c_str(),
                    m.functions, (unsigned long long)m.instructions, m.files, m.written, name.c_str());
        emitted.push_back(m);
    }
    std::string t = "// Written by xfrecomp emit: every module's table, for the game library (ppc_module.h).\n"
                    "#include \"xenoforge/ppc_module.h\"\n\n";
    for (const Emitted &m : emitted)
        t += "namespace " + m.ns + " {\nextern const ::xf::ppc::GeneratedModule module;\n}\n";
    t += "\nextern \"C\" const ::xf::ppc::GeneratedModule *const xf_modules[] = {\n";
    for (const Emitted &m : emitted) t += "    &" + m.ns + "::module,\n";
    t += "};\nextern \"C\" const size_t xf_module_count = " + std::to_string(emitted.size()) + ";\n";
    write_if_changed(out / "modules.cpp", t);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && std::string(argv[1]) == "emit") {
        std::vector<std::string> only;
        int files = 0; // by each module's size
        recomp::DiscoverOptions options;
        for (int k = 4; k < argc; k++) {
            std::string a = argv[k];
            if (a == "--files" && k + 1 < argc) files = std::max(1, std::atoi(argv[++k]));
            else if (a == "--xex" && k + 1 < argc) only.push_back(argv[++k]);
            else if (a == "--no-pointers") options.scan_pointers = false;
            else return usage();
        }
        return emit(argv[2], only, argv[3], files, options);
    }
    if (argc >= 4 && std::string(argv[1]) == "tests") {
        int files = 16;
        for (int k = 4; k + 1 < argc; k++)
            if (std::string(argv[k]) == "--files") files = std::max(1, std::atoi(argv[k + 1]));
        return tests(argv[2], argv[3], files);
    }
    if (argc < 3 || std::string(argv[1]) != "analyze") return usage();
    std::string input = argv[2], xex_path = "default.xex", compare, functions_out, tables_out, unresolved_out;
    recomp::DiscoverOptions options;
    for (int k = 3; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--compare" && k + 1 < argc) compare = argv[++k];
        else if (a == "--functions" && k + 1 < argc) functions_out = argv[++k];
        else if (a == "--tables" && k + 1 < argc) tables_out = argv[++k];
        else if (a == "--unresolved" && k + 1 < argc) unresolved_out = argv[++k];
        else if (a == "--no-pointers") options.scan_pointers = false;
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

    recomp::Discovery d = recomp::discover(image, options);
    for (const auto &[what, n] : d.counts) std::printf("  %-36s %llu\n", what.c_str(), (unsigned long long)n);
    std::map<int, uint64_t> kinds;
    for (const auto &[at, t] : d.tables) kinds[t.kind]++;
    const char *kind_names[] = {"absolute", "byte offsets", "shifted byte offsets", "halfword offsets"};
    for (const auto &[k, n] : kinds) std::printf("  jump tables, %-22s %llu\n", kind_names[k], (unsigned long long)n);

    if (!functions_out.empty()) {
        std::ofstream f(functions_out);
        for (const auto &[a, fn] : d.functions)
            f << std::hex << a << '\t' << std::dec << fn.size << '\t' << recomp::source_name(fn.source) << '\t' << fn.name
              << (fn.switches_stack ? "\tswitches the stack" : "") << '\n';
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
