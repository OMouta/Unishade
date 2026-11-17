#include "backdrop.h"
#include "log.h"

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
// Read as HLSL by ReShade's compiler, as upscale.cpp's FSR is. The picture is gamma-encoded, and so is what these
// write, as browsers blur. The blur is 25 taps Step apart, 4 steps to a standard deviation, so 3 deviations each way.
constexpr char kSource[] = R"(
uniform float4 Step;
texture2D InputTexture;
sampler2D Input { Texture = InputTexture; };

void vs(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 shrink(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 across = float2(Step.x, -Step.y);
    // Effects leave anything in the picture's alpha, which would show through where the interface draws it.
    float4 sum = tex2D(Input, uv + Step.xy) + tex2D(Input, uv - Step.xy) + tex2D(Input, uv + across) + tex2D(Input, uv - across);
    return float4(sum.rgb * 0.25, 1);
}

float3 Gaussian(float2 uv)
{
    float3 sum = 0;
    float total = 0;
    for (float i = -12.0; i <= 12.0; i += 1.0)
    {
        float weight = exp(-i * i / 32.0);
        sum += tex2D(Input, uv + Step.xy * i).rgb * weight;
        total += weight;
    }
    return sum / total;
}

float4 blur(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(Gaussian(uv), 1);
}

float4 tint(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 color = Gaussian(uv);
    float luma = dot(color, float3(0.213, 0.715, 0.072));
    return float4(saturate(luma + (color - luma) * Step.z), 1);
}

technique Backdrop
{
    pass { VertexShader = vs; PixelShader = shrink; }
    pass { VertexShader = vs; PixelShader = blur; }
    pass { VertexShader = vs; PixelShader = tint; }
}
)";

constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;
// The passes a frame can record: the blur's three, the launcher's preview and a screenshot's thumbnail, and room.
constexpr uint32_t kSlots = 8;

// What one pass reads: its constants, then its picture, as ReShade's compiler lays them out in sets 0 and 1.
struct Slot
{
    GpuBuffer constants;
    VkDescriptorSet sets[2]{};
};

struct Backdrop
{
    bool made = false;
    bool failed = false; // set after an error, so the tray keeps its plain white instead of failing every frame
    VkDescriptorSetLayout setLayouts[2]{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline shrink = VK_NULL_HANDLE;
    VkPipeline blur = VK_NULL_HANDLE;
    VkPipeline tint = VK_NULL_HANDLE;
    Slot slots[kSlots];
    uint32_t used = 0; // slots this frame
};
Backdrop b;

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("could not ") + what + " (VkResult " + std::to_string(result) + ")");
}

VkPipeline CreatePipeline(VkShaderModule vertex, const std::string& vertexEntry, VkShaderModule pixel, const std::string& pixelEntry)
{
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
    info.layout = b.layout;
    info.renderPass = b.renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "create a pipeline");
    return pipeline;
}

void Create()
{
    const auto started = std::chrono::steady_clock::now();
    reshadefx::preprocessor preprocessor;
    preprocessor.append_string(kSource, "backdrop.fx");
    // The same SPIR-V as for effects, with Direct3D's clip space.
    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(true, false, false, false, true));
    reshadefx::parser parser;
    if (!preprocessor.errors().empty() || !parser.parse(preprocessor.output(), codegen.get()))
        throw std::runtime_error("the shader did not compile: " + preprocessor.errors() + parser.errors());
    const reshadefx::effect_module& module = codegen->module();
    if (module.techniques.size() != 1 || module.techniques[0].passes.size() != 3)
        throw std::runtime_error("the shader has no technique");
    const std::vector<reshadefx::pass>& passes = module.techniques[0].passes;

    std::map<std::string, VkShaderModule> shaders;
    const auto destroyShaders = [&] {
        for (const auto& [name, shader] : shaders)
            vkDestroyShaderModule(gpu.device, shader, nullptr);
    };
    try
    {
        for (const std::string& name : { passes[0].vs_entry_point, passes[0].ps_entry_point, passes[1].ps_entry_point, passes[2].ps_entry_point })
        {
            if (shaders.count(name))
                continue;
            std::string binary, assembly, errors;
            if (!codegen->assemble_code_for_entry_point(name, binary, assembly, errors))
                throw std::runtime_error("the shader did not compile: " + errors);
            std::vector<uint32_t> code(binary.size() / 4);
            std::memcpy(code.data(), binary.data(), code.size() * 4);
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = code.size() * 4;
            info.pCode = code.data();
            Check(vkCreateShaderModule(gpu.device, &info, nullptr, &shaders[name]), "create a shader");
        }

        const VkDescriptorSetLayoutBinding bindings[] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr },
                                                          { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr } };
        for (int set = 0; set < 2; ++set)
        {
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = 1;
            info.pBindings = &bindings[set];
            Check(vkCreateDescriptorSetLayout(gpu.device, &info, nullptr, &b.setLayouts[set]), "create a descriptor layout");
        }
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = b.setLayouts;
        Check(vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &b.layout), "create a pipeline layout");
        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots } };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = kSlots * 2;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &b.pool), "create a descriptor pool");
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(gpu.device, &samplerInfo, nullptr, &b.sampler), "create a sampler");

        VkAttachmentDescription attachment{};
        attachment.format = kFormat;
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
        Check(vkCreateRenderPass(gpu.device, &passInfo, nullptr, &b.renderPass), "create a render pass");

        const std::string& vertex = passes[0].vs_entry_point;
        b.shrink = CreatePipeline(shaders[vertex], vertex, shaders[passes[0].ps_entry_point], passes[0].ps_entry_point);
        b.blur = CreatePipeline(shaders[vertex], vertex, shaders[passes[1].ps_entry_point], passes[1].ps_entry_point);
        b.tint = CreatePipeline(shaders[vertex], vertex, shaders[passes[2].ps_entry_point], passes[2].ps_entry_point);

        for (Slot& slot : b.slots)
        {
            if (!gpu.CreateBuffer(slot.constants, sizeof(float) * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
                throw std::runtime_error("could not create a buffer");
            VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocation.descriptorPool = b.pool;
            allocation.descriptorSetCount = 2;
            allocation.pSetLayouts = b.setLayouts;
            Check(vkAllocateDescriptorSets(gpu.device, &allocation, slot.sets), "allocate descriptors");
            const VkDescriptorBufferInfo buffer{ slot.constants.buffer, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = slot.sets[0];
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
        }
    }
    catch (...)
    {
        destroyShaders();
        throw;
    }
    destroyShaders();
    b.made = true;
    Log(LogLevel::Info, "Backdrop shaders compiled in %.0f ms.", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

bool Ready()
{
    if (b.failed)
        return false;
    if (b.made)
        return true;
    try
    {
        Create();
    }
    catch (const std::exception& e)
    {
        // What it made stays until the device goes, as with FSR.
        Log(LogLevel::Error, "The blur behind the menu is off: %s.", e.what());
        b.failed = true;
    }
    return b.made;
}

// Runs a pass over target, reading input. Each pass of a frame has a slot of its own, since a set cannot change once
// commands that use it are recorded.
bool Draw(VkCommandBuffer commands, VkPipeline pipeline, VkImageView input, const backdrop::Target& target, const float (&step)[4])
{
    if (!b.made || b.used >= kSlots || !target.framebuffer)
        return false;
    const Slot& slot = b.slots[b.used++];
    std::memcpy(slot.constants.mapped, step, sizeof(step));
    const VkDescriptorImageInfo image{ b.sampler, input, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = slot.sets[1];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);

    const VkExtent2D extent{ target.image.width, target.image.height };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = b.renderPass;
    begin.framebuffer = target.framebuffer;
    begin.renderArea.extent = extent;
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, b.layout, 0, 2, slot.sets, 0, nullptr);
    const VkViewport viewport{ 0, 0, float(extent.width), float(extent.height), 0, 1 };
    const VkRect2D scissor{ { 0, 0 }, extent };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    FullBarrier(commands);
    return true;
}
} // namespace

namespace backdrop
{
bool Make(VkCommandBuffer commands, Target& target, uint32_t width, uint32_t height)
{
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (target.framebuffer && target.image.width == width && target.image.height == height)
        return true;
    Destroy(target);
    if (!Ready() || !gpu.CreateImage(target.image, width, height, 1, kFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return false;
    InitLayout(commands, target.image);
    VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    info.renderPass = b.renderPass;
    info.attachmentCount = 1;
    info.pAttachments = &target.image.target;
    info.width = width;
    info.height = height;
    info.layers = 1;
    if (vkCreateFramebuffer(gpu.device, &info, nullptr, &target.framebuffer) != VK_SUCCESS)
    {
        Destroy(target);
        return false;
    }
    return true;
}

void Destroy(Target& target)
{
    if (target.framebuffer)
        vkDestroyFramebuffer(gpu.device, target.framebuffer, nullptr);
    target.framebuffer = VK_NULL_HANDLE;
    gpu.DestroyImage(target.image);
}

void Begin(VkCommandBuffer commands)
{
    b.used = 0;
    FullBarrier(commands);
}

bool Shrink(VkCommandBuffer commands, const GpuImage& input, const Target& target)
{
    // Four taps a quarter of target's pixel out from its middle, each blending four of input's pixels when target is a
    // quarter of input's size.
    const float step[4] = { 0.25f / float(target.image.width), 0.25f / float(target.image.height), 0, 0 };
    return Draw(commands, b.shrink, input.view, target, step);
}

bool Blur(VkCommandBuffer commands, const Target& target, const Target& scratch, float radius, float saturation)
{
    // radius is the standard deviation, and the shader takes 4 steps to one.
    const float spacing = std::max(radius, 0.0f) / 4;
    const float across[4] = { spacing / float(target.image.width), 0, 1, 0 };
    const float down[4] = { 0, spacing / float(target.image.height), saturation, 0 };
    return Draw(commands, b.blur, target.image.view, scratch, across) && Draw(commands, b.tint, scratch.image.view, target, down);
}

void Release()
{
    if (!gpu.device)
        return;
    for (VkPipeline pipeline : { b.shrink, b.blur, b.tint })
        if (pipeline)
            vkDestroyPipeline(gpu.device, pipeline, nullptr);
    if (b.renderPass)
        vkDestroyRenderPass(gpu.device, b.renderPass, nullptr);
    for (Slot& slot : b.slots)
        gpu.DestroyBuffer(slot.constants);
    if (b.sampler)
        vkDestroySampler(gpu.device, b.sampler, nullptr);
    if (b.pool)
        vkDestroyDescriptorPool(gpu.device, b.pool, nullptr);
    if (b.layout)
        vkDestroyPipelineLayout(gpu.device, b.layout, nullptr);
    for (VkDescriptorSetLayout layout : b.setLayouts)
        if (layout)
            vkDestroyDescriptorSetLayout(gpu.device, layout, nullptr);
    b = {};
}
} // namespace backdrop
