// The launcher's windows as RML (launcher_html.h).

#include "launcher_html.h"

#include "console_options.h"
#include "system_ui.h"

#include <cstdio>
#include <cstring>

namespace xf::frontend {

namespace {

std::string e(std::string_view text) { return html_escape(text); }

std::string utf8_of(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6)), s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xE0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
    }
    return s;
}

std::string initial_of(const std::string &title) {
    // the first letter or digit, as UTF-8 when it is ASCII (else a plain mark)
    for (char c : title)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return std::string(1, c >= 'a' && c <= 'z' ? char(c - 32) : c);
    return "?";
}

std::string dp(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0fdp", value);
    return text;
}

std::string column_style(Column c, const Widths &w) {
    // the title takes what the others leave, never less than its width
    if (c == Column::Title) return "flex: 1 1 0; min-width: " + dp(w[size_t(c)]) + ";";
    return "flex: 0 0 auto; width: " + dp(w[size_t(c)]) + ";";
}

const char *column_class(Column c) {
    switch (c) {
    case Column::Icon: return "c-icon";
    case Column::Title: return "c-title";
    case Column::TitleId: return "c-id";
    case Column::Version: return "c-ver";
    case Column::Status: return "c-status";
    case Column::RunsIn: return "c-runs";
    case Column::File: return "c-file";
    }
    return "";
}

// the settings' rows: a check box, a slider, a dropdown
std::string check_row(const char *key, const char *name, const std::string &hint, bool on) {
    std::string s = "<div class=\"setting check-row f\" data-kind=\"toggle\" data-action=\"toggle\" data-key=\"";
    s += key;
    s += std::string("\"><div class=\"check") + (on ? " on" : "") + "\">" + icon_html("check") + "</div><div class=\"label\"><div class=\"name\">";
    s += e(name);
    s += "</div>";
    if (!hint.empty()) s += "<div class=\"hint\">" + e(hint) + "</div>";
    s += "</div></div>";
    return s;
}

std::string slider_row(const char *key, const char *name, const std::string &hint, double fraction, const std::string &text) {
    char pos[32];
    std::snprintf(pos, sizeof(pos), "%.2f%%", fraction * 100);
    std::string s = "<div class=\"setting f\" data-kind=\"slider\" data-key=\"";
    s += key;
    s += "\"><div class=\"label\"><div class=\"name\">" + e(name) + "</div>";
    if (!hint.empty()) s += "<div class=\"hint\">" + e(hint) + "</div>";
    s += "</div><div class=\"control\"><div class=\"slider\"><div class=\"track\"></div><div class=\"fill\" style=\"width: ";
    s += pos;
    s += ";\"></div><div class=\"thumb\" style=\"left: ";
    s += pos;
    s += ";\"></div></div><div class=\"value\">" + e(text) + "</div></div></div>";
    return s;
}

std::string select_row(const char *key, const char *name, const std::string &hint, const std::string &current) {
    std::string s = "<div class=\"setting f\" data-kind=\"select\" data-action=\"select\" data-key=\"";
    s += key;
    s += "\"><div class=\"label\"><div class=\"name\">" + e(name) + "</div>";
    if (!hint.empty()) s += "<div class=\"hint\">" + e(hint) + "</div>";
    s += "</div><div class=\"control\"><div class=\"select\"><div class=\"value-text nowrap\">" + e(current) + "</div>" +
         icon_html("chevron-down", "ic chev") + "</div></div></div>";
    return s;
}

std::string group(const std::string &title, const std::string &rows, const std::string &note = {}) {
    std::string s = "<div class=\"group\"><div class=\"group-title\">" + e(title) + "</div>" + rows;
    if (!note.empty()) s += "<div class=\"group-note\">" + e(note) + "</div>";
    return s + "</div>";
}

std::string utf16_to_utf8(const std::u16string &text) { return xf::frontend::utf8_of(text); }

// the gamertag of the profile the games use
std::string selected_gamertag_of(const SettingsPage &page) {
    for (const ProfileEntry &p : page.profiles)
        if (p.selected && !p.name.empty()) return p.name;
    return page.settings->profile.gamertag;
}

std::string label_of(const std::vector<Choice> &choices, const std::string &value) {
    for (const Choice &c : choices)
        if (c.value == value) return c.label;
    return choices.empty() ? std::string() : choices[0].label;
}

} // namespace

std::string icon_glyph(std::string_view name) {
    // Lucide 0.460.0's codepoints (lucide-static, font/lucide.css)
    static const std::pair<const char *, uint32_t> icons[] = {
        {"play", 0xE140},          {"hammer", 0xE0EF},        {"folder-open", 0xE247},    {"folder", 0xE0DC},
        {"refresh-cw", 0xE149},    {"settings", 0xE158},      {"gamepad-2", 0xE0E2},      {"info", 0xE0FE},
        {"search", 0xE155},        {"square", 0xE16B},        {"circle-stop", 0xE087},    {"x", 0xE1B2},
        {"check", 0xE070},         {"chevron-down", 0xE071},  {"chevron-up", 0xE074},     {"arrow-up", 0xE04E},
        {"arrow-down", 0xE046},    {"copy", 0xE0A2},          {"file-text", 0xE0D0},      {"log-out", 0xE112},
        {"monitor", 0xE121},       {"sun", 0xE17C},           {"moon", 0xE122},           {"keyboard", 0xE284},
        {"volume-2", 0xE1AB},      {"package", 0xE12D},       {"triangle-alert", 0xE193}, {"circle-check", 0xE226},
        {"circle-alert", 0xE07B},  {"loader-circle", 0xE10E}, {"hard-drive", 0xE0F0},     {"layers", 0xE104},
        {"sliders-horizontal", 0xE29A}, {"cpu", 0xE0AD}};
    for (const auto &[n, cp] : icons)
        if (name == n) return utf8_of(cp);
    return {};
}

std::string icon_html(std::string_view name, const char *classes) {
    return std::string("<span class=\"") + classes + "\">" + icon_glyph(name) + "</span>";
}

std::string expand_icons(std::string text) {
    for (size_t at = text.find("{i:"); at != std::string::npos; at = text.find("{i:", at)) {
        const size_t end = text.find('}', at);
        if (end == std::string::npos) break;
        const std::string html = icon_html(std::string_view(text).substr(at + 3, end - at - 3));
        text.replace(at, end + 1 - at, html);
        at += html.size();
    }
    return text;
}

std::string art_style(const std::string &icon, uint32_t title_id) {
    if (!icon.empty()) return "decorator: image(mem:" + icon + " cover);";
    return tile_background(title_id);
}

std::string art_letter(const std::string &icon, const std::string &title) {
    if (!icon.empty()) return {};
    return "<div class=\"letter\">" + e(initial_of(title)) + "</div>";
}

// ---------------------------------------------------------------------------
// the main window

MenuItem menu_separator() {
    MenuItem m;
    m.separator = true;
    return m;
}

std::string menu_html(const std::vector<MenuItem> &items) {
    std::string s = "<div class=\"menu\" id=\"menu\">";
    for (const MenuItem &m : items) {
        if (m.separator) {
            s += "<div class=\"msep\"></div>";
            continue;
        }
        s += std::string("<div class=\"mi") + (m.enabled ? "" : " off") + "\" data-mi=\"1\"";
        if (m.enabled) s += " data-action=\"" + e(m.action) + "\" data-arg=\"" + e(m.arg) + "\"";
        s += "><span class=\"mi-ic\">" + (m.checked ? icon_glyph("check") : icon_glyph(m.icon)) + "</span><div class=\"mi-l\">" + e(m.label) +
             "</div><div class=\"mi-k\">" + e(m.shortcut) + "</div></div>";
    }
    return s + "</div>";
}

std::string table_head_html(const Widths &widths, Column sort, bool ascending) {
    std::string s;
    for (int i = 0; i < COLUMNS; i++) {
        const Column c = Column(i);
        const ColumnInfo &info = column_info(c);
        s += std::string("<div class=\"th ") + column_class(c) + (c == sort ? " sorted" : "") + "\" style=\"" + column_style(c, widths) + "\"";
        if (c != Column::Icon) s += " data-action=\"sort\" data-arg=\"" + std::string(info.name) + "\"";
        s += "><div class=\"th-l nowrap\">" + e(info.label) + "</div>";
        if (c == sort) s += icon_html(ascending ? "chevron-up" : "chevron-down", "ic arrow");
        if (c != Column::Icon) s += "<div class=\"grip\" data-grip=\"" + std::to_string(i) + "\"></div>";
        s += "</div>";
    }
    return s;
}

std::string table_row_html(const RowInfo &r, const Widths &w, bool selected) {
    std::string s = "<div class=\"tr f";
    if (r.alternate) s += " alt";
    if (selected) s += " sel";
    if (r.unreadable) s += " broken";
    s += "\" data-index=\"" + std::to_string(r.index) + "\" id=\"row-" + std::to_string(r.index) + "\">";
    auto cell = [&](Column c, const std::string &inner) {
        s += std::string("<div class=\"td ") + column_class(c) + "\" style=\"" + column_style(c, w) + "\">" + inner + "</div>";
    };
    if (r.unreadable) {
        cell(Column::Icon, "<div class=\"thumb broken\">" + icon_html("triangle-alert") + "</div>");
        cell(Column::Title, "<div class=\"nowrap\">" + e(r.title) + "</div>");
        cell(Column::TitleId, "");
        cell(Column::Version, "");
        cell(Column::Status, "<div class=\"status\"><div class=\"dot bad\"></div><div class=\"nowrap\">Cannot be read</div></div>");
        cell(Column::RunsIn, "");
        cell(Column::File, "<div class=\"nowrap\">" + e(r.file) + "</div>");
    } else {
        cell(Column::Icon, "<div class=\"thumb\" style=\"" + art_style(r.icon, r.title_id) + "\">" + art_letter(r.icon, r.title) + "</div>");
        cell(Column::Title, "<div class=\"nowrap\">" + e(r.title) + "</div>");
        cell(Column::TitleId, e(r.id_text));
        cell(Column::Version, e(r.version));
        const std::string status = r.running ? std::string("<div class=\"status\"><div class=\"dot run\"></div><div class=\"nowrap\">Running</div></div>")
                                             : std::string("<div class=\"status\"><div class=\"dot ") + status_class(r.status) + "\"></div><div class=\"nowrap\">" +
                                                   status_text(r.status) + "</div></div>";
        cell(Column::Status, status);
        cell(Column::RunsIn, "<div class=\"nowrap\">" + e(r.runs_in) + "</div>");
        cell(Column::File, "<div class=\"nowrap\">" + e(r.file) + "</div>");
    }
    return s + "</div>";
}

std::string empty_html(bool no_folder, bool filtered, const std::string &folder) {
    std::string s = "<div class=\"empty\">";
    if (filtered) {
        s += icon_html("search", "big-ic") + "<div class=\"empty-title\">No game matches the search</div>";
        s += "<div class=\"empty-text\">The title, the title ID and the disc's file name are searched.</div>";
        s += "<div class=\"empty-buttons\">" + button_html("clear-search", "Clear the search") + "</div>";
    } else if (no_folder) {
        s += icon_html("folder-open", "big-ic") + "<div class=\"empty-title\">Choose your games folder</div>";
        s += "<div class=\"empty-text\">XenoForge reads your discs where they are: nothing is copied, and nothing leaves this computer. "
             "Disc images (.iso) and extracted games (default.xex) are found in the folder and one level below it.</div>";
        s += "<div class=\"empty-buttons\">" + button_html("browse", "Choose folder...", true, "empty-primary", "folder-open") + "</div>";
    } else {
        s += icon_html("folder", "big-ic") + "<div class=\"empty-title\">No games found</div>";
        s += "<div class=\"empty-text\">There are no disc images or default.xex files in <b>" + e(folder) +
             "</b> or in the folders directly inside it.</div>";
        s += "<div class=\"empty-buttons\">" + button_html("browse", "Choose another folder...", true, "empty-primary", "folder-open") +
             button_html("rescan", "Scan again", false, {}, "refresh-cw") + "</div>";
    }
    return s + "</div>";
}

std::string button_html(const std::string &action, const std::string &label, bool primary, const std::string &id, const std::string &icon) {
    std::string s = std::string("<div class=\"btn f") + (primary ? " primary" : "") + "\" data-action=\"" + e(action) + "\"";
    if (!id.empty()) s += " id=\"" + e(id) + "\"";
    s += ">";
    if (!icon.empty()) s += icon_html(icon);
    return s + "<div class=\"btn-l\">" + e(label) + "</div></div>";
}

std::string dialog_html(const std::string &title, const std::string &body, const std::string &buttons, int width_dp) {
    return "<div class=\"dialog\" id=\"dialog\" style=\"width: " + std::to_string(width_dp) + "dp;\"><div class=\"dlg-title\"><div class=\"grow nowrap\" id=\"dlg-title\">" +
           e(title) + "</div><div class=\"dlg-x\" data-action=\"dialog-close\">" + icon_glyph("x") + "</div></div><div class=\"dlg-body\" id=\"dlg-body\">" + body +
           "</div><div class=\"dlg-foot\" id=\"dlg-foot\">" + buttons + "</div></div>";
}

std::string about_body_html() {
    auto lic = [](const char *name, const char *license, const char *text) {
        return std::string("<div class=\"lic\"><div class=\"lic-top\"><div class=\"lic-name\">") + name + "</div><div class=\"lic-tag\">" + license +
               "</div></div><div class=\"lic-text\">" + text + "</div></div>";
    };
    std::string s = "<div class=\"about-head\"><div class=\"mark\"><div class=\"mark-core\"></div></div><div><div class=\"about-name\">XenoForge</div>"
                    "<div class=\"about-ver\">Pre-release build, " __DATE__ "</div></div></div>";
    s += "<div class=\"about-text\">A lean Xbox 360 system for statically recompiled games. Your discs are prepared into game libraries on this "
         "computer, once, and played from there.</div>";
    s += "<div class=\"about-note\">XenoForge is an independent project. It is not affiliated with, endorsed by or sponsored by Microsoft or Xbox, "
         "and it ships no game, firmware or console software: you bring your own discs.</div>";
    s += "<div class=\"about-licenses\">Licenses</div>";
    s += lic("XenoForge", "MIT", "The launcher, the player, the recompiler and the system around them. The full text is LICENSE beside the program.");
    s += lic("FFmpeg", "LGPL 2.1+",
             "Sound for games that use XMA is decoded by ffmpeg-xma.dll, a build of FFmpeg that XenoForge loads at run time. It is a separate file "
             "you may replace with your own build of the same source; where the source is, and how it was configured, is in FFMPEG-SOURCE.txt "
             "beside it, with the license's text in COPYING.LGPLv2.1.");
    s += lic("Xenia", "BSD 3-Clause",
             "The reference the console's behaviour is checked against; parts of the kernel, GPU and input follow its code and say so. See LICENSE-xenia.");
    s += lic("SDL 3", "zlib", "Windowing, input, gamepads, audio output and the folder dialog. Includes HIDAPI (BSD).");
    s += lic("RmlUi", "MIT", "The interface you are looking at: laid out in RML and RCSS, drawn through XenoForge's own Vulkan.");
    s += lic("FreeType", "FreeType License",
             "Renders the text. Portions of this software are copyright The FreeType Project (freetype.org). All rights reserved.");
    s += lic("Inter", "SIL OFL 1.1", "The typeface, by The Inter Project Authors (rsms.me/inter). Its license is in ui/fonts/LICENSE-Inter.txt.");
    s += lic("Lucide", "ISC", "The icons, by the Lucide Contributors (lucide.dev; parts from Feather, MIT). Its license is in ui/fonts/LICENSE-Lucide.txt.");
    s += lic("Dear ImGui", "MIT", "The player's debugging overlay (F1).");
    s += lic("toml++", "MIT", "Reads and writes config.toml.");
    s += lic("stb", "MIT / public domain", "Image decoding and PNG writing.");
    s += "<div class=\"about-foot dim\">The complete notices ship as THIRD-PARTY-NOTICES.txt beside the launcher.</div>";
    return s;
}

const char *prepare_stage_label(int stage) {
    static const char *labels[] = {"Read disc", "Analyze", "Write code", "Compile", "Link"};
    return stage >= 0 && stage < 5 ? labels[stage] : "";
}

std::string prepare_body_html(const std::string &title, const std::string &icon, uint32_t title_id) {
    std::string s = "<div class=\"prep-head\"><div class=\"prep-icon\" style=\"" + art_style(icon, title_id) + "\">" + art_letter(icon, title) +
                    "</div><div class=\"grow\"><div class=\"prep-title nowrap\">" + e(title) +
                    "</div><div class=\"prep-state\"><div class=\"dot prep\" id=\"prep-dot\"></div><div class=\"dim\" id=\"prep-state\">Starting</div></div></div></div>";
    s += "<div class=\"stages\">";
    for (int i = 0; i < 5; i++)
        s += "<div class=\"stage\" id=\"stage-" + std::to_string(i) + "\"><div class=\"bar\"><div class=\"in\" id=\"stage-in-" + std::to_string(i) +
             "\"></div></div><div class=\"nm\">" + prepare_stage_label(i) + "</div></div>";
    s += "</div>";
    s += "<div class=\"pbar\" id=\"pbar\"><div class=\"pfill\" id=\"pfill\"></div></div>";
    s += "<div class=\"prep-meta\"><div id=\"prep-count\">Starting...</div><div class=\"grow\"></div><div class=\"dim\" id=\"prep-time\"></div></div>";
    s += "<div class=\"prep-file nowrap\" id=\"prep-file\"></div>";
    s += "<div class=\"errbox\" id=\"prep-error\"></div>";
    s += "<div class=\"log-toggle f\" id=\"log-toggle\" data-action=\"toggle-log\">" + icon_html("chevron-down", "ic chev") +
         "<div id=\"log-toggle-text\">Show log</div></div>";
    s += "<div class=\"log\" id=\"log\"></div>";
    return s;
}

// ---------------------------------------------------------------------------
// the settings window

const char *settings_tab_label(SettingsTab tab) {
    static const char *labels[] = {"Video", "Sound", "Controllers", "Keyboard", "System", "Profile", "Interface"};
    return labels[int(tab)];
}

const char *settings_tab_name(SettingsTab tab) {
    static const char *names[] = {"video", "sound", "controllers", "keyboard", "system", "profile", "interface"};
    return names[int(tab)];
}

std::string settings_tabs_html(SettingsTab current) {
    static const char *icons[] = {"monitor", "volume-2", "gamepad-2", "keyboard", "cpu", "hard-drive", "sliders-horizontal"};
    std::string s;
    for (int i = 0; i < SETTINGS_TABS; i++)
        s += std::string("<div class=\"tab f") + (i == int(current) ? " active" : "") + "\" data-action=\"tab\" data-arg=\"" +
             settings_tab_name(SettingsTab(i)) + "\" id=\"tab-" + settings_tab_name(SettingsTab(i)) + "\">" + icon_html(icons[i]) + "<div>" +
             settings_tab_label(SettingsTab(i)) + "</div></div>";
    return s;
}

std::string settings_scope_html(const SettingsPage &page) {
    const Settings &st = *page.settings;
    if (page.game_title.empty())
        return "<div class=\"scope-text\"><div class=\"name\">Shared settings</div><div class=\"hint\">Every game uses these unless it has settings of its "
               "own. Select a game in the list to give it its own.</div></div>";
    const bool own = !st.game.empty();
    std::string s = check_row("own", ("Settings of its own for " + page.game_title).c_str(),
                              own ? "Only " + page.title_id + " reads the settings below; the shared ones are untouched."
                                  : "Off: " + page.game_title + " uses the settings every game shares, edited below.",
                              own);
    return s;
}

std::vector<Choice> aspect_choices() { return {{"auto", "The picture's own shape"}, {"stretch", "Stretched to the window"}}; }

std::vector<Choice> user_choices() { return {{"0", "User 1"}, {"1", "User 2"}, {"2", "User 3"}, {"3", "User 4"}}; }

std::vector<Choice> theme_choices() {
    return {{"system", "The system's (light or dark)"}, {"light", "Light"}, {"dark", "Dark"}};
}

std::string keycaps_html(const std::string &keys, bool waiting) {
    if (waiting) return "<div class=\"keycap press\">Press a key... (Esc cancels)</div>";
    std::string s;
    const std::vector<KeyCap> caps = parse_keys(keys);
    if (caps.empty()) return "<div class=\"keycap mod\">not set</div>";
    for (const KeyCap &c : caps) {
        if (c.shift) s += "<div class=\"keycap mod\">Shift +</div>";
        if (c.no_shift) s += "<div class=\"keycap mod\">no Shift</div>";
        s += "<div class=\"keycap\">" + e(c.label) + "</div>";
    }
    return s;
}

std::string settings_page_html(SettingsTab tab, const SettingsPage &page) {
    const Settings &st = *page.settings;
    std::string s = "<div class=\"page\" id=\"page-" + std::string(settings_tab_name(tab)) + "\">";
    switch (tab) {
    case SettingsTab::Video: {
        std::string rows = check_row("video.fullscreen", "Start in fullscreen", "F11 switches in the game.", st.video.fullscreen);
        rows += select_row("video.aspect", "Picture shape", "How the game's picture fits the window.", label_of(aspect_choices(), st.video.aspect));
        rows += check_row("video.smooth", "Smooth scaling", "Linear filtering when the picture is scaled to the window.", st.video.smooth);
        s += group("Display", rows);
        s += group("Timing", check_row("video.vsync", "Vertical sync", "The window waits for the display; the game keeps its own pace.", st.video.vsync));
        break;
    }
    case SettingsTab::Sound: {
        std::string rows = slider_row("audio.volume", "Volume", "", st.audio.volume / 100.0, std::to_string(st.audio.volume) + "%");
        rows += check_row("audio.mute", "Mute", "No sound from the game.", st.audio.mute);
        s += group("Output", rows, "The game's 5.1 is folded to stereo, as Xenia folds it.");
        break;
    }
    case SettingsTab::Controllers: {
        std::string pads = "<div class=\"pads\">";
        if (page.gamepads.empty())
            pads += "<div class=\"gamepad\"><div class=\"dot off\"></div><div class=\"dim\">No gamepad connected</div></div>";
        for (size_t i = 0; i < page.gamepads.size(); i++)
            pads += "<div class=\"gamepad\"><div class=\"dot ready\"></div><div class=\"pad-user dim\">User " + std::to_string(i + 1) + "</div><div class=\"nowrap\">" +
                    e(page.gamepads[i]) + "</div></div>";
        pads += "</div>";
        s += group("Connected", pads, "Plugging one in is noticed at once. The first pad is user 1; the keyboard can stand in for another user.");
        std::string rows = check_row("input.vibration", "Vibration", "", st.input.vibration);
        rows += slider_row("input.left_deadzone", "Left stick dead zone", "", st.input.left_deadzone,
                           std::to_string(int(st.input.left_deadzone * 100 + 0.5)) + "%");
        rows += slider_row("input.right_deadzone", "Right stick dead zone", "", st.input.right_deadzone,
                           std::to_string(int(st.input.right_deadzone * 100 + 0.5)) + "%");
        s += group("Behaviour", rows);
        break;
    }
    case SettingsTab::Keyboard: {
        std::string rows = check_row("input.keyboard", "Keyboard as a controller", "The keys below press the pad's controls.", st.input.keyboard);
        rows += select_row("input.keyboard_user", "For user", "Which player the keyboard is.", label_of(user_choices(), std::to_string(st.input.keyboard_user)));
        s += group("Keyboard", rows);
        const char *groups[] = {"Face buttons", "Bumpers and triggers", "D-pad", "Left stick", "Right stick", "System"};
        for (const char *name : groups) {
            std::string list;
            for (const Binding &b : BINDINGS) {
                const BindingInfo info = binding_info(b.name);
                if (std::string(info.group) != name) continue;
                const auto it = st.keys.find(b.name);
                const bool waiting = page.capturing == b.name;
                list += std::string("<div class=\"bind f") + (waiting ? " capturing" : "") + "\" data-action=\"capture\" data-arg=\"" + b.name +
                        "\"><div class=\"bname\">" + e(info.label) + "</div><div class=\"keys\">" +
                        keycaps_html(it == st.keys.end() ? std::string() : it->second, waiting) + "</div><div class=\"bset\">Change</div></div>";
            }
            s += group(name, list);
        }
        s += "<div class=\"page-buttons\">" + button_html("default-keys", "Use Xenia's keys", false, {}, "refresh-cw") + "</div>";
        break;
    }
    case SettingsTab::System: {
        std::string rows = select_row("system.language", "Language", "What games show their menus in.",
                                      languages()[index_of(languages(), st.system.language)].name);
        rows += select_row("system.country", "Country or region", "Where the console says it is: some games choose content by it.",
                           countries()[index_of(countries(), st.system.country)].name);
        s += group("Region", rows);
        std::string video = select_row("system.resolution", "Resolution", "The screen the console says is attached.",
                                       resolutions()[index_of(resolutions(), resolution_of(st.system))].name);
        video += select_row("system.widescreen", "Aspect ratio", "The console's 4:3 or 16:9 video flag.",
                            widescreen_modes()[index_of(widescreen_modes(), st.system.widescreen)].name);
        video += select_row("system.refresh", "Refresh rate", "50 or 60 Hz, as a PAL or an NTSC console.",
                            refresh_rates()[index_of(refresh_rates(), st.system.refresh)].name);
        s += group("Video", video, "Games read these when they start; a game that is running keeps what it read.");
        break;
    }
    case SettingsTab::Profile: {
        const std::string &confirm = page.confirm_delete;
        std::string list;
        for (const ProfileEntry &p : page.profiles) {
            list += std::string("<div class=\"profile-row") + (p.selected ? " current" : "") + "\"><div class=\"pname nowrap\">" +
                    e(p.name.empty() ? std::string("(no gamertag)") : p.name) + (p.selected ? "  (used by the games)" : "") + "</div><div class=\"pid\">" +
                    e(p.xuid) + "</div>";
            if (!p.selected)
                list += "<div class=\"btn f\" data-action=\"profile-use\" data-arg=\"" + p.xuid + "\"><div class=\"btn-l\">Use</div></div>";
            if (confirm == p.xuid)
                list += "<div class=\"btn f primary\" data-action=\"profile-delete\" data-arg=\"" + p.xuid +
                        "\"><div class=\"btn-l\">Delete it and its saves?</div></div><div class=\"btn f\" data-action=\"profile-keep\"><div class=\"btn-l\">Keep</div></div>";
            else
                list += "<div class=\"btn f\" data-action=\"profile-delete\" data-arg=\"" + p.xuid + "\"><div class=\"btn-l\">Delete</div></div>";
            list += "</div>";
        }
        if (page.profiles.empty())
            list += "<div class=\"profile-row\"><div class=\"pname dim\">No profile yet: the first game that runs makes the console's default one.</div></div>";
        s += group("Profiles", list,
                   "A profile is a folder of the storage root with its saves and achievements. The game uses the one marked; "
                   "changing it takes a restart of the game.");
        std::string name = "<div class=\"folder-row\"><input type=\"text\" id=\"gamertag-input\" class=\"text f\" maxlength=\"15\" value=\"" +
                           e(selected_gamertag_of(page)) + "\" placeholder=\"Gamertag\"/>" +
                           button_html("profile-rename", "Rename", false, {}, "check") + "</div>";
        name += "<div class=\"folder-row\"><input type=\"text\" id=\"new-profile-input\" class=\"text f\" maxlength=\"15\" value=\"\" "
                "placeholder=\"A new profile's gamertag\"/>" +
                button_html("profile-create", "Create", false, {}, "check") + "</div>";
        if (!page.profile_message.empty()) name += "<div class=\"pmsg\" id=\"profile-message\">" + e(page.profile_message) + "</div>";
        else name += "<div class=\"pmsg\" id=\"profile-message\"></div>";
        s += group("Gamertag", name, "1 to 15 letters, digits and spaces; it starts with a letter and has no double spaces. The games show it from the next start.");
        s += group("Saves", select_row("saves.format", "Format", "STFS packages are what the console keeps; folders are Xenia's layout.",
                                       st.saves.format == "directory" ? "Folders (Xenia's)" : "STFS packages (the console's)"));
        std::string ach;
        if (page.game_title.empty()) {
            ach = "<div class=\"profile-row\"><div class=\"pname dim\">Select a game to see its achievements.</div></div>";
        } else if (page.achievements.empty()) {
            ach = "<div class=\"profile-row\"><div class=\"pname dim\">" +
                  e(page.achievements_read ? "This game lists no achievements." : "None yet: the profile has no record of this game until it runs.") + "</div></div>";
        } else {
            uint32_t earned = 0, total = 0;
            for (const auto &a : page.achievements) {
                total += a.gamerscore;
                if (a.unlocked) earned += a.gamerscore;
                ach += std::string("<div class=\"ach") + (a.unlocked ? "" : " locked") + "\"><div class=\"ach-icon\">" + (a.unlocked ? icon_html("check") : std::string()) +
                       "</div><div class=\"ach-text\"><div class=\"ach-name\">" + e(utf16_to_utf8(a.label)) + "  " + std::to_string(a.gamerscore) + " G" +
                       "</div><div class=\"dim\">" + e(utf16_to_utf8(a.unlocked || a.unachieved.empty() ? a.description : a.unachieved)) + "</div></div></div>";
            }
            ach = "<div class=\"profile-row\"><div class=\"pname\">" + std::to_string(earned) + " / " + std::to_string(total) + " G earned</div></div>" + ach;
        }
        s += group("Achievements" + (page.game_title.empty() ? std::string() : " - " + page.game_title), ach,
                   "Read from the selected profile's record; the game writes it as the player earns them.");
        break;
    }
    case SettingsTab::Interface: {
        std::string look = select_row("theme", "Theme", "Light or dark; the system's follows the desktop as it changes.",
                                      label_of(theme_choices(), page.theme));
        look += "<div class=\"setting\" data-kind=\"swatches\"><div class=\"label\"><div class=\"name\">Accent colour</div>"
                "<div class=\"hint\">Selections, focus and the main buttons.</div></div><div class=\"control swatches\">";
        for (const std::string &hex : accent_choices()) {
            look += "<div class=\"swatch f";
            if (hex == page.accent) look += " on";
            look += "\" data-action=\"accent\" data-arg=\"" + hex + "\" style=\"background-color: " + hex + ";\"></div>";
        }
        look += "</div></div>";
        s += group("Appearance", look);
        std::string folder = "<div class=\"setting\"><div class=\"label\"><div class=\"name\">Games folder</div>"
                             "<div class=\"hint\">Discs are read in place, one level of subfolders deep.</div></div></div>"
                             "<div class=\"folder-row\"><input type=\"text\" id=\"folder-input\" class=\"text f\" value=\"" + e(page.folder) +
                             "\" placeholder=\"No folder chosen\"/>" + button_html("browse", "Browse...", false, {}, "folder-open") +
                             button_html("rescan", "Rescan", false, {}, "refresh-cw") + "</div>";
        s += group("Library", folder);
        break;
    }
    }
    return s + "</div>";
}

} // namespace xf::frontend
