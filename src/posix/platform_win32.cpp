// Windows: windows and processes through the Win32 API, and the game's picture through Windows.Graphics.Capture,
// copied through memory.
#include <unknwn.h>

#include "platform.h"
#include "log.h"
#include "text.h"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
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

// Shortcuts arrive as WM_HOTKEY at a window of the main thread, which GLFW's message loop hands to it.
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

LRESULT CALLBACK MessageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_HOTKEY)
    {
        pressedHotkeys.push_back(static_cast<int>(wParam));
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

// Capture

constexpr DirectXPixelFormat kCaptureFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int32_t kCaptureBuffers = 3;
// The host holds up to three frames: the one it shows, the one the graphics card may still draw and the newest one it
// has not taken. Capture copies into a fourth.
constexpr size_t kSlots = 4;

struct Direct3D
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    IDirect3DDevice captureDevice{ nullptr };
};
Direct3D d3d;

// A texture the processor reads, which frames are copied into. It stays mapped while a frame holds it.
struct Slot
{
    winrt::com_ptr<ID3D11Texture2D> texture;
    UINT width = 0;
    UINT height = 0;
    D3D11_MAPPED_SUBRESOURCE mapped{};

    ~Slot()
    {
        if (mapped.pData)
            d3d.context->Unmap(texture.get(), 0);
    }
};

struct Capture
{
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
    wgc::GraphicsCaptureItem::Closed_revoker closed;
    bool idle = false;
    // Held while a frame is copied, so stopping waits for it.
    std::mutex copying;
    // The rest is shared with the threads frames arrive on.
    std::mutex mutex;
    std::atomic<bool> running = false;
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
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
    // The capture pool uses the device from its own threads.
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), inspectable.put()));
    d3d.captureDevice = inspectable.as<IDirect3DDevice>();
    d3d.device = std::move(device);
    d3d.context = std::move(context);
    return true;
}

// A slot no frame holds, of the size frames are copied at now, made when there is none and room for one. Called with
// capture.mutex held.
std::shared_ptr<Slot> FreeSlot(UINT width, UINT height)
{
    std::erase_if(capture.slots, [&](const std::shared_ptr<Slot>& slot) {
        return slot.use_count() == 1 && (slot->width != width || slot->height != height);
    });
    for (const std::shared_ptr<Slot>& slot : capture.slots)
        if (slot.use_count() == 1)
            return slot;
    if (capture.slots.size() >= kSlots)
        return nullptr;

    auto slot = std::make_shared<Slot>();
    slot->width = width;
    slot->height = height;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(d3d.device->CreateTexture2D(&desc, nullptr, slot->texture.put()));
    capture.slots.push_back(slot);
    return slot;
}

void OnFrameArrived(const wgc::Direct3D11CaptureFramePool& pool)
{
    std::lock_guard copying(capture.copying);
    try
    {
        // Only the newest frame matters.
        wgc::Direct3D11CaptureFrame frame{ nullptr };
        for (wgc::Direct3D11CaptureFrame next = pool.TryGetNextFrame(); next; next = pool.TryGetNextFrame())
            frame = next;
        if (!frame || !capture.running)
            return;
        const SizeInt32 size = frame.ContentSize();
        if (size.Width != capture.size.Width || size.Height != capture.size.Height)
        {
            // The window changed size. Frames come at the new size once the pool is made again.
            if (size.Width > 0 && size.Height > 0)
            {
                capture.size = size;
                pool.Recreate(d3d.captureDevice, kCaptureFormat, kCaptureBuffers, size);
            }
            return;
        }
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()->GetInterface(
            IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        std::shared_ptr<Slot> slot;
        {
            std::lock_guard lock(capture.mutex);
            slot = FreeSlot(desc.Width, desc.Height);
        }
        if (!slot)
            return;
        if (slot->mapped.pData)
        {
            d3d.context->Unmap(slot->texture.get(), 0);
            slot->mapped = {};
        }
        d3d.context->CopyResource(slot->texture.get(), texture.get());
        // Waits for the copy.
        winrt::check_hresult(d3d.context->Map(slot->texture.get(), 0, D3D11_MAP_READ, 0, &slot->mapped));
        Frame copied;
        copied.width = desc.Width;
        copied.height = desc.Height;
        copied.memory = static_cast<const uint8_t*>(slot->mapped.pData);
        copied.stride = slot->mapped.RowPitch;
        copied.hold = std::move(slot);
        {
            std::lock_guard lock(capture.mutex);
            if (!capture.running)
                return;
            copied.serial = ++capture.serial;
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

// An icon as a PNG, from its color bitmap, with the mask for icons that have no alpha.
bool SaveIcon(HICON icon, const std::string& path)
{
    ICONINFO info{};
    if (!GetIconInfo(icon, &info))
        return false;
    BITMAP bitmap{};
    bool saved = false;
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
        saved = read && stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
    }
    if (info.hbmColor)
        DeleteObject(info.hbmColor);
    if (info.hbmMask)
        DeleteObject(info.hbmMask);
    return saved;
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
    windowClass.lpszClassName = L"UnishadeMessages";
    RegisterClassW(&windowClass);
    messageWindow = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    if (!messageWindow)
    {
        error = "Could not create a window for shortcuts.";
        return false;
    }
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

bool SaveWindowIcon(const Window& window, const std::string& path)
{
    const std::wstring executable = Wide(ProcessExecutable(window.pid));
    if (executable.empty())
        return false;
    bool saved = false;
    InSingleThreadedApartment([&] {
        HICON icon = nullptr;
        if (SHDefExtractIconW(executable.c_str(), 0, 0, &icon, nullptr, 256) == S_OK && icon)
        {
            saved = SaveIcon(icon, path);
            DestroyIcon(icon);
        }
    });
    return saved;
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

void ShowOverlay(GLFWwindow* window, bool visible)
{
    const HWND overlay = glfwGetWin32Window(window);
    if (visible)
        SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ShowWindow(overlay, SW_HIDE);
}

// A game can be topmost too, as borderless ones often are, and Windows raises the window it activates to the top of
// its group, above the overlay.
void MatchOverlay(GLFWwindow* window, WindowId game)
{
    const HWND overlay = glfwGetWin32Window(window);
    for (HWND above = GetWindow(overlay, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV))
        if (above == Handle(game))
        {
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
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
        {
            std::lock_guard lock(capture.mutex);
            capture.error.clear();
            capture.ready = {};
            capture.slots.clear();
            capture.serial = 0;
            capture.size = item.Size();
            capture.running = true;
        }
        capture.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d.captureDevice, kCaptureFormat, kCaptureBuffers, capture.size);
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
        Log(LogLevel::Info, "Capturing %dx%d. Frames are copied through memory.", capture.size.Width, capture.size.Height);
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

bool TakeFrame(Frame& frame)
{
    std::lock_guard lock(capture.mutex);
    if (capture.ready.serial <= frame.serial || !capture.ready.hold)
        return false;
    frame = std::move(capture.ready);
    capture.ready = {};
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

bool DisplayDrmDevice(int64_t&, int64_t&)
{
    return false;
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
