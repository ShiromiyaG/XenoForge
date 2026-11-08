// What the launcher's screens say (launcher_model.h).

#include "launcher_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xf::frontend {

GameStatus game_status(bool cached_library, bool generated_player, bool prepare_failed, bool stale) {
    if (cached_library && stale) return GameStatus::Stale;
    if (cached_library || generated_player) return GameStatus::Ready;
    return prepare_failed ? GameStatus::Interpreted : GameStatus::NeedsPreparing;
}

const char *status_class(GameStatus status) {
    switch (status) {
    case GameStatus::Ready: return "ready";
    case GameStatus::NeedsPreparing: return "prep";
    case GameStatus::Interpreted: return "interp";
    case GameStatus::Stale: return "stale";
    }
    return "";
}

const char *status_text(GameStatus status) {
    switch (status) {
    case GameStatus::Ready: return "Ready";
    case GameStatus::NeedsPreparing: return "Needs preparing";
    case GameStatus::Interpreted: return "Interpreted";
    case GameStatus::Stale: return "Update available";
    }
    return "";
}

const char *status_detail(GameStatus status) {
    switch (status) {
    case GameStatus::Ready: return "Its game library is built.";
    case GameStatus::NeedsPreparing: return "Built once here from your disc, in a few minutes.";
    case GameStatus::Interpreted: return "No library could be built: it runs interpreted.";
    case GameStatus::Stale: return "The game ran code its library lacks: rebuilding takes it in.";
    }
    return "";
}

std::string format_duration(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    const long total = long(seconds + 0.5);
    char text[32];
    if (total >= 3600)
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
    else
        std::snprintf(text, sizeof(text), "%ld:%02ld", total / 60, total % 60);
    return text;
}

std::string version_text(uint32_t version) {
    char text[40];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", version >> 28, (version >> 24) & 15, (version >> 8) & 0xFFFF,
                  version & 0xFF);
    return text;
}

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

uint32_t hue_of(uint32_t title_id) {
    uint32_t h = title_id * 2654435761u;
    h ^= h >> 15;
    return (h % 360);
}

std::string tile_background(uint32_t title_id) {
    const uint32_t hue = hue_of(title_id);
    // two hsl stops as a gradient
    char text[160];
    std::snprintf(text, sizeof(text), "decorator: linear-gradient(145deg, hsl(%u, 62%%, 44%%), hsl(%u, 70%%, 20%%));", hue,
                  (hue + 40) % 360);
    return text;
}

namespace {

bool parse_hex(const std::string &hex, int &r, int &g, int &b) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    unsigned v = 0;
    for (size_t i = 1; i < 7; i++) {
        const char c = hex[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | unsigned(d);
    }
    r = int(v >> 16), g = int(v >> 8 & 255), b = int(v & 255);
    return true;
}

std::string hex_of(int r, int g, int b, int a = -1) {
    char text[16];
    if (a < 0)
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", r, g, b);
    else
        std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", r, g, b, a);
    return text;
}

int mix(int from, int to, double t) { return std::clamp(int(std::lround(from + (to - from) * t)), 0, 255); }

} // namespace

const std::vector<std::string> &accent_choices() {
    static const std::vector<std::string> choices = {"#52b043", "#ff7a45", "#4c9bff", "#9b7bff", "#ff5c8a", "#ffc247"};
    return choices;
}

Accent accent_from_hex(const std::string &hex) {
    int r = 0, g = 0, b = 0;
    if (!parse_hex(hex, r, g, b)) parse_hex(accent_choices()[0], r, g, b);
    Accent a;
    a.base = hex_of(r, g, b);
    a.hi = hex_of(mix(r, 255, 0.28), mix(g, 255, 0.28), mix(b, 255, 0.28));
    a.lo = hex_of(mix(r, 0, 0.32), mix(g, 0, 0.32), mix(b, 0, 0.32));
    a.a08 = hex_of(r, g, b, 0x14);
    a.a12 = hex_of(r, g, b, 0x1f);
    a.a15 = hex_of(r, g, b, 0x26);
    a.a25 = hex_of(r, g, b, 0x40);
    a.a40 = hex_of(r, g, b, 0x66);
    a.a60 = hex_of(r, g, b, 0x99);
    // a dark label on a light accent, a white one on a dark accent
    const double luminance = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
    a.on = luminance > 0.55 ? "#1a0f08" : "#ffffff";
    auto toward = [&](const char *token, int to, double t) {
        a.shades.emplace_back(token, hex_of(mix(r, to, t), mix(g, to, t), mix(b, to, t)));
    };
    toward("{ac-top}", 0, 0.0), toward("{ac-mid}", 0, 0.04), toward("{ac-b1}", 0, 0.07), toward("{ac-b2}", 0, 0.12);
    toward("{ac-low}", 0, 0.20), toward("{ac-end}", 0, 0.26), toward("{ac-ring}", 0, 0.42);
    toward("{ac-lit}", 255, 0.10), toward("{ac-glow}", 255, 0.20), toward("{ac-shine}", 255, 0.45);
    toward("{ac-edge}", 255, 0.65), toward("{ac-pale}", 255, 0.82);
    return a;
}

std::string apply_tokens(std::string text, const Accent &a) {
    for (const auto &[token, value] : a.shades)
        for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
            text.replace(at, token.size(), value);
    const std::pair<const char *, const std::string *> tokens[] = {
        {"{accent-hi}", &a.hi},   {"{accent-lo}", &a.lo}, {"{accent08}", &a.a08}, {"{accent12}", &a.a12},
        {"{accent15}", &a.a15},   {"{accent25}", &a.a25}, {"{accent40}", &a.a40}, {"{accent60}", &a.a60},
        {"{on-accent}", &a.on},   {"{accent}", &a.base}};
    for (const auto &[token, value] : tokens) {
        const size_t length = std::strlen(token);
        for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value->size()))
            text.replace(at, length, *value);
    }
    return text;
}

ThemeChoice theme_from_text(const std::string &text) {
    if (text == "light") return ThemeChoice::Light;
    if (text == "dark") return ThemeChoice::Dark;
    return ThemeChoice::System;
}

const char *theme_text(ThemeChoice choice) {
    switch (choice) {
    case ThemeChoice::Light: return "light";
    case ThemeChoice::Dark: return "dark";
    default: return "system";
    }
}

Palette palette(bool dark, const Accent &accent) {
    int r = 0, g = 0, b = 0;
    parse_hex(accent.base, r, g, b);
    Palette p;
    p.dark = dark;
    // neutral greys in the manner of desktop tools: the window, its chrome
    // (menus, toolbar, status bar), lines, three levels of text, and the
    // states; the selection is the accent, thinner on light
    if (dark)
        p.tokens = {{"{bg}", "#1e1f22"},          {"{bg-alt}", "#222326"},     {"{chrome}", "#2b2d30"},
                    {"{head}", "#26282b"},        {"{border}", "#393b40"},     {"{border-strong}", "#4e5157"},
                    {"{text}", "#dfe1e5"},        {"{text-2}", "#a1a4ab"},     {"{text-3}", "#6f737a"},
                    {"{hover}", "#ffffff10"},     {"{press}", "#ffffff1c"},    {"{button}", "#3a3d42"},
                    {"{button-hover}", "#44474d"}, {"{input}", "#1b1c1f"},     {"{menu}", "#2b2d30"},
                    {"{menu-border}", "#45484e"}, {"{shadow}", "#000000a8"},   {"{good}", "#5fb878"},
                    {"{warn}", "#e0b040"},        {"{bad}", "#e5676c"},        {"{scroll}", "#ffffff2e"},
                    {"{scroll-hover}", "#ffffff4c"}, {"{overlay}", "#00000078"}, {"{dialog}", "#2b2d30"},
                    {"{sel}", hex_of(r, g, b, 0x40)}, {"{sel-strong}", hex_of(r, g, b, 0x5c)}, {"{link}", accent.hi}};
    else
        p.tokens = {{"{bg}", "#ffffff"},          {"{bg-alt}", "#f7f7f8"},     {"{chrome}", "#f3f3f3"},
                    {"{head}", "#f8f8f9"},        {"{border}", "#e3e3e6"},     {"{border-strong}", "#c4c6cb"},
                    {"{text}", "#1f2023"},        {"{text-2}", "#5c5f66"},     {"{text-3}", "#9a9da4"},
                    {"{hover}", "#0000000b"},     {"{press}", "#00000016"},    {"{button}", "#fdfdfd"},
                    {"{button-hover}", "#f0f0f1"}, {"{input}", "#ffffff"},     {"{menu}", "#fbfbfb"},
                    {"{menu-border}", "#d5d6da"}, {"{shadow}", "#0000003a"},   {"{good}", "#2f8f4e"},
                    {"{warn}", "#a86e00"},        {"{bad}", "#c4373d"},        {"{scroll}", "#0000002b"},
                    {"{scroll-hover}", "#00000048"}, {"{overlay}", "#00000060"}, {"{dialog}", "#fbfbfb"},
                    {"{sel}", hex_of(r, g, b, 0x2c)}, {"{sel-strong}", hex_of(r, g, b, 0x48)}, {"{link}", accent.lo}};

    // The console's look (launcher.rcss and settings.rcss: glass over the backdrop, glossy buttons, let-in slots):
    // dark is the original, grey glass under white text over the dark-to-silver gradient; light is pearl glass under
    // graphite text over a #3e4241-to-#919191 one (what sits straight on the backdrop is white in both: the column
    // heads, the group titles, the hints). A value may name the accent's tones ({ac-edge}...), which are applied
    // after these.
    struct Pair {
        const char *token, *dark, *light;
    };
    static const Pair console[] = {
        // text over the backdrop and the glass, from strongest to faintest
        {"{g-text}", "#f6f6f6", "#1d1d20"},        {"{g-text-1}", "#e0dfe4", "#3a3a40"},     {"{g-text-2}", "#c8c8cc", "#55555c"},
        {"{g-text-3}", "#8d8c92", "#85858c"},      {"{g-ic}", "#c4c3c9", "#55555c"},         {"{g-on}", "#ffffff", "#0b0b0d"},
        {"{g-bad}", "#ff8a80", "#c4373d"},         {"{g-link}", "#9fd98a", "#1f7a35"},       {"{bad-ic}", "#ff7a6e", "#d03a36"},
        // text on the selected row and the focused setting (the accent's glossy green, in both themes)
        {"{sel-text}", "#f6f6f6", "#ffffff"},      {"{sel-sub}", "#c8c8cc", "#f0f0f0"},      {"{sel-hint}", "#e8ffe0", "#f4fff0"},
        {"{bset}", "#e8ffe0", "#3a3a40"},
        // the header and the sections
        {"{hd-a}", "#2f2e33b4", "#ffffffb4"},      {"{hd-b}", "#3a393eb4", "#f6f6f8b4"},     {"{hd-c}", "#57565cb4", "#dcdce2b4"},
        {"{hd-text}", "#f4f4f4", "#1d1d20"},       {"{mark-core}", "#2f2e33", "#f6f6f8"},    {"{nav}", "#8b8a90", "#7a7a82"},
        {"{nav-hover}", "#c9c8cd", "#45454c"},     {"{nav-on}", "#ffffff", "#0b0b0d"},
        // the toolbar and the tab bar
        {"{bar-top}", "#a3a2a8", "#ffffff"},       {"{bar-bot}", "#6f6e74", "#b8b8be"},      {"{bar-a}", "#74737ac4", "#ffffffa8"},
        {"{bar-b}", "#908f94c4", "#e8e8ecc4"},
        // the glossy buttons: at rest, hovered, pressed
        {"{btn-border}", "#7c7b79", "#a4a4aa"},    {"{btn-0}", "#62615f", "#ffffff"},        {"{btn-1}", "#504f4e", "#f4f4f6"},
        {"{btn-2}", "#444342", "#e3e3e7"},         {"{btn-3}", "#504f4e", "#ececf0"},        {"{btn-ring}", "#2b2b2a", "#b4b4ba"},
        {"{btn-drop}", "#00000050", "#00000028"},  {"{btn-inset}", "#ffffff26", "#ffffffe0"}, {"{btn-text}", "#f4f4f4", "#1d1d20"},
        {"{btn-h-border}", "#918f8d", "#8a8a92"},  {"{btn-h0}", "#727170", "#ffffff"},       {"{btn-h1}", "#5e5d5c", "#fafafb"},
        {"{btn-h2}", "#504f4e", "#ececf0"},        {"{btn-h3}", "#5c5b5a", "#f3f3f6"},       {"{btn-p0}", "#424140", "#d9d9de"},
        {"{btn-p1}", "#494847", "#e0e0e5"},        {"{btn-p2}", "#4c4b4a", "#e6e6ea"},       {"{btn-p3}", "#565554", "#eeeef2"},
        {"{btn-p-inset}", "#00000090", "#00000040"}, {"{btn-focus}", "#ffffff", "#1d1d20"},  {"{btn-focus-glow}", "#ffffffa0", "#1d1d2060"},
        // the row's Options button, a little lighter than the toolbar's
        {"{opt-border}", "#858484", "#a4a4aa"},    {"{opt-0}", "#747373", "#ffffff"},        {"{opt-1}", "#5d5c5c", "#f4f4f6"},
        {"{opt-2}", "#4c4b4b", "#e3e3e7"},         {"{opt-3}", "#5b5a5a", "#ececf0"},        {"{opt-ring}", "#343435", "#b4b4ba"},
        {"{opt-inset}", "#ffffff30", "#ffffffe0"}, {"{opt-h-border}", "#9a9999", "#8a8a92"}, {"{opt-h0}", "#838282", "#ffffff"},
        {"{opt-h1}", "#6a6969", "#fafafb"},        {"{opt-h2}", "#585757", "#ececf0"},       {"{opt-h3}", "#686767", "#f3f3f6"},
        {"{opt-p0}", "#4a4949", "#d9d9de"},        {"{opt-p1}", "#545353", "#e6e6ea"},       {"{opt-p2}", "#636262", "#eeeef2"},
        {"{opt-p-ring}", "#1f1f20", "#b4b4ba"},
        // the let-in slots (the search box, the dropdowns, the text fields, the check boxes and slider tracks)
        {"{slot-border}", "#3c3c3f", "#bcbcc2"},   {"{slot-border-h}", "#55555a", "#9a9aa2"}, {"{slot-0}", "#161618d8", "#e6e6eae6"},
        {"{slot-1}", "#232325d0", "#f6f6f8e0"},    {"{slot-lip}", "#ffffff38", "#ffffffd0"}, {"{slot-inset}", "#000000a0", "#00000030"},
        {"{slot-f0}", "#1a1c19e0", "#eef3ece8"},   {"{slot-f1}", "#272a25d8", "#fafcf9e0"},  {"{check-0}", "#151517", "#e2e2e6"},
        {"{check-1}", "#282829", "#f6f6f8"},       {"{check-border}", "#8d8c92", "#8d8c94"},
        // the rows (games and settings): glass, hovered; the same opaque, for the empty page's buttons
        {"{row-border}", "#666668", "#c6c6cc"},    {"{row-border-h}", "#77777a", "#aeaeb6"}, {"{row-ring}", "#1c1c1d", "#b4b4ba"},
        {"{row-drop}", "#00000080", "#00000030"},  {"{row-inset}", "#ffffff1a", "#ffffffe0"}, {"{row-shine}", "#ffffff30", "#ffffffa0"},
        {"{row-shine-h}", "#ffffff34", "#ffffffc0"}, {"{row-0}", "#2c2c2eb8", "#ffffffd0"},  {"{row-1}", "#2d2d2fb8", "#fbfbfcd0"},
        {"{row-2}", "#2e2e30b8", "#f4f4f6d0"},     {"{row-3}", "#2f2f31b8", "#eeeef1d0"},    {"{row-h0}", "#343436c0", "#ffffffe6"},
        {"{row-h1}", "#353537c0", "#ffffffe6"},    {"{row-h2}", "#363638c0", "#f8f8fae6"},   {"{row-h3}", "#373739c0", "#f3f3f6e6"},
        {"{row-0o}", "#2c2c2e", "#ffffff"},        {"{row-3o}", "#2f2f31", "#eeeef1"},       {"{row-h0o}", "#343436", "#ffffff"},
        {"{row-h3o}", "#373739", "#f3f3f6"},
        // the status dots, on a row and on the selected one
        {"{dot-ready}", "#8bdc55", "#2e9e44"},     {"{dot-prep}", "#fad145", "#d99a00"},     {"{dot-bad}", "#ff6a5e", "#d6403f"},
        {"{dot-run}", "#ffffff", "#3a3a40"},       {"{sel-dot-ready}", "#8bdc55", "#eaffdc"}, {"{sel-dot-prep}", "#fad145", "#fff0a0"},
        {"{sel-dot-bad}", "#ff6a5e", "#ffd6d0"},   {"{sel-dot-run}", "#ffffff", "#ffffff"},
        // the status bar
        {"{sb-bg}", "#313131", "#e8e8eb"},         {"{sb-top}", "#3e3e3e", "#cacad0"},       {"{sb-text}", "#9f9ea3", "#5f5f66"},
        {"{sb-sep}", "#4a4a4c", "#c4c4ca"},        {"{sb-ok}", "#e8e8ea", "#26262a"},
        // the keys' caps and the hint bar's icons
        {"{key-0}", "#3b3a3b", "#ffffff"},         {"{key-1}", "#232223", "#e6e6ea"},        {"{key-border}", "#777677", "#a8a8b0"},
        {"{key-text}", "#ffffff", "#1d1d20"},      {"{key-mod}", "#c8c8cc", "#6a6a72"},      {"{key-press}", "{ac-edge}", "{accent-lo}"},
        // the modals' content: white under #4f5655 text in the light theme, inverted in the dark one
        {"{dlg-bg}", "#4f5655", "#ffffff"},        {"{dlg-text}", "#ffffff", "#4f5655"},     {"{dlg-text-2}", "#d3d8d7", "#7b8382"},
        {"{dlg-text-3}", "#b3bab9", "#9aa1a0"},    {"{dlg-line}", "#ffffff2e", "#e3e6e6"},   {"{dlg-soft}", "#ffffff18", "#f1f3f3"},
        {"{dlg-border}", "#2f3534", "#b9bfbf"},    {"{dlg-btn}", "#646c6b", "#ffffff"},      {"{dlg-btn-h}", "#6f7877", "#f0f2f2"},
        {"{dlg-btn-border}", "#7f8887", "#c4c9c9"},
        // the accent's swatches
        {"{sw}", "#ffffff50", "#00000040"},        {"{sw-hover}", "#ffffffb0", "#00000090"}, {"{sw-on}", "#ffffff", "#1d1d20"},
        {"{sw-halo}", "#ffffff50", "#00000040"},
    };
    for (const Pair &pair : console) p.tokens.emplace_back(pair.token, dark ? pair.dark : pair.light);
    // under a light accent (yellow) the selection's text is dark, as the accent's own label is; the default
    // green and the other swatches (all below the threshold) keep the light text
    if ((0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0 > 0.65) {
        const std::pair<const char *, const char *> ink[] = {
            {"{sel-text}", "#1a0f08"},      {"{sel-sub}", "#3a2a14"},       {"{sel-hint}", "#2a1c0c"},
            {"{sel-dot-ready}", "#1d6b2a"}, {"{sel-dot-prep}", "#7a5200"},  {"{sel-dot-bad}", "#8a1c1c"},
            {"{sel-dot-run}", "#1a0f08"}};
        for (auto &[token, value] : p.tokens)
            for (const auto &[name, ink_value] : ink)
                if (token == name) value = ink_value;
    }
    return p;
}

std::string apply_tokens(std::string text, const Accent &accent, const Palette &palette) {
    for (const auto &[token, value] : palette.tokens)
        for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
            text.replace(at, token.size(), value);
    return apply_tokens(std::move(text), accent);
}

namespace {

const ColumnInfo COLUMN_INFOS[COLUMNS] = {
    {Column::Title, "title", "Game"},
    {Column::Status, "status", "Status"},
    {Column::TitleId, "id", "Title ID"},
    {Column::Version, "version", "Version"},
};

std::string lower(std::string_view text) {
    std::string s(text);
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

} // namespace

const ColumnInfo &column_info(Column column) { return COLUMN_INFOS[std::clamp(int(column), 0, COLUMNS - 1)]; }

void parse_sort(const std::string &text, Column &column, bool &ascending) {
    column = Column::Title, ascending = true;
    std::string name = text;
    if (!name.empty() && name[0] == '-') ascending = false, name.erase(0, 1);
    for (int i = 0; i < COLUMNS; i++)
        if (name == COLUMN_INFOS[i].name) return void(column = COLUMN_INFOS[i].id);
    ascending = true; // an unknown name: the default as a whole
}

std::string sort_text(Column column, bool ascending) { return std::string(ascending ? "" : "-") + column_info(column).name; }

std::vector<size_t> sorted_rows(const std::vector<RowKey> &rows, Column by, bool ascending) {
    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    auto compare = [&](const RowKey &a, const RowKey &b) -> int {
        auto text = [](const std::string &x, const std::string &y) {
            const std::string lx = lower(x), ly = lower(y);
            return lx < ly ? -1 : lx > ly ? 1 : 0;
        };
        switch (by) {
        case Column::TitleId: return text(a.id_text, b.id_text);
        case Column::Version: return a.version < b.version ? -1 : a.version > b.version ? 1 : 0;
        case Column::Status: return a.status < b.status ? -1 : a.status > b.status ? 1 : 0;
        default: return text(a.title, b.title);
        }
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        int c = compare(rows[x], rows[y]);
        if (!ascending) c = -c;
        if (c == 0 && by != Column::Title) {
            const std::string tx = lower(rows[x].title), ty = lower(rows[y].title);
            c = tx < ty ? -1 : tx > ty ? 1 : 0;
        }
        return c < 0;
    });
    return order;
}

bool row_matches(const RowKey &row, std::string_view filter) {
    const std::string haystack = lower(row.title) + " " + lower(row.id_text) + " " + lower(row.file);
    const std::string words = lower(filter);
    size_t at = 0;
    while (at < words.size()) {
        if (words[at] == ' ') {
            at++;
            continue;
        }
        const size_t end = std::min(words.find(' ', at), words.size());
        if (haystack.find(words.substr(at, end - at)) == std::string::npos) return false;
        at = end;
    }
    return true;
}

std::string key_label(const std::string &name) {
    static const std::pair<const char *, const char *> names[] = {
        {"Return", "Enter"}, {"Left", "\xE2\x86\x90"}, {"Right", "\xE2\x86\x92"}, {"Up", "\xE2\x86\x91"},
        {"Down", "\xE2\x86\x93"}, {"Escape", "Esc"}, {"Backspace", "Backspace"}, {"Space", "Space"}};
    for (const auto &[from, to] : names)
        if (name == from) return to;
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return std::string(1, char(name[0] - 32));
    return name;
}

std::vector<KeyCap> parse_keys(const std::string &keys) {
    std::vector<KeyCap> caps;
    size_t at = 0;
    while (at < keys.size()) {
        const size_t end = keys.find(' ', at);
        std::string token = keys.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? keys.size() : end + 1;
        if (token.empty()) continue;
        KeyCap cap;
        if (token[0] == '^') cap.shift = true, token.erase(0, 1);
        else if (token[0] == '_') cap.no_shift = true, token.erase(0, 1);
        cap.label = key_label(token);
        caps.push_back(std::move(cap));
    }
    return caps;
}

BindingInfo binding_info(const std::string &name) {
    static const struct {
        const char *name, *label, *group, *part;
    } table[] = {
        {"a", "A button", "Face buttons", "a"},
        {"b", "B button", "Face buttons", "b"},
        {"x", "X button", "Face buttons", "x"},
        {"y", "Y button", "Face buttons", "y"},
        {"left_shoulder", "Left bumper", "Bumpers and triggers", "lb"},
        {"right_shoulder", "Right bumper", "Bumpers and triggers", "rb"},
        {"left_trigger", "Left trigger", "Bumpers and triggers", "lt"},
        {"right_trigger", "Right trigger", "Bumpers and triggers", "rt"},
        {"dpad_up", "D-pad up", "D-pad", "dpad-up"},
        {"dpad_down", "D-pad down", "D-pad", "dpad-down"},
        {"dpad_left", "D-pad left", "D-pad", "dpad-left"},
        {"dpad_right", "D-pad right", "D-pad", "dpad-right"},
        {"left_thumb_up", "Up", "Left stick", "ls"},
        {"left_thumb_down", "Down", "Left stick", "ls"},
        {"left_thumb_left", "Left", "Left stick", "ls"},
        {"left_thumb_right", "Right", "Left stick", "ls"},
        {"left_thumb", "Press", "Left stick", "ls"},
        {"right_thumb_up", "Up", "Right stick", "rs"},
        {"right_thumb_down", "Down", "Right stick", "rs"},
        {"right_thumb_left", "Left", "Right stick", "rs"},
        {"right_thumb_right", "Right", "Right stick", "rs"},
        {"right_thumb", "Press", "Right stick", "rs"},
        {"back", "Back", "System", "back"},
        {"start", "Start", "System", "start"},
        {"guide", "Guide", "System", "guide"},
    };
    for (const auto &e : table)
        if (name == e.name) return {e.label, e.group, e.part};
    return {"?", "System", ""};
}

int nav_pick(const std::vector<NavRect> &rects, int from, NavDir dir) {
    if (from < 0 || from >= int(rects.size())) return -1;
    const NavRect &f = rects[size_t(from)];
    const float fcx = f.x + f.w / 2, fcy = f.y + f.h / 2;
    const bool vertical = dir == NavDir::Up || dir == NavDir::Down;
    int best = -1;
    float best_score = 0;
    for (int i = 0; i < int(rects.size()); i++) {
        if (i == from) continue;
        const NavRect &c = rects[size_t(i)];
        const float ccx = c.x + c.w / 2, ccy = c.y + c.h / 2;
        // the candidate's centre must lie on the direction's side of ours
        const float along = vertical ? (dir == NavDir::Down ? ccy - fcy : fcy - ccy) : (dir == NavDir::Right ? ccx - fcx : fcx - ccx);
        if (along <= 1) continue;
        // the gap between the edges along the direction (0 when they overlap there)
        float gap;
        if (dir == NavDir::Down) gap = c.y - (f.y + f.h);
        else if (dir == NavDir::Up) gap = f.y - (c.y + c.h);
        else if (dir == NavDir::Right) gap = c.x - (f.x + f.w);
        else gap = f.x - (c.x + c.w);
        gap = std::max(gap, 0.f);
        // how far the two are from overlapping across the direction
        float off, centre_off;
        if (vertical) {
            off = std::max({c.x - (f.x + f.w), f.x - (c.x + c.w), 0.f});
            centre_off = std::fabs(ccx - fcx);
        } else {
            off = std::max({c.y - (f.y + f.h), f.y - (c.y + c.h), 0.f});
            centre_off = std::fabs(ccy - fcy);
        }
        const float score = gap + along * 0.35f + off * 3.f + centre_off * 0.6f;
        if (best < 0 || score < best_score) best = i, best_score = score;
    }
    return best;
}

} // namespace xf::frontend
