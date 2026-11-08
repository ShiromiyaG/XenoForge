// A port that draws its own frames (docs/sdk.md, "Native rendering"): at each
// swap it draws the guest's last emulated front buffer, tinted, with a
// spinning triangle over it, into the output image the renderer gives it, and
// the emulated GPU skips the game's draws and resolves while it does. It is a
// test of the API, not a renderer for any game: a real port draws the scene
// its hooks capture of the game's engine.
//
//   player_native <disc> [--capture <hex address>] [--option native_render=false] ...
//
// --capture hooks a guest function (xenoforge/port.h's before()): its calls
// are counted, and a frame with none since the last is yielded to the
// emulated picture, as a port yields frames its capture did not see (a menu, a
// loading screen). The triangle's color follows the calls a frame. A function
// that only the generated code's direct calls reach needs to be in the
// project file's `hooks` (docs/recompiler.md); one called through a pointer
// (a callback, a virtual function) is reached through dispatch as it is.
//
// F6 in the player switches between the port's frames and the emulated ones;
// the overlay (F1) shows both and what was skipped.

#include <xenoforge/native_render.h>
#include <xenoforge/port.h>
#include <xenoforge/run.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// the shaders, SPIR-V that glslc made at the port's build (CMakeLists.txt)
const uint32_t triangle_vert[] = {
#include "triangle.vert.inc"
};
const uint32_t triangle_frag[] = {
#include "triangle.frag.inc"
};
const uint32_t background_vert[] = {
#include "background.vert.inc"
};
const uint32_t background_frag[] = {
#include "background.frag.inc"
};

// the port's options (config.toml's [port], the overlay, --option)
bool g_suppress = true;   // skip the emulated draws and resolves while drawing (else only draw over them)
bool g_background = true; // the guest's front buffer behind the triangle
int g_yield_every = 0;    // yield runs of this many swaps every other run (0: never), to see the switch both ways

// what the hook captures, on the guest's threads
uint32_t g_capture = 0;
std::atomic<uint64_t> g_calls{0};
std::atomic<uint32_t> g_last_argument{0};
uint64_t g_calls_seen = 0; // at the last frame (the renderer's thread)

// the port's Vulkan objects, made at its first frame on the renderer's device
// (left to the process's end: the guest's threads may still run at on_shutdown)
struct Pipelines {
    bool made = false, failed = false;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout triangle_layout = VK_NULL_HANDLE, background_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    static constexpr uint32_t SETS = 8; // a ring: a set is written again only frames after the GPU is done with it
    VkDescriptorSet sets[SETS] = {};
    uint32_t next_set = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipeline triangle = VK_NULL_HANDLE, background = VK_NULL_HANDLE;
} g_vk;

VkShaderModule module(VkDevice device, const uint32_t *words, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes, info.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &m);
    return m;
}

VkPipeline make_pipeline(VkDevice device, VkPipelineLayout layout, const uint32_t *vs, size_t vs_bytes,
                         const uint32_t *fs, size_t fs_bytes, bool vertices) {
    VkShaderModule v = module(device, vs, vs_bytes), f = module(device, fs, fs_bytes);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (v && f) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, v, "main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f, "main"}};
        VkVertexInputBindingDescription binding{0, 5 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                           {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 2 * sizeof(float)}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (vertices) {
            input.vertexBindingDescriptionCount = 1, input.pVertexBindingDescriptions = &binding;
            input.vertexAttributeDescriptionCount = 2, input.pVertexAttributeDescriptions = attributes;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL, raster.cullMode = VK_CULL_MODE_NONE, raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blending.attachmentCount = 1, blending.pAttachments = &blend;
        VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2, dynamic.pDynamicStates = dynamic_states;
        // dynamic rendering into the output image's format
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1, rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = 2, info.pStages = stages;
        info.pVertexInputState = &input, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
        info.pRasterizationState = &raster, info.pMultisampleState = &multisample, info.pColorBlendState = &blending;
        info.pDynamicState = &dynamic, info.layout = layout;
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    }
    if (v) vkDestroyShaderModule(device, v, nullptr);
    if (f) vkDestroyShaderModule(device, f, nullptr);
    return pipeline;
}

bool make_objects(VkDevice device) {
    g_vk.device = device;
    VkPipelineLayoutCreateInfo plain{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(device, &plain, nullptr, &g_vk.triangle_layout) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1, set_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &g_vk.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float)};
    VkPipelineLayoutCreateInfo with_set{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    with_set.setLayoutCount = 1, with_set.pSetLayouts = &g_vk.set_layout;
    with_set.pushConstantRangeCount = 1, with_set.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &with_set, nullptr, &g_vk.background_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Pipelines::SETS};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = Pipelines::SETS, pool.poolSizeCount = 1, pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device, &pool, nullptr, &g_vk.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[Pipelines::SETS];
    for (VkDescriptorSetLayout &l : layouts) l = g_vk.set_layout;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g_vk.pool, allocate.descriptorSetCount = Pipelines::SETS, allocate.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &allocate, g_vk.sets) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &sampler, nullptr, &g_vk.sampler) != VK_SUCCESS) return false;
    g_vk.triangle = make_pipeline(device, g_vk.triangle_layout, triangle_vert, sizeof(triangle_vert), triangle_frag,
                                  sizeof(triangle_frag), true);
    g_vk.background = make_pipeline(device, g_vk.background_layout, background_vert, sizeof(background_vert),
                                    background_frag, sizeof(background_frag), false);
    return g_vk.triangle && g_vk.background;
}

// The port's frame (native_render.h's OutputRenderer), on the renderer's thread.
bool draw_frame(const xf::native::OutputContext &ctx) {
    if (!g_vk.made) {
        g_vk.made = true;
        g_vk.failed = !make_objects(ctx.vk_device);
        if (g_vk.failed) std::fprintf(stderr, "native port: could not make its pipelines\n");
    }
    if (g_vk.failed) return false;
    // with a capture: a frame the hooked function was not called in is the
    // emulated one's
    const uint64_t calls = g_calls.load(std::memory_order_relaxed);
    const uint64_t new_calls = calls - g_calls_seen;
    g_calls_seen = calls;
    if (g_capture && new_calls == 0) return false;
    if (g_yield_every > 0 && (ctx.swap / uint64_t(g_yield_every)) % 2 == 1) return false;

    xf::native::Frame &frame = *ctx.frame;
    // the guest's last front buffer, through the renderer's texture cache
    xf::native::TextureImage front;
    std::string why;
    const bool background = g_background && ctx.guest.address && frame.texture(ctx.guest.fetch, front, why);
    // the triangle: spinning with the swaps, its color from the captured calls
    const float angle = float(ctx.swap) * 0.03f;
    const float hue = g_capture ? float(new_calls % 16) / 16.0f : 0.5f + 0.5f * std::sin(angle * 0.5f);
    float vertices[3 * 5];
    for (int i = 0; i < 3; i++) {
        const float a = angle + float(i) * 2.0943951f;
        float *v = vertices + i * 5;
        v[0] = 0.6f * std::cos(a) * float(ctx.height) / float(ctx.width), v[1] = 0.6f * std::sin(a);
        v[2] = i == 0 ? 1.0f : hue, v[3] = i == 1 ? 1.0f : 1.0f - hue, v[4] = i == 2 ? 1.0f : 0.25f;
    }
    VkBuffer buffer;
    VkDeviceSize offset;
    if (!frame.upload(vertices, sizeof(vertices), 16, buffer, offset)) return false;

    VkCommandBuffer cmd = frame.commands(); // after the helpers: they may have begun a new one
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = ctx.output_view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // the output image's layout, kept
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.05f, 0.05f, 0.1f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {ctx.width, ctx.height}};
    rendering.layerCount = 1, rendering.colorAttachmentCount = 1, rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0, 0, float(ctx.width), float(ctx.height), 0, 1};
    VkRect2D scissor{{0, 0}, {ctx.width, ctx.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (background) {
        VkDescriptorSet set = g_vk.sets[g_vk.next_set++ % Pipelines::SETS];
        VkDescriptorImageInfo image{g_vk.sampler, front.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set, write.descriptorCount = 1, write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(ctx.vk_device, 1, &write, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.background);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.background_layout, 0, 1, &set, 0, nullptr);
        const float tint[4] = {0.55f, 0.7f, 1.0f, 1.0f};
        vkCmdPushConstants(cmd, g_vk.background_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tint), tint);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.triangle);
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    // the port's own argument, taken out before the player's or boot's
    std::vector<char *> args;
    for (int i = 0; i < argc; i++) {
        if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            g_capture = uint32_t(std::strtoul(argv[++i], nullptr, 16));
            continue;
        }
        args.push_back(argv[i]);
    }
    xf::PortInfo port;
    port.name = "Native rendering example";
    port.options.push_back(
        xf::PortOption::boolean("native_render", "Native renderer (F6)", xf::native::enabled_flag()));
    port.options.push_back(xf::PortOption::boolean("suppress", "Skip the emulated draws", g_suppress));
    port.options.push_back(xf::PortOption::boolean("background", "The guest's front buffer behind", g_background));
    port.options.push_back(
        xf::PortOption::integer_range("yield_every", "Yield every other run of N swaps", g_yield_every, 0, 6000));
    port.on_init = [](xf::kernel::Kernel &) {
        if (g_capture) {
            xf::port::before(
                g_capture,
                [](xf::port::Context &c) {
                    g_calls.fetch_add(1, std::memory_order_relaxed);
                    g_last_argument.store(xf::port::arg(c, 0), std::memory_order_relaxed);
                },
                "native_port_capture");
            std::printf("native port: capturing the calls to 0x%08X\n", g_capture);
        }
        xf::native::set_output_renderer(draw_frame);
        // every draw and resolve skipped while the port draws (no filter), or
        // none of them with suppress off (the cost of drawing on top)
        if (!g_suppress) xf::native::set_pass_filter([](const xf::native::PassInfo &) { return false; });
    };
    port.on_shutdown = [](xf::kernel::Kernel &) {
        const xf::native::Counters n = xf::native::counters();
        std::printf("native port: %llu frames drawn, %llu yielded (%llu held); skipped %llu draws and %llu resolves "
                    "(kept %llu, %llu; %llu exporting memory); %llu occlusion reports faked; %llu captured calls "
                    "(last argument 0x%08X)\n",
                    (unsigned long long)n.frames_native, (unsigned long long)n.frames_yielded,
                    (unsigned long long)n.frames_held, (unsigned long long)n.draws_skipped,
                    (unsigned long long)n.resolves_skipped, (unsigned long long)n.draws_kept,
                    (unsigned long long)n.resolves_kept, (unsigned long long)n.memexport_kept,
                    (unsigned long long)n.occlusion_faked, (unsigned long long)g_calls.load(), g_last_argument.load());
        xf::native::set_output_renderer({});
    };
    return xf::run(int(args.size()), args.data(), port);
}
