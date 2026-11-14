/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Game patches (patches.h), translated from Xenia Canary's patcher:
// patcher/patch_db.cc (the file format and the hash matching), patcher/
// patcher.cc (the writes) and UserModule::CalculateHash (the module's hash).
// Where it differs: the AoT split of code and data (patches.h); a patch with
// a wrong field is left out whole with a warning, where Xenia dereferences
// the missing field or keeps the entries before the bad one; u16string is
// written big-endian, as the guest reads wide characters (Xenia copies the
// host's little-endian code units); the toml reader is xf::toml, so a file
// that is not strictly TOML (dates, multi-line strings) is refused.

#include "xenoforge/patches.h"

#include "xenoforge/toml.h"
#include "xenoforge/xxh3.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace xf::patches {
namespace {

std::string hex(uint64_t v, int digits) {
    char b[24];
    std::snprintf(b, sizeof(b), "%0*llX", digits, (unsigned long long)v);
    return b;
}

// string_util::from_string<uint64_t>(text, true): hex digits, "0x" optional
bool parse_hex(std::string_view text, uint64_t &out) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.empty() || text.size() > 16) return false;
    out = 0;
    for (char c : text) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        out = out << 4 | uint64_t(d);
    }
    return true;
}

// string_util::hex_string_to_array: pairs of digits from the left, a last
// single digit read alone ("ABC" is AB 0C)
bool hex_array(std::string_view text, std::vector<uint8_t> &out) {
    if (text.starts_with("0x")) text.remove_prefix(2);
    out.clear();
    for (size_t i = 0; i < text.size();) {
        const size_t n = std::min<size_t>(2, text.size() - i);
        uint64_t v;
        if (!parse_hex(text.substr(i, n), v)) return out.clear(), false;
        out.push_back(uint8_t(v));
        i += n;
    }
    return true;
}

void put_be(std::vector<uint8_t> &out, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) out.push_back(uint8_t(v >> (8 * i)));
}

// UTF-8 to UTF-16 code units, each stored big-endian
bool utf16_be(std::string_view s, std::vector<uint8_t> &out) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp;
        int more;
        if (c < 0x80) cp = c, more = 0;
        else if ((c & 0xE0) == 0xC0) cp = c & 0x1F, more = 1;
        else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, more = 2;
        else if ((c & 0xF8) == 0xF0) cp = c & 0x07, more = 3;
        else return false;
        for (int k = 1; k <= more; k++) {
            if (i + size_t(k) >= s.size() || (uint8_t(s[i + size_t(k)]) & 0xC0) != 0x80) return false;
            cp = cp << 6 | (uint8_t(s[i + size_t(k)]) & 0x3F);
        }
        i += size_t(more) + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put_be(out, 0xD800 + (cp >> 10), 2);
            put_be(out, 0xDC00 + (cp & 0x3FF), 2);
        } else {
            put_be(out, cp, 2);
        }
    }
    return true;
}

const toml::Value *field(const toml::Value &table, const char *key, toml::Value::Kind kind) {
    const toml::Value *v = table.find(key);
    return v && v->kind == kind ? v : nullptr;
}

// One [[patch.<type>]] table's entry; the reason when it is wrong
std::string read_entry(const std::string &type, const toml::Value &t, std::vector<Entry> &into) {
    if (!t.is_table()) return "";
    const toml::Value *address = field(t, "address", toml::Value::Integer);
    if (!address) return type + " entry without an integer address";
    const toml::Value *value = t.find("value");
    if (!value) return type + " entry without a value";
    Entry e;
    e.address = uint32_t(address->integer);
    auto integer = [&](int bytes) {
        if (!value->is_integer()) return false;
        put_be(e.bytes, uint64_t(value->integer), bytes);
        return true;
    };
    auto number = [&](double &d) {
        if (value->kind == toml::Value::Float) d = value->number;
        else if (value->is_integer()) d = double(value->integer);
        else return false;
        return true;
    };
    bool ok = false;
    if (type == "be8") ok = integer(1);
    else if (type == "be16") ok = integer(2);
    else if (type == "be32") ok = integer(4);
    else if (type == "be64") ok = integer(8);
    else if (type == "f32") {
        double d;
        if ((ok = number(d))) {
            const float f = float(d);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            put_be(e.bytes, bits, 4);
        }
    } else if (type == "f64") {
        double d;
        if ((ok = number(d))) {
            uint64_t bits;
            std::memcpy(&bits, &d, 8);
            put_be(e.bytes, bits, 8);
        }
    } else if (type == "string") {
        if ((ok = value->is_string())) e.bytes.assign(value->string.begin(), value->string.end());
    } else if (type == "u16string") {
        ok = value->is_string() && utf16_be(value->string, e.bytes);
    } else if (type == "array") {
        ok = value->is_string() && hex_array(value->string, e.bytes);
    }
    if (!ok) return type + " entry at 0x" + hex(e.address, 8) + " has a value that is not one";
    into.push_back(std::move(e));
    return "";
}

// Xenia's pages: a heap of 64 KB pages below 0x90000000, else 4 KB
uint32_t heap_page_size(uint32_t base) { return base < 0x90000000u ? 0x10000u : 0x1000u; }

} // namespace

bool is_patch_file_name(std::string_view name) {
    constexpr std::string_view tail = ".patch.toml";
    if (name.size() < 8 + tail.size() || !name.ends_with(tail)) return false;
    for (size_t i = 0; i < 8; i++)
        if (!std::isxdigit(uint8_t(name[i]))) return false;
    return true;
}

bool parse(std::string_view text, File &out, std::string &error) {
    toml::Value root;
    if (!toml::parse(text, root, error)) return false;
    const toml::Value *name = field(root, "title_name", toml::Value::String);
    const toml::Value *id = field(root, "title_id", toml::Value::String);
    const toml::Value *hash = root.find("hash");
    if (!name || !id || !hash) return error = "missing title_name, title_id or hash", false;
    uint64_t title;
    if (!parse_hex(id->string, title)) return error = "title_id is not hexadecimal", false;
    out = File{};
    out.title_id = uint32_t(title);
    out.title_name = name->string;
    auto add_hash = [&](const toml::Value &h) {
        uint64_t v;
        if (h.is_string() && !h.string.empty() && parse_hex(h.string, v)) out.hashes.push_back(v);
        else if (h.is_string() && !h.string.empty()) out.warnings.push_back("hash \"" + h.string + "\" is not hexadecimal");
    };
    if (hash->is_array()) for (const toml::Value &h : hash->array) add_hash(h);
    else add_hash(*hash);

    const toml::Value *list = root.find("patch");
    if (!list || !list->is_array()) return true;
    static const char *types[] = {"string", "u16string", "array", "f64", "f32", "be64", "be32", "be16", "be8"};
    for (const toml::Value &p : list->array) {
        if (!p.is_table()) continue;
        Patch patch;
        if (const toml::Value *v = field(p, "name", toml::Value::String)) patch.name = v->string;
        if (const toml::Value *v = field(p, "desc", toml::Value::String)) patch.description = v->string;
        if (const toml::Value *v = field(p, "author", toml::Value::String)) patch.author = v->string;
        if (const toml::Value *v = field(p, "is_enabled", toml::Value::Boolean)) patch.enabled = v->boolean;
        std::string bad;
        for (const char *type : types) {
            const toml::Value *entries = p.find(type);
            if (!entries || !entries->is_array()) continue;
            for (const toml::Value &e : entries->array)
                if (bad.empty()) bad = read_entry(type, e, patch.entries);
        }
        if (!bad.empty()) {
            out.warnings.push_back("patch \"" + patch.name + "\" left out: " + bad);
            continue;
        }
        out.patches.push_back(std::move(patch));
    }
    return true;
}

std::vector<File> load_directory(const std::filesystem::path &directory, std::vector<std::string> *problems) {
    std::vector<File> files;
    std::error_code ec;
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (is_patch_file_name(entry.path().filename().string())) paths.push_back(entry.path());
        else if (problems) problems->push_back(entry.path().filename().string() + ": not a patch file's name (skipped)");
    }
    std::sort(paths.begin(), paths.end());
    for (const std::filesystem::path &path : paths) {
        std::ifstream f(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        File file;
        std::string why;
        if (!f || !parse(text, file, why)) {
            if (problems) problems->push_back(path.filename().string() + ": " + why);
            continue;
        }
        file.path = path.filename().string();
        for (const std::string &w : file.warnings)
            if (problems) problems->push_back(file.path + ": " + w);
        files.push_back(std::move(file));
    }
    return files;
}

std::filesystem::path default_directory() {
#ifdef _WIN32
    if (const char *roaming = std::getenv("APPDATA")) return std::filesystem::path(roaming) / "xenoforge" / "patches";
#else
    if (const char *xdg = std::getenv("XDG_DATA_HOME")) return std::filesystem::path(xdg) / "xenoforge" / "patches";
    if (const char *home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "xenoforge" / "patches";
#endif
    return std::filesystem::path("patches");
}

bool ModuleInfo::is_code(uint32_t address) const {
    for (const auto &[begin, end] : code)
        if (address >= begin && address < end) return true;
    return false;
}

ModuleInfo module_info(const Xex &xex) {
    ModuleInfo m;
    m.title_id = xex.execution.title_id;
    m.base = xex.base_address;
    m.size = uint32_t(xex.image.size());
    // the runs of code pages, as the file's page descriptors lay them out
    const uint32_t page = xex.page_size();
    uint64_t at = 0;
    uint64_t first = UINT64_MAX, last = 0, index = 0;
    for (const XexPages &run : xex.pages) {
        if (run.type == uint8_t(XexSection::Code)) {
            const uint32_t begin = uint32_t(xex.base_address + at * page), end = begin + run.count * page;
            if (!m.code.empty() && m.code.back().second == begin) m.code.back().second = end;
            else m.code.push_back({begin, end});
        }
        at += run.count;
    }
    // Xenia's hash: the first code page to the end of the last, in pages of the module's heap
    for (const XexPages &run : xex.pages) {
        if (run.type == uint8_t(XexSection::Code) && run.count) first = std::min(first, index), last = index + run.count - 1;
        index += run.count;
    }
    if (first != UINT64_MAX) {
        const uint64_t hp = heap_page_size(xex.base_address);
        const uint64_t begin = first * hp, end = std::min<uint64_t>((last + 1) * hp, xex.image.size());
        if (begin < end) m.hash = xxh3_64(xex.image.data() + begin, size_t(end - begin));
    }
    return m;
}

Plan make_plan(const ModuleInfo &module, const std::vector<File> &files, const std::map<std::string, bool> &enabled) {
    Plan plan;
    std::string key;
    for (const File &file : files) {
        if (file.title_id != module.title_id) continue;
        if (std::find(file.hashes.begin(), file.hashes.end(), module.hash) == file.hashes.end()) {
            plan.log.push_back("patches: " + file.path + " refused: it is for another version of the module (hash " +
                               hex(module.hash, 16) + " is not among its hashes)");
            continue;
        }
        for (const Patch &patch : file.patches) {
            auto choice = enabled.find(patch.name);
            if (!(choice != enabled.end() ? choice->second : patch.enabled)) continue;
            size_t code_bytes = 0, data_bytes = 0;
            for (const Entry &e : patch.entries) {
                // cut where the bytes pass from code to data
                for (size_t i = 0; i < e.bytes.size();) {
                    const bool code = module.is_code(e.address + uint32_t(i));
                    size_t j = i;
                    while (j < e.bytes.size() && module.is_code(e.address + uint32_t(j)) == code) j++;
                    Item item;
                    item.file = file.path, item.name = patch.name, item.code = code;
                    item.entry.address = e.address + uint32_t(i);
                    item.entry.bytes.assign(e.bytes.begin() + ptrdiff_t(i), e.bytes.begin() + ptrdiff_t(j));
                    (code ? code_bytes : data_bytes) += j - i;
                    if (code) {
                        key += hex(item.entry.address, 8);
                        for (uint8_t b : item.entry.bytes) key += hex(b, 2);
                        key += ';';
                    }
                    plan.items.push_back(std::move(item));
                    i = j;
                }
            }
            plan.log.push_back("patches: " + file.path + ": \"" + patch.name + "\" enabled: " +
                               std::to_string(code_bytes) + " bytes of code, " + std::to_string(data_bytes) +
                               " of data");
        }
    }
    if (!key.empty()) {
        // lower case, as the cache names digests
        plan.code_key = hex(xxh3_64(key.data(), key.size()), 16);
        for (char &c : plan.code_key) c = char(std::tolower(uint8_t(c)));
    }
    return plan;
}

Plan make_plan(const ModuleInfo &module, const Config &config) {
    if (config.off || !module.valid()) return {};
    const std::filesystem::path dir = config.directory.empty() ? default_directory() : config.directory;
    std::vector<std::string> problems;
    std::vector<File> files = load_directory(dir, &problems);
    Plan plan = make_plan(module, files, config.enabled);
    for (const std::string &p : problems) plan.log.push_back("patches: " + p);
    return plan;
}

std::string cache_key(const std::string &digest, const Plan &plan) {
    return plan.any_code() ? digest + "+" + plan.code_key : digest;
}

bool patched_file(std::span<const uint8_t> file, const Xex &xex, std::vector<uint8_t> &out) {
    if (xex.header_size == 0 || xex.header_size > file.size()) return false;
    uint32_t at = 0;
    for (const auto &[key, value] : xex.opt_headers)
        if (key == xex_key::FILE_FORMAT_INFO) at = value;
    if (!at || uint64_t(at) + 8 > xex.header_size) return false;
    out.assign(file.begin(), file.begin() + xex.header_size);
    out[at + 4] = out[at + 5] = out[at + 6] = out[at + 7] = 0; // no encryption, no compression
    out.insert(out.end(), xex.image.begin(), xex.image.end());
    return true;
}

size_t apply_code(const Plan &plan, std::vector<uint8_t> &image, uint32_t base) {
    size_t changed = 0;
    for (const Item &item : plan.items) {
        if (!item.code) continue;
        const uint64_t offset = uint64_t(item.entry.address) - base;
        if (item.entry.address < base || offset + item.entry.bytes.size() > image.size()) continue;
        std::memcpy(image.data() + offset, item.entry.bytes.data(), item.entry.bytes.size());
        changed += item.entry.bytes.size();
    }
    return changed;
}

std::vector<std::string> apply_data(const Plan &plan, Memory &memory) {
    std::vector<std::string> log;
    for (const Item &item : plan.items) {
        if (item.code) continue;
        const uint32_t address = item.entry.address, size = uint32_t(item.entry.bytes.size());
        Heap *heap = memory.heap_for(address);
        RegionInfo region;
        if (!heap || !heap->query(address, region) || !(region.state & ALLOCATE_COMMIT) ||
            region.allocation_base + region.allocation_size < address + size) {
            log.push_back("patches: \"" + item.name + "\": 0x" + hex(address, 8) + " is not committed memory (left out)");
            continue;
        }
        const uint32_t old = region.protect;
        heap->protect(address, size, PROTECT_READ_WRITE);
        std::memcpy(memory.translate_for_host_write(address), item.entry.bytes.data(), size);
        memory.written(address, size);
        heap->protect(address, size, old);
        log.push_back("patches: \"" + item.name + "\": " + std::to_string(size) + " bytes of data at 0x" +
                      hex(address, 8) + " applied");
    }
    return log;
}

} // namespace xf::patches
