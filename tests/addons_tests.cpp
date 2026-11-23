// The add-on host with the example add-on, src/addons/example, and no graphics card: finding add-ons by their
// addon.ini, asking before loading, settings, events, the add-on's own controls, drawing over the game and what it
// asks of Unishade.

#include "../src/posix/addons.h"
#include "../src/posix/config.h"
#include "../src/ini_text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{
bool Check(bool condition, const char* what)
{
    if (!condition)
        std::printf("Failed: %s\n", what);
    return condition;
}

// What the add-on asks of Unishade, kept for the tests to look at.
class Services final : public AddonHost
{
public:
    State state;
    std::vector<std::string> messages;
    int screenshots = 0;

    State DescribeForAddons() override { return state; }
    void ShowMessage(const std::string& text) override { messages.push_back(text); }
    void SetEffectsOn(bool on) override { state.effectsOn = on; }
    void TakeScreenshot() override { ++screenshots; }
    std::vector<fs::path> ListPresets() override { return {}; }
    bool UsePreset(const fs::path&) override { return false; }
};

// What the add-on drew over the game, by the functions of a UnishadeDraw that only keep it.
std::vector<std::string> drawn;

void DrawnLine(const UnishadeDraw*, float, float, float, float, uint32_t, float)
{
    drawn.push_back("line");
}

void DrawnRect(const UnishadeDraw*, float x, float y, float width, float, uint32_t, float, float thickness)
{
    drawn.push_back("rect " + std::to_string(int(x)) + " " + std::to_string(int(y)) + " " + std::to_string(int(width)) + (thickness > 0 ? " outline" : " filled"));
}

void DrawnCircle(const UnishadeDraw*, float, float, float, uint32_t, float)
{
    drawn.push_back("circle");
}

void DrawnText(const UnishadeDraw*, float, float, float height, uint32_t color, const char* text)
{
    char line[128];
    std::snprintf(line, sizeof(line), "text %d %08x %s", int(height), color, text);
    drawn.push_back(line);
}

float DrawnTextWidth(const UnishadeDraw*, float height, const char* text)
{
    return height * static_cast<float>(std::strlen(text));
}

void Put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

const Addons::Addon* Named(const Addons& addons, const std::string& folder)
{
    for (const Addons::Addon& addon : addons.List())
        if (addon.folder == folder)
            return &addon;
    return nullptr;
}

const Addons::Control* Labeled(const std::vector<Addons::Control>& controls, const std::string& label)
{
    for (const Addons::Control& control : controls)
        if (control.label == label)
            return &control;
    return nullptr;
}

size_t PlaceOf(const std::vector<Addons::Control>& controls, const std::string& label)
{
    for (size_t i = 0; i < controls.size(); ++i)
        if (controls[i].label == label)
            return i;
    return controls.size();
}
} // namespace

int main()
{
    // A data folder of the test's own, which the host looks up once.
    const fs::path home = fs::temp_directory_path() / ("unishade-addons-tests-" + std::to_string(std::rand()));
    fs::remove_all(home);
    fs::create_directories(home);
    setenv("HOME", home.string().c_str(), 1);
    setenv("XDG_DATA_HOME", (home / "data").string().c_str(), 1);
    const fs::path folder = DataDirectory() / "Addons";

    const std::string library = fs::path(EXAMPLE_ADDON).filename().string();
    Put(folder / "example" / "addon.ini", ReadFile(EXAMPLE_MANIFEST));
    fs::copy_file(EXAMPLE_ADDON, folder / "example" / library);
    Put(folder / "no-id" / "addon.ini", "[Addon]\nName=Nameless\nApi=1\n");
    Put(folder / "newer" / "addon.ini", "[Addon]\nId=someone.newer\nName=Newer\nApi=99\n[windows]\nLibrary=a.dll\n[macos]\nLibrary=a.dylib\n[linux]\nLibrary=a.so\n");
    Put(folder / "other-system" / "addon.ini", "[Addon]\nId=someone.elsewhere\nApi=1\n[amiga]\nLibrary=a.library\n");
    Put(folder / "twin" / "addon.ini", ReadFile(EXAMPLE_MANIFEST));
    Put(folder / "unlisted" / "readme.txt", "A folder without an addon.ini is not an add-on.");
    Put(folder / "Loose.dll", "A file in the Addons folder is not an add-on either.");

    bool ok = true;
    const float one[4] = { 1, 0, 0, 0 };
    const float two[4] = { 2, 0, 0, 0 };
    const float zero[4] = {};
    {
        fx::Runtime runtime;
        Services services;
        Addons addons;
        addons.Init(runtime, services);

        ok &= Check(addons.List().size() == 5, "every folder with an addon.ini is listed, and nothing else");
        const Addons::Addon* example = Named(addons, "example");
        ok &= Check(example && example->manifest.id == "unishade.example" && example->manifest.name == "Example" &&
                        example->manifest.author == "Unishade" && example->manifest.version == "1.0.0",
                    "an add-on is known by its addon.ini before anything of it runs");
        ok &= Check(example && example->state == Addons::State::Pending && !example->official && !addons.Loaded("example"),
                    "one that is not official waits to be allowed, and is not loaded meanwhile");
        const Addons::Addon* nameless = Named(addons, "no-id");
        ok &= Check(nameless && nameless->state == Addons::State::Failed && nameless->manifest.name == "Nameless" && !nameless->error.empty(),
                    "an addon.ini without an Id fails, and says why");
        const Addons::Addon* newer = Named(addons, "newer");
        ok &= Check(newer && newer->state == Addons::State::Failed && newer->error.find("99") != std::string::npos,
                    "an add-on for a newer Unishade says so without being loaded");
        const Addons::Addon* elsewhere = Named(addons, "other-system");
        ok &= Check(elsewhere && elsewhere->state == Addons::State::Failed && elsewhere->error == "It isn't made for this system.",
                    "an add-on without a library for this system says so");
        const Addons::Addon* twin = Named(addons, "twin");
        ok &= Check(twin && twin->state == Addons::State::Failed && twin->error.find("same Id") != std::string::npos,
                    "of two add-ons with one Id, the second fails");

        // Turning on one that cannot be used leaves it failed.
        addons.SetEnabled("twin", true);
        addons.Update();
        ok &= Check(Named(addons, "twin")->state == Addons::State::Failed, "an add-on that cannot be used stays failed");

        addons.Approve("example", true);
        addons.Update();
        example = Named(addons, "example");
        ok &= Check(example->state == Addons::State::Enabled && addons.Loaded("example"), "once allowed, it loads and starts");
        ok &= Check(example->settings.size() == 4 && example->settings[1].label == "Times" && example->settings[1].group == "Message" &&
                        example->settings[1].value[0] == 1 && example->settings[0].choices.size() == 3,
                    "its settings are the ones it lists, with their defaults and groups");
        ok &= Check(!addons.ChangesPicture(*example) && addons.HasControls(*example) && !addons.ReadsKeys(), "it says what it does");

        // A button's press is for the next update.
        addons.SetSetting("example", 2, one);
        addons.Update();
        ok &= Check(services.messages == std::vector<std::string>{ "Hello!" }, "a press reaches its update, which asks Unishade for a message");
        addons.Update();
        ok &= Check(services.messages.size() == 1, "and only once");

        // It is told of the game, and its settings are kept for this computer, since it leaves the picture alone.
        services.state.playing = true;
        services.state.gameName = "Test Game";
        addons.Update();
        addons.SetSetting("example", 0, one);
        addons.SetSetting("example", 1, two);
        addons.SetSetting("example", 2, one);
        addons.Update();
        ok &= Check(services.messages.size() == 2 && services.messages[1] == "Welcome, Test Game! Welcome, Test Game!",
                    "it is told when a game starts, and gets its settings' values by their place");
        std::string kept;
        ok &= Check(IniText(ReadFile(DataDirectory() / "Unishade.ini")).Get("ADDON:unishade.example", "Times", kept) && kept == "2",
                    "its settings are kept under its Id");
        ok &= Check(IniText(ReadFile(DataDirectory() / "Unishade.ini")).Get("ADDONS", "unishade.example", kept) && kept == "1",
                    "and so is the answer");

        // Its own controls: drawn for every frame they show, and told what the user did the next time.
        const std::vector<Addons::Control>* controls = &addons.Controls("example");
        ok &= Check(controls->size() == 6 && (*controls)[0].kind == Addons::Control::Kind::Heading && (*controls)[1].kind == Addons::Control::Kind::Text &&
                        (*controls)[1].label.find("Test Game") != std::string::npos,
                    "it draws controls of its own, with what Unishade tells it of the game");
        const Addons::Control* quiet = Labeled(*controls, "Quiet");
        ok &= Check(quiet && quiet->kind == Addons::Control::Kind::Toggle && quiet->value[0] == 0 && !quiet->tooltip.empty(), "with values it keeps itself");
        ok &= Check(Labeled(*controls, "Turn effects off") && Labeled(*controls, "Take a screenshot"), "and buttons for what Unishade is doing");
        addons.UseControl("example", PlaceOf(*controls, "Turn effects off"), one);
        addons.UseControl("example", PlaceOf(*controls, "Take a screenshot"), one);
        addons.UseControl("example", PlaceOf(*controls, "Quiet"), one);
        controls = &addons.Controls("example");
        ok &= Check(!services.state.effectsOn && services.screenshots == 1, "a press reaches the add-on, which asks Unishade to act");
        ok &= Check(Labeled(*controls, "Quiet") && Labeled(*controls, "Quiet")->value[0] == 1, "a value the user changed is the add-on's from then on");
        controls = &addons.Controls("example");
        ok &= Check(Labeled(*controls, "Turn effects on") && services.screenshots == 1, "what it draws follows what happened, and a press counts once");
        addons.SetSetting("example", 2, one);
        addons.Update();
        ok &= Check(services.messages.size() == 2, "its own values change what it does");

        // Over the game: it draws with the functions it is handed, in the picture's pixels, with its settings.
        UnishadeDraw draw{};
        draw.size = sizeof(draw);
        draw.width = 1920;
        draw.height = 1080;
        draw.scale = 2;
        draw.line = DrawnLine;
        draw.rect = DrawnRect;
        draw.circle = DrawnCircle;
        draw.text = DrawnText;
        draw.textWidth = DrawnTextWidth;
        addons.Draw(draw);
        ok &= Check(drawn.empty(), "it draws nothing over the game until its setting says to");
        services.state.frameRate = 60;
        addons.SetSetting("example", 3, one);
        addons.Draw(draw);
        ok &= Check(drawn == std::vector<std::string>{ "rect 24 24 224 filled", "text 32 ffffffff 60 fps" },
                    "it draws over the game, with its settings, what Unishade says and the size Unishade's menu has");
        drawn.clear();
        addons.SetBypassed("example", true);
        addons.Draw(draw);
        ok &= Check(drawn.empty(), "an add-on that is bypassed draws nothing");
        addons.SetBypassed("example", false);
        addons.SetSetting("example", 3, zero);

        services.state.playing = false;
        addons.Update();
        controls = &addons.Controls("example");
        ok &= Check(controls->size() == 5 && (*controls)[1].label.find("No game") != std::string::npos, "it is told when the game stops");

        addons.SetEnabled("example", false);
        addons.Update();
        ok &= Check(Named(addons, "example")->state == Addons::State::Disabled && !addons.Loaded("example") && addons.Controls("example").empty(),
                    "turned off, it is unloaded");
        addons.SetEnabled("example", true);
        addons.Update();
        addons.Shutdown();
    }
    {
        // Unishade starting again.
        fx::Runtime runtime;
        Services services;
        Addons addons;
        addons.Init(runtime, services);
        const Addons::Addon* example = Named(addons, "example");
        ok &= Check(example && example->state == Addons::State::Enabled, "an add-on that was allowed loads without asking again");
        ok &= Check(example && example->settings.size() == 4 && example->settings[0].value[0] == 1 && example->settings[1].value[0] == 2,
                    "with its settings as they were left");

        // The folder is read again when it changes: an add-on taken away stops.
        fs::remove_all(folder / "example");
        addons.Scan();
        addons.Update();
        ok &= Check(!Named(addons, "example") && Named(addons, "twin") && Named(addons, "twin")->manifest.id == "unishade.example" &&
                        Named(addons, "twin")->error.find("missing") != std::string::npos,
                    "an add-on taken away is gone, and its twin has the Id to itself, though not its library");
        addons.Shutdown();
    }

    std::error_code ignored;
    fs::remove_all(home, ignored);
    return ok ? 0 : 1;
}
