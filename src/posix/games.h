#pragma once

#include "names.h"
#include "platform.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The same games.ini as on Windows.
struct AutoGame
{
    std::string executable; // a filename such as RobloxPlayer, or a full path
    std::string name;
    bool enabled = true;
};

// The games found automatically when games.ini does not exist yet.
std::vector<AutoGame> DefaultAutoGames();
// Throws std::runtime_error when the file is damaged, after keeping a copy as games.ini.damaged, since the next
// save replaces it. The message says where the copy is.
std::vector<AutoGame> LoadAutoGames(const std::filesystem::path& path);
bool SaveAutoGames(const std::filesystem::path& path, std::span<const AutoGame> games);

// Games saved with a folder match that exact executable. Games saved by filename match it in any folder, and
// also match the first argument of the command line, which names the .exe of games that run through Wine or
// Proton. Filenames are compared without case.
bool MatchesProcess(const AutoGame& game, const std::string& executable, const std::string& command);
// The index of the first enabled game the process matches, or -1.
int MatchingGame(std::span<const AutoGame> games, const std::string& executable, const std::string& command);

// Adds the game that owns the window, or turns it back on. Returns false when its process is gone.
bool AddAutoGame(std::vector<AutoGame>& games, const platform::Window& window);

// A saved game's running process, by its index in the list.
struct GameProcess
{
    int pid;
    size_t game;
};

// An empty selection finds any enabled saved game, preferring the window in front. A closed selection does not
// attach to another game. Looks through every process, so the host only does it now and then. Processes of saved
// games that have no window to attach to are added to windowless.
std::optional<platform::Window> FindGameTarget(const std::optional<platform::Window>& selection, std::span<const AutoGame> games,
                                              platform::WindowId preferred, std::vector<GameProcess>* windowless = nullptr);
