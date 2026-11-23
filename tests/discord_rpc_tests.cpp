// Tests for what goes through Discord's Rich Presence connection, and for finding a game's icon in Discord's list.

#include "discord/discord_rpc.h"

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
    using namespace std::string_literals;

    // Frames go out and come back the same, one whole frame at a time.
    const std::string frame = discord::Frame(discord::kFrame, "{}");
    ok &= Check(frame == "\x01\0\0\0\x02\0\0\0{}"s, "a frame is its opcode and length, little-endian, then the JSON");
    std::string received = frame.substr(0, 9);
    discord::Message message;
    ok &= Check(!discord::TakeFrame(received, message) && received.size() == 9, "waits for the rest of a frame");
    received += frame.substr(9) + discord::Frame(discord::kClose, "{\"code\":4000}");
    ok &= Check(discord::TakeFrame(received, message) && message.opcode == discord::kFrame && message.json == "{}", "takes a whole frame");
    ok &= Check(discord::TakeFrame(received, message) && message.opcode == discord::kClose && message.json == "{\"code\":4000}" && received.empty(),
                "takes the next frame");
    received = "\x01\0\0\0\xFF\xFF\xFF\x7F"s;
    try
    {
        discord::TakeFrame(received, message);
        ok &= Check(false, "refuses a frame longer than Discord sends");
    }
    catch (const std::runtime_error&)
    {
    }

    ok &= Check(discord::Quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\u000a\\u0001\"", "escapes quotes, backslashes and control characters");
    ok &= Check(discord::Quote("Pok\xC3\xA9mon") == "\"Pok\xC3\xA9mon\"", "keeps UTF-8 as it is");
    ok &= Check(discord::Fit(std::string(128, 'a')) == std::string(128, 'a'), "keeps text of 128 bytes");
    ok &= Check(discord::Fit(std::string(127, 'a') + "\xC3\xA9") == std::string(127, 'a'), "cuts long text between two characters");

    // The game's icon, with Unishade's logo beside it, or the logo alone without one.
    const discord::Activity activity{ "Roblox", "Cinematic", "C:\\Roblox\\RobloxPlayerBeta.exe", 1700000000 };
    const std::string json = discord::SetActivityJson(42, activity, "https://cdn.discordapp.com/app-icons/1/a.png", 7);
    ok &= Check(json == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"details\":\"Roblox\",\"state\":\"Preset: Cinematic\","
                        "\"timestamps\":{\"start\":1700000000},\"assets\":{\"large_image\":\"https://cdn.discordapp.com/app-icons/1/a.png\","
                        "\"large_text\":\"Roblox\",\"small_image\":\"" +
                            std::string(discord::kLogo) + "\",\"small_text\":\"Unishade\"}}},\"nonce\":\"7\"}",
                "shows the game, its icon and the preset");
    const std::string plain = discord::SetActivityJson(42, { "X", "", "", 5 }, "", 1);
    ok &= Check(plain == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"timestamps\":{\"start\":5},\"assets\":{\"large_image\":\"" +
                             std::string(discord::kLogo) + "\"}}},\"nonce\":\"1\"}",
                "leaves out text Discord refuses and shows the logo without an icon");

    // Discord's messages are read for their event and error.
    const std::string error = "{\"cmd\":\"SET_ACTIVITY\",\"data\":{\"code\":4000,\"message\":\"Bad \\\"thing\\\" \\u00e9\\ud83d\\ude00\"},\"evt\":\"ERROR\"}";
    ok &= Check(discord::FindString(error, "evt") == "ERROR", "finds the event");
    ok &= Check(discord::FindString(error, "message") == "Bad \"thing\" \xC3\xA9\xF0\x9F\x98\x80", "decodes escapes in an error");
    ok &= Check(discord::FindString("{\"evt\":null,\"data\":{\"user\":{\"id\":\"1\"}}}", "evt").empty(), "finds no event in a reply");
    ok &= Check(discord::FindString(std::string(1000, '[') + std::string(1000, ']'), "evt").empty(), "gives up on JSON nested too deep");

    // Icons are found by the end of the game's path, with as much of the folder as Discord names.
    const std::string list = R"([
        {"executables": [{"is_launcher": false, "name": "robloxplayerbeta.exe", "os": "win32"}, {"name": "roblox.app", "os": "darwin"}],
         "icon_hash": "f2b60e350a2097289b3b0b877495e55f", "id": "363445589247131668", "name": "Roblox"},
        {"executables": [{"name": "_retail_/wow.exe", "os": "win32"}, {"name": ">java", "os": "darwin"}], "icon_hash": "abc", "id": "2"},
        {"executables": [{"name": "noicon.exe", "os": "win32"}], "icon_hash": null, "id": "3"},
        {"executables": null, "icon_hash": "def", "id": "4"},
        {"executables": [{"name": "bad.exe"}], "icon_hash": "../x", "id": "5"}
    ])";
    const std::vector<discord::GameIcon> icons = discord::ParseGameIcons(list);
    ok &= Check(icons.size() == 3, "keeps executables of games with an icon");
    ok &= Check(discord::FindGameIcon(icons, "C:\\Users\\a\\AppData\\Local\\Roblox\\Versions\\version-1\\RobloxPlayerBeta.exe") ==
                    "https://cdn.discordapp.com/app-icons/363445589247131668/f2b60e350a2097289b3b0b877495e55f.png?size=512",
                "finds Roblox's icon in any folder and case");
    ok &= Check(discord::FindGameIcon(icons, "D:\\Games\\World of Warcraft\\_retail_\\Wow.exe") == "https://cdn.discordapp.com/app-icons/2/abc.png?size=512",
                "matches the folder Discord names");
    ok &= Check(discord::FindGameIcon(icons, "D:\\Games\\_classic_\\Wow.exe").empty(), "needs the folder Discord names");
    ok &= Check(discord::FindGameIcon(icons, "D:\\Games\\NotRobloxPlayerBeta.exe").empty(), "matches whole names");
    ok &= Check(discord::FindGameIcon(icons, "C:\\Games\\noicon.exe").empty() && discord::FindGameIcon(icons, "").empty(), "has no icon for other games");
    ok &= Check(discord::ParseGameIcons("[{\"id\": \"1\", ").empty(), "reads nothing from a broken list");
    ok &= Check(discord::ParseGameIcons(R"([{"executables": [{"name": "a\tb.exe"}], "icon_hash": "ab", "id": "1"}])").empty(),
                "leaves out names with control characters");

    // The icons are kept between runs as lines, and lines that are not one are left out.
    const std::vector<discord::GameIcon> kept = discord::ReadGameIcons(discord::FormatGameIcons(icons) + "broken\n\tno name\n");
    ok &= Check(kept.size() == icons.size() && kept[2].executable == icons[2].executable && kept[2].url == icons[2].url,
                "reads back the icons it kept");

    std::printf(ok ? "All tests passed.\n" : "Some tests failed.\n");
    return ok ? 0 : 1;
}
