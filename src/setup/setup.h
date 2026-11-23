#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Downloads effects and presets into the data folder, for --install-effects, which Setup on Windows runs, and the
// launcher's download button: every package in ReShade's own EffectPackages.ini, then the presets listed in this
// repository's presets/downloads.ini. Files download through WinHTTP on Windows and the system's curl on macOS and
// Linux, and unpack with miniz.
class EffectSetup
{
public:
    struct State
    {
        bool running = false;
        bool finished = false; // set once, when a run ends
        std::string status;
        std::string detail;
        float fraction = 0;
        std::string error;
        std::vector<std::string> notes; // packages that were skipped
    };

    ~EffectSetup();
    void Start();
    void Cancel();
    State Read() const;
    // Returns true once after a run finished, so the caller can reload effects.
    bool TakeFinished();

private:
    void Run();
    void Status(const std::string& status, const std::string& detail = {});
    void Fraction(float fraction);

    std::thread thread;
    std::atomic<bool> cancel = false;
    mutable std::mutex mutex;
    State state;
    bool finishedFlag = false;
};

// Lowercase hex. Exposed for tests.
std::string Sha256(const std::string& data);

#ifndef _WIN32
// Downloads an https address to a file with curl, as effects download, refusing more than limit bytes. Throws
// std::runtime_error with a message to show, or something else once cancel is set. progress gets the bytes so far.
void DownloadFile(const std::string& url, const std::filesystem::path& path, const std::atomic<bool>& cancel, uintmax_t limit,
                  const std::function<void(uintmax_t received)>& progress);
#endif

// Where the effect packages and presets come from. Tests point them at local files.
struct SetupSources
{
    std::string effects = "https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini";
    std::string presets = "https://raw.githubusercontent.com/OMouta/Unishade/main/presets";
    // Set by --effects-url and --presets-url, whose file:// URLs are allowed. Everything else, the packages the list
    // names included, downloads over https only.
    bool fromCommandLine = false;
};
inline SetupSources setupSources;
