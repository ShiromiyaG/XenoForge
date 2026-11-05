// xfrecomp: the recompiler's command (plan step A2). So far, discovery:
//
//   xfrecomp analyze <game.iso | default.xex> [--xex <path on the disc>]
//            [--compare <rexglue's *_init.cpp>] [--functions <out.tsv>]
//
// prints what discovery found by kind; --compare sets the function starts
// against rexglue's PPCFuncMappings for the same executable; --functions
// writes every function (address, size, source, name).

#include "xenoforge/disc.h"
#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <cstdio>
#include <cstdlib>
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
                         "[--functions <out.tsv>]\n");
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

} // namespace

int main(int argc, char **argv) {
    if (argc < 3 || std::string(argv[1]) != "analyze") return usage();
    std::string input = argv[2], xex_path = "default.xex", compare, functions_out;
    for (int k = 3; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--xex" && k + 1 < argc) xex_path = argv[++k];
        else if (a == "--compare" && k + 1 < argc) compare = argv[++k];
        else if (a == "--functions" && k + 1 < argc) functions_out = argv[++k];
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
