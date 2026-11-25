#include "net/update.h"
#include "app/log.h"
#include "net/net.h"
#include "net/releases.h"
#include "net/version.h"
#include "setup/setup.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <unistd.h>

// macOS and Linux: looks for a newer Unishade on GitHub. There is no Setup to run on these systems, so the user gets
// the new version from the release's page.

namespace
{
namespace fs = std::filesystem;

// Never destroyed, since the check's thread may still run while Unishade exits.
struct Shared
{
    std::mutex mutex;
    Update available;
};
Shared& shared = *new Shared;

// GitHub's list of the releases, through curl as effects download, in a folder only this user can open.
std::string ReleaseList()
{
    std::error_code error;
    fs::path temporary = fs::temp_directory_path(error);
    if (error)
        temporary = "/tmp";
    std::string folder = (temporary / "unishade-update-XXXXXX").string();
    if (!mkdtemp(folder.data()))
        throw std::runtime_error("Could not create a folder for the download in " + temporary.string() + ".");
    struct Remove
    {
        const std::string& folder;
        ~Remove()
        {
            std::error_code ignored;
            fs::remove_all(folder, ignored);
        }
    } remove{ folder };
    const fs::path list = fs::path(folder) / "releases.json";
    const std::atomic<bool> cancel = false;
    DownloadFile("https://api.github.com/repos/OMouta/Unishade/releases?per_page=30", list, cancel, kListLimit, {});
    std::ostringstream text;
    text << std::ifstream(list, std::ios::binary).rdbuf();
    return text.str();
}

void Check()
{
    const std::string tag = NewestRelease(ReleaseList(), ParseVersion(UNISHADE_VERSION).value_or(Version{}));
    if (tag.empty())
    {
        Log(LogLevel::Info, "Unishade is up to date.");
        return;
    }
    const std::string version = ParseVersion(tag.substr(1)).value_or(Version{}).Text();
    Log(LogLevel::Info, "Unishade %s is available.", version.c_str());
    std::lock_guard lock(shared.mutex);
    shared.available = { version, "https://github.com/OMouta/Unishade/releases/tag/" + tag, false };
}
} // namespace

void CheckForUpdate()
{
    std::thread([] {
        try
        {
            Check();
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Info, "Could not check for updates: %s", e.what());
        }
        catch (...)
        {
            Log(LogLevel::Info, "Could not check for updates.");
        }
    }).detach();
}

Update AvailableUpdate()
{
    std::lock_guard lock(shared.mutex);
    return shared.available;
}

// Nothing here is installable, so there is nothing to install or to stop.
void InstallUpdate() {}

void CancelUpdate() {}

UpdateInstall InstallingUpdate()
{
    return {};
}

std::string NotFromPublisher(const std::filesystem::path&, bool)
{
    return {};
}
