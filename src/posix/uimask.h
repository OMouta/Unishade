#pragma once

#include "gpu.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

// Keeps the game's own interface out of effects. The capture has it drawn into the picture already, so before effects
// run, the parts marked as interface are filled in from the scene around them, and afterwards the game's own pixels go
// back over the result. Bloom then has no bright health bar to glow from, and the health bar comes out as the game drew
// it. What is interface is a mask: areas marked by hand, and with detection on, what stays still while the picture
// around it moves. ReShade's effect compiler builds the shaders the first time something is marked or detected.
namespace uimask
{
// A marked part of the picture: its left, top, width and height, in fractions of the picture's, then 1 for scene that
// detection must leave out, or 0 for interface.
using Rect = std::array<float, 5>;

constexpr size_t kMostRects = 16;

void Set(const std::vector<Rect>& rects);
// Turns detection on or off. remembered is the file the game's mask is kept in, which detection starts from, so a still
// menu is covered from the first frame, or empty to keep none.
void Detect(bool on, const std::filesystem::path& remembered);
// The serial of the game's picture the next Fill gets. Detection learns from each picture once, however often it is
// drawn.
void SetPicture(uint64_t serial);
// Records the fill of picture, in VK_IMAGE_LAYOUT_GENERAL, keeping what was there for Restore, after detection learned
// from it. With fill false, as while effects are off, only detection runs. sdrWhite is SDR white in scRGB's units for an
// HDR picture, or 0. Returns false and records no fill when nothing is marked, or once it failed.
bool Fill(VkCommandBuffer commands, const GpuImage& picture, bool fill, float sdrWhite);
// Records putting back what Fill kept, after the effects ran on picture.
void Restore(VkCommandBuffer commands, const GpuImage& picture);
// Whether Fill also draws the mask tinted, for drawing over the game.
void Show(bool shown);
// That tint at half the picture's size, in VK_IMAGE_LAYOUT_GENERAL, or null while there is none. made goes up whenever
// the image is made again.
const GpuImage* View(uint32_t& made);
// Call before the picture's image or the graphics card's device goes, with the graphics card idle.
void Release();
} // namespace uimask
