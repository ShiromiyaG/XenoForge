// The console screens' look (guide_theme.h).

#include "guide_theme.h"

#include <imgui_internal.h>

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace xf::frontend::guide {

namespace {

Fonts g_fonts;
int g_pushed_vars = 0, g_pushed_colors = 0;

// Basic Latin, Latin-1, Latin Extended-A, the dashes, quotes, bullet and ellipsis, the euro sign
const ImWchar k_ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x20AC, 0x20AC, 0};

// a point of the mark's drawing (assets/branding/xenoforge.svg, 512 units, the disc's radius 236) on the screen
struct Mapper {
    ImVec2 c;
    float k;
    ImVec2 operator()(float x, float y) const { return ImVec2(c.x + (x - 256) * k, c.y + (y - 256) * k); }
};

void star(ImDrawList *dl, const Mapper &m, float cx, float cy, float arm, float waist, ImU32 color) {
    const ImVec2 p[8] = {m(cx, cy - arm),   m(cx + waist, cy - waist), m(cx + arm, cy),   m(cx + waist, cy + waist),
                         m(cx, cy + arm),   m(cx - waist, cy + waist), m(cx - arm, cy),   m(cx - waist, cy - waist)};
    dl->AddConcavePolyFilled(p, 8, color);
}

} // namespace

std::filesystem::path find_fonts() {
    std::filesystem::path base;
    if (const char *b = SDL_GetBasePath()) base = std::filesystem::path(reinterpret_cast<const char8_t *>(b));
    for (const std::filesystem::path &dir : {base / "ui" / "fonts", base.parent_path() / "ui" / "fonts",
                                             base.parent_path().parent_path() / "ui" / "fonts", base / "fonts"}) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "Inter-Regular.ttf", ec)) return dir;
    }
    return {};
}

bool load_fonts(const std::filesystem::path &dir, float scale) {
    ImGuiIO &io = ImGui::GetIO();
    g_fonts = Fonts{};
    g_fonts.built_for = scale;
    auto add = [&](const char *file, float size) -> ImFont * {
        std::error_code ec;
        const std::filesystem::path path = dir / file;
        if (dir.empty() || !std::filesystem::exists(path, ec)) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2, cfg.OversampleV = 1, cfg.PixelSnapH = false;
        return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size * scale, &cfg, k_ranges);
    };
    // the first one added is ImGui's default: the overlay's widgets
    g_fonts.ui = add("Inter-Regular.ttf", 16);
    if (!g_fonts.ui) {
        ImFont *def = io.Fonts->AddFontDefault();
        g_fonts.ui = g_fonts.body = g_fonts.strong = g_fonts.title = g_fonts.small = g_fonts.glyph = def;
        return false;
    }
    g_fonts.body = add("Inter-Regular.ttf", 21);
    g_fonts.small = add("Inter-Medium.ttf", 16);
    g_fonts.strong = add("Inter-SemiBold.ttf", 22);
    g_fonts.title = add("Inter-SemiBold.ttf", 30);
    g_fonts.glyph = add("Inter-Bold.ttf", 16);
    for (ImFont **f : {&g_fonts.body, &g_fonts.small, &g_fonts.strong, &g_fonts.title, &g_fonts.glyph})
        if (!*f) *f = g_fonts.ui;
    g_fonts.inter = true;
    return true;
}

const Fonts &fonts() { return g_fonts; }

Scale scale_for(const ImVec2 &viewport) {
    Scale sc;
    sc.s = std::clamp(std::min(viewport.y / 720.0f, viewport.x / 1280.0f), 0.6f, 3.0f);
    return sc;
}

ImU32 fade(ImU32 color, float alpha) {
    const uint32_t a = uint32_t(float((color >> IM_COL32_A_SHIFT) & 0xFF) * clamp01(alpha) + 0.5f);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

float ease_out(float t) {
    t = clamp01(t);
    const float u = 1 - t;
    return 1 - u * u * u;
}

void backdrop(ImDrawList *dl, ImVec2 min, ImVec2 max, float alpha) {
    dl->AddRectFilled(min, max, fade(IM_COL32(4, 7, 5, 255), 0.62f * alpha));
    // a vignette: darker at the top and the bottom
    const float h = (max.y - min.y) * 0.25f;
    dl->AddRectFilledMultiColor(min, ImVec2(max.x, min.y + h), fade(IM_COL32(0, 0, 0, 255), 0.35f * alpha),
                                fade(IM_COL32(0, 0, 0, 255), 0.35f * alpha), 0, 0);
    dl->AddRectFilledMultiColor(ImVec2(min.x, max.y - h), max, 0, 0, fade(IM_COL32(0, 0, 0, 255), 0.35f * alpha),
                                fade(IM_COL32(0, 0, 0, 255), 0.35f * alpha));
}

void panel(ImDrawList *dl, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    // the shadow: rings fading out
    for (int i = 6; i >= 1; i--) {
        const float grow = float(i) * 3.0f;
        dl->AddRectFilled(ImVec2(min.x - grow, min.y - grow + 6), ImVec2(max.x + grow, max.y + grow + 6),
                          fade(IM_COL32(0, 0, 0, 255), alpha * 0.07f), rounding + grow);
    }
    const int from = dl->VtxBuffer.Size;
    dl->AddRectFilled(min, max, fade(col::panel_top, 0.97f * alpha), rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size, min, ImVec2(min.x, max.y),
                                                  col::panel_top, col::panel_bottom);
    // the light on the top edge, the fine border
    dl->AddLine(ImVec2(min.x + rounding, min.y + 1), ImVec2(max.x - rounding, min.y + 1),
                fade(IM_COL32(255, 255, 255, 255), 0.10f * alpha), 1.0f);
    dl->AddRect(min, max, fade(col::edge, alpha), rounding, 0, 1.0f);
}

void accent_line(ImDrawList *dl, ImVec2 from, float width, float thickness, float alpha) {
    dl->AddRectFilledMultiColor(from, ImVec2(from.x + width, from.y + thickness), fade(col::green, alpha),
                                fade(col::green_lo, 0), fade(col::green_lo, 0), fade(col::green, alpha));
}

void item(ImDrawList *dl, ImVec2 min, ImVec2 max, float rounding, bool focused, bool hovered, float alpha) {
    if (focused) {
        for (int i = 3; i >= 1; i--) {
            const float grow = float(i) * 3.0f;
            dl->AddRectFilled(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow),
                              fade(col::green, alpha * 0.08f), rounding + grow);
        }
        const int from = dl->VtxBuffer.Size;
        dl->AddRectFilled(min, max, fade(col::green, alpha), rounding);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size, min, ImVec2(min.x, max.y),
                                                      col::green_hi, col::green_lo);
        dl->AddLine(ImVec2(min.x + rounding, min.y + 1), ImVec2(max.x - rounding, min.y + 1),
                    fade(IM_COL32(255, 255, 255, 255), 0.35f * alpha), 1.0f);
    } else {
        dl->AddRectFilled(min, max, fade(IM_COL32(255, 255, 255, 255), (hovered ? 0.10f : 0.05f) * alpha), rounding);
        dl->AddRect(min, max, fade(IM_COL32(255, 255, 255, 255), 0.06f * alpha), rounding, 0, 1.0f);
    }
}

void button_glyph(ImDrawList *dl, ImVec2 center, float radius, char letter, float alpha) {
    ImU32 color = IM_COL32(120, 128, 122, 255), ink = IM_COL32(255, 255, 255, 255);
    switch (letter) {
    case 'A': color = col::button_a; break;
    case 'B': color = col::button_b; break;
    case 'X': color = col::button_x; break;
    case 'Y': color = col::button_y, ink = IM_COL32(58, 42, 0, 255); break;
    default: break;
    }
    const int from = dl->VtxBuffer.Size;
    dl->AddCircleFilled(center, radius, fade(color, alpha), 32);
    const ImU32 lit = ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(color), ImVec4(1, 1, 1, 1), 0.3f));
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size, ImVec2(center.x, center.y - radius),
                                                  ImVec2(center.x, center.y + radius), lit, color);
    dl->AddCircle(center, radius, fade(IM_COL32(0, 0, 0, 255), 0.35f * alpha), 32, 1.0f);
    const char s[2] = {letter, 0};
    ImFont *f = fonts().glyph ? fonts().glyph : ImGui::GetFont();
    const float size = radius * 1.25f;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, s);
    dl->AddText(f, size, ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), fade(ink, alpha), s);
}

float hint(ImDrawList *dl, ImVec2 at, float height, char letter, const char *label, float alpha, bool draw) {
    const float r = height * 0.42f;
    ImFont *f = fonts().small ? fonts().small : ImGui::GetFont();
    const float size = height * 0.62f;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, label);
    if (draw) {
        button_glyph(dl, ImVec2(at.x + r, at.y + height / 2), r, letter, alpha);
        dl->AddText(f, size, ImVec2(at.x + 2 * r + height * 0.3f, at.y + (height - ts.y) / 2),
                    fade(col::text_dim, alpha), label);
    }
    return 2 * r + height * 0.3f + ts.x;
}

void mark(ImDrawList *dl, ImVec2 center, float radius, float alpha) {
    const Mapper m{center, radius / 236.0f};
    const float k = m.k;
    // the disc
    int from = dl->VtxBuffer.Size;
    dl->AddCircleFilled(center, radius, fade(IM_COL32(28, 42, 31, 255), alpha), 48);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size, ImVec2(center.x, center.y - radius),
                                                  ImVec2(center.x, center.y + radius), IM_COL32(28, 42, 31, 255),
                                                  IM_COL32(7, 11, 8, 255));
    dl->AddCircle(center, radius, fade(IM_COL32(44, 63, 47, 255), alpha), 48, std::max(1.0f, 4 * k));
    // the ring of light, four quadrants with gaps at the cardinal points; a wide faint stroke under it glows
    const float r = 197 * k, t = 30 * k;
    const float a0 = -85.9f * IM_PI / 180, a1 = -4.1f * IM_PI / 180;
    for (int pass = 0; pass < 2; pass++) {
        from = dl->VtxBuffer.Size;
        for (int q = 0; q < 4; q++) {
            dl->PathArcTo(center, r, a0 + q * IM_PI / 2, a1 + q * IM_PI / 2, 24);
            dl->PathStroke(fade(col::green, pass ? alpha : alpha * 0.22f), 0, pass ? t : t * 2.0f);
        }
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size,
                                                      ImVec2(center.x - r, center.y - r),
                                                      ImVec2(center.x + r, center.y + r), col::green_hi, col::green_lo);
    }
    // the spark, its glow and two small ones
    if (radius >= 10) {
        const ImVec2 sc = m(256, 186);
        for (int i = 4; i >= 1; i--)
            dl->AddCircleFilled(sc, 62 * k * float(i) / 4, fade(IM_COL32(216, 255, 138, 255), alpha * 0.10f), 24);
    }
    star(dl, m, 256, 186, 58, 10, fade(IM_COL32(242, 255, 214, 255), alpha));
    if (radius >= 16) {
        star(dl, m, 206, 154, 14, 4, fade(IM_COL32(201, 247, 122, 255), alpha));
        star(dl, m, 312, 236, 10, 3, fade(IM_COL32(201, 247, 122, 255), alpha));
    }
    // the anvil
    from = dl->VtxBuffer.Size;
    dl->PathLineTo(m(122, 262));
    dl->PathLineTo(m(372, 262));
    dl->PathLineTo(m(372, 296));
    dl->PathLineTo(m(336, 296));
    dl->PathBezierQuadraticCurveTo(m(312, 302), m(306, 324), 8);
    dl->PathLineTo(m(306, 344));
    dl->PathLineTo(m(334, 344));
    dl->PathLineTo(m(342, 372));
    dl->PathLineTo(m(170, 372));
    dl->PathLineTo(m(178, 344));
    dl->PathLineTo(m(206, 344));
    dl->PathLineTo(m(206, 324));
    dl->PathBezierQuadraticCurveTo(m(200, 302), m(176, 296), 8);
    dl->PathLineTo(m(196, 296));
    dl->PathBezierQuadraticCurveTo(m(150, 296), m(122, 262), 10);
    dl->PathFillConcave(fade(IM_COL32(244, 251, 239, 255), alpha));
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size, m(256, 262), m(256, 372),
                                                  IM_COL32(244, 251, 239, 255), IM_COL32(185, 201, 181, 255));
}

void status_icon(ImDrawList *dl, ImVec2 center, float radius, int icon, float alpha) {
    ImU32 color = IM_COL32(52, 150, 210, 255);
    const char *s = "i";
    if (icon == 1) color = IM_COL32(214, 64, 52, 255), s = "!";
    if (icon == 2) color = IM_COL32(232, 168, 30, 255), s = "!";
    dl->AddCircleFilled(center, radius, fade(color, alpha), 32);
    ImFont *f = fonts().glyph ? fonts().glyph : ImGui::GetFont();
    const float size = radius * 1.35f;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, s);
    dl->AddText(f, size, ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), fade(IM_COL32(255, 255, 255, 255), alpha),
                s);
}

void text(ImDrawList *dl, ImFont *font, float size, ImVec2 at, ImU32 color, const char *s, float wrap) {
    if (!font) font = ImGui::GetFont();
    dl->AddText(font, size, at, color, s, nullptr, wrap);
}

ImVec2 measure(ImFont *font, float size, const char *s, float wrap) {
    if (!font) font = ImGui::GetFont();
    return font->CalcTextSizeA(size, FLT_MAX, wrap, s);
}

void push_widgets(const Scale &sc) {
    auto var = [&](ImGuiStyleVar v, ImVec2 value) { ImGui::PushStyleVar(v, value), g_pushed_vars++; };
    auto var1 = [&](ImGuiStyleVar v, float value) { ImGui::PushStyleVar(v, value), g_pushed_vars++; };
    auto color = [&](ImGuiCol c, ImU32 value) { ImGui::PushStyleColor(c, value), g_pushed_colors++; };
    var(ImGuiStyleVar_FramePadding, ImVec2(sc.px(12), sc.px(8)));
    var(ImGuiStyleVar_ItemSpacing, ImVec2(sc.px(10), sc.px(10)));
    var(ImGuiStyleVar_ItemInnerSpacing, ImVec2(sc.px(8), sc.px(6)));
    var(ImGuiStyleVar_CellPadding, ImVec2(sc.px(8), sc.px(6)));
    var1(ImGuiStyleVar_FrameRounding, sc.px(6));
    var1(ImGuiStyleVar_PopupRounding, sc.px(6));
    var1(ImGuiStyleVar_GrabRounding, sc.px(6));
    var1(ImGuiStyleVar_ChildRounding, sc.px(8));
    var1(ImGuiStyleVar_ScrollbarSize, sc.px(10));
    var1(ImGuiStyleVar_ScrollbarRounding, sc.px(5));
    var1(ImGuiStyleVar_FrameBorderSize, 1.0f);
    color(ImGuiCol_Text, col::text);
    color(ImGuiCol_TextDisabled, col::text_dim);
    color(ImGuiCol_FrameBg, IM_COL32(12, 15, 13, 200));
    color(ImGuiCol_FrameBgHovered, IM_COL32(28, 40, 30, 220));
    color(ImGuiCol_FrameBgActive, IM_COL32(32, 52, 34, 230));
    color(ImGuiCol_Border, col::edge);
    color(ImGuiCol_Button, IM_COL32(255, 255, 255, 18));
    color(ImGuiCol_ButtonHovered, fade(col::green, 0.45f));
    color(ImGuiCol_ButtonActive, col::green);
    color(ImGuiCol_Header, fade(col::green, 0.35f));
    color(ImGuiCol_HeaderHovered, fade(col::green, 0.50f));
    color(ImGuiCol_HeaderActive, col::green);
    color(ImGuiCol_CheckMark, col::green_hi);
    color(ImGuiCol_SliderGrab, col::green);
    color(ImGuiCol_SliderGrabActive, col::green_hi);
    color(ImGuiCol_PopupBg, IM_COL32(24, 28, 25, 250));
    color(ImGuiCol_Separator, IM_COL32(255, 255, 255, 30));
    color(ImGuiCol_TableRowBg, IM_COL32(255, 255, 255, 0));
    color(ImGuiCol_TableRowBgAlt, IM_COL32(255, 255, 255, 10));
    color(ImGuiCol_TableBorderLight, IM_COL32(255, 255, 255, 20));
    color(ImGuiCol_NavCursor, col::green_hi);
    color(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_ScrollbarGrab, IM_COL32(255, 255, 255, 50));
    color(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 60));
}

void pop_widgets() {
    ImGui::PopStyleVar(g_pushed_vars);
    ImGui::PopStyleColor(g_pushed_colors);
    g_pushed_vars = g_pushed_colors = 0;
}

} // namespace xf::frontend::guide
