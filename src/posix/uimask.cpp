#include "uimask.h"
#include "config.h"
#include "image.h"
#include "log.h"

#include <effect_codegen.hpp>
#include <effect_parser.hpp>
#include <effect_preprocessor.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
// Compose draws the mask: detection's result, upsampled to the picture's size, with the marked rectangles over it, each
// 1 inside and falling to 0 over the feather outside it. Interface rectangles add to the mask and scene ones take from
// it. Seed and Fill are a push-pull fill: Seed writes the scene at half size with the masked pixels left out, each level
// of the pyramid above it averages what the one below has, and Fill gives a fully masked pixel the finest levels that
// reach it. Only fully masked pixels are filled, and only pixels outside the mask are filled from, so a soft edge is
// scene the effects see as it is. Restore blends the game's own pixels back by the mask.
//
// Detection works at half the picture's size on each new picture. Observe measures it against the history: Low holds
// each pixel's lowest color lately and how sure detection is that it is interface, High its highest color and its
// brightness when last counted. Update counts a picture only when enough of it moved since the last one counted, so a
// still camera teaches nothing and the mask holds, and a hard cut, where everything moves at once, is one more picture.
// On a counted picture, a pixel that stayed within the tolerance while its neighbourhood moved, with an edge near it,
// grows surer, and the ranges narrow toward the color now, so a part that stops moving can be found. A pixel away from
// where it was grows less sure quickly on any picture, so interface that hides, as for a cutscene, is let go even when
// nothing else moves. Shape grows what is sure by a pixel, Holes fills what lies between masked pixels on both sides,
// such as a health bar's moving fill inside its still frame, and Compose snaps the result to the picture's edges.
constexpr char kSource[] = R"(
uniform float4 Rects[16]; // left, top, right and bottom, in pixels: the interface ones, then the scene ones
uniform float4 Params;    // how many interface rectangles, how many scene ones, the feather in pixels, the pyramid's levels
uniform float4 Size;      // of the picture, then of detection's images, half of it
uniform float4 Detect;    // 1 while detecting, 1 when the history starts over, an HDR picture's 1 / SDR white or 0, Activity's top level
uniform float4 Lods;      // levels of Activity: a pixel's neighbourhood, a wider one, and around it for edges
texture2D PictureTexture;
sampler2D Picture { Texture = PictureTexture; };
texture2D MaskTexture;
sampler2D Mask { Texture = MaskTexture; };
texture2D PyramidTexture;
sampler2D Pyramid { Texture = PyramidTexture; };
texture2D LowTexture;
sampler2D Low { Texture = LowTexture; };
texture2D HighTexture;
sampler2D High { Texture = HighTexture; };
texture2D ObservedTexture;
sampler2D Observed { Texture = ObservedTexture; };
texture2D ActivityTexture;
sampler2D Activity { Texture = ActivityTexture; };
texture2D ShapeTexture;
sampler2D Shape { Texture = ShapeTexture; };
texture2D DetectedTexture;
sampler2D Detected { Texture = DetectedTexture; };
texture2D RememberedTexture;
sampler2D Remembered { Texture = RememberedTexture; };

// In the units Perceived gives, where SDR's black to white is 0 to 1. A pixel within this of its range has not moved:
// about 10 of 255, above what scaling the picture for effect resolution changes in a still one.
static const float Tolerance = 0.04;
// The share of the picture that must have moved for a picture to count, so a character walking in front of a still
// camera does not make the room around them interface.
static const float Moving = 0.2;
// How much a neighbourhood's pixels moved on average for a still pixel among them to count: a part of the picture
// that moves, not noise.
static const float Busy = 0.06;
// The average edge around a pixel that interface needs, which a flat sky does not have.
static const float Edges = 0.03;
// About 12 counted pictures where a pixel stays still mask it, and 2 new pictures where it is away from where it was
// release it, counted or not. A range narrows by Narrowing on each counted picture.
static const float Rise = 1.0 / 24;
static const float Fall = 0.5;
static const float Narrowing = 0.02;
// How far apart two colors are before the mask stops following one into the other.
static const float ColorSigma = 0.1;

void vs(uint id : SV_VertexID, out float4 position : SV_Position)
{
    float2 at = float2((id << 1) & 2, id & 2);
    position = float4(at * float2(2, -2) + float2(-1, 1), 0, 1);
}

// Gamma-encoded, as SDR is already, so a step means about as much in shadows as in highlights. An HDR picture is
// brought to SDR white first, and keeps what is brighter.
float3 Perceived(float3 color)
{
    if (Detect.z > 0)
        color = pow(max(color * Detect.z, 0.0), 1.0 / 2.2);
    return color;
}

float4 compose(float4 position : SV_Position) : SV_Target
{
    float detected = 0;
    if (Detect.x > 0)
    {
        // A joint bilateral upsample: each of the 4 detection pixels around this one counts by how close it is, and by
        // how close the color it saw is to this pixel's, so the mask's edges follow the picture's.
        float3 color = Perceived(tex2Dfetch(Picture, int2(position.xy)).rgb);
        float2 at = position.xy / 2 - 0.5;
        int2 corner = int2(floor(at));
        float2 offset = at - floor(at);
        float sum = 0;
        float weights = 0;
        for (int i = 0; i < 4; ++i)
        {
            int2 side = int2(i & 1, i >> 1);
            int2 texel = clamp(corner + side, int2(0, 0), int2(Size.zw) - 1);
            float2 closeness = lerp(1 - offset, offset, float2(side.x, side.y));
            float3 difference = tex2Dfetch(Observed, texel).rgb - color;
            float weight = closeness.x * closeness.y * (exp(-dot(difference, difference) / (2 * ColorSigma * ColorSigma)) + 0.001);
            sum += weight * tex2Dfetch(Detected, texel).x;
            weights += weight;
        }
        detected = sum / max(weights, 0.000001);
    }
    float marked = 0;
    float scene = 0;
    for (int r = 0; r < 16; ++r)
    {
        float2 away = max(Rects[r].xy - position.xy, position.xy - Rects[r].zw);
        float inside = saturate(1 - max(away.x, away.y) / Params.z);
        if (r < int(Params.x))
            marked = max(marked, inside);
        else if (r < int(Params.x + Params.y))
            scene = max(scene, inside);
    }
    return float4(min(max(detected, marked), 1 - scene), 0, 0, 1);
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
    for (int level = 0; level < int(Params.w); ++level)
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

void observe(float4 position : SV_Position, out float4 observed : SV_Target0, out float4 activity : SV_Target1)
{
    // The middle of the 2 by 2 pixels this one covers, where the filter averages them, and the blocks beside it.
    float2 at = position.xy * 2 / Size.xy;
    float2 across = float2(2 / Size.x, 0);
    float2 down = float2(0, 2 / Size.y);
    float3 color = Perceived(tex2D(Picture, at).rgb);
    float3 gradient = max(abs(Perceived(tex2D(Picture, at + across).rgb) - Perceived(tex2D(Picture, at - across).rgb)),
                          abs(Perceived(tex2D(Picture, at + down).rgb) - Perceived(tex2D(Picture, at - down).rgb)));
    float4 low = tex2Dfetch(Low, int2(position.xy));
    float4 high = tex2Dfetch(High, int2(position.xy));
    // How far it moved lately, this picture included, and whether it moved since the last picture counted: its
    // brightness changed, or its color left the range it had.
    float3 range = max(high.rgb, color) - min(low.rgb, color);
    float3 outside = max(low.rgb - color, color - high.rgb);
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float spread = max(range.r, max(range.g, range.b));
    float moved = max(abs(luma - high.a), max(outside.r, max(outside.g, outside.b))) > Tolerance ? 1 : 0;
    // A history that starts over holds nothing yet.
    if (Detect.y > 0)
    {
        spread = 0;
        moved = 0;
    }
    observed = float4(color, 1);
    activity = float4(spread, moved, max(gradient.r, max(gradient.g, gradient.b)), 1);
}

void update(float4 position : SV_Position, out float4 low : SV_Target0, out float4 high : SV_Target1)
{
    int2 at = int2(position.xy);
    float2 uv = position.xy / Size.zw;
    float3 color = tex2Dfetch(Observed, at).rgb;
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    low = tex2Dfetch(Low, at);
    high = tex2Dfetch(High, at);
    if (Detect.y > 0)
    {
        // What it remembers of the game holds until pictures say otherwise.
        low = float4(color, tex2Dlod(Remembered, float4(uv, 0, 0)).x);
        high = float4(color, luma);
    }
    else
    {
        float4 here = tex2Dfetch(Activity, at);
        bool counted = tex2Dlod(Activity, float4(0.5, 0.5, 0, Detect.w)).y >= Moving;
        float sure = low.a;
        if (here.y > 0)
            sure -= Fall;
        else if (counted)
        {
            float around = max(tex2Dlod(Activity, float4(uv, 0, Lods.x)).x, tex2Dlod(Activity, float4(uv, 0, Lods.y)).x);
            float edges = tex2Dlod(Activity, float4(uv, 0, Lods.z)).z;
            if (around > Busy)
                sure += here.x <= Tolerance && edges > Edges ? Rise : -Rise;
        }
        low.a = saturate(sure);
        if (counted)
        {
            low.rgb = min(low.rgb + Narrowing, color);
            high = float4(max(high.rgb - Narrowing, color), luma);
        }
    }
}

float4 shape(float4 position : SV_Position) : SV_Target
{
    float sure = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sure = max(sure, tex2Dfetch(Low, clamp(int2(position.xy) + int2(x, y), int2(0, 0), int2(Size.zw) - 1)).a);
    return float4(smoothstep(0.4, 0.6, sure), 0, 0, 1);
}

float4 holes(float4 position : SV_Position) : SV_Target
{
    // Masked on both sides, 4, 8 or 16 pixels away, across or up and down.
    float2 uv = position.xy / Size.zw;
    float left = 0;
    float right = 0;
    float up = 0;
    float down = 0;
    for (int i = 0; i < 3; ++i)
    {
        float2 reach = exp2(i + 2.0) / Size.zw;
        left = max(left, tex2Dlod(Shape, float4(uv - float2(reach.x, 0), 0, 0)).x);
        right = max(right, tex2Dlod(Shape, float4(uv + float2(reach.x, 0), 0, 0)).x);
        up = max(up, tex2Dlod(Shape, float4(uv - float2(0, reach.y), 0, 0)).x);
        down = max(down, tex2Dlod(Shape, float4(uv + float2(0, reach.y), 0, 0)).x);
    }
    float covered = tex2Dfetch(Shape, int2(position.xy)).x;
    return float4(max(covered, max(min(left, right), min(up, down))), 0, 0, 1);
}

float4 view(float4 position : SV_Position) : SV_Target
{
    return float4(1, 0.2, 0.6, tex2Dlod(Mask, float4(position.xy / Size.zw, 0, 0)).x * 0.6);
}

technique UiMask
{
    pass { VertexShader = vs; PixelShader = compose; }
    pass { VertexShader = vs; PixelShader = seed; }
    pass { VertexShader = vs; PixelShader = fill; }
    pass { VertexShader = vs; PixelShader = restore; }
    pass { VertexShader = vs; PixelShader = observe; }
    pass { VertexShader = vs; PixelShader = update; }
    pass { VertexShader = vs; PixelShader = shape; }
    pass { VertexShader = vs; PixelShader = holes; }
    pass { VertexShader = vs; PixelShader = view; }
}
)";

// The feather, as a share of the picture's height: 4 pixels at 1080.
constexpr float kFeather = 4.0f / 1080;
// The mask kept for a game: detection's certainty, stretched to this size whatever the picture's, each pixel the most
// of those it covers, so thin parts survive.
constexpr uint32_t kRememberedWidth = 384;
constexpr uint32_t kRememberedHeight = 216;
// Pictures learned from, since the history started over, before what detection holds is worth keeping.
constexpr uint32_t kSettled = 600;

struct Constants
{
    float rects[uimask::kMostRects][4];
    float params[4];
    float size[4];
    float detect[4];
    float lods[4];
};

struct Pass
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayouts[2]{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Detection's passes read and write the history, which takes turns between two images, so they have one of each
    // for either turn. The other passes use the first.
    VkFramebuffer framebuffers[2]{};
    VkDescriptorSet constants = VK_NULL_HANDLE;
    VkDescriptorSet textures[2]{};
    std::vector<std::pair<uint32_t, std::string>> names; // each binding's sampler, by name
};

struct State
{
    std::vector<uimask::Rect> rects;
    bool detecting = false;
    fs::path rememberedPath;
    std::vector<uint8_t> remembered; // kRememberedWidth by kRememberedHeight, or empty for nothing yet
    uint64_t serial = 0;        // of the picture the next Fill gets
    uint64_t learned = ~0ull;   // the last picture detection learned from
    bool fresh = true;          // the history starts over from remembered with the next picture
    bool reseed = true;         // remembered goes to the graphics card first
    uint32_t pictures = 0;      // learned from since the history started over
    bool shown = false;
    uint32_t viewMade = 0;
    float hdrScale = 0; // as the constants have it
    bool made = false;   // the pipelines
    bool failed = false; // set after an error, so effects run on the whole picture instead of failing every frame
    bool changed = true; // the rectangles or detection, since the mask was drawn
    bool filled = false; // this frame, so Restore has something to put back
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    GpuBuffer constants;
    size_t offsets[5]{}; // of Rects, Params, Size, Detect and Lods in the constants, as the compiler laid them out
    GpuImage rememberedImage;
    GpuBuffer upload;
    Pass compose;
    Pass seed;
    Pass fill;
    Pass restore;
    Pass observe;
    Pass update;
    Pass shape;
    Pass holes;
    Pass view;
    // For the picture these were made for.
    VkImage picture = VK_NULL_HANDLE;
    GpuImage covered;  // the mask
    GpuImage original; // the picture as the game drew it
    GpuImage pyramid;  // half the picture's size, with every level down to one pixel
    // Detection's, at half the picture's size while it is on.
    GpuImage observed; // the picture, as Perceived sees it
    GpuImage activity; // how far each pixel moved lately, whether it moved now and its edge, with every level
    GpuImage low[2];   // the history, in turns
    GpuImage high[2];
    GpuImage shaped;
    GpuImage detected;
    uint32_t turn = 0;  // which of low and high hold the history
    GpuImage tint;      // the mask tinted, at half the picture's size, while shown
    VkRect2D area{};    // everything the mask touches
};
State m;

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("could not ") + what + " (VkResult " + std::to_string(result) + ")");
}

bool Marked()
{
    return std::any_of(m.rects.begin(), m.rects.end(), [](const uimask::Rect& rect) { return rect[4] == 0; });
}

void CreatePass(Pass& pass, const reshadefx::effect_module& module, const reshadefx::pass& described, const std::vector<VkFormat>& formats, bool keep,
                bool blend, const std::map<std::string, VkShaderModule>& shaders)
{
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> references;
    for (VkFormat format : formats)
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
        references.push_back({ static_cast<uint32_t>(attachments.size()), VK_IMAGE_LAYOUT_GENERAL });
        attachments.push_back(attachment);
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(references.size());
    subpass.pColorAttachments = references.data();
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    passInfo.pAttachments = attachments.data();
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    Check(vkCreateRenderPass(gpu.device, &passInfo, nullptr, &pass.renderPass), "create a render pass");

    // Set 0 holds the constants and set 1 the textures the pixel shader reads, as ReShade's compiler lays them out.
    std::vector<VkDescriptorSetLayoutBinding> bindings[2];
    bindings[0].push_back({ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr });
    for (const reshadefx::texture_binding& binding : described.texture_bindings)
    {
        bindings[1].push_back({ binding.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr });
        pass.names.push_back({ binding.entry_point_binding, module.samplers[binding.index].name });
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
    // The same for every target, so devices without independent blending take it too.
    const std::vector<VkPipelineColorBlendAttachmentState> blendings(formats.size(), blending);
    VkPipelineColorBlendStateCreateInfo blendState{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blendState.attachmentCount = static_cast<uint32_t>(blendings.size());
    blendState.pAttachments = blendings.data();
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

    const VkDescriptorSetLayout layouts[] = { pass.setLayouts[0], pass.setLayouts[1], pass.setLayouts[1] };
    VkDescriptorSet sets[3]{};
    VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocation.descriptorPool = m.pool;
    allocation.descriptorSetCount = 3;
    allocation.pSetLayouts = layouts;
    Check(vkAllocateDescriptorSets(gpu.device, &allocation, sets), "allocate descriptors");
    pass.constants = sets[0];
    pass.textures[0] = sets[1];
    pass.textures[1] = sets[2];
    const VkDescriptorBufferInfo buffer{ m.constants.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = pass.constants;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
}

void Create(VkCommandBuffer commands, VkFormat format)
{
    // Formats Vulkan requires anyway. MoltenVK and drivers that leave something out stop here rather than fail later.
    const VkFormatFeatureFlags drawn = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!gpu.Supports(VK_FORMAT_R16G16B16A16_SFLOAT, drawn | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !gpu.Supports(VK_FORMAT_R8_UNORM, drawn) || !gpu.Supports(VK_FORMAT_R8G8B8A8_UNORM, drawn))
        throw std::runtime_error("the graphics card cannot draw into the images it needs");

    reshadefx::preprocessor preprocessor;
    preprocessor.append_string(kSource, "uimask.fx");
    // The same SPIR-V as for effects, with Direct3D's clip space.
    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(true, false, false, false, true));
    reshadefx::parser parser;
    if (!preprocessor.errors().empty() || !parser.parse(preprocessor.output(), codegen.get()))
        throw std::runtime_error("its shader did not compile: " + preprocessor.errors() + parser.errors());
    const reshadefx::effect_module& module = codegen->module();
    if (module.techniques.size() != 1 || module.techniques[0].passes.size() != 9)
        throw std::runtime_error("its shader has no technique");
    const std::vector<reshadefx::pass>& passes = module.techniques[0].passes;
    const char* const names[] = { "Rects", "Params", "Size", "Detect", "Lods" };
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

        // Each pass has its constants and two sets of textures, at most 5 in a set.
        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 9 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 48 } };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 27;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &m.pool), "create a descriptor pool");
        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(gpu.device, &samplerInfo, nullptr, &m.sampler), "create a sampler");
        if (!gpu.CreateBuffer(m.constants, sizeof(Constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true) ||
            !gpu.CreateBuffer(m.upload, kRememberedWidth * kRememberedHeight, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
            throw std::runtime_error("could not create a buffer");
        if (!gpu.CreateImage(m.rememberedImage, kRememberedWidth, kRememberedHeight, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("the graphics card has no room for its images");
        InitLayout(commands, m.rememberedImage);

        const VkFormat history = VK_FORMAT_R16G16B16A16_SFLOAT;
        CreatePass(m.compose, module, passes[0], { VK_FORMAT_R8_UNORM }, false, false, shaders);
        CreatePass(m.seed, module, passes[1], { history }, false, false, shaders);
        CreatePass(m.fill, module, passes[2], { format }, true, false, shaders);
        CreatePass(m.restore, module, passes[3], { format }, true, true, shaders);
        CreatePass(m.observe, module, passes[4], { history, history }, false, false, shaders);
        CreatePass(m.update, module, passes[5], { history, history }, false, false, shaders);
        CreatePass(m.shape, module, passes[6], { VK_FORMAT_R8_UNORM }, false, false, shaders);
        CreatePass(m.holes, module, passes[7], { VK_FORMAT_R8_UNORM }, false, false, shaders);
        CreatePass(m.view, module, passes[8], { VK_FORMAT_R8G8B8A8_UNORM }, false, false, shaders);
    }
    catch (...)
    {
        destroyShaders();
        throw;
    }
    destroyShaders();
    m.made = true;
    m.reseed = true;
}

std::vector<Pass*> Passes()
{
    return { &m.compose, &m.seed, &m.fill, &m.restore, &m.observe, &m.update, &m.shape, &m.holes, &m.view };
}

void DestroyTargets()
{
    for (Pass* pass : Passes())
    {
        // The passes without turns have the same one twice.
        if (pass->framebuffers[1] && pass->framebuffers[1] != pass->framebuffers[0])
            vkDestroyFramebuffer(gpu.device, pass->framebuffers[1], nullptr);
        if (pass->framebuffers[0])
            vkDestroyFramebuffer(gpu.device, pass->framebuffers[0], nullptr);
        pass->framebuffers[0] = pass->framebuffers[1] = VK_NULL_HANDLE;
    }
    for (GpuImage* image : { &m.covered, &m.original, &m.pyramid, &m.observed, &m.activity, &m.low[0], &m.low[1], &m.high[0], &m.high[1], &m.shaped,
                             &m.detected, &m.tint })
        gpu.DestroyImage(*image);
    m.picture = VK_NULL_HANDLE;
}

VkFramebuffer CreateFramebuffer(const Pass& pass, std::initializer_list<VkImageView> views, uint32_t width, uint32_t height)
{
    VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    info.renderPass = pass.renderPass;
    info.attachmentCount = static_cast<uint32_t>(views.size());
    info.pAttachments = views.begin();
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    Check(vkCreateFramebuffer(gpu.device, &info, nullptr, &framebuffer), "create a framebuffer");
    return framebuffer;
}

// Points a pass's textures for a turn at the images it reads, by the names its shader gives them.
void Bind(const Pass& pass, uint32_t turn, const std::map<std::string, VkImageView>& views)
{
    for (const auto& [binding, name] : pass.names)
    {
        const VkDescriptorImageInfo image{ m.sampler, views.at(name), VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pass.textures[turn];
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    }
}

// Compose reads detection's images, or while there are none, the remembered mask in their place, which it then
// leaves alone.
void BindCompose(const GpuImage& picture)
{
    const VkImageView observed = m.observed.image ? m.observed.view : m.rememberedImage.view;
    const VkImageView detected = m.detected.image ? m.detected.view : m.rememberedImage.view;
    Bind(m.compose, 0, { { "Picture", picture.view }, { "Observed", observed }, { "Detected", detected } });
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
    m.compose.framebuffers[0] = m.compose.framebuffers[1] = CreateFramebuffer(m.compose, { m.covered.target }, picture.width, picture.height);
    m.seed.framebuffers[0] = m.seed.framebuffers[1] = CreateFramebuffer(m.seed, { m.pyramid.target }, halfWidth, halfHeight);
    m.fill.framebuffers[0] = m.fill.framebuffers[1] = CreateFramebuffer(m.fill, { picture.target }, picture.width, picture.height);
    m.restore.framebuffers[0] = m.restore.framebuffers[1] = CreateFramebuffer(m.restore, { picture.target }, picture.width, picture.height);
    BindCompose(picture);
    Bind(m.seed, 0, { { "Picture", picture.view }, { "Mask", m.covered.view } });
    Bind(m.fill, 0, { { "Mask", m.covered.view }, { "Pyramid", m.pyramid.view } });
    Bind(m.restore, 0, { { "Picture", m.original.view }, { "Mask", m.covered.view } });
    m.picture = picture.image;
    m.changed = true;
}

// Detection's images, made the first time it runs on a picture. The graphics card is let finish first, since Compose
// is pointed at them and earlier frames drew with it.
void MakeDetector(VkCommandBuffer commands, const GpuImage& picture)
{
    vkDeviceWaitIdle(gpu.device);
    const uint32_t width = (picture.width + 1) / 2, height = (picture.height + 1) / 2;
    const uint32_t levels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    const VkFormat history = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags drawn = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool made = gpu.CreateImage(m.observed, width, height, 1, history, drawn) &&
                gpu.CreateImage(m.activity, width, height, levels, history, drawn | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
                gpu.CreateImage(m.shaped, width, height, 1, VK_FORMAT_R8_UNORM, drawn) &&
                gpu.CreateImage(m.detected, width, height, 1, VK_FORMAT_R8_UNORM, drawn);
    for (uint32_t turn = 0; turn < 2 && made; ++turn)
        made = gpu.CreateImage(m.low[turn], width, height, 1, history, drawn | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
               gpu.CreateImage(m.high[turn], width, height, 1, history, drawn);
    if (!made)
        throw std::runtime_error("the graphics card has no room for detecting the interface");
    for (const GpuImage* image : { &m.observed, &m.activity, &m.shaped, &m.detected, &m.low[0], &m.low[1], &m.high[0], &m.high[1] })
        InitLayout(commands, *image);
    m.observe.framebuffers[0] = m.observe.framebuffers[1] = CreateFramebuffer(m.observe, { m.observed.target, m.activity.target }, width, height);
    m.shape.framebuffers[0] = m.shape.framebuffers[1] = CreateFramebuffer(m.shape, { m.shaped.target }, width, height);
    m.holes.framebuffers[0] = m.holes.framebuffers[1] = CreateFramebuffer(m.holes, { m.detected.target }, width, height);
    for (uint32_t turn = 0; turn < 2; ++turn)
    {
        // A turn reads the history in its images and writes the other's.
        const uint32_t next = 1 - turn;
        m.update.framebuffers[turn] = CreateFramebuffer(m.update, { m.low[next].target, m.high[next].target }, width, height);
        Bind(m.observe, turn, { { "Picture", picture.view }, { "Low", m.low[turn].view }, { "High", m.high[turn].view } });
        Bind(m.update, turn,
             { { "Observed", m.observed.view }, { "Low", m.low[turn].view }, { "High", m.high[turn].view }, { "Activity", m.activity.view },
               { "Remembered", m.rememberedImage.view } });
        Bind(m.shape, turn, { { "Low", m.low[next].view } });
    }
    Bind(m.holes, 0, { { "Shape", m.shaped.view } });
    BindCompose(picture);
    m.turn = 0;
    m.fresh = true;
    m.pictures = 0;
    m.changed = true;
}

// The mask tinted, made the first time it is shown.
void MakeTint(VkCommandBuffer commands, const GpuImage& picture)
{
    const uint32_t width = (picture.width + 1) / 2, height = (picture.height + 1) / 2;
    if (!gpu.CreateImage(m.tint, width, height, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        throw std::runtime_error("the graphics card has no room for showing the mask");
    InitLayout(commands, m.tint);
    m.view.framebuffers[0] = m.view.framebuffers[1] = CreateFramebuffer(m.view, { m.tint.target }, width, height);
    Bind(m.view, 0, { { "Mask", m.covered.view } });
    ++m.viewMade;
}

// Puts the remembered mask where Update starts the history from.
void Upload(VkCommandBuffer commands)
{
    uint8_t* mapped = static_cast<uint8_t*>(m.upload.mapped);
    if (m.remembered.size() == size_t(kRememberedWidth) * kRememberedHeight)
        std::memcpy(mapped, m.remembered.data(), m.remembered.size());
    else
        std::memset(mapped, 0, size_t(kRememberedWidth) * kRememberedHeight);
    FullBarrier(commands);
    VkBufferImageCopy copy{};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { kRememberedWidth, kRememberedHeight, 1 };
    vkCmdCopyBufferToImage(commands, m.upload.buffer, m.rememberedImage.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    FullBarrier(commands);
    m.reseed = false;
    m.fresh = true;
    m.pictures = 0;
}

void Draw(VkCommandBuffer commands, const Pass& pass, uint32_t turn, VkExtent2D extent, VkRect2D area)
{
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = pass.renderPass;
    begin.framebuffer = pass.framebuffers[turn];
    begin.renderArea = area;
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
    const VkDescriptorSet sets[] = { pass.constants, pass.textures[turn] };
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layout, 0, 2, sets, 0, nullptr);
    const VkViewport viewport{ 0, 0, float(extent.width), float(extent.height), 0, 1 };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &area);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    FullBarrier(commands);
}

void Draw(VkCommandBuffer commands, const Pass& pass, uint32_t turn, VkExtent2D extent)
{
    Draw(commands, pass, turn, extent, { { 0, 0 }, extent });
}

// Writes what the passes read for the rectangles and detection as they are now, and notes the part of the picture the
// mask can touch.
void WriteConstants(const GpuImage& picture)
{
    const float width = float(picture.width), height = float(picture.height);
    const float feather = std::max(2.0f, std::round(height * kFeather));
    Constants constants{};
    float left = width, top = height, right = 0, bottom = 0;
    size_t count[2]{};
    // The interface ones first.
    for (int kind = 0; kind < 2; ++kind)
        for (const uimask::Rect& rect : m.rects)
        {
            if ((rect[4] != 0) != (kind == 1))
                continue;
            float* out = constants.rects[count[0] + count[1]];
            out[0] = std::round(rect[0] * width);
            out[1] = std::round(rect[1] * height);
            out[2] = std::round((rect[0] + rect[2]) * width);
            out[3] = std::round((rect[1] + rect[3]) * height);
            ++count[kind];
            if (kind == 1)
                continue;
            left = std::min(left, out[0]);
            top = std::min(top, out[1]);
            right = std::max(right, out[2]);
            bottom = std::max(bottom, out[3]);
        }
    constants.params[0] = float(count[0]);
    constants.params[1] = float(count[1]);
    constants.params[2] = feather;
    constants.params[3] = float(m.pyramid.levels);
    constants.size[0] = width;
    constants.size[1] = height;
    constants.size[2] = float((picture.width + 1) / 2);
    constants.size[3] = float((picture.height + 1) / 2);
    constants.detect[0] = m.detecting ? 1.0f : 0.0f;
    constants.detect[1] = m.fresh ? 1.0f : 0.0f;
    constants.detect[2] = m.hdrScale;
    if (m.detecting && m.activity.levels > 1)
    {
        // A neighbourhood is a sixteenth of the picture's height, 64 pixels at 1080, and edges count within a quarter of
        // that, whatever the picture's size.
        const float last = float(m.activity.levels - 1), rows = constants.size[3];
        constants.detect[3] = last;
        constants.lods[0] = std::clamp(std::round(std::log2(rows / 16)), 1.0f, last);
        constants.lods[1] = std::min(constants.lods[0] + 1, last);
        constants.lods[2] = std::clamp(std::round(std::log2(rows / 64)), 0.0f, last);
    }
    uint8_t* mapped = static_cast<uint8_t*>(m.constants.mapped);
    std::memcpy(mapped + m.offsets[0], constants.rects, sizeof(constants.rects));
    std::memcpy(mapped + m.offsets[1], constants.params, sizeof(constants.params));
    std::memcpy(mapped + m.offsets[2], constants.size, sizeof(constants.size));
    std::memcpy(mapped + m.offsets[3], constants.detect, sizeof(constants.detect));
    std::memcpy(mapped + m.offsets[4], constants.lods, sizeof(constants.lods));

    if (m.detecting)
    {
        m.area = { { 0, 0 }, { picture.width, picture.height } };
        return;
    }
    const int32_t x0 = std::clamp(int32_t(left - feather), 0, int32_t(picture.width)), y0 = std::clamp(int32_t(top - feather), 0, int32_t(picture.height));
    const int32_t x1 = std::clamp(int32_t(right + feather), x0, int32_t(picture.width)), y1 = std::clamp(int32_t(bottom + feather), y0, int32_t(picture.height));
    m.area = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
}

// Detection's passes for a new picture, ending in the half-size mask Compose upsamples.
void Learn(VkCommandBuffer commands)
{
    const VkExtent2D half{ m.observed.width, m.observed.height };
    Draw(commands, m.observe, m.turn, half);
    for (uint32_t level = 1; level < m.activity.levels; ++level)
    {
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
        blit.srcOffsets[1] = { std::max(1, int(half.width >> (level - 1))), std::max(1, int(half.height >> (level - 1))), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        blit.dstOffsets[1] = { std::max(1, int(half.width >> level)), std::max(1, int(half.height >> level)), 1 };
        vkCmdBlitImage(commands, m.activity.image, VK_IMAGE_LAYOUT_GENERAL, m.activity.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        FullBarrier(commands);
    }
    Draw(commands, m.update, m.turn, half);
    Draw(commands, m.shape, m.turn, half);
    Draw(commands, m.holes, 0, half);
    m.turn = 1 - m.turn;
}

float HalfToFloat(uint16_t bits)
{
    const uint32_t exponent = (bits >> 10) & 0x1F, mantissa = bits & 0x3FF;
    const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent < 31 ? std::ldexp(float(mantissa | 0x400), int(exponent) - 25) : 0.0f;
    return (bits & 0x8000) ? -value : value;
}

// Reads what detection is sure of into remembered, once it learned from enough pictures to be worth more than what was
// there. Waits for the graphics card.
bool Remember()
{
    const GpuImage& history = m.low[m.turn];
    if (!history.image || m.pictures < kSettled || gpu.lost)
        return false;
    GpuBuffer readback;
    if (!gpu.CreateBuffer(readback, VkDeviceSize(history.width) * history.height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
        return false;
    VkCommandBuffer commands = gpu.BeginCommands();
    bool read = false;
    if (commands)
    {
        FullBarrier(commands);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { history.width, history.height, 1 };
        vkCmdCopyImageToBuffer(commands, history.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
        VkMemoryBarrier toHost{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
        read = gpu.SubmitAndWait(commands);
    }
    if (read)
    {
        gpu.Invalidate(readback);
        // Certainty is the alpha of each RGBA half-float pixel.
        const uint16_t* halves = static_cast<const uint16_t*>(readback.mapped);
        std::vector<uint8_t> kept(size_t(kRememberedWidth) * kRememberedHeight);
        for (uint32_t y = 0; y < kRememberedHeight; ++y)
        {
            const uint32_t y0 = y * history.height / kRememberedHeight, y1 = std::max(y0 + 1, (y + 1) * history.height / kRememberedHeight);
            for (uint32_t x = 0; x < kRememberedWidth; ++x)
            {
                const uint32_t x0 = x * history.width / kRememberedWidth, x1 = std::max(x0 + 1, (x + 1) * history.width / kRememberedWidth);
                float most = 0;
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx)
                        most = std::max(most, HalfToFloat(halves[(size_t(sy) * history.width + sx) * 4 + 3]));
                kept[size_t(y) * kRememberedWidth + x] = static_cast<uint8_t>(std::lround(std::clamp(most, 0.0f, 1.0f) * 255));
            }
        }
        m.remembered = std::move(kept);
    }
    gpu.DestroyBuffer(readback);
    return read;
}

// A grey PNG, with the game's other files. A folder that went, as when the game was renamed or removed, is not made
// again.
void Save()
{
    std::error_code error;
    if (m.rememberedPath.empty() || m.remembered.empty() || !fs::is_directory(m.rememberedPath.parent_path(), error))
        return;
    std::string png;
    const auto append = [](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), size_t(size)); };
    if (!stbi_write_png_to_func(append, &png, int(kRememberedWidth), int(kRememberedHeight), 1, m.remembered.data(), int(kRememberedWidth)) ||
        !WriteFile(m.rememberedPath, png))
        Log(LogLevel::Warning, "Could not write %s.", m.rememberedPath.string().c_str());
}

void Load()
{
    m.remembered.clear();
    if (m.rememberedPath.empty())
        return;
    const image::Pixels pixels = image::Read(m.rememberedPath);
    if (!pixels.data || pixels.width != int(kRememberedWidth) || pixels.height != int(kRememberedHeight))
        return;
    // Grey, read as RGBA.
    const uint8_t* rgba = static_cast<const uint8_t*>(pixels.data.get());
    m.remembered.resize(size_t(kRememberedWidth) * kRememberedHeight);
    for (size_t i = 0; i < m.remembered.size(); ++i)
        m.remembered[i] = rgba[i * 4];
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

void Detect(bool on, const std::filesystem::path& remembered)
{
    if (on == m.detecting && remembered == m.rememberedPath)
        return;
    // What it learned stays with the game it learned it from.
    if (m.detecting && Remember())
        Save();
    m.detecting = on;
    m.rememberedPath = remembered;
    if (on)
        Load();
    // The history is another game's until it starts over, so none of it is kept for this one.
    m.pictures = 0;
    m.reseed = true;
    m.changed = true;
}

void SetPicture(uint64_t serial)
{
    m.serial = serial;
}

bool Fill(VkCommandBuffer commands, const GpuImage& picture, bool fill, float sdrWhite)
{
    m.filled = false;
    if (m.failed || (!m.detecting && !Marked()) || !picture.image)
        return false;
    try
    {
        if (!m.made)
            Create(commands, picture.format);
        if (m.picture != picture.image || m.covered.width != picture.width || m.covered.height != picture.height)
            Resize(commands, picture);
        if (m.detecting && !m.observed.image)
            MakeDetector(commands, picture);
        if (m.shown && !m.tint.image)
            MakeTint(commands, picture);
        if (m.reseed)
            Upload(commands);
        const float hdrScale = picture.format == VK_FORMAT_R16G16B16A16_SFLOAT && sdrWhite > 0 ? 1 / sdrWhite : 0;
        // Each new picture once. Frames drawn again for the menu, or a picture drawn twice, teach nothing.
        const bool learn = m.detecting && (m.fresh || m.serial != m.learned);
        if (learn || m.changed || hdrScale != m.hdrScale)
        {
            m.hdrScale = hdrScale;
            WriteConstants(picture);
        }
        const VkExtent2D extent{ picture.width, picture.height };
        if (learn)
        {
            Learn(commands);
            m.learned = m.serial;
            m.fresh = false;
            if (++m.pictures == kSettled && Remember())
                Save();
        }
        if (learn || m.changed)
        {
            Draw(commands, m.compose, 0, extent);
            m.changed = false;
        }
        if (m.shown)
            Draw(commands, m.view, 0, { m.tint.width, m.tint.height });
        if (!fill || !m.area.extent.width || !m.area.extent.height)
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
        Draw(commands, m.seed, 0, half);
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
        Draw(commands, m.fill, 0, extent, m.area);
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
    Draw(commands, m.restore, 0, { picture.width, picture.height }, m.area);
}

void Show(bool shown)
{
    m.shown = shown;
}

const GpuImage* View(uint32_t& made)
{
    made = m.viewMade;
    return m.shown && !m.failed && m.tint.image && (m.detecting || Marked()) ? &m.tint : nullptr;
}

void Release()
{
    if (!gpu.device)
        return;
    // What detection learned outlives its images, which a new size or format makes again, and Unishade quitting.
    if (m.detecting && Remember())
        Save();
    DestroyTargets();
    for (Pass* pass : Passes())
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
    gpu.DestroyBuffer(m.upload);
    gpu.DestroyImage(m.rememberedImage);
    m.sampler = VK_NULL_HANDLE;
    m.pool = VK_NULL_HANDLE;
    m.made = false;
    m.filled = false;
    m.reseed = true;
}
} // namespace uimask
