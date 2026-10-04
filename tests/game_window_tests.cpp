#include "../src/game_integration.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s (Windows error %lu)\n", message, GetLastError());
    return condition;
}

HWND Window(const wchar_t* title, DWORD style = WS_VISIBLE | WS_OVERLAPPEDWINDOW, DWORD extended = 0, HWND owner = nullptr)
{
    return CreateWindowExW(extended, L"UnishadeTestGame", title, style, 0, 0, 640, 480, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void RegisterWindows()
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"UnishadeTestGame";
    RegisterClassW(&windowClass);
}

int RunFixtures(const wchar_t* readyName)
{
    RegisterWindows();
    const HWND first = Window(L"Game A");
    const HWND second = Window(L"Game B");
    Window(L"Tool", WS_VISIBLE | WS_POPUP, WS_EX_TOOLWINDOW);
    Window(L"Owned popup", WS_VISIBLE | WS_POPUP, 0, first);
    Window(L"Hidden", WS_OVERLAPPEDWINDOW);
    Window(L"");
    const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    if (!first || !second || !ready)
        return 1;
    SetEvent(ready);
    CloseHandle(ready);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

void StopFixture(PROCESS_INFORMATION& process)
{
    if (!process.hProcess)
        return;
    PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    process = {};
}

struct Fixtures
{
    HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = nullptr;
    HANDLE ready = nullptr;
    PROCESS_INFORMATION process{};
    PROCESS_INFORMATION otherProcess{};
    PROCESS_INFORMATION sameNameProcess{};
    HWND ownWindow = nullptr;
    fs::path executable;
    fs::path otherExecutable;
    fs::path sameNameExecutable;
    fs::path config;

    ~Fixtures()
    {
        StopFixture(process);
        StopFixture(otherProcess);
        StopFixture(sameNameProcess);
        if (ready)
            CloseHandle(ready);
        if (ownWindow)
            DestroyWindow(ownWindow);
        SetThreadDesktop(previous);
        if (desktop)
            CloseDesktop(desktop);
        std::error_code ignored;
        if (!config.empty())
            fs::remove(config, ignored);
        for (const fs::path& copy : { sameNameExecutable, otherExecutable, executable })
            if (!copy.empty())
            {
                fs::remove(copy, ignored);
                fs::remove(copy.parent_path(), ignored);
            }
    }
};

// A fixture window by title, including the ones ListGameWindows leaves out.
HWND FixtureWindow(DWORD process, const wchar_t* title)
{
    struct Search
    {
        DWORD process;
        const wchar_t* title;
        HWND window;
    } search{ process, title, nullptr };
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto& wanted = *reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            wchar_t className[64]{};
            wchar_t text[64]{};
            GetClassNameW(window, className, 64);
            GetWindowTextW(window, text, 64);
            if (owner != wanted.process || wcscmp(className, L"UnishadeTestGame") != 0 || wcscmp(text, wanted.title) != 0)
                return TRUE;
            wanted.window = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool StartFixture(const fs::path& self, const fs::path& executable, std::wstring& desktopName,
                  const std::wstring& readyName, HANDLE ready, PROCESS_INFORMATION& process)
{
    fs::create_directories(executable.parent_path());
    fs::copy_file(self, executable, fs::copy_options::overwrite_existing);
    ResetEvent(ready);
    std::wstring command = L"\"" + executable.wstring() + L"\" --fixture " + readyName;
    STARTUPINFOW startup{ sizeof(startup) };
    startup.lpDesktop = desktopName.data();
    return Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) &&
                     WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0,
                 "start game windows in another process");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--fixture") == 0)
        return RunFixtures(argv[2]);

    // All test windows stay on an inactive desktop. Never switch the user's desktop or send global input.
    Fixtures fixture;
    std::wstring desktopName = L"UnishadeWindowTests-" + std::to_wstring(GetCurrentProcessId());
    fixture.desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!Check(fixture.desktop && SetThreadDesktop(fixture.desktop), "create an isolated test desktop"))
        return 1;
    RegisterWindows();
    fixture.ownWindow = Window(L"Unishade");

    const std::wstring readyName = L"Local\\" + desktopName;
    fixture.ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    wchar_t self[32768]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    fixture.executable = fs::path(self).parent_path() / desktopName / L"RobloxPlayerBeta.exe";
    if (!StartFixture(self, fixture.executable, desktopName, readyName, fixture.ready, fixture.process))
        return 1;

    const auto games = ListGameWindows();
    if (!Check(games.size() == 2 && games[0].name == L"Game A" && games[1].name == L"Game B",
               "list game windows without the host, tools, owned popups, hidden or untitled windows"))
        return 1;
    bool ok = true;
    fixture.config = fixture.executable.parent_path() / L"games.ini";
    auto autoGames = LoadAutoGames(fixture.config);
    const auto roblox = std::find_if(autoGames.begin(), autoGames.end(), [](const AutoGame& game) {
        return game.name == L"Roblox";
    });
    ok &= Check(roblox != autoGames.end() && roblox->enabled && roblox->executable == L"RobloxPlayerBeta.exe",
                "Roblox is enabled by default");
    std::vector<AutoGame> studioGames = DefaultAutoGames();
    AddInstalledStudio(studioGames);
    const fs::path studioExecutable = InstalledStudioExecutable();
    const auto studio = std::find_if(studioGames.begin(), studioGames.end(), [](const AutoGame& game) {
        return game.name == L"Roblox Studio";
    });
    ok &= Check(studioExecutable.empty()
                    ? studio == studioGames.end()
                    : studio != studioGames.end() && !studio->enabled && studio->executable == L"RobloxStudioBeta.exe" &&
                          MatchesExecutable(*studio, L"C:\\Users\\Player\\Roblox\\Versions\\version-new\\RobloxStudioBeta.exe") &&
                          fs::exists(studioExecutable),
                "installed Roblox Studio is added disabled, follows versioned folders and exposes its installed icon path");
    std::erase_if(autoGames, [](const AutoGame& game) { return game.name == L"Roblox Studio"; });
    for (const GameWindow& game : games)
    {
        const auto selected = FindGameTarget(game, autoGames);
        ok &= Check(selected && selected->window == game.window && selected->processId == fixture.process.dwProcessId,
                    "attach to the selected game window");
    }
    GameWindow wrongProcess = games[0];
    wrongProcess.processId = GetCurrentProcessId();
    ok &= Check(!FindGameTarget(wrongProcess, autoGames), "reject a window belonging to a different process");
    ok &= Check(FindGameTarget(std::nullopt, autoGames).has_value(), "keep automatic Roblox detection");
    const auto matched = MatchGameWindow(games[0].window, autoGames);
    ok &= Check(matched && matched->window == games[0].window && matched->name == L"Roblox" &&
                    matched->processId == fixture.process.dwProcessId,
                "match one window of a saved game");
    for (const wchar_t* title : { L"Tool", L"Owned popup", L"Hidden", L"" })
    {
        const HWND window = FixtureWindow(fixture.process.dwProcessId, title);
        ok &= Check(window && !MatchGameWindow(window, autoGames), "do not match tools, owned popups, hidden or untitled windows");
    }
    ok &= Check(!MatchGameWindow(fixture.ownWindow, autoGames) && !MatchGameWindow(nullptr, autoGames),
                "do not match the host's own windows or no window");
    SendMessageW(games[1].window, WM_CLOSE, 0, 0);
    ok &= Check(!FindGameTarget(games[1], autoGames), "a closed selection does not fall back to another game");
    ok &= Check(FindGameTarget(std::nullopt, autoGames).has_value(), "automatic detection remains available after a selected game closes");
    autoGames[0].enabled = false;
    ok &= Check(!FindGameTarget(std::nullopt, autoGames) && !MatchGameWindow(games[0].window, autoGames), "disabled Roblox is not detected");
    AddAutoGame(autoGames, games[0]);
    ok &= Check(autoGames.size() == 1 && autoGames[0].enabled && !autoGames[0].executable.has_parent_path(),
                "adding Roblox again re-enables its default entry without pinning its installation folder");

    fixture.otherExecutable = fixture.executable.parent_path() / L"Other game \u6e38.exe";
    if (!StartFixture(self, fixture.otherExecutable, desktopName, readyName, fixture.ready, fixture.otherProcess))
        return 1;
    const auto allWindows = ListGameWindows();
    const auto other = std::find_if(allWindows.begin(), allWindows.end(), [&](const GameWindow& window) {
        return window.processId == fixture.otherProcess.dwProcessId;
    });
    if (!Check(other != allWindows.end(), "find a second game's window"))
        return 1;
    AddAutoGame(autoGames, *other);
    AddAutoGame(autoGames, *other);
    ok &= Check(autoGames.size() == 2 && autoGames[1].executable == fixture.otherExecutable,
                "add a running game by executable without duplicates");
    autoGames[0].enabled = false;
    autoGames[1].name = L"Other game \u6e38";
    SaveAutoGames(fixture.config, autoGames);
    autoGames = LoadAutoGames(fixture.config);
    ok &= Check(autoGames.size() == 2 && !autoGames[0].enabled && autoGames[1].enabled &&
                    autoGames[1].executable == fixture.otherExecutable && autoGames[1].name == L"Other game \u6e38",
                "persist executable paths, Unicode names and enabled state across restarts");
    const HANDLE lockedConfig = CreateFileW(fixture.config.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!Check(lockedConfig != INVALID_HANDLE_VALUE, "lock the saved list for a write-failure test"))
        return 1;
    bool saveFailed = false;
    try
    {
        SaveAutoGames(fixture.config, {});
    }
    catch (const std::exception&)
    {
        saveFailed = true;
    }
    CloseHandle(lockedConfig);
    ok &= Check(saveFailed && LoadAutoGames(fixture.config).size() == 2 && !fs::exists(fixture.config.wstring() + L".tmp"),
                "a failed save preserves the previous game list and removes the temporary file");
    const auto detectedOther = FindGameTarget(std::nullopt, autoGames);
    ok &= Check(detectedOther && detectedOther->processId == fixture.otherProcess.dwProcessId,
                "automatically detect an added game while Roblox is disabled");
    autoGames[0].enabled = true;
    const auto preferredOther = FindGameTarget(std::nullopt, autoGames, other->window);
    const auto preferredRoblox = FindGameTarget(std::nullopt, autoGames, games[0].window);
    ok &= Check(preferredOther && preferredOther->window == other->window &&
                    preferredRoblox && preferredRoblox->window == games[0].window,
                "prefer the foreground game when multiple saved games are running");
    const auto manual = FindGameTarget(games[0], autoGames, other->window);
    ok &= Check(manual && manual->window == games[0].window, "manual selection keeps its chosen window");

    fixture.sameNameExecutable = fixture.executable.parent_path() / L"different-folder" / fixture.otherExecutable.filename();
    if (!StartFixture(self, fixture.sameNameExecutable, desktopName, readyName, fixture.ready, fixture.sameNameProcess))
        return 1;
    autoGames.erase(autoGames.begin());
    const auto sameNames = ListGameWindows();
    const auto impostor = std::find_if(sameNames.begin(), sameNames.end(), [&](const GameWindow& window) {
        return window.processId == fixture.sameNameProcess.dwProcessId;
    });
    const auto matchedPath = FindGameTarget(std::nullopt, autoGames, impostor == sameNames.end() ? nullptr : impostor->window);
    ok &= Check(matchedPath && matchedPath->processId == fixture.otherProcess.dwProcessId,
                "do not attach to another executable with the same filename in a different folder");
    const auto matchedOther = MatchGameWindow(other->window, autoGames);
    ok &= Check(matchedOther && matchedOther->name == L"Other game \u6e38" && impostor != sameNames.end() &&
                    !MatchGameWindow(impostor->window, autoGames),
                "match a window by the saved executable's full path");
    for (const GameWindow& window : allWindows)
        if (window.processId == fixture.otherProcess.dwProcessId)
            SendMessageW(window.window, WM_CLOSE, 0, 0);
    ok &= Check(!FindGameTarget(std::nullopt, autoGames), "do not fall back to a removed game or a different executable");
    StopFixture(fixture.otherProcess);
    if (!StartFixture(self, fixture.otherExecutable, desktopName, readyName, fixture.ready, fixture.otherProcess))
        return 1;
    const auto reopened = FindGameTarget(std::nullopt, autoGames);
    ok &= Check(reopened && reopened->processId == fixture.otherProcess.dwProcessId,
                "detect a saved executable again after its process restarts");
    SaveAutoGames(fixture.config, {});
    ok &= Check(LoadAutoGames(fixture.config).empty(), "an empty saved list stays empty after a restart");
    return ok ? 0 : 1;
}
