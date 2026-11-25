// Depth estimation as a Unishade add-on on macOS, Depth.dylib, as depth_addon.cpp is on Windows. Depth Anything V2 runs
// through ONNX Runtime on Core ML (depth_model.cpp) on a worker thread. Metal on the graphics card Unishade draws with
// takes the frame Unishade shares, makes the model's input from it and turns the model's output into DEPTH. The model
// can take longer than a frame: frames that arrive meanwhile are skipped, and DEPTH keeps the last estimate. Unishade
// needs the add-on's own work on the graphics card done when frame returns, so each frame waits for it. That is the
// box filter and the upsample, never the model.
//
// The files it needs are in its folder with it: depth-anything-v2-small.onnx and libonnxruntime.dylib. The models Core
// ML compiles from it go in CoreML there.

#include "depth_model.h"
#include "depth_shared.h"
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
    float Blend; // how far the smoothed anchor moves towards this estimate's
};

float Load(constant Sizes& sizes, device const float* result, device const half* halfResult, uint index)
{
    return sizes.HalfOutput ? float(halfResult[index]) : result[index];
}

// Box-filters the frame down to the model size and writes planar, ImageNet-normalized RGB, and the colors themselves
// for Upsample.
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

// Counts the output's values between the lowest and the highest, and the picture's pixels by brightness in each
// section.
kernel void Histogram(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], device const float* result [[buffer(3)]],
                      device const half* halfResult [[buffer(4)]], device atomic_uint* keys [[buffer(5)]],
                      texture2d<float, access::read> guide [[texture(1)]])
{
    if (any(id >= sizes.ModelSize))
        return;
    float low = Unordered(~atomic_load_explicit(&keys[KeyLowest], memory_order_relaxed));
    float high = Unordered(atomic_load_explicit(&keys[KeyHighest], memory_order_relaxed));
    float value = Load(sizes, result, halfResult, id.y * sizes.ModelSize.x + id.x);
    uint bin = min(uint(saturate((value - low) / max(high - low, 1e-20f)) * float(AnchorBins)), AnchorBins - 1u);
    atomic_fetch_add_explicit(&keys[KeyBins + bin], 1u, memory_order_relaxed);
    uint2 cell = min(id * 3u / sizes.ModelSize, uint2(2u));
    uint level = min(uint(dot(guide.read(id).rgb, Luma) * float(SectionLevels)), SectionLevels - 1u);
    atomic_fetch_add_explicit(&keys[KeySections + (cell.y * 3u + cell.x) * SectionLevels + level], 1u, memory_order_relaxed);
}

// Finds the output's anchor in the counts and moves the one kept in state towards it, or takes it as it is after a
// cut. State starts out zero, until z marks it as set. Leaves the counts zero for the next estimate, and the sections'
// for it to tell a cut by.
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
    uint changed = 0;
    for (uint section = 0; section < 9; ++section)
    {
        uint pixels = 0;
        uint moved = 0;
        for (uint level = 0; level < SectionLevels; ++level)
        {
            uint index = section * SectionLevels + level;
            uint count = keys[KeySections + index];
            uint before = keys[KeyPrevious + index];
            pixels += count;
            moved += max(count, before) - min(count, before);
            keys[KeyPrevious + index] = count;
            keys[KeySections + index] = 0;
        }
        // Every pixel that left a level arrived at another, so each is counted twice.
        if (float(moved) > 2 * CutShare * float(pixels))
            ++changed;
    }
    keys[KeyHighest] = 0;
    keys[KeyLowest] = 0;
    float4 current = state[0];
    bool cut = changed >= CutSections;
    if (current.z == 0 || cut)
        current.x = anchor;
    state[0] = float4(current.x + (anchor - current.x) * sizes.Blend, cut ? 1 : 0, 1, 0);
}

// Upsamples the output to the frame's size with a joint bilateral filter, as Weight says.
kernel void Upsample(uint2 id [[thread_position_in_grid]], constant Sizes& sizes [[buffer(0)]], device const float* result [[buffer(3)]],
                     device const half* halfResult [[buffer(4)]], device const float4* state [[buffer(6)]],
                     texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::read> guide [[texture(1)]],
                     texture2d<float, access::write> depth [[texture(2)]])
{
    // An estimate can land after Unishade made its images again at another size.
    if (any(id >= sizes.SourceSize) || any(id >= uint2(depth.get_width(), depth.get_height())))
        return;
    float3 color = source.read(id).rgb;
    float2 position = (float2(id) + 0.5f) * float2(sizes.ModelSize) / float2(sizes.SourceSize) - 0.5f;
    int2 corner = int2(floor(position)) - 1;
    float sum = 0;
    float weights = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            int2 texel = corner + int2(x, y);
            float2 offset = float2(texel) - position;
            texel = clamp(texel, int2(0), int2(sizes.ModelSize) - 1);
            float3 difference = guide.read(uint2(texel)).rgb - color;
            float weight = Weight(offset, difference);
            sum += weight * Load(sizes, result, halfResult, uint(texel.y) * sizes.ModelSize.x + uint(texel.x));
            weights += weight;
        }
    depth.write(float4(StoredDepth(sum / weights / max(state[0].x, 1e-6f))), id);
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
    kUpsample,
    kFar,
    kKernelCount,
};
constexpr const char* kKernelNames[] = { "Preprocess", "Range", "Histogram", "Resolve", "Upsample", "Far" };

// As Sizes in the shaders.
struct Sizes
{
    uint32_t sourceSize[2];
    uint32_t modelSize[2];
    uint32_t halfInput;
    uint32_t halfOutput;
    float blend;
    uint32_t unused;
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
    void Update(id<MTLCommandBuffer> commands, const UnishadeFrame& frame);
    void Resize(uint32_t frameWidth, uint32_t frameHeight, int longest);
    void CreateShared(id<MTLCommandBuffer> commands);
    void ReleaseShared();
    void Far(id<MTLCommandBuffer> commands);
    void Publish(id<MTLCommandBuffer> commands);
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

    // Made for each frame and model size. The model reads input and writes output where the processor reaches them.
    id<MTLBuffer> input = nil;
    id<MTLBuffer> output = nil;
    id<MTLTexture> guide = nil;
    id<MTLBuffer> keys = nil;
    id<MTLBuffer> state = nil;
    Sizes sizes{};

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
        throw std::runtime_error(std::string("The shaders did not compile: ") + compileError.localizedDescription.UTF8String);
    for (size_t i = 0; i < kKernelCount; ++i)
    {
        id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:kKernelNames[i]]];
        NSError* pipelineError = nil;
        pipelines[i] = function ? [device newComputePipelineStateWithFunction:function error:&pipelineError] : nil;
        if (!pipelines[i])
            throw std::runtime_error(std::string("Metal could not prepare ") + kKernelNames[i] +
                                     (pipelineError ? std::string(": ") + pipelineError.localizedDescription.UTF8String : std::string(".")));
    }
    if (!queue || pipelines[kRange].maxTotalThreadsPerThreadgroup < 256)
        throw std::runtime_error("Metal cannot run the depth shaders on this graphics card.");

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
// it.
void Depth::Resize(uint32_t frameWidth, uint32_t frameHeight, int longest)
{
    size = longest;
    const ModelSize model = ModelSizeFor(frameWidth, frameHeight, longest);
    width = model.width;
    height = model.height;
    sourceWidth = frameWidth;
    sourceHeight = frameHeight;
    lastInput.reset();
    Log(UNISHADE_LOG_INFO, "Resized: source=%ux%u, model=%dx%d.", frameWidth, frameHeight, width, height);
    ReleaseShared();
    frameCopy = nil;
    frameCopy = CreateTexture(MTLPixelFormatBGRA8Unorm, frameWidth, frameHeight, MTLTextureUsageShaderRead);
}

// Makes what follows the model's size, once the model is built for it. DEPTH starts out at the far plane until the
// first estimate arrives.
void Depth::CreateShared(id<MTLCommandBuffer> commands)
{
    const NSUInteger pixels = static_cast<NSUInteger>(width) * height;
    input = [device newBufferWithLength:3 * pixels * (halfInput ? 2 : 4) options:MTLResourceStorageModeShared];
    output = [device newBufferWithLength:pixels * (halfOutput ? 2 : 4) options:MTLResourceStorageModeShared];
    keys = [device newBufferWithLength:kKeyCount * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    state = [device newBufferWithLength:4 * sizeof(float) options:MTLResourceStorageModeShared];
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
    sizes = { { sourceWidth, sourceHeight }, { static_cast<uint32_t>(width), static_cast<uint32_t>(height) }, halfInput, halfOutput, 0, 0 };
    Far(commands);
}

void Depth::Far(id<MTLCommandBuffer> commands)
{
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipelines[kFar]];
    [encoder setTexture:depth atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake((depth.width + 15) / 16, (depth.height + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [encoder endEncoding];
}

// Turns the model's output into DEPTH. The frame copy and guide still hold the frame it was estimated from, since the
// next one is only copied after this.
void Depth::Publish(id<MTLCommandBuffer> commands)
{
    const auto now = Clock::now();
    sizes.blend = publishedAt ? AnchorBlend(std::chrono::duration<float>(now - *publishedAt).count()) : 1;
    publishedAt = now;
    const NSUInteger pixels = static_cast<NSUInteger>(width) * height;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setBytes:&sizes length:sizeof(sizes) atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:3];
    [encoder setBuffer:output offset:0 atIndex:4];
    [encoder setBuffer:keys offset:0 atIndex:5];
    [encoder setBuffer:state offset:0 atIndex:6];
    [encoder setTexture:frameCopy atIndex:0];
    [encoder setTexture:guide atIndex:1];
    [encoder setTexture:depth atIndex:2];
    [encoder setComputePipelineState:pipelines[kRange]];
    [encoder dispatchThreadgroups:MTLSizeMake((pixels + 255) / 256, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [encoder setComputePipelineState:pipelines[kHistogram]];
    [encoder dispatchThreadgroups:MTLSizeMake((width + 15) / 16, (height + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [encoder setComputePipelineState:pipelines[kResolve]];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder setComputePipelineState:pipelines[kUpsample]];
    [encoder dispatchThreadgroups:MTLSizeMake((sourceWidth + 15) / 16, (sourceHeight + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [encoder endEncoding];
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
        Update(commands, frame);
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

// Estimates depth for the frame on the worker and publishes the newest result to DEPTH. Frames that arrive while an
// estimate is in progress are skipped. The same picture again still publishes a completed result.
void Depth::Update(id<MTLCommandBuffer> commands, const UnishadeFrame& frame)
{
    if (busy)
    {
        if (!slowRequestLogged && Clock::now() - requestedAt >= std::chrono::seconds(2))
        {
            slowRequestLogged = true;
            LogState();
        }
        if (!done)
            return;
        if (failed)
            throw std::runtime_error(failure);
        done = false;
        busy = false;
        if (withInput && input)
        {
            Publish(commands);
            ++estimates;
            observedMs += std::chrono::duration<double, std::milli>(Clock::now() - requestedAt).count();
            submitMs += workerMs.load();
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
    if (frame.width != sourceWidth || frame.height != sourceHeight || size != longest)
        Resize(frame.width, frame.height, longest);
    if (lastInput && *lastInput == frame.picture)
        return;
    // The worker builds the model for each size, which tells what types it takes. Core ML compiles it meanwhile, which
    // can take seconds the first time.
    if (modelWidth != width || modelHeight != height)
    {
        Request(false);
        return;
    }
    if (!input)
        CreateShared(commands);

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sourceWidth, sourceHeight, 1)
                toTexture:frameCopy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipelines[kPreprocess]];
    [encoder setBytes:&sizes length:sizeof(sizes) atIndex:0];
    [encoder setBuffer:input offset:0 atIndex:1];
    [encoder setBuffer:input offset:0 atIndex:2];
    [encoder setTexture:frameCopy atIndex:0];
    [encoder setTexture:guide atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake((width + 15) / 16, (height + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
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
