#include "../src/game_scan.h"

#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}
} // namespace

int main()
{
    bool ok = true;

    // Steam lists its libraries with their paths, and other keys with sections after them.
    const char* libraries = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps"
		{
			"228980"		"451654321"
		}
	}
	"1"
	{
		"Path"		"D:\\SteamLibrary"
		"apps"
		{
		}
	}
}
)";
    const std::vector<std::string> paths = game_scan::VdfValues(libraries, "path");
    ok &= Check(paths == std::vector<std::string>{ "C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary" },
                "each library's path is found, with its backslashes single");
    ok &= Check(game_scan::VdfValues(libraries, "apps").empty(), "a key with a section has no value");
    ok &= Check(game_scan::VdfValues(libraries, "228980") == std::vector<std::string>{ "451654321" }, "a value inside a section is found");

    const char* manifest = R"("AppState"
{
	"appid"		"3241660"
	"name"		"R.E.P.O."
	"installdir"		"REPO"
	"UserConfig"
	{
		"name"		"R.E.P.O."
	}
}
)";
    ok &= Check(game_scan::VdfValues(manifest, "installdir") == std::vector<std::string>{ "REPO" }, "a manifest names the game's folder");
    ok &= Check(game_scan::VdfValues(manifest, "name").front() == "R.E.P.O.", "and the game, first at the top");
    ok &= Check(game_scan::VdfValues("\"name\" \"A \\\"quoted\\\" game\"", "name") == std::vector<std::string>{ "A \"quoted\" game" },
                "an escaped quote stays in the value");
    ok &= Check(game_scan::VdfValues("\"name\"", "name").empty() && game_scan::VdfValues("", "name").empty(), "a file cut short gives nothing");

    const char* item = R"({
	"FormatVersion": 0,
	"bIsIncompleteInstall": false,
	"LaunchExecutable": "FortniteGame/Binaries/Win64/FortniteLauncher.exe",
	"DisplayName": "Fortnite",
	"InstallLocation": "D:\\Epic Games\\Fortnite",
	"AppCategories": [ "games" ]
})";
    ok &= Check(game_scan::JsonString(item, "DisplayName") == "Fortnite", "an Epic item names its game");
    ok &= Check(game_scan::JsonString(item, "InstallLocation") == "D:\\Epic Games\\Fortnite", "and its folder, with its backslashes single");
    ok &= Check(!game_scan::JsonString(item, "FormatVersion") && !game_scan::JsonString(item, "Missing"), "a key without a string gives nothing");
    return ok ? 0 : 1;
}
