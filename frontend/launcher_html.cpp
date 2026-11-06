// The launcher's screens as RML (launcher_html.h).

#include "launcher_html.h"

#include <cstdio>

namespace xf::frontend {

namespace {

std::string e(std::string_view text) { return html_escape(text); }

std::string percent_text(double fraction) {
    char text[16];
    std::snprintf(text, sizeof(text), "%d%%", int(fraction * 100 + 0.5));
    return text;
}

std::string initial_of(const std::string &title) {
    // the first letter or digit, as UTF-8 when it is ASCII (else a plain mark)
    for (char c : title)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return std::string(1, c >= 'a' && c <= 'z' ? char(c - 32) : c);
    return "?";
}

std::string toggle_row(const char *key, const char *name, const std::string &hint, bool on, bool first = false) {
    std::string s = first ? "" : "<div class=\"sep\"></div>";
    s += "<div class=\"setting f";
    if (first) s += " first";
    s += "\" data-kind=\"toggle\" data-action=\"toggle\" data-key=\"";
    s += key;
    s += "\"><div class=\"label\"><div class=\"name\">";
    s += e(name);
    s += "</div>";
    if (!hint.empty()) s += "<div class=\"hint\">" + e(hint) + "</div>";
    s += "</div><div class=\"control\"><div class=\"toggle";
    if (on) s += " on";
    s += "\"><div class=\"knob\"></div></div></div></div>";
    return s;
}

std::string slider_row(const char *key, const char *name, const std::string &hint, double fraction, const std::string &text,
                       bool first = false) {
    std::string s = first ? "" : "<div class=\"sep\"></div>";
    s += "<div class=\"setting f";
    if (first) s += " first";
    s += "\" data-kind=\"slider\" data-key=\"";
    s += key;
    s += "\"><div class=\"label\"><div class=\"name\">" + e(name) + "</div>";
    if (!hint.empty()) s += "<div class=\"hint\">" + e(hint) + "</div>";
    char pos[32];
    std::snprintf(pos, sizeof(pos), "%.2f%%", fraction * 100);
    s += "</div><div class=\"control\"><div class=\"slider\"><div class=\"track\"></div><div class=\"fill\" style=\"width: ";
    s += pos;
    s += ";\"></div><div class=\"thumb\" style=\"left: ";
    s += pos;
    s += ";\"></div></div><div class=\"value\">" + e(text) + "</div></div></div>";
    return s;
}

std::string select_row(const char *key, const char *name, const std::string &hint, const std::string &current,
                       bool first = false) {
    std::string s = first ? "" : "<div class=\"sep\"></div>";
    s += "<div class=\"setting f";
    if (first) s += " first";
    s += "\" data-kind=\"select\" data-action=\"select\" data-key=\"";
    s += key;
    s += "\"><div class=\"label\"><div class=\"name\">" + e(name) + "</div>";
    if (!hint.empty()) s += "<div class=\"hint\">" + e(hint) + "</div>";
    s += "</div><div class=\"control\"><div class=\"select\"><div class=\"value-text nowrap\">" + e(current) +
         "</div><div class=\"chevron\"></div></div></div></div>";
    return s;
}

std::string card(const std::string &title, const std::string &tag, const std::string &rows) {
    return "<div class=\"card\"><div class=\"card-title\"><div class=\"eyebrow\">" + e(title) + "</div>" +
           (tag.empty() ? "" : "<div class=\"tag\">" + e(tag) + "</div>") + "</div>" + rows + "</div>";
}

std::string label_of(const std::vector<Choice> &choices, const std::string &value) {
    for (const Choice &c : choices)
        if (c.value == value) return c.label;
    return choices.empty() ? std::string() : choices[0].label;
}

} // namespace

std::string art_style(const std::string &icon, uint32_t title_id) {
    if (!icon.empty()) return "decorator: image(mem:" + icon + " cover);";
    return tile_background(title_id);
}

std::string art_letter(const std::string &icon, const std::string &title) {
    if (!icon.empty()) return {};
    return "<div class=\"letter\">" + e(initial_of(title)) + "</div>";
}

std::string tiles_html(const std::vector<TileInfo> &tiles) {
    std::string s;
    for (const TileInfo &t : tiles) {
        const bool broken = !t.error.empty();
        s += "<div class=\"tile f";
        if (broken) s += " broken";
        s += "\" data-index=\"" + std::to_string(t.index) + "\" data-action=\"tile\">";
        s += "<div class=\"tile-art\" style=\"" + (broken ? std::string("background-color: #1a1e2b;") : art_style(t.icon, t.title_id)) + "\">";
        s += broken ? "<div class=\"letter\">!</div>" : art_letter(t.icon, t.title);
        s += "<div class=\"sheen\"></div></div>";
        s += "<div class=\"glow\"></div>";
        s += "<div class=\"tile-name nowrap\">" + e(broken ? std::string("Cannot be read") : t.title) + "</div>";
        s += "<div class=\"tile-sub\">";
        if (broken)
            s += "<div class=\"dot bad\"></div><div class=\"lbl nowrap\">" + e(t.error) + "</div>";
        else
            s += std::string("<div class=\"dot ") + status_class(t.status) + "\"></div><div class=\"lbl\">" + e(status_text(t.status)) + "</div>";
        s += "</div></div>";
    }
    return s;
}

std::string skeleton_html(int count) {
    std::string s;
    for (int i = 0; i < count; i++)
        s += "<div class=\"tile skeleton\"><div class=\"tile-art\"></div><div class=\"bar\"></div><div class=\"bar short\"></div></div>";
    return s;
}

std::string detail_empty_html(bool scanning) {
    return std::string("<div class=\"detail-body\" style=\"align-items: center; justify-content: center;\">") +
           "<div class=\"eyebrow\">" + (scanning ? "Reading your games" : "Nothing selected") + "</div>" +
           "<div class=\"muted\" style=\"margin-top: 10dp; text-align: center;\">" +
           (scanning ? "Each disc is opened in place to read its name and picture." : "Pick a game to see its details.") +
           "</div></div>";
}

std::string detail_html(const DetailInfo &g) {
    std::string s;
    s += "<div class=\"hero\"><div class=\"hero-art\" style=\"" + art_style(g.icon.empty() ? g.icon : "soft_" + g.icon, g.title_id) + "\"></div><div class=\"hero-shade\"></div>";
    s += "<div class=\"hero-icon\" style=\"" + art_style(g.icon, g.title_id) + "\">" + art_letter(g.icon, g.title) + "</div></div>";
    s += "<div class=\"detail-body\">";
    s += "<div class=\"detail-title\">" + e(g.title) + "</div>";
    s += "<div class=\"detail-meta\">" + e(g.id_text) + "  \xC2\xB7  v" + e(g.version) + "</div>";
    s += std::string("<div class=\"chips\"><div class=\"chip ") + status_class(g.status) + "\"><div class=\"dot " + status_class(g.status) + "\"></div><div class=\"lbl\">" +
         status_text(g.status) + "</div></div></div>";
    s += "<div class=\"facts\">";
    s += "<div class=\"fact\"><div class=\"k\">Runs in</div><div class=\"v nowrap\">" + e(g.runs_in) + "</div></div>";
    s += "<div class=\"fact\"><div class=\"k\">Disc</div><div class=\"v nowrap\">" + e(g.file) + "</div></div>";
    s += "</div>";
    if (!g.last_run.empty()) {
        s += "<div class=\"small dim\" style=\"margin-top: 14dp; text-align: center;\">";
        for (size_t i = 0; i < g.last_run.size() && i < 2; i++) s += e(g.last_run[i]) + "<br/>";
        s += "</div>";
    }
    s += "<div class=\"detail-spacer\"></div>";
    if (g.status != GameStatus::Ready && !g.running) s += "<div class=\"hint-line\">" + e(status_detail(g.status)) + "</div>";
    if (g.running) {
        s += "<div class=\"btn primary danger f\" data-action=\"stop\" style=\"decorator: linear-gradient(180deg, #f87171, #dc2626);\">Stop</div>";
    } else {
        s += std::string("<div class=\"btn primary row f") + (g.has_player ? "" : " off") + "\" data-action=\"play\" id=\"play-button\"><div class=\"play-icon\"></div><div class=\"lbl\">Play</div></div>";
    }
    s += "<div class=\"btn-row\" style=\"margin-top: 12dp;\">";
    s += "<div class=\"btn small f\" data-action=\"game-settings\">Game settings</div>";
    if (g.status == GameStatus::NeedsPreparing && g.has_player)
        s += "<div class=\"btn small f\" data-action=\"play-interpreted\">Play interpreted</div>";
    else if (g.status != GameStatus::Interpreted)
        s += "<div class=\"btn small f\" data-action=\"prepare\">Rebuild library</div>";
    else
        s += "<div class=\"btn small f\" data-action=\"prepare\">Try preparing again</div>";
    s += "</div></div>";
    return s;
}

std::string empty_html(bool no_folder, const std::string &folder) {
    std::string s = "<div class=\"empty-art\"><div class=\"folder-back\"></div><div class=\"folder-tab\"></div><div class=\"folder-front\"></div>";
    s += "<div class=\"folder-spark\"></div></div>";
    if (no_folder) {
        s += "<h2>Choose your games folder</h2>";
        s += "<p>Point XenoForge at the folder with your game discs. It reads them where they are: nothing is copied, "
             "and nothing leaves this computer.</p>";
        s += "<div class=\"btn-row\"><div class=\"btn primary f\" data-action=\"browse\" id=\"empty-primary\">Choose folder</div></div>";
        s += "<div class=\"note\">Disc images (.iso) and extracted games (default.xex), in the folder or one level below.</div>";
    } else {
        s += "<h2>No games found</h2>";
        s += "<p>There are no disc images or default.xex files in</p><div class=\"pathchip nowrap\">" + e(folder) +
             "</div><p style=\"margin-top: 8dp;\">or in the folders directly inside it.</p>";
        s += "<div class=\"btn-row\"><div class=\"btn primary f\" data-action=\"browse\" id=\"empty-primary\">Choose another folder</div>"
             "<div class=\"btn f\" data-action=\"rescan\" style=\"margin-left: 14dp;\">Scan again</div></div>";
        s += "<div class=\"note\">Disc images (.iso) and extracted games (default.xex) are found; archives are not opened.</div>";
    }
    return s;
}

std::vector<Choice> aspect_choices() {
    return {{"auto", "The picture's own shape"}, {"stretch", "Stretched to the window"}};
}

std::vector<Choice> user_choices() { return {{"0", "User 1"}, {"1", "User 2"}, {"2", "User 3"}, {"3", "User 4"}}; }

std::string settings_html(const SettingsPage &page) {
    const Settings &st = *page.settings;
    std::string video = toggle_row("video.fullscreen", "Fullscreen", "F11 does the same in the game.", st.video.fullscreen, true);
    video += toggle_row("video.vsync", "Vertical sync", "The window waits for the display; the game keeps its own pace.", st.video.vsync);
    video += toggle_row("video.smooth", "Smooth scaling", "Linear filtering when the picture is scaled to the window.", st.video.smooth);
    video += select_row("video.aspect", "Picture shape", "How the picture fits the window.", label_of(aspect_choices(), st.video.aspect));
    std::string sound = slider_row("audio.volume", "Volume", "", st.audio.volume / 100.0, std::to_string(st.audio.volume) + "%", true);
    sound += toggle_row("audio.mute", "Mute", "", st.audio.mute);

    std::string left = card("Video", "", video) + card("Sound", "", sound);

    std::string right;
    if (!page.game_title.empty()) {
        const bool own = !st.game.empty();
        right += card("This game", page.game_title,
                      toggle_row("own", "Settings of its own",
                                 own ? "Only " + page.title_id + " reads these; the shared ones are untouched."
                                     : "Off: this game follows the settings every game shares.",
                                 own, true));
    }
    std::string look = "<div class=\"setting first\" data-kind=\"swatches\"><div class=\"label\"><div class=\"name\">Accent colour</div>"
                       "<div class=\"hint\">The one colour the launcher lights up with.</div></div><div class=\"control swatches\">";
    for (const std::string &hex : accent_choices()) {
        look += "<div class=\"swatch f";
        if (hex == page.accent) look += " on";
        look += "\" data-action=\"accent\" data-arg=\"" + hex + "\" style=\"background-color: " + hex + ";\"></div>";
    }
    look += "</div></div>";
    right += card("Appearance", "", look);

    std::string folder = "<div class=\"setting first\"><div class=\"label\"><div class=\"name\">Games folder</div>"
                         "<div class=\"hint\">Discs are read in place, one level of subfolders deep.</div></div></div>"
                         "<div class=\"row\" style=\"margin: 0 0 18dp 0;\"><input type=\"text\" id=\"folder-input\" class=\"text f\" "
                         "style=\"flex: 1 1 0; width: auto; margin-right: 10dp;\" value=\"" + e(page.folder) + "\"/>"
                         "<div class=\"btn small f\" data-action=\"browse\">Browse...</div>"
                         "<div class=\"btn small f\" data-action=\"rescan\" style=\"margin-left: 8dp;\">Rescan</div></div>";
    right += card("Library", "", folder);

    std::string s = "<div class=\"page\" style=\"width: 100%; height: 100%;\"><div class=\"page-head\"><h1 class=\"title\">Settings</h1>";
    s += "<div class=\"sub\">" + e(page.game_title.empty() ? std::string("Shared by every game") : st.game.empty() ? "Shared by every game, " + page.game_title + " included" : "Just for " + page.game_title) + "</div></div>";
    s += "<div class=\"page-scroll\"><div class=\"scroller\" id=\"settings-scroll\"><div class=\"cols\"><div class=\"col\">" + left + "</div><div class=\"col\">" + right + "</div></div></div><div class=\"page-fade-top\"></div><div class=\"page-fade\"></div></div></div>";
    return s;
}

std::string keycaps_html(const std::string &keys, bool waiting) {
    if (waiting) return "<div class=\"keycap press\">Press a key...</div>";
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

std::string controls_html(const ControlsPage &page) {
    const Settings &st = *page.settings;
    // the drawn controller
    auto part = [](const char *id, const char *classes, int x, int y, const char *label = "") {
        char style[96];
        std::snprintf(style, sizeof(style), "left: %ddp; top: %ddp;", x, y);
        return std::string("<div class=\"pad-part ") + classes + "\" data-part=\"" + id + "\" id=\"part-" + id + "\" style=\"" + style + "\">" + label + "</div>";
    };
    std::string pad = "<div id=\"pad\"><div class=\"pad-grip l\"></div><div class=\"pad-grip r\"></div><div class=\"pad-body\"></div>";
    pad += part("lt", "trigger", 46, 0) + part("rt", "trigger", 318, 0);
    pad += part("lb", "bumper", 30, 32) + part("rb", "bumper", 314, 32);
    pad += "<div class=\"pad-part stick\" id=\"part-ls\" data-part=\"ls\" style=\"left: 56dp; top: 74dp;\"><div class=\"stick-cap\"></div></div>";
    pad += "<div class=\"pad-part stick\" id=\"part-rs\" data-part=\"rs\" style=\"left: 244dp; top: 138dp;\"><div class=\"stick-cap\"></div></div>";
    pad += part("dpad-up", "dpad-arm", 140, 124) + part("dpad-down", "dpad-arm", 140, 168) + part("dpad-left", "dpad-arm", 118, 146) +
           part("dpad-right", "dpad-arm", 162, 146);
    pad += "<div class=\"pad-part dpad-arm\" style=\"left: 140dp; top: 146dp; border-radius: 0dp;\"></div>";
    pad += part("y", "face pad-circle", 328, 70, "Y") + part("a", "face pad-circle", 328, 138, "A") + part("x", "face pad-circle", 294, 104, "X") +
           part("b", "face pad-circle", 362, 104, "B");
    pad += part("back", "small-btn", 176, 100) + part("start", "small-btn", 226, 100);
    pad += part("guide", "pad-circle face", 195, 62);
    pad += "</div>";
    std::string stage = "<div id=\"pad-stage\">" + pad +
                        "<div class=\"pad-caption\" id=\"pad-caption\">Move over a control to see its keys</div></div>";

    // the controllers and the keyboard
    std::string pads = "<div class=\"setting first\" style=\"display: block; min-height: 0; padding-top: 4dp;\"><div class=\"pads\">";
    if (page.gamepads.empty())
        pads += "<div class=\"gamepad none\"><div class=\"dot bad\" style=\"background-color: #3a4156;\"></div><div class=\"lbl\">No gamepad connected</div></div>";
    for (size_t i = 0; i < page.gamepads.size(); i++)
        pads += "<div class=\"gamepad\"><div class=\"dot ready\"></div><div class=\"lbl dim\" style=\"margin-right: 8dp;\">User " + std::to_string(i + 1) + "</div><div class=\"lbl\">" +
                e(page.gamepads[i]) + "</div></div>";
    pads += "</div><div class=\"hint tiny dim\" style=\"margin-top: 2dp;\">Plugging one in is noticed at once. A pad is user 1 and the keyboard can stand in for another.</div></div>";
    pads += toggle_row("input.vibration", "Vibration", "", st.input.vibration);
    pads += slider_row("input.left_deadzone", "Left stick dead zone", "", st.input.left_deadzone, std::to_string(int(st.input.left_deadzone * 100 + 0.5)) + "%");
    pads += slider_row("input.right_deadzone", "Right stick dead zone", "", st.input.right_deadzone, std::to_string(int(st.input.right_deadzone * 100 + 0.5)) + "%");
    std::string keyboard = toggle_row("input.keyboard", "Keyboard as a controller", "The keys below press the pad's controls.", st.input.keyboard, true);
    keyboard += select_row("input.keyboard_user", "For user", "", label_of(user_choices(), std::to_string(st.input.keyboard_user)));

    std::string list;
    const char *groups[] = {"Face buttons", "Bumpers and triggers", "D-pad", "Left stick", "Right stick", "System"};
    bool first = true;
    for (const char *group : groups) {
        list += std::string("<div class=\"eyebrow group-title") + (first ? " first" : "") + "\">" + group + "</div>";
        first = false;
        for (const Binding &b : BINDINGS) {
            const BindingInfo info = binding_info(b.name);
            if (std::string(info.group) != group) continue;
            const auto it = st.keys.find(b.name);
            const bool waiting = page.capturing == b.name;
            list += std::string("<div class=\"bind f") + (waiting ? " capturing" : "") + "\" data-action=\"capture\" data-arg=\"" + b.name +
                    "\" data-part=\"" + info.part + "\"><div class=\"bname\">" + e(info.label) + "</div><div class=\"keys row\">" +
                    keycaps_html(it == st.keys.end() ? std::string() : it->second, waiting) + "</div></div>";
        }
    }
    list += "<div class=\"btn-row\" style=\"margin: 20dp 0 26dp 0;\"><div class=\"btn small f\" data-action=\"default-keys\" style=\"flex: 0 0 auto;\">Use Xenia's keys</div></div>";

    std::string s = "<div class=\"page\" style=\"width: 100%; height: 100%;\"><div class=\"page-head\"><h1 class=\"title\">Controls</h1><div class=\"sub\">";
    s += e(page.game_title.empty() ? std::string("The keyboard's keys for the pad's controls") : "For " + page.game_title) + "</div></div>";
    s += "<div id=\"controls-body\">" + stage + "<div id=\"bindings\"><div class=\"page-scroll\" style=\"position: absolute; left: 0; top: 0; width: 100%; height: 100%;\">";
    s += "<div class=\"scroller\" id=\"controls-scroll\">" + card("Keyboard", "", keyboard) + list + card("Controllers", "", pads) + "</div><div class=\"page-fade-top\"></div><div class=\"page-fade\"></div></div></div></div></div>";
    return s;
}

std::string about_html() {
    auto lic = [](const char *name, const char *license, const char *text, const char *extra = "") {
        return std::string("<div class=\"lic ") + extra + "\"><div class=\"top\"><div class=\"nm\">" + name + "</div><div class=\"tag\">" + license +
               "</div></div><div class=\"tx\">" + text + "</div></div>";
    };
    std::string s = "<div class=\"page\" style=\"width: 100%; height: 100%; flex-direction: row;\">";
    s += "<div id=\"about-left\"><div id=\"about-mark\"><div class=\"m\"></div><div class=\"c\"></div></div>"
         "<div id=\"about-name\">XENO<span>FORGE</span></div>"
         "<div id=\"about-tag\">A lean Xbox 360 system for statically recompiled games. Your discs are prepared into game libraries on this computer, once, and "
         "played from there.</div>"
         "<div id=\"about-ver\">Pre-release build, " __DATE__ "<br/>Interface: RmlUi on Vulkan 1.3</div>"
         "<div class=\"notice\">XenoForge is an independent project. It is not affiliated with, endorsed by or sponsored by Microsoft or Xbox, and it "
         "ships no game, firmware or console software: you bring your own discs.</div></div>";
    s += "<div class=\"page-scroll grow\" style=\"padding-top: 8dp;\"><div class=\"scroller\" id=\"about-scroll\" style=\"top: 0;\">"
         "<div class=\"page-head\" style=\"margin-bottom: 4dp;\"><h1 class=\"title\">Licenses</h1></div>";
    s += lic("XenoForge", "MIT", "The launcher, the player, the recompiler and the system around them. The full text is LICENSE beside the program.");
    s += lic("FFmpeg", "LGPL 2.1+",
             "Sound for games that use XMA is decoded by ffmpeg-xma.dll, a build of FFmpeg that XenoForge loads at run time. It is a separate file you may "
             "replace with your own build of the same source; where the source is, and how it was configured, is in FFMPEG-SOURCE.txt beside it, "
             "with the license's text in COPYING.LGPLv2.1.",
             "note-card");
    s += lic("Xenia", "BSD 3-Clause", "The reference the console's behaviour is checked against; parts of the kernel, GPU and input follow its code and say so. See LICENSE-xenia.");
    s += lic("SDL 3", "zlib", "Windowing, input, gamepads, audio output and the folder dialog. Includes HIDAPI (BSD).");
    s += lic("RmlUi", "MIT", "The interface you are looking at: laid out in RML and RCSS, drawn through XenoForge's own Vulkan.");
    s += lic("FreeType", "FreeType License", "Renders the text. Portions of this software are copyright The FreeType Project (freetype.org). All rights reserved.");
    s += lic("Inter", "SIL OFL 1.1", "The typeface, by The Inter Project Authors (rsms.me/inter). Its license is in ui/fonts/LICENSE-Inter.txt.");
    s += lic("Dear ImGui", "MIT", "The player's debugging overlay (F1).");
    s += lic("toml++", "MIT", "Reads and writes config.toml.");
    s += lic("stb", "MIT / public domain", "Image decoding and PNG writing.");
    s += "<div class=\"small dim\" style=\"margin: 22dp 0 0 4dp;\">The complete notices ship as THIRD-PARTY-NOTICES.txt beside the launcher.</div>";
    s += "</div><div class=\"page-fade\"></div></div></div>";
    return s;
}

const char *prepare_stage_label(int stage) {
    static const char *labels[] = {"Read disc", "Analyze", "Write code", "Compile", "Link"};
    return stage >= 0 && stage < 5 ? labels[stage] : "";
}

std::string prepare_html(const std::string &title, const std::string &icon, uint32_t title_id) {
    std::string s = "<div class=\"prep-head\"><div class=\"prep-icon\" style=\"" + art_style(icon, title_id) + "\">" + art_letter(icon, title) +
                    "</div><div class=\"grow\"><div class=\"eyebrow\" id=\"prep-eyebrow\">Preparing</div><div class=\"prep-title nowrap\">" + e(title) +
                    "</div><div class=\"prep-state\" style=\"margin-top: 4dp;\"><div class=\"dot prep\" id=\"prep-dot\"></div>"
                    "<div class=\"small muted\" id=\"prep-state\">Starting</div></div></div></div>";
    s += "<div class=\"stages\" id=\"stages\">";
    for (int i = 0; i < 5; i++)
        s += "<div class=\"stage\" id=\"stage-" + std::to_string(i) + "\"><div class=\"bar\"><div class=\"in\" id=\"stage-in-" + std::to_string(i) +
             "\"></div></div><div class=\"nm\">" + prepare_stage_label(i) + "</div></div>";
    s += "</div>";
    s += "<div class=\"pbar\" id=\"pbar\"><div class=\"fill\" id=\"pfill\"></div><div class=\"sheen\"></div></div>";
    s += "<div class=\"prep-meta\"><div class=\"big\" id=\"prep-count\">Starting...</div><div class=\"grow rt\" id=\"prep-time\"></div></div>";
    s += "<div class=\"prep-file nowrap\" id=\"prep-file\"></div>";
    s += "<div class=\"errbox\" id=\"prep-error\"></div>";
    s += "<div class=\"log-toggle f\" id=\"log-toggle\" data-action=\"toggle-log\"><div class=\"chevron\"></div><div class=\"lbl\" id=\"log-toggle-text\">Show log</div></div>";
    s += "<div class=\"log\" id=\"log\"></div>";
    s += "<div class=\"prep-actions\" id=\"prep-actions\"></div>";
    return s;
}

} // namespace xf::frontend
