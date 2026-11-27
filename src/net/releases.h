#pragma once

#include "net/version.h"

#include <optional>
#include <string>
#include <string_view>

// Picks an update out of GitHub's list of Unishade's releases. Apart from the update check itself, so tests read
// lists without GitHub.

// The name the release workflow gives the Setup of the release with this tag, such as v0.7.0.
inline std::string SetupName(std::string_view tag)
{
    return "Unishade-Setup-" + std::string(tag.substr(1)) + ".exe";
}

// The SHA-256 a release's SHA256SUMS.txt lists for one of its files, in lowercase. Empty when it lists none. Its
// lines are a SHA-256, two spaces or a space and a star, and the file's name, as sha256sum writes them.
inline std::string ListedSha256(std::string_view sums, std::string_view file)
{
    for (size_t start = 0; start < sums.size();)
    {
        size_t end = sums.find('\n', start);
        end = end == std::string_view::npos ? sums.size() : end;
        std::string_view line = sums.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.size() != 66 + file.size() || line[64] != ' ' || (line[65] != ' ' && line[65] != '*') || line.substr(66) != file)
            continue;
        std::string hash(line.substr(0, 64));
        for (char& digit : hash)
        {
            if (digit >= 'A' && digit <= 'F')
                digit += 'a' - 'A';
            if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
                return {};
        }
        return hash;
    }
    return {};
}

namespace releases
{
constexpr size_t kMissing = std::string_view::npos;
constexpr std::string_view kSpace = " \t\r\n";

// Where the value of "key": starts in GitHub's JSON, searching from from. kMissing when there is none.
inline size_t JsonValue(std::string_view json, std::string_view key, size_t from)
{
    const std::string pattern = "\"" + std::string(key) + "\"";
    for (size_t position = json.find(pattern, from); position != kMissing; position = json.find(pattern, position + 1))
    {
        // The same text can be a value, which no colon follows.
        const size_t colon = json.find_first_not_of(kSpace, position + pattern.size());
        if (colon != kMissing && json[colon] == ':')
            return json.find_first_not_of(kSpace, colon + 1);
    }
    return kMissing;
}

// The string starting at start. Empty when there is none, such as for null.
inline std::string_view StringAt(std::string_view json, size_t start)
{
    if (start == kMissing || json[start] != '"')
        return {};
    const size_t end = json.find('"', start + 1);
    return end == kMissing ? std::string_view{} : json.substr(start + 1, end - start - 1);
}

inline std::string_view JsonString(std::string_view json, std::string_view key, size_t from)
{
    return StringAt(json, JsonValue(json, key, from));
}

inline std::optional<bool> JsonBool(std::string_view json, std::string_view key, size_t from)
{
    const size_t start = JsonValue(json, key, from);
    if (start == kMissing)
        return std::nullopt;
    if (json.substr(start, 4) == "true")
        return true;
    if (json.substr(start, 5) == "false")
        return false;
    return std::nullopt;
}

// Whether a release has a file of this name. Only releases with Setup can be installed, like on the website: the
// add-ons and the files they need are releases too.
inline bool HasAsset(std::string_view release, std::string_view file)
{
    const size_t assets = JsonValue(release, "assets", 0);
    for (size_t name = JsonValue(release, "name", assets); assets != kMissing && name != kMissing; name = JsonValue(release, "name", name))
        if (StringAt(release, name) == file)
            return true;
    return false;
}
} // namespace releases

// The tag of the newest release after current that a build of that version is offered, from GitHub's JSON list of
// the releases. Empty when there is none. The tag is a v and nothing but a version after it, since it goes into the
// addresses of the release's page and its Setup.
inline std::string NewestRelease(std::string_view json, const Version& current)
{
    using namespace releases;
    Version newest = current;
    std::string tag;
    // Each release lists its "tag_name", then "draft", "prerelease" and its "assets", before the next release's
    // "tag_name".
    for (size_t position = json.find("\"tag_name\""); position != kMissing;)
    {
        const size_t next = json.find("\"tag_name\"", position + 1);
        const std::string_view release = json.substr(position, next == kMissing ? kMissing : next - position);
        position = next;
        const std::string_view name = JsonString(release, "tag_name", 0);
        const std::optional<Version> version = name.starts_with('v') ? ParseVersion(name.substr(1)) : std::nullopt;
        if (!version || !(newest < *version) || JsonBool(release, "draft", 0) == true || !HasAsset(release, SetupName(name)))
            continue;
        // Only a beta is offered betas. Every build is offered a version itself, unless GitHub has it as a prerelease,
        // and a prerelease flag that is missing or unreadable counts as set.
        if (version->beta ? current.beta > 0 : JsonBool(release, "prerelease", 0) == false)
        {
            newest = *version;
            tag = name;
        }
    }
    return tag;
}
