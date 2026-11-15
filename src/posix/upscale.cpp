#include "upscale.h"
#include "log.h"

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// AMD's ffx_a.h and ffx_fsr1.h, which posix.cmake builds into the program.
extern const unsigned char kFfxA[];
extern const unsigned kFfxASize;
extern const unsigned char kFfxFsr1[];
extern const unsigned kFfxFsr1Size;

namespace
{
// AMD's headers go between these, read as HLSL, which ReShade's compiler takes. EASU upscales and RCAS sharpens. Both
// expect gamma-encoded colors, which the picture has.
constexpr char kPrelude[] = R"(
#define A_GPU 1
#define A_HLSL 1
)";

constexpr char kCallbacks[] = R"(
uniform uint4 Con0;
uniform uint4 Con1;
uniform uint4 Con2;
uniform uint4 Con3;
texture2D InputTexture;
sampler2D Input { Texture = InputTexture; };
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
AF4 FsrEasuRF(AF2 p) { return tex2DgatherR(Input, p); }
AF4 FsrEasuGF(AF2 p) { return tex2DgatherG(Input, p); }
AF4 FsrEasuBF(AF2 p) { return tex2DgatherB(Input, p); }
AF4 FsrRcasLoadF(ASU2 p) { return tex2Dfetch(Input, p); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";

constexpr char kEntryPoints[] = R"(
void vs(uint id : SV_VertexID, out float4 position : SV_Position)
{
    float2 at = float2((id << 1) & 2, id & 2);
    position = float4(at * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 easu(float4 position : SV_Position) : SV_Target
{
    AF3 color;
    FsrEasuF(color, AU2(position.xy), Con0, Con1, Con2, Con3);
    return float4(color, 1);
}

float4 rcas(float4 position : SV_Position) : SV_Target
{
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(position.xy), Con0);
    return float4(r, g, b, 1);
}

technique FSR
{
    pass { VertexShader = vs; PixelShader = easu; }
    pass { VertexShader = vs; PixelShader = rcas; }
}
)";

// RCAS sharpening, in stops below its strongest at 0.
constexpr float kSharpness = 0.2f;

struct Constants
{
    uint32_t con[4][4];
};

// EASU, which reads the picture and writes upscaled, or RCAS, which reads upscaled and writes sharpened.
struct Pass
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    GpuBuffer constants;
    VkDescriptorSet sets[2]{}; // the constants, then the picture it reads
};

struct Upscaler
{
    bool made = false;   // the pipelines
    bool failed = false; // set after an error, so the picture is stretched instead of failing every frame
    bool on = false;     // for the log
    VkDescriptorSetLayout setLayouts[2]{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    Pass easu;
    Pass rcas;
    // At the window's size. EASU writes more than 8 bits, so RCAS does not sharpen EASU's rounding.
    GpuImage upscaled;
    GpuImage sharpened;
};
Upscaler u;

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("could not ") + what + " (VkResult " + std::to_string(result) + ")");
}

uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

// FsrEasuCon from ffx_fsr1.h, for an input that fills its texture.
Constants EasuConstants(float inputWidth, float inputHeight, float outputWidth, float outputHeight)
{
    const float x = 1 / inputWidth;
    const float y = 1 / inputHeight;
    return { { { Bits(inputWidth / outputWidth), Bits(inputHeight / outputHeight), Bits(0.5f * inputWidth / outputWidth - 0.5f),
                 Bits(0.5f * inputHeight / outputHeight - 0.5f) },
               { Bits(x), Bits(y), Bits(x), Bits(-y) },
               { Bits(-x), Bits(2 * y), Bits(x), Bits(2 * y) },
               { Bits(0), Bits(4 * y), 0, 0 } } };
}

void CreatePass(Pass& pass, VkFormat format, VkShaderModule vertex, const std::string& vertexEntry, VkShaderModule pixel,
                const std::string& pixelEntry)
{
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = vertexEntry.c_str();
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixel;
    stages[1].pName = pixelEntry.c_str();
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
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
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
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamicState;
    info.layout = u.layout;
    info.renderPass = pass.renderPass;
    Check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pass.pipeline), "create a pipeline");

    if (!gpu.CreateBuffer(pass.constants, sizeof(Constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
        throw std::runtime_error("could not create a buffer");
    VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocation.descriptorPool = u.pool;
    allocation.descriptorSetCount = 2;
    allocation.pSetLayouts = u.setLayouts;
    Check(vkAllocateDescriptorSets(gpu.device, &allocation, pass.sets), "allocate descriptors");
    const VkDescriptorBufferInfo buffer{ pass.constants.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = pass.sets[0];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
}

void Create()
{
    const auto started = std::chrono::steady_clock::now();
    const std::string source = std::string(kPrelude) + std::string(reinterpret_cast<const char*>(kFfxA), kFfxASize) + kCallbacks +
                               std::string(reinterpret_cast<const char*>(kFfxFsr1), kFfxFsr1Size) + kEntryPoints;
    reshadefx::preprocessor preprocessor;
    preprocessor.append_string(source, "fsr.fx");
    // The same SPIR-V as for effects, with Direct3D's clip space.
    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(true, false, false, false, true));
    reshadefx::parser parser;
    if (!preprocessor.errors().empty() || !parser.parse(preprocessor.output(), codegen.get()))
        throw std::runtime_error("the FSR shader did not compile: " + preprocessor.errors() + parser.errors());
    const reshadefx::effect_module& module = codegen->module();
    if (module.techniques.size() != 1 || module.techniques[0].passes.size() != 2)
        throw std::runtime_error("the FSR shader has no technique");
    const reshadefx::pass& easu = module.techniques[0].passes[0];
    const reshadefx::pass& rcas = module.techniques[0].passes[1];

    std::map<std::string, VkShaderModule> shaders;
    const auto destroyShaders = [&] {
        for (const auto& [name, shader] : shaders)
            vkDestroyShaderModule(gpu.device, shader, nullptr);
    };
    try
    {
        for (const std::string& name : { easu.vs_entry_point, easu.ps_entry_point, rcas.ps_entry_point })
        {
            std::string binary, assembly, errors;
            if (!codegen->assemble_code_for_entry_point(name, binary, assembly, errors))
                throw std::runtime_error("the FSR shader did not compile: " + errors);
            std::vector<uint32_t> code(binary.size() / 4);
            std::memcpy(code.data(), binary.data(), code.size() * 4);
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = code.size() * 4;
            info.pCode = code.data();
            Check(vkCreateShaderModule(gpu.device, &info, nullptr, &shaders[name]), "create a shader");
        }

        // Set 0 holds the constants and set 1 the one texture, as ReShade's compiler lays them out.
        const VkDescriptorSetLayoutBinding bindings[] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
                                                          { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr } };
        for (int set = 0; set < 2; ++set)
        {
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = 1;
            info.pBindings = &bindings[set];
            Check(vkCreateDescriptorSetLayout(gpu.device, &info, nullptr, &u.setLayouts[set]), "create a descriptor layout");
        }
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = u.setLayouts;
        Check(vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &u.layout), "create a pipeline layout");
        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &u.pool), "create a descriptor pool");
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(gpu.device, &samplerInfo, nullptr, &u.sampler), "create a sampler");

        CreatePass(u.easu, VK_FORMAT_A2B10G10R10_UNORM_PACK32, shaders[easu.vs_entry_point], easu.vs_entry_point, shaders[easu.ps_entry_point],
                   easu.ps_entry_point);
        CreatePass(u.rcas, VK_FORMAT_B8G8R8A8_UNORM, shaders[rcas.vs_entry_point], rcas.vs_entry_point, shaders[rcas.ps_entry_point],
                   rcas.ps_entry_point);
    }
    catch (...)
    {
        destroyShaders();
        throw;
    }
    destroyShaders();
    u.made = true;
    Log(LogLevel::Info, "FSR shaders compiled in %.0f ms.", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void DestroyTargets()
{
    for (Pass* pass : { &u.easu, &u.rcas })
    {
        if (pass->framebuffer)
            vkDestroyFramebuffer(gpu.device, pass->framebuffer, nullptr);
        pass->framebuffer = VK_NULL_HANDLE;
    }
    gpu.DestroyImage(u.upscaled);
    gpu.DestroyImage(u.sharpened);
}

VkFramebuffer CreateFramebuffer(const Pass& pass, const GpuImage& image)
{
    VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    info.renderPass = pass.renderPass;
    info.attachmentCount = 1;
    info.pAttachments = &image.target;
    info.width = image.width;
    info.height = image.height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    Check(vkCreateFramebuffer(gpu.device, &info, nullptr, &framebuffer), "create a framebuffer");
    return framebuffer;
}

// Makes what the passes write the window's size. The previous frame is done with the old ones.
void Resize(VkCommandBuffer commands, VkExtent2D extent)
{
    DestroyTargets();
    if (!gpu.CreateImage(u.upscaled, extent.width, extent.height, 1, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !gpu.CreateImage(u.sharpened, extent.width, extent.height, 1, VK_FORMAT_B8G8R8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("the graphics card has no room for its images");
    InitLayout(commands, u.upscaled);
    InitLayout(commands, u.sharpened);
    u.easu.framebuffer = CreateFramebuffer(u.easu, u.upscaled);
    u.rcas.framebuffer = CreateFramebuffer(u.rcas, u.sharpened);
}

// Runs a pass over extent, reading view.
void Draw(VkCommandBuffer commands, const Pass& pass, VkImageView view, VkExtent2D extent, const Constants& constants)
{
    std::memcpy(pass.constants.mapped, &constants, sizeof(constants));
    // The picture effects drew can be another image on every frame, so the set is written each time. The previous
    // frame is done with it.
    const VkDescriptorImageInfo image{ u.sampler, view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = pass.sets[1];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);

    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = pass.renderPass;
    begin.framebuffer = pass.framebuffer;
    begin.renderArea.extent = extent;
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, u.layout, 0, 2, pass.sets, 0, nullptr);
    const VkViewport viewport{ 0, 0, float(extent.width), float(extent.height), 0, 1 };
    const VkRect2D scissor{ { 0, 0 }, extent };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    FullBarrier(commands);
}
} // namespace

bool Upscale(VkCommandBuffer commands, int effectResolution, const GpuImage& input, VkImage target, VkExtent2D extent)
{
    // FSR is for effects below 100%, and only scales up. At 100% the picture can be a few pixels off the window's size,
    // which stretching handles as before.
    if (u.failed || effectResolution >= 100 || input.width >= extent.width || input.height >= extent.height)
    {
        if (std::exchange(u.on, false))
            Log(LogLevel::Info, "FSR upscaling off.");
        return false;
    }
    try
    {
        if (!u.made)
            Create();
        if (u.upscaled.width != extent.width || u.upscaled.height != extent.height)
            Resize(commands, extent);
        Draw(commands, u.easu, input.view, extent,
             EasuConstants(float(input.width), float(input.height), float(extent.width), float(extent.height)));
        // FsrRcasCon turns sharpness in stops into a linear scale.
        Draw(commands, u.rcas, u.upscaled.view, extent, { { { Bits(std::exp2(-kSharpness)) } } });
    }
    catch (const std::exception& e)
    {
        // What it made stays until the device goes, since commands already recorded may use it.
        Log(LogLevel::Error, "FSR upscaling stopped: %s. Effects below 100%% are stretched instead.", e.what());
        u.failed = true;
        u.on = false;
        return false;
    }

    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { int(extent.width), int(extent.height), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(commands, u.sharpened.image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    if (!std::exchange(u.on, true))
        Log(LogLevel::Info, "FSR upscaling on: %ux%u -> %ux%u.", input.width, input.height, extent.width, extent.height);
    return true;
}

void ReleaseUpscaler()
{
    if (!gpu.device)
        return;
    DestroyTargets();
    for (Pass* pass : { &u.easu, &u.rcas })
    {
        if (pass->pipeline)
            vkDestroyPipeline(gpu.device, pass->pipeline, nullptr);
        if (pass->renderPass)
            vkDestroyRenderPass(gpu.device, pass->renderPass, nullptr);
        gpu.DestroyBuffer(pass->constants);
    }
    if (u.sampler)
        vkDestroySampler(gpu.device, u.sampler, nullptr);
    if (u.pool)
        vkDestroyDescriptorPool(gpu.device, u.pool, nullptr);
    if (u.layout)
        vkDestroyPipelineLayout(gpu.device, u.layout, nullptr);
    for (VkDescriptorSetLayout layout : u.setLayouts)
        if (layout)
            vkDestroyDescriptorSetLayout(gpu.device, layout, nullptr);
    u = {};
}
