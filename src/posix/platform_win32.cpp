// Windows: windows and processes through the Win32 API, and the game's picture through Windows.Graphics.Capture,
// copied into Direct3D 11 textures that Vulkan reads as its own images, or through memory where it cannot.
#include <unknwn.h>

#include "platform.h"
#include "gpu.h"
#include "log.h"
#include "text.h"
#include "theme.h"
#include "ui/caption.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <vulkan/vulkan_win32.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>

namespace wgc = winrt::Windows::Graphics::Capture;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace platform
{
namespace
{
HWND Handle(WindowId id)
{
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(id));
}

WindowId Id(HWND window)
{
    return static_cast<WindowId>(reinterpret_cast<uintptr_t>(window));
}

bool Cloaked(HWND window)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// The ReShade host's rule: visible, not owned, not a tool window, not cloaked, with a client area and a title.
// Unishade's own windows never count.
bool IsGameWindow(HWND window, DWORD process)
{
    if (process == GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || Cloaked(window))
        return false;
    RECT client{};
    return GetClientRect(window, &client) && client.right > 0 && client.bottom > 0 && GetWindowTextLengthW(window) > 0;
}

std::optional<Window> Listed(HWND window)
{
    DWORD process = 0;
    if (!GetWindowThreadProcessId(window, &process) || !IsGameWindow(window, process))
        return std::nullopt;
    std::wstring title(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    title.resize(static_cast<size_t>(GetWindowTextW(window, title.data(), static_cast<int>(title.size()))));
    if (title.empty())
        return std::nullopt;
    return Window{ Id(window), Utf8(title), static_cast<int>(process) };
}

std::vector<HWND> TopLevelWindows()
{
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND window, LPARAM found) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(found)->push_back(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// Runs work on a thread in a single-threaded apartment, which the shell's functions want, and waits for it. The main
// thread's apartment is multithreaded, as Windows.Graphics.Capture has it in the ReShade host.
template <typename Work>
void InSingleThreadedApartment(Work work)
{
    std::thread([&work] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        work();
        if (SUCCEEDED(com))
            CoUninitialize();
    }).join();
}

// Shortcuts, the notification area icon, Setup's request to quit and the end of the session arrive at a hidden window
// of the main thread, which GLFW's message loop hands them to. Not a message-only window, since Windows sends
// TaskbarCreated and the end of the session only to top-level windows.
HWND messageWindow = nullptr;
HotkeyCallback hotkeyCallback;
// Windows only says that a shortcut was pressed, so one held down ends when its key is up again.
struct Registration
{
    UINT key = 0;
    bool held = false;
};
std::map<int, Registration> registrations;
std::vector<int> pressedHotkeys;

constexpr UINT kTrayMessage = WM_APP + 16;
constexpr UINT kTrayIcon = 1;
constexpr UINT_PTR kTrayTimer = 1;
enum TrayCommand : UINT
{
    kOpenCommand = 1,
    kEffectsCommand,
    kQuitCommand,
};
Tray tray;
bool trayWanted = false;
bool trayShown = false;
HICON trayIcon = nullptr;
UINT taskbarCreated = 0;
std::function<void()> sessionEnd;

NOTIFYICONDATAW TrayData()
{
    NOTIFYICONDATAW data{ sizeof(data) };
    data.hWnd = messageWindow;
    data.uID = kTrayIcon;
    return data;
}

// Tried again every few seconds while the notification area is not ready.
void ShowTrayIcon()
{
    if (!trayIcon)
        trayIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                 GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    NOTIFYICONDATAW data = TrayData();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = trayIcon;
    wcscpy_s(data.szTip, std::size(data.szTip), L"Unishade");
    // TaskbarCreated also comes when the taskbar's scaling changes, while the icon is still there.
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayShown = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (trayShown)
    {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    else
        SetTimer(messageWindow, kTrayTimer, 5000, nullptr);
}

void TrayMenu(int x, int y)
{
    const HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, kOpenCommand, L"Open Unishade");
    AppendMenuW(menu, MF_STRING | (tray.effectsShown() ? MF_CHECKED : MF_UNCHECKED), kEffectsCommand, L"Show effects");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit");
    SetMenuDefaultItem(menu, kOpenCommand, FALSE);
    // Otherwise the menu stays open when clicking elsewhere.
    SetForegroundWindow(messageWindow);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, align | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, messageWindow, nullptr));
    PostMessageW(messageWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command)
    {
    case kOpenCommand: tray.open(); break;
    case kEffectsCommand: tray.toggleEffects(); break;
    case kQuitCommand: tray.quit(); break;
    }
}

LRESULT CALLBACK MessageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == taskbarCreated && taskbarCreated)
    {
        if (trayWanted)
            ShowTrayIcon();
        return 0;
    }
    switch (message)
    {
    case WM_HOTKEY:
        pressedHotkeys.push_back(static_cast<int>(wParam));
        return 0;
    case WM_TIMER:
        if (wParam == kTrayTimer)
        {
            KillTimer(window, kTrayTimer);
            if (trayWanted)
                ShowTrayIcon();
        }
        return 0;
    case kTrayMessage:
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT: tray.open(); break;
        case WM_CONTEXTMENU: TrayMenu(static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))); break;
        }
        return 0;
    // Setup asks Unishade to quit before it replaces or removes it, as the notification area's Quit does.
    case WM_CLOSE:
        if (tray.quit)
            tray.quit();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    // Windows ends the process once this returns.
    case WM_ENDSESSION:
        if (wParam && sessionEnd)
            sessionEnd();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

UINT VirtualKey(ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return 'A' + (key - ImGuiKey_A);
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return '0' + (key - ImGuiKey_0);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return VK_F1 + (key - ImGuiKey_F1);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return VK_NUMPAD0 + (key - ImGuiKey_Keypad0);
    switch (key)
    {
    case ImGuiKey_Home: return VK_HOME;
    case ImGuiKey_End: return VK_END;
    case ImGuiKey_Insert: return VK_INSERT;
    case ImGuiKey_Delete: return VK_DELETE;
    case ImGuiKey_PageUp: return VK_PRIOR;
    case ImGuiKey_PageDown: return VK_NEXT;
    case ImGuiKey_Pause: return VK_PAUSE;
    case ImGuiKey_ScrollLock: return VK_SCROLL;
    case ImGuiKey_Space: return VK_SPACE;
    case ImGuiKey_Tab: return VK_TAB;
    case ImGuiKey_Escape: return VK_ESCAPE;
    case ImGuiKey_LeftArrow: return VK_LEFT;
    case ImGuiKey_RightArrow: return VK_RIGHT;
    case ImGuiKey_UpArrow: return VK_UP;
    case ImGuiKey_DownArrow: return VK_DOWN;
    case ImGuiKey_KeypadMultiply: return VK_MULTIPLY;
    case ImGuiKey_KeypadAdd: return VK_ADD;
    case ImGuiKey_KeypadSubtract: return VK_SUBTRACT;
    case ImGuiKey_KeypadDecimal: return VK_DECIMAL;
    case ImGuiKey_KeypadDivide: return VK_DIVIDE;
    default: return 0;
    }
}

std::string Describe(const winrt::hresult_error& e)
{
    char code[16];
    snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(e.code()));
    return Utf8(e.message()) + " (0x" + code + ")";
}

// Asks Windows once, without waiting, to capture without the border it draws around a captured window. Programs that
// are not packaged are allowed without a prompt.
std::atomic<bool> borderRequired = false;
constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";

void RequestBorderlessCapture()
{
    using winrt::Windows::Foundation::AsyncStatus;
    using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
    if (!ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired"))
        return;
    try
    {
        wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).Completed([](const auto& request, AsyncStatus status) {
            if (status != AsyncStatus::Completed || request.GetResults() != AppCapabilityAccessStatus::Allowed)
            {
                borderRequired = true;
                Log(LogLevel::Info, "Windows did not allow capture without a border, so it may draw one around the game.");
            }
        });
    }
    catch (const winrt::hresult_error& e)
    {
        borderRequired = true;
        Log(LogLevel::Info, "Could not ask to capture without a border: %s.", Describe(e).c_str());
    }
}

// HDR

// The window's monitor and Windows' display paths only share the GDI device name, such as \\.\DISPLAY1.
bool FindDisplayPath(HWND window, DISPLAYCONFIG_PATH_INFO& found)
{
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitorInfo))
        return false;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    for (UINT32 i = 0; i < pathCount; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && monitorInfo.szDevice == std::wstring(source.viewGdiDeviceName))
        {
            found = paths[i];
            return true;
        }
    }
    // Asked every second, so each display that cannot be found is said once.
    static std::wstring missing;
    if (missing != monitorInfo.szDevice)
    {
        missing = monitorInfo.szDevice;
        Log(LogLevel::Info, "Could not find %s in Windows' display settings, so it is captured without HDR.", Utf8(missing).c_str());
    }
    return false;
}

// How bright Windows makes SDR white on the window's display, in nits, while HDR is on there. Nothing while it is off,
// including wide color on an SDR display, or when the display cannot be found.
std::optional<float> HdrWhiteLevel(HWND window)
{
    DISPLAYCONFIG_PATH_INFO path{};
    if (!FindDisplayPath(window, path))
        return std::nullopt;

    // Wide color on an SDR display is advanced color too, but keeps SDR brightness, so it is captured as SDR.
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
    color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color.header.size = sizeof(color);
    color.header.adapterId = path.targetInfo.adapterId;
    color.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS || !color.advancedColorEnabled || color.wideColorEnforced)
        return std::nullopt;

    // 1000 is 80 nits, scRGB's own SDR white, which is also taken when Windows does not say.
    DISPLAYCONFIG_SDR_WHITE_LEVEL level{};
    level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    level.header.size = sizeof(level);
    level.header.adapterId = path.targetInfo.adapterId;
    level.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&level.header) != ERROR_SUCCESS || level.SDRWhiteLevel == 0)
        return 80.0f;
    return level.SDRWhiteLevel / 1000.0f * 80.0f;
}

// Capture

// While HDR is on, frames are captured in 16-bit floats, since 8 bits would clip them, and turned back into SDR before
// effects see them.
constexpr DirectXPixelFormat kCaptureFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr DirectXPixelFormat kHdrCaptureFormat = DirectXPixelFormat::R16G16B16A16Float;
// HDR can be turned on or off, SDR brightness changed and the game moved to another display while it is captured,
// so the display is asked again this often. Asking takes QueryDisplayConfig, which is too slow for every frame.
constexpr ULONGLONG kHdrCheckInterval = 1000;
constexpr int32_t kCaptureBuffers = 3;
// The host holds up to three frames: the one it shows, the one the graphics card may still draw and the newest one it
// has not taken. Capture copies into a fourth.
constexpr size_t kSlots = 4;

// Draws a texture over the whole target with one triangle, the ReShade host's shader. tonemap does it for an HDR frame.
constexpr char kDrawShader[] = R"(
Texture2D Frame : register(t0);
SamplerState Smooth : register(s0);
cbuffer Hdr : register(b0)
{
    float SdrWhite; // in scRGB, where 1 is 80 nits
};
struct Corner
{
    float4 position : SV_Position;
    float2 at : TEXCOORD;
};

Corner vs(uint id : SV_VertexID)
{
    Corner corner;
    corner.at = float2((id << 1) & 2, id & 2);
    corner.position = float4(corner.at * float2(2, -2) + float2(-1, 1), 0, 1);
    return corner;
}

float4 ps(Corner corner) : SV_Target
{
    return Frame.Sample(Smooth, corner.at);
}

// Turns linear scRGB back into the sRGB an SDR frame has, with SDR white at 1.
float4 tonemap(Corner corner) : SV_Target
{
    float3 color = saturate(Frame.Sample(Smooth, corner.at).rgb / SdrWhite);
    return float4(color <= 0.0031308 ? color * 12.92 : 1.055 * pow(color, 1 / 2.4) - 0.055, 1);
}
)";

struct Direct3D
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice captureDevice{ nullptr };
    // On the graphics card Vulkan uses, so its textures can be shared with Vulkan, which reads one as soon as it gets
    // it. The fence says when the copy into it is done.
    bool shareable = false;
    winrt::com_ptr<ID3D11DeviceContext4> fenceContext;
    winrt::com_ptr<ID3D11Fence> fence;
    uint64_t fenceValue = 0;
    winrt::handle fenceEvent;

    // What drawing a frame as SDR takes, made on first use.
    winrt::com_ptr<ID3D11VertexShader> vertexShader;
    winrt::com_ptr<ID3D11PixelShader> pixelShader;
    winrt::com_ptr<ID3D11PixelShader> tonemapShader;
    winrt::com_ptr<ID3D11Buffer> sdrWhite;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    // Capture's textures are not promised to be readable by shaders. Frames whose texture is not are copied into this
    // one first, and copyFrames is set once a view on one could not be made.
    winrt::com_ptr<ID3D11Texture2D> readable;
    winrt::com_ptr<ID3D11ShaderResourceView> readableView;
    bool copyFrames = false;
    // What frames copied through memory are drawn into, since a texture the processor reads cannot be drawn into.
    winrt::com_ptr<ID3D11Texture2D> drawn;
    winrt::com_ptr<ID3D11RenderTargetView> drawnTarget;
};
Direct3D d3d;

// A texture that frames are copied into: shared with Vulkan, or one the processor reads for copies through memory.
// Destroyed once no frame holds it, which the host only lets happen when the graphics card is done with it.
struct Slot
{
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11RenderTargetView> target; // the shared texture's, made when a frame is first drawn into it
    UINT width = 0;
    UINT height = 0;
    bool shared = false;
    HANDLE handle = nullptr; // the shared texture's, until Vulkan imports it
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    D3D11_MAPPED_SUBRESOURCE mapped{};

    ~Slot()
    {
        if (mapped.pData)
            d3d.context->Unmap(texture.get(), 0);
        if (handle)
            CloseHandle(handle);
        if (image)
            vkDestroyImage(gpu.device, image, nullptr);
        if (memory)
            vkFreeMemory(gpu.device, memory, nullptr);
    }
};

struct Capture
{
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
    wgc::GraphicsCaptureItem::Closed_revoker closed;
    bool idle = false;
    HWND window = nullptr;
    // HdrWhiteLevel of the window's display, asked again at nextHdrCheck. Only the threads frames arrive on use them
    // once capture runs.
    std::optional<float> sdrWhite;
    ULONGLONG nextHdrCheck = 0;
    std::atomic<int> scale = 100; // SetFrameScale's
    // Held while a frame is copied, so stopping waits for it.
    std::mutex copying;
    // The rest is shared with the threads frames arrive on.
    std::mutex mutex;
    std::atomic<bool> running = false;
    std::atomic<bool> onGpu = false;
    SizeInt32 size{};
    std::vector<std::shared_ptr<Slot>> slots;
    Frame ready;
    uint64_t serial = 0;
    std::string error;
};
Capture capture;

void FailCapture(const std::string& message)
{
    {
        std::lock_guard lock(capture.mutex);
        if (!capture.running)
            return;
        capture.error = message;
        capture.running = false;
    }
    glfwPostEmptyEvent();
}

bool CreateDevice()
{
    if (d3d.device && SUCCEEDED(d3d.device->GetDeviceRemovedReason()))
        return true;
    d3d = {};
    // The graphics card Vulkan runs on, found by its LUID.
    VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties);
    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (id.deviceLUIDValid && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        for (UINT i = 0;; ++i)
        {
            winrt::com_ptr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, candidate.put()) != S_OK)
                break;
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(candidate->GetDesc1(&description)) && !memcmp(&description.AdapterLuid, id.deviceLUID, sizeof(LUID)))
            {
                adapter = candidate;
                break;
            }
        }
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
    // The capture pool uses the device from its own threads.
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), inspectable.put()));
    d3d.captureDevice = inspectable.as<IDirect3DDevice>();
    if (adapter)
    {
        const auto device5 = device.try_as<ID3D11Device5>();
        d3d.fenceContext = context.try_as<ID3D11DeviceContext4>();
        d3d.fenceEvent.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        d3d.shareable = device5 && d3d.fenceContext && d3d.fenceEvent &&
                        SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(d3d.fence.put())));
    }
    d3d.device = std::move(device);
    d3d.context = std::move(context);
    return true;
}

// Whether Vulkan can make an image of a shared Direct3D 11 texture of the captured format, to copy from.
bool CanImport()
{
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkPhysicalDeviceImageFormatInfo2 info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
    info.pNext = &externalInfo;
    info.format = VK_FORMAT_B8G8R8A8_UNORM;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties external{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 result{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    result.pNext = &external;
    return vkGetPhysicalDeviceImageFormatProperties2(gpu.physicalDevice, &info, &result) == VK_SUCCESS &&
           (external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

// A slot no frame holds, of the size and kind frames are copied into now, made when there is none and room for one.
// Called with capture.mutex held.
std::shared_ptr<Slot> FreeSlot(UINT width, UINT height, bool shared)
{
    std::erase_if(capture.slots, [&](const std::shared_ptr<Slot>& slot) {
        return slot.use_count() == 1 && (slot->width != width || slot->height != height || slot->shared != shared);
    });
    for (const std::shared_ptr<Slot>& slot : capture.slots)
        if (slot.use_count() == 1)
            return slot;
    if (capture.slots.size() >= kSlots)
        return nullptr;

    auto slot = std::make_shared<Slot>();
    slot->width = width;
    slot->height = height;
    slot->shared = shared;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    if (shared)
    {
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    }
    else
    {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    }
    winrt::check_hresult(d3d.device->CreateTexture2D(&desc, nullptr, slot->texture.put()));
    if (shared)
        winrt::check_hresult(slot->texture.as<IDXGIResource1>()->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                                                    nullptr, &slot->handle));
    capture.slots.push_back(slot);
    return slot;
}

DirectXPixelFormat CaptureFormat()
{
    return capture.sdrWhite ? kHdrCaptureFormat : kCaptureFormat;
}

// Follows HDR turning on or off on the window's display and its SDR brightness changing. On the threads frames
// arrive on.
void UpdateHdr(const wgc::Direct3D11CaptureFramePool& pool)
{
    const ULONGLONG now = GetTickCount64();
    if (now < capture.nextHdrCheck)
        return;
    capture.nextHdrCheck = now + kHdrCheckInterval;
    const std::optional<float> level = HdrWhiteLevel(capture.window);
    if (level.has_value() == capture.sdrWhite.has_value())
    {
        if (level != capture.sdrWhite)
            Log(LogLevel::Info, "SDR white on the game's display changed: %.1f -> %.1f nits.", *capture.sdrWhite, *level);
        capture.sdrWhite = level;
        return;
    }
    // Frames already captured in the old format are drawn by their own format.
    capture.sdrWhite = level;
    pool.Recreate(d3d.captureDevice, CaptureFormat(), kCaptureBuffers, capture.size);
    Log(LogLevel::Info, "HDR turned %s on the game's display, so capture continues in format %d, SDR_white=%.1f nits.", level ? "on" : "off",
        static_cast<int>(CaptureFormat()), level.value_or(80));
}

winrt::com_ptr<ID3DBlob> CompileShader(const char* entry, const char* target)
{
    winrt::com_ptr<ID3DBlob> code;
    winrt::check_hresult(D3DCompile(kDrawShader, sizeof(kDrawShader) - 1, "draw", nullptr, nullptr, entry, target, 0, 0, code.put(), nullptr));
    return code;
}

// Draws the frame into target, which is width by height: smaller for effects below 100%, and an HDR frame as SDR.
// Whether it is HDR comes from the frame's own format, since a frame captured before HDR was turned on or off can still
// arrive.
void DrawFrame(ID3D11Texture2D* frame, const D3D11_TEXTURE2D_DESC& desc, ID3D11RenderTargetView* target, UINT width, UINT height)
{
    if (!d3d.sampler)
    {
        const auto vertex = CompileShader("vs", "vs_4_0");
        const auto pixel = CompileShader("ps", "ps_4_0");
        const auto tonemap = CompileShader("tonemap", "ps_4_0");
        winrt::check_hresult(d3d.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, d3d.vertexShader.put()));
        winrt::check_hresult(d3d.device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, d3d.pixelShader.put()));
        winrt::check_hresult(d3d.device->CreatePixelShader(tonemap->GetBufferPointer(), tonemap->GetBufferSize(), nullptr, d3d.tonemapShader.put()));
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 16;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        winrt::check_hresult(d3d.device->CreateBuffer(&constants, nullptr, d3d.sdrWhite.put()));
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        winrt::check_hresult(d3d.device->CreateSamplerState(&sampler, d3d.sampler.put()));
    }

    // A view made for this draw only, so none outlives the frame's return to the pool.
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    if (!d3d.copyFrames && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        if (const HRESULT made = d3d.device->CreateShaderResourceView(frame, nullptr, view.put()); FAILED(made))
        {
            if (FAILED(d3d.device->GetDeviceRemovedReason()))
                winrt::throw_hresult(made);
            view = nullptr;
            d3d.copyFrames = true;
            Log(LogLevel::Info, "Captured frames cannot be read directly (0x%08X), so they are copied first.", static_cast<unsigned>(made));
        }
    if (!view)
    {
        D3D11_TEXTURE2D_DESC copy{};
        if (d3d.readable)
            d3d.readable->GetDesc(&copy);
        if (copy.Width != desc.Width || copy.Height != desc.Height || copy.Format != desc.Format)
        {
            copy = desc;
            copy.MipLevels = 1;
            copy.ArraySize = 1;
            copy.SampleDesc = { 1, 0 };
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags = 0;
            copy.MiscFlags = 0;
            d3d.readableView = nullptr;
            d3d.readable = nullptr;
            winrt::check_hresult(d3d.device->CreateTexture2D(&copy, nullptr, d3d.readable.put()));
            winrt::check_hresult(d3d.device->CreateShaderResourceView(d3d.readable.get(), nullptr, d3d.readableView.put()));
        }
        d3d.context->CopyResource(d3d.readable.get(), frame);
        view = d3d.readableView;
    }
    const bool hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (hdr)
    {
        const float sdrWhite[4] = { capture.sdrWhite.value_or(80) / 80 };
        d3d.context->UpdateSubresource(d3d.sdrWhite.get(), 0, nullptr, sdrWhite, 0, 0);
    }

    const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    ID3D11ShaderResourceView* views[] = { view.get() };
    ID3D11SamplerState* samplers[] = { d3d.sampler.get() };
    ID3D11Buffer* constants[] = { d3d.sdrWhite.get() };
    d3d.context->IASetInputLayout(nullptr);
    d3d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d.context->VSSetShader(d3d.vertexShader.get(), nullptr, 0);
    d3d.context->PSSetShader(hdr ? d3d.tonemapShader.get() : d3d.pixelShader.get(), nullptr, 0);
    d3d.context->PSSetShaderResources(0, 1, views);
    d3d.context->PSSetSamplers(0, 1, samplers);
    d3d.context->PSSetConstantBuffers(0, 1, constants);
    d3d.context->RSSetState(nullptr);
    d3d.context->RSSetViewports(1, &viewport);
    d3d.context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    d3d.context->OMSetDepthStencilState(nullptr, 0);
    d3d.context->OMSetRenderTargets(1, &target, nullptr);
    d3d.context->Draw(3, 0);
    // Neither stays bound: the frame goes back to the pool and the target is Vulkan's to read.
    views[0] = nullptr;
    d3d.context->PSSetShaderResources(0, 1, views);
    d3d.context->OMSetRenderTargets(0, nullptr, nullptr);
}

// Draws the frame into the slot. A frame copied through memory is drawn into a texture of the slot's size first.
void DrawFrame(ID3D11Texture2D* frame, const D3D11_TEXTURE2D_DESC& desc, Slot& slot)
{
    if (slot.shared)
    {
        if (!slot.target)
            winrt::check_hresult(d3d.device->CreateRenderTargetView(slot.texture.get(), nullptr, slot.target.put()));
        DrawFrame(frame, desc, slot.target.get(), slot.width, slot.height);
        return;
    }
    D3D11_TEXTURE2D_DESC drawn{};
    if (d3d.drawn)
        d3d.drawn->GetDesc(&drawn);
    if (drawn.Width != slot.width || drawn.Height != slot.height)
    {
        drawn = {};
        drawn.Width = slot.width;
        drawn.Height = slot.height;
        drawn.MipLevels = 1;
        drawn.ArraySize = 1;
        drawn.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        drawn.SampleDesc = { 1, 0 };
        drawn.Usage = D3D11_USAGE_DEFAULT;
        drawn.BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d.drawnTarget = nullptr;
        d3d.drawn = nullptr;
        winrt::check_hresult(d3d.device->CreateTexture2D(&drawn, nullptr, d3d.drawn.put()));
        winrt::check_hresult(d3d.device->CreateRenderTargetView(d3d.drawn.get(), nullptr, d3d.drawnTarget.put()));
    }
    DrawFrame(frame, desc, d3d.drawnTarget.get(), slot.width, slot.height);
    d3d.context->CopyResource(slot.texture.get(), d3d.drawn.get());
}

void OnFrameArrived(const wgc::Direct3D11CaptureFramePool& pool)
{
    std::lock_guard copying(capture.copying);
    try
    {
        // Only the newest frame is shown. The ones before it are frames the game drew all the same, so the serial counts
        // them, and the frame rates see them as skipped rather than not at all.
        wgc::Direct3D11CaptureFrame frame{ nullptr };
        uint64_t drawn = 0;
        for (wgc::Direct3D11CaptureFrame next = pool.TryGetNextFrame(); next; next = pool.TryGetNextFrame(), ++drawn)
            frame = next;
        {
            std::lock_guard lock(capture.mutex);
            capture.serial += drawn;
        }
        if (!frame || !capture.running)
            return;
        UpdateHdr(pool);
        const SizeInt32 size = frame.ContentSize();
        if (size.Width != capture.size.Width || size.Height != capture.size.Height)
        {
            // The window changed size. Frames come at the new size once the pool is made again.
            if (size.Width > 0 && size.Height > 0)
            {
                capture.size = size;
                pool.Recreate(d3d.captureDevice, CaptureFormat(), kCaptureBuffers, size);
            }
            return;
        }
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()->GetInterface(
            IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        const UINT scale = static_cast<UINT>(capture.scale.load());
        const UINT width = std::max(desc.Width * scale / 100, 1u);
        const UINT height = std::max(desc.Height * scale / 100, 1u);

        const bool shared = capture.onGpu;
        std::shared_ptr<Slot> slot;
        {
            std::lock_guard lock(capture.mutex);
            slot = FreeSlot(width, height, shared);
        }
        if (!slot)
            return;
        if (slot->mapped.pData)
        {
            d3d.context->Unmap(slot->texture.get(), 0);
            slot->mapped = {};
        }
        if (width != desc.Width || height != desc.Height || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            DrawFrame(texture.get(), desc, *slot);
        else
            d3d.context->CopyResource(slot->texture.get(), texture.get());
        Frame copied;
        copied.width = width;
        copied.height = height;
        if (shared)
        {
            // Vulkan reads the texture as soon as the host takes the frame, so the copy has to be done by then.
            const uint64_t value = ++d3d.fenceValue;
            winrt::check_hresult(d3d.fenceContext->Signal(d3d.fence.get(), value));
            winrt::check_hresult(d3d.fence->SetEventOnCompletion(value, d3d.fenceEvent.get()));
            d3d.context->Flush();
            if (WaitForSingleObject(d3d.fenceEvent.get(), 2000) != WAIT_OBJECT_0)
                throw std::runtime_error("The graphics card did not finish copying the game's picture.");
        }
        else
        {
            // Waits for the copy.
            winrt::check_hresult(d3d.context->Map(slot->texture.get(), 0, D3D11_MAP_READ, 0, &slot->mapped));
            copied.memory = static_cast<const uint8_t*>(slot->mapped.pData);
            copied.stride = slot->mapped.RowPitch;
        }
        copied.hold = std::move(slot);
        {
            std::lock_guard lock(capture.mutex);
            if (!capture.running)
                return;
            copied.serial = capture.serial;
            capture.ready = std::move(copied);
        }
        glfwPostEmptyEvent();
    }
    catch (const winrt::hresult_error& e)
    {
        FailCapture(Describe(e));
    }
    catch (const std::exception& e)
    {
        FailCapture(e.what());
    }
}

// The slot's texture as a Vulkan image, made the first time.
VkImage Import(Slot& slot)
{
    if (slot.image)
        return slot.image;
    VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_B8G8R8A8_UNORM;
    info.extent = { slot.width, slot.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const auto failed = [&]() -> VkImage {
        vkDestroyImage(gpu.device, image, nullptr);
        if (memory)
            vkFreeMemory(gpu.device, memory, nullptr);
        return VK_NULL_HANDLE;
    };
    if (vkCreateImage(gpu.device, &info, nullptr, &image) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    const auto getHandleProperties =
        reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(gpu.device, "vkGetMemoryWin32HandlePropertiesKHR"));
    VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(gpu.device, image, &requirements);
    uint32_t type = UINT32_MAX;
    if (getHandleProperties &&
        getHandleProperties(gpu.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, slot.handle, &handleProperties) == VK_SUCCESS)
        type = gpu.FindMemoryType(requirements.memoryTypeBits & handleProperties.memoryTypeBits, 0);
    VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = image;
    VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import.handle = slot.handle;
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.pNext = &import;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(gpu.device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(gpu.device, image, memory, 0) != VK_SUCCESS)
        return failed();

    // Takes the image from Direct3D once to move it into the general layout, keeping its contents, then hands it back.
    // From here on each use takes and returns it in that layout.
    VkCommandBuffer commands = gpu.BeginCommands();
    if (!commands)
        return failed();
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = gpu.queueFamily;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = gpu.queueFamily;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (!gpu.SubmitAndWait(commands))
    {
        // Commands that failed to finish may still use the image.
        vkDeviceWaitIdle(gpu.device);
        return failed();
    }
    // Importing does not take the handle.
    CloseHandle(slot.handle);
    slot.handle = nullptr;
    slot.image = image;
    slot.memory = memory;
    return image;
}

// An icon's pixels, from its color bitmap, with the mask for icons that have no alpha.
Icon IconPixels(HICON icon)
{
    ICONINFO info{};
    if (!GetIconInfo(icon, &info))
        return {};
    BITMAP bitmap{};
    Icon result;
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) && bitmap.bmWidth > 0 && bitmap.bmHeight > 0)
    {
        const int width = bitmap.bmWidth, height = bitmap.bmHeight;
        BITMAPINFO format{};
        format.bmiHeader.biSize = sizeof(format.bmiHeader);
        format.bmiHeader.biWidth = width;
        format.bmiHeader.biHeight = -height;
        format.bmiHeader.biPlanes = 1;
        format.bmiHeader.biBitCount = 32;
        format.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> pixels(size_t(width) * height * 4), mask(pixels.size());
        const HDC screen = GetDC(nullptr);
        const bool read = GetDIBits(screen, info.hbmColor, 0, height, pixels.data(), &format, DIB_RGB_COLORS) == height;
        bool alpha = false;
        for (size_t i = 3; i < pixels.size() && !alpha; i += 4)
            alpha = pixels[i] != 0;
        const bool masked = !alpha && GetDIBits(screen, info.hbmMask, 0, height, mask.data(), &format, DIB_RGB_COLORS) == height;
        ReleaseDC(nullptr, screen);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            std::swap(pixels[i], pixels[i + 2]);
            if (masked)
                pixels[i + 3] = mask[i] ? 0 : 255;
        }
        if (read)
            result = { std::move(pixels), width, height };
    }
    if (info.hbmColor)
        DeleteObject(info.hbmColor);
    if (info.hbmMask)
        DeleteObject(info.hbmMask);
    return result;
}

std::string FontsFolder()
{
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    return Utf8(windows) + "\\Fonts\\";
}

bool Readable(const std::string& path)
{
    return GetFileAttributesW(Wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}
} // namespace

bool Init(std::string& error)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (const winrt::hresult_error&)
    {
        // COM was already started on this thread in a single-threaded apartment, where capture works too.
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = MessageProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    // Setup finds a running Unishade by this name.
    windowClass.lpszClassName = L"UnishadeMessages";
    RegisterClassW(&windowClass);
    messageWindow =
        CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!messageWindow)
    {
        error = "Could not create a window for shortcuts.";
        return false;
    }
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Explorer runs without administrator rights, so Unishade running with them must accept its messages.
    ChangeWindowMessageFilterEx(messageWindow, taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(messageWindow, kTrayMessage, MSGFLT_ALLOW, nullptr);
    if (!wgc::GraphicsCaptureSession::IsSupported())
    {
        error = "Windows Graphics Capture is not available, and Unishade needs it to copy the game's picture. Update Windows and the graphics driver.";
        return false;
    }
    RequestBorderlessCapture();
    return true;
}

void Shutdown()
{
    RemoveTrayIcon();
    StopCapture();
    for (const auto& [id, registration] : std::map<int, Registration>(registrations))
        UnregisterHotkey(id);
    if (messageWindow)
        DestroyWindow(messageWindow);
    messageWindow = nullptr;
    d3d = {};
}

std::vector<Window> ListWindows()
{
    std::vector<Window> windows;
    for (HWND window : TopLevelWindows())
        if (std::optional<Window> listed = Listed(window))
            windows.push_back(std::move(*listed));
    std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return _stricmp(a.title.c_str(), b.title.c_str()) < 0; });
    return windows;
}

std::optional<Window> ListedWindow(WindowId window)
{
    return IsWindow(Handle(window)) ? Listed(Handle(window)) : std::nullopt;
}

std::set<int> WindowOwners()
{
    std::set<int> owners;
    for (HWND window : TopLevelWindows())
        if (DWORD process = 0; GetWindowThreadProcessId(window, &process))
            owners.insert(static_cast<int>(process));
    return owners;
}

bool WindowExists(const Window& window)
{
    DWORD process = 0;
    return IsWindow(Handle(window.id)) && GetWindowThreadProcessId(Handle(window.id), &process) && static_cast<int>(process) == window.pid;
}

// The window's frame as DWM draws it, which is what capture copies, title bar included for a game in a window.
bool WindowBounds(WindowId window, Rect& bounds)
{
    const HWND handle = Handle(window);
    RECT frame{};
    if (!IsWindowVisible(handle) || IsIconic(handle) || Cloaked(handle) ||
        FAILED(DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        return false;
    bounds = { frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top };
    return bounds.width > 0 && bounds.height > 0;
}

int WindowLayer(WindowId)
{
    return 0;
}

bool WindowFillsScreen(WindowId)
{
    return false;
}

bool WindowHasTitleBar(WindowId)
{
    return false;
}

void SetSkipTitleBar(bool)
{
}

WindowId ForegroundWindow()
{
    return Id(GetForegroundWindow());
}

bool ProcessInFront(int pid)
{
    DWORD process = 0;
    const HWND foreground = GetForegroundWindow();
    return foreground && GetWindowThreadProcessId(foreground, &process) && static_cast<int>(process) == pid;
}

void Activate(const Window& window)
{
    SetForegroundWindow(Handle(window.id));
}

std::string ProcessExecutable(int pid)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size))
        {
            path.resize(size);
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768)
        {
            path.clear();
            break;
        }
        path.resize(path.size() * 4);
    }
    CloseHandle(process);
    return Utf8(path);
}

// Windows games are their executables, so there is no command line to match.
std::string ProcessCommand(int)
{
    return {};
}

std::vector<Process> ListProcesses()
{
    std::vector<Process> processes;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return processes;
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry))
    {
        // A process Unishade may not ask about, such as one running as administrator, still matches games saved by
        // their file name.
        const int pid = static_cast<int>(entry.th32ProcessID);
        std::string executable = ProcessExecutable(pid);
        processes.push_back({ pid, executable.empty() ? Utf8(entry.szExeFile) : std::move(executable), {} });
    }
    CloseHandle(snapshot);
    return processes;
}

Icon FileIcon(const std::string& executable)
{
    const std::wstring file = Wide(executable);
    Icon result;
    if (file.empty())
        return result;
    InSingleThreadedApartment([&] {
        HICON icon = nullptr;
        if (SHDefExtractIconW(file.c_str(), 0, 0, &icon, nullptr, 256) == S_OK && icon)
        {
            result = IconPixels(icon);
            DestroyIcon(icon);
        }
    });
    return result;
}

bool SaveWindowIcon(const Window& window, const std::string& path)
{
    const Icon icon = FileIcon(ProcessExecutable(window.pid));
    return !icon.rgba.empty() && stbi_write_png(path.c_str(), icon.width, icon.height, 4, icon.rgba.data(), icon.width * 4);
}

namespace
{
// The launcher's caption is left off so the launcher can draw the title bar, and the system's caption buttons with
// it, which the launcher draws instead. Everything else stays the system's: the frame's style, so snapping, the
// shadow, rounded corners and the animations stay, the resize borders, and moving, maximizing and the window menu
// from the row through HTCAPTION. GLFW has no hook for this, so its window procedure is wrapped.
WNDPROC glfwLauncherProc = nullptr;

// The resize border along the top, which is inside the client area once the caption is gone.
int ResizeBorder(HWND window)
{
    const UINT dpi = GetDpiForWindow(window);
    return GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi) + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi);
}

caption::Button CaptionButton(WPARAM hit)
{
    switch (hit)
    {
    case HTMINBUTTON: return caption::Button::Minimize;
    case HTMAXBUTTON: return caption::Button::Maximize;
    case HTCLOSE: return caption::Button::Close;
    default: return caption::Button::None;
    }
}

// standard is what GLFW's procedure, which leaves it to DefWindowProc, says: the frame on the left, right and bottom,
// and HTCLIENT for the rest.
LRESULT LauncherHitTest(HWND window, LPARAM lParam, LRESULT standard)
{
    POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
    ScreenToClient(window, &point);
    const bool top = !IsZoomed(window) && point.y < ResizeBorder(window);
    if (standard != HTCLIENT)
    {
        if (top && standard == HTLEFT)
            return HTTOPLEFT;
        if (top && standard == HTRIGHT)
            return HTTOPRIGHT;
        return standard;
    }
    if (top)
        return HTTOP;
    const caption::Bar& bar = caption::bar;
    const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
    // HTMAXBUTTON is also what shows Windows 11's snap layouts.
    constexpr LRESULT kButtons[] = { HTMINBUTTON, HTMAXBUTTON, HTCLOSE };
    for (int i = 0; i < 3 && bar.drawButtons; ++i)
        if (bar.buttons[i].Contains(x, y))
            return kButtons[i];
    for (const caption::Box& box : bar.controls)
        if (box.Contains(x, y))
            return HTCLIENT;
    return y < bar.height ? HTCAPTION : HTCLIENT;
}

LRESULT CALLBACK LauncherProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    caption::Bar& bar = caption::bar;
    switch (message)
    {
    case WM_NCCALCSIZE:
    {
        if (!wParam)
            break;
        // The system's frame stays outside the client area on the left, right and bottom. The top keeps none.
        RECT& client = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0];
        const LONG top = client.top;
        const LRESULT result = CallWindowProcW(glfwLauncherProc, window, message, wParam, lParam);
        if (result != 0)
            return result;
        client.top = top;
        if (IsZoomed(window))
        {
            // Maximized, the frame hangs off the screen on every side, the top too.
            client.top += ResizeBorder(window);
            // A taskbar that hides itself only comes back at a screen edge the window leaves a little of.
            MONITORINFO info{ sizeof(info) };
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info))
            {
                const auto hides = [&](UINT edge) {
                    APPBARDATA data{ sizeof(data) };
                    data.uEdge = edge;
                    data.rc = info.rcMonitor;
                    return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data) != 0;
                };
                if (hides(ABE_BOTTOM))
                    client.bottom -= 2;
                else if (hides(ABE_TOP))
                    client.top += 2;
                else if (hides(ABE_LEFT))
                    client.left += 2;
                else if (hides(ABE_RIGHT))
                    client.right -= 2;
            }
        }
        return 0;
    }
    case WM_NCHITTEST: return LauncherHitTest(window, lParam, CallWindowProcW(glfwLauncherProc, window, message, wParam, lParam));
    case WM_SIZE: bar.maximized = IsZoomed(window) != FALSE; break;
    // The system sends the mouse over the buttons here, as over its own, rather than to the client area.
    case WM_NCMOUSEMOVE:
    {
        bar.hovered = bar.drawButtons ? CaptionButton(wParam) : caption::Button::None;
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE | TME_NONCLIENT, window, HOVER_DEFAULT };
        TrackMouseEvent(&track);
        break;
    }
    case WM_NCMOUSELEAVE:
    case WM_MOUSEMOVE:
        bar.hovered = caption::Button::None;
        bar.pressed = caption::Button::None;
        break;
    // DefWindowProc would run its own loop for the buttons and draw the old ones, so the buttons act here, on release
    // over the one pressed.
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (const caption::Button button = CaptionButton(wParam); bar.drawButtons && button != caption::Button::None)
        {
            bar.pressed = bar.hovered = button;
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (const caption::Button button = CaptionButton(wParam); bar.drawButtons && button != caption::Button::None)
        {
            const bool clicked = bar.pressed == button;
            bar.pressed = bar.hovered = caption::Button::None;
            // As the system's buttons do, so closing goes through WM_CLOSE to GLFW's close callback.
            if (clicked)
                PostMessageW(window, WM_SYSCOMMAND,
                             button == caption::Button::Minimize ? SC_MINIMIZE
                             : button == caption::Button::Close  ? SC_CLOSE
                             : IsZoomed(window)                  ? SC_RESTORE
                                                                 : SC_MAXIMIZE,
                             0);
            return 0;
        }
        break;
    }
    return CallWindowProcW(glfwLauncherProc, window, message, wParam, lParam);
}
} // namespace

void SetupLauncherWindow(GLFWwindow* window)
{
    const HWND launcher = glfwGetWin32Window(window);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(launcher, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // For what the system still draws of the frame, such as the edge left for a taskbar that hides itself.
    const COLORREF color = RGB((theme::kBackground >> 16) & 0xFF, (theme::kBackground >> 8) & 0xFF, theme::kBackground & 0xFF);
    DwmSetWindowAttribute(launcher, DWMWA_CAPTION_COLOR, &color, sizeof(color));

    glfwLauncherProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(launcher, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(LauncherProc)));
    caption::bar.drawButtons = true;
    caption::bar.maximized = IsZoomed(launcher) != FALSE;
    // The client area takes the caption's place from here on.
    SetWindowPos(launcher, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void AddTrayIcon(Tray actions)
{
    tray = std::move(actions);
    trayWanted = true;
    ShowTrayIcon();
}

void RemoveTrayIcon()
{
    trayWanted = false;
    if (messageWindow)
        KillTimer(messageWindow, kTrayTimer);
    if (trayShown)
    {
        NOTIFYICONDATAW data = TrayData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayShown = false;
    }
    if (trayIcon)
        DestroyIcon(trayIcon);
    trayIcon = nullptr;
}

bool TrayIconShown()
{
    return trayShown;
}

namespace
{
// Windows starts Unishade at sign-in with this entry. Task Manager's Startup apps turn entries off in the other key
// without removing them.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunValue[] = L"Unishade";

std::wstring StartCommand()
{
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return L"\"" + path + L"\" --minimized";
}
} // namespace

bool StartsWithWindows()
{
    std::wstring command(32768, L'\0');
    DWORD size = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, command.data(), &size) != ERROR_SUCCESS ||
        _wcsicmp(command.c_str(), StartCommand().c_str()) != 0)
        return false;
    // The first byte is odd when Task Manager turned it off.
    BYTE approved[64]{};
    size = sizeof(approved);
    return RegGetValueW(HKEY_CURRENT_USER, kStartupApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, approved, &size) != ERROR_SUCCESS ||
           !(approved[0] & 1);
}

void SetStartWithWindows(bool start)
{
    LSTATUS status = ERROR_SUCCESS;
    if (start)
    {
        const std::wstring command = StartCommand();
        HKEY key = nullptr;
        status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status == ERROR_SUCCESS)
        {
            status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
        // Turned on here, it should not stay off in Task Manager.
        if (status == ERROR_SUCCESS)
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApprovedKey, kRunValue);
    }
    else
    {
        status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }
    if (status != ERROR_SUCCESS)
        Report(LogLevel::Warning, "Could not change whether Unishade starts with Windows (error %ld).", static_cast<long>(status));
    else
        Log(LogLevel::Info, start ? "Unishade starts with Windows." : "Unishade no longer starts with Windows.");
}

void SetSessionEndCallback(std::function<void()> callback)
{
    sessionEnd = std::move(callback);
}

void PrepareOverlayWindow()
{
}

// Clicks reach the game through a layered window that is transparent to input, which GLFW's mouse passthrough
// makes. A layered window shows only once it has its opacity set, and GLFW keeps it when it switches passthrough.
void SetupOverlayWindow(GLFWwindow* window)
{
    const HWND overlay = glfwGetWin32Window(window);
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW | WS_EX_LAYERED);
    SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);
    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// In the place MatchOverlay gave it.
void ShowOverlay(GLFWwindow* window, bool visible)
{
    const HWND overlay = glfwGetWin32Window(window);
    if (visible)
        SetWindowPos(overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ShowWindow(overlay, SW_HIDE);
}

bool IsTopmost(HWND window)
{
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// Above every window, or right above the game so that whatever covers the game also covers its effects. A game can
// be topmost too, as borderless ones often are, and Windows raises the window it activates to the top of its group,
// above the overlay.
void MatchOverlay(GLFWwindow* window, WindowId gameId, bool behind)
{
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    const HWND overlay = glfwGetWin32Window(window);
    const HWND game = Handle(gameId);
    if (behind)
    {
        if (GetWindow(game, GW_HWNDPREV) == overlay)
            return;
        // Topmost windows stay above all others, so the overlay first joins the game's group. HWND_NOTOPMOST leaves
        // the topmost group and does nothing out of it.
        const bool topmost = IsTopmost(game);
        SetWindowPos(overlay, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        // Without a window of its own group above it, the game is the first of its group.
        const HWND above = GetWindow(game, GW_HWNDPREV);
        if (above != overlay)
            SetWindowPos(overlay, above && IsTopmost(above) == topmost ? above : HWND_TOP, 0, 0, 0, 0, flags);
        return;
    }
    if (!IsTopmost(overlay))
    {
        SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, flags);
        return;
    }
    for (HWND above = GetWindow(overlay, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV))
        if (above == game)
        {
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, flags);
            return;
        }
}

bool OverlayCanStayBehind()
{
    return true;
}

void FocusOverlay(GLFWwindow* window)
{
    SetForegroundWindow(glfwGetWin32Window(window));
}

void ReturnFocus(GLFWwindow*, const Window& game)
{
    Activate(game);
}

bool GameWindowCovered(const Window&)
{
    return false;
}

bool StartCapture(const Window& window, std::string& error)
{
    StopCapture();
    try
    {
        CreateDevice();
        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(interop->CreateForWindow(Handle(window.id), winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        const bool onGpu = d3d.shareable && gpu.win32Memory && CanImport();
        capture.window = Handle(window.id);
        capture.sdrWhite = HdrWhiteLevel(capture.window);
        capture.nextHdrCheck = GetTickCount64() + kHdrCheckInterval;
        {
            std::lock_guard lock(capture.mutex);
            capture.error.clear();
            capture.ready = {};
            capture.slots.clear();
            capture.serial = 0;
            capture.size = item.Size();
            capture.onGpu = onGpu;
            capture.running = true;
        }
        capture.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d.captureDevice, CaptureFormat(), kCaptureBuffers, capture.size);
        capture.frameArrived = capture.pool.FrameArrived(winrt::auto_revoke, [](const wgc::Direct3D11CaptureFramePool& pool, const auto&) { OnFrameArrived(pool); });
        capture.closed = item.Closed(winrt::auto_revoke, [](const auto&, const auto&) { FailCapture("The window closed."); });
        capture.session = capture.pool.CreateCaptureSession(item);
        // The real cursor is drawn over the overlay anyway.
        capture.session.IsCursorCaptureEnabled(false);
        if (!borderRequired && ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired"))
        {
            try
            {
                capture.session.IsBorderRequired(false);
            }
            catch (const winrt::hresult_error&)
            {
                borderRequired = true;
            }
        }
        // Without it, capture can be held to 60 frames a second.
        capture.idle = false;
        if (ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval"))
            capture.session.MinUpdateInterval(std::chrono::milliseconds(1));
        capture.session.StartCapture();
        char hdr[64] = "";
        if (capture.sdrWhite)
            snprintf(hdr, sizeof(hdr), " in HDR, SDR_white=%.1f nits", *capture.sdrWhite);
        Log(LogLevel::Info, "Capturing %dx%d%s. Frames %s.", capture.size.Width, capture.size.Height, hdr,
            onGpu                  ? "stay on the graphics card, shared from Direct3D 11"
            : !d3d.shareable       ? "are copied through memory, since Direct3D 11 cannot share them with Vulkan's graphics card"
            : !gpu.win32Memory     ? "are copied through memory, since Vulkan has no VK_KHR_external_memory_win32"
                                   : "are copied through memory, since Vulkan cannot import Direct3D 11 textures");
    }
    catch (const winrt::hresult_error& e)
    {
        StopCapture();
        error = Describe(e);
        return false;
    }
    return true;
}

void StopCapture()
{
    capture.running = false;
    capture.frameArrived.revoke();
    capture.closed.revoke();
    if (capture.session)
        capture.session.Close();
    if (capture.pool)
        capture.pool.Close();
    capture.session = nullptr;
    capture.pool = nullptr;
    std::lock_guard copying(capture.copying);
    std::lock_guard lock(capture.mutex);
    capture.ready = {};
    capture.slots.clear();
}

bool Capturing()
{
    return capture.running;
}

void SetCaptureIdle(bool idle)
{
    static const bool intervals = ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval");
    if (!capture.session || capture.idle == idle || !intervals)
        return;
    capture.idle = idle;
    capture.session.MinUpdateInterval(std::chrono::milliseconds(idle ? 200 : 1));
}

void SetFrameScale(int percent)
{
    capture.scale = percent;
}

bool TakeFrame(Frame& frame)
{
    {
        std::lock_guard lock(capture.mutex);
        if (capture.ready.serial <= frame.serial || !capture.ready.hold)
            return false;
        frame = std::move(capture.ready);
        capture.ready = {};
    }
    if (frame.memory)
        return true;
    frame.image = Import(*static_cast<Slot*>(frame.hold.get()));
    frame.foreign = true;
    if (!frame.image)
    {
        // Copies through memory from the next frame on.
        Log(LogLevel::Warning, "Could not use the game's picture on the graphics card. Copying it instead.");
        capture.onGpu = false;
        frame = {};
        return false;
    }
    return true;
}

std::string CaptureError()
{
    std::lock_guard lock(capture.mutex);
    return capture.error;
}

bool CaptureStoppedByUser()
{
    return false;
}

bool HasCapturePermission()
{
    return true;
}

void RequestCapturePermission()
{
}

bool Relaunch()
{
    return false;
}

// Started from Explorer, nobody sees the terminal.
void ShowStartupError(const std::string& message)
{
    MessageBoxW(nullptr, Wide(message).c_str(), L"Unishade could not start", MB_OK | MB_ICONERROR);
}

// On a thread of its own, since the dialog runs until it is answered and the game still has to be drawn over.
void ChooseProgram(std::function<void(std::optional<std::string>)> done)
{
    std::thread([done = std::move(done), owner = FindWindowW(L"GLFW30", L"Unishade")] {
        std::wstring file(32768, L'\0');
        OPENFILENAMEW ask{ sizeof(ask) };
        ask.hwndOwner = owner;
        ask.lpstrFilter = L"Programs\0*.exe\0";
        ask.lpstrFile = file.data();
        ask.nMaxFile = static_cast<DWORD>(file.size());
        ask.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        done(GetOpenFileNameW(&ask) ? Utf8(file.c_str()) : std::string());
    }).detach();
}

// Brings the running Unishade's launcher to the front. GLFW names its windows' class GLFW30.
void ShowAlreadyRunning()
{
    if (const HWND launcher = FindWindowW(L"GLFW30", L"Unishade"))
    {
        ShowWindow(launcher, IsIconic(launcher) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(launcher);
    }
}

void SetHotkeyCallback(HotkeyCallback callback)
{
    hotkeyCallback = std::move(callback);
}

bool RegisterHotkey(int id, const Hotkey& hotkey)
{
    UnregisterHotkey(id);
    const UINT key = VirtualKey(hotkey.key);
    UINT modifiers = MOD_NOREPEAT;
    if (hotkey.modifiers & kCtrl)
        modifiers |= MOD_CONTROL;
    if (hotkey.modifiers & kAlt)
        modifiers |= MOD_ALT;
    if (hotkey.modifiers & kShift)
        modifiers |= MOD_SHIFT;
    if (hotkey.modifiers & kSuper)
        modifiers |= MOD_WIN;
    if (!key || !RegisterHotKey(messageWindow, id, modifiers, key))
        return false;
    registrations[id] = { key };
    return true;
}

void UnregisterHotkey(int id)
{
    if (registrations.erase(id))
        UnregisterHotKey(messageWindow, id);
}

void PollHotkeys()
{
    std::vector<std::pair<int, bool>> events;
    for (const int id : std::exchange(pressedHotkeys, {}))
        if (const auto found = registrations.find(id); found != registrations.end() && !found->second.held)
        {
            found->second.held = true;
            events.emplace_back(id, true);
        }
    for (auto& [id, registration] : registrations)
        if (registration.held && !(GetAsyncKeyState(static_cast<int>(registration.key)) & 0x8000))
        {
            registration.held = false;
            events.emplace_back(id, false);
        }
    // The callback can change the shortcuts.
    for (const auto& [id, pressed] : events)
        if (hotkeyCallback)
            hotkeyCallback(id, pressed);
}

void ReadInput(std::array<bool, 256>& keys, std::array<bool, 5>& buttons)
{
    for (size_t key = 0; key < keys.size(); ++key)
        keys[key] = GetAsyncKeyState(static_cast<int>(key)) & 0x8000;
    const int codes[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (size_t button = 0; button < buttons.size(); ++button)
        buttons[button] = GetAsyncKeyState(codes[button]) & 0x8000;
}

bool WaylandDesktop()
{
    return false;
}

void Open(const std::string& target)
{
    const std::wstring wide = Wide(target);
    InSingleThreadedApartment([&] { ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
}

bool CanCopyImages()
{
    return true;
}

bool CopyImage(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    // A device-independent bitmap: its header, then BGRA rows from the bottom one up.
    const size_t row = size_t(width) * 4;
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + row * height);
    if (!memory)
        return false;
    auto* const header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(width);
    header->biHeight = static_cast<LONG>(height);
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(row * height);
    uint8_t* out = reinterpret_cast<uint8_t*>(header + 1);
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* in = rgba + size_t(height - 1 - y) * row;
        for (uint32_t x = 0; x < width; ++x, in += 4, out += 4)
        {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = 255;
        }
    }
    GlobalUnlock(memory);
    // Another program can hold the clipboard for a moment.
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !(opened = OpenClipboard(nullptr) != FALSE); ++attempt)
        Sleep(10);
    if (!opened)
    {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    // The clipboard owns the memory once it takes it.
    const bool set = SetClipboardData(CF_DIB, memory) != nullptr;
    if (!set)
        GlobalFree(memory);
    CloseClipboard();
    return set;
}

bool MoveToTrash(const std::string& path)
{
    // The list of files ends with an empty one.
    const std::wstring files = Wide(path) + L'\0';
    int result = 1;
    InSingleThreadedApartment([&] {
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = files.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;
        result = SHFileOperationW(&operation);
        if (operation.fAnyOperationsAborted)
            result = 1;
    });
    return result == 0;
}

bool DisplayDrmDevice(int64_t&, int64_t&)
{
    return false;
}

// Loaded for good, so the first Vulkan call finds it. Looked for as Windows looks for a program's own DLLs.
bool VulkanInstalled()
{
    return LoadLibraryW(L"vulkan-1.dll") != nullptr;
}

// The fonts of the ReShade host's launcher.
std::string UiFont(bool bold)
{
    const std::string path = FontsFolder() + (bold ? "seguisb.ttf" : "segoeui.ttf");
    return Readable(path) ? path : std::string();
}

std::vector<std::pair<std::string, int>> UiFallbackFonts(bool bold)
{
    // Chinese, Japanese and Korean draw the same characters differently, so the user's own language comes first.
    std::vector<std::pair<const char*, const char*>> cjk = { { "msyh.ttc", "msyhbd.ttc" }, { "YuGothM.ttc", "YuGothB.ttc" },
                                                             { "malgun.ttf", "malgunbd.ttf" }, { "msjh.ttc", "msjhbd.ttc" } };
    wchar_t language[LOCALE_NAME_MAX_LENGTH]{};
    LCIDToLocaleName(GetUserDefaultUILanguage(), language, LOCALE_NAME_MAX_LENGTH, 0);
    const std::wstring_view name = language;
    const size_t first = name.starts_with(L"ja") ? 1
                       : name.starts_with(L"ko") ? 2
                       : name.starts_with(L"zh-TW") || name.starts_with(L"zh-HK") || name.starts_with(L"zh-MO") || name.starts_with(L"zh-Hant") ? 3
                                                                                                                                                     : 0;
    std::rotate(cjk.begin(), cjk.begin() + static_cast<std::ptrdiff_t>(first), cjk.begin() + static_cast<std::ptrdiff_t>(first) + 1);
    std::vector<std::pair<const char*, const char*>> all = { { "seguisym.ttf", "seguisym.ttf" } };
    all.insert(all.end(), cjk.begin(), cjk.end());
    // Windows 11 has Nirmala UI as a collection, Windows 10 as a single font.
    all.insert(all.end(), { { "LeelawUI.ttf", "LeelaUIb.ttf" }, { "Nirmala.ttc", "Nirmala.ttc" }, { "Nirmala.ttf", "NirmalaB.ttf" },
                            { "seguiemj.ttf", "seguiemj.ttf" } });
    std::vector<std::pair<std::string, int>> fonts;
    for (const auto& [regular, heavy] : all)
        if (const std::string path = FontsFolder() + (bold ? heavy : regular); Readable(path))
            fonts.emplace_back(path, 0);
    return fonts;
}
} // namespace platform
