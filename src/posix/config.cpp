#include "config.h"
#include "ini_text.h"
#include "log.h"
#include "names.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace
{
// Empty when there is none, rather than a folder everyone can write to.
fs::path Home()
{
    if (const char* home = getenv("HOME"); home && *home == '/')
        return home;
    // Programs started without HOME, such as by some service managers, still have one in the user database.
    const long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size > 0 ? size_t(size) : 16384);
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found && found->pw_dir && found->pw_dir[0] == '/')
        return found->pw_dir;
    return {};
}

// Relative paths in Unishade.ini are relative to the data folder, so the folder can move.
fs::path Resolve(const std::string& text)
{
    if (text == "~" || text.rfind("~/", 0) == 0)
        return Home() / text.substr(std::min<size_t>(2, text.size()));
    const fs::path path(text);
    return path.is_absolute() ? path : DataDirectory() / path;
}

std::string Relative(const fs::path& path)
{
    const fs::path relative = path.lexically_relative(DataDirectory());
    return !relative.empty() && *relative.begin() != ".." ? relative.string() : path.string();
}

std::vector<fs::path> ParsePaths(const std::string& text)
{
    std::vector<fs::path> paths;
    for (size_t start = 0; start <= text.size();)
    {
        const size_t end = std::min(text.find(',', start), text.size());
        std::string entry = text.substr(start, end - start);
        start = end + 1;
        // ReShade's recursive marker. Every path is searched recursively here.
        for (const char* suffix : { "/**", "\\**" })
            if (entry.size() >= 3 && entry.compare(entry.size() - 3, 3, suffix) == 0)
                entry.resize(entry.size() - 3);
        if (!entry.empty())
            paths.push_back(Resolve(entry));
    }
    return paths;
}

std::string FormatPaths(const std::vector<fs::path>& paths)
{
    std::string text;
    for (const fs::path& path : paths)
        text += (text.empty() ? "" : ",") + Relative(path);
    return text;
}
} // namespace

const fs::path& DataDirectory()
{
    static const fs::path directory = [] {
#ifdef __APPLE__
        if (Home().empty())
            return fs::path();
        fs::path path = Home() / "Library" / "Application Support" / "Unishade";
#else
        const char* xdg = getenv("XDG_DATA_HOME");
        if ((!xdg || *xdg != '/') && Home().empty())
            return fs::path();
        fs::path path = (xdg && *xdg == '/' ? fs::path(xdg) : Home() / ".local" / "share") / "unishade";
#endif
        std::error_code ignored;
        fs::create_directories(path, ignored);
        return path;
    }();
    return directory;
}

fs::path PresetsDirectory()
{
    return DataDirectory() / "presets";
}

fs::path EffectsDirectory()
{
    return DataDirectory() / "reshade-shaders";
}

fs::path ScreenshotDirectory()
{
    const fs::path home = Home();
#ifndef __APPLE__
    fs::path pictures;
    const char* config = getenv("XDG_CONFIG_HOME");
    if (const char* folder = getenv("XDG_PICTURES_DIR"); folder && *folder == '/')
        pictures = folder;
    else if ((config && *config == '/') || !home.empty())
        pictures = UserDirectory(ReadFile((config && *config == '/' ? fs::path(config) : home / ".config") / "user-dirs.dirs"), "XDG_PICTURES_DIR", home);
    if (!pictures.empty())
        return pictures / "Unishade";
#endif
    return home.empty() ? DataDirectory() / "Screenshots" : home / "Pictures" / "Unishade";
}

fs::path UserDirectory(const std::string& userDirs, const std::string& name, const fs::path& home)
{
    std::istringstream lines(userDirs);
    fs::path result;
    // Read like the shell does, so the last line that sets it counts.
    for (std::string line; std::getline(lines, line);)
    {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, name.size() + 1, name + "=") != 0)
            continue;
        std::string value = line.substr(start + name.size() + 1);
        value.erase(value.find_last_not_of(" \t\r") + 1);
        if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            continue;
        std::string path;
        for (size_t i = 1; i + 1 < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 2 < value.size())
                ++i;
            path += value[i];
        }
        if (path == "$HOME" || path.rfind("$HOME/", 0) == 0)
            result = home / path.substr(std::min<size_t>(6, path.size()));
        else if (!path.empty() && path[0] == '/')
            result = path;
    }
    return result.is_absolute() && result.lexically_relative(home) != "." ? result : fs::path();
}

std::string SafeFileName(std::string name)
{
    for (char& c : name)
        if (name_rules::InvalidInName(c) || c == 127)
            c = ' ';
    // A name starting with a dot would be hidden, and ".." would name the folder above.
    name.erase(0, name.find_first_not_of(". "));
    if (name.size() > 80)
    {
        size_t end = 80;
        while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80)
            --end;
        name.resize(end);
    }
    name.erase(name.find_last_not_of(". ") + 1);
    return name.empty() ? "Unishade" : name;
}

FileLock::FileLock(const fs::path& path)
{
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0)
        return;
    const int code = errno;
    busy = fd >= 0 && code == EWOULDBLOCK;
    error = strerror(code);
    if (fd >= 0)
        close(fd);
    fd = -1;
}

FileLock::~FileLock()
{
    if (fd >= 0)
        close(fd);
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

bool WriteFile(const fs::path& path, const std::string& contents)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output.flush())
        {
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, path, error);
    if (error)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::string Lowercase(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::vector<std::pair<std::string, std::string>> ParseDefinitions(const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> definitions;
    for (size_t start = 0; start < text.size();)
    {
        const size_t end = std::min(text.find(',', start), text.size());
        const std::string entry = text.substr(start, end - start);
        start = end + 1;
        const size_t equals = entry.find('=');
        const std::string name = entry.substr(0, equals);
        if (!name.empty())
            definitions.emplace_back(name, equals == std::string::npos ? "" : entry.substr(equals + 1));
    }
    return definitions;
}

std::string FormatDefinitions(const std::vector<std::pair<std::string, std::string>>& definitions)
{
    std::string text;
    for (const auto& [name, value] : definitions)
        text += (text.empty() ? "" : ",") + name + (value.empty() ? "" : "=" + value);
    return text;
}

Settings LoadSettings()
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    std::error_code error;
    const bool existed = fs::exists(path, error);
    IniText ini(ReadFile(path));
    Settings settings;
    std::string value;

    for (const Shortcut& shortcut : kShortcuts)
    {
        Hotkey& hotkey = settings.hotkeys.*shortcut.member;
        ParseHotkey(shortcut.fallback, hotkey);
        if (!ini.Get("INPUT", shortcut.name, value))
            ini.Set("INPUT", shortcut.name, shortcut.fallback);
        else if (!ParseHotkey(value, hotkey))
        {
            Report(LogLevel::Warning, "%s=%s in Unishade.ini is not a valid shortcut, so %s is used.", shortcut.name, value.c_str(), shortcut.fallback);
            ParseHotkey(shortcut.fallback, hotkey);
        }
    }

    if (!ini.Get("GENERAL", "EffectSearchPaths", value))
        ini.Set("GENERAL", "EffectSearchPaths", value = "reshade-shaders/Shaders");
    settings.effectPaths = ParsePaths(value);
    if (!ini.Get("GENERAL", "TextureSearchPaths", value))
        ini.Set("GENERAL", "TextureSearchPaths", value = "reshade-shaders/Textures");
    settings.texturePaths = ParsePaths(value);
    if (ini.Get("GENERAL", "PreprocessorDefinitions", value))
        settings.definitions = ParseDefinitions(value);

    if (!ini.Get("GENERAL", "PresetPath", value) || value.empty())
    {
        value = fs::exists(PresetsDirectory() / "GenericPreset1.ini", error) ? "presets/GenericPreset1.ini" : "presets/ReShadePreset.ini";
        ini.Set("GENERAL", "PresetPath", value);
    }
    settings.preset = Resolve(value);

    settings.autoSavePresets = !ini.Get("HOST", "AutoSavePresets", value) || value != "0";

    if ((ini.Changed() || !existed) && !WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.c_str());
    return settings;
}

bool SaveSettings(const Settings& settings)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    for (const Shortcut& shortcut : kShortcuts)
        ini.Set("INPUT", shortcut.name, FormatHotkey(settings.hotkeys.*shortcut.member));
    ini.Set("GENERAL", "EffectSearchPaths", FormatPaths(settings.effectPaths));
    ini.Set("GENERAL", "TextureSearchPaths", FormatPaths(settings.texturePaths));
    ini.Set("GENERAL", "PresetPath", Relative(settings.preset));
    ini.Set("GENERAL", "PreprocessorDefinitions", FormatDefinitions(settings.definitions));
    ini.Set("HOST", "AutoSavePresets", settings.autoSavePresets ? "1" : "0");
    if (WriteFile(path, ini.Text()))
        return true;
    Log(LogLevel::Warning, "Could not write %s.", path.c_str());
    return false;
}

fs::path GamePreset(const std::string& game)
{
    std::string value;
    return IniText(ReadFile(DataDirectory() / "Unishade.ini")).Get("GAMEPRESETS", game, value) && !value.empty() ? Resolve(value) : fs::path();
}

void SetGamePreset(const std::string& game, const fs::path& preset)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set("GAMEPRESETS", game, Relative(preset));
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.c_str());
}
