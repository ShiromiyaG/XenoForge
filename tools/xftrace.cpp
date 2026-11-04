// xftrace: what Xenia's GPU traces (.xtr) use, counted. One report per trace
// and, for several, one for all of them: the corpus report that decides what
// the renderer does first (docs/renderer.md).

#include "xenoforge/gpu_trace.h"
#include "xenoforge/trace_stats.h"
#include "xenoforge/xenos.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace xf;

namespace {

using Namer = const char *(*)(uint32_t);

void print_counts(const char *label, const Counts &counts, Namer name) {
    if (counts.empty()) return;
    std::vector<std::pair<uint32_t, uint64_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return a.second > b.second; });
    std::printf("  %-18s", label);
    int column = 20;
    for (size_t i = 0; i < sorted.size(); i++) {
        char item[96];
        const char *n = name(sorted[i].first);
        int len = n[0] == '?' ? std::snprintf(item, sizeof(item), "%s0x%X %llu", i ? ", " : "", sorted[i].first,
                                              static_cast<unsigned long long>(sorted[i].second))
                              : std::snprintf(item, sizeof(item), "%s%s %llu", i ? ", " : "", n,
                                              static_cast<unsigned long long>(sorted[i].second));
        if (column + len > 118) {
            std::printf(",\n  %-18s", "");
            column = 20;
            std::printf("%s", item + 2);
            column += len - 2;
        } else {
            std::printf("%s", item);
            column += len;
        }
    }
    std::printf("\n");
}

const char *msaa_name(uint32_t v) { return v == 0 ? "1x" : v == 1 ? "2x" : v == 2 ? "4x" : "?"; }
const char *source_name(uint32_t v) { return v == 0 ? "index buffer" : v == 1 ? "immediate" : v == 2 ? "auto index" : "?"; }

void report(const TraceStats &s) {
    std::printf("  %-18s%llu MB, %llu memory reads (%llu MB), %llu writes, %llu EDRAM snapshots, %llu swaps\n", "data",
                static_cast<unsigned long long>(s.bytes >> 20), static_cast<unsigned long long>(s.memory_reads),
                static_cast<unsigned long long>(s.memory_read_bytes >> 20),
                static_cast<unsigned long long>(s.memory_writes), static_cast<unsigned long long>(s.edram_snapshots),
                static_cast<unsigned long long>(s.swaps));
    std::printf("  %-18stype 0 %llu, type 1 %llu, type 2 %llu, type 3 %llu, bad %llu\n", "packets",
                static_cast<unsigned long long>(s.packets[0]), static_cast<unsigned long long>(s.packets[1]),
                static_cast<unsigned long long>(s.packets[2]), static_cast<unsigned long long>(s.packets[3]),
                static_cast<unsigned long long>(s.bad_packets));
    print_counts("type 3", s.type3, xenos::pm4_opcode_name);

    std::printf("  %-18s%llu (%llu resolves)\n", "draws", static_cast<unsigned long long>(s.draws),
                static_cast<unsigned long long>(s.resolves));
    print_counts("primitives", s.primitives, xenos::primitive_name);
    print_counts("indices", s.sources, source_name);
    print_counts("EDRAM", s.edram_modes, xenos::edram_mode_name);
    print_counts("color targets", s.color_formats, xenos::color_target_format_name);
    print_counts("depth", s.depth_formats, xenos::depth_target_format_name);
    print_counts("MSAA", s.msaa, msaa_name);
    print_counts("resolves to", s.resolve_color_formats, xenos::format_name);
    if (s.resolves)
        std::printf("  %-18sdepth %llu, with a clear %llu\n", "resolves", static_cast<unsigned long long>(s.resolve_depth),
                    static_cast<unsigned long long>(s.resolve_clears));

    // the shaders, each counted once however often it is drawn with
    uint64_t vertex = 0, pixel = 0, dwords = 0, memexport = 0, kills = 0, truncated = 0;
    Counts cf, vec, sca, fetch, vformats, tdims;
    for (const auto &[hash, shader] : s.shaders) {
        (shader.type == 0 ? vertex : pixel)++;
        const auto &u = shader.summary;
        dwords += u.dwords;
        memexport += u.memexport, kills += u.kills, truncated += u.truncated;
        for (auto [k, v] : u.cf_ops) cf[k] += v;
        for (auto [k, v] : u.vector_ops) vec[k] += v;
        for (auto [k, v] : u.scalar_ops) sca[k] += v;
        for (auto [k, v] : u.fetch_ops) fetch[k] += v;
        for (auto [k, v] : u.vertex_formats) vformats[k] += v;
        for (auto [k, v] : u.texture_dims) tdims[k] += v;
    }
    std::printf("  %-18s%llu vertex, %llu pixel (%llu KB of microcode); %llu with memexport, %llu that kill, %llu cut "
                "short; draws without a shader %llu, with memexport %llu\n",
                "shaders", static_cast<unsigned long long>(vertex), static_cast<unsigned long long>(pixel),
                static_cast<unsigned long long>(dwords * 4 / 1024), static_cast<unsigned long long>(memexport),
                static_cast<unsigned long long>(kills), static_cast<unsigned long long>(truncated),
                static_cast<unsigned long long>(s.draws_without_shader),
                static_cast<unsigned long long>(s.draws_with_memexport));
    print_counts("control flow", cf, xenos::ucode::cf_opcode_name);
    print_counts("vector ops", vec, xenos::ucode::vector_opcode_name);
    print_counts("scalar ops", sca, xenos::ucode::scalar_opcode_name);
    print_counts("fetches", fetch, xenos::ucode::fetch_opcode_name);
    print_counts("tfetch dims", tdims, xenos::dimension_name);
    print_counts("vertex formats", vformats, xenos::format_name);

    Counts tformats, tdimensions;
    for (const auto &t : s.textures) {
        tformats[std::get<1>(t)]++;
        tdimensions[std::get<5>(t)]++;
    }
    std::printf("  %-18s%zu (%llu bindings, %llu without a texture constant; vertex fetches without a vertex "
                "constant %llu)\n",
                "textures", s.textures.size(), static_cast<unsigned long long>(s.texture_bindings),
                static_cast<unsigned long long>(s.missing_texture_constants),
                static_cast<unsigned long long>(s.missing_vertex_constants));
    print_counts("texture formats", tformats, xenos::format_name);
    print_counts("dimensions", tdimensions, xenos::dimension_name);
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; i++) {
        std::filesystem::path p(argv[i]);
        if (std::filesystem::is_directory(p)) {
            for (const auto &e : std::filesystem::recursive_directory_iterator(p))
                if (e.is_regular_file() && e.path().extension() == ".xtr") files.push_back(e.path());
        } else {
            files.push_back(p);
        }
    }
    if (files.empty()) {
        std::printf("usage: xftrace <trace.xtr | folder>...\n"
                    "  counts what Xenia's GPU traces use (xenia-canary --trace_gpu_frames=...)\n");
        return 2;
    }
    std::sort(files.begin(), files.end());

    TraceStats total;
    int failed = 0;
    for (const auto &file : files) {
        GpuTrace trace;
        TraceStats stats;
        std::string error;
        if (!open_gpu_trace(file, trace, error) || !count_gpu_trace(trace, stats, error)) {
            std::printf("%s: %s\n", file.string().c_str(), error.c_str());
            failed++;
            continue;
        }
        std::printf("%s (title %08X, Xenia %.9s)\n", file.string().c_str(), trace.title_id, trace.commit.c_str());
        report(stats);
        total.merge(stats);
    }
    if (files.size() > 1) {
        std::printf("all %u traces\n", total.traces);
        report(total);
    }
    return failed ? 1 : 0;
}
