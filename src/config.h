#pragma once

#include "hotkey.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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

// What WM_HOTKEY reports for each shortcut.
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
    const wchar_t* name; // the entry in RobloxShadeHost.ini
    const wchar_t* fallback;
    // Held for as long as the host runs. The others only while the game or the menu is in front, so other
    // programs keep the keys.
    bool always;
    // What Settings calls it, in the menu and in the launcher.
    const char* title;
    const char* description;
};

// Every shortcut, in the order Settings lists them.
inline constexpr Shortcut kShortcuts[] = {
    { &InputHotkeys::input, kEditModeHotkey, L"ToggleKey", L"Home", false, "Open the menu",
      "Press it again, or Escape, to go back to the game." },
    { &InputHotkeys::overlay, kOverlayToggleHotkey, L"OverlayToggleKey", L"Ctrl+F8", true, "Overlay off and on",
      "Shows the game without effects and stops capturing it." },
    { &InputHotkeys::compare, kCompareHotkey, L"CompareKey", L"F7", false, "Compare while held",
      "Shows the game without effects for as long as you hold it." },
    { &InputHotkeys::screenshot, kScreenshotHotkey, L"ScreenshotKey", L"Ctrl+F9", false, "Screenshot", "Saves what you see, without the menu." },
    { &InputHotkeys::beforeAfter, kBeforeAfterHotkey, L"BeforeAfterKey", L"Ctrl+F10", false, "Before and after screenshots",
      "Saves the same moment with and without effects." },
    { &InputHotkeys::nextPreset, kNextPresetHotkey, L"NextPresetKey", L"Ctrl+PageDown", false, "Next preset",
      "Switches to the next preset in the Presets tab." },
    { &InputHotkeys::previousPreset, kPreviousPresetHotkey, L"PreviousPresetKey", L"Ctrl+PageUp", false, "Previous preset",
      "Switches to the preset before it." },
};

// Two shortcuts on the same keys, by their index in kShortcuts.
struct ShortcutClash
{
    size_t earlier;
    size_t later;
};

// The first two shortcuts on the same keys, in kShortcuts' order, or nothing when each has keys of its own.
// Shortcuts without a key never clash.
std::optional<ShortcutClash> FindShortcutClash(const InputHotkeys& hotkeys);

// Reads shortcuts from RobloxShadeHost.ini beside the exe into g.hotkeys, creating the file on first run.
// Invalid values are reported and replaced by the defaults.
void LoadInputHotkeys();

// Registers the shortcuts held all the time and reports shortcuts another program already holds.
void RegisterHotkeys();

// Holding a bare key such as Home all the time would break it in every other program, so most shortcuts are
// only registered while the game or the menu is in front. Called every loop.
void UpdateInputHotkey();

// Unregisters every shortcut until called with false, so the menu can read them as ordinary keys.
void SuspendHotkeys(bool suspended);

// Switches to new shortcuts and saves them. Returns what went wrong, and keeps the current shortcuts, when two
// of them are on the same keys, another program holds one of them or the file cannot be written.
std::wstring ChangeHotkeys(const InputHotkeys& hotkeys);

// The shortcuts Unishade starts with.
InputHotkeys DefaultHotkeys();

// What to tell someone who pressed a key no shortcut can be on.
std::wstring UnusableKeyText();

// The keys other programs or the game no longer receive because a shortcut is on them without a modifier, one
// line each.
std::vector<std::wstring> ShortcutWarnings();

// Goes up whenever a setting or a shortcut changes, so the launcher shows what was changed in the menu.
unsigned SettingsVersion();

// Whether the menu saves preset changes as they happen, from RobloxShadeHost.ini. On unless turned off.
bool AutoSavePresets();
void SetAutoSavePresets(bool enabled);

bool DebugInfoEnabled();
void SetDebugInfoEnabled(bool enabled);

// The user's size for the menu, on top of the size that follows the game's window, from RobloxShadeHost.ini. 1 unless
// changed, and kept between kSmallestMenuScale and kLargestMenuScale.
constexpr float kSmallestMenuScale = 0.75f;
constexpr float kLargestMenuScale = 2;
float MenuScale();
void SetMenuScale(float scale);

// Whether the host asks GitHub for a newer version when it starts, from RobloxShadeHost.ini. On unless turned off.
bool UpdateChecksEnabled();
void SetUpdateChecksEnabled(bool enabled);

// Whether effects stay over the game while another window is in front, from RobloxShadeHost.ini. On unless turned off.
bool KeepEffectsVisible();
void SetKeepEffectsVisible(bool enabled);

// Whether Discord shows the game Unishade runs on, from RobloxShadeHost.ini. On unless turned off.
bool DiscordPresenceEnabled();
void SetDiscordPresenceEnabled(bool enabled);

// The frame rate limit, effect resolution and depth size below are kept for each saved game, in RobloxShadeHost.ini
// under [Performance.<game>]. What a game has not changed comes from [Performance], which windows that are not a
// saved game use and change.
// The game they are for, by its presets folder's name, or empty for the defaults.
void SetPerformanceGame(const std::wstring& game);
const std::wstring& PerformanceGame();
// Moves a game's values along when the game is renamed, and forgets them when it is removed.
void RenamePerformanceGame(const std::wstring& from, const std::wstring& to);
void RemovePerformanceGame(const std::wstring& game);

// The most frames a second the overlay shows. 0 unless changed, which shows every frame the game draws. A limit is
// kept between kSlowestFrameRate and kFastestFrameRate. The menu is held to it too, so the slowest still leaves the
// menu usable.
constexpr int kSlowestFrameRate = 30;
constexpr int kFastestFrameRate = 500;
// The limits Settings offers, besides a custom one.
inline constexpr int kFrameRates[] = { 0, 120, 60 };
int FrameRateLimit();
void SetFrameRateLimit(int fps);

// The percentage of the game's resolution that effects run at. 100 unless changed, and never under 25.
inline constexpr int kEffectResolutions[] = { 100, 75, 50 };
int EffectResolution();
void SetEffectResolution(int percent);

// The longest side of the picture depth is estimated from, in the multiples of 14 Depth Anything V2 expects. It was
// trained with the shortest side at 518, which the largest size gives a 16:9 frame. The default is 518, which is faster.
inline constexpr int kDepthSizes[] = { 924, 518, 392, 266 };
constexpr int kDefaultDepthSize = 518;
int DepthSize();
void SetDepthSize(int size);

// The same settings of any saved game, by its presets folder's name, or the defaults for an empty name, for the
// launcher's game pages. A change to the game being played, or to the defaults it uses, applies right away.
int FrameRateLimit(const std::wstring& game);
void SetFrameRateLimit(const std::wstring& game, int fps);
int EffectResolution(const std::wstring& game);
void SetEffectResolution(const std::wstring& game, int percent);
int DepthSize(const std::wstring& game);
void SetDepthSize(const std::wstring& game, int size);

// The preset last used in a saved game, relative to the presets folder, from RobloxShadeHost.ini. Empty when the
// game has none yet.
std::wstring GamePreset(const std::wstring& game);
void SetGamePreset(const std::wstring& game, const std::wstring& preset);
// Forgets the game's preset, such as when the game is removed or renamed.
void RemoveGamePreset(const std::wstring& game);

// Where the launcher's window was when Unishade last closed, from RobloxShadeHost.ini, so it opens there again. Empty
// until it closed once.
struct LauncherPlace
{
    RECT rect; // not maximized, as GetWindowPlacement gives it
    bool maximized = false;
};
std::optional<LauncherPlace> LoadLauncherPlace();
void SaveLauncherPlace(const LauncherPlace& place);

// Folder of Unishade.exe, with a trailing backslash.
std::wstring ExeDirectory();
