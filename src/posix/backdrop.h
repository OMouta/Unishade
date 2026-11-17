#pragma once

#include "gpu.h"

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
// input, in VK_IMAGE_LAYOUT_GENERAL, shrunk into target, each pixel the average of up to 4 by 4 of input's.
bool Shrink(VkCommandBuffer commands, const GpuImage& input, const Target& target);
// Blurs target as CSS's blur(radius), with radius in target's pixels, through scratch of the same size, then saturates
// it as CSS's saturate(saturation).
bool Blur(VkCommandBuffer commands, const Target& target, const Target& scratch, float radius, float saturation);
// Call before the graphics card's device goes, once the targets are destroyed.
void Release();
} // namespace backdrop
