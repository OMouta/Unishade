#include "../src/ui/kit.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kit
{
ImTextureID Picture(const std::string&, float) { return ImTextureID_Invalid; }
ImU32 PictureTint(const std::string&) { return 0; }
ImTextureID GameImage(GameView, ImVec2*) { return ImTextureID_Invalid; }
} // namespace kit

// Dear ImGui's hooks for its test engine tell where each control of the last frame was, by its label.
namespace
{
struct Item
{
    ImGuiID id;
    ImRect rect;
    std::string label;
};
std::vector<Item> items;
} // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*)
{
    items.push_back({ id, bb, {} });
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
    for (auto item = items.rbegin(); item != items.rend(); ++item)
        if (item->id == id)
        {
            item->label = label ? label : "";
            return;
        }
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID)
{
    return nullptr;
}

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

class Host final : public launcher::Host
{
public:
    bool picking = false;
    bool add = true;
    std::optional<size_t> chosen;
    int closed = 0;
    std::vector<std::string> calls;

    void OpenPicker(bool adding) override { picking = true; add = adding; }
    void ChooseWindow(size_t window) override { chosen = window; picking = false; }
    void ClosePicker() override { ++closed; picking = false; }

    void UsePreset(size_t game, std::string_view preset) override { calls.push_back("use " + std::to_string(game) + " " + std::string(preset)); }
    void SaveChanges() override { calls.push_back("save"); }
    void DiscardChanges() override { calls.push_back("discard"); }
    void SetEffect(size_t effect, bool on) override { calls.push_back("effect " + std::to_string(effect) + (on ? " on" : " off")); }
    void RemoveEffect(size_t effect) override { calls.push_back("effect " + std::to_string(effect) + " removed"); }
    void MoveEffect(size_t from, size_t to) override { calls.push_back("move " + std::to_string(from) + " " + std::to_string(to)); }
    void CloseMenu() override { calls.push_back("close"); }
    void Quit(bool save) override { calls.push_back(save ? "quit saving" : "quit"); }
    void KeepRunning() override { calls.push_back("keep running"); }
    const launcher::EffectSettings* DescribeSettings(size_t) override { return &settings; }
    launcher::EffectSettings settings;

    void SetSwitch(launcher::Switch, bool) override {}
    void PressCard(size_t, size_t) override {}
    void OpenLink(size_t) override {}
    void OpenUrl(std::string_view) override {}
    void DismissNotices() override {}
    void DetectAutomatically() override {}
    void SetGameEnabled(size_t, bool) override {}
    bool RenameGame(size_t, std::string_view) override { return true; }
    void RemoveGame(size_t) override {}
    void UndoRemove() override {}
    void OpenPresetsFolder(size_t) override {}
    void SetPerformance(std::optional<size_t>, launcher::PerformanceSetting, int) override {}
    void StepMenuSize(int) override {}
    void SetMenuSize(int) override {}
    void PlaceMenu(float, float) override {}
    void SetMenuOpacity(float) override {}
    void RecordShortcut(int) override {}
    void ClearShortcut(size_t) override {}
    void ResetShortcuts() override {}
    bool NewPreset(size_t, std::string_view) override { return true; }
    bool CopyPreset(size_t, std::string_view, std::string_view) override { return true; }
    bool RenamePreset(std::string_view, std::string_view) override { return true; }
    void MovePreset(std::string_view, std::string_view) override {}
    void DeletePreset(std::string_view) override {}
    void SetSetting(size_t, size_t, const launcher::EffectSetting&) override {}
    void ResetSettings(size_t, std::optional<size_t>) override {}
    void SetDefinition(size_t, size_t, std::string_view) override {}
    void PointAtSettings(size_t, std::optional<size_t>, std::optional<size_t>) override {}
    void Compare(bool) override {}
    void TakeScreenshot(bool) override {}
    void OpenScreenshots() override {}
    void DownloadEffects() override { calls.push_back("download"); }
    void ReloadEffects() override {}
    void OpenAddonsFolder() override {}

    void InstallAddon(size_t addon) override { calls.push_back("install " + std::to_string(addon)); }
    void CancelInstall(size_t addon) override { calls.push_back("cancel " + std::to_string(addon)); }
    void LoadAddon(size_t addon, bool load) override { calls.push_back((load ? "load " : "leave ") + std::to_string(addon)); }
    void SetAddon(size_t addon, bool on) override { calls.push_back("addon " + std::to_string(addon) + (on ? " on" : " off")); }
    void BypassAddon(size_t addon, bool bypassed) override { calls.push_back("addon " + std::to_string(addon) + (bypassed ? " bypassed" : " shown")); }
    void SetAddonSetting(size_t addon, size_t setting, const launcher::EffectSetting& value) override
    {
        calls.push_back("addon " + std::to_string(addon) + " setting " + std::to_string(setting) + " " + std::to_string(value.ints[0]));
    }
};

void Frame(Host& host, const launcher::Model& model, launcher::Place place = launcher::Place::Window)
{
    items.clear();
    ImGui::NewFrame();
    launcher::Draw(model, host, 1, place);
    ImGui::Render();
}

void Frame(Host& host)
{
    launcher::Model model;
    if (host.picking)
        model.picker = launcher::Picker{ host.add, { { "Game window", "game.exe", "" } } };
    Frame(host, model);
}

// How many controls of the last frame have a label.
size_t Count(const char* label)
{
    return static_cast<size_t>(std::count_if(items.begin(), items.end(), [&](const Item& item) { return item.label == label; }));
}

// A control of the last frame that shares its ID with another, which Dear ImGui cannot tell apart.
std::optional<std::string> SharedId()
{
    for (size_t i = 0; i < items.size(); ++i)
        for (size_t j = i + 1; j < items.size(); ++j)
            if (items[i].id && items[i].id == items[j].id && items[i].rect.Min.x != items[j].rect.Min.x)
                return items[i].label.empty() ? "an unlabelled control" : items[i].label;
    return std::nullopt;
}

// The place of the nth control with a label in the last frame.
std::optional<ImVec2> Find(const char* label, size_t nth = 0)
{
    for (const Item& item : items)
        if (item.label == label && nth-- == 0)
            return item.rect.GetCenter();
    return std::nullopt;
}

// Presses and lets go of the left button on the nth control with a label. Returns false when there is none.
bool Click(Host& host, const launcher::Model& model, const char* label, size_t nth = 0, launcher::Place place = launcher::Place::Window)
{
    const std::optional<ImVec2> at = Find(label, nth);
    if (!at)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at->x, at->y);
    Frame(host, model, place);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Frame(host, model, place);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Frame(host, model, place);
    Frame(host, model, place);
    return true;
}

// A second of frames, long enough for what opens or closes to get there.
void Settle(Host& host, const launcher::Model& model, launcher::Place place = launcher::Place::Window)
{
    for (int i = 0; i < 60; ++i)
        Frame(host, model, place);
}

void PressEscape(Host& host, const launcher::Model& model, launcher::Place place = launcher::Place::Window)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Escape, true);
    Frame(host, model, place);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    Frame(host, model, place);
}

void OpenPicker(Host& host, bool add)
{
    host.OpenPicker(add);
    host.chosen.reset();
    for (int i = 0; i < 3; ++i)
        Frame(host);
}

bool PickerTests(Host& host)
{
    ImGuiIO& io = ImGui::GetIO();
    bool ok = true;
    Frame(host);
    for (bool add : { true, false })
    {
        OpenPicker(host, add);
        const auto row = Find("window");
        ok &= Check(row.has_value(), "picker lists an open window");
        if (!row)
            break;
        io.AddMousePosEvent(row->x, row->y);
        Frame(host);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Frame(host);
        ok &= Check(host.picking && host.closed == 0 && !host.chosen, "pressing a window row keeps the picker open");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Frame(host);
        ok &= Check(!host.picking && host.chosen == 0, "releasing a window row selects it");
        Frame(host);
    }

    OpenPicker(host, true);
    io.AddMousePosEvent(10, 10);
    Frame(host);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Frame(host);
    ok &= Check(!host.picking && host.closed == 1 && !host.chosen, "clicking outside dismisses the picker");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Frame(host);
    return ok;
}

// Without effects nothing shows in a game, so the status offers the download where a new user looks first.
bool StatusTests(Host& host)
{
    bool ok = true;
    launcher::Model model;
    model.status.title = "Waiting for a game";
    Frame(host, model);
    ok &= Check(!Find("Download effects and presets"), "the status offers no download while effects are installed");
    model.status.canDownload = true;
    Frame(host, model);
    ok &= Check(Click(host, model, "Download effects and presets"), "the status offers the download when no effects are installed");
    ok &= Check(host.calls == std::vector<std::string>{ "download" }, "its button starts the download");
    return ok;
}

// A game being played with two presets of its own, the first in use.
launcher::Model PlayedGame(bool changed)
{
    launcher::Model model;
    model.editPresets = true;
    launcher::Game game{ .id = "game", .name = "Game", .executable = "game.exe", .running = true };
    game.presets = { { "a.ini", "A", "", true }, { "b.ini", "B", "" } };
    game.preset = "a.ini";
    game.changed = changed;
    model.games.push_back(std::move(game));
    return model;
}

// Switching presets with unsaved changes asks first, and switches only once they are saved or discarded.
bool UnsavedTests(Host& host)
{
    bool ok = true;
    launcher::Model model = PlayedGame(false);
    Settle(host, model);
    ok &= Check(Click(host, model, "next"), "the page of the game being played has the tray");
    ok &= Check(host.calls == std::vector<std::string>{ "use 0 b.ini" }, "a preset without unsaved changes switches at once");

    host.calls.clear();
    model = PlayedGame(true);
    Settle(host, model);
    Click(host, model, "next");
    Settle(host, model);
    ok &= Check(host.calls.empty(), "unsaved changes hold the switch");
    ok &= Check(Click(host, model, "Save"), "the switch asks what to do with the changes");
    ok &= Check(host.calls == std::vector<std::string>{ "save", "use 0 b.ini" }, "saving the changes then switches");

    host.calls.clear();
    Settle(host, model);
    Click(host, model, "next");
    Settle(host, model);
    // The question is above the tray's buttons, which offer Discard too.
    ok &= Check(Click(host, model, "Discard"), "the question offers to discard the changes");
    ok &= Check(host.calls == std::vector<std::string>{ "discard", "use 0 b.ini" }, "discarding the changes then switches");

    host.calls.clear();
    Settle(host, model);
    Click(host, model, "next");
    Settle(host, model);
    ok &= Check(Click(host, model, "Cancel"), "the question can be cancelled");
    ok &= Check(host.calls.empty(), "cancelling keeps the preset");
    return ok;
}
// The game being played with three effects, the first and the last on.
launcher::Model EffectsModel()
{
    launcher::Model model = PlayedGame(false);
    model.effects = launcher::Effects{ .list = { { "A@a.fx", "A", "a.fx", true }, { "B@b.fx", "B", "b.fx", false }, { "C@c.fx", "C", "c.fx", true } } };
    return model;
}

// The page of the game being played has the tray: a tile for each effect that is on.
bool EffectsTests(Host& host)
{
    bool ok = true;
    launcher::Model model = EffectsModel();
    Settle(host, model);
    ok &= Check(Count("effect") == 2, "the effects that are on have tiles, and the others wait in Add effect");
    ok &= Check(Click(host, model, "on"), "each tile has a dot");
    ok &= Check(host.calls == std::vector<std::string>{ "effect 0 off" }, "the dot turns the effect off");
    model.effects->list[0].on = false;
    model.effects->list[0].kept = true;
    Frame(host, model);
    ok &= Check(Count("effect") == 2, "an effect turned off keeps its tile, to be turned back on where it was");

    // Its settings can take it out of the preset.
    host.calls.clear();
    ok &= Check(Click(host, model, "effect"), "a tile opens its effect's settings");
    Settle(host, model);
    ok &= Check(Click(host, model, "Remove"), "the settings offer to remove the effect");
    ok &= Check(host.calls == std::vector<std::string>{ "effect 0 removed" }, "removing is not turning off");
    model.effects->list[0].kept = false;
    Settle(host, model);
    ok &= Check(Count("effect") == 1, "an effect that was removed has no tile");

    // Dragging the first tile onto the other one runs its effect after that one.
    host.calls.clear();
    model = EffectsModel();
    Settle(host, model);
    const std::optional<ImVec2> first = Find("effect", 0), second = Find("effect", 1);
    ok &= Check(first && second, "both effects that are on have tiles");
    if (first && second)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(first->x, first->y);
        Frame(host, model);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Frame(host, model);
        for (int step = 1; step <= 8; ++step)
        {
            io.AddMousePosEvent(first->x + (second->x - first->x) * step / 8, first->y + (second->y - first->y) * step / 8);
            Frame(host, model);
        }
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Frame(host, model);
        Frame(host, model);
    }
    ok &= Check(host.calls == std::vector<std::string>{ "move 0 2" }, "dropping a tile on another moves its effect there");
    return ok;
}
// Over the game the menu is a tray of the effects of the game being played, and Escape goes back to the game.
bool PanelTests(Host& host)
{
    bool ok = true;
    launcher::Model model = EffectsModel();
    model.games.insert(model.games.begin(), launcher::Game{ .id = "other", .name = "Other", .executable = "other.exe" });
    const auto panel = [&] { Frame(host, model, launcher::Place::Game); };
    // The tray rises into place when it opens.
    const auto settle = [&] { Settle(host, model, launcher::Place::Game); };
    settle();
    ok &= Check(Count("effect") == 2, "the tray has a tile for each effect that is on");
    ok &= Check(Find("power") && Find("back"), "the tray has the menu's buttons");
    if (const std::optional<ImVec2> dot = Find("on"))
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(dot->x, dot->y);
        panel();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        panel();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        panel();
    }
    ok &= Check(host.calls == std::vector<std::string>{ "effect 0 off" }, "a tile's dot turns its effect off");
    model.effects->list[0].on = false;
    model.effects->list[0].kept = true;
    panel();
    ok &= Check(Count("effect") == 2, "an effect turned off keeps its tile");
    launcher::Reset(launcher::Place::Game);
    settle();
    ok &= Check(Count("effect") == 2, "it still has its tile when the menu opens again");
    model.effects->list[0].on = true;
    model.effects->list[0].kept = false;
    settle();

    host.calls.clear();
    PressEscape(host, model, launcher::Place::Game);
    ok &= Check(host.calls == std::vector<std::string>{ "close" }, "Escape goes back to the game");

    // More opens the rest of the launcher over the game, which takes Escape first.
    launcher::Reset(launcher::Place::Game);
    settle();
    host.calls.clear();
    ok &= Check(Click(host, model, "more", 0, launcher::Place::Game), "the tray has More");
    settle();
    ok &= Check(Click(host, model, "Games", 0, launcher::Place::Game), "More opens the games");
    settle();
    ok &= Check(Count("game") == 2, "the games show over the game, as on the window's shelf");
    PressEscape(host, model, launcher::Place::Game);
    settle();
    ok &= Check(Count("game") == 0 && host.calls.empty(), "Escape closes them and leaves the menu open");
    return ok;
}
// Quitting with unsaved changes waits for the answer to whether to save them.
bool QuitTests(Host& host)
{
    bool ok = true;
    launcher::Model model = PlayedGame(true);
    model.quitting = "A";
    Frame(host, model);
    Frame(host, model);
    ok &= Check(Click(host, model, "Save"), "quitting asks about unsaved changes");
    ok &= Check(host.calls == std::vector<std::string>{ "quit saving" }, "Save saves them and quits");
    host.calls.clear();
    Frame(host, model);
    Frame(host, model);
    ok &= Check(Click(host, model, "Cancel"), "the question can be cancelled");
    ok &= Check(host.calls == std::vector<std::string>{ "keep running" }, "Cancel keeps Unishade running");
    model.quitting.clear();
    // It fades away.
    Settle(host, model);
    ok &= Check(!Find("Don't save"), "the question goes once nothing asks it");
    return ok;
}
// An official add-on that is on with a setting, one that can be installed, and a file that waits to be allowed.
std::vector<launcher::Addon> Addons()
{
    using launcher::AddonState;
    launcher::EffectSetting detail{ .control = launcher::EffectSetting::Control::Check, .floating = false, .label = "Detail", .resettable = false };
    return { { .id = "Depth", .name = "Depth", .file = "Depth.dll", .official = true, .state = AddonState::On, .settings = { detail } },
             { .id = "DLSS5", .name = "DLSS5", .file = "DLSS5.dll", .official = true, .state = AddonState::Available },
             { .id = "Mine.dll", .name = "Mine", .file = "Mine.dll", .state = AddonState::Pending } };
}

// Add-ons installs official add-ons, asks about files that are not official, and turns add-ons on and off.
bool AddonsTests(Host& host)
{
    bool ok = true;
    launcher::Model model;
    Frame(host, model);
    ok &= Check(!Find("addons"), "without add-ons there is no Add-ons button");
    model.addons = Addons();
    Frame(host, model);
    ok &= Check(Click(host, model, "addons"), "the window has an Add-ons button");
    Settle(host, model);
    ok &= Check(Click(host, model, "Install"), "an official add-on that is not installed offers Install");
    ok &= Check(Click(host, model, "Load"), "a file that is not official asks whether to load it");
    ok &= Check(Click(host, model, "Don't load"), "it can be left unloaded");
    ok &= Check(Click(host, model, "Off"), "an add-on that is installed can be turned off");
    ok &= Check(host.calls == std::vector<std::string>{ "install 1", "load 2", "leave 2", "addon 0 off" }, "each control acts on its own add-on");

    host.calls.clear();
    (*model.addons)[1].state = launcher::AddonState::Installing;
    (*model.addons)[1].progress = 0.5f;
    Frame(host, model);
    ok &= Check(!Find("Install"), "an add-on that installs cannot be installed again");
    ok &= Check(Click(host, model, "Cancel"), "an install can be cancelled");
    (*model.addons)[1].state = launcher::AddonState::Available;
    (*model.addons)[1].error = "The download does not match its checksum.";
    Frame(host, model);
    ok &= Check(Click(host, model, "Try again"), "a failed install can be tried again");
    (*model.addons)[0].state = launcher::AddonState::Failed;
    (*model.addons)[0].error = "It did not start.";
    Frame(host, model);
    ok &= Check(Click(host, model, "On"), "a failed add-on can be turned on");
    ok &= Check(host.calls == std::vector<std::string>{ "cancel 1", "install 1", "addon 0 on" }, "turning a failed add-on on tries it again");
    return ok;
}

// The settings of the add-ons that are on are in the More of the tray of the game being played, since its preset keeps
// them.
bool AddonSettingsTests(Host& host)
{
    bool ok = true;
    // Tall enough for the whole page.
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    io.DisplaySize.y = 1400;
    // The test before leaves the Add-ons window open, and a click beside it would only close it.
    launcher::Reset(launcher::Place::Window);
    launcher::Model model = EffectsModel();
    model.addons = Addons();
    Settle(host, model);
    ok &= Check(Click(host, model, "more"), "the tray on the game's page has More");
    Settle(host, model);
    ok &= Check(Click(host, model, "Add-ons"), "More has the add-ons that are on");
    Settle(host, model);
    host.calls.clear();
    // Above the page's own choices, which follow the tray.
    ok &= Check(Click(host, model, "On"), "the setting can be changed");
    ok &= Check(host.calls == std::vector<std::string>{ "addon 0 setting 0 1" }, "the change goes to the add-on");
    (*model.addons)[0].state = launcher::AddonState::Off;
    Settle(host, model);
    ok &= Check(!Find("Add-ons"), "an add-on that is off shows no settings");
    model.games[0].running = false;
    (*model.addons)[0].state = launcher::AddonState::On;
    Settle(host, model);
    ok &= Check(!Find("more"), "settings show only while the game is played");
    io.DisplaySize = size;
    return ok;
}

// The window and the menu, with an effect's settings open and each of the launcher's windows, have controls Dear
// ImGui can tell apart.
bool IdTests(Host& host)
{
    bool ok = true;
    launcher::Model model = EffectsModel();
    model.settings.performance = launcher::Performance{};
    model.settings.shortcuts = { { "Open the menu", "", "Home", true }, { "Screenshot", "", "F9" } };
    model.games[0].performance = launcher::Performance{};
    model.games.push_back(launcher::Game{ .id = "other", .name = "Other", .executable = "other.exe" });
    model.addons = Addons();
    model.discord = launcher::Discord{ .on = true, .status = "Showing Game on Discord." };
    model.cards = { { .title = "Unishade 9.9.9 is available", .text = "Download the new Setup and run it.", .attention = true, .buttons = { "Download" } } };
    model.notices = { { launcher::Level::Warning, "See https://unishade.me/docs/ for help." } };
    model.links = { "Open log", "Docs" };
    using Control = launcher::EffectSetting::Control;
    host.settings = { .tooltip = "Does things.",
                      .settings = { { .control = Control::Slider, .label = "Amount", .category = "Look", .range = true, .max = 1 },
                                    { .control = Control::Check, .floating = false, .label = "On", .category = "Look" },
                                    { .control = Control::Drag, .count = 2, .label = "Focus" } },
                      .definitions = { { "QUALITY", "", "1" } } };
    for (const launcher::Place place : { launcher::Place::Window, launcher::Place::Game })
    {
        launcher::Reset(place);
        Settle(host, model, place);
        const auto check = [&](const char* page) {
            if (const std::optional<std::string> shared = SharedId())
            {
                std::printf("Failed: %s shares its ID on %s %s\n", shared->c_str(), page, place == launcher::Place::Game ? "over the game" : "in the window");
                ok = false;
            }
        };
        const auto press = [&](const char* label) {
            Click(host, model, label, 0, place);
            Settle(host, model, place);
        };
        const auto escape = [&] {
            PressEscape(host, model, place);
            Settle(host, model, place);
        };
        press("effect");
        check("the game's page");
        if (place == launcher::Place::Window)
        {
            press("settings");
            check("Settings");
            for (const char* page : { "Performance", "Shortcuts", "Discord", "About" })
            {
                press(page);
                check(page);
            }
            escape();
            press("addons");
            check("Add-ons");
            escape();
            press("messages");
            check("Messages");
            escape();
            Click(host, model, "game", 1, place);
            Settle(host, model, place);
            check("the page of a game that is not played");
        }
        else
        {
            press("more");
            press("Menu");
            check("More");
            // Add-ons last, since the tray's own tab of that name comes first.
            for (const char* window : { "Games", "Settings", "Messages", "Add-ons" })
            {
                press(window);
                check(window);
                escape();
            }
        }
    }
    return ok;
}
} // namespace

int main()
{
    ImGuiContext* context = ImGui::CreateContext();
    context->TestEngineHookItems = true;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1000, 750);
    io.DeltaTime = 1.0f / 60;
    // The launcher measures capitals at 100 pixels. Bake that size without a renderer for dynamic fonts.
    ImFontConfig font;
    font.SizePixels = 100;
    io.Fonts->AddFontDefault(&font);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    bool ok = true;
    {
        Host host;
        ok &= PickerTests(host);
    }
    {
        Host host;
        ok &= StatusTests(host);
    }
    {
        Host host;
        ok &= UnsavedTests(host);
    }
    {
        Host host;
        ok &= EffectsTests(host);
    }
    {
        Host host;
        ok &= PanelTests(host);
    }
    {
        Host host;
        ok &= QuitTests(host);
    }
    {
        Host host;
        ok &= AddonsTests(host);
    }
    {
        Host host;
        ok &= AddonSettingsTests(host);
    }
    {
        Host host;
        ok &= IdTests(host);
    }

    kit::ForgetContext(context);
    ImGui::DestroyContext(context);
    return ok ? 0 : 1;
}
