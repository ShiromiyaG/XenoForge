// The system dialogs' view (system_ui_view.h).

#include "system_ui_view.h"

#include <imgui_impl_vulkan.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstring>

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

namespace {

const char *icon_name(int icon) {
    switch (icon) {
    case 1: return "Error";
    case 2: return "Warning";
    case 3: return "Information";
    default: return nullptr;
    }
}

// the choices as buttons in a row; the selected one is drawn pressed. Clicking one selects and accepts it.
void choices_row(SystemUi &ui, Dialog &d) {
    for (size_t i = 0; i < d.choices.size(); i++) {
        if (i) ImGui::SameLine();
        const bool on = i == d.selected;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button((d.choices[i] + "##c" + std::to_string(i)).c_str(), ImVec2(120, 0))) {
            d.selected = i;
            if (on) ImGui::PopStyleColor();
            ui.navigate(Nav::Accept);
            return;
        }
        if (on) ImGui::PopStyleColor();
    }
}

void draw_dialog(SystemUi &ui, IconCache &icons, Dialog &d) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    // the game dimmed behind
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(0, 0, 0, 140));
    const float width = std::min(640.0f, vp->Size.x - 40);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, d.achievements ? std::min(520.0f, vp->Size.y - 40) : 0));
    ImGui::SetNextWindowFocus();
    ImGui::Begin(d.title.c_str(), nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | (d.achievements ? 0 : ImGuiWindowFlags_AlwaysAutoResize));
    if (const char *name = icon_name(d.icon)) ImGui::TextDisabled("%s", name);
    for (const std::string &line : d.lines) ImGui::TextWrapped("%s", line.c_str());
    if (d.text_entry) {
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        // (the room is in characters, the field's in bytes: a few more, then the answer is cut by characters)
        std::string buffer = d.text;
        buffer.resize(std::max<size_t>(buffer.size(), 1) + 256);
        if (ImGui::InputText("##text", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            d.text = buffer.c_str();
            ui.confirm_text();
            ImGui::End();
            return;
        }
        d.text = buffer.c_str();
        if (d.max_length) ImGui::TextDisabled("%zu / %u characters", utf16_of(d.text).size(), d.max_length);
    }
    if (d.achievements) {
        ImGui::Separator();
        ImGui::BeginChild("rows", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f));
        if (d.profile.achievements.empty())
            ImGui::TextDisabled("This title lists no achievements (or has not read them yet).");
        for (size_t i = d.first_row; i < d.profile.achievements.size(); i++) {
            const kernel::ProfileAchievement &a = d.profile.achievements[i];
            ImGui::PushID(int(i));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (i == d.first_row)
                ImGui::GetWindowDrawList()->AddRectFilled(
                    at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + 56), IM_COL32(255, 255, 255, 30));
            if (ImTextureID tex = icons.get(a.image_id, a.icon))
                ImGui::ImageWithBg(tex, ImVec2(48, 48), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                             a.unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.45f, 0.45f, 1));
            else
                ImGui::Dummy(ImVec2(48, 48));
            ImGui::SameLine();
            ImGui::BeginGroup();
            const std::string label = utf8_of(a.label);
            ImGui::TextColored(a.unlocked ? ImVec4(0.55f, 0.9f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s  %u G%s",
                               label.c_str(), a.gamerscore, a.unlocked ? "  (unlocked)" : "");
            const std::u16string &text = a.unlocked || a.unachieved.empty() ? a.description : a.unachieved;
            ImGui::TextWrapped("%s", utf8_of(text).c_str());
            ImGui::EndGroup();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Up/Down scroll, A or Enter closes");
    }
    ImGui::Spacing();
    if (!d.achievements) ImGui::Separator();
    choices_row(ui, d);
    ImGui::End();
}

void draw_toasts(SystemUi &ui, IconCache &icons, double now) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float y = vp->Pos.y + 16;
    int n = 0;
    for (const ToastItem &t : ui.toasts(now)) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 16, y), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowSizeConstraints(ImVec2(320, 0), ImVec2(460, 200));
        ImGui::SetNextWindowBgAlpha(0.85f);
        const bool achievement = t.toast.kind == kernel::Toast::Kind::Achievement;
        ImGui::Begin(("##toast" + std::to_string(n++)).c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav);
        if (achievement) {
            if (ImTextureID tex = icons.get(0x80000000u + uint32_t(n), t.toast.icon)) {
                ImGui::Image(tex, ImVec2(48, 48));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.4f, 1), "Achievement unlocked  %u G", t.toast.gamerscore);
            ImGui::TextWrapped("%s", utf8_of(t.toast.title).c_str());
            ImGui::EndGroup();
        } else {
            ImGui::TextWrapped("%s", utf8_of(t.toast.text).c_str());
        }
        y += ImGui::GetWindowSize().y + 8;
        ImGui::End();
    }
}

} // namespace

void draw_system_ui(SystemUi &ui, IconCache &icons, double now) {
    if (Dialog *d = ui.front()) draw_dialog(ui, icons, *d);
    draw_toasts(ui, icons, now);
}

} // namespace xf::frontend
