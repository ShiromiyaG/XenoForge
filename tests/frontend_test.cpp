// The player's parts that need no window: config.toml written and read
// back, Xenia's default keyboard bindings all naming SDL keys, the sound's
// fold to stereo and its six channels interleaved, and the scripted presses
// (tools/game.h) parsed and held at their times.

#include "check.h"

#include "../frontend/sdl_audio.h"
#include "../frontend/sdl_input.h"
#include "../frontend/settings.h"
#include "../tools/game.h"

#include <cmath>
#include <filesystem>
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
    std::filesystem::remove_all(dir, ec);

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
    return finish("frontend");
}
