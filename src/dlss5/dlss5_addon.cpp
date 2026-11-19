// DLSS5 as a Unishade add-on. It is part of Unishade_nvngx.dll, since nvngx_dlssnr.dll only takes calls from a module
// whose path contains nvngx.dll, and runs dlss5.cpp on every frame before effects. nvngx_dlssnr.dll goes in
// Addons/Unishade_nvngx. Evaluating waits for the graphics card, so the frame waits on the processor too.

#include "dlss5.h"
#include <addons/unishade_addon.h>

#include <d3d12.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
// Every pass keeps its own copy of the network, about 290 MB at 1582x863.
constexpr int kMostPasses = 4;

const UnishadeSetting kSettings[] = {
    // nvngx_dlssnr.dll 310.8.0.0 has one more look, for every style but 0 and 1. The name is Unishade's.
    { "Style", "Style", nullptr, UNISHADE_SETTING_CHOICE, 0, 0, 2, "Natural\0Cinematic\0Soft\0" },
    { "Intensity", "Intensity", "From the game's picture as it is to the full effect.", UNISHADE_SETTING_SLIDER, 1, 0, 1, nullptr },
    // The DLL does not clamp the strengths. 1 is its own default.
    { "LocalToneStrength", "Local tone", nullptr, UNISHADE_SETTING_SLIDER, 1, 0, 2, nullptr },
    { "LocalStructureStrength", "Structure", nullptr, UNISHADE_SETTING_SLIDER, 1, 0, 2, nullptr },
    { "SkinStructureStrength", "Skin structure", "Structure on characters, when Auto mask is on.", UNISHADE_SETTING_SLIDER, 1, 0, 2, nullptr },
    { "AutoMask", "Auto mask", "Finds characters, so Skin structure applies to them.", UNISHADE_SETTING_TOGGLE, 1, 0, 1, nullptr },
    { "PassCount", "Passes", "Runs DLSS5 again on its own result. Each pass takes more of the graphics card's memory.",
      UNISHADE_SETTING_INTEGER, 1, 1, kMostPasses, nullptr },
};

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
        winrt::check_hresult(device->OpenSharedHandle(frame.fence, IID_PPV_ARGS(fence.put())));
        winrt::check_hresult(device->OpenSharedHandle(frame.color.handle, IID_PPV_ARGS(color.put())));
        winrt::check_hresult(device->OpenSharedHandle(frame.output.handle, IID_PPV_ARGS(output.put())));
    }
    const float* values = frame.settings;
    const Dlss5Look wanted{ static_cast<int>(values[0]), values[1], values[2], values[3], values[4], values[5] != 0,
                            std::clamp(static_cast<int>(values[6]), 1, kMostPasses) };
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
    if (!same && !Dlss5Evaluate(dlss5, color.get(), output.get(), &look))
        throw std::runtime_error(Dlss5Error(dlss5));
    winrt::check_hresult(fence->Signal(frame.signal));
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
    UNISHADE_ADDON_VERSION,
    "DLSS5",
    "NVIDIA's DLSS neural rendering on the game's picture. Needs an NVIDIA RTX graphics card.",
    UNISHADE_ADDON_CHANGES_PICTURE,
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
