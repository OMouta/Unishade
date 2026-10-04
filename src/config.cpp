#include "config.h"
#include "log.h"
#include "state.h"
#include "text.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <iterator>

namespace
{
// Read from the file once, since the menu and the overlay ask for them every frame. -1, or 0 for the scale, until
// read. Atomic because the update check may ask from its own thread.
std::atomic<int> autoSavePresets = -1;
std::atomic<int> debugInfo = -1;
std::atomic<int> updateChecks = -1;
std::atomic<int> keepEffectsVisible = -1;
std::atomic<int> discordPresence = -1;
std::atomic<float> menuScale = 0.0f;
std::atomic<int> frameRateLimit = -1;
std::atomic<int> effectResolution = -1;
std::atomic<int> depthSize = -1;
std::atomic<unsigned> settingsVersion = 0;
// Only used on the host's thread.
std::wstring performanceGame;

std::wstring IniPath()
{
    // Keep the existing filename so upgrades retain shortcuts and menu settings.
    return ExeDirectory() + L"RobloxShadeHost.ini";
}

bool CachedFlag(std::atomic<int>& cached, const wchar_t* name, bool fallback)
{
    int value = cached;
    if (value < 0)
    {
        value = GetPrivateProfileIntW(L"Menu", name, fallback, IniPath().c_str()) != 0;
        cached = value;
    }
    return value != 0;
}

// The new value applies until the host closes even when the file cannot be written.
bool SaveFlag(std::atomic<int>& cached, const wchar_t* name, bool enabled)
{
    cached = enabled;
    ++settingsVersion;
    return WritePrivateProfileStringW(L"Menu", name, enabled ? L"1" : L"0", IniPath().c_str()) != FALSE;
}

// Also turns a scale that is not a number into the default.
float ValidScale(float scale)
{
    return scale > 0 ? std::clamp(scale, kSmallestMenuScale, kLargestMenuScale) : 1.0f;
}

std::wstring PerformanceSection(const std::wstring& game)
{
    return game.empty() ? L"Performance" : L"Performance." + game;
}

// A game's performance setting, or the default for an empty game. valid turns what the file holds into a value the
// setting takes, which is never negative.
int ReadNumber(const std::wstring& game, const wchar_t* name, int fallback, int (*valid)(int))
{
    const std::wstring path = IniPath();
    int number = static_cast<int>(GetPrivateProfileIntW(L"Performance", name, fallback, path.c_str()));
    if (!game.empty())
        number = static_cast<int>(GetPrivateProfileIntW(PerformanceSection(game).c_str(), name, number, path.c_str()));
    return valid(number);
}

void WriteNumber(const std::wstring& game, const wchar_t* name, int value)
{
    ++settingsVersion;
    if (!WritePrivateProfileStringW(PerformanceSection(game).c_str(), name, std::to_wstring(value).c_str(), IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save %ls to RobloxShadeHost.ini. It applies until Unishade closes.", name);
}

// A performance setting of the game being played, read from the file once.
int CachedNumber(std::atomic<int>& cached, const wchar_t* name, int fallback, int (*valid)(int))
{
    int value = cached;
    if (value < 0)
    {
        value = ReadNumber(performanceGame, name, fallback, valid);
        cached = value;
    }
    return value;
}

void SaveNumber(std::atomic<int>& cached, const wchar_t* name, int value)
{
    if (cached.exchange(value) == value)
        return;
    WriteNumber(performanceGame, name, value);
}

// Changes a game's setting, and the game being played's when it is that game or uses the defaults that changed.
void SaveGameNumber(std::atomic<int>& cached, const std::wstring& game, const wchar_t* name, int value)
{
    WriteNumber(game, name, value);
    if (game.empty() || _wcsicmp(game.c_str(), performanceGame.c_str()) == 0)
        cached = -1;
}

int ValidFrameRate(int fps)
{
    return fps <= 0 ? 0 : std::clamp(fps, kSlowestFrameRate, kFastestFrameRate);
}

int ValidResolution(int percent)
{
    return percent < 25 || percent > 100 ? 100 : percent;
}

int ValidDepthSize(int size)
{
    return size < 140 || size > kDepthSizes[0] ? kDefaultDepthSize : size / 14 * 14;
}

// A missing entry uses the default. An empty one leaves the shortcut unassigned when allowEmpty is set.
Hotkey ReadHotkey(const std::wstring& path, const wchar_t* name, const wchar_t* fallback, bool allowEmpty)
{
    wchar_t value[128]{};
    const DWORD count = GetPrivateProfileStringW(L"Input", name, fallback, value, static_cast<DWORD>(std::size(value)), path.c_str());
    Hotkey hotkey;
    if (allowEmpty && count == 0)
        return hotkey;
    if (count < std::size(value) - 1 && ParseHotkey(value, hotkey))
        return hotkey;
    Log(LogLevel::Warning, L"%ls=%ls in RobloxShadeHost.ini is not a supported shortcut. Using %ls instead.", name, value, fallback);
    ParseHotkey(fallback, hotkey);
    return hotkey;
}

bool Same(const Hotkey& a, const Hotkey& b)
{
    return a.key == b.key && a.modifiers == b.modifiers;
}

void UseHotkeys(const InputHotkeys& hotkeys)
{
    g.hotkeys = hotkeys;
    g.inputHotkey = FormatHotkey(hotkeys.input);
    g.overlayHotkey = FormatHotkey(hotkeys.overlay);
}

bool Register(const Shortcut& shortcut, const InputHotkeys& hotkeys)
{
    const Hotkey& hotkey = hotkeys.*shortcut.member;
    return !hotkey.key || RegisterHotKey(g.overlay, shortcut.id, hotkey.modifiers, hotkey.key);
}

// Returns the first shortcut that another program holds, or nullptr.
const Shortcut* RegisterSet(bool always, const InputHotkeys& hotkeys)
{
    const Shortcut* taken = nullptr;
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.always == always && !Register(shortcut, hotkeys) && !taken)
            taken = &shortcut;
    return taken;
}

void UnregisterSet(bool always)
{
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.always == always)
            UnregisterHotKey(g.overlay, shortcut.id);
}

void UnregisterAll()
{
    UnregisterSet(true);
    UnregisterSet(false);
    g.gameHotkeysRegistered = false;
}
} // namespace

std::optional<ShortcutClash> FindShortcutClash(const InputHotkeys& hotkeys)
{
    for (size_t later = 1; later < std::size(kShortcuts); ++later)
        for (size_t earlier = 0; earlier < later; ++earlier)
        {
            const Hotkey& hotkey = hotkeys.*kShortcuts[later].member;
            if (hotkey.key && Same(hotkey, hotkeys.*kShortcuts[earlier].member))
                return ShortcutClash{ earlier, later };
        }
    return std::nullopt;
}

std::wstring ExeDirectory()
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    const std::wstring path(executable, length);
    return path.substr(0, path.find_last_of(L"\\/") + 1);
}

void LoadInputHotkeys()
{
    const std::wstring path = IniPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        bool written = true;
        for (const Shortcut& shortcut : kShortcuts)
            written &= WritePrivateProfileStringW(L"Input", shortcut.name, shortcut.fallback, path.c_str()) != FALSE;
        if (!written)
            Log(LogLevel::Warning, L"Could not create %ls. Using the default shortcuts.", path.c_str());
    }

    InputHotkeys hotkeys;
    for (const Shortcut& shortcut : kShortcuts)
        hotkeys.*shortcut.member = ReadHotkey(path, shortcut.name, shortcut.fallback, shortcut.member != &InputHotkeys::input);
    // A key can only do one thing, so a later shortcut on the same key is turned off.
    while (const auto clash = FindShortcutClash(hotkeys))
    {
        const Shortcut& earlier = kShortcuts[clash->earlier];
        const Shortcut& later = kShortcuts[clash->later];
        Hotkey& hotkey = hotkeys.*later.member;
        Log(LogLevel::Warning, L"%ls and %ls in RobloxShadeHost.ini are both %ls, so %ls is off. Pick another one in the menu's Settings.",
            earlier.name, later.name, FormatHotkey(hotkey).c_str(), later.name);
        hotkey = {};
    }
    UseHotkeys(hotkeys);
}

void RegisterHotkeys()
{
    // Reports taken shortcuts now rather than on the first press in the game.
    for (const Shortcut& shortcut : kShortcuts)
    {
        if (Register(shortcut, g.hotkeys))
            continue;
        const std::wstring key = FormatHotkey(g.hotkeys.*shortcut.member);
        if (shortcut.member == &InputHotkeys::input)
            Log(LogLevel::Warning, L"%ls is in use by another program, so it cannot open the menu. Close that program, or change "
                                   L"ToggleKey in RobloxShadeHost.ini and restart Unishade.",
                key.c_str());
        else
            Log(LogLevel::Warning, L"%ls is in use by another program, so %ls is off. Pick another one in the menu's Settings.", key.c_str(),
                shortcut.name);
    }
    UnregisterSet(false);
}

void UpdateInputHotkey()
{
    const bool wanted = g.target && !g.hotkeysSuspended && (g.editMode || GetForegroundWindow() == g.target);
    if (wanted == g.gameHotkeysRegistered)
        return;
    if (wanted)
        RegisterSet(false, g.hotkeys);
    else
        UnregisterSet(false);
    g.gameHotkeysRegistered = wanted;
}

void SuspendHotkeys(bool suspended)
{
    if (suspended == g.hotkeysSuspended)
        return;
    g.hotkeysSuspended = suspended;
    if (suspended)
        UnregisterAll();
    else
    {
        RegisterSet(true, g.hotkeys);
        UpdateInputHotkey();
    }
}

unsigned SettingsVersion()
{
    return settingsVersion;
}

bool AutoSavePresets()
{
    return CachedFlag(autoSavePresets, L"AutoSavePresets", true);
}

void SetAutoSavePresets(bool enabled)
{
    if (!SaveFlag(autoSavePresets, L"AutoSavePresets", enabled))
        Log(LogLevel::Warning, L"Could not save the auto-save setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

bool DebugInfoEnabled()
{
    return CachedFlag(debugInfo, L"ShowDebugInfo", false);
}

void SetDebugInfoEnabled(bool enabled)
{
    if (!SaveFlag(debugInfo, L"ShowDebugInfo", enabled))
        Log(LogLevel::Warning, L"Could not save the debug info setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

float MenuScale()
{
    float scale = menuScale;
    if (scale == 0)
    {
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"Menu", L"Scale", L"1", value, static_cast<DWORD>(std::size(value)), IniPath().c_str());
        scale = ValidScale(wcstof(value, nullptr));
        menuScale = scale;
    }
    return scale;
}

void SetMenuScale(float scale)
{
    scale = ValidScale(scale);
    // A slider may set it on every frame, and the file only needs writing when it changes.
    if (scale == MenuScale())
        return;
    menuScale = scale;
    ++settingsVersion;
    wchar_t value[32]{};
    swprintf_s(value, L"%.2f", scale);
    if (!WritePrivateProfileStringW(L"Menu", L"Scale", value, IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save the menu size to RobloxShadeHost.ini. It applies until Unishade closes.");
}

bool UpdateChecksEnabled()
{
    return CachedFlag(updateChecks, L"CheckForUpdates", true);
}

void SetUpdateChecksEnabled(bool enabled)
{
    if (!SaveFlag(updateChecks, L"CheckForUpdates", enabled))
        Log(LogLevel::Warning, L"Could not save the update check setting to RobloxShadeHost.ini.");
}

bool KeepEffectsVisible()
{
    return CachedFlag(keepEffectsVisible, L"KeepEffectsVisible", true);
}

void SetKeepEffectsVisible(bool enabled)
{
    if (!SaveFlag(keepEffectsVisible, L"KeepEffectsVisible", enabled))
        Log(LogLevel::Warning, L"Could not save the effects visibility setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

bool DiscordPresenceEnabled()
{
    return CachedFlag(discordPresence, L"DiscordPresence", true);
}

void SetDiscordPresenceEnabled(bool enabled)
{
    if (!SaveFlag(discordPresence, L"DiscordPresence", enabled))
        Log(LogLevel::Warning, L"Could not save the Discord setting to RobloxShadeHost.ini. It applies until Unishade closes.");
}

void SetPerformanceGame(const std::wstring& game)
{
    if (game == performanceGame)
        return;
    performanceGame = game;
    frameRateLimit = -1;
    effectResolution = -1;
    depthSize = -1;
    ++settingsVersion;
}

const std::wstring& PerformanceGame()
{
    return performanceGame;
}

void RenamePerformanceGame(const std::wstring& from, const std::wstring& to)
{
    // Sections are found without case, so a name that only changes case already has its values.
    if (from.empty() || to.empty() || _wcsicmp(from.c_str(), to.c_str()) == 0)
        return;
    const std::wstring path = IniPath();
    std::wstring values(32768, L'\0');
    values.resize(GetPrivateProfileSectionW(PerformanceSection(from).c_str(), values.data(), static_cast<DWORD>(values.size()), path.c_str()));
    // The values are separated by nulls, and c_str adds the second null that ends them.
    if (!values.empty() && !WritePrivateProfileSectionW(PerformanceSection(to).c_str(), values.c_str(), path.c_str()))
        Log(LogLevel::Warning, L"Could not save the performance settings of %ls to RobloxShadeHost.ini.", to.c_str());
    RemovePerformanceGame(from);
    if (_wcsicmp(performanceGame.c_str(), from.c_str()) == 0)
        performanceGame = to;
}

void RemovePerformanceGame(const std::wstring& game)
{
    // Empty is [Performance] itself.
    if (game.empty())
        return;
    WritePrivateProfileStringW(PerformanceSection(game).c_str(), nullptr, nullptr, IniPath().c_str());
}

int FrameRateLimit()
{
    return CachedNumber(frameRateLimit, L"FrameRateLimit", 0, ValidFrameRate);
}

void SetFrameRateLimit(int fps)
{
    SaveNumber(frameRateLimit, L"FrameRateLimit", ValidFrameRate(fps));
}

int EffectResolution()
{
    return CachedNumber(effectResolution, L"EffectResolution", 100, ValidResolution);
}

void SetEffectResolution(int percent)
{
    SaveNumber(effectResolution, L"EffectResolution", ValidResolution(percent));
}

int DepthSize()
{
    return CachedNumber(depthSize, L"DepthSize", kDefaultDepthSize, ValidDepthSize);
}

void SetDepthSize(int size)
{
    SaveNumber(depthSize, L"DepthSize", ValidDepthSize(size));
}

int FrameRateLimit(const std::wstring& game)
{
    return ReadNumber(game, L"FrameRateLimit", 0, ValidFrameRate);
}

void SetFrameRateLimit(const std::wstring& game, int fps)
{
    SaveGameNumber(frameRateLimit, game, L"FrameRateLimit", ValidFrameRate(fps));
}

int EffectResolution(const std::wstring& game)
{
    return ReadNumber(game, L"EffectResolution", 100, ValidResolution);
}

void SetEffectResolution(const std::wstring& game, int percent)
{
    SaveGameNumber(effectResolution, game, L"EffectResolution", ValidResolution(percent));
}

int DepthSize(const std::wstring& game)
{
    return ReadNumber(game, L"DepthSize", kDefaultDepthSize, ValidDepthSize);
}

void SetDepthSize(const std::wstring& game, int size)
{
    SaveGameNumber(depthSize, game, L"DepthSize", ValidDepthSize(size));
}

std::wstring GamePreset(const std::wstring& game)
{
    std::wstring value(32768, L'\0');
    value.resize(GetPrivateProfileStringW(L"GamePresets", game.c_str(), L"", value.data(), static_cast<DWORD>(value.size()), IniPath().c_str()));
    return value;
}

void SetGamePreset(const std::wstring& game, const std::wstring& preset)
{
    if (!WritePrivateProfileStringW(L"GamePresets", game.c_str(), preset.c_str(), IniPath().c_str()))
        Log(LogLevel::Warning, L"Could not save the preset for %ls to RobloxShadeHost.ini.", game.c_str());
}

void RemoveGamePreset(const std::wstring& game)
{
    WritePrivateProfileStringW(L"GamePresets", game.c_str(), nullptr, IniPath().c_str());
}

std::wstring ChangeHotkeys(const InputHotkeys& hotkeys)
{
    if (const auto clash = FindShortcutClash(hotkeys))
    {
        // Names the shortcut that already had these keys, not the one just changed.
        const Hotkey& hotkey = hotkeys.*kShortcuts[clash->later].member;
        const bool changed = !Same(hotkey, g.hotkeys.*kShortcuts[clash->later].member);
        const char* holder = kShortcuts[changed ? clash->earlier : clash->later].title;
        return FormatHotkey(hotkey) + L" is already used by \"" + Wide(holder) + L"\".";
    }
    const InputHotkeys previous = g.hotkeys;
    UnregisterAll();

    // Registering each new shortcut once shows whether another program holds it.
    std::wstring error;
    const Shortcut* taken = RegisterSet(true, hotkeys);
    if (!taken)
        taken = RegisterSet(false, hotkeys);
    UnregisterAll();
    if (taken)
        error = FormatHotkey(hotkeys.*taken->member) + L" is in use by another program.";
    else
        for (const Shortcut& shortcut : kShortcuts)
            if (!WritePrivateProfileStringW(L"Input", shortcut.name, FormatHotkey(hotkeys.*shortcut.member).c_str(), IniPath().c_str()))
                error = L"Could not save RobloxShadeHost.ini.";

    UseHotkeys(error.empty() ? hotkeys : previous);
    if (error.empty())
    {
        ++settingsVersion;
        Log(LogLevel::Info, L"Shortcuts changed.");
    }
    RegisterSet(true, g.hotkeys);
    UpdateInputHotkey();
    return error;
}

InputHotkeys DefaultHotkeys()
{
    InputHotkeys hotkeys;
    for (const Shortcut& shortcut : kShortcuts)
        ParseHotkey(shortcut.fallback, hotkeys.*shortcut.member);
    return hotkeys;
}

std::wstring UnusableKeyText()
{
    // Any key a shortcut can be written with works, so the list of keys comes from there too.
    std::wstring keys;
    for (const NamedKey& named : kNamedKeys)
        keys += (keys.empty() ? L"" : L", ") + std::wstring(named.name);
    return L"That key cannot be used. Use a letter, a number, an F key other than F12 or one of these: " + keys +
           L". Ctrl, Alt, Shift and Win can go with any of them.";
}

std::vector<std::wstring> ShortcutWarnings()
{
    std::vector<std::wstring> warnings;
    for (const Shortcut& shortcut : kShortcuts)
    {
        const Hotkey& hotkey = g.hotkeys.*shortcut.member;
        if (!hotkey.key || (hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)))
            continue;
        const std::wstring key = FormatHotkey(hotkey);
        const bool typing = hotkey.key == VK_SPACE || hotkey.key == VK_TAB || (hotkey.key >= '0' && hotkey.key <= 'Z') ||
                            (hotkey.key >= VK_NUMPAD0 && hotkey.key <= VK_DIVIDE);
        if (shortcut.always)
            warnings.push_back(L"Other programs will not receive " + key + L" while Unishade runs.");
        else if (typing)
            warnings.push_back(L"The game will not receive " + key + L" while Unishade runs.");
    }
    return warnings;
}
