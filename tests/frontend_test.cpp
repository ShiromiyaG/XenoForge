// The player's parts that need no window: config.toml written and read
// back (a game's own settings too), the title's name from an XDBF (the
// launcher's), Xenia's default keyboard bindings all naming SDL keys, the
// sound's fold to stereo and its six channels interleaved, and the scripted
// presses (tools/game.h) parsed and held at their times.

#include "check.h"

#include "../frontend/discord_presence.h"
#include "../frontend/launcher_core.h"
#include "../frontend/launcher_html.h"
#include "../frontend/launcher_model.h"
#include "../frontend/prepare_job.h"
#include "../frontend/pointer_hider.h"
#include "../frontend/presenter.h"
#include "../frontend/library.h"
#include "../frontend/sdl_audio.h"
#include "../frontend/sdl_input.h"
#include "../frontend/ui/cursor_file.h"
#include "../frontend/ui/ribbons.h"
#include "../frontend/ui_sound.h"
#include "../frontend/settings.h"
#include "../tools/game.h"
#include "xenoforge/render_scale.h"

#include <cctype>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

using namespace xf;

int main() {
    {
        // Discord Rich Presence: the frames and the JSON
        namespace d = frontend::discord;
        const std::string hs = d::encode_frame(d::OP_HANDSHAKE, d::handshake_json("42"));
        CHECK(hs == std::string("\0\0\0\0\x18\0\0\0{\"v\":1,\"client_id\":\"42\"}", 8 + 24));
        const std::string close = d::encode_frame(d::OP_CLOSE, "{}");
        CHECK(close.size() == 10 && close[0] == 2 && close[4] == 2 && close[1] == 0 && close[5] == 0);
        const std::string big = d::encode_frame(d::OP_FRAME, std::string(300, 'x'));
        CHECK(big[0] == 1 && big[4] == char(300 & 255) && big[5] == 1 && big.size() == 308);
        // a made-up title with quotes, a backslash, a control character and non-ASCII
        const std::string title = "The \"Test\" \\ Game\n \xC3\xA9\xE2\x84\xA2";
        const std::string act = d::activity_json(1234, title, "XenoForge", 1700000000, "7");
        CHECK(act == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1234,\"activity\":{\"details\":"
                     "\"The \\\"Test\\\" \\\\ Game\\n \xC3\xA9\xE2\x84\xA2\",\"state\":\"XenoForge\","
                     "\"timestamps\":{\"start\":1700000000},\"assets\":{\"large_image\":\"xenoforge\","
                     "\"large_text\":\"XenoForge\"}}},\"nonce\":\"7\"}");
        // a picture by its address as the large image, and the box art's address by a made-up title ID
        CHECK(d::activity_json(1, "ab", "s", 2, "3", "http://x/y.jpg", "ab").find("\"large_image\":\"http://x/y.jpg\",\"large_text\":\"ab\"") != std::string::npos);
        CHECK(d::activity_json(1, "XenoForge", "", 2, "3", "k", "Some Game", "Some Game").find("{\"name\":\"Some Game\",\"details\":\"XenoForge\",\"timestamps\"") != std::string::npos);
        CHECK(d::title_art_url("58460001") == "http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d80258460001/1033/boxartlg.jpg");
        CHECK(d::title_art_url("5846000") == "" && d::title_art_url("5846000G") == "");
        CHECK(d::clear_json(1234, "8") == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1234},\"nonce\":\"8\"}");
        CHECK(d::json_string("a\x01" "b") == "\"a\\u0001b\"");
        CHECK(d::json_string("bad\xFF" "x") == "\"bad\xEF\xBF\xBD" "x\"");     // invalid UTF-8 replaced
        CHECK(d::json_string("\xC3\xA9\xC3\xA9", 3) == "\"\xC3\xA9\"");      // cut on a character boundary
        // no client ID: nothing starts
        d::Presence off;
        CHECK(!off.start("", "x") && !off.connected());
    }
    // the pointer over the game's picture: gone after 2 s at rest (windowed), at once in fullscreen, back on a move, and
    // always there for the overlay, the guide and the dialogs
    {
        frontend::PointerHider p;
        p.moved(10000);
        CHECK(p.visible(10500, false, false));
        CHECK(!p.visible(12100, false, false));
        CHECK(p.visible(12100, true, false));
        CHECK(p.visible(10100, false, true));
        CHECK(!p.visible(10300, false, true));
        p.moved(20000);
        CHECK(p.visible(20001, false, false));
    }
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
    CHECK(frontend::load(dir / "config.toml", own, error, "5846000A") && own.game.empty());
    own.game = "5846000A", own.audio.volume = 70, own.keys["start"] = "Space";
    CHECK(frontend::save(dir / "config.toml", own, error));
    CHECK(frontend::load(dir / "config.toml", back, error, "5846000A") && back.game == "5846000A");
    CHECK(back.audio.volume == 70 && back.keys.at("start") == "Space" && back.video.aspect == "stretch");
    CHECK(frontend::load(dir / "config.toml", back, error) && back.game.empty());
    CHECK(back.audio.volume == 40 && back.keys.at("start") == "Return" && back.launcher.games == "D:/games");
    CHECK(frontend::load(dir / "config.toml", back, error, "41560817") && back.game.empty() && back.audio.volume == 40);
    s.audio.volume = 30; // the shared ones saved again keep the game's table
    CHECK(frontend::save(dir / "config.toml", s, error));
    CHECK(frontend::load(dir / "config.toml", back, error, "5846000A") && back.audio.volume == 70);
    CHECK(frontend::forget_game(dir / "config.toml", "5846000A", error));
    CHECK(frontend::load(dir / "config.toml", back, error, "5846000A") && back.game.empty() && back.audio.volume == 30);
    // the picture options (settings.h's video section): saved and read back,
    // per game too, the values out of range or unknown made valid
    {
        frontend::Settings v;
        CHECK(v.video.resolution_scale == 100 && v.video.min_scalable == 16 && v.video.upscaler == "none");
        v.video.resolution_scale = 150, v.video.min_scalable = 32, v.video.upscaler = "fsr";
        v.video.fsr_sharpness = 0.5, v.video.cas_sharpness = 0.25, v.video.antialiasing = "fxaa_extreme";
        v.video.anisotropy = 16, v.video.texture_filter = "trilinear", v.video.frame_graph = true;
        v.video.aspect = "integer";
        CHECK(frontend::save(dir / "video.toml", v, error));
        frontend::Settings r;
        CHECK(frontend::load(dir / "video.toml", r, error));
        CHECK(r.video.resolution_scale == 150 && r.video.min_scalable == 32 && r.video.upscaler == "fsr");
        CHECK(r.video.fsr_sharpness == 0.5 && r.video.cas_sharpness == 0.25 && r.video.antialiasing == "fxaa_extreme");
        CHECK(r.video.anisotropy == 16 && r.video.texture_filter == "trilinear" && r.video.frame_graph);
        CHECK(r.video.aspect == "integer");
        // a game's own scale over the shared one
        frontend::Settings g = r;
        g.game = "5846000A", g.video.resolution_scale = 300;
        CHECK(frontend::save(dir / "video.toml", g, error));
        CHECK(frontend::load(dir / "video.toml", r, error, "5846000A") && r.video.resolution_scale == 300);
        CHECK(frontend::load(dir / "video.toml", r, error) && r.video.resolution_scale == 150);
        // made valid: 25% steps within 50-800, the known choices, powers of two
        CHECK(frontend::clamp_resolution_scale(10) == 50 && frontend::clamp_resolution_scale(112) == 100);
        CHECK(frontend::clamp_resolution_scale(113) == 125 && frontend::clamp_resolution_scale(5000) == 800);
        frontend::Settings bad;
        bad.video.upscaler = "dlss", bad.video.anisotropy = 6, bad.video.fsr_sharpness = 9, bad.video.aspect = "x";
        bad.video.min_scalable = -4;
        frontend::clamp_video(bad);
        CHECK(bad.video.upscaler == "none" && bad.video.anisotropy == 4 && bad.video.fsr_sharpness == 2.0);
        CHECK(bad.video.aspect == "auto" && bad.video.min_scalable == 0);
        // the shared choice lists (the launcher's Video tab and the player's pages)
        CHECK(frontend::video_choices("video.resolution_scale").size() == 31);
        CHECK(frontend::set_video_value(bad, "video.resolution_scale", "175") && bad.video.resolution_scale == 175);
        CHECK(!frontend::set_video_value(bad, "video.resolution_scale", "130"));
        CHECK(frontend::video_value(bad, "video.upscaler") == "none" && frontend::video_needs_restart("video.min_scalable"));
        CHECK(!frontend::video_needs_restart("video.upscaler"));
        // the output resampling and SMAA's presets: listed, kept, an unknown one back to the default
        CHECK(frontend::Settings().video.resampling == "auto");
        CHECK(frontend::video_choices("video.resampling").size() == 7);
        CHECK(frontend::set_video_value(bad, "video.resampling", "catmull_rom") &&
              frontend::video_value(bad, "video.resampling") == "catmull_rom");
        CHECK(frontend::set_video_value(bad, "video.antialiasing", "smaa_ultra") && bad.video.antialiasing == "smaa_ultra");
        CHECK(!frontend::set_video_value(bad, "video.resampling", "lanczos"));
        CHECK(frontend::save(dir / "resample.toml", bad, error));
        frontend::Settings rs;
        CHECK(frontend::load(dir / "resample.toml", rs, error) && rs.video.resampling == "catmull_rom" &&
              rs.video.antialiasing == "smaa_ultra");
        rs.video.resampling = "x";
        frontend::clamp_video(rs);
        CHECK(rs.video.resampling == "auto");
    }
    // no lost updates: two processes (the launcher and a player) load the file, each changes a few fields and
    // saves; the other's fields survive, whichever saves first
    {
        const auto file = dir / "shared.toml";
        frontend::Settings first;
        CHECK(frontend::save(file, first, error));
        frontend::Settings launcher, player;
        CHECK(frontend::load(file, launcher, error) && frontend::load(file, player, error));
        launcher.video.upscaler = "fsr", launcher.video.antialiasing = "fxaa", launcher.video.resolution_scale = 200;
        CHECK(frontend::save(file, launcher, error));
        player.audio.volume = 55; // the player closes having changed only the volume
        CHECK(frontend::save(file, player, error));
        frontend::Settings r;
        CHECK(frontend::load(file, r, error));
        CHECK(r.video.upscaler == "fsr" && r.video.antialiasing == "fxaa" && r.video.resolution_scale == 200);
        CHECK(r.audio.volume == 55);
        // the reverse: the launcher saves after the player did
        player.audio.volume = 55, player.video.smooth = false;
        CHECK(frontend::save(file, player, error));
        launcher.video.upscaler = "cas";
        CHECK(frontend::save(file, launcher, error));
        CHECK(frontend::load(file, r, error));
        CHECK(r.video.upscaler == "cas" && !r.video.smooth && r.audio.volume == 55 && r.video.resolution_scale == 200);
        // a field both changed: the later save wins; the one saved twice keeps writing only what changed since
        launcher.audio.volume = 20;
        CHECK(frontend::save(file, launcher, error));
        player.video.antialiasing = "fxaa_extreme";
        CHECK(frontend::save(file, player, error));
        CHECK(frontend::load(file, r, error));
        CHECK(r.audio.volume == 20 && r.video.antialiasing == "fxaa_extreme" && r.video.upscaler == "cas");
        // a game's own table: its fields merged too
        frontend::Settings a, b;
        CHECK(frontend::load(file, a, error, "5846000A") && frontend::load(file, b, error, "5846000A"));
        a.game = b.game = "5846000A";
        a.video.antialiasing = "fxaa", b.audio.volume = 10;
        CHECK(frontend::save(file, a, error));
        CHECK(frontend::save(file, b, error));
        CHECK(frontend::load(file, r, error, "5846000A") && r.game == "5846000A");
        CHECK(r.video.antialiasing == "fxaa" && r.audio.volume == 10);
    }
    // the resolution scale's arithmetic (render_scale.h)
    {
        using xf::render::Scale;
        CHECK(Scale::from_percent(150).q4 == 6 && Scale::from_percent(40).q4 == 2 && Scale::from_percent(900).q4 == 32);
        CHECK(Scale{6}.percent() == 150 && Scale{4}.one() && !Scale{8}.one());
        // a 1280x720 surface at 125%, 150%, 50%; lengths not a multiple of 4 rounded up for a surface's size
        CHECK(Scale{5}.up(1280) == 1600 && Scale{5}.up(720) == 900 && Scale{6}.down(8) == 12 && Scale{2}.up(5) == 3);
        // EDRAM's tiles: 80x16 at 100%, whole at every quarter
        CHECK(Scale{4}.tile_dwords() == 1280 && Scale{5}.tile_width() == 100 && Scale{5}.tile_height() == 20);
        CHECK(Scale{2}.tile_dwords() == 320 && Scale{32}.tile_dwords() == 81920);
        // the host pixel a console pixel is taken from: its center's (at 300%, 1 of 0-2; at 200%, 1 of 0-1)
        CHECK(Scale::map(0, Scale{4}, Scale{12}) == 1 && Scale::map(5, Scale{4}, Scale{12}) == 16);
        CHECK(Scale::map(0, Scale{4}, Scale{8}) == 1 && Scale::map(3, Scale{4}, Scale{2}) == 1);
        CHECK(Scale::map(7, Scale{8}, Scale{4}) == 3 && Scale::map(4, Scale{4}, Scale{4}) == 4);
        CHECK(xf::render::scalable(16, 16, 16) && !xf::render::scalable(1280, 8, 16) && xf::render::scalable(1, 1, 0));
        // a picture box filtered down: 2x2 blocks averaged
        std::vector<uint8_t> four = {0, 0, 0, 255, 100, 100, 100, 255, 200, 200, 200, 255, 100, 100, 100, 255};
        std::vector<uint8_t> one = xf::render::resample_box(four, 2, 2, 1, 1);
        CHECK(one.size() == 4 && one[0] == 100 && one[3] == 255);
        CHECK(xf::render::resample_box(four, 2, 2, 4, 4).size() == 64); // made larger: nearest
    }
    // the window's picture placement and FSR's passes (presenter.h)
    {
        frontend::Rect r = frontend::fit_picture(1920, 1080, 1280, 720, frontend::Fit::aspect);
        CHECK(r.x == 0 && r.y == 0 && r.width == 1920 && r.height == 1080);
        r = frontend::fit_picture(1920, 1200, 1280, 720, frontend::Fit::aspect);
        CHECK(r.width == 1920 && r.height == 1080 && r.y == 60);
        r = frontend::fit_picture(2000, 1200, 640, 360, frontend::Fit::integer);
        CHECK(r.width == 1920 && r.height == 1080 && r.x == 40 && r.y == 60);
        r = frontend::fit_picture(1000, 600, 1280, 720, frontend::Fit::integer); // not even 1x: fitted
        CHECK(r.width == 1000 && r.height == 563);
        r = frontend::fit_picture(800, 800, 1280, 720, frontend::Fit::stretch);
        CHECK(r.width == 800 && r.height == 800);
        auto passes = frontend::easu_passes(640, 360, 2560, 1440);
        CHECK(passes.size() == 2 && passes[0].first == 1280 && passes[1].first == 2560 && passes[1].second == 1440);
        CHECK(frontend::easu_passes(1280, 720, 1920, 1080).size() == 1);
        CHECK(frontend::easu_passes(1280, 720, 1280, 720).empty() && frontend::easu_passes(2560, 1440, 1920, 1080).empty());
        CHECK(frontend::easu_passes(100, 100, 10000, 100, 2).size() == 2 &&
              frontend::easu_passes(100, 100, 10000, 100, 2)[1].first == 10000);
        CHECK(std::abs(frontend::cas_sharpness_constant(0.0f) + 0.125f) < 1e-6f &&
              std::abs(frontend::cas_sharpness_constant(1.0f) + 0.2f) < 1e-6f);
    }
    // the launcher's logic: a game given settings of its own and put back,
    // the games folder kept through both
    {
        frontend::LauncherCore core(dir / "config.toml", dir);
        CHECK(core.games_folder() == "D:/games");
        frontend::GameEntry game;
        game.title_id = 0x5846000A;
        frontend::Settings edit;
        CHECK(core.load_settings(game, edit, error) && edit.game.empty() && edit.audio.volume == 30);
        CHECK(core.set_own_settings(game, true, edit, error) && edit.game == "5846000A" && edit.audio.volume == 30);
        edit.audio.volume = 10;
        frontend::LauncherCore::default_keys(edit);
        CHECK(core.save_settings(edit, error));
        CHECK(frontend::load(dir / "config.toml", back, error, "5846000A") && back.audio.volume == 10);
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
            xex.execution.title_id = 0x5846000A;
            xex.base_address = 0x82000000;
            xex.image.assign(0x100, 0);
            std::vector<uint8_t> data = xdbf(english);
            xex.image.insert(xex.image.end(), data.begin(), data.end());
            xex.header.assign(0x20, 0);
            put32(xex.header, 4 + 16 * 2);
            for (const char *name : {"other", "5846000A"}) {
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
        row.title = "A <b> & C", row.id_text = "5846000A", row.index = 3;
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
        for (const char *key : {"video.fullscreen", "video.aspect", "audio.volume", "audio.mute", "input.vibration", "input.keyboard", "theme", "system.language", "system.country",
                                 "system.resolution", "system.widescreen", "system.refresh", "saves.format"})
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
        // the console look: every token of the shipped stylesheets is filled in both themes, and the two differ
        for (const char *folder : {"ui", "../frontend/ui/assets", "frontend/ui/assets"}) {
            std::string all;
            for (const char *file : {"theme.rcss", "launcher.rcss", "settings.rcss"}) {
                std::ifstream in(std::string(folder) + "/" + file, std::ios::binary);
                all.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (all.find("#spage") == std::string::npos) continue;
            const std::string d = frontend::apply_tokens(all, accent, frontend::palette(true, accent));
            const std::string l = frontend::apply_tokens(all, accent, frontend::palette(false, accent));
            // (the rules' own braces remain: a token is '{', lower-case letters, digits and dashes, '}')
            auto unfilled = [](const std::string &css) {
                for (size_t at = css.find('{'); at != std::string::npos; at = css.find('{', at + 1)) {
                    const size_t end = css.find('}', at);
                    if (end == std::string::npos || end - at > 24) continue;
                    bool name = end > at + 1;
                    for (size_t i = at + 1; i < end; i++) name = name && (std::islower(uint8_t(css[i])) || std::isdigit(uint8_t(css[i])) || css[i] == '-');
                    if (name) return css.substr(at, end - at + 1);
                }
                return std::string();
            };
            CHECK(unfilled(d).empty() && unfilled(l).empty() && d != l);
            break;
        }
    }
    {
        // the cursors' files (ui/cursor_file.h): a made 2x2 .cur and a two-frame .ani read back, then the shipped ones
        auto cur = [](int hot_x, int hot_y, uint8_t alpha) {
            std::vector<uint8_t> d;
            auto w16 = [&](uint32_t v) { d.push_back(uint8_t(v)), d.push_back(uint8_t(v >> 8)); };
            auto w32 = [&](uint32_t v) { w16(v & 0xffff), w16(v >> 16); };
            w16(0), w16(2), w16(1);
            d.push_back(2), d.push_back(2), d.push_back(0), d.push_back(0), w16(uint32_t(hot_x)), w16(uint32_t(hot_y));
            w32(40 + 16 + 8), w32(22);
            w32(40), w32(2), w32(4), w16(1), w16(32), w32(0), w32(24), w32(0), w32(0), w32(0), w32(0);
            const uint8_t bottom_up[] = {0, 0, 255, alpha, 0, 255, 0, alpha, /* then the top row: */ 255, 0, 0, alpha, 255, 255, 255, alpha};
            d.insert(d.end(), bottom_up, bottom_up + 16);
            for (int i = 0; i < 8; i++) d.push_back(0); // the mask
            return d;
        };
        ui::CursorImage image;
        std::string why;
        const std::vector<uint8_t> one = cur(1, 0, 255);
        CHECK(ui::parse_cursor(one.data(), one.size(), image, why));
        CHECK(image.width == 2 && image.height == 2 && image.hot_x == 1 && image.hot_y == 0);
        CHECK(image.rgba[0] == 0 && image.rgba[1] == 0 && image.rgba[2] == 255 && image.rgba[3] == 255); // top left: blue (the file is bottom-up)
        CHECK(image.rgba[8] == 255 && image.rgba[9] == 0 && image.rgba[10] == 0);                      // bottom left: red
        const std::vector<uint8_t> clear = cur(0, 0, 0); // no alpha at all: the mask (all zero) makes it opaque
        CHECK(ui::parse_cursor(clear.data(), clear.size(), image, why) && image.rgba[3] == 255);
        const ui::CursorImage big = ui::scaled_cursor(image, 2);
        CHECK(big.width == 4 && big.height == 4 && big.rgba.size() == 64);
        std::vector<uint8_t> broken = one;
        broken.resize(30);
        CHECK(!ui::parse_cursor(broken.data(), broken.size(), image, why) && !why.empty());
        // an .ani of two frames of 100 ms
        std::vector<uint8_t> ani;
        auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) ani.push_back(uint8_t(v >> (8 * i))); };
        auto tag = [&](const char *t) { ani.insert(ani.end(), t, t + 4); };
        const std::vector<uint8_t> a = cur(0, 0, 255), b = cur(0, 0, 200);
        tag("RIFF"), put32(4 + 8 + 36 + 8 + 4 + 2 * (8 + uint32_t(a.size()))), tag("ACON");
        tag("anih"), put32(36), put32(36), put32(2), put32(2), put32(0), put32(0), put32(0), put32(0), put32(6), put32(1);
        tag("LIST"), put32(4 + 2 * (8 + uint32_t(a.size()))), tag("fram");
        for (const std::vector<uint8_t> *f : {&a, &b}) tag("icon"), put32(uint32_t(f->size())), ani.insert(ani.end(), f->begin(), f->end());
        ui::AnimatedCursor moving;
        CHECK(ui::parse_animated_cursor(ani.data(), ani.size(), moving, why));
        CHECK(moving.frames.size() == 2 && moving.steps == std::vector<int>({0, 1}) && moving.duration_ms == std::vector<int>({100, 100}));
        CHECK(moving.frames[1].rgba[3] == 200);
        CHECK(!ui::parse_animated_cursor(one.data(), one.size(), moving, why));
        // the files that ship (found beside the program once the launcher is built, or in the source tree)
        for (const char *folder : {"ui/cursors", "../frontend/ui/assets/cursors", "frontend/ui/assets/cursors"}) {
            if (!std::filesystem::exists(std::filesystem::path(folder) / "normal.cur")) continue;
            auto read = [&](const char *name) {
                std::ifstream f(std::filesystem::path(folder) / name, std::ios::binary);
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            };
            for (const char *name : {"normal.cur", "link.cur", "text.cur", "unavailable.cur", "move.cur", "horizontal.cur", "vertical.cur",
                                     "diagonal1.cur", "diagonal2.cur", "precision.cur"}) {
                const std::vector<uint8_t> bytes = read(name);
                CHECK(ui::parse_cursor(bytes.data(), bytes.size(), image, why) && image.width == 32 && image.height == 32);
            }
            for (const char *name : {"busy.ani", "working.ani"}) {
                const std::vector<uint8_t> bytes = read(name);
                CHECK(ui::parse_animated_cursor(bytes.data(), bytes.size(), moving, why) && moving.steps.size() == 8 && moving.duration_ms[0] == 300);
            }
            break;
        }
    }
    {
        // the window's own sounds: each cue is made, finite, audible, well under full scale and a fraction of a second long
        for (int c = 0; c < int(frontend::Cue::Count); c++) {
            const std::vector<float> cue = frontend::synth_cue(frontend::Cue(c));
            float peak = 0;
            bool finite = true;
            for (float v : cue) finite = finite && std::isfinite(v), peak = std::max(peak, std::fabs(v));
            CHECK(cue.size() % 2 == 0 && cue.size() >= 2 * 2000 && cue.size() <= 2 * frontend::UI_SOUND_RATE);
            CHECK(finite && peak > 0.1f && peak < 0.5f);
        }
        CHECK(frontend::synth_cue(frontend::Cue::Page) == frontend::synth_cue(frontend::Cue::Page)); // the same every time
        CHECK(frontend::synth_cue(frontend::Cue::Focus) != frontend::synth_cue(frontend::Cue::Select));
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
        a.launcher.sounds = false;
        a.launcher.smooth_edges = false;
        a.discord.enabled = false, a.discord.client_id = "123456789012345678";
        a.launcher.x = -1200, a.launcher.y = 40, a.launcher.width = 1500, a.launcher.height = 900, a.launcher.maximized = true;
        std::string err;
        CHECK(frontend::save(file, a, err) && frontend::load(file, b, err));
        CHECK(b.launcher.games == "D:/games" && b.launcher.accent == "#9b7bff" && b.launcher.theme == "light");
        CHECK(b.launcher.sort == "-status");
        CHECK(b.launcher.backdrop == "still");
        CHECK(frontend::Settings().launcher.backdrop == "animated");
        CHECK(!b.launcher.sounds && frontend::Settings().launcher.sounds); // on unless the file says off
        CHECK(!b.launcher.smooth_edges && frontend::Settings().launcher.smooth_edges); // on unless the file says off
        CHECK(frontend::backdrop_choices().size() == 3);
        CHECK(!b.discord.enabled && b.discord.client_id == "123456789012345678");
        // on by default; the ID is the build's default (XF_DISCORD_CLIENT_ID): empty, or digits
        CHECK(frontend::Settings().discord.enabled &&
              frontend::Settings().discord.client_id.find_first_not_of("0123456789") == std::string::npos);
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
