// Unishade redraws the target game window in its own D3D11 swapchain, so ReShade can be
// installed on this exe. The game is only observed from outside, through window
// enumeration, executable metadata and Windows.Graphics.Capture. No game memory is read or code injected.

#include "addon.h"
#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "discord.h"
#include "frame_limit.h"
#include "launcher.h"
#include "log.h"
#include "menu.h"
#include "names.h"
#include "overlay.h"
#include "reshade_config.h"
#include "game_integration.h"
#include "setup_check.h"
#include "state.h"
#include "update.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

State g;

namespace
{
// While nothing is attached, every window is looked at this often. While a game is attached, only the window in front.
constexpr ULONGLONG kSearchInterval = 1000;
// A capture that fails to start is tried again after this, doubling each time up to kMaxCaptureRetry.
constexpr ULONGLONG kFirstCaptureRetry = 2000;
constexpr ULONGLONG kMaxCaptureRetry = 30000;
// The same frame is shown again this often, so toasts and hints still change over a paused game.
constexpr ULONGLONG kRepeatInterval = 100;
// After a shortcut, frames are shown on every wake-up for this long, so what it changed shows right away.
constexpr ULONGLONG kShortcutResponse = 1000;
// Recovery from a lost graphics device gives up after this many losses within kLossWindow, well before Windows stops
// resetting a GPU that keeps hanging, or after failing to make a new device for kDeviceRetryLimit. A new device is
// made kDeviceRetryInterval after a loss or a failed attempt, since a GPU reset can take a moment.
constexpr size_t kMaxLosses = 3;
constexpr ULONGLONG kLossWindow = 60000;
constexpr ULONGLONG kDeviceRetryLimit = 30000;
constexpr ULONGLONG kDeviceRetryInterval = 1000;

struct CaptureRetry
{
    ULONGLONG at = 0;
    ULONGLONG delay = 0;
    unsigned failures = 0;
};

struct Loop
{
    ULONGLONG nextSearch = 0;
    HWND lastForeground = nullptr;
    HWND lastTarget = nullptr;
    std::map<HWND, CaptureRetry> captureRetries;

    ULONGLONG nextRepeat = 0;
    ULONGLONG fastUntil = 0;
    FrameLimit frameLimit;
    bool presentPending = false;
    bool wasInteractive = false;
    bool wasVisible = false;

    // The game to attach to again once a new device is made.
    std::optional<GameWindow> reattach;
    std::vector<ULONGLONG> losses;
    ULONGLONG deviceRetryAt = 0;
    ULONGLONG deviceFailingSince = 0;
};
Loop loop;

void ShowError(const std::wstring& message)
{
    MessageBoxW(g.launcher, (message + L"\n\nMore details are in " + LogPath() + L".").c_str(), L"Unishade", MB_ICONERROR);
}

// A damaged list is kept beside it, since the next change to the list replaces the file.
void LoadGames()
{
    const std::wstring path = ExeDirectory() + L"games.ini";
    try
    {
        g.autoGames = LoadAutoGames(path);
        if (AddInstalledStudio(g.autoGames))
            try
            {
                SaveAutoGames(path, g.autoGames);
            }
            catch (const std::exception& e)
            {
                Report(LogLevel::Warning, L"Roblox Studio was added for this session but could not be saved to games.ini (%hs).", e.what());
            }
    }
    catch (const std::exception& e)
    {
        g.autoGames = DefaultAutoGames();
        if (CopyFileW(path.c_str(), (path + L".damaged").c_str(), FALSE))
            Report(LogLevel::Warning, L"Your saved games could not be loaded (%hs), so Unishade is using the default list. The old list "
                                      L"was kept as games.ini.damaged.",
                   e.what());
        else
            Report(LogLevel::Warning, L"Your saved games could not be loaded (%hs), so Unishade is using the default list. Changing "
                                      L"the list replaces games.ini.",
                   e.what());
    }
}

void GiveUpOnDevice(const wchar_t* reason)
{
    g.captureEnabled = false;
    loop.reattach.reset();
    loop.losses.clear();
    loop.deviceRetryAt = 0;
    loop.deviceFailingSince = 0;
    const std::wstring retry = g.overlayHotkey.empty() ? L"restart Unishade" : L"press " + g.overlayHotkey + L" or restart Unishade";
    Report(LogLevel::Error, L"%ls, so Unishade turned the overlay off. Update the graphics driver, then %ls.", reason, retry.c_str());
}

// After a driver update, a GPU reset or a switch to another GPU, everything made on the device is released and capture
// starts again on a new device with the same game.
void OnDeviceLost(HRESULT error)
{
    const HRESULT reason = g.device ? g.device->GetDeviceRemovedReason() : S_OK;
    Log(LogLevel::Warning, L"Lost the graphics device (0x%08X, reason 0x%08X). Starting again on a new one.", static_cast<unsigned>(error),
        static_cast<unsigned>(reason));
    const HWND game = g.target;
    const bool editing = g.editMode;
    loop.reattach = g.activeGame;
    ReleaseDevice();
    // The menu closed with the swapchain, so input goes back to the game.
    if (editing && game)
        SetForegroundWindow(game);

    const ULONGLONG now = GetTickCount64();
    std::erase_if(loop.losses, [now](ULONGLONG at) { return now - at >= kLossWindow; });
    loop.losses.push_back(now);
    loop.deviceRetryAt = now + kDeviceRetryInterval;
    if (loop.losses.size() >= kMaxLosses)
        GiveUpOnDevice(L"The graphics device was lost several times in a minute");
}

// Makes a new device once the previous one was lost. Returns whether there is a device.
bool EnsureDevice()
{
    if (g.device)
        return true;
    const ULONGLONG now = GetTickCount64();
    if (now < loop.deviceRetryAt)
        return false;
    try
    {
        CreateDevice();
        loop.deviceFailingSince = 0;
        Log(LogLevel::Info, L"Made a new graphics device.");
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        if (!loop.deviceFailingSince)
        {
            loop.deviceFailingSince = now;
            Log(LogLevel::Warning, L"Could not make a new graphics device: %ls (0x%08X). Trying again.", e.message().c_str(),
                static_cast<unsigned>(e.code()));
        }
        else if (now - loop.deviceFailingSince >= kDeviceRetryLimit)
        {
            GiveUpOnDevice(L"The graphics device could not be made again after it was lost");
            return false;
        }
        loop.deviceRetryAt = now + kDeviceRetryInterval;
        return false;
    }
}

// The saved game a window belongs to, by its presets folder's name, or empty for a window picked for this session only.
std::wstring SavedGame(const GameWindow& game)
{
    try
    {
        const std::filesystem::path executable = ProcessExecutable(game.processId);
        for (const AutoGame& saved : g.autoGames)
            if (MatchesExecutable(saved, executable))
                return FolderName(saved.name);
    }
    catch (const std::system_error&)
    {
    }
    return {};
}

// A window whose capture fails to start is tried again less and less often, and the failure is logged once.
void Attach(const GameWindow& game)
{
    const ULONGLONG now = GetTickCount64();
    if (const auto retry = loop.captureRetries.find(game.window); retry != loop.captureRetries.end() && now < retry->second.at)
        return;
    if (g.target)
        StopCapture();
    g.activeGame = game;
    try
    {
        StartCapture(game.window);
        SetPerformanceGame(SavedGame(game));
        loop.captureRetries.erase(game.window);
    }
    catch (const winrt::hresult_error& e)
    {
        if (DeviceLost(e.code()))
        {
            OnDeviceLost(e.code());
            return;
        }
        StopCapture();
        CaptureRetry& retry = loop.captureRetries[game.window];
        retry.delay = retry.delay ? std::min(retry.delay * 2, kMaxCaptureRetry) : kFirstCaptureRetry;
        retry.at = now + retry.delay;
        if (++retry.failures == 1)
            Log(LogLevel::Error, L"Could not capture %ls: %ls (0x%08X)", game.name.c_str(), e.message().c_str(), static_cast<unsigned>(e.code()));
        else if (retry.failures == 2)
            Log(LogLevel::Info, L"Still could not capture %ls. Trying again less often, up to every %llu seconds.", game.name.c_str(),
                kMaxCaptureRetry / 1000);
    }
}

// Looks at every window only while nothing is attached. An attached game in front needs no search, and another saved
// game coming to the front takes over, so only the window in front is checked then.
void SearchForGame()
{
    // A game that was let go of may be followed by another right away.
    if (!g.target && loop.lastTarget)
        loop.nextSearch = 0;
    loop.lastTarget = g.target;
    const HWND foreground = GetForegroundWindow();
    const bool foregroundChanged = foreground != loop.lastForeground;
    loop.lastForeground = foreground;
    if (g.target && (g.selectedGame || g.editMode))
        return;

    const ULONGLONG now = GetTickCount64();
    std::optional<GameWindow> game;
    if (!g.target && g.selectedGame)
        game = FindGameTarget(g.selectedGame, g.autoGames);
    else if (!g.target)
    {
        if (foregroundChanged)
            game = MatchGameWindow(foreground, g.autoGames);
        if (!game && now >= loop.nextSearch)
        {
            loop.nextSearch = now + kSearchInterval;
            std::erase_if(loop.captureRetries, [](const auto& retry) { return !IsWindow(retry.first); });
            game = FindGameTarget(std::nullopt, g.autoGames, foreground);
        }
    }
    else if (foreground != g.target && (foregroundChanged || now >= loop.nextSearch))
    {
        loop.nextSearch = now + kSearchInterval;
        game = MatchGameWindow(foreground, g.autoGames);
    }
    if (game && game->window != g.target)
        Attach(*game);
}

// Effects run on every present, so the same picture is not shown over and over: a new frame is shown when it arrives,
// and frames are shown on every wake-up only while the menu takes input, ReShade makes effects or a shortcut was just
// used. Otherwise the last frame is shown again every kRepeatInterval.
void ShowFrames()
{
    // Only the newest frame matters. The capture worker keeps it, and the one shown before goes back to the pool.
    bool fresh = false;
    {
        const std::lock_guard lock(g.frameMutex);
        if (g.arrivedFrame)
        {
            g.latestFrame = std::move(g.arrivedFrame);
            fresh = true;
        }
    }

    if (g.latestFrame)
    {
        SizeInt32 size = g.latestFrame.ContentSize();
        if ((size.Width != g.poolSize.Width || size.Height != g.poolSize.Height) && size.Width > 0 && size.Height > 0)
        {
            g.latestFrame = nullptr;
            {
                const std::lock_guard lock(g.frameMutex);
                g.arrivedFrame = nullptr;
            }
            g.poolSize = size;
            g.pool.Recreate(g.captureDevice, g.hdrWhiteLevel ? kHdrPixelFormat : kPixelFormat, kFrameBuffers, size);
            Log(LogLevel::Info, L"%ls resized to %dx%d", g.activeGame->name.c_str(), size.Width, size.Height);
        }
    }

    // Opening or closing a menu and showing the overlay change the picture without a new frame.
    const bool interactive = g.editMode || ReShadeMenuOpen();
    if (interactive != loop.wasInteractive || (g.overlayVisible && !loop.wasVisible))
        loop.presentPending = true;
    loop.wasInteractive = interactive;
    loop.wasVisible = g.overlayVisible;
    if (!g.overlayVisible || !g.latestFrame)
        return;

    const ULONGLONG now = GetTickCount64();
    if (fresh || interactive || loop.presentPending || ReShadeLoadingEffects() || now < loop.fastUntil || now >= loop.nextRepeat)
    {
        // A frame rate limit holds back whatever comes too soon, the menu included. A frame held back is shown with
        // the next one, or on the next repeat when the game stops drawing.
        if (const int limit = FrameRateLimit())
        {
            using namespace std::chrono;
            const nanoseconds time = FrameStatistics::Clock::now().time_since_epoch();
            if (!loop.frameLimit.Allow(time.count(), nanoseconds(seconds(1)).count() / limit))
                return;
        }
        PresentLatestFrame();
        loop.presentPending = false;
        loop.nextRepeat = now + kRepeatInterval;
    }
}

int Run()
{
    if (!GraphicsCaptureSession::IsSupported())
    {
        const wchar_t* message = L"Windows Graphics Capture is not available, and Unishade needs it to copy the game's picture. "
                                 L"Update Windows and your graphics driver.";
        Log(LogLevel::Error, L"%ls", message);
        ShowError(message);
        return 1;
    }
    RequestBorderlessCapture();

    LoadInputHotkeys();
    LoadGames();
    if (ReShadeLoaded())
        PrepareReShadeConfig();
    CreateOverlayWindow();
    InitAddon();
    InitMenu();
    g.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CreateDevice();
    CheckSetup();
    InitDepth();
    RegisterHotkeys();
    CheckForUpdate();
    CreateLauncher();
    Log(LogLevel::Info, L"Waiting for a supported game...");

    for (;;)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                FlushPresets();
                ShutdownDepth();
                ShutdownAddon();
                DestroyLauncher();
                return 0;
            }
            if (msg.message == WM_HOTKEY)
                loop.fastUntil = GetTickCount64() + kShortcutResponse;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g.captureEnabled && g.target)
            StopCapture();

        if (g.target && !GameWindowExists(*g.activeGame))
        {
            Log(LogLevel::Info, L"%ls closed.", g.activeGame->name.c_str());
            StopCapture();
        }

        if (g.captureEnabled && EnsureDevice())
        {
            if (loop.reattach)
            {
                const GameWindow game = *loop.reattach;
                loop.reattach.reset();
                if (!g.target && GameWindowExists(game) && (!g.selectedGame || g.selectedGame->window == game.window))
                    Attach(game);
            }
            SearchForGame();
        }

        UpdateOverlay();
        if (!g.overlayVisible)
        {
            g.frameStatistics.Reset(FrameStatistics::Clock::now(), g.capturedFrames.load(std::memory_order_relaxed));
            // ReShade writes preset changes from its present, which stops while the overlay is hidden.
            FlushPresets();
        }
        UpdateInputHotkey();
        UpdateHeldCompare();
        UpdateDiscord();
        UpdateLauncher();

        if (g.target)
        {
            try
            {
                ShowFrames();
            }
            catch (const winrt::hresult_error& e)
            {
                if (!DeviceLost(e.code()))
                    throw;
                OnDeviceLost(e.code());
            }
        }

        MsgWaitForMultipleObjects(1, &g.frameEvent, FALSE, g.overlayVisible ? 16 : 250, QS_ALLINPUT);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Keep the original mutex name so old and new hosts cannot run together.
    const HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\RobloxShadeHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const HWND other = FindWindowW(kLauncherClass, nullptr))
        {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    InitLog();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int result = 1;
    try
    {
        result = Run();
    }
    catch (const winrt::hresult_error& e)
    {
        Log(LogLevel::Error, L"Unishade stopped: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
        ShowError(L"Unishade stopped because of an error: " + std::wstring(e.message()));
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Unishade stopped: %hs", e.what());
        ShowError(L"Unishade stopped because of an error.");
    }
    // Removes the tray icon after an error. After a normal exit the launcher is already gone.
    DestroyLauncher();
    CloseHandle(instance);
    FlushLog();
    // Releasing the swapchain makes ReShade wait for the effects it is still compiling, which can take minutes
    // right after installing. ReShade writes settings and presets a second after they change, so the host exits
    // without that wait.
    ExitProcess(static_cast<UINT>(result));
}
