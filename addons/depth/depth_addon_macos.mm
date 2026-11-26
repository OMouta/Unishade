// Depth estimation as a Unishade add-on on macOS, Depth.dylib, as depth_addon.cpp is on Windows. Depth Anything V2 runs
// through ONNX Runtime on Core ML (depth_model.cpp) on a worker thread. Metal on the graphics card Unishade draws with
// takes the frame Unishade shares, makes the model's input from it and turns the model's output into DEPTH. The model
// can take longer than a frame: frames that arrive meanwhile are not estimated. Depth is kept as a history that
// follows the picture's motion, from motion_metal.h, in every frame, and each estimate is blended into it. Unishade
// needs the add-on's own work on the graphics card done when frame returns, so each frame waits for it. That is the
// motion, the box filter and the blend, never the model.
//
// The files it needs are in its folder with it: depth-anything-v2-small.onnx and libonnxruntime.dylib. The models Core
// ML compiles from it go in CoreML there.

#include "depth_model.h"
#include "depth_shared.h"
#include "motion/motion_metal.h"
#include <unishade_addon.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOSurface/IOSurfaceRef.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
using namespace depth;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

// The kernels of depth_addon.cpp in the Metal shading language. The model's input and output are in buffers the
// processor reads and writes, bound twice: as float and as half, for the model's type. kShaderShared goes before them.
constexpr char kShaders[] = R"(
struct Sizes
{
    uint2 SourceSize;
    uint2 ModelSize; // of the model's input and output
    uint HalfInput;
    uint HalfOutput;
    uint Restart; // whether this frame is the next estimate's, which the history's motion is counted from
    float Relax;  // how far this estimate's scale and shift move back towards none
    uint Show;    // whether the history holds depth yet, to write to DEPTH
    float Share;  // how much of this estimate a trusted history takes
    uint Moving;  // whether there is motion to follow yet
};

float Load(constant Sizes& sizes, device const float* result, device const half* halfResult, uint index)
{
    return sizes.HalfOutput ? float(halfResult[index]) : result[index];
}

// Box-filters the frame down to the model size and writes planar, ImageNet-normalized RGB, and the colors themselves
// for EstimateAt.
kernel void Preprocess(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], device float* input [[buffer(1)]],
                       device half* halfInput [[buffer(2)]], texture2d<float, access::read> source [[texture(0)]],
                       texture2d<float, access::write> guide [[texture(1)]])
{
    if (any(id >= sizes.ModelSize))
        return;
    uint2 begin = id * sizes.SourceSize / sizes.ModelSize;
    uint2 end = max((id + 1u) * sizes.SourceSize / sizes.ModelSize, begin + 1u);
    float3 sum = float3(0);
    for (uint y = begin.y; y < end.y; ++y)
        for (uint x = begin.x; x < end.x; ++x)
            sum += source.read(uint2(x, y)).rgb;
    float3 average = sum / float((end.x - begin.x) * (end.y - begin.y));
    guide.write(float4(average, 1), id);
    float3 color = (average - Mean) / Std;
    uint plane = sizes.ModelSize.x * sizes.ModelSize.y;
    uint index = id.y * sizes.ModelSize.x + id.x;
    for (uint channel = 0; channel < 3; ++channel)
    {
        if (sizes.HalfInput)
            halfInput[index + channel * plane] = half(color[channel]);
        else
            input[index + channel * plane] = color[channel];
    }
}

// Floats as unsigned numbers in the same order, for atomics.
uint Ordered(float value)
{
    uint bits = as_type<uint>(value);
    return (bits & 0x80000000) ? ~bits : bits | 0x80000000;
}

float Unordered(uint key)
{
    return as_type<float>((key & 0x80000000) ? key & 0x7FFFFFFF : ~key);
}

// Finds the highest and the lowest value of the output. The keys start out zero, below every key, so the lowest is
// kept as the highest of the keys turned over.
kernel void Range(uint id [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]], constant Sizes& sizes [[buffer(0)]],
                  device const float* result [[buffer(3)]], device const half* halfResult [[buffer(4)]], device atomic_uint* keys [[buffer(5)]])
{
    threadgroup float highs[256];
    threadgroup float lows[256];
    float value = Load(sizes, result, halfResult, min(id, sizes.ModelSize.x * sizes.ModelSize.y - 1));
    highs[lane] = value;
    lows[lane] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = 128; step > 0; step >>= 1)
    {
        if (lane < step)
        {
            highs[lane] = max(highs[lane], highs[lane + step]);
            lows[lane] = min(lows[lane], lows[lane + step]);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0)
    {
        atomic_fetch_max_explicit(&keys[KeyHighest], Ordered(highs[0]), memory_order_relaxed);
        atomic_fetch_max_explicit(&keys[KeyLowest], ~Ordered(lows[0]), memory_order_relaxed);
    }
}

// Counts the output's values between the lowest and the highest.
kernel void Histogram(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], device const float* result [[buffer(3)]],
                      device const half* halfResult [[buffer(4)]], device atomic_uint* keys [[buffer(5)]])
{
    if (any(id >= sizes.ModelSize))
        return;
    float low = Unordered(~atomic_load_explicit(&keys[KeyLowest], memory_order_relaxed));
    float high = Unordered(atomic_load_explicit(&keys[KeyHighest], memory_order_relaxed));
    float value = Load(sizes, result, halfResult, id.y * sizes.ModelSize.x + id.x);
    uint bin = min(uint(saturate((value - low) / max(high - low, 1e-20f)) * float(AnchorBins)), AnchorBins - 1u);
    atomic_fetch_add_explicit(&keys[KeyBins + bin], 1u, memory_order_relaxed);
}

// Finds the output's anchor in the counts and keeps it in state[0]. Leaves the counts zero for the next estimate.
kernel void Resolve(constant Sizes& sizes [[buffer(0)]], device uint* keys [[buffer(5)]], device float4* state [[buffer(6)]])
{
    float low = Unordered(~keys[KeyLowest]);
    float high = Unordered(keys[KeyHighest]);
    uint wanted = uint(float(sizes.ModelSize.x * sizes.ModelSize.y) * AnchorShare);
    uint below = 0;
    float anchor = high;
    bool found = false;
    for (uint bin = 0; bin < AnchorBins; ++bin)
    {
        uint count = keys[KeyBins + bin];
        if (!found && count > 0 && below + count >= wanted)
        {
            anchor = low + (float(bin) + float(wanted - below) / float(count)) * (high - low) / float(AnchorBins);
            found = true;
        }
        below += count;
        keys[KeyBins + bin] = 0;
    }
    keys[KeyHighest] = 0;
    keys[KeyLowest] = 0;
    state[0] = float4(anchor, 0.0f, 0.0f, 0.0f);
}

// The estimate's closeness at a point of the frame it was estimated from, in its pixels, divided by the anchor, as
// Upsample says.
float EstimateAt(constant Sizes& sizes, device const float* result, device const half* halfResult, device const float4* state,
                 texture2d<float, access::read> source, texture2d<float, access::read> guide, float2 at)
{
    float3 color = source.read(uint2(clamp(int2(floor(at)), int2(0), int2(sizes.SourceSize) - 1))).rgb;
    float2 position = at * float2(sizes.ModelSize) / float2(sizes.SourceSize) - 0.5f;
    int2 corner = int2(floor(position));
    int2 last = int2(sizes.ModelSize) - 1;
    uint2 a = uint2(clamp(corner, int2(0), last));
    uint2 b = uint2(clamp(corner + int2(1, 0), int2(0), last));
    uint2 c = uint2(clamp(corner + int2(0, 1), int2(0), last));
    uint2 d = uint2(clamp(corner + int2(1, 1), int2(0), last));
    uint width = sizes.ModelSize.x;
    float4 values = float4(Load(sizes, result, halfResult, a.y * width + a.x), Load(sizes, result, halfResult, b.y * width + b.x),
                           Load(sizes, result, halfResult, c.y * width + c.x), Load(sizes, result, halfResult, d.y * width + d.x));
    float value = Upsample(values, guide.read(a).rgb, guide.read(b).rgb, guide.read(c).rgb, guide.read(d).rgb, color, position - float2(corner));
    return value / max(state[0].x, 1e-6f);
}

// The history at a point of the frame before, in its pixels, as Reproject says.
float4 HistoryAt(texture2d<float, access::read> history, uint2 size, float2 at)
{
    int2 last = int2(size) - 1;
    float2 position = at - 0.5f;
    int2 corner = int2(floor(position));
    float2 fraction = position - float2(corner);
    float4 a = history.read(uint2(clamp(corner, int2(0), last)));
    float4 b = history.read(uint2(clamp(corner + int2(1, 0), int2(0), last)));
    float4 c = history.read(uint2(clamp(corner + int2(0, 1), int2(0), last)));
    float4 d = history.read(uint2(clamp(corner + int2(1, 1), int2(0), last)));
    float4 nearest = fraction.y < 0.5f ? (fraction.x < 0.5f ? a : b) : (fraction.x < 0.5f ? c : d);
    float4 between = mix(mix(a, b, float4(fraction.x)), mix(c, d, float4(fraction.x)), float4(fraction.y));
    return float4(Reproject(float4(a.x, b.x, c.x, d.x)) ? between.xyz : nearest.xyz, nearest.w);
}

// Moves the history along the picture's motion. What a pixel was in the frame before is at its vector, and the motion
// from there to the next estimate's frame adds up. A pixel the vector cannot be trusted for, such as one that came
// into view, loses its history's trust, and the estimate's frame's view of it too. Without motion to follow yet, the
// history stands still and trusts nothing.
kernel void Warp(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], texture2d<float, access::write> depth [[texture(2)]],
                 texture2d<float, access::read> motion [[texture(3)]], texture2d<float, access::read> confidence [[texture(4)]],
                 texture2d<float, access::read> history [[texture(5)]], texture2d<float, access::write> historyTarget [[texture(6)]])
{
    if (any(id >= sizes.SourceSize))
        return;
    float2 shift = sizes.Moving != 0 ? motion.read(id).xy : float2(0.0f);
    float4 before = HistoryAt(history, sizes.SourceSize, float2(id) + 0.5f + shift);
    uint flags = sizes.Moving != 0 && confidence.read(id).x >= TrustedConfidence ? uint(before.w) : 0u;
    float2 toEstimate = shift + before.yz;
    if (sizes.Restart != 0)
    {
        toEstimate = float2(0.0f);
        flags |= SeenByEstimate;
    }
    historyTarget.write(float4(before.x, toEstimate, float(flags)), id);
    // Unishade can make its images again at another size before the history follows.
    if (sizes.Show != 0 && all(id < uint2(depth.get_width(), depth.get_height())))
        depth.write(float4(StoredDepth(before.x)), id);
}

// Sums what Fit needs over every FitStep-th pixel the history and the estimate both see: the count, and the history's
// and the estimate's closeness and their products, less 1 so the sums keep their precision. Each group writes its own,
// in two float4 a group.
kernel void Gather(uint2 id [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]], uint2 group [[threadgroup_position_in_grid]],
                   constant Sizes& sizes [[buffer(0)]], device const float* result [[buffer(3)]], device const half* halfResult [[buffer(4)]],
                   device const float4* state [[buffer(6)]], device float4* sums [[buffer(7)]], texture2d<float, access::read> source [[texture(0)]],
                   texture2d<float, access::read> guide [[texture(1)]], texture2d<float, access::read> history [[texture(5)]])
{
    threadgroup float4 firstSums[256];
    threadgroup float4 secondSums[256];
    uint2 pixel = id * FitStep + FitStep / 2;
    float4 first = float4(0.0f);
    float4 second = float4(0.0f);
    if (all(pixel < sizes.SourceSize))
    {
        float4 before = history.read(pixel);
        if ((uint(before.w) & (Trusted | SeenByEstimate)) == (Trusted | SeenByEstimate))
        {
            float h = before.x - 1;
            float e = EstimateAt(sizes, result, halfResult, state, source, guide, float2(pixel) + 0.5f + before.yz) - 1;
            first = float4(1.0f, h, e, h * h);
            second = float4(h * e, 0.0f, 0.0f, 0.0f);
        }
    }
    firstSums[lane] = first;
    secondSums[lane] = second;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = 128; step > 0; step >>= 1)
    {
        if (lane < step)
        {
            firstSums[lane] += firstSums[lane + step];
            secondSums[lane] += secondSums[lane + step];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0)
    {
        uint groups = (sizes.SourceSize.x + FitStep * 16 - 1) / (FitStep * 16);
        uint index = group.y * groups + group.x;
        sums[2 * index] = firstSums[0];
        sums[2 * index + 1] = secondSums[0];
    }
}

// Fits the scale and shift that bring the estimate to the history in state[1], as Align says.
kernel void Fit(constant Sizes& sizes [[buffer(0)]], device float4* state [[buffer(6)]], device const float4* sums [[buffer(7)]])
{
    uint2 groups = (sizes.SourceSize + FitStep * 16 - 1) / (FitStep * 16);
    float4 first = float4(0.0f);
    float4 second = float4(0.0f);
    for (uint index = 0; index < groups.x * groups.y; ++index)
    {
        first += sums[2 * index];
        second += sums[2 * index + 1];
    }
    uint2 samples = (sizes.SourceSize + FitStep - 1) / FitStep;
    state[1] = Align(first, second.x, samples.x * samples.y, sizes.Relax);
}

// Blends the estimate, aligned to the history, into it where the estimate's frame saw the pixel: at once where the
// history is not trusted, and by Share where it is. Where the estimate's frame did not see it, the history stays.
kernel void Accumulate(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], device const float* result [[buffer(3)]],
                       device const half* halfResult [[buffer(4)]], device const float4* state [[buffer(6)]],
                       texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::read> guide [[texture(1)]],
                       texture2d<float, access::write> depth [[texture(2)]], texture2d<float, access::read> history [[texture(5)]],
                       texture2d<float, access::write> historyTarget [[texture(6)]])
{
    if (any(id >= sizes.SourceSize))
        return;
    float4 before = history.read(id);
    uint flags = uint(before.w);
    float closeness = before.x;
    if ((flags & SeenByEstimate) != 0)
    {
        float4 fit = state[1];
        float estimate = max(EstimateAt(sizes, result, halfResult, state, source, guide, float2(id) + 0.5f + before.yz) * fit.x + fit.y, 0.0f);
        closeness = (flags & Trusted) != 0 ? mix(closeness, estimate, sizes.Share) : estimate;
        flags |= Trusted;
    }
    float2 toEstimate = before.yz;
    if (sizes.Restart != 0)
    {
        toEstimate = float2(0.0f);
        flags |= SeenByEstimate;
    }
    historyTarget.write(float4(closeness, toEstimate, float(flags)), id);
    // An estimate can land after Unishade made its images again at another size.
    if (all(id < uint2(depth.get_width(), depth.get_height())))
        depth.write(float4(StoredDepth(closeness)), id);
}

// Empties the history.
kernel void Clear(uint2 id [[thread_position_in_grid]], texture2d<float, access::write> historyTarget [[texture(6)]])
{
    if (all(id < uint2(historyTarget.get_width(), historyTarget.get_height())))
        historyTarget.write(float4(0.0f), id);
}

// Puts DEPTH at the far plane.
kernel void Far(uint2 id [[thread_position_in_grid]], texture2d<float, access::write> depth [[texture(2)]])
{
    if (all(id < uint2(depth.get_width(), depth.get_height())))
        depth.write(float4(1), id);
}
)";

enum Kernel
{
    kPreprocess,
    kRange,
    kHistogram,
    kResolve,
    kWarp,
    kGather,
    kFit,
    kAccumulate,
    kClear,
    kFar,
    kKernelCount,
};
constexpr const char* kKernelNames[] = { "Preprocess", "Range", "Histogram", "Resolve", "Warp", "Gather", "Fit", "Accumulate", "Clear", "Far" };

// As Sizes in the shaders.
struct Sizes
{
    uint32_t sourceSize[2];
    uint32_t modelSize[2];
    uint32_t halfInput;
    uint32_t halfOutput;
    uint32_t restart;
    float relax;
    uint32_t show;
    float share;
    uint32_t moving;
    uint32_t unused; // Metal rounds the struct up to a multiple of its uint2's 8 bytes
};

class Depth
{
public:
    explicit Depth(const UnishadeHost& host);
    ~Depth();
    void Frame(const UnishadeFrame& frame);

private:
    void Log(UnishadeLogLevel level, const char* format, ...);
    void Worker();
    void Request(bool withInput);
    void Open(const UnishadeFrame& frame);
    void Update(id<MTLCommandBuffer> commands, const UnishadeFrame& frame, bool opened);
    void Resize(uint32_t frameWidth, uint32_t frameHeight, int longest);
    void CreateShared();
    void ReleaseShared();
    void Far(id<MTLCommandBuffer> commands);
    void Follow(id<MTLCommandBuffer> commands, bool restart);
    void Publish(id<MTLCommandBuffer> commands, bool restart);
    void Dispatch(id<MTLComputeCommandEncoder> encoder, Kernel kernel, NSUInteger threadsWide, NSUInteger threadsHigh, NSUInteger group);
    Sizes SizesFor(bool restart, float relax, float share) const;
    void LogState();
    id<MTLTexture> CreateTexture(MTLPixelFormat format, NSUInteger textureWidth, NSUInteger textureHeight, MTLTextureUsage usage);

    const UnishadeHost& host;
    std::string directory;
    // Core ML's compiled models for this model file, a folder for each input size, and the folder holding them and
    // those of model files before it.
    fs::path cache;
    fs::path cacheRoot;

    // Only the main thread uses Metal.
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    std::array<id<MTLComputePipelineState>, kKernelCount> pipelines;
    // Unishade's, for the frame's generation.
    uint64_t generation = UINT64_MAX;
    id<MTLTexture> color = nil;
    id<MTLTexture> depth = nil;

    int width = 0; // model input and output size, follows the frame's aspect ratio
    int height = 0;
    int size = 0; // their longest side, as Detail picks it
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    std::optional<uint64_t> lastInput;
    // When the last estimate became DEPTH, or nothing before the first.
    std::optional<Clock::time_point> publishedAt;
    // The frame's work made the model's input, which the worker gets once the work is done.
    bool inputMade = false;
    Clock::time_point requestedAt{};
    std::atomic<double> workerMs = 0;
    bool slowRequestLogged = false;
    id<MTLTexture> frameCopy = nil;

    // Depth follows the picture's motion, which the estimator finds between the frame and a copy of the one before.
    // Its kernels take seconds to compile, on a thread of their own, and depth only follows motion once they are.
    std::future<motion::MetalEstimator::Pipelines> compiling;
    std::optional<motion::MetalEstimator::Pipelines> motionPipelines;
    std::unique_ptr<motion::MetalEstimator> estimator;
    id<MTLTexture> previous = nil;
    bool hasPrevious = false;
    std::optional<uint64_t> lastPicture;
    Clock::time_point changedAt{};
    // The history, as kShaders lays it out, in two textures each pass reads one of and writes the other.
    std::array<id<MTLTexture>, 2> history;
    size_t historyIndex = 0;
    // Until the first frame that follows motion clears it.
    bool historyEmpty = false;
    // Whether the history holds an estimate yet.
    bool showing = false;
    id<MTLBuffer> sums = nil;

    // Made for each frame and model size. The model reads input and writes output where the processor reaches them.
    id<MTLBuffer> input = nil;
    id<MTLBuffer> output = nil;
    id<MTLTexture> guide = nil;
    id<MTLBuffer> keys = nil;
    id<MTLBuffer> state = nil;

    // The worker owns the model. While busy is set it reads the fields below and writes the model's size and types,
    // and the main thread only touches them while it is clear.
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool requested = false; // under mutex
    bool stopping = false;  // under mutex
    std::atomic<bool> busy = false;
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    std::string failure;
    // Whether to run the model on the input, or only build it for width and height.
    bool withInput = false;
    // Goes up whenever input and output are made again.
    unsigned version = 0;
    void* inputMemory = nullptr;
    void* outputMemory = nullptr;
    int modelWidth = 0;
    int modelHeight = 0;
    bool halfInput = false;
    bool halfOutput = false;

    // For the log line every half minute.
    Clock::time_point statisticsSince = Clock::now();
    unsigned estimates = 0;
    double observedMs = 0;
    double submitMs = 0;
};

void Depth::Log(UnishadeLogLevel level, const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    host.log(&host, level, text);
}

Depth::Depth(const UnishadeHost& unishade) : host(unishade), directory(unishade.folder)
{
    const fs::path model = fs::path(directory) / DEPTH_MODEL_FILE;
    std::error_code error;
    const uintmax_t modelSize = fs::file_size(model, error);
    if (error)
        throw std::runtime_error("The model is missing: " + model.string() + ".");
    // Core ML keeps what it compiled for as long as the cache says, so a model file that changes needs a cache of its
    // own. ONNX Runtime keys the cache by the model's path only.
    // The file clock counts in 128 bits here, which std::to_string does not take.
    const long long modified = std::chrono::duration_cast<std::chrono::nanoseconds>(
        fs::last_write_time(model, error).time_since_epoch()).count();
    cacheRoot = fs::path(directory) / "CoreML";
    cache = cacheRoot / (std::to_string(modelSize) + "-" + std::to_string(modified));

    uint64_t registry = 0;
    std::memcpy(&registry, host.adapter, sizeof(registry));
    for (id<MTLDevice> candidate in MTLCopyAllDevices())
        if (candidate.registryID == registry)
            device = candidate;
    if (!device)
        throw std::runtime_error("Metal does not know the graphics card Unishade draws with.");
    queue = [device newCommandQueue];

    const std::string source = std::string("#include <metal_stdlib>\nusing namespace metal;\n#define CONSTANT constant\n") + kShaderShared + kShaders;
    NSError* compileError = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&compileError];
    if (!library)
        throw std::runtime_error(std::string("The shaders did not compile: ") +
                                 (compileError ? compileError.localizedDescription.UTF8String : "Metal gave no reason."));
    for (size_t i = 0; i < kKernelCount; ++i)
    {
        id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:kKernelNames[i]]];
        NSError* pipelineError = nil;
        pipelines[i] = function ? [device newComputePipelineStateWithFunction:function error:&pipelineError] : nil;
        if (!pipelines[i])
            throw std::runtime_error(std::string("Metal could not prepare ") + kKernelNames[i] +
                                     (pipelineError ? std::string(": ") + pipelineError.localizedDescription.UTF8String : std::string(".")));
    }
    if (!queue || pipelines[kRange].maxTotalThreadsPerThreadgroup < 256 || pipelines[kGather].maxTotalThreadsPerThreadgroup < 256)
        throw std::runtime_error("Metal cannot run the depth shaders on this graphics card.");
    compiling = std::async(std::launch::async, [device = device] { return motion::MetalEstimator::Compile(device); });

    worker = std::thread(&Depth::Worker, this);
    Log(UNISHADE_LOG_INFO, "Started with %s, on %s.", model.c_str(), device.name.UTF8String);
}

Depth::~Depth()
{
    if (worker.joinable())
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        // Waits for a run or a compile in progress, which use no memory of Unishade's.
        worker.join();
    }
}

void Depth::Worker()
{
    // The compiled models of earlier model files are never used again.
    std::error_code error;
    for (fs::directory_iterator it(cacheRoot, error), end; !error && it != end; it.increment(error))
        if (it->path() != cache)
        {
            std::error_code ignored;
            fs::remove_all(it->path(), ignored);
        }

    std::unique_ptr<DepthModel> model;
    int loadedWidth = 0, loadedHeight = 0;
    unsigned boundVersion = 0;
    for (;;)
    {
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return requested || stopping; });
            if (stopping)
                return;
            requested = false;
        }
        try
        {
            const auto started = Clock::now();
            if (width != loadedWidth || height != loadedHeight)
            {
                model.reset();
                boundVersion = 0;
                model = std::make_unique<DepthModel>();
                const fs::path sizeCache = cache / (std::to_string(width) + "x" + std::to_string(height));
                model->Load(directory, DEPTH_MODEL_FILE, width, height, sizeCache.string());
                loadedWidth = modelWidth = width;
                loadedHeight = modelHeight = height;
                halfInput = model->HalfInput();
                halfOutput = model->HalfOutput();
                Log(UNISHADE_LOG_INFO, "Model loaded: input=%dx%d, half_input=%d, half_output=%d, %.1f s.", width, height, halfInput, halfOutput,
                    std::chrono::duration<double>(Clock::now() - started).count());
            }
            if (withInput)
            {
                if (boundVersion != version)
                {
                    model->Bind(inputMemory, outputMemory);
                    boundVersion = version;
                }
                model->Run();
            }
            workerMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        }
        catch (const std::exception& e)
        {
            failure = e.what();
            failed = true;
        }
        done = true;
    }
}

// Hands the worker a model size to build, or with input, the input to run the model on.
void Depth::Request(bool runOnInput)
{
    withInput = runOnInput;
    requestedAt = Clock::now();
    slowRequestLogged = false;
    busy = true;
    {
        std::lock_guard lock(mutex);
        requested = true;
    }
    wake.notify_one();
}

id<MTLTexture> Depth::CreateTexture(MTLPixelFormat format, NSUInteger textureWidth, NSUInteger textureHeight, MTLTextureUsage usage)
{
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:textureWidth height:textureHeight mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = usage;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    if (!texture)
        throw std::runtime_error("The graphics card is out of memory.");
    return texture;
}

void Depth::ReleaseShared()
{
    input = nil;
    output = nil;
    guide = nil;
    keys = nil;
    state = nil;
    inputMemory = outputMemory = nullptr;
}

// Sizes the model to the frame's aspect ratio. What follows the model's size is made again once the model is built for
// it. What follows the frame's size is made again here when that changed, and the history starts out empty.
void Depth::Resize(uint32_t frameWidth, uint32_t frameHeight, int longest)
{
    size = longest;
    const ModelSize model = ModelSizeFor(frameWidth, frameHeight, longest);
    width = model.width;
    height = model.height;
    lastInput.reset();
    Log(UNISHADE_LOG_INFO, "Resized: source=%ux%u, model=%dx%d.", frameWidth, frameHeight, width, height);
    ReleaseShared();
    if (frameWidth == sourceWidth && frameHeight == sourceHeight)
        return;
    sourceWidth = frameWidth;
    sourceHeight = frameHeight;
    frameCopy = nil;
    previous = nil;
    frameCopy = CreateTexture(MTLPixelFormatBGRA8Unorm, frameWidth, frameHeight, MTLTextureUsageShaderRead);
    previous = CreateTexture(MTLPixelFormatBGRA8Unorm, frameWidth, frameHeight, MTLTextureUsageShaderRead);
    hasPrevious = false;
    estimator = nullptr;
    // The history is 32-bit floats, since the motion to the next estimate's frame adds up to hundreds of pixels.
    for (id<MTLTexture> __strong& texture : history)
    {
        texture = nil;
        texture = CreateTexture(MTLPixelFormatRGBA32Float, frameWidth, frameHeight, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    }
    historyEmpty = true;
    showing = false;
    // Two float4 for each group of Gather.
    const NSUInteger groups = ((frameWidth + kFitStep * 16 - 1) / (kFitStep * 16)) * ((frameHeight + kFitStep * 16 - 1) / (kFitStep * 16));
    sums = nil;
    sums = [device newBufferWithLength:2 * groups * 4 * sizeof(float) options:MTLResourceStorageModePrivate];
    if (!sums)
        throw std::runtime_error("The graphics card is out of memory.");
}

// Makes what follows the model's size, once the model is built for it.
void Depth::CreateShared()
{
    const NSUInteger pixels = static_cast<NSUInteger>(width) * height;
    input = [device newBufferWithLength:3 * pixels * (halfInput ? 2 : 4) options:MTLResourceStorageModeShared];
    output = [device newBufferWithLength:pixels * (halfOutput ? 2 : 4) options:MTLResourceStorageModeShared];
    keys = [device newBufferWithLength:kKeyCount * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    // The anchor, then the fit.
    state = [device newBufferWithLength:8 * sizeof(float) options:MTLResourceStorageModeShared];
    if (!input || !output || !keys || !state)
    {
        ReleaseShared();
        throw std::runtime_error("The graphics card is out of memory.");
    }
    guide = CreateTexture(MTLPixelFormatRGBA8Unorm, width, height, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    std::memset(keys.contents, 0, keys.length);
    std::memset(state.contents, 0, state.length);
    publishedAt.reset();
    inputMemory = input.contents;
    outputMemory = output.contents;
    ++version;
}

// What the kernels are told. relax is for Fit and share for Accumulate.
Sizes Depth::SizesFor(bool restart, float relax, float share) const
{
    return { { sourceWidth, sourceHeight }, { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }, halfInput, halfOutput, restart, relax,
             showing, share, estimator != nullptr };
}

void Depth::Dispatch(id<MTLComputeCommandEncoder> encoder, Kernel kernel, NSUInteger threadsWide, NSUInteger threadsHigh, NSUInteger group)
{
    [encoder setComputePipelineState:pipelines[kernel]];
    [encoder dispatchThreadgroups:MTLSizeMake((threadsWide + group - 1) / group, (threadsHigh + group - 1) / group, 1)
            threadsPerThreadgroup:MTLSizeMake(group, group, 1)];
}

void Depth::Far(id<MTLCommandBuffer> commands)
{
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setTexture:depth atIndex:2];
    Dispatch(encoder, kFar, depth.width, depth.height, 16);
    [encoder endEncoding];
}

// Moves the history along the picture's motion from the frame before, which the estimator finds once its kernels are
// compiled. Until then the history stands still and takes each estimate whole, as if every estimate were of the frame
// it lands in.
void Depth::Follow(id<MTLCommandBuffer> commands, bool restart)
{
    if (!motionPipelines && compiling.valid() && compiling.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        // Depth goes on without following motion when Metal cannot compile the estimator, rather than stopping.
        try
        {
            motionPipelines = compiling.get();
        }
        catch (const std::exception& e)
        {
            Log(UNISHADE_LOG_ERROR, "Depth does not follow motion: %s", e.what());
        }
    }
    if (!estimator && motionPipelines)
        estimator = std::make_unique<motion::MetalEstimator>(device, *motionPipelines, sourceWidth, sourceHeight);
    // The first frame has none before it, so it is compared with itself and has no motion.
    if (estimator)
        estimator->Estimate(commands, color, hasPrevious ? previous : color, 1);
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sourceWidth, sourceHeight, 1)
                toTexture:previous destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    hasPrevious = true;

    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (historyEmpty)
    {
        historyEmpty = false;
        [encoder setTexture:history[historyIndex] atIndex:6];
        Dispatch(encoder, kClear, sourceWidth, sourceHeight, 16);
    }
    const Sizes values = SizesFor(restart || !estimator, 0, 0);
    [encoder setBytes:&values length:sizeof(values) atIndex:0];
    [encoder setTexture:depth atIndex:2];
    // Warp reads no motion before there is any, but every texture a kernel takes is bound.
    [encoder setTexture:estimator ? estimator->motion : history[historyIndex] atIndex:3];
    [encoder setTexture:estimator ? estimator->confidence : history[historyIndex] atIndex:4];
    [encoder setTexture:history[historyIndex] atIndex:5];
    [encoder setTexture:history[1 - historyIndex] atIndex:6];
    Dispatch(encoder, kWarp, sourceWidth, sourceHeight, 16);
    [encoder endEncoding];
    historyIndex = 1 - historyIndex;
}

// Blends the model's output into the history. The frame copy and guide still hold the frame it was estimated from,
// since the next one is only copied after this.
void Depth::Publish(id<MTLCommandBuffer> commands, bool restart)
{
    const auto now = Clock::now();
    const float seconds = publishedAt ? std::chrono::duration<float>(now - *publishedAt).count() : 0;
    const Sizes values = SizesFor(restart || !estimator, publishedAt ? FitRelax(seconds) : 1, HistoryShare(seconds));
    publishedAt = now;
    const NSUInteger pixels = static_cast<NSUInteger>(width) * height;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setBytes:&values length:sizeof(values) atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:3];
    [encoder setBuffer:output offset:0 atIndex:4];
    [encoder setBuffer:keys offset:0 atIndex:5];
    [encoder setBuffer:state offset:0 atIndex:6];
    [encoder setBuffer:sums offset:0 atIndex:7];
    [encoder setTexture:frameCopy atIndex:0];
    [encoder setTexture:guide atIndex:1];
    [encoder setTexture:depth atIndex:2];
    [encoder setTexture:history[historyIndex] atIndex:5];
    [encoder setTexture:history[1 - historyIndex] atIndex:6];
    [encoder setComputePipelineState:pipelines[kRange]];
    [encoder dispatchThreadgroups:MTLSizeMake((pixels + 255) / 256, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    Dispatch(encoder, kHistogram, width, height, 16);
    Dispatch(encoder, kResolve, 1, 1, 1);
    Dispatch(encoder, kGather, (sourceWidth + kFitStep - 1) / kFitStep, (sourceHeight + kFitStep - 1) / kFitStep, 16);
    Dispatch(encoder, kFit, 1, 1, 1);
    Dispatch(encoder, kAccumulate, sourceWidth, sourceHeight, 16);
    [encoder endEncoding];
    historyIndex = 1 - historyIndex;
    showing = true;
}

void Depth::LogState()
{
    const double pendingMs = busy ? std::chrono::duration<double, std::milli>(Clock::now() - requestedAt).count() : 0;
    Log(UNISHADE_LOG_INFO, "State: busy=%d, worker_done=%d, pending_ms=%.2f, worker_ms=%.2f, with_input=%d, source=%ux%u, model=%dx%d, resource_version=%u.",
        busy.load(), done.load(), pendingMs, workerMs.load(), withInput, sourceWidth, sourceHeight, width, height, version);
}

// Unishade's images, again whenever it makes them again. The images it made are empty.
void Depth::Open(const UnishadeFrame& frame)
{
    color = nil;
    depth = nil;
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:frame.width
                                                                                   height:frame.height mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    color = [device newTextureWithDescriptor:desc iosurface:static_cast<IOSurfaceRef>(frame.color.handle) plane:0];
    desc.pixelFormat = MTLPixelFormatR32Float;
    desc.usage = MTLTextureUsageShaderWrite;
    depth = [device newTextureWithDescriptor:desc iosurface:static_cast<IOSurfaceRef>(frame.depth.handle) plane:0];
    if (!color || !depth)
        throw std::runtime_error("Metal could not open the images Unishade shares.");
    generation = frame.generation;
    lastInput.reset();
}

void Depth::Frame(const UnishadeFrame& frame)
{
    if (!frame.color.handle || !frame.depth.handle)
        throw std::runtime_error("Unishade gave it no picture or no depth to write.");
    if (frame.color.format != UNISHADE_FORMAT_BGRA8)
        throw std::runtime_error("Unishade gave it a picture in a format it does not take.");
    @autoreleasepool
    {
        const bool opened = frame.generation != generation;
        if (opened)
            Open(frame);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (opened)
            Far(commands);
        Update(commands, frame, opened);
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError)
        {
            inputMade = false;
            throw std::runtime_error(std::string("Metal failed: ") + commands.error.localizedDescription.UTF8String);
        }
        if (inputMade)
        {
            inputMade = false;
            Request(true);
        }
    }
}

// Moves depth along the picture's motion in every frame that changed, estimates depth for the frame on the worker and
// blends the newest result in. Frames that arrive while an estimate is in progress are not estimated. The same picture
// again still blends in a completed result. Depth is written to DEPTH whenever it changed, or the image is new.
void Depth::Update(id<MTLCommandBuffer> commands, const UnishadeFrame& frame, bool opened)
{
    bool landed = false;
    if (busy)
    {
        if (!slowRequestLogged && Clock::now() - requestedAt >= std::chrono::seconds(2))
        {
            slowRequestLogged = true;
            LogState();
        }
        if (done)
        {
            if (failed)
                throw std::runtime_error(failure);
            done = false;
            busy = false;
            landed = withInput && input;
        }
    }
    if (Clock::now() - statisticsSince >= kStatisticsInterval)
    {
        if (estimates)
            Log(UNISHADE_LOG_INFO, "%u estimates in %lld s, %.1f ms each from frame to result, %.1f ms in the model.", estimates,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - statisticsSince).count()), observedMs / estimates,
                submitMs / estimates);
        statisticsSince = Clock::now();
        estimates = 0;
        observedMs = submitMs = 0;
    }

    const int longest = LongestSide(frame);
    const bool resized = frame.width != sourceWidth || frame.height != sourceHeight || size != longest;
    const bool moved = opened || !lastPicture || *lastPicture != frame.picture;
    lastPicture = frame.picture;
    if (moved)
    {
        // A picture that stood still past its settling is not estimated, and its history is as good as just blended
        // into, so the first estimate once it moves again blends in as one that came soon after the last would.
        if (publishedAt && Clock::now() - changedAt >= kSettleTime)
            publishedAt = Clock::now();
        changedAt = Clock::now();
    }
    // What the worker reads is only made again while it is idle. Depth still follows the picture meanwhile.
    if (busy)
    {
        if (moved && frame.width == sourceWidth && frame.height == sourceHeight)
            Follow(commands, false);
        return;
    }
    // An estimate that lands after a resize is of the size before, so it is dropped.
    if (resized)
    {
        Resize(frame.width, frame.height, longest);
        landed = false;
    }
    // The worker builds the model for each size, which tells what types it takes. Core ML compiles it meanwhile, which
    // can take seconds the first time.
    const bool modelReady = modelWidth == width && modelHeight == height;
    if (modelReady && !input)
        CreateShared();
    const bool estimate = modelReady && (!lastInput || *lastInput != frame.picture || Clock::now() - changedAt < kSettleTime);
    // A picture that is estimated again without changing still has the history's motion counted from it.
    if (moved || (estimate && !landed))
        Follow(commands, estimate && !landed);
    if (landed)
    {
        Publish(commands, estimate);
        ++estimates;
        observedMs += std::chrono::duration<double, std::milli>(Clock::now() - requestedAt).count();
        submitMs += workerMs.load();
    }
    if (!modelReady)
    {
        Request(false);
        return;
    }
    if (!estimate)
        return;

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sourceWidth, sourceHeight, 1)
                toTexture:frameCopy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    const Sizes values = SizesFor(false, 0, 0);
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setBytes:&values length:sizeof(values) atIndex:0];
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder setBuffer:input offset:0 atIndex:2];
    [encoder setTexture:frameCopy atIndex:0];
    [encoder setTexture:guide atIndex:1];
    Dispatch(encoder, kPreprocess, width, height, 16);
    [encoder endEncoding];
    inputMade = true;
    lastInput = frame.picture;
}

void Fill(char* error, size_t errorSize, const std::string& message)
{
    if (error && errorSize)
        std::snprintf(error, errorSize, "%s", message.c_str());
}

void* Create(const UnishadeHost* host, char* error, size_t errorSize)
{
    try
    {
        @autoreleasepool
        {
            return new Depth(*host);
        }
    }
    catch (const std::exception& e)
    {
        Fill(error, errorSize, e.what());
    }
    return nullptr;
}

bool RunFrame(void* addon, const UnishadeFrame* frame, char* error, size_t errorSize)
{
    try
    {
        static_cast<Depth*>(addon)->Frame(*frame);
        return true;
    }
    catch (const std::exception& e)
    {
        Fill(error, errorSize, e.what());
    }
    return false;
}

void Destroy(void* addon)
{
    delete static_cast<Depth*>(addon);
}

const UnishadeAddon kAddon = {
    sizeof(UnishadeAddon),
    UNISHADE_API_VERSION,
    UNISHADE_WRITES_DEPTH,
    static_cast<uint32_t>(std::size(kSettings)),
    kSettings,
    sizeof(UnishadeSetting),
    Create,
    Destroy,
    RunFrame,
};
} // namespace

extern "C" UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void)
{
    return &kAddon;
}
