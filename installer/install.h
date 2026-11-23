#pragma once

#include "net/net.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct InstallOptions
{
    std::filesystem::path directory;
    // The effects and presets, which the installed Unishade downloads with --install-effects.
    bool effects = true;
    // Leaves out the Start menu shortcuts, the entry in Windows' app list and the copy of Setup.
    bool portable = false;
};

// Where Unishade downloads effects and presets from, when not its own defaults. Tests point them elsewhere from the
// command line, and may give the full path of a local file for the effect package list or a local presets folder.
struct Sources
{
    std::wstring effects;
    std::wstring presets;
};
inline Sources sources;

// Written by the install thread and read by the window.
class Progress
{
public:
    struct State
    {
        std::string status;
        std::string detail;
        float fraction = 0;
        std::vector<std::string> history;
        std::vector<std::string> notes;
    };

    std::atomic<bool> cancel = false;

    // A new step. Also written to the setup log.
    void Status(const std::string& text);
    void Detail(const std::string& text);
    void Fraction(float value);
    // Something the user should know at the end, such as a skipped effect package. Also written to the setup log.
    void Note(const std::string& text);
    State Read() const;

private:
    mutable std::mutex mutex;
    State state;
};

void OpenSetupLog(const std::filesystem::path& path);
void SetupLog(const std::string& line);
const std::filesystem::path& SetupLogPath();

// A resource embedded in Setup, such as the host exe.
std::string_view Resource(int id);

// Puts Unishade in the folder, then has it download the effects and presets. Effects that cannot be installed are
// left out with a note, and the result is false. Throws std::runtime_error, or Cancelled before the folder changes.
bool Install(const InstallOptions& options, Progress& progress);

// Removes the files Setup installed, what Unishade made for itself and the logs. With deleteUserFiles, also the
// settings, the game list and the presets, reshade-shaders and Addons folders. Refuses a folder that is not a Unishade
// installation. When Setup runs from the folder, it moves its own exe out first; call DeleteMovedSetup before exiting.
void Uninstall(const std::filesystem::path& directory, bool deleteUserFiles);
void DeleteMovedSetup();

struct Installation
{
    std::filesystem::path directory;
    std::string version;
};
std::optional<Installation> FindInstallation();

bool HostRunning(const std::filesystem::path& directory);
void LaunchHost(const std::filesystem::path& directory);

// The GPU preference for the folder's Unishade.exe in Windows' graphics settings: 0 lets Windows decide, 1 saves
// power and 2 is high performance. -1 when it has none.
int GpuPreference(const std::filesystem::path& directory);
// Sets it to high performance, keeping its other graphics settings.
void SetHighPerformanceGpu(const std::filesystem::path& directory);
