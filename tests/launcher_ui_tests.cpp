#include "../src/ui/kit.h"

#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kit
{
ImTextureID Picture(const std::string&, float) { return ImTextureID_Invalid; }
ImU32 PictureTint(const std::string&) { return 0; }
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
    void RecordShortcut(int) override {}
    void ClearShortcut(size_t) override {}
    void ResetShortcuts() override {}
};

void Frame(Host& host, const launcher::Model& model)
{
    items.clear();
    ImGui::NewFrame();
    launcher::Draw(model, host, 1);
    ImGui::Render();
}

void Frame(Host& host)
{
    launcher::Model model;
    if (host.picking)
        model.picker = launcher::Picker{ host.add, { { "Game window", "game.exe", "" } } };
    Frame(host, model);
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
bool Click(Host& host, const launcher::Model& model, const char* label, size_t nth = 0)
{
    const std::optional<ImVec2> at = Find(label, nth);
    if (!at)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at->x, at->y);
    Frame(host, model);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Frame(host, model);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Frame(host, model);
    Frame(host, model);
    return true;
}

void OpenPicker(Host& host, bool add)
{
    host.OpenPicker(add);
    host.chosen.reset();
    for (int i = 0; i < 3; ++i)
        Frame(host);
}

std::optional<ImVec2> WindowRow()
{
    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
        if ((window->Flags & ImGuiWindowFlags_ChildWindow) && window->ParentWindow &&
            std::strcmp(window->ParentWindow->Name, "##picker") == 0)
            return ImVec2(window->DC.CursorStartPos.x + 20, window->DC.CursorStartPos.y + 26);
    return std::nullopt;
}

bool PickerTests(Host& host)
{
    ImGuiIO& io = ImGui::GetIO();
    bool ok = true;
    Frame(host);
    for (bool add : { true, false })
    {
        OpenPicker(host, add);
        const auto row = WindowRow();
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
    Frame(host, model);
    ok &= Check(Click(host, model, "game"), "the sidebar lists the game");
    ok &= Check(Click(host, model, "preset", 1), "the game's page lists its presets");
    ok &= Check(host.calls == std::vector<std::string>{ "use 0 b.ini" }, "a preset without unsaved changes switches at once");

    host.calls.clear();
    model = PlayedGame(true);
    Frame(host, model);
    Click(host, model, "preset", 1);
    ok &= Check(host.calls.empty(), "unsaved changes hold the switch");
    ok &= Check(Click(host, model, "Save"), "the switch asks what to do with the changes");
    ok &= Check(host.calls == std::vector<std::string>{ "save", "use 0 b.ini" }, "saving the changes then switches");

    host.calls.clear();
    Click(host, model, "preset", 1);
    // The card offers Discard too, before the dialog does.
    ok &= Check(Click(host, model, "Discard", 1), "the question offers to discard the changes");
    ok &= Check(host.calls == std::vector<std::string>{ "discard", "use 0 b.ini" }, "discarding the changes then switches");

    host.calls.clear();
    Click(host, model, "preset", 1);
    ok &= Check(Click(host, model, "Cancel"), "the question can be cancelled");
    ok &= Check(host.calls.empty(), "cancelling keeps the preset");
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
        ok &= UnsavedTests(host);
    }

    kit::ForgetContext(context);
    ImGui::DestroyContext(context);
    return ok ? 0 : 1;
}
