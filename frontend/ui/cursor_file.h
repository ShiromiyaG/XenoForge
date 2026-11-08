// Windows cursor files (.cur, and the animated .ani), read into plain RGBA
// images for SDL's colour cursors (ui_system.cpp). No SDL here, so the
// reading is tested without a window. A .cur is an icon directory whose
// entries hold a DIB (the image, bottom-up, then a 1-bit mask for the images
// without an alpha channel) and the hot spot; an .ani is a RIFF "ACON" with
// a frame per .cur, the time each is shown (in 1/60 s, one rate or one per
// step) and optionally the order of the steps.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xf::ui {

struct CursorImage {
    int width = 0, height = 0;
    int hot_x = 0, hot_y = 0;
    std::vector<uint8_t> rgba; // width * height * 4, top row first, not premultiplied
};

// The largest image of a .cur (or .ico) that is a DIB of 32 or 24 bits; false, and why in `error`, for
// anything else (a PNG inside, a palette, a truncated file).
bool parse_cursor(const uint8_t *data, size_t size, CursorImage &out, std::string &error);

struct AnimatedCursor {
    std::vector<CursorImage> frames;
    std::vector<int> steps;       // the frame shown at each step
    std::vector<int> duration_ms; // how long each step lasts, as many as steps
};

bool parse_animated_cursor(const uint8_t *data, size_t size, AnimatedCursor &out, std::string &error);

// The image scaled up by a whole factor with linear filtering (a cursor drawn for 100% display scale,
// given a larger version for a high-density screen).
CursorImage scaled_cursor(const CursorImage &image, int factor);

} // namespace xf::ui
