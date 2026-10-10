#include "depth.h"
#include "depth_model.h"
#include "../addon.h"
#include "../capture.h"
#include "../config.h"
#include "../log.h"
#include "../reshade_imgui.h"
#include "../state.h"

#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr wchar_t kModelFile[] = L"depth-anything-v2-small.onnx";
// The model's input and output go between the D3D11 and D3D12 devices in textures, since only textures can be
// shared between them, and DirectML takes buffers. The textures hold the values in order, in rows of this many bytes,
// so D3D12 copies them to and from packed buffers. Rows this wide keep the largest input under 16384 of them.
constexpr UINT kRowBytes = 16 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

// Preprocess turns the frame into the model's input. Peak, Smooth and Upsample turn its output into ReShade's DEPTH.
constexpr char kShaders[] = R"(
cbuffer Sizes : register(b0)
{
    uint2 SourceSize;
    uint2 ModelSize;  // of the model's input and output
    uint InputWidth;  // values in a row of the input texture
    uint OutputWidth; // and of the output texture
};
Texture2D<float4> Source : register(t0);
Texture2D<float> Result : register(t1);
StructuredBuffer<float4> Smoothed : register(t2);
Texture2D<float4> Guide : register(t3);
RWTexture2D<float> Input : register(u0);
RWByteAddressBuffer Keys : register(u1);
RWStructuredBuffer<float4> State : register(u2);
RWTexture2D<float> Depth : register(u3);
RWTexture2D<float4> GuideTarget : register(u4);

static const float3 Mean = float3(0.485, 0.456, 0.406);
static const float3 Std = float3(0.229, 0.224, 0.225);
// Blend factor for the depth scale, so the whole image does not pulse when something close enters or leaves the view.
static const float PeakSmoothing = 0.2;
// Default RESHADE_DEPTH_LINEARIZATION_FAR_PLANE, so no preprocessor changes are needed in ReShade.
static const float FarPlane = 1000.0;
// The model only knows how far things are relative to each other. The nearest thing in view is put at this fraction
// of the far plane, and anything more than 1 / Nearest times farther than it on the far plane.
static const float Nearest = 0.01;
// How far, in model pixels and in color, an output value reaches when upsampled.
static const float SpatialSigma = 1.0;
static const float ColorSigma = 0.1;

void Store(uint index, float value)
{
    Input[uint2(index % InputWidth, index / InputWidth)] = value;
}

float Load(uint index)
{
    return Result.Load(int3(index % OutputWidth, index / OutputWidth, 0));
}

// Box-filters the frame down to the model size and writes planar, ImageNet-normalized RGB, and the colors themselves
// for Upsample.
[numthreads(16, 16, 1)]
void Preprocess(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= ModelSize))
        return;
    uint2 begin = id.xy * SourceSize / ModelSize;
    uint2 end = max((id.xy + 1) * SourceSize / ModelSize, begin + 1);
    float3 sum = 0;
    for (uint y = begin.y; y < end.y; ++y)
        for (uint x = begin.x; x < end.x; ++x)
            sum += Source.Load(int3(x, y, 0)).rgb;
    float3 average = sum / ((end.x - begin.x) * (end.y - begin.y));
    GuideTarget[id.xy] = float4(average, 1);
    float3 color = (average - Mean) / Std;
    uint plane = ModelSize.x * ModelSize.y;
    uint index = id.y * ModelSize.x + id.x;
    Store(index, color.r);
    Store(index + plane, color.g);
    Store(index + 2 * plane, color.b);
}

// Floats as unsigned numbers in the same order, for atomics.
uint Ordered(float value)
{
    uint bits = asuint(value);
    return bits & 0x80000000 ? ~bits : bits | 0x80000000;
}

float Unordered(uint key)
{
    return asfloat(key & 0x80000000 ? key & 0x7FFFFFFF : ~key);
}

groupshared float Peaks[256];

// Finds the highest value of the output, the nearest point. Keys starts out zero, below every key.
[numthreads(256, 1, 1)]
void Peak(uint3 id : SV_DispatchThreadID, uint thread : SV_GroupIndex)
{
    Peaks[thread] = Load(min(id.x, ModelSize.x * ModelSize.y - 1));
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 128; step > 0; step >>= 1)
    {
        if (thread < step)
            Peaks[thread] = max(Peaks[thread], Peaks[thread + step]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread == 0)
    {
        uint previous;
        Keys.InterlockedMax(0, Ordered(Peaks[0]), previous);
    }
}

// Moves the peak kept in State towards the output's. State starts out zero, until z marks it as set.
[numthreads(1, 1, 1)]
void Smooth()
{
    float peak = Unordered(Keys.Load(0));
    float4 state = State[0];
    if (state.z == 0)
        state.x = peak;
    State[0] = float4(state.x + (peak - state.x) * PeakSmoothing, 0, 1, 0);
}

// Upsamples the output to the frame's size with a joint bilateral filter: each nearby output value counts by how close
// the color it was estimated from is to this pixel's, so depth edges follow the frame's edges instead of blurring
// across them. Values that match no color still count a little, by distance alone.
[numthreads(16, 16, 1)]
void Upsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= SourceSize))
        return;
    float3 color = Source.Load(int3(id.xy, 0)).rgb;
    float2 position = (id.xy + 0.5) * ModelSize / SourceSize - 0.5;
    int2 corner = int2(floor(position)) - 1;
    float sum = 0;
    float weights = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            int2 texel = corner + int2(x, y);
            float2 offset = texel - position;
            texel = clamp(texel, 0, int2(ModelSize) - 1);
            float3 difference = Guide.Load(int3(texel, 0)).rgb - color;
            float weight = exp(-dot(offset, offset) / (2 * SpatialSigma * SpatialSigma)) *
                           (exp(-dot(difference, difference) / (2 * ColorSigma * ColorSigma)) + 1e-3);
            sum += weight * Load(texel.y * ModelSize.x + texel.x);
            weights += weight;
        }

    // The output is inverse depth, so distance goes with its reciprocal, which also keeps flat surfaces flat. It is
    // stored as the non-linear depth ReShade linearizes by default: linear = z / (far - z * (far - 1)), solved for z.
    float closeness = sum / weights / max(Smoothed[0].x, 1e-6);
    float distance01 = Nearest / max(closeness, Nearest);
    Depth[id.xy] = distance01 * FarPlane / (1 + distance01 * (FarPlane - 1));
}
)";

constexpr const char* kEntryPoints[] = { "Preprocess", "Peak", "Smooth", "Upsample" };

struct Depth
{
    bool enabled = false;
    std::wstring directory;

    int width = 0;  // model input and output size, follows the frame's aspect ratio
    int height = 0;
    int size = 0;   // their longest side, as picked in the menu's Settings
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    std::optional<int64_t> lastInput;
    FrameStatistics::Clock::time_point requestedAt{};
    std::atomic<double> workerMs = 0;
    bool slowRequestLogged = false;
    // Compiled once, so the shaders can be made again on a new device.
    std::array<winrt::com_ptr<ID3DBlob>, std::size(kEntryPoints)> code;
    std::array<winrt::com_ptr<ID3D11ComputeShader>, std::size(kEntryPoints)> shaders;
    winrt::com_ptr<ID3D11Buffer> sizes;
    winrt::com_ptr<ID3D11Texture2D> frameCopy;
    winrt::com_ptr<ID3D11ShaderResourceView> frameView;

    // Made for each frame and model size. The input and output textures are shared with the D3D12 device, like the
    // fence.
    winrt::com_ptr<ID3D11Texture2D> input;
    winrt::com_ptr<ID3D11UnorderedAccessView> inputTarget;
    winrt::com_ptr<ID3D11Texture2D> guide;
    winrt::com_ptr<ID3D11UnorderedAccessView> guideTarget;
    winrt::com_ptr<ID3D11ShaderResourceView> guideView;
    winrt::com_ptr<ID3D11Texture2D> output;
    winrt::com_ptr<ID3D11ShaderResourceView> outputView;
    winrt::com_ptr<ID3D11Buffer> keys;
    winrt::com_ptr<ID3D11UnorderedAccessView> keysTarget;
    winrt::com_ptr<ID3D11Buffer> state;
    winrt::com_ptr<ID3D11UnorderedAccessView> stateTarget;
    winrt::com_ptr<ID3D11ShaderResourceView> stateView;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11UnorderedAccessView> textureTarget;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    winrt::com_ptr<ID3D11Fence> fence;
    // The fence value the last output is ready at. Each run takes the next two, with its input ready at the first.
    uint64_t fenceValue = 0;
    std::vector<reshade::api::effect_runtime*> runtimes;

    // ReShade wraps every D3D12 device created in this process in a proxy, and DirectML fails on the
    // proxy with DXGI_ERROR_DEVICE_REMOVED. The model runs on the native device ReShade reports. The
    // proxy is kept so ReShade releases the native device last.
    winrt::com_ptr<ID3D12Device> d3d12Proxy;
    winrt::com_ptr<ID3D12Device> d3d12;
    winrt::com_ptr<ID3D12Fence> d3d12Fence;
    HANDLE fenceHandle = nullptr;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;

    // The worker owns the model and the D3D12 side. While busy is set it reads the fields below and writes the model's
    // size and types, and the main thread only touches them while it is clear.
    std::thread worker;
    HANDLE request = nullptr;
    std::atomic<bool> stopping = false;
    std::atomic<bool> busy = false;
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    // The fence value the input is ready at, or 0 to only build the model for width and height.
    uint64_t inputValue = 0;
    // Goes up whenever the shared textures are made again, with their handles.
    unsigned version = 0;
    HANDLE inputHandle = nullptr;
    HANDLE outputHandle = nullptr;
    int modelWidth = 0;
    int modelHeight = 0;
    bool halfInput = false;
    bool halfOutput = false;
};
Depth d;

// D3D12 makes one device per adapter, and ReShade only reports the native device when it is first made, which
// another add-on, such as the DLSS5 one, may have done before the worker. So every native device is kept, by
// init_device and destroy_device, which can come from any thread.
std::mutex nativeMutex;
std::vector<ID3D12Device*> nativeDevices;

// Binds view to DEPTH, or unbinds it when view is null.
void Bind(reshade::api::effect_runtime* runtime, ID3D11ShaderResourceView* view)
{
    const reshade::api::resource_view handle{ reinterpret_cast<uint64_t>(view) };
    runtime->update_texture_bindings("DEPTH", handle, handle);
    runtime->enumerate_uniform_variables(nullptr, [ready = view != nullptr](reshade::api::effect_runtime* runtime, reshade::api::effect_uniform_variable variable) {
        char source[32];
        if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
            runtime->set_uniform_value_bool(variable, ready);
    });
}

void OnInitRuntime(reshade::api::effect_runtime* runtime)
{
    d.runtimes.push_back(runtime);
    if (d.view)
        Bind(runtime, d.view.get());
}

void OnDestroyRuntime(reshade::api::effect_runtime* runtime)
{
    d.runtimes.erase(std::remove(d.runtimes.begin(), d.runtimes.end(), runtime), d.runtimes.end());
}

void OnReloadedEffects(reshade::api::effect_runtime* runtime)
{
    // Effect textures were recreated, and the generic depth add-on has just bound nothing to DEPTH.
    if (d.view)
        Bind(runtime, d.view.get());
}

void OnInitDevice(reshade::api::device* device)
{
    if (device->get_api() != reshade::api::device_api::d3d12)
        return;
    const std::lock_guard lock(nativeMutex);
    nativeDevices.push_back(reinterpret_cast<ID3D12Device*>(device->get_native()));}

void OnDestroyDevice(reshade::api::device* device)
{
    if (device->get_api() != reshade::api::device_api::d3d12)
        return;
    const std::lock_guard lock(nativeMutex);
    std::erase(nativeDevices, reinterpret_cast<ID3D12Device*>(device->get_native()));
}

// Makes the device the model runs on, the fence shared with the D3D11 device, and what copies the model's input and
// output. Runs on the worker thread.
void CreateD3D12()
{
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d.d3d12Proxy.put()))))
        throw std::runtime_error("DirectX 12 is unavailable on this GPU.");
    const LUID adapter = d.d3d12Proxy->GetAdapterLuid();
    {
        const std::lock_guard lock(nativeMutex);
        for (ID3D12Device* native : nativeDevices)
            if (const LUID luid = native->GetAdapterLuid(); luid.LowPart == adapter.LowPart && luid.HighPart == adapter.HighPart)
                d.d3d12.copy_from(native);
    }
    // Without ReShade's hooks there is no proxy and the created device is the native one.
    if (!d.d3d12)
        d.d3d12 = d.d3d12Proxy;

    winrt::check_hresult(d.d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d.d3d12Fence.put())));
    winrt::check_hresult(d.d3d12->CreateSharedHandle(d.d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &d.fenceHandle));
    winrt::check_hresult(d.d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(d.allocator.put())));
    winrt::check_hresult(d.d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocator.get(), nullptr, IID_PPV_ARGS(d.list.put())));
    winrt::check_hresult(d.list->Close());
}

winrt::com_ptr<ID3D12Resource> CreateD3D12Buffer(UINT64 size)
{
    const D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    winrt::com_ptr<ID3D12Resource> buffer;
    winrt::check_hresult(d.d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(buffer.put())));
    return buffer;
}

// Copies the values of a shared texture into a buffer, packed, or back.
void Copy(ID3D12CommandQueue* queue, ID3D12Resource* texture, ID3D12Resource* buffer, bool toBuffer)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION textureLocation{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    textureLocation.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION bufferLocation{ buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    bufferLocation.PlacedFootprint = { 0, { desc.Format, static_cast<UINT>(desc.Width), desc.Height, 1, kRowBytes } };
    winrt::check_hresult(d.list->Reset(d.allocator.get(), nullptr));
    if (toBuffer)
        d.list->CopyTextureRegion(&bufferLocation, 0, 0, 0, &textureLocation, nullptr);
    else
    {
        d.list->CopyTextureRegion(&textureLocation, 0, 0, 0, &bufferLocation, nullptr);
        // D3D11 reads it next, which needs it back in the common state. Read-only states return there on their own.
        D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        barrier.Transition = { texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON };
        d.list->ResourceBarrier(1, &barrier);
    }
    winrt::check_hresult(d.list->Close());
    ID3D12CommandList* lists[] = { d.list.get() };
    queue->ExecuteCommandLists(1, lists);
}

void Worker()
{
    InitThreadLog();
    Log(LogLevel::Info, L"Depth worker started.");
    winrt::com_ptr<ID3D12Resource> input, output, inputBuffer, outputBuffer;
    // Declared last, so it goes first.
    std::unique_ptr<DepthModel> model;
    int loadedWidth = 0, loadedHeight = 0;
    unsigned boundVersion = 0;
    for (;;)
    {
        WaitForSingleObject(d.request, INFINITE);
        if (d.stopping)
        {
            // The last run may still be on the GPU. Its input never comes when the D3D11 device was lost, so the wait
            // is short.
            if (d.inputValue && d.d3d12Fence && d.d3d12Fence->GetCompletedValue() < d.inputValue + 1)
                if (const HANDLE finished = CreateEventW(nullptr, FALSE, FALSE, nullptr))
                {
                    if (SUCCEEDED(d.d3d12Fence->SetEventOnCompletion(d.inputValue + 1, finished)))
                        WaitForSingleObject(finished, 1000);
                    CloseHandle(finished);
                }
            return;
        }
        try
        {
            const auto started = FrameStatistics::Clock::now();
            if (!d.d3d12)
                CreateD3D12();
            if (d.width != loadedWidth || d.height != loadedHeight)
            {
                model.reset();
                boundVersion = 0;
                model = std::make_unique<DepthModel>();
                model->Load(d.directory, kModelFile, d.width, d.height, d.d3d12.get());
                loadedWidth = d.modelWidth = d.width;
                loadedHeight = d.modelHeight = d.height;
                Log(LogLevel::Info, L"Depth model loaded: %ls%ls, input=%dx%d, half_input=%d, half_output=%d.", d.directory.c_str(),
                    kModelFile, d.width, d.height, model->HalfInput(), model->HalfOutput());
                d.halfInput = model->HalfInput();
                d.halfOutput = model->HalfOutput();
                Report(LogLevel::Ok, L"Depth estimation ready (%dx%d)", d.width, d.height);
            }
            if (d.inputValue)
            {
                if (boundVersion != d.version)
                {
                    winrt::check_hresult(d.d3d12->OpenSharedHandle(d.inputHandle, IID_PPV_ARGS(input.put())));
                    winrt::check_hresult(d.d3d12->OpenSharedHandle(d.outputHandle, IID_PPV_ARGS(output.put())));
                    inputBuffer = CreateD3D12Buffer(input->GetDesc().Height * kRowBytes);
                    outputBuffer = CreateD3D12Buffer(output->GetDesc().Height * kRowBytes);
                    model->Bind(inputBuffer.get(), outputBuffer.get());
                    boundVersion = d.version;
                }
                // Returns before the GPU is done. The main thread waits for the fence.
                ID3D12CommandQueue* queue = model->Queue();
                winrt::check_hresult(queue->Wait(d.d3d12Fence.get(), d.inputValue));
                winrt::check_hresult(d.allocator->Reset());
                Copy(queue, input.get(), inputBuffer.get(), true);
                model->Run();
                Copy(queue, output.get(), outputBuffer.get(), false);
                winrt::check_hresult(queue->Signal(d.d3d12Fence.get(), d.inputValue + 1));
            }
            d.workerMs = std::chrono::duration<double, std::milli>(FrameStatistics::Clock::now() - started).count();
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Error, L"Depth estimation stopped: %hs", e.what());
            LogStackTrace();
            d.failed = true;
        }
        catch (const winrt::hresult_error& e)
        {
            Log(LogLevel::Error, L"Depth estimation stopped: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
            Log(LogLevel::Info, L"Depth worker state: model=%dx%d, requested=%dx%d, input_fence=%llu, resource_version=%u.",
                loadedWidth, loadedHeight, d.width, d.height, d.inputValue, d.version);
            LogStackTrace();
            d.failed = true;
        }
        d.done = true;
    }
}

// Hands the worker a model size to build, or with value, an input that is ready once the fence reaches it.
void Request(uint64_t value)
{
    d.inputValue = value;
    d.requestedAt = FrameStatistics::Clock::now();
    d.slowRequestLogged = false;
    d.busy = true;
    SetEvent(d.request);
}

void CreateShaders()
{
    for (size_t i = 0; i < std::size(kEntryPoints); ++i)
        winrt::check_hresult(g.device->CreateComputeShader(d.code[i]->GetBufferPointer(), d.code[i]->GetBufferSize(), nullptr, d.shaders[i].put()));

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 32;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(g.device->CreateBuffer(&desc, nullptr, d.sizes.put()));
}

void CompileShaders()
{
    for (size_t i = 0; i < std::size(kEntryPoints); ++i)
    {
        winrt::com_ptr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "depth", nullptr, nullptr, kEntryPoints[i], "cs_5_0", 0, 0, d.code[i].put(), errors.put());
        if (FAILED(hr))
            throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compute shaders are unavailable on this GPU");
    }
    CreateShaders();
}

// Runs a shader with views in their registers, t0 to t3 and u0 to u4, and unbinds them after.
void Dispatch(size_t shader, std::array<ID3D11ShaderResourceView*, 4> views, std::array<ID3D11UnorderedAccessView*, 5> targets, UINT x, UINT y)
{
    ID3D11Buffer* constants[] = { d.sizes.get() };
    g.context->CSSetShader(d.shaders[shader].get(), nullptr, 0);
    g.context->CSSetConstantBuffers(0, 1, constants);
    g.context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    g.context->CSSetUnorderedAccessViews(0, static_cast<UINT>(targets.size()), targets.data(), nullptr);
    g.context->Dispatch(x, y, 1);
    views.fill(nullptr);
    targets.fill(nullptr);
    g.context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    g.context->CSSetUnorderedAccessViews(0, static_cast<UINT>(targets.size()), targets.data(), nullptr);
}

// Unbinds DEPTH, so effects stop reading an estimate that no longer updates, and releases what is made for each frame
// and model size.
void ReleaseShared()
{
    for (auto* runtime : d.runtimes)
        Bind(runtime, nullptr);
    d.view = nullptr;
    d.textureTarget = nullptr;
    d.texture = nullptr;
    d.stateView = nullptr;
    d.stateTarget = nullptr;
    d.state = nullptr;
    d.keysTarget = nullptr;
    d.keys = nullptr;
    d.outputView = nullptr;
    d.output = nullptr;
    d.guideView = nullptr;
    d.guideTarget = nullptr;
    d.guide = nullptr;
    d.inputTarget = nullptr;
    d.input = nullptr;
    for (HANDLE* handle : { &d.inputHandle, &d.outputHandle })
        if (*handle)
            CloseHandle(std::exchange(*handle, nullptr));
}

// Releases everything made on the D3D11 device. UpdateDepth makes it again while depth is on.
void ReleaseResources()
{
    // The worker's queue waits for the input, which a lost device no longer signals.
    if (d.busy && d.inputValue && d.d3d12Fence->GetCompletedValue() < d.inputValue)
        d.d3d12Fence->Signal(d.inputValue);
    ReleaseShared();
    d.fence = nullptr;
    d.frameView = nullptr;
    d.frameCopy = nullptr;
    d.sizes = nullptr;
    for (auto& shader : d.shaders)
        shader = nullptr;
    d.sourceWidth = 0;
    d.sourceHeight = 0;
    d.lastInput.reset();
}

// Turns depth off after an error. At a size other than the default it goes back to the default instead, since the size
// may be what failed, such as the GPU running out of memory for the largest. Otherwise the menus would hide the setting
// with depth off, and it would fail the same way on every start.
void Stop()
{
    ReleaseResources();
    if (DepthSize() == kDefaultDepthSize)
    {
        d.enabled = false;
        return;
    }
    SetDepthSize(kDefaultDepthSize);
    Report(LogLevel::Warning, L"Depth estimation failed, so Depth detail went back to High.");
}

// Sizes the model to the frame's aspect ratio. What follows the model's size is made again once the model is built
// for it.
void Resize(const D3D11_TEXTURE2D_DESC& frame)
{
    const bool landscape = frame.Width >= frame.Height;
    const float ratio = landscape ? static_cast<float>(frame.Height) / frame.Width : static_cast<float>(frame.Width) / frame.Height;
    d.size = DepthSize();
    const int shortSide = std::max(14, static_cast<int>(std::lround(d.size * ratio / 14)) * 14);
    d.width = landscape ? d.size : shortSide;
    d.height = landscape ? shortSide : d.size;
    d.sourceWidth = frame.Width;
    d.sourceHeight = frame.Height;
    d.lastInput.reset();
    Log(LogLevel::Info, L"Depth resize: source=%ux%u, model=%dx%d, detail=%d.", frame.Width, frame.Height, d.width, d.height, d.size);
    ReleaseShared();

    D3D11_TEXTURE2D_DESC copy = frame;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.SampleDesc = { 1, 0 };
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    d.frameCopy = nullptr;
    d.frameView = nullptr;
    winrt::check_hresult(g.device->CreateTexture2D(&copy, nullptr, d.frameCopy.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.frameCopy.get(), nullptr, d.frameView.put()));
}

// A texture the D3D12 device opens, holding count values of the model's type in rows of kRowBytes.
void CreateSharedTexture(size_t count, bool half, UINT bind, winrt::com_ptr<ID3D11Texture2D>& texture, HANDLE& handle)
{
    const UINT width = kRowBytes / (half ? 2 : 4);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = static_cast<UINT>((count + width - 1) / width);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = half ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    winrt::check_hresult(g.device->CreateTexture2D(&desc, nullptr, texture.put()));
    winrt::check_hresult(texture.as<IDXGIResource1>()->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
}

// Makes what follows the model's size, once the model is built for it, and binds the estimate to DEPTH. It starts out
// at the far plane until the first estimate arrives.
void CreateShared()
{
    const size_t pixels = static_cast<size_t>(d.width) * d.height;
    CreateSharedTexture(3 * pixels, d.halfInput, D3D11_BIND_UNORDERED_ACCESS, d.input, d.inputHandle);
    CreateSharedTexture(pixels, d.halfOutput, D3D11_BIND_SHADER_RESOURCE, d.output, d.outputHandle);
    ++d.version;
    winrt::check_hresult(g.device->CreateUnorderedAccessView(d.input.get(), nullptr, d.inputTarget.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.output.get(), nullptr, d.outputView.put()));
    if (!d.fence)
        winrt::check_hresult(g.device.as<ID3D11Device5>()->OpenSharedFence(d.fenceHandle, IID_PPV_ARGS(d.fence.put())));

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    winrt::check_hresult(g.device->CreateBuffer(&buffer, nullptr, d.keys.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements = 4;
    raw.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    winrt::check_hresult(g.device->CreateUnorderedAccessView(d.keys.get(), &raw, d.keysTarget.put()));

    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer.StructureByteStride = 16;
    winrt::check_hresult(g.device->CreateBuffer(&buffer, nullptr, d.state.put()));
    winrt::check_hresult(g.device->CreateUnorderedAccessView(d.state.get(), nullptr, d.stateTarget.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.state.get(), nullptr, d.stateView.put()));
    const UINT zero[4] = {};
    g.context->ClearUnorderedAccessViewUint(d.stateTarget.get(), zero);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = d.width;
    desc.Height = d.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    winrt::check_hresult(g.device->CreateTexture2D(&desc, nullptr, d.guide.put()));
    winrt::check_hresult(g.device->CreateUnorderedAccessView(d.guide.get(), nullptr, d.guideTarget.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.guide.get(), nullptr, d.guideView.put()));

    // DEPTH has the frame's size, so its edges can follow the frame's.
    desc.Width = d.sourceWidth;
    desc.Height = d.sourceHeight;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    winrt::check_hresult(g.device->CreateTexture2D(&desc, nullptr, d.texture.put()));
    winrt::check_hresult(g.device->CreateUnorderedAccessView(d.texture.get(), nullptr, d.textureTarget.put()));
    winrt::check_hresult(g.device->CreateShaderResourceView(d.texture.get(), nullptr, d.view.put()));
    const float farPlane[4] = { 1, 1, 1, 1 };
    g.context->ClearUnorderedAccessViewFloat(d.textureTarget.get(), farPlane);

    const UINT sizes[8] = { d.sourceWidth, d.sourceHeight, static_cast<UINT>(d.width), static_cast<UINT>(d.height),
                            kRowBytes / (d.halfInput ? 2 : 4), kRowBytes / (d.halfOutput ? 2 : 4) };
    g.context->UpdateSubresource(d.sizes.get(), 0, nullptr, sizes, 0, 0);
    for (auto* runtime : d.runtimes)
        Bind(runtime, d.view.get());
}

// Turns the model's output into the DEPTH texture, once the fence says it is ready. The frame copy and guide still hold
// the frame it was estimated from, since the next one is only copied after this.
void Publish()
{
    winrt::check_hresult(g.context.as<ID3D11DeviceContext4>()->Wait(d.fence.get(), d.inputValue + 1));
    const UINT zero[4] = {};
    g.context->ClearUnorderedAccessViewUint(d.keysTarget.get(), zero);
    const UINT pixels = static_cast<UINT>(d.width * d.height);
    Dispatch(1, { nullptr, d.outputView.get() }, { nullptr, d.keysTarget.get() }, (pixels + 255) / 256, 1);
    Dispatch(2, {}, { nullptr, d.keysTarget.get(), d.stateTarget.get() }, 1, 1);
    Dispatch(3, { d.frameView.get(), d.outputView.get(), d.stateView.get(), d.guideView.get() }, { nullptr, nullptr, nullptr, d.textureTarget.get() },
             (d.sourceWidth + 15) / 16, (d.sourceHeight + 15) / 16);
}
} // namespace

bool InitDepth()
{
    // The setup check at startup reports missing files.
    d.directory = ExeDirectory();
    if (GetFileAttributesW((d.directory + kModelFile).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log(LogLevel::Info, L"Depth model not found: %ls%ls.", d.directory.c_str(), kModelFile);
        return false;
    }
    if (!AddonRegistered())
    {
        Log(LogLevel::Warning, L"Depth estimation is off because ReShade did not load the Unishade add-on.");
        return false;
    }
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
    reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
    reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);

    try
    {
        CompileShaders();
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Depth estimation is off: %hs", e.what());
        LogStackTrace();
        return false;
    }
    d.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d.worker = std::thread(Worker);
    d.enabled = true;
    Log(LogLevel::Info, L"Depth estimation initialized: model=%ls%ls, depth_size=%d.", d.directory.c_str(), kModelFile, DepthSize());
    return true;
}

void UpdateDepth(ID3D11Texture2D* frame, int64_t frameTimestamp)
{
    if (!d.enabled)
        return;

    try
    {
        if (d.busy)
        {
            if (!d.slowRequestLogged && FrameStatistics::Clock::now() - d.requestedAt >= std::chrono::seconds(2))
            {
                d.slowRequestLogged = true;
                LogDepthDiagnostics();
            }
            if (!d.done)
                return;
            if (d.failed)
            {
                Stop();
                d.done = false;
                d.busy = false;
                d.failed = false;
                return;
            }
            if (d.inputValue && d.d3d12Fence->GetCompletedValue() < d.inputValue + 1)
                return;
            d.done = false;
            d.busy = false;
            // Nothing is left to publish to after the device was lost.
            if (d.inputValue && d.texture)
            {
                Publish();
                g.frameStatistics.RecordDepth(std::chrono::duration<double, std::milli>(FrameStatistics::Clock::now() - d.requestedAt).count(),
                                              d.workerMs.load());
            }
        }

        if (!d.shaders[0])
            CreateShaders();
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        if (desc.Width != d.sourceWidth || desc.Height != d.sourceHeight || d.size != DepthSize())
            Resize(desc);
        // Publish above even for repeated captures, then avoid another inference on unchanged input.
        if (d.lastInput && *d.lastInput == frameTimestamp)
            return;
        // The worker builds the model for each size, which tells what types it takes.
        if (d.modelWidth != d.width || d.modelHeight != d.height)
        {
            Request(0);
            return;
        }
        if (!d.texture)
            CreateShared();

        g.context->CopyResource(d.frameCopy.get(), frame);
        Dispatch(0, { d.frameView.get() }, { d.inputTarget.get(), nullptr, nullptr, nullptr, d.guideTarget.get() }, (d.width + 15) / 16, (d.height + 15) / 16);
        d.fenceValue += 2;
        winrt::check_hresult(g.context.as<ID3D11DeviceContext4>()->Signal(d.fence.get(), d.fenceValue - 1));
        Request(d.fenceValue - 1);
        d.lastInput = frameTimestamp;
    }
    catch (const winrt::hresult_error& e)
    {
        // The host recovers from a lost device and depth starts again on the new one. Any other error only stops depth.
        if (DeviceLost(e.code()))
            throw;
        Log(LogLevel::Error, L"Depth estimation stopped: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
        Stop();
    }
}

bool DepthEnabled()
{
    return d.enabled;
}

void ResetDepthInput()
{
    d.lastInput.reset();
}

void LogDepthDiagnostics()
{
    const double pendingMs = d.busy ? std::chrono::duration<double, std::milli>(FrameStatistics::Clock::now() - d.requestedAt).count() : 0;
    // The worker may still be creating its fence during the first model-only request.
    const bool fenceReady = !d.busy || d.done || d.inputValue;
    Log(LogLevel::Info, L"Depth state: enabled=%d, busy=%d, worker_done=%d, pending_ms=%.2f, worker_submit_ms=%.2f, "
                       L"input_fence=%llu, completed_fence=%llu, source=%ux%u, requested_model=%dx%d, resource_version=%u.",
        d.enabled, d.busy.load(), d.done.load(), pendingMs, d.workerMs.load(), d.inputValue,
        fenceReady && d.d3d12Fence ? d.d3d12Fence->GetCompletedValue() : 0, d.sourceWidth, d.sourceHeight, d.width, d.height, d.version);
}

void ReleaseDepthDevice()
{
    ReleaseResources();
}

void ShutdownDepth()
{
    if (d.worker.joinable())
    {
        d.stopping = true;
        SetEvent(d.request);
        d.worker.join();
    }
    d.list = nullptr;
    d.allocator = nullptr;
    d.d3d12Fence = nullptr;
    d.d3d12 = nullptr;
    d.d3d12Proxy = nullptr;
    for (HANDLE handle : { d.fenceHandle, d.inputHandle, d.outputHandle, d.request })
        if (handle)
            CloseHandle(handle);
}
