#include "app.h"
#include "log.h"
#include "theme.h"
#include "ui.h"

#include <GLFW/glfw3.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <strings.h>
#include <system_error>

App app;

namespace
{
double Now()
{
    return glfwGetTime();
}

const Shortcut* FindShortcut(int id)
{
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.id == id)
            return &shortcut;
    return nullptr;
}
} // namespace

bool App::Init(std::string& error)
{
    settings = LoadSettings();
    try
    {
        autoGames = LoadAutoGames(DataDirectory() / "games.ini");
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Warning, "Could not load games.ini: %s", e.what());
        autoGames = DefaultAutoGames();
    }

    if (!platform::Init(error) || !gpu.Init(false, error))
        return false;
    if (!runtime.Init(settings))
    {
        error = "Could not prepare the graphics card for effects.";
        return false;
    }
    runtime.LoadPreset(settings.preset);

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // The same sizes as on Windows, at 100% scaling.
    launcher.window = glfwCreateWindow(1120, 740, "Unishade", nullptr, nullptr);
    if (!launcher.window || !launcher.surface.Create(launcher.window, false, error) || !InitUi(launcher, error))
    {
        if (error.empty())
            error = "Could not open the launcher window.";
        return false;
    }
    SetWindowIcon(launcher.window);
    // Limits are in the screen's units, which macOS keeps the same at every scaling.
    float scaleX = 1, scaleY = 1;
#ifndef __APPLE__
    glfwGetWindowContentScale(launcher.window, &scaleX, &scaleY);
#endif
    glfwSetWindowSizeLimits(launcher.window, static_cast<int>(880 * scaleX), static_cast<int>(560 * scaleY), GLFW_DONT_CARE, GLFW_DONT_CARE);

    // The overlay covers the game's window exactly. Clicks go through to the game until the menu opens.
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
    overlay.window = glfwCreateWindow(64, 64, "Unishade overlay", nullptr, nullptr);
    if (!overlay.window)
    {
        error = "Could not create the overlay window.";
        return false;
    }
    platform::SetupOverlayWindow(overlay.window);
    if (!overlay.surface.Create(overlay.window, true, error) || !InitUi(overlay, error))
        return false;

    platform::SetHotkeyCallback([this](int id, bool pressed) { OnHotkey(id, pressed); });
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.always && !RegisterShortcut(shortcut, settings.hotkeys.*shortcut.member))
            Report(LogLevel::Warning, "%s is used by another program, so it cannot %s.", FormatHotkey(settings.hotkeys.*shortcut.member).c_str(),
                   Lowercase(shortcut.label).c_str());
    if (!platform::HasCapturePermission())
        Report(LogLevel::Warning, "Unishade needs permission to record the screen before it can copy the game's picture.");
    return true;
}

void App::Run()
{
    Log(LogLevel::Info, "Waiting for a supported game...");
    while (!glfwWindowShouldClose(launcher.window))
    {
        glfwWaitEventsTimeout(overlayVisible ? (menuOpen ? 1.0 / 60 : 0.1) : 0.25);
        platform::PollHotkeys();
        runtime.Update();
        if (setup.TakeFinished())
        {
            effectsCheckTime = -10;
            runtime.LoadPreset(settings.preset);
            runtime.Reload();
        }
        TakeScreenshots();

        UpdateTarget();
        // Once per pass, since on X11 each asks the X server.
        const bool overlayFocused = glfwGetWindowAttrib(overlay.window, GLFW_FOCUSED);
        inFront = active && (platform::ProcessInFront(active->pid) || overlayFocused);
        UpdateGameHotkeys();
        UpdateOverlay();

        if (menuOpen)
        {
            // Another window took focus, as the menu closes on Windows when the host loses it.
            if (overlayFocused)
                menuFocused = true;
            else if (menuFocused)
                CloseMenu(false);
        }
        else if (overlayVisible && overlayFocused && active)
            platform::Activate(*active); // the window manager focused the overlay, which should never have it

        const bool newFrame = active && platform::TakeFrame(frame);
        const bool toastShowing = !toast.empty() && Now() < toastUntil;
        if (overlayVisible && HasFrame() && (newFrame || menuOpen || toastShowing || Now() - lastOverlayFrame > 0.1))
            RenderOverlay();

        // While effects compile, the ones not done yet would be missing from the preset's order.
        if (settings.autoSavePresets && runtime.Dirty() && !runtime.Loading() && Now() - lastAutoSave > 1.0)
        {
            runtime.SavePreset();
            lastAutoSave = Now();
        }

        if (!glfwGetWindowAttrib(launcher.window, GLFW_ICONIFIED) &&
            (glfwGetWindowAttrib(launcher.window, GLFW_FOCUSED) || glfwGetWindowAttrib(launcher.window, GLFW_HOVERED) ||
             Now() - lastLauncherFrame > 0.25))
            RenderLauncher();
    }
}

void App::Shutdown()
{
    for (std::future<std::string>& screenshot : screenshots)
        screenshot.wait();
    screenshots.clear();
    StopCapture();
    if (runtime.Dirty() && settings.autoSavePresets)
        runtime.SavePreset();
    setup.Cancel();
    if (gpu.device)
        vkDeviceWaitIdle(gpu.device);
    for (UiWindow* ui : { &overlay, &launcher })
    {
        ShutdownUi(*ui);
        ui->surface.Destroy();
        if (ui->window)
            glfwDestroyWindow(ui->window);
        ui->window = nullptr;
    }
    runtime.Shutdown();
    platform::Shutdown();
    gpu.Shutdown();
}

// Finding the game

void App::StartCapture(const platform::Window& window)
{
    if (!platform::HasCapturePermission())
    {
        lastCaptureError = "Unishade needs permission to record the screen.";
        captureRetry = Now() + 2;
        return;
    }
    std::string error;
    if (!platform::StartCapture(window, error))
    {
        lastCaptureError = "Could not capture " + window.title + ": " + error;
        Log(LogLevel::Warning, "%s", lastCaptureError.c_str());
        captureRetry = Now() + 2;
        return;
    }
    active = window;
    activeExecutable = platform::ProcessExecutable(window.pid);
    activeCommand = platform::ProcessCommand(window.pid);
    frame = {};
    failedWidth = failedHeight = 0;
    cursorKnown = false;
    lastCaptureError.clear();
    Log(LogLevel::Info, "Capturing %s", window.title.c_str());
    FollowGame();
    if (!startHintShown)
    {
        ShowToast("opens the Unishade menu", 6, HotkeyText(kEditModeHotkey));
        startHintShown = true;
    }
}

void App::StopCapture()
{
    if (menuOpen)
        CloseMenu(false);
    // The platform's images go away with the capture, so the graphics card must be done with them.
    if (gpu.device)
        vkDeviceWaitIdle(gpu.device);
    frame = {};
    inFlight.reset();
    platform::StopCapture();
    if (active)
        Log(LogLevel::Info, "Stopped capturing %s.", active->title.c_str());
    active.reset();
    activeExecutable.clear();
    activeCommand.clear();
    game.clear();
    if (overlayVisible)
        platform::ShowOverlay(overlay.window, false);
    overlayVisible = false;
}

void App::UpdateTarget()
{
    // Nothing can draw once the graphics card is lost, so there is nothing to capture for.
    if (!captureEnabled || gpu.lost)
    {
        if (active)
            StopCapture();
        return;
    }
    if (active && !platform::WindowExists(*active))
    {
        Log(LogLevel::Info, "%s closed.", active->title.c_str());
        StopCapture();
    }
    else if (active && !platform::Capturing())
    {
        lastCaptureError = platform::CaptureError();
        if (!lastCaptureError.empty())
            Log(LogLevel::Warning, "%s", lastCaptureError.c_str());
        StopCapture();
        captureRetry = Now() + 2;
    }

    // Nothing else is looked for while the game is in front or the menu is open, or while a picked window is.
    if ((active && (selected || menuOpen || inFront)) || Now() < nextSearch || Now() < captureRetry)
        return;
    nextSearch = Now() + 0.5;
    std::optional<platform::Window> found;
    if (selected)
        found = FindGameTarget(selected, autoGames, 0);
    else
    {
        // The window in front first, which is quick to check. Every process only while no game is attached, and
        // only now and then, since that reads all of them.
        found = GameInFront();
        if (!found && !active && Now() >= nextScan)
        {
            nextScan = Now() + 2;
            std::vector<GameProcess> windowless;
            found = FindGameTarget(std::nullopt, autoGames, platform::ForegroundWindow(), &windowless);
            NoticeWindowless(windowless);
        }
    }
    if (found && (!active || found->id != active->id))
    {
        if (active)
            StopCapture();
        StartCapture(*found);
    }
}

// The saved game whose window is in front, other than the one attached.
std::optional<platform::Window> App::GameInFront()
{
    const platform::WindowId foreground = platform::ForegroundWindow();
    if (!foreground || foreground == notGame || (active && foreground == active->id))
        return std::nullopt;
    std::optional<platform::Window> window = platform::ListedWindow(foreground);
    if (!window)
        return std::nullopt;
    const int index = MatchingGame(autoGames, platform::ProcessExecutable(window->pid), platform::ProcessCommand(window->pid));
    if (index < 0)
    {
        notGame = foreground;
        return std::nullopt;
    }
    window->title = autoGames[index].name;
    return window;
}

// Under Wayland, a game that draws to Wayland directly has no X11 window, which looks like a game that never opens.
// That is said once per process, after a while, since games also start without a window.
void App::NoticeWindowless(const std::vector<GameProcess>& processes)
{
    std::map<int, double> since;
    if (!processes.empty() && platform::WaylandDesktop())
    {
        const std::set<int> owners = platform::WindowOwners();
        for (const GameProcess& process : processes)
        {
            if (owners.count(process.pid))
                continue;
            const auto known = windowlessSince.find(process.pid);
            const double first = known != windowlessSince.end() ? known->second : Now();
            since[process.pid] = first;
            if (Now() - first >= 10 && windowlessNoticed.insert(process.pid).second)
                Report(LogLevel::Warning,
                       "%s is running without an X11 window, so Unishade cannot see it. Unishade works with games that run through "
                       "XWayland: start this one with SDL_VIDEODRIVER=x11, or turn off Wayland in Wine, Proton or the game's launcher.",
                       autoGames[process.game].name.c_str());
        }
    }
    windowlessSince = std::move(since);
}

void App::Select(std::optional<platform::Window> window)
{
    selected = std::move(window);
    nextSearch = 0;
    captureRetry = 0;
    if (active && (!selected || selected->id != active->id))
        StopCapture();
}

void App::SaveGames()
{
    // The window in front may be one of the games now.
    notGame = 0;
    if (!SaveAutoGames(DataDirectory() / "games.ini", autoGames))
        Report(LogLevel::Warning, "Could not save the game list.");
}

const std::vector<platform::Window>& App::Windows()
{
    if (Now() - windowsTime > 2)
    {
        windows = platform::ListWindows();
        windowsTime = Now();
    }
    return windows;
}

// The overlay

void App::UpdateOverlay()
{
    platform::Rect bounds;
    const bool sized = frame.width != failedWidth || frame.height != failedHeight;
    const bool visible = captureEnabled && active && HasFrame() && sized && (menuOpen || inFront) && platform::WindowBounds(active->id, bounds);
    platform::SetCaptureIdle(!visible);
    if (!visible)
    {
        if (overlayVisible)
            platform::ShowOverlay(overlay.window, false);
        overlayVisible = false;
        return;
    }
    if (!overlayVisible || !(bounds == overlayBounds))
    {
        glfwSetWindowPos(overlay.window, bounds.x, bounds.y);
        glfwSetWindowSize(overlay.window, bounds.width, bounds.height);
        overlayBounds = bounds;
    }
    if (!overlayVisible)
    {
        platform::ShowOverlay(overlay.window, true);
        overlayVisible = true;
    }
}

void App::RenderOverlay()
{
    // Effects work at the size of the game's picture. Where the graphics card cannot make room for that, the overlay
    // hides rather than cover the game with nothing.
    runtime.SetSize(frame.width, frame.height);
    if (runtime.Width() != frame.width || runtime.Height() != frame.height)
    {
        // A size still being prepared, or a lost graphics card, says nothing about room for effects.
        if (runtime.Loading() || gpu.lost)
            return;
        failedWidth = frame.width;
        failedHeight = frame.height;
        Report(LogLevel::Warning, "The graphics card has no room for effects on a %ux%u picture. The overlay stays hidden until the game's window changes size.",
               frame.width, frame.height);
        if (menuOpen)
            CloseMenu();
        UpdateOverlay();
        return;
    }
    failedWidth = failedHeight = 0;

    Surface& surface = overlay.surface;
    if (!surface.BeginFrame())
        return;
    // The previous frame's commands are done, so its buffer can go back to the platform.
    inFlight = frame.hold;
    lastOverlayFrame = Now();
    runtime.menuOpen = menuOpen;
    double x = 0, y = 0;
    int width = 1, height = 1;
    glfwGetCursorPos(overlay.window, &x, &y);
    glfwGetWindowSize(overlay.window, &width, &height);
    runtime.mouseX = static_cast<float>(x * frame.width / std::max(width, 1));
    runtime.mouseY = static_cast<float>(y * frame.height / std::max(height, 1));
    // Effects can leave themselves out of screenshots, so only a frame rendered knowing it is one is saved. A
    // request made in this frame's menu is taken by the next frame.
    const bool screenshot = screenshotRequested;
    UpdateInput();

    runtime.Render(surface.commands, Source(), effectsEnabled && !comparing && !compareButton);
    FullBarrier(surface.commands);
    const GpuImage& output = runtime.Output();
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { int(output.width), int(output.height), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1] = { int(surface.extent.width), int(surface.extent.height), 1 };
    vkCmdBlitImage(surface.commands, output.image, VK_IMAGE_LAYOUT_GENERAL, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    BeginUi(overlay);
    DrawOverlay(*this);
    // For effects in the next frame. The wheel only reaches the overlay while the menu is open.
    const ImGuiIO& io = ImGui::GetIO();
    menuWheel = io.MouseWheel;
    menuActive = ImGui::IsAnyItemActive();
    menuHovered = io.WantCaptureMouse;
    menuTyping = io.WantTextInput;
    EndUi(overlay);

    if (screenshot)
        SaveScreenshot();
}

// What effects read of the keyboard and mouse. Keys are read for the whole system, so only while they go to the game
// or the menu.
void App::UpdateInput()
{
    std::array<bool, 256> keys{};
    std::array<bool, 5> buttons{};
    if (menuOpen || inFront)
        platform::ReadInput(keys, buttons);
    // Windows also has a code for Shift, Ctrl and Alt on either side, and counts the mouse buttons as keys.
    keys[0x10] = keys[0xA0] || keys[0xA1];
    keys[0x11] = keys[0xA2] || keys[0xA3];
    keys[0x12] = keys[0xA4] || keys[0xA5];
    keys[0x01] = buttons[0];
    keys[0x02] = buttons[1];
    keys[0x04] = buttons[2];
    keys[0x05] = buttons[3];
    keys[0x06] = buttons[4];

    fx::EffectInput& input = runtime.input;
    // Typing in the menu, such as a search, must not set off technique shortcuts.
    const bool typing = menuOpen && menuTyping;
    for (size_t i = 0; i < keys.size(); ++i)
    {
        input.keysPressed[i] = keys[i] && !input.keysDown[i] && !typing;
        input.keysDown[i] = keys[i];
    }
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        input.buttonsPressed[i] = buttons[i] && !input.buttonsDown[i];
        input.buttonsDown[i] = buttons[i];
    }
    // The first frame of a capture has nothing to compare with.
    input.cursorDeltaX = cursorKnown ? runtime.mouseX - lastMouseX : 0;
    input.cursorDeltaY = cursorKnown ? runtime.mouseY - lastMouseY : 0;
    lastMouseX = runtime.mouseX;
    lastMouseY = runtime.mouseY;
    cursorKnown = true;
    input.wheelDelta = menuOpen ? menuWheel : 0;
    input.overlayActive = menuOpen && menuActive;
    input.overlayHovered = menuOpen && menuHovered;
    input.activeUniform = menuOpen ? menuActiveUniform : nullptr;
    input.hoveredUniform = menuOpen ? menuHoveredUniform : nullptr;
    input.screenshot = screenshotRequested;
}

void App::RenderLauncher()
{
    Surface& surface = launcher.surface;
    if (!surface.BeginFrame())
        return;
    lastLauncherFrame = Now();
    VkClearColorValue background{};
    background.float32[0] = ((theme::kBackground >> 16) & 0xFF) / 255.0f;
    background.float32[1] = ((theme::kBackground >> 8) & 0xFF) / 255.0f;
    background.float32[2] = (theme::kBackground & 0xFF) / 255.0f;
    background.float32[3] = 1.0f;
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(surface.commands, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &range);
    BeginUi(launcher);
    DrawLauncher(*this);
    EndUi(launcher);
}

void App::OpenMenu()
{
    if (menuOpen || !captureEnabled || !active || !overlayVisible)
        return;
    menuOpen = true;
    menuFocused = false;
    glfwSetWindowAttrib(overlay.window, GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);
    platform::FocusOverlay(overlay.window);
    Log(LogLevel::Info, "Menu opened.");
}

void App::CloseMenu(bool returnToGame)
{
    if (!menuOpen)
        return;
    menuOpen = false;
    ResetMenu(*this);
    glfwSetWindowAttrib(overlay.window, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    if (settings.autoSavePresets && runtime.Dirty())
        runtime.SavePreset();
    if (returnToGame && active)
        platform::Activate(*active);
    Log(LogLevel::Info, "Input returned to the game.");
}

void App::ToggleOverlay()
{
    captureEnabled = !captureEnabled;
    if (!captureEnabled && menuOpen)
        CloseMenu();
    Log(LogLevel::Info, captureEnabled ? "Overlay on." : "Overlay off. Frame capture stopped.");
}

void App::ShowToast(std::string text, double seconds, std::string key)
{
    toast = std::move(text);
    toastKey = std::move(key);
    toastUntil = Now() + seconds;
}

// Shortcuts

std::string App::HotkeyText(int id) const
{
    const Shortcut* shortcut = FindShortcut(id);
    return shortcut ? FormatHotkey(settings.hotkeys.*shortcut->member) : std::string();
}

bool App::RegisterShortcut(const Shortcut& shortcut, const Hotkey& hotkey)
{
    return platform::RegisterHotkey(shortcut.id, hotkey);
}

void App::OnHotkey(int id, bool pressed)
{
    switch (id)
    {
    case kEditModeHotkey:
        if (pressed)
            menuOpen ? CloseMenu() : OpenMenu();
        break;
    case kOverlayToggleHotkey:
        if (pressed)
            ToggleOverlay();
        break;
    case kCompareHotkey:
        comparing = pressed;
        break;
    case kScreenshotHotkey:
    case kBeforeAfterHotkey:
        if (pressed)
            RequestScreenshot(id == kBeforeAfterHotkey);
        break;
    case kNextPresetHotkey:
    case kPreviousPresetHotkey:
        if (pressed)
            StepPreset(id == kNextPresetHotkey ? 1 : -1);
        break;
    }
}

// Holding a bare key such as Home all the time would break it in every other program, so most shortcuts are only
// held while the game or the menu is in front.
void App::UpdateGameHotkeys()
{
    const bool wanted = !hotkeysSuspended && active && (menuOpen || inFront);
    if (wanted == gameHotkeys)
        return;
    gameHotkeys = wanted;
    for (const Shortcut& shortcut : kShortcuts)
    {
        if (shortcut.always)
            continue;
        if (!wanted)
            platform::UnregisterHotkey(shortcut.id);
        else if (!RegisterShortcut(shortcut, settings.hotkeys.*shortcut.member))
            Report(LogLevel::Warning, "%s is used by another program, so it cannot %s.", FormatHotkey(settings.hotkeys.*shortcut.member).c_str(),
                   Lowercase(shortcut.label).c_str());
    }
    if (!wanted)
        comparing = false;
}

void App::SuspendHotkeys(bool suspended)
{
    if (suspended == hotkeysSuspended)
        return;
    hotkeysSuspended = suspended;
    for (const Shortcut& shortcut : kShortcuts)
    {
        if (suspended)
            platform::UnregisterHotkey(shortcut.id);
        else if (shortcut.always || gameHotkeys)
            RegisterShortcut(shortcut, settings.hotkeys.*shortcut.member);
    }
}

bool App::ChangeHotkeys(const InputHotkeys& hotkeys, std::string& error)
{
    for (const Shortcut& a : kShortcuts)
        for (const Shortcut& b : kShortcuts)
            if (&a < &b && hotkeys.*a.member == hotkeys.*b.member)
            {
                error = std::string(a.label) + " and " + Lowercase(b.label) + " cannot share " + FormatHotkey(hotkeys.*a.member) + ".";
                return false;
            }
    for (const Shortcut& shortcut : kShortcuts)
        platform::UnregisterHotkey(shortcut.id);
    const auto registerAll = [this](const InputHotkeys& keys, const Shortcut** failed) {
        for (const Shortcut& shortcut : kShortcuts)
            if ((shortcut.always || gameHotkeys) && !RegisterShortcut(shortcut, keys.*shortcut.member))
            {
                if (failed)
                    *failed = &shortcut;
                return false;
            }
        return true;
    };
    const Shortcut* failed = nullptr;
    if (!registerAll(hotkeys, &failed))
    {
        error = FormatHotkey(hotkeys.*failed->member) + " is used by another program.";
        for (const Shortcut& shortcut : kShortcuts)
            platform::UnregisterHotkey(shortcut.id);
        registerAll(settings.hotkeys, nullptr);
        return false;
    }
    settings.hotkeys = hotkeys;
    SaveSettings(settings);
    return true;
}

// Presets

std::vector<fs::path> PresetsIn(const fs::path& folder)
{
    std::vector<fs::path> presets;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error))
        if (entry.is_regular_file(error) && Lowercase(entry.path().extension().string()) == ".ini")
            presets.push_back(entry.path());
    std::sort(presets.begin(), presets.end(), [](const fs::path& a, const fs::path& b) {
        return strcasecmp(a.stem().c_str(), b.stem().c_str()) < 0;
    });
    return presets;
}

std::vector<PresetFolder> App::PresetFolders() const
{
    const fs::path root = PresetsDirectory();
    // The game being played is listed even before it has presets, since new ones go there.
    PresetFolder playing{ .path = root / game, .name = game, .game = true, .playing = true };
    PresetFolder all{ .path = root, .name = "All games", .all = true, .presets = PresetsIn(root) };
    std::vector<PresetFolder> others;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error))
    {
        if (!entry.is_directory(error))
            continue;
        const std::string name = entry.path().filename().string();
        if (!game.empty() && !strcasecmp(name.c_str(), game.c_str()))
        {
            playing.path = entry.path();
            playing.name = name;
            playing.presets = PresetsIn(entry.path());
            continue;
        }
        PresetFolder folder{ .path = entry.path(), .name = name, .presets = PresetsIn(entry.path()) };
        folder.game = std::any_of(autoGames.begin(), autoGames.end(),
                                  [&](const AutoGame& saved) { return !strcasecmp(FolderName(saved.name).c_str(), name.c_str()); });
        if (!folder.presets.empty())
            others.push_back(std::move(folder));
    }
    std::sort(others.begin(), others.end(), [](const PresetFolder& a, const PresetFolder& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });

    std::vector<PresetFolder> folders;
    if (!game.empty())
        folders.push_back(std::move(playing));
    folders.push_back(std::move(all));
    std::move(others.begin(), others.end(), std::back_inserter(folders));
    // Unishade.ini can name a preset elsewhere, which is listed with all games.
    const fs::path& current = runtime.PresetPath();
    const auto listed = [&](const PresetFolder& folder) {
        return std::find(folder.presets.begin(), folder.presets.end(), current) != folder.presets.end();
    };
    if (!current.empty() && std::none_of(folders.begin(), folders.end(), listed))
    {
        const auto folder = std::find_if(folders.begin(), folders.end(), [&](const PresetFolder& folder) { return folder.path == current.parent_path(); });
        (folder != folders.end() ? *folder : folders[game.empty() ? 0 : 1]).presets.push_back(current);
    }
    return folders;
}

bool App::FolderOpen(const PresetFolder& folder) const
{
    if (const auto found = folderOpen.find(folder.path.string()); found != folderOpen.end())
        return found->second;
    return !folder.game || folder.playing || std::find(folder.presets.begin(), folder.presets.end(), runtime.PresetPath()) != folder.presets.end();
}

fs::path App::NewPresetFolder() const
{
    const fs::path root = PresetsDirectory();
    if (game.empty())
        return root;
    // Names on Linux differ by case, and a folder named in another case is still the game's.
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error))
        if (entry.is_directory(error) && !strcasecmp(entry.path().filename().c_str(), game.c_str()))
            return entry.path();
    return root / game;
}

bool App::MovePreset(const fs::path& preset, const fs::path& folder, std::string& error)
{
    const fs::path target = folder / preset.filename();
    std::error_code failure;
    if (fs::exists(target, failure))
    {
        error = "There is already a preset named " + preset.stem().string() + " there.";
        return false;
    }
    fs::create_directories(folder, failure);
    if (!failure)
        fs::rename(preset, target, failure);
    if (failure)
    {
        error = "Could not move " + preset.stem().string() + ".";
        return false;
    }
    return true;
}

// Switching to a saved game switches to the preset last used in it.
void App::FollowGame()
{
    game.clear();
    for (const AutoGame& saved : autoGames)
        if (MatchesProcess(saved, activeExecutable, activeCommand))
        {
            game = FolderName(saved.name);
            break;
        }
    if (game.empty())
        return;

    // Games saved by filename, like Roblox, can only be found while they run, so their icon is kept.
    const fs::path logo = NewPresetFolder() / "logo.png";
    std::error_code error;
    if (!fs::exists(logo, error))
    {
        fs::create_directories(logo.parent_path(), error);
        platform::SaveWindowIcon(*active, logo.string());
    }

    const fs::path preset = GamePreset(game);
    if (preset.empty())
        SetGamePreset(game, runtime.PresetPath());
    else if (preset != runtime.PresetPath() && fs::exists(preset, error) && !SwitchPreset(preset, settings.autoSavePresets, false))
        ShowToast("Save or discard the changes to " + runtime.PresetPath().stem().string() + " to switch to " + preset.stem().string());
}

void App::UsePreset(const fs::path& path)
{
    settings.preset = path;
    SaveSettings(settings);
    if (!game.empty())
        SetGamePreset(game, path);
}

bool App::SwitchPreset(const fs::path& path, bool save, bool discard)
{
    if (path == runtime.PresetPath() && !discard)
        return true;
    if (runtime.Dirty() && path != runtime.PresetPath())
    {
        if (save)
            runtime.SavePreset();
        else if (!discard)
            return false;
    }
    runtime.LoadPreset(path);
    UsePreset(path);
    ShowToast("Preset: " + path.stem().string());
    return true;
}

std::string PresetNameProblem(const std::string& name)
{
    if (!name.empty() && name[0] == '.')
        return "Names cannot start with a dot.";
    switch (CheckName(name, false))
    {
    case NameIssue::None:
        return {};
    case NameIssue::Empty:
        return "Type a name.";
    case NameIssue::Control:
    case NameIssue::Character:
        return "Names cannot contain \\ / : * ? \" < > | or control characters.";
    case NameIssue::End:
        return "Names cannot end in a dot or a space.";
    case NameIssue::Device:
        return "Windows keeps that name for a device. Pick another one.";
    }
    return {};
}

bool App::NewPreset(const std::string& name, bool copyCurrent, std::string& error)
{
    if (std::string problem = PresetNameProblem(name); !problem.empty())
    {
        error = std::move(problem);
        return false;
    }
    const fs::path path = NewPresetFolder() / (name + ".ini");
    std::error_code missing;
    if (fs::exists(path, missing))
    {
        error = "A preset with that name already exists.";
        return false;
    }
    if (copyCurrent)
    {
        // The active preset is copied as it is on screen, unsaved changes included.
        if (!runtime.SavePresetAs(path))
        {
            error = "Could not write the preset.";
            return false;
        }
        UsePreset(path);
        return true;
    }
    if (runtime.Dirty() && !settings.autoSavePresets)
    {
        error = "Save or discard the current preset's changes first.";
        return false;
    }
    if (!WriteFile(path, "Techniques=\n"))
    {
        error = "Could not write the preset.";
        return false;
    }
    return SwitchPreset(path, true, false);
}

void App::StepPreset(int step)
{
    // The presets in open folders, in the order the menu shows them.
    std::vector<fs::path> presets;
    for (const PresetFolder& folder : PresetFolders())
        if (FolderOpen(folder))
            presets.insert(presets.end(), folder.presets.begin(), folder.presets.end());
    if (presets.empty())
        return;
    if (runtime.Dirty() && !settings.autoSavePresets)
    {
        ShowToast("Save or discard the preset's changes first");
        return;
    }
    const auto current = std::find(presets.begin(), presets.end(), runtime.PresetPath());
    const long count = static_cast<long>(presets.size());
    // From a preset in a closed folder, the next one is the first shown and the previous one the last.
    const long index = current != presets.end() ? current - presets.begin() : step > 0 ? -1 : count;
    SwitchPreset(presets[((index + step) % count + count) % count], true, false);
}

// Screenshots

void App::RequestScreenshot(bool beforeAfter)
{
    screenshotRequested = true;
    beforeAfterRequested = beforeAfter;
}

bool App::HasFrame() const
{
    return frame.image || !frame.pixels.empty();
}

fx::Runtime::Source App::Source() const
{
    return { frame.pixels.empty() ? nullptr : frame.pixels.data(), frame.image, frame.x, frame.y, frame.foreign };
}

void App::SaveScreenshot()
{
    screenshotRequested = false;
    std::vector<uint8_t> after = runtime.ReadOutput();
    if (after.empty())
        return;
    const uint32_t width = runtime.Width(), height = runtime.Height();
    std::vector<uint8_t> before;
    std::vector<uint32_t> beforePixels;
    const bool beforeAfter = beforeAfterRequested && frame.width == width && frame.height == height;
    if (beforeAfter && frame.image)
        before = runtime.ReadSource(Source());
    else if (beforeAfter)
        beforePixels = frame.pixels;

    char stamp[64];
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
    // Window titles can hold anything, slashes included, so the name keeps none of that.
    std::string name = SafeFileName(active ? active->title : "Unishade") + " " + stamp;
    // Another screenshot in the same second gets a name of its own, rather than the same file written twice at once.
    screenshotsInStamp = screenshotStamp == stamp ? screenshotsInStamp + 1 : 1;
    screenshotStamp = stamp;
    if (screenshotsInStamp > 1)
        name += " " + std::to_string(screenshotsInStamp);
    const fs::path folder = ScreenshotDirectory();
    const fs::path path = folder / (name + (beforeAfter ? " before-after" : "") + ".png");
    std::string shown = folder.string();
    if (const char* home = getenv("HOME"); home && *home && shown.rfind(home, 0) == 0)
        shown = "~" + shown.substr(strlen(home));

    // Encoding the PNG takes long enough to hold up the game's picture, so it happens on a thread of its own.
    auto write = [path, shown, width, height, after = std::move(after), before = std::move(before),
                  beforePixels = std::move(beforePixels)]() mutable -> std::string {
        if (!beforePixels.empty())
        {
            before.resize(size_t(width) * height * 4);
            for (size_t i = 0; i < beforePixels.size(); ++i)
            {
                const uint32_t pixel = beforePixels[i];
                before[i * 4] = (pixel >> 16) & 0xFF;
                before[i * 4 + 1] = (pixel >> 8) & 0xFF;
                before[i * 4 + 2] = pixel & 0xFF;
                before[i * 4 + 3] = 255;
            }
        }
        std::vector<uint8_t> image;
        uint32_t imageWidth = width;
        if (!before.empty())
        {
            // The game's own picture on the left, with effects on the right.
            imageWidth = width * 2;
            image.resize(size_t(imageWidth) * height * 4);
            for (uint32_t y = 0; y < height; ++y)
            {
                std::memcpy(&image[size_t(y) * imageWidth * 4], &before[size_t(y) * width * 4], size_t(width) * 4);
                std::memcpy(&image[(size_t(y) * imageWidth + width) * 4], &after[size_t(y) * width * 4], size_t(width) * 4);
            }
        }
        else
            image = std::move(after);
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (!stbi_write_png(path.c_str(), int(imageWidth), int(height), 4, image.data(), int(imageWidth) * 4))
        {
            Report(LogLevel::Warning, "Could not save the screenshot to %s.", path.c_str());
            return {};
        }
        Log(LogLevel::Info, "Screenshot saved to %s", path.c_str());
        return "Screenshot saved to " + shown;
    };
    try
    {
        screenshots.push_back(std::async(std::launch::async, std::move(write)));
    }
    catch (const std::system_error&)
    {
        Report(LogLevel::Warning, "Could not save the screenshot to %s.", path.c_str());
    }
}

// Shows where each screenshot went once it is written.
void App::TakeScreenshots()
{
    for (auto screenshot = screenshots.begin(); screenshot != screenshots.end();)
    {
        if (screenshot->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++screenshot;
            continue;
        }
        if (const std::string message = screenshot->get(); !message.empty())
            ShowToast(message);
        screenshot = screenshots.erase(screenshot);
    }
}

bool App::EffectsInstalled()
{
    if (Now() - effectsCheckTime < 5)
        return effectsInstalled;
    effectsCheckTime = Now();
    effectsInstalled = false;
    for (const fs::path& root : settings.effectPaths)
    {
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, error), end; it != end && !effectsInstalled; it.increment(error))
            effectsInstalled = !error && Lowercase(it->path().extension().string()) == ".fx";
    }
    return effectsInstalled;
}
