#pragma once

// What the Depth add-on does the same on every system: its settings, the size depth is estimated at, and the numbers
// and functions its shaders share. depth_addon.cpp is the add-on on Windows, depth_addon_macos.mm on macOS.

#include <unishade_addon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>

// A macro, so Windows can make a wide string of it.
#define DEPTH_MODEL_FILE "depth-anything-v2-small.onnx"

namespace depth
{
// The longest side of the picture depth is estimated from, for each choice of Detail, in the multiples of 14 Depth
// Anything V2 expects.
constexpr int kSizes[] = { 924, 518, 392, 266 };
// The shortest the other side may be: 16 of the model's patches, which Medium gives a 16:9 frame. With fewer than 14,
// as Low gave one, the model returns a blur that does not follow the picture, and with fewer than 16 the farthest
// things stop short of the far plane. A smaller picture is stretched to it.
constexpr int kShortestSide = 224;
constexpr auto kStatisticsInterval = std::chrono::seconds(30);
// How many unsigned numbers the keys hold, as the shaders lay them out.
constexpr unsigned kKeyCount = 546;
// The anchor the output is divided by follows each estimate's own over about this long, so depth does not pulse from
// one estimate to the next. A cut to another scene takes the new one at once.
constexpr float kAnchorSeconds = 0.15f;

// How far the smoothed anchor moves towards the anchor of an estimate that came seconds after the one before.
inline float AnchorBlend(float seconds)
{
    return 1 - std::exp(-std::max(seconds, 0.0f) / kAnchorSeconds);
}

inline const UnishadeSetting kSettings[] = {
    { "Detail", "Detail", UNISHADE_SETTING_CHOICE, { 1 }, 0, 3, "Ultra\0High\0Medium\0Low\0",
      "Depth is estimated from a smaller picture. Lower is faster, and effects that use depth lose fine detail." },
};

// The longest side of the model's input for the frame's Detail setting.
inline int LongestSide(const UnishadeFrame& frame)
{
    return kSizes[std::clamp(static_cast<int>(frame.settings[0][0]), 0, static_cast<int>(std::size(kSizes)) - 1)];
}

// The model's input size for a frame, which follows the frame's aspect ratio.
struct ModelSize
{
    int width = 0;
    int height = 0;
};

inline ModelSize ModelSizeFor(uint32_t frameWidth, uint32_t frameHeight, int longest)
{
    const bool landscape = frameWidth >= frameHeight;
    const float ratio = landscape ? static_cast<float>(frameHeight) / frameWidth : static_cast<float>(frameWidth) / frameHeight;
    const int shortSide = std::max(kShortestSide, static_cast<int>(std::lround(longest * ratio / 14)) * 14);
    return landscape ? ModelSize{ longest, shortSide } : ModelSize{ shortSide, longest };
}

// HLSL and the Metal shading language both take this once CONSTANT is defined: static const in HLSL, constant in Metal.
constexpr char kShaderShared[] = R"(
// ImageNet's mean and deviation, which the model's input is normalized with.
CONSTANT float3 Mean = float3(0.485, 0.456, 0.406);
CONSTANT float3 Std = float3(0.229, 0.224, 0.225);
// The model's output has no fixed scale, so it is divided by its value at the anchor: the value that this share of the
// picture is at or below. The few nearest things do not move it as they would move the highest value, so sparks, a
// weapon's tip or an icon in front of everything do not make the whole depth pulse. A smaller share follows how much
// of the picture is near: at 0.98, depth as a whole drifted three times as far while a picture panned.
CONSTANT float AnchorShare = 0.995;
// The anchor is found in a count of the output's values, in this many steps from the lowest to the highest.
CONSTANT uint AnchorBins = 256;
// A cut to another scene is told from the picture's brightness: each of 3 by 3 sections counts its pixels by
// brightness, in this many levels, and a cut is when at least CutSections of them have CutShare of their pixels at
// another level than in the estimate before. A score or a map that changes only reaches the sections it is in.
CONSTANT uint SectionLevels = 16;
CONSTANT uint CutSections = 7;
CONSTANT float CutShare = 0.3;
CONSTANT float3 Luma = float3(0.299, 0.587, 0.114);
// Where each count is in the keys, which are unsigned numbers: the highest and the lowest output value, the output's
// values, the sections' levels, and those of the estimate before.
CONSTANT uint KeyHighest = 0;
CONSTANT uint KeyLowest = 1;
CONSTANT uint KeyBins = 2;
CONSTANT uint KeySections = 258;
CONSTANT uint KeyPrevious = 402;
// Unishade's RESHADE_DEPTH_LINEARIZATION_FAR_PLANE.
CONSTANT float FarPlane = 1000.0;
// The model only knows how far things are relative to each other. What is at the anchor is put at this fraction of the
// far plane, and anything more than 1 / Nearest times farther than it on the far plane.
CONSTANT float Nearest = 0.01;
// How far, in model pixels and in color, an output value reaches when upsampled.
CONSTANT float SpatialSigma = 1.0;
CONSTANT float ColorSigma = 0.1;

// How much an output value counts in the joint bilateral upsample: by how far it is, in model pixels, and by how close
// the color it was estimated from is to this pixel's, so depth edges follow the frame's edges instead of blurring
// across them. Values that match no color still count a little, by distance alone.
float Weight(float2 offset, float3 difference)
{
    return exp(-dot(offset, offset) / (2 * SpatialSigma * SpatialSigma)) *
           (exp(-dot(difference, difference) / (2 * ColorSigma * ColorSigma)) + 1e-3f);
}

// DEPTH for an output value divided by the smoothed anchor. The output is inverse depth, so distance goes with its
// reciprocal, which also keeps flat surfaces flat. It is stored as the non-linear depth ReShade linearizes by default:
// linear = z / (far - z * (far - 1)), solved for z.
float StoredDepth(float closeness)
{
    float distance01 = Nearest / max(closeness, Nearest);
    return distance01 * FarPlane / (1 + distance01 * (FarPlane - 1));
}
)";
} // namespace depth
