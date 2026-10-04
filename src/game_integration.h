#pragma once

#include <windows.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct AutoGame
{
    std::filesystem::path executable;
    std::wstring name;
    bool enabled = true;
};

// Roblox, which is detected until the user turns it off.
std::vector<AutoGame> DefaultAutoGames();
// The versioned executable used as the icon for the version-independent game entry.
std::filesystem::path InstalledStudioExecutable();
// Adds the installed Roblox Studio executable, when found, as an opt-in game.
bool AddInstalledStudio(std::vector<AutoGame>& games);
// The defaults when the file does not exist. Throws when it cannot be read or is damaged.
std::vector<AutoGame> LoadAutoGames(const std::filesystem::path& path);
void SaveAutoGames(const std::filesystem::path& path, std::span<const AutoGame> games);
// Games saved with a folder match that exact file. Games saved by filename, like Roblox, match it in any folder.
bool MatchesExecutable(const AutoGame& game, const std::filesystem::path& executable);

// Throws std::system_error when the process has exited or denies access.
std::filesystem::path ProcessExecutable(DWORD processId);

struct GameWindow
{
    HWND window;
    std::wstring name;
    DWORD processId;
};

// Every window a game could be drawing in, sorted by title, for the window picker.
std::vector<GameWindow> ListGameWindows();
bool GameWindowExists(const GameWindow& game);
void AddAutoGame(std::vector<AutoGame>& games, const GameWindow& window);
// An empty selection keeps automatic detection. A closed selection does not attach to another game. Detection
// prefers preferredWindow, then the frontmost window of an enabled game.
std::optional<GameWindow> FindGameTarget(const std::optional<GameWindow>& selection, std::span<const AutoGame> games,
                                       HWND preferredWindow = nullptr);
// The window as FindGameTarget would report it when it belongs to an enabled game, without looking at any other
// window or process.
std::optional<GameWindow> MatchGameWindow(HWND window, std::span<const AutoGame> games);
