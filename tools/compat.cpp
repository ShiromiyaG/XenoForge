// xfcompat's rows, files and the parser of boot's output (compat.h).

#include "compat.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace xf::compat {
namespace {

// ---- JSON: one flat object of strings, numbers, booleans and arrays of strings ----

struct Value {
    std::string text; // a string's, or a number's / boolean's own text
    std::vector<std::string> list;
};
using Object = std::map<std::string, Value>;

std::string escape(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04x", c);
            out += hex;
        } else
            out += char(c);
    }
    return out;
}

struct Reader {
    std::string_view s;
    size_t i = 0;
    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }
    bool eat(char c) {
        space();
        if (i < s.size() && s[i] == c) return i++, true;
        return false;
    }
    bool string(std::string &out) {
        space();
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > s.size()) return false;
                unsigned code = unsigned(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
                i += 4;
                out += code < 0x80 ? char(code) : '?';
                break;
            }
            default: out += e; break;
            }
        }
        return i < s.size() && s[i++] == '"';
    }
    bool object(Object &out) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        for (;;) {
            std::string key;
            if (!string(key) || !eat(':')) return false;
            Value v;
            space();
            if (i < s.size() && s[i] == '"') {
                if (!string(v.text)) return false;
            } else if (i < s.size() && s[i] == '[') {
                i++;
                if (!eat(']')) {
                    for (;;) {
                        std::string item;
                        if (!string(item)) return false;
                        v.list.push_back(item);
                        if (eat(',')) continue;
                        if (eat(']')) break;
                        return false;
                    }
                }
            } else {
                size_t from = i;
                while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\n') i++;
                if (i == from) return false;
                v.text = std::string(s.substr(from, i - from));
            }
            out[key] = std::move(v);
            if (eat(',')) continue;
            return eat('}');
        }
    }
};

bool parse_object(std::string_view text, Object &out) {
    Reader r{text};
    return r.object(out);
}

std::string text_of(const Object &o, const char *key) {
    auto it = o.find(key);
    return it == o.end() ? std::string() : it->second.text;
}
uint64_t number_of(const Object &o, const char *key) { return std::strtoull(text_of(o, key).c_str(), nullptr, 10); }
double real_of(const Object &o, const char *key) { return std::strtod(text_of(o, key).c_str(), nullptr); }

std::string join(const std::vector<std::string> &items) {
    std::string out;
    for (const std::string &item : items) out += (out.empty() ? "" : "; ") + item;
    return out;
}

std::string csv_field(const std::string &text) {
    if (text.find_first_of(",\"\n\r") == std::string::npos) return text;
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += '"';
        out += c == '\n' || c == '\r' ? ' ' : c;
    }
    return out + "\"";
}

std::string number(double v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", v);
    return text;
}

} // namespace

std::string to_json(const Row &r) {
    std::string out = "{";
    auto str = [&](const char *key, const std::string &v) { out += "\"" + std::string(key) + "\":\"" + escape(v) + "\","; };
    auto num = [&](const char *key, const std::string &v) { out += "\"" + std::string(key) + "\":" + v + ","; };
    auto u = [&](const char *key, uint64_t v) { num(key, std::to_string(v)); };
    str("disc", r.disc), str("title", r.title), str("title_id", r.title_id), str("digest", r.digest);
    str("state", r.state), str("detail", r.detail);
    num("build_seconds", number(r.build_seconds)), num("cached", r.cached ? "true" : "false");
    u("library_bytes", r.library_bytes), num("left_out", std::to_string(r.left_out));
    u("modules", r.modules), u("functions", r.functions);
    u("interpreted_entries", r.interpreted_entries), u("interpreted_addresses", r.interpreted_addresses);
    u("swaps", r.swaps), u("draws", r.draws);
    str("first_unimplemented", r.first_unimplemented), str("first_gpu", r.first_gpu);
    str("unimplemented", r.unimplemented_all), str("gpu", r.gpu_all), str("excerpt", r.excerpt);
    num("boot_seconds", number(r.boot_seconds));
    out += "\"exit_code\":" + std::to_string(r.exit_code) + "}";
    return out;
}

bool from_json(std::string_view line, Row &r) {
    Object o;
    if (!parse_object(line, o) || !o.count("disc")) return false;
    r = Row{};
    r.disc = text_of(o, "disc"), r.title = text_of(o, "title"), r.title_id = text_of(o, "title_id");
    r.digest = text_of(o, "digest"), r.state = text_of(o, "state"), r.detail = text_of(o, "detail");
    r.build_seconds = real_of(o, "build_seconds"), r.cached = text_of(o, "cached") == "true";
    r.library_bytes = number_of(o, "library_bytes");
    r.left_out = std::strtoll(text_of(o, "left_out").c_str(), nullptr, 10);
    r.modules = number_of(o, "modules"), r.functions = number_of(o, "functions");
    r.interpreted_entries = number_of(o, "interpreted_entries");
    r.interpreted_addresses = number_of(o, "interpreted_addresses");
    r.swaps = number_of(o, "swaps"), r.draws = number_of(o, "draws");
    r.first_unimplemented = text_of(o, "first_unimplemented"), r.first_gpu = text_of(o, "first_gpu");
    r.unimplemented_all = text_of(o, "unimplemented"), r.gpu_all = text_of(o, "gpu");
    r.excerpt = text_of(o, "excerpt");
    r.boot_seconds = real_of(o, "boot_seconds");
    r.exit_code = int(std::strtol(text_of(o, "exit_code").c_str(), nullptr, 10));
    return true;
}

std::string csv_header() {
    return "disc,title,title_id,digest,state,detail,build_seconds,cached,library_mb,left_out,modules,functions,"
           "interpreted_entries,interpreted_addresses,swaps,draws,first_unimplemented,first_gpu,boot_seconds,"
           "exit_code,excerpt";
}

std::string to_csv(const Row &r) {
    char mb[32];
    std::snprintf(mb, sizeof(mb), "%.1f", double(r.library_bytes) / (1 << 20));
    std::string out;
    auto add = [&](const std::string &v) { out += (out.empty() ? "" : ",") + v; };
    add(csv_field(r.disc)), add(csv_field(r.title)), add(r.title_id), add(r.digest), add(r.state);
    add(csv_field(r.detail)), add(number(r.build_seconds)), add(r.cached ? "yes" : "no"), add(mb);
    add(r.left_out < 0 ? "" : std::to_string(r.left_out)), add(std::to_string(r.modules));
    add(std::to_string(r.functions)), add(std::to_string(r.interpreted_entries));
    add(std::to_string(r.interpreted_addresses)), add(std::to_string(r.swaps)), add(std::to_string(r.draws));
    add(csv_field(r.first_unimplemented)), add(csv_field(r.first_gpu)), add(number(r.boot_seconds));
    add(std::to_string(r.exit_code)), add(csv_field(r.excerpt));
    return out;
}

BootResult parse_boot_output(std::string_view output, int exit_code, bool timed_out) {
    BootResult result;
    Row &row = result.row;
    row.exit_code = exit_code;
    std::string summary_line, fault, stall;
    std::vector<std::string> stacks; // the stack lines after the fault or stall report
    enum { None, AfterFault, AfterStall } mode = None;
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("xf-summary ")) {
            summary_line = line.substr(11);
        } else if (line.starts_with("fault: ")) {
            fault = line, mode = AfterFault;
        } else if (line.starts_with("stall: ")) {
            if (stall.empty()) stall = line;
            mode = AfterStall;
        } else if (line.find("    stack:") == 0 && mode != None && stacks.size() < 3) {
            stacks.push_back(line.substr(4));
        } else if (!line.empty() && line[0] != ' ') {
            mode = None;
        }
    }
    auto excerpt = [&](const std::string &head) {
        std::string out = head;
        for (const std::string &s : stacks) out += (out.empty() ? "" : " | ") + s;
        return out;
    };
    Object o;
    if (!summary_line.empty() && parse_object(summary_line, o)) {
        result.summary = true;
        row.title_id = text_of(o, "title_id"), row.digest = text_of(o, "digest");
        row.state = text_of(o, "state"), row.detail = text_of(o, "detail");
        row.modules = number_of(o, "modules"), row.functions = number_of(o, "functions");
        row.interpreted_entries = number_of(o, "interpreted_entries");
        row.interpreted_addresses = number_of(o, "interpreted_addresses");
        row.swaps = number_of(o, "swaps"), row.draws = number_of(o, "draws");
        if (auto it = o.find("unimplemented"); it != o.end()) {
            row.unimplemented_all = join(it->second.list);
            if (!it->second.list.empty()) row.first_unimplemented = it->second.list.front();
        }
        if (auto it = o.find("gpu"); it != o.end()) {
            row.gpu_all = join(it->second.list);
            if (!it->second.list.empty()) row.first_gpu = it->second.list.front();
        }
        if (row.state == "stall") row.excerpt = excerpt(stall);
        if (row.state == "crash") row.excerpt = excerpt(fault);
        return result;
    }
    // no summary: the process was killed (a timeout), died, or is not a boot of ours
    if (!fault.empty()) {
        row.state = "crash", row.detail = fault.substr(7), row.excerpt = excerpt(fault);
    } else if (timed_out) {
        row.state = "stall", row.detail = "boot did not end within its hard timeout", row.excerpt = excerpt(stall);
    } else if (exit_code != 0) {
        row.state = "crash", row.detail = "boot exited with code " + std::to_string(exit_code);
        row.excerpt = excerpt(stall);
    } else {
        row.state = "no-summary", row.detail = "boot ended without its summary line";
    }
    return result;
}

Files files_for(const std::filesystem::path &base) {
    Files f;
    f.jsonl = base, f.jsonl += ".jsonl";
    f.csv = base, f.csv += ".csv";
    return f;
}

std::set<std::string> done_discs(const Files &files) {
    std::set<std::string> done;
    std::ifstream in(files.jsonl);
    std::string line;
    Row row;
    while (std::getline(in, line))
        if (from_json(line, row)) done.insert(row.disc);
    return done;
}

bool append_row(const Files &files, const Row &row) {
    std::error_code ec;
    if (files.jsonl.has_parent_path()) std::filesystem::create_directories(files.jsonl.parent_path(), ec);
    const bool new_csv = !std::filesystem::exists(files.csv, ec) || std::filesystem::file_size(files.csv, ec) == 0;
    std::ofstream json(files.jsonl, std::ios::app), csv(files.csv, std::ios::app);
    if (!json || !csv) return false;
    json << to_json(row) << "\n" << std::flush;
    if (new_csv) csv << csv_header() << "\n";
    csv << to_csv(row) << "\n" << std::flush;
    return bool(json) && bool(csv);
}

void clear(const Files &files) {
    std::error_code ec;
    std::filesystem::remove(files.jsonl, ec);
    std::filesystem::remove(files.csv, ec);
}

} // namespace xf::compat
