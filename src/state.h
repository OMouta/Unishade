#pragma once

#include "config.h"
#include "frame_statistics.h"
#include "game_integration.h"

#include <unknwn.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <string>
#include <atomic>
#include <mutex>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

constexpr auto kPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
// What frames are captured in while HDR is on, since 8 bits would clip them.
constexpr auto kHdrPixelFormat = DirectXPixelFormat::R16G16B16A16Float;
// The frame being shown, the newest one waiting for the host and the one Windows captures into.
constexpr int32_t kFrameBuffers = 3;
// Posted by the menu to give input back to the game. The menu runs inside ReShade's present, so window
// changes wait for the message loop.
constexpr UINT kLeaveMenuMessage = WM_APP + 1;
// Posted when the menu's import dialog closes, since opening it closed the menu.
constexpr UINT kOpenMenuMessage = WM_APP + 2;

struct State
{
    HWND overlay = nullptr;
    HWND target = nullptr;
    std::optional<GameWindow> selectedGame;
    std::optional<GameWindow> activeGame;
    std::vector<AutoGame> autoGames;
    HWND launcher = nullptr;
    InputHotkeys hotkeys;
    // The shortcuts as shown to the user, such as Ctrl+F8.
    std::wstring inputHotkey;
    std::wstring overlayHotkey;
    bool editMode = false;
    // The shortcuts that are only held while the game or the menu is in front.
    bool gameHotkeysRegistered = false;
    // While the menu waits for a new shortcut, so the current ones arrive as ordinary keys.
    bool hotkeysSuspended = false;
    bool overlayVisible = false;
    bool captureEnabled = true;
    RECT overlayRect{};

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGISwapChain1> swapchain;
    IDirect3DDevice captureDevice{ nullptr };

    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
    Direct3D11CaptureFrame latestFrame{ nullptr };
    // The capture worker takes every frame from the pool as it arrives and keeps the newest here, so the pool always
    // has a buffer to capture into and runs at the game's rate, however slow the host is.
    std::mutex frameMutex;
    Direct3D11CaptureFrame arrivedFrame{ nullptr };
    // Cleared while the capture stops, so a frame the worker was still taking is dropped.
    bool takingFrames = false;
    SizeInt32 poolSize{};
    // HdrWhiteLevel of the game's display when capture started. While HDR is on, frames are captured in kHdrPixelFormat
    // and turned back into SDR before effects and depth estimation see them.
    std::optional<float> hdrWhiteLevel;
    HANDLE frameEvent = nullptr;
    // Every frame captured from the game, counted on the capture worker. Statistics are sampled on the host thread.
    std::atomic<uint64_t> capturedFrames = 0;
    FrameStatistics frameStatistics;
    // CPU time inside the host's ReShade overlay callback for the current present.
    double menuCpuMs = 0;
};

extern State g;
