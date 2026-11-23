#pragma once

#include <array>
#include <climits>
#include <optional>
#include <string>
#include <string_view>

// A version of Unishade, as CMakeLists.txt names it and a release's tag does after its v: 0.7.0, or 0.7.0-beta.2 for
// the second beta of 0.7.0.
struct Version
{
    std::array<int, 3> number{};
    int beta = 0; // which beta of the version, or 0 for the version itself

    bool operator==(const Version&) const = default;
    // A version's betas come before it, in their order.
    bool operator<(const Version& other) const
    {
        if (number != other.number)
            return number < other.number;
        return (beta ? beta : INT_MAX) < (other.beta ? other.beta : INT_MAX);
    }
    std::string Text() const
    {
        const std::string text = std::to_string(number[0]) + "." + std::to_string(number[1]) + "." + std::to_string(number[2]);
        return beta ? text + "-beta." + std::to_string(beta) : text;
    }
};

// Reads a whole version, with nothing before or after it. Empty for anything else, such as depth-assets, which is a
// release's tag too.
inline std::optional<Version> ParseVersion(std::string_view text)
{
    size_t position = 0;
    // Up to nine digits, which always fit in an int.
    const auto number = [&](int& value) {
        const size_t start = position;
        for (value = 0; position < text.size() && text[position] >= '0' && text[position] <= '9'; ++position)
        {
            if (position - start == 9)
                return false;
            value = value * 10 + (text[position] - '0');
        }
        return position > start;
    };
    Version version;
    for (int part = 0; part < 3; ++part)
        if (!number(version.number[part]) || (part < 2 && (position >= text.size() || text[position++] != '.')))
            return std::nullopt;
    if (position == text.size())
        return version;
    constexpr std::string_view kBeta = "-beta.";
    if (text.substr(position, kBeta.size()) != kBeta)
        return std::nullopt;
    position += kBeta.size();
    if (!number(version.beta) || version.beta == 0 || position != text.size())
        return std::nullopt;
    return version;
}
