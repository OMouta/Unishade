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
constexpr unsigned kKeyCount = 258;
// FitStep in the shaders.
constexpr unsigned kFitStep = 4;
// Each estimate's fit to the history moves back towards no scale and shift over about this long, so that depth as a
// whole follows the anchor over seconds but does not jump when an estimate lands.
constexpr float kFitSeconds = 1.0f;

inline float FitRelax(float seconds)
{
    return 1 - std::exp(-std::max(seconds, 0.0f) / kFitSeconds);
}

// A trusted history takes each estimate in over about this long, and keeps the rest of itself, so one estimate's
// flicker does not show whole. In seconds rather than estimates, since Detail and the graphics card make estimates
// land from every frame to a few times a second. An estimate that lands is the goal the history moves towards in every
// frame until the next one lands, by the share of this the frame took: taken in at once, Ultra's estimate every fifth
// frame moved depth by 1.5% to 2.6% in that frame against 0.08% in the frames between.
constexpr float kHistorySeconds = 0.3f;
// A picture that stopped changing is estimated again for this long, so the history settles on it rather than keeping
// what the estimates during the motion left, such as where something moved away.
constexpr auto kSettleTime = std::chrono::seconds(1);
// After an estimate lands, the history moves towards it for this long while the picture stands still, which leaves
// a 1500th of the way.
constexpr auto kApproachTime = std::chrono::milliseconds(2200);

inline float HistoryShare(float seconds)
{
    return 1 - std::exp(-std::max(seconds, 0.0f) / kHistorySeconds);
}

// How many whole pixels across and down the index-th estimate samples the frame from where the first did. The model's
// picture is smaller than the frame, so its output depends on where its pixels fall on the frame's. In a sideways pan
// every estimate fell at a new place across but the same place down, and blending them into the history averaged
// away what that changed across and kept it down, which the normals showed as short horizontal dashes. The offsets
// step through a model pixel evenly in both directions, and each estimate is read back with its own.
struct Offset
{
    int x = 0;
    int y = 0;
};

inline Offset EstimateOffset(unsigned index, uint32_t frameWidth, uint32_t frameHeight, int modelWidth, int modelHeight)
{
    const auto halton = [](unsigned i, unsigned base) {
        float fraction = 1, result = 0;
        for (; i; i /= base)
        {
            fraction /= base;
            result += fraction * (i % base);
        }
        return result;
    };
    return { static_cast<int>(halton(index, 2) * std::ceil(static_cast<float>(frameWidth) / modelWidth)),
             static_cast<int>(halton(index, 3) * std::ceil(static_cast<float>(frameHeight) / modelHeight)) };
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
// The motion estimator tells a cut to another scene from the picture's brightness: each of 3 by 3 sections counts its
// pixels by brightness, in this many levels, and a cut is when at least CutSections of them have CutShare of their
// pixels at another level than in the picture before. A score or a map that changes only reaches the sections it is
// in. A cut drops the history of depth.
CONSTANT uint SectionLevels = 16;
CONSTANT uint CutSections = 7;
CONSTANT float CutShare = 0.3;
CONSTANT float3 Luma = float3(0.299, 0.587, 0.114);
// Where each count is in the keys, which are unsigned numbers: the highest and the lowest output value, and the
// output's values.
CONSTANT uint KeyHighest = 0;
CONSTANT uint KeyLowest = 1;
CONSTANT uint KeyBins = 2;
// Unishade's RESHADE_DEPTH_LINEARIZATION_FAR_PLANE.
CONSTANT float FarPlane = 1000.0;
// The model only knows how far things are relative to each other. What is at the anchor is put at this fraction of the
// far plane, and anything more than 1 / Nearest times farther than it on the far plane.
CONSTANT float Nearest = 0.01;
// The four output values around a point are interpolated between unless they differ by more than this share of the
// nearest one's closeness, which is a depth edge.
CONSTANT float UpsampleSpread = 0.3;
// At a depth edge, how far apart the colors of its two sides must be for a pixel's color to say which side it is on.
// Closer than this, as on a dark picture or where the sides are alike, grain and texture would decide it, so the
// sides are interpolated between as on a surface, in proportion to how close they are.
CONSTANT float SideContrast = 0.4;

// The output's value at a point between four of its values, a to d in the order of the four corners from the top left
// in rows, from the colors they were estimated from and the color of the pixel the point is in. On a surface the four
// are interpolated between, and color plays no part, since a texture's colors would ripple the depth. At a depth edge
// they split into the near and the far side, each interpolated over its own values and colors, and the point takes
// each side by where its pixel's color lies between the sides' colors, so that the edge follows the picture's. Taking
// one side outright makes every pixel whose color is nearer the wrong side a speck of the other surface, and on a dark
// picture, where all colors are near, specks that follow the grain and change with every estimate.
float Upsample(float4 values, float3 a, float3 b, float3 c, float3 d, float3 color, float2 fraction)
{
    float4 weights = float4((1.0f - fraction.x) * (1.0f - fraction.y), fraction.x * (1.0f - fraction.y), (1.0f - fraction.x) * fraction.y,
                            fraction.x * fraction.y);
    float low = min(min(values.x, values.y), min(values.z, values.w));
    float high = max(max(values.x, values.y), max(values.z, values.w));
    if (high - low <= UpsampleSpread * high)
        return dot(weights, values);
    float middle = (low + high) * 0.5f;
    float4 nearSide = step(float4(middle, middle, middle, middle), values);
    // A point on the far side of its four can have no weight left on a side, so each value keeps a little.
    float4 nearWeights = (weights + 1e-3f) * nearSide;
    float4 farWeights = (weights + 1e-3f) * (1.0f - nearSide);
    float nearTotal = dot(nearWeights, float4(1.0f, 1.0f, 1.0f, 1.0f));
    float farTotal = dot(farWeights, float4(1.0f, 1.0f, 1.0f, 1.0f));
    float3 nearColor = (nearWeights.x * a + nearWeights.y * b + nearWeights.z * c + nearWeights.w * d) / nearTotal;
    float3 farColor = (farWeights.x * a + farWeights.y * b + farWeights.z * c + farWeights.w * d) / farTotal;
    float3 across = nearColor - farColor;
    float apart = dot(across, across);
    float byColor = saturate(dot(color - farColor, across) / max(apart, 1e-8f));
    float byPlace = nearTotal / (nearTotal + farTotal);
    float nearShare = byPlace + (byColor - byPlace) * saturate(sqrt(apart) / SideContrast);
    float nearValue = dot(nearWeights, values) / nearTotal;
    float farValue = dot(farWeights, values) / farTotal;
    return farValue + (nearValue - farValue) * nearShare;
}

// Depth follows the picture's motion between estimates: a history of closeness, at the picture's size, is moved along
// it in every frame and each estimate is blended into it. A pixel's history is trusted while every vector that moved
// it had at least this confidence since an estimate was blended in. The estimator's confidence falls where the color a
// vector brings does not match, such as where something came into view, and is 0 after a cut.
CONSTANT float TrustedConfidence = 0.3;
// Each pixel's flags in the history: whether it is trusted, and whether the frame the next estimate is made from saw
// it, which is when every vector since that frame was trusted too.
CONSTANT uint Trusted = 1;
CONSTANT uint SeenByEstimate = 2;
// The history is read between pixels, except where the four it lies between differ by more than this share of the
// nearest one's closeness. There it takes the nearest, so an edge stays sharp however often it moves.
CONSTANT float EdgeSpread = 0.1;
// Each estimate is aligned to the history by a scale and a shift fitted over every FitStep-th pixel of the picture both
// see, unless they are fewer than FitMinimum of them, as after a cut.
CONSTANT uint FitStep = 4;
CONSTANT float FitMinimum = 0.05;

// Whether the history is read between the pixels whose closeness these are.
bool Reproject(float4 taps)
{
    float low = min(min(taps.x, taps.y), min(taps.z, taps.w));
    float high = max(max(taps.x, taps.y), max(taps.z, taps.w));
    return high - low <= EdgeSpread * high;
}

// The scale and shift in x and y that bring an estimate to the history, from the sums over the pixels both see of 1,
// the history's closeness h, the estimate's e, h * h in first, and h * e in crossed, each closeness less 1. The
// estimate is the noisier of the two, so it is fitted to the history rather than the other way round, which would
// shrink the scale by its noise. The fit moves back towards none by relax, so that over many estimates depth follows
// the anchor and does not wander off from it.
float4 Align(float4 first, float crossed, uint samples, float relax)
{
    float count = first.x;
    if (count < FitMinimum * float(samples))
        return float4(1, 0, count, 0);
    float history = first.y / count;
    float estimate = first.z / count;
    float spread = first.w / count - history * history;
    float together = crossed / count - history * estimate;
    float slope = clamp(spread > 1e-8f ? together / spread : 1.0f, 0.5f, 2.0f);
    float offset = estimate - slope * history;
    float scale = 1 / slope;
    float shift = 1 - (1 + offset) / slope;
    return float4(scale + (1 - scale) * relax, shift * (1 - relax), count, 0);
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
