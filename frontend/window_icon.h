// The windows' icons: XenoForge's mark for its own windows (the launcher, its settings), the game's own image while a
// game runs (the player), from PNG bytes.

#pragma once

#include <cstdint>
#include <span>

struct SDL_Window;

namespace xf::frontend {

// XenoForge's mark, 256 pixels (assets/branding/xenoforge-256.png, built in)
std::span<const uint8_t> xenoforge_icon_png();
// the window's icon (its title bar, the task bar) from a PNG; false when the bytes do not decode
bool set_window_icon(SDL_Window *window, std::span<const uint8_t> png);

} // namespace xf::frontend
