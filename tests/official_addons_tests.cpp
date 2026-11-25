// The official add-ons from their own releases, without GitHub: cmake/addon_release.cmake packs releases of add-ons
// made up here, a stand-in for GitHub serves them, and Unishade picks the release of each that it can offer, keeps
// it, and tells an installed add-on from the one it offers. Also packs the repository's own add-ons, so a mistake in
// their addon.ini or files.ini fails here and not in a release.

#include "addons/addon_releases.h"
#include "addons/official_addons.h"
#include "app/config.h"
#include "setup/setup.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <thread>

namespace
{
bool Check(bool condition, const std::string& what)
{
    if (!condition)
        std::printf("Failed: %s\n", what.c_str());
    return condition;
}

void Put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

fs::path work;

// Packs a release of a made-up add-on as the workflow packs a real one's, and returns its folder. Its one library is
// for every system, so the tests are the same on each.
fs::path Pack(const std::string& id, const std::string& version, int api = 1, const std::string& system = "windows]\nLibrary=One.lib\n[macos]\nLibrary=One.lib\n[linux")
{
    const fs::path source = work / "source" / (id + "-" + version);
    Put(source / "addon.ini", "; A comment, which the release leaves out.\n\n[Addon]\nId=" + id + "\nName=Add-on " + id + "\nVersion=" + version +
                                  "\nAuthor=Tester\nIcon=art/icon.png\nApi=" + std::to_string(api) + "\n\n[" + system + "]\nLibrary=One.lib\n");
    std::string files = "; A comment.\n";
    for (const char* name : { "windows", "macos", "linux" })
        files += std::string("[") + name + "]\nOne.lib=@One.lib@ https://github.com/OMouta/Unishade/releases/download/@TAG@/One.lib\ndata/model.bin=" +
                 std::string(64, 'a') + " https://example.com/model.bin\n\n";
    Put(source / "files.ini", files);
    Put(source / "art" / "icon.png", "the icon of " + id);
    Put(source / "One.lib", "the library of " + id + " " + version);
    // Into a folder named from where the script runs, as the workflow names it.
    const fs::path packed = work / "releases" / (id + "-v" + version);
    fs::create_directories(packed.parent_path());
    const std::string command = "cd \"" + packed.parent_path().string() + "\" && \"" + CMAKE_COMMAND + "\" \"-DADDON=" + source.string() +
                                "\" \"-DBINARIES=One.lib=" + (source / "One.lib").string() + "\" \"-DOUTPUT=" + packed.filename().string() + "\" -P \"" +
                                PACK_SCRIPT + "\" > /dev/null";
    if (std::system(command.c_str()) != 0)
        std::printf("Failed: could not pack %s %s\n", id.c_str(), version.c_str());
    return packed;
}

// GitHub, as far as Unishade asks it: the list of the releases, and the files of the ones packed here.
struct GitHub
{
    std::vector<std::string> pages; // the list, a page each
    std::vector<std::string> asked; // every address, in order
    std::string down;               // addresses with this in them cannot be reached

    std::string Fetch(const std::string& url)
    {
        asked.push_back(url);
        if (!down.empty() && url.find(down) != std::string::npos)
            throw std::runtime_error("Could not download " + url + ".");
        for (size_t page = 0; page < pages.size(); ++page)
            if (url == addon::ReleasesPage(page + 1))
                return pages[page];
        const std::string releases = "https://github.com/OMouta/Unishade/releases/download/";
        const fs::path file = work / "releases" / url.substr(std::min(url.size(), releases.size()));
        if (url.rfind(releases, 0) != 0 || !fs::is_regular_file(file))
            throw std::runtime_error("Could not download " + url + ".");
        return ReadFile(file);
    }

    FetchText Fetcher()
    {
        return [this](const std::string& url) { return Fetch(url); };
    }
};

// A release as GitHub lists it.
std::string Listed(const std::string& tag, bool prerelease = false, const std::string& asset = "addon.ini")
{
    return R"({"tag_name":")" + tag + R"(","draft":false,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"assets":[{"name":"SHA256SUMS.txt"},{"name":")" + asset + R"("}]},)";
}

// The tags of a list, separated by spaces.
std::string Tags(const std::vector<Addons::Official>& list)
{
    std::string tags;
    for (const Addons::Official& official : list)
        tags += (tags.empty() ? "" : " ") + official.tag;
    return tags;
}

class Services final : public AddonHost
{
public:
    State DescribeForAddons() override { return {}; }
    void ShowMessage(const std::string&) override {}
    void SetEffectsOn(bool) override {}
    void TakeScreenshot() override {}
    std::vector<fs::path> ListPresets() override { return {}; }
    bool UsePreset(const fs::path&) override { return false; }
};
} // namespace

int main()
{
    // A data folder of the test's own, which the host looks up once.
    work = fs::temp_directory_path() / ("unishade-official-addons-tests-" + std::to_string(std::rand()));
    fs::remove_all(work);
    fs::create_directories(work);
    setenv("HOME", work.string().c_str(), 1);
    setenv("XDG_DATA_HOME", (work / "data").string().c_str(), 1);
    const fs::path kept = DataDirectory() / "cache" / "addons";
    // Whether this build is a beta, which says whether it is offered the add-ons' betas.
    const bool beta = ParseVersion(UNISHADE_VERSION).value_or(Version{}).beta > 0;
    bool ok = true;

    // The repository's own add-ons, with a stand-in for each library.
    for (const auto& [folder, libraries] : std::map<std::string, std::vector<std::string>>{ { "depth", { "Depth.dll", "Depth.dylib" } }, { "dlss5", { "Unishade_nvngx.dll" } } })
    {
        Put(work / "stand-in.lib", "a library");
        std::string binaries;
        for (const std::string& library : libraries)
            binaries += (binaries.empty() ? "" : "|") + library + "=" + (work / "stand-in.lib").string();
        const fs::path packed = work / "own" / folder;
        const std::string command = std::string("\"") + CMAKE_COMMAND + "\" \"-DADDON=" + ADDONS_FOLDER + "/" + folder + "\" \"-DBINARIES=" + binaries + "\" \"-DOUTPUT=" +
                                    packed.string() + "\" -P \"" + PACK_SCRIPT + "\" > /dev/null";
        ok &= Check(std::system(command.c_str()) == 0, "packs the release of " + folder);
        const std::string sums = ReadFile(packed / addon::kSumsFile);
        for (const std::string& library : libraries)
        {
            const std::string system = library.ends_with(".dll") ? "windows-x64" : "macos-arm64";
            addon::Manifest manifest;
            ok &= Check(addon::ReadManifest(ReadFile(packed / addon::kManifestFile), system, manifest).empty() && manifest.library == library &&
                            ParseVersion(manifest.version).has_value(),
                        folder + "'s addon.ini has a version and its library for " + system);
            const std::vector<addon::Download> files = addon::ReadFiles(ReadFile(packed / addon::kFilesFile), system);
            ok &= Check(!files.empty() && files[0].path == library && files[0].sha256 == Sha256("a library") &&
                            files[0].url == addon::ReleaseFile(addon::ReleaseTag(manifest.id, ParseVersion(manifest.version).value_or(Version{})), library),
                        folder + "'s files.ini has that library for " + system + ", with its SHA-256 and its address in the release");
            ok &= Check(ListedSha256(sums, library) == Sha256("a library") && ListedSha256(sums, addon::kManifestFile) == Sha256(ReadFile(packed / addon::kManifestFile)) &&
                            ListedSha256(sums, addon::kFilesFile) == Sha256(ReadFile(packed / addon::kFilesFile)) &&
                            (manifest.icon.empty() || ListedSha256(sums, manifest.icon) == Sha256(ReadFile(packed / manifest.icon))),
                        folder + "'s SHA256SUMS.txt lists what Unishade reads of the release");
        }
    }

    // Two add-ons with a version and a newer beta each, one whose newest version needs a newer Unishade, one that is
    // not for this system, and releases that are no add-on's.
    Pack("tester.first", "1.0.0");
    Pack("tester.first", "1.1.0-beta.1");
    Pack("tester.second", "1.0.0");
    Pack("tester.second", "2.0.0", UNISHADE_API_VERSION + 1);
    Pack("tester.elsewhere", "1.0.0", 1, "amiga");
    GitHub github;
    github.pages = { "[" + Listed("tester.second-v2.0.0") + Listed("v0.7.0", false, "Unishade-Setup-0.7.0.exe") + Listed("tester.first-v1.1.0-beta.1", true) +
                     Listed("tester.elsewhere-v1.0.0") + Listed("tester.first-v1.0.0") + Listed("depth-assets", false, "onnxruntime.dll") +
                     Listed("tester.second-v1.0.0") + "]" };
    const std::string first = beta ? "tester.first-v1.1.0-beta.1" : "tester.first-v1.0.0";
    std::vector<Addons::Official> list = FindOfficialAddons(github.Fetcher(), {});
    ok &= Check(Tags(list) == first + " tester.second-v1.0.0",
                "offers each add-on's newest release that this build is offered, that this Unishade's version of the interface runs and that is for this system");
    if (list.size() == 2)
    {
        const Addons::Official& official = list[0];
        const fs::path packed = work / "releases" / first;
        ok &= Check(official.beta == beta && official.manifest.id == "tester.first" && official.manifest.name == "Add-on tester.first" &&
                        official.manifest.library == "One.lib" && official.manifestText == ReadFile(packed / "addon.ini"),
                    "with the release's addon.ini, which has no comments");
        ok &= Check(official.files.size() == 2 && official.files[0].path == "One.lib" && official.files[0].url == addon::ReleaseFile(first, "One.lib") &&
                        official.sha256 == Sha256(ReadFile(packed / "One.lib")) && official.files[1].path == "data/model.bin",
                    "its files, with the SHA-256 of the library the release has");
        ok &= Check(official.icon == "the icon of tester.first" && ReadFile(official.picture) == official.icon, "and its icon, which Unishade keeps for the Add-ons page");
    }
    ok &= Check(fs::is_regular_file(kept / "tester.second-v2.0.0" / "addon.ini") && fs::is_regular_file(kept / "tester.elsewhere-v1.0.0" / "addon.ini"),
                "a release this Unishade cannot offer is kept too, so it is not downloaded again");

    github.asked.clear();
    ok &= Check(Tags(FindOfficialAddons(github.Fetcher(), list)) == Tags(list) && github.asked == std::vector<std::string>{ addon::ReleasesPage(1) },
                "the next look asks GitHub for the list alone");
    ok &= Check(Tags(KeptOfficialAddons()) == Tags(list), "and what a look found is there without asking");

    // Releases that are not what they say.
    fs::path bad = Pack("tester.first", "1.2.0");
    Put(bad / "addon.ini", ReadFile(bad / "addon.ini") + "; Changed after the release was packed.\n");
    github.pages[0].insert(1, Listed("tester.first-v1.2.0"));
    ok &= Check(Tags(FindOfficialAddons(github.Fetcher(), list)) == Tags(list) && !fs::exists(kept / "tester.first-v1.2.0"),
                "a release with a file that is not the one its SHA256SUMS.txt lists is passed over for the one before it, and not kept");
    fs::remove_all(bad);
    fs::copy(Pack("tester.second", "1.3.0"), bad, fs::copy_options::recursive);
    ok &= Check(Tags(FindOfficialAddons(github.Fetcher(), list)) == Tags(list) && !fs::exists(kept / "tester.first-v1.2.0"),
                "and so is the release of one add-on under the tag of another");
    fs::remove_all(bad);
    bad = Pack("tester.first", "1.2.0");
    std::string sums = ReadFile(bad / "SHA256SUMS.txt");
    sums.erase(sums.find(Sha256(ReadFile(bad / "icon.png"))), 64 + 2 + 8 + 1);
    Put(bad / "SHA256SUMS.txt", sums);
    ok &= Check(Tags(FindOfficialAddons(github.Fetcher(), list)) == Tags(list), "and one with a file its SHA256SUMS.txt does not list");

    // A good one takes its place.
    Pack("tester.first", "1.2.0");
    list = FindOfficialAddons(github.Fetcher(), list);
    ok &= Check(Tags(list) == "tester.first-v1.2.0 tester.second-v1.0.0", "a newer version is offered once it is released");
    if (!beta)
        ok &= Check(!fs::exists(kept / "tester.first-v1.1.0-beta.1"), "a version of Unishade keeps nothing of a beta");

    // GitHub out of reach.
    Pack("tester.second", "1.4.0");
    github.pages[0].insert(1, Listed("tester.second-v1.4.0"));
    github.down = "tester.second-v1.4.0";
    Pack("tester.first", "1.3.0");
    github.pages[0].insert(1, Listed("tester.first-v1.3.0"));
    list = FindOfficialAddons(github.Fetcher(), list);
    ok &= Check(Tags(list) == "tester.first-v1.3.0 tester.second-v1.0.0", "an add-on whose release cannot be downloaded stays as it was, and the others go on");
    github.down = "api.github.com";
    bool threw = false;
    try
    {
        FindOfficialAddons(github.Fetcher(), list);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    ok &= Check(threw && Tags(KeptOfficialAddons()) == Tags(list), "without GitHub's list a look fails, and what is kept stays");
    github.down.clear();
    list = FindOfficialAddons(github.Fetcher(), list);
    ok &= Check(Tags(list) == "tester.first-v1.3.0 tester.second-v1.4.0", "the next look finds what the last could not");

    // A release that was taken back.
    github.pages[0].erase(github.pages[0].find(Listed("tester.first-v1.3.0")), Listed("tester.first-v1.3.0").size());
    list = FindOfficialAddons(github.Fetcher(), list);
    ok &= Check(Tags(list) == "tester.first-v1.2.0 tester.second-v1.4.0" && !fs::exists(kept / "tester.first-v1.3.0") && Tags(KeptOfficialAddons()) == Tags(list),
                "a release that is gone from GitHub is not offered or kept anymore");

    // More releases than a page lists.
    std::string full = "[";
    for (size_t i = 0; i < addon::kReleasesPerPage; ++i)
        full += Listed("v0.1." + std::to_string(i), false, "Unishade-Setup-0.1." + std::to_string(i) + ".exe");
    const std::string rest = github.pages[0];
    github.pages = { full + "]", rest };
    github.asked.clear();
    ok &= Check(Tags(FindOfficialAddons(github.Fetcher(), list)) == Tags(list) &&
                    github.asked == std::vector<std::string>{ addon::ReleasesPage(1), addon::ReleasesPage(2) },
                "a full page of releases has another one read after it");
    github.pages = { rest };

    // An installed add-on next to the list: the one Unishade offers, and another version of it.
    const fs::path folder = DataDirectory() / "Addons";
    fs::create_directories(folder);
    fs::copy(work / "releases" / "tester.first-v1.2.0", folder / "tester.first", fs::copy_options::recursive);
    fs::copy(work / "releases" / "tester.second-v1.0.0", folder / "tester.second", fs::copy_options::recursive);
    // Turned off, since their libraries are no libraries.
    Put(DataDirectory() / "Unishade.ini", "[ADDONS]\ntester.first=0\ntester.second=0\n");
    {
        fx::Runtime runtime;
        Services services;
        Addons addons;
        addons.Init(runtime, services);
        ok &= Check(Tags(addons.OfficialList()) == Tags(list), "Unishade starts with the official add-ons the last look found");
        const auto installed = [&](const std::string& name) -> const Addons::Addon* {
            for (const Addons::Addon& addon : addons.List())
                if (addon.folder == name)
                    return &addon;
            return nullptr;
        };
        ok &= Check(installed("tester.first") && installed("tester.first")->official && !installed("tester.first")->replaceable,
                    "an installed add-on whose library is the offered release's is official");
        ok &= Check(installed("tester.second") && !installed("tester.second")->official && installed("tester.second")->replaceable,
                    "one of another version is not, and can be replaced");

        // A new version comes out while Unishade runs.
        Pack("tester.first", "1.5.0");
        github.pages[0].insert(1, Listed("tester.first-v1.5.0"));
        std::atomic<int> woken = 0;
        const unsigned before = addons.OfficialGeneration();
        addons.LookForOfficial([&] { ++woken; }, github.Fetcher());
        ok &= Check(addons.LookingForOfficial(), "the Add-ons page has GitHub asked");
        for (int tries = 0; tries < 500 && addons.OfficialGeneration() == before; ++tries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            addons.Update();
        }
        ok &= Check(woken == 1 && !addons.LookingForOfficial() && !addons.OfficialLookFailed() &&
                        Tags(addons.OfficialList()) == "tester.first-v1.5.0 tester.second-v1.4.0",
                    "and the next pass of the main loop has the answer");
        ok &= Check(installed("tester.first") && !installed("tester.first")->official && installed("tester.first")->replaceable,
                    "the installed add-on then has an update");
        github.asked.clear();
        addons.LookForOfficial({}, github.Fetcher());
        ok &= Check(!addons.LookingForOfficial() && github.asked.empty(), "GitHub is not asked again right away");
        addons.Shutdown();
    }

    std::error_code ignored;
    fs::remove_all(work, ignored);
    return ok ? 0 : 1;
}
