#include "games.h"
#include "config.h"
#include "game_list.h"

#include <algorithm>
#include <stdexcept>
#include <strings.h>

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
#else
    // Sober runs Roblox's Android version natively. Roblox's Windows version also runs through Wine.
    return { { "sober", "Roblox" }, { "RobloxPlayerBeta.exe", "Roblox" } };
#endif
}

bool MatchesProcess(const AutoGame& game, const std::string& executable, const std::string& command)
{
    if (game.executable.find('/') != std::string::npos)
        return executable == game.executable;
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
