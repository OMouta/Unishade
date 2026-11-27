// DLSS5 as a Unishade add-on. It is part of Unishade_nvngx.dll, since nvngx_dlssnr.dll only takes calls from a module
// whose path contains nvngx.dll, and runs dlss5.cpp on every frame before effects. nvngx_dlssnr.dll goes in its folder
// with it. Evaluating waits for the graphics card, so the frame waits on the processor too.
//
// Nothing known about nvngx_dlssnr.dll says it takes HDR color: .plan/dlss5-research.md lists no such parameter or
// creation flag. So an HDR picture is brought into an SDR one for it with a curve that can be undone, and what it
// changed is brought back.
//
// A captured picture comes with no motion vectors, and DLSS5 holds its history steady only with them. DLSS5 takes the
// motion another add-on wrote for the picture, where one did. Where none did, the add-on estimates them itself from the
// picture DLSS5 gets and the one it got before. The estimator is motion_d3d11.h's, run through Direct3D 11 on 12 on
// DLSS5's own device, so it reads those pictures and DLSS5 reads its vectors with no copy. Whether the picture cut to
// another scene is told by the estimator's own test either way, which only costs a pass over the picture.

#include "dlss5.h"
#include "motion/motion_d3d11.h"
#include <unishade_addon.h>

#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
// Every pass keeps its own copy of the network, about 290 MB at 1582x863.
constexpr int kMostPasses = 4;

// In the order Addon::Frame reads their values.
enum
{
    kStyle,
    kIntensity,
    kLocalTone,
    kLocalStructure,
    kSkinStructure,
    kAutoMask,
    kPassCount,
};

const UnishadeSetting kSettings[] = {
    // nvngx_dlssnr.dll 310.8.0.0 has one more look, for every style but 0 and 1. The name is Unishade's.
    { "Style", "Style", UNISHADE_SETTING_CHOICE, { 0 }, 0, 2, "Natural\0Cinematic\0Soft\0" },
    { "Intensity", "Intensity", UNISHADE_SETTING_SLIDER, { 1 }, 0, 1, nullptr, "From the game's picture as it is to the full effect." },
    // The DLL does not clamp the strengths. 1 is its own default.
    { "LocalToneStrength", "Local tone", UNISHADE_SETTING_SLIDER, { 1 }, 0, 2 },
    { "LocalStructureStrength", "Structure", UNISHADE_SETTING_SLIDER, { 1 }, 0, 2 },
    { "SkinStructureStrength", "Skin structure", UNISHADE_SETTING_SLIDER, { 1 }, 0, 2, nullptr, "Structure on characters, when Auto mask is on." },
    { "AutoMask", "Auto mask", UNISHADE_SETTING_TOGGLE, { 1 }, 0, 1, nullptr, "Finds characters, so Skin structure applies to them." },
    { "PassCount", "Passes", UNISHADE_SETTING_INTEGER, { 1 }, 1, kMostPasses, nullptr,
      "Runs DLSS5 again on its own result. Each pass takes more of the graphics card's memory." },
};

// An HDR picture to the SDR one DLSS5 gets and back, as compute shaders. The picture is divided by SDR white, and
// what is above Knee is compressed by its largest channel, so hues stay, with x / (1 + x), which Expand undoes, then
// encoded as sRGB, as Unishade's SDR pictures are.
constexpr char kShaders[] = R"(
cbuffer Constants : register(b0)
{
    float White; // SDR white in the picture's units
};
Texture2D<float4> Picture : register(t0); // the HDR picture
Texture2D<float4> Given : register(t1);   // what DLSS5 got
Texture2D<float4> Changed : register(t2); // what it made of it
RWTexture2D<float4> Target : register(u0);

static const float Knee = 0.75;

float3 Compress(float3 color)
{
    color = max(color, 0);
    float peak = max(color.r, max(color.g, color.b));
    if (peak <= Knee)
        return color;
    float over = (peak - Knee) / (1 - Knee);
    return color * ((Knee + (1 - Knee) * over / (1 + over)) / peak);
}

float3 Expand(float3 color)
{
    float peak = max(color.r, max(color.g, color.b));
    if (peak <= Knee)
        return color;
    float over = min((peak - Knee) / (1 - Knee), 0.999);
    return color * ((Knee + (1 - Knee) * over / (1 - over)) / peak);
}

float3 Encode(float3 color)
{
    return lerp(color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055, step(0.0031308, color));
}

float3 Decode(float3 color)
{
    return lerp(color / 12.92, pow((color + 0.055) / 1.055, 2.4), step(0.04045, color));
}

[numthreads(8, 8, 1)]
void ToSdr(uint3 id : SV_DispatchThreadID)
{
    Target[id.xy] = float4(Encode(Compress(Picture[id.xy].rgb / White)), 1);
}

// What DLSS5 changed is added to the picture, rather than its result taking the picture's place, so what it leaves
// alone keeps the precision it had, which eight bits would lose in highlights.
[numthreads(8, 8, 1)]
void ToHdr(uint3 id : SV_DispatchThreadID)
{
    float4 picture = Picture[id.xy];
    float3 change = Expand(Decode(Changed[id.xy].rgb)) - Expand(Decode(Given[id.xy].rgb));
    Target[id.xy] = float4(picture.rgb + change * White, picture.a);
}
)";
// Compares the picture DLSS5 gets with the one it got before, to find a frame the game showed again. Whether it is a
// cut to another scene is the motion estimator's to say, since a cut is where its vectors are zero.
constexpr char kAnalysis[] = R"(
Texture2D<float4> Picture : register(t0);  // what DLSS5 gets
Texture2D<float4> Previous : register(t1); // what it got before
RWByteAddressBuffer Counts : register(u0); // the pixels that differ, then whether the picture is the same

groupshared uint differing;

// Each group counts its own pixels first, since every pixel adding to the same count would make them wait.
[numthreads(16, 16, 1)]
void Count(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index == 0)
        differing = 0;
    GroupMemoryBarrierWithGroupSync();
    uint2 size;
    Picture.GetDimensions(size.x, size.y);
    if (all(id.xy < size) && any(Picture[id.xy].rgb != Previous[id.xy].rgb))
        InterlockedAdd(differing, 1);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && differing > 0)
        Counts.InterlockedAdd(0, differing);
}

// Turns the count into the answer and leaves the count zero for the next picture.
[numthreads(1, 1, 1)]
void Resolve()
{
    Counts.Store(4, Counts.Load(0) == 0);
    Counts.Store(0, 0);
}
)";
// As kAnalysis lays them out: the count, then the answer.
constexpr UINT kCounts = 2;

struct Shader
{
    const char* source;
    const char* entryPoint;
};
constexpr Shader kEntryPoints[] = { { kShaders, "ToSdr" }, { kShaders, "ToHdr" }, { kAnalysis, "Count" }, { kAnalysis, "Resolve" } };

// For log lines from the module, whose callback has no context. Only one DLSS5 runs at a time.
const UnishadeHost* logHost;

void LogLine(const char* line)
{
    if (logHost)
        logHost->log(logHost, UNISHADE_LOG_INFO, line);
}

std::wstring Wide(const char* text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
    return wide;
}

class Addon
{
public:
    explicit Addon(const UnishadeHost& host);
    ~Addon();
    void Frame(const UnishadeFrame& frame);

private:
    void CreateShaders();
    winrt::com_ptr<ID3D12Resource> CreateTexture(ID3D12Resource* like, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags);
    // Says whether the picture DLSS5 gets is the same as the one before, and waits for it.
    bool Analyze(ID3D12Resource* sdr);
    // Estimates the motion from the picture before to sdr, keeps sdr for the next and waits for it. Returns whether it
    // is a cut. The motion is lent to DLSS5's device in motion, which goes back with ReturnUnderlyingResource. With
    // shared, which says another add-on wrote the motion, it only tells the cut and lends nothing.
    bool Estimate(ID3D12Resource* sdr, bool shared, winrt::com_ptr<ID3D12Resource>& motion);
    // Runs one of the shaders over target, reading up to three textures, and waits for it.
    void Convert(size_t shader, std::array<ID3D12Resource*, 3> inputs, ID3D12Resource* target, float white);
    void SetInputs(std::array<ID3D12Resource*, 3> inputs);
    void Run();

    Dlss5* dlss5 = nullptr;
    ID3D12Device* device = nullptr;
    winrt::handle event;
    // Unishade's, for the frame's generation.
    uint64_t generation = UINT64_MAX;
    winrt::com_ptr<ID3D12Fence> fence;
    winrt::com_ptr<ID3D12Resource> color;
    winrt::com_ptr<ID3D12Resource> output;
    // Opened with the first frame of the generation that has them.
    winrt::com_ptr<ID3D12Resource> depth;
    winrt::com_ptr<ID3D12Resource> sharedMotion;
    // What the last frame was evaluated with, so the same picture with the same look is not evaluated again.
    uint64_t picture = 0;
    Dlss5Look look{};
    bool hadDepth = false;

    // Made with the first frame: the shaders and what they run with.
    winrt::com_ptr<ID3D12RootSignature> rootSignature;
    std::array<winrt::com_ptr<ID3D12PipelineState>, std::size(kEntryPoints)> shaders;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> done;
    uint64_t doneValue = 0;
    winrt::handle idle;
    // Direct3D 11 on 12, on the device and queue above, for the motion estimator.
    winrt::com_ptr<ID3D11On12Device2> on12;
    winrt::com_ptr<ID3D11DeviceContext> context;
    // For each generation: the picture DLSS5 got before and the counts it is compared by, and for HDR pictures the SDR
    // picture DLSS5 gets and the one it makes.
    winrt::com_ptr<ID3D12Resource> previous;
    winrt::com_ptr<ID3D12Resource> counts;
    winrt::com_ptr<ID3D12Resource> answers;
    winrt::com_ptr<ID3D12Resource> given;
    winrt::com_ptr<ID3D12Resource> changed;
    // And the estimator, the two pictures it reads as Direct3D 11 sees them, and a buffer whether it is a cut is read
    // back through.
    std::unique_ptr<motion::Estimator> estimator;
    winrt::com_ptr<ID3D11Texture2D> current11;
    winrt::com_ptr<ID3D11Texture2D> previous11;
    winrt::com_ptr<ID3D11ShaderResourceView> currentView;
    winrt::com_ptr<ID3D11ShaderResourceView> previousView;
    winrt::com_ptr<ID3D11Buffer> cutAnswer;
};

Addon::Addon(const UnishadeHost& host)
{
    // nvngx_dlssnr.dll writes its own log only when this is set, into the data folder NGX gets, beside Unishade.log.
    if (!GetEnvironmentVariableW(L"__NGX_LOG_LEVEL", nullptr, 0))
        SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", L"1");
    const std::wstring runtime = Wide(host.folder) + L"nvngx_dlssnr.dll";
    if (GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error(std::string("nvngx_dlssnr.dll is missing from ") + host.folder + ".");
    logHost = &host;
    char error[512] = "";
    dlss5 = Dlss5Create(runtime.c_str(), Wide(host.dataFolder).c_str(), LogLine, error, sizeof(error));
    if (!dlss5)
    {
        logHost = nullptr;
        throw std::runtime_error(error);
    }
    device = Dlss5Device(dlss5);
    LUID unishade;
    std::memcpy(&unishade, host.adapter, sizeof(unishade));
    const LUID nvidia = device->GetAdapterLuid();
    if (nvidia.LowPart != unishade.LowPart || nvidia.HighPart != unishade.HighPart)
    {
        Dlss5Destroy(dlss5);
        logHost = nullptr;
        throw std::runtime_error("DLSS5 runs on the NVIDIA graphics card, and Unishade draws with another one.");
    }
    event.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

Addon::~Addon()
{
    Dlss5Destroy(dlss5);
    logHost = nullptr;
}

void Addon::Frame(const UnishadeFrame& frame)
{
    if (!frame.color.handle || !frame.output.handle || !frame.fence)
        throw std::runtime_error("Unishade gave it no picture to change.");
    if (frame.generation != generation)
    {
        color = output = depth = sharedMotion = nullptr;
        fence = nullptr;
        previous = counts = answers = given = changed = nullptr;
        estimator = nullptr;
        current11 = previous11 = nullptr;
        currentView = previousView = nullptr;
        cutAnswer = nullptr;
        winrt::check_hresult(device->OpenSharedHandle(frame.fence, IID_PPV_ARGS(fence.put())));
        winrt::check_hresult(device->OpenSharedHandle(frame.color.handle, IID_PPV_ARGS(color.put())));
        winrt::check_hresult(device->OpenSharedHandle(frame.output.handle, IID_PPV_ARGS(output.put())));
    }
    // Unishade gives depth only while another add-on, such as Depth, wrote it for this picture.
    const bool hasDepth = frame.depth.handle != nullptr;
    if (hasDepth && !depth)
        winrt::check_hresult(device->OpenSharedHandle(frame.depth.handle, IID_PPV_ARGS(depth.put())));
    // And motion, only in the frames whose picture another add-on estimated it for. It is at the picture's size, and
    // estimated from the picture before any add-on changed it, which in HDR is Unishade's SDR copy rather than the one
    // made here. Their content moves the same, so the vectors are the ones this picture would give.
    const bool hasMotion = UNISHADE_HAS(UnishadeFrame, &frame, motion) && frame.motion.handle != nullptr;
    if (hasMotion && !sharedMotion)
        winrt::check_hresult(device->OpenSharedHandle(frame.motion.handle, IID_PPV_ARGS(sharedMotion.put())));
    const auto value = [&](int setting) { return frame.settings[setting][0]; };
    const Dlss5Look wanted{ static_cast<int>(value(kStyle)), value(kIntensity),       value(kLocalTone),
                            value(kLocalStructure),          value(kSkinStructure),   value(kAutoMask) != 0,
                            std::clamp(static_cast<int>(value(kPassCount)), 1, kMostPasses) };
    const bool sameLook = frame.generation == generation && hasDepth == hadDepth && wanted.style == look.style &&
                          wanted.intensity == look.intensity && wanted.localTone == look.localTone &&
                          wanted.localStructure == look.localStructure && wanted.skinStructure == look.skinStructure &&
                          wanted.autoMask == look.autoMask && wanted.passCount == look.passCount;
    const bool same = sameLook && frame.picture == picture;
    // Depth coming or going changes what the feature's history was made with.
    const bool depthChanged = frame.generation == generation && hasDepth != hadDepth;
    generation = frame.generation;
    picture = frame.picture;
    look = wanted;
    hadDepth = hasDepth;

    // Dlss5Evaluate waits for the graphics card itself, so the picture has to be there before it starts. The fence is
    // only set after that, since it cannot go back.
    winrt::check_hresult(fence->SetEventOnCompletion(frame.wait, event.get()));
    if (WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0)
        throw std::runtime_error("Unishade's picture did not arrive.");
    if (!same)
    {
        if (!rootSignature)
            CreateShaders();
        // The format follows the game's, and a change makes the images again, which changes the generation.
        const bool hdr = frame.color.format == UNISHADE_FORMAT_RGBA16_FLOAT;
        if (hdr && !given)
        {
            // As Unishade's SDR pictures, which DLSS5 is known to take.
            given = CreateTexture(color.get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            changed = CreateTexture(color.get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }
        if (hdr)
            Convert(0, { color.get(), nullptr, nullptr }, given.get(), frame.sdrWhite);
        ID3D12Resource* sdr = hdr ? given.get() : color.get();
        // A game that shows each of its frames twice, such as at 30 frames a second on a 60 Hz screen, gives the same
        // picture again under a new number. DLSS5 does not make the same result of it twice, so the result would flicker
        // at the game's rate. Output still holds the last result, which shows again instead. Nothing moved, so the
        // picture is not estimated, and a new look gets no motion.
        const bool repeated = Analyze(sdr);
        winrt::com_ptr<ID3D12Resource> estimated;
        const bool cut = !repeated && Estimate(sdr, hasMotion, estimated);
        ID3D12Resource* motion = repeated ? nullptr : hasMotion ? sharedMotion.get() : estimated.get();
        if (!(sameLook && repeated))
        {
            // The vectors point from each pixel to where it was in the previous picture, in pixels, as DLSS5 takes them:
            // dlss5_check's pan holds steady with them and shimmers as much as with none when they are negated.
            const bool evaluated = Dlss5Evaluate(dlss5, sdr, hasDepth ? depth.get() : nullptr, motion, 1, 1, hdr ? changed.get() : output.get(), &look,
                                                 cut || depthChanged);
            if (estimated)
                on12->ReturnUnderlyingResource(estimator->motion.get(), 0, nullptr, nullptr);
            if (!evaluated)
                throw std::runtime_error(Dlss5Error(dlss5));
            if (hdr)
                Convert(1, { color.get(), given.get(), changed.get() }, output.get(), frame.sdrWhite);
        }
    }
    winrt::check_hresult(fence->Signal(frame.signal));
}

winrt::com_ptr<ID3D12Resource> Addon::CreateTexture(ID3D12Resource* like, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    const D3D12_RESOURCE_DESC source = like->GetDesc();
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = source.Width;
    description.Height = source.Height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc = { 1, 0 };
    description.Flags = flags;
    const D3D12_HEAP_PROPERTIES heapType{ D3D12_HEAP_TYPE_DEFAULT };
    winrt::com_ptr<ID3D12Resource> texture;
    winrt::check_hresult(device->CreateCommittedResource(&heapType, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(texture.put())));
    return texture;
}

void Addon::CreateShaders()
{
    // Loaded when first needed, so a system without it gets a message saying so. The compiled code and the errors are
    // blobs it made, which call into it when they are released, so it is freed after them.
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> compiler(LoadLibraryW(L"d3dcompiler_47.dll"), FreeLibrary);
    const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler.get(), "D3DCompile")) : nullptr;
    if (!compile)
        throw std::runtime_error("DLSS5 needs d3dcompiler_47.dll, which Windows could not load.");
    std::array<winrt::com_ptr<ID3DBlob>, std::size(kEntryPoints)> code;
    for (size_t i = 0; i < code.size(); ++i)
    {
        const std::string source = kEntryPoints[i].source;
        winrt::com_ptr<ID3DBlob> errors;
        if (FAILED(compile(source.data(), source.size(), "dlss5", nullptr, nullptr, kEntryPoints[i].entryPoint, "cs_5_0", 0, 0, code[i].put(),
                           errors.put())))
            throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "DLSS5's shaders did not compile.");
    }

    // SDR white as a constant, then a table of the three textures read and the one written.
    const D3D12_DESCRIPTOR_RANGE ranges[] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3 } };
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = { 0, 0, 1 };
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = { static_cast<UINT>(std::size(ranges)), ranges };
    const D3D12_ROOT_SIGNATURE_DESC signature{ static_cast<UINT>(std::size(parameters)), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    winrt::com_ptr<ID3DBlob> serialized, errors;
    winrt::check_hresult(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
    winrt::check_hresult(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(rootSignature.put())));
    for (size_t i = 0; i < shaders.size(); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = rootSignature.get();
        pipeline.CS = { code[i]->GetBufferPointer(), code[i]->GetBufferSize() };
        winrt::check_hresult(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(shaders[i].put())));
    }

    const D3D12_DESCRIPTOR_HEAP_DESC descriptors{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    winrt::check_hresult(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(heap.put())));
    const D3D12_COMMAND_QUEUE_DESC queueDescription{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    winrt::check_hresult(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(queue.put())));
    winrt::check_hresult(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
    winrt::check_hresult(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())));
    winrt::check_hresult(list->Close());
    winrt::check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(done.put())));
    idle.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));

    IUnknown* queues[] = { queue.get() };
    winrt::com_ptr<ID3D11Device> d3d11;
    winrt::check_hresult(D3D11On12CreateDevice(device, 0, nullptr, 0, queues, 1, 0, d3d11.put(), context.put(), nullptr));
    on12 = d3d11.as<ID3D11On12Device2>();
}

bool Addon::Analyze(ID3D12Resource* sdr)
{
    if (!previous)
    {
        // Zero, so the first picture differs from it.
        previous = CreateTexture(sdr, sdr->GetDesc().Format, D3D12_RESOURCE_FLAG_NONE);
        D3D12_RESOURCE_DESC description{ D3D12_RESOURCE_DIMENSION_BUFFER };
        description.Width = kCounts * 4;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc = { 1, 0 };
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_HEAP_PROPERTIES gpuHeap{ D3D12_HEAP_TYPE_DEFAULT }, readHeap{ D3D12_HEAP_TYPE_READBACK };
        winrt::check_hresult(device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(counts.put())));
        description.Width = 4;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        winrt::check_hresult(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(answers.put())));
    }
    SetInputs({ sdr, previous.get(), nullptr });
    D3D12_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = DXGI_FORMAT_R32_TYPELESS;
    access.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    access.Buffer.NumElements = kCounts;
    access.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(counts.get(), nullptr, &access, D3D12_CPU_DESCRIPTOR_HANDLE{ start.ptr + 3 * step });

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    const auto move = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        barriers.push_back(barrier);
    };
    const auto flush = [&] {
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        barriers.clear();
    };
    constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(list->Reset(allocator.get(), shaders[2].get()));
    move(sdr, D3D12_RESOURCE_STATE_COMMON, read);
    move(previous.get(), D3D12_RESOURCE_STATE_COMMON, read);
    move(counts.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    flush();
    ID3D12DescriptorHeap* heaps[] = { heap.get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rootSignature.get());
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    const D3D12_RESOURCE_DESC size = sdr->GetDesc();
    list->Dispatch(static_cast<UINT>((size.Width + 15) / 16), (size.Height + 15) / 16, 1);
    D3D12_RESOURCE_BARRIER counted{ D3D12_RESOURCE_BARRIER_TYPE_UAV };
    counted.UAV.pResource = counts.get();
    list->ResourceBarrier(1, &counted);
    list->SetPipelineState(shaders[3].get());
    list->Dispatch(1, 1, 1);
    move(counts.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    move(sdr, read, D3D12_RESOURCE_STATE_COMMON);
    move(previous.get(), read, D3D12_RESOURCE_STATE_COMMON);
    flush();
    list->CopyBufferRegion(answers.get(), 0, counts.get(), 4, 4);
    move(counts.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    flush();
    Run();

    uint32_t* mapped = nullptr;
    const D3D12_RANGE range{ 0, 4 };
    winrt::check_hresult(answers->Map(0, &range, reinterpret_cast<void**>(&mapped)));
    const bool repeated = mapped[0] != 0;
    const D3D12_RANGE written{};
    answers->Unmap(0, &written);
    return repeated;
}

bool Addon::Estimate(ID3D12Resource* sdr, bool shared, winrt::com_ptr<ID3D12Resource>& motion)
{
    if (!estimator)
    {
        const D3D12_RESOURCE_DESC size = sdr->GetDesc();
        winrt::com_ptr<ID3D11Device> d3d11 = on12.as<ID3D11Device>();
        estimator = std::make_unique<motion::Estimator>(d3d11.get(), static_cast<UINT>(size.Width), size.Height);
        // Unishade's textures and these are left in the COMMON state.
        const D3D11_RESOURCE_FLAGS read{ D3D11_BIND_SHADER_RESOURCE };
        winrt::check_hresult(on12->CreateWrappedResource(sdr, &read, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                                         IID_PPV_ARGS(current11.put())));
        winrt::check_hresult(on12->CreateWrappedResource(previous.get(), &read, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                                         IID_PPV_ARGS(previous11.put())));
        winrt::check_hresult(d3d11->CreateShaderResourceView(current11.get(), nullptr, currentView.put()));
        winrt::check_hresult(d3d11->CreateShaderResourceView(previous11.get(), nullptr, previousView.put()));
        D3D11_BUFFER_DESC answer{};
        answer.ByteWidth = 4;
        answer.Usage = D3D11_USAGE_STAGING;
        answer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(d3d11->CreateBuffer(&answer, nullptr, cutAnswer.put()));
    }
    ID3D11Resource* wrapped[] = { current11.get(), previous11.get() };
    on12->AcquireWrappedResources(wrapped, 2);
    // DLSS5's picture is SDR, so white is 1.
    if (shared)
        estimator->Cut(context.get(), currentView.get(), previousView.get(), 1);
    else
        estimator->Estimate(context.get(), currentView.get(), previousView.get(), 1);
    context->CopyResource(previous11.get(), current11.get());
    const D3D11_BOX first{ 0, 0, 0, 4, 1, 1 };
    context->CopySubresourceRegion(cutAnswer.get(), 0, 0, 0, 0, estimator->keys.get(), 0, &first);
    on12->ReleaseWrappedResources(wrapped, 2);
    // Mapping waits for the estimate, so DLSS5 can read the motion on its own queue.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(cutAnswer.get(), 0, D3D11_MAP_READ, 0, &mapped));
    const bool cut = *static_cast<const uint32_t*>(mapped.pData) != 0;
    context->Unmap(cutAnswer.get(), 0);
    if (!shared)
        winrt::check_hresult(on12->UnwrapUnderlyingResource(estimator->motion.get(), queue.get(), IID_PPV_ARGS(motion.put())));
    return cut;
}

void Addon::SetInputs(std::array<ID3D12Resource*, 3> inputs)
{
    // The previous run is done, so the views can be written again.
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        // A texture not read still gets a view, of nothing.
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = inputs[i] ? inputs[i]->GetDesc().Format : DXGI_FORMAT_B8G8R8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(inputs[i], &view, D3D12_CPU_DESCRIPTOR_HANDLE{ start.ptr + i * step });
    }
}

void Addon::Convert(size_t shader, std::array<ID3D12Resource*, 3> inputs, ID3D12Resource* target, float white)
{
    SetInputs(inputs);
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_RESOURCE_DESC size = target->GetDesc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = size.Format;
    access.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(target, nullptr, &access, D3D12_CPU_DESCRIPTOR_HANDLE{ start.ptr + 3 * step });

    // Unishade's textures have to be left in the COMMON state, and these are too.
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    const auto move = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, state };
        barriers.push_back(barrier);
    };
    for (ID3D12Resource* input : inputs)
        if (input)
            move(input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    move(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    winrt::check_hresult(allocator->Reset());
    winrt::check_hresult(list->Reset(allocator.get(), shaders[shader].get()));
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    ID3D12DescriptorHeap* heaps[] = { heap.get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rootSignature.get());
    list->SetComputeRoot32BitConstants(0, 1, &white, 0);
    list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(static_cast<UINT>((size.Width + 7) / 8), (size.Height + 7) / 8, 1);
    for (D3D12_RESOURCE_BARRIER& barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    Run();
}

// Closes the list, runs it and waits for it.
void Addon::Run()
{
    winrt::check_hresult(list->Close());
    ID3D12CommandList* lists[] = { list.get() };
    queue->ExecuteCommandLists(1, lists);
    winrt::check_hresult(queue->Signal(done.get(), ++doneValue));
    if (done->GetCompletedValue() < doneValue)
    {
        winrt::check_hresult(done->SetEventOnCompletion(doneValue, idle.get()));
        WaitForSingleObject(idle.get(), INFINITE);
    }
    winrt::check_hresult(device->GetDeviceRemovedReason());
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
        return new Addon(*host);
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
        static_cast<Addon*>(addon)->Frame(*frame);
        return true;
    }
    catch (const std::exception& e)
    {
        Fill(error, errorSize, e.what());
    }
    catch (const winrt::hresult_error& e)
    {
        Fill(error, errorSize, "Direct3D 12 failed: " + winrt::to_string(e.message()));
    }
    return false;
}

void Destroy(void* addon)
{
    delete static_cast<Addon*>(addon);
}

const UnishadeAddon kAddon = {
    sizeof(UnishadeAddon),
    UNISHADE_API_VERSION,
    UNISHADE_WRITES_PICTURE | UNISHADE_READS_DEPTH | UNISHADE_READS_MOTION | UNISHADE_TAKES_HDR,
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
