// Tests for the names every platform gives preset folders and accepts for presets, folders and games: the same
// rules for Windows' std::wstring and the UTF-8 text of macOS and Linux.

#include "base/names.h"

#include <cstdio>
#include <string>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}
} // namespace

int main()
{
    bool ok = true;
    using namespace std::string_literals;

    // A game's presets folder is named after it, with a name Windows also accepts.
    ok &= Check(FolderName("Roblox"s) == "Roblox" && FolderName(L"Roblox"s) == L"Roblox", "keeps a plain name");
    ok &= Check(FolderName("Half-Life: Alyx"s) == "Half-Life  Alyx" && FolderName("a/b\\c|d"s) == "a b c d" &&
                    FolderName(L"Half-Life: Alyx"s) == L"Half-Life  Alyx",
                "replaces what Windows does not allow");
    ok &= Check(FolderName("Game [Beta]; a=b"s) == "Game  Beta   a b" && FolderName(L"[Game]"s) == L"Game", "replaces what would break its line in the ini");
    ok &= Check(FolderName("  Game... "s) == "Game" && FolderName("\t?*"s).empty() && FolderName(L" .. "s).empty(), "trims the ends Windows drops");
    ok &= Check(FolderName("Pok\xC3\xA9mon \xE2\x98\x85"s) == "Pok\xC3\xA9mon \xE2\x98\x85" && FolderName(L"Pok\u00E9mon \u2605"s) == L"Pok\u00E9mon \u2605",
                "keeps letters and symbols beyond ASCII");
    // U+015C ends in the byte of a backslash, and U+013A in that of a colon.
    ok &= Check(FolderName(L"\u015Cpiew \u013A"s) == L"\u015Cpiew \u013A", "compares whole characters");

    // Names Windows keeps for devices, whatever extension follows them, get a word added, since Windows would not
    // open them as folders.
    for (const char* device : { "CON", "con", "PRN.txt", "Nul .ini", "CONIN$", "conout$.log", "COM0", "com9", "LPT1", "COM\xC2\xB9", "lpt\xC2\xB3.x" })
        ok &= Check(CheckName(device, false) == NameIssue::Device && FolderName(device) == device + " game"s, device);
    ok &= Check(FolderName("aux "s) == "aux game" && FolderName("AUX:"s) == "AUX game", "looks for device names once the ends are trimmed");
    ok &= Check(FolderName(L"COM\u00B2"s) == L"COM\u00B2 game" && FolderName(L"nul.txt"s) == L"nul.txt game" &&
                    CheckName(L"lPt\u00B9"s, true) == NameIssue::Device,
                "reads wide device names");
    for (const char* name : { "CONSOLE", "COM10", "LPT", "COMA", "COM\xC2\xB4", "COM\xC2\xB9\xC2\xB9", "Con Game", "xCON" })
        ok &= Check(CheckName(name, false) == NameIssue::None && FolderName(name) == name, name);
    ok &= Check(CheckName(L"COM\u00B4"s, false) == NameIssue::None && CheckName(L"COM\u2079"s, false) == NameIssue::None,
                "takes only the superscripts Windows does as numbers");
    ok &= Check(DeviceName("COM1 .txt") == "COM1" && DeviceName(L"nul.") == L"nul", "a device name ends at the first dot");

    // Names typed for presets and folders, with spaces trimmed from their ends.
    ok &= Check(CheckName(""s, false) == NameIssue::Empty && CheckName(L""s, true) == NameIssue::Empty, "a name cannot be empty");
    ok &= Check(CheckName("a\tb"s, false) == NameIssue::Control && CheckName(L"a\x1F"s, true) == NameIssue::Control, "rejects control characters");
    ok &= Check(CheckName("a:b"s, false) == NameIssue::Character && CheckName(L"a?"s, false) == NameIssue::Character, "rejects what Windows does not allow");
    ok &= Check(CheckName("Cinematic [v2] a=b;c"s, false) == NameIssue::None, "presets and folders may hold = ; [ ]");
    ok &= Check(CheckName("Game [v2]"s, true) == NameIssue::Character && CheckName(L"a=b"s, true) == NameIssue::Character,
                "games may not, since their name is also a key");
    ok &= Check(CheckName("name."s, false) == NameIssue::End && CheckName("."s, false) == NameIssue::End && CheckName(L".."s, true) == NameIssue::End &&
                    CheckName("name "s, true) == NameIssue::End,
                "rejects what Windows drops from the end");
    ok &= Check(CheckName("CON"s, false) == NameIssue::Device && CheckName(L"com1.ini"s, true) == NameIssue::Device, "rejects device names");
    ok &= Check(CheckName("Pok\xC3\xA9mon"s, true) == NameIssue::None && CheckName(L"Preset \u2605"s, false) == NameIssue::None, "accepts the rest");

    // A game name that is accepted is its own folder's name, so renaming a game can move its folder there.
    for (const std::string& name : { "Roblox"s, "Half-Life Alyx"s, "Pok\xC3\xA9mon"s, "Game 2.0"s, "a b"s })
        ok &= Check(CheckName(name, true) == NameIssue::None && FolderName(name) == name, name.c_str());
    for (const std::string& name : { "a:b"s, "a=b"s, "[a]"s, "a."s, "CON"s, "com\xC2\xB2"s })
        ok &= Check(CheckName(name, true) != NameIssue::None && FolderName(name) != name, name.c_str());

    std::printf(ok ? "All tests passed.\n" : "Some tests failed.\n");
    return ok ? 0 : 1;
}
