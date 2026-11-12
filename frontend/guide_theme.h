// The look of the console's own screens on the player's window (the system's dialogs, the toasts, the guide's
// pages): dark panels lit by a green ring, items that light up when focused, the controller's buttons in the
// footer, the Inter typeface. An original look in the spirit of the console's dashboard of its time; nothing of
// the console maker's art, fonts or marks is used (the mark drawn here is XenoForge's own, assets/branding).
//
// Everything is drawn with Dear ImGui's draw lists, so the dialogs keep their model (system_ui.h) and the
// overlay keeps its widgets: push_widgets() gives ImGui's own widgets the same look inside a guide panel.

#pragma once

#include <imgui.h>

#include <filesystem>

namespace xf::frontend::guide {

// The typefaces at the sizes the screens use, made at load for a 720-line window times `scale`; a taller
// window scales them (Scale below). Without the Inter files every member is ImGui's default font.
struct Fonts {
    ImFont *ui = nullptr;     // the overlay's widgets
    ImFont *body = nullptr;   // a dialog's text
    ImFont *strong = nullptr; // a focused item, a name
    ImFont *title = nullptr;  // a dialog's or page's title
    ImFont *small = nullptr;  // captions, footers
    ImFont *glyph = nullptr;  // the letter on a controller button
    float built_for = 1;      // the scale the atlas was made at
    bool inter = false;
};

// the Inter files: `ui/fonts` beside the executable or one level up (the launcher's copy), else empty
std::filesystem::path find_fonts();
// Adds the fonts to ImGui's atlas (before the backend makes its font texture); the first added is the default.
// False, with ImGui's default font kept, when the files are missing.
bool load_fonts(const std::filesystem::path &dir, float scale);
const Fonts &fonts();

// sizes for the current frame: a 720-line window is 1
struct Scale {
    float s = 1;
    float px(float v) const { return v * s; }
};
Scale scale_for(const ImVec2 &viewport);

namespace col {
inline constexpr ImU32 green_hi = IM_COL32(184, 242, 90, 255);
inline constexpr ImU32 green = IM_COL32(95, 210, 44, 255);
inline constexpr ImU32 green_lo = IM_COL32(19, 154, 27, 255);
inline constexpr ImU32 panel_top = IM_COL32(44, 50, 46, 255);
inline constexpr ImU32 panel_bottom = IM_COL32(18, 21, 19, 255);
inline constexpr ImU32 edge = IM_COL32(255, 255, 255, 34);
inline constexpr ImU32 text = IM_COL32(236, 241, 234, 255);
inline constexpr ImU32 text_dim = IM_COL32(160, 170, 158, 255);
inline constexpr ImU32 text_on_green = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 button_a = IM_COL32(96, 196, 52, 255);
inline constexpr ImU32 button_b = IM_COL32(216, 56, 48, 255);
inline constexpr ImU32 button_x = IM_COL32(40, 120, 220, 255);
inline constexpr ImU32 button_y = IM_COL32(236, 180, 32, 255);
} // namespace col

ImU32 fade(ImU32 color, float alpha);
float clamp01(float v);
float ease_out(float t); // 0..1 -> 0..1, fast then settling

// the game behind a dialog, dimmed
void backdrop(ImDrawList *dl, ImVec2 min, ImVec2 max, float alpha);
// a panel: soft shadow, top-lit gradient, a fine edge
void panel(ImDrawList *dl, ImVec2 min, ImVec2 max, float rounding, float alpha);
// the green line under a title, fading to the right
void accent_line(ImDrawList *dl, ImVec2 from, float width, float thickness, float alpha);
// a choice's background: lit green when focused, faint otherwise
void item(ImDrawList *dl, ImVec2 min, ImVec2 max, float rounding, bool focused, bool hovered, float alpha);
// a controller button: a coloured disc with its letter (A, B, X, Y; others grey)
void button_glyph(ImDrawList *dl, ImVec2 center, float radius, char letter, float alpha);
// a footer hint: the button and its label; returns its width (draw = false only measures)
float hint(ImDrawList *dl, ImVec2 at, float height, char letter, const char *label, float alpha, bool draw = true);
// XenoForge's mark (the ring of light around the anvil) in a disc of `radius`
void mark(ImDrawList *dl, ImVec2 center, float radius, float alpha);
// a message box's icon: 1 error, 2 warning, 3 information
void status_icon(ImDrawList *dl, ImVec2 center, float radius, int icon, float alpha);

// text with a font at a size; wrap 0 = one line
void text(ImDrawList *dl, ImFont *font, float size, ImVec2 at, ImU32 color, const char *s, float wrap = 0);
ImVec2 measure(ImFont *font, float size, const char *s, float wrap = 0);

// ImGui's widgets (inputs, combos, buttons, tables) in the guide's look, until pop_widgets()
void push_widgets(const Scale &sc);
void pop_widgets();

} // namespace xf::frontend::guide
