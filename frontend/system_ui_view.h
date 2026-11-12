// The view of the system's dialogs and toasts (system_ui.h's model) in Dear ImGui, on the player's window: a
// centered modal over the game's picture for the dialog on top, the unlocked achievements' toasts in the corner.
// Navigation is the model's (the pad and the keyboard feed SystemUi::navigate); the mouse clicks a choice.

#pragma once

#include "guide_theme.h"
#include "system_ui.h"

#include "xenoforge/vk_device.h"

#include <imgui.h>

#include <map>
#include <memory>

namespace xf::frontend {

// PNG icons (the SPA's achievement images) as Dear ImGui textures, made on first use and kept by their id
class IconCache {
  public:
    explicit IconCache(vk::Device &device) : device_(device) {}
    ~IconCache();
    // the texture for an icon, 0 when it has no picture or it cannot be made
    ImTextureID get(uint32_t id, const std::vector<uint8_t> &png);

  private:
    struct Icon {
        vk::Image image;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    vk::Device &device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::map<uint32_t, Icon> icons_;
};

// Draws the top dialog and the toasts (between ImGui::NewFrame and Render); `now` in seconds. The dialog's
// choices clicked and a keyboard dialog's Enter answer through `ui`.
void draw_system_ui(SystemUi &ui, IconCache &icons, double now);

// One achievement as a row of the console's lists (the achievements dialog, the guide's page): its picture, its
// name and description, its gamerscore in a pill; lit when focused, greyed while locked.
void draw_achievement_row(ImDrawList *dl, const guide::Scale &sc, ImVec2 min, ImVec2 max,
                          const kernel::ProfileAchievement &achievement, ImTextureID picture, bool focused,
                          bool hovered, float alpha);

} // namespace xf::frontend
