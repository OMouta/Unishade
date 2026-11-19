#pragma once

#include "gpu.h"

#include <array>
#include <vector>

// Keeps the game's own interface out of effects. The capture has it drawn into the picture already, so before effects
// run, the parts marked as interface are filled in from the scene around them, and afterwards the game's own pixels go
// back over the result. Bloom then has no bright health bar to glow from, and the health bar comes out as the game drew
// it. ReShade's effect compiler builds the shaders the first time something is marked.
namespace uimask
{
// A part of the picture that is interface: its left, top, width and height, in fractions of the picture's.
using Rect = std::array<float, 4>;

constexpr size_t kMostRects = 16;

void Set(const std::vector<Rect>& rects);
// Records the fill of picture, in VK_IMAGE_LAYOUT_GENERAL, keeping what was there for Restore. Returns false and
// records nothing when nothing is marked, or once it failed.
bool Fill(VkCommandBuffer commands, const GpuImage& picture);
// Records putting back what Fill kept, after the effects ran on picture.
void Restore(VkCommandBuffer commands, const GpuImage& picture);
// Call before the picture's image or the graphics card's device goes.
void Release();
} // namespace uimask
