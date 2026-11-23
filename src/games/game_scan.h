#pragma once

// What the stores write about the games they installed, read as far as finding those games takes. Text is UTF-8.
// Reading the files and the registry is the host's.

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game_scan
{
// Every value of a key in Valve's text format, as Steam's libraryfolders.vdf and appmanifest_*.acf are written:
// quoted keys followed by a quoted value or by a section in braces. Keys are compared without case.
inline std::vector<std::string> VdfValues(std::string_view text, std::string_view key)
{
    size_t at = 0;
    // The quoted string at or after at, or nothing when a brace or the end comes first.
    const auto quoted = [&](bool stopAtBrace) -> std::optional<std::string> {
        while (at < text.size() && text[at] != '"')
        {
            if (stopAtBrace && (text[at] == '{' || text[at] == '}'))
                return std::nullopt;
            ++at;
        }
        if (at >= text.size())
            return std::nullopt;
        std::string value;
        for (++at; at < text.size() && text[at] != '"'; ++at)
        {
            if (text[at] == '\\' && at + 1 < text.size())
                ++at;
            value += text[at];
        }
        ++at;
        return value;
    };
    const auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    std::vector<std::string> values;
    while (const std::optional<std::string> name = quoted(false))
    {
        // A key with a section after it has no value of its own.
        const std::optional<std::string> value = quoted(true);
        if (value && same(*name, key))
            values.push_back(*value);
    }
    return values;
}

// The first string a JSON text gives a key, at any depth, as Epic's .item files name a game and its folder.
inline std::optional<std::string> JsonString(std::string_view text, std::string_view key)
{
    const std::string name = "\"" + std::string(key) + "\"";
    for (size_t found = text.find(name); found != std::string_view::npos; found = text.find(name, found + 1))
    {
        size_t at = found + name.size();
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
        if (at >= text.size() || text[at] != ':')
            continue;
        for (++at; at < text.size() && std::isspace(static_cast<unsigned char>(text[at])); ++at)
        {
        }
        if (at >= text.size() || text[at] != '"')
            continue;
        std::string value;
        for (++at; at < text.size() && text[at] != '"'; ++at)
        {
            if (text[at] == '\\' && at + 1 < text.size())
                ++at;
            value += text[at];
        }
        return value;
    }
    return std::nullopt;
}
} // namespace game_scan
