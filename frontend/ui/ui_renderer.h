// RmlUi's render interface on XenoForge's Vulkan (docs/runtime.md, "The
// launcher"). It does what RmlUi 6 asks of a renderer: geometry with
// textures, scissor and a stencil clip mask, transforms, gradients (linear,
// radial, conic), and layers with filters (blur, drop shadow, opacity, color
// matrices, mask images), which are what box shadows, rounded clipping of
// pictures and frosted panels are made of. After the structure of RmlUi's
// own GL3 renderer (MIT, Backends/RmlUi_Renderer_GL3.cpp), written again for
// Vulkan 1.3's dynamic rendering.
//
// A frame is recorded into a command buffer the caller owns and submits
// (UiWindow does, with the swapchain): begin_frame(), the context's
// Render(), end_frame(); the picture is then output(), the base layer, 8-bit
// RGBA, premultiplied, in VK_IMAGE_LAYOUT_GENERAL. Every image here stays in
// GENERAL, with one global memory barrier between passes: this is a UI, not
// a game, and the simplicity is worth more than the layout transitions.
// What RmlUi releases while a frame is still on the GPU is destroyed once
// that frame is done (begin_frame is told which have finished).

#pragma once

#include "xenoforge/vk_device.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xf::ui {

class Renderer final : public Rml::RenderInterface {
  public:
    Renderer();
    ~Renderer() override;
    bool open(vk::Device &device, std::string &error);

    // A picture the document can name without a file: "mem:<name>" in an
    // <img> or a decorator. png: a PNG's bytes (the game's icon from its
    // XDBF). False when it does not decode.
    bool add_image(const std::string &name, const std::vector<uint8_t> &png);
    // The same picture as a soft colour field ("soft_" + name): shrunk, blurred
    // and enriched on the CPU, for backdrops, which would cost a full-screen
    // filter every frame (and fade at the window's edges) on the GPU.
    bool add_soft_image(const std::string &name, const std::vector<uint8_t> &png);
    void remove_image(const std::string &name);
    // The window's backdrop, drawn under the interface at the start of every
    // frame, in place of an element's own background: the launcher's grey
    // gradient with ribbons over it (ui.frag, MODE_BACKDROP): two greys at the
    // left, four greens at the right, tones of `tint`. `time` is the ribbons'
    // clock in seconds (a fixed value holds them still); `strength` 0 leaves
    // the gradient alone. Not `on`: nothing drawn, the base layer starts clear.
    // `light` is the light theme's gradient, #3e4241 to #919191, in place of the dark theme's.
    struct Backdrop {
        bool on = false, light = false;
        float time = 0, strength = 1;
        float tint[3] = {0.33f, 0.66f, 0.16f};
    };
    void set_backdrop(const Backdrop &backdrop);
    // Multisampling for the interface's layers (the stencil and the clip masks too): the most of 8, 4 and 2
    // that is not above `samples` and that the device takes, applied at the next frame (the targets and
    // pipelines are made again); 1 draws without. The filters' images stay at one sample.
    void set_msaa(uint32_t samples);
    uint32_t msaa() const; // the sample count in use

    // The frame recorded into cmd (its resources free: the frame this slot
    // had before is done). `serial` numbers the frame; `completed` is the
    // newest serial the GPU has finished.
    void begin_frame(VkCommandBuffer cmd, uint32_t slot, uint32_t width, uint32_t height, uint64_t serial,
                     uint64_t completed);
    void end_frame();
    VkImage output() const;
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // draws and passes in the last frame, for the counters
    uint32_t draws() const { return draws_; }
    uint32_t passes() const { return passes_; }

    // Rml::RenderInterface
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i &texture_dimensions, const Rml::String &source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;
    void SetTransform(const Rml::Matrix4f *transform) override;
    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;
    Rml::TextureHandle SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
    Rml::CompiledFilterHandle CompileFilter(const Rml::String &name, const Rml::Dictionary &parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String &name, const Rml::Dictionary &parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
    vk::Device *device_ = nullptr;
    uint32_t width_ = 0, height_ = 0, draws_ = 0, passes_ = 0;
};

} // namespace xf::ui
