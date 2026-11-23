#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Rules for names that become file and folder names, such as a game's presets folder, so that a name made on any
// platform also works on Windows. Windows passes std::wstring, macOS and Linux UTF-8 std::string, and both take
// the same rules from the templates below.

// What is wrong with a name typed for a preset, a folder or a game, once spaces are trimmed from its ends. A game's
// name is also its folder's name and its key in [GamePresets], so it cannot hold what FolderName replaces.
enum class NameIssue
{
    None,
    Empty,
    Control,   // a control character
    Character, // one of \ / : * ? " < > |, and for a game also = ; [ ]
    End,       // a dot or space at the end, which Windows drops; this also rules out . and ..
    Device,    // a name Windows keeps for a device
};

namespace name_rules
{
// What Windows does not allow in file names.
template <class Char>
bool InvalidInName(Char character)
{
    const auto code = static_cast<std::make_unsigned_t<Char>>(character);
    return code < 32 || (code < 128 && std::string_view("\\/:*?\"<>|").find(static_cast<char>(code)) != std::string_view::npos);
}

// What a game's name cannot hold either, since it is also the game's key in [GamePresets].
template <class Char>
bool InvalidInGameName(Char character)
{
    const auto code = static_cast<std::make_unsigned_t<Char>>(character);
    return InvalidInName(character) || (code < 128 && std::string_view("=;[]").find(static_cast<char>(code)) != std::string_view::npos);
}

template <class Char>
char32_t LowerAscii(Char character)
{
    const char32_t code = static_cast<std::make_unsigned_t<Char>>(character);
    return code >= U'A' && code <= U'Z' ? static_cast<char32_t>(code + (U'a' - U'A')) : code;
}

template <class Char>
bool SameAscii(std::basic_string_view<Char> text, std::string_view ascii)
{
    if (text.size() != ascii.size())
        return false;
    for (size_t i = 0; i < text.size(); ++i)
        if (LowerAscii(text[i]) != LowerAscii(ascii[i]))
            return false;
    return true;
}

template <class Char>
std::basic_string_view<Char> DeviceName(std::basic_string_view<Char> name)
{
    name = name.substr(0, name.find(Char('.')));
    while (!name.empty() && name.back() == Char(' '))
        name.remove_suffix(1);
    return name;
}

// Names Windows keeps for devices, such as CON or COM1, whatever extension follows them. COM and LPT also take the
// superscripts one to three as numbers.
template <class Char>
bool ReservedName(std::basic_string_view<Char> name)
{
    const std::basic_string_view<Char> device = DeviceName(name);
    for (const std::string_view reserved : { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" })
        if (SameAscii(device, reserved))
            return true;
    if (device.size() < 4 || !(SameAscii(device.substr(0, 3), "COM") || SameAscii(device.substr(0, 3), "LPT")))
        return false;
    const std::basic_string_view<Char> number = device.substr(3);
    if (number.size() == 1 && number[0] >= Char('0') && number[0] <= Char('9'))
        return true;
    // U+00B9, U+00B2 and U+00B3, which take two bytes in UTF-8.
    const auto superscript = [](char32_t code) { return code == U'\xB9' || code == U'\xB2' || code == U'\xB3'; };
    if constexpr (sizeof(Char) == 1)
        return number.size() == 2 && static_cast<unsigned char>(number[0]) == 0xC2 && superscript(static_cast<unsigned char>(number[1]));
    else
        return number.size() == 1 && superscript(static_cast<char32_t>(number[0]));
}

template <class Char>
std::basic_string<Char> FolderName(std::basic_string<Char> name)
{
    for (Char& character : name)
        if (InvalidInGameName(character))
            character = Char(' ');
    name.erase(0, name.find_first_not_of(Char(' ')));
    // Windows drops dots and spaces from the end of names.
    const Char ends[] = { Char('.'), Char(' '), Char() };
    name.erase(name.find_last_not_of(ends) + 1);
    if (ReservedName(std::basic_string_view<Char>(name)))
        for (const char character : std::string_view(" game"))
            name += Char(character);
    return name;
}

template <class Char>
NameIssue CheckName(std::basic_string_view<Char> name, bool game)
{
    if (name.empty())
        return NameIssue::Empty;
    for (const Char character : name)
    {
        if (static_cast<std::make_unsigned_t<Char>>(character) < 32)
            return NameIssue::Control;
        if (game ? InvalidInGameName(character) : InvalidInName(character))
            return NameIssue::Character;
    }
    if (name.back() == Char('.') || name.back() == Char(' '))
        return NameIssue::End;
    if (ReservedName(name))
        return NameIssue::Device;
    return NameIssue::None;
}
} // namespace name_rules

// A game's presets folder is named after it, with what Windows does not allow in names replaced and what Windows
// drops from their ends left out, and with " game" after names Windows keeps for devices. The name is also the
// game's key in [GamePresets], so it leaves out what would break that line too.
inline std::string FolderName(std::string name)
{
    return name_rules::FolderName(std::move(name));
}

inline std::wstring FolderName(std::wstring name)
{
    return name_rules::FolderName(std::move(name));
}

// What keeps a typed name from being used, if anything. game is set for a game's name.
inline NameIssue CheckName(std::string_view name, bool game)
{
    return name_rules::CheckName(name, game);
}

inline NameIssue CheckName(std::wstring_view name, bool game)
{
    return name_rules::CheckName(name, game);
}

// The part of a name Windows compares with its device names: up to the first dot, without spaces at its end.
inline std::string_view DeviceName(std::string_view name)
{
    return name_rules::DeviceName(name);
}

inline std::wstring_view DeviceName(std::wstring_view name)
{
    return name_rules::DeviceName(name);
}
