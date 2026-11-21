#include "uimask.h"
#include "log.h"

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
// Mask marks the interface: 1 inside a rectangle, falling to 0 over the feather outside it. Seed and Fill are a
// push-pull fill: Seed writes the scene at half size with the marked pixels left out, each level of the pyramid above
// it averages what the one below has, and Fill gives a marked pixel the finest levels that reach it. Only pixels fully
// inside a rectangle are filled, and only pixels fully outside one are filled from, so the feather is scene the effects
// see as it is. Restore blends the game's own pixels back by the mask.
constexpr char kSource[] = R"(
uniform float4 Rects[16]; // left, top, right and bottom, in pixels
uniform float4 Params;    // how many rectangles, the feather in pixels, the pyramid's levels
uniform float4 Size;      // of the picture
texture2D PictureTexture;
sampler2D Picture { Texture = PictureTexture; };
texture2D MaskTexture;
sampler2D Mask { Texture = MaskTexture; };
texture2D PyramidTexture;
sampler2D Pyramid { Texture = PyramidTexture; };

void vs(uint id : SV_VertexID, out float4 position : SV_Position)
{
    float2 at = float2((id << 1) & 2, id & 2);
    position = float4(at * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 mask(float4 position : SV_Position) : SV_Target
{
    float covered = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (i < int(Params.x))
        {
            float2 away = max(Rects[i].xy - position.xy, position.xy - Rects[i].zw);
            covered = max(covered, saturate(1 - max(away.x, away.y) / Params.y));
        }
    }
    return float4(covered, 0, 0, 1);
}

float4 seed(float4 position : SV_Position) : SV_Target
{
    int2 corner = int2(position.xy) * 2;
    int2 last = int2(Size.xy) - 1;
    float4 sum = 0;
    for (int i = 0; i < 4; ++i)
    {
        int2 at = min(corner + int2(i & 1, i >> 1), last);
        float scene = tex2Dfetch(Mask, at).x > 0 ? 0 : 1;
        sum += float4(tex2Dfetch(Picture, at).rgb * scene, scene);
    }
    return sum / 4;
}

float4 fill(float4 position : SV_Position) : SV_Target
{
    if (tex2Dfetch(Mask, int2(position.xy)).x < 0.999)
        discard;
    float2 at = position.xy / Size.xy;
    float4 sum = 0;
    float weight = 1;
    for (int level = 0; level < int(Params.z); ++level)
    {
        sum += tex2Dlod(Pyramid, float4(at, 0, level)) * weight;
        weight *= 0.25;
    }
    return float4(sum.rgb / max(sum.a, 0.00001), 1);
}

float4 restore(float4 position : SV_Position) : SV_Target
{
    float covered = tex2Dfetch(Mask, int2(position.xy)).x;
    if (covered <= 0)
        discard;
    return float4(tex2Dfetch(Picture, int2(position.xy)).rgb, covered);
}

technique UiMask
{
    pass { VertexShader = vs; PixelShader = mask; }
    pass { VertexShader = vs; PixelShader = seed; }
    pass { VertexShader = vs; PixelShader = fill; }
    pass { VertexShader = vs; PixelShader = restore; }
}
)";

// The feather, as a share of the picture's height: 4 pixels at 1080.
constexpr float kFeather = 4.0f / 1080;

struct Constants
{
    float rects[uimask::kMostRects][4];
    float params[4];
    float size[4];
};

struct Pass
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[2]{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSet sets[2]{}; // the constants, then the textures it reads
    std::vector<std::pair<uint32_t, std::string>> textures; // each binding's sampler, by name
};

struct State
{
    std::vector<uimask::Rect> rects;
    bool made = false;   // the pipelines
    bool failed = false; // set after an error, so effects run on the whole picture instead of failing every frame
    bool changed = true; // the rectangles, since the mask was drawn
    bool filled = false; // this frame, so Restore has something to put back
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    GpuBuffer constants;
    size_t offsets[3]{}; // of Rects, Params and Size in the constants, as the compiler laid them out
    Pass mask;
    Pass seed;
    Pass fill;
    Pass restore;
    // For the picture these were made for.
    VkImage picture = VK_NULL_HANDLE;
    GpuImage covered;  // the mask
    GpuImage original; // the picture as the game drew it
    GpuImage pyramid;  // half the picture's size, with every level down to one pixel
    VkRect2D area{};   // everything the mask touches
};
State m;

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("could not ") + what + " (VkResult " + std::to_string(result) + ")");
}

void CreatePass(Pass& pass, const reshadefx::effect_module& module, const reshadefx::pass& described, VkFormat format, bool keep, bool blend,
                const std::map<std::string, VkShaderModule>& shaders)
{
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = keep ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    Check(vkCreateRenderPass(gpu.device, &passInfo, nullptr, &pass.renderPass), "create a render pass");

    // Set 0 holds the constants and set 1 the textures the pixel shader reads, as ReShade's compiler lays them out.
    std::vector<VkDescriptorSetLayoutBinding> bindings[2];
    bindings[0].push_back({ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr });
    for (const reshadefx::texture_binding& binding : described.texture_bindings)
    {
        bindings[1].push_back({ binding.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr });
        pass.textures.push_back({ binding.entry_point_binding, module.samplers[binding.index].name });
    }
    for (int set = 0; set < 2; ++set)
    {
        VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        info.bindingCount = static_cast<uint32_t>(bindings[set].size());
        info.pBindings = bindings[set].data();
        Check(vkCreateDescriptorSetLayout(gpu.device, &info, nullptr, &pass.setLayouts[set]), "create a descriptor layout");
    }
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = pass.setLayouts;
    Check(vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &pass.layout), "create a pipeline layout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shaders.at(described.vs_entry_point);
    stages[0].pName = described.vs_entry_point.c_str();
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shaders.at(described.ps_entry_point);
    stages[1].pName = described.ps_entry_point.c_str();
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blending{};
    blending.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    // The picture keeps the alpha it came with.
    if (!keep)
        blending.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    if (blend)
    {
        blending.blendEnable = VK_TRUE;
        blending.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blending.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blending.colorBlendOp = VK_BLEND_OP_ADD;
        blending.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blending.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blending.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blendState{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blending;
    const VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamicState;
    info.layout = pass.layout;
    info.renderPass = pass.renderPass;
    Check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pass.pipeline), "create a pipeline");

    VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocation.descriptorPool = m.pool;
    allocation.descriptorSetCount = 2;
    allocation.pSetLayouts = pass.setLayouts;
    Check(vkAllocateDescriptorSets(gpu.device, &allocation, pass.sets), "allocate descriptors");
    const VkDescriptorBufferInfo buffer{ m.constants.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = pass.sets[0];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
}

void Create(VkFormat format)
{
    reshadefx::preprocessor preprocessor;
    preprocessor.append_string(kSource, "uimask.fx");
    // The same SPIR-V as for effects, with Direct3D's clip space.
    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(true, false, false, false, true));
    reshadefx::parser parser;
    if (!preprocessor.errors().empty() || !parser.parse(preprocessor.output(), codegen.get()))
        throw std::runtime_error("its shader did not compile: " + preprocessor.errors() + parser.errors());
    const reshadefx::effect_module& module = codegen->module();
    if (module.techniques.size() != 1 || module.techniques[0].passes.size() != 4)
        throw std::runtime_error("its shader has no technique");
    const std::vector<reshadefx::pass>& passes = module.techniques[0].passes;
    const char* const names[] = { "Rects", "Params", "Size" };
    for (size_t i = 0; i < std::size(names); ++i)
    {
        const auto found = std::find_if(module.uniforms.begin(), module.uniforms.end(), [&](const reshadefx::uniform& uniform) { return uniform.name == names[i]; });
        if (found == module.uniforms.end())
            throw std::runtime_error("its shader lost a variable");
        m.offsets[i] = found->offset;
    }

    std::map<std::string, VkShaderModule> shaders;
    const auto destroyShaders = [&] {
        for (const auto& [name, shader] : shaders)
            vkDestroyShaderModule(gpu.device, shader, nullptr);
    };
    try
    {
        for (const reshadefx::pass& pass : passes)
            for (const std::string& name : { pass.vs_entry_point, pass.ps_entry_point })
            {
                if (shaders.count(name))
                    continue;
                std::string binary, assembly, errors;
                if (!codegen->assemble_code_for_entry_point(name, binary, assembly, errors))
                    throw std::runtime_error("its shader did not compile: " + errors);
                std::vector<uint32_t> code(binary.size() / 4);
                std::memcpy(code.data(), binary.data(), code.size() * 4);
                VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                info.codeSize = code.size() * 4;
                info.pCode = code.data();
                Check(vkCreateShaderModule(gpu.device, &info, nullptr, &shaders[name]), "create a shader");
            }

        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 } };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 8;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &m.pool), "create a descriptor pool");
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(gpu.device, &samplerInfo, nullptr, &m.sampler), "create a sampler");
        if (!gpu.CreateBuffer(m.constants, sizeof(Constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
            throw std::runtime_error("could not create a buffer");

        CreatePass(m.mask, module, passes[0], VK_FORMAT_R8_UNORM, false, false, shaders);
        CreatePass(m.seed, module, passes[1], VK_FORMAT_R16G16B16A16_SFLOAT, false, false, shaders);
        CreatePass(m.fill, module, passes[2], format, true, false, shaders);
        CreatePass(m.restore, module, passes[3], format, true, true, shaders);
    }
    catch (...)
    {
        destroyShaders();
        throw;
    }
    destroyShaders();
    m.made = true;
}

void DestroyTargets()
{
    for (Pass* pass : { &m.mask, &m.seed, &m.fill, &m.restore })
    {
        if (pass->framebuffer)
            vkDestroyFramebuffer(gpu.device, pass->framebuffer, nullptr);
        pass->framebuffer = VK_NULL_HANDLE;
    }
    for (GpuImage* image : { &m.covered, &m.original, &m.pyramid })
        gpu.DestroyImage(*image);
    m.picture = VK_NULL_HANDLE;
}

VkFramebuffer CreateFramebuffer(const Pass& pass, VkImageView view, uint32_t width, uint32_t height)
{
    VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    info.renderPass = pass.renderPass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    Check(vkCreateFramebuffer(gpu.device, &info, nullptr, &framebuffer), "create a framebuffer");
    return framebuffer;
}

// Points a pass's textures at the images it reads, by the names its shader gives them.
void Bind(const Pass& pass, const std::map<std::string, VkImageView>& views)
{
    for (const auto& [binding, name] : pass.textures)
    {
        const VkDescriptorImageInfo image{ m.sampler, views.at(name), VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pass.sets[1];
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    }
}

// Makes the images for a picture. Whatever used the old ones is done: a picture is only made again once the graphics
// card is idle.
void Resize(VkCommandBuffer commands, const GpuImage& picture)
{
    DestroyTargets();
    const uint32_t halfWidth = (picture.width + 1) / 2, halfHeight = (picture.height + 1) / 2;
    const uint32_t levels = static_cast<uint32_t>(std::floor(std::log2(std::max(halfWidth, halfHeight)))) + 1;
    if (!gpu.CreateImage(m.covered, picture.width, picture.height, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !gpu.CreateImage(m.original, picture.width, picture.height, 1, picture.format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !gpu.CreateImage(m.pyramid, halfWidth, halfHeight, levels, VK_FORMAT_R16G16B16A16_SFLOAT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        throw std::runtime_error("the graphics card has no room for its images");
    for (const GpuImage* image : { &m.covered, &m.original, &m.pyramid })
        InitLayout(commands, *image);
    m.mask.framebuffer = CreateFramebuffer(m.mask, m.covered.target, picture.width, picture.height);
    m.seed.framebuffer = CreateFramebuffer(m.seed, m.pyramid.target, halfWidth, halfHeight);
    m.fill.framebuffer = CreateFramebuffer(m.fill, picture.target, picture.width, picture.height);
    m.restore.framebuffer = CreateFramebuffer(m.restore, picture.target, picture.width, picture.height);
    Bind(m.seed, { { "Picture", picture.view }, { "Mask", m.covered.view } });
    Bind(m.fill, { { "Mask", m.covered.view }, { "Pyramid", m.pyramid.view } });
    Bind(m.restore, { { "Picture", m.original.view }, { "Mask", m.covered.view } });
    m.picture = picture.image;
    m.changed = true;
}

void Draw(VkCommandBuffer commands, const Pass& pass, VkExtent2D extent, VkRect2D area)
{
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = pass.renderPass;
    begin.framebuffer = pass.framebuffer;
    begin.renderArea = area;
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 2, pass.sets, 0, nullptr);
    const VkViewport viewport{ 0, 0, float(extent.width), float(extent.height), 0, 1 };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &area);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    FullBarrier(commands);
}

// Draws the mask for the rectangles as they are now, and notes the part of the picture it touches.
void DrawMask(VkCommandBuffer commands, const GpuImage& picture)
{
    const float width = float(picture.width), height = float(picture.height);
    const float feather = std::max(2.0f, std::round(height * kFeather));
    Constants constants{};
    float left = width, top = height, right = 0, bottom = 0;
    for (size_t i = 0; i < m.rects.size(); ++i)
    {
        const uimask::Rect& rect = m.rects[i];
        float* out = constants.rects[i];
        out[0] = std::round(rect[0] * width);
        out[1] = std::round(rect[1] * height);
        out[2] = std::round((rect[0] + rect[2]) * width);
        out[3] = std::round((rect[1] + rect[3]) * height);
        left = std::min(left, out[0]);
        top = std::min(top, out[1]);
        right = std::max(right, out[2]);
        bottom = std::max(bottom, out[3]);
    }
    constants.params[0] = float(m.rects.size());
    constants.params[1] = feather;
    constants.params[2] = float(m.pyramid.levels);
    constants.size[0] = width;
    constants.size[1] = height;
    uint8_t* mapped = static_cast<uint8_t*>(m.constants.mapped);
    std::memcpy(mapped + m.offsets[0], constants.rects, sizeof(constants.rects));
    std::memcpy(mapped + m.offsets[1], constants.params, sizeof(constants.params));
    std::memcpy(mapped + m.offsets[2], constants.size, sizeof(constants.size));

    const int32_t x0 = std::clamp(int32_t(left - feather), 0, int32_t(picture.width)), y0 = std::clamp(int32_t(top - feather), 0, int32_t(picture.height));
    const int32_t x1 = std::clamp(int32_t(right + feather), x0, int32_t(picture.width)), y1 = std::clamp(int32_t(bottom + feather), y0, int32_t(picture.height));
    m.area = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
    const VkExtent2D extent{ picture.width, picture.height };
    Draw(commands, m.mask, extent, { { 0, 0 }, extent });
    m.changed = false;
}
} // namespace

namespace uimask
{
void Set(const std::vector<Rect>& rects)
{
    std::vector<Rect> kept(rects.begin(), rects.begin() + static_cast<std::ptrdiff_t>(std::min(rects.size(), kMostRects)));
    if (kept == m.rects)
        return;
    m.rects = std::move(kept);
    m.changed = true;
}

bool Fill(VkCommandBuffer commands, const GpuImage& picture)
{
    m.filled = false;
    if (m.failed || m.rects.empty() || !picture.image)
        return false;
    try
    {
        if (!m.made)
            Create(picture.format);
        if (m.picture != picture.image || m.covered.width != picture.width || m.covered.height != picture.height)
            Resize(commands, picture);
        if (m.changed)
            DrawMask(commands, picture);
        if (!m.area.extent.width || !m.area.extent.height)
            return false;

        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.srcOffset = { m.area.offset.x, m.area.offset.y, 0 };
        region.dstSubresource = region.srcSubresource;
        region.dstOffset = region.srcOffset;
        region.extent = { m.area.extent.width, m.area.extent.height, 1 };
        vkCmdCopyImage(commands, picture.image, VK_IMAGE_LAYOUT_GENERAL, m.original.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        FullBarrier(commands);

        const VkExtent2D half{ m.pyramid.width, m.pyramid.height };
        Draw(commands, m.seed, half, { { 0, 0 }, half });
        for (uint32_t level = 1; level < m.pyramid.levels; ++level)
        {
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
            blit.srcOffsets[1] = { std::max(1, int(half.width >> (level - 1))), std::max(1, int(half.height >> (level - 1))), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            blit.dstOffsets[1] = { std::max(1, int(half.width >> level)), std::max(1, int(half.height >> level)), 1 };
            vkCmdBlitImage(commands, m.pyramid.image, VK_IMAGE_LAYOUT_GENERAL, m.pyramid.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
            FullBarrier(commands);
        }
        Draw(commands, m.fill, { picture.width, picture.height }, m.area);
    }
    catch (const std::exception& e)
    {
        // What it made stays until the device goes, since commands already recorded may use it.
        Log(LogLevel::Error, "Keeping the game's interface out of effects stopped: %s.", e.what());
        m.failed = true;
        return false;
    }
    m.filled = true;
    return true;
}

void Restore(VkCommandBuffer commands, const GpuImage& picture)
{
    if (!std::exchange(m.filled, false))
        return;
    FullBarrier(commands);
    Draw(commands, m.restore, { picture.width, picture.height }, m.area);
}

void Release()
{
    if (!gpu.device)
        return;
    DestroyTargets();
    for (Pass* pass : { &m.mask, &m.seed, &m.fill, &m.restore })
    {
        if (pass->pipeline)
            vkDestroyPipeline(gpu.device, pass->pipeline, nullptr);
        if (pass->layout)
            vkDestroyPipelineLayout(gpu.device, pass->layout, nullptr);
        for (VkDescriptorSetLayout layout : pass->setLayouts)
            if (layout)
                vkDestroyDescriptorSetLayout(gpu.device, layout, nullptr);
        if (pass->renderPass)
            vkDestroyRenderPass(gpu.device, pass->renderPass, nullptr);
        *pass = {};
    }
    if (m.sampler)
        vkDestroySampler(gpu.device, m.sampler, nullptr);
    if (m.pool)
        vkDestroyDescriptorPool(gpu.device, m.pool, nullptr);
    gpu.DestroyBuffer(m.constants);
    m.sampler = VK_NULL_HANDLE;
    m.pool = VK_NULL_HANDLE;
    m.made = false;
    m.filled = false;
}
} // namespace uimask
