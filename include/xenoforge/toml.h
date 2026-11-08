// A small TOML reader for the recompiler's project file (docs/recompiler.md,
// "The project file"), so xfrecomp and the core need no TOML library (the
// player's settings use toml++). It reads the subset project files use:
// comments; [tables], [[arrays of tables]] and dotted or quoted keys
// ("0x82012340", modules."default.xex"); strings (basic, with escapes, and
// literal); integers (decimal, 0x, 0o, 0b, with underscores); floats;
// booleans; arrays and inline tables, over several lines where TOML allows.
// Not read: multi-line strings, dates and times (an error, with the line).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xf::toml {

struct Value {
    enum Kind : uint8_t { Table, Array, String, Integer, Float, Boolean } kind = Table;
    std::string string;
    int64_t integer = 0;
    double number = 0;
    bool boolean = false;
    std::vector<Value> array;
    std::map<std::string, Value> table;
    int line = 0; // where it was written (1-based; 0: made by the reader, a table a header implied)
    // a header ([a.b]) or an inline table, which no later header or key may
    // extend; an array an [[array of tables]] made
    bool closed = false, of_tables = false;

    bool is_table() const { return kind == Table; }
    bool is_array() const { return kind == Array; }
    bool is_string() const { return kind == String; }
    bool is_integer() const { return kind == Integer; }
    bool is_bool() const { return kind == Boolean; }
    // a key of a table (nullptr: none, or this is not a table)
    const Value *find(const std::string &key) const;
};

// The document's root table; false, and why (with the line), when the text is
// not TOML this reader takes.
bool parse(std::string_view text, Value &root, std::string &error);

} // namespace xf::toml
