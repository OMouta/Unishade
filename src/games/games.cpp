#include "games/games.h"
#include "app/config.h"
#include "games/game_list.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

namespace
{
// The last part of a Unix or Windows path.
std::string BaseName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
} // namespace

std::vector<AutoGame> DefaultAutoGames()
{
#ifdef __APPLE__
    return { { "RobloxPlayer", "Roblox" } };
#elif defined(_WIN32)
    return {};
#else
    // Sober runs Roblox's Android version natively. Roblox's Windows version also runs through Wine.
    return { { "sober", "Roblox" }, { "RobloxPlayerBeta.exe", "Roblox" } };
#endif
}

bool MatchesProcess(const AutoGame& game, const std::string& executable, const std::string& command)
{
    if (!game.executable.empty() && (game.executable.back() == '/' || game.executable.back() == '\\'))
    {
        if (executable.size() <= game.executable.size())
            return false;
#ifdef _WIN32
        return !_strnicmp(executable.c_str(), game.executable.c_str(), game.executable.size());
#else
        return !executable.compare(0, game.executable.size(), game.executable);
#endif
    }
#ifdef _WIN32
    // Windows paths take either slash and ignore case.
    if (game.executable.find_first_of("/\\") != std::string::npos)
        return !strcasecmp(executable.c_str(), game.executable.c_str());
#else
    if (game.executable.find('/') != std::string::npos)
        return executable == game.executable;
#endif
    return !strcasecmp(BaseName(executable).c_str(), game.executable.c_str()) ||
           (!command.empty() && !strcasecmp(BaseName(command).c_str(), game.executable.c_str()));
}

int MatchingGame(std::span<const AutoGame> games, const std::string& executable, const std::string& command)
{
    for (size_t i = 0; i < games.size(); ++i)
        if (games[i].enabled && MatchesProcess(games[i], executable, command))
            return static_cast<int>(i);
    return -1;
}

std::vector<AutoGame> LoadAutoGames(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return DefaultAutoGames();
    const std::string text = ReadFile(path);
    std::vector<GameListEntry> entries;
    try
    {
        entries = ParseGameList(text);
    }
    catch (const std::exception& e)
    {
        // The host falls back to the default list, and its next change to the list replaces the file, so the
        // damaged one is kept beside it for the user to repair, as on Windows.
        const std::filesystem::path kept = path.string() + ".damaged";
        if (WriteFile(kept, text))
            throw std::runtime_error(std::string(e.what()) + ". The old list was kept as " + kept.string() + ".");
        throw std::runtime_error(std::string(e.what()) + ". The old list could not be kept as " + kept.string() + ".");
    }
    std::vector<AutoGame> games;
    for (GameListEntry& entry : entries)
        games.push_back({ std::move(entry.executable), std::move(entry.name), entry.enabled });
    return games;
}

bool SaveAutoGames(const std::filesystem::path& path, std::span<const AutoGame> games)
{
    std::vector<GameListEntry> entries;
    for (const AutoGame& game : games)
        entries.push_back({ game.executable, game.name, game.enabled });
    return WriteFile(path, FormatGameList(entries));
}

bool AddAutoGame(std::vector<AutoGame>& games, const platform::Window& window)
{
    const std::string executable = platform::ProcessExecutable(window.pid);
    if (executable.empty())
        return false;
    const std::string command = platform::ProcessCommand(window.pid);
    for (AutoGame& game : games)
        if (MatchesProcess(game, executable, command))
        {
            game.enabled = true;
            return true;
        }
    // Wine and Proton games are saved by their .exe, since the executable is Wine's own.
    const std::string windowsExe = BaseName(command);
    const bool wine = windowsExe.size() > 4 && !strcasecmp(windowsExe.c_str() + windowsExe.size() - 4, ".exe");
    std::string name = window.title;
    std::replace(name.begin(), name.end(), '\r', ' ');
    std::replace(name.begin(), name.end(), '\n', ' ');
    games.push_back({ wine ? windowsExe : executable, name.empty() ? BaseName(executable) : name });
    return true;
}

std::string AddAutoGame(std::vector<AutoGame>& games, std::string program)
{
    const std::string file = program;
    std::string name = BaseName(program);
    if (const size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
        name.erase(dot);
#ifdef __APPLE__
    // An app is a folder, with the program that runs somewhere inside it.
    if (program.ends_with(".app"))
        program += '/';
#elif !defined(_WIN32)
    // Wine and Proton games are saved by their .exe, since the executable is Wine's own.
    if (program.size() > 4 && !strcasecmp(program.c_str() + program.size() - 4, ".exe"))
        program = BaseName(program);
#endif
    // Already there, perhaps by its folder.
    for (AutoGame& game : games)
        if (game.executable == program || MatchesProcess(game, file, {}))
        {
            game.enabled = true;
            return game.name;
        }
    games.push_back({ program, name });
    return name;
}

std::optional<platform::Window> FindGameTarget(const std::optional<platform::Window>& selection, std::span<const AutoGame> games,
                                              platform::WindowId preferred, std::vector<GameProcess>* windowless)
{
    if (selection)
        return platform::WindowExists(*selection) ? selection : std::nullopt;
    if (std::none_of(games.begin(), games.end(), [](const AutoGame& game) { return game.enabled; }))
        return std::nullopt;

    std::vector<GameProcess> processes;
    for (const platform::Process& process : platform::ListProcesses())
        if (const int index = MatchingGame(games, process.executable, process.command); index >= 0)
            processes.push_back({ process.pid, size_t(index) });
    if (processes.empty())
        return std::nullopt;

    std::optional<platform::Window> first;
    std::vector<bool> found(processes.size());
    for (platform::Window window : platform::ListWindows())
        for (size_t i = 0; i < processes.size(); ++i)
            if (window.pid == processes[i].pid)
            {
                found[i] = true;
                window.title = games[processes[i].game].name;
                if (window.id == preferred)
                    return window;
                if (!first)
                    first = window;
                break;
            }
    if (windowless)
        for (size_t i = 0; i < processes.size(); ++i)
            if (!found[i])
                windowless->push_back(processes[i]);
    return first;
}
