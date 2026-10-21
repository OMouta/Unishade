#pragma once

#include <algorithm>
#include <string_view>
#include <type_traits>

// How much of a word in a message is a web address, which the launcher and the menu show as a link: all of it but
// punctuation after the address, such as a full stop, or nothing when the word is not one.
template <class Char>
size_t WebAddressLengthOf(std::basic_string_view<Char> word)
{
    const auto startsWith = [word](std::string_view prefix) {
        return word.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), word.begin(), [](char a, Char b) { return Char(a) == b; });
    };
    const auto punctuation = [](Char character) {
        const auto code = static_cast<std::make_unsigned_t<Char>>(character);
        return code < 128 && std::string_view(".,;:!?)'\"").find(static_cast<char>(code)) != std::string_view::npos;
    };
    if (!startsWith("https://") && !startsWith("http://"))
        return 0;
    // The slashes after http: are no punctuation, so this stops there at the latest.
    size_t length = word.size();
    while (punctuation(word[length - 1]))
        --length;
    return length;
}
