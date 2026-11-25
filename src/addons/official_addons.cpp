#include "addons/official_addons.h"
#include "addons/addon_releases.h"
#include "app/config.h"
#include "app/log.h"
#include "net/net.h"
#include "setup/setup.h"

#ifdef _WIN32
#include "base/text.h"
#else
#include <cstdlib>
#include <unistd.h>
#endif

#include <atomic>
#include <set>
#include <stdexcept>

namespace
{
// The icon as the cache folder keeps it, whatever the release calls it.
constexpr char kIconFile[] = "icon.png";
// How many of an add-on's releases are looked at for one this Unishade can run, from the newest.
constexpr size_t kNewestTried = 8;
// How many pages of GitHub's list are read, which is more releases than Unishade and its add-ons will have for years.
constexpr size_t kPagesRead = 10;

// A folder for each release that was looked at, named after its tag, with its addon.ini, files.ini and icon.
fs::path KeptFolder()
{
    return DataDirectory() / "cache" / "addons";
}

bool BetaBuild()
{
    return ParseVersion(UNISHADE_VERSION).value_or(Version{}).beta > 0;
}

enum class Use
{
    Offered,
    NotForThisSystem,
    NeedsNewerUnishade,
    Broken,
};

// Reads a kept release, and says whether this Unishade can offer it.
Use ReadKept(const addon::Release& release, Addons::Official& official)
{
    const fs::path folder = KeptFolder() / fs::path(release.tag);
    official = {};
    official.tag = release.tag;
    official.beta = release.version.beta > 0;
    official.manifestText = ReadFile(folder / addon::kManifestFile);
    // The release is the add-on and the version its tag says, so one add-on's release cannot stand in for another's.
    if (!addon::ReadManifest(official.manifestText, addon::Platform(), official.manifest).empty() || official.manifest.id != release.id ||
        ParseVersion(official.manifest.version) != release.version)
        return Use::Broken;
    if (official.manifest.api > UNISHADE_API_VERSION)
        return Use::NeedsNewerUnishade;
    if (official.manifest.library.empty())
        return Use::NotForThisSystem;
    official.files = addon::ReadFiles(ReadFile(folder / addon::kFilesFile), addon::Platform());
    for (const addon::Download& file : official.files)
        if (file.path == official.manifest.library)
            official.sha256 = file.sha256;
    if (official.sha256.empty())
        return Use::Broken;
    std::error_code error;
    if (!official.manifest.icon.empty() && fs::is_regular_file(folder / kIconFile, error))
    {
        official.picture = (folder / kIconFile).string();
        official.icon = ReadFile(folder / kIconFile);
    }
    return Use::Offered;
}

// Downloads what Unishade reads of a release into the cache folder. False when a file is not the one the release's
// SHA256SUMS.txt lists, which leaves nothing behind.
bool Keep(const FetchText& fetch, const addon::Release& release)
{
    const std::string sums = fetch(addon::ReleaseFile(release.tag, addon::kSumsFile));
    bool listed = true;
    const auto checked = [&](const std::string& file) {
        const std::string sha256 = ListedSha256(sums, file);
        std::string data = sha256.empty() ? std::string() : fetch(addon::ReleaseFile(release.tag, file));
        if (sha256.empty() || Sha256(data) != sha256)
        {
            Log(LogLevel::Warning, "%s of the release %s is not the file its SHA256SUMS.txt lists.", file.c_str(), release.tag.c_str());
            listed = false;
        }
        return data;
    };
    const std::string manifestText = checked(addon::kManifestFile);
    const std::string files = listed ? checked(addon::kFilesFile) : std::string();
    addon::Manifest manifest;
    addon::ReadManifest(manifestText, addon::Platform(), manifest);
    // The release has the icon under its file's name, wherever in the add-on's folder the manifest puts it.
    const std::string icon = listed && !manifest.icon.empty() ? checked(fs::path(manifest.icon).filename().string()) : std::string();
    if (!listed)
        return false;

    // Whole or not at all: a folder with an addon.ini is a release that was looked at.
    const fs::path folder = KeptFolder() / fs::path(release.tag);
    fs::path part = folder;
    part += ".part";
    std::error_code error;
    fs::remove_all(part, error);
    fs::create_directories(part, error);
    if (!WriteFile(part / addon::kManifestFile, manifestText) || !WriteFile(part / addon::kFilesFile, files) ||
        (!icon.empty() && !WriteFile(part / kIconFile, icon)))
        throw std::runtime_error("Could not write to " + part.string() + ".");
    fs::remove_all(folder, error);
    fs::rename(part, folder, error);
    if (error)
        throw std::runtime_error("Could not write to " + folder.string() + ".");
    return true;
}

// For each add-on, the newest of its releases that this Unishade can offer. Without fetch only kept releases count.
std::vector<Addons::Official> Choose(std::vector<addon::Release> releases, const FetchText* fetch, const std::vector<Addons::Official>& known)
{
    addon::NewestFirst(releases);
    std::vector<Addons::Official> chosen;
    for (size_t first = 0, next = 0; first < releases.size(); first = next)
    {
        for (next = first; next < releases.size() && releases[next].id == releases[first].id;)
            ++next;
        try
        {
            for (size_t i = first; i < next && i < first + kNewestTried; ++i)
            {
                std::error_code error;
                if (!fs::is_regular_file(KeptFolder() / fs::path(releases[i].tag) / addon::kManifestFile, error) && (!fetch || !Keep(*fetch, releases[i])))
                    continue;
                Addons::Official official;
                const Use use = ReadKept(releases[i], official);
                if (use == Use::Offered)
                    chosen.push_back(std::move(official));
                else if (use == Use::Broken)
                {
                    // Not kept, so a release that is put right is read again.
                    Log(LogLevel::Warning, "The release %s cannot be used: its addon.ini or files.ini is not as Unishade reads them.", releases[i].tag.c_str());
                    fs::remove_all(KeptFolder() / fs::path(releases[i].tag), error);
                }
                // A release for a newer Unishade, or a broken one, may have one before it that this Unishade can run.
                if (use == Use::Offered || use == Use::NotForThisSystem)
                    break;
            }
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Warning, "Could not look at the releases of the add-on %s: %s", releases[first].id.c_str(), e.what());
            for (const Addons::Official& entry : known)
                if (entry.manifest.id == releases[first].id)
                    chosen.push_back(entry);
        }
    }
    return chosen;
}
} // namespace

std::string FetchFromTheNet(const std::string& url)
{
    const std::atomic<bool> cancel = false;
#ifdef _WIN32
    return Fetch(Wide(url), cancel, kListLimit);
#else
    // Through a file, as curl downloads, in a folder only this user can open.
    std::error_code error;
    fs::path temporary = fs::temp_directory_path(error);
    if (error)
        temporary = "/tmp";
    std::string folder = (temporary / "unishade-addons-XXXXXX").string();
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
    const fs::path file = fs::path(folder) / "download";
    DownloadFile(url, file, cancel, kListLimit, {});
    return ReadFile(file);
#endif
}

std::vector<Addons::Official> KeptOfficialAddons()
{
    std::vector<addon::Release> releases;
    std::error_code error;
    for (fs::directory_iterator it(KeptFolder(), error), end; !error && it != end; it.increment(error))
        if (std::optional<addon::Release> release = addon::ReadTag(it->path().filename().string()); release && (BetaBuild() || !release->version.beta))
            releases.push_back(std::move(*release));
    return Choose(std::move(releases), nullptr, {});
}

std::vector<Addons::Official> FindOfficialAddons(const FetchText& fetch, const std::vector<Addons::Official>& known)
{
    std::vector<addon::Release> releases;
    for (size_t page = 1; page <= kPagesRead; ++page)
    {
        addon::ReleasesRead read = addon::ReadReleases(fetch(addon::ReleasesPage(page)), BetaBuild());
        releases.insert(releases.end(), read.offered.begin(), read.offered.end());
        if (read.listed < addon::kReleasesPerPage)
            break;
    }
    std::vector<Addons::Official> chosen = Choose(releases, &fetch, known);

    // What is kept of a release that is gone, or that this build is no longer offered, is not kept for anything.
    std::set<std::string> tags;
    for (const addon::Release& release : releases)
        tags.insert(release.tag);
    for (const Addons::Official& official : chosen)
        tags.insert(official.tag);
    std::vector<fs::path> gone;
    std::error_code error;
    for (fs::directory_iterator it(KeptFolder(), error), end; !error && it != end; it.increment(error))
        if (!tags.count(it->path().filename().string()))
            gone.push_back(it->path());
    for (const fs::path& folder : gone)
        fs::remove_all(folder, error);
    return chosen;
}
