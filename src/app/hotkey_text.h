#pragma once

#include <string>
#include <string_view>

// How shortcuts are written on every platform, such as Ctrl+Shift+F8: modifiers first, each once, then exactly
// one key, separated by plus signs with optional spaces. Which names are modifiers and keys, and what they turn
// into, is each platform's own.
namespace hotkey_text
{
inline bool SameName(std::string_view a, std::string_view b)
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

// Splits text into the modifiers' bits and the key's name. modifierBit returns the bit of a modifier's name, or 0
// for anything else. Returns false for text that breaks the rules above.
template <class ModifierBit>
bool Split(std::string_view text, ModifierBit modifierBit, unsigned& modifiers, std::string& key)
{
    unsigned bits = 0;
    std::string name;
    while (!text.empty())
    {
        const size_t separator = text.find('+');
        std::string_view token = text.substr(0, separator);
        const size_t first = token.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return false;
        token = token.substr(first, token.find_last_not_of(" \t") - first + 1);

        if (const unsigned bit = modifierBit(token))
        {
            if (!name.empty() || (bits & bit))
                return false;
            bits |= bit;
        }
        else
        {
            if (!name.empty())
                return false;
            name = token;
        }
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1);
        if (text.empty())
            return false;
    }
    if (name.empty())
        return false;
    modifiers = bits;
    key = std::move(name);
    return true;
}

// The number of a function key name such as F8, or 0 when the name is not one from F1 to F24.
inline int FunctionKey(std::string_view name)
{
    if (name.size() < 2 || name.size() > 3 || (name[0] != 'F' && name[0] != 'f'))
        return 0;
    int number = 0;
    for (char c : name.substr(1))
    {
        if (c < '0' || c > '9')
            return 0;
        number = number * 10 + (c - '0');
    }
    return number >= 1 && number <= 24 ? number : 0;
}

// A letter or digit key name, uppercased, or 0.
inline char Character(std::string_view name)
{
    if (name.size() != 1)
        return 0;
    const char c = name[0] >= 'a' && name[0] <= 'z' ? static_cast<char>(name[0] - 'a' + 'A') : name[0];
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : 0;
}
} // namespace hotkey_text
