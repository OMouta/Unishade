#pragma once

// motion_shared.h's estimator on Metal, for Objective-C++ with ARC. Its kernels are motion_shared.h's compute shaders
// in the Metal shading language, in the same order and with the same names.

#include "motion/motion_shared.h"

#import <Metal/Metal.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace motion
{
// They follow depth::kShaderShared and kShaderShared. Each pass gets its Sizes at buffer 0, SDR white at buffer 1 and
// the keys at buffer 2. The textures are at the indices kTexture names.
constexpr char kMetalShaders[] = R"(
struct Sizes
{
    uint2 Size;        // of the level the pass writes, or the picture's for Reduce and Finish
    uint2 PictureSize;
    float Scale;       // how many picture pixels one pixel of that level is wide, or of the finest level for Finish
    uint Coarsest;     // whether that level is the coarsest, with none above to start from
};

constant int Side = 2 * Radius + 3;
// A match is costed on every other tap of the window, in a checkerboard, as in motion_shared.h.
constant int Taps = ((2 * Radius + 1) * (2 * Radius + 1) + 1) / 2;
constexpr sampler Linear(coord::normalized, address::clamp_to_edge, filter::linear);

kernel void Reduce(uint2 id [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]], constant Sizes& sizes [[buffer(0)]],
                   constant float& white [[buffer(1)]], device atomic_uint* keys [[buffer(2)]], texture2d<float, access::read> current [[texture(0)]],
                   texture2d<float, access::read> previous [[texture(1)]], texture2d<float, access::write> levelTarget [[texture(3)]])
{
    threadgroup atomic_uint sections[2 * 9 * SectionLevels];
    for (uint i = lane; i < 2 * 9 * SectionLevels; i += 256)
        atomic_store_explicit(&sections[i], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (all(id < sizes.PictureSize))
    {
        float2 luma = float2(Brightness(current.read(id).rgb, white), Brightness(previous.read(id).rgb, white));
        levelTarget.write(float4(luma, 0.0f, 0.0f), id);
        uint2 cell = min(id * 3u / sizes.PictureSize, uint2(2u));
        uint section = (cell.y * 3u + cell.x) * SectionLevels;
        atomic_fetch_add_explicit(&sections[section + Band(luma.x)], 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&sections[9 * SectionLevels + section + Band(luma.y)], 1u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint j = lane; j < 2 * 9 * SectionLevels; j += 256)
    {
        uint count = atomic_load_explicit(&sections[j], memory_order_relaxed);
        if (count > 0)
            atomic_fetch_add_explicit(&keys[MotionKeySections + j], count, memory_order_relaxed);
    }
}

kernel void Downsample(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], texture2d<float, access::read> finer [[texture(2)]],
                       texture2d<float, access::write> levelTarget [[texture(3)]])
{
    if (any(id >= sizes.Size))
        return;
    uint2 last = uint2(finer.get_width(), finer.get_height()) - 1u;
    float2 sum = float2(0.0f);
    for (uint y = 0; y < 2; ++y)
        for (uint x = 0; x < 2; ++x)
            sum += finer.read(min(id * 2u + uint2(x, y), last)).xy;
    levelTarget.write(float4(sum / 4.0f, 0.0f, 0.0f), id);
}

// Leaves the counts zero for the next estimate.
kernel void Resolve(device uint* keys [[buffer(2)]])
{
    uint changed = 0;
    for (uint section = 0; section < 9; ++section)
    {
        uint pixels = 0;
        uint moved = 0;
        for (uint band = 0; band < SectionLevels; ++band)
        {
            uint index = section * SectionLevels + band;
            uint count = keys[MotionKeySections + index];
            uint before = keys[MotionKeyPrevious + index];
            pixels += count;
            moved += max(count, before) - min(count, before);
            keys[MotionKeySections + index] = 0;
            keys[MotionKeyPrevious + index] = 0;
        }
        // Every pixel that left a level arrived at another, so each is counted twice.
        if (float(moved) > 2.0f * CutShare * float(pixels))
            ++changed;
    }
    keys[MotionKeyCut] = changed >= CutSections ? 1u : 0u;
}

// Where a window's taps are in the luma it is matched in: around origin, spacing apart, in pixels of that luma, each
// of which is texel picture pixels wide.
struct Grid
{
    int2 origin;
    int spacing;
    float texel;
};

// The current luma at the window's taps, and one more on each side so Refine has the gradient at the window's edge.
void LoadWindow(texture2d<float> luma, int2 size, Grid grid, thread float* window)
{
    for (int y = 0; y < Side; ++y)
        for (int x = 0; x < Side; ++x)
            window[y * Side + x] = luma.read(uint2(clamp(grid.origin + (int2(x, y) - Radius - 1) * grid.spacing, int2(0), size - 1))).x;
}

// The previous luma at a point in luma's pixels, between pixels.
float Before(texture2d<float> luma, int2 size, float2 at)
{
    return luma.sample(Linear, (at + 0.5f) / float2(size), level(0.0f)).y;
}

// How much the previous luma at the taps moved by offset, in luma's pixels, differs from the window on average.
float Cost(thread const float* window, texture2d<float> luma, int2 size, Grid grid, float2 offset)
{
    float sum = 0.0f;
    for (int y = -Radius; y <= Radius; ++y)
        for (int x = -Radius; x <= Radius; ++x)
            if (((x + y) & 1) == 0)
                sum += abs(window[(y + Radius + 1) * Side + x + Radius + 1] - Before(luma, size, float2(grid.origin + int2(x, y) * grid.spacing) + offset));
    return sum / float(Taps);
}

// The best vector so far, in picture pixels, how well it matches, and that with what it costs to leave the motion
// expected from the level above.
struct Best
{
    float2 shift;
    float cost;
    float score;
};

// Keeps shift if it scores better than the best so far. Where the motion is even, most candidates are the best
// already, and are not matched again.
void Try(thread const float* window, texture2d<float> luma, int2 size, Grid grid, float scale, float2 shift, float2 expected, thread Best& best)
{
    if (all(shift == best.shift))
        return;
    float cost = Cost(window, luma, size, grid, shift / grid.texel);
    float score = cost + Smoothness * length(shift - expected) / scale;
    if (score < best.score)
    {
        best.shift = shift;
        best.cost = cost;
        best.score = score;
    }
}

// The best vector for p at this level, the cost of none and the motion the level above expects at p, searched in
// steps of step picture pixels around the best candidate. The expected motion is tried first, so it wins a tie.
Best Match(thread const float* window, texture2d<float> luma, int2 size, Grid grid, int2 p, float step, constant Sizes& sizes,
           texture2d<float> coarser, texture2d<float> last, thread float& stillCost, thread float2& expected)
{
    expected = float2(0.0f);
    Best best;
    if (sizes.Coarsest != 0)
    {
        best.shift = float2(0.0f);
        best.cost = Cost(window, luma, size, grid, float2(0.0f));
        best.score = best.cost;
        for (int y = -CoarsestReach; y <= CoarsestReach; ++y)
            for (int x = -CoarsestReach; x <= CoarsestReach; ++x)
                Try(window, luma, size, grid, sizes.Scale, float2(x, y) * sizes.Scale, expected, best);
    }
    else
    {
        // The four pixels above that p lies between.
        int2 last2 = int2(coarser.get_width(), coarser.get_height()) - 1;
        int2 parent = p >> 1;
        int2 toward = (p & 1) * 2 - 1;
        expected = coarser.read(uint2(clamp(parent, int2(0), last2))).xy;
        best.shift = expected;
        best.cost = Cost(window, luma, size, grid, expected / grid.texel);
        best.score = best.cost;
        Try(window, luma, size, grid, sizes.Scale, coarser.read(uint2(clamp(parent + int2(toward.x, 0), int2(0), last2))).xy, expected, best);
        Try(window, luma, size, grid, sizes.Scale, coarser.read(uint2(clamp(parent + int2(0, toward.y), int2(0), last2))).xy, expected, best);
        Try(window, luma, size, grid, sizes.Scale, coarser.read(uint2(clamp(parent + toward, int2(0), last2))).xy, expected, best);
        Try(window, luma, size, grid, sizes.Scale, float2(0.0f), expected, best);
    }
    stillCost = Cost(window, luma, size, grid, float2(0.0f));
    Try(window, luma, size, grid, sizes.Scale, last.read(min(uint2((float2(p) + 0.5f) * sizes.Scale), sizes.PictureSize - 1u)).xy, expected, best);
    float2 centre = best.shift;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            if (x != 0 || y != 0)
                Try(window, luma, size, grid, sizes.Scale, centre + float2(x, y) * step, expected, best);
    return best;
}

kernel void Search(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], texture2d<float> luma [[texture(2)]],
                   texture2d<float> coarser [[texture(4)]], texture2d<float> last [[texture(5)]],
                   texture2d<float, access::write> motionTarget [[texture(6)]])
{
    if (any(id >= sizes.Size))
        return;
    int2 size = int2(sizes.Size);
    Grid grid = { int2(id), 1, sizes.Scale };
    float window[Side * Side];
    LoadWindow(luma, size, grid, window);
    float stillCost;
    float2 expected;
    Best best = Match(window, luma, size, grid, int2(id), sizes.Scale, sizes, coarser, last, stillCost, expected);
    // Half a step more, so the level below starts within its own step of the motion.
    float2 centre = best.shift;
    float halfStep = sizes.Scale / 2.0f;
    Try(window, luma, size, grid, sizes.Scale, centre + float2(halfStep, 0.0f), expected, best);
    Try(window, luma, size, grid, sizes.Scale, centre - float2(halfStep, 0.0f), expected, best);
    Try(window, luma, size, grid, sizes.Scale, centre + float2(0.0f, halfStep), expected, best);
    Try(window, luma, size, grid, sizes.Scale, centre - float2(0.0f, halfStep), expected, best);
    motionTarget.write(float4(best.shift, best.cost, 0.0f), id);
}

// The finest level is matched in the luma at the picture's size, a level's pixel apart, so that a whole pixel of
// motion matches exactly and a fraction of one is found between pixels of the picture rather than of a smaller one.
kernel void Refine(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], device const uint* keys [[buffer(2)]],
                   texture2d<float> coarser [[texture(4)]], texture2d<float> last [[texture(5)]], texture2d<float, access::write> motionTarget [[texture(6)]],
                   texture2d<float> fine [[texture(7)]], texture2d<uint, access::read_write> stillCount [[texture(8)]])
{
    if (any(id >= sizes.Size))
        return;
    int2 p = int2(id);
    int2 size = int2(sizes.PictureSize);
    int spacing = int(sizes.Scale);
    Grid grid = { p * spacing, spacing, 1.0f };
    bool cut = keys[MotionKeyCut] != 0;
    float window[Side * Side];
    LoadWindow(fine, size, grid, window);
    float stillCost;
    float2 expected;
    Best best = Match(window, fine, size, grid, p, 1.0f, sizes, coarser, last, stillCost, expected);
    float2 shift = best.shift;
    float cost = best.cost;

    // Two steps of Lucas and Kanade's method from the best match, as in motion_shared.h.
    float xx = 0.0f, xy = 0.0f, yy = 0.0f;
    for (int y = 1; y < Side - 1; ++y)
        for (int x = 1; x < Side - 1; ++x)
        {
            float2 gradient = float2(window[y * Side + x + 1] - window[y * Side + x - 1], window[(y + 1) * Side + x] - window[(y - 1) * Side + x]) / 2.0f;
            xx += gradient.x * gradient.x;
            xy += gradient.x * gradient.y;
            yy += gradient.y * gradient.y;
        }
    float2 offset = shift;
    for (uint iteration = 0; iteration < 2; ++iteration)
    {
        float2 mismatch = float2(0.0f);
        for (int y = 1; y < Side - 1; ++y)
            for (int x = 1; x < Side - 1; ++x)
            {
                float2 gradient = float2(window[y * Side + x + 1] - window[y * Side + x - 1], window[(y + 1) * Side + x] - window[(y - 1) * Side + x]) / 2.0f;
                float before = Before(fine, size, float2(grid.origin + (int2(x, y) - Radius - 1) * grid.spacing) + offset);
                mismatch += gradient * (before - window[y * Side + x]);
            }
        offset = clamp(offset + Step(xx, xy, yy, mismatch) * float(grid.spacing), shift - 1.0f, shift + 1.0f);
    }
    float refinedCost = Cost(window, fine, size, grid, offset);
    if (refinedCost < cost)
    {
        shift = offset;
        cost = refinedCost;
    }

    bool calm = stillCost <= StillCost && stillCost <= cost + StillMargin && length(expected) < 1.0f;
    uint wins = cut ? 0u : calm ? min(stillCount.read(id).x + 1u, StillWins) : 0u;
    stillCount.write(uint4(wins), id);
    bool still = calm && (stillCost == 0.0f || wins >= StillWins);
    // How much the window's luma varies, as the mean distance of its taps from their mean.
    float mean = 0.0f;
    for (int y = 1; y < Side - 1; ++y)
        for (int x = 1; x < Side - 1; ++x)
            mean += window[y * Side + x];
    mean /= float((Side - 2) * (Side - 2));
    float contrast = 0.0f;
    for (int y = 1; y < Side - 1; ++y)
        for (int x = 1; x < Side - 1; ++x)
            contrast += abs(window[y * Side + x] - mean);
    contrast /= float((Side - 2) * (Side - 2));
    float confidence = cut ? 0.0f : MatchConfidence(still ? stillCost : cost, contrast);
    motionTarget.write(float4(cut || still ? float2(0.0f) : shift, confidence, still ? 1.0f : 0.0f), id);
}

// The largest difference in a channel between color and the previous picture's color that shift brings to q.
float Difference(texture2d<float> previous, uint2 pictureSize, float white, float3 color, uint2 q, float2 shift)
{
    float3 before = previous.sample(Linear, (float2(q) + 0.5f + shift) / float2(pictureSize), level(0.0f)).rgb / white;
    float3 difference = abs(color - before);
    return max(difference.r, max(difference.g, difference.b));
}

kernel void Finish(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], constant float& white [[buffer(1)]],
                   device const uint* keys [[buffer(2)]], texture2d<float, access::read> current [[texture(0)]], texture2d<float> previous [[texture(1)]],
                   texture2d<float> coarser [[texture(4)]], texture2d<float, access::write> motion [[texture(9)]],
                   texture2d<float, access::write> confidence [[texture(10)]])
{
    if (any(id >= sizes.PictureSize))
        return;
    if (keys[MotionKeyCut] != 0)
    {
        motion.write(float4(0.0f), id);
        confidence.write(float4(0.0f), id);
        return;
    }
    // The finest level's pixel that q is in and the three beside it that q is nearest.
    int2 last = int2(coarser.get_width(), coarser.get_height()) - 1;
    uint scale = uint(sizes.Scale);
    int2 cell = int2(id / scale);
    int2 toward = select(int2(-1), int2(1), (id % scale) * 2u >= uint2(scale));
    float3 color = current.read(id).rgb / white;
    float4 chosen = coarser.read(uint2(clamp(cell, int2(0), last)));
    float chosenDifference = Difference(previous, sizes.PictureSize, white, color, id, chosen.xy);
    int2 others[3] = { int2(toward.x, 0), int2(0, toward.y), toward };
    for (uint i = 0; i < 3; ++i)
    {
        float4 other = coarser.read(uint2(clamp(cell + others[i], int2(0), last)));
        float difference = Difference(previous, sizes.PictureSize, white, color, id, other.xy);
        if (difference + ChoiceMargin < chosenDifference)
        {
            chosen = other;
            chosenDifference = difference;
        }
    }
    // A pixel whose vector leads out of the picture came into view.
    float2 from = float2(id) + 0.5f + chosen.xy;
    bool inside = all(from >= float2(0.5f)) && all(from <= float2(sizes.PictureSize) - 0.5f);
    motion.write(float4(chosen.xy, 0.0f, 0.0f), id);
    confidence.write(float4(inside ? chosen.z * saturate(1.0f - chosenDifference / PixelTolerance) : 0.0f), id);
}

// The still counts and the last estimate start out zero.
kernel void ClearMotion(uint2 id [[thread_position_in_grid]], texture2d<float, access::write> target [[texture(9)]])
{
    if (all(id < uint2(target.get_width(), target.get_height())))
        target.write(float4(0.0f), id);
}

kernel void ClearStill(uint2 id [[thread_position_in_grid]], texture2d<uint, access::write> target [[texture(8)]])
{
    if (all(id < uint2(target.get_width(), target.get_height())))
        target.write(uint4(0u), id);
}
)";

// As kMetalShaders' Sizes lays it out.
struct MetalSizes
{
    uint32_t size[2];
    uint32_t pictureSize[2];
    float scale;
    uint32_t coarsest;
};

// Makes everything an estimate at one picture size needs on its device, and its results. A new size needs a new one.
class MetalEstimator
{
public:
    enum Kernel
    {
        kReduce,
        kDownsample,
        kResolve,
        kSearch,
        kRefine,
        kFinish,
        kClearMotion,
        kClearStill,
        kKernelCount,
    };
    using Pipelines = std::array<id<MTLComputePipelineState>, kKernelCount>;

    // Compiles the kernels, which takes seconds, so a caller that cannot wait calls this on another thread. The device
    // can be used from several threads at once.
    static Pipelines Compile(id<MTLDevice> device)
    {
        // A thread of its own has no autorelease pool of its own.
        @autoreleasepool
        {
            const std::string source = std::string("#include <metal_stdlib>\nusing namespace metal;\n#define CONSTANT constant\n") + depth::kShaderShared +
                                       kShaderShared + kMetalShaders;
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
            if (!library)
                throw std::runtime_error(std::string("The motion shaders did not compile: ") +
                                         (error ? error.localizedDescription.UTF8String : "Metal gave no reason."));
            const char* names[] = { "Reduce", "Downsample", "Resolve", "Search", "Refine", "Finish", "ClearMotion", "ClearStill" };
            Pipelines pipelines;
            for (size_t i = 0; i < pipelines.size(); ++i)
            {
                id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:names[i]]];
                NSError* pipelineError = nil;
                pipelines[i] = function ? [device newComputePipelineStateWithFunction:function error:&pipelineError] : nil;
                if (!pipelines[i])
                    throw std::runtime_error(std::string("Metal could not prepare ") + names[i] +
                                             (pipelineError ? std::string(": ") + pipelineError.localizedDescription.UTF8String : std::string(".")));
            }
            // Reduce, Downsample, Finish and the clears run 256 at once, Search and Refine 64.
            for (Kernel kernel : { kReduce, kDownsample, kFinish, kClearMotion, kClearStill })
                if (pipelines[kernel].maxTotalThreadsPerThreadgroup < 256)
                    throw std::runtime_error("Metal cannot run the motion shaders on this graphics card.");
            for (Kernel kernel : { kSearch, kRefine })
                if (pipelines[kernel].maxTotalThreadsPerThreadgroup < 64)
                    throw std::runtime_error("Metal cannot run the motion shaders on this graphics card.");
            return pipelines;
        }
    }

    MetalEstimator(id<MTLDevice> device, const Pipelines& compiled, uint32_t width, uint32_t height) : pipelines(compiled)
    {
        // Each level is half the size of the one below, rounded up. Only the finest and those above it are matched.
        bool found = false;
        for (uint32_t levelWidth = (width + 1) / 2, levelHeight = (height + 1) / 2, scale = 2;;
             levelWidth = (levelWidth + 1) / 2, levelHeight = (levelHeight + 1) / 2, scale *= 2)
        {
            Level& level = levels.emplace_back();
            const bool coarsest = levelWidth <= kCoarsestWidth || levelHeight <= 2;
            level.sizes = { { levelWidth, levelHeight }, { width, height }, static_cast<float>(scale), coarsest };
            level.luma = CreateTexture(device, MTLPixelFormatRG16Float, levelWidth, levelHeight);
            if (!found && (levelWidth <= kFinestWidth || coarsest))
            {
                found = true;
                finest = levels.size() - 1;
            }
            if (found)
                level.motion = CreateTexture(device, MTLPixelFormatRGBA16Float, levelWidth, levelHeight);
            if (coarsest)
                break;
        }
        picture.sizes = { { width, height }, { width, height }, static_cast<float>(2u << finest), 0 };
        picture.luma = CreateTexture(device, MTLPixelFormatRG16Float, width, height);
        still = CreateTexture(device, MTLPixelFormatR32Uint, levels[finest].sizes.size[0], levels[finest].sizes.size[1]);
        motion = CreateTexture(device, MTLPixelFormatRG16Float, width, height);
        confidence = CreateTexture(device, MTLPixelFormatR8Unorm, width, height);
        keys = [device newBufferWithLength:kKeyCount * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        if (!keys)
            throw std::runtime_error("The graphics card is out of memory.");
        std::memset(keys.contents, 0, keys.length);
    }

    // Estimates the motion from previous to current, two pictures of the estimator's size that the device can read as
    // color, the previous one also sampled. White is the value of SDR white in them. Leaves the results in motion,
    // confidence and keys once the command buffer is done.
    void Estimate(id<MTLCommandBuffer> commands, id<MTLTexture> current, id<MTLTexture> previous, float white)
    {
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        if (!cleared)
        {
            cleared = true;
            [encoder setTexture:motion atIndex:9];
            Dispatch(encoder, kClearMotion, motion.width, motion.height, 16);
            [encoder setTexture:still atIndex:8];
            Dispatch(encoder, kClearStill, still.width, still.height, 16);
        }
        [encoder setBytes:&white length:sizeof(white) atIndex:1];
        [encoder setBuffer:keys offset:0 atIndex:2];
        [encoder setTexture:current atIndex:0];
        [encoder setTexture:previous atIndex:1];
        [encoder setBytes:&picture.sizes length:sizeof(picture.sizes) atIndex:0];
        [encoder setTexture:picture.luma atIndex:3];
        Dispatch(encoder, kReduce, picture.sizes.size[0], picture.sizes.size[1], 16);
        for (size_t i = 0; i < levels.size(); ++i)
        {
            [encoder setBytes:&levels[i].sizes length:sizeof(levels[i].sizes) atIndex:0];
            [encoder setTexture:i ? levels[i - 1].luma : picture.luma atIndex:2];
            [encoder setTexture:levels[i].luma atIndex:3];
            Dispatch(encoder, kDownsample, levels[i].sizes.size[0], levels[i].sizes.size[1], 16);
        }
        Dispatch(encoder, kResolve, 1, 1, 1);
        [encoder setTexture:motion atIndex:5];
        [encoder setTexture:picture.luma atIndex:7];
        [encoder setTexture:still atIndex:8];
        for (size_t i = levels.size(); i-- > finest;)
        {
            const Level& level = levels[i];
            // The coarsest level reads none above it, but every texture a kernel takes is bound.
            [encoder setTexture:i + 1 < levels.size() ? levels[i + 1].motion : motion atIndex:4];
            [encoder setBytes:&level.sizes length:sizeof(level.sizes) atIndex:0];
            [encoder setTexture:level.luma atIndex:2];
            [encoder setTexture:level.motion atIndex:6];
            Dispatch(encoder, i > finest ? kSearch : kRefine, level.sizes.size[0], level.sizes.size[1], 8);
        }
        [encoder setBytes:&picture.sizes length:sizeof(picture.sizes) atIndex:0];
        [encoder setTexture:levels[finest].motion atIndex:4];
        [encoder setTexture:motion atIndex:9];
        [encoder setTexture:confidence atIndex:10];
        Dispatch(encoder, kFinish, picture.sizes.size[0], picture.sizes.size[1], 16);
        [encoder endEncoding];
    }

    // Each pixel's vector to where it was in the previous picture, in pixels, as RG16Float.
    id<MTLTexture> motion = nil;
    // How sure each vector is, from 0 to 1, as R8Unorm. 0 where the pixel came into view or the match failed.
    id<MTLTexture> confidence = nil;
    // Whose first unsigned number is 1 after a cut to another scene and 0 otherwise. After a cut, every vector and
    // confidence is 0.
    id<MTLBuffer> keys = nil;

private:
    struct Level
    {
        MetalSizes sizes{};
        id<MTLTexture> luma = nil;   // the current luma in x and the previous in y
        id<MTLTexture> motion = nil; // vector, then the cost, or the confidence and whether it is still at the finest level
    };

    static id<MTLTexture> CreateTexture(id<MTLDevice> device, MTLPixelFormat format, NSUInteger width, NSUInteger height)
    {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
        if (!texture)
            throw std::runtime_error("The graphics card is out of memory.");
        return texture;
    }

    void Dispatch(id<MTLComputeCommandEncoder> encoder, Kernel kernel, NSUInteger width, NSUInteger height, NSUInteger group)
    {
        [encoder setComputePipelineState:pipelines[kernel]];
        [encoder dispatchThreadgroups:MTLSizeMake((width + group - 1) / group, (height + group - 1) / group, 1)
                threadsPerThreadgroup:MTLSizeMake(group, group, 1)];
    }

    Pipelines pipelines;
    Level picture;             // the luma at the picture's size, and the sizes of Reduce and Finish
    std::vector<Level> levels; // from half the picture's size down
    size_t finest = 0;         // the level Refine matches
    id<MTLTexture> still = nil;
    bool cleared = false;
};
} // namespace motion
