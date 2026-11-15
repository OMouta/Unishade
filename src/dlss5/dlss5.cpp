#include "dlss5.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// nvngx_dlssnr.dll only takes calls from NVIDIA's NGX core: it looks at the module the calling code is in, and unless
// that module's path contains "nvngx.dll", as the core's, _nvngx.dll, does, its Init_Ext fails with PlatformError and
// logs nothing. The calls are made from this file, so the module it is built into is named Unishade_nvngx.dll.

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
// Identifies Unishade to the NGX core, which takes a project ID from apps NVIDIA has not given an application ID.
constexpr char kProjectId[] = "33c965e4-ca76-43bc-ab68-435e2b780a2c";
constexpr UINT kNvidia = 0x10DE;

// The feature DLL's exports, as nvsdk_ngx.h declares them for NGX_SNIPPET_BUILD.
using RuntimeInit = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using RuntimeShutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using RuntimePopulate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using RuntimeCreate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using RuntimeEvaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using RuntimeRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using RuntimeApiVersion = unsigned(NVSDK_CONV*)();
using RuntimeApplicationId = unsigned long long(NVSDK_CONV*)();

// NVIDIA's headers declare the SDK library that wraps the core, not the core's own exports. These are the signatures
// OptiScaler forwards games' calls to the core with. The core's Init_ProjectID takes the version before the feature
// info, unlike the library's, and its Shutdown1 also reports how many initializations remain.
using CoreInit = NVSDK_NGX_Result(NVSDK_CONV*)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using CoreParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using CoreDestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using CoreShutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*, unsigned*);

Dlss5LogCallback logCallback;
Dlss5* instance;

std::string Format(const char* format, va_list args)
{
    char text[1024];
    std::vsnprintf(text, sizeof(text), format, args);
    return text;
}

void Log(const char* format, ...)
{
    if (!logCallback)
        return;
    va_list args;
    va_start(args, format);
    const std::string line = Format(format, args);
    va_end(args);
    logCallback(line.c_str());
}

[[noreturn]] void Fail(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const std::string message = Format(format, args);
    va_end(args);
    throw std::runtime_error(message);
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if (!logCallback || !message)
        return;
    std::string line = "[NGX] ";
    line += message;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    logCallback(line.c_str());
}

std::string ResultText(NVSDK_NGX_Result result)
{
    static constexpr const char* kFailures[] = { "Fail", "FeatureNotSupported", "PlatformError", "FeatureAlreadyExists",
        "FeatureNotFound", "InvalidParameter", "ScratchBufferTooSmall", "NotInitialized", "UnsupportedInputFormat",
        "RWFlagMissing", "MissingInput", "UnableToInitializeFeature", "OutOfDate", "OutOfGPUMemory", "UnsupportedFormat",
        "UnableToWriteToAppDataPath", "UnsupportedParameter", "Denied", "NotImplemented" };
    char text[64];
    const unsigned code = static_cast<unsigned>(result) & 0xFFFFF;
    if (NVSDK_NGX_FAILED(result) && code < std::size(kFailures))
        std::snprintf(text, sizeof(text), "0x%08X (%s)", static_cast<unsigned>(result), kFailures[code]);
    else
        std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
    return text;
}

void Check(NVSDK_NGX_Result result, const char* call)
{
    if (NVSDK_NGX_FAILED(result))
        Fail("%s failed: %s", call, ResultText(result).c_str());
}

void Check(HRESULT result, const char* call)
{
    if (FAILED(result))
        Fail("%s failed: 0x%08X", call, static_cast<unsigned>(result));
}

template <typename Function>
void Resolve(HMODULE module, const char* file, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!function)
        Fail("%s has no export %s.", file, name);
}

struct Feature
{
    NVSDK_NGX_Handle* handle = nullptr;
    bool reset = true;
};
} // namespace

struct Dlss5
{
    winrt::com_ptr<ID3D12Device> device;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE idle = nullptr;

    HMODULE core = nullptr;
    CoreInit coreInit = nullptr;
    CoreParameters coreParameters = nullptr;
    CoreDestroyParameters coreDestroyParameters = nullptr;
    CoreShutdown coreShutdown = nullptr;
    bool coreStarted = false;
    NVSDK_NGX_Parameter* parameters = nullptr;

    HMODULE runtime = nullptr;
    RuntimeInit init = nullptr;
    RuntimeShutdown shutdown = nullptr;
    RuntimePopulate populate = nullptr;
    RuntimeCreate create = nullptr;
    RuntimeEvaluate evaluate = nullptr;
    RuntimeRelease release = nullptr;
    RuntimeApiVersion apiVersion = nullptr;
    RuntimeApplicationId applicationId = nullptr;
    bool runtimeStarted = false;

    // One feature for each pass, since each keeps a history of the frames it was given.
    std::vector<Feature> features;
    UINT64 width = 0;
    UINT height = 0;
    // Passes after the first take turns writing to these.
    std::array<winrt::com_ptr<ID3D12Resource>, 2> between;
    std::string error;

    ~Dlss5();
    void CreateDevice();
    void LoadRuntime(const wchar_t* path);
    void StartCore(const wchar_t* dataPath);
    void StartRuntime(const wchar_t* dataPath);
    void Evaluate(ID3D12Resource* color, ID3D12Resource* output, const Dlss5Look& look);
    void Record(ID3D12Resource* color, ID3D12Resource* output, const Dlss5Look& look);
    void ReleaseFeature(Feature& feature);
    void ReleaseFeatures();
    void WaitForGpu();
};

Dlss5::~Dlss5()
{
    // Evaluate waits for the GPU, so none of this is in use.
    ReleaseFeatures();
    if (runtimeStarted)
    {
        const NVSDK_NGX_Result result = shutdown(device.get());
        if (NVSDK_NGX_FAILED(result))
            Log("nvngx_dlssnr.dll NVSDK_NGX_D3D12_Shutdown1 failed: %s", ResultText(result).c_str());
    }
    if (parameters)
        coreDestroyParameters(parameters);
    if (coreStarted)
    {
        unsigned remaining = 0;
        const NVSDK_NGX_Result result = coreShutdown(device.get(), &remaining);
        if (NVSDK_NGX_FAILED(result))
            Log("_nvngx.dll NVSDK_NGX_D3D12_Shutdown1 failed: %s", ResultText(result).c_str());
    }
    if (runtime)
        FreeLibrary(runtime);
    if (core)
        FreeLibrary(core);
    if (idle)
        CloseHandle(idle);
    Log("Shut down");
}

void Dlss5::CreateDevice()
{
    winrt::com_ptr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())), "CreateDXGIFactory1");
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT index = 0; !device && factory->EnumAdapters1(index, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++index, adapter = nullptr)
    {
        DXGI_ADAPTER_DESC1 description;
        Check(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if (description.VendorId != kNvidia || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        Check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())), "D3D12CreateDevice");
        Log("Made a D3D12 device on %ls", description.Description);
    }
    if (!device)
        Fail("There is no NVIDIA graphics card.");

    const D3D12_COMMAND_QUEUE_DESC queueDescription{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(queue.put())), "CreateCommandQueue");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateCommandAllocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())), "CreateCommandList");
    Check(list->Close(), "ID3D12GraphicsCommandList::Close");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence");
    idle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!idle)
        Fail("CreateEvent failed: %lu", GetLastError());
}

void Dlss5::LoadRuntime(const wchar_t* path)
{
    runtime = LoadLibraryW(path);
    if (!runtime)
        Fail("Could not load %ls: error %lu", path, GetLastError());
    const char file[] = "nvngx_dlssnr.dll";
    Resolve(runtime, file, "NVSDK_NGX_D3D12_Init_Ext", init);
    Resolve(runtime, file, "NVSDK_NGX_D3D12_Shutdown1", shutdown);
    Resolve(runtime, file, "NVSDK_NGX_D3D12_PopulateParameters_Impl", populate);
    Resolve(runtime, file, "NVSDK_NGX_D3D12_CreateFeature", create);
    Resolve(runtime, file, "NVSDK_NGX_D3D12_EvaluateFeature", evaluate);
    Resolve(runtime, file, "NVSDK_NGX_D3D12_ReleaseFeature", release);
    Resolve(runtime, file, "NVSDK_NGX_GetAPIVersion", apiVersion);
    Resolve(runtime, file, "NVSDK_NGX_GetApplicationId", applicationId);
    Log("Loaded %ls: NGX API 0x%X, application ID 0x%llX", path, apiVersion(), applicationId());
}

void Dlss5::StartCore(const wchar_t* dataPath)
{
    wchar_t folder[MAX_PATH];
    DWORD size = sizeof(folder);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ,
                     nullptr, folder, &size) != ERROR_SUCCESS)
        Fail("The NVIDIA driver has no NGX core: HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore has no FullPath.");
    const std::wstring path = std::wstring(folder) + L"\\_nvngx.dll";
    core = LoadLibraryW(path.c_str());
    if (!core)
        Fail("Could not load %ls: error %lu", path.c_str(), GetLastError());
    const char file[] = "_nvngx.dll";
    Resolve(core, file, "NVSDK_NGX_D3D12_Init_ProjectID", coreInit);
    Resolve(core, file, "NVSDK_NGX_D3D12_GetCapabilityParameters", coreParameters);
    Resolve(core, file, "NVSDK_NGX_D3D12_DestroyParameters", coreDestroyParameters);
    Resolve(core, file, "NVSDK_NGX_D3D12_Shutdown1", coreShutdown);
    Log("Loaded %ls", path.c_str());

    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = logCallback != nullptr;
    Check(coreInit(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, UNISHADE_VERSION, dataPath, device.get(), NVSDK_NGX_Version_API, &info),
          "_nvngx.dll NVSDK_NGX_D3D12_Init_ProjectID");
    coreStarted = true;
    Check(coreParameters(&parameters), "_nvngx.dll NVSDK_NGX_D3D12_GetCapabilityParameters");

    // Whether the core could load nvngx_dlssnr.dll itself, which it is not asked to. Only for diagnosis.
    Log("Started the NGX core");
    for (const char* name : { "DLSSNR.Available", "DLSSNR.NeedsUpdatedDriver", "DLSSNR.FeatureInitResult" })
    {
        int value = 0;
        const NVSDK_NGX_Result result = parameters->Get(name, &value);
        if (NVSDK_NGX_FAILED(result))
            Log("The core does not report %s: %s", name, ResultText(result).c_str());
        else
            Log("The core reports %s=%d (0x%08X)", name, value, static_cast<unsigned>(value));
    }
}

void Dlss5::StartRuntime(const wchar_t* dataPath)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
    Log("Calling nvngx_dlssnr.dll from %ls", self);
    const NVSDK_NGX_Result result = init(applicationId(), dataPath, device.get(), static_cast<NVSDK_NGX_Version>(apiVersion()), parameters);
    std::wstring path = self;
    std::transform(path.begin(), path.end(), path.begin(), towlower);
    if (NVSDK_NGX_FAILED(result) && path.find(L"nvngx.dll") == std::wstring::npos)
        Fail("nvngx_dlssnr.dll NVSDK_NGX_D3D12_Init_Ext failed: %s. It only takes calls from a module whose path contains "
             "nvngx.dll, and they come from %ls.", ResultText(result).c_str(), self);
    Check(result, "nvngx_dlssnr.dll NVSDK_NGX_D3D12_Init_Ext");
    runtimeStarted = true;
    Check(populate(parameters), "nvngx_dlssnr.dll NVSDK_NGX_D3D12_PopulateParameters_Impl");
    Log("Started nvngx_dlssnr.dll");
}

void Dlss5::Evaluate(ID3D12Resource* color, ID3D12Resource* output, const Dlss5Look& look)
{
    if (!color || !output || color == output)
        Fail("Color and output have to be two textures.");
    if (look.passCount < 1 || look.passCount > 10)
        Fail("The pass count is %d, not 1 to 10.", look.passCount);
    const D3D12_RESOURCE_DESC in = color->GetDesc(), out = output->GetDesc();
    if (in.Width != out.Width || in.Height != out.Height)
        Fail("Color is %llux%u but output is %llux%u.", in.Width, in.Height, out.Width, out.Height);
    if (!(out.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        Fail("Output does not allow unordered access.");

    if (out.Width != width || out.Height != height)
    {
        ReleaseFeatures();
        between = {};
        width = out.Width;
        height = out.Height;
    }
    while (features.size() > static_cast<size_t>(look.passCount))
    {
        ReleaseFeature(features.back());
        features.pop_back();
    }
    for (size_t index = 0; index < between.size() && index + 1 < static_cast<size_t>(look.passCount); ++index)
    {
        if (between[index] && between[index]->GetDesc().Format == out.Format)
            continue;
        between[index] = nullptr;
        const D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC description = out;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(between[index].put())), "CreateCommittedResource");
    }

    Check(allocator->Reset(), "ID3D12CommandAllocator::Reset");
    Check(list->Reset(allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset");
    try
    {
        Record(color, output, look);
    }
    catch (...)
    {
        // Nothing recorded runs, including the setup of features made in this call.
        list->Close();
        ReleaseFeatures();
        throw;
    }
    Check(list->Close(), "ID3D12GraphicsCommandList::Close");
    ID3D12CommandList* lists[] = { list.get() };
    queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    Check(device->GetDeviceRemovedReason(), "The D3D12 device");
    for (Feature& feature : features)
        feature.reset = false;
}

void Dlss5::Record(ID3D12Resource* color, ID3D12Resource* output, const Dlss5Look& look)
{
    struct Tracked
    {
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    std::array<Tracked, 4> tracked{ { { color }, { output }, { between[0].get() }, { between[1].get() } } };
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    // NGX reads its inputs as shader resources and writes its output through unordered access.
    const auto move = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
        for (Tracked& entry : tracked)
        {
            if (entry.resource != resource || entry.state == state)
                continue;
            D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, entry.state, state };
            barriers.push_back(barrier);
            entry.state = state;
        }
    };
    const auto flush = [&] {
        if (!barriers.empty())
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        barriers.clear();
    };
    const auto setRect = [&](const std::string& name) {
        parameters->Set((name + "SubrectBaseX").c_str(), 0u);
        parameters->Set((name + "SubrectBaseY").c_str(), 0u);
        parameters->Set((name + "SubrectWidth").c_str(), static_cast<unsigned>(width));
        parameters->Set((name + "SubrectHeight").c_str(), height);
    };

    ID3D12Resource* source = color;
    for (int pass = 0; pass < look.passCount; ++pass)
    {
        ID3D12Resource* target = pass + 1 == look.passCount ? output : between[pass % 2].get();
        move(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        move(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flush();

        // Names and types are the ones DLSS5ForUE5 passes. The DLL contains each name.
        parameters->Set("DLSSNR.Color", source);
        parameters->Set("DLSSNR.Output", target);
        setRect("DLSSNR.Color");
        setRect("DLSSNR.Output");
        parameters->Set("DLSSNR.Width", static_cast<unsigned>(width));
        parameters->Set("DLSSNR.Height", height);
        parameters->Set("DLSSNR.ScalingRatio", 1.0f);
        parameters->Set("DLSSNR.Enabled", 1);

        if (features.size() <= static_cast<size_t>(pass))
            features.emplace_back();
        Feature& feature = features[pass];
        if (!feature.handle)
        {
            parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            parameters->Set("DLSSNR.Reset", 1);
            const NVSDK_NGX_Result result = create(list.get(), NVSDK_NGX_Feature_Reserved18, parameters, &feature.handle);
            if (NVSDK_NGX_FAILED(result))
            {
                ReleaseFeature(feature);
                Fail("nvngx_dlssnr.dll NVSDK_NGX_D3D12_CreateFeature for %llux%u failed: %s", width, height, ResultText(result).c_str());
            }
            feature.reset = true;
            Log("Made the feature for pass %d at %llux%u", pass + 1, width, height);
        }

        parameters->Set("DLSSNR.Reset", feature.reset ? 1 : 0);
        parameters->Set("DLSSNR.Style", look.style);
        parameters->Set("DLSSNR.Intensity", look.intensity);
        parameters->Set("DLSSNR.LocalToneStrength", look.localTone);
        parameters->Set("DLSSNR.LocalStructureStrength", look.localStructure);
        parameters->Set("DLSSNR.SkinStructureStrength", look.skinStructure);
        parameters->Set("DLSSNR.UseAutoMask", look.autoMask ? 1 : 0);
        Check(evaluate(list.get(), feature.handle, parameters, nullptr), "nvngx_dlssnr.dll NVSDK_NGX_D3D12_EvaluateFeature");
        source = target;
    }
    for (Tracked& entry : tracked)
        if (entry.resource)
            move(entry.resource, D3D12_RESOURCE_STATE_COMMON);
    flush();
}

void Dlss5::ReleaseFeature(Feature& feature)
{
    if (!feature.handle)
        return;
    const NVSDK_NGX_Result result = release(feature.handle);
    if (NVSDK_NGX_FAILED(result))
        Log("nvngx_dlssnr.dll NVSDK_NGX_D3D12_ReleaseFeature failed: %s", ResultText(result).c_str());
    feature.handle = nullptr;
}

void Dlss5::ReleaseFeatures()
{
    for (Feature& feature : features)
        ReleaseFeature(feature);
    features.clear();
}

void Dlss5::WaitForGpu()
{
    Check(queue->Signal(fence.get(), ++fenceValue), "ID3D12CommandQueue::Signal");
    if (fence->GetCompletedValue() >= fenceValue)
        return;
    Check(fence->SetEventOnCompletion(fenceValue, idle), "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObject(idle, INFINITE);
}

Dlss5* Dlss5Create(const wchar_t* runtimePath, const wchar_t* dataPath, Dlss5LogCallback log, char* error, size_t errorSize)
{
    const auto report = [&](const char* message) {
        if (error && errorSize)
            std::snprintf(error, errorSize, "%s", message);
    };
    if (instance)
    {
        report("DLSS5 is already running.");
        return nullptr;
    }
    logCallback = log;
    auto dlss5 = std::make_unique<Dlss5>();
    try
    {
        dlss5->CreateDevice();
        dlss5->LoadRuntime(runtimePath);
        dlss5->StartCore(dataPath);
        dlss5->StartRuntime(dataPath);
    }
    catch (const std::exception& exception)
    {
        report(exception.what());
        dlss5.reset();
        logCallback = nullptr;
        return nullptr;
    }
    instance = dlss5.get();
    return dlss5.release();
}

ID3D12Device* Dlss5Device(Dlss5* dlss5)
{
    return dlss5->device.get();
}

bool Dlss5Evaluate(Dlss5* dlss5, ID3D12Resource* color, ID3D12Resource* output, const Dlss5Look* look)
{
    try
    {
        if (!look)
            Fail("No look settings were given.");
        dlss5->Evaluate(color, output, *look);
        dlss5->error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        dlss5->error = exception.what();
        return false;
    }
}

const char* Dlss5Error(const Dlss5* dlss5)
{
    return dlss5->error.c_str();
}

void Dlss5Destroy(Dlss5* dlss5)
{
    if (!dlss5)
        return;
    delete dlss5;
    instance = nullptr;
    logCallback = nullptr;
}
