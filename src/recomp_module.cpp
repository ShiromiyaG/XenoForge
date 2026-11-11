// A disc's modules discovered and emitted (recomp.h, "Modules"): what
// xfrecomp emit writes, and the recompile step (game_build.cpp) in process.

#include "xenoforge/game_source.h"
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
void find_executables(const GameSource &source, std::vector<ModuleFile> &out) {
    for (const GameSource::File &e : source.files()) {
        std::string lower = e.path.substr(0, 14);
        for (char &ch : lower) ch = char(std::tolower(uint8_t(ch)));
        if (lower == "$systemupdate\\") continue;
        char magic[4] = {};
        if (e.size < 0x18 || source.read(e, 0, magic, 4) != 4 || std::memcmp(magic, "XEX2", 4) != 0) continue;
        out.push_back({e.path, source.read_all(e)});
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

// a name as a C++ string literal's text
std::string escaped(const std::string &text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\' || ch == '"') out += '\\', out += ch;
        else if (uint8_t(ch) < 0x20 || uint8_t(ch) >= 0x7F) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\%03o", unsigned(uint8_t(ch)));
            out += b;
        } else out += ch;
    }
    return out;
}

// What the project file asks of one module: the discovery's extra starts and
// tables, the functions' names, hooks and mid hooks by function, the ones
// left out, and what of it did not apply.
struct ModulePlan {
    DiscoverOptions discover;
    std::map<uint32_t, EmitHooks> hooks;    // by function
    std::map<uint32_t, std::string> names;  // the names given to functions
    std::set<uint32_t> skip;
};

ModulePlan plan(const ProjectModule *pm, const EmitOptions &options) {
    ModulePlan p;
    p.discover = options.discover;
    if (!pm) return p;
    for (const auto &[a, end] : pm->starts) p.discover.functions.emplace(a, end);
    // a function the port hooks is a function, wherever discovery would put it
    for (uint32_t a : pm->hooks) p.discover.functions.emplace(a, 0);
    for (const auto &[bctr, targets] : pm->jump_tables) p.discover.jump_tables[bctr] = targets;
    p.skip = pm->skip;
    return p;
}

// Applies the project's names and hooks to what discovery found; warnings for
// what has nowhere to go.
void apply_project(const ProjectModule &pm, Discovery &d, ModulePlan &p, EmittedModule &m) {
    size_t unplaced_names = 0;
    for (const auto &[a, name] : pm.names) {
        auto it = d.functions.find(a);
        if (it == d.functions.end() || name.empty()) {
            unplaced_names += !pm.mid_hooks.count(a); // (a mid hook's name is the hook's)
            continue;
        }
        it->second.name = name;
        p.names[a] = name;
    }
    if (unplaced_names)
        m.warnings.push_back(std::to_string(unplaced_names) + " names at addresses no function starts at");
    for (uint32_t a : pm.hooks) {
        auto it = d.functions.find(a);
        if (it == d.functions.end()) {
            m.warnings.push_back("hook 0x" + hex(a, 8) + ": no function there (not code?)");
        } else if (it->second.source == Source::Import) {
            m.warnings.push_back("hook 0x" + hex(a, 8) +
                                 ": an import's thunk, reached through dispatch: its hooks need no slot");
        } else if (p.skip.count(a)) {
            m.warnings.push_back("hook 0x" + hex(a, 8) + ": skipped too, so interpreted: its hooks run there");
        } else {
            p.hooks[a].slot = true;
        }
    }
    for (const auto &[a, jumps] : pm.mid_hooks) {
        auto it = d.functions.upper_bound(a);
        if (it == d.functions.begin() || a >= std::prev(it)->first + std::max<uint32_t>(std::prev(it)->second.size, 4) ||
            std::prev(it)->second.source == Source::Import) {
            m.warnings.push_back("mid hook 0x" + hex(a, 8) + ": inside no function");
            continue;
        }
        p.hooks[std::prev(it)->first].mid[a] = jumps;
    }
}

// The port's names as constants, for its sources (names.h beside table.cpp)
std::string names_header(const std::string &path, const std::string &ns, const std::map<uint32_t, std::string> &names) {
    std::string t = "// Written by xfrecomp emit from " + path +
                    ": the project file's names for its functions, for the port's sources.\n#pragma once\n\n"
                    "#include <cstdint>\n\nnamespace " + ns + "::names {\n";
    std::set<std::string> used;
    for (const auto &[a, name] : names) {
        std::string id;
        for (char ch : name) id += std::isalnum(uint8_t(ch)) ? ch : '_';
        if (id.empty() || std::isdigit(uint8_t(id[0]))) id = "_" + id;
        if (!used.insert(id).second) used.insert(id += "_" + hex(a, 8));
        t += "inline constexpr uint32_t " + id + " = 0x" + hex(a, 8) + "u;\n";
    }
    return t + "} // namespace " + ns + "::names\n";
}

} // namespace

std::string alias_identifier(const std::string &name, uint32_t address, std::set<std::string> &used) {
    static const std::set<std::string> keywords = {
        "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "concept",
        "const", "consteval", "constexpr", "constinit", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "double", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "nullptr",
        "operator", "or", "private", "protected", "public", "register", "requires", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor", "c"};
    std::string id;
    for (char ch : name) id += std::isalnum(uint8_t(ch)) || ch == '_' ? ch : '_';
    if (id.empty() || std::isdigit(uint8_t(id[0]))) id = "_" + id;
    bool capitals = true;
    for (char ch : id) capitals &= !std::islower(uint8_t(ch));
    if (keywords.count(id) || capitals || id.starts_with("xf") || id.starts_with("XF") || id.starts_with("__") ||
        used.count(id))
        id += "_" + hex(address, 8);
    used.insert(id);
    return id;
}

namespace {

// The alias's declaration: the target's symbol as the C++ ABI of the
// compiler mangles `void <ns>::<target>(xf::ppc::Context &)` (MSVC's, else
// the Itanium ABI's), so the alias needs no change to the target.
std::string alias_line(const std::string &ns, const std::string &target, const std::string &alias) {
    const std::string msvc = "?" + target + "@" + ns + "@@YAXAEAUContext@ppc@xf@@@Z";
    const std::string itanium =
        "_ZN" + std::to_string(ns.size()) + ns + std::to_string(target.size()) + target + "ERN2xf3ppc7ContextE";
    return "#ifdef _MSC_VER\nXF_FN(" + alias + ") __attribute__((alias(\"" + msvc + "\")));\n#else\nXF_FN(" + alias +
           ") __attribute__((alias(\"" + itanium + "\")));\n#endif\n";
}

// One module's directory from its image (recomp.h's emit_image); false, and
// why, when the emission is stopped.
bool emit_image_at(const Image &image, const std::string &path, uint64_t digest, const std::filesystem::path &out,
                   const std::string &directory, const EmitOptions &options, size_t index, size_t count,
                   EmittedModule &m, std::string &error) {
    const ProjectModule *pm = options.project ? options.project->find(path) : nullptr;
    if (pm && pm->empty()) pm = nullptr;
    ModulePlan p = plan(pm, options);
    Discovery d = discover(image, p.discover);
    if (pm) apply_project(*pm, d, p, m);
    if (stopped(options, EmitStage::Discovering, index + 1, count, path)) return error = "stopped", false;
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
    const std::string head = "// Written by xfrecomp emit from " + path +
                             ". Game code: not for the repository.\n#include \"xenoforge/ppc_generated.h\"\n\n";
    std::set<std::string> current;
    std::set<uint32_t> left; // the functions left out: no generated code in the table
    // the hook slots the emitted code holds: functions (their original body
    // xfo_), mid-function hooks
    std::map<uint32_t, uint32_t> slots; // address, the function's size
    std::set<uint32_t> mids;
    std::set<std::string> aliases; // the alias symbols made (project names only)
    size_t done = 0;
    for (const auto &[k, functions] : by_file) {
        std::string bodies;
        std::set<uint32_t> calls;
        std::set<uint32_t> file_slots, file_mids;
        for (const Function *fn : functions) {
            // a function the emitter cannot emit (or the options or the
            // project leave out) becomes a stub that dispatches to the
            // interpreter
            std::string reason = options.leave_out ? options.leave_out(path, fn->address) : std::string();
            if (reason.empty() && p.skip.count(fn->address)) reason = "the project file skips it";
            if (reason.empty()) {
                try {
                    std::set<uint32_t> its;
                    auto h = p.hooks.find(fn->address);
                    const EmitHooks *hooks = h == p.hooks.end() ? nullptr : &h->second;
                    if (hooks) hooks->placed.clear();
                    bodies += emit_function(image, d, *fn, prefix, &its, hooks);
                    // the project file's name as a symbol of its own: the body's
                    // (a hooked function's original, xfo_)
                    if (auto n = p.names.find(fn->address); n != p.names.end())
                        bodies += alias_line(ns, (hooks && hooks->slot ? "xfo_" : prefix) + hex(fn->address, 8),
                                             alias_identifier(n->second, fn->address, aliases));
                    bodies += "\n";
                    calls.insert(its.begin(), its.end());
                    if (hooks && hooks->slot) file_slots.insert(fn->address), slots[fn->address] = fn->size;
                    if (hooks) file_mids.insert(hooks->placed.begin(), hooks->placed.end());
                } catch (const std::exception &x) {
                    reason = std::string("the emitter failed: ") + x.what();
                }
            }
            if (!reason.empty()) {
                bodies += left_out_stub(prefix, fn->address, reason) + "\n";
                left.insert(fn->address);
                m.left_out.push_back({path, fn->address, reason});
            }
            calls.insert(fn->address);
        }
        mids.insert(file_mids.begin(), file_mids.end());
        std::string text = head + open;
        for (uint32_t a : calls) text += "XF_FN(" + function_name(prefix, a) + ");\n";
        // (the slots are table.cpp's)
        for (uint32_t a : file_slots) text += "extern ::xf::ppc::HookSlot xf_hook_" + hex(a, 8) + ";\n";
        for (uint32_t a : file_mids) text += "extern ::xf::ppc::HookSlot xf_mid_" + hex(a, 8) + ";\n";
        text += "\n" + bodies + close;
        std::string name = "functions_" + std::to_string(k) + ".cpp";
        current.insert(name);
        m.written += write_if_changed(dir / name, text);
        m.sources.push_back(directory + "/" + name);
        if (stopped(options, EmitStage::Emitting, ++done, by_file.size() + 1, path + ": " + name))
            return error = "stopped", false;
    }
    if (pm)
        for (const auto &[a, jumps] : pm->mid_hooks)
            if (!mids.count(a)) {
                bool inside = false;
                for (const auto &[f, h] : p.hooks) inside |= h.mid.count(a) != 0;
                if (inside) m.warnings.push_back("mid hook 0x" + hex(a, 8) + ": no instruction its function runs");
            }
    // files of an earlier emission that hold nothing now: left empty
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.starts_with("functions_") && name.ends_with(".cpp") && !current.count(name))
            m.written += write_if_changed(entry.path(), "\n");
    }
    // an earlier layout's header of every function, and an earlier
    // project's names
    std::error_code ec;
    std::filesystem::remove(dir / "functions.h", ec);
    if (p.names.empty()) std::filesystem::remove(dir / "names.h", ec);
    else m.written += write_if_changed(dir / "names.h", names_header(path, ns, p.names));
    // the path as a C++ string
    std::string literal;
    for (char ch : path) literal += ch == '\\' ? std::string("\\\\") : std::string(1, ch);
    std::string t = "// Written by xfrecomp emit: " + path + "'s functions by address, for dispatch.\n" +
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
    t += "};\n\n";
    // The project file's: the hook slots (the runtime fills them; the
    // generated code reads them) and the names, for the reports. Nothing
    // without one, so the table is what it was.
    std::string extra;
    if (!slots.empty() || !mids.empty()) {
        t += "// the hook slots of what the port hooks (ppc_module.h's HookSlot): functions, then mid-function\n";
        for (const auto &[a, size] : slots) t += "XF_FN(xfo_" + hex(a, 8) + ");\n";
        for (const auto &[a, size] : slots)
            t += "::xf::ppc::HookSlot xf_hook_" + hex(a, 8) + " = {0x" + hex(a, 8) + "u, 0x" + hex(size, 0) + "u, xfo_" +
                 hex(a, 8) + ", nullptr};\n";
        for (uint32_t a : mids) t += "::xf::ppc::HookSlot xf_mid_" + hex(a, 8) + " = {0x" + hex(a, 8) + "u, 0u, nullptr, nullptr};\n";
        t += "::xf::ppc::HookSlot *const hooks[] = {";
        for (const auto &[a, size] : slots) t += "&xf_hook_" + hex(a, 8) + ", ";
        for (uint32_t a : mids) t += "&xf_mid_" + hex(a, 8) + ", ";
        t += "};\n\n";
        extra = ", hooks, " + std::to_string(slots.size() + mids.size());
    }
    if (!p.names.empty()) {
        t += "// the project file's names for its functions\nconst ::xf::ppc::FunctionName names[] = {\n";
        for (const auto &[a, name] : p.names) t += "    {0x" + hex(a, 8) + "u, \"" + escaped(name) + "\"},\n";
        t += "};\n\n";
        extra = (extra.empty() ? std::string(", nullptr, 0") : extra) + ", names, " + std::to_string(p.names.size());
    }
    m.hooks = slots.size(), m.mid_hooks = mids.size(), m.names = p.names.size();
    m.digest = digest;
    t += "// the image it was made from: path, base, size, digest (xf::image_digest)\n"
         "extern const ::xf::ppc::GeneratedModule module = {\"" +
         literal + "\", 0x" + hex(image.base, 8) + "u, 0x" + hex(image.bytes.size(), 8) + "u, 0x" + hex(m.digest, 16) +
         "ull, functions, " + std::to_string(list.size()) + ", resumes, " + std::to_string(resumes.size()) + extra +
         "};\n" + close;
    m.written += write_if_changed(dir / "table.cpp", t);
    m.sources.push_back(directory + "/table.cpp");
    if (stopped(options, EmitStage::Emitting, ++done, by_file.size() + 1, path + ": table.cpp"))
        return error = "stopped", false;
    m.path = path, m.directory = directory, m.ns = ns;
    m.functions = list.size(), m.instructions = total, m.files = int(by_file.size());
    return true;
}

// One module's directory (recomp.h); false, and why, when the executable
// cannot be read or the emission is stopped.
bool emit_module(const ModuleFile &e, const std::filesystem::path &out, const std::string &directory,
                 const EmitOptions &options, size_t index, size_t count, EmittedModule &m, std::string &error) {
    Xex xex;
    Image image;
    if (!parse_xex(e.file, xex, error) || !load_xex_image(e.file, xex, error) || !read_image(xex, image, error))
        return error = e.path + ": " + error, false;
    return emit_image_at(image, e.path, image_digest(xex.image), out, directory, options, index, count, m, error);
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
        auto source = GameSource::open(input, error);
        if (!source) return false;
        if (only.empty()) find_executables(*source, out);
        for (const std::string &path : only) {
            const GameSource::File *entry = source->find(path);
            if (!entry) return error = "no " + path + (source->package() ? " in the package" : " on the disc"), false;
            out.push_back({path, source->read_all(*entry)});
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

bool emit_image(const Image &image, const std::string &path, uint64_t digest, const std::filesystem::path &out,
                const std::string &directory, const EmitOptions &options, EmittedModule &m, std::string &error) {
    return emit_image_at(image, path, digest, out, directory, options, 0, 1, m, error);
}

std::string left_out_stub(const std::string &prefix, uint32_t address, const std::string &reason) {
    std::string why;
    for (char ch : reason) why += ch == '\n' || ch == '\r' ? ' ' : ch;
    return "// left out of the generated code: " + why + "\nXF_FN(" + function_name(prefix, address) +
           ") { ::xf::ppc::dispatch(c, 0x" + function_name("", address) + "u); }\n";
}

std::string left_out_original_stub(const std::string &prefix, uint32_t address, const std::string &reason) {
    std::string why;
    for (char ch : reason) why += ch == '\n' || ch == '\r' ? ' ' : ch;
    return "// left out of the generated code: " + why + "\nXF_FN(" + function_name(prefix, address) +
           ") { c.system->run_original(c, 0x" + function_name("", address) + "u); }\n";
}

} // namespace xf::recomp
