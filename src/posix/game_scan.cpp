// Finds the games installed on this computer, for the list Unishade offers the first time it starts.

#include "games.h"
#include "config.h"
#include "game_scan.h"

#include <algorithm>
#include <cctype>
#include <set>

#ifdef _WIN32
#include "text.h"

#include <cwchar>
#include <windows.h>

namespace
{
std::wstring RegistryText(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t text[1024] = {};
    DWORD size = sizeof(text) - sizeof(wchar_t);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS)
        return {};
    return text;
}

std::string Lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A folder as a game is saved by it: with Windows' slashes and one at the end, which is what says it is a folder.
std::string FolderKey(fs::path folder)
{
    std::string key = Utf8(folder.make_preferred().wstring());
    while (!key.empty() && key.back() == '\\')
        key.pop_back();
    return key + "\\";
}

struct Scan
{
    std::vector<FoundGame> found;
    std::set<std::string> folders; // without case, so a game two stores list is offered once

    void Add(const fs::path& folder, const std::string& name, const char* store)
    {
        std::error_code error;
        if (name.empty() || !fs::is_directory(folder, error))
            return;
        const std::string key = FolderKey(folder);
        if (folders.insert(Lower(key)).second)
            found.push_back({ { key, name }, store });
    }
};

// Steam keeps a manifest for each game beside the folder it installed it in, in every library it has.
void FindSteamGames(Scan& scan)
{
    std::wstring steam = RegistryText(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty())
        steam = RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty())
        return;
    // The libraries as Steam writes them first, since the registry has its own folder in lower case.
    std::vector<fs::path> libraries;
    for (const std::string& path : game_scan::VdfValues(ReadFile(fs::path(steam) / "steamapps" / "libraryfolders.vdf"), "path"))
        libraries.push_back(fs::path(Wide(path)));
    libraries.push_back(fs::path(steam));
    for (const fs::path& library : libraries)
    {
        std::error_code error;
        for (fs::directory_iterator entry(library / "steamapps", error), end; !error && entry != end; entry.increment(error))
        {
            const std::wstring file = entry->path().filename().wstring();
            if (file.rfind(L"appmanifest_", 0) != 0 || entry->path().extension() != L".acf")
                continue;
            const std::string text = ReadFile(entry->path());
            const std::vector<std::string> names = game_scan::VdfValues(text, "name"), folders = game_scan::VdfValues(text, "installdir"),
                                           ids = game_scan::VdfValues(text, "appid");
            // Steamworks Common Redistributables is in every library and is no game.
            if (names.empty() || folders.empty() || (!ids.empty() && ids.front() == "228980"))
                continue;
            scan.Add(library / "steamapps" / "common" / fs::path(Wide(folders.front())), names.front(), "Steam");
        }
    }
}

// Epic's launcher keeps an item for each thing it installed, add-ons too, which name the game they belong to.
void FindEpicGames(Scan& scan)
{
    const wchar_t* data = _wgetenv(L"ProgramData");
    if (!data)
        return;
    std::error_code error;
    for (fs::directory_iterator entry(fs::path(data) / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests", error), end; !error && entry != end;
         entry.increment(error))
    {
        if (entry->path().extension() != L".item")
            continue;
        const std::string text = ReadFile(entry->path());
        const std::optional<std::string> name = game_scan::JsonString(text, "DisplayName"), folder = game_scan::JsonString(text, "InstallLocation"),
                                         app = game_scan::JsonString(text, "AppName"), main = game_scan::JsonString(text, "MainGameAppName");
        if (!name || !folder || (app && main && *app != *main))
            continue;
        scan.Add(fs::path(Wide(*folder)), *name, "Epic Games");
    }
}

// Roblox installs each version in a folder of its own, so it is saved by its file's name, which stays.
void FindRoblox(Scan& scan)
{
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local)
        return;
    std::error_code error;
    for (fs::directory_iterator entry(fs::path(local) / "Roblox" / "Versions", error), end; !error && entry != end; entry.increment(error))
        if (fs::exists(entry->path() / "RobloxPlayerBeta.exe", error))
        {
            scan.found.push_back({ { "RobloxPlayerBeta.exe", "Roblox" }, "" });
            return;
        }
}
} // namespace

std::vector<FoundGame> FindInstalledGames()
{
    Scan scan;
    FindRoblox(scan);
    FindSteamGames(scan);
    FindEpicGames(scan);
    std::sort(scan.found.begin(), scan.found.end(), [](const FoundGame& a, const FoundGame& b) { return Lower(a.game.name) < Lower(b.game.name); });
    return std::move(scan.found);
}
#else
std::vector<FoundGame> FindInstalledGames()
{
    return {};
}
#endif
