#include "overlay.h"
#include "addon.h"
#include "log.h"
#include "menu.h"
#include "state.h"

#include <dwmapi.h>

namespace
{
// WS_EX_LAYERED + WS_EX_TRANSPARENT is what makes clicks reach the game. Returning HTTRANSPARENT from
// WM_NCHITTEST only passes input to windows owned by the same thread.
constexpr DWORD kPassThroughStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
constexpr DWORD kEditStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;

// Whether the overlay sits directly above the game rather than above every window, for KeepEffectsVisible.
bool aboveGameOnly = false;

bool IsTopmost(HWND window)
{
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool IsCloaked(HWND window)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// Puts the overlay directly under the window above the game, so whatever covers the game also covers its effects.
// Topmost windows stay above all others, so the overlay first joins the game's group: HWND_NOTOPMOST leaves the
// topmost group and does nothing when already out of it.
void PlaceAboveGame(const RECT& bounds)
{
    const bool topmost = IsTopmost(g.target);
    HWND above = GetWindow(g.target, GW_HWNDPREV);
    if (above == g.overlay)
        above = GetWindow(g.overlay, GW_HWNDPREV);
    SetWindowPos(g.overlay, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Without a window of its own group above it, the game is the first of its group.
    SetWindowPos(g.overlay, above && IsTopmost(above) == topmost ? above : HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
                 bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void OpenMenu()
{
    if (g.editMode || !g.captureEnabled || !g.target || IsIconic(g.target))
        return;
    SetEditMode(true);
    UpdateOverlay();
    SetForegroundWindow(g.overlay);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_HOTKEY:
        switch (wParam)
        {
        case kOverlayToggleHotkey:
            ToggleOverlay();
            break;
        case kEditModeHotkey:
            if (g.editMode)
                ReturnToGame();
            else
                OpenMenu();
            break;
        case kCompareHotkey:
            StartHeldCompare(g.hotkeys.compare.key);
            break;
        case kScreenshotHotkey:
        case kBeforeAfterHotkey:
            RequestScreenshot(wParam == kBeforeAfterHotkey);
            break;
        case kNextPresetHotkey:
        case kPreviousPresetHotkey:
            RequestPresetStep(wParam == kNextPresetHotkey ? 1 : -1);
            break;
        }
        return 0;
    case kLeaveMenuMessage:
        if (g.editMode)
            ReturnToGame();
        return 0;
    case kOpenMenuMessage:
        OpenMenu();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            SetEditMode(false);
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g.editMode)
        {
            SetCursor(LoadCursorW(nullptr, MenuCursor()));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

void CreateOverlayWindow()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Keep the class name so existing installers can find and close the host.
    wc.lpszClassName = L"RobloxShadeHost";
    RegisterClassW(&wc);

    g.overlay = CreateWindowExW(kPassThroughStyle, wc.lpszClassName, L"Unishade", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                wc.hInstance, nullptr);
    winrt::check_bool(g.overlay != nullptr);
    Log(LogLevel::Info, L"Overlay window created: hwnd=%p, dpi=%u.", g.overlay, GetDpiForWindow(g.overlay));
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);
}

void SetEditMode(bool enabled)
{
    if (enabled == g.editMode)
        return;
    g.editMode = enabled;
    SetWindowLongPtrW(g.overlay, GWL_EXSTYLE, enabled ? kEditStyle : kPassThroughStyle);
    SetWindowPos(g.overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!enabled)
    {
        OpenReShadeMenu(false);
        ResetMenu();
    }
    Log(LogLevel::Info, !enabled ? L"Input returned to the game." : AddonRegistered() ? L"Menu opened." : L"Input captured.");
}

void ReturnToGame()
{
    SetEditMode(false);
    SetForegroundWindow(g.target);
}

void ToggleOverlay()
{
    g.captureEnabled = !g.captureEnabled;
    if (!g.captureEnabled && g.editMode)
        ReturnToGame();
    UpdateOverlay();
    Log(LogLevel::Info, g.captureEnabled ? L"Overlay on." : L"Overlay off. Frame capture stopped.");
}

void UpdateOverlay()
{
    RECT bounds{};
    const bool inFront = g.editMode || GetForegroundWindow() == g.target;
    // A game that is visible behind other windows keeps its effects when the user asked for that.
    const bool behind = !inFront && g.target && KeepEffectsVisible() && !IsCloaked(g.target);
    bool visible = g.captureEnabled && g.target && IsWindowVisible(g.target) && !IsIconic(g.target) && (inFront || behind) &&
                   SUCCEEDED(DwmGetWindowAttribute(g.target, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)));

    if (!visible)
    {
        if (g.overlayVisible)
        {
            Log(LogLevel::Info, L"Overlay hidden: capture=%d, target=%p, minimized=%d, in_front=%d, keep_visible=%d, cloaked=%d.",
                g.captureEnabled, g.target, g.target && IsIconic(g.target), inFront, KeepEffectsVisible(), g.target && IsCloaked(g.target));
            ShowWindow(g.overlay, SW_HIDE);
        }
        g.overlayVisible = false;
        return;
    }

    if (behind)
    {
        // Placed again only when the game moved or another window came between it and the overlay.
        if (!g.overlayVisible || !aboveGameOnly || !EqualRect(&bounds, &g.overlayRect) || GetWindow(g.target, GW_HWNDPREV) != g.overlay)
        {
            PlaceAboveGame(bounds);
            if (!g.overlayVisible || !aboveGameOnly || !EqualRect(&bounds, &g.overlayRect))
                Log(LogLevel::Info, L"Overlay placed above game: bounds=%ld,%ld %ldx%ld.", bounds.left, bounds.top,
                    bounds.right - bounds.left, bounds.bottom - bounds.top);
            aboveGameOnly = true;
            g.overlayRect = bounds;
            g.overlayVisible = true;
        }
        return;
    }

    if (!g.overlayVisible || aboveGameOnly || !EqualRect(&bounds, &g.overlayRect))
    {
        Log(LogLevel::Info, L"Overlay placed in front: bounds=%ld,%ld %ldx%ld.", bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top);
        SetWindowPos(g.overlay, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        aboveGameOnly = false;
        g.overlayRect = bounds;
        g.overlayVisible = true;
    }
}
