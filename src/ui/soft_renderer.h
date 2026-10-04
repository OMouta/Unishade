#pragma once

// Draws Dear ImGui on the CPU, for the launcher on Windows: ReShade puts its effects on every Direct3D window in the
// host, so the launcher stays away from the graphics card. Draws the way Dear ImGui's Direct3D backend does, with
// textures sampled linearly.

#include <imgui.h>

#include <cstdint>

namespace soft
{
// Tells Dear ImGui what this renderer supports. Call after creating the context.
void Init();
// Lets go of every texture, Dear ImGui's and the ones made with CreateTexture.
void Shutdown();
// Draws into pixels, width by height, top row first, as 0x00RRGGBB like Windows' 32-bit bitmaps.
void Render(ImDrawData* data, uint32_t* pixels, int width, int height);
// A texture from RGBA pixels, until DestroyTexture.
ImTextureID CreateTexture(const uint8_t* rgba, int width, int height);
void DestroyTexture(ImTextureID texture);
} // namespace soft
