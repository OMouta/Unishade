#pragma once

#include "../src/net.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class Addon
{
    Depth,
    DLSS5,
};

struct InstallOptions
{
    std::filesystem::path directory;
    bool reshade = true;
    bool presets = true;
    bool depth = false;
    bool dlss5 = false;
    // Leaves out the Start menu shortcuts, the entry in Windows' app list and the copy of Setup.
    bool portable = false;
};

// Download locations. Tests point them elsewhere from the command line, and may give a local file for the effect
// package list and the add-on manifests.
struct Sources
{
    std::wstring effects = L"https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini";
    std::wstring presets = L"https://raw.githubusercontent.com/OMouta/Unishade/main/presets";
    std::wstring dlss5 = L"https://github.com/OMouta/Unishade/releases/download/dlss5-assets/downloads.ini";
    std::wstring depth = L"https://github.com/OMouta/Unishade/releases/download/depth-assets/downloads.ini";
};
inline Sources sources;

struct ReShadeRelease
{
    std::string version;
    std::string license;
};

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
    // Something the user should know at the end, such as a skipped add-on. Also written to the setup log.
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

// Finds the newest ReShade on reshade.me, or on GitHub when reshade.me fails, and downloads its license from that
// version's source tag.
ReShadeRelease FetchReShadeRelease(const std::atomic<bool>& cancel);

// Downloads everything into a temporary folder first, so a failed or cancelled download leaves the installation
// folder untouched. An effect package, preset or add-on that cannot be installed is left out with a note, and the
// result is false. Throws std::runtime_error or Cancelled.
bool Install(const InstallOptions& options, const ReShadeRelease& release, Progress& progress);

// Removes the files Setup installed and the logs. With deleteUserFiles, also removes ReShade.ini, ReShadePreset.ini,
// RobloxShadeHost.ini, games.ini and the presets and reshade-shaders folders. Refuses a folder that is not a Unishade
// installation. When Setup runs from the folder, it moves its own exe out first; call DeleteMovedSetup before exiting.
void Uninstall(const std::filesystem::path& directory, bool deleteUserFiles);
void DeleteMovedSetup();

struct Installation
{
    std::filesystem::path directory;
    std::string version;
};
std::optional<Installation> FindInstallation();

// Whether the add-on's files are in the folder, to preselect it.
bool AddonInstalled(const std::filesystem::path& directory, Addon addon);

// A shortcut in the folder's RobloxShadeHost.ini, which the host reads when it starts.
std::wstring ReadShortcut(const std::filesystem::path& directory, const wchar_t* name, const wchar_t* fallback);
bool WriteShortcut(const std::filesystem::path& directory, const wchar_t* name, const std::wstring& value);

bool HostRunning(const std::filesystem::path& directory);
void LaunchHost(const std::filesystem::path& directory);

// The GPU preference for the folder's Unishade.exe in Windows' graphics settings: 0 lets Windows decide, 1 saves
// power and 2 is high performance. -1 when it has none.
int GpuPreference(const std::filesystem::path& directory);
// Sets it to high performance, keeping its other graphics settings.
void SetHighPerformanceGpu(const std::filesystem::path& directory);
