#pragma once

#include "gpu.h"

// AMD's FSR 1, as on the ReShade host: for effects below 100%, EASU upscales the picture to the window's size and RCAS
// sharpens it. ReShade's effect compiler builds it the first time it runs. Windows only, as the effect resolution is.

// Records the upscale of input, in VK_IMAGE_LAYOUT_GENERAL, into target, in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL and
// extent big, when effects run below 100% and target is larger both ways. Returns false and records nothing otherwise,
// or once FSR failed, and the caller stretches the picture as before.
bool Upscale(VkCommandBuffer commands, int effectResolution, const GpuImage& input, VkImage target, VkExtent2D extent);
// Call before the graphics card's device goes.
void ReleaseUpscaler();
