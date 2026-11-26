// DLSS5 as a Unishade add-on. It is part of Unishade_nvngx.dll, since nvngx_dlssnr.dll only takes calls from a module
// whose path contains nvngx.dll, and runs dlss5.cpp on every frame before effects. nvngx_dlssnr.dll goes in its folder
// with it. Evaluating waits for the graphics card, so the frame waits on the processor too.
//
// Nothing known about nvngx_dlssnr.dll says it takes HDR color: .plan/dlss5-research.md lists no such parameter or
// creation flag. So an HDR picture is brought into an SDR one for it with a curve that can be undone, and what it
// changed is brought back.

#include "dlss5.h"
#include <unishade_addon.h>

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
constexpr const char* kEntryPoints[] = { "ToSdr", "ToHdr" };

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
    void EvaluateHdr(float white);
    void CreateConverter();
    // Runs one of the shaders over target, reading up to three textures, and waits for it.
    void Convert(size_t shader, std::array<ID3D12Resource*, 3> inputs, ID3D12Resource* target, float white);

    Dlss5* dlss5 = nullptr;
    ID3D12Device* device = nullptr;
    winrt::handle event;
    // Unishade's, for the frame's generation.
    uint64_t generation = UINT64_MAX;
    winrt::com_ptr<ID3D12Fence> fence;
    winrt::com_ptr<ID3D12Resource> color;
    winrt::com_ptr<ID3D12Resource> output;
    // What the last frame was evaluated with, so the same picture with the same look is not evaluated again.
    uint64_t picture = 0;
    Dlss5Look look{};

    // For HDR pictures, made with the first one: the shaders, and the SDR picture DLSS5 gets and the one it makes.
    winrt::com_ptr<ID3D12RootSignature> rootSignature;
    std::array<winrt::com_ptr<ID3D12PipelineState>, 2> shaders;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> done;
    uint64_t doneValue = 0;
    winrt::handle idle;
    winrt::com_ptr<ID3D12Resource> given;
    winrt::com_ptr<ID3D12Resource> changed;
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
        color = output = nullptr;
        fence = nullptr;
        given = changed = nullptr;
        winrt::check_hresult(device->OpenSharedHandle(frame.fence, IID_PPV_ARGS(fence.put())));
        winrt::check_hresult(device->OpenSharedHandle(frame.color.handle, IID_PPV_ARGS(color.put())));
        winrt::check_hresult(device->OpenSharedHandle(frame.output.handle, IID_PPV_ARGS(output.put())));
    }
    const auto value = [&](int setting) { return frame.settings[setting][0]; };
    const Dlss5Look wanted{ static_cast<int>(value(kStyle)), value(kIntensity),       value(kLocalTone),
                            value(kLocalStructure),          value(kSkinStructure),   value(kAutoMask) != 0,
                            std::clamp(static_cast<int>(value(kPassCount)), 1, kMostPasses) };
    const bool same = frame.generation == generation && frame.picture == picture && wanted.style == look.style &&
                      wanted.intensity == look.intensity && wanted.localTone == look.localTone && wanted.localStructure == look.localStructure &&
                      wanted.skinStructure == look.skinStructure && wanted.autoMask == look.autoMask && wanted.passCount == look.passCount;
    generation = frame.generation;
    picture = frame.picture;
    look = wanted;

    // Dlss5Evaluate waits for the graphics card itself, so the picture has to be there before it starts. The fence is
    // only set after that, since it cannot go back.
    winrt::check_hresult(fence->SetEventOnCompletion(frame.wait, event.get()));
    if (WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0)
        throw std::runtime_error("Unishade's picture did not arrive.");
    // The format follows the game's, and a change makes the images again, which changes the generation.
    if (!same && frame.color.format == UNISHADE_FORMAT_RGBA16_FLOAT)
        EvaluateHdr(frame.sdrWhite);
    else if (!same && !Dlss5Evaluate(dlss5, color.get(), output.get(), &look))
        throw std::runtime_error(Dlss5Error(dlss5));
    winrt::check_hresult(fence->Signal(frame.signal));
}

void Addon::EvaluateHdr(float white)
{
    if (!rootSignature)
        CreateConverter();
    if (!given)
    {
        const D3D12_RESOURCE_DESC source = color->GetDesc();
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = source.Width;
        description.Height = source.Height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        // As Unishade's SDR pictures, which DLSS5 is known to take.
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc = { 1, 0 };
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_HEAP_PROPERTIES heapType{ D3D12_HEAP_TYPE_DEFAULT };
        for (winrt::com_ptr<ID3D12Resource>* texture : { &given, &changed })
            winrt::check_hresult(device->CreateCommittedResource(&heapType, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                 IID_PPV_ARGS(texture->put())));
    }
    Convert(0, { color.get(), nullptr, nullptr }, given.get(), white);
    if (!Dlss5Evaluate(dlss5, given.get(), changed.get(), &look))
        throw std::runtime_error(Dlss5Error(dlss5));
    Convert(1, { color.get(), given.get(), changed.get() }, output.get(), white);
}

void Addon::CreateConverter()
{
    // Loaded here, so SDR pictures do not need it. The compiled code and the errors are blobs it made, which call into
    // it when they are released, so it is freed after them.
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> compiler(LoadLibraryW(L"d3dcompiler_47.dll"), FreeLibrary);
    const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler.get(), "D3DCompile")) : nullptr;
    if (!compile)
        throw std::runtime_error("HDR pictures need d3dcompiler_47.dll, which Windows could not load.");
    std::array<winrt::com_ptr<ID3DBlob>, 2> code;
    for (size_t i = 0; i < code.size(); ++i)
    {
        winrt::com_ptr<ID3DBlob> errors;
        if (FAILED(compile(kShaders, sizeof(kShaders) - 1, "dlss5", nullptr, nullptr, kEntryPoints[i], "cs_5_0", 0, 0, code[i].put(), errors.put())))
            throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "The HDR shaders did not compile.");
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
}

void Addon::Convert(size_t shader, std::array<ID3D12Resource*, 3> inputs, ID3D12Resource* target, float white)
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
    UNISHADE_WRITES_PICTURE | UNISHADE_TAKES_HDR,
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
