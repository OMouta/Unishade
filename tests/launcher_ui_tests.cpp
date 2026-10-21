#include "../src/ui/kit.h"

#include <imgui_internal.h>

#include <cstdio>
#include <cstring>

namespace kit
{
ImTextureID Picture(const std::string&, float) { return ImTextureID_Invalid; }
ImU32 PictureTint(const std::string&) { return 0; }
} // namespace kit

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

    void OpenPicker(bool adding) override { picking = true; add = adding; }
    void ChooseWindow(size_t window) override { chosen = window; picking = false; }
    void ClosePicker() override { ++closed; picking = false; }

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
    void UsePreset(size_t, std::string_view) override {}
    void OpenPresetsFolder(size_t) override {}
    void SetPerformance(std::optional<size_t>, launcher::PerformanceSetting, int) override {}
    void StepMenuSize(int) override {}
    void RecordShortcut(int) override {}
    void ClearShortcut(size_t) override {}
    void ResetShortcuts() override {}
};

void Frame(Host& host)
{
    launcher::Model model;
    if (host.picking)
        model.picker = launcher::Picker{ host.add, { { "Game window", "game.exe", "" } } };
    ImGui::NewFrame();
    launcher::Draw(model, host, 1);
    ImGui::Render();
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
} // namespace

int main()
{
    ImGuiContext* context = ImGui::CreateContext();
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

    Host host;
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

    kit::ForgetContext(context);
    ImGui::DestroyContext(context);
    return ok ? 0 : 1;
}
