// The project file (recomp.h; docs/recompiler.md, "The project file"): what a
// port says about the executables on its disc, read from TOML (toml.h), and
// the names it imports from an MSVC map or a Ghidra function list. rexglue's
// function list (`[functions] "0x82012340" = {}`, `[[switch_tables]]`,
// `[[midasm_hook]]`) is read as it is, so a port moves over as it stands.

#include "xenoforge/recomp.h"
#include "xenoforge/toml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace xf::recomp {
namespace {

std::string lower(std::string s) {
    for (char &ch : s) ch = char(std::tolower(uint8_t(ch)));
    return s;
}

bool read_text(const std::filesystem::path &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// An address in hex, with or without 0x ("0x82012340" as rexglue's keys,
// "82012340" or "ram:82012340" as Ghidra's columns); never decimal
bool parse_address(std::string_view s, uint32_t &out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\r')) s.remove_suffix(1);
    if (size_t colon = s.rfind(':'); colon != std::string_view::npos) s.remove_prefix(colon + 1); // ram:82000000
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char ch : s) {
        int d = ch >= '0' && ch <= '9'   ? ch - '0'
                : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                         : -1;
        if (d < 0) return false;
        v = v << 4 | uint32_t(d);
    }
    out = v;
    return true;
}

class Loader {
public:
    Loader(Project &p, std::string &error) : p_(p), error_(error) {}

    bool file(const std::filesystem::path &path, int depth) {
        if (depth > 8) return fail(path.string() + ": includes nest too deep");
        std::string text;
        if (!read_text(path, text)) return fail("cannot read " + path.string());
        return document(text, path.parent_path(), path.filename().string(), depth);
    }

    bool document(std::string_view text, const std::filesystem::path &dir, const std::string &where, int depth) {
        toml::Value root;
        std::string why;
        if (!toml::parse(text, root, why)) return fail(where + ": " + why);
        std::string module = "default.xex";
        std::vector<std::string> includes;
        if (const toml::Value *project = root.find("project")) {
            if (!project->is_table()) return fail(where + ": [project] is not a table");
            for (const auto &[key, v] : project->table) {
                if (key == "name" && v.is_string()) p_.name = v.string;
                else if (key == "module" && v.is_string()) module = v.string;
                else if (key == "includes" || key == "include") strings(v, includes, where);
                else warn(where, v.line, "[project] " + key + " is not read");
            }
        }
        if (const toml::Value *v = root.find("includes")) strings(*v, includes, where);
        for (const std::string &inc : includes)
            if (!file(dir / inc, depth + 1)) return false;
        if (!apply(root, p_.module(module), dir, where, true)) return false;
        if (const toml::Value *modules = root.find("modules")) {
            if (!modules->is_table()) return fail(where + ": modules is not a table");
            for (const auto &[path, t] : modules->table) {
                if (!t.is_table()) return fail(where + ": modules." + path + " is not a table");
                if (!apply(t, p_.module(path), dir, where, false)) return false;
            }
        }
        return true;
    }

private:
    bool fail(const std::string &why) {
        error_ = why;
        return false;
    }
    void warn(const std::string &where, int line, const std::string &what) {
        p_.warnings.push_back(where + (line ? ":" + std::to_string(line) : std::string()) + ": " + what);
    }
    void strings(const toml::Value &v, std::vector<std::string> &out, const std::string &where) {
        if (v.is_string()) out.push_back(v.string);
        else if (v.is_array())
            for (const toml::Value &e : v.array)
                if (e.is_string()) out.push_back(e.string);
                else warn(where, e.line, "expected a file name");
        else warn(where, v.line, "expected a file name or a list of them");
    }
    // an address: an integer, or a string in hex
    bool address(const toml::Value &v, uint32_t &out) {
        if (v.is_integer() && v.integer >= 0 && v.integer <= 0xFFFFFFFFll) return out = uint32_t(v.integer), true;
        return v.is_string() && parse_address(v.string, out);
    }
    bool addresses(const toml::Value &v, std::vector<uint32_t> &out, const std::string &where, const char *what) {
        if (!v.is_array()) return fail(where + ":" + std::to_string(v.line) + ": " + what + " is not a list");
        for (const toml::Value &e : v.array) {
            uint32_t a;
            if (!address(e, a)) return fail(where + ":" + std::to_string(e.line) + ": " + what + " holds a non-address");
            out.push_back(a);
        }
        return true;
    }

    // a module's tables and keys (the top level's for [project].module)
    bool apply(const toml::Value &t, ProjectModule &m, const std::filesystem::path &dir, const std::string &where,
               bool top) {
        std::map<uint32_t, std::string> imported; // names from maps and Ghidra lists: below the explicit ones
        for (const auto &[key, v] : t.table) {
            if (top && (key == "project" || key == "modules" || key == "includes")) continue;
            std::vector<uint32_t> list;
            if (key == "functions") {
                if (!v.is_table()) return fail(where + ": [functions] is not a table");
                for (const auto &[k, entry] : v.table)
                    if (!function(k, entry, m, where)) return false;
            } else if (key == "names") {
                if (!v.is_table()) return fail(where + ": [names] is not a table");
                for (const auto &[k, name] : v.table) {
                    uint32_t a;
                    if (!parse_address(k, a) || !name.is_string())
                        return fail(where + ":" + std::to_string(name.line) + ": [names] wants \"0xADDRESS\" = \"name\"");
                    m.names[a] = name.string;
                }
            } else if (key == "hooks" || key == "skip") {
                if (!addresses(v, list, where, key.c_str())) return false;
                (key == "hooks" ? m.hooks : m.skip).insert(list.begin(), list.end());
            } else if (key == "names_from") {
                if (!v.is_table()) return fail(where + ": names_from is not a table");
                for (const auto &[kind, files] : v.table) {
                    std::vector<std::string> paths;
                    strings(files, paths, where);
                    for (const std::string &file : paths) {
                        std::string text;
                        if (!read_text(dir / file, text)) return fail(where + ": cannot read " + (dir / file).string());
                        size_t n = kind == "map"      ? read_msvc_map(text, imported)
                                   : kind == "ghidra" ? read_ghidra_functions(text, imported)
                                                      : size_t(-1);
                        if (n == size_t(-1)) return fail(where + ": names_from." + kind + " is not map or ghidra");
                        if (n == 0) warn(where, files.line, file + " holds no names");
                    }
                }
            } else if (key == "jump_tables" || key == "switch_tables") {
                // ours (bctr, targets) and rexglue's (address, labels, register)
                if (!v.is_array()) return fail(where + ": " + key + " is not an array of tables");
                for (const toml::Value &e : v.array) {
                    const toml::Value *bctr = e.find(key == "jump_tables" ? "bctr" : "address");
                    const toml::Value *targets = e.find(key == "jump_tables" ? "targets" : "labels");
                    uint32_t at;
                    if (!bctr || !targets || !address(*bctr, at))
                        return fail(where + ":" + std::to_string(e.line) + ": " + key + " entry without its " +
                                    (key == "jump_tables" ? "bctr and targets" : "address and labels"));
                    std::vector<uint32_t> &out = m.jump_tables[at];
                    out.clear();
                    if (!addresses(*targets, out, where, "targets")) return false;
                }
            } else if (key == "mid_hooks" || key == "midasm_hook") {
                // ours (address, jumps, name) and rexglue's (address, name,
                // jump_address*: where the hook may send the code)
                if (!v.is_array()) return fail(where + ": " + key + " is not an array of tables");
                for (const toml::Value &e : v.array) {
                    const toml::Value *at = e.find("address");
                    uint32_t a;
                    if (!at || !address(*at, a))
                        return fail(where + ":" + std::to_string(e.line) + ": " + key + " entry without its address");
                    std::vector<uint32_t> &jumps = m.mid_hooks[a];
                    for (const auto &[k, x] : e.table) {
                        uint32_t j;
                        if (k == "address") continue;
                        if (k == "jumps") {
                            if (!addresses(x, jumps, where, "jumps")) return false;
                        } else if (k.starts_with("jump_address")) {
                            if (address(x, j) && j) jumps.push_back(j);
                        } else if (k == "name") {
                            if (x.is_string() && !m.names.count(a)) m.names[a] = x.string;
                        } else if (k == "registers" || k.starts_with("return") || k == "after_instruction") {
                            // rexglue's: the hook here gets the whole context, and says itself
                            // whether it returns (port.h's MidHook)
                        } else {
                            warn(where, x.line, key + "." + k + " is not read");
                        }
                    }
                }
            } else {
                warn(where, v.line, key + " is not read");
            }
        }
        for (auto &[a, name] : imported) m.names.emplace(a, std::move(name));
        return true;
    }

    // [functions] "0xADDRESS" = { name, end | size, hook, skip }
    bool function(const std::string &key, const toml::Value &e, ProjectModule &m, const std::string &where) {
        uint32_t a;
        if (!parse_address(key, a)) return fail(where + ":" + std::to_string(e.line) + ": [functions] key " + key + " is not an address");
        if (!e.is_table()) return fail(where + ":" + std::to_string(e.line) + ": [functions] " + key + " is not a table");
        uint32_t end = 0;
        bool start = true;
        for (const auto &[k, v] : e.table) {
            uint32_t x;
            if (k == "name" && v.is_string()) m.names[a] = v.string;
            else if (k == "end" && address(v, x)) end = x;
            else if (k == "size" && address(v, x)) end = x ? a + x : 0;
            else if (k == "hook" && v.is_bool()) {
                if (v.boolean) m.hooks.insert(a);
            } else if (k == "skip" && v.is_bool()) {
                if (v.boolean) m.skip.insert(a);
            } else if (k == "start" && v.is_bool()) {
                start = v.boolean;
            } else {
                // rexglue's parent and share_registers have no meaning here
                warn(where, v.line, "[functions] " + key + "." + k + " is not read");
            }
        }
        if (end && end <= a) return fail(where + ":" + std::to_string(e.line) + ": [functions] " + key + ": end before start");
        if (start) m.starts[a] = end;
        return true;
    }

    Project &p_;
    std::string &error_;
};

std::vector<std::string> split_csv(const std::string &line, char delimiter) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t k = 0; k < line.size(); k++) {
        char ch = line[k];
        if (ch == '"') {
            if (quoted && k + 1 < line.size() && line[k + 1] == '"') out.back() += '"', k++;
            else quoted = !quoted;
        } else if (ch == delimiter && !quoted) {
            out.emplace_back();
        } else if (ch != '\r') {
            out.back() += ch;
        }
    }
    return out;
}

std::vector<std::string> split_space(const std::string &line) {
    std::vector<std::string> out;
    std::string word;
    for (char ch : line) {
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            if (!word.empty()) out.push_back(std::move(word)), word.clear();
        } else {
            word += ch;
        }
    }
    if (!word.empty()) out.push_back(std::move(word));
    return out;
}

// Ghidra's names for functions it did not name (FUN_82000000) say nothing
bool meaningful(const std::string &name) {
    return !name.empty() && !name.starts_with("FUN_") && !name.starts_with("thunk_FUN_");
}

} // namespace

const ProjectModule *Project::find(const std::string &path) const {
    std::string want = lower(path);
    for (const ProjectModule &m : modules)
        if (lower(m.path) == want) return &m;
    return nullptr;
}

ProjectModule &Project::module(const std::string &path) {
    std::string want = lower(path);
    for (ProjectModule &m : modules)
        if (lower(m.path) == want) return m;
    modules.push_back({});
    modules.back().path = path;
    return modules.back();
}

bool load_project(const std::filesystem::path &file, Project &out, std::string &error) {
    out = {};
    return Loader(out, error).file(file, 0);
}

bool parse_project(std::string_view text, const std::filesystem::path &dir, Project &out, std::string &error) {
    out = {};
    return Loader(out, error).document(text, dir, "project", 0);
}

size_t read_msvc_map(std::string_view text, std::map<uint32_t, std::string> &names) {
    // every symbol line, then only the functions when the map marks any
    struct Entry {
        uint32_t address;
        std::string name;
        bool function;
    };
    std::vector<Entry> entries;
    bool marked = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::vector<std::string> w = split_space(std::string(text.substr(at, end - at)));
        at = end + 1;
        // 0001:00000000  name  82000400 [f] [i] object
        if (w.size() < 3 || w[0].size() != 13 || w[0][4] != ':') continue;
        uint32_t section, address;
        if (!parse_address(w[0].substr(0, 4), section) || !parse_address(w[2], address) || w[2].size() != 8) continue;
        if (section == 0 || address == 0) continue; // absolute symbols
        bool function = false;
        for (size_t k = 3; k < w.size() && k < 5; k++) function |= w[k] == "f";
        marked |= function;
        entries.push_back({address, w[1], function});
    }
    size_t n = 0;
    for (Entry &e : entries)
        if (!marked || e.function) n += names.emplace(e.address, std::move(e.name)).second;
    return n;
}

size_t read_ghidra_functions(std::string_view text, std::map<uint32_t, std::string> &names) {
    size_t n = 0, at = 0;
    int name_col = -1, address_col = -1;
    char delimiter = ',';
    bool first = true;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line == "\r" || line[0] == '#') continue;
        if (first) {
            first = false;
            delimiter = line.find('\t') != std::string::npos ? '\t' : ',';
            std::vector<std::string> head = split_csv(line, delimiter);
            for (size_t k = 0; k < head.size(); k++) {
                std::string h = lower(head[k]);
                while (!h.empty() && h.back() == ' ') h.pop_back();
                if (h == "name" || h == "function name") name_col = int(k);
                if (h == "location" || h == "address" || h == "entry point" || h == "entry") address_col = int(k);
            }
            if (name_col >= 0 && address_col >= 0) continue; // a header
            name_col = address_col = -1;
        }
        uint32_t a = 0;
        std::string name;
        if (name_col >= 0) {
            std::vector<std::string> cells = split_csv(line, delimiter);
            if (int(cells.size()) <= std::max(name_col, address_col) || !parse_address(cells[address_col], a)) continue;
            name = cells[name_col];
        } else {
            // an address and a name, either order (a hex word is the address)
            std::vector<std::string> w = line.find(',') != std::string::npos ? split_csv(line, ',') : split_space(line);
            if (w.size() < 2) continue;
            if (parse_address(w[0], a) && (w[0].size() >= 8 || w[0].starts_with("0x"))) name = w[1];
            else if (parse_address(w[1], a)) name = w[0];
            else continue;
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
        if (meaningful(name) && a) n += names.emplace(a, std::move(name)).second;
    }
    return n;
}

} // namespace xf::recomp
