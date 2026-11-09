#include "overlay_system.h"

#include "console_options.h"
#include "guide_theme.h"
#include "profiles.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xf::frontend {

namespace {

// In the guide, a setting's name at the left and its control filling the rest of the row; elsewhere ImGui's own
// layout (the name after the control). Returns the label to give the control.
float g_label_w = 0;
std::string labelled(const char *label) {
    if (g_label_w <= 0) return label;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(g_label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return std::string("##") + label;
}

// a section's caption in the guide: small, dim, spaced
void caption(const char *text) {
    if (g_label_w <= 0) {
        ImGui::SeparatorText(text);
        return;
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, guide::col::green_hi);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// a combo over a list of choices; true when another was picked (its value into `value`)
bool choice_combo(const char *label, const std::vector<Option> &list, int &value) {
    bool changed = false;
    if (ImGui::BeginCombo(labelled(label).c_str(), list[index_of(list, value)].name)) {
        for (const Option &c : list) {
            if (ImGui::Selectable(c.name, c.value == value)) {
                value = c.value;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

std::string guide_label(const char *label) { return labelled(label); }
void guide_caption(const char *text) { caption(text); }

void ConsolePages::load(Host &h) {
    if (loaded_) return;
    loaded_ = true;
    std::snprintf(gamertag_, sizeof(gamertag_), "%s", kernel::profile_summary(h.kernel).gamertag.c_str());
}

bool ConsolePages::system_page(Host &h) {
    Settings &s = h.settings;
    bool system = false;
    system |= choice_combo("Language", languages(), s.system.language);
    system |= choice_combo("Country", countries(), s.system.country);
    if (g_label_w > 0) caption("Video");
    int height = resolution_of(s.system);
    if (choice_combo("Resolution", resolutions(), height)) set_resolution(s.system, height), system = true;
    system |= choice_combo("Aspect", widescreen_modes(), s.system.widescreen);
    system |= choice_combo("Refresh rate", refresh_rates(), s.system.refresh);
    if (system) {
        clamp_system(s.system);
        h.apply_system();
        restart_needed_ = true;
    }
    return system;
}

// The emulator's picture options (settings.h's video section): the resolution scale and the minimum scalable
// dimension wait for a restart, the rest applies at once.
bool ConsolePages::picture_page(Host &h) {
    Settings &s = h.settings;
    bool changed = false, live = false;
    auto combo = [&](const char *label, const char *key) {
        const std::vector<VideoChoice> list = video_choices(key);
        const std::string current = video_value(s, key);
        std::string shown = current;
        for (const VideoChoice &c : list)
            if (c.value == current) shown = c.label;
        if (ImGui::BeginCombo(labelled(label).c_str(), shown.c_str())) {
            for (const VideoChoice &c : list) {
                if (ImGui::Selectable(c.label.c_str(), c.value == current) && c.value != current &&
                    set_video_value(s, key, c.value)) {
                    changed = true;
                    if (video_needs_restart(key)) restart_needed_ = true;
                    else live = true;
                }
            }
            ImGui::EndCombo();
        }
    };
    combo("Resolution scale", "video.resolution_scale");
    combo("Minimum scalable size", "video.min_scalable");
    if (!h.picture_notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.85f, 0.4f, 1));
        ImGui::TextWrapped("%s", h.picture_notice.c_str());
        ImGui::PopStyleColor();
    }
    combo("Output scaling", "video.upscaler");
    if (s.video.upscaler == "fsr") {
        float v = float(s.video.fsr_sharpness);
        if (ImGui::SliderFloat(labelled("FSR sharpness reduction").c_str(), &v, 0.0f, 2.0f, "%.2f stops"))
            s.video.fsr_sharpness = v, changed = live = true;
    } else if (s.video.upscaler == "cas") {
        float v = float(s.video.cas_sharpness);
        if (ImGui::SliderFloat(labelled("CAS sharpness").c_str(), &v, 0.0f, 1.0f, "%.2f"))
            s.video.cas_sharpness = v, changed = live = true;
    }
    combo("Antialiasing", "video.antialiasing");
    combo("Picture shape", "video.aspect");
    combo("Anisotropic filtering", "video.anisotropy");
    combo("Texture filtering", "video.texture_filter");
    combo("Render targets", "video.edram");
    if (g_label_w > 0) {
        changed |= ImGui::Checkbox(labelled("Frame-time graph (F1)").c_str(), &s.video.frame_graph);
    } else {
        changed |= ImGui::Checkbox("Frame-time graph", &s.video.frame_graph);
    }
    if (live) {
        clamp_video(s);
        if (h.apply_video) h.apply_video();
    }
    // (the guide's System page shows the restart notice under all of its sections)
    if (g_label_w <= 0) restart_notice(h);
    return changed;
}

bool ConsolePages::profile_page(Host &h, float field_width) {
    Settings &s = h.settings;
    bool changed = false;
    std::string why;
    const bool valid = valid_gamertag(gamertag_, why);
    caption("Gamertag");
    const float rename_w = ImGui::CalcTextSize("Rename").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(g_label_w > 0 ? ImGui::GetContentRegionAvail().x - rename_w - ImGui::GetStyle().ItemSpacing.x
                                          : field_width);
    ImGui::InputText(g_label_w > 0 ? "##gamertag" : "Gamertag", gamertag_, sizeof(gamertag_));
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Rename")) {
        const auto profiles = list_profiles(h.storage, s);
        std::string xuid;
        for (const ProfileEntry &e : profiles)
            if (e.selected) xuid = e.xuid;
        if (xuid.empty()) {
            // no profile on disk yet: the name is the default profile's, kept in the settings
            s.profile.gamertag = gamertag_;
            kernel::set_gamertag(h.kernel, gamertag_);
            changed = true, error_.clear();
        } else if (rename_profile(s, xuid, gamertag_, error_)) {
            kernel::set_gamertag(h.kernel, gamertag_); // at once: the dialogs show it from now on
            changed = true, error_.clear();
        }
    }
    ImGui::EndDisabled();
    if (!valid && gamertag_[0]) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", why.c_str());
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", error_.c_str());

    caption("Profiles");
    if (ImGui::BeginTable("profiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const ProfileEntry &e : list_profiles(h.storage, s)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (e.selected)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(guide::col::green_hi), "%s",
                                   e.name.empty() ? "(unnamed)" : e.name.c_str());
            else
                ImGui::Text("%s", e.name.empty() ? "(unnamed)" : e.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e.xuid.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(e.xuid.c_str());
            if (!e.selected && ImGui::SmallButton("Use")) {
                if (select_profile(s, e.xuid, error_)) restart_needed_ = true, changed = true, error_.clear();
            }
            ImGui::SameLine();
            if (confirm_delete_ == e.xuid) {
                if (ImGui::SmallButton("Delete with its saves?")) {
                    if (delete_profile(h.storage, s, e.xuid, error_)) restart_needed_ = true, changed = true, error_.clear();
                    confirm_delete_.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Keep")) confirm_delete_.clear();
            } else if (ImGui::SmallButton("Delete")) {
                confirm_delete_ = e.xuid;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    caption("New profile");
    const float create_w = ImGui::CalcTextSize("Create").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(g_label_w > 0 ? ImGui::GetContentRegionAvail().x - create_w - ImGui::GetStyle().ItemSpacing.x
                                          : field_width);
    ImGui::InputTextWithHint(g_label_w > 0 ? "##new" : "New profile", "A gamertag", new_name_, sizeof(new_name_));
    ImGui::SameLine();
    std::string new_why;
    ImGui::BeginDisabled(!valid_gamertag(new_name_, new_why));
    if (ImGui::Button("Create")) {
        std::string xuid;
        if (create_profile(h.storage, s, new_name_, xuid, error_)) new_name_[0] = 0, changed = true, error_.clear();
    }
    ImGui::EndDisabled();

    caption("Saves");
    bool directory = s.saves.format == "directory";
    if (ImGui::RadioButton("Saves as STFS packages", !directory) && directory) {
        s.saves.format = "stfs", changed = true;
        kernel::set_content_format(h.kernel, kernel::ContentFormat::Stfs);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Saves as folders (Xenia's)", directory) && !directory) {
        s.saves.format = "directory", changed = true;
        kernel::set_content_format(h.kernel, kernel::ContentFormat::Directory);
    }
    return changed;
}

void ConsolePages::restart_notice(Host &h) {
    if (!restart_needed_) return;
    ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "The title reads these when it starts: restart it to apply them.");
    if (ImGui::Button("Restart the title")) h.restart();
}

void ConsolePages::achievement_rows(Host &h, double now, const guide::Scale &sc, float width) {
    if (now - summary_at_ >= 1.0) summary_ = kernel::profile_summary(h.kernel), summary_at_ = now;
    if (summary_.achievements.empty()) {
        ImGui::TextDisabled("No achievements listed for this title.");
        return;
    }
    const float row_h = sc.px(74);
    for (size_t i = 0; i < summary_.achievements.size(); i++) {
        const kernel::ProfileAchievement &ach = summary_.achievements[i];
        ImGui::PushID(int(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // a selectable under each row: the pad's and the mouse's focus, the list's scrolling
        ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(width, row_h - sc.px(8)));
        const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavActive;
        draw_achievement_row(ImGui::GetWindowDrawList(), sc, at, ImVec2(at.x + width, at.y + row_h - sc.px(8)), ach,
                             h.icons.get(ach.image_id, ach.icon), focused, ImGui::IsItemHovered(), 1);
        ImGui::PopID();
    }
}

// The guide: a panel sliding in from the left over the game, the profile on top (the mark, the gamertag, the
// title's gamerscore), three pages switched by the shoulders (or the mouse), ImGui's widgets in the guide's look,
// the controller's buttons at the bottom. `open` goes false when it is closed from inside (B, Escape, the X).
bool ConsolePages::draw_guide(Host &h, double now, bool &open) {
    load(h);
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const guide::Scale sc = guide::scale_for(vp->Size);
    const guide::Fonts &f = guide::fonts();
    if (guide_shown_at_ < 0) guide_shown_at_ = now;
    const float a = guide::ease_out(float((now - guide_shown_at_) / 0.22));
    if (now - summary_at_ >= 1.0) summary_ = kernel::profile_summary(h.kernel), summary_at_ = now;

    const float width = std::min(std::max(sc.px(560), vp->Size.x * 0.40f), vp->Size.x - sc.px(24));
    const float slide = (1 - a) * (width + sc.px(40));
    const ImVec2 min(vp->Pos.x - sc.px(16) - slide, vp->Pos.y + sc.px(16));
    const ImVec2 max(min.x + sc.px(16) + width, vp->Pos.y + vp->Size.y - sc.px(16));
    const float pad = sc.px(26);

    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  guide::fade(IM_COL32(0, 0, 0, 255), 0.35f * a));
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(ImVec2(max.x - min.x, max.y - min.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##guide", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    guide::panel(dl, min, max, sc.px(14), a);

    // the profile: the mark, the gamertag, the gamerscore
    const float x = min.x + sc.px(16) + pad;
    float y = min.y + pad;
    const float disc = sc.px(30);
    guide::mark(dl, ImVec2(x + disc, y + disc), disc, a);
    const std::string tag = summary_.gamertag.empty() ? std::string("Player") : summary_.gamertag;
    guide::text(dl, f.title, sc.px(28), ImVec2(x + 2 * disc + sc.px(16), y + sc.px(2)), guide::fade(guide::col::text, a),
                tag.c_str());
    char score[64];
    std::snprintf(score, sizeof(score), "%u / %u G in this title", summary_.gamerscore, summary_.gamerscore_total);
    guide::text(dl, f.small, sc.px(16), ImVec2(x + 2 * disc + sc.px(16), y + sc.px(38)),
                guide::fade(guide::col::text_dim, a), score);
    y += 2 * disc + sc.px(18);
    const float inner = max.x - pad - x;
    guide::accent_line(dl, ImVec2(x, y), inner, sc.px(2), a);
    y += sc.px(16);

    // the pages' tabs
    std::vector<std::string> names = {"Achievements", "Profile", "System"};
    for (const Host::Page &page : h.extra_pages) names.push_back(page.name);
    const int pages = int(names.size());
    tab_ = std::min(tab_, pages - 1);
    const float tab_h = sc.px(40), tab_gap = sc.px(6);
    // the names at 18 pixels, shrunk together when the row would not fit; each tab as wide as its name
    float want = 0;
    for (const std::string &n : names) want += guide::measure(f.strong, sc.px(18), n.c_str()).x + sc.px(24);
    const float fit = std::min(1.0f, (inner - float(pages - 1) * tab_gap) / want);
    const float tab_font = sc.px(18) * fit;
    float tab_x = x;
    for (int i = 0; i < pages; i++) {
        const float tab_w = (guide::measure(f.strong, sc.px(18), names[size_t(i)].c_str()).x + sc.px(24)) * fit;
        const ImVec2 tmin(tab_x, y), tmax(tmin.x + tab_w, y + tab_h);
        tab_x += tab_w + tab_gap;
        ImGui::SetCursorScreenPos(tmin);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tab_w, tab_h))) tab_ = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        guide::item(dl, tmin, tmax, sc.px(8), tab_ == i, hovered, a);
        const ImVec2 ts = guide::measure(f.strong, tab_font, names[size_t(i)].c_str());
        guide::text(dl, f.strong, tab_font, ImVec2(tmin.x + (tab_w - ts.x) / 2, tmin.y + (tab_h - ts.y) / 2),
                    guide::fade(tab_ == i ? guide::col::text_on_green : guide::col::text_dim, a),
                    names[size_t(i)].c_str());
    }
    y += tab_h + sc.px(16);

    // the page: ImGui's widgets in the guide's look, scrolling inside
    const float footer_h = sc.px(52);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    guide::push_widgets(sc);
    ImGui::PushFont(f.body);
    ImGui::SetWindowFontScale(sc.px(19) / (f.body->FontSize * ImGui::GetIO().FontGlobalScale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    bool changed = false;
    if (ImGui::BeginChild("##page", ImVec2(inner, max.y - footer_h - y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NavFlattened)) {
        ImGui::SetWindowFontScale(sc.px(19) / (f.body->FontSize * ImGui::GetIO().FontGlobalScale));
        g_label_w = inner * 0.36f;
        switch (tab_) {
        case 0: achievement_rows(h, now, sc, inner - sc.px(14)); break;
        case 1:
            changed |= profile_page(h, inner * 0.55f);
            restart_notice(h);
            break;
        case 2:
            caption("Language and region");
            changed |= system_page(h);
            caption("Picture (beyond the console)");
            changed |= picture_page(h);
            restart_notice(h);
            break;
        default:
            changed |= h.extra_pages[size_t(tab_ - 3)].draw();
            break;
        }
        g_label_w = 0;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1);
    ImGui::PopFont();
    guide::pop_widgets();

    // the footer: the controller's buttons
    const float hh = sc.px(28), fy = max.y - pad * 0.5f - hh - sc.px(6);
    dl->AddLine(ImVec2(x, fy - sc.px(10)), ImVec2(x + inner, fy - sc.px(10)),
                guide::fade(IM_COL32(255, 255, 255, 18), a), 1.0f);
    float hx = x;
    hx += guide::hint(dl, ImVec2(hx, fy), hh, 'A', "Select", a) + sc.px(22);
    hx += guide::hint(dl, ImVec2(hx, fy), hh, 'B', "Close", a) + sc.px(22);
    const ImVec2 ps = guide::measure(f.small, hh * 0.62f, "LB / RB  pages");
    guide::text(dl, f.small, hh * 0.62f, ImVec2(hx, fy + (hh - ps.y) / 2), guide::fade(guide::col::text_dim, a),
                "LB / RB  pages");

    // closing from inside: Escape, or B with nothing being edited
    if (!ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        open = false;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false))
        tab_ = (tab_ + pages - 1) % pages;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false))
        tab_ = (tab_ + 1) % pages;
    ImGui::End();
    if (!open) guide_shown_at_ = -1;
    return changed;
}

} // namespace xf::frontend
