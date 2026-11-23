#include "app/config.h"
#include "base/ini_text.h"
#include "app/log.h"
#include "base/names.h"

#ifdef _WIN32
#include "app/old_settings.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <shlobj.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
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
#ifdef _WIN32
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    return profile && *profile ? fs::path(profile) : fs::path();
#else
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
#endif
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
#ifdef _WIN32
        // Beside Unishade.exe, as with the ReShade host.
        std::wstring executable(32768, L'\0');
        executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
        fs::path path = fs::path(executable).parent_path();
#elif defined(__APPLE__)
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
#ifdef _WIN32
    // Where the system keeps pictures, which can be elsewhere than the profile's Pictures, such as in OneDrive.
    PWSTR folder = nullptr;
    fs::path pictures;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &folder)))
        pictures = folder;
    CoTaskMemFree(folder);
    if (!pictures.empty())
        return pictures / "Unishade";
#elif !defined(__APPLE__)
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

#ifdef _WIN32
FileLock::FileLock(const fs::path& path)
{
    if (_wsopen_s(&fd, path.c_str(), _O_RDWR | _O_CREAT | _O_NOINHERIT | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
    {
        error = strerror(errno);
        fd = -1;
        return;
    }
    OVERLAPPED whole{};
    if (LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &whole))
        return;
    const DWORD code = GetLastError();
    busy = code == ERROR_LOCK_VIOLATION;
    error = std::system_category().message(static_cast<int>(code));
    _close(fd);
    fd = -1;
}

FileLock::~FileLock()
{
    if (fd >= 0)
        _close(fd);
}
#else
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
#endif

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
#ifdef _WIN32
    // Made where Unishade 0.6.3 or earlier ran, it starts with that version's settings. Its files stay as they are.
    std::string text = existed ? ReadFile(path) : CarryOldSettings(OldHostIniText(ReadFile(DataDirectory() / "RobloxShadeHost.ini")),
                                                                    ReadFile(DataDirectory() / "ReShade.ini"));
    if (!existed && !text.empty())
        Log(LogLevel::Info, "Unishade.ini starts with the settings in RobloxShadeHost.ini and ReShade.ini.");
    IniText ini(std::move(text));
#else
    IniText ini(ReadFile(path));
#endif
    Settings settings;
    std::string value;

    for (const Shortcut& shortcut : kShortcuts)
    {
        Hotkey& hotkey = settings.hotkeys.*shortcut.member;
        ParseHotkey(shortcut.fallback, hotkey);
        if (!ini.Get("INPUT", shortcut.name, value))
            ini.Set("INPUT", shortcut.name, shortcut.fallback);
        // Every shortcut but the menu's can be left unassigned.
        else if (value.empty() && shortcut.member != &InputHotkeys::input)
            hotkey = {};
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
    // ReShade's own, without which ReShade.fxh takes depth as reversed.
    if (!ini.Get("GENERAL", "PreprocessorDefinitions", value))
        ini.Set("GENERAL", "PreprocessorDefinitions",
                value = "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,RESHADE_DEPTH_INPUT_IS_REVERSED=0,"
                        "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0");
    settings.definitions = ParseDefinitions(value);

    if (!ini.Get("GENERAL", "PresetPath", value) || value.empty())
    {
        value = fs::exists(PresetsDirectory() / "GenericPreset1.ini", error) ? "presets/GenericPreset1.ini" : "presets/ReShadePreset.ini";
        ini.Set("GENERAL", "PresetPath", value);
    }
    settings.preset = Resolve(value);

    settings.autoSavePresets = !ini.Get("HOST", "AutoSavePresets", value) || value != "0";
    settings.skipTitleBar = !ini.Get("HOST", "SkipTitleBar", value) || value != "0";
    settings.discordPresence = !ini.Get("HOST", "DiscordPresence", value) || value != "0";
    settings.updateChecks = !ini.Get("HOST", "CheckForUpdates", value) || value != "0";
    settings.debugInfo = ini.Get("HOST", "DebugInfo", value) && value == "1";
    settings.keepEffectsVisible = !ini.Get("HOST", "KeepEffectsVisible", value) || value != "0";
    if (ini.Get("HOST", "MenuScale", value))
        settings.menuScale = std::clamp(std::round(std::strtof(value.c_str(), nullptr) * 100) / 100, kSmallestMenuScale, kLargestMenuScale);
    std::string y;
    if (ini.Get("HOST", "MenuX", value) && ini.Get("HOST", "MenuY", y))
    {
        const float menuX = std::strtof(value.c_str(), nullptr), menuY = std::strtof(y.c_str(), nullptr);
        if (menuX >= 0 && menuX <= 1 && menuY >= 0 && menuY <= 1)
        {
            settings.menuX = menuX;
            settings.menuY = menuY;
        }
    }
    if (ini.Get("HOST", "MenuOpacity", value))
        settings.menuOpacity = std::clamp(std::round(std::strtof(value.c_str(), nullptr) * 100) / 100, 0.2f, 1.0f);
    settings.smallTiles = ini.Get("HOST", "SmallTiles", value) && value == "1";
    settings.shotOriginal = ini.Get("HOST", "ScreenshotOriginal", value) && value == "1";
    // One of the two is always on.
    settings.shotEffects = !settings.shotOriginal || !ini.Get("HOST", "ScreenshotEffects", value) || value != "0";
    settings.shotTwoFiles = ini.Get("HOST", "ScreenshotTwoFiles", value) && value == "1";
    settings.menuHints = !ini.Get("HOST", "MenuHints", value) || value != "0";
    settings.darkMenu = !ini.Get("HOST", "DarkMenu", value) || value != "0";

    if ((ini.Changed() || !existed) && !WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
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
    char scale[16];
    std::snprintf(scale, sizeof(scale), "%.2f", settings.menuScale);
    ini.Set("HOST", "MenuScale", scale);
    if (settings.menuX >= 0 && settings.menuY >= 0)
    {
        char place[16];
        std::snprintf(place, sizeof(place), "%.3f", settings.menuX);
        ini.Set("HOST", "MenuX", place);
        std::snprintf(place, sizeof(place), "%.3f", settings.menuY);
        ini.Set("HOST", "MenuY", place);
    }
    char opacity[16];
    std::snprintf(opacity, sizeof(opacity), "%.2f", settings.menuOpacity);
    ini.Set("HOST", "MenuOpacity", opacity);
    ini.Set("HOST", "SmallTiles", settings.smallTiles ? "1" : "0");
    ini.Set("HOST", "ScreenshotEffects", settings.shotEffects ? "1" : "0");
    ini.Set("HOST", "ScreenshotOriginal", settings.shotOriginal ? "1" : "0");
    ini.Set("HOST", "ScreenshotTwoFiles", settings.shotTwoFiles ? "1" : "0");
    ini.Set("HOST", "MenuHints", settings.menuHints ? "1" : "0");
    ini.Set("HOST", "DarkMenu", settings.darkMenu ? "1" : "0");
#ifdef __APPLE__
    ini.Set("HOST", "SkipTitleBar", settings.skipTitleBar ? "1" : "0");
#endif
#ifdef _WIN32
    ini.Set("HOST", "DiscordPresence", settings.discordPresence ? "1" : "0");
    ini.Set("HOST", "CheckForUpdates", settings.updateChecks ? "1" : "0");
    ini.Set("HOST", "DebugInfo", settings.debugInfo ? "1" : "0");
    ini.Set("HOST", "KeepEffectsVisible", settings.keepEffectsVisible ? "1" : "0");
#endif
    if (WriteFile(path, ini.Text()))
        return true;
    Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
    return false;
}

namespace
{
int ValidFrameRate(int fps)
{
    return fps <= 0 ? 0 : std::clamp(fps, 30, 500);
}

std::string PerformanceSection(const std::string& game)
{
    return game.empty() ? "PERFORMANCE" : "PERFORMANCE." + game;
}

// What a game's section of performance settings holds.
constexpr const char* kPerformanceKeys[] = { "FrameRateLimit", "EffectResolution", "HdrOutput", "UiMask", "DetectUi" };
} // namespace

int FrameRateLimit(const std::string& game)
{
    const IniText ini(ReadFile(DataDirectory() / "Unishade.ini"));
    std::string value;
    int fps = ini.Get("PERFORMANCE", "FrameRateLimit", value) ? std::atoi(value.c_str()) : 0;
    // An empty value is a game without its own.
    if (!game.empty() && ini.Get(PerformanceSection(game), "FrameRateLimit", value) && !value.empty())
        fps = std::atoi(value.c_str());
    return ValidFrameRate(fps);
}

void SetFrameRateLimit(const std::string& game, int fps)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set(PerformanceSection(game), "FrameRateLimit", std::to_string(ValidFrameRate(fps)));
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
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
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}

int EffectResolution(const std::string& game)
{
    const IniText ini(ReadFile(DataDirectory() / "Unishade.ini"));
    std::string value;
    int percent = ini.Get("PERFORMANCE", "EffectResolution", value) ? atoi(value.c_str()) : 100;
    // An empty value is a game without its own.
    if (!game.empty() && ini.Get(PerformanceSection(game), "EffectResolution", value) && !value.empty())
        percent = atoi(value.c_str());
    // Anything else is 100, as on the ReShade host.
    return percent < 25 || percent > 100 ? 100 : percent;
}

void SetEffectResolution(const std::string& game, int percent)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set(PerformanceSection(game), "EffectResolution", std::to_string(percent));
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}

bool HdrOutput(const std::string& game)
{
    const IniText ini(ReadFile(DataDirectory() / "Unishade.ini"));
    std::string value;
    bool on = ini.Get("PERFORMANCE", "HdrOutput", value) && atoi(value.c_str()) != 0;
    // An empty value is a game without its own.
    if (!game.empty() && ini.Get(PerformanceSection(game), "HdrOutput", value) && !value.empty())
        on = atoi(value.c_str()) != 0;
    return on;
}

void SetHdrOutput(const std::string& game, bool on)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set(PerformanceSection(game), "HdrOutput", on ? "1" : "0");
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}

std::vector<std::array<float, 5>> UiMask(const std::string& game)
{
    const IniText ini(ReadFile(DataDirectory() / "Unishade.ini"));
    std::vector<std::array<float, 5>> areas;
    std::string value;
    if (!ini.Get(PerformanceSection(game), "UiMask", value))
        return areas;
    // "left,top,width,height" for each, with ",scene" after the scene ones, and semicolons between.
    for (size_t at = 0; at < value.size();)
    {
        const size_t end = std::min(value.find(';', at), value.size());
        const std::string text = value.substr(at, end - at);
        std::array<float, 5> area{};
        if (sscanf(text.c_str(), "%f,%f,%f,%f", &area[0], &area[1], &area[2], &area[3]) == 4 && area[2] > 0 && area[3] > 0)
        {
            area[4] = text.size() > 6 && text.compare(text.size() - 6, 6, ",scene") == 0 ? 1.0f : 0.0f;
            areas.push_back(area);
        }
        at = end + 1;
    }
    return areas;
}

void SetUiMask(const std::string& game, const std::vector<std::array<float, 5>>& areas)
{
    std::string value;
    for (const std::array<float, 5>& area : areas)
    {
        char text[80];
        snprintf(text, sizeof(text), "%s%.4f,%.4f,%.4f,%.4f%s", value.empty() ? "" : ";", area[0], area[1], area[2], area[3], area[4] != 0 ? ",scene" : "");
        value += text;
    }
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set(PerformanceSection(game), "UiMask", value);
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}

bool DetectUi(const std::string& game)
{
    std::string value;
    return IniText(ReadFile(DataDirectory() / "Unishade.ini")).Get(PerformanceSection(game), "DetectUi", value) && value == "1";
}

void SetDetectUi(const std::string& game, bool on)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set(PerformanceSection(game), "DetectUi", on ? "1" : "0");
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}

void RenamePerformance(const std::string& from, const std::string& to)
{
    // Sections are found without case, so a name that only changes case already has its settings.
    if (Lowercase(from) == Lowercase(to))
        return;
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    std::string value;
    for (const char* key : kPerformanceKeys)
        if (ini.Get(PerformanceSection(from), key, value))
        {
            ini.Set(PerformanceSection(to), key, value);
            ini.Set(PerformanceSection(from), key, "");
        }
    if (ini.Changed() && !WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}

void RemovePerformance(const std::string& game)
{
    // Empty is [PERFORMANCE] itself.
    if (game.empty())
        return;
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    std::string value;
    for (const char* key : kPerformanceKeys)
        if (ini.Get(PerformanceSection(game), key, value) && !value.empty())
            ini.Set(PerformanceSection(game), key, "");
    if (ini.Changed() && !WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
}
