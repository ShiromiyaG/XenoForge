// RmlUi's render interface on Vulkan (ui_renderer.h). The structure and the
// filters' arithmetic follow RmlUi's GL3 backend (MIT, Backends/
// RmlUi_Renderer_GL3.cpp); the Vulkan side is this project's own.

#include "ui_renderer.h"
#include "ribbons.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Vertex.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace xf::ui {

namespace {

const uint32_t vert_spv[] = {
#include "ui.vert.inc"
};
const uint32_t frag_spv[] = {
#include "ui.frag.inc"
};

constexpr int MAX_STOPS = 16;
constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkDeviceSize ARENA_CHUNK = 4u << 20;
constexpr VkDeviceSize UNIFORM_RING = 8u << 20;

static_assert(std::is_same_v<Rml::Matrix4f, Rml::ColumnMajorMatrix4f>, "the shaders take column-major matrices");

// the modes of ui.frag
enum Mode : int32_t { M_TEXTURE, M_GRADIENT, M_COLOR_MATRIX, M_MASK, M_SHADOW, M_BLUR, M_CLAMPED, M_BACKDROP };
enum class Blend { Over, Replace };
enum class Clip { None, Test, WriteReplace, WriteIncrement };

// ui_draw.glsl's block, field for field
struct DrawUniforms {
    float transform[16];
    float color_matrix[16];
    float stop_colors[MAX_STOPS][4];
    float stop_positions[MAX_STOPS];
    int32_t imode[4];
    float translate[4];
    float pv[4];
    float color[4];
    float limits[4];
    float blur_weights[4];
    float texel[4];
    float uv_transform[4];
};
static_assert(sizeof(DrawUniforms) == 576);

struct Texture {
    vk::Image image;
    VkImageView view = VK_NULL_HANDLE;
    // a layer with multisampling draws into this, and `image` is what it resolves to at the end of every pass
    vk::Image ms;
    VkImageView ms_view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    int width = 0, height = 0;
};

// vertices and indices of one geometry, in a chunk of the arena
struct Geometry {
    size_t chunk = 0;
    VkDeviceSize offset = 0, size = 0; // the allocation
    VkDeviceSize index_offset = 0;     // from the chunk's start
    uint32_t count = 0;
};

enum class FilterType { Invalid, Opacity, Blur, DropShadow, ColorMatrix, MaskImage };
struct Filter {
    FilterType type = FilterType::Invalid;
    float value = 1; // opacity
    float sigma = 0;
    Rml::Vector2f offset;
    Rml::ColourbPremultiplied color;
    Rml::Matrix4f color_matrix;
};

enum class ShaderType { Invalid, Gradient };
struct Shader {
    ShaderType type = ShaderType::Invalid;
    int function = 0; // ui.frag's LINEAR ... REPEATING_CONIC
    Rml::Vector2f p, v;
    std::vector<float> positions;
    std::vector<Rml::Colourf> colors;
};

struct MemoryImage {
    std::vector<uint8_t> rgba; // straight alpha
    int width = 0, height = 0;
};

Rml::Colourf to_colorf(Rml::ColourbPremultiplied c) {
    Rml::Colourf r;
    for (int i = 0; i < 4; i++) r[i] = float(c[i]) / 255.f;
    return r;
}

// straight-alpha RGBA made premultiplied, in place
void premultiply(uint8_t *rgba, size_t pixels) {
    for (size_t i = 0; i < pixels; i++, rgba += 4) {
        const uint32_t a = rgba[3];
        if (a == 255) continue;
        rgba[0] = uint8_t((rgba[0] * a + 127) / 255);
        rgba[1] = uint8_t((rgba[1] * a + 127) / 255);
        rgba[2] = uint8_t((rgba[2] * a + 127) / 255);
    }
}

} // namespace

struct Renderer::Impl {
    static constexpr uint32_t SLOTS = 2;

    vk::Device *dev = nullptr;
    VkDevice d = VK_NULL_HANDLE;

    // pipelines
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout ubo_layout = VK_NULL_HANDLE, tex_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipelines[3][2][4] = {}; // by depth (none, test and write, test), blend and clip
    VkPipeline flat[2] = {};            // with multisampling, the passes over single-sample images (by blend): no stencil
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkFormat stencil_format = VK_FORMAT_UNDEFINED;

    // multisampling: what the layers are drawn with (1: none), what was asked, and what the device does
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t requested = 1;
    VkSampleCountFlags supported = VK_SAMPLE_COUNT_1_BIT;

    // descriptors and per-draw uniforms
    VkDescriptorPool ubo_pool = VK_NULL_HANDLE;
    VkDescriptorSet ubo_sets[SLOTS] = {};
    vk::Buffer ubo[SLOTS];
    VkDeviceSize ubo_stride = 0, ubo_offset = 0;
    std::vector<VkDescriptorPool> tex_pools;

    // geometry arena
    struct Chunk {
        vk::Buffer buffer;
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free; // offset, size, sorted
    };
    std::vector<Chunk> chunks;
    Geometry quad; // the layers' passes

    // the frame
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t slot = 0, width = 0, height = 0;
    uint64_t serial = 0;
    uint32_t draws = 0, passes = 0;
    bool overflow_reported = false;

    // targets: the layer stack (0 is the base), the filters' images, the mask
    std::vector<std::unique_ptr<Texture>> layers;
    size_t layer_count = 1;
    std::unique_ptr<Texture> pp[3], mask;
    vk::Image stencil;
    VkImageView stencil_view = VK_NULL_HANDLE;
    Texture *white = nullptr;
    Texture *pass_target = nullptr; // rendering now
    bool first_pass = true;         // the stencil is cleared by the frame's first pass
    VkPipeline bound = VK_NULL_HANDLE;

    // RmlUi's state
    bool scissor_on = false, clip_on = false;
    Rml::Rectanglei scissor = Rml::Rectanglei::MakeInvalid();
    uint32_t clip_ref = 1;
    Rml::Matrix4f projection, transform;

    std::map<std::string, MemoryImage> images;
    Backdrop backdrop;

    struct Deferred {
        uint64_t serial;
        std::function<void()> free;
    };
    std::vector<Deferred> deferred;

    // ---- setup

    bool open(vk::Device &device, std::string &error);
    void close();
    bool make_pipelines(std::string &error);
    void destroy_pipelines();
    // the pipelines' compiled form kept in a file between runs: the driver's compile of the 26 pipelines is most of
    // the time to the first frame on a cold start, and a file read on every later one
    std::filesystem::path pipeline_cache_file;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    size_t pipeline_cache_loaded = 0; // the bytes the file had
    void open_pipeline_cache();
    void save_pipeline_cache();
    VkSampleCountFlagBits pick_samples() const;

    // ---- resources

    void defer(std::function<void()> free) { deferred.push_back({serial, std::move(free)}); }
    void run_deferred(uint64_t completed) {
        size_t kept = 0;
        for (Deferred &item : deferred) {
            if (item.serial <= completed)
                item.free();
            else
                deferred[kept++] = std::move(item);
        }
        deferred.resize(kept);
    }
    VkDescriptorSet alloc_set(VkDescriptorPool &pool_out);
    Texture *make_texture(int w, int h, VkImageUsageFlags usage, const uint8_t *premultiplied);
    void free_texture(Texture *t);
    void destroy_texture_now(Texture *t);
    bool alloc_geometry(Geometry &g, const void *vertices, size_t vertex_bytes, const void *indices,
                        size_t index_bytes);
    void free_geometry_now(const Geometry &g);
    void make_stencil();
    void make_ms(Texture &t);
    void drop_targets();
    Texture &layer(size_t i);
    Texture &filter_image(int i);

    // ---- recording

    void memory_barrier();
    void begin_pass(Texture &target, bool clear = false);
    void end_pass();
    void clear_color(const Rml::Rectanglei &rect);

    struct Draw {
        Texture *target = nullptr;
        Blend blend = Blend::Over;
        Clip clip = Clip::None;
        Texture *tex = nullptr, *mask = nullptr;
        DrawUniforms u{};
        const Geometry *geometry = nullptr;
        bool scissor = false;
        Rml::Rectanglei rect;
        uint32_t viewport_w = 0, viewport_h = 0; // 0: the frame's
        uint32_t stencil_ref = 1;
        int depth = 0; // 0: none, 1: test and write, 2: test only (the backdrop's ribbons)
    };
    Draw make_draw(Texture &target) const;
    void draw(Draw &s);
    void pass(Texture &dst, Texture &src, Mode mode, Blend blend, const Rml::Rectanglei &window, bool windowed,
              const std::function<void(Draw &)> &tweak = {});
    void blur(float sigma, Texture &a, Texture &b, Rml::Rectanglei window);
    void filters(Rml::Span<const Rml::CompiledFilterHandle> list, Texture *&primary, Texture *&secondary,
                 Texture *&tertiary, const Rml::Rectanglei &window);
    Rml::Rectanglei window() const {
        return scissor_on && scissor.Valid() ? scissor : Rml::Rectanglei::FromSize({int(width), int(height)});
    }
    void set_transform(const Rml::Matrix4f *t) { transform = t ? projection * (*t) : projection; }
};

// ---------------------------------------------------------------------------
// setup

bool Renderer::Impl::open(vk::Device &device, std::string &error) {
    dev = &device;
    d = device.device();
    const VkPhysicalDevice physical = device.physical();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    const VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
    ubo_stride = (sizeof(DrawUniforms) + align - 1) / align * align;

    stencil_format = device.format_supported(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                         ? VK_FORMAT_D24_UNORM_S8_UINT
                         : VK_FORMAT_D32_SFLOAT_S8_UINT;

    // the sample counts the layers' formats take as attachments
    supported = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                props.limits.framebufferStencilSampleCounts;
    for (const auto &[format, usage] : {std::pair{COLOR_FORMAT, VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)},
                                        std::pair{stencil_format, VkImageUsageFlags(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)}}) {
        VkImageFormatProperties fp{};
        if (vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                                     &fp) == VK_SUCCESS)
            supported &= fp.sampleCounts;
        else
            supported = VK_SAMPLE_COUNT_1_BIT;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d, &si, nullptr, &sampler) != VK_SUCCESS) return error = "no sampler", false;

    VkDescriptorSetLayoutBinding ub{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &ub;
    vkCreateDescriptorSetLayout(d, &li, nullptr, &ubo_layout);
    VkDescriptorSetLayoutBinding tb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                    nullptr};
    li.pBindings = &tb;
    vkCreateDescriptorSetLayout(d, &li, nullptr, &tex_layout);
    const VkDescriptorSetLayout sets[3] = {ubo_layout, tex_layout, tex_layout};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 3;
    pl.pSetLayouts = sets;
    vkCreatePipelineLayout(d, &pl, nullptr, &pipeline_layout);

    // the uniforms: one ring a frame slot, one descriptor set each
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SLOTS};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = SLOTS;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    vkCreateDescriptorPool(d, &dp, nullptr, &ubo_pool);
    for (uint32_t i = 0; i < SLOTS; i++) {
        ubo[i] = device.create_buffer(UNIFORM_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        if (!ubo[i].mapped) return error = "no uniform buffer", false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ubo_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &ubo_layout;
        vkAllocateDescriptorSets(d, &ai, &ubo_sets[i]);
        VkDescriptorBufferInfo bi{ubo[i].buffer, 0, sizeof(DrawUniforms)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ubo_sets[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
    }
    if (!make_pipelines(error)) return false;
    save_pipeline_cache();

    // a white texel for the draws without a texture, and the layers' quad
    const uint8_t one[4] = {255, 255, 255, 255};
    white = make_texture(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, one);
    if (!white) return error = "no texture", false;
    Rml::Vertex v[4];
    const float xs[4] = {-1, 1, 1, -1}, ys[4] = {-1, -1, 1, 1}, us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; i++) {
        v[i].position = {xs[i], ys[i]};
        v[i].colour = Rml::ColourbPremultiplied(255, 255, 255, 255);
        v[i].tex_coord = {us[i], vs[i]};
    }
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    if (!alloc_geometry(quad, v, sizeof(v), idx, sizeof(idx))) return error = "no geometry", false;
    return true;
}

// the most samples, up to what was asked, that the device takes for the layers (1: none)
VkSampleCountFlagBits Renderer::Impl::pick_samples() const {
    for (const VkSampleCountFlagBits count : {VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT})
        if (uint32_t(count) <= requested && (supported & count)) return count;
    return VK_SAMPLE_COUNT_1_BIT;
}

void Renderer::Impl::destroy_pipelines() {
    for (auto &plane : pipelines)
        for (auto &row : plane)
            for (VkPipeline &p : row) vkDestroyPipeline(d, p, nullptr), p = VK_NULL_HANDLE;
    for (VkPipeline &p : flat) vkDestroyPipeline(d, p, nullptr), p = VK_NULL_HANDLE;
    bound = VK_NULL_HANDLE;
}

void Renderer::Impl::open_pipeline_cache() {
    if (pipeline_cache) return;
    std::vector<char> data;
    if (!pipeline_cache_file.empty()) {
        std::ifstream f(pipeline_cache_file, std::ios::binary);
        if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size(); // the driver ignores data that is not its own (another driver, another version)
    ci.pInitialData = data.data();
    if (vkCreatePipelineCache(d, &ci, nullptr, &pipeline_cache) != VK_SUCCESS) pipeline_cache = VK_NULL_HANDLE;
    pipeline_cache_loaded = data.size();
}

void Renderer::Impl::save_pipeline_cache() {
    if (!pipeline_cache || pipeline_cache_file.empty()) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(d, pipeline_cache, &size, nullptr) != VK_SUCCESS || !size || size == pipeline_cache_loaded) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(d, pipeline_cache, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(pipeline_cache_file.parent_path(), ec);
    const std::filesystem::path tmp = pipeline_cache_file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(data.data(), std::streamsize(size));
        if (!f) return;
    }
    std::filesystem::rename(tmp, pipeline_cache_file, ec);
    pipeline_cache_loaded = size;
}

bool Renderer::Impl::make_pipelines(std::string &error) {
    open_pipeline_cache();
    if (!vert) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(vert_spv);
        mi.pCode = vert_spv;
        vkCreateShaderModule(d, &mi, nullptr, &vert);
        mi.codeSize = sizeof(frag_spv);
        mi.pCode = frag_spv;
        vkCreateShaderModule(d, &mi, nullptr, &frag);
    }

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(Rml::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(Rml::Vertex, position))},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(Rml::Vertex, colour))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(Rml::Vertex, tex_coord))}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 3;
    ds.pDynamicStates = dynamic;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &COLOR_FORMAT;
    rendering.depthAttachmentFormat = stencil_format;
    rendering.stencilAttachmentFormat = stencil_format;

    for (int dm = 0; dm < 3; dm++)
    for (int b = 0; b < 2; b++) {
        for (int c = 0; c < 4; c++) {
            VkPipelineColorBlendAttachmentState att{};
            att.blendEnable = b == int(Blend::Over);
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
            att.colorWriteMask = (c == int(Clip::WriteReplace) || c == int(Clip::WriteIncrement))
                                     ? 0
                                     : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &att;
            VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            dss.stencilTestEnable = VK_TRUE;
            dss.depthTestEnable = dm != 0;
            dss.depthWriteEnable = dm == 1;
            dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkStencilOpState st{};
            st.failOp = VK_STENCIL_OP_KEEP;
            st.depthFailOp = VK_STENCIL_OP_KEEP;
            st.passOp = VK_STENCIL_OP_KEEP;
            st.compareOp = VK_COMPARE_OP_ALWAYS;
            st.compareMask = st.writeMask = 0xFF;
            if (c == int(Clip::Test)) st.compareOp = VK_COMPARE_OP_EQUAL, st.writeMask = 0;
            if (c == int(Clip::None)) st.writeMask = 0;
            if (c == int(Clip::WriteReplace)) st.passOp = VK_STENCIL_OP_REPLACE;
            if (c == int(Clip::WriteIncrement)) st.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
            dss.front = dss.back = st;
            VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pi.pNext = &rendering;
            pi.stageCount = 2;
            pi.pStages = stages;
            pi.pVertexInputState = &vi;
            pi.pInputAssemblyState = &ia;
            pi.pViewportState = &vp;
            pi.pRasterizationState = &rs;
            pi.pMultisampleState = &ms;
            pi.pDepthStencilState = &dss;
            pi.pColorBlendState = &cb;
            pi.pDynamicState = &ds;
            pi.layout = pipeline_layout;
            if (vkCreateGraphicsPipelines(d, pipeline_cache, 1, &pi, nullptr, &pipelines[dm][b][c]) != VK_SUCCESS)
                return error = "no pipeline", false;
        }
    }

    // with multisampling, the passes over the filters' images (one sample, no stencil): whole-window quads,
    // never clipped
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineRenderingCreateInfo plain{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        plain.colorAttachmentCount = 1;
        plain.pColorAttachmentFormats = &COLOR_FORMAT;
        for (int b = 0; b < 2; b++) {
            VkPipelineColorBlendAttachmentState att{};
            att.blendEnable = b == int(Blend::Over);
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
            att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                 VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &att;
            VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pi.pNext = &plain;
            pi.stageCount = 2;
            pi.pStages = stages;
            pi.pVertexInputState = &vi;
            pi.pInputAssemblyState = &ia;
            pi.pViewportState = &vp;
            pi.pRasterizationState = &rs;
            pi.pMultisampleState = &ms1;
            pi.pDepthStencilState = &dss;
            pi.pColorBlendState = &cb;
            pi.pDynamicState = &ds;
            pi.layout = pipeline_layout;
            if (vkCreateGraphicsPipelines(d, pipeline_cache, 1, &pi, nullptr, &flat[b]) != VK_SUCCESS)
                return error = "no pipeline", false;
        }
    }
    return true;
}

void Renderer::Impl::close() {
    if (!d) return;
    vkDeviceWaitIdle(d);
    run_deferred(UINT64_MAX);
    drop_targets();
    if (white) destroy_texture_now(white), white = nullptr;
    if (quad.count) free_geometry_now(quad);
    for (Chunk &c : chunks) dev->destroy(c.buffer);
    for (vk::Buffer &b : ubo) dev->destroy(b);
    for (VkDescriptorPool p : tex_pools) vkDestroyDescriptorPool(d, p, nullptr);
    vkDestroyDescriptorPool(d, ubo_pool, nullptr);
    destroy_pipelines();
    if (pipeline_cache) vkDestroyPipelineCache(d, pipeline_cache, nullptr), pipeline_cache = VK_NULL_HANDLE;
    vkDestroyShaderModule(d, vert, nullptr);
    vkDestroyShaderModule(d, frag, nullptr);
    vkDestroyPipelineLayout(d, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(d, ubo_layout, nullptr);
    vkDestroyDescriptorSetLayout(d, tex_layout, nullptr);
    vkDestroySampler(d, sampler, nullptr);
    d = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// resources

VkDescriptorSet Renderer::Impl::alloc_set(VkDescriptorPool &pool_out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!tex_pools.empty()) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = tex_pools.back();
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &tex_layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(d, &ai, &set) == VK_SUCCESS) return pool_out = tex_pools.back(), set;
        }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets = 512;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ps;
        VkDescriptorPool pool;
        if (vkCreateDescriptorPool(d, &dp, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        tex_pools.push_back(pool);
    }
    return VK_NULL_HANDLE;
}

Texture *Renderer::Impl::make_texture(int w, int h, VkImageUsageFlags usage, const uint8_t *premultiplied) {
    auto t = std::make_unique<Texture>();
    t->width = w, t->height = h;
    t->image = dev->create_image(VK_IMAGE_TYPE_2D, COLOR_FORMAT, {uint32_t(w), uint32_t(h), 1}, 1, 1,
                                 VK_SAMPLE_COUNT_1_BIT, usage | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!t->image.image) return nullptr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = COLOR_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(d, &vi, nullptr, &t->view);
    t->set = alloc_set(t->pool);
    if (!t->set) return dev->destroy(t->image), vkDestroyImageView(d, t->view, nullptr), nullptr;
    VkDescriptorImageInfo ii{sampler, t->view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = t->set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d, 1, &wr, 0, nullptr);

    // every image lives in GENERAL: moved there once, the pixels uploaded
    vk::Buffer staging;
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    if (premultiplied) {
        staging = dev->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        if (!staging.mapped) return nullptr;
        std::memcpy(staging.mapped, premultiplied, bytes);
    }
    std::string error;
    VkImage image = t->image.image;
    dev->run(
        [&](VkCommandBuffer c) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(c, &dep);
            if (premultiplied) {
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {uint32_t(w), uint32_t(h), 1};
                vkCmdCopyBufferToImage(c, staging.buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                dep.imageMemoryBarrierCount = 0;
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &m;
                vkCmdPipelineBarrier2(c, &dep);
            }
        },
        error);
    if (premultiplied) dev->destroy(staging);
    return t.release();
}

void Renderer::Impl::destroy_texture_now(Texture *t) {
    vkFreeDescriptorSets(d, t->pool, 1, &t->set);
    vkDestroyImageView(d, t->view, nullptr);
    dev->destroy(t->image);
    if (t->ms_view) vkDestroyImageView(d, t->ms_view, nullptr);
    if (t->ms.image) dev->destroy(t->ms);
    delete t;
}

void Renderer::Impl::free_texture(Texture *t) {
    defer([this, t] { destroy_texture_now(t); });
}

bool Renderer::Impl::alloc_geometry(Geometry &g, const void *vertices, size_t vertex_bytes, const void *indices,
                                    size_t index_bytes) {
    const VkDeviceSize index_at = (vertex_bytes + 3) & ~VkDeviceSize(3);
    const VkDeviceSize need = (index_at + index_bytes + 15) & ~VkDeviceSize(15);
    size_t chunk = chunks.size();
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < chunks.size() && chunk == chunks.size(); i++) {
        for (auto &[start, size] : chunks[i].free) {
            if (size < need) continue;
            chunk = i, offset = start;
            start += need, size -= need;
            break;
        }
    }
    if (chunk == chunks.size()) {
        Chunk c;
        const VkDeviceSize size = std::max(ARENA_CHUNK, need);
        c.buffer = dev->create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        if (!c.buffer.mapped) return false;
        c.free.push_back({need, size - need});
        chunks.push_back(std::move(c));
    }
    uint8_t *base = static_cast<uint8_t *>(chunks[chunk].buffer.mapped) + offset;
    std::memcpy(base, vertices, vertex_bytes);
    std::memcpy(base + index_at, indices, index_bytes);
    g.chunk = chunk;
    g.offset = offset, g.size = need;
    g.index_offset = offset + index_at;
    g.count = uint32_t(index_bytes / 4);
    return true;
}

void Renderer::Impl::free_geometry_now(const Geometry &g) {
    auto &free = chunks[g.chunk].free;
    auto at = std::lower_bound(free.begin(), free.end(), std::make_pair(g.offset, VkDeviceSize(0)));
    at = free.insert(at, {g.offset, g.size});
    if (at + 1 != free.end() && at->first + at->second == (at + 1)->first) {
        at->second += (at + 1)->second;
        free.erase(at + 1);
    }
    if (at != free.begin() && (at - 1)->first + (at - 1)->second == at->first) {
        (at - 1)->second += at->second;
        free.erase(at);
    }
}

void Renderer::Impl::make_stencil() {
    stencil = dev->create_image(VK_IMAGE_TYPE_2D, stencil_format, {width, height, 1}, 1, 1, samples,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = stencil.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = stencil_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCreateImageView(d, &vi, nullptr, &stencil_view);
    const VkImage image = stencil.image;
    std::string error;
    dev->run(
        [&](VkCommandBuffer c) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(c, &dep);
        },
        error);
}

// the multisampled image a layer is drawn into (the layer's own image is what it resolves to)
void Renderer::Impl::make_ms(Texture &t) {
    t.ms = dev->create_image(VK_IMAGE_TYPE_2D, COLOR_FORMAT, {uint32_t(t.width), uint32_t(t.height), 1}, 1, 1, samples,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!t.ms.image) return;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.ms.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = COLOR_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(d, &vi, nullptr, &t.ms_view);
    const VkImage image = t.ms.image;
    std::string error;
    dev->run(
        [&](VkCommandBuffer c) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(c, &dep);
        },
        error);
}

void Renderer::Impl::drop_targets() {
    for (auto &l : layers) destroy_texture_now(l.release());
    layers.clear();
    for (auto &p : pp)
        if (p) destroy_texture_now(p.release());
    if (mask) destroy_texture_now(mask.release());
    if (stencil_view) vkDestroyImageView(d, stencil_view, nullptr), stencil_view = VK_NULL_HANDLE;
    if (stencil.image) dev->destroy(stencil);
}

Texture &Renderer::Impl::layer(size_t i) {
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    while (layers.size() <= i) {
        Texture *t = make_texture(int(width), int(height), usage, nullptr);
        if (t && samples != VK_SAMPLE_COUNT_1_BIT) make_ms(*t);
        layers.emplace_back(t);
    }
    return *layers[i];
}

Texture &Renderer::Impl::filter_image(int i) {
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    std::unique_ptr<Texture> &slot = i < 3 ? pp[i] : mask;
    if (!slot) slot.reset(make_texture(int(width), int(height), usage, nullptr));
    return *slot;
}

// ---------------------------------------------------------------------------
// recording

void Renderer::Impl::memory_barrier() {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Renderer::Impl::end_pass() {
    if (!pass_target) return;
    vkCmdEndRendering(cmd);
    pass_target = nullptr;
    bound = VK_NULL_HANDLE;
    memory_barrier();
}

void Renderer::Impl::begin_pass(Texture &target, bool clear) {
    if (pass_target == &target) return;
    end_pass();
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (target.ms_view) {
        // drawn multisampled, resolved into the layer's image as the pass ends; the samples are kept for the
        // next pass over this layer, which loads them
        color.imageView = target.ms_view;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = target.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = stencil_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingAttachmentInfo st = depth;
    st.loadOp = first_pass ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    st.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    st.clearValue.depthStencil = {0, 0};
    first_pass = false;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {uint32_t(target.width), uint32_t(target.height)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    // the stencil has the layers' sample count: the single-sample images of the filters go without
    if (samples == VK_SAMPLE_COUNT_1_BIT || target.ms_view) {
        ri.pDepthAttachment = &depth;
        ri.pStencilAttachment = &st;
    }
    vkCmdBeginRendering(cmd, &ri);
    pass_target = &target;
    passes++;
}

void Renderer::Impl::clear_color(const Rml::Rectanglei &rect) {
    VkClearAttachment ca{};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkClearRect cr{};
    cr.rect.offset = {rect.Left(), rect.Top()};
    cr.rect.extent = {uint32_t(rect.Width()), uint32_t(rect.Height())};
    cr.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
}

Renderer::Impl::Draw Renderer::Impl::make_draw(Texture &target) const {
    Draw s;
    s.target = &target;
    s.tex = white;
    s.mask = white;
    for (float &c : s.u.color) c = 1;
    s.u.uv_transform[0] = s.u.uv_transform[1] = 1;
    return s;
}

void Renderer::Impl::draw(Draw &s) {
    if (!s.geometry || !s.geometry->count) return;
    if (ubo_offset + sizeof(DrawUniforms) > UNIFORM_RING) {
        if (!overflow_reported) Rml::Log::Message(Rml::Log::LT_WARNING, "the launcher's frame has too many draws");
        overflow_reported = true;
        return;
    }
    begin_pass(*s.target);
    const uint32_t offset = uint32_t(ubo_offset);
    std::memcpy(static_cast<uint8_t *>(ubo[slot].mapped) + offset, &s.u, sizeof(DrawUniforms));
    ubo_offset += ubo_stride;

    const bool plain = samples != VK_SAMPLE_COUNT_1_BIT && !s.target->ms_view;
    VkPipeline pipeline = plain ? flat[int(s.blend)] : pipelines[s.depth][int(s.blend)][int(s.clip)];
    if (pipeline != bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline), bound = pipeline;
    const uint32_t vw = s.viewport_w ? s.viewport_w : uint32_t(s.target->width);
    const uint32_t vh = s.viewport_h ? s.viewport_h : uint32_t(s.target->height);
    VkViewport viewport{0, 0, float(vw), float(vh), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D rect{{0, 0}, {uint32_t(s.target->width), uint32_t(s.target->height)}};
    if (s.scissor && s.rect.Valid()) {
        const int x0 = std::clamp(s.rect.Left(), 0, s.target->width), y0 = std::clamp(s.rect.Top(), 0, s.target->height);
        const int x1 = std::clamp(s.rect.Right(), x0, s.target->width), y1 = std::clamp(s.rect.Bottom(), y0, s.target->height);
        rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil_ref);
    const VkDescriptorSet sets[3] = {ubo_sets[slot], s.tex->set, s.mask->set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 3, sets, 1, &offset);
    const Chunk &chunk = chunks[s.geometry->chunk];
    const VkDeviceSize vb = s.geometry->offset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &chunk.buffer.buffer, &vb);
    vkCmdBindIndexBuffer(cmd, chunk.buffer.buffer, s.geometry->index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, s.geometry->count, 1, 0, 0, 0);
    draws++;
}

// a pass over a whole image: src drawn into dst, in the window's rectangle
void Renderer::Impl::pass(Texture &dst, Texture &src, Mode mode, Blend blend, const Rml::Rectanglei &window,
                          bool windowed, const std::function<void(Draw &)> &tweak) {
    Draw s = make_draw(dst);
    s.blend = blend;
    s.tex = &src;
    s.geometry = &quad;
    s.u.imode[0] = mode;
    s.u.imode[3] = 1;
    s.scissor = windowed;
    s.rect = window;
    if (tweak) tweak(s);
    draw(s);
}

void Renderer::Impl::blur(float sigma, Texture &a, Texture &b, Rml::Rectanglei window) {
    const float W = float(width), H = float(height);
    // the large sigmas by halving first, so that the Gaussian stays narrow
    const int v = int(sigma * (2.f / 3.f));
    const int level = std::clamp(v <= 0 ? 0 : int(std::floor(std::log2(float(v)))), 0, 10);
    sigma = std::clamp(sigma / float(1 << level), 0.f, 3.f);

    Texture *from = &a, *to = &b;
    int fw = int(width), fh = int(height);
    Rml::Rectanglei rect = window;
    for (int i = 0; i < level; i++) {
        const int dw = std::max(fw / 2, 1), dh = std::max(fh / 2, 1);
        rect.p0 = (rect.p0 + Rml::Vector2i(1)) / 2;
        rect.p1 = Rml::Math::Max(rect.p1 / 2, rect.p0);
        const float su = float(fw) / W, sv = float(fh) / H;
        pass(*to, *from, M_TEXTURE, Blend::Replace, rect, true, [&](Draw &s) {
            s.viewport_w = uint32_t(dw), s.viewport_h = uint32_t(dh);
            s.u.uv_transform[0] = su, s.u.uv_transform[1] = sv;
        });
        std::swap(from, to);
        fw = dw, fh = dh;
    }

    // the weights of the seven taps
    float weights[4], sum = 0;
    for (int i = 0; i < 4; i++) {
        weights[i] = sigma < 0.1f ? float(i == 0)
                                  : std::exp(-float(i * i) / (2.f * sigma * sigma)) /
                                        (std::sqrt(2.f * 3.14159265f) * sigma);
        sum += (i == 0 ? 1.f : 2.f) * weights[i];
    }
    for (float &w : weights) w /= sum;
    // taps stay inside the region: a half texel in, to not blend in what is outside
    const float limits[4] = {(float(rect.Left()) + 0.5f) / W, (float(rect.Top()) + 0.5f) / H,
                             (float(rect.Right()) - 0.5f) / W, (float(rect.Bottom()) - 0.5f) / H};
    auto blur_pass = [&](Texture &dst, Texture &src, float tx, float ty) {
        pass(dst, src, M_BLUR, Blend::Replace, rect, true, [&](Draw &s) {
            std::memcpy(s.u.blur_weights, weights, sizeof(weights));
            std::memcpy(s.u.limits, limits, sizeof(limits));
            s.u.texel[0] = tx, s.u.texel[1] = ty;
        });
    };
    blur_pass(*to, *from, 0, 1.f / H); // vertical
    // a transparent border around the region, so that what the upscale
    // blends in at its edge is clear
    begin_pass(*from);
    Rml::Rectanglei border = rect.Extend(1);
    border = border.Intersect(Rml::Rectanglei::FromSize({int(width), int(height)}));
    if (border.Valid()) clear_color(border);
    blur_pass(*from, *to, 1.f / W, 0); // horizontal

    if (level == 0) return; // in a, where it was
    if (from == &a) { // the upscale cannot read what it writes
        pass(*to, *from, M_TEXTURE, Blend::Replace, rect, true);
        std::swap(from, to);
    }
    pass(a, *from, M_CLAMPED, Blend::Replace, window, true, [&](Draw &s) {
        s.u.uv_transform[0] = s.u.uv_transform[1] = 1.f / float(1 << level);
        std::memcpy(s.u.limits, limits, sizeof(limits));
    });
}

void Renderer::Impl::filters(Rml::Span<const Rml::CompiledFilterHandle> list, Texture *&primary, Texture *&secondary,
                             Texture *&tertiary, const Rml::Rectanglei &window) {
    const float W = float(width), H = float(height);
    for (const Rml::CompiledFilterHandle handle : list) {
        const Filter &f = *reinterpret_cast<const Filter *>(handle);
        switch (f.type) {
        case FilterType::Opacity:
            pass(*secondary, *primary, M_TEXTURE, Blend::Replace, window, true, [&](Draw &s) {
                for (float &c : s.u.color) c = f.value;
            });
            std::swap(primary, secondary);
            break;
        case FilterType::Blur:
            blur(f.sigma, *primary, *secondary, window);
            break;
        case FilterType::DropShadow: {
            const Rml::Colourf color = to_colorf(f.color);
            pass(*secondary, *primary, M_SHADOW, Blend::Replace, window, true, [&](Draw &s) {
                std::memcpy(s.u.color, &color[0], sizeof(float) * 4);
                s.u.limits[0] = (float(window.Left()) + 0.5f) / W;
                s.u.limits[1] = (float(window.Top()) + 0.5f) / H;
                s.u.limits[2] = (float(window.Right()) - 0.5f) / W;
                s.u.limits[3] = (float(window.Bottom()) - 0.5f) / H;
                s.u.uv_transform[2] = -f.offset.x / W;
                s.u.uv_transform[3] = -f.offset.y / H;
            });
            if (f.sigma >= 0.5f) blur(f.sigma, *secondary, *tertiary, window);
            pass(*secondary, *primary, M_TEXTURE, Blend::Over, window, true);
            std::swap(primary, secondary);
            break;
        }
        case FilterType::ColorMatrix:
            pass(*secondary, *primary, M_COLOR_MATRIX, Blend::Replace, window, true, [&](Draw &s) {
                std::memcpy(s.u.color_matrix, f.color_matrix.data(), sizeof(float) * 16);
            });
            std::swap(primary, secondary);
            break;
        case FilterType::MaskImage:
            pass(*secondary, *primary, M_MASK, Blend::Replace, window, true, [&](Draw &s) { s.mask = &filter_image(3); });
            std::swap(primary, secondary);
            break;
        case FilterType::Invalid: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Renderer

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (impl_) impl_->close();
}

bool Renderer::open(vk::Device &device, std::string &error) {
    device_ = &device;
    impl_ = std::make_unique<Impl>();
    impl_->pipeline_cache_file = pipeline_cache_file_;
    return impl_->open(device, error);
}

bool Renderer::add_image(const std::string &name, const std::vector<uint8_t> &png) {
    int w = 0, h = 0, n = 0;
    uint8_t *pixels = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
    if (!pixels) return false;
    MemoryImage image;
    image.width = w, image.height = h;
    image.rgba.assign(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    impl_->images[name] = std::move(image);
    return true;
}

bool Renderer::add_soft_image(const std::string &name, const std::vector<uint8_t> &png) {
    int w = 0, h = 0, n = 0;
    uint8_t *pixels = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
    if (!pixels) return false;
    // the average of each cell of a 40 x 40 grid, over the picture's alpha
    constexpr int N = 40;
    std::vector<float> grid(size_t(N) * N * 3, 0.f), weight(size_t(N) * N, 0.f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = pixels + (size_t(y) * w + x) * 4;
            const size_t cell = size_t(y * N / h) * N + size_t(x * N / w);
            const float a = float(p[3]) / 255.f;
            for (int c = 0; c < 3; c++) grid[cell * 3 + size_t(c)] += float(p[c]) * a;
            weight[cell] += a;
        }
    stbi_image_free(pixels);
    for (size_t i = 0; i < weight.size(); i++)
        for (int c = 0; c < 3; c++) grid[i * 3 + size_t(c)] = weight[i] > 0 ? grid[i * 3 + size_t(c)] / weight[i] : 20.f;
    // two box blurs of 3 x 3 with the edges held
    for (int pass = 0; pass < 3; pass++) {
        std::vector<float> next(grid.size());
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            sum += grid[(size_t(std::clamp(y + dy, 0, N - 1)) * N + size_t(std::clamp(x + dx, 0, N - 1))) * 3 + size_t(c)];
                    next[(size_t(y) * N + size_t(x)) * 3 + size_t(c)] = sum / 9.f;
                }
        grid.swap(next);
    }
    // more colour, a little darker: it sits behind the interface
    MemoryImage image;
    image.width = image.height = N;
    image.rgba.resize(size_t(N) * N * 4);
    for (size_t i = 0; i < size_t(N) * N; i++) {
        const float r = grid[i * 3], g = grid[i * 3 + 1], b = grid[i * 3 + 2];
        const float gray = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        const float k = 1.45f; // saturation
        const float rgb[3] = {gray + (r - gray) * k, gray + (g - gray) * k, gray + (b - gray) * k};
        for (int c = 0; c < 3; c++) image.rgba[i * 4 + size_t(c)] = uint8_t(std::clamp(rgb[c] * 0.82f, 0.f, 255.f));
        image.rgba[i * 4 + 3] = 255;
    }
    impl_->images["soft_" + name] = std::move(image);
    return true;
}

void Renderer::remove_image(const std::string &name) { impl_->images.erase(name); }

void Renderer::begin_frame(VkCommandBuffer cmd, uint32_t slot, uint32_t width, uint32_t height, uint64_t serial,
                           uint64_t completed) {
    Impl &m = *impl_;
    width = std::max(width, 1u), height = std::max(height, 1u);
    m.run_deferred(completed);
    const VkSampleCountFlagBits want = m.pick_samples();
    if (width != m.width || height != m.height || want != m.samples) {
        vkDeviceWaitIdle(m.d);
        m.run_deferred(UINT64_MAX);
        m.drop_targets();
        if (want != m.samples) { // the pipelines are made for a sample count
            m.destroy_pipelines();
            m.samples = want;
            std::string error;
            if (!m.make_pipelines(error)) Rml::Log::Message(Rml::Log::LT_ERROR, "the launcher's pipelines: %s", error.c_str());
            m.save_pipeline_cache();
        }
        m.width = width_ = width, m.height = height_ = height;
        m.make_stencil();
    }
    m.serial = serial;
    m.cmd = cmd;
    m.slot = slot;
    m.ubo_offset = 0;
    m.draws = m.passes = 0;
    m.layer_count = 1;
    m.first_pass = true;
    m.pass_target = nullptr;
    m.bound = VK_NULL_HANDLE;
    m.scissor_on = m.clip_on = false;
    m.scissor = Rml::Rectanglei::MakeInvalid();
    m.clip_ref = 1;
    m.projection = Rml::Matrix4f::ProjectOrtho(0, float(width), 0, float(height), -10000, 10000);
    m.set_transform(nullptr);
    m.begin_pass(m.layer(0), true);
    if (m.backdrop.on) {
        const Backdrop &b = m.backdrop;
        // the grey gradient: ui.frag's MODE_BACKDROP (the clock-free part)
        m.pass(m.layer(0), *m.white, M_BACKDROP, Blend::Replace, {}, false,
               [&b](Impl::Draw &d) { d.u.imode[1] = b.light ? 1 : 0; });
        // the ribbons over it: triangles built on the CPU (ribbons.cpp), opaque, with a depth test (no
        // sorting: where they cross or fold the edge is exact), then their one-pixel fringes, tested
        // against the depth but not written, blended, standing in for multisampling
        if (b.strength > 0) {
            std::vector<RibbonVertex> rv;
            std::vector<uint32_t> body, fringe;
            build_ribbons(b.time, m.width, m.height, b.tint, b.strength, rv, body, fringe);
            if (!body.empty()) {
                std::vector<Rml::Vertex> verts(rv.size());
                for (size_t i = 0; i < rv.size(); i++) {
                    verts[i].position = {rv[i].x, rv[i].y};
                    verts[i].colour = Rml::ColourbPremultiplied(rv[i].r, rv[i].g, rv[i].b, rv[i].a);
                    verts[i].tex_coord = {rv[i].depth, 0.f};
                }
                VkClearAttachment ca{};
                ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                ca.clearValue.depthStencil = {1.f, 0};
                VkClearRect cr{{{0, 0}, {m.width, m.height}}, 0, 1};
                vkCmdClearAttachments(m.cmd, 1, &ca, 1, &cr);
                const std::vector<uint32_t> *lists[2] = {&body, &fringe};
                for (int part = 0; part < 2; part++) {
                    if (lists[part]->empty()) continue;
                    if (part == 1 && m.samples != VK_SAMPLE_COUNT_1_BIT) continue; // multisampling softens the edges
                    std::vector<int> idx(lists[part]->begin(), lists[part]->end());
                    auto *g = new Geometry;
                    if (m.alloc_geometry(*g, verts.data(), verts.size() * sizeof(Rml::Vertex), idx.data(),
                                         idx.size() * sizeof(int))) {
                        Impl::Draw s = m.make_draw(m.layer(0));
                        s.geometry = g;
                        s.depth = part == 0 ? 1 : 2;
                        s.u.imode[3] = 2;
                        std::memcpy(s.u.transform, m.projection.data(), sizeof(float) * 16);
                        m.draw(s);
                        m.defer([&m, g] {
                            m.free_geometry_now(*g);
                            delete g;
                        });
                    } else {
                        delete g;
                    }
                }
            }
        }
    }
}

void Renderer::set_backdrop(const Backdrop &backdrop) {
    if (impl_) impl_->backdrop = backdrop;
}

void Renderer::set_msaa(uint32_t samples) {
    if (impl_) impl_->requested = std::max(samples, 1u);
}

uint32_t Renderer::msaa() const { return impl_ ? uint32_t(impl_->samples) : 1; }

void Renderer::end_frame() {
    impl_->end_pass();
    draws_ = impl_->draws;
    passes_ = impl_->passes;
}

VkImage Renderer::output() const { return impl_->layers.empty() ? VK_NULL_HANDLE : impl_->layers[0]->image.image; }

Rml::CompiledGeometryHandle Renderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    auto *g = new Geometry;
    if (!impl_->alloc_geometry(*g, vertices.data(), vertices.size() * sizeof(Rml::Vertex), indices.data(),
                               indices.size() * sizeof(int))) {
        delete g;
        return {};
    }
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void Renderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) {
    Impl &m = *impl_;
    Impl::Draw s = m.make_draw(m.layer(m.layer_count - 1));
    s.geometry = reinterpret_cast<Geometry *>(handle);
    if (texture) s.tex = reinterpret_cast<Texture *>(texture);
    std::memcpy(s.u.transform, m.transform.data(), sizeof(float) * 16);
    s.u.translate[0] = translation.x, s.u.translate[1] = translation.y;
    s.scissor = m.scissor_on;
    s.rect = m.scissor;
    s.clip = m.clip_on ? Clip::Test : Clip::None;
    s.stencil_ref = m.clip_ref;
    m.draw(s);
}

void Renderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    Geometry *g = reinterpret_cast<Geometry *>(handle);
    impl_->defer([this, g] {
        impl_->free_geometry_now(*g);
        delete g;
    });
}

Rml::TextureHandle Renderer::LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) {
    Impl &m = *impl_;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (source.starts_with("mem:")) {
        auto it = m.images.find(source.substr(4));
        if (it == m.images.end()) return {};
        pixels = it->second.rgba, w = it->second.width, h = it->second.height;
    } else {
        std::ifstream f(std::filesystem::path(std::u8string(source.begin(), source.end())), std::ios::binary);
        if (!f) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "no image %s", source.c_str());
            return {};
        }
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int n = 0;
        uint8_t *p = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
        if (!p) return {};
        pixels.assign(p, p + size_t(w) * h * 4);
        stbi_image_free(p);
    }
    premultiply(pixels.data(), size_t(w) * h);
    Texture *t = m.make_texture(w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT, pixels.data());
    if (!t) return {};
    dimensions = {w, h};
    return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::TextureHandle Renderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    Texture *t = impl_->make_texture(dimensions.x, dimensions.y, VK_IMAGE_USAGE_TRANSFER_DST_BIT, source.data());
    return reinterpret_cast<Rml::TextureHandle>(t);
}

void Renderer::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture) impl_->free_texture(reinterpret_cast<Texture *>(texture));
}

void Renderer::EnableScissorRegion(bool enable) {
    // enable is followed by SetScissorRegion; disabling is here
    if (!enable) impl_->scissor_on = false, impl_->scissor = Rml::Rectanglei::MakeInvalid();
}

void Renderer::SetScissorRegion(Rml::Rectanglei region) {
    impl_->scissor_on = region.Valid();
    impl_->scissor = region;
}

void Renderer::EnableClipMask(bool enable) { impl_->clip_on = enable; }

void Renderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                                Rml::Vector2f translation) {
    Impl &m = *impl_;
    Texture &top = m.layer(m.layer_count - 1);
    m.begin_pass(top);
    uint32_t write = 1, test = 1;
    Clip clip = Clip::WriteReplace;
    auto clear_stencil = [&](uint32_t value) {
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        ca.clearValue.depthStencil = {0, value};
        VkClearRect cr{{{0, 0}, {m.width, m.height}}, 0, 1};
        vkCmdClearAttachments(m.cmd, 1, &ca, 1, &cr);
    };
    switch (operation) {
    case Rml::ClipMaskOperation::Set: clear_stencil(0); break;
    case Rml::ClipMaskOperation::SetInverse:
        clear_stencil(1);
        write = 0;
        break;
    case Rml::ClipMaskOperation::Intersect:
        clip = Clip::WriteIncrement;
        test = m.clip_ref + 1;
        break;
    }
    Impl::Draw s = m.make_draw(top);
    s.geometry = reinterpret_cast<Geometry *>(geometry);
    std::memcpy(s.u.transform, m.transform.data(), sizeof(float) * 16);
    s.u.translate[0] = translation.x, s.u.translate[1] = translation.y;
    s.scissor = m.scissor_on;
    s.rect = m.scissor;
    s.clip = clip;
    s.stencil_ref = write;
    m.draw(s);
    m.clip_ref = test;
}

void Renderer::SetTransform(const Rml::Matrix4f *transform) { impl_->set_transform(transform); }

Rml::LayerHandle Renderer::PushLayer() {
    Impl &m = *impl_;
    m.layer_count++;
    Texture &t = m.layer(m.layer_count - 1);
    m.begin_pass(t);
    m.clear_color(m.window());
    return Rml::LayerHandle(m.layer_count - 1);
}

void Renderer::PopLayer() {
    if (impl_->layer_count > 1) impl_->layer_count--;
}

void Renderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                               Rml::Span<const Rml::CompiledFilterHandle> filters) {
    Impl &m = *impl_;
    Texture &src = m.layer(size_t(source)), &dst = m.layer(size_t(destination));
    const Rml::Rectanglei window = m.window();
    Texture *primary = &m.filter_image(0), *secondary = &m.filter_image(1), *tertiary = nullptr;
    // the source into the first filter image (the drop shadow's blur needs a third)
    m.pass(*primary, src, M_TEXTURE, Blend::Replace, window, true);
    if (!filters.empty()) {
        tertiary = &m.filter_image(2);
        m.filters(filters, primary, secondary, tertiary, window);
    }
    // into the destination, through the clip mask when it is on
    m.pass(dst, *primary, M_TEXTURE, blend_mode == Rml::BlendMode::Replace ? Blend::Replace : Blend::Over, window,
           m.scissor_on, [&](Impl::Draw &s) {
               s.clip = m.clip_on ? Clip::Test : Clip::None;
               s.stencil_ref = m.clip_ref;
           });
}

Rml::TextureHandle Renderer::SaveLayerAsTexture() {
    Impl &m = *impl_;
    const Rml::Rectanglei bounds = m.window();
    Texture *t = m.make_texture(bounds.Width(), bounds.Height(),
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, nullptr);
    if (!t) return {};
    m.end_pass();
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffset = {bounds.Left(), bounds.Top(), 0};
    copy.extent = {uint32_t(bounds.Width()), uint32_t(bounds.Height()), 1};
    vkCmdCopyImage(m.cmd, m.layer(m.layer_count - 1).image.image, VK_IMAGE_LAYOUT_GENERAL, t->image.image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    m.memory_barrier();
    return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::CompiledFilterHandle Renderer::SaveLayerAsMaskImage() {
    Impl &m = *impl_;
    m.pass(m.filter_image(3), m.layer(m.layer_count - 1), M_TEXTURE, Blend::Replace, m.window(), true);
    auto *f = new Filter;
    f->type = FilterType::MaskImage;
    return reinterpret_cast<Rml::CompiledFilterHandle>(f);
}

Rml::CompiledFilterHandle Renderer::CompileFilter(const Rml::String &name, const Rml::Dictionary &parameters) {
    auto *f = new Filter;
    const float value = Rml::Get(parameters, "value", 1.0f);
    using Rml::Matrix4f;
    using Rml::Vector3f;
    using Rml::Vector4f;
    if (name == "opacity") {
        f->type = FilterType::Opacity, f->value = value;
    } else if (name == "blur") {
        f->type = FilterType::Blur, f->sigma = Rml::Get(parameters, "sigma", 1.0f);
    } else if (name == "drop-shadow") {
        f->type = FilterType::DropShadow;
        f->sigma = Rml::Get(parameters, "sigma", 0.f);
        f->color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        f->offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
    } else if (name == "brightness") {
        f->type = FilterType::ColorMatrix;
        f->color_matrix = Matrix4f::Diag(value, value, value, 1.f);
    } else if (name == "contrast") {
        f->type = FilterType::ColorMatrix;
        const float gray = 0.5f - 0.5f * value;
        f->color_matrix = Matrix4f::Diag(value, value, value, 1.f);
        f->color_matrix.SetColumn(3, Vector4f(gray, gray, gray, 1.f));
    } else if (name == "invert") {
        f->type = FilterType::ColorMatrix;
        const float v = std::clamp(value, 0.f, 1.f), inverted = 1.f - 2.f * v;
        f->color_matrix = Matrix4f::Diag(inverted, inverted, inverted, 1.f);
        f->color_matrix.SetColumn(3, Vector4f(v, v, v, 1.f));
    } else if (name == "grayscale") {
        f->type = FilterType::ColorMatrix;
        const float rev = 1.f - value;
        const Vector3f g = value * Vector3f(0.2126f, 0.7152f, 0.0722f);
        f->color_matrix = Matrix4f::FromRows({g.x + rev, g.y, g.z, 0.f}, {g.x, g.y + rev, g.z, 0.f},
                                             {g.x, g.y, g.z + rev, 0.f}, {0.f, 0.f, 0.f, 1.f});
    } else if (name == "sepia") {
        f->type = FilterType::ColorMatrix;
        const float rev = 1.f - value;
        const Vector3f r = value * Vector3f(0.393f, 0.769f, 0.189f), g = value * Vector3f(0.349f, 0.686f, 0.168f),
                       b = value * Vector3f(0.272f, 0.534f, 0.131f);
        f->color_matrix = Matrix4f::FromRows({r.x + rev, r.y, r.z, 0.f}, {g.x, g.y + rev, g.z, 0.f},
                                             {b.x, b.y, b.z + rev, 0.f}, {0.f, 0.f, 0.f, 1.f});
    } else if (name == "hue-rotate") {
        f->type = FilterType::ColorMatrix;
        const float s = std::sin(value), c = std::cos(value);
        f->color_matrix = Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.f},
            {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.f},
            {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.f},
            {0.f, 0.f, 0.f, 1.f});
    } else if (name == "saturate") {
        f->type = FilterType::ColorMatrix;
        f->color_matrix = Matrix4f::FromRows(
            {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.f},
            {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.f},
            {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.f}, {0.f, 0.f, 0.f, 1.f});
    }
    if (f->type == FilterType::Invalid) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter type '%s'.", name.c_str());
        delete f;
        return {};
    }
    return reinterpret_cast<Rml::CompiledFilterHandle>(f);
}

void Renderer::ReleaseFilter(Rml::CompiledFilterHandle filter) { delete reinterpret_cast<Filter *>(filter); }

Rml::CompiledShaderHandle Renderer::CompileShader(const Rml::String &name, const Rml::Dictionary &parameters) {
    auto *s = new Shader;
    const bool repeating = Rml::Get(parameters, "repeating", false);
    auto stops = [&] {
        auto it = parameters.find("color_stop_list");
        if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) return false;
        const Rml::ColorStopList &list = it->second.GetReference<Rml::ColorStopList>();
        const int n = std::min(int(list.size()), MAX_STOPS);
        for (int i = 0; i < n; i++) {
            s->positions.push_back(list[size_t(i)].position.number);
            s->colors.push_back(to_colorf(list[size_t(i)].color));
        }
        return n > 0;
    };
    if (name == "linear-gradient") {
        s->type = ShaderType::Gradient;
        s->function = repeating ? 3 : 0;
        s->p = Rml::Get(parameters, "p0", Rml::Vector2f(0.f));
        s->v = Rml::Get(parameters, "p1", Rml::Vector2f(0.f)) - s->p;
    } else if (name == "radial-gradient") {
        s->type = ShaderType::Gradient;
        s->function = repeating ? 4 : 1;
        s->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        s->v = Rml::Vector2f(1.f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.f));
    } else if (name == "conic-gradient") {
        s->type = ShaderType::Gradient;
        s->function = repeating ? 5 : 2;
        s->p = Rml::Get(parameters, "center", Rml::Vector2f(0.f));
        const float angle = Rml::Get(parameters, "angle", 0.f);
        s->v = {std::cos(angle), std::sin(angle)};
    }
    if (s->type == ShaderType::Invalid || !stops()) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader type '%s'.", name.c_str());
        delete s;
        return {};
    }
    return reinterpret_cast<Rml::CompiledShaderHandle>(s);
}

void Renderer::RenderShader(Rml::CompiledShaderHandle handle, Rml::CompiledGeometryHandle geometry,
                            Rml::Vector2f translation, Rml::TextureHandle) {
    Impl &m = *impl_;
    const Shader &shader = *reinterpret_cast<Shader *>(handle);
    Impl::Draw s = m.make_draw(m.layer(m.layer_count - 1));
    s.geometry = reinterpret_cast<Geometry *>(geometry);
    std::memcpy(s.u.transform, m.transform.data(), sizeof(float) * 16);
    s.u.translate[0] = translation.x, s.u.translate[1] = translation.y;
    s.u.imode[0] = M_GRADIENT;
    s.u.imode[1] = shader.function;
    s.u.imode[2] = int32_t(shader.positions.size());
    s.u.pv[0] = shader.p.x, s.u.pv[1] = shader.p.y, s.u.pv[2] = shader.v.x, s.u.pv[3] = shader.v.y;
    for (size_t i = 0; i < shader.positions.size(); i++) {
        s.u.stop_positions[i] = shader.positions[i];
        std::memcpy(s.u.stop_colors[i], &shader.colors[i][0], sizeof(float) * 4);
    }
    s.scissor = m.scissor_on;
    s.rect = m.scissor;
    s.clip = m.clip_on ? Clip::Test : Clip::None;
    s.stencil_ref = m.clip_ref;
    m.draw(s);
}

void Renderer::ReleaseShader(Rml::CompiledShaderHandle shader) { delete reinterpret_cast<Shader *>(shader); }

} // namespace xf::ui
