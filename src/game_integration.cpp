#include "game_integration.h"
#include "game_list.h"
#include "text.h"

#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace
{
// Whether a game could be drawing in the window: visible, not owned, not a tool window, not cloaked, with a client area
// and a title. The host's own windows never count.
bool IsGameWindow(HWND window, DWORD process)
{
    if (process == GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    RECT client{};
    return GetClientRect(window, &client) && client.right > 0 && client.bottom > 0 && GetWindowTextLengthW(window) > 0;
}

struct Enumeration
{
    // Only these processes' windows are looked at, when set.
    const std::vector<DWORD>* processes = nullptr;
    std::vector<GameWindow> windows;
};

// Game windows from front to back, with their titles.
std::vector<GameWindow> EnumerateGameWindows(const std::vector<DWORD>* processes)
{
    Enumeration enumeration{ processes };
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto& found = *reinterpret_cast<Enumeration*>(param);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (found.processes && std::find(found.processes->begin(), found.processes->end(), process) == found.processes->end())
                return TRUE;
            if (!IsGameWindow(window, process))
                return TRUE;
            std::wstring name(static_cast<size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            name.resize(GetWindowTextW(window, name.data(), static_cast<int>(name.size())));
            if (!name.empty())
                found.windows.push_back({ window, std::move(name), process });
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&enumeration));
    return std::move(enumeration.windows);
}
} // namespace

fs::path ProcessExecutable(DWORD processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        throw std::system_error(GetLastError(), std::system_category(), "Could not identify the game executable");
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    const DWORD error = GetLastError();
    CloseHandle(process);
    if (!success)
        throw std::system_error(error, std::system_category(), "Could not identify the game executable");
    path.resize(size);
    return path;
}

bool MatchesExecutable(const AutoGame& game, const fs::path& executable)
{
    return _wcsicmp((game.executable.has_parent_path() ? executable : executable.filename()).c_str(), game.executable.c_str()) == 0;
}

std::vector<AutoGame> DefaultAutoGames()
{
    return { { L"RobloxPlayerBeta.exe", L"Roblox" } };
}

fs::path InstalledStudioExecutable()
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)))
        return {};
    const fs::path versions = fs::path(localAppData) / L"Roblox" / L"Versions";
    CoTaskMemFree(localAppData);

    std::error_code error;
    for (fs::directory_iterator it(versions, error), end; !error && it != end; it.increment(error))
    {
        if (error)
            break;
        if (!it->is_directory(error))
            continue;
        if (error)
        {
            error.clear();
            continue;
        }
        const fs::path executable = it->path() / L"RobloxStudioBeta.exe";
        if (!fs::is_regular_file(executable, error))
        {
            error.clear();
            continue;
        }
        return executable;
    }
    return {};
}

bool AddInstalledStudio(std::vector<AutoGame>& games)
{
    const fs::path executable = InstalledStudioExecutable();
    if (executable.empty())
        return false;
    const auto existing = std::find_if(games.begin(), games.end(), [](const AutoGame& game) {
        return _wcsicmp(game.executable.filename().c_str(), L"RobloxStudioBeta.exe") == 0;
    });
    if (existing != games.end())
    {
        if (existing->executable == L"RobloxStudioBeta.exe")
            return false;
        existing->executable = L"RobloxStudioBeta.exe";
        return true;
    }
    games.push_back({ L"RobloxStudioBeta.exe", L"Roblox Studio", false });
    return true;
}

std::vector<AutoGame> LoadAutoGames(const fs::path& path)
{
    if (!fs::exists(path))
    {
        auto games = DefaultAutoGames();
        AddInstalledStudio(games);
        return games;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read the saved game list");
    std::vector<AutoGame> games;
    for (const GameListEntry& entry : ParseGameList(std::string{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() }))
        games.push_back({ Wide(entry.executable), Wide(entry.name), entry.enabled });
    return games;
}

void SaveAutoGames(const fs::path& path, std::span<const AutoGame> games)
{
    std::vector<GameListEntry> entries;
    for (const AutoGame& game : games)
        entries.push_back({ Utf8(game.executable.wstring()), Utf8(game.name), game.enabled });
    const std::string text = FormatGameList(entries);
    const fs::path temporary = path.wstring() + L".tmp";
    try
    {
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(temporary, std::ios::binary | std::ios::trunc);
        output << text;
        output.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Could not save the game list");
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

void AddAutoGame(std::vector<AutoGame>& games, const GameWindow& window)
{
    if (!GameWindowExists(window))
        throw std::runtime_error("The game window has closed. Open it again before adding it");
    const fs::path executable = ProcessExecutable(window.processId);
    for (AutoGame& game : games)
        if (MatchesExecutable(game, executable))
        {
            game.enabled = true;
            return;
        }
    std::wstring name = window.name;
    std::replace(name.begin(), name.end(), L'\r', L' ');
    std::replace(name.begin(), name.end(), L'\n', L' ');
    games.push_back({ executable, std::move(name) });
}

std::vector<GameWindow> ListGameWindows()
{
    std::vector<GameWindow> windows = EnumerateGameWindows(nullptr);
    std::sort(windows.begin(), windows.end(), [](const GameWindow& a, const GameWindow& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return windows;
}

std::optional<GameWindow> FindGameTarget(const std::optional<GameWindow>& selection, std::span<const AutoGame> games, HWND preferredWindow)
{
    if (selection)
        return GameWindowExists(*selection) ? selection : std::nullopt;
    if (std::none_of(games.begin(), games.end(), [](const AutoGame& game) { return game.enabled; }))
        return std::nullopt;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return std::nullopt;
    std::vector<DWORD> processes;
    std::vector<size_t> matches; // the game each process runs
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        for (size_t i = 0; i < games.size(); ++i)
        {
            const AutoGame& game = games[i];
            if (!game.enabled || _wcsicmp(entry.szExeFile, game.executable.filename().c_str()) != 0)
                continue;
            if (game.executable.has_parent_path())
            {
                try
                {
                    if (_wcsicmp(ProcessExecutable(entry.th32ProcessID).c_str(), game.executable.c_str()) != 0)
                        continue;
                }
                catch (const std::system_error&)
                {
                    // A process can exit or deny metadata access while its window is still listed.
                    continue;
                }
            }
            processes.push_back(entry.th32ProcessID);
            matches.push_back(i);
            break;
        }
    CloseHandle(snapshot);
    if (processes.empty())
        return std::nullopt;
    std::optional<GameWindow> first;
    for (GameWindow window : EnumerateGameWindows(&processes))
    {
        const auto process = std::find(processes.begin(), processes.end(), window.processId);
        window.name = games[matches[static_cast<size_t>(process - processes.begin())]].name;
        if (window.window == preferredWindow)
            return window;
        if (!first)
            first = std::move(window);
    }
    return first;
}

std::optional<GameWindow> MatchGameWindow(HWND window, std::span<const AutoGame> games)
{
    DWORD process = 0;
    if (!window || !GetWindowThreadProcessId(window, &process) || !IsGameWindow(window, process) ||
        std::none_of(games.begin(), games.end(), [](const AutoGame& game) { return game.enabled; }))
        return std::nullopt;
    fs::path executable;
    try
    {
        executable = ProcessExecutable(process);
    }
    catch (const std::system_error&)
    {
        return std::nullopt;
    }
    for (const AutoGame& game : games)
        if (game.enabled && MatchesExecutable(game, executable))
            return GameWindow{ window, game.name, process };
    return std::nullopt;
}

bool GameWindowExists(const GameWindow& game)
{
    DWORD process = 0;
    return IsWindow(game.window) && GetWindowThreadProcessId(game.window, &process) && process == game.processId;
}
