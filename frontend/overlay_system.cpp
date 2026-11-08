#include "overlay_system.h"

#include "console_options.h"
#include "profiles.h"

#include <imgui.h>

#include <cstring>

namespace xf::frontend {

namespace {

// a combo over a list of choices; true when another was picked (its value into `value`)
bool choice_combo(const char *label, const std::vector<Option> &list, int &value) {
    bool changed = false;
    if (ImGui::BeginCombo(label, list[index_of(list, value)].name)) {
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

bool ConsolePages::draw(Host &h, double now) {
    Settings &s = h.settings;
    bool changed = false;
    if (!loaded_) {
        loaded_ = true;
        std::snprintf(gamertag_, sizeof(gamertag_), "%s", kernel::profile_summary(h.kernel).gamertag.c_str());
    }

    ImGui::SeparatorText("System");
    bool system = false;
    system |= choice_combo("Language", languages(), s.system.language);
    system |= choice_combo("Country", countries(), s.system.country);
    int height = resolution_of(s.system);
    if (choice_combo("Resolution", resolutions(), height)) set_resolution(s.system, height), system = true;
    system |= choice_combo("Aspect", widescreen_modes(), s.system.widescreen);
    system |= choice_combo("Refresh rate", refresh_rates(), s.system.refresh);
    if (system) {
        clamp_system(s.system);
        h.apply_system();
        restart_needed_ = true, changed = true;
    }

    ImGui::SeparatorText("Profile");
    std::string why;
    const bool valid = valid_gamertag(gamertag_, why);
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("Gamertag", gamertag_, sizeof(gamertag_));
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

    if (ImGui::BeginTable("profiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const ProfileEntry &e : list_profiles(h.storage, s)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", e.selected ? "> " : "  ", e.name.empty() ? "(unnamed)" : e.name.c_str());
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
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("New profile", new_name_, sizeof(new_name_));
    ImGui::SameLine();
    std::string new_why;
    ImGui::BeginDisabled(!valid_gamertag(new_name_, new_why));
    if (ImGui::Button("Create")) {
        std::string xuid;
        if (create_profile(h.storage, s, new_name_, xuid, error_)) new_name_[0] = 0, changed = true, error_.clear();
    }
    ImGui::EndDisabled();

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

    if (restart_needed_) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1),
                           "The title reads these when it starts: restart it to apply them.");
        if (ImGui::Button("Restart the title")) h.restart();
    }

    if (ImGui::CollapsingHeader("Achievements")) {
        if (now - summary_at_ >= 1.0) summary_ = kernel::profile_summary(h.kernel), summary_at_ = now;
        ImGui::Text("%s: %u / %u G", summary_.gamertag.c_str(), summary_.gamerscore, summary_.gamerscore_total);
        if (summary_.achievements.empty()) ImGui::TextDisabled("No achievements listed for this title.");
        ImGui::BeginChild("achievements", ImVec2(0, 260), ImGuiChildFlags_Borders);
        for (const kernel::ProfileAchievement &a : summary_.achievements) {
            ImGui::PushID(int(a.id));
            if (ImTextureID tex = h.icons.get(a.image_id, a.icon))
                ImGui::ImageWithBg(tex, ImVec2(40, 40), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                             a.unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.45f, 0.45f, 1));
            else
                ImGui::Dummy(ImVec2(40, 40));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(a.unlocked ? ImVec4(0.55f, 0.9f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s  %u G%s",
                               utf8_of(a.label).c_str(), a.gamerscore, a.unlocked ? "  (unlocked)" : "");
            ImGui::TextWrapped("%s", utf8_of(a.unlocked || a.unachieved.empty() ? a.description : a.unachieved).c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    return changed;
}

} // namespace xf::frontend
