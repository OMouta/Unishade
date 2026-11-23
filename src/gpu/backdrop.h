#pragma once

#include "gpu/gpu.h"

// The game's picture as the interface shows it besides drawing over it: blurred behind the menu's tray, as the design's
// backdrop-filter, and shrunk for the launcher's page and a screenshot's thumbnail. ReShade's effect compiler builds its
// shaders the first time they are needed. Everything records into the overlay's commands, and images stay in
// VK_IMAGE_LAYOUT_GENERAL, where Dear ImGui reads them.
namespace backdrop
{
// An image the passes draw into.
struct Target
{
    GpuImage image;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

// Makes target width by height, or keeps it when it is that size already. The old one goes, so the graphics card must
// be done with it. Returns false when it could not be made, or the shaders did not build.
bool Make(VkCommandBuffer commands, Target& target, uint32_t width, uint32_t height);
void Destroy(Target& target);
// Starts a frame's passes, after whatever read the targets before. A frame records a few at most.
void Begin(VkCommandBuffer commands);
// input, in VK_IMAGE_LAYOUT_GENERAL, shrunk into target, each pixel the average of up to 4 by 4 of input's. An scRGB
// input, R16G16B16A16_SFLOAT, is turned into SDR with SDR white at sdrWhite, in scRGB's units.
bool Shrink(VkCommandBuffer commands, const GpuImage& input, const Target& target, float sdrWhite);
// input, an scRGB picture in VK_IMAGE_LAYOUT_GENERAL, turned into SDR in target of the same size, pixel for pixel, as
// Shrink turns it. For add-ons' commands, outside the frame's passes: slot 0 or 1, each for one set of commands that may
// still run on the graphics card while the other records.
bool ToSdr(VkCommandBuffer commands, const GpuImage& input, const Target& target, float sdrWhite, int slot);
// Draws menu, which Dear ImGui drew in SDR with premultiplied alpha, over an scRGB picture with SDR white at sdrWhite, in
// the R16G16B16A16_SFLOAT render pass that has begun, such as Surface::BeginComposite's.
bool DrawMenu(VkCommandBuffer commands, const GpuImage& menu, float sdrWhite);
// Blurs target as CSS's blur(radius), with radius in target's pixels, through scratch of the same size, then saturates
// it as CSS's saturate(saturation).
bool Blur(VkCommandBuffer commands, const Target& target, const Target& scratch, float radius, float saturation);
// Call before the graphics card's device goes, once the targets are destroyed.
void Release();
} // namespace backdrop
