// The player's parts that need no window: config.toml written and read
// back (a game's own settings too), the title's name from an XDBF (the
// launcher's), Xenia's default keyboard bindings all naming SDL keys, the
// sound's fold to stereo and its six channels interleaved, and the scripted
// presses (tools/game.h) parsed and held at their times.

#include "check.h"

#include "../frontend/launcher_core.h"
#include "../frontend/launcher_html.h"
#include "../frontend/launcher_model.h"
#include "../frontend/prepare_job.h"
#include "../frontend/library.h"
#include "../frontend/sdl_audio.h"
#include "../frontend/sdl_input.h"
#include "../frontend/ui/ribbons.h"
#include "../frontend/settings.h"
#include "../tools/game.h"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using namespace xf;

int main() {
    // the settings
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "xenoforge_frontend_test";
    std::filesystem::remove_all(dir, ec);
    frontend::Settings s, back;
    std::string error;
    CHECK(frontend::load(dir / "missing.toml", back, error)); // a missing file: the defaults
    CHECK(back.keys.at("start") == "X" && back.keys.at("left_trigger") == "Q I" && back.keys.at("a") == ";");
    s.audio.volume = 40, s.audio.mute = true, s.video.vsync = false, s.video.aspect = "stretch";
    s.input.keyboard_user = 2, s.input.left_deadzone = 0.25, s.keys["start"] = "Return";
    CHECK(frontend::save(dir / "config.toml", s, error));
    CHECK(frontend::load(dir / "config.toml", back, error));
    CHECK(back.audio.volume == 40 && back.audio.mute && !back.video.vsync && back.video.aspect == "stretch");
    CHECK(back.input.keyboard_user == 2 && back.input.left_deadzone == 0.25 && back.keys.at("start") == "Return");
    CHECK(back.keys.at("dpad_up") == "^W");
    // a game's own settings: read over the shared ones for it alone, saved
    // into its table without touching the shared ones or the launcher's
    s.launcher.games = "D:/games";
    CHECK(frontend::save(dir / "config.toml", s, error));
    frontend::Settings own;
    CHECK(frontend::load(dir / "config.toml", own, error, "4D5307E6") && own.game.empty());
    own.game = "4D5307E6", own.audio.volume = 70, own.keys["start"] = "Space";
    CHECK(frontend::save(dir / "config.toml", own, error));
    CHECK(frontend::load(dir / "config.toml", back, error, "4D5307E6") && back.game == "4D5307E6");
    CHECK(back.audio.volume == 70 && back.keys.at("start") == "Space" && back.video.aspect == "stretch");
    CHECK(frontend::load(dir / "config.toml", back, error) && back.game.empty());
    CHECK(back.audio.volume == 40 && back.keys.at("start") == "Return" && back.launcher.games == "D:/games");
    CHECK(frontend::load(dir / "config.toml", back, error, "41560817") && back.game.empty() && back.audio.volume == 40);
    s.audio.volume = 30; // the shared ones saved again keep the game's table
    CHECK(frontend::save(dir / "config.toml", s, error));
    CHECK(frontend::load(dir / "config.toml", back, error, "4D5307E6") && back.audio.volume == 70);
    CHECK(frontend::forget_game(dir / "config.toml", "4D5307E6", error));
    CHECK(frontend::load(dir / "config.toml", back, error, "4D5307E6") && back.game.empty() && back.audio.volume == 30);
    // the launcher's logic: a game given settings of its own and put back,
    // the games folder kept through both
    {
        frontend::LauncherCore core(dir / "config.toml", dir);
        CHECK(core.games_folder() == "D:/games");
        frontend::GameEntry game;
        game.title_id = 0x4D5307E6;
        frontend::Settings edit;
        CHECK(core.load_settings(game, edit, error) && edit.game.empty() && edit.audio.volume == 30);
        CHECK(core.set_own_settings(game, true, edit, error) && edit.game == "4D5307E6" && edit.audio.volume == 30);
        edit.audio.volume = 10;
        frontend::LauncherCore::default_keys(edit);
        CHECK(core.save_settings(edit, error));
        CHECK(frontend::load(dir / "config.toml", back, error, "4D5307E6") && back.audio.volume == 10);
        CHECK(back.keys.at("start") == "X" && back.launcher.games == "D:/games");
        CHECK(frontend::load(dir / "config.toml", back, error) && back.audio.volume == 30);
        CHECK(core.set_own_settings(game, false, edit, error) && edit.game.empty() && edit.audio.volume == 30);
        CHECK(core.listing().games.empty() && !core.run().running && !core.poll());
    }
    std::filesystem::remove_all(dir, ec);

    // the title's name from an executable's XDBF resource: English, else the
    // XDBF's default language
    {
        auto put16 = [](std::vector<uint8_t> &v, uint16_t x) { v.push_back(uint8_t(x >> 8)), v.push_back(uint8_t(x)); };
        auto put32 = [&](std::vector<uint8_t> &v, uint32_t x) { put16(v, uint16_t(x >> 16)), put16(v, uint16_t(x)); };
        auto put64 = [&](std::vector<uint8_t> &v, uint64_t x) { put32(v, uint32_t(x >> 32)), put32(v, uint32_t(x)); };
        auto xstr = [&](const std::string &title) {
            std::vector<uint8_t> v;
            put32(v, 0x58535452), put32(v, 1), put32(v, 0), put16(v, 2);
            put16(v, 0x8001), put16(v, 2), v.push_back('x'), v.push_back('y');
            put16(v, 0x8000), put16(v, uint16_t(title.size()));
            v.insert(v.end(), title.begin(), title.end());
            return v;
        };
        auto xdbf = [&](bool english) {
            std::vector<uint8_t> xstc, strings = xstr(english ? "The Title" : "El Titulo");
            put32(xstc, 0x58535443), put32(xstc, 1), put32(xstc, 4), put32(xstc, english ? 1 : 3);
            std::vector<uint8_t> v;
            put32(v, 0x58444246), put32(v, 0x10000), put32(v, 2), put32(v, 2), put32(v, 1), put32(v, 1);
            put16(v, 1), put64(v, 0x58535443), put32(v, 0), put32(v, uint32_t(xstc.size()));
            put16(v, 3), put64(v, english ? 1 : 3), put32(v, uint32_t(xstc.size())), put32(v, uint32_t(strings.size()));
            put32(v, uint32_t(xstc.size() + strings.size())), put32(v, 0);
            v.insert(v.end(), xstc.begin(), xstc.end());
            v.insert(v.end(), strings.begin(), strings.end());
            return v;
        };
        for (bool english : {true, false}) {
            Xex xex;
            xex.execution.title_id = 0x4D5307E6;
            xex.base_address = 0x82000000;
            xex.image.assign(0x100, 0);
            std::vector<uint8_t> data = xdbf(english);
            xex.image.insert(xex.image.end(), data.begin(), data.end());
            xex.header.assign(0x20, 0);
            put32(xex.header, 4 + 16 * 2);
            for (const char *name : {"other", "4D5307E6"}) {
                std::string n = name;
                n.resize(8, '\0');
                xex.header.insert(xex.header.end(), n.begin(), n.end());
                put32(xex.header, 0x82000100), put32(xex.header, uint32_t(data.size()));
            }
            xex.opt_headers.push_back({xex_key::RESOURCE_INFO, 0x20});
            CHECK(frontend::title_name(xex) == (english ? "The Title" : "El Titulo"));
        }
        Xex none;
        CHECK(frontend::title_name(none).empty());
    }

    // every default binding names a key SDL knows
    for (const frontend::Binding &b : frontend::BINDINGS) {
        std::string list = b.keys;
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find(' ', at);
            std::string token = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? list.size() : end + 1;
            if (token.size() > 1 && (token[0] == '^' || token[0] == '_')) token.erase(0, 1);
            CHECK(SDL_GetScancodeFromName(token.c_str()) != SDL_SCANCODE_UNKNOWN);
        }
    }

    // the fold to stereo: (front + back + center / 2) / 2.5, LFE dropped
    std::vector<float> frame(audio::Output::FRAME_SAMPLES), out(audio::Output::CHANNEL_SAMPLES * 6);
    for (uint32_t ch = 0; ch < 6; ch++)
        for (uint32_t i = 0; i < 256; i++) frame[ch * 256 + i] = float(ch + 1) * 0.1f;
    frontend::downmix_stereo(frame.data(), out.data());
    CHECK(std::fabs(out[0] - (0.1f + 0.5f + 0.15f) / 2.5f) < 1e-6f);
    CHECK(std::fabs(out[511] - (0.2f + 0.6f + 0.15f) / 2.5f) < 1e-6f);
    frontend::interleave_six(frame.data(), out.data());
    CHECK(out[0] == frame[0] && out[3] == frame[3 * 256] && out[6 * 255 + 5] == frame[5 * 256 + 255]);

    // the scripted presses
    game::Press p;
    CHECK(game::parse_press("start@40", p, error) && p.pad.buttons == input::START && p.from_ms == 40000 &&
          p.hold_ms == 300 && p.from_swap == UINT64_MAX);
    CHECK(game::parse_press("A+lt@12.5s:1", p, error) && p.pad.buttons == input::A && p.pad.left_trigger == 255 &&
          p.from_ms == 12500 && p.hold_ms == 1000);
    CHECK(game::parse_press("b@#1100:0.5", p, error) && p.pad.buttons == input::B && p.from_swap == 1100 &&
          p.hold_ms == 500);
    CHECK(!game::parse_press("start", p, error) && !game::parse_press("jump@3", p, error) &&
          !game::parse_press("a@x", p, error));
    std::vector<game::Press> presses(2);
    game::parse_press("start@1", presses[0], error);
    game::parse_press("a@1.2", presses[1], error);
    input::Overlay overlay = game::presses_overlay(presses);
    CHECK(overlay(0, 999).buttons == 0 && overlay(0, 1000).buttons == input::START);
    CHECK(overlay(0, 1250).buttons == (input::START | input::A) && overlay(1, 1250).buttons == 0);
    CHECK(overlay(0, 1500).buttons == 0 && !game::presses_overlay({}));
    // at a swap: from when the swap is first seen, for its hold
    uint64_t swaps = 5;
    std::vector<game::Press> at_swap(1);
    game::parse_press("y@#10", at_swap[0], error);
    input::Overlay by_swap = game::presses_overlay(at_swap, [&swaps] { return swaps; });
    CHECK(by_swap(0, 100).buttons == 0);
    swaps = 10;
    CHECK(by_swap(0, 5000).buttons == input::Y && by_swap(0, 5299).buttons == input::Y);
    CHECK(by_swap(0, 5300).buttons == 0);

    // the keyboard's driver: off for other users, a controller for its own
    frontend::Controllers controllers(s, nullptr);
    auto keyboard = controllers.keyboard();
    input::State state;
    CHECK(keyboard->state(0, 0, state) == input::DEVICE_NOT_CONNECTED);
    CHECK(keyboard->state(2, 0, state) == input::SUCCESS && state.packet_number == 1 && state.gamepad.buttons == 0);
    CHECK(keyboard->state(2, 0, state) == input::SUCCESS && state.packet_number == 2);
    auto pads = controllers.gamepads();
    CHECK(pads->state(0, 0, state) == input::DEVICE_NOT_CONNECTED);

    // the launcher's screens: what they say, apart from how they are drawn
    CHECK(frontend::game_status(true, false, false) == frontend::GameStatus::Ready);
    CHECK(frontend::game_status(false, true, true) == frontend::GameStatus::Ready);
    CHECK(frontend::game_status(false, false, false) == frontend::GameStatus::NeedsPreparing);
    CHECK(frontend::game_status(false, false, true) == frontend::GameStatus::Interpreted);
    CHECK(frontend::game_status(true, false, false, true) == frontend::GameStatus::Stale);
    CHECK(frontend::game_status(false, false, false, true) == frontend::GameStatus::NeedsPreparing);
    CHECK(std::string(frontend::status_class(frontend::GameStatus::Stale)) == "stale");
    {
        // which player: a cached library means the generic one, else the player carrying the code, else the generic
        frontend::PlayerBinary generic, carrying, other;
        generic.path = "player", generic.generic = true;
        carrying.path = "player_game", carrying.modules = {{0x1234, "default.xex"}};
        other.path = "player_other", other.modules = {{0x9999, "default.xex"}};
        const std::vector<frontend::PlayerBinary> all{carrying, generic, other};
        frontend::GameEntry game;
        game.digest = 0x1234;
        CHECK(frontend::player_for(all, game, true)->generic);
        CHECK(frontend::player_for(all, game, false)->path == "player_game");
        game.digest = 0x5555;
        CHECK(frontend::player_for(all, game, false)->generic);
        const std::vector<frontend::PlayerBinary> no_generic{carrying, other};
        game.digest = 0x1234;
        CHECK(frontend::player_for(no_generic, game, true)->path == "player_game");
        game.digest = 0x5555;
        CHECK(frontend::player_for(no_generic, game, false) == nullptr);
        // the plan is that choice with the arguments, apart from how it is started
        frontend::LaunchPlan plan;
        game.digest = 0x1234;
        CHECK(frontend::plan_launch(all, game, true, {"--quit-after", "5"}, plan));
        CHECK(plan.program == "player" && plan.generic && plan.from_library && plan.args.size() == 2);
        CHECK(frontend::plan_launch(all, game, false, {}, plan));
        CHECK(plan.program == "player_game" && !plan.generic && !plan.from_library);
        game.digest = 0x5555;
        CHECK(!frontend::plan_launch(no_generic, game, false, {}, plan));
    }
    CHECK(frontend::format_duration(42) == "0:42" && frontend::format_duration(725) == "12:05");
    CHECK(frontend::format_duration(3729) == "1:02:09" && frontend::format_duration(-3) == "0:00");
    CHECK(frontend::version_text(0x10000100) == "1.0.1.0");
    CHECK(frontend::html_escape("a<b>&\"c") == "a&lt;b&gt;&amp;&quot;c");
    {
        const frontend::Accent a = frontend::accent_from_hex("#4c9bff"), bad = frontend::accent_from_hex("nonsense");
        CHECK(a.base == "#4c9bff" && a.a40 == "#4c9bff66" && a.on == "#1a0f08" && frontend::accent_from_hex("#2a3fa0").on == "#ffffff");
        CHECK(bad.base == frontend::accent_choices()[0]);
        const std::string css = frontend::apply_tokens("x {accent} {accent-hi} {accent-lo} {accent25} {on-accent} {accent}", a);
        CHECK(css.find('{') == std::string::npos && css.starts_with("x #4c9bff #"));
        CHECK(frontend::accent_from_hex("#ffc247").on == "#1a0f08"); // a dark label on a light accent
    }
    {
        const auto caps = frontend::parse_keys("^A Return");
        CHECK(caps.size() == 2 && caps[0].shift && caps[0].label == "A" && caps[1].label == "Enter" && !caps[1].shift);
        CHECK(frontend::parse_keys("_W")[0].no_shift && frontend::parse_keys("").empty());
        for (const frontend::Binding &b : frontend::BINDINGS) CHECK(std::string(frontend::binding_info(b.name).label) != "?");
    }
    {
        // a grid of two rows of three, a wide row below, and one off to the right
        std::vector<frontend::NavRect> r = {{0, 0, 100, 100},     {120, 0, 100, 100},   {240, 0, 100, 100}, {0, 120, 100, 100},
                                            {120, 120, 100, 100}, {240, 120, 100, 100}, {0, 260, 340, 50},  {400, 0, 100, 100}};
        using frontend::NavDir;
        CHECK(frontend::nav_pick(r, 0, NavDir::Right) == 1 && frontend::nav_pick(r, 1, NavDir::Down) == 4);
        CHECK(frontend::nav_pick(r, 4, NavDir::Up) == 1 && frontend::nav_pick(r, 5, NavDir::Left) == 4);
        CHECK(frontend::nav_pick(r, 4, NavDir::Down) == 6 && frontend::nav_pick(r, 6, NavDir::Up) == 4);
        CHECK(frontend::nav_pick(r, 2, NavDir::Right) == 7 && frontend::nav_pick(r, 0, NavDir::Up) == -1);
        CHECK(frontend::nav_pick(r, 0, NavDir::Left) == -1 && frontend::nav_pick(r, 99, NavDir::Left) == -1);
    }
    {
        // the windows' structure: escaped titles, every binding a row, the settings' keys
        frontend::RowInfo row;
        row.title = "A <b> & C", row.id_text = "4D5307E6", row.index = 3;
        const std::string rows_html = frontend::table_row_html(row, true);
        CHECK(rows_html.find("A &lt;b&gt; &amp; C") != std::string::npos && rows_html.find("<b>") == std::string::npos);
        CHECK(rows_html.find("data-index=\"3\"") != std::string::npos && rows_html.find(" sel") != std::string::npos);
        CHECK(rows_html.find("data-row-options") != std::string::npos); // the Options button
        const std::string head = frontend::table_head_html(frontend::Column::Version, false);
        CHECK(head.find("data-arg=\"version\"") != std::string::npos && head.find("class=\"th c-ver sorted\"") != std::string::npos);
        CHECK(head.find("data-arg=\"title\"") != std::string::npos && head.find("c-opts") != std::string::npos);
        frontend::Settings own;
        frontend::SettingsPage page;
        page.settings = &own, page.accent = "#ff7a45", page.theme = "system";
        std::string settings;
        for (int i = 0; i < frontend::SETTINGS_TABS; i++) settings += frontend::settings_page_html(frontend::SettingsTab(i), page);
        for (const char *key : {"video.fullscreen", "video.aspect", "audio.volume", "audio.mute", "input.vibration", "input.keyboard", "theme"})
            CHECK(settings.find(std::string("data-key=\"") + key + "\"") != std::string::npos);
        size_t binds = 0;
        for (size_t at = settings.find("data-action=\"capture\""); at != std::string::npos; at = settings.find("data-action=\"capture\"", at + 1)) binds++;
        CHECK(binds == 25);
        CHECK(frontend::settings_tabs_html(frontend::SettingsTab::Keyboard).find("tab f active\" data-action=\"tab\" data-arg=\"keyboard\"") != std::string::npos);
        CHECK(frontend::about_body_html().find("FFmpeg") != std::string::npos && frontend::about_body_html().find("Lucide") != std::string::npos);
        // menus: an item's action, a greyed one without, a check mark
        frontend::MenuItem play, stop;
        play.action = "play", play.label = "Play", play.shortcut = "Enter", play.icon = "play";
        stop.action = "stop", stop.label = "Stop", stop.enabled = false;
        const std::string menu = frontend::menu_html({play, frontend::menu_separator(), stop});
        CHECK(menu.find("data-action=\"play\"") != std::string::npos && menu.find("data-action=\"stop\"") == std::string::npos);
        CHECK(menu.find("class=\"msep\"") != std::string::npos && menu.find("Enter") != std::string::npos);
        // icons: Lucide's private-use glyphs, and the template tokens made spans
        CHECK(frontend::icon_glyph("play") == "\xEE\x85\x80" && frontend::icon_glyph("nonsense").empty());
        CHECK(frontend::expand_icons("a{i:x}b") == "a<span class=\"ic\">\xEE\x86\xB2</span>b");
    }
    {
        // the games table: the sort, the order and the filter
        frontend::Column c = frontend::Column::Title;
        bool ascending = false;
        frontend::parse_sort("-version", c, ascending);
        CHECK(c == frontend::Column::Version && !ascending && frontend::sort_text(c, ascending) == "-version");
        frontend::parse_sort("bogus", c, ascending);
        CHECK(c == frontend::Column::Title && ascending);
        std::vector<frontend::RowKey> rows(4);
        rows[0].title = "delta", rows[0].version = 3, rows[0].status = 1, rows[0].id_text = "58460001", rows[0].file = "One.iso";
        rows[1].title = "Charlie", rows[1].version = 9, rows[1].status = 0, rows[1].id_text = "58460002", rows[1].file = "Two.iso";
        rows[2].title = "Bravo", rows[2].version = 3, rows[2].status = 0, rows[2].id_text = "58460003", rows[2].file = "Three.iso";
        rows[3].title = "alpha", rows[3].version = 1, rows[3].status = 1, rows[3].id_text = "58460004", rows[3].file = "Four.iso";
        CHECK((frontend::sorted_rows(rows, frontend::Column::Title, true) == std::vector<size_t>{3, 2, 1, 0})); // case aside
        CHECK((frontend::sorted_rows(rows, frontend::Column::Title, false) == std::vector<size_t>{0, 1, 2, 3}));
        CHECK((frontend::sorted_rows(rows, frontend::Column::Version, true) == std::vector<size_t>{3, 2, 0, 1})); // ties by title
        CHECK((frontend::sorted_rows(rows, frontend::Column::Status, true) == std::vector<size_t>{2, 1, 3, 0}));
        CHECK(frontend::row_matches(rows[1], "") && frontend::row_matches(rows[1], "  CHARLIE ") && frontend::row_matches(rows[1], "5846 two"));
        CHECK(!frontend::row_matches(rows[1], "charlie bravo") && !frontend::row_matches(rows[0], "two"));
        // the themes: every token filled, light and dark different, the selection the accent's
        const frontend::Accent accent = frontend::accent_from_hex("#4c9bff");
        const std::string css = "{bg} {text} {sel} {border} {accent} {on-accent} {shadow} {good}";
        const std::string dark = frontend::apply_tokens(css, accent, frontend::palette(true, accent));
        const std::string light = frontend::apply_tokens(css, accent, frontend::palette(false, accent));
        CHECK(dark.find('{') == std::string::npos && light.find('{') == std::string::npos && dark != light);
        CHECK(dark.find("#4c9bff40") != std::string::npos && light.find("#4c9bff2c") != std::string::npos);
        CHECK(frontend::theme_from_text("dark") == frontend::ThemeChoice::Dark && frontend::theme_from_text("x") == frontend::ThemeChoice::System);
        CHECK(std::string(frontend::theme_text(frontend::ThemeChoice::Light)) == "light");
    }
    {
        // the backdrop's ribbons: the mesh built on the CPU
        const float tint[3] = {0.33f, 0.66f, 0.16f};
        std::vector<ui::RibbonVertex> v, v2, v3;
        std::vector<uint32_t> i, i2, i3, f, f2, f3;
        ui::build_ribbons(5.f, 1240, 760, tint, 1.f, v, i, f);
        ui::build_ribbons(5.f, 1240, 760, tint, 1.f, v2, i2, f2);
        ui::build_ribbons(9.f, 1240, 760, tint, 1.f, v3, i3, f3);
        CHECK(!v.empty() && !i.empty() && i.size() % 3 == 0 && !f.empty() && f.size() % 3 == 0);
        bool ok = true, inside = false;
        for (uint32_t k : i) ok = ok && k < v.size();
        for (uint32_t k : f) ok = ok && k < v.size();
        for (const ui::RibbonVertex &p : v) ok = ok && p.depth >= 0.f && p.depth <= 1.f;
        for (const ui::RibbonVertex &p : v) {
            ok = ok && std::isfinite(p.x) && std::isfinite(p.y) && p.r <= p.a && p.g <= p.a && p.b <= p.a; // premultiplied
            inside = inside || (p.x > 0 && p.x < 1240 && p.y > 0 && p.y < 760);
        }
        CHECK(ok && inside);
        CHECK(v.size() == v2.size() && i == i2 && f == f2 && v[100].x == v2[100].x && v[100].y == v2[100].y); // a pure function
        CHECK(v3.size() == v.size() && (v3[100].x != v[100].x || v3[100].y != v[100].y));          // the clock moves them
        std::vector<ui::RibbonVertex> none;
        std::vector<uint32_t> no, no2;
        ui::build_ribbons(5.f, 1240, 760, tint, 0.f, none, no, no2);
        CHECK(none.empty() && no.empty() && no2.empty()); // strength 0: no ribbons
    }
    {
        // the launcher's own section saved in config.toml: the folder, the look, the window and the sort
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "xf_frontend_test_accent.toml";
        std::filesystem::remove(file);
        frontend::Settings a, b;
        a.launcher.games = "D:/games", a.launcher.accent = "#9b7bff", a.launcher.theme = "light";
        a.launcher.sort = "-status";
        a.launcher.backdrop = "still";
        a.launcher.x = -1200, a.launcher.y = 40, a.launcher.width = 1500, a.launcher.height = 900, a.launcher.maximized = true;
        std::string err;
        CHECK(frontend::save(file, a, err) && frontend::load(file, b, err));
        CHECK(b.launcher.games == "D:/games" && b.launcher.accent == "#9b7bff" && b.launcher.theme == "light");
        CHECK(b.launcher.sort == "-status");
        CHECK(b.launcher.backdrop == "still");
        CHECK(frontend::Settings().launcher.backdrop == "animated");
        CHECK(frontend::backdrop_choices().size() == 3);
        CHECK(b.launcher.x == -1200 && b.launcher.y == 40 && b.launcher.width == 1500 && b.launcher.height == 900 && b.launcher.maximized);
        std::filesystem::remove(file);
    }
    {
        // a game being prepared: through its stages, failing, and cancelled
        using build::Stage;
        auto wait = [](frontend::PrepareJob &job) {
            for (int i = 0; i < 500 && job.active(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        };
        frontend::PrepareJob job;
        CHECK(!job.state().started);
        job.start("Game", [](build::Progress &p, std::string &) {
            p.report(Stage::Reading, 0, 1, "default.xex");
            p.report(Stage::Compiling, 3, 6, "file_003.cpp");
            p.report(Stage::Done, 0, 0, "");
            return true;
        });
        wait(job);
        frontend::PrepareJob::State st = job.state();
        CHECK(st.started && st.finished && st.ok && !st.running && st.stage == Stage::Done && st.fraction == 1);
        CHECK(job.log_from(0).size() >= 4 && job.log_from(0).front() == "Preparing Game" && job.log_from(0).back() == "Done.");
        CHECK(st.log_lines == job.log_from(0).size() && job.log_from(st.log_lines).empty());
        job.start("Game", [](build::Progress &p, std::string &error) {
            p.report(Stage::Compiling, 3, 6, "file_003.cpp");
            error = "compiler said no";
            p.report(Stage::Failed, 0, 0, error);
            return false;
        });
        wait(job);
        st = job.state();
        CHECK(st.finished && !st.ok && !st.cancelled && st.error == "compiler said no" && st.stage == Stage::Failed);
        CHECK(job.log_from(0).back() == "Failed: compiler said no");
        // half of compiling is the stages before it and half its weight
        job.start("Game", [](build::Progress &p, std::string &) {
            p.report(Stage::Compiling, 1, 2, "x");
            while (!p.cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return false;
        });
        for (int i = 0; i < 500 && job.state().stage != Stage::Compiling; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        st = job.state();
        CHECK(st.running && std::fabs(st.fraction - (0.03 + 0.07 + 0.15 + 0.68 / 2)) < 1e-9);
        job.cancel();
        wait(job);
        CHECK(job.state().cancelled && !job.state().ok);
        // the placeholder build of today ends in a failure at once
        job.start("Game", frontend::PrepareJob::build_runner("no.iso", std::filesystem::temp_directory_path()));
        wait(job);
        CHECK(job.state().finished && (job.state().ok || !job.state().error.empty()));
    }
    return finish("frontend");
}
