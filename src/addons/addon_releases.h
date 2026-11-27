#pragma once

// The official add-ons' releases on GitHub, as text. Nothing here touches the disk or the network.
//
// Unishade's own add-ons are not part of Unishade's releases. Each is released on its own, under a tag made of its Id
// and its version: unishade.depth-v1.2.0, or unishade.depth-v1.2.0-beta.1 for a beta. Such a release has
//
//     addon.ini       the add-on's manifest, as src/addons/addon_manifest.h describes it
//     files.ini       the add-on's files for each system, as ReadFiles there reads them
//     the icon        the PNG the manifest names
//     SHA256SUMS.txt  the SHA-256 of each of the release's files, which Unishade checks the three above against
//
// and the libraries this repository builds, which files.ini points at. So a new version of an add-on, or a new add-on,
// reaches every Unishade that can run it without a new Unishade.
//
// Betas work as Unishade's own do: only a beta of Unishade is offered them, and a version is offered to every build
// unless GitHub has it as a prerelease.

#include "addons/addon_manifest.h"
#include "net/releases.h"
#include "net/version.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace addon
{
inline constexpr char kFilesFile[] = "files.ini";
inline constexpr char kSumsFile[] = "SHA256SUMS.txt";
// GitHub lists at most this many releases a page.
inline constexpr size_t kReleasesPerPage = 100;

struct Release
{
    std::string tag;
    std::string id;
    Version version;
};

inline std::string ReleaseTag(std::string_view id, const Version& version)
{
    return std::string(id) + "-v" + version.Text();
}

// Reads the tag of an add-on's release. Empty for any other tag, such as v0.7.0 of Unishade itself or depth-assets.
// The tag is nothing but an Id, -v and a version, since it goes into the addresses of the release's files and names
// the folder Unishade keeps them in.
inline std::optional<Release> ReadTag(std::string_view tag)
{
    // An Id can have -v in it, so every one is tried as the start of the version.
    for (size_t at = tag.find("-v"); at != std::string_view::npos; at = tag.find("-v", at + 1))
    {
        const std::optional<Version> version = ParseVersion(tag.substr(at + 2));
        if (version && ValidId(tag.substr(0, at)))
            return Release{ std::string(tag), std::string(tag.substr(0, at)), *version };
    }
    return std::nullopt;
}

// Where a file of a release downloads from.
inline std::string ReleaseFile(std::string_view tag, std::string_view file)
{
    return "https://github.com/OMouta/Unishade/releases/download/" + std::string(tag) + "/" + std::string(file);
}

// A page of GitHub's JSON list of the releases, with as many a page as kReleasesPerPage. Pages count from 1.
inline std::string ReleasesPage(size_t page)
{
    return "https://api.github.com/repos/OMouta/Unishade/releases?per_page=" + std::to_string(kReleasesPerPage) + "&page=" + std::to_string(page);
}

// What a page of GitHub's JSON list of the releases has of the add-ons' releases.
struct ReleasesRead
{
    // The ones a build is offered, in the page's order.
    std::vector<Release> offered;
    // Every release the page lists, Unishade's own included. Fewer than kReleasesPerPage means it is the last page.
    size_t listed = 0;
};

// Reads a page of the list for a build of Unishade, which is a beta or not. A draft and a release without an
// addon.ini are never offered. Only a beta is offered betas. Every build is offered a version, unless GitHub has it
// as a prerelease, and a prerelease flag that is missing or unreadable counts as set.
inline ReleasesRead ReadReleases(std::string_view json, bool beta)
{
    using namespace releases;
    ReleasesRead read;
    // Each release lists its "tag_name", then "draft", "prerelease" and its "assets", before the next release's
    // "tag_name".
    for (size_t position = json.find("\"tag_name\""); position != kMissing;)
    {
        const size_t next = json.find("\"tag_name\"", position + 1);
        const std::string_view release = json.substr(position, next == kMissing ? kMissing : next - position);
        position = next;
        ++read.listed;
        std::optional<Release> found = ReadTag(JsonString(release, "tag_name", 0));
        if (!found || JsonBool(release, "draft", 0) == true || !HasAsset(release, kManifestFile))
            continue;
        if (found->version.beta ? beta : JsonBool(release, "prerelease", 0) == false)
            read.offered.push_back(std::move(*found));
    }
    return read;
}

// Orders releases by their add-on, and each add-on's from the newest to the oldest.
inline void NewestFirst(std::vector<Release>& releases)
{
    std::sort(releases.begin(), releases.end(), [](const Release& a, const Release& b) { return a.id != b.id ? a.id < b.id : b.version < a.version; });
}
} // namespace addon
