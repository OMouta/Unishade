#pragma once

#include "base/ini_text.h"

#include <charconv>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// games.ini, the saved games every platform's host attaches to. Text is UTF-8. Reading and writing the file is
// left to each platform, which also turns the entries into its own kind of path.
struct GameListEntry
{
    std::string executable; // a filename such as RobloxPlayerBeta.exe, or a full path
    std::string name;
    bool enabled = true;
};

// Throws std::runtime_error when the list is damaged.
inline std::vector<GameListEntry> ParseGameList(const std::string& text)
{
    IniText ini(text);
    std::string countText;
    size_t count = 0;
    if (!ini.Get("Games", "Count", countText))
        throw std::runtime_error("The saved game list has no game count");
    const auto parsed = std::from_chars(countText.data(), countText.data() + countText.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != countText.data() + countText.size())
        throw std::runtime_error("The saved game count is invalid");
    std::vector<GameListEntry> games;
    for (size_t i = 0; i < count; ++i)
    {
        const std::string section = "Game" + std::to_string(i);
        GameListEntry game;
        std::string enabled;
        if (!ini.Get(section, "Executable", game.executable) || game.executable.empty() || !ini.Get(section, "Name", game.name) ||
            game.name.empty() || !ini.Get(section, "Enabled", enabled) || (enabled != "0" && enabled != "1"))
            throw std::runtime_error("A saved game entry is invalid");
        game.enabled = enabled == "1";
        games.push_back(std::move(game));
    }
    return games;
}

inline std::string FormatGameList(std::span<const GameListEntry> games)
{
    IniText ini("");
    ini.Set("Games", "Count", std::to_string(games.size()));
    for (size_t i = 0; i < games.size(); ++i)
    {
        const std::string section = "Game" + std::to_string(i);
        ini.Set(section, "Executable", games[i].executable);
        ini.Set(section, "Name", games[i].name);
        ini.Set(section, "Enabled", games[i].enabled ? "1" : "0");
    }
    return ini.Text();
}
