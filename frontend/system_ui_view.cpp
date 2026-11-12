// The system dialogs' view (system_ui_view.h).

#include "system_ui_view.h"

#include "guide_theme.h"

#include <imgui_impl_vulkan.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>

namespace xf::frontend {

IconCache::~IconCache() {
    VkDevice dev = device_.device();
    for (auto &[id, icon] : icons_) {
        if (icon.set) ImGui_ImplVulkan_RemoveTexture(icon.set);
        vkDestroyImageView(dev, icon.view, nullptr);
        device_.destroy(icon.image);
    }
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
}

ImTextureID IconCache::get(uint32_t id, const std::vector<uint8_t> &png) {
    if (auto it = icons_.find(id); it != icons_.end()) return ImTextureID(it->second.set);
    if (png.empty() || icons_.size() >= 48) return 0;
    icons_[id]; // whatever happens below, not tried again
    int w = 0, h = 0, n = 0;
    stbi_uc *pixels = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
    if (!pixels) return 0;
    VkDevice dev = device_.device();
    Icon icon;
    icon.image = device_.create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {uint32_t(w), uint32_t(h), 1}, 1, 1,
                                      VK_SAMPLE_COUNT_1_BIT,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vk::Buffer staging = device_.create_buffer(size_t(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    bool ok = icon.image.image && staging.buffer && staging.mapped;
    if (ok) {
        std::memcpy(staging.mapped, pixels, size_t(w) * h * 4);
        std::string error;
        ok = device_.run(
            [&](VkCommandBuffer cmd) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = icon.image.image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {uint32_t(w), uint32_t(h), 1};
                vkCmdCopyBufferToImage(cmd, staging.buffer, icon.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &copy);
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
            },
            error);
    }
    if (staging.buffer) device_.destroy(staging);
    stbi_image_free(pixels);
    if (ok) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = icon.image.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = vkCreateImageView(dev, &vi, nullptr, &icon.view) == VK_SUCCESS;
    }
    if (ok && !sampler_) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ok = vkCreateSampler(dev, &si, nullptr, &sampler_) == VK_SUCCESS;
    }
    if (!ok) {
        if (icon.view) vkDestroyImageView(dev, icon.view, nullptr);
        if (icon.image.image) device_.destroy(icon.image);
        return 0;
    }
    icon.set = ImGui_ImplVulkan_AddTexture(sampler_, icon.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    icons_[id] = icon;
    return ImTextureID(icon.set);
}

void draw_achievement_row(ImDrawList *dl, const guide::Scale &sc, ImVec2 rmin, ImVec2 rmax,
                          const kernel::ProfileAchievement &ach, ImTextureID tex, bool focused, bool hovered,
                          float a) {
    const guide::Fonts &f = guide::fonts();
    const float small_size = sc.px(16), strong_size = sc.px(22);
    const float icon = std::min(sc.px(56), rmax.y - rmin.y - sc.px(10));
    guide::item(dl, rmin, rmax, sc.px(8), focused, hovered, a);
    const ImVec2 imin(rmin.x + sc.px(8), rmin.y + (rmax.y - rmin.y - icon) / 2);
    if (tex)
        dl->AddImageRounded(tex, imin, ImVec2(imin.x + icon, imin.y + icon), ImVec2(0, 0), ImVec2(1, 1),
                            guide::fade(ach.unlocked ? IM_COL32(255, 255, 255, 255) : IM_COL32(110, 116, 110, 255), a),
                            sc.px(6));
    else
        dl->AddRectFilled(imin, ImVec2(imin.x + icon, imin.y + icon), guide::fade(IM_COL32(0, 0, 0, 90), a), sc.px(6));
    // the gamerscore at the right, a pill
    char g[32];
    std::snprintf(g, sizeof(g), "%u G", ach.gamerscore);
    const ImVec2 gs = guide::measure(f.strong, small_size, g);
    const ImVec2 pmax(rmax.x - sc.px(12), rmin.y + (rmax.y - rmin.y) / 2 + gs.y / 2 + sc.px(4));
    const ImVec2 pmin(pmax.x - gs.x - sc.px(20), pmax.y - gs.y - sc.px(8));
    dl->AddRectFilled(pmin, pmax,
                      guide::fade(focused        ? IM_COL32(0, 0, 0, 70)
                                  : ach.unlocked ? guide::fade(guide::col::green, 0.85f)
                                                 : IM_COL32(255, 255, 255, 26),
                                  a),
                      (pmax.y - pmin.y) / 2);
    guide::text(dl, f.strong, small_size, ImVec2(pmin.x + sc.px(10), pmin.y + sc.px(4)),
                guide::fade(IM_COL32(255, 255, 255, 255), a), g);
    // the name and the description
    const float tx = imin.x + icon + sc.px(14), tw = pmin.x - sc.px(12) - tx;
    const std::string label = utf8_of(ach.label);
    const std::string desc = utf8_of(ach.unlocked || ach.unachieved.empty() ? ach.description : ach.unachieved);
    const ImU32 name_color = focused ? guide::col::text_on_green : ach.unlocked ? guide::col::text : guide::col::text_dim;
    const float name_size = strong_size * 0.9f;
    const float block = name_size + sc.px(4) + guide::measure(f.small, small_size, desc.c_str(), tw).y;
    const float ty = rmin.y + std::max(sc.px(6), (rmax.y - rmin.y - block) / 2);
    dl->PushClipRect(ImVec2(tx, rmin.y), ImVec2(tx + tw, rmax.y), true);
    guide::text(dl, f.strong, name_size, ImVec2(tx, ty), guide::fade(name_color, a), label.c_str());
    guide::text(dl, f.small, small_size, ImVec2(tx, ty + name_size + sc.px(4)),
                guide::fade(focused ? IM_COL32(236, 250, 226, 255) : guide::col::text_dim, a), desc.c_str(), tw);
    dl->PopClipRect();
}

namespace {

using guide::Scale;

// the footer's hints for a dialog: what A and B do
struct Hints {
    const char *a = "Select";
    const char *b = "Back";
};

Hints hints_for(const Dialog &d) {
    if (d.achievements) return {"Close", "Back"};
    if (d.text_entry) return {"Done", "Cancel"};
    if (d.choices.size() <= 1) return {"OK", "Back"};
    return {};
}

// The dialog on top: a lit panel over the dimmed game, the title under the mark or the message's icon, the text,
// the choices stacked as items (the focused one lit green), the controller's buttons in the footer. It eases in.
void draw_dialog(SystemUi &ui, IconCache &icons, Dialog &d, double now) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const Scale sc = guide::scale_for(vp->Size);
    const guide::Fonts &f = guide::fonts();
    if (d.shown_at < 0) d.shown_at = now;
    const float a = guide::ease_out(float((now - d.shown_at) / 0.20));

    guide::backdrop(ImGui::GetBackgroundDrawList(), vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                    a);

    // the layout, measured before anything is drawn
    const float pad = sc.px(30), round = sc.px(12);
    const float width = std::min(std::clamp(vp->Size.x * 0.44f, sc.px(560), sc.px(820)), vp->Size.x - sc.px(32));
    const float inner = width - 2 * pad;
    const float title_size = sc.px(30), body_size = sc.px(21), small_size = sc.px(16);
    const float header_h = sc.px(56);
    std::vector<float> line_h;
    float body_h = 0;
    for (const std::string &line : d.lines) {
        if (d.achievements) break; // (the header shows the gamertag and the score)
        line_h.push_back(guide::measure(f.body, body_size, line.c_str(), inner).y);
        body_h += line_h.back() + sc.px(10);
    }
    const float field_h = d.text_entry ? sc.px(52) + small_size + sc.px(14) : 0;
    const float row_h = sc.px(76);
    float list_h = 0;
    size_t rows_visible = 0;
    if (d.achievements) {
        list_h = std::max(row_h * 2, std::min(vp->Size.y * 0.58f, row_h * 6));
        rows_visible = size_t(list_h / row_h);
        list_h = float(rows_visible) * row_h;
    }
    const float choice_h = sc.px(50), choice_gap = sc.px(8);
    const float choices_h = d.achievements ? 0 : float(d.choices.size()) * (choice_h + choice_gap);
    const float footer_h = sc.px(44);
    const float height = pad + header_h + sc.px(18) + body_h + field_h + list_h + (list_h ? sc.px(14) : 0) +
                         choices_h + sc.px(6) + footer_h + sc.px(14);

    const float rise = (1 - a) * sc.px(26);
    const ImVec2 min(vp->Pos.x + (vp->Size.x - width) / 2, vp->Pos.y + (vp->Size.y - height) / 2 + rise);
    const ImVec2 max(min.x + width, min.y + height);

    // a window over the panel takes the mouse and the text field's keys; the panel is drawn on its list first
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##system_dialog", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    guide::panel(dl, min, max, round, a);

    // the header: the message's icon, else the mark; the title; the green line
    float x = min.x + pad, y = min.y + pad;
    const float badge = sc.px(22);
    if (d.icon >= 1 && d.icon <= 3)
        guide::status_icon(dl, ImVec2(x + badge, y + header_h / 2 - sc.px(4)), badge, d.icon, a);
    else
        guide::mark(dl, ImVec2(x + badge, y + header_h / 2 - sc.px(4)), badge, a);
    const float title_x = x + 2 * badge + sc.px(16);
    const ImVec2 ts = guide::measure(f.title, title_size, d.title.c_str());
    guide::text(dl, f.title, title_size, ImVec2(title_x, y + (header_h - ts.y) / 2 - sc.px(4)),
                guide::fade(guide::col::text, a), d.title.c_str());
    if (d.achievements) {
        char score[96];
        std::snprintf(score, sizeof(score), "%s  \xc2\xb7  %u / %u G", d.profile.gamertag.c_str(),
                      d.profile.gamerscore, d.profile.gamerscore_total);
        const ImVec2 ss = guide::measure(f.small, small_size, score);
        guide::text(dl, f.small, small_size, ImVec2(max.x - pad - ss.x, y + (header_h - ss.y) / 2 - sc.px(4)),
                    guide::fade(guide::col::text_dim, a), score);
    }
    y += header_h;
    guide::accent_line(dl, ImVec2(x, y), inner, sc.px(2), a);
    y += sc.px(18);

    // the text
    for (size_t i = 0; i < line_h.size(); i++) {
        guide::text(dl, f.body, body_size, ImVec2(x, y), guide::fade(IM_COL32(214, 222, 211, 255), a),
                    d.lines[i].c_str(), inner);
        y += line_h[i] + sc.px(10);
    }

    // a keyboard's field
    if (d.text_entry) {
        guide::push_widgets(sc);
        ImGui::PushFont(f.body);
        ImGui::SetWindowFontScale(body_size / (f.body->FontSize * ImGui::GetIO().FontGlobalScale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sc.px(14), (sc.px(52) - body_size) / 2));
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::SetNextItemWidth(inner);
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        // (the room is in characters, the field's in bytes: a few more, then the answer is cut by characters)
        std::string buffer = d.text;
        buffer.resize(std::max<size_t>(buffer.size(), 1) + 256);
        const bool entered =
            ImGui::InputText("##text", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        d.text = buffer.c_str();
        ImGui::PopStyleVar();
        ImGui::SetWindowFontScale(1);
        ImGui::PopFont();
        guide::pop_widgets();
        y += sc.px(52) + sc.px(8);
        if (d.max_length) {
            char count[64];
            std::snprintf(count, sizeof(count), "%zu / %u", utf16_of(d.text).size(), d.max_length);
            const ImVec2 cs = guide::measure(f.small, small_size, count);
            guide::text(dl, f.small, small_size, ImVec2(max.x - pad - cs.x, y), guide::fade(guide::col::text_dim, a),
                        count);
        }
        y += small_size + sc.px(6);
        if (entered) {
            ui.confirm_text();
            ImGui::End();
            return;
        }
    }

    // the achievements: a row each, the focused one lit; locked ones greyed
    if (d.achievements) {
        const std::vector<kernel::ProfileAchievement> &rows = d.profile.achievements;
        if (rows.empty())
            guide::text(dl, f.body, body_size, ImVec2(x, y + sc.px(8)), guide::fade(guide::col::text_dim, a),
                        "This title lists no achievements.");
        for (size_t r = 0; r < rows_visible && d.first_row + r < rows.size(); r++) {
            const kernel::ProfileAchievement &ach = rows[d.first_row + r];
            const ImVec2 rmin(x, y + float(r) * row_h), rmax(x + inner - sc.px(14), rmin.y + row_h - sc.px(6));
            draw_achievement_row(dl, sc, rmin, rmax, ach, icons.get(ach.image_id, ach.icon), r == 0, false, a);
        }
        // where the list is
        if (rows.size() > rows_visible) {
            const float track_x = x + inner - sc.px(4);
            dl->AddRectFilled(ImVec2(track_x, y), ImVec2(track_x + sc.px(4), y + list_h - sc.px(6)),
                              guide::fade(IM_COL32(255, 255, 255, 24), a), sc.px(2));
            const float frac = float(rows_visible) / float(rows.size());
            const float top = float(d.first_row) / float(rows.size());
            dl->AddRectFilled(ImVec2(track_x, y + top * list_h),
                              ImVec2(track_x + sc.px(4), y + (top + frac) * list_h - sc.px(6)),
                              guide::fade(guide::col::green, a), sc.px(2));
        }
        y += list_h + sc.px(14);
    }

    // the choices, stacked; the mouse focuses what it points at and a click answers
    if (!d.achievements) {
        const ImVec2 mouse_delta = ImGui::GetIO().MouseDelta;
        for (size_t i = 0; i < d.choices.size(); i++) {
            const ImVec2 cmin(x, y), cmax(x + inner, y + choice_h);
            ImGui::SetCursorScreenPos(cmin);
            ImGui::PushID(int(i));
            const bool clicked = ImGui::InvisibleButton("##choice", ImVec2(inner, choice_h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered && (mouse_delta.x != 0 || mouse_delta.y != 0)) d.selected = i;
            const bool on = i == d.selected;
            guide::item(dl, cmin, cmax, sc.px(8), on, hovered, a);
            const ImVec2 cs = guide::measure(on ? f.strong : f.body, body_size, d.choices[i].c_str());
            guide::text(dl, on ? f.strong : f.body, body_size,
                        ImVec2(cmin.x + sc.px(18), cmin.y + (choice_h - cs.y) / 2),
                        guide::fade(on ? guide::col::text_on_green : guide::col::text, a), d.choices[i].c_str());
            if (clicked) {
                d.selected = i;
                ImGui::End();
                ui.navigate(Nav::Accept);
                return;
            }
            y += choice_h + choice_gap;
        }
    }

    // the footer: the controller's buttons, at the right
    const Hints h = hints_for(d);
    const float hh = sc.px(28);
    const float fy = max.y - pad * 0.6f - hh;
    dl->AddLine(ImVec2(x, fy - sc.px(10)), ImVec2(x + inner, fy - sc.px(10)),
                guide::fade(IM_COL32(255, 255, 255, 18), a), 1.0f);
    const float wb = guide::hint(dl, ImVec2(), hh, 'B', h.b, a, false);
    const float wa = guide::hint(dl, ImVec2(), hh, 'A', h.a, a, false);
    float hx = max.x - pad - wb;
    guide::hint(dl, ImVec2(hx, fy), hh, 'B', h.b, a);
    hx -= sc.px(26) + wa;
    guide::hint(dl, ImVec2(hx, fy), hh, 'A', h.a, a);
    ImGui::End();
}

// The toasts: capsules rising at the bottom centre, the mark in a disc at the left, the achievement's picture at
// the right, eased in and faded out.
void draw_toasts(SystemUi &ui, IconCache &icons, double now) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const Scale sc = guide::scale_for(vp->Size);
    const guide::Fonts &f = guide::fonts();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const double lifetime = 5.0;
    const std::vector<ToastItem> items = ui.toasts(now, lifetime);
    float bottom = vp->Pos.y + vp->Size.y - sc.px(54);
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        const ToastItem &t = *it;
        const double age = t.born < 0 ? 0 : now - t.born;
        const float in = guide::ease_out(float(age / 0.35));
        const float out = guide::clamp01(float((lifetime - age) / 0.45));
        const float a = in * out;
        const bool achievement = t.toast.kind == kernel::Toast::Kind::Achievement;

        const float h = sc.px(68), disc = h / 2 - sc.px(5);
        const float small_size = sc.px(15), strong_size = sc.px(21);
        std::string top, main;
        if (achievement) {
            top = "Achievement unlocked";
            main = std::to_string(t.toast.gamerscore) + "G \xe2\x80\x93 " + utf8_of(t.toast.title);
        } else {
            top = "Notification";
            main = utf8_of(t.toast.text);
        }
        const float max_text = std::min(vp->Size.x * 0.5f, sc.px(620));
        const ImVec2 ms = guide::measure(f.strong, strong_size, main.c_str());
        const ImVec2 tsz = guide::measure(f.small, small_size, top.c_str());
        const float text_w = std::min(max_text, std::max(ms.x, tsz.x));
        ImTextureID tex = 0;
        if (achievement && !t.toast.icon.empty()) {
            const uint32_t id = 0x80000000u | (uint32_t(std::hash<std::string_view>{}(std::string_view(
                                                   reinterpret_cast<const char *>(t.toast.icon.data()), t.toast.icon.size()))) &
                                               0x7FFFFFFFu);
            tex = icons.get(id, t.toast.icon);
        }
        const float picture = tex ? h - sc.px(14) : 0;
        const float width = sc.px(6) + 2 * disc + sc.px(18) + text_w + sc.px(22) + (tex ? picture + sc.px(8) : sc.px(8));
        const float rise = (1 - in) * sc.px(30);
        const ImVec2 min(vp->Pos.x + (vp->Size.x - width) / 2, bottom - h + rise), max(min.x + width, bottom + rise);

        guide::panel(dl, min, max, h / 2, a);
        guide::mark(dl, ImVec2(min.x + sc.px(5) + disc, min.y + h / 2), disc, a);
        const float tx = min.x + sc.px(5) + 2 * disc + sc.px(16);
        dl->PushClipRect(ImVec2(tx, min.y), ImVec2(tx + text_w, max.y), true);
        guide::text(dl, f.small, small_size, ImVec2(tx, min.y + sc.px(13)), guide::fade(guide::col::green_hi, a),
                    top.c_str());
        guide::text(dl, f.strong, strong_size, ImVec2(tx, min.y + sc.px(13) + small_size + sc.px(3)),
                    guide::fade(guide::col::text, a), main.c_str());
        dl->PopClipRect();
        if (tex) {
            const ImVec2 pmin(max.x - sc.px(14) - picture, min.y + sc.px(7));
            dl->AddImageRounded(tex, pmin, ImVec2(pmin.x + picture, pmin.y + picture), ImVec2(0, 0), ImVec2(1, 1),
                                guide::fade(IM_COL32(255, 255, 255, 255), a), picture / 2);
        }
        bottom -= (h + sc.px(12)) * in;
    }
}

} // namespace

void draw_system_ui(SystemUi &ui, IconCache &icons, double now) {
    if (Dialog *d = ui.front()) draw_dialog(ui, icons, *d, now);
    draw_toasts(ui, icons, now);
}

} // namespace xf::frontend
