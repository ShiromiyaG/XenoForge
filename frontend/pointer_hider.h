// When the player's window shows the mouse pointer: as emulators do, it is hidden over the game's picture once it has
// rested for a while (a moment after it moves in fullscreen), and shown whenever it moves, and for as long as
// something of the system's is on the window that a person points at (the F1 overlay, the guide, a dialog). The state
// is plain so a test can drive it; the player turns its answer into SDL_ShowCursor / SDL_HideCursor.
#pragma once

#include <cstdint>

namespace xf::frontend {

class PointerHider {
  public:
    static constexpr uint64_t windowed_rest_ms = 2000;  // still for this long and the pointer goes
    static constexpr uint64_t fullscreen_rest_ms = 250; // in fullscreen it goes at once: a flicker of a move only

    // the mouse moved or a button or the wheel was used, at time now_ms
    void moved(uint64_t now_ms) { last_move_ms_ = now_ms; }

    // whether the pointer shows, at time now_ms: wanted (something of the system's to point at) keeps it, else it
    // shows while it has moved lately
    bool visible(uint64_t now_ms, bool wanted, bool fullscreen) const {
        if (wanted) return true;
        const uint64_t rest = fullscreen ? fullscreen_rest_ms : windowed_rest_ms;
        return now_ms - last_move_ms_ < rest;
    }

  private:
    uint64_t last_move_ms_ = 0;
};

} // namespace xf::frontend
