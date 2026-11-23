#pragma once

#include <imgui.h>

#include <string>
#include <string_view>

// Shortcuts are written the same way as on Windows, such as Ctrl+F8. Keys are Dear ImGui's, so the menu can
// record a new shortcut from its own key events. The platform layer turns them into X11 keysyms or macOS key
// codes.
enum Modifier : unsigned
{
    kCtrl = 1,
    kAlt = 2,
    kShift = 4,
    kSuper = 8, // Cmd on macOS
};

struct Hotkey
{
    unsigned modifiers = 0;
    ImGuiKey key = ImGuiKey_None;

    bool operator==(const Hotkey&) const = default;
};

struct InputHotkeys
{
    Hotkey input; // opens and closes the menu
    Hotkey overlay;
    Hotkey compare;
    Hotkey screenshot;
    Hotkey beforeAfter;
    Hotkey nextPreset;
    Hotkey previousPreset;
};

enum HotkeyId
{
    kEditModeHotkey = 1,
    kOverlayToggleHotkey,
    kCompareHotkey,
    kScreenshotHotkey,
    kBeforeAfterHotkey,
    kNextPresetHotkey,
    kPreviousPresetHotkey,
};

struct Shortcut
{
    Hotkey InputHotkeys::*member;
    int id;
    const char* name; // the entry in Unishade.ini, the same as RobloxShadeHost.ini on Windows
    const char* fallback;
    // Held for as long as the host runs. The others only while the game or the menu is in front, so other
    // programs keep the keys.
    bool always;
    const char* label;
};

// Mac keyboards have no Home or Page Up keys without Fn, so macOS uses Cmd shortcuts instead.
#ifdef __APPLE__
inline constexpr Shortcut kShortcuts[] = {
    { &InputHotkeys::input, kEditModeHotkey, "ToggleKey", "Cmd+Shift+U", false, "Open or close the menu" },
    { &InputHotkeys::overlay, kOverlayToggleHotkey, "OverlayToggleKey", "Ctrl+Cmd+O", true, "Turn effects on or off" },
    { &InputHotkeys::compare, kCompareHotkey, "CompareKey", "Cmd+Shift+C", false, "Hold to compare with the game" },
    { &InputHotkeys::screenshot, kScreenshotHotkey, "ScreenshotKey", "Cmd+Shift+P", false, "Screenshot" },
    { &InputHotkeys::beforeAfter, kBeforeAfterHotkey, "BeforeAfterKey", "Cmd+Shift+B", false, "Before and after screenshot" },
    { &InputHotkeys::nextPreset, kNextPresetHotkey, "NextPresetKey", "Cmd+Shift+Right", false, "Next preset" },
    { &InputHotkeys::previousPreset, kPreviousPresetHotkey, "PreviousPresetKey", "Cmd+Shift+Left", false, "Previous preset" },
};
#else
inline constexpr Shortcut kShortcuts[] = {
    { &InputHotkeys::input, kEditModeHotkey, "ToggleKey", "Home", false, "Open or close the menu" },
    { &InputHotkeys::overlay, kOverlayToggleHotkey, "OverlayToggleKey", "Ctrl+F8", true, "Turn effects on or off" },
    { &InputHotkeys::compare, kCompareHotkey, "CompareKey", "F7", false, "Hold to compare with the game" },
    { &InputHotkeys::screenshot, kScreenshotHotkey, "ScreenshotKey", "Ctrl+F9", false, "Screenshot" },
    { &InputHotkeys::beforeAfter, kBeforeAfterHotkey, "BeforeAfterKey", "Ctrl+F10", false, "Before and after screenshot" },
    { &InputHotkeys::nextPreset, kNextPresetHotkey, "NextPresetKey", "Ctrl+PageDown", false, "Next preset" },
    { &InputHotkeys::previousPreset, kPreviousPresetHotkey, "PreviousPresetKey", "Ctrl+PageUp", false, "Previous preset" },
};
#endif

// Reads "Ctrl+Shift+F8". Modifiers come first, then exactly one key: a letter, a digit, F1 to F24, an arrow, a
// number pad key such as Numpad5 or NumpadAdd, or one of the other named keys. Win, Super, Cmd and Command all mean
// the same modifier, as do Alt, Option and Opt.
bool ParseHotkey(std::string_view text, Hotkey& result);

// Writes a hotkey the way ParseHotkey reads it. Empty for a key ParseHotkey does not accept.
std::string FormatHotkey(const Hotkey& hotkey);

// Whether this system can make the key part of a shortcut, for the menu's shortcut recorder. macOS stops at F20.
bool IsShortcutKey(ImGuiKey key);
