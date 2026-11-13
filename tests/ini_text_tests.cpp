#include "../src/ini_text.h"
#include <cstdio>

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

    // A key for a missing section goes under that section's new header, not the section before it.
    IniText ini("[GENERAL]\r\nPresetPath=a.ini\r\n\r\n[OVERLAY]\r\nTutorialProgress=0\r\n");
    ini.Set("STYLE", "Font", "segoeui.ttf");
    ini.Set("OVERLAY", "TutorialProgress", "4");
    ini.Set("GENERAL", "NoReloadOnInit", "0");
    ok &= Check(ini.Text() == "[GENERAL]\r\nPresetPath=a.ini\r\nNoReloadOnInit=0\r\n\r\n[OVERLAY]\r\nTutorialProgress=4\r\n\r\n[STYLE]\r\nFont=segoeui.ttf\r\n",
                "sections and keys land where they belong");

    std::string value;
    ok &= Check(ini.Get("STYLE", "Font", value) && value == "segoeui.ttf", "reads a key back");
    ok &= Check(!ini.Get("OVERLAY", "Font", value), "a key belongs to one section");
    ok &= Check(!ini.Get("GENERAL", "Preset", value), "a key must match whole");

    // The byte order mark and line endings stay as they were.
    IniText unix("\xEF\xBB\xBF[ADDON]\nDisabledAddons=A\n");
    unix.Set("ADDON", "DisabledAddons", "");
    ok &= Check(unix.Text() == "\xEF\xBB\xBF[ADDON]\nDisabledAddons=\n", "keeps the byte order mark and LF endings");

    IniText empty("");
    empty.Set("OVERLAY", "TutorialProgress", "4");
    ok &= Check(empty.Text() == "[OVERLAY]\r\nTutorialProgress=4\r\n", "fills an empty file");
    ok &= Check(!IniText("[A]\r\nB=1\r\n").Changed(), "unchanged until Set");

    // Hand-edited files: spaces and tabs around names and values, and names in another case.
    IniText edited("[INPUT]\nToggleKey = End\n[ HOST ] ; mine\n\tAutoSavePresets\t=0 \nkeep this line \n\n[General]\nPresetPath=a.ini\n");
    ok &= Check(edited.Get("INPUT", "ToggleKey", value) && value == "End", "reads a key with spaces around the equals sign");
    ok &= Check(edited.Get("HOST", "AutoSavePresets", value) && value == "0", "reads a section and value with spaces and tabs");
    ok &= Check(edited.Get("GENERAL", "presetpath", value) && value == "a.ini", "section names and keys ignore case");
    ok &= Check(!edited.Get("INPUT", "Toggle", value) && !edited.Get("HOST", "keep this line", value), "a key still must match whole");
    edited.Set("INPUT", "ToggleKey", "Home");
    edited.Set("HOST", "AutoSavePresets", "1");
    edited.Set("host", "SkipTitleBar", "0");
    edited.Set("GENERAL", "PresetPath", "b.ini");
    ok &= Check(edited.Text() == "[INPUT]\nToggleKey = Home\n[ HOST ] ; mine\n\tAutoSavePresets\t=1\nkeep this line \nSkipTitleBar=0\n\n"
                                 "[General]\nPresetPath=b.ini\n",
                "replaces values in place, keeping the key as written and every other line");
    IniText spaced("[A]\nB = \n \n[C]\n");
    spaced.Set("A", "B", "1");
    spaced.Set("A", "D", "2");
    ok &= Check(spaced.Text() == "[A]\nB = 1\nD=2\n \n[C]\n", "a line of spaces ends a section");

    return ok ? 0 : 1;
}
