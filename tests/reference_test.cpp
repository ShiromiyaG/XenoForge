// The three-way check (docs/renderer.md, "The three-way check"):
// every trace under the traces directory with Xenia's dump of the same name
// is drawn by the software reference renderer (reference.h, the Xenos on the
// CPU from the documents) and by the Vulkan renderer on each of its render
// target paths (exact: fragment shader interlock; host: host render targets),
// and the three pictures are compared pair by pair: the mean difference over
// RGB (of 255) and the share of pixels with a component off by more than 16.
// A pair agrees within replay_test's bound (mean 1, 0.1% of pixels). Each
// frame on each path is put in a class:
//
//   a  all three agree (two pairs of three within the bound count too: the
//      third is then just past it);
//   b  ours and Xenia agree, the reference differs: the reference is
//      suspect, or an error the two emulators share;
//   c  ours and the reference agree, Xenia differs: ours is likely right;
//   d  ours alone differs: our bug;
//   e  all three differ.
//
// Xenia's picture for a path: on the exact path its Direct3D 12 ROV dump
// (<dumps>/rov/d3d12), then its Vulkan FSI one (<dumps>/rov); on the host
// path its Direct3D 12 default dump (<dumps>/d3d12), then its Vulkan one
// (<dumps>), then the ROV one. The first that exists is the picture, unless
// ours agrees only with a later one, as replay_test accepts it (the line
// names the picture taken).
//
// The gate. XF_REFERENCE_BASELINE names a TOML file outside the repository
// (it names traces, which carry title IDs), with the classes and numbers a
// run is held to; a frame fails when its class gets worse (a, c, b, d, e in
// that order: how far ours is from being right) or any pair's numbers grow
// past the baseline's by more than MEAN_SLACK or SHARE_SLACK. Without the file
// there is no gate: the table is printed, and only crashes and failed replays
// fail. `reference_test --update` writes the run's classes and numbers into
// the file (frames not run, with XF_REFERENCE_FILTER, are kept). The file:
//
//   traces = "<dir>"              # where the .xtr are (XF_TRACES wins)
//   dumps = ["<dir>", "<dir>"]    # Xenia's dumps, each in the layout above
//                                 # (XF_DUMPS wins: dirs split by ";")
//   [[frame]]
//   name = "<dir>/<stem>"         # the trace, relative to traces
//   path = "exact"                # or "host"
//   class = "a"
//   xenia = "rov/d3d12"           # the Xenia picture compared
//   ref_xenia = [0.1000, 0.0000]  # mean (of 255), share off by > 16 (%)
//   ref_ours = [0.1000, 0.0000]
//   ours_xenia = [0.0000, 0.0000]
//
// Skipped (77) with no traces to draw (neither XF_TRACES nor the baseline's).
// XF_REFERENCE_FILTER=<substring> runs only the traces whose names hold it;
// XF_REFERENCE_PATHS=exact,host picks the paths (default: both, host alone
// where the device has no interlock; XF_RENDER_TARGET_PATH, if set, alone);
// XF_REFERENCE_OUT=<dir> writes the reference's frames there (<stem>.png) and
// ours (<stem>_<path>.png).

#include "check.h"
#include "xenoforge/reference.h"
#include "xenoforge/renderer.h"
#include "xenoforge/toml.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace {

// replay_test's bound: two pictures agree within it
constexpr double AGREE_MEAN = 1.0, AGREE_SHARE = 0.1;
// how far a pair's numbers may grow past the baseline's before the gate fails
// (the reference is deterministic; the Vulkan renderer's frames are too, on
// one device and driver: this is room for another driver's rounding)
constexpr double MEAN_SLACK = 0.05, SHARE_SLACK = 0.05;

struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

std::optional<Picture> load(const fs::path &png) {
    if (!fs::exists(png)) return std::nullopt;
    int w = 0, h = 0, n = 0;
    unsigned char *data = stbi_load(png.string().c_str(), &w, &h, &n, 4);
    if (!data) return std::nullopt;
    Picture p{uint32_t(w), uint32_t(h), std::vector<unsigned char>(data, data + size_t(w) * h * 4)};
    stbi_image_free(data);
    return p;
}

void save(const fs::path &png, const Picture &p) {
    stbi_write_png(png.string().c_str(), int(p.width), int(p.height), 4, p.rgba.data(), int(p.width) * 4);
}

struct Pair {
    bool valid = false; // both pictures there, of the same size
    double mean = 0, share = 0;
    bool agree() const { return valid && mean <= AGREE_MEAN && share <= AGREE_SHARE; }
};

Pair compare(const Picture &a, const Picture &b) {
    Pair d;
    if (a.width != b.width || a.height != b.height || a.rgba.empty()) return d;
    d.valid = true;
    uint64_t sum = 0, off = 0;
    const size_t pixels = size_t(a.width) * a.height;
    for (size_t i = 0; i < pixels; i++) {
        int worst = 0;
        for (int c = 0; c < 3; c++) {
            int v = std::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c]));
            sum += v;
            worst = std::max(worst, v);
        }
        off += worst > 16;
    }
    d.mean = double(sum) / (double(pixels) * 3);
    d.share = 100.0 * double(off) / double(pixels);
    return d;
}

char classify(const Pair &rx, const Pair &ro, const Pair &ox) {
    if (!ro.valid || !ox.valid) return '-';
    if (int(rx.agree()) + int(ro.agree()) + int(ox.agree()) >= 2) return 'a';
    if (ox.agree()) return 'b';
    if (ro.agree()) return 'c';
    if (rx.agree()) return 'd';
    return 'e';
}

// how far ours is from being right, for the gate: a, c, b, d, e
int rank(char c) {
    switch (c) {
    case 'a': return 0;
    case 'c': return 1;
    case 'b': return 2;
    case 'd': return 3;
    case 'e': return 4;
    default: return 5;
    }
}

std::string cell(const Pair &d) {
    if (!d.valid) return "-";
    char text[48];
    std::snprintf(text, sizeof(text), "%.3f, %.2f%%", d.mean, d.share);
    return text;
}

struct Row {
    std::string name, path, xenia;
    char klass = '-';
    Pair rx, ro, ox;
};

std::string key(const std::string &name, const std::string &path) { return name + "|" + path; }

Pair pair_of(const xf::toml::Value &frame, const char *what) {
    Pair p;
    const xf::toml::Value *v = frame.find(what);
    if (!v || !v->is_array() || v->array.size() != 2) return p;
    auto number = [](const xf::toml::Value &n) {
        return n.kind == xf::toml::Value::Float ? n.number : double(n.integer);
    };
    p.valid = true;
    p.mean = number(v->array[0]);
    p.share = number(v->array[1]);
    return p;
}

std::string toml_string(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

void write_pair(FILE *f, const char *what, const Pair &p) {
    if (p.valid) std::fprintf(f, "%s = [%.4f, %.4f]\n", what, p.mean, p.share);
}

// the Xenia pictures a path is compared with, in order, under one dumps root
std::vector<std::string> xenia_pictures(const std::string &path) {
    if (path == "exact") return {"rov/d3d12", "rov"};
    return {"d3d12", ".", "rov/d3d12"};
}

} // namespace

int main(int argc, char **argv) {
    bool update = false;
    for (int k = 1; k < argc; k++)
        if (std::strcmp(argv[k], "--update") == 0) update = true;

    // the baseline, and the directories it names
    const char *baseline_path = std::getenv("XF_REFERENCE_BASELINE");
    if (baseline_path && !*baseline_path) baseline_path = nullptr;
    xf::toml::Value baseline;
    std::string error;
    std::string traces_dir;
    std::vector<fs::path> dumps_dirs;
    std::map<std::string, Row> expected;
    if (baseline_path && fs::exists(baseline_path)) {
        std::ifstream in(baseline_path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!xf::toml::parse(text, baseline, error)) {
            std::printf("%s: %s\n", baseline_path, error.c_str());
            return 1;
        }
        if (const auto *v = baseline.find("traces"); v && v->is_string()) traces_dir = v->string;
        if (const auto *v = baseline.find("dumps"); v && v->is_array())
            for (const auto &d : v->array)
                if (d.is_string()) dumps_dirs.push_back(d.string);
        if (const auto *frames = baseline.find("frame"); frames && frames->is_array())
            for (const auto &t : frames->array) {
                Row r;
                if (const auto *v = t.find("name"); v && v->is_string()) r.name = v->string;
                if (const auto *v = t.find("path"); v && v->is_string()) r.path = v->string;
                if (const auto *v = t.find("xenia"); v && v->is_string()) r.xenia = v->string;
                if (const auto *v = t.find("class"); v && v->is_string() && !v->string.empty()) r.klass = v->string[0];
                r.rx = pair_of(t, "ref_xenia");
                r.ro = pair_of(t, "ref_ours");
                r.ox = pair_of(t, "ours_xenia");
                expected[key(r.name, r.path)] = r;
            }
    } else if (baseline_path && !update) {
        std::printf("XF_REFERENCE_BASELINE (%s) does not exist: no gate\n", baseline_path);
        baseline_path = nullptr;
    }
    if (const char *t = std::getenv("XF_TRACES"); t && *t) traces_dir = t;
    if (const char *d = std::getenv("XF_DUMPS"); d && *d) {
        // one directory, or several separated by semicolons
        dumps_dirs.clear();
        for (std::string rest = d; !rest.empty();) {
            const size_t semicolon = rest.find(';');
            if (semicolon) dumps_dirs.push_back(rest.substr(0, semicolon));
            rest = semicolon == std::string::npos ? "" : rest.substr(semicolon + 1);
        }
    }
    if (traces_dir.empty() || dumps_dirs.empty()) {
        std::printf("no traces or dumps (XF_TRACES and XF_DUMPS, or XF_REFERENCE_BASELINE's): skipped\n");
        return 77;
    }
    // (a relative path is the working directory's: ctest runs in the build directory)
    if (!fs::is_directory(traces_dir)) {
        std::printf("the traces (%s) are not a directory from %s\n", traces_dir.c_str(),
                    fs::current_path().string().c_str());
        return 1;
    }
    if (update && !baseline_path) {
        std::printf("--update needs XF_REFERENCE_BASELINE\n");
        return 1;
    }
    const bool gate = baseline_path && !update;
    const char *filter = std::getenv("XF_REFERENCE_FILTER"), *out = std::getenv("XF_REFERENCE_OUT");
    if (out) fs::create_directories(out);

    std::unique_ptr<xf::vk::Device> device = xf::vk::Device::open(error);
    std::vector<std::string> paths;
    if (device) {
        xf::render::Options probe;
        probe.render_target_path = xf::render::Options::RenderTargetPath::fsi;
        const bool interlock =
            xf::render::render_target_path(*device, probe) == xf::render::Options::RenderTargetPath::fsi;
        const char *forced = std::getenv("XF_RENDER_TARGET_PATH");
        std::string wanted = forced && *forced ? (std::strcmp(forced, "fsi") == 0 ? "exact" : forced) : "exact,host";
        if (const char *p = std::getenv("XF_REFERENCE_PATHS"); p && *p) wanted = p;
        for (const char *name : {"exact", "host"})
            if (wanted.find(name) != std::string::npos && (interlock || std::strcmp(name, "host") == 0))
                paths.push_back(name);
    } else {
        std::printf("no Vulkan device (%s): the reference against Xenia alone, no classes\n", error.c_str());
        paths.push_back("exact");
    }

    std::vector<fs::path> traces;
    for (const auto &entry : fs::recursive_directory_iterator(traces_dir))
        if (entry.path().extension() == ".xtr") traces.push_back(entry.path());
    std::sort(traces.begin(), traces.end());

    std::printf("%-34s %-5s %-9s | %-16s | %-16s | %-16s | %s\n", "frame", "path", "Xenia", "ref : Xenia", "ref : ours",
                "ours : Xenia", "class");
    std::vector<Row> rows;
    std::map<char, int> counts;
    int drawn = 0, regressions = 0;
    double seconds_total = 0;
    for (const fs::path &trace_path : traces) {
        const std::string stem = trace_path.stem().string(), png = stem + ".png";
        const std::string name = fs::relative(trace_path, traces_dir).replace_extension().generic_string();
        if (filter && name.find(filter) == std::string::npos) continue;
        // Xenia's pictures of it, by subdirectory, the first dumps root holding one
        std::map<std::string, Picture> xenia;
        for (const fs::path &root : dumps_dirs) {
            for (const char *sub : {"rov/d3d12", "rov", "d3d12", "."})
                if (!xenia.count(sub))
                    if (auto p = load(root / sub / png)) xenia[sub] = std::move(*p);
        }
        if (!xenia.count("rov/d3d12") && !xenia.count("rov") && !xenia.count("d3d12") && !xenia.count(".")) continue;
        xf::GpuTrace trace;
        if (!xf::open_gpu_trace(trace_path, trace, error)) {
            std::printf("%-34s the trace does not open: %s\n", name.c_str(), error.c_str());
            CHECK(false);
            continue;
        }
        // the reference
        xf::reference::Options options;
        xf::reference::Image frame;
        xf::reference::Report report;
        auto start = std::chrono::steady_clock::now();
        bool ok = xf::reference::replay(trace, options, frame, report, error);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        seconds_total += seconds;
        CHECK(ok);
        CHECK(frame.rgba.size() == size_t(frame.width) * frame.height * 4);
        if (!ok || frame.rgba.empty()) {
            std::printf("%-34s the reference failed: %s\n", name.c_str(), error.c_str());
            continue;
        }
        drawn++;
        const Picture ref{frame.width, frame.height, frame.rgba};
        if (out) save(fs::path(out) / png, ref);
        for (const std::string &path : paths) {
            Row row;
            row.name = name;
            row.path = path;
            std::optional<Picture> ours;
            if (device) {
                xf::render::Image image;
                xf::render::Report vulkan_report;
                xf::render::Options vulkan_options;
                vulkan_options.render_target_path = path == "exact" ? xf::render::Options::RenderTargetPath::fsi
                                                                    : xf::render::Options::RenderTargetPath::host;
                const bool replayed = xf::render::replay(trace, *device, vulkan_options, image, vulkan_report, error);
                CHECK(replayed);
                if (!replayed)
                    std::printf("%-34s %-5s the Vulkan replay failed: %s\n", name.c_str(), path.c_str(), error.c_str());
                if (!image.rgba.empty()) ours = Picture{image.width, image.height, image.rgba};
                if (ours && out) save(fs::path(out) / (stem + "_" + path + ".png"), *ours);
            }
            // Xenia's picture: the first there, or a later one ours agrees with alone
            const Picture *x = nullptr;
            for (const std::string &sub : xenia_pictures(path)) {
                auto it = xenia.find(sub);
                if (it == xenia.end()) continue;
                if (!x) {
                    x = &it->second;
                    row.xenia = sub;
                    if (!ours || compare(*ours, *x).agree()) break;
                } else if (compare(*ours, it->second).agree()) {
                    x = &it->second;
                    row.xenia = sub;
                    break;
                }
            }
            if (!x) continue;
            row.rx = compare(ref, *x);
            if (ours) {
                row.ro = compare(ref, *ours);
                row.ox = compare(*ours, *x);
            }
            row.klass = classify(row.rx, row.ro, row.ox);
            counts[row.klass]++;
            std::string verdict;
            if (gate) {
                auto it = expected.find(key(name, path));
                if (it == expected.end()) {
                    verdict = "new (not in the baseline)";
                } else {
                    const Row &base = it->second;
                    std::string worse;
                    if (rank(row.klass) > rank(base.klass))
                        worse += std::string(" class ") + base.klass + " to " + row.klass + ";";
                    auto check = [&](const char *what, const Pair &now, const Pair &then) {
                        if (!then.valid) return;
                        if (!now.valid || now.mean > then.mean + MEAN_SLACK || now.share > then.share + SHARE_SLACK) {
                            char text[160];
                            std::snprintf(text, sizeof(text), " %s %s to %s;", what, cell(then).c_str(),
                                          cell(now).c_str());
                            worse += text;
                        }
                    };
                    check("ref : Xenia", row.rx, base.rx);
                    check("ref : ours", row.ro, base.ro);
                    check("ours : Xenia", row.ox, base.ox);
                    if (!worse.empty()) {
                        verdict = "WORSE than the baseline:" + worse;
                        regressions++;
                    } else if (rank(row.klass) < rank(base.klass)) {
                        verdict = std::string("better than the baseline (") + base.klass + ")";
                    }
                }
            }
            std::printf("%-34s %-5s %-9s | %-16s | %-16s | %-16s | %c %s\n", name.c_str(), path.c_str(),
                        row.xenia.c_str(), cell(row.rx).c_str(), cell(row.ro).c_str(), cell(row.ox).c_str(), row.klass,
                        verdict.c_str());
            rows.push_back(row);
        }
        std::printf("    the reference: %.1f s, %llu draws", seconds, (unsigned long long)report.draws);
        for (auto &[what, n] : report.skipped) std::printf("; skipped %llu: %s", (unsigned long long)n, what.c_str());
        for (auto &[what, n] : report.approximate)
            std::printf("; approximate %llu: %s", (unsigned long long)n, what.c_str());
        std::printf("\n");
        std::fflush(stdout);
    }
    std::printf("%d frames drawn by the reference in %.1f s; classes:", drawn, seconds_total);
    for (auto &[klass, n] : counts) std::printf(" %c %d", klass, n);
    std::printf("\n");
    if (gate) {
        std::printf("the gate (%s): %d worse than the baseline\n", baseline_path, regressions);
        CHECK(regressions == 0);
    }

    if (update) {
        // the run's rows over the baseline's, the rest kept
        std::map<std::string, Row> merged = expected;
        for (const Row &r : rows) merged[key(r.name, r.path)] = r;
        FILE *f = std::fopen(baseline_path, "wb");
        if (!f) {
            std::printf("%s: cannot be written\n", baseline_path);
            return 1;
        }
        std::fprintf(f, "# The three-way check's baseline (XenoForge tests/reference_test.cpp, docs/renderer.md,\n"
                        "# \"The three-way check\"). Outside the repository: it names traces. Refresh with\n"
                        "#   XF_REFERENCE_BASELINE=<this file> build/reference_test --update\n\n");
        std::fprintf(f, "traces = %s\n", toml_string(traces_dir).c_str());
        std::fprintf(f, "dumps = [");
        for (size_t i = 0; i < dumps_dirs.size(); i++)
            std::fprintf(f, "%s%s", i ? ", " : "", toml_string(dumps_dirs[i].generic_string()).c_str());
        std::fprintf(f, "]\n");
        for (auto &[k, r] : merged) {
            std::fprintf(f, "\n[[frame]]\nname = %s\npath = %s\nclass = \"%c\"\nxenia = %s\n",
                         toml_string(r.name).c_str(), toml_string(r.path).c_str(), r.klass,
                         toml_string(r.xenia).c_str());
            write_pair(f, "ref_xenia", r.rx);
            write_pair(f, "ref_ours", r.ro);
            write_pair(f, "ours_xenia", r.ox);
        }
        std::fclose(f);
        std::printf("%s: %zu frames written\n", baseline_path, merged.size());
    }
    return finish("reference");
}
