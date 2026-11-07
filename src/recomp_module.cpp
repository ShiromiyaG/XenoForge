// A disc's modules discovered and emitted (recomp.h, "Modules"): what
// xfrecomp emit writes, and the recompile step (game_build.cpp) in process.

#include "xenoforge/disc.h"
#include "xenoforge/recomp.h"
#include "xenoforge/xex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace xf::recomp {
namespace {

bool is_xex_file(const std::vector<uint8_t> &file) {
    return file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0;
}

// every XEX2 executable under dir, but the system update's
void find_executables(const Disc &disc, const DiscEntry &dir, const std::string &prefix, std::vector<ModuleFile> &out) {
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

bool stopped(const EmitOptions &o, EmitStage stage, size_t done, size_t total, const std::string &detail) {
    return o.progress && !o.progress(stage, done, total, detail);
}

// One module's directory (recomp.h); false, and why, when the executable
// cannot be read or the emission is stopped.
bool emit_module(const ModuleFile &e, const std::filesystem::path &out, const std::string &directory,
                 const EmitOptions &options, size_t index, size_t count, EmittedModule &m, std::string &error) {
    Xex xex;
    Image image;
    if (!parse_xex(e.file, xex, error) || !load_xex_image(e.file, xex, error) || !read_image(xex, image, error))
        return error = e.path + ": " + error, false;
    Discovery d = discover(image, options.discover);
    if (stopped(options, EmitStage::Discovering, index + 1, count, e.path)) return error = "stopped", false;
    const std::filesystem::path dir = out / directory;
    std::filesystem::create_directories(dir);
    const std::string ns = "xfm_" + directory, prefix = "xf_";
    // the functions with code: all but the imports
    std::vector<const Function *> list;
    uint64_t total = 0;
    for (const auto &[a, f] : d.functions)
        if (f.source != Source::Import) list.push_back(&f), total += f.size / 4;
    // Files by address: 64 KB of the image each (about 16,000 instructions,
    // a file the build compiles in seconds), or what options.files asks;
    // empty ones not written. A function found or lost changes its own file
    // and its callers' (each file declares what it calls), not every file
    // after it.
    uint32_t bucket = 0x10000;
    if (options.files > 0 && !list.empty()) {
        uint32_t span = list.back()->address + list.back()->size - list.front()->address;
        bucket = std::max<uint32_t>(0x1000, (span / uint32_t(options.files) + 0xFFF) & ~0xFFFu);
    }
    std::map<uint32_t, std::vector<const Function *>> by_file;
    for (const Function *fn : list) by_file[(fn->address - image.base) / bucket].push_back(fn);
    const std::string open = "namespace " + ns + " {\n\n", close = "\n} // namespace " + ns + "\n";
    const std::string head = "// Written by xfrecomp emit from " + e.path +
                             ". Game code: not for the repository.\n#include \"xenoforge/ppc_generated.h\"\n\n";
    std::set<std::string> current;
    std::set<uint32_t> left; // the functions left out: no generated code in the table
    size_t done = 0;
    for (const auto &[k, functions] : by_file) {
        std::string bodies;
        std::set<uint32_t> calls;
        for (const Function *fn : functions) {
            // a function the emitter cannot emit (or the options leave out)
            // becomes a stub that dispatches to the interpreter
            std::string reason = options.leave_out ? options.leave_out(e.path, fn->address) : std::string();
            if (reason.empty()) {
                try {
                    std::set<uint32_t> its;
                    bodies += emit_function(image, d, *fn, prefix, &its) + "\n";
                    calls.insert(its.begin(), its.end());
                } catch (const std::exception &x) {
                    reason = std::string("the emitter failed: ") + x.what();
                }
            }
            if (!reason.empty()) {
                bodies += left_out_stub(prefix, fn->address, reason) + "\n";
                left.insert(fn->address);
                m.left_out.push_back({e.path, fn->address, reason});
            }
            calls.insert(fn->address);
        }
        std::string text = head + open;
        for (uint32_t a : calls) text += "XF_FN(" + function_name(prefix, a) + ");\n";
        text += "\n" + bodies + close;
        std::string name = "functions_" + std::to_string(k) + ".cpp";
        current.insert(name);
        m.written += write_if_changed(dir / name, text);
        m.sources.push_back(directory + "/" + name);
        if (stopped(options, EmitStage::Emitting, ++done, by_file.size() + 1, e.path + ": " + name))
            return error = "stopped", false;
    }
    // files of an earlier emission that hold nothing now: left empty
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.starts_with("functions_") && name.ends_with(".cpp") && !current.count(name))
            m.written += write_if_changed(entry.path(), "\n");
    }
    // an earlier layout's header of every function
    std::error_code ec;
    std::filesystem::remove(dir / "functions.h", ec);
    // the path as a C++ string
    std::string literal;
    for (char ch : e.path) literal += ch == '\\' ? std::string("\\\\") : std::string(1, ch);
    std::string t = "// Written by xfrecomp emit: " + e.path + "'s functions by address, for dispatch.\n" +
                    "#include \"xenoforge/ppc_generated.h\"\n\n" + open;
    for (const Function *fn : list) t += "XF_FN(" + function_name(prefix, fn->address) + ");\n";
    t += "\nconst ::xf::ppc::GeneratedFunction functions[] = {\n";
    // (a function left out has none: dispatch runs its address in the
    // interpreter, and its stub's own dispatch does not come back to it)
    for (const Function *fn : list)
        t += "    {0x" + function_name("", fn->address) + "u, " +
             (left.count(fn->address) ? std::string("nullptr") : function_name(prefix, fn->address)) + "},\n";
    if (list.empty()) t += "    {0, nullptr}, // (none: the count says 0)\n";
    // where the functions take a return from a stack switch elsewhere, in
    // order (the kernel's dispatch lets those through to them)
    std::set<uint32_t> resumes;
    for (const Function *fn : list) resumes.insert(fn->resumes.begin(), fn->resumes.end());
    t += "};\n\nconst uint32_t resumes[] = {";
    for (uint32_t a : resumes) t += "0x" + function_name("", a) + "u, ";
    if (resumes.empty()) t += "0 /* (none: the count says 0) */";
    m.digest = image_digest(xex.image);
    t += "};\n\n// the image it was made from: path, base, size, digest (xf::image_digest)\n"
         "extern const ::xf::ppc::GeneratedModule module = {\"" +
         literal + "\", 0x" + hex(xex.base_address, 8) + "u, 0x" + hex(xex.image.size(), 8) + "u, 0x" +
         hex(m.digest, 16) + "ull, functions, " + std::to_string(list.size()) + ", resumes, " +
         std::to_string(resumes.size()) + "};\n" + close;
    m.written += write_if_changed(dir / "table.cpp", t);
    m.sources.push_back(directory + "/table.cpp");
    if (stopped(options, EmitStage::Emitting, ++done, by_file.size() + 1, e.path + ": table.cpp"))
        return error = "stopped", false;
    m.path = e.path, m.directory = directory, m.ns = ns;
    m.functions = list.size(), m.instructions = total, m.files = int(by_file.size());
    return true;
}

} // namespace

bool find_modules(const std::filesystem::path &input, const std::vector<std::string> &only,
                  std::vector<ModuleFile> &out, std::string &error) {
    out.clear();
    {
        std::ifstream f(input, std::ios::binary);
        if (!f) return error = "cannot read " + input.string(), false;
        char magic[4] = {};
        f.read(magic, 4);
        if (f && std::memcmp(magic, "XEX2", 4) == 0) {
            f.seekg(0);
            out.push_back({input.filename().string(),
                           std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>())});
        }
    }
    if (out.empty()) {
        auto disc = Disc::open(input, error);
        if (!disc) return false;
        if (only.empty()) find_executables(*disc, disc->root(), "", out);
        for (const std::string &path : only) {
            const DiscEntry *entry = disc->find(path);
            if (!entry || entry->directory) return error = "no " + path + " on the disc", false;
            out.push_back({path, disc->read_all(*entry)});
        }
    }
    // title updates are patches to apply, not modules
    std::erase_if(out, [](const ModuleFile &e) {
        Xex x;
        std::string ignored;
        return !is_xex_file(e.file) || (parse_xex(e.file, x, ignored) && x.is_patch());
    });
    if (out.empty()) return error = "no executable to emit", false;
    return true;
}

bool emit_modules(const std::vector<ModuleFile> &modules, const std::filesystem::path &out, const EmitOptions &options,
                  std::vector<EmittedModule> &emitted, std::string &error) {
    emitted.clear();
    std::vector<EmittedModule> skipped;
    std::filesystem::create_directories(out);
    std::set<std::string> names;
    for (size_t i = 0; i < modules.size(); i++) {
        const ModuleFile &e = modules[i];
        std::string name = identifier(e.path);
        for (int k = 2; !names.insert(name).second; k++) name = identifier(e.path) + "_" + std::to_string(k);
        if (stopped(options, EmitStage::Discovering, i, modules.size(), e.path)) return error = "stopped", false;
        EmittedModule m;
        bool emitted_it = false;
        try {
            emitted_it = emit_module(e, out, name, options, i, modules.size(), m, error);
        } catch (const std::exception &x) {
            if (!options.keep_going) throw;
            error = e.path + ": " + x.what();
        }
        if (!emitted_it) {
            if (!options.keep_going || error == "stopped") return false;
            // left out whole: no table, so the kernel interprets it
            EmittedModule whole;
            whole.path = e.path;
            whole.left_out = {{e.path, 0, "the module was not emitted: " + error}};
            skipped.push_back(std::move(whole));
            continue;
        }
        emitted.push_back(std::move(m));
    }
    std::string t = "// Written by xfrecomp emit: every module's table, for the game library (ppc_module.h).\n"
                    "#include \"xenoforge/ppc_module.h\"\n\n";
    for (const EmittedModule &m : emitted)
        t += "namespace " + m.ns + " {\nextern const ::xf::ppc::GeneratedModule module;\n}\n";
    t += "\nextern \"C\" const ::xf::ppc::GeneratedModule *const xf_modules[] = {\n";
    for (const EmittedModule &m : emitted) t += "    &" + m.ns + "::module,\n";
    t += "};\nextern \"C\" const size_t xf_module_count = " + std::to_string(emitted.size()) + ";\n";
    write_if_changed(out / "modules.cpp", t);
    // the modules left out whole after the emitted ones: no directory, no
    // table, no sources, only their left_out
    for (EmittedModule &m : skipped) emitted.push_back(std::move(m));
    return true;
}

std::string left_out_stub(const std::string &prefix, uint32_t address, const std::string &reason) {
    std::string why;
    for (char ch : reason) why += ch == '\n' || ch == '\r' ? ' ' : ch;
    return "// left out of the generated code: " + why + "\nXF_FN(" + function_name(prefix, address) +
           ") { ::xf::ppc::dispatch(c, 0x" + function_name("", address) + "u); }\n";
}

} // namespace xf::recomp
