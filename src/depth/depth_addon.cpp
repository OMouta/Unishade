// Depth estimation as a Unishade add-on, Depth.dll. Depth Anything V2 runs through ONNX Runtime on DirectML
// (depth_model.cpp) on a Direct3D 12 device of its own. Direct3D 11 on the same graphics card takes the frame Unishade
// shares, makes the model's input from it and turns the model's output into DEPTH. The model runs on a worker thread
// and can take longer than a frame: frames that arrive meanwhile are skipped, and DEPTH keeps the last estimate.
//
// The files it needs are in its folder with it: depth-anything-v2-small.onnx, onnxruntime.dll and DirectML.dll.

#include "depth_model.h"
#include "depth_shared.h"
#include <addons/unishade_addon.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace
{
using Clock = std::chrono::steady_clock;

using namespace depth;

constexpr wchar_t kModelFile[] = L"" DEPTH_MODEL_FILE;
// The model's input and output go between the Direct3D 11 and 12 devices in textures, since only textures can be
// shared between them, and DirectML takes buffers. The textures hold the values in order, in rows of this many bytes,
// so Direct3D 12 copies them to and from packed buffers. Rows this wide keep the largest input under 16384 of them.
constexpr UINT kRowBytes = 16 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

// Preprocess turns the frame into the model's input. Peak, Smooth and Upsample turn its output into DEPTH. They follow
// kShaderShared.
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

// Upsamples the output to the frame's size with a joint bilateral filter, as Weight says.
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
            float weight = Weight(offset, difference);
            sum += weight * Load(texel.y * ModelSize.x + texel.x);
            weights += weight;
        }
    Depth[id.xy] = StoredDepth(sum / weights / max(Smoothed[0].x, 1e-6));
}
)";

constexpr const char* kEntryPoints[] = { "Preprocess", "Peak", "Smooth", "Upsample" };

std::wstring Wide(const char* text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
    return wide;
}

std::string Narrow(const std::wstring& text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(size > 0 ? size - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

class Depth
{
public:
    explicit Depth(const UnishadeHost& host);
    ~Depth();
    void Frame(const UnishadeFrame& frame);

private:
    void Log(UnishadeLogLevel level, const char* format, ...);
    void CreateD3D12();
    void ReleaseD3D12();
    bool D3D12Removed() const;
    winrt::com_ptr<ID3D12Resource> CreateD3D12Buffer(UINT64 size);
    void Copy(ID3D12CommandQueue* queue, ID3D12Resource* texture, ID3D12Resource* buffer, bool toBuffer);
    void Worker();
    void Request(uint64_t value);
    void Dispatch(size_t shader, std::array<ID3D11ShaderResourceView*, 4> views, std::array<ID3D11UnorderedAccessView*, 5> targets, UINT x, UINT y);
    void Open(const UnishadeFrame& frame);
    void Update(ID3D11Texture2D* source, uint64_t picture, int size);
    void Resize(const D3D11_TEXTURE2D_DESC& frame, int size);
    void CreateSharedTexture(size_t count, bool half, UINT bind, winrt::com_ptr<ID3D11Texture2D>& texture, HANDLE& handle);
    void CreateShared();
    void ReleaseShared();
    void Publish();
    void LogState();

    const UnishadeHost& host;
    std::wstring directory;
    winrt::com_ptr<IDXGIAdapter1> adapter;

    // Direct3D 11, which Unishade's frames and DEPTH are opened on. Only the main thread uses it.
    winrt::com_ptr<ID3D11Device5> device;
    winrt::com_ptr<ID3D11DeviceContext4> context;
    std::array<winrt::com_ptr<ID3D11ComputeShader>, std::size(kEntryPoints)> shaders;
    winrt::com_ptr<ID3D11Buffer> sizes;
    // Unishade's, for the frame's generation.
    uint64_t generation = UINT64_MAX;
    winrt::com_ptr<ID3D11Fence> hostFence;
    winrt::com_ptr<ID3D11Texture2D> color;
    winrt::com_ptr<ID3D11Texture2D> depth;
    winrt::com_ptr<ID3D11UnorderedAccessView> depthTarget;

    int width = 0; // model input and output size, follows the frame's aspect ratio
    int height = 0;
    int size = 0;  // their longest side, as Detail picks it
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    std::optional<uint64_t> lastInput;
    Clock::time_point requestedAt{};
    std::atomic<double> workerMs = 0;
    bool slowRequestLogged = false;
    winrt::com_ptr<ID3D11Texture2D> frameCopy;
    winrt::com_ptr<ID3D11ShaderResourceView> frameView;

    // Made for each frame and model size. The input and output textures are shared with the Direct3D 12 device, like
    // the fence.
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
    winrt::com_ptr<ID3D11Fence> fence;
    // The fence value the last output is ready at. Each run takes the next two, with its input ready at the first.
    uint64_t fenceValue = 0;

    winrt::com_ptr<ID3D12Device> d3d12;
    winrt::com_ptr<ID3D12Fence> d3d12Fence;
    HANDLE fenceHandle = nullptr;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;

    // The worker owns the model and the Direct3D 12 side. While busy is set it reads the fields below and writes the
    // model's size and types, and the main thread only touches them while it is clear.
    std::thread worker;
    HANDLE request = nullptr;
    std::atomic<bool> stopping = false;
    std::atomic<bool> busy = false;
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    std::string failure;
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

Depth::Depth(const UnishadeHost& unishade) : host(unishade), directory(Wide(unishade.folder))
{
    if (GetFileAttributesW((directory + kModelFile).c_str()) == INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("The model is missing: " + Narrow(directory + kModelFile) + ".");
    winrt::com_ptr<IDXGIFactory4> factory;
    winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
    LUID luid;
    std::memcpy(&luid, host.adapter, sizeof(luid));
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()))))
        throw std::runtime_error("Direct3D does not know the graphics card Unishade draws with.");

    winrt::com_ptr<ID3D11Device> created;
    winrt::com_ptr<ID3D11DeviceContext> createdContext;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, created.put(), nullptr,
                                           createdContext.put()));
    device = created.as<ID3D11Device5>();
    context = createdContext.as<ID3D11DeviceContext4>();
    const std::string source = std::string("#define CONSTANT static const\n") + kShaderShared + kShaders;
    for (size_t i = 0; i < std::size(kEntryPoints); ++i)
    {
        winrt::com_ptr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(source.data(), source.size(), "depth", nullptr, nullptr, kEntryPoints[i], "cs_5_0", 0, 0, code.put(), errors.put())))
            throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Compute shaders are unavailable on this graphics card.");
        winrt::check_hresult(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shaders[i].put()));
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 32;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device->CreateBuffer(&desc, nullptr, sizes.put()));

    request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker = std::thread(&Depth::Worker, this);
    Log(UNISHADE_LOG_INFO, "Started with %ls%ls.", directory.c_str(), kModelFile);
}

Depth::~Depth()
{
    if (worker.joinable())
    {
        stopping = true;
        SetEvent(request);
        worker.join();
    }
    ReleaseD3D12();
    for (HANDLE handle : { inputHandle, outputHandle, request })
        if (handle)
            CloseHandle(handle);
}

// Makes the device the model runs on, the fence shared with the Direct3D 11 device, and what copies the model's input
// and output. Runs on the worker thread.
void Depth::CreateD3D12()
{
    if (FAILED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d3d12.put()))))
        throw std::runtime_error("DirectX 12 is unavailable on this graphics card.");
    winrt::check_hresult(d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3d12Fence.put())));
    winrt::check_hresult(d3d12->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle));
    winrt::check_hresult(d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
    winrt::check_hresult(d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())));
    winrt::check_hresult(list->Close());
}

// Releases what CreateD3D12 made. Release the model and anything made on the device before this.
void Depth::ReleaseD3D12()
{
    list = nullptr;
    allocator = nullptr;
    d3d12Fence = nullptr;
    if (fenceHandle)
        CloseHandle(std::exchange(fenceHandle, nullptr));
    d3d12 = nullptr;
}

// True once a GPU reset or driver update removed the model's device. Call from the worker, or from the main thread
// while the worker is idle.
bool Depth::D3D12Removed() const
{
    return d3d12 && FAILED(d3d12->GetDeviceRemovedReason());
}

winrt::com_ptr<ID3D12Resource> Depth::CreateD3D12Buffer(UINT64 bytes)
{
    const D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    winrt::com_ptr<ID3D12Resource> buffer;
    winrt::check_hresult(d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(buffer.put())));
    return buffer;
}

// Copies the values of a shared texture into a buffer, packed, or back.
void Depth::Copy(ID3D12CommandQueue* queue, ID3D12Resource* texture, ID3D12Resource* buffer, bool toBuffer)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION textureLocation{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    textureLocation.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION bufferLocation{ buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    bufferLocation.PlacedFootprint = { 0, { desc.Format, static_cast<UINT>(desc.Width), desc.Height, 1, kRowBytes } };
    winrt::check_hresult(list->Reset(allocator.get(), nullptr));
    if (toBuffer)
        list->CopyTextureRegion(&bufferLocation, 0, 0, 0, &textureLocation, nullptr);
    else
    {
        list->CopyTextureRegion(&textureLocation, 0, 0, 0, &bufferLocation, nullptr);
        // Direct3D 11 reads it next, which needs it back in the common state. Read-only states return there on their own.
        D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        barrier.Transition = { texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON };
        list->ResourceBarrier(1, &barrier);
    }
    winrt::check_hresult(list->Close());
    ID3D12CommandList* lists[] = { list.get() };
    queue->ExecuteCommandLists(1, lists);
}

void Depth::Worker()
{
    winrt::com_ptr<ID3D12Resource> inputTexture, outputTexture, inputBuffer, outputBuffer;
    // Declared last, so it goes first.
    std::unique_ptr<DepthModel> model;
    int loadedWidth = 0, loadedHeight = 0;
    unsigned boundVersion = 0;
    for (;;)
    {
        WaitForSingleObject(request, INFINITE);
        if (stopping)
        {
            // The last run may still be on the graphics card. Its input never comes when the Direct3D 11 device was
            // lost, so the wait is short.
            if (inputValue && d3d12Fence && d3d12Fence->GetCompletedValue() < inputValue + 1)
                if (const HANDLE finished = CreateEventW(nullptr, FALSE, FALSE, nullptr))
                {
                    if (SUCCEEDED(d3d12Fence->SetEventOnCompletion(inputValue + 1, finished)))
                        WaitForSingleObject(finished, 1000);
                    CloseHandle(finished);
                }
            return;
        }
        try
        {
            const auto started = Clock::now();
            // While anything holds a removed device, every D3D12CreateDevice in this process fails. Release it, then
            // make a new one. Only on requests without input, because the main thread reads the fence during those.
            if (!inputValue && d3d12 && (!list || D3D12Removed()))
            {
                Log(UNISHADE_LOG_INFO, "Releasing the Direct3D 12 device: removed_reason=0x%08X.", static_cast<unsigned>(d3d12->GetDeviceRemovedReason()));
                model.reset();
                inputTexture = outputTexture = inputBuffer = outputBuffer = nullptr;
                loadedWidth = loadedHeight = 0;
                ReleaseD3D12();
            }
            if (!d3d12)
                CreateD3D12();
            if (width != loadedWidth || height != loadedHeight)
            {
                model.reset();
                boundVersion = 0;
                model = std::make_unique<DepthModel>();
                model->Load(directory, kModelFile, width, height, d3d12.get());
                loadedWidth = modelWidth = width;
                loadedHeight = modelHeight = height;
                halfInput = model->HalfInput();
                halfOutput = model->HalfOutput();
                Log(UNISHADE_LOG_INFO, "Model loaded: input=%dx%d, half_input=%d, half_output=%d.", width, height, halfInput, halfOutput);
            }
            if (inputValue)
            {
                if (boundVersion != version)
                {
                    winrt::check_hresult(d3d12->OpenSharedHandle(inputHandle, IID_PPV_ARGS(inputTexture.put())));
                    winrt::check_hresult(d3d12->OpenSharedHandle(outputHandle, IID_PPV_ARGS(outputTexture.put())));
                    inputBuffer = CreateD3D12Buffer(inputTexture->GetDesc().Height * kRowBytes);
                    outputBuffer = CreateD3D12Buffer(outputTexture->GetDesc().Height * kRowBytes);
                    model->Bind(inputBuffer.get(), outputBuffer.get());
                    boundVersion = version;
                }
                // Returns before the graphics card is done. The main thread waits for the fence.
                ID3D12CommandQueue* queue = model->Queue();
                winrt::check_hresult(queue->Wait(d3d12Fence.get(), inputValue));
                winrt::check_hresult(allocator->Reset());
                Copy(queue, inputTexture.get(), inputBuffer.get(), true);
                model->Run();
                Copy(queue, outputTexture.get(), outputBuffer.get(), false);
                winrt::check_hresult(queue->Signal(d3d12Fence.get(), inputValue + 1));
            }
            workerMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        }
        // A removed device doesn't stop depth. Update notices it and asks for a new one.
        catch (const std::exception& e)
        {
            if (D3D12Removed())
                Log(UNISHADE_LOG_INFO, "A run failed on a removed Direct3D 12 device: %s", e.what());
            else
            {
                failure = e.what();
                failed = true;
            }
        }
        catch (const winrt::hresult_error& e)
        {
            if (D3D12Removed())
                Log(UNISHADE_LOG_INFO, "A run failed on a removed Direct3D 12 device: 0x%08X", static_cast<unsigned>(e.code()));
            else
            {
                char text[64];
                std::snprintf(text, sizeof(text), " (0x%08X)", static_cast<unsigned>(e.code()));
                failure = winrt::to_string(e.message()) + text;
                failed = true;
            }
        }
        done = true;
    }
}

// Hands the worker a model size to build, or with value, an input that is ready once the fence reaches it.
void Depth::Request(uint64_t value)
{
    inputValue = value;
    requestedAt = Clock::now();
    slowRequestLogged = false;
    busy = true;
    SetEvent(request);
}

// Runs a shader with views in their registers, t0 to t3 and u0 to u4, and unbinds them after.
void Depth::Dispatch(size_t shader, std::array<ID3D11ShaderResourceView*, 4> views, std::array<ID3D11UnorderedAccessView*, 5> targets, UINT x, UINT y)
{
    ID3D11Buffer* constants[] = { sizes.get() };
    context->CSSetShader(shaders[shader].get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, constants);
    context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(targets.size()), targets.data(), nullptr);
    context->Dispatch(x, y, 1);
    views.fill(nullptr);
    targets.fill(nullptr);
    context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(targets.size()), targets.data(), nullptr);
}

// Releases what is made for each frame and model size.
void Depth::ReleaseShared()
{
    stateView = nullptr;
    stateTarget = nullptr;
    state = nullptr;
    keysTarget = nullptr;
    keys = nullptr;
    outputView = nullptr;
    output = nullptr;
    guideView = nullptr;
    guideTarget = nullptr;
    guide = nullptr;
    inputTarget = nullptr;
    input = nullptr;
    for (HANDLE* handle : { &inputHandle, &outputHandle })
        if (*handle)
            CloseHandle(std::exchange(*handle, nullptr));
}

// Sizes the model to the frame's aspect ratio. What follows the model's size is made again once the model is built for
// it.
void Depth::Resize(const D3D11_TEXTURE2D_DESC& frame, int longest)
{
    size = longest;
    const ModelSize model = ModelSizeFor(frame.Width, frame.Height, longest);
    width = model.width;
    height = model.height;
    sourceWidth = frame.Width;
    sourceHeight = frame.Height;
    lastInput.reset();
    Log(UNISHADE_LOG_INFO, "Resized: source=%ux%u, model=%dx%d.", frame.Width, frame.Height, width, height);
    ReleaseShared();

    D3D11_TEXTURE2D_DESC copy = frame;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.SampleDesc = { 1, 0 };
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    frameCopy = nullptr;
    frameView = nullptr;
    winrt::check_hresult(device->CreateTexture2D(&copy, nullptr, frameCopy.put()));
    winrt::check_hresult(device->CreateShaderResourceView(frameCopy.get(), nullptr, frameView.put()));
}

// A texture the Direct3D 12 device opens, holding count values of the model's type in rows of kRowBytes.
void Depth::CreateSharedTexture(size_t count, bool half, UINT bind, winrt::com_ptr<ID3D11Texture2D>& texture, HANDLE& handle)
{
    const UINT rowWidth = kRowBytes / (half ? 2 : 4);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = rowWidth;
    desc.Height = static_cast<UINT>((count + rowWidth - 1) / rowWidth);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = half ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
    winrt::check_hresult(texture.as<IDXGIResource1>()->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
}

// Makes what follows the model's size, once the model is built for it. DEPTH starts out at the far plane until the
// first estimate arrives.
void Depth::CreateShared()
{
    const size_t pixels = static_cast<size_t>(width) * height;
    CreateSharedTexture(3 * pixels, halfInput, D3D11_BIND_UNORDERED_ACCESS, input, inputHandle);
    CreateSharedTexture(pixels, halfOutput, D3D11_BIND_SHADER_RESOURCE, output, outputHandle);
    ++version;
    winrt::check_hresult(device->CreateUnorderedAccessView(input.get(), nullptr, inputTarget.put()));
    winrt::check_hresult(device->CreateShaderResourceView(output.get(), nullptr, outputView.put()));
    if (!fence)
        winrt::check_hresult(device->OpenSharedFence(fenceHandle, IID_PPV_ARGS(fence.put())));

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, keys.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements = 4;
    raw.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    winrt::check_hresult(device->CreateUnorderedAccessView(keys.get(), &raw, keysTarget.put()));

    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer.StructureByteStride = 16;
    winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, state.put()));
    winrt::check_hresult(device->CreateUnorderedAccessView(state.get(), nullptr, stateTarget.put()));
    winrt::check_hresult(device->CreateShaderResourceView(state.get(), nullptr, stateView.put()));
    const UINT zero[4] = {};
    context->ClearUnorderedAccessViewUint(stateTarget.get(), zero);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, guide.put()));
    winrt::check_hresult(device->CreateUnorderedAccessView(guide.get(), nullptr, guideTarget.put()));
    winrt::check_hresult(device->CreateShaderResourceView(guide.get(), nullptr, guideView.put()));

    const float farPlane[4] = { 1, 1, 1, 1 };
    context->ClearUnorderedAccessViewFloat(depthTarget.get(), farPlane);
    const UINT values[8] = { sourceWidth, sourceHeight, static_cast<UINT>(width), static_cast<UINT>(height), kRowBytes / (halfInput ? 2 : 4),
                             kRowBytes / (halfOutput ? 2 : 4) };
    context->UpdateSubresource(sizes.get(), 0, nullptr, values, 0, 0);
}

// Turns the model's output into DEPTH, once the fence says it is ready. The frame copy and guide still hold the frame
// it was estimated from, since the next one is only copied after this.
void Depth::Publish()
{
    winrt::check_hresult(context->Wait(fence.get(), inputValue + 1));
    const UINT zero[4] = {};
    context->ClearUnorderedAccessViewUint(keysTarget.get(), zero);
    const UINT pixels = static_cast<UINT>(width * height);
    Dispatch(1, { nullptr, outputView.get() }, { nullptr, keysTarget.get() }, (pixels + 255) / 256, 1);
    Dispatch(2, {}, { nullptr, keysTarget.get(), stateTarget.get() }, 1, 1);
    Dispatch(3, { frameView.get(), outputView.get(), stateView.get(), guideView.get() }, { nullptr, nullptr, nullptr, depthTarget.get() },
             (sourceWidth + 15) / 16, (sourceHeight + 15) / 16);
}

void Depth::LogState()
{
    const double pendingMs = busy ? std::chrono::duration<double, std::milli>(Clock::now() - requestedAt).count() : 0;
    // The worker may still be creating its fence during the first model-only request.
    const bool fenceReady = !busy || done || inputValue;
    Log(UNISHADE_LOG_INFO, "State: busy=%d, worker_done=%d, pending_ms=%.2f, worker_submit_ms=%.2f, input_fence=%llu, completed_fence=%llu, "
                           "source=%ux%u, model=%dx%d, resource_version=%u.",
        busy.load(), done.load(), pendingMs, workerMs.load(), inputValue, fenceReady && d3d12Fence ? d3d12Fence->GetCompletedValue() : 0,
        sourceWidth, sourceHeight, width, height, version);
}

// Unishade's images and fence, again whenever it makes them again. The textures it made are empty.
void Depth::Open(const UnishadeFrame& frame)
{
    color = nullptr;
    depth = nullptr;
    depthTarget = nullptr;
    hostFence = nullptr;
    winrt::check_hresult(device->OpenSharedFence(frame.fence, IID_PPV_ARGS(hostFence.put())));
    winrt::check_hresult(device->OpenSharedResource1(frame.color.handle, IID_PPV_ARGS(color.put())));
    winrt::check_hresult(device->OpenSharedResource1(frame.depth.handle, IID_PPV_ARGS(depth.put())));
    winrt::check_hresult(device->CreateUnorderedAccessView(depth.get(), nullptr, depthTarget.put()));
    generation = frame.generation;
    lastInput.reset();
}

void Depth::Frame(const UnishadeFrame& frame)
{
    if (!frame.color.handle || !frame.depth.handle || !frame.fence)
        throw std::runtime_error("Unishade gave it no picture or no depth to write.");
    const bool opened = frame.generation != generation;
    if (opened)
        Open(frame);
    winrt::check_hresult(context->Wait(hostFence.get(), frame.wait));
    if (opened)
    {
        const float farPlane[4] = { 1, 1, 1, 1 };
        context->ClearUnorderedAccessViewFloat(depthTarget.get(), farPlane);
    }
    Update(color.get(), frame.picture, LongestSide(frame));
    winrt::check_hresult(context->Signal(hostFence.get(), frame.signal));
    context->Flush();
}

// Estimates depth for the frame on the worker and publishes the newest result to DEPTH. Frames that arrive while an
// estimate is in progress are skipped. The same picture again still publishes a completed result.
void Depth::Update(ID3D11Texture2D* source, uint64_t picture, int longest)
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
        // GetCompletedValue returns UINT64_MAX on a removed device.
        if (inputValue && d3d12Fence->GetCompletedValue() < inputValue + 1)
            return;
        done = false;
        busy = false;
        // Nothing is left to publish to after the device was lost.
        if (inputValue && input && !D3D12Removed())
        {
            Publish();
            ++estimates;
            observedMs += std::chrono::duration<double, std::milli>(Clock::now() - requestedAt).count();
            submitMs += workerMs.load();
        }
    }
    if (Clock::now() - statisticsSince >= kStatisticsInterval)
    {
        if (estimates)
            Log(UNISHADE_LOG_INFO, "%u estimates in %lld s, %.1f ms each from frame to result, %.1f ms to submit.", estimates,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - statisticsSince).count()), observedMs / estimates,
                submitMs / estimates);
        statisticsSince = Clock::now();
        estimates = 0;
        observedMs = submitMs = 0;
    }

    // The shared textures and fence belong to the removed device. Asking for the model again makes the worker create a
    // new device first. CreateShared opens its fence after that.
    if (D3D12Removed())
    {
        ReleaseShared();
        fence = nullptr;
        modelWidth = modelHeight = 0;
    }

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Width != sourceWidth || desc.Height != sourceHeight || size != longest)
        Resize(desc, longest);
    if (lastInput && *lastInput == picture)
        return;
    // The worker builds the model for each size, which tells what types it takes.
    if (modelWidth != width || modelHeight != height)
    {
        Request(0);
        return;
    }
    if (!input)
        CreateShared();

    context->CopyResource(frameCopy.get(), source);
    Dispatch(0, { frameView.get() }, { inputTarget.get(), nullptr, nullptr, nullptr, guideTarget.get() }, (width + 15) / 16, (height + 15) / 16);
    fenceValue += 2;
    winrt::check_hresult(context->Signal(fence.get(), fenceValue - 1));
    Request(fenceValue - 1);
    lastInput = picture;
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
        return new Depth(*host);
    }
    catch (const std::exception& e)
    {
        Fill(error, errorSize, e.what());
    }
    catch (const winrt::hresult_error& e)
    {
        Fill(error, errorSize, "Direct3D failed: " + winrt::to_string(e.message()));
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
    catch (const winrt::hresult_error& e)
    {
        Fill(error, errorSize, "Direct3D failed: " + winrt::to_string(e.message()));
    }
    return false;
}

void Destroy(void* addon)
{
    delete static_cast<Depth*>(addon);
}

const UnishadeAddon kAddon = {
    UNISHADE_ADDON_VERSION,
    kName,
    kDescription,
    UNISHADE_ADDON_WRITES_DEPTH,
    kSettings,
    static_cast<uint32_t>(std::size(kSettings)),
    Create,
    RunFrame,
    Destroy,
};
} // namespace

extern "C" UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void)
{
    return &kAddon;
}
