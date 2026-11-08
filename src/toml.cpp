// The project file's TOML reader (toml.h): one pass over the text, a value
// at a time, the tables built as their headers and keys come.

#include "xenoforge/toml.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace xf::toml {
namespace {

class Reader {
  public:
    Reader(std::string_view text, Value &root) : text_(text), root_(root) {}

    bool run(std::string &error) {
        root_ = Value{};
        current_ = &root_;
        while (pos_ < text_.size()) {
            skip_blank();
            if (at_end_of_line()) {
                if (!end_of_line()) return fail(error);
                continue;
            }
            if (peek() == '[') {
                if (!header()) return fail(error);
            } else if (!key_value(*current_)) {
                return fail(error);
            }
            skip_blank();
            if (!end_of_line()) return fail(error);
        }
        return true;
    }

  private:
    char peek(size_t ahead = 0) const { return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0'; }
    bool fail(std::string &error) const {
        error = "line " + std::to_string(error_line_ ? error_line_ : line_) + ": " + why_;
        return false;
    }
    bool bad(const std::string &why) {
        if (why_.empty()) why_ = why, error_line_ = line_;
        return false;
    }

    void skip_blank() {
        while (peek() == ' ' || peek() == '\t') pos_++;
    }
    bool at_end_of_line() const { return pos_ >= text_.size() || peek() == '\n' || peek() == '\r' || peek() == '#'; }
    // a comment, then the line's end (or the text's)
    bool end_of_line() {
        if (peek() == '#')
            while (pos_ < text_.size() && peek() != '\n') pos_++;
        if (pos_ >= text_.size()) return true;
        if (peek() == '\r' && peek(1) == '\n') pos_++;
        if (peek() != '\n') return bad("expected the end of the line");
        pos_++, line_++;
        return true;
    }
    // blanks, comments and line ends (inside arrays and inline tables)
    void skip_space() {
        for (;;) {
            skip_blank();
            if (peek() == '#')
                while (pos_ < text_.size() && peek() != '\n') pos_++;
            if (peek() == '\r' && peek(1) == '\n') pos_++;
            if (peek() == '\n') {
                pos_++, line_++;
                continue;
            }
            return;
        }
    }

    static bool bare(char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
               ch == '-';
    }

    bool key(std::vector<std::string> &parts) {
        parts.clear();
        for (;;) {
            skip_blank();
            std::string part;
            if (peek() == '"' || peek() == '\'') {
                Value v;
                if (!string(v)) return false;
                part = v.string;
            } else {
                while (bare(peek())) part += text_[pos_++];
                if (part.empty()) return bad("expected a key");
            }
            parts.push_back(std::move(part));
            skip_blank();
            if (peek() != '.') return true;
            pos_++;
        }
    }

    // the table a dotted key's parts lead to from `from`, made where missing
    // (an [[array of tables]] leads to its last table)
    Value *descend(Value *from, const std::string &part, bool through_closed) {
        auto it = from->table.find(part);
        if (it == from->table.end()) {
            Value t;
            t.line = line_;
            return &from->table.emplace(part, std::move(t)).first->second;
        }
        Value *v = &it->second;
        if (v->kind == Value::Array && v->of_tables && !v->array.empty()) return &v->array.back();
        if (v->kind != Value::Table) return bad("\"" + part + "\" is not a table"), nullptr;
        if (v->closed && !through_closed) return bad("\"" + part + "\" is an inline table, closed"), nullptr;
        return v;
    }

    bool header() {
        bool array = peek(1) == '[';
        pos_ += array ? 2 : 1;
        std::vector<std::string> parts;
        if (!key(parts)) return false;
        skip_blank();
        if (peek() != ']' || (array && peek(1) != ']')) return bad("expected ']' after the table's name");
        pos_ += array ? 2 : 1;
        Value *t = &root_;
        for (size_t n = 0; n + 1 < parts.size(); n++)
            if (!(t = descend(t, parts[n], false))) return false;
        const std::string &last = parts.back();
        if (!array) {
            if (!(t = descend(t, last, false))) return false;
            current_ = t;
            return true;
        }
        auto it = t->table.find(last);
        if (it == t->table.end()) {
            Value a;
            a.kind = Value::Array, a.of_tables = true, a.line = line_;
            it = t->table.emplace(last, std::move(a)).first;
        } else if (it->second.kind != Value::Array || !it->second.of_tables) {
            return bad("\"" + last + "\" is not an array of tables");
        }
        Value element;
        element.line = line_;
        it->second.array.push_back(std::move(element));
        current_ = &it->second.array.back();
        return true;
    }

    bool key_value(Value &into) {
        std::vector<std::string> parts;
        if (!key(parts)) return false;
        skip_blank();
        if (peek() != '=') return bad("expected '=' after the key");
        pos_++;
        skip_blank();
        Value v;
        if (!value(v)) return false;
        Value *t = &into;
        for (size_t n = 0; n + 1 < parts.size(); n++)
            if (!(t = descend(t, parts[n], false))) return false;
        if (!t->table.emplace(parts.back(), std::move(v)).second) return bad("\"" + parts.back() + "\" is given twice");
        return true;
    }

    static void utf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
        }
    }

    bool string(Value &v) {
        char quote = peek();
        if (peek(1) == quote && peek(2) == quote) return bad("multi-line strings are not read");
        pos_++;
        v.kind = Value::String, v.line = line_;
        for (;;) {
            if (pos_ >= text_.size() || peek() == '\n') return bad("a string not closed on its line");
            char ch = text_[pos_++];
            if (ch == quote) return true;
            if (ch != '\\' || quote == '\'') {
                v.string += ch;
                continue;
            }
            char e = text_[pos_++];
            switch (e) {
            case 'b': v.string += '\b'; break;
            case 't': v.string += '\t'; break;
            case 'n': v.string += '\n'; break;
            case 'f': v.string += '\f'; break;
            case 'r': v.string += '\r'; break;
            case '"': v.string += '"'; break;
            case '\\': v.string += '\\'; break;
            case 'u':
            case 'U': {
                size_t digits = e == 'u' ? 4 : 8;
                if (pos_ + digits > text_.size()) return bad("a short \\u escape");
                std::string hex(text_.substr(pos_, digits));
                char *end = nullptr;
                uint32_t cp = uint32_t(std::strtoul(hex.c_str(), &end, 16));
                if (end != hex.c_str() + digits) return bad("a bad \\u escape");
                pos_ += digits;
                utf8(v.string, cp);
                break;
            }
            default: return bad(std::string("an unknown escape \\") + e);
            }
        }
    }

    bool number(Value &v) {
        v.line = line_;
        size_t start = pos_;
        std::string digits;
        bool negative = false;
        if (peek() == '+' || peek() == '-') negative = text_[pos_++] == '-';
        int base = 10;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'o' || peek(1) == 'b')) {
            base = peek(1) == 'x' ? 16 : peek(1) == 'o' ? 8 : 2;
            pos_ += 2;
        }
        bool real = false;
        while (pos_ < text_.size()) {
            char ch = peek();
            if (ch == '_') {
                pos_++;
                continue;
            }
            bool digit = base == 16 ? std::strchr("0123456789abcdefABCDEF", ch) != nullptr : ch >= '0' && ch <= '9';
            if (base == 10 &&
                (ch == '.' || ch == 'e' || ch == 'E' ||
                 ((ch == '+' || ch == '-') && !digits.empty() && (digits.back() == 'e' || digits.back() == 'E')))) {
                real = true;
                digit = true;
            }
            if (!digit || ch == '\0') break;
            digits += ch;
            pos_++;
        }
        if (digits.empty()) {
            // inf and nan
            std::string_view rest = text_.substr(pos_);
            if (rest.starts_with("inf") || rest.starts_with("nan")) {
                pos_ += 3;
                v.kind = Value::Float;
                v.number = rest[0] == 'i' ? (negative ? -std::numeric_limits<double>::infinity()
                                                      : std::numeric_limits<double>::infinity())
                                          : std::numeric_limits<double>::quiet_NaN();
                return true;
            }
            pos_ = start;
            return bad("expected a value");
        }
        if (bare(peek()) || peek() == ':') return bad("dates, times and other values are not read");
        char *end = nullptr;
        if (real) {
            v.kind = Value::Float;
            v.number = std::strtod(digits.c_str(), &end);
            if (negative) v.number = -v.number;
        } else {
            v.kind = Value::Integer;
            uint64_t u = std::strtoull(digits.c_str(), &end, base);
            v.integer = negative ? -int64_t(u) : int64_t(u);
        }
        if (end != digits.c_str() + digits.size()) return bad("a malformed number");
        return true;
    }

    bool value(Value &v) {
        char ch = peek();
        if (ch == '"' || ch == '\'') return string(v);
        if (ch == '[') {
            pos_++;
            v.kind = Value::Array, v.line = line_;
            for (;;) {
                skip_space();
                if (peek() == ']') {
                    pos_++;
                    return true;
                }
                Value element;
                if (!value(element)) return false;
                v.array.push_back(std::move(element));
                skip_space();
                if (peek() == ',') {
                    pos_++;
                    continue;
                }
                if (peek() != ']') return bad("expected ',' or ']' in an array");
            }
        }
        if (ch == '{') {
            pos_++;
            v.kind = Value::Table, v.line = line_;
            skip_space();
            if (peek() == '}') {
                pos_++;
                v.closed = true;
                return true;
            }
            for (;;) {
                skip_space();
                if (!key_value(v)) return false;
                skip_space();
                if (peek() == ',') {
                    pos_++;
                    continue;
                }
                if (peek() != '}') return bad("expected ',' or '}' in an inline table");
                pos_++;
                v.closed = true;
                return true;
            }
        }
        std::string_view rest = text_.substr(pos_);
        auto word = [&](std::string_view w) {
            return rest.starts_with(w) && !bare(rest.size() > w.size() ? rest[w.size()] : ' ');
        };
        if (word("true") || word("false")) {
            v.kind = Value::Boolean, v.boolean = rest[0] == 't', v.line = line_;
            pos_ += v.boolean ? 4 : 5;
            return true;
        }
        return number(v);
    }

    std::string_view text_;
    Value &root_;
    Value *current_ = nullptr;
    size_t pos_ = 0;
    int line_ = 1, error_line_ = 0;
    std::string why_;
};

} // namespace

const Value *Value::find(const std::string &key) const {
    if (kind != Table) return nullptr;
    auto it = table.find(key);
    return it == table.end() ? nullptr : &it->second;
}

bool parse(std::string_view text, Value &root, std::string &error) { return Reader(text, root).run(error); }

} // namespace xf::toml
