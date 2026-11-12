// The windows' icons (window_icon.h).

#include "window_icon.h"

#include <SDL3/SDL.h>

namespace xf::frontend {

namespace {

const unsigned char k_mark[] = {
#include "xenoforge_icon.inc"
};

} // namespace

std::span<const uint8_t> xenoforge_icon_png() { return {k_mark, sizeof(k_mark)}; }

bool set_window_icon(SDL_Window *window, std::span<const uint8_t> png) {
    if (!window || png.empty()) return false;
    SDL_IOStream *io = SDL_IOFromConstMem(png.data(), png.size());
    if (!io) return false;
    SDL_Surface *surface = SDL_LoadPNG_IO(io, true);
    if (!surface) return false;
    const bool ok = SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
    return ok;
}

} // namespace xf::frontend
