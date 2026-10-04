// The launcher's window and its notification area icon. What it shows is the launcher every platform shares, in
// src/ui, which UnishadeUi.dll draws: this file describes Unishade to it and carries out what the user does.

#include "launcher.h"
#include "addon.h"
#include "capture.h"
#include "config.h"
#include "depth/depth.h"
#include "discord.h"
#include "log.h"
#include "menu.h"
#include "names.h"
#include "overlay.h"
#include "resource.h"
#include "shell.h"
#include "state.h"
#include "text.h"
#include "theme.h"
#include "update.h"
#include "ui/launcher_module.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
static_assert(std::ranges::equal(kFrameRates, launcher::kFrameRates) && std::ranges::equal(kEffectResolutions, launcher::kEffectResolutions) &&
              std::ranges::equal(kDepthSizes, launcher::kDepthSizes) && kSlowestFrameRate == launcher::kSlowestFrameRate &&
              kFastestFrameRate == launcher::kFastestFrameRate);

constexpr wchar_t kUiModule[] = L"UnishadeUi.dll";
// The window's size in pixels at 100% scaling, the first time it opens, and the smallest it gets.
constexpr int kWidth = 1120;
constexpr int kHeight = 740;
constexpr int kMinWidth = 880;
constexpr int kMinHeight = 560;
// How long a removed game can be put back.
constexpr UINT kUndoMilliseconds = 8000;
// The presets folder is read this often while the launcher shows.
constexpr ULONGLONG kPresetScanInterval = 2000;

enum Timer : UINT_PTR
{
    kUndoTimer = 1,
    kTrayTimer,
    kFrameTimer,
};

// Sent to the launcher by its notification area icon.
constexpr UINT kTrayMessage = WM_APP + 16;
constexpr UINT kTrayIcon = 1;

enum TrayCommand : UINT
{
    kOpenCommand = 1,
    kEffectsCommand,
    kQuitCommand,
};

// Windows starts Unishade at sign-in with this entry, giving it kMinimizedFlag.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Task Manager's Startup apps turn entries off here, without removing them.
constexpr wchar_t kStartupApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunValue[] = L"Unishade";
constexpr wchar_t kMinimizedFlag[] = L"--minimized";

constexpr wchar_t kSupportUrl[] = L"https://ko-fi.com/omouta";

// What the launcher shows depends on, compared every loop to know when to describe it again. Cheap to read.
struct Shown
{
    bool captureEnabled = false;
    HWND target = nullptr;
    HWND selected = nullptr;
    bool selectedOpen = false;
    unsigned notices = 0;
    unsigned update = 0;
    UINT inputKey = 0;
    UINT inputModifiers = 0;
    UINT overlayKey = 0;
    UINT overlayModifiers = 0;
    unsigned settings = 0;
    unsigned discord = 0;
    std::wstring preset;

    bool operator==(const Shown&) const = default;
};

// A removed game, which can be put back for a few seconds.
struct Removed
{
    AutoGame game;
    size_t index = 0; // where it was in the list
};

struct Launcher
{
    HMODULE module = nullptr;
    LauncherUi* ui = nullptr;
    winrt::com_ptr<IWICImagingFactory> imaging;

    launcher::Model model;
    Shown shown;
    // While the window draws, what the user does only sets dirty, since the model it draws must not change.
    bool drawing = false;
    bool dirty = false;
    // Goes up when pictures may have changed.
    unsigned pictures = 0;
    std::vector<uint32_t> pixels;
    // Frames still to draw after input, so hover and clicks settle.
    int settle = 0;

    // Where games saved by filename were found running.
    std::map<std::wstring, fs::path> located;
    fs::path activeExecutable;
    Update update;
    std::vector<std::function<void()>> links;
    std::optional<Removed> removed;
    std::wstring renameProblem;
    // The open windows while the picker lists them, to add a game or use for this session.
    bool picking = false;
    bool pickToAdd = true;
    std::vector<GameWindow> windows;
    // The shortcut waiting for its keys, by its index in kShortcuts, or -1.
    int recording = -1;
    std::wstring shortcutError;
    // The presets the games' pages list, to tell when a new reading of the presets folder changed them.
    std::vector<fs::path> presets;
    unsigned presetScan = 0;
    ULONGLONG nextPresetScan = 0;

    // Explorer forgets the notification area icon when it restarts, and then sends TaskbarCreated.
    UINT taskbarCreated = 0;
    HICON trayIcon = nullptr;
    bool trayAdded = false;
};
Launcher l;

// Empty when the process has exited or denies access, which shows the name's first letter instead of the icon.
fs::path Executable(DWORD processId)
{
    try
    {
        return ProcessExecutable(processId);
    }
    catch (const std::system_error&)
    {
        return {};
    }
}

// Games saved by filename, like Roblox, move with every update, so their icon comes from a running copy.
void LocateGames()
{
    // Found again when the copy found is gone, such as after an update.
    std::erase_if(l.located, [](const auto& entry) { return GetFileAttributesW(entry.second.c_str()) == INVALID_FILE_ATTRIBUTES; });
    const auto unlocated = [](const AutoGame& game) { return !game.executable.has_parent_path() && !l.located.contains(game.executable.wstring()); };
    if (std::none_of(g.autoGames.begin(), g.autoGames.end(), unlocated))
        return;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W process{ sizeof(process) };
    for (BOOL ok = Process32FirstW(snapshot, &process); ok; ok = Process32NextW(snapshot, &process))
        for (const AutoGame& game : g.autoGames)
            if (unlocated(game) && _wcsicmp(process.szExeFile, game.executable.c_str()) == 0)
                if (fs::path executable = Executable(process.th32ProcessID); !executable.empty())
                    l.located[game.executable.wstring()] = std::move(executable);
    CloseHandle(snapshot);
}

// What Windows runs at sign-in: this exe, starting in the notification area.
std::wstring StartCommand()
{
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return L"\"" + path + L"\" " + kMinimizedFlag;
}

// Only while the entry starts this exe, so it shows off after Unishade moves, until it is turned on again.
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
        Log(LogLevel::Warning, L"Could not change whether Unishade starts with Windows (error %ld).", status);
    else
        Log(LogLevel::Info, start ? L"Unishade starts with Windows." : L"Unishade no longer starts with Windows.");
}

// Whether Windows started Unishade at sign-in, which starts it in the notification area.
bool StartedWithWindows()
{
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return false;
    bool found = false;
    for (int i = 1; i < count; ++i)
        if (_wcsicmp(arguments[i], kMinimizedFlag) == 0)
            found = true;
    LocalFree(arguments);
    return found;
}

// Pictures

// Copies a picture into rgba, size by size, scaled with premultiplied alpha so its transparent edge stays clean.
bool CopyScaled(IWICBitmapSource* source, int size, uint8_t* rgba)
{
    winrt::com_ptr<IWICFormatConverter> premultiplied;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> straight;
    const UINT side = static_cast<UINT>(size);
    return l.imaging && SUCCEEDED(l.imaging->CreateFormatConverter(premultiplied.put())) &&
           SUCCEEDED(premultiplied->Initialize(source, GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(l.imaging->CreateBitmapScaler(scaler.put())) &&
           SUCCEEDED(scaler->Initialize(premultiplied.get(), side, side, WICBitmapInterpolationModeHighQualityCubic)) &&
           SUCCEEDED(l.imaging->CreateFormatConverter(straight.put())) &&
           SUCCEEDED(straight->Initialize(scaler.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(straight->CopyPixels(nullptr, side * 4, side * side * 4, rgba));
}

bool DecodedPixels(IWICBitmapDecoder* decoder, int size, uint8_t* rgba)
{
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    return SUCCEEDED(decoder->GetFrame(0, frame.put())) && CopyScaled(frame.get(), size, rgba);
}

// The pictures the model names: "logo", "exe:" and an executable for its icon, or "png:" and a picture file.
class Pictures final : public LauncherPictures
{
public:
    bool Pixels(std::string_view key, int size, uint8_t* rgba) override
    {
        if (!l.imaging)
            return false;
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        if (key == "logo")
        {
            const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
            const HGLOBAL resource = info ? LoadResource(nullptr, info) : nullptr;
            winrt::com_ptr<IWICStream> stream;
            return resource && SUCCEEDED(l.imaging->CreateStream(stream.put())) &&
                   SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(resource)), SizeofResource(nullptr, info))) &&
                   SUCCEEDED(l.imaging->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) &&
                   DecodedPixels(decoder.get(), size, rgba);
        }
        const std::wstring path = Wide(key.substr(std::min<size_t>(4, key.size())));
        if (key.starts_with("png:"))
            return SUCCEEDED(l.imaging->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put())) &&
                   DecodedPixels(decoder.get(), size, rgba);
        if (!key.starts_with("exe:"))
            return false;
        HICON icon = nullptr;
        if (SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, static_cast<UINT>(size)) != S_OK || !icon)
            return false;
        winrt::com_ptr<IWICBitmap> bitmap;
        const bool copied = SUCCEEDED(l.imaging->CreateBitmapFromHICON(icon, bitmap.put())) && CopyScaled(bitmap.get(), size, rgba);
        DestroyIcon(icon);
        return copied;
    }
};
Pictures pictures;

std::string ExecutablePicture(const fs::path& executable)
{
    return executable.empty() ? std::string() : "exe:" + Utf8(executable.wstring());
}

// A saved game's icon: its executable's, or the one the menu saved in its presets folder while it ran.
std::string GamePicture(const AutoGame& game)
{
    fs::path executable = game.executable;
    if (!executable.has_parent_path())
    {
        const auto located = l.located.find(game.executable.wstring());
        executable = located != l.located.end() ? located->second : fs::path();
    }
    if (!executable.empty() && GetFileAttributesW(executable.c_str()) != INVALID_FILE_ATTRIBUTES)
        return ExecutablePicture(executable);
    const std::wstring folder = FolderName(game.name);
    const fs::path logo = fs::path(ExeDirectory()) / L"presets" / folder / L"logo.png";
    return !folder.empty() && GetFileAttributesW(logo.c_str()) != INVALID_FILE_ATTRIBUTES ? "png:" + Utf8(logo.wstring()) : std::string();
}

// Describing the launcher

launcher::Level LevelOf(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Ok: return launcher::Level::Ok;
    case LogLevel::Warning: return launcher::Level::Warning;
    case LogLevel::Error: return launcher::Level::Error;
    default: return launcher::Level::Info;
    }
}

launcher::Performance PerformanceOf(const std::wstring& game)
{
    launcher::Performance performance{ FrameRateLimit(game), EffectResolution(game) };
    if (DepthEnabled())
        performance.depthSize = DepthSize(game);
    return performance;
}

launcher::Status DescribeStatus()
{
    launcher::Status status;
    l.activeExecutable = g.activeGame ? Executable(g.activeGame->processId) : fs::path{};
    const std::optional<GameWindow>& game = g.activeGame ? g.activeGame : g.selectedGame;
    status.game = game ? Utf8(game->name) : "";
    status.picture = ExecutablePicture(g.activeGame ? l.activeExecutable : game ? Executable(game->processId) : fs::path{});
    status.effectsShown = g.captureEnabled;
    bool windowClosed = false;
    if (!g.captureEnabled)
    {
        status.tone = launcher::Tone::Off;
        status.title = "Effects off";
        status.detail = g.hotkeys.overlay.key ? "Turn them back on below, or press " + Utf8(g.overlayHotkey) + " in the game." : "Turn them back on below.";
    }
    else if (g.target)
    {
        status.tone = launcher::Tone::Running;
        status.title = "Running on " + Utf8(g.activeGame->name);
        status.detail = AddonRegistered() ? "Press " + Utf8(g.inputHotkey) + " in the game to open the menu." : "The menu is off. See the messages below.";
    }
    else if (g.selectedGame)
    {
        windowClosed = !l.shown.selectedOpen;
        status.title = "Waiting for " + Utf8(g.selectedGame->name);
        status.detail = windowClosed ? "Its window closed. Pick its new window." : "Return to the game to see the effects.";
    }
    else
    {
        status.title = "Waiting for a game";
        status.detail = std::any_of(g.autoGames.begin(), g.autoGames.end(), [](const AutoGame& saved) { return saved.enabled; })
                            ? "Open one of your games."
                            : "Open a game and choose Add game.";
    }
    status.canPick = !g.selectedGame || windowClosed;
    status.canDetect = g.selectedGame.has_value();
    return status;
}

launcher::Game DescribeGame(const AutoGame& saved)
{
    const std::wstring folder = FolderName(saved.name);
    launcher::Game game{ .id = Utf8(saved.executable.wstring()),
                         .name = Utf8(saved.name),
                         .executable = Utf8(saved.executable.wstring()),
                         .picture = GamePicture(saved),
                         .enabled = saved.enabled,
                         .running = g.target && MatchesExecutable(saved, l.activeExecutable) };
    // Settings are kept under the folder's name, which a name of only characters a folder cannot have lacks.
    if (folder.empty())
        return game;
    game.performance = PerformanceOf(folder);
    // Its own presets, then those for all games, then the one it uses when that is in another game's folder.
    const GamePresetList presets = GamePresets(folder);
    const auto add = [&](const fs::path& preset, const std::string& from) {
        game.presets.push_back({ Utf8(preset.wstring()), Utf8(preset.stem().wstring()), from });
        l.presets.push_back(preset);
        if (_wcsicmp(preset.c_str(), presets.inUse.c_str()) == 0)
            game.preset = game.presets.back().id;
    };
    for (const fs::path& preset : presets.own)
        add(preset, "");
    for (const fs::path& preset : presets.shared)
        add(preset, "All games");
    if (game.preset.empty() && !presets.inUse.empty())
        add(presets.inUse, Utf8(presets.inUse.parent_path().filename().wstring()));
    return game;
}

void Describe()
{
    launcher::Model model;
    model.version = UNISHADE_VERSION;
    model.status = DescribeStatus();

    l.update = AvailableUpdate();
    if (!l.update.version.empty())
        model.cards.push_back({ .title = "Unishade " + Utf8(l.update.version) + " is available", .attention = true, .buttons = { "Download" } });

    LocateGames();
    l.presets.clear();
    for (const AutoGame& saved : g.autoGames)
        model.games.push_back(DescribeGame(saved));
    if (l.removed)
        model.removed = launcher::Removed{ l.removed->index, Utf8(l.removed->game.name) };

    for (const Notice& notice : Notices())
        model.notices.push_back({ LevelOf(notice.level), Utf8(notice.text) });

    l.links.clear();
    const auto link = [&](const char* label, std::function<void()> open) {
        model.links.push_back(label);
        l.links.push_back(std::move(open));
    };
    link("Open log", [] { ShellOpen(LogPath()); });
    link("Get help on Discord", [] { ShellOpen(kHelpUrl); });
    if (const std::wstring setup = ExeDirectory() + L"Unishade-Setup.exe"; GetFileAttributesW(setup.c_str()) != INVALID_FILE_ATTRIBUTES)
        link("Run Setup", [setup] { ShellOpen(setup); });
    link("Support on Ko-fi", [] { ShellOpen(kSupportUrl); });

    launcher::Settings& settings = model.settings;
    settings.startWithWindows = StartsWithWindows();
    settings.autoSavePresets = AutoSavePresets();
    settings.menuSize = static_cast<int>(std::lround(MenuScale() * 100));
    settings.canShrinkMenu = MenuScale() > kSmallestMenuScale + 0.01f;
    settings.canGrowMenu = MenuScale() < kLargestMenuScale - 0.01f;
    settings.keepEffectsVisible = KeepEffectsVisible();
    settings.updateChecks = UpdateChecksEnabled();
    settings.performance = PerformanceOf({});
    settings.debugInfo = DebugInfoEnabled();
    for (const Shortcut& shortcut : kShortcuts)
    {
        const Hotkey& hotkey = g.hotkeys.*shortcut.member;
        settings.shortcuts.push_back({ shortcut.title, shortcut.description, hotkey.key ? Utf8(FormatHotkey(hotkey)) : "",
                                       shortcut.member == &InputHotkeys::input });
    }
    settings.recording = l.recording;
    settings.shortcutError = Utf8(l.shortcutError);
    for (const std::wstring& warning : ShortcutWarnings())
        settings.shortcutWarnings.push_back(Utf8(warning));

    launcher::Discord discord{ .on = DiscordPresenceEnabled() };
    if (!g.target)
        discord.status = "Shows up once Unishade is running on a game.";
    else
        switch (const DiscordStatus status = CurrentDiscordStatus(); status.state)
        {
        case DiscordState::Idle:
        case DiscordState::Connecting: discord.status = "Connecting to Discord..."; break;
        case DiscordState::Closed: discord.status = "Waiting for Discord to open."; break;
        case DiscordState::Showing:
            discord.status = "Showing " + Utf8(g.activeGame->name) + " on Discord.";
            discord.level = launcher::Level::Ok;
            break;
        case DiscordState::Refused:
            discord.status = "Discord refused it: " + Utf8(status.error);
            discord.level = launcher::Level::Warning;
            break;
        }
    model.discord = std::move(discord);

    if (l.picking)
    {
        launcher::Picker picker{ .add = l.pickToAdd };
        for (const GameWindow& window : l.windows)
        {
            const fs::path executable = Executable(window.processId);
            picker.windows.push_back({ Utf8(window.name), Utf8(executable.filename().wstring()), ExecutablePicture(executable) });
        }
        model.picker = std::move(picker);
    }
    model.renameProblem = Utf8(l.renameProblem);
    model.pictures = l.pictures;
    l.model = std::move(model);
}

Shown CurrentShown()
{
    Shown shown{ .captureEnabled = g.captureEnabled,
                 .target = g.target,
                 .selected = g.selectedGame ? g.selectedGame->window : nullptr,
                 .notices = NoticeVersion(),
                 .update = AvailableUpdateVersion(),
                 .inputKey = g.hotkeys.input.key,
                 .inputModifiers = g.hotkeys.input.modifiers,
                 .overlayKey = g.hotkeys.overlay.key,
                 .overlayModifiers = g.hotkeys.overlay.modifiers,
                 .settings = SettingsVersion(),
                 .discord = DiscordStatusVersion(),
                 .preset = ActivePresetName() };
    // Only shown while waiting for the picked window. The main loop notices the game's window closing.
    if (g.captureEnabled && !g.target && g.selectedGame)
        shown.selectedOpen = GameWindowExists(*g.selectedGame);
    return shown;
}

// Draws again for a few frames, which lets hover and clicks settle.
void Wake()
{
    l.settle = 3;
    if (g.launcher)
        InvalidateRect(g.launcher, nullptr, FALSE);
}

void Refresh()
{
    if (l.drawing)
    {
        l.dirty = true;
        return;
    }
    const Shown shown = CurrentShown();
    // A game that starts or stops may have a new icon, such as one the menu just saved.
    if (shown.target != l.shown.target)
        ++l.pictures;
    l.shown = shown;
    Describe();
    Wake();
}

// What the user does

// Saves a changed game list. A game that was turned off or removed stops being used, unless its window was
// picked for this session. Returns false when it could not be saved.
bool SaveGames(std::vector<AutoGame> games)
{
    try
    {
        SaveAutoGames(ExeDirectory() + L"games.ini", games);
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Could not save the game list: %hs", e.what());
        return false;
    }
    g.autoGames = std::move(games);
    ++l.pictures;
    if (g.target && !g.selectedGame &&
        std::none_of(g.autoGames.begin(), g.autoGames.end(),
                     [](const AutoGame& game) { return game.enabled && MatchesExecutable(game, l.activeExecutable); }))
        StopCapture();
    return true;
}

// Once a removed game can no longer be put back, its preset and performance settings are forgotten, unless a game
// with the same name was added since.
void ForgetRemoved()
{
    if (!l.removed)
        return;
    if (g.launcher)
        KillTimer(g.launcher, kUndoTimer);
    const std::wstring key = FolderName(l.removed->game.name);
    if (!key.empty() && std::none_of(g.autoGames.begin(), g.autoGames.end(),
                                     [&](const AutoGame& game) { return _wcsicmp(FolderName(game.name).c_str(), key.c_str()) == 0; }))
    {
        RemoveGamePreset(key);
        RemovePerformanceGame(key);
    }
    l.removed.reset();
}

void RemoveGame(size_t index)
{
    if (index >= g.autoGames.size())
        return;
    ForgetRemoved();
    auto games = g.autoGames;
    Removed removed{ games[index], index };
    games.erase(games.begin() + static_cast<std::ptrdiff_t>(index));
    if (!SaveGames(std::move(games)))
        return;
    l.removed = std::move(removed);
    SetTimer(g.launcher, kUndoTimer, kUndoMilliseconds, nullptr);
}

void UndoRemove()
{
    if (!l.removed)
        return;
    auto games = g.autoGames;
    // Unless it was added again meanwhile.
    if (std::none_of(games.begin(), games.end(),
                     [](const AutoGame& game) { return _wcsicmp(game.executable.c_str(), l.removed->game.executable.c_str()) == 0; }))
    {
        games.insert(games.begin() + static_cast<std::ptrdiff_t>(std::min(l.removed->index, games.size())), l.removed->game);
        if (!SaveGames(std::move(games)))
            return;
    }
    KillTimer(g.launcher, kUndoTimer);
    l.removed.reset();
}

// Why a game cannot have this name, or nothing. The name also names its presets folder and its entry in
// RobloxShadeHost.ini.
std::wstring NameProblem(const std::wstring& name, size_t index)
{
    switch (CheckName(name, true))
    {
    case NameIssue::None:
        break;
    case NameIssue::Empty:
        return L"Type a name.";
    case NameIssue::Control:
        return L"A name can't contain control characters.";
    case NameIssue::Character:
        return L"A name can't contain \\ / : * ? \" < > | = ; [ or ].";
    case NameIssue::End:
        return L"A name can't end with a dot or a space.";
    case NameIssue::Device:
        return L"Windows keeps " + std::wstring(DeviceName(name)) + L" for devices.";
    }
    for (size_t i = 0; i < g.autoGames.size(); ++i)
        if (i != index && _wcsicmp(FolderName(g.autoGames[i].name).c_str(), name.c_str()) == 0)
            return L"Another game has this name.";
    return {};
}

// Renames a saved game, with its presets folder, its remembered preset and its performance settings. Returns why it
// could not, or nothing.
std::wstring RenameGame(size_t index, std::wstring name)
{
    if (index >= g.autoGames.size())
        return {};
    // Spaces around it would not survive as a folder name or an ini key.
    name.erase(0, name.find_first_not_of(L' '));
    name.erase(name.find_last_not_of(L' ') + 1);
    if (g.autoGames[index].name == name)
        return {};
    if (std::wstring problem = NameProblem(name, index); !problem.empty())
        return problem;
    const std::wstring previous = g.autoGames[index].name;
    const std::wstring key = FolderName(previous);
    // Windows and RobloxShadeHost.ini ignore case, so a name that only changes case keeps both.
    const bool sameKey = _wcsicmp(key.c_str(), name.c_str()) == 0;
    const fs::path root = fs::path(ExeDirectory()) / L"presets";
    std::error_code error;
    const bool moveFolder = !sameKey && !key.empty() && fs::is_directory(root / key, error) && !fs::exists(root / name, error);
    if (moveFolder)
    {
        fs::rename(root / key, root / name, error);
        if (error)
        {
            Log(LogLevel::Warning, L"Could not rename %ls: %hs", (root / key).c_str(), error.message().c_str());
            return L"Could not rename its presets folder.";
        }
    }
    auto games = g.autoGames;
    games[index].name = name;
    if (!SaveGames(std::move(games)))
    {
        if (moveFolder)
            fs::rename(root / name, root / key, error);
        return L"Could not save the game list.";
    }
    if (std::wstring preset = sameKey ? L"" : GamePreset(key); !preset.empty())
    {
        // A preset in the game's folder moved with it.
        if (moveFolder && preset.size() > key.size() && _wcsnicmp(preset.c_str(), key.c_str(), key.size()) == 0 &&
            (preset[key.size()] == L'\\' || preset[key.size()] == L'/'))
            preset = name + preset.substr(key.size());
        SetGamePreset(name, preset);
        RemoveGamePreset(key);
    }
    RenamePerformanceGame(key, name);
    // The game being played shows the new name. A picked window keeps its title.
    if (g.target && g.activeGame && !g.selectedGame && MatchesExecutable(g.autoGames[index], l.activeExecutable))
        g.activeGame->name = name;
    Log(LogLevel::Info, L"Renamed %ls to %ls.", previous.c_str(), name.c_str());
    if (moveFolder)
        RequestPresetScan();
    return {};
}

// Lists the open windows for the picker. Returns whether they changed.
bool ListWindows()
{
    std::vector<GameWindow> windows = ListGameWindows();
    if (std::equal(windows.begin(), windows.end(), l.windows.begin(), l.windows.end(),
                   [](const GameWindow& a, const GameWindow& b) { return a.window == b.window && a.name == b.name; }))
        return false;
    l.windows = std::move(windows);
    return true;
}

void UseWindow(bool add, const GameWindow& window)
{
    if (add)
    {
        auto games = g.autoGames;
        try
        {
            AddAutoGame(games, window);
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Error, L"Could not add %ls: %hs", window.name.c_str(), e.what());
            return;
        }
        // Detection follows the game in front, which is the one just added.
        g.selectedGame.reset();
        SaveGames(std::move(games));
    }
    else
    {
        if (g.target)
            StopCapture();
        g.selectedGame = window;
    }
    if (GameWindowExists(window))
    {
        if (IsIconic(window.window))
            ShowWindow(window.window, SW_RESTORE);
        SetForegroundWindow(window.window);
    }
}

// Stops waiting for a shortcut's keys.
void StopShortcut()
{
    if (l.recording < 0)
        return;
    l.recording = -1;
    SuspendHotkeys(false);
}

// While a shortcut waits for its keys, the next key pressed with any modifiers becomes it.
void TakeShortcut(UINT key)
{
    // Modifiers on their own.
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN)
        return;
    Hotkey hotkey;
    if (GetKeyState(VK_CONTROL) < 0)
        hotkey.modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0)
        hotkey.modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0)
        hotkey.modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
        hotkey.modifiers |= MOD_WIN;
    hotkey.key = key;
    if (key == VK_ESCAPE && hotkey.modifiers == MOD_NOREPEAT)
        StopShortcut();
    else if (FormatHotkey(hotkey).empty())
        l.shortcutError = UnusableKeyText();
    else
    {
        InputHotkeys hotkeys = g.hotkeys;
        hotkeys.*kShortcuts[l.recording].member = hotkey;
        StopShortcut();
        l.shortcutError = ChangeHotkeys(hotkeys);
    }
    Refresh();
}

class Host final : public launcher::Host
{
public:
    void SetSwitch(launcher::Switch which, bool on) override
    {
        switch (which)
        {
        case launcher::Switch::EffectsShown:
            if (g.captureEnabled != on)
                ToggleOverlay();
            break;
        case launcher::Switch::StartWithWindows: SetStartWithWindows(on); break;
        case launcher::Switch::AutoSavePresets:
            SetAutoSavePresets(on);
            // From here on every change saves as it happens, so changes that were waiting are saved too.
            if (on)
                FlushPresets(true);
            break;
        case launcher::Switch::KeepEffectsVisible: SetKeepEffectsVisible(on); break;
        case launcher::Switch::UpdateChecks: SetUpdateChecksEnabled(on); break;
        case launcher::Switch::DebugInfo: SetDebugInfoEnabled(on); break;
        case launcher::Switch::DiscordPresence: SetDiscordPresenceEnabled(on); break;
        }
        l.dirty = true;
    }

    void PressCard(size_t, size_t) override
    {
        // The only card is the update's.
        if (!l.update.url.empty())
            ShellOpen(l.update.url);
    }

    void OpenLink(size_t link) override
    {
        if (link < l.links.size())
            l.links[link]();
    }

    void OpenUrl(std::string_view url) override
    {
        ShellOpen(Wide(url));
    }

    void DismissNotices() override
    {
        ClearNotices();
        l.dirty = true;
    }

    void OpenPicker(bool add) override
    {
        l.picking = true;
        l.pickToAdd = add;
        l.windows.clear();
        ListWindows();
        l.dirty = true;
    }

    void ChooseWindow(size_t window) override
    {
        if (!l.picking || window >= l.windows.size())
            return;
        l.picking = false;
        UseWindow(l.pickToAdd, l.windows[window]);
        l.dirty = true;
    }

    void ClosePicker() override
    {
        l.picking = false;
        l.dirty = true;
    }

    void DetectAutomatically() override
    {
        g.selectedGame.reset();
        if (g.target)
            StopCapture();
        l.dirty = true;
    }

    void SetGameEnabled(size_t game, bool enabled) override
    {
        if (game >= g.autoGames.size())
            return;
        auto games = g.autoGames;
        games[game].enabled = enabled;
        SaveGames(std::move(games));
        l.dirty = true;
    }

    bool RenameGame(size_t game, std::string_view name) override
    {
        l.renameProblem = ::RenameGame(game, Wide(name));
        l.dirty = true;
        return l.renameProblem.empty();
    }

    void RemoveGame(size_t game) override
    {
        ::RemoveGame(game);
        l.dirty = true;
    }

    void UndoRemove() override
    {
        ::UndoRemove();
        l.dirty = true;
    }

    void UsePreset(size_t game, std::string_view preset) override
    {
        if (game >= g.autoGames.size())
            return;
        UseGamePreset(FolderName(g.autoGames[game].name), fs::path(Wide(preset)));
        l.dirty = true;
    }

    void OpenPresetsFolder(size_t game) override
    {
        if (game >= g.autoGames.size())
            return;
        const fs::path folder = fs::path(ExeDirectory()) / L"presets" / FolderName(g.autoGames[game].name);
        std::error_code error;
        fs::create_directories(folder, error);
        ShellOpen(folder.wstring());
    }

    void SetPerformance(std::optional<size_t> game, launcher::PerformanceSetting setting, int value) override
    {
        if (game && *game >= g.autoGames.size())
            return;
        const std::wstring key = game ? FolderName(g.autoGames[*game].name) : std::wstring();
        // An empty name would change the defaults.
        if (game && key.empty())
            return;
        switch (setting)
        {
        case launcher::PerformanceSetting::FrameRate: SetFrameRateLimit(key, value); break;
        case launcher::PerformanceSetting::EffectResolution: SetEffectResolution(key, value); break;
        case launcher::PerformanceSetting::DepthSize: SetDepthSize(key, value); break;
        }
        l.dirty = true;
    }

    void StepMenuSize(int step) override
    {
        SetMenuScale(std::round((MenuScale() + 0.25f * static_cast<float>(step)) * 4) / 4);
        l.dirty = true;
    }

    void RecordShortcut(int shortcut) override
    {
        StopShortcut();
        if (shortcut >= 0 && static_cast<size_t>(shortcut) < std::size(kShortcuts))
        {
            l.recording = shortcut;
            l.shortcutError.clear();
            SuspendHotkeys(true);
        }
        l.dirty = true;
    }

    void ClearShortcut(size_t shortcut) override
    {
        if (shortcut >= std::size(kShortcuts))
            return;
        InputHotkeys hotkeys = g.hotkeys;
        hotkeys.*kShortcuts[shortcut].member = {};
        l.shortcutError = ChangeHotkeys(hotkeys);
        l.dirty = true;
    }

    void ResetShortcuts() override
    {
        l.shortcutError = ChangeHotkeys(DefaultHotkeys());
        l.dirty = true;
    }
};
Host host;

// The window

void Paint(HWND window)
{
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    unsigned next = 0;
    if (width > 0 && height > 0)
    {
        l.pixels.resize(static_cast<size_t>(width) * height);
        const float scale = static_cast<float>(GetDpiForWindow(window)) / USER_DEFAULT_SCREEN_DPI;
        l.drawing = true;
        next = l.ui->Draw(l.model, host, pictures, scale, l.pixels.data(), width, height);
        l.drawing = false;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
        SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(width), static_cast<DWORD>(height), 0, 0, 0, static_cast<UINT>(height), l.pixels.data(), &bitmap,
                          DIB_RGB_COLORS);
    }
    EndPaint(window, &paint);

    // What the user did shows in the next frame.
    if (std::exchange(l.dirty, false))
        Refresh();
    else if (l.settle > 0)
    {
        --l.settle;
        next = next ? std::min(next, 16u) : 16;
    }
    if (next)
        SetTimer(window, kFrameTimer, next, nullptr);
    else
        KillTimer(window, kFrameTimer);
}

NOTIFYICONDATAW TrayData()
{
    NOTIFYICONDATAW data{ sizeof(data) };
    data.hWnd = g.launcher;
    data.uID = kTrayIcon;
    return data;
}

// Shows Unishade in the notification area while it runs. Tried again later when the notification area is not
// ready, such as right after signing in.
void AddTrayIcon()
{
    if (!l.trayIcon)
        l.trayIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    NOTIFYICONDATAW data = TrayData();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = l.trayIcon;
    wcscpy_s(data.szTip, std::size(data.szTip), L"Unishade");
    // TaskbarCreated also comes when the taskbar's scaling changes, while the icon is still there.
    Shell_NotifyIconW(NIM_DELETE, &data);
    l.trayAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (l.trayAdded)
    {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    else
        SetTimer(g.launcher, kTrayTimer, 5000, nullptr);
}

void RemoveTrayIcon()
{
    if (l.trayAdded)
    {
        NOTIFYICONDATAW data = TrayData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        l.trayAdded = false;
    }
    if (l.trayIcon)
        DestroyIcon(l.trayIcon);
    l.trayIcon = nullptr;
}

// Brings the launcher back from the notification area or the taskbar, maximized if it was.
void ShowLauncher()
{
    if (IsIconic(g.launcher))
        ShowWindow(g.launcher, SW_RESTORE);
    else if (!IsWindowVisible(g.launcher))
        ShowWindow(g.launcher, SW_SHOW);
    SetForegroundWindow(g.launcher);
}

void SavePlace()
{
    WINDOWPLACEMENT placement{ sizeof(placement) };
    if (GetWindowPlacement(g.launcher, &placement))
        SaveLauncherPlace({ placement.rcNormalPosition, placement.showCmd == SW_SHOWMAXIMIZED ||
                                                            (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED)) });
}

// Asks about preset changes that are not saved before Unishade quits, since they are lost otherwise. Returns false
// when the user wants to stay.
bool ConfirmQuit()
{
    if (!MenuHasUnsavedChanges())
    {
        FlushPresets();
        return true;
    }
    const std::wstring name = ActivePresetName();
    const std::wstring question = L"Save the changes to " + (name.empty() ? std::wstring(L"the preset") : name) + L" before quitting?";
    switch (MessageBoxW(g.launcher, question.c_str(), L"Unishade", MB_YESNOCANCEL | MB_ICONWARNING))
    {
    case IDYES:
        FlushPresets(true);
        return true;
    case IDNO:
        FlushPresets();
        return true;
    default:
        return false;
    }
}

void TrayMenu(int x, int y)
{
    const HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, kOpenCommand, L"Open Unishade");
    AppendMenuW(menu, MF_STRING | (g.captureEnabled ? MF_CHECKED : MF_UNCHECKED), kEffectsCommand, L"Show effects");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit");
    SetMenuDefaultItem(menu, kOpenCommand, FALSE);
    // Otherwise the menu stays open when clicking elsewhere.
    SetForegroundWindow(g.launcher);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command =
        static_cast<UINT>(TrackPopupMenuEx(menu, align | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, g.launcher, nullptr));
    PostMessageW(g.launcher, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command)
    {
    case kOpenCommand:
        ShowLauncher();
        break;
    case kEffectsCommand:
        ToggleOverlay();
        break;
    case kQuitCommand:
        if (ConfirmQuit())
            DestroyWindow(g.launcher);
        break;
    }
}

// Mouse and keyboard input, after which the window draws again.
bool Input(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_MOUSELEAVE ||
           message == WM_SETFOCUS || message == WM_KILLFOCUS;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == l.taskbarCreated && l.taskbarCreated)
    {
        AddTrayIcon();
        return 0;
    }
    // While a shortcut waits for its keys, they go to it. A held key repeats, which is not a new shortcut.
    if (l.recording >= 0)
    {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        {
            if (!(lParam & (1 << 30)))
                TakeShortcut(static_cast<UINT>(wParam));
            return 0;
        }
        if (message == WM_CHAR || message == WM_SYSCHAR)
            return 0;
        // Alt would open the window's menu.
        if (message == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
    }
    if (l.ui && l.ui->Message(hwnd, message, wParam, lParam))
        return TRUE;
    if (Input(message))
        Wake();

    switch (message)
    {
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == kFrameTimer)
        {
            KillTimer(hwnd, kFrameTimer);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kUndoTimer)
        {
            ForgetRemoved();
            Refresh();
        }
        else if (wParam == kTrayTimer)
        {
            KillTimer(hwnd, kTrayTimer);
            AddTrayIcon();
        }
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            Wake();
        return 0;
    case WM_GETMINMAXINFO:
    {
        const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / USER_DEFAULT_SCREEN_DPI;
        RECT frame{ 0, 0, static_cast<LONG>(std::lround(kMinWidth * scale)), static_cast<LONG>(std::lround(kMinHeight * scale)) };
        AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, 0, GetDpiForWindow(hwnd));
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = { frame.right - frame.left, frame.bottom - frame.top };
        return 0;
    }
    // Moved to a monitor with another scaling.
    case WM_DPICHANGED:
    {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Wake();
        return 0;
    }
    case WM_SHOWWINDOW:
        if (wParam)
            Refresh();
        break;
    // A game opened while the picker was open shows up when the launcher is activated again.
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && l.picking && ListWindows())
            Refresh();
        break;
    case WM_KILLFOCUS:
        if (l.recording >= 0)
        {
            StopShortcut();
            Refresh();
        }
        break;
    // Closing keeps Unishade running in the notification area, where Quit ends it. Without a notification area, it
    // quits.
    case WM_CLOSE:
        if (l.trayAdded)
        {
            SavePlace();
            StopShortcut();
            l.picking = false;
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (!ConfirmQuit())
            return 0;
        break;
    case kTrayMessage:
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowLauncher();
            break;
        case WM_CONTEXTMENU:
            TrayMenu(static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam)));
            break;
        }
        return 0;
    case WM_DESTROY:
        SavePlace();
        RemoveTrayIcon();
        g.launcher = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// The dark title bar, in the sidebar's color.
void DarkFrame(HWND window)
{
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = RGB((theme::kSidebar >> 16) & 0xFF, (theme::kSidebar >> 8) & 0xFF, theme::kSidebar & 0xFF);
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
}

// Where the window was last time, unless that is off every screen now. Otherwise centered on the main screen.
void PlaceWindow(int show)
{
    WINDOWPLACEMENT placement{ sizeof(placement) };
    GetWindowPlacement(g.launcher, &placement);
    placement.showCmd = static_cast<UINT>(show);
    const std::optional<LauncherPlace> saved = LoadLauncherPlace();
    if (saved && MonitorFromRect(&saved->rect, MONITOR_DEFAULTTONULL))
    {
        placement.rcNormalPosition = saved->rect;
        if (saved->maximized && show == SW_SHOWNORMAL)
            placement.showCmd = SW_SHOWMAXIMIZED;
        else if (saved->maximized)
            placement.flags |= WPF_RESTORETOMAXIMIZED;
    }
    else
    {
        // The window opened on the main screen, so it has that screen's scaling.
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const int dpi = static_cast<int>(GetDpiForWindow(g.launcher));
        const RECT& work = monitor.rcWork;
        const int width = std::min<int>(MulDiv(kWidth, dpi, USER_DEFAULT_SCREEN_DPI), work.right - work.left);
        const int height = std::min<int>(MulDiv(kHeight, dpi, USER_DEFAULT_SCREEN_DPI), work.bottom - work.top);
        // The window placement counts from the top left of the main screen's work area.
        const int left = (work.right - work.left - width) / 2;
        const int top = (work.bottom - work.top - height) / 2;
        placement.rcNormalPosition = { left, top, left + width, top + height };
    }
    SetWindowPlacement(g.launcher, &placement);
}
} // namespace

void CreateLauncher()
{
    l.module = LoadLibraryExW((ExeDirectory() + kUiModule).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const auto create = l.module ? reinterpret_cast<LauncherUi* (*)()>(GetProcAddress(l.module, "UnishadeLauncherUi")) : nullptr;
    l.ui = create ? create() : nullptr;
    if (!l.ui)
    {
        Log(LogLevel::Error, L"Could not load %ls (error %lu). Run Setup again to repair Unishade.", kUiModule, GetLastError());
        throw std::runtime_error("Could not load UnishadeUi.dll");
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(l.imaging.put()))))
        Log(LogLevel::Warning, L"The launcher could not load pictures, so games show their first letter.");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kLauncherClass;
    RegisterClassExW(&wc);
    l.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g.launcher = CreateWindowExW(0, kLauncherClass, L"Unishade", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    winrt::check_bool(g.launcher != nullptr);
    DarkFrame(g.launcher);
    // Explorer runs without administrator rights, so Unishade running with them must accept its messages.
    ChangeWindowMessageFilterEx(g.launcher, l.taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g.launcher, kTrayMessage, MSGFLT_ALLOW, nullptr);
    AddTrayIcon();
    if (!l.ui->Init(g.launcher))
        throw std::runtime_error("Could not start the launcher's window");
    Refresh();

    // Started with Windows, it waits in the notification area, or on the taskbar until there is one.
    if (!StartedWithWindows())
        PlaceWindow(SW_SHOWNORMAL);
    else if (!l.trayAdded)
        PlaceWindow(SW_SHOWMINNOACTIVE);
    else
        PlaceWindow(SW_HIDE);
}

void UpdateLauncher()
{
    // Nothing is described or drawn while the launcher is hidden or minimized. Showing it describes it again.
    if (!g.launcher || !IsWindowVisible(g.launcher) || IsIconic(g.launcher))
        return;
    // The presets folder is read now and then while the games' pages may show it, and only presets that came or went
    // describe the launcher again.
    const ULONGLONG now = GetTickCount64();
    if (now >= l.nextPresetScan)
    {
        l.nextPresetScan = now + kPresetScanInterval;
        RequestPresetScan();
    }
    if (CurrentShown() != l.shown)
    {
        Refresh();
        return;
    }
    if (const unsigned version = PresetScanVersion(); version != l.presetScan)
    {
        l.presetScan = version;
        const std::vector<fs::path> before = l.presets;
        Describe();
        if (l.presets != before)
            Wake();
    }
}

void DestroyLauncher()
{
    ForgetRemoved();
    if (g.launcher)
        DestroyWindow(g.launcher);
    g.launcher = nullptr;
    if (l.ui)
        l.ui->Shutdown();
    l.ui = nullptr;
    l.imaging = nullptr;
    // The DLL stays loaded until the process exits, which also keeps its C runtime from shutting down early.
}
