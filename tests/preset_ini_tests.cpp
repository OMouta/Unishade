// Tests for preset_ini.h: saving a preset changes only what changed, and keeps its comments and blank lines where
// they were.

#include "../src/preset_ini.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

const char* const kAnnotated = "; Made for dark games\n"
                               "Techniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\n"
                               "\n"
                               "; Softer than the default\n"
                               "[Curves.fx]\n"
                               "  ; was 0.8\n"
                               "Contrast=0.650000\n"
                               "# kept as is\n"
                               "Mode=2\n"
                               "a line without an equals sign\n"
                               "\n"
                               "[Empty.fx]\n"
                               "\n"
                               "[LumaSharpen.fx]\n"
                               "sharp_strength=0.800000\n"
                               "; the end\n";
} // namespace

int main()
{
    bool ok = true;

    ok &= Check(PresetIni(kAnnotated).Text() == kAnnotated, "a preset with comments, blank lines and an empty section reads back the same");

    PresetIni edited(kAnnotated);
    edited.Set("curves.fx", "Contrast", "0.500000");
    ok &= Check(edited.Text() == std::string(kAnnotated).replace(std::string(kAnnotated).find("0.650000"), 8, "0.500000"),
                "editing a value changes only that line");

    PresetIni added(kAnnotated);
    added.Set("Curves.fx", "Formula", "4");
    added.Set("", "TechniqueSorting", "Curves@Curves.fx,LumaSharpen@LumaSharpen.fx");
    added.Set("LumaSharpen.fx", "pattern", "1");
    added.Set("Vibrance.fx", "Vibrance", "0.150000");
    ok &= Check(added.Text() == "; Made for dark games\n"
                                "Techniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\n"
                                "TechniqueSorting=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\n"
                                "\n"
                                "; Softer than the default\n"
                                "[Curves.fx]\n"
                                "  ; was 0.8\n"
                                "Contrast=0.650000\n"
                                "# kept as is\n"
                                "Mode=2\n"
                                "Formula=4\n"
                                "a line without an equals sign\n"
                                "\n"
                                "[Empty.fx]\n"
                                "\n"
                                "[LumaSharpen.fx]\n"
                                "sharp_strength=0.800000\n"
                                "pattern=1\n"
                                "; the end\n"
                                "\n"
                                "[Vibrance.fx]\n"
                                "Vibrance=0.150000\n",
                "a new key goes after the section's last key, before the lines that end it");

    PresetIni removed(kAnnotated);
    removed.Remove("Curves.fx", "contrast");
    removed.Set("Curves.fx", "Contrast", "0.1");
    ok &= Check(removed.Text() == "; Made for dark games\n"
                                  "Techniques=Curves@Curves.fx,LumaSharpen@LumaSharpen.fx\n"
                                  "\n"
                                  "; Softer than the default\n"
                                  "[Curves.fx]\n"
                                  "  ; was 0.8\n"
                                  "# kept as is\n"
                                  "Mode=2\n"
                                  "Contrast=0.1\n"
                                  "a line without an equals sign\n"
                                  "\n"
                                  "[Empty.fx]\n"
                                  "\n"
                                  "[LumaSharpen.fx]\n"
                                  "sharp_strength=0.800000\n"
                                  "; the end\n",
                "removing a key keeps the lines around it");

    // A section the preset did not have, emptied again, is left out as before.
    PresetIni created("Techniques=A@A.fx\n");
    created.Set("A.fx", "PreprocessorDefinitions", "X=1");
    created.Remove("A.fx", "PreprocessorDefinitions");
    ok &= Check(created.Text() == "Techniques=A@A.fx\n", "a new section without keys is not written");

    // Line endings become LF and trailing spaces go, as before. Keys and values lose the spaces around them.
    std::string value;
    PresetIni spaced("\xEF\xBB\xBF; note \r\n Techniques = A@A.fx \r\n[A.fx]\r\n");
    ok &= Check(spaced.Get("", "Techniques", value) && value == "A@A.fx", "reads a key with spaces around it");
    ok &= Check(spaced.Text() == "; note\nTechniques=A@A.fx\n[A.fx]\n", "writes LF line endings without trailing spaces");

    std::printf(ok ? "All tests passed.\n" : "Some tests failed.\n");
    return ok ? 0 : 1;
}
