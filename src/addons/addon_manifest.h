#pragma once

// An add-on's manifest and the list of official add-ons' files, as text. Nothing here touches the disk.
//
// An add-on is a folder in the Addons folder with an addon.ini in it, which says what the add-on is without anything
// of it being run:
//
//     [Addon]
//     Id=someone.my-addon
//     Name=My add-on
//     Version=1.2.0
//     Author=Someone
//     Description=What it does, in a sentence.
//     Icon=icon.png
//     Website=https://example.com/my-addon
//     License=MIT
//     Api=1
//
//     [windows-x64]
//     Library=MyAddon.dll
//
//     [macos]
//     Library=MyAddon.dylib
//
// Id names the add-on in Unishade.ini and in presets, so it stays the same when anything else changes. Api is the
// version of addons/unishade_addon.h it was built for. Each system the add-on runs on has a section with its
// library, a path in the folder: windows-x64, windows-arm64, macos-arm64, macos-x64, linux-x64 or linux-arm64, or
// windows, macos or linux alone for a library that runs on every processor there, as a universal library does on macOS.
// Only Id, Api and the library are needed. Icon is a PNG in the folder.

#include "base/ini_text.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace addon
{
inline constexpr char kManifestFile[] = "addon.ini";

// The system this program was built for, as manifests and the official list name it.
constexpr std::string_view Platform()
{
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#else
    return "windows-x64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos-arm64";
#else
    return "macos-x64";
#endif
#else
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
#endif
}

struct Manifest
{
    std::string id;
    std::string name; // the id without one
    std::string version;
    std::string author;
    std::string description;
    std::string website;
    std::string license;
    std::string icon;    // a PNG in the folder, or empty
    std::string library; // in the folder, for the system asked about. Empty when the add-on has none for it.
    int api = 0;
};

// One to 64 lowercase letters, digits, dots, hyphens and underscores, starting with a letter or a digit. It names a
// section of presets, where effects' sections are named after their files, so it cannot end as an effect file does.
inline bool ValidId(std::string_view id)
{
    if (id.empty() || id.size() > 64 || id.ends_with(".fx") || id.ends_with(".addonfx"))
        return false;
    const auto letterOrDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return letterOrDigit(id[0]) && std::all_of(id.begin(), id.end(), [&](char c) { return letterOrDigit(c) || c == '.' || c == '-' || c == '_'; });
}

// A path that stays in the add-on's folder: names separated by slashes, none of them empty, . or .., and nothing
// Windows reads as a drive, another separator or a stream.
inline bool InsideFolder(std::string_view path)
{
    if (path.empty() || path.size() > 200 || path.find_first_of("\\:") != std::string_view::npos)
        return false;
    for (size_t start = 0; start <= path.size();)
    {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string_view name = path.substr(start, end - start);
        if (name.empty() || name == "." || name == "..")
            return false;
        start = end + 1;
    }
    return true;
}

// Reads a manifest for a system. Returns why it cannot be used, or nothing. A manifest without a library for the
// system is fine: its library is empty.
inline std::string ReadManifest(const std::string& text, std::string_view platform, Manifest& manifest)
{
    const IniText ini(text);
    const auto get = [&](const char* key, size_t longest) {
        std::string value;
        ini.Get("Addon", key, value);
        // Cut between characters, not inside one.
        if (value.size() > longest)
        {
            size_t end = longest;
            while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80)
                --end;
            value.resize(end);
        }
        return value;
    };
    manifest = {};
    // What it calls itself first, so an add-on that cannot be used can still be named.
    manifest.name = get("Name", 64);
    manifest.version = get("Version", 32);
    manifest.author = get("Author", 64);
    manifest.description = get("Description", 400);
    manifest.website = get("Website", 200);
    manifest.license = get("License", 64);
    manifest.icon = get("Icon", 200);
    if (!manifest.icon.empty() && !InsideFolder(manifest.icon))
        manifest.icon.clear();
    // A website opens in the browser, so it has to be an address for one.
    if (!manifest.website.starts_with("https://") && !manifest.website.starts_with("http://"))
        manifest.website.clear();
    manifest.id = get("Id", 200);
    if (manifest.id.empty())
        return "Its addon.ini has no Id.";
    if (!ValidId(manifest.id))
        return "Its Id, " + manifest.id + ", isn't one to 64 lowercase letters, digits, dots, hyphens and underscores.";
    if (manifest.name.empty())
        manifest.name = manifest.id;
    manifest.api = std::atoi(get("Api", 16).c_str());
    if (manifest.api < 1)
        return "Its addon.ini doesn't say which version of Unishade's add-ons it was built for, as Api.";
    // The system and processor first, then the system alone.
    const std::string whole(platform);
    if (!ini.Get(whole, "Library", manifest.library))
        ini.Get(whole.substr(0, whole.find('-')), "Library", manifest.library);
    if (!manifest.library.empty() && !InsideFolder(manifest.library))
        return "Its library, " + manifest.library + ", isn't a file in its folder.";
    return {};
}

// A file of an official add-on: where it goes in the add-on's folder, what it is and where it downloads from.
struct Download
{
    std::string path;
    std::string sha256; // lowercase
    std::string url;
};

// An official add-on's files for one system.
struct Files
{
    std::string id;
    std::vector<Download> files;
};

inline bool IsSha256(std::string_view text)
{
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

// Reads the official list, addons/official.ini, for a system. Each section is named after an add-on's Id and a
// system, as [unishade.depth windows-x64], and each of its keys is a file in the add-on's folder, set to the file's
// SHA-256 and the address it downloads from. An add-on with a file that lacks either is left out.
inline std::vector<Files> ReadFiles(const std::string& text, std::string_view platform)
{
    const auto trim = [](std::string_view part) {
        const size_t start = part.find_first_not_of(" \t\r");
        return start == std::string_view::npos ? std::string() : std::string(part.substr(start, part.find_last_not_of(" \t\r") - start + 1));
    };
    std::vector<Files> list;
    std::vector<bool> broken;
    bool inside = false;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);)
    {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[')
        {
            const std::string header = trim(std::string_view(line).substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1));
            const size_t space = header.find(' ');
            inside = space != std::string::npos && trim(std::string_view(header).substr(space)) == platform && ValidId(header.substr(0, space));
            if (inside)
            {
                list.push_back({ header.substr(0, space), {} });
                broken.push_back(false);
            }
            continue;
        }
        const size_t equals = line.find('=');
        if (!inside || equals == std::string::npos)
            continue;
        Download file{ trim(std::string_view(line).substr(0, equals)), {}, {} };
        std::istringstream value(line.substr(equals + 1));
        value >> file.sha256 >> file.url;
        std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(), [](char c) { return c >= 'A' && c <= 'F' ? char(c - 'A' + 'a') : c; });
        if (!InsideFolder(file.path) || !IsSha256(file.sha256) || !file.url.starts_with("https://"))
            broken.back() = true;
        list.back().files.push_back(std::move(file));
    }
    std::vector<Files> whole;
    for (size_t i = 0; i < list.size(); ++i)
        if (!broken[i] && !list[i].files.empty())
            whole.push_back(std::move(list[i]));
    return whole;
}
} // namespace addon
