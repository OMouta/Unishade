// GDI+ comes first: its headers name parameters g, like the host's global state.
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus
{
// GDI+ uses min and max, which NOMINMAX removes.
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

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

#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Size in pixels at 100% scaling. Longer content makes the window taller, up to the screen, and then scrolls.
// The height fits every page of Settings, so the window keeps its size from tab to tab.
constexpr float kWidth = 620;
constexpr float kMinHeight = 672;
// The picker scrolls past its height.
constexpr float kPickerWidth = 480;
constexpr float kPickerMaxHeight = 520;
constexpr wchar_t kPickerClass[] = L"UnishadePicker";
// How long a removed game can be put back.
constexpr UINT kUndoMilliseconds = 8000;

// The launcher window's timers.
enum Timer : UINT_PTR
{
    kUndoTimer = 1,
    kTipTimer,
    kTrayTimer,
};

// Sent to the launcher by its notification area icon.
constexpr UINT kTrayMessage = WM_APP + 16;
constexpr UINT kTrayIcon = 1;
// Posted by the rename box or the frame rate box when it is done, so it is not destroyed while it handles a message.
// lParam is the box.
constexpr UINT kRenameMessage = WM_APP + 17;
// Longer names would not fit in the list.
constexpr WPARAM kMaxNameLength = 100;

enum RenameEnd : WPARAM
{
    kSaveName,   // Enter, which keeps the box open when the name is not allowed
    kLeaveName,  // leaving the box, which drops a name that is not allowed
    kCancelName, // Escape
};

enum class Tab
{
    Games,
    Settings,
    Discord,
};

// The pages of Settings, which are the menu's.
enum class Page
{
    General,
    Performance,
    Shortcuts,
};

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

constexpr wchar_t kTrayNote[] = L"Minimize to keep the effects running from the notification area. Closing Unishade turns them off.";

enum class Action
{
    OpenLog,
    Help,
    Support,
    RunSetup,
    Download,
    AddGame,
    PickWindow,
    DetectAutomatically,
    ToggleGame,
    RenameGame,
    RemoveGame,
    UndoRemove,
    DismissNotices,
    OpenUrl,
    ShowTab,
    ShowPage,
    StartWithWindows,
    AutoSave,
    MenuSize,
    KeepEffects,
    UpdateChecks,
    FrameRate,
    EffectResolution,
    DepthDetail,
    DebugInfo,
    SetShortcut,
    ClearShortcut,
    ResetShortcuts,
    DiscordPresence,
};

enum class Look
{
    Link,
    Button,
    PrimaryButton,
    Switch,
    Tab,
    Segment, // one choice of several side by side
    Rename,
    Remove,
};

// Something that can be clicked. index is the game, window, tab, choice or shortcut it acts on.
struct Target
{
    RECT rect;
    Action action;
    Look look;
    size_t index = 0;
    std::wstring label;
    bool on = false;
    const wchar_t* tip = nullptr; // for buttons that are only an icon
    bool enabled = true;
};

// Text that is not a control, such as a setting's title.
struct Label
{
    RECT rect;
    HFONT font;
    unsigned color;
    std::wstring text;
    UINT format = DT_WORDBREAK;
};

// Which control a target is. The targets are made again with every layout, so they are told apart by this.
struct Control
{
    Action action;
    size_t index = 0;

    bool operator==(const Control&) const = default;
};

// A saved game, or an open window in the picker.
struct Entry
{
    std::wstring name;
    std::wstring detail;
    fs::path icon; // the executable, or empty to show the name's first letter
    bool enabled = true;
    bool running = false;
    size_t game = 0;      // its index in g.autoGames
    bool removed = false; // until it can no longer be put back
    bool renaming = false;
    RECT rect{};
    RECT iconRect{};
    RECT nameRect{};
    RECT detailRect{};
    RECT badge{};
};

// A word of a notice, placed where DrawText's word wrapping would put it.
struct Word
{
    RECT rect;
    std::wstring text;
};

struct Row
{
    RECT rect;
    LogLevel level;
    std::vector<Word> words; // without the web addresses, which are links
};

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

    bool operator==(const Shown&) const = default;
};

// A removed game, which can be put back for a few seconds.
struct Removed
{
    AutoGame game;
    size_t index = 0; // where it was in the list
};

// Picking a window either saves its game or uses the window until Detect automatically.
enum class Picker
{
    None,
    Add,
    Session,
};

// A window's scaling and its fonts at that scaling.
struct Scaling
{
    float scale = 1;
    HFONT title = nullptr;
    HFONT semibold = nullptr;
    HFONT strong = nullptr;
    HFONT body = nullptr;
    HFONT note = nullptr;
};

struct Launcher
{
    ULONG_PTR gdiplus = 0;
    winrt::com_ptr<IStream> logoStream;
    std::unique_ptr<Gdiplus::Bitmap> logo;
    // The picker has its own, since it can be on a monitor with another scaling.
    Scaling launcherScaling;
    Scaling pickerScaling;
    // By pixel size and executable. nullptr when the executable has no icon.
    std::map<std::wstring, HICON> icons;
    // Where games saved by filename were found running.
    std::map<std::wstring, fs::path> located;

    Shown shown;
    Tab tab = Tab::Games;
    Page page = Page::General;
    bool minimized = false;
    std::wstring statusTitle;
    std::wstring statusDetail;
    std::wstring statusName;
    fs::path statusIcon;
    unsigned statusColor = 0;
    bool windowClosed = false; // the picked window, while waiting for it
    fs::path activeExecutable;
    Update update;
    std::wstring setup;
    bool startWithWindows = false;
    std::vector<Entry> entries;

    // Layout in client pixels, moved up by scroll.
    int height = 0; // of everything, unscrolled
    int scroll = 0;
    RECT logoRect{};
    int tabLine = 0; // under the tabs
    RECT statusCard{};
    RECT statusIconRect{};
    RECT statusTitleRect{};
    RECT statusDetailRect{};
    RECT updateCard{};
    RECT updateText{};
    RECT gamesTitle{};
    RECT list{};
    RECT noticesTitle{};
    std::vector<Row> rows;
    std::vector<Label> labels;
    std::vector<RECT> groups; // behind each row of segments
    int footer = 0;
    RECT trayNote{};
    std::vector<Target> targets;
    int hovered = -1;
    bool tracking = false;
    // A click only acts when it is let go over the control it was pressed on.
    std::optional<Control> pressed;
    // Whose tip shows, after the mouse rests on it.
    std::optional<Control> tip;
    // Like in Windows, the focus only shows once the keyboard is used.
    std::optional<Control> focus;
    bool focusShown = false;
    std::optional<Removed> removed;

    // The picker, a dialog over the launcher listing the open windows.
    HWND pickerWindow = nullptr;
    Picker picker = Picker::None;
    std::vector<GameWindow> windows;
    std::vector<Entry> choices;
    RECT pickerDetail{};
    RECT pickerList{};
    int pickerHeight = 0; // of everything, unscrolled
    int pickerScroll = 0;
    int pickerHovered = -1;
    bool pickerTracking = false;
    HWND pickerPressed = nullptr; // the window whose row the click was pressed on
    HWND pickerFocus = nullptr;   // the window whose row has the keyboard focus

    // Renaming a game in a box over its name. The game is told by its executable, which a rename keeps.
    HWND edit = nullptr;
    WNDPROC editProc = nullptr;
    fs::path renaming;
    std::wstring renameProblem;
    RECT editBox{}; // drawn around the edit control
    RECT editRect{};
    HBRUSH editBrush = nullptr;

    // The shortcut waiting for its keys, by its index in kShortcuts, or -1.
    int capturing = -1;
    std::wstring shortcutError;
    // A custom frame rate is typed in a box, which is there while the limit is a custom one or one is being typed.
    HWND rate = nullptr;
    RECT rateBox{};
    RECT rateRect{};

    // Explorer forgets the notification area icon when it restarts, and then sends TaskbarCreated.
    UINT taskbarCreated = 0;
    HICON trayIcon = nullptr;
    bool trayAdded = false;
};
Launcher l;
// The scaling of the window being laid out or painted, which P, F and the fonts follow.
Scaling* ui = &l.launcherScaling;

// Uses the picker's scaling until the end of the scope.
struct PickerScaling
{
    Scaling* previous = ui;
    PickerScaling()
    {
        ui = &l.pickerScaling;
    }
    ~PickerScaling()
    {
        ui = previous;
    }
};

// Pixels at the window's scaling, whole for GDI and fractional for GDI+.
int P(float value)
{
    return static_cast<int>(std::lround(value * ui->scale));
}

float F(float value)
{
    return value * ui->scale;
}

float F(LONG pixels)
{
    return static_cast<float>(pixels);
}

COLORREF Gdi(unsigned rgb)
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

Gdiplus::Color Plus(unsigned rgb, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

HFONT Font(const Scaling& scaling, float size, bool semibold)
{
    return CreateFontW(-static_cast<int>(std::lround(size * scaling.scale)), 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       semibold ? L"Segoe UI Semibold" : L"Segoe UI");
}

void DeleteFonts(Scaling& scaling)
{
    for (HFONT* font : { &scaling.title, &scaling.semibold, &scaling.strong, &scaling.body, &scaling.note })
    {
        if (*font)
            DeleteObject(*font);
        *font = nullptr;
    }
}

// Sets a window's scaling from its DPI and creates its fonts at it.
void Scale(Scaling& scaling, UINT dpi)
{
    DeleteFonts(scaling);
    scaling.scale = static_cast<float>(dpi) / 96.0f;
    scaling.title = Font(scaling, 19, true);
    scaling.semibold = Font(scaling, 15, true);
    scaling.strong = Font(scaling, 13.5f, true);
    scaling.body = Font(scaling, 13.5f, false);
    scaling.note = Font(scaling, 12, false);
}

int TextHeight(HDC dc, HFONT font, const std::wstring& text, int width)
{
    RECT rect{ 0, 0, width, 0 };
    const HGDIOBJ previous = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, previous);
    return rect.bottom;
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text)
{
    RECT rect{};
    const HGDIOBJ previous = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
    return rect.right;
}

// The executable's icon at a size in pixels. nullptr when it has none.
HICON ExecutableIcon(const fs::path& executable, int size)
{
    if (executable.empty())
        return nullptr;
    const std::wstring key = std::to_wstring(size) + L"|" + executable.wstring();
    if (const auto found = l.icons.find(key); found != l.icons.end())
        return found->second;
    HICON icon = nullptr;
    if (SHDefExtractIconW(executable.c_str(), 0, 0, &icon, nullptr, static_cast<UINT>(size)) != S_OK)
        icon = nullptr;
    l.icons[key] = icon;
    return icon;
}

void ClearIcons()
{
    for (const auto& [key, icon] : l.icons)
        if (icon)
            DestroyIcon(icon);
    l.icons.clear();
}

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
    const auto studio = std::find_if(g.autoGames.begin(), g.autoGames.end(), [](const AutoGame& game) {
        return _wcsicmp(game.executable.filename().c_str(), L"RobloxStudioBeta.exe") == 0;
    });
    if (studio != g.autoGames.end() && !l.located.contains(studio->executable.wstring()))
        if (const fs::path executable = InstalledStudioExecutable(); !executable.empty())
            l.located[studio->executable.wstring()] = executable;
    const auto unlocated = [](const AutoGame& game) {
        return !game.executable.has_parent_path() && !l.located.contains(game.executable.wstring());
    };
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

void Describe()
{
    l.activeExecutable = g.activeGame ? Executable(g.activeGame->processId) : fs::path{};
    const std::optional<GameWindow>& game = g.activeGame ? g.activeGame : g.selectedGame;
    l.statusName = game ? game->name : L"";
    l.statusIcon = g.activeGame ? l.activeExecutable : game ? Executable(game->processId) : fs::path{};
    l.windowClosed = false;
    if (!g.captureEnabled)
    {
        l.statusTitle = L"Overlay off";
        l.statusDetail = g.hotkeys.overlay.key ? L"Press " + g.overlayHotkey + L" in the game to turn it back on." : L"";
        l.statusColor = theme::kDim;
    }
    else if (g.target)
    {
        l.statusTitle = L"Running on " + g.activeGame->name;
        l.statusDetail = AddonRegistered() ? L"Press " + g.inputHotkey + L" in the game to open the menu." : L"The menu is off. See below.";
        l.statusColor = theme::kSuccess;
    }
    else if (g.selectedGame)
    {
        l.windowClosed = !l.shown.selectedOpen;
        l.statusTitle = L"Waiting for " + g.selectedGame->name;
        l.statusDetail = l.windowClosed ? L"Its window closed. Pick its new window." : L"Return to the game to see the effects.";
        l.statusColor = theme::kAccent;
    }
    else
    {
        l.statusTitle = L"Waiting for a game";
        l.statusDetail = std::any_of(g.autoGames.begin(), g.autoGames.end(), [](const AutoGame& saved) { return saved.enabled; })
                             ? L"Open one of your games."
                             : L"Open a game and choose Add game.";
        l.statusColor = theme::kAccent;
    }

    l.entries.clear();
    LocateGames();
    // A removed game keeps its row while it can be put back, so a second click does not land on the next game.
    const auto removed = [](size_t index) {
        if (l.removed && l.removed->index == index)
            l.entries.push_back({ .name = l.removed->game.name, .removed = true });
    };
    for (size_t i = 0; i < g.autoGames.size(); ++i)
    {
        removed(i);
        const AutoGame& saved = g.autoGames[i];
        const auto located = l.located.find(saved.executable.wstring());
        l.entries.push_back({ .name = saved.name,
                              .detail = saved.executable.wstring(),
                              .icon = saved.executable.has_parent_path() ? saved.executable
                                    : located != l.located.end()        ? located->second
                                                                        : fs::path{},
                              .enabled = saved.enabled,
                              .running = g.target && MatchesExecutable(saved, l.activeExecutable),
                              .game = i,
                              .renaming = l.edit && saved.executable == l.renaming });
    }
    if (l.removed && l.removed->index >= g.autoGames.size())
        removed(l.removed->index);

    l.update = AvailableUpdate();
    l.startWithWindows = StartsWithWindows();
    const std::wstring setup = ExeDirectory() + L"Unishade-Setup.exe";
    l.setup = GetFileAttributesW(setup.c_str()) != INVALID_FILE_ATTRIBUTES ? setup : L"";
}

// Places a notice's words one by one, so the web addresses in it can be links. links counts the links so far.
// Returns the bottom of the last line.
int PlaceWords(HDC dc, Row& row, const std::wstring& text, int left, int top, int right, size_t& links)
{
    const HGDIOBJ previous = SelectObject(dc, ui->body);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const auto width = [dc](const std::wstring& part) {
        SIZE size{};
        GetTextExtentPoint32W(dc, part.c_str(), static_cast<int>(part.size()), &size);
        return static_cast<int>(size.cx);
    };
    const int space = width(L" ");
    const int line = metrics.tmHeight;
    int x = left;
    int y = top;
    for (size_t start = 0; start < text.size();)
    {
        const size_t end = std::min(text.find_first_of(L" \n", start), text.size());
        if (std::wstring word = text.substr(start, end - start); !word.empty())
        {
            const std::wstring link = word.substr(0, WebAddressLength(word));
            word.erase(0, link.size());
            const int linkWidth = link.empty() ? 0 : width(link);
            const int wordWidth = linkWidth + (word.empty() ? 0 : width(word));
            if (x > left && x + wordWidth > right)
            {
                x = left;
                y += line;
            }
            if (!link.empty())
                l.targets.push_back({ { x, y, std::min(x + linkWidth, right), y + line }, Action::OpenUrl, Look::Link, links++, link });
            if (!word.empty())
                row.words.push_back({ { x + linkWidth, y, std::min(x + wordWidth, right), y + line }, word });
            x += wordWidth + space;
        }
        if (end < text.size() && text[end] == L'\n')
        {
            x = left;
            y += line;
        }
        start = end + 1;
    }
    SelectObject(dc, previous);
    return y + line;
}

int ButtonWidth(HDC dc, HFONT font, const wchar_t* label)
{
    return TextWidth(dc, font, label) + P(28);
}

// Places an entry's icon, name and detail in a row, with the text ending at textRight.
void PlaceEntry(Entry& entry, const RECT& row, int iconSize, int textRight)
{
    const int middle = (row.top + row.bottom) / 2;
    entry.rect = row;
    entry.iconRect = { row.left + P(14), middle - iconSize / 2, row.left + P(14) + iconSize, middle - iconSize / 2 + iconSize };
    const int nameLeft = entry.iconRect.right + P(12);
    entry.nameRect = entry.detail.empty() ? RECT{ nameLeft, row.top, textRight, row.bottom } : RECT{ nameLeft, middle - P(20), textRight, middle };
    entry.detailRect = { nameLeft, middle + P(1), textRight, middle + P(19) };
}

// The Games tab between pad and right, from y down. Returns where it ends.
int GamesLayout(HDC dc, int pad, int right, int y)
{
    const int inner = P(18);

    // The game's icon, what is happening, and switching between detection and a picked window, stacked when the
    // picked window closed.
    const int iconSize = P(40);
    std::vector<std::pair<Action, const wchar_t*>> buttons;
    if (!g.selectedGame || l.windowClosed)
        buttons.emplace_back(Action::PickWindow, L"Pick a window");
    if (g.selectedGame)
        buttons.emplace_back(Action::DetectAutomatically, L"Detect automatically");
    int buttonWidth = 0;
    for (const auto& [action, label] : buttons)
        buttonWidth = std::max(buttonWidth, ButtonWidth(dc, ui->body, label));
    const int buttonsHeight = static_cast<int>(buttons.size()) * (P(30) + P(8)) - P(8);
    const int textLeft = pad + inner + iconSize + P(14);
    const int textRight = right - inner - buttonWidth - P(16);
    const int titleHeight = TextHeight(dc, ui->semibold, l.statusTitle, textRight - textLeft);
    const int detailHeight = l.statusDetail.empty() ? 0 : TextHeight(dc, ui->body, l.statusDetail, textRight - textLeft);
    const int textHeight = titleHeight + (detailHeight ? P(3) + detailHeight : 0);
    l.statusCard = { pad, y, right, y + std::max({ iconSize, textHeight, buttonsHeight }) + inner * 2 };
    int middle = (l.statusCard.top + l.statusCard.bottom) / 2;
    l.statusIconRect = { pad + inner, middle - iconSize / 2, pad + inner + iconSize, middle - iconSize / 2 + iconSize };
    l.statusTitleRect = { textLeft, middle - textHeight / 2, textRight, middle - textHeight / 2 + titleHeight };
    l.statusDetailRect = { textLeft, l.statusTitleRect.bottom + P(3), textRight, l.statusTitleRect.bottom + P(3) + detailHeight };
    int buttonTop = middle - buttonsHeight / 2;
    for (const auto& [action, label] : buttons)
    {
        l.targets.push_back({ { right - inner - buttonWidth, buttonTop, right - inner, buttonTop + P(30) }, action, Look::Button, 0, label });
        buttonTop += P(30) + P(8);
    }
    y = l.statusCard.bottom + P(14);

    l.updateCard = {};
    if (!l.update.version.empty())
    {
        l.updateCard = { pad, y, right, y + P(44) };
        const std::wstring download = L"Download";
        const int downloadWidth = TextWidth(dc, ui->semibold, download);
        const RECT downloadRect{ right - inner - downloadWidth, y, right - inner, l.updateCard.bottom };
        l.targets.push_back({ downloadRect, Action::Download, Look::Link, 0, download });
        l.updateText = { pad + inner, y, downloadRect.left - P(8), l.updateCard.bottom };
        y = l.updateCard.bottom + P(14);
    }

    y += P(12);
    const int addWidth = ButtonWidth(dc, ui->strong, L"Add game");
    l.gamesTitle = { pad, y, right - addWidth - P(12), y + P(30) };
    l.targets.push_back({ { right - addWidth, y, right, y + P(30) }, Action::AddGame, Look::PrimaryButton, 0, L"Add game" });
    y += P(30) + P(12);

    const int rowHeight = P(56);
    l.list = { pad, y, right, y + std::max<int>(1, static_cast<int>(l.entries.size())) * rowHeight };
    for (size_t i = 0; i < l.entries.size(); ++i)
    {
        Entry& entry = l.entries[i];
        const int top = y + static_cast<int>(i) * rowHeight;
        middle = top + rowHeight / 2;
        const RECT remove{ right - P(12) - P(28), middle - P(14), right - P(12), middle + P(14) };
        if (entry.removed)
        {
            // Undo follows the text, away from where the remove button was.
            const int left = pad + P(14);
            const int undoWidth = TextWidth(dc, ui->body, L"Undo");
            const int textWidth = TextWidth(dc, ui->body, L"Removed " + entry.name + L".");
            const int textEnd = left + std::min<int>(textWidth, remove.left - P(24) - undoWidth - P(10) - left);
            entry.rect = { pad, top, right, top + rowHeight };
            entry.nameRect = { left, top, textEnd, top + rowHeight };
            l.targets.push_back({ { textEnd + P(10), middle - P(10), textEnd + P(10) + undoWidth, middle + P(10) }, Action::UndoRemove,
                                  Look::Link, 0, L"Undo" });
            continue;
        }
        const RECT toggle{ remove.left - P(10) - P(36), middle - P(10), remove.left - P(10), middle + P(10) };
        const RECT pencil{ toggle.left - P(8) - P(28), middle - P(14), toggle.left - P(8), middle + P(14) };
        l.targets.push_back({ pencil, Action::RenameGame, Look::Rename, entry.game, L"", false, L"Rename" });
        l.targets.push_back({ toggle, Action::ToggleGame, Look::Switch, entry.game, L"", entry.enabled });
        l.targets.push_back({ remove, Action::RemoveGame, Look::Remove, entry.game, L"", false, L"Remove from list" });
        int nameRight = pencil.left - P(14);
        entry.badge = {};
        if (entry.running)
        {
            entry.badge = { nameRight - TextWidth(dc, ui->note, L"Running") - P(18), middle - P(11), nameRight, middle + P(11) };
            nameRight = entry.badge.left - P(12);
        }
        PlaceEntry(entry, { pad, top, right, top + rowHeight }, P(32), nameRight);
        if (entry.renaming && l.edit)
        {
            // The box covers the name, and the edit control sits in it, moved only when its place changes.
            l.editBox = { entry.nameRect.left - P(6), middle - P(23), entry.nameRect.right, middle + P(1) };
            const int line = TextHeight(dc, ui->strong, L"Ag", entry.nameRect.right - entry.nameRect.left);
            const int editTop = (l.editBox.top + l.editBox.bottom - line) / 2;
            const RECT edit{ l.editBox.left + P(6), editTop, l.editBox.right - P(6), editTop + line };
            if (!EqualRect(&edit, &l.editRect))
            {
                l.editRect = edit;
                SetWindowPos(l.edit, nullptr, edit.left, edit.top, edit.right - edit.left, edit.bottom - edit.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    y = l.list.bottom + P(26);

    // Setup's checks, warnings and errors, until dismissed.
    l.rows.clear();
    l.noticesTitle = {};
    const std::vector<Notice> notices = Notices();
    if (!notices.empty())
    {
        const int dismissWidth = TextWidth(dc, ui->body, L"Dismiss");
        l.noticesTitle = { pad, y, right - dismissWidth - P(12), y + P(22) };
        l.targets.push_back({ { right - dismissWidth, y, right, y + P(22) }, Action::DismissNotices, Look::Link, 0, L"Dismiss" });
        y += P(22) + P(10);
        size_t links = 0;
        for (const Notice& notice : notices)
        {
            const int left = pad + P(26);
            Row row{ .level = notice.level };
            const int bottom = std::max(PlaceWords(dc, row, notice.text, left, y, right, links), y + P(18));
            row.rect = { left, y, right, bottom };
            l.rows.push_back(std::move(row));
            y = bottom + P(8);
        }
        y += P(8);
    }
    return y;
}

// A setting's title with its description under it, between left and right. Returns the bottom.
int PlaceSetting(HDC dc, int left, int right, int top, const std::wstring& title, const std::wstring& description)
{
    const int titleHeight = TextHeight(dc, ui->body, title, right - left);
    l.labels.push_back({ { left, top, right, top + titleHeight }, ui->body, theme::kText, title });
    const int detailTop = top + titleHeight + P(2);
    const int detailHeight = TextHeight(dc, ui->note, description, right - left);
    l.labels.push_back({ { left, detailTop, right, detailTop + detailHeight }, ui->note, theme::kDim, description });
    return detailTop + detailHeight;
}

void PlaceHeading(int left, int right, int& y, const wchar_t* text)
{
    l.labels.push_back({ { left, y, right, y + P(16) }, ui->note, theme::kDim, text, DT_SINGLELINE });
    y += P(16) + P(10);
}

// A switch with its title and description beside it.
void PlaceSwitch(HDC dc, int left, int right, int& y, Action action, bool on, const wchar_t* title, const wchar_t* description)
{
    l.targets.push_back({ { left, y, left + P(36), y + P(20) }, action, Look::Switch, 0, L"", on });
    y = PlaceSetting(dc, left + P(36) + P(12), right, y, title, description) + P(18);
}

// Choices side by side in one control, each as wide as the others, with the selected one filled.
void PlaceSegments(int left, int right, int& y, Action action, std::initializer_list<const wchar_t*> choices, int selected)
{
    const int count = static_cast<int>(choices.size());
    l.groups.push_back({ left, y, right, y + P(32) });
    int index = 0;
    for (const wchar_t* choice : choices)
    {
        l.targets.push_back({ { left + (right - left) * index / count, y, left + (right - left) * (index + 1) / count, y + P(32) }, action,
                              Look::Segment, static_cast<size_t>(index), choice, index == selected });
        ++index;
    }
    y += P(32);
}

// A setting with its choices under it.
void PlaceChoices(HDC dc, int left, int right, int& y, Action action, const wchar_t* title, const wchar_t* description,
                  std::initializer_list<const wchar_t*> choices, int selected)
{
    y = PlaceSetting(dc, left, right, y, title, description) + P(8);
    PlaceSegments(left, right, y, action, choices, selected);
    y += P(18);
}

// The place of value among a setting's choices, or past the last one for a value that is none of them.
template <size_t N>
int ChoiceOf(const int (&values)[N], int value)
{
    return static_cast<int>(std::find(std::begin(values), std::end(values), value) - std::begin(values));
}

void GeneralLayout(HDC dc, int pad, int right, int& y)
{
    PlaceHeading(pad, right, y, L"STARTUP");
    PlaceSwitch(dc, pad, right, y, Action::StartWithWindows, l.startWithWindows, L"Start with Windows",
                L"Starts in the notification area when you sign in.");

    PlaceHeading(pad, right, y, L"PRESETS");
    PlaceSwitch(dc, pad, right, y, Action::AutoSave, AutoSavePresets(), L"Save changes automatically",
                L"Turn off to try changes first and save them with the icon at the top of the menu.");

    PlaceHeading(pad, right, y, L"DISPLAY");
    // The menu's size in quarter steps.
    const int button = P(32);
    const int valueWidth = P(58);
    const float scale = MenuScale();
    const int bottom = PlaceSetting(dc, pad, right - button * 2 - valueWidth - P(14), y, L"Menu size",
                                    L"On top of the size that follows the game's window. Changes right away.");
    l.targets.push_back({ { right - button * 2 - valueWidth, y, right - button - valueWidth, y + button }, Action::MenuSize, Look::Button, 0, L"-",
                          false, nullptr, scale > kSmallestMenuScale + 0.01f });
    l.labels.push_back({ { right - button - valueWidth, y, right - button, y + button }, ui->body, theme::kText,
                         std::to_wstring(std::lround(scale * 100)) + L"%", DT_SINGLELINE | DT_CENTER | DT_VCENTER });
    l.targets.push_back({ { right - button, y, right, y + button }, Action::MenuSize, Look::Button, 1, L"+", false, nullptr,
                          scale < kLargestMenuScale - 0.01f });
    y = std::max(bottom, y + button) + P(18);
    PlaceSwitch(dc, pad, right, y, Action::KeepEffects, KeepEffectsVisible(), L"Keep effects visible when another window is in front",
                L"Effects stay over the game while another window, such as a chat or a browser, is in front of it.");

    PlaceHeading(pad, right, y, L"UPDATES");
    PlaceSwitch(dc, pad, right, y, Action::UpdateChecks, UpdateChecksEnabled(), L"Check for updates",
                L"Asks GitHub for a newer version when Unishade starts. Applies from the next start.");
}

void PerformanceLayout(HDC dc, int pad, int right, int& y)
{
    const std::wstring& game = PerformanceGame();
    const std::wstring scope = game.empty() ? L"For every game without its own settings." : L"For " + game + L" only.";
    const int scopeHeight = TextHeight(dc, ui->note, scope, right - pad);
    l.labels.push_back({ { pad, y, right, y + scopeHeight }, ui->note, theme::kDim, scope });
    y += scopeHeight + P(14);

    y = PlaceSetting(dc, pad, right, y, L"Frame rate", L"The most frames a second Unishade shows. A lower limit leaves more of the GPU to the game.") +
        P(8);
    PlaceSegments(pad, right, y, Action::FrameRate, { L"Follow game", L"120 FPS", L"60 FPS", L"Custom" },
                  l.rate ? static_cast<int>(std::size(kFrameRates)) : ChoiceOf(kFrameRates, FrameRateLimit()));
    if (l.rate)
    {
        // The box holds the edit control, which is moved only when its place changes.
        y += P(8);
        l.rateBox = { pad, y, pad + P(84), y + P(30) };
        const int line = TextHeight(dc, ui->body, L"0", P(84));
        const int editTop = (l.rateBox.top + l.rateBox.bottom - line) / 2;
        const RECT edit{ l.rateBox.left + P(10), editTop, l.rateBox.right - P(10), editTop + line };
        if (!EqualRect(&edit, &l.rateRect))
        {
            l.rateRect = edit;
            SetWindowPos(l.rate, nullptr, edit.left, edit.top, edit.right - edit.left, edit.bottom - edit.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        l.labels.push_back({ { l.rateBox.right + P(10), y, right, y + P(30) }, ui->note, theme::kDim,
                             L"frames a second, from " + std::to_wstring(kSlowestFrameRate) + L" to " + std::to_wstring(kFastestFrameRate),
                             DT_SINGLELINE | DT_VCENTER });
        y += P(30);
    }
    y += P(18);

    PlaceChoices(dc, pad, right, y, Action::EffectResolution, L"Effect resolution",
                 L"Effects run on a smaller picture, stretched back to the game's size. Lower is faster and blurrier. The menu blurs too, and "
                 L"screenshots get smaller.",
                 { L"100%", L"75%", L"50%" }, ChoiceOf(kEffectResolutions, EffectResolution()));
    if (DepthEnabled())
        PlaceChoices(dc, pad, right, y, Action::DepthDetail, L"Depth detail",
                     L"Depth is estimated from a smaller picture. Lower is faster, and effects that use depth lose fine detail.",
                     { L"Ultra", L"High", L"Medium", L"Low" }, ChoiceOf(kDepthSizes, DepthSize()));

    PlaceHeading(pad, right, y, L"DEBUG");
    PlaceSwitch(dc, pad, right, y, Action::DebugInfo, DebugInfoEnabled(), L"Show debug info", L"Captured game FPS, output FPS and frame loss.");
}

void ShortcutsLayout(HDC dc, int pad, int right, int& y)
{
    const wchar_t* intro = L"Click a shortcut, then press the keys you want. They work right away.";
    const int introHeight = TextHeight(dc, ui->note, intro, right - pad);
    l.labels.push_back({ { pad, y, right, y + introHeight }, ui->note, theme::kDim, intro });
    y += introHeight + P(14);

    // The keys line up, also on the menu's shortcut, which cannot be left without them.
    const int keyRight = right - P(28) - P(6);
    const int keyLeft = keyRight - P(130);
    for (size_t i = 0; i < std::size(kShortcuts); ++i)
    {
        const Shortcut& shortcut = kShortcuts[i];
        const Hotkey& hotkey = g.hotkeys.*shortcut.member;
        const bool capturing = l.capturing == static_cast<int>(i);
        const int bottom = PlaceSetting(dc, pad, keyLeft - P(14), y, Wide(shortcut.title), Wide(shortcut.description));
        l.targets.push_back({ { keyLeft, y + P(1), keyRight, y + P(33) }, Action::SetShortcut, Look::Button, i,
                              capturing ? L"Press keys..." : hotkey.key ? FormatHotkey(hotkey) : L"Not set", capturing });
        if (shortcut.member != &InputHotkeys::input)
            l.targets.push_back({ { right - P(28), y + P(3), right, y + P(31) }, Action::ClearShortcut, Look::Remove, i, L"", false,
                                  L"Leave unassigned", hotkey.key != 0 });
        y = std::max(bottom, y + P(34)) + P(12);
    }

    const auto line = [&](const std::wstring& text, unsigned color) {
        const int height = TextHeight(dc, ui->body, text, right - pad);
        l.labels.push_back({ { pad, y, right, y + height }, ui->body, color, text });
        y += height + P(6);
    };
    if (!l.shortcutError.empty())
        line(l.shortcutError, theme::kError);
    for (const std::wstring& warning : ShortcutWarnings())
        line(warning, theme::kWarning);
    const wchar_t* reset = L"Reset to defaults";
    l.targets.push_back({ { pad, y, pad + TextWidth(dc, ui->body, reset), y + P(20) }, Action::ResetShortcuts, Look::Link, 0, reset });
    y += P(20) + P(18);
}

// The Settings tab between pad and right, from y down. Returns where it ends.
int SettingsLayout(HDC dc, int pad, int right, int y)
{
    PlaceSegments(pad, right, y, Action::ShowPage, { L"General", L"Performance", L"Shortcuts" }, static_cast<int>(l.page));
    y += P(20);
    switch (l.page)
    {
    case Page::General: GeneralLayout(dc, pad, right, y); break;
    case Page::Performance: PerformanceLayout(dc, pad, right, y); break;
    case Page::Shortcuts: ShortcutsLayout(dc, pad, right, y); break;
    }
    return y;
}

// The Discord tab between pad and right, from y down. Returns where it ends.
int DiscordLayout(HDC dc, int pad, int right, int y)
{
    const bool on = DiscordPresenceEnabled();
    PlaceSwitch(dc, pad, right, y, Action::DiscordPresence, on, L"Show on Discord",
                L"Your Discord profile shows the game Unishade is running on, with its icon and your preset.");
    if (!on)
        return y;
    std::wstring status;
    unsigned color = theme::kDim;
    const DiscordStatus discord = CurrentDiscordStatus();
    if (!g.target)
        status = L"Shows up once Unishade is running on a game.";
    else
        switch (discord.state)
        {
        case DiscordState::Idle:
        case DiscordState::Connecting: status = L"Connecting to Discord..."; break;
        case DiscordState::Closed: status = L"Waiting for Discord to open."; break;
        case DiscordState::Showing:
            status = L"Showing " + g.activeGame->name + L" on Discord.";
            color = theme::kSuccess;
            break;
        case DiscordState::Refused:
            status = L"Discord refused it: " + discord.error;
            color = theme::kWarning;
            break;
        }
    // Under the switch's title.
    const int left = pad + P(36) + P(12);
    const int height = TextHeight(dc, ui->body, status, right - left);
    l.labels.push_back({ { left, y, right, y + height }, ui->body, color, status });
    return y + height + P(18);
}

void Layout(HDC dc)
{
    const int width = P(kWidth);
    const int pad = P(28);
    const int right = width - pad;
    int y = P(3) + P(24) - l.scroll;
    l.targets.clear();
    l.labels.clear();
    l.groups.clear();

    l.logoRect = { pad, y, pad + P(44), y + P(44) };
    y += P(44) + P(12);

    // The tabs' names line up with the logo, and each is clicked a little past its name.
    int x = pad - P(8);
    for (const auto& [tab, name] : { std::pair{ Tab::Games, L"Games" }, std::pair{ Tab::Settings, L"Settings" }, std::pair{ Tab::Discord, L"Discord" } })
    {
        const int tabWidth = TextWidth(dc, ui->strong, name) + P(16);
        l.targets.push_back({ { x, y, x + tabWidth, y + P(36) }, Action::ShowTab, Look::Tab, static_cast<size_t>(tab), name, l.tab == tab });
        x += tabWidth + P(10);
    }
    l.tabLine = y + P(36);
    y = l.tabLine + P(20);

    switch (l.tab)
    {
    case Tab::Games: y = GamesLayout(dc, pad, right, y); break;
    case Tab::Settings: y = SettingsLayout(dc, pad, right, y); break;
    case Tab::Discord: y = DiscordLayout(dc, pad, right, y); break;
    }

    // The footer stays at the bottom of the window when the content is shorter.
    const int noteHeight = TextHeight(dc, ui->note, kTrayNote, right - pad);
    l.footer = std::max(y, P(kMinHeight) - (P(14) + noteHeight + P(12) + P(20) + P(16)) - l.scroll);
    l.trayNote = { pad, l.footer + P(14), right, l.footer + P(14) + noteHeight };
    int linkY = l.trayNote.bottom + P(12);
    x = pad;
    const auto link = [&](Action action, const wchar_t* label) {
        const int linkWidth = TextWidth(dc, ui->body, label);
        if (x + linkWidth > right)
        {
            x = pad;
            linkY += P(28);
        }
        l.targets.push_back({ { x, linkY, x + linkWidth, linkY + P(20) }, action, Look::Link, 0, label });
        x += linkWidth + P(20);
    };
    link(Action::OpenLog, L"Open log");
    link(Action::Help, L"Get help on Discord");
    if (!l.setup.empty())
        link(Action::RunSetup, L"Run Setup");
    link(Action::Support, L"Support on Ko-fi");
    l.height = linkY + P(20) + P(16) + l.scroll;
}

void Relayout()
{
    const HDC dc = GetDC(g.launcher);
    Layout(dc);
    ReleaseDC(g.launcher, dc);
}

// Sizes the window to its content, keeping it where it is unless it grows past the bottom of the screen.
void Resize()
{
    Relayout();
    // A minimized window is sized when it is restored.
    if (IsIconic(g.launcher))
        return;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.launcher, GWL_STYLE));
    const UINT dpi = GetDpiForWindow(g.launcher);
    RECT frame{ 0, 0, P(kWidth), l.height };
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(g.launcher, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    RECT window{};
    GetWindowRect(g.launcher, &window);
    const int height = std::min<int>(frame.bottom - frame.top, work.bottom - work.top);
    // Only moved when it grows, so a window placed partly off the screen on purpose stays there.
    int top = window.top;
    if (height > window.bottom - window.top && top + height > work.bottom)
        top = std::max<int>(work.top, work.bottom - height);
    SetWindowPos(g.launcher, nullptr, window.left, top, frame.right - frame.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g.launcher, nullptr, FALSE);
}

Control ControlOf(const Target& target)
{
    return { target.action, target.index };
}

int TargetAt(POINT point)
{
    for (size_t i = 0; i < l.targets.size(); ++i)
        if (l.targets[i].enabled && PtInRect(&l.targets[i].rect, point))
            return static_cast<int>(i);
    return -1;
}

int FindTarget(const std::optional<Control>& control)
{
    for (size_t i = 0; control && i < l.targets.size(); ++i)
        if (ControlOf(l.targets[i]) == *control)
            return static_cast<int>(i);
    return -1;
}

// A button that is only an icon shows its tip once the mouse rests on it, like a tooltip.
void SetHovered(int hovered)
{
    if (hovered == l.hovered)
        return;
    l.hovered = hovered;
    l.tip.reset();
    KillTimer(g.launcher, kTipTimer);
    if (hovered >= 0 && l.targets[hovered].tip)
        SetTimer(g.launcher, kTipTimer, GetDoubleClickTime(), nullptr);
    InvalidateRect(g.launcher, nullptr, FALSE);
}

void UpdateHover()
{
    POINT point{};
    GetCursorPos(&point);
    const bool inside = WindowFromPoint(point) == g.launcher;
    ScreenToClient(g.launcher, &point);
    SetHovered(inside ? TargetAt(point) : -1);
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
                 .discord = DiscordStatusVersion() };
    // Only shown while waiting for the picked window. The main loop notices the game's window closing.
    if (g.captureEnabled && !g.target && g.selectedGame)
        shown.selectedOpen = GameWindowExists(*g.selectedGame);
    return shown;
}

// Closes the rename box without saving.
void CloseRename()
{
    const HWND edit = l.edit;
    if (!edit)
        return;
    // Cleared first, so the box losing the focus does not end the rename again.
    l.edit = nullptr;
    l.renameProblem.clear();
    l.editRect = {};
    if (GetFocus() == edit)
        SetFocus(g.launcher);
    DestroyWindow(edit);
}

// Closes the frame rate box without using the number in it.
void CloseRate()
{
    const HWND rate = l.rate;
    if (!rate)
        return;
    // Cleared first, so the box losing the focus does not use its number.
    l.rate = nullptr;
    l.rateRect = {};
    if (GetFocus() == rate)
        SetFocus(g.launcher);
    DestroyWindow(rate);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

// Opens the box a custom frame rate is typed in, holding the limit, or 60 without one.
void OpenRate()
{
    const int limit = FrameRateLimit();
    l.rate = CreateWindowExW(0, L"EDIT", std::to_wstring(limit ? limit : 60).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 0,
                             0, g.launcher, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!l.rate)
    {
        Log(LogLevel::Error, L"Could not open the frame rate box (error %lu).", GetLastError());
        return;
    }
    l.editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(l.rate, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    SendMessageW(l.rate, EM_SETLIMITTEXT, 3, 0);
    SendMessageW(l.rate, WM_SETFONT, reinterpret_cast<WPARAM>(l.launcherScaling.body), FALSE);
}

// Keeps the frame rate box for as long as Performance shows a custom limit or one is being typed. The menu may
// have changed the limit.
void SyncRate()
{
    const int limit = FrameRateLimit();
    const bool typing = l.rate && GetFocus() == l.rate;
    if (l.tab != Tab::Settings || l.page != Page::Performance || (ChoiceOf(kFrameRates, limit) < static_cast<int>(std::size(kFrameRates)) && !typing))
    {
        CloseRate();
        return;
    }
    if (!l.rate)
        OpenRate();
    else if (!typing)
        SetWindowTextW(l.rate, std::to_wstring(limit).c_str());
}

// Stops waiting for a shortcut's keys.
void StopShortcut()
{
    if (l.capturing < 0)
        return;
    l.capturing = -1;
    SuspendHotkeys(false);
}

void Refresh()
{
    // The game being renamed was removed.
    if (l.edit && std::none_of(g.autoGames.begin(), g.autoGames.end(), [](const AutoGame& game) { return game.executable == l.renaming; }))
        CloseRename();
    l.shown = CurrentShown();
    Describe();
    SyncRate();
    Resize();
    UpdateHover();
}

RECT Inset(RECT rect, int by)
{
    InflateRect(&rect, -by, -by);
    return rect;
}

void RoundedPath(Gdiplus::GraphicsPath& path, const RECT& rect, float radius)
{
    const float x = F(rect.left) + 0.5f, y = F(rect.top) + 0.5f, w = F(rect.right - rect.left) - 1.0f, h = F(rect.bottom - rect.top) - 1.0f;
    const float d = std::min({ radius * 2, w, h });
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRounded(Gdiplus::Graphics& graphics, const RECT& rect, float radius, const Gdiplus::Color& fill, const Gdiplus::Color& border)
{
    Gdiplus::GraphicsPath path;
    RoundedPath(path, rect, radius);
    Gdiplus::SolidBrush brush(fill);
    graphics.FillPath(&brush, &path);
    Gdiplus::Pen pen(border, 1.0f);
    graphics.DrawPath(&pen, &path);
}

// The keyboard focus, around a control.
void FocusRing(Gdiplus::Graphics& graphics, const RECT& control)
{
    const RECT rect = Inset(control, -P(3));
    Gdiplus::GraphicsPath path;
    RoundedPath(path, rect, std::min(F(8.0f), F(rect.bottom - rect.top) / 2));
    Gdiplus::Pen pen(Plus(theme::kAccentHover), F(2.0f));
    graphics.DrawPath(&pen, &path);
}

void PaintIcon(Gdiplus::Graphics& graphics, HICON icon, const RECT& rect)
{
    const HDC dc = graphics.GetHDC();
    DrawIconEx(dc, rect.left, rect.top, icon, rect.right - rect.left, rect.bottom - rect.top, 0, nullptr, DI_NORMAL);
    graphics.ReleaseHDC(dc);
}

// The name's first letter stands in for a missing icon.
std::wstring Initial(const std::wstring& name)
{
    for (wchar_t character : name)
        if (IsCharAlphaNumericW(character))
        {
            wchar_t letter[] = { character, L'\0' };
            CharUpperW(letter);
            return letter;
        }
    return L"?";
}

void LetterTile(Gdiplus::Graphics& graphics, const RECT& rect)
{
    FillRounded(graphics, rect, F(8.0f), Plus(theme::kCardHover), Plus(theme::kBorder));
}

void StatusIcon(Gdiplus::Graphics& graphics)
{
    const RECT& rect = l.statusIconRect;
    Gdiplus::SolidBrush solid(Plus(l.statusColor));
    if (l.statusName.empty())
    {
        const Gdiplus::PointF center(F(rect.left + rect.right) / 2, F(rect.top + rect.bottom) / 2);
        Gdiplus::SolidBrush glow(Plus(l.statusColor, 0x40));
        graphics.FillEllipse(&glow, center.X - F(10.0f), center.Y - F(10.0f), F(20.0f), F(20.0f));
        graphics.FillEllipse(&solid, center.X - F(5.5f), center.Y - F(5.5f), F(11.0f), F(11.0f));
        return;
    }
    if (const HICON icon = ExecutableIcon(l.statusIcon, rect.right - rect.left))
        PaintIcon(graphics, icon, rect);
    else
        LetterTile(graphics, rect);
    // The state as a dot on the icon's corner.
    const Gdiplus::PointF dot(F(rect.right) - F(3.0f), F(rect.bottom) - F(3.0f));
    Gdiplus::SolidBrush ring(Plus(theme::kCard));
    graphics.FillEllipse(&ring, dot.X - F(7.5f), dot.Y - F(7.5f), F(15.0f), F(15.0f));
    graphics.FillEllipse(&solid, dot.X - F(4.5f), dot.Y - F(4.5f), F(9.0f), F(9.0f));
}

void Switch(Gdiplus::Graphics& graphics, const RECT& rect, bool on, bool hovered)
{
    const unsigned track = on ? (hovered ? theme::kAccentHover : theme::kAccent) : (hovered ? theme::kBorderStrong : theme::kBorder);
    const float height = F(rect.bottom - rect.top);
    FillRounded(graphics, rect, height / 2, Plus(track), Plus(track));
    const float knob = height - F(6.0f);
    const float x = on ? F(rect.right) - F(3.0f) - knob : F(rect.left) + F(3.0f);
    Gdiplus::SolidBrush brush(Plus(on ? 0xFFFFFF : theme::kDim));
    graphics.FillEllipse(&brush, x, F(rect.top) + F(3.0f), knob, knob);
}

// A pencil.
void RenameIcon(Gdiplus::Graphics& graphics, const RECT& rect, bool hovered)
{
    const Gdiplus::PointF center(F(rect.left + rect.right) / 2, F(rect.top + rect.bottom) / 2);
    const float r = F(rect.right - rect.left) / 2;
    if (hovered)
    {
        Gdiplus::SolidBrush background(Plus(theme::kBorder));
        graphics.FillEllipse(&background, center.X - r, center.Y - r, r * 2, r * 2);
    }
    const float s = ui->scale;
    Gdiplus::Pen pen(Plus(hovered ? theme::kText : theme::kDim), 1.4f * s);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    // From the tip at the bottom left to the end at the top right, with a band below the end.
    const Gdiplus::PointF outline[] = { { center.X - 5.0f * s, center.Y + 5.0f * s }, { center.X - 4.5f * s, center.Y + 2.5f * s },
                                        { center.X + 2.5f * s, center.Y - 4.5f * s }, { center.X + 4.5f * s, center.Y - 2.5f * s },
                                        { center.X - 2.5f * s, center.Y + 4.5f * s } };
    graphics.DrawPolygon(&pen, outline, 5);
    graphics.DrawLine(&pen, center.X + 1.0f * s, center.Y - 3.0f * s, center.X + 3.0f * s, center.Y - 1.0f * s);
}

void RemoveIcon(Gdiplus::Graphics& graphics, const RECT& rect, bool hovered, bool enabled)
{
    const Gdiplus::PointF center(F(rect.left + rect.right) / 2, F(rect.top + rect.bottom) / 2);
    const float r = F(rect.right - rect.left) / 2;
    if (hovered)
    {
        Gdiplus::SolidBrush background(Plus(theme::kError, 0x24));
        graphics.FillEllipse(&background, center.X - r, center.Y - r, r * 2, r * 2);
    }
    Gdiplus::Pen pen(Plus(hovered ? theme::kError : enabled ? theme::kDim : theme::kBorderStrong), 1.6f * ui->scale);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float s = F(4.5f);
    graphics.DrawLine(&pen, center.X - s, center.Y - s, center.X + s, center.Y + s);
    graphics.DrawLine(&pen, center.X + s, center.Y - s, center.X - s, center.Y + s);
}

void Icon(Gdiplus::Graphics& graphics, Gdiplus::PointF center, LogLevel level)
{
    const unsigned color = level == LogLevel::Ok ? theme::kSuccess : level == LogLevel::Warning ? theme::kWarning
                         : level == LogLevel::Error ? theme::kError : theme::kDim;
    const float r = F(8.0f);
    const float s = ui->scale;
    Gdiplus::SolidBrush tint(Plus(color, 0x30));
    Gdiplus::SolidBrush solid(Plus(color));
    Gdiplus::Pen pen(Plus(color), 1.6f * s);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.FillEllipse(&tint, center.X - r, center.Y - r, r * 2, r * 2);
    switch (level)
    {
    case LogLevel::Ok:
    {
        const Gdiplus::PointF points[] = { { center.X - 3.5f * s, center.Y }, { center.X - 1.0f * s, center.Y + 2.8f * s }, { center.X + 3.8f * s, center.Y - 2.8f * s } };
        graphics.DrawLines(&pen, points, 3);
        break;
    }
    case LogLevel::Warning:
        graphics.DrawLine(&pen, center.X, center.Y - 4 * s, center.X, center.Y + 1 * s);
        graphics.FillEllipse(&solid, center.X - 1.1f * s, center.Y + 2.7f * s, 2.2f * s, 2.2f * s);
        break;
    case LogLevel::Error:
        graphics.DrawLine(&pen, center.X - 3 * s, center.Y - 3 * s, center.X + 3 * s, center.Y + 3 * s);
        graphics.DrawLine(&pen, center.X + 3 * s, center.Y - 3 * s, center.X - 3 * s, center.Y + 3 * s);
        break;
    default:
        graphics.FillEllipse(&solid, center.X - 2.2f * s, center.Y - 2.2f * s, 4.4f * s, 4.4f * s);
        break;
    }
}

// The logo's ring as a strip along the top, like Setup. Drawn last so content scrolls under it.
void Strip(HDC dc, int width)
{
    Gdiplus::Graphics graphics(dc);
    constexpr int count = static_cast<int>(std::size(theme::kRainbow));
    Gdiplus::Color colors[count];
    Gdiplus::REAL positions[count];
    for (int i = 0; i < count; ++i)
    {
        colors[i] = Plus(theme::kRainbow[i]);
        positions[i] = static_cast<Gdiplus::REAL>(i) / (count - 1);
    }
    Gdiplus::LinearGradientBrush strip(Gdiplus::Point(0, 0), Gdiplus::Point(width, 0), colors[0], colors[count - 1]);
    strip.SetInterpolationColors(colors, positions, count);
    graphics.FillRectangle(&strip, 0, 0, width, P(3));
}

void PaintText(HDC dc, HFONT font, unsigned color, const std::wstring& text, RECT rect, UINT format = DT_WORDBREAK)
{
    SelectObject(dc, font);
    SetTextColor(dc, Gdi(color));
    DrawTextW(dc, text.c_str(), -1, &rect, format | DT_NOPREFIX);
}

// The executable's icon, or the name's first letter on a tile. Turned-off games are faded.
void EntryIcon(Gdiplus::Graphics& graphics, const Entry& entry)
{
    if (const HICON icon = ExecutableIcon(entry.icon, entry.iconRect.right - entry.iconRect.left))
        PaintIcon(graphics, icon, entry.iconRect);
    else
        LetterTile(graphics, entry.iconRect);
    if (!entry.enabled)
    {
        Gdiplus::SolidBrush fade(Plus(theme::kCard, 0xA0));
        graphics.FillRectangle(&fade, F(entry.iconRect.left), F(entry.iconRect.top), F(entry.iconRect.right - entry.iconRect.left),
                               F(entry.iconRect.bottom - entry.iconRect.top));
    }
}

void EntryText(HDC dc, const Entry& entry)
{
    const unsigned color = entry.enabled ? theme::kText : theme::kDim;
    if (!ExecutableIcon(entry.icon, entry.iconRect.right - entry.iconRect.left))
        PaintText(dc, ui->semibold, color, Initial(entry.name), entry.iconRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    // The rename box covers the name, and what is wrong with the new one shows below it.
    if (entry.renaming)
    {
        const bool problem = !l.renameProblem.empty();
        PaintText(dc, ui->note, problem ? theme::kError : theme::kDim, problem ? l.renameProblem : L"Enter saves the name. Esc cancels.",
                  entry.detailRect, DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    PaintText(dc, ui->strong, color, entry.name, entry.nameRect, DT_SINGLELINE | (entry.detail.empty() ? DT_VCENTER : DT_BOTTOM) | DT_END_ELLIPSIS);
    PaintText(dc, ui->note, theme::kDim, entry.detail, entry.detailRect, DT_SINGLELINE | DT_PATH_ELLIPSIS);
}

void Paint(HDC output)
{
    RECT client{};
    GetClientRect(g.launcher, &client);
    const int width = client.right;
    const int height = client.bottom;
    l.scroll = std::clamp(l.scroll, 0, std::max(0, l.height - height));
    const bool focusShown = l.focusShown && GetFocus() == g.launcher;
    const bool games = l.tab == Tab::Games;
    const HDC dc = CreateCompatibleDC(output);
    const HBITMAP bitmap = CreateCompatibleBitmap(output, width, height);
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    const HGDIOBJ previousFont = SelectObject(dc, ui->body);
    Layout(dc);

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        Gdiplus::SolidBrush background(Plus(theme::kBackground));
        graphics.FillRectangle(&background, 0, 0, width, height);

        if (l.logo)
            graphics.DrawImage(l.logo.get(), F(l.logoRect.left), F(l.logoRect.top), F(l.logoRect.right - l.logoRect.left),
                               F(l.logoRect.bottom - l.logoRect.top));

        Gdiplus::Pen line(Plus(theme::kBorder), 1.0f);
        graphics.DrawLine(&line, 0, l.tabLine, width, l.tabLine);
        if (games)
        {
            FillRounded(graphics, l.statusCard, F(10.0f), Plus(theme::kCard), Plus(theme::kBorder));
            StatusIcon(graphics);

            if (!l.update.version.empty())
                FillRounded(graphics, l.updateCard, F(10.0f), Plus(theme::kAccent, 0x22), Plus(theme::kAccent, 0x90));

            FillRounded(graphics, l.list, F(10.0f), Plus(theme::kCard), Plus(theme::kBorder));
            for (size_t i = 1; i < l.entries.size(); ++i)
            {
                const int top = l.entries[i].rect.top;
                graphics.DrawLine(&line, static_cast<int>(l.list.left) + P(14), top, static_cast<int>(l.list.right) - P(14), top);
            }
        }
        for (const RECT& group : l.groups)
            FillRounded(graphics, group, F(7.0f), Plus(theme::kSidebar), Plus(theme::kBorder));
        if (l.rate)
            FillRounded(graphics, l.rateBox, F(6.0f), Plus(theme::kCardHover), Plus(GetFocus() == l.rate ? theme::kAccent : theme::kBorderStrong));

        for (size_t i = 0; i < l.targets.size(); ++i)
        {
            const Target& target = l.targets[i];
            const bool hovered = l.hovered == static_cast<int>(i);
            switch (target.look)
            {
            case Look::Button:
                // A shortcut's button is outlined while it waits for keys.
                FillRounded(graphics, target.rect, F(8.0f), Plus(hovered ? theme::kBorder : theme::kCardHover),
                            Plus(target.on ? theme::kAccent : hovered ? theme::kBorderStrong : theme::kBorder));
                break;
            case Look::PrimaryButton:
            {
                const unsigned color = hovered ? theme::kAccentHover : theme::kAccent;
                FillRounded(graphics, target.rect, F(8.0f), Plus(color), Plus(color));
                break;
            }
            case Look::Switch:
                Switch(graphics, target.rect, target.on, hovered);
                break;
            case Look::Tab:
                if (target.on)
                {
                    Gdiplus::SolidBrush underline(Plus(theme::kAccent));
                    graphics.FillRectangle(&underline, F(target.rect.left) + F(2.0f), F(target.rect.bottom) - F(2.0f),
                                           F(target.rect.right - target.rect.left) - F(4.0f), F(2.0f));
                }
                break;
            case Look::Segment:
                if (target.on)
                {
                    // The pages of Settings are quieter than a setting's choices, like in the menu.
                    const unsigned fill = target.action == Action::ShowPage ? theme::kBorder : theme::kAccent;
                    FillRounded(graphics, Inset(target.rect, P(3)), F(5.0f), Plus(fill), Plus(fill));
                }
                break;
            case Look::Rename:
                RenameIcon(graphics, target.rect, hovered);
                break;
            case Look::Remove:
                RemoveIcon(graphics, target.rect, hovered, target.enabled);
                break;
            case Look::Link:
                break;
            }
        }

        if (games)
        {
            for (const Entry& entry : l.entries)
            {
                if (entry.removed)
                    continue;
                EntryIcon(graphics, entry);
                if (entry.running)
                    FillRounded(graphics, entry.badge, F(entry.badge.bottom - entry.badge.top) / 2, Plus(theme::kSuccess, 0x26),
                                Plus(theme::kSuccess, 0x26));
            }

            for (const Row& row : l.rows)
                Icon(graphics, Gdiplus::PointF(F(row.rect.left) - F(18.0f), F(row.rect.top) + F(9.0f)), row.level);

            if (l.edit)
                FillRounded(graphics, l.editBox, F(6.0f), Plus(theme::kCardHover), Plus(l.renameProblem.empty() ? theme::kAccent : theme::kError));
        }

        if (const int focused = FindTarget(l.focus); focused >= 0 && focusShown)
            FocusRing(graphics, l.targets[focused].rect);

        graphics.DrawLine(&line, 0, l.footer, width, l.footer);
    }

    SetBkMode(dc, TRANSPARENT);
    PaintText(dc, ui->title, theme::kText, L"Unishade",
             { l.logoRect.right + P(14), l.logoRect.top + P(1), width, l.logoRect.top + P(26) }, DT_SINGLELINE);
    PaintText(dc, ui->body, theme::kDim, L"Version " UNISHADE_VERSION,
             { l.logoRect.right + P(14), l.logoRect.top + P(26), width, l.logoRect.bottom }, DT_SINGLELINE);

    if (games)
    {
        if (!l.statusName.empty() && !ExecutableIcon(l.statusIcon, l.statusIconRect.right - l.statusIconRect.left))
            PaintText(dc, ui->title, theme::kText, Initial(l.statusName), l.statusIconRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        PaintText(dc, ui->semibold, theme::kText, l.statusTitle, l.statusTitleRect);
        PaintText(dc, ui->body, theme::kDim, l.statusDetail, l.statusDetailRect);
        if (!l.update.version.empty())
            PaintText(dc, ui->body, theme::kText, L"Unishade " + l.update.version + L" is available.", l.updateText,
                     DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        PaintText(dc, ui->semibold, theme::kText, L"Games", l.gamesTitle, DT_SINGLELINE | DT_VCENTER);
        for (const Entry& entry : l.entries)
        {
            if (entry.removed)
            {
                PaintText(dc, ui->body, theme::kDim, L"Removed " + entry.name + L".", entry.nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                continue;
            }
            EntryText(dc, entry);
            if (entry.running)
                PaintText(dc, ui->note, theme::kSuccess, L"Running", entry.badge, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        if (l.entries.empty())
            PaintText(dc, ui->body, theme::kDim, L"No games yet. Open one and click Add game.", Inset(l.list, P(18)),
                     DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        if (!l.rows.empty())
            PaintText(dc, ui->semibold, theme::kText, L"Messages", l.noticesTitle, DT_SINGLELINE | DT_VCENTER);
        for (const Row& row : l.rows)
            for (const Word& word : row.words)
                PaintText(dc, ui->body, theme::kText, word.text, word.rect, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    for (const Label& label : l.labels)
        PaintText(dc, label.font, label.color, label.text, label.rect, label.format);
    PaintText(dc, ui->note, theme::kDim, kTrayNote, l.trayNote);

    for (size_t i = 0; i < l.targets.size(); ++i)
    {
        const Target& target = l.targets[i];
        const bool hovered = l.hovered == static_cast<int>(i);
        switch (target.look)
        {
        case Look::Link:
            if (target.action == Action::Download)
                PaintText(dc, ui->semibold, hovered ? theme::kText : theme::kAccentHover, target.label, target.rect, DT_SINGLELINE | DT_VCENTER);
            else
                PaintText(dc, ui->body, hovered ? theme::kText : theme::kAccentHover, target.label, target.rect,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            break;
        case Look::Button:
            PaintText(dc, ui->body, target.enabled ? theme::kText : theme::kBorderStrong, target.label, target.rect,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        case Look::PrimaryButton:
            PaintText(dc, ui->strong, 0xFFFFFF, target.label, target.rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        case Look::Tab:
            PaintText(dc, ui->strong, target.on || hovered ? theme::kText : theme::kDim, target.label, target.rect,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        case Look::Segment:
            PaintText(dc, ui->body, target.on ? 0xFFFFFF : hovered ? theme::kText : theme::kDim, target.label, target.rect,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        default:
            break;
        }
    }

    // Over everything else, below the button or above it at the bottom of the window.
    int tip = FindTarget(l.tip);
    if (tip < 0 && focusShown)
        tip = FindTarget(l.focus);
    if (tip >= 0 && l.targets[tip].tip)
    {
        const Target& target = l.targets[tip];
        const int tipWidth = TextWidth(dc, ui->note, target.tip) + P(16);
        const int tipHeight = P(24);
        const int top = target.rect.bottom + P(6) + tipHeight <= height ? target.rect.bottom + P(6) : target.rect.top - P(6) - tipHeight;
        const RECT rect{ target.rect.right - tipWidth, top, target.rect.right, top + tipHeight };
        {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            FillRounded(graphics, rect, F(6.0f), Plus(theme::kCardHover), Plus(theme::kBorderStrong));
        }
        PaintText(dc, ui->note, theme::kText, target.tip, rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }

    Strip(dc, width);
    BitBlt(output, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

const wchar_t* PickerDetail()
{
    return l.picker == Picker::Add ? L"Pick your game's window. If it isn't here, open the game first."
                                   : L"Unishade uses this window until you click Detect automatically.";
}

// Lists the open windows. Returns whether they changed, which moves the rows.
bool ListWindows()
{
    std::vector<GameWindow> windows = ListGameWindows();
    if (std::equal(windows.begin(), windows.end(), l.windows.begin(), l.windows.end(),
                   [](const GameWindow& a, const GameWindow& b) { return a.window == b.window && a.name == b.name; }))
        return false;
    l.windows = std::move(windows);
    l.choices.clear();
    for (const GameWindow& window : l.windows)
    {
        const fs::path executable = Executable(window.processId);
        l.choices.push_back({ .name = window.name, .detail = executable.filename().wstring(), .icon = executable });
    }
    l.pickerHovered = -1;
    return true;
}

void PickerLayout(HDC dc)
{
    const int width = P(kPickerWidth);
    const int pad = P(20);
    const int right = width - pad;
    int y = P(3) + P(16) - l.pickerScroll;
    l.pickerDetail = { pad, y, right, y + TextHeight(dc, ui->body, PickerDetail(), right - pad) };
    y = l.pickerDetail.bottom + P(14);
    const int rowHeight = P(52);
    l.pickerList = { pad, y, right, y + std::max<int>(1, static_cast<int>(l.choices.size())) * rowHeight };
    for (size_t i = 0; i < l.choices.size(); ++i)
    {
        const int top = y + static_cast<int>(i) * rowHeight;
        PlaceEntry(l.choices[i], { pad, top, right, top + rowHeight }, P(32), right - P(14));
    }
    l.pickerHeight = l.pickerList.bottom + pad + l.pickerScroll;
}

// Sizes the picker to its content, up to kPickerMaxHeight, keeping it where it is.
void ResizePicker()
{
    const PickerScaling scaling;
    const HDC dc = GetDC(l.pickerWindow);
    PickerLayout(dc);
    ReleaseDC(l.pickerWindow, dc);
    RECT frame{ 0, 0, P(kPickerWidth), std::min(l.pickerHeight, P(kPickerMaxHeight)) };
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(l.pickerWindow, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(l.pickerWindow, GWL_EXSTYLE)), GetDpiForWindow(l.pickerWindow));
    SetWindowPos(l.pickerWindow, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(l.pickerWindow, nullptr, FALSE);
}

int PickerFocusRow()
{
    for (size_t i = 0; i < l.windows.size(); ++i)
        if (l.windows[i].window == l.pickerFocus)
            return static_cast<int>(i);
    return -1;
}

int ChoiceAt(POINT point)
{
    for (size_t i = 0; i < l.choices.size(); ++i)
        if (PtInRect(&l.choices[i].rect, point))
            return static_cast<int>(i);
    return -1;
}

void PaintPicker(HDC output)
{
    const PickerScaling scaling;
    RECT client{};
    GetClientRect(l.pickerWindow, &client);
    const int width = client.right;
    const int height = client.bottom;
    l.pickerScroll = std::clamp(l.pickerScroll, 0, std::max(0, l.pickerHeight - height));
    const HDC dc = CreateCompatibleDC(output);
    const HBITMAP bitmap = CreateCompatibleBitmap(output, width, height);
    const HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    const HGDIOBJ previousFont = SelectObject(dc, ui->body);
    PickerLayout(dc);

    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        Gdiplus::SolidBrush background(Plus(theme::kBackground));
        graphics.FillRectangle(&background, 0, 0, width, height);
        FillRounded(graphics, l.pickerList, F(10.0f), Plus(theme::kCard), Plus(theme::kBorder));
        Gdiplus::Pen line(Plus(theme::kBorder), 1.0f);
        for (size_t i = 1; i < l.choices.size(); ++i)
        {
            const int top = l.choices[i].rect.top;
            graphics.DrawLine(&line, static_cast<int>(l.pickerList.left) + P(14), top, static_cast<int>(l.pickerList.right) - P(14), top);
        }
        if (l.pickerHovered >= 0 && l.pickerHovered < static_cast<int>(l.choices.size()))
            FillRounded(graphics, Inset(l.choices[l.pickerHovered].rect, P(4)), F(8.0f), Plus(theme::kCardHover), Plus(theme::kCardHover));
        if (const int focused = PickerFocusRow(); focused >= 0 && focused < static_cast<int>(l.choices.size()))
            FocusRing(graphics, Inset(l.choices[focused].rect, P(7)));
        for (const Entry& choice : l.choices)
            EntryIcon(graphics, choice);
    }

    SetBkMode(dc, TRANSPARENT);
    PaintText(dc, ui->body, theme::kDim, PickerDetail(), l.pickerDetail);
    for (const Entry& choice : l.choices)
        EntryText(dc, choice);
    if (l.choices.empty())
        PaintText(dc, ui->body, theme::kDim, L"No open windows.", Inset(l.pickerList, P(18)), DT_SINGLELINE | DT_VCENTER);

    Strip(dc, width);
    BitBlt(output, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void DarkFrame(HWND window)
{
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = Gdi(theme::kBackground);
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
}

// Opens the list of open windows over the launcher, which stays disabled until it closes.
void OpenPicker(Picker picker)
{
    l.picker = picker;
    l.windows.clear();
    l.choices.clear();
    l.pickerScroll = 0;
    l.pickerHovered = -1;
    ListWindows();
    // Opened with the keyboard, the first window has the focus.
    l.pickerFocus = l.focusShown && !l.windows.empty() ? l.windows.front().window : nullptr;
    // Opened over the launcher, so it starts at the scaling of the launcher's monitor.
    RECT owner{};
    GetWindowRect(g.launcher, &owner);
    l.pickerWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, kPickerClass, picker == Picker::Add ? L"Add a game" : L"Pick a window",
                                     WS_POPUP | WS_CAPTION | WS_SYSMENU, owner.left, owner.top, 100, 100, g.launcher, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!l.pickerWindow)
    {
        Log(LogLevel::Error, L"Could not open the window list (error %lu).", GetLastError());
        return;
    }
    DarkFrame(l.pickerWindow);
    Scale(l.pickerScaling, GetDpiForWindow(l.pickerWindow));
    ResizePicker();
    RECT own{};
    GetWindowRect(l.pickerWindow, &own);
    SetWindowPos(l.pickerWindow, nullptr, (owner.left + owner.right - (own.right - own.left)) / 2,
                 std::max<int>(owner.top, (owner.top + owner.bottom - (own.bottom - own.top)) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    EnableWindow(g.launcher, FALSE);
    ShowWindow(l.pickerWindow, SW_SHOW);
}

void ClosePicker()
{
    if (!l.pickerWindow)
        return;
    // Enabled first, so the launcher becomes the active window again.
    EnableWindow(g.launcher, TRUE);
    DestroyWindow(l.pickerWindow);
}

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
    // The focus stays in the list when Undo goes away.
    if (l.focus == Control{ Action::UndoRemove } && !g.autoGames.empty())
        l.focus = Control{ Action::ToggleGame, std::min(l.removed->index, g.autoGames.size() - 1) };
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
    // So pressing Enter again does not remove the next game.
    l.focus = Control{ Action::UndoRemove };
}

void UndoRemove()
{
    if (!l.removed)
        return;
    auto games = g.autoGames;
    // Unless it was added again meanwhile.
    if (std::none_of(games.begin(), games.end(), [](const AutoGame& game) {
            return _wcsicmp(game.executable.c_str(), l.removed->game.executable.c_str()) == 0;
        }))
    {
        const size_t index = std::min(l.removed->index, games.size());
        games.insert(games.begin() + static_cast<std::ptrdiff_t>(index), l.removed->game);
        if (!SaveGames(std::move(games)))
            return;
        l.focus = Control{ Action::RemoveGame, index };
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
std::wstring RenameGame(const fs::path& executable, const std::wstring& name)
{
    const auto game = std::find_if(g.autoGames.begin(), g.autoGames.end(), [&](const AutoGame& saved) { return saved.executable == executable; });
    if (game == g.autoGames.end() || game->name == name)
        return {};
    const size_t index = static_cast<size_t>(game - g.autoGames.begin());
    if (std::wstring problem = NameProblem(name, index); !problem.empty())
        return problem;
    const std::wstring previous = game->name;
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
    return {};
}

// Saves the name in the rename box, unless it is cancelled or the name is not allowed.
void EndRename(WPARAM how)
{
    if (!l.edit)
        return;
    if (how != kCancelName)
    {
        const int length = GetWindowTextLengthW(l.edit);
        std::wstring name(static_cast<size_t>(length) + 1, L'\0');
        name.resize(static_cast<size_t>(GetWindowTextW(l.edit, name.data(), length + 1)));
        // Spaces around it would not survive as a folder name or an ini key.
        name.erase(0, name.find_first_not_of(L' '));
        name.erase(name.find_last_not_of(L' ') + 1);
        l.renameProblem = RenameGame(l.renaming, name);
        if (!l.renameProblem.empty() && how == kSaveName)
        {
            InvalidateRect(g.launcher, nullptr, FALSE);
            return;
        }
    }
    CloseRename();
    Refresh();
}

// Leaving the frame rate box uses the number in it. Enter and Tab leave it, and Escape leaves it with the limit as it was.
void EndRate(WPARAM how)
{
    if (how == kCancelName)
        SetWindowTextW(l.rate, std::to_wstring(FrameRateLimit()).c_str());
    if (GetFocus() == l.rate)
    {
        // Which comes back here.
        SetFocus(g.launcher);
        return;
    }
    wchar_t text[8]{};
    GetWindowTextW(l.rate, text, static_cast<int>(std::size(text)));
    if (const int rate = _wtoi(text); rate > 0)
        SetFrameRateLimit(rate);
    Refresh();
}

// Enter saves what is in the rename box or the frame rate box, Escape cancels, and Tab or clicking elsewhere leaves it.
LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)
        {
            const WPARAM how = wParam == VK_RETURN ? kSaveName : wParam == VK_ESCAPE ? kCancelName : kLeaveName;
            PostMessageW(g.launcher, kRenameMessage, how, reinterpret_cast<LPARAM>(hwnd));
            return 0;
        }
        break;
    // They would beep.
    case WM_CHAR:
        if (wParam == L'\r' || wParam == L'\x1B' || wParam == L'\t')
            return 0;
        break;
    // Not while CloseRename or CloseRate takes the focus away.
    case WM_KILLFOCUS:
        if (hwnd == l.edit || hwnd == l.rate)
            PostMessageW(g.launcher, kRenameMessage, kLeaveName, reinterpret_cast<LPARAM>(hwnd));
        break;
    }
    return CallWindowProcW(l.editProc, hwnd, message, wParam, lParam);
}

void StartRename(size_t index)
{
    if (index >= g.autoGames.size())
        return;
    CloseRename();
    l.edit = CreateWindowExW(0, L"EDIT", g.autoGames[index].name.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, g.launcher, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!l.edit)
    {
        Log(LogLevel::Error, L"Could not rename %ls (error %lu).", g.autoGames[index].name.c_str(), GetLastError());
        return;
    }
    l.renaming = g.autoGames[index].executable;
    l.editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(l.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    SendMessageW(l.edit, EM_SETLIMITTEXT, kMaxNameLength, 0);
    SendMessageW(l.edit, WM_SETFONT, reinterpret_cast<WPARAM>(l.launcherScaling.strong), FALSE);
    // Laid out first, which places the box.
    Refresh();
    ShowWindow(l.edit, SW_SHOW);
    SendMessageW(l.edit, EM_SETSEL, 0, -1);
    SetFocus(l.edit);
}

void UseWindow(Picker picker, const GameWindow& window)
{
    if (picker == Picker::Add)
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

void Run(const Target& target)
{
    switch (target.action)
    {
    case Action::OpenLog: ShellOpen(LogPath()); return;
    case Action::Help: ShellOpen(kHelpUrl); return;
    case Action::Support: ShellOpen(kSupportUrl); return;
    case Action::RunSetup: ShellOpen(l.setup); return;
    case Action::Download: ShellOpen(l.update.url); return;
    case Action::OpenUrl: ShellOpen(target.label); return;
    case Action::AddGame: OpenPicker(Picker::Add); break;
    case Action::PickWindow: OpenPicker(Picker::Session); break;
    case Action::DetectAutomatically:
        g.selectedGame.reset();
        if (g.target)
            StopCapture();
        break;
    case Action::RenameGame:
        StartRename(target.index);
        return;
    case Action::ToggleGame:
        if (target.index < g.autoGames.size())
        {
            auto games = g.autoGames;
            games[target.index].enabled = !games[target.index].enabled;
            SaveGames(std::move(games));
        }
        break;
    case Action::RemoveGame:
        RemoveGame(target.index);
        break;
    case Action::UndoRemove:
        UndoRemove();
        break;
    case Action::DismissNotices:
        ClearNotices();
        break;
    case Action::ShowTab:
        CloseRename();
        l.tab = static_cast<Tab>(target.index);
        l.scroll = 0;
        break;
    case Action::ShowPage:
        l.page = static_cast<Page>(target.index);
        l.scroll = 0;
        break;
    case Action::StartWithWindows:
        SetStartWithWindows(!l.startWithWindows);
        break;
    case Action::AutoSave:
        SetAutoSavePresets(!AutoSavePresets());
        // From here on every change saves as it happens, so changes that were waiting are saved too.
        if (AutoSavePresets())
            FlushPresets(true);
        break;
    case Action::MenuSize:
        SetMenuScale(std::round((MenuScale() + (target.index ? 0.25f : -0.25f)) * 4) / 4);
        break;
    case Action::KeepEffects:
        SetKeepEffectsVisible(!KeepEffectsVisible());
        break;
    case Action::UpdateChecks:
        SetUpdateChecksEnabled(!UpdateChecksEnabled());
        break;
    case Action::FrameRate:
        if (target.index < std::size(kFrameRates))
        {
            CloseRate();
            SetFrameRateLimit(kFrameRates[target.index]);
        }
        else
        {
            if (!l.rate)
                OpenRate();
            if (l.rate)
            {
                SendMessageW(l.rate, EM_SETSEL, 0, -1);
                SetFocus(l.rate);
            }
        }
        break;
    case Action::EffectResolution:
        SetEffectResolution(kEffectResolutions[target.index]);
        break;
    case Action::DepthDetail:
        SetDepthSize(kDepthSizes[target.index]);
        break;
    case Action::DebugInfo:
        SetDebugInfoEnabled(!DebugInfoEnabled());
        break;
    case Action::SetShortcut:
    {
        // Clicking the shortcut that waits for keys stops the wait.
        const bool waiting = l.capturing == static_cast<int>(target.index);
        StopShortcut();
        if (!waiting)
        {
            l.capturing = static_cast<int>(target.index);
            l.shortcutError.clear();
            SuspendHotkeys(true);
        }
        break;
    }
    case Action::ClearShortcut:
    {
        InputHotkeys hotkeys = g.hotkeys;
        hotkeys.*kShortcuts[target.index].member = {};
        l.shortcutError = ChangeHotkeys(hotkeys);
        break;
    }
    case Action::ResetShortcuts:
        if (MessageBoxW(g.launcher, L"Reset every shortcut?\n\nThey go back to the keys Unishade starts with.", L"Unishade",
                        MB_OKCANCEL | MB_ICONWARNING) == IDOK)
            l.shortcutError = ChangeHotkeys(DefaultHotkeys());
        break;
    case Action::DiscordPresence:
        SetDiscordPresenceEnabled(!DiscordPresenceEnabled());
        break;
    }
    Refresh();
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
        hotkeys.*kShortcuts[l.capturing].member = hotkey;
        StopShortcut();
        l.shortcutError = ChangeHotkeys(hotkeys);
    }
    Refresh();
}

void Choose(size_t index)
{
    const Picker picker = l.picker;
    const GameWindow window = l.windows[index];
    ClosePicker();
    UseWindow(picker, window);
    Refresh();
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

// Brings the launcher back from the notification area or the taskbar.
void ShowLauncher()
{
    if (!IsWindowVisible(g.launcher) || IsIconic(g.launcher))
        ShowWindow(g.launcher, SW_RESTORE);
    SetForegroundWindow(l.pickerWindow ? l.pickerWindow : g.launcher);
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

// Scrolls the launcher so a control is in view.
void ScrollTo(RECT rect)
{
    RECT client{};
    GetClientRect(g.launcher, &client);
    const int margin = P(12);
    int scroll = l.scroll;
    if (rect.top < P(3) + margin)
        scroll += rect.top - P(3) - margin;
    else if (rect.bottom > client.bottom - margin)
        scroll += rect.bottom - (client.bottom - margin);
    scroll = std::clamp(scroll, 0, std::max(0, l.height - static_cast<int>(client.bottom)));
    if (scroll == l.scroll)
        return;
    l.scroll = scroll;
    Relayout();
    UpdateHover();
}

// Tab and Shift+Tab move the focus through the controls, and Enter or Space use the focused one. Returns whether
// the key was used.
bool LauncherKey(WPARAM key, LPARAM flags)
{
    if (key == VK_TAB && !l.targets.empty())
    {
        const int count = static_cast<int>(l.targets.size());
        const int current = FindTarget(l.focus);
        const bool back = GetKeyState(VK_SHIFT) < 0;
        int next = current < 0 ? (back ? count - 1 : 0) : (current + (back ? count - 1 : 1)) % count;
        // Past the ones that cannot be used, such as the button for a larger menu at the largest size.
        for (int skipped = 0; skipped < count && !l.targets[next].enabled; ++skipped)
            next = (next + (back ? count - 1 : 1)) % count;
        l.focus = ControlOf(l.targets[next]);
        l.focusShown = true;
        ScrollTo(l.targets[next].rect);
        InvalidateRect(g.launcher, nullptr, FALSE);
        return true;
    }
    // A held key repeats, which should not use the control again.
    const bool repeated = (flags & (1 << 30)) != 0;
    if ((key == VK_RETURN || key == VK_SPACE) && l.focusShown && !repeated)
        if (const int index = FindTarget(l.focus); index >= 0 && l.targets[index].enabled)
        {
            // Copied, since running it lays the window out again.
            const Target target = l.targets[index];
            Run(target);
            return true;
        }
    // F2 renames the game whose control has the focus, like in Explorer.
    if (key == VK_F2)
        if (const int index = FindTarget(l.focus); index >= 0)
            if (const Target& target = l.targets[index];
                target.action == Action::RenameGame || target.action == Action::ToggleGame || target.action == Action::RemoveGame)
            {
                StartRename(target.index);
                return true;
            }
    return false;
}

// Scrolls the picker so a row is in view.
void ScrollPickerTo(RECT rect)
{
    const PickerScaling scaling;
    RECT client{};
    GetClientRect(l.pickerWindow, &client);
    int scroll = l.pickerScroll;
    if (rect.top < P(3))
        scroll += rect.top - P(3);
    else if (rect.bottom > client.bottom)
        scroll += rect.bottom - client.bottom;
    scroll = std::clamp(scroll, 0, std::max(0, l.pickerHeight - static_cast<int>(client.bottom)));
    if (scroll != l.pickerScroll)
    {
        l.pickerScroll = scroll;
        l.pickerHovered = -1;
    }
}

// The arrows, Home and End, Tab and Shift+Tab move the focus through the windows, Enter or Space picks the focused
// one, and Escape closes the picker. Returns whether the key was used.
bool PickerKey(WPARAM key, LPARAM flags)
{
    const int count = static_cast<int>(l.windows.size());
    int row = PickerFocusRow();
    const bool back = key == VK_UP || (key == VK_TAB && GetKeyState(VK_SHIFT) < 0);
    switch (key)
    {
    case VK_ESCAPE:
        ClosePicker();
        return true;
    case VK_RETURN:
    case VK_SPACE:
        // A held key repeats, which should not pick again.
        if (row >= 0 && !(flags & (1 << 30)))
            Choose(static_cast<size_t>(row));
        return true;
    case VK_UP:
    case VK_DOWN:
    case VK_TAB:
        row = row < 0 ? (back ? count - 1 : 0) : std::clamp(row + (back ? -1 : 1), 0, count - 1);
        break;
    case VK_HOME:
        row = 0;
        break;
    case VK_END:
        row = count - 1;
        break;
    default:
        return false;
    }
    if (row < 0 || row >= count)
        return true;
    l.pickerFocus = l.windows[row].window;
    ScrollPickerTo(l.choices[row].rect);
    InvalidateRect(l.pickerWindow, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK PickerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        PaintPicker(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        if (!l.pickerTracking)
        {
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
            l.pickerTracking = TrackMouseEvent(&track);
        }
        const int hovered = ChoiceAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
        if (hovered != l.pickerHovered)
        {
            l.pickerHovered = hovered;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        l.pickerTracking = false;
        if (l.pickerHovered >= 0)
        {
            l.pickerHovered = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        const PickerScaling scaling;
        RECT client{};
        GetClientRect(hwnd, &client);
        const int scroll = std::clamp(l.pickerScroll - GET_WHEEL_DELTA_WPARAM(wParam) * P(52) / WHEEL_DELTA, 0,
                                      std::max(0, l.pickerHeight - static_cast<int>(client.bottom)));
        if (scroll != l.pickerScroll)
        {
            l.pickerScroll = scroll;
            l.pickerHovered = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && l.pickerHovered >= 0)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (const int index = ChoiceAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) }); index >= 0)
        {
            l.pickerPressed = l.windows[index].window;
            SetCapture(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
    {
        const HWND pressed = l.pickerPressed;
        l.pickerPressed = nullptr;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        if (const int index = ChoiceAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
            index >= 0 && pressed && l.windows[index].window == pressed)
            Choose(static_cast<size_t>(index));
        return 0;
    }
    case WM_CAPTURECHANGED:
        l.pickerPressed = nullptr;
        return 0;
    case WM_KEYDOWN:
        if (PickerKey(wParam, lParam))
            return 0;
        break;
    // A game opened while the picker was open shows up when the picker is activated again. A click that activates
    // it while the rows move is dropped, so it cannot pick the wrong window.
    case WM_MOUSEACTIVATE:
        if (LOWORD(lParam) == HTCLIENT && ListWindows())
        {
            ResizePicker();
            return MA_ACTIVATEANDEAT;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_ACTIVE && ListWindows())
            ResizePicker();
        break;
    // Moved to a monitor with another scaling.
    case WM_DPICHANGED:
    {
        Scale(l.pickerScaling, HIWORD(wParam));
        ClearIcons();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizePicker();
        return 0;
    }
    case WM_CLOSE:
        ClosePicker();
        return 0;
    case WM_DESTROY:
        l.pickerWindow = nullptr;
        l.picker = Picker::None;
        l.pickerTracking = false;
        l.pickerFocus = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == l.taskbarCreated && l.taskbarCreated)
    {
        AddTrayIcon();
        return 0;
    }
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(hwnd, &paint);
        Paint(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        if (!l.tracking)
        {
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
            l.tracking = TrackMouseEvent(&track);
        }
        SetHovered(TargetAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) }));
        return 0;
    }
    case WM_MOUSELEAVE:
        l.tracking = false;
        SetHovered(-1);
        return 0;
    case WM_MOUSEWHEEL:
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int scroll = std::clamp(l.scroll - GET_WHEEL_DELTA_WPARAM(wParam) * P(60) / WHEEL_DELTA, 0,
                                      std::max(0, l.height - static_cast<int>(client.bottom)));
        if (scroll != l.scroll)
        {
            l.scroll = scroll;
            Relayout();
            UpdateHover();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && l.hovered >= 0)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        KillTimer(hwnd, kTipTimer);
        if (l.tip)
        {
            l.tip.reset();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // Clicking outside the rename box or the frame rate box leaves it.
        if (l.edit || l.rate)
            SetFocus(hwnd);
        // Clicking hides the focus until a key is used again.
        if (l.focusShown)
        {
            l.focusShown = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (const int index = TargetAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) }); index >= 0)
        {
            l.pressed = ControlOf(l.targets[index]);
            l.focus = l.pressed;
            SetCapture(hwnd);
        }
        // Clicking anything but the shortcut that waits for keys stops the wait. Its own button does when let go.
        if (l.capturing >= 0 && l.pressed != Control{ Action::SetShortcut, static_cast<size_t>(l.capturing) })
        {
            StopShortcut();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    {
        const std::optional<Control> pressed = l.pressed;
        l.pressed.reset();
        if (GetCapture() == hwnd)
            ReleaseCapture();
        if (const int index = TargetAt({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
            index >= 0 && pressed == ControlOf(l.targets[index]))
        {
            // Copied, since running it lays the window out again.
            const Target target = l.targets[index];
            Run(target);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        l.pressed.reset();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (l.capturing >= 0)
        {
            TakeShortcut(static_cast<UINT>(wParam));
            return 0;
        }
        if (message == WM_KEYDOWN && LauncherKey(wParam, lParam))
            return 0;
        break;
    // Alt would open the window's menu while a shortcut waits for keys.
    case WM_SYSCOMMAND:
        if (l.capturing >= 0 && (wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    // The focus only shows while the launcher has it, and a shortcut only waits for keys there.
    case WM_KILLFOCUS:
        StopShortcut();
        [[fallthrough]];
    case WM_SETFOCUS:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_TIMER:
        if (wParam == kUndoTimer)
        {
            ForgetRemoved();
            Refresh();
        }
        else if (wParam == kTrayTimer)
        {
            KillTimer(hwnd, kTrayTimer);
            AddTrayIcon();
        }
        else if (wParam == kTipTimer)
        {
            KillTimer(hwnd, kTipTimer);
            if (l.hovered >= 0 && l.targets[l.hovered].tip)
            {
                l.tip = ControlOf(l.targets[l.hovered]);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wParam && !IsIconic(hwnd))
            Refresh();
        break;
    case WM_CLOSE:
        if (!ConfirmQuit())
            return 0;
        break;
    case WM_SIZE:
    {
        const bool restored = l.minimized && wParam != SIZE_MINIMIZED;
        l.minimized = wParam == SIZE_MINIMIZED;
        // Minimized, it waits in the notification area while the effects keep running.
        if (l.minimized && l.trayAdded)
        {
            ClosePicker();
            CloseRename();
            ShowWindow(hwnd, SW_HIDE);
        }
        if (restored)
            Refresh();
        return 0;
    }
    case kRenameMessage:
        // Not when it was sent by a box that is already closed.
        if (l.edit && reinterpret_cast<HWND>(lParam) == l.edit)
            EndRename(wParam);
        else if (l.rate && reinterpret_cast<HWND>(lParam) == l.rate)
            EndRate(wParam);
        return 0;
    // Typing again hides what was wrong with the name.
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && l.edit && reinterpret_cast<HWND>(lParam) == l.edit && !l.renameProblem.empty())
        {
            l.renameProblem.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_CTLCOLOREDIT:
    {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, Gdi(theme::kText));
        SetBkColor(dc, Gdi(theme::kCardHover));
        if (!l.editBrush)
            l.editBrush = CreateSolidBrush(Gdi(theme::kCardHover));
        return reinterpret_cast<LRESULT>(l.editBrush);
    }
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
    case WM_DPICHANGED:
    {
        Scale(l.launcherScaling, HIWORD(wParam));
        if (l.edit)
            SendMessageW(l.edit, WM_SETFONT, reinterpret_cast<WPARAM>(l.launcherScaling.strong), TRUE);
        if (l.rate)
            SendMessageW(l.rate, WM_SETFONT, reinterpret_cast<WPARAM>(l.launcherScaling.body), TRUE);
        ClearIcons();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Resize();
        return 0;
    }
    case WM_DESTROY:
        RemoveTrayIcon();
        // Destroyed with the launcher.
        l.edit = nullptr;
        l.rate = nullptr;
        g.launcher = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void LoadLogo()
{
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    const HGLOBAL resource = info ? LoadResource(nullptr, info) : nullptr;
    if (!resource)
        return;
    l.logoStream.attach(SHCreateMemStream(static_cast<const BYTE*>(LockResource(resource)), SizeofResource(nullptr, info)));
    if (!l.logoStream)
        return;
    l.logo.reset(Gdiplus::Bitmap::FromStream(l.logoStream.get()));
    if (l.logo && l.logo->GetLastStatus() != Gdiplus::Ok)
        l.logo.reset();
}
} // namespace

void CreateLauncher()
{
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&l.gdiplus, &input, nullptr);
    LoadLogo();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kLauncherClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = PickerProc;
    wc.lpszClassName = kPickerClass;
    RegisterClassExW(&wc);
    // Clipping keeps the launcher's drawing off the rename box.
    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    l.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g.launcher = CreateWindowExW(0, kLauncherClass, L"Unishade", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    winrt::check_bool(g.launcher != nullptr);
    DarkFrame(g.launcher);
    // Explorer runs without administrator rights, so Unishade running with them must accept its messages.
    ChangeWindowMessageFilterEx(g.launcher, l.taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g.launcher, kTrayMessage, MSGFLT_ALLOW, nullptr);
    AddTrayIcon();

    Scale(l.launcherScaling, GetDpiForWindow(g.launcher));
    Refresh();

    // Centered on the monitor the window opened on.
    RECT window{};
    GetWindowRect(g.launcher, &window);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(g.launcher, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    SetWindowPos(g.launcher, nullptr, (monitor.rcWork.left + monitor.rcWork.right - width) / 2,
                 (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Started with Windows, it waits in the notification area, or on the taskbar until there is one.
    if (!StartedWithWindows())
        ShowWindow(g.launcher, SW_SHOWNORMAL);
    else if (!l.trayAdded)
        ShowWindow(g.launcher, SW_SHOWMINNOACTIVE);
}

void UpdateLauncher()
{
    // Nothing is drawn while the launcher is hidden or minimized. Showing it describes it again.
    if (g.launcher && IsWindowVisible(g.launcher) && !IsIconic(g.launcher) && CurrentShown() != l.shown)
        Refresh();
}

void DestroyLauncher()
{
    ForgetRemoved();
    if (g.launcher)
        DestroyWindow(g.launcher);
    g.launcher = nullptr;
    DeleteFonts(l.launcherScaling);
    DeleteFonts(l.pickerScaling);
    ClearIcons();
    if (l.editBrush)
        DeleteObject(l.editBrush);
    l.editBrush = nullptr;
    l.logo.reset();
    l.logoStream = nullptr;
    if (l.gdiplus)
        Gdiplus::GdiplusShutdown(l.gdiplus);
    l.gdiplus = 0;
}
