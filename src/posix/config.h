#pragma once

#include "hotkeys.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Where settings, presets, effects and the log live: ~/Library/Application Support/Unishade on macOS,
// $XDG_DATA_HOME/unishade (~/.local/share/unishade) on Linux. Created on first use. Empty when there is no home
// folder to put it in, which the host refuses to start without.
const fs::path& DataDirectory();
fs::path PresetsDirectory();
fs::path EffectsDirectory(); // reshade-shaders, laid out like ReShade's: Shaders and Textures
// Unishade in the pictures folder: ~/Pictures on macOS, and on Linux XDG_PICTURES_DIR from the environment or
// user-dirs.dirs, which names it in the desktop's language, or ~/Pictures without one.
fs::path ScreenshotDirectory();
// A folder such as XDG_PICTURES_DIR from the text of user-dirs.dirs, whose lines look like
// XDG_PICTURES_DIR="$HOME/Pictures". Empty when the text does not name it, or names the home folder, which turns it
// off.
fs::path UserDirectory(const std::string& userDirs, const std::string& name, const fs::path& home);
// A window title made into one file name: without slashes, control characters, what Windows does not allow or dots
// in front, and at most 80 bytes. "Unishade" when nothing is left.
std::string SafeFileName(std::string name);

// An exclusive lock on a file, such as the one that keeps a second Unishade from starting. Held until the object
// goes away or the process ends.
class FileLock
{
public:
    explicit FileLock(const fs::path& path);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    bool Locked() const { return fd >= 0; }
    // Another process holds it.
    bool Busy() const { return busy; }
    // Why the lock could not be taken, when it is not busy.
    const std::string& Error() const { return error; }

private:
    int fd = -1;
    bool busy = false;
    std::string error;
};

struct Settings
{
    InputHotkeys hotkeys;
    bool autoSavePresets = true;
    // macOS only: effects leave out the title bar of a game in a window. platform.h says how it is told apart.
    bool skipTitleBar = true;
    fs::path preset;
    // Searched recursively, like ReShade's paths ending in \**.
    std::vector<fs::path> effectPaths;
    std::vector<fs::path> texturePaths;
    std::vector<std::pair<std::string, std::string>> definitions; // global preprocessor definitions
};

// Reads Unishade.ini from the data folder, creating it on first run. Invalid shortcuts are reported and
// replaced by the defaults.
Settings LoadSettings();
// Returns false when the file cannot be written.
bool SaveSettings(const Settings& settings);

// The preset last used in a saved game, from Unishade.ini. Empty when the game has none yet.
fs::path GamePreset(const std::string& game);
void SetGamePreset(const std::string& game, const fs::path& preset);

// "A=1,B" as ReShade writes preprocessor definitions, and back.
std::vector<std::pair<std::string, std::string>> ParseDefinitions(const std::string& text);
std::string FormatDefinitions(const std::vector<std::pair<std::string, std::string>>& definitions);

std::string ReadFile(const fs::path& path);
// Writes through a temporary file, so a failed write leaves the old file. Returns false on failure.
bool WriteFile(const fs::path& path, const std::string& contents);

std::string Lowercase(std::string text);
