#pragma once

// Motion estimated from color alone, for the add-ons that have no motion vectors from the game: where each pixel of the
// current picture was in the previous one, how sure that is, and whether the picture cut to another scene. The motion
// is in picture pixels from current to previous: for a picture whose content moves 3 pixels to the right, a pixel's
// vector is (-3, 0). motion_d3d11.h runs it on Direct3D 11.
//
// Luma is matched in a pyramid, coarsest first, each level starting from the vectors of the one above, the last
// estimate's and none. The finest level has a vector for every 2 by 2 pixels of the picture, or 4 by 4 on a picture
// wider than 2560, and is matched in the picture's own luma and refined to a fraction of a pixel. A pixel whose
// neighbourhood did not change is given no motion at all, since a small wrong vector on a still picture makes what
// follows it creep. Each pixel of the picture then takes the vector of the nearest finest pixels that brings its color
// from the previous picture best, and the confidence falls where none does, as where something came into view.

namespace motion
{
// The shader text the compute shaders follow once CONSTANT is defined.
constexpr char kShaderShared[] = R"(
// A cut to another scene is told from the picture's brightness: each of 3 by 3 sections counts its pixels by
// brightness, in this many levels, and a cut is when at least CutSections of them have CutShare of their pixels at
// another level than in the picture before. A score or a map that changes only reaches the sections it is in.
CONSTANT uint SectionLevels = 16;
CONSTANT uint CutSections = 7;
CONSTANT float CutShare = 0.3;
CONSTANT float3 Luma = float3(0.299, 0.587, 0.114);
// Luma is matched over a window this many taps to each side of a pixel, a pixel of its level apart.
CONSTANT int Radius = 2;
// The coarsest level searches every offset this many of its pixels to each side. It is 33 to 64 pixels wide, so that
// reaches an eighth to a sixteenth of the picture's width.
CONSTANT int CoarsestReach = 4;
// A window whose luma differs from the previous picture's by no more than this on average has not changed, which
// allows for the noise of a picture that was compressed or dithered, if no vector matches it better by more than
// StillMargin and the level above found no motion around it either. Without the last two, a slow pan of a faint
// texture and a plain sky in a fast one would stop.
CONSTANT float StillCost = 0.004;
CONSTANT float StillMargin = 0.0005;
// A picture with grain, such as a game's at night, changes in every window from one picture to the next even where
// nothing moved, so every vector matches about as well on a plain area, and a vector a fraction of a pixel off, whose
// luma is read between pixels, averages the grain and matches best. The noise is the median cost of the plainest
// NoiseShare of the finest level's windows in the estimate before, those whose luma varies least, since grain is all
// their cost can be. A clean picture that moves by a fraction of a pixel matches a little worse on its textures, and
// the costs of every window took that for noise: motion_check's slow pan stopped. The windows are counted by contrast
// in ContrastBins steps of half an octave up to 1/16, and by cost in CostBins steps up to CostRange. A vector only
// replaces the one tried before it, the motion expected from the level above first, if it matches better by more than
// NoiseMargin times the noise beyond NoiseFloor, at each level that over how many picture pixels a level's pixel is
// wide, since each level averages it. On a plain grainy area the expected motion stays, which is none on a still
// picture. NoiseFloor is about what an 8-bit picture that moves by a fraction of a pixel differs by from rounding,
// which matching has always taken: a margin for it stopped more of motion_check's slow pan.
CONSTANT float NoiseShare = 0.1;
CONSTANT float NoiseFloor = 0.001;
CONSTANT uint ContrastBins = 16;
CONSTANT uint CostBins = 64;
CONSTANT float CostRange = 0.032;
CONSTANT float NoiseMargin = 1.0;
// A pixel is still once its window has not changed in this many estimates in a row. Deciding afresh each time makes
// vectors that are almost zero alternate with zero.
CONSTANT uint StillWins = 2;
// The confidence falls from 1 to 0 as the luma that a window's vector brings from the previous picture differs from the
// window's on average, beyond the picture's noise, by up to this share of how much the window's own luma varies, plus
// ContrastFloor. Measured against the window's contrast, a vector that is a fraction of a pixel off on a sharp texture
// keeps its confidence, and a wrong vector on a faint texture, which brings luma nearly as faint, loses it.
CONSTANT float MatchTolerance = 0.5;
CONSTANT float ContrastFloor = 0.01;
// And for each pixel of the picture, as the color its vector brings differs from its own by up to this in any channel.
CONSTANT float PixelTolerance = 0.15;
// A pixel takes another vector than its own window's only if that brings a color this much closer to its own, so
// that still pixels next to moving ones stay still.
CONSTANT float ChoiceMargin = 0.02;
// Where the four windows around a pixel have vectors no further apart than this in either direction, in picture
// pixels, and are all still or all not, as on one surface, the pixel's vector is between theirs by where it lies
// between them. Taking one of the four whole gave neighbours vectors as far apart as their windows', and where that was
// half a pixel or more, as grain makes it, what follows the motion read one pixel twice and skipped the next.
CONSTANT float EvenSpread = 1.0;
// A vector costs this much more for each pixel of its level it is away from the level above's, so a window with
// too little texture to tell vectors apart keeps the motion around it.
CONSTANT float Smoothness = 0.002;
// A window of the finest level too plain to match anything, such as unlit ground or a dusk sky, kept the motion the
// level above expected, which was none, and sat still in blocks while the texture around it moved with the camera.
// Its own vector now counts only as far as its luma varies by more than PlainContrast plus the noise, fully at twice
// that, and it takes the rest from the textured windows around it. The coarsest level's cells with no texture take
// it from those within FillReach of them, nearer ones counting more.
CONSTANT float PlainContrast = 0.004;
CONSTANT int FillReach = 16;
// Keeps the sub-pixel step from growing without bound where a window has an edge in only one direction, or none.
CONSTANT float Damping = 1e-5;
// Where each count is in the keys, which are unsigned numbers: whether this is a cut, the sections' levels of the
// current picture, those of the previous one, the noise as a float's bits, and the count of the finest level's costs.
CONSTANT uint MotionKeyCut = 0;
CONSTANT uint MotionKeySections = 1;
CONSTANT uint MotionKeyPrevious = 1 + 9 * SectionLevels;
CONSTANT uint MotionKeyNoise = 1 + 18 * SectionLevels;
CONSTANT uint MotionKeyCosts = 2 + 18 * SectionLevels;

// The brightness a color is matched by, in units of SDR white.
float Brightness(float3 color, float white)
{
    return max(dot(color, Luma), 0.0f) / white;
}

// Which of the levels the cut test counts a brightness in.
uint Band(float brightness)
{
    return min(uint(brightness * SectionLevels), SectionLevels - 1);
}

float MatchConfidence(float cost, float contrast, float noise)
{
    return saturate(1 - max(cost - noise, 0.0f) / (MatchTolerance * (contrast + ContrastFloor)));
}

// Where a window of the finest level is counted, for the noise.
uint NoiseBin(float contrast, float cost)
{
    uint row = uint(clamp(2 * (log2(max(contrast, 1e-9f)) + 4) + ContrastBins - 1, 0.0f, float(ContrastBins - 1)));
    return row * CostBins + min(uint(cost / CostRange * CostBins), CostBins - 1);
}

// The sub-pixel step that brings the window's luma closest to the previous picture's, from the sums of its gradient's
// products and of the gradient times the difference: one step of Lucas and Kanade's method.
float2 Step(float xx, float xy, float yy, float2 mismatch)
{
    xx += Damping;
    yy += Damping;
    return -float2(yy * mismatch.x - xy * mismatch.y, xx * mismatch.y - xy * mismatch.x) / (xx * yy - xy * xy);
}
)";

// The finest level is the first no wider than this, half the picture's size up to 2560 pixels wide and a quarter
// beyond, since Refine's time grows with its pixels and a 4K picture has enough of them at a quarter.
constexpr unsigned kFinestWidth = 1280;
// The coarsest level is the first no wider than this, so the coarsest search reaches about as far on every size.
constexpr unsigned kCoarsestWidth = 64;
// As kShaders lays them out: whether it is a cut, the levels of 9 sections of 2 pictures, the noise and the count of
// the costs. 16 is kShaderShared's SectionLevels and ContrastBins, and 64 its CostBins.
constexpr unsigned kKeyCount = 2 + 2 * 9 * 16 + 16 * 64;

// The Direct3D compute shaders, in the order they run for each estimate:
// Reduce takes both pictures' luma and counts their brightness for the cut test.
// Downsample makes each level from the one below.
// Resolve decides whether it is a cut.
// Search matches each level from the coarsest down to the one above the finest.
// Refine matches the finest level, to a fraction of a pixel, and decides which pixels are still.
// Gather, Spread, Fill and Settle give the finest level's plain windows the motion of the textured ones around them.
// Finish gives each pixel of the picture its vector and confidence.
constexpr char kShaders[] = R"(
cbuffer Sizes : register(b0)
{
    uint2 Size;        // of the level the pass writes, or the picture's for Reduce and Finish
    uint2 PictureSize;
    float Scale;       // how many picture pixels one pixel of that level is wide, or of the finest level for Finish
    uint Coarsest;     // whether that level is the coarsest, with none above to start from
};
cbuffer Frame : register(b1)
{
    float White;       // the value of SDR white in the pictures
};
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
Texture2D<float2> Level : register(t2);    // the current luma in x and the previous in y, at the level below or this one
Texture2D<float4> Coarser : register(t3);  // the level above's motion, or the finest level's once filled for Finish
Texture2D<float2> Last : register(t4);     // the last estimate's motion
Texture2D<float2> Fine : register(t5);     // the luma at the picture's size
Texture2D<float4> Gathered : register(t6); // the finest level's motion, or a level's as Gather left it
Texture2D<float4> Filled : register(t7);   // the level above's fill as Spread or Fill left it
SamplerState Linear : register(s0);
RWTexture2D<float2> LevelTarget : register(u0);
RWTexture2D<float4> MotionTarget : register(u1); // vector in picture pixels, then the cost, or at the finest level
                                                 // the confidence and how textured the window is, or above it once
                                                 // Gather is done, the moving windows' vector, the share of still
                                                 // windows and how textured
RWByteAddressBuffer Keys : register(u2);
RWTexture2D<uint> Still : register(u3);          // how many estimates in a row the finest level's window has not changed
RWTexture2D<float2> Motion : register(u4);
RWTexture2D<unorm float> Confidence : register(u5);
RWTexture2D<float4> FillTarget : register(u6); // the moving windows' vector and the share of still ones, or at the
                                               // finest level the vector and the confidence

static const int Side = 2 * Radius + 3;
// A match is costed on every other tap of the window, in a checkerboard, which reaches as far for half the samples.
// That made an estimate about a third faster, and the error on motion_check's pans grew by 0.15 px at most.
static const int Taps = ((2 * Radius + 1) * (2 * Radius + 1) + 1) / 2;

groupshared uint Sections[2 * 9 * SectionLevels];

[numthreads(16, 16, 1)]
void Reduce(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 2 * 9 * SectionLevels; i += 256)
        Sections[i] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < PictureSize))
    {
        float2 luma = float2(Brightness(Current.Load(int3(id.xy, 0)).rgb, White), Brightness(Previous.Load(int3(id.xy, 0)).rgb, White));
        LevelTarget[id.xy] = luma;
        uint2 cell = min(id.xy * 3 / PictureSize, 2);
        uint section = (cell.y * 3 + cell.x) * SectionLevels;
        InterlockedAdd(Sections[section + Band(luma.x)], 1);
        InterlockedAdd(Sections[9 * SectionLevels + section + Band(luma.y)], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = index; j < 2 * 9 * SectionLevels; j += 256)
        if (Sections[j] > 0)
            Keys.InterlockedAdd(4 * (MotionKeySections + j), Sections[j]);
}

[numthreads(16, 16, 1)]
void Downsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    uint2 finer;
    Level.GetDimensions(finer.x, finer.y);
    float2 sum = 0;
    for (uint y = 0; y < 2; ++y)
        for (uint x = 0; x < 2; ++x)
            sum += Level.Load(int3(min(id.xy * 2 + uint2(x, y), finer - 1), 0));
    LevelTarget[id.xy] = sum / 4;
}

// A cut is told as the Depth add-on tells one, between the current picture and the previous one. Leaves the counts
// zero for the next estimate.
[numthreads(1, 1, 1)]
void Resolve()
{
    uint changed = 0;
    for (uint section = 0; section < 9; ++section)
    {
        uint pixels = 0;
        uint moved = 0;
        for (uint level = 0; level < SectionLevels; ++level)
        {
            uint index = section * SectionLevels + level;
            uint count = Keys.Load(4 * (MotionKeySections + index));
            uint before = Keys.Load(4 * (MotionKeyPrevious + index));
            pixels += count;
            moved += max(count, before) - min(count, before);
            Keys.Store(4 * (MotionKeySections + index), 0);
            Keys.Store(4 * (MotionKeyPrevious + index), 0);
        }
        // Every pixel that left a level arrived at another, so each is counted twice.
        if (float(moved) > 2 * CutShare * float(pixels))
            ++changed;
    }
    Keys.Store(4 * MotionKeyCut, changed >= CutSections);

    // The noise from the windows Refine counted: the rows of the lowest contrast that hold NoiseShare of them, and the
    // median of their costs between the bounds of its bin. An estimate that only looked for a cut counted none and
    // leaves it as it was.
    uint costs[CostBins];
    for (uint bin = 0; bin < CostBins; ++bin)
        costs[bin] = 0;
    uint total = 0;
    for (uint i = 0; i < ContrastBins * CostBins; ++i)
        total += Keys.Load(4 * (MotionKeyCosts + i));
    uint plain = 0;
    for (uint row = 0; row < ContrastBins && float(plain) < NoiseShare * float(total); ++row)
        for (uint column = 0; column < CostBins; ++column)
        {
            uint count = Keys.Load(4 * (MotionKeyCosts + row * CostBins + column));
            costs[column] += count;
            plain += count;
        }
    float below = 0;
    for (uint column = 0; column < CostBins && plain > 0; ++column)
    {
        float count = float(costs[column]);
        if (count > 0 && below + count >= 0.5 * float(plain))
        {
            Keys.Store(4 * MotionKeyNoise, asuint((float(column) + (0.5 * float(plain) - below) / count) * CostRange / CostBins));
            break;
        }
        below += count;
    }
    for (uint j = 0; j < ContrastBins * CostBins; ++j)
        Keys.Store(4 * (MotionKeyCosts + j), 0);
}
)"
// The compiler takes a string of at most about 16 KB at once, so the shaders go on in another.
                             R"(
// Where a window's taps are in the luma it is matched in: around origin, spacing apart, in pixels of that luma, each
// of which is texel picture pixels wide.
struct Grid
{
    int2 origin;
    int spacing;
    float texel;
};

// The current luma at the window's taps, and one more on each side so Refine has the gradient at the window's edge.
void LoadWindow(Texture2D<float2> luma, int2 size, Grid grid, out float window[Side * Side])
{
    [unroll] for (int y = 0; y < Side; ++y)
        [unroll] for (int x = 0; x < Side; ++x)
            window[y * Side + x] = luma.Load(int3(clamp(grid.origin + (int2(x, y) - Radius - 1) * grid.spacing, 0, size - 1), 0)).x;
}

// The previous luma at a point in luma's pixels, between pixels.
float Before(Texture2D<float2> luma, int2 size, float2 at)
{
    return luma.SampleLevel(Linear, (at + 0.5) / float2(size), 0).y;
}

// How much the previous luma at the taps moved by offset, in luma's pixels, differs from the window on average.
float Cost(float window[Side * Side], Texture2D<float2> luma, int2 size, Grid grid, float2 offset)
{
    float sum = 0;
    [unroll] for (int y = -Radius; y <= Radius; ++y)
        [unroll] for (int x = -Radius; x <= Radius; ++x)
            if (((x + y) & 1) == 0)
                sum += abs(window[(y + Radius + 1) * Side + x + Radius + 1] - Before(luma, size, grid.origin + int2(x, y) * grid.spacing + offset));
    return sum / Taps;
}

// The best vector so far, in picture pixels, how well it matches, and that with what it costs to leave the motion
// expected from the level above.
struct Best
{
    float2 shift;
    float cost;
    float score;
};

// Keeps shift if it scores better than the best so far by more than margin, the share of the noise a level allows.
// Where the motion is even, most candidates are the best already, and are not matched again.
void Try(float window[Side * Side], Texture2D<float2> luma, int2 size, Grid grid, float margin, float2 shift, float2 expected, inout Best best)
{
    if (all(shift == best.shift))
        return;
    float cost = Cost(window, luma, size, grid, shift / grid.texel);
    float score = cost + Smoothness * length(shift - expected) / Scale;
    if (score + margin < best.score)
    {
        best.shift = shift;
        best.cost = cost;
        best.score = score;
    }
}

// The best vector for p at this level, the cost of none and the motion the level above expects at p, searched in
// steps of step picture pixels around the best candidate. The expected motion is tried first, so it wins a tie.
Best Match(float window[Side * Side], Texture2D<float2> luma, int2 size, Grid grid, float margin, int2 p, float step, out float stillCost,
           out float2 expected)
{
    expected = 0;
    Best best;
    if (Coarsest)
    {
        best.shift = 0;
        best.cost = best.score = Cost(window, luma, size, grid, 0);
        for (int y = -CoarsestReach; y <= CoarsestReach; ++y)
            for (int x = -CoarsestReach; x <= CoarsestReach; ++x)
                Try(window, luma, size, grid, margin, float2(x, y) * Scale, expected, best);
    }
    else
    {
        // The four pixels above that p lies between.
        uint2 above;
        Coarser.GetDimensions(above.x, above.y);
        int2 parent = p >> 1;
        int2 toward = (p & 1) * 2 - 1;
        int2 last = int2(above) - 1;
        expected = Coarser.Load(int3(clamp(parent, 0, last), 0)).xy;
        best.shift = expected;
        best.cost = best.score = Cost(window, luma, size, grid, expected / grid.texel);
        Try(window, luma, size, grid, margin, Coarser.Load(int3(clamp(parent + int2(toward.x, 0), 0, last), 0)).xy, expected, best);
        Try(window, luma, size, grid, margin, Coarser.Load(int3(clamp(parent + int2(0, toward.y), 0, last), 0)).xy, expected, best);
        Try(window, luma, size, grid, margin, Coarser.Load(int3(clamp(parent + toward, 0, last), 0)).xy, expected, best);
        Try(window, luma, size, grid, margin, 0, expected, best);
    }
    stillCost = Cost(window, luma, size, grid, 0);
    Try(window, luma, size, grid, margin, Last.Load(int3(min(uint2((float2(p) + 0.5) * Scale), PictureSize - 1), 0)), expected, best);
    float2 centre = best.shift;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            if (x != 0 || y != 0)
                Try(window, luma, size, grid, margin, centre + float2(x, y) * step, expected, best);
    return best;
}

[numthreads(8, 8, 1)]
void Search(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    Grid grid = { int2(id.xy), 1, Scale };
    float window[Side * Side];
    LoadWindow(Level, int2(Size), grid, window);
    float margin = NoiseMargin * max(asfloat(Keys.Load(4 * MotionKeyNoise)) - NoiseFloor, 0.0f) / Scale;
    float stillCost;
    float2 expected;
    Best best = Match(window, Level, int2(Size), grid, margin, int2(id.xy), Scale, stillCost, expected);
    // Half a step more, so the level below starts within its own step of the motion.
    float2 centre = best.shift;
    Try(window, Level, int2(Size), grid, margin, centre + float2(Scale / 2, 0), expected, best);
    Try(window, Level, int2(Size), grid, margin, centre - float2(Scale / 2, 0), expected, best);
    Try(window, Level, int2(Size), grid, margin, centre + float2(0, Scale / 2), expected, best);
    Try(window, Level, int2(Size), grid, margin, centre - float2(0, Scale / 2), expected, best);
    MotionTarget[id.xy] = float4(best.shift, best.cost, 0);
}

// The finest level is matched in the luma at the picture's size, a level's pixel apart, so that a whole pixel of
// motion matches exactly and a fraction of one is found between pixels of the picture rather than of a smaller one.
[numthreads(8, 8, 1)]
void Refine(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    int2 p = int2(id.xy);
    int2 size = int2(PictureSize);
    Grid grid = { p * int(Scale), int(Scale), 1 };
    bool cut = Keys.Load(4 * MotionKeyCut) != 0;
    float window[Side * Side];
    LoadWindow(Fine, size, grid, window);
    float noise = asfloat(Keys.Load(4 * MotionKeyNoise));
    float margin = NoiseMargin * max(noise - NoiseFloor, 0.0f);
    float stillCost;
    float2 expected;
    Best best = Match(window, Fine, size, grid, margin, p, 1, stillCost, expected);
    float2 shift = best.shift;
    float cost = best.cost;

    // Two steps of Lucas and Kanade's method from the best match, with the current picture's gradient standing
    // in for the previous one's, so it is found once. The gradient is per tap, so the steps are too.
    float xx = 0, xy = 0, yy = 0;
    [unroll] for (int y = 1; y < Side - 1; ++y)
        [unroll] for (int x = 1; x < Side - 1; ++x)
        {
            float2 gradient = float2(window[y * Side + x + 1] - window[y * Side + x - 1], window[(y + 1) * Side + x] - window[(y - 1) * Side + x]) / 2;
            xx += gradient.x * gradient.x;
            xy += gradient.x * gradient.y;
            yy += gradient.y * gradient.y;
        }
    float2 offset = shift;
    for (uint iteration = 0; iteration < 2; ++iteration)
    {
        float2 mismatch = 0;
        [unroll] for (int y = 1; y < Side - 1; ++y)
            [unroll] for (int x = 1; x < Side - 1; ++x)
            {
                float2 gradient = float2(window[y * Side + x + 1] - window[y * Side + x - 1], window[(y + 1) * Side + x] - window[(y - 1) * Side + x]) / 2;
                float before = Before(Fine, size, grid.origin + (int2(x, y) - Radius - 1) * grid.spacing + offset);
                mismatch += gradient * (before - window[y * Side + x]);
            }
        offset = clamp(offset + Step(xx, xy, yy, mismatch) * grid.spacing, shift - 1, shift + 1);
    }
    float refinedCost = Cost(window, Fine, size, grid, offset);
    if (refinedCost + margin < cost)
    {
        shift = offset;
        cost = refinedCost;
    }

    bool calm = stillCost <= StillCost && stillCost <= cost + StillMargin && length(expected) < 1;
    uint wins = cut ? 0 : calm ? min(Still[id.xy] + 1, StillWins) : 0;
    Still[id.xy] = wins;
    bool still = calm && (stillCost == 0 || wins >= StillWins);
    // How much the window's luma varies, as the mean distance of its taps from their mean.
    float mean = 0;
    [unroll] for (int y = 1; y < Side - 1; ++y)
        [unroll] for (int x = 1; x < Side - 1; ++x)
            mean += window[y * Side + x];
    mean /= (Side - 2) * (Side - 2);
    float contrast = 0;
    [unroll] for (int y = 1; y < Side - 1; ++y)
        [unroll] for (int x = 1; x < Side - 1; ++x)
            contrast += abs(window[y * Side + x] - mean);
    contrast /= (Side - 2) * (Side - 2);
    // A quarter of the windows is enough to measure the noise by, and keeps the counting cheap.
    if (!cut && all((p & 1) == 0))
        Keys.InterlockedAdd(4 * (MotionKeyCosts + NoiseBin(contrast, cost)), 1);
    float confidence = cut ? 0 : MatchConfidence(still ? stillCost : cost, contrast, noise);
    float textured = saturate(contrast / (PlainContrast + noise) - 1);
    MotionTarget[id.xy] = float4(cut || still ? float2(0, 0) : shift, confidence, textured);
}

// Each pixel of a level above the finest counts the finest level's windows it covers by how textured they are: how
// many of them are still, with no motion at all, and the average vector of those that move. It is as textured as one
// textured window.
[numthreads(8, 8, 1)]
void Gather(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    uint2 finest;
    Gathered.GetDimensions(finest.x, finest.y);
    uint span = uint(round(Scale * float(finest.x) / float(PictureSize.x)));
    float3 moving = 0;
    float still = 0;
    for (uint y = 0; y < span; ++y)
        for (uint x = 0; x < span; ++x)
        {
            uint2 q = id.xy * span + uint2(x, y);
            if (all(q < finest))
            {
                float4 window = Gathered.Load(int3(q, 0));
                if (all(window.xy == 0))
                    still += window.w;
                else
                    moving += float3(window.xy, 1) * window.w;
            }
        }
    float textured = moving.z + still;
    MotionTarget[id.xy] = float4(moving.z > 0 ? moving.xy / moving.z : float2(0, 0), textured > 0 ? still / textured : 1, min(textured, 1));
}

// Takes what a pixel of the fill lacks in texture from more: its share of still windows, and its moving windows'
// vector as far as it has no moving windows.
float4 Complete(float4 own, float4 more)
{
    return float4(lerp(more.xy, own.xy, saturate(own.w * (1 - own.z))), lerp(more.z, own.z, own.w), 0);
}

// The coarsest level's pixels take what they lack in texture from the textured pixels within FillReach of them.
[numthreads(8, 8, 1)]
void Spread(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    float3 moving = 0;
    float still = 0;
    for (int y = -FillReach; y <= FillReach; ++y)
        for (int x = -FillReach; x <= FillReach; ++x)
        {
            int2 q = int2(id.xy) + int2(x, y);
            if (all(q >= 0) && all(q < int2(Size)))
            {
                float4 cell = Gathered.Load(int3(q, 0));
                float distance = 1 + x * x + y * y;
                float weight = cell.w / (distance * distance);
                moving += float3(cell.xy, 1) * weight * (1 - cell.z);
                still += weight * cell.z;
            }
        }
    float textured = moving.z + still;
    float4 around = float4(moving.z > 0 ? moving.xy / moving.z : float2(0, 0), textured > 0 ? still / textured : 1, 1);
    FillTarget[id.xy] = Complete(Gathered.Load(int3(id.xy, 0)), around);
}

// The level above's fill at p of this level.
float4 Above(int2 p)
{
    uint2 size;
    Filled.GetDimensions(size.x, size.y);
    return Filled.SampleLevel(Linear, (float2(p) + 0.5) / 2 / float2(size), 0);
}

// The levels between the coarsest and the finest take what each pixel lacks in texture from the level above.
[numthreads(16, 16, 1)]
void Fill(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    FillTarget[id.xy] = Complete(Gathered.Load(int3(id.xy, 0)), Above(int2(id.xy)));
}

// Each window of the finest level takes what it lacks in texture from the level above: no motion where most of the
// texture around it is still, so a still picture's plain windows stay exactly still, and otherwise the vector of what
// moves.
[numthreads(16, 16, 1)]
void Settle(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    float4 own = Gathered.Load(int3(id.xy, 0));
    float4 above = Above(int2(id.xy));
    FillTarget[id.xy] = float4(lerp(above.z > 0.5 ? float2(0, 0) : above.xy, own.xy, own.w), own.z, 0);
}

// The largest difference in a channel between color and the previous picture's color that shift brings to q.
float Difference(float3 color, uint2 q, float2 shift)
{
    float3 before = Previous.SampleLevel(Linear, (float2(q) + 0.5 + shift) / float2(PictureSize), 0).rgb / White;
    float3 difference = abs(color - before);
    return max(difference.r, max(difference.g, difference.b));
}

[numthreads(16, 16, 1)]
void Finish(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= PictureSize))
        return;
    if (Keys.Load(4 * MotionKeyCut) != 0)
    {
        Motion[id.xy] = 0;
        Confidence[id.xy] = 0;
        return;
    }
    // The finest level's pixel that q is in and the three beside it that q is nearest.
    uint2 finest;
    Coarser.GetDimensions(finest.x, finest.y);
    uint scale = uint(Scale);
    int2 cell = int2(id.xy / scale);
    int2 toward = int2((id.xy % scale) * 2 >= scale) * 2 - 1;
    int2 last = int2(finest) - 1;
    float3 color = Current.Load(int3(id.xy, 0)).rgb / White;
    float4 around[4] = { Coarser.Load(int3(cell, 0)), Coarser.Load(int3(clamp(cell + int2(toward.x, 0), 0, last), 0)),
                         Coarser.Load(int3(clamp(cell + int2(0, toward.y), 0, last), 0)), Coarser.Load(int3(clamp(cell + toward, 0, last), 0)) };
    // A window is still if it has no motion at all, as a plain one among still ones also has once it is filled.
    float still = 0;
    for (uint j = 0; j < 4; ++j)
        still += all(around[j].xy == 0);
    float2 low = min(min(around[0].xy, around[1].xy), min(around[2].xy, around[3].xy));
    float2 high = max(max(around[0].xy, around[1].xy), max(around[2].xy, around[3].xy));
    bool even = all(high - low <= EvenSpread) && (still == 0 || still == 4);
    float4 chosen = around[0];
    if (even)
    {
        // How far q is from its own window's centre towards the others', as a share of the way.
        float2 share = abs(float2(id.xy % scale) + 0.5 - Scale / 2) / Scale;
        chosen = lerp(lerp(around[0], around[1], share.x), lerp(around[2], around[3], share.x), share.y);
    }
    float chosenDifference = Difference(color, id.xy, chosen.xy);
    for (uint i = 1; i < 4 && !even; ++i)
    {
        float difference = Difference(color, id.xy, around[i].xy);
        if (difference + ChoiceMargin < chosenDifference)
        {
            chosen = around[i];
            chosenDifference = difference;
        }
    }
    // A pixel whose vector leads out of the picture came into view.
    float2 from = float2(id.xy) + 0.5 + chosen.xy;
    bool inside = all(from >= 0.5) && all(from <= float2(PictureSize) - 0.5);
    Motion[id.xy] = chosen.xy;
    Confidence[id.xy] = inside ? chosen.z * saturate(1 - chosenDifference / PixelTolerance) : 0;
}
)";
} // namespace motion
