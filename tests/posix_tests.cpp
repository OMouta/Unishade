// Tests for the macOS and Linux host's parts that need no window: shortcuts, presets, the saved game list, file
// names, notices and the checksum Setup verifies presets with.

#include "config.h"
#include "game_list.h"
#include "games.h"
#include "hotkeys.h"
#include "log.h"
#include "preset_ini.h"
#include "setup.h"

#include <unistd.h>

#include <cstdio>
#include <stdexcept>
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

    const fs::path directory = fs::temp_directory_path() / ("unishade-write-test-" + std::to_string(getpid()));
    if (!fs::create_directory(directory))
        return 1;
    const fs::path file = directory / "preset.ini";
    ok &= Check(WriteFile(file, "value=1\n") && ReadFile(file) == "value=1\n", "writes a new file");
    ok &= Check(WriteFile(file, "value=2\n") && ReadFile(file) == "value=2\n", "replaces an existing file");
    const fs::path blocked = directory / "blocked";
    fs::create_directory(blocked);
    ok &= Check(!WriteFile(blocked, "contents"), "a failed rename is reported even when cleanup succeeds");
    ok &= Check(fs::is_directory(blocked) && !fs::exists(blocked.string() + ".tmp"), "failed write keeps the destination and removes the temporary file");
    fs::remove_all(directory);

    // Shortcuts are written as on Windows and read back the same.
    Hotkey hotkey;
    ok &= Check(ParseHotkey("Ctrl+F8", hotkey) && hotkey.modifiers == kCtrl && hotkey.key == ImGuiKey_F8, "reads Ctrl+F8");
    ok &= Check(ParseHotkey("ctrl + shift + pagedown", hotkey) && hotkey.modifiers == (kCtrl | kShift) && hotkey.key == ImGuiKey_PageDown,
                "reads names without case and with spaces");
    ok &= Check(ParseHotkey("Cmd+Shift+U", hotkey) && hotkey.modifiers == (kSuper | kShift) && hotkey.key == ImGuiKey_U, "Cmd is the Super key");
    ok &= Check(ParseHotkey("Win+1", hotkey) && hotkey.modifiers == kSuper && hotkey.key == ImGuiKey_1, "Win is the Super key too");
    ok &= Check(ParseHotkey("Option+Left", hotkey) && hotkey.modifiers == kAlt && hotkey.key == ImGuiKey_LeftArrow, "Option is Alt");
    ok &= Check(!ParseHotkey("Ctrl", hotkey), "a modifier alone is no shortcut");
    ok &= Check(!ParseHotkey("Ctrl+Ctrl+A", hotkey), "a modifier counts once");
    ok &= Check(!ParseHotkey("A+B", hotkey), "one key per shortcut");
    ok &= Check(!ParseHotkey("F25", hotkey) && !ParseHotkey("Ctrl+", hotkey) && !ParseHotkey("", hotkey), "rejects what is not a key");
    for (const Shortcut& shortcut : kShortcuts)
    {
        Hotkey parsed, again;
        ok &= Check(ParseHotkey(shortcut.fallback, parsed) && ParseHotkey(FormatHotkey(parsed), again) && again == parsed,
                    "every default shortcut survives writing and reading");
    }

    // Number pad keys have the same names as on Windows.
    ok &= Check(ParseHotkey("Numpad0", hotkey) && hotkey.modifiers == 0 && hotkey.key == ImGuiKey_Keypad0, "reads Numpad0");
    ok &= Check(ParseHotkey("ctrl+numpad9", hotkey) && hotkey.modifiers == kCtrl && hotkey.key == ImGuiKey_Keypad9, "reads number pad names without case");
    const std::pair<const char*, ImGuiKey> numpad[] = { { "NumpadMultiply", ImGuiKey_KeypadMultiply }, { "NumpadAdd", ImGuiKey_KeypadAdd },
                                                        { "NumpadSubtract", ImGuiKey_KeypadSubtract }, { "NumpadDecimal", ImGuiKey_KeypadDecimal },
                                                        { "NumpadDivide", ImGuiKey_KeypadDivide }, { "Numpad5", ImGuiKey_Keypad5 } };
    for (const auto& [name, key] : numpad)
        ok &= Check(ParseHotkey(std::string("Shift+") + name, hotkey) && hotkey.key == key && FormatHotkey(hotkey) == std::string("Shift+") + name &&
                        IsShortcutKey(key),
                    "reads and writes every number pad key, which can be recorded");
    ok &= Check(!ParseHotkey("Numpad10", hotkey) && !ParseHotkey("Numpad", hotkey) && !ParseHotkey("NumpadEnter", hotkey),
                "rejects number pad keys Windows does not name");
#ifdef __APPLE__
    ok &= Check(IsShortcutKey(ImGuiKey_F20) && !IsShortcutKey(ImGuiKey_F21), "macOS records function keys up to F20");
#else
    ok &= Check(IsShortcutKey(ImGuiKey_F24), "records function keys up to F24");
#endif

    // Presets keep keys before the first section and the order of everything else.
    const std::string text = "PreprocessorDefinitions=A=1,B\r\nTechniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\r\n\r\n"
                             "[Curves.fx]\r\nContrast=0.650000\r\n\r\n[LumaSharpen.fx]\r\nsharp_strength=0.800000\r\n";
    PresetIni preset(text);
    std::string value;
    ok &= Check(preset.Get("", "Techniques", value) && PresetIni::Split(value).size() == 2, "reads the technique list");
    ok &= Check(preset.Get("Curves.fx", "Contrast", value) && value == "0.650000", "reads an effect's value");
    ok &= Check(!preset.Get("", "Contrast", value), "values belong to their section");
    ok &= Check(preset.SectionNames() == std::vector<std::string>{ "Curves.fx", "LumaSharpen.fx" }, "lists sections in order");
    preset.Set("Curves.fx", "Contrast", "0.500000");
    preset.Set("Vibrance.fx", "Vibrance", "0.150000");
    preset.Set("", "TechniqueSorting", "LumaSharpen@LumaSharpen.fx,Curves@Curves.fx");
    ok &= Check(preset.Text() ==
                    "PreprocessorDefinitions=A=1,B\nTechniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\n"
                    "TechniqueSorting=LumaSharpen@LumaSharpen.fx,Curves@Curves.fx\n\n[Curves.fx]\nContrast=0.500000\n\n"
                    "[LumaSharpen.fx]\nsharp_strength=0.800000\n\n[Vibrance.fx]\nVibrance=0.150000\n",
                "writes changes in place and new keys at the end of their section");
    ok &= Check(PresetIni(PresetIni("\xEF\xBB\xBFTechniques=A@A.fx\n").Text()).Get("", "Techniques", value) && value == "A@A.fx",
                "reads presets with a byte order mark");

    ok &= Check(preset.Get("curves.FX", "contrast", value) && value == "0.500000", "section and key names ignore case, as on Windows");
    ok &= Check(PresetEffectFiles(PresetIni("Techniques=A@A.fx,B@b.fx,C@A.FX,Loose\n")) == std::vector<std::string>{ "A.fx", "b.fx" },
                "lists each effect file a preset uses once");

    // games.ini is the same file on every platform.
    const std::vector<GameListEntry> games = { { "RobloxPlayerBeta.exe", "Roblox", true }, { "/usr/games/Café Game", "Café", false } };
    const std::vector<GameListEntry> read = ParseGameList(FormatGameList(games));
    ok &= Check(read.size() == 2 && read[1].executable == games[1].executable && read[1].name == games[1].name && !read[1].enabled,
                "writes and reads the game list, UTF-8 included");
    bool rejected = false;
    try
    {
        ParseGameList("[Games]\nCount=1\n");
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    ok &= Check(rejected, "rejects a damaged game list");

    const auto definitions = ParseDefinitions("A=1,B,,C=x=y");
    ok &= Check(definitions.size() == 3 && definitions[0] == std::pair<std::string, std::string>{ "A", "1" } && definitions[1].second.empty() &&
                    definitions[2].second == "x=y",
                "reads preprocessor definitions");
    ok &= Check(FormatDefinitions(definitions) == "A=1,B,C=x=y", "writes preprocessor definitions back");

    // Saved games match by filename anywhere, by full path exactly, and Wine games by the .exe they run.
    const AutoGame roblox{ "RobloxPlayer", "Roblox" };
    ok &= Check(MatchesProcess(roblox, "/Applications/Roblox.app/Contents/MacOS/RobloxPlayer", ""), "matches a filename in any folder");
    ok &= Check(MatchesProcess(roblox, "/opt/robloxplayer", ""), "compares filenames without case");
    ok &= Check(!MatchesProcess(roblox, "/usr/bin/RobloxPlayerBeta", ""), "a filename must match whole");
    const AutoGame wine{ "RobloxPlayerBeta.exe", "Roblox" };
    ok &= Check(MatchesProcess(wine, "/usr/bin/wine64-preloader", "C:\\Program Files\\Roblox\\RobloxPlayerBeta.exe"),
                "matches the Windows executable a Wine process runs");
    const AutoGame path{ "/usr/games/game", "Game" };
    ok &= Check(MatchesProcess(path, "/usr/games/game", "") && !MatchesProcess(path, "/opt/game", "game"), "a saved path must match exactly");

    // A game's presets folder is named after it, with a name Windows also accepts.
    ok &= Check(FolderName("Roblox") == "Roblox", "keeps a plain name");
    ok &= Check(FolderName("Half-Life: Alyx") == "Half-Life  Alyx" && FolderName("a/b\\c") == "a b c", "replaces what Windows does not allow");
    ok &= Check(FolderName("Pok\xC3\xA9mon \xE2\x98\x85") == "Pok\xC3\xA9mon \xE2\x98\x85", "keeps UTF-8 letters");
    ok &= Check(FolderName("  Game... ") == "Game" && FolderName("\t?*") == "", "trims the ends Windows drops");

    // Screenshots are named after the window's title, which can hold anything.
    ok &= Check(SafeFileName("Roblox") == "Roblox", "keeps a plain title");
    ok &= Check(SafeFileName("../../etc/passwd") == "etc passwd", "a title cannot name another folder");
    ok &= Check(SafeFileName(std::string("a\0b\nc/d\x7f", 8)) == "a b c d", "replaces control characters and slashes");
    ok &= Check(SafeFileName(".hidden") == "hidden" && SafeFileName(" . ") == "Unishade" && SafeFileName("") == "Unishade",
                "no dots in front, and a name when nothing is left");
    const std::string longTitle = std::string(79, 'a') + "\xC3\xA9" + std::string(20, 'b');
    ok &= Check(SafeFileName(longTitle) == std::string(79, 'a'), "shortens long titles without cutting a character in half");

    // The pictures folder from user-dirs.dirs.
    const std::string userDirs = "# written by xdg-user-dirs-update\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_PICTURES_DIR=\"$HOME/Bilder\"\n";
    ok &= Check(UserDirectory(userDirs, "XDG_PICTURES_DIR", "/home/a") == "/home/a/Bilder", "reads a folder in the home folder");
    ok &= Check(UserDirectory("XDG_PICTURES_DIR=\"/data/My \\\"Pictures\\\"\"\n", "XDG_PICTURES_DIR", "/home/a") == "/data/My \"Pictures\"",
                "reads an absolute folder with escaped quotes");
    ok &= Check(UserDirectory("XDG_PICTURES_DIR=\"$HOME/\"\n", "XDG_PICTURES_DIR", "/home/a").empty() &&
                    UserDirectory(userDirs, "XDG_MUSIC_DIR", "/home/a").empty() && UserDirectory("XDG_PICTURES_DIR=\"Pictures\"", "XDG_PICTURES_DIR", "/home/a").empty(),
                "no folder when it is turned off, missing or not a path");

    // Notices keep the last 40, each message once.
    for (int i = 0; i < 50; ++i)
        Report(LogLevel::Info, "Notice %d", i);
    Report(LogLevel::Warning, "Notice %d", 49);
    const std::vector<Notice> notices = Notices();
    ok &= Check(notices.size() == 40 && notices.front().text == "Notice 10" && notices.back().text == "Notice 49", "keeps the last 40 notices once each");

    // SHA-256, from the standard's own examples.
    ok &= Check(Sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "hashes nothing");
    ok &= Check(Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "hashes abc");
    ok &= Check(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                "hashes across two blocks");
    ok &= Check(Sha256(std::string(1000, 'a')).size() == 64, "hashes long input");

    std::printf(ok ? "All tests passed.\n" : "Some tests failed.\n");
    return ok ? 0 : 1;
}
