// The launcher and the menu. The launcher is the one every platform shares, in src/ui. The menu is drawn the way the
// Windows host draws it in src/menu.cpp, with the same layout, sizes and colors.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"
#include "log.h"
#include "menu_layout.h"
#include "theme.h"
#include "ui/kit.h"
#include "ui/launcher_ui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <utility>

// assets/Unishade.png, which the build puts into the program.
extern const unsigned char kLogoPng[];
extern const unsigned kLogoPngSize;

namespace
{
using namespace kit;

// The menu's layout in pixels at 1080p.
using menu_layout::kWidth;
using menu_layout::kMargin;
using menu_layout::kHeader;
using menu_layout::kTabs;
using menu_layout::kFooter;
constexpr float kPadding = 18;

// How long a removed game can be put back.
constexpr double kUndoSeconds = 8;
// The presets folder is read this often while the launcher shows.
constexpr double kPresetsInterval = 2;

// Help is given on the Unishade Discord server.
constexpr char kHelpUrl[] = "https://discord.gg/wVbVUdENas";

// A picture drawn in a window, made at the size it is drawn. Each takes a set from Dear ImGui's descriptor pool,
// which also holds its fonts, so a window has at most kMaxPictures.
constexpr size_t kMaxPictures = 192;
constexpr uint32_t kDescriptorPoolSize = kMaxPictures + 64;
constexpr char kLogoPicture[] = "logo";

struct GpuPicture
{
    GpuImage image;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool used = false; // drawn since ForgetPictures last looked
    ImU32 tint = 0;    // the average color of the pixels that show
};

// The logo, and folders' logos, of each window's Dear ImGui context, by folder and size in pixels: the launcher draws
// a game's icon at more than one size. A folder without a logo keeps an empty entry until it is looked for again.
std::map<ImGuiContext*, std::map<std::pair<std::string, int>, GpuPicture>> pictureSets;

std::map<std::pair<std::string, int>, GpuPicture>& Pictures()
{
    return pictureSets[ImGui::GetCurrentContext()];
}

launcher::Level LevelOf(LogLevel level)
{
    static_assert(static_cast<int>(LogLevel::Ok) == static_cast<int>(launcher::Level::Ok) &&
                  static_cast<int>(LogLevel::Error) == static_cast<int>(launcher::Level::Error));
    return static_cast<launcher::Level>(level);
}

// Pictures

void DestroyPicture(GpuPicture& picture)
{
    if (picture.set)
        ImGui_ImplVulkan_RemoveTexture(picture.set);
    gpu.DestroyImage(picture.image);
    picture = {};
}

// Averages the pixels each pixel of the result covers, weighted by alpha so transparent edges stay clean.
std::vector<uint8_t> Shrink(const uint8_t* rgba, int width, int height, int size)
{
    std::vector<uint8_t> result(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const int x0 = x * width / size, x1 = std::max(x0 + 1, (x + 1) * width / size);
            const int y0 = y * height / size, y1 = std::max(y0 + 1, (y + 1) * height / size);
            uint64_t color[3] = {}, alpha = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                {
                    const uint8_t* pixel = rgba + (size_t(sy) * width + sx) * 4;
                    for (int c = 0; c < 3; ++c)
                        color[c] += uint64_t(pixel[c]) * pixel[3];
                    alpha += pixel[3];
                }
            uint8_t* out = &result[(size_t(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c)
                out[c] = alpha ? uint8_t(color[c] / alpha) : 0;
            out[3] = uint8_t(alpha / (uint64_t(x1 - x0) * uint64_t(y1 - y0)));
        }
    return result;
}

// The logo, or a folder's logo.png, at a size in the window's own units. Invalid when the folder has none.
ImTextureID PictureTexture(const std::string& key, float drawn)
{
    const int size = static_cast<int>(std::round(drawn * ImGui::GetIO().DisplayFramebufferScale.x));
    auto& pictures = Pictures();
    const auto [entry, added] = pictures.try_emplace({ key, size });
    GpuPicture& picture = entry->second;
    picture.used = true;
    if (!added || size <= 0)
        return (ImTextureID)picture.set;
    if (std::count_if(pictures.begin(), pictures.end(), [](const auto& other) { return other.second.set != VK_NULL_HANDLE; }) >=
        std::ptrdiff_t(kMaxPictures))
        return ImTextureID_Invalid;
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = key == kLogoPicture ? stbi_load_from_memory(kLogoPng, static_cast<int>(kLogoPngSize), &width, &height, &channels, 4)
                                          : stbi_load((fs::path(key) / "logo.png").c_str(), &width, &height, &channels, 4);
    if (!pixels)
        return ImTextureID_Invalid;
    const std::vector<uint8_t> rgba = Shrink(pixels, width, height, size);
    stbi_image_free(pixels);
    uint64_t sums[3] = {}, weight = 0;
    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        for (int c = 0; c < 3; ++c)
            sums[c] += uint64_t(rgba[i + c]) * rgba[i + 3];
        weight += rgba[i + 3];
    }
    if (weight)
        picture.tint = IM_COL32(sums[0] / weight, sums[1] / weight, sums[2] / weight, 255);
    GpuBuffer upload;
    if (gpu.CreateImage(picture.image, size, size, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        gpu.CreateBuffer(upload, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
    {
        std::memcpy(upload.mapped, rgba.data(), rgba.size());
        if (VkCommandBuffer commands = gpu.BeginCommands())
        {
            InitLayout(commands, picture.image);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { uint32_t(size), uint32_t(size), 1 };
            vkCmdCopyBufferToImage(commands, upload.buffer, picture.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            FullBarrier(commands);
            if (gpu.SubmitAndWait(commands))
                picture.set = ImGui_ImplVulkan_AddTexture(picture.image.view, VK_IMAGE_LAYOUT_GENERAL);
            else
                vkDeviceWaitIdle(gpu.device); // commands that failed to finish may still use the image and buffer
        }
    }
    gpu.DestroyBuffer(upload);
    if (!picture.set)
        gpu.DestroyImage(picture.image);
    return (ImTextureID)picture.set;
}

// Drops the pictures not drawn since the last call, the folder logos that are not kept, and the folders found without
// one, so a logo saved since, such as the icon of a game that just started, shows the next time it is drawn.
void ForgetPictures(const std::function<bool(const std::string& folder)>& keep)
{
    auto& pictures = Pictures();
    bool waited = false;
    for (auto entry = pictures.begin(); entry != pictures.end();)
    {
        const std::string& folder = entry->first.first;
        if (std::exchange(entry->second.used, false) && entry->second.set && (folder == kLogoPicture || keep(folder)))
        {
            ++entry;
            continue;
        }
        if (entry->second.set && !std::exchange(waited, true))
            vkDeviceWaitIdle(gpu.device);
        DestroyPicture(entry->second);
        entry = pictures.erase(entry);
    }
}

// A floppy disk: an outline with one corner cut, the shutter at the top and the label below.
void SaveIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 min = center - ImVec2(size, size) * 0.5f;
    const ImVec2 max = center + ImVec2(size, size) * 0.5f;
    const float corner = size * 0.25f;
    const ImVec2 outline[] = { min, ImVec2(max.x - corner, min.y), ImVec2(max.x, min.y + corner), max, ImVec2(min.x, max.y) };
    draw->AddPolyline(outline, 5, color, ImDrawFlags_Closed, S(1.5f));
    draw->AddRectFilled(ImVec2(min.x + size * 0.25f, min.y), ImVec2(max.x - size * 0.35f, min.y + size * 0.3f), color);
    draw->AddRect(ImVec2(min.x + size * 0.2f, center.y + size * 0.1f), ImVec2(max.x - size * 0.2f, max.y), color, 0, 0, S(1.5f));
}

// A camera: the body, the bump on top and the lens.
void CameraIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 min = center + ImVec2(-size * 0.5f, -size * 0.3f);
    const ImVec2 max = center + ImVec2(size * 0.5f, size * 0.4f);
    draw->AddRect(min, max, color, S(2), 0, S(1.5f));
    draw->AddRectFilled(ImVec2(center.x - size * 0.2f, min.y - size * 0.15f), ImVec2(center.x + size * 0.2f, min.y), color, S(1));
    draw->AddCircle(ImVec2(center.x, center.y + size * 0.05f), size * 0.2f, color, 0, S(1.5f));
}

// Menu

enum class Tab
{
    Presets,
    Effects,
    Settings,
    Status,
};

// The parts of the Settings tab, in the order their buttons show.
enum class SettingsPage
{
    General,
    Shortcuts,
};

enum class NameAction
{
    New,
    Duplicate,
    SaveAsNew,
    NewFolder,
};

struct MenuState
{
    Tab tab = Tab::Presets;
    SettingsPage settingsPage = SettingsPage::General;
    char search[128] = {};
    char presetSearch[128] = {};
    // The effect whose settings show, as presets name it.
    std::string expanded;
    bool showAll = false;
    // The effects listed under Active since the menu opened or the preset changed, as presets name them.
    std::set<std::string> active;
    fs::path activePreset;
    std::vector<PresetFolder> folders;
    double presetsListed = -10;
    fs::path pendingPreset; // waiting for an answer about unsaved changes
    bool openUnsavedPopup = false;
    bool openNamePopup = false;
    NameAction nameAction = NameAction::New;
    fs::path nameTarget;
    char name[128] = {};
    std::string nameError;
    bool openConfirmPopup = false;
    std::string confirmEffect; // the file whose settings Reset all asks about
    int recording = -1;        // the shortcut being recorded
    std::string shortcutError;
    std::vector<Notice> notices; // read once a frame
    std::string shownErrors;     // the effect file whose compiler errors show
};
MenuState menu;

// Settings lists the shortcuts in the order of kShortcuts.
constexpr const char* kShortcutDescriptions[] = {
    "Press it again, or Escape, to go back to the game.",
    "Shows the game without effects and stops capturing it.",
    "Shows the game without effects for as long as you hold it.",
    "Saves what you see, without the menu.",
    "Saves the same moment with and without effects.",
    "Switches to the next preset in the Presets tab.",
    "Switches to the preset before it.",
};
static_assert(std::size(kShortcutDescriptions) == std::size(kShortcuts));

void StopRecording(App& app)
{
    if (menu.recording >= 0)
        app.SuspendHotkeys(false);
    menu.recording = -1;
}

// Presets

void OpenNamePopup(NameAction action, const fs::path& target)
{
    menu.nameAction = action;
    menu.nameTarget = target;
    menu.nameError.clear();
    const std::string name = action == NameAction::New         ? "New preset"
                             : action == NameAction::NewFolder ? "New folder"
                                                               : target.stem().string() + " copy";
    snprintf(menu.name, sizeof(menu.name), "%s", name.c_str());
    menu.openNamePopup = true;
}

// Switches presets, or asks first when there are unsaved changes.
void SwitchTo(App& app, const fs::path& preset)
{
    if (app.SwitchPreset(preset, app.settings.autoSavePresets, false))
        return;
    menu.pendingPreset = preset;
    menu.openUnsavedPopup = true;
}

bool ApplyName(App& app)
{
    std::string name = menu.name;
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    menu.nameError.clear();
    const bool current = menu.nameTarget == app.runtime.PresetPath();
    if (menu.nameAction == NameAction::New)
        return app.NewPreset(name, false, menu.nameError);
    // The active preset is copied as it is on screen, unsaved changes included.
    if (menu.nameAction == NameAction::SaveAsNew || (menu.nameAction == NameAction::Duplicate && current))
        return app.NewPreset(name, true, menu.nameError);
    menu.nameError = PresetNameProblem(name);
    if (!menu.nameError.empty())
        return false;
    if (menu.nameAction == NameAction::NewFolder)
        return app.MovePreset(menu.nameTarget, PresetsDirectory() / name, menu.nameError);

    // A copy stays in the folder of the preset it comes from.
    const fs::path path = menu.nameTarget.parent_path() / (name + ".ini");
    std::error_code error;
    if (fs::exists(path, error))
    {
        menu.nameError = "A preset with that name already exists.";
        return false;
    }
    if (!fs::copy_file(menu.nameTarget, path, error))
    {
        menu.nameError = "Could not write the preset.";
        return false;
    }
    SwitchTo(app, path);
    return true;
}

void NameDialog(App& app)
{
    if (!BeginDialog("##name", menu.openNamePopup, 380))
        return;
    const char* titles[] = { "New preset", "Duplicate preset", "Save as new preset", "Move to a new folder" };
    const char* actions[] = { "Create", "Duplicate", "Save", "Move" };
    const int action = static_cast<int>(menu.nameAction);
    DialogText(titles[action], menu.nameAction == NameAction::New ? "Starts with every effect off." : "");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputText("##value", menu.name, sizeof(menu.name), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!menu.nameError.empty())
        Text(menu.nameError, kError, 13.5f);
    const int clicked = DialogButtons({ "Cancel", actions[action] });
    if ((clicked == 1 || enter) && ApplyName(app))
    {
        menu.presetsListed = -10;
        ImGui::CloseCurrentPopup();
    }
    if (clicked == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Asks what to do with unsaved changes when switching presets. The switch waits for the answer.
void UnsavedDialog(App& app)
{
    if (!BeginDialog("##unsaved", menu.openUnsavedPopup, 420))
        return;
    DialogText("Save your changes to " + app.runtime.PresetPath().stem().string() + "?", "Discarding goes back to how the preset was last saved.");
    const int clicked = DialogButtons({ "Discard", "Cancel", "Save" });
    if (clicked == 0)
        app.SwitchPreset(menu.pendingPreset, false, true);
    else if (clicked == 2)
        app.SwitchPreset(menu.pendingPreset, true, false);
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        menu.pendingPreset.clear();
        ImGui::CloseCurrentPopup();
    }
    EndDialog();
}

// The folders a preset can move to: the ones listed, then saved games that have no presets yet.
void MoveMenu(App& app, const fs::path& preset)
{
    std::vector<std::pair<std::string, fs::path>> targets;
    for (const PresetFolder& folder : menu.folders)
        targets.emplace_back(folder.name, folder.path);
    for (const AutoGame& game : app.autoGames)
    {
        const std::string name = FolderName(game.name);
        if (!name.empty() && std::none_of(targets.begin(), targets.end(), [&](const auto& target) {
                return !strcasecmp(target.second.filename().c_str(), name.c_str());
            }))
            targets.emplace_back(name, PresetsDirectory() / name);
    }
    for (const auto& [name, folder] : targets)
        if (folder != preset.parent_path() && ImGui::MenuItem(name.c_str()))
        {
            if (std::string error; !app.MovePreset(preset, folder, error))
                app.ShowToast(error);
            menu.presetsListed = -10;
        }
    ImGui::Separator();
    if (ImGui::MenuItem("New folder..."))
        OpenNamePopup(NameAction::NewFolder, preset);
}

// The preset's menu, opened from the three dots on its row.
void PresetActions(App& app, const fs::path& path, bool active)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("actions"))
    {
        if (ImGui::MenuItem("Duplicate"))
            OpenNamePopup(NameAction::Duplicate, path);
        if (ImGui::BeginMenu("Move to", !active))
        {
            MoveMenu(app, path);
            ImGui::EndMenu();
        }
        if (active)
        {
            ImGui::Separator();
            PushSize(12.5f);
            ImGui::TextDisabled("Switch to another preset to\nmove this one.");
            ImGui::PopFont();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void PresetRow(App& app, const fs::path& path, bool active)
{
    const std::string name = path.stem().string();
    ImGui::PushID(name.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(44));
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav) && !active;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));
    draw->AddRect(start, start + size, active ? kAccent : kBorder, S(8), 0, active ? S(1.5f) : 1.0f);

    // The menu button, drawn as three dots.
    const ImVec2 dots = start + ImVec2(size.x - S(38), (size.y - S(28)) / 2);
    ImGui::SetCursorScreenPos(dots);
    if (ImGui::InvisibleButton("actions", ImVec2(S(28), S(28)), ImGuiButtonFlags_EnableNav))
        ImGui::OpenPopup("actions");
    const bool dotsHovered = ImGui::IsItemHovered();
    HandOnHover();
    if (dotsHovered)
        draw->AddRectFilled(dots, dots + ImVec2(S(28), S(28)), kBorder, S(6));
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(dots + ImVec2(S(14) + i * S(5), S(14)), S(1.6f), hovered || dotsHovered ? kText : kDim);

    PushSize(14.5f);
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(dots.x - S(70), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(14), textY), kText, name.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    if (active)
    {
        PushSize(11.5f);
        const char* tag = "ACTIVE";
        const ImVec2 tagText = ImGui::CalcTextSize(tag);
        const ImVec2 tagSize = tagText + ImVec2(S(14), S(6));
        const ImVec2 tagStart(dots.x - S(8) - tagSize.x, start.y + (size.y - tagSize.y) / 2);
        draw->AddRectFilled(tagStart, tagStart + tagSize, Color(theme::kAccent, 50), tagSize.y / 2);
        draw->AddText(tagStart + (tagSize - tagText) * 0.5f, kAccentHover, tag);
        ImGui::PopFont();
    }

    PresetActions(app, path, active);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    ImGui::PopID();
    if (clicked)
        SwitchTo(app, path);
}

// The folder's logo.png, such as a game's icon, or a stand-in: a globe for all games, a game's first letter, or a
// folder.
void FolderIcon(ImDrawList* draw, const PresetFolder& folder, ImVec2 min, float size)
{
    const ImVec2 max = min + ImVec2(size, size);
    if (const ImTextureID logo = PictureTexture(folder.path.string(), size))
    {
        draw->AddImageRounded(ImTextureRef(logo), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
        return;
    }
    const ImVec2 center = (min + max) * 0.5f;
    const float t = S(1.5f);
    if (folder.all)
    {
        const float r = size * 0.4f;
        draw->AddCircle(center, r, kDim, 0, t);
        draw->AddEllipse(center, ImVec2(r * 0.45f, r), kDim, 0, 0, t);
        draw->AddLine(center - ImVec2(r, 0), center + ImVec2(r, 0), kDim, t);
    }
    else if (folder.game)
    {
        draw->AddRectFilled(min, max, kBorder, S(5));
        const std::string letter = Initial(folder.name);
        PushSize(13);
        draw->AddText(center - ImGui::CalcTextSize(letter.c_str()) * 0.5f, kText, letter.c_str());
        ImGui::PopFont();
    }
    else
    {
        const ImVec2 body(min.x + size * 0.1f, min.y + size * 0.32f);
        draw->AddRectFilled(ImVec2(body.x, min.y + size * 0.2f), ImVec2(body.x + size * 0.35f, body.y + t), kDim, S(1.5f));
        draw->AddRect(body, ImVec2(max.x - size * 0.1f, max.y - size * 0.18f), kDim, S(2), 0, t);
    }
}

// A folder's logo, name and preset count. Clicking it opens or closes the folder.
bool FolderHeader(const PresetFolder& folder, bool open, size_t presets)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(34));
    const bool clicked = ImGui::InvisibleButton("folder", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(start, start + size, kCard, S(8));

    const float icon = std::round(S(22));
    const ImVec2 iconStart(std::round(start.x + S(6)), std::round(start.y + (size.y - icon) / 2));
    FolderIcon(draw, folder, iconStart, icon);

    const float chevron = start.x + size.x - S(20);
    PushSize(13);
    const std::string count = std::to_string(presets);
    const ImVec2 countSize = ImGui::CalcTextSize(count.c_str());
    const float countX = chevron - S(16) - countSize.x;
    draw->AddText(ImVec2(countX, start.y + (size.y - countSize.y) / 2), kDim, count.c_str());
    ImGui::PopFont();
    PushSize(14.5f);
    draw->PushClipRect(start, ImVec2(countX - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(iconStart.x + icon + S(10), start.y + (size.y - ImGui::GetFontSize()) / 2), kText, folder.name.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    Chevron(draw, ImVec2(chevron, start.y + size.y / 2), open, open || hovered ? kText : kDim);
    return clicked;
}

// Returns false when searching found nothing in the folder, which is then left out.
bool FolderSection(App& app, const PresetFolder& folder, const fs::path& current, const std::string& filter)
{
    // While searching, folders are open and show the presets that match, or all of them when the folder's name does.
    const bool searching = !filter.empty();
    const bool folderMatches = searching && Lowercase(folder.name).find(filter) != std::string::npos;
    std::vector<const fs::path*> shown;
    for (const fs::path& preset : folder.presets)
        if (!searching || folderMatches || Lowercase(preset.stem().string()).find(filter) != std::string::npos)
            shown.push_back(&preset);
    if (searching && shown.empty())
        return false;

    ImGui::PushID(folder.path.c_str());
    const bool open = searching || app.FolderOpen(folder);
    if (FolderHeader(folder, open, shown.size()) && !searching)
        app.folderOpen[folder.path.string()] = !open;
    if (open)
    {
        for (const fs::path* preset : shown)
            PresetRow(app, *preset, *preset == current);
        if (folder.presets.empty() && (folder.playing || (folder.all && app.game.empty())))
            Text("New presets go here.", kDim, 13.5f);
    }
    ImGui::PopID();
    return true;
}

void PresetsTab(App& app)
{
    const fs::path current = app.runtime.PresetPath();
    if (glfwGetTime() - menu.presetsListed > 1)
    {
        menu.folders = app.PresetFolders();
        menu.presetsListed = glfwGetTime();
        ForgetPictures([](const std::string& path) {
            return std::any_of(menu.folders.begin(), menu.folders.end(), [&](const PresetFolder& folder) { return folder.path.string() == path; });
        });
    }

    if (Button("New preset", ImVec2(S(130), S(32)), true))
        OpenNamePopup(NameAction::New, {});
    if (!app.settings.autoSavePresets)
    {
        ImGui::SameLine(0, S(8));
        if (Button("Save as new", ImVec2(S(120), S(32))))
            OpenNamePopup(NameAction::SaveAsNew, current);
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##presetsearch", "Search presets", menu.presetSearch, sizeof(menu.presetSearch));

    const std::string filter = Lowercase(menu.presetSearch);
    bool any = false;
    for (const PresetFolder& folder : menu.folders)
        if (FolderSection(app, folder, current, filter))
            any = true;
    if (!any && !filter.empty())
        Text("No presets match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    Text(app.settings.autoSavePresets ? "Changes save to the active preset as you make them."
                                      : "Changes apply right away. Save them with the icon at the top.",
         kDim, 13);
    PushSize(13.5f);
    if (Link("Open presets folder"))
        platform::Open(PresetsDirectory().string());
    if (!app.settings.autoSavePresets && app.runtime.Dirty())
    {
        ImGui::SameLine(0, S(16));
        if (Link("Discard changes", kDim))
            app.SwitchPreset(current, false, true);
    }
    ImGui::PopFont();
}

// Effects

// Draws the control for one variable of an effect. Returns true when its value changed.
bool DrawParameter(App& app, fx::Effect& effect, const fx::Uniform& uniform)
{
    const reshadefx::type& type = uniform.type;
    const int components = static_cast<int>(type.components());
    if (!uniform.text.empty())
        Text(uniform.text, kDim, 13.5f);
    if (type.is_array() || type.is_matrix() || components > 4)
        return false;

    bool changed = false;
    ImGui::PushID(uniform.name.c_str());
    const float x = ImGui::GetCursorPosX();
    const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
    // One group for the label and the control, so effects can tell which of their variables is in use or under the
    // cursor.
    ImGui::BeginGroup();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x + labelWidth - S(10));
    ImGui::PushStyleColor(ImGuiCol_Text, uniform.tooltip.empty() ? kText : Color(0xD8D9E6));
    ImGui::TextUnformatted(uniform.label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    Tooltip(uniform.tooltip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x + labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    const bool bounded = uniform.min > std::numeric_limits<float>::lowest() && uniform.max < std::numeric_limits<float>::max();
    if (type.is_boolean())
    {
        int value = 0;
        app.runtime.GetValue(effect, uniform, &value, 1);
        bool on = value != 0;
        if ((changed = ImGui::Checkbox("##value", &on)))
        {
            value = on;
            app.runtime.SetValue(effect, uniform, &value, 1);
        }
    }
    else if (!uniform.items.empty() && !type.is_floating_point() && components == 1)
    {
        int value = 0;
        app.runtime.GetValue(effect, uniform, &value, 1);
        if ((changed = ImGui::Combo("##value", &value, uniform.items.c_str())))
            app.runtime.SetValue(effect, uniform, &value, 1);
    }
    else if (uniform.uiType == "color" && type.is_floating_point() && components >= 3)
    {
        float value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        changed = components == 3 ? ImGui::ColorEdit3("##value", value) : ImGui::ColorEdit4("##value", value, ImGuiColorEditFlags_AlphaBar);
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    else if (type.is_floating_point())
    {
        float value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        if (uniform.uiType == "slider" && bounded)
            changed = ImGui::SliderScalarN("##value", ImGuiDataType_Float, value, components, &uniform.min, &uniform.max, "%.3f");
        else if (uniform.uiType == "input")
            changed = ImGui::InputScalarN("##value", ImGuiDataType_Float, value, components);
        else
            changed = ImGui::DragScalarN("##value", ImGuiDataType_Float, value, components, std::max(uniform.step, 0.0001f),
                                         bounded ? &uniform.min : nullptr, bounded ? &uniform.max : nullptr, "%.3f");
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    else
    {
        int value[4] = {};
        app.runtime.GetValue(effect, uniform, value, components);
        const int low = bounded ? static_cast<int>(uniform.min) : INT_MIN, high = bounded ? static_cast<int>(uniform.max) : INT_MAX;
        if (uniform.uiType == "slider" && bounded)
            changed = ImGui::SliderScalarN("##value", ImGuiDataType_S32, value, components, &low, &high);
        else if (uniform.uiType == "input")
            changed = ImGui::InputScalarN("##value", ImGuiDataType_S32, value, components);
        else
            changed = ImGui::DragScalarN("##value", ImGuiDataType_S32, value, components, std::max(uniform.step, 0.2f), bounded ? &low : nullptr,
                                         bounded ? &high : nullptr);
        if (changed)
            app.runtime.SetValue(effect, uniform, value, components);
    }
    if (ImGui::BeginPopupContextItem("reset"))
    {
        if (ImGui::MenuItem("Reset to default"))
        {
            app.runtime.ResetValue(effect, uniform);
            changed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    if (ImGui::IsItemActive())
        app.menuActiveUniform = &uniform;
    if (ImGui::IsItemHovered())
        app.menuHoveredUniform = &uniform;
    ImGui::PopID();
    return changed;
}

// The preprocessor definitions the effect checks, such as quality levels. A change compiles the effects again, so it
// applies on Enter, as in ReShade's menu.
void DrawDefinitions(App& app, const fx::Effect& effect)
{
    if (effect.definitions.empty() || !ImGui::CollapsingHeader("Preprocessor definitions"))
        return;
    for (const auto& [name, compiled] : effect.definitions)
    {
        ImGui::PushID(name.c_str());
        const float x = ImGui::GetCursorPosX();
        const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(x + labelWidth - S(10));
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        ImGui::SetCursorPosX(x + labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Empty while the effect uses its own value, which shows as the hint.
        const std::string current = app.runtime.DefinitionValue(effect.file, name);
        char value[256]{};
        current.copy(value, sizeof(value) - 1);
        if (ImGui::InputTextWithHint("##value", compiled.c_str(), value, sizeof(value), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) &&
            current != value)
            app.runtime.SetDefinition(effect.file, name, value);
        if (ImGui::BeginPopupContextItem("reset"))
        {
            if (ImGui::MenuItem("Reset to default"))
                app.runtime.SetDefinition(effect.file, name, "");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

void DrawParameters(App& app, const fx::Technique& technique, fx::Effect& effect)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("settings", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    PushSize(13.5f);
    if (!technique.tooltip.empty())
        Text(technique.tooltip, kDim, 13.5f);

    // Grouped by category in the order they first appear, as in ReShade.
    std::vector<std::string> categories;
    for (const fx::Uniform& uniform : effect.uniforms)
        if (!uniform.hidden && std::find(categories.begin(), categories.end(), uniform.category) == categories.end())
            categories.push_back(uniform.category);
    if (categories.empty() && effect.definitions.empty())
        Text("This effect has no settings.", kDim, 13.5f);
    bool changed = false;
    for (const std::string& category : categories)
    {
        if (!category.empty() && !ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        for (const fx::Uniform& uniform : effect.uniforms)
            if (!uniform.hidden && uniform.category == category)
                changed |= DrawParameter(app, effect, uniform);
    }
    if (changed)
        app.runtime.SetDirty();
    if (!categories.empty())
    {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (Link("Reset all", kDim))
        {
            menu.confirmEffect = effect.file;
            menu.openConfirmPopup = true;
        }
        Tooltip("Right-click a setting to reset only that one.");
    }
    DrawDefinitions(app, effect);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Asks before resetting an effect's settings, which cannot be got back with auto-save on.
void ConfirmDialog(App& app)
{
    if (!BeginDialog("##confirm", menu.openConfirmPopup, 380))
        return;
    DialogText("Reset every setting of " + menu.confirmEffect + "?",
               app.settings.autoSavePresets ? "They go back to their defaults and the preset is saved." : "They go back to their defaults.");
    const int clicked = DialogButtons({ "Cancel", "Reset" });
    if (clicked == 1)
        for (fx::Effect& effect : app.runtime.Effects())
            if (effect.file == menu.confirmEffect)
            {
                for (const fx::Uniform& uniform : effect.uniforms)
                    if (uniform.source.empty())
                        app.runtime.ResetValue(effect, uniform);
                app.runtime.SetDirty();
            }
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Effects that are on can be dragged onto each other to change the order they run in, which from and to take.
void TechniqueRow(App& app, int index, int& moveFrom, int& moveTo)
{
    fx::Technique& technique = app.runtime.Techniques()[index];
    fx::Effect& effect = app.runtime.Effects()[technique.effect];
    const std::string key = fx::TechniqueKey(technique, effect);
    const std::string& label = technique.label.empty() ? technique.name : technique.label;
    const bool expanded = menu.expanded == key;
    ImGui::PushID(key.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(38));
    if (!expanded && !ImGui::IsRectVisible(size))
    {
        ImGui::Dummy(size);
        ImGui::PopID();
        return;
    }

    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("row", size, ImGuiButtonFlags_EnableNav))
        menu.expanded = expanded ? std::string() : key;
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    if (technique.enabled)
    {
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("technique", &index, sizeof(index));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("technique"))
            {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = index;
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || expanded)
        draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));

    ImGui::SetCursorScreenPos(start + ImVec2(S(10), (size.y - S(18)) / 2));
    if (Switch("on", technique.enabled))
        app.runtime.SetEnabled(index, !technique.enabled);

    const float chevron = start.x + size.x - S(20);
    PushSize(14.5f);
    const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(chevron - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(52), textY), effect.gpuFailed ? kDim : kText, label.c_str());
    ImGui::PopFont();
    PushSize(12.5f);
    draw->AddText(ImVec2(start.x + S(52) + labelSize.x + S(8), textY + S(1.5f)), effect.gpuFailed ? kWarning : kDim,
                  effect.gpuFailed ? "failed" : effect.file.c_str());
    ImGui::PopFont();
    draw->PopClipRect();

    Chevron(draw, ImVec2(chevron, start.y + size.y / 2), expanded, expanded || hovered ? kText : kDim);

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    if (expanded)
        DrawParameters(app, technique, effect);
    ImGui::PopID();
}

void EffectsTab(App& app)
{
    std::vector<fx::Technique>& techniques = app.runtime.Techniques();
    const std::vector<fx::Effect>& effects = app.runtime.Effects();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search effects", menu.search, sizeof(menu.search));

    if (app.runtime.Loading())
    {
        const auto [loaded, total] = app.runtime.LoadingProgress();
        ImGui::Dummy(ImVec2(0, S(2)));
        Spinner(S(10));
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
        Text(techniques.empty() ? "Loading effects. The first time can take a minute."
                                : "Compiling effects " + std::to_string(loaded) + " of " + std::to_string(total),
             kDim, 14);
    }
    else if (techniques.empty())
    {
        ImGui::Dummy(ImVec2(0, S(6)));
        Text("No effects are installed. Install them from the launcher.", kDim, 14);
    }
    if (techniques.empty())
        return;

    const std::string filter = Lowercase(menu.search);
    const auto label = [](const fx::Technique& technique) -> const std::string& { return technique.label.empty() ? technique.name : technique.label; };
    const auto shown = [&](const fx::Technique& technique) {
        return !technique.hidden &&
               (filter.empty() || Lowercase(label(technique)).find(filter) != std::string::npos ||
                Lowercase(effects[technique.effect].file).find(filter) != std::string::npos);
    };

    // An effect turned off stays in Active until the menu closes, so it can be turned back on where it was.
    if (menu.activePreset != app.runtime.PresetPath())
    {
        menu.active.clear();
        menu.activePreset = app.runtime.PresetPath();
    }
    for (const fx::Technique& technique : techniques)
        if (technique.enabled)
            menu.active.insert(fx::TechniqueKey(technique, effects[technique.effect]));
    const auto active = [&](const fx::Technique& technique) { return menu.active.count(fx::TechniqueKey(technique, effects[technique.effect])) != 0; };

    const int count = static_cast<int>(techniques.size());
    int moveFrom = -1, moveTo = -1;
    Heading("ACTIVE");
    bool any = false;
    for (int i = 0; i < count; ++i)
        if (active(techniques[i]) && shown(techniques[i]))
        {
            TechniqueRow(app, i, moveFrom, moveTo);
            any = true;
        }
    if (!any)
        Text(filter.empty() ? "No effects are on. Pick a preset or turn effects on below." : "No active effects match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    // The rest by name.
    std::vector<int> others;
    for (int i = 0; i < count; ++i)
        if (!active(techniques[i]) && shown(techniques[i]))
            others.push_back(i);
    std::sort(others.begin(), others.end(), [&](int a, int b) { return strcasecmp(label(techniques[a]).c_str(), label(techniques[b]).c_str()) < 0; });
    const bool searching = !filter.empty();
    const std::string title = "ALL EFFECTS (" + std::to_string(others.size()) + ")";
    PushSize(12);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::SetNextItemOpen(searching || menu.showAll);
    const bool open = ImGui::TreeNodeEx((title + "###all").c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::IsItemToggledOpen() && !searching)
        menu.showAll = !menu.showAll;
    HandOnHover();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (open)
        for (int index : others)
            TechniqueRow(app, index, moveFrom, moveTo);

    if (moveFrom >= 0)
        app.runtime.MoveTechnique(moveFrom, moveTo);
}

// Settings

// While the menu waits for a shortcut, the next key pressed with any modifiers becomes it.
void RecordShortcut(App& app)
{
    if (menu.recording < 0)
        return;
    // Space would otherwise also press the control the keyboard is on.
    ImGui::SetNavCursorVisible(false);
    // Modifiers straight from GLFW: Dear ImGui swaps Cmd and Ctrl on macOS.
    GLFWwindow* window = app.overlay.window;
    const auto down = [window](int a, int b) { return glfwGetKey(window, a) == GLFW_PRESS || glfwGetKey(window, b) == GLFW_PRESS; };
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
    {
        if (!IsShortcutKey(static_cast<ImGuiKey>(key)) || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
            continue;
        Hotkey hotkey;
        hotkey.key = static_cast<ImGuiKey>(key);
        hotkey.modifiers = (down(GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL) ? kCtrl : 0u) | (down(GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT) ? kAlt : 0u) |
                           (down(GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT) ? kShift : 0u) |
                           (down(GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER) ? kSuper : 0u);
        InputHotkeys changed = app.settings.hotkeys;
        changed.*kShortcuts[menu.recording].member = hotkey;
        StopRecording(app);
        // Escape alone stops waiting.
        if (hotkey.key != ImGuiKey_Escape || hotkey.modifiers)
            app.ChangeHotkeys(changed, menu.shortcutError);
        break;
    }
}

void ShortcutRow(App& app, int index)
{
    const Shortcut& shortcut = kShortcuts[index];
    ImGui::PushID(index);
    const float x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = S(130);

    ImGui::BeginGroup();
    Text(shortcut.label, kText, 14.5f, x + width - buttonWidth - S(14));
    Text(kShortcutDescriptions[index], kDim, 13, x + width - buttonWidth - S(14));
    ImGui::EndGroup();
    const float bottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos(ImVec2(x + width - buttonWidth, top + S(2)));
    const bool recording = menu.recording == index;
    const std::string label = recording ? "Press keys..." : FormatHotkey(app.settings.hotkeys.*shortcut.member);
    ImGui::PushStyleColor(ImGuiCol_Border, recording ? kAccent : kBorderStrong);
    if (Button((label + "##key").c_str(), ImVec2(buttonWidth, S(32))))
    {
        if (recording)
            StopRecording(app);
        else
        {
            StopRecording(app);
            menu.recording = index;
            menu.shortcutError.clear();
            app.SuspendHotkeys(true);
        }
    }
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(x, std::max(bottom, top + S(40)) + S(6)));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

// A switch with its title and description beside it. Returns true when clicked.
bool SwitchRow(const char* id, bool on, const char* title, const char* description)
{
    const bool clicked = Switch(id, on);
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    Text(title, kText, 14.5f);
    Text(description, kDim, 13);
    ImGui::EndGroup();
    return clicked;
}

void GeneralSettings(App& app)
{
    Heading("PRESETS");
    if (SwitchRow("autosave", app.settings.autoSavePresets, "Save changes automatically",
                  "Turn off to try changes first and save them with the icon at the top."))
    {
        app.settings.autoSavePresets = !app.settings.autoSavePresets;
        // From here on every change saves as it happens, so changes that were waiting are saved too.
        if (app.settings.autoSavePresets && app.runtime.Dirty())
            app.runtime.SavePreset();
        SaveSettings(app.settings);
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    Heading("EFFECTS");
    for (const fs::path& path : app.settings.effectPaths)
        Text(path.string(), kDim, 13);
    ImGui::Dummy(ImVec2(0, S(2)));
    if (Button("Reload effects", ImVec2(S(130), S(32))))
        app.runtime.Reload();
    ImGui::SameLine(0, S(8));
    if (Button("Open data folder", ImVec2(S(150), S(32))))
        platform::Open(DataDirectory().string());
}

void ShortcutSettings(App& app)
{
    Text("Click a shortcut, then press the keys you want. They work right away.", kDim, 13);
    ImGui::Dummy(ImVec2(0, S(4)));
    for (int i = 0; i < static_cast<int>(std::size(kShortcuts)); ++i)
        ShortcutRow(app, i);
    if (!menu.shortcutError.empty())
        Text(menu.shortcutError, kError, 13.5f);
}

void SettingsTab(App& app)
{
    const int clicked = Segmented("page", { "General", "Shortcuts" }, static_cast<int>(menu.settingsPage), kBorder);
    if (clicked >= 0)
    {
        // A shortcut left waiting for keys on its page would take the next key pressed on another.
        StopRecording(app);
        menu.settingsPage = static_cast<SettingsPage>(clicked);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    if (menu.settingsPage == SettingsPage::General)
        GeneralSettings(app);
    else
        ShortcutSettings(app);
}

// Status

void StatusTab(App& app)
{
    const float x = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    Heading("STATUS");
    if (!menu.notices.empty())
    {
        // The log keeps them.
        PushSize(12);
        const char* dismiss = "Dismiss all";
        ImGui::SameLine(x + width - ImGui::CalcTextSize(dismiss).x);
        if (Link(dismiss, kDim))
            ClearNotices();
        ImGui::PopFont();
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PushSize(13.5f);
    for (auto notice = menu.notices.rbegin(); notice != menu.notices.rend(); ++notice)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        NoticeIcon(draw, start + ImVec2(S(8), ImGui::GetFontSize() / 2 + S(1)), LevelOf(notice->level));
        ImGui::SetCursorScreenPos(start + ImVec2(S(26), 0));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(notice->text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (menu.notices.empty())
        ImGui::TextDisabled("No messages.");
    ImGui::PopFont();

    std::vector<fx::Effect>& effects = app.runtime.Effects();
    const size_t failed = static_cast<size_t>(std::count_if(effects.begin(), effects.end(), [](const fx::Effect& effect) { return !effect.compiled; }));
    if (failed)
    {
        ImGui::Dummy(ImVec2(0, S(8)));
        Heading(("DID NOT COMPILE (" + std::to_string(failed) + ")").c_str());
        for (const fx::Effect& effect : effects)
        {
            if (effect.compiled)
                continue;
            ImGui::PushID(effect.file.c_str());
            const bool open = menu.shownErrors == effect.file;
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const ImVec2 size(ImGui::GetContentRegionAvail().x, S(34));
            if (ImGui::InvisibleButton("errors", size, ImGuiButtonFlags_EnableNav))
                menu.shownErrors = open ? std::string() : effect.file;
            const bool hovered = ImGui::IsItemHovered();
            HandOnHover();
            if (hovered || open)
                draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));
            PushSize(14.5f);
            draw->AddText(ImVec2(start.x + S(12), start.y + (size.y - ImGui::GetFontSize()) / 2), kText, effect.file.c_str());
            ImGui::PopFont();
            Chevron(draw, ImVec2(start.x + size.x - S(20), start.y + size.y / 2), open, open || hovered ? kText : kDim);
            if (open)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
                ImGui::BeginChild("text", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleVar();
                Text(effect.errors, kDim, 12.5f);
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0, S(4)));
    PushSize(13.5f);
    if (Link("Open log"))
        platform::Open(LogPath().string());
    ImGui::SameLine(0, S(16));
    if (Link("Get help on Discord"))
        platform::Open(kHelpUrl);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(6)));
    Text("Unishade " UNISHADE_VERSION ". Effects compile with ReShade's compiler by crosire.", kDim, 12.5f);
}

// Frame

void Header(App& app, ImVec2 origin, float width)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float logo = std::round(S(38));
    const ImVec2 logoPosition = origin + ImVec2(std::round(S(kPadding)), std::round(S(18)));
    if (const ImTextureID texture = PictureTexture(kLogoPicture, logo))
        draw->AddImage(ImTextureRef(texture), logoPosition, logoPosition + ImVec2(logo, logo));
    const float textX = S(kPadding) + logo + S(12);
    PushSize(16.5f);
    draw->AddText(origin + ImVec2(textX, S(17)), kText, "Unishade");
    ImGui::PopFont();
    // The right side holds the effects switch, its label and, with auto-save off, the save icon.
    const bool autoSave = app.settings.autoSavePresets;
    const bool unsaved = !autoSave && app.runtime.Dirty();
    PushSize(13);
    const ImVec2 labelSize = ImGui::CalcTextSize("Effects");
    const float switchX = width - S(kPadding) - S(32);
    const float labelX = switchX - S(8) - labelSize.x;
    const ImVec2 saveSize(S(30), S(30));
    const float saveX = labelX - S(14) - saveSize.x;

    const std::string preset = app.runtime.PresetPath().stem().string();
    const ImVec2 nameSize = ImGui::CalcTextSize(preset.c_str());
    draw->PushClipRect(origin, origin + ImVec2((autoSave ? labelX : saveX) - S(12), S(kHeader)), true);
    draw->AddText(origin + ImVec2(textX, S(39)), kDim, preset.c_str());
    if (unsaved)
        draw->AddCircleFilled(origin + ImVec2(textX + nameSize.x + S(7), S(39) + ImGui::GetFontSize() / 2 + S(1)), S(3), kWarning);
    draw->PopClipRect();
    ImGui::PopFont();

    if (!autoSave)
    {
        const ImVec2 start = origin + ImVec2(saveX, S(22));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton("save", saveSize, ImGuiButtonFlags_EnableNav) && unsaved)
            app.runtime.SavePreset();
        if (ImGui::IsItemHovered() && unsaved)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            draw->AddRectFilled(start, start + saveSize, kBorder, S(6));
        }
        Tooltip((unsaved ? "Save changes to " : "No unsaved changes in ") + preset);
        SaveIcon(draw, start + saveSize * 0.5f, S(14), unsaved ? kAccentHover : kBorderStrong);
    }

    // Effects on and off for everything.
    ImGui::SetCursorScreenPos(origin + ImVec2(switchX, S(28)));
    if (Switch("effects", app.effectsEnabled))
        app.effectsEnabled = !app.effectsEnabled;
    Tooltip(app.effectsEnabled ? "Turn all effects off" : "Turn effects back on");
    PushSize(13);
    draw->AddText(origin + ImVec2(labelX, S(28) + (S(18) - labelSize.y) / 2), kDim, "Effects");
    ImGui::PopFont();

    Rainbow(draw, origin + ImVec2(0, S(kHeader) - S(2)), origin + ImVec2(width, S(kHeader)));
}

void Tabs(ImVec2 origin, float width)
{
    constexpr std::pair<const char*, Tab> entries[] = {
        { "Presets", Tab::Presets }, { "Effects", Tab::Effects }, { "Settings", Tab::Settings }, { "Status", Tab::Status }
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tabWidth = (width - S(kPadding) * 2) / static_cast<float>(std::size(entries));
    const bool problems = std::any_of(menu.notices.begin(), menu.notices.end(), [](const Notice& notice) { return notice.level >= LogLevel::Warning; });
    PushSize(14);
    int index = 0;
    for (const auto& [name, tab] : entries)
    {
        const ImVec2 start = origin + ImVec2(S(kPadding) + static_cast<float>(index++) * tabWidth, S(kHeader));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton(name, ImVec2(tabWidth, S(kTabs)), ImGuiButtonFlags_EnableNav))
            menu.tab = tab;
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        const bool active = tab == menu.tab;
        const ImVec2 text = ImGui::CalcTextSize(name);
        const ImVec2 textStart = start + ImVec2((tabWidth - text.x) / 2, (S(kTabs) - text.y) / 2);
        draw->AddText(textStart, active || hovered ? kText : kDim, name);
        if (active)
            draw->AddRectFilled(ImVec2(textStart.x - S(6), start.y + S(kTabs) - S(2)), ImVec2(textStart.x + text.x + S(6), start.y + S(kTabs)), kAccent,
                                S(1));
        if (tab == Tab::Status && problems)
            draw->AddCircleFilled(textStart + ImVec2(text.x + S(6), S(3)), S(3), kWarning);
    }
    ImGui::PopFont();
    draw->AddLine(origin + ImVec2(0, S(kHeader + kTabs)), origin + ImVec2(width, S(kHeader + kTabs)), kBorder);
}

void Footer(App& app, ImVec2 origin, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float top = size.y - S(kFooter);
    draw->AddLine(origin + ImVec2(0, top), origin + ImVec2(size.x, top), kBorder);
    const float buttonY = top + (S(kFooter) - S(34)) / 2;

    // Effects stay off while the compare button is held.
    ImGui::SetCursorScreenPos(origin + ImVec2(S(kPadding), buttonY));
    PushSize(14);
    Button("Compare", ImVec2(S(100), S(34)));
    app.compareButton = ImGui::IsItemActive();
    if (!app.compareButton)
        Tooltip("Hold to see the game without effects");
    ImGui::PopFont();

    // Screenshots leave out the menu.
    ImGui::SameLine(0, S(8));
    const ImVec2 cameraStart = ImGui::GetCursorScreenPos();
    const ImVec2 cameraSize(S(34), S(34));
    if (ImGui::InvisibleButton("camera", cameraSize, ImGuiButtonFlags_EnableNav))
        ImGui::OpenPopup("screenshots");
    const bool cameraHovered = ImGui::IsItemHovered();
    HandOnHover();
    Tooltip("Screenshots");
    draw->AddRectFilled(cameraStart, cameraStart + cameraSize, cameraHovered ? kCardHover : kCard, S(6));
    draw->AddRect(cameraStart, cameraStart + cameraSize, kBorder, S(6));
    CameraIcon(draw, cameraStart + cameraSize * 0.5f, S(15), cameraHovered ? kText : kDim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("screenshots"))
    {
        if (ImGui::MenuItem("Screenshot"))
            app.RequestScreenshot(false);
        if (ImGui::MenuItem("Before and after"))
            app.RequestScreenshot(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Open screenshots folder"))
        {
            const fs::path folder = ScreenshotDirectory();
            std::error_code ignored;
            fs::create_directories(folder, ignored);
            platform::Open(folder.string());
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    // The menu shortcut, which also leaves.
    const char* label = "Back to the game";
    const std::string key = app.HotkeyText(kEditModeHotkey);
    PushSize(13.5f);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    PushSize(13);
    const float keyWidth = std::max(ImGui::CalcTextSize(key.c_str()).x + S(14), S(28));
    const float keyHeight = ImGui::GetFontSize() + S(6);
    ImGui::PopFont();
    const ImVec2 backSize(keyWidth + S(8) + labelSize.x, S(34));
    const ImVec2 backStart = origin + ImVec2(size.x - S(kPadding) - backSize.x, buttonY);
    ImGui::SetCursorScreenPos(backStart);
    const bool back = ImGui::InvisibleButton("back", backSize, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImGui::SetCursorScreenPos(backStart + ImVec2(0, (backSize.y - keyHeight) / 2));
    KeyCap(key);
    PushSize(13.5f);
    draw->AddText(backStart + ImVec2(keyWidth + S(8), (backSize.y - labelSize.y) / 2), hovered ? kText : kDim, label);
    ImGui::PopFont();
    if (back)
        app.CloseMenu();
}

void DrawMenu(App& app)
{
    menu.notices = Notices();
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(S(kWidth), io.DisplaySize.x - S(kMargin) * 2);
    const float available = std::max(io.DisplaySize.y - S(kMargin) * 2, 1.0f);
    // A window too short for the menu scrolls all of it, so the text keeps a readable size.
    const ImVec2 size(width, std::max(available, S(menu_layout::MinHeight())));
    const bool scrolls = size.y > available;
    ImGui::SetNextWindowPos(ImVec2(S(kMargin), S(kMargin)));
    ImGui::SetNextWindowSize(ImVec2(width, available));
    ImGui::Begin("Unishade##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     (scrolls ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollWithMouse));
    const ImVec2 origin = ImGui::GetWindowPos() - ImVec2(0, ImGui::GetScrollY());
    Header(app, origin, width);
    Tabs(origin, width);

    const float top = S(kHeader + kTabs) + 1;
    ImGui::SetCursorScreenPos(origin + ImVec2(0, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(kPadding), S(16)));
    ImGui::BeginChild("content", ImVec2(width, size.y - top - S(kFooter)), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar();
    switch (menu.tab)
    {
    case Tab::Presets: PresetsTab(app); break;
    case Tab::Effects: EffectsTab(app); break;
    case Tab::Settings: SettingsTab(app); break;
    case Tab::Status: StatusTab(app); break;
    }
    ImGui::EndChild();
    Footer(app, origin, size);
    // Dialogs show over any tab.
    NameDialog(app);
    UnsavedDialog(app);
    ConfirmDialog(app);
    if (scrolls)
    {
        // Lets the window scroll down to the footer.
        ImGui::SetCursorScreenPos(origin + ImVec2(0, size.y));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::End();
}

void DrawMenuFrame(App& app)
{
    // Escape leaves the menu, unless it closes a popup, ends typing or stops waiting for a shortcut.
    const bool leave = menu.recording < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
                       !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    RecordShortcut(app);

    DrawMenu(app);

    if (menu.recording >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        StopRecording(app);
    if (leave)
        app.CloseMenu();
}

// A short message at the bottom of the game, with a key before it when it has one.
void DrawToast(App& app)
{
    const double left = app.toastUntil - glfwGetTime();
    if (app.toast.empty() || left <= 0)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y - S(48)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(static_cast<float>(left / 0.6), 0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    if (!app.toastKey.empty())
    {
        KeyCap(app.toastKey, 14);
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
    }
    Text(app.toast, kText, 14.5f, -1.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
}


// Launcher

// What the launcher keeps between frames. The model is described again every frame.
struct LauncherState
{
    // The open windows the picker lists, to add a game or use a window until Detect automatically.
    bool picking = false;
    bool pickToAdd = true;
    std::map<platform::WindowId, std::string> details; // the executable's name for each window listed
    // A removed game, which can be put back for a few seconds.
    std::optional<AutoGame> removed;
    size_t removedIndex = 0;
    double removedAt = 0;
    std::string renameProblem;
    int recording = -1; // the shortcut waiting for its keys
    std::string shortcutError;
    // Whether the picked window is still open, asked now and then.
    bool selectedOpen = true;
    double selectedChecked = -10;
    double picturesChecked = 0;
    // Each folder's presets, by path, read now and then.
    std::map<std::string, std::vector<fs::path>> presets;
    double presetsRead = -10;
    // What the cards' buttons and the links do.
    std::vector<std::vector<std::function<void()>>> cards;
    std::vector<std::function<void()>> links;
};
LauncherState launcherState;

// A saved game's presets folder. Names on Linux differ by case, and a folder named in another case is still the game's.
fs::path GameFolder(const std::string& folder)
{
    const fs::path root = PresetsDirectory();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error))
        if (entry.is_directory(error) && !strcasecmp(entry.path().filename().c_str(), folder.c_str()))
            return entry.path();
    return root / folder;
}

const std::vector<fs::path>& PresetsOf(const fs::path& folder)
{
    auto [entry, added] = launcherState.presets.try_emplace(folder.string());
    if (added)
        entry->second = PresetsIn(folder);
    return entry->second;
}

void StopShortcut(App& app)
{
    if (launcherState.recording >= 0)
        app.SuspendHotkeys(false);
    launcherState.recording = -1;
}

// While a shortcut waits for its keys, the next key pressed with any modifiers becomes it.
void TakeShortcut(App& app)
{
    if (launcherState.recording < 0)
        return;
    // Space would otherwise also press the control the keyboard is on.
    ImGui::SetNavCursorVisible(false);
    // Modifiers straight from GLFW: Dear ImGui swaps Cmd and Ctrl on macOS.
    GLFWwindow* window = app.launcher.window;
    const auto down = [window](int a, int b) { return glfwGetKey(window, a) == GLFW_PRESS || glfwGetKey(window, b) == GLFW_PRESS; };
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
    {
        if (!IsShortcutKey(static_cast<ImGuiKey>(key)) || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
            continue;
        Hotkey hotkey;
        hotkey.key = static_cast<ImGuiKey>(key);
        hotkey.modifiers = (down(GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL) ? kCtrl : 0u) | (down(GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT) ? kAlt : 0u) |
                           (down(GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT) ? kShift : 0u) |
                           (down(GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER) ? kSuper : 0u);
        InputHotkeys changed = app.settings.hotkeys;
        changed.*kShortcuts[launcherState.recording].member = hotkey;
        StopShortcut(app);
        // Escape alone stops waiting.
        launcherState.shortcutError.clear();
        if (hotkey.key != ImGuiKey_Escape || hotkey.modifiers)
            app.ChangeHotkeys(changed, launcherState.shortcutError);
        break;
    }
}

// Renames a saved game, with its presets folder and its remembered preset. Returns why it could not, or nothing.
std::string RenameGame(App& app, size_t index, std::string name)
{
    if (index >= app.autoGames.size())
        return {};
    // Spaces around it would not survive as a folder name or an ini key.
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    AutoGame& game = app.autoGames[index];
    if (game.name == name)
        return {};
    switch (CheckName(name, true))
    {
    case NameIssue::None: break;
    case NameIssue::Empty: return "Type a name.";
    case NameIssue::Control: return "A name can't contain control characters.";
    case NameIssue::Character: return "A name can't contain \\ / : * ? \" < > | = ; [ or ].";
    case NameIssue::End: return "A name can't end with a dot or a space.";
    case NameIssue::Device: return std::string(DeviceName(name)) + " is kept for devices on Windows, where presets can go too.";
    }
    for (size_t i = 0; i < app.autoGames.size(); ++i)
        if (i != index && !strcasecmp(FolderName(app.autoGames[i].name).c_str(), name.c_str()))
            return "Another game has this name.";

    const std::string key = FolderName(game.name);
    const fs::path from = GameFolder(key);
    const fs::path to = PresetsDirectory() / name;
    const bool playing = !app.game.empty() && !strcasecmp(app.game.c_str(), key.c_str());
    const fs::path current = app.runtime.PresetPath();
    const bool currentMoves = !current.empty() && current.parent_path() == from;
    std::error_code error;
    const bool moveFolder = strcasecmp(key.c_str(), name.c_str()) && fs::is_directory(from, error) && !fs::exists(to, error);
    if (moveFolder)
    {
        // Saved changes go with the folder. Unsaved ones would be written back to where it was.
        if (playing && currentMoves && app.runtime.Dirty() && app.settings.autoSavePresets)
            app.runtime.SavePreset();
        fs::rename(from, to, error);
        if (error)
            return "Could not rename its presets folder.";
    }
    const fs::path preset = GamePreset(key);
    game.name = name;
    app.SaveGames();
    if (!preset.empty())
    {
        SetGamePreset(name, moveFolder && preset.parent_path() == from ? to / preset.filename() : preset);
        SetGamePreset(key, fs::path());
    }
    if (playing)
    {
        app.game = FolderName(name);
        if (moveFolder && currentMoves)
        {
            if (app.runtime.Dirty())
                app.ShowToast("Unsaved changes to " + current.stem().string() + " were dropped, since its game was renamed");
            app.SwitchPreset(to / current.filename(), false, true);
        }
    }
    launcherState.presets.clear();
    Log(LogLevel::Info, "Renamed %s to %s.", key.c_str(), name.c_str());
    return {};
}

void UseWindow(App& app, bool add, const platform::Window& window)
{
    if (add)
    {
        if (!AddAutoGame(app.autoGames, window))
        {
            Report(LogLevel::Warning, "%s has closed.", window.title.c_str());
            return;
        }
        // Detection follows the game in front, which is the one just added.
        app.Select(std::nullopt);
        app.SaveGames();
    }
    else
        app.Select(window);
    platform::Activate(window);
}

class Host final : public launcher::Host
{
public:
    explicit Host(App& app) : app(app) {}

    void SetSwitch(launcher::Switch which, bool on) override
    {
        if (which == launcher::Switch::EffectsShown && app.captureEnabled != on)
            app.ToggleOverlay();
        else if (which == launcher::Switch::AutoSavePresets)
        {
            app.settings.autoSavePresets = on;
            // From here on every change saves as it happens, so changes that were waiting are saved too.
            if (on && app.runtime.Dirty())
                app.runtime.SavePreset();
            SaveSettings(app.settings);
        }
    }

    void PressCard(size_t card, size_t button) override
    {
        if (card < launcherState.cards.size() && button < launcherState.cards[card].size())
            launcherState.cards[card][button]();
    }

    void OpenLink(size_t link) override
    {
        if (link < launcherState.links.size())
            launcherState.links[link]();
    }

    void OpenUrl(std::string_view url) override
    {
        platform::Open(std::string(url));
    }

    void DismissNotices() override
    {
        ClearNotices();
    }

    void OpenPicker(bool add) override
    {
        launcherState.picking = true;
        launcherState.pickToAdd = add;
        launcherState.details.clear();
    }

    void ChooseWindow(size_t window) override
    {
        const std::vector<platform::Window>& windows = app.Windows();
        if (!launcherState.picking || window >= windows.size())
            return;
        launcherState.picking = false;
        UseWindow(app, launcherState.pickToAdd, platform::Window(windows[window]));
    }

    void ClosePicker() override
    {
        launcherState.picking = false;
    }

    void DetectAutomatically() override
    {
        app.Select(std::nullopt);
    }

    void SetGameEnabled(size_t game, bool enabled) override
    {
        if (game >= app.autoGames.size())
            return;
        app.autoGames[game].enabled = enabled;
        app.SaveGames();
    }

    bool RenameGame(size_t game, std::string_view name) override
    {
        launcherState.renameProblem = ::RenameGame(app, game, std::string(name));
        return launcherState.renameProblem.empty();
    }

    void RemoveGame(size_t game) override
    {
        if (game >= app.autoGames.size())
            return;
        launcherState.removed = app.autoGames[game];
        launcherState.removedIndex = game;
        launcherState.removedAt = glfwGetTime();
        app.autoGames.erase(app.autoGames.begin() + static_cast<std::ptrdiff_t>(game));
        app.SaveGames();
    }

    void UndoRemove() override
    {
        if (!launcherState.removed)
            return;
        const AutoGame removed = *std::exchange(launcherState.removed, std::nullopt);
        // Unless it was added again meanwhile.
        if (std::any_of(app.autoGames.begin(), app.autoGames.end(),
                        [&](const AutoGame& game) { return !strcasecmp(game.executable.c_str(), removed.executable.c_str()); }))
            return;
        app.autoGames.insert(app.autoGames.begin() + static_cast<std::ptrdiff_t>(std::min(launcherState.removedIndex, app.autoGames.size())), removed);
        app.SaveGames();
    }

    void UsePreset(size_t game, std::string_view preset) override
    {
        if (game >= app.autoGames.size())
            return;
        const std::string folder = FolderName(app.autoGames[game].name);
        const fs::path path{ std::string(preset) };
        if (app.game.empty() || strcasecmp(app.game.c_str(), folder.c_str()))
            SetGamePreset(folder, path);
        // The launcher is not over the game, so what goes wrong is also reported here.
        else if (!app.SwitchPreset(path, false, false))
            Report(LogLevel::Warning, "Save or discard the changes to %s in the menu to switch to %s.", app.runtime.PresetPath().stem().c_str(),
                   path.stem().c_str());
    }

    void OpenPresetsFolder(size_t game) override
    {
        if (game >= app.autoGames.size())
            return;
        const fs::path folder = GameFolder(FolderName(app.autoGames[game].name));
        std::error_code error;
        fs::create_directories(folder, error);
        platform::Open(folder.string());
    }

    void SetPerformance(std::optional<size_t>, launcher::PerformanceSetting, int) override {}
    void StepMenuSize(int) override {}

    void RecordShortcut(int shortcut) override
    {
        StopShortcut(app);
        if (shortcut >= 0 && static_cast<size_t>(shortcut) < std::size(kShortcuts))
        {
            launcherState.recording = shortcut;
            launcherState.shortcutError.clear();
            app.SuspendHotkeys(true);
        }
    }

    // Shortcuts here always have keys, since two without would count as sharing them.
    void ClearShortcut(size_t) override {}

    void ResetShortcuts() override
    {
        InputHotkeys hotkeys;
        for (const Shortcut& shortcut : kShortcuts)
            ParseHotkey(shortcut.fallback, hotkeys.*shortcut.member);
        launcherState.shortcutError.clear();
        app.ChangeHotkeys(hotkeys, launcherState.shortcutError);
    }

private:
    App& app;
};

launcher::Status DescribeStatus(App& app)
{
    launcher::Status status;
    const std::optional<platform::Window>& game = app.active ? app.active : app.selected;
    status.game = game ? game->title : std::string();
    status.picture = app.active && !app.game.empty() ? GameFolder(app.game).string() : std::string();
    status.warning = app.lastCaptureError;
    status.effectsShown = app.captureEnabled;
    bool windowClosed = false;
    if (!app.captureEnabled)
    {
        status.tone = launcher::Tone::Off;
        status.title = "Effects off";
        status.detail = "Turn them back on below, or press " + app.HotkeyText(kOverlayToggleHotkey) + ".";
    }
    else if (app.active)
    {
        status.tone = launcher::Tone::Running;
        status.title = "Running on " + app.active->title;
        status.detail = "Press " + app.HotkeyText(kEditModeHotkey) + " in the game to open the menu.";
    }
    else if (app.selected)
    {
        // Each check asks the system, so only a few times a second.
        if (glfwGetTime() - launcherState.selectedChecked > 0.5)
        {
            launcherState.selectedOpen = platform::WindowExists(*app.selected);
            launcherState.selectedChecked = glfwGetTime();
        }
        windowClosed = !launcherState.selectedOpen;
        status.title = "Waiting for " + app.selected->title;
        status.detail = windowClosed ? "Its window closed. Pick its new window." : "Return to the game to see the effects.";
    }
    else
    {
        status.title = "Waiting for a game";
        status.detail = std::any_of(app.autoGames.begin(), app.autoGames.end(), [](const AutoGame& saved) { return saved.enabled; })
                            ? "Open one of your games."
                            : "Open a game and choose Add game.";
    }
    status.canPick = !app.selected || windowClosed;
    status.canDetect = app.selected.has_value();
    return status;
}

// Cards for what Windows' Setup does elsewhere: screen recording on macOS, compiling effects and installing them.
void DescribeCards(App& app, launcher::Model& model)
{
    launcherState.cards.clear();
    const auto card = [&](launcher::Card shown, std::vector<std::function<void()>> actions = {}) {
        model.cards.push_back(std::move(shown));
        launcherState.cards.push_back(std::move(actions));
    };
    if (!platform::HasCapturePermission())
        card({ .title = "Unishade needs to record the screen",
               .text = "macOS asks before a program can copy another program's window. Allow Unishade under Privacy & Security, Screen & System "
                       "Audio Recording, then open Unishade again.",
               .attention = true,
               .buttons = { "Allow screen recording" } },
             { [] { platform::RequestCapturePermission(); } });
    if (app.runtime.Loading())
    {
        const auto [loaded, total] = app.runtime.LoadingProgress();
        card({ .title = "Compiling effects",
               .text = std::to_string(loaded) + " of " + std::to_string(total),
               .progress = total ? float(loaded) / float(total) : 0.0f });
    }
    const EffectSetup::State setup = app.setup.Read();
    if (setup.running)
        card({ .title = "Installing effects",
               .text = setup.status + (setup.detail.empty() ? "" : "\n" + setup.detail),
               .progress = setup.fraction,
               .buttons = { "Cancel" } },
             { [&app] { app.setup.Cancel(); } });
    else if (setup.finished)
    {
        std::string text = setup.error.empty() ? setup.status : setup.error;
        for (const std::string& note : setup.notes)
            text += "\nSkipped " + note;
        launcher::Card finished{ .title = setup.error.empty() ? "Effects are installed" : "Installing effects failed", .text = text };
        if (!setup.error.empty())
            finished.buttons = { "Try again" };
        card(std::move(finished), { [&app] { app.setup.Start(); } });
    }
    else if (!app.EffectsInstalled())
        card({ .title = "Effects are not installed yet",
               .text = "Unishade runs ReShade's effects. Download every package from ReShade's official list and the presets made for Unishade into "
                       "the data folder.",
               .attention = true,
               .buttons = { "Download effects and presets" } },
             { [&app] { app.setup.Start(); } });
}

launcher::Model DescribeLauncher(App& app)
{
    launcher::Model model;
    model.version = UNISHADE_VERSION;
    model.status = DescribeStatus(app);
    DescribeCards(app, model);

    // The presets folder is read again now and then.
    if (glfwGetTime() - launcherState.presetsRead > kPresetsInterval)
    {
        launcherState.presets.clear();
        launcherState.presetsRead = glfwGetTime();
    }
    const fs::path root = PresetsDirectory();
    for (const AutoGame& saved : app.autoGames)
    {
        const std::string folder = FolderName(saved.name);
        const fs::path path = GameFolder(folder);
        launcher::Game game{ .id = saved.executable,
                             .name = saved.name,
                             .executable = saved.executable,
                             .picture = folder.empty() ? std::string() : path.string(),
                             .enabled = saved.enabled,
                             .running = saved.enabled && app.active && MatchesProcess(saved, app.activeExecutable, app.activeCommand) };
        const bool playing = !folder.empty() && !strcasecmp(app.game.c_str(), folder.c_str());
        const fs::path inUse = playing ? app.runtime.PresetPath() : folder.empty() ? fs::path() : GamePreset(folder);
        const auto add = [&](const fs::path& preset, const std::string& from) {
            game.presets.push_back({ preset.string(), preset.stem().string(), from });
            if (preset == inUse)
                game.preset = preset.string();
        };
        if (!folder.empty())
            for (const fs::path& preset : PresetsOf(path))
                add(preset, "");
        for (const fs::path& preset : PresetsOf(root))
            add(preset, "All games");
        if (game.preset.empty() && !inUse.empty())
            add(inUse, inUse.parent_path().filename().string());
        model.games.push_back(std::move(game));
    }
    if (launcherState.removed && glfwGetTime() - launcherState.removedAt > kUndoSeconds)
        launcherState.removed.reset();
    if (launcherState.removed)
        model.removed = launcher::Removed{ launcherState.removedIndex, launcherState.removed->name };

    for (const Notice& notice : Notices())
        model.notices.push_back({ LevelOf(notice.level), notice.text });

    launcherState.links.clear();
    const auto link = [&](const char* label, std::function<void()> open) {
        model.links.push_back(label);
        launcherState.links.push_back(std::move(open));
    };
    link("Open log", [] { platform::Open(LogPath().string()); });
    link("Get help on Discord", [] { platform::Open(kHelpUrl); });
    link("Data folder", [] { platform::Open(DataDirectory().string()); });
    link("Screenshots", [] {
        const fs::path folder = ScreenshotDirectory();
        std::error_code error;
        fs::create_directories(folder, error);
        platform::Open(folder.string());
    });
    link("Docs", [] { platform::Open("https://unishade.me/docs/"); });

    model.settings.autoSavePresets = app.settings.autoSavePresets;
    for (size_t i = 0; i < std::size(kShortcuts); ++i)
        model.settings.shortcuts.push_back({ kShortcuts[i].label, kShortcutDescriptions[i], FormatHotkey(app.settings.hotkeys.*kShortcuts[i].member), true });
    model.settings.recording = launcherState.recording;
    model.settings.shortcutError = launcherState.shortcutError;

    if (launcherState.picking)
    {
        launcher::Picker picker{ .add = launcherState.pickToAdd };
        for (const platform::Window& window : app.Windows())
        {
            auto detail = launcherState.details.find(window.id);
            if (detail == launcherState.details.end())
                detail = launcherState.details.emplace(window.id, fs::path(platform::ProcessExecutable(window.pid)).filename().string()).first;
            picker.windows.push_back({ window.title, detail->second, "" });
        }
        model.picker = std::move(picker);
    }
    model.renameProblem = launcherState.renameProblem;
    return model;
}
// The interface's scale for the window's monitor. On X11 it is the same on every monitor.
float WindowScale([[maybe_unused]] GLFWwindow* window)
{
#ifdef __APPLE__
    // macOS reports the Retina factor as the content scale, which Dear ImGui applies through the framebuffer scale.
    return 1.0f;
#else
    float scaleX = 1, scaleY = 1;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    return std::max(1.0f, scaleX);
#endif
}
} // namespace

namespace kit
{
ImTextureID Picture(const std::string& key, float size)
{
    return PictureTexture(key, size);
}

ImU32 PictureTint(const std::string& key)
{
    const auto& pictures = Pictures();
    const auto found = pictures.lower_bound({ key, 0 });
    return found != pictures.end() && found->first.first == key ? found->second.tint : 0;
}
} // namespace kit

bool InitUi(UiWindow& ui, std::string& error)
{
    ui.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Tab and the arrows move through the controls, and Escape closes popups, as in the menu on Windows.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ui.scale = WindowScale(ui.window);
    // A monitor with another scale, or a new scale in the system's settings. BeginUi applies it.
    glfwSetWindowUserPointer(ui.window, &ui);
    glfwSetWindowContentScaleCallback(ui.window, [](GLFWwindow* window, float, float) {
        static_cast<UiWindow*>(glfwGetWindowUserPointer(window))->rescale = true;
    });

    // Headings fall back to the regular font.
    const auto fallbacks = [](bool bold) {
        std::vector<kit::FontFile> fonts;
        for (auto& [path, index] : platform::UiFallbackFonts(bold))
            fonts.push_back({ std::move(path), index });
        return fonts;
    };
    kit::LoadFonts({ platform::UiFont(false) }, fallbacks(false), { platform::UiFont(true) }, fallbacks(true));

    if (!ImGui_ImplGlfw_InitForVulkan(ui.window, true))
    {
        error = "Could not start the window's input.";
        return false;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = gpu.apiVersion;
    info.Instance = gpu.instance;
    info.PhysicalDevice = gpu.physicalDevice;
    info.Device = gpu.device;
    info.QueueFamily = gpu.queueFamily;
    info.Queue = gpu.queue;
    info.DescriptorPoolSize = kDescriptorPoolSize;
    info.MinImageCount = ui.surface.minImageCount;
    info.ImageCount = std::max(ui.surface.imageCount, ui.surface.minImageCount);
    info.PipelineInfoMain.RenderPass = ui.surface.renderPass;
    if (!ImGui_ImplVulkan_Init(&info))
    {
        error = "Could not start drawing the interface.";
        return false;
    }
    ui.surface.recreated = false;
    return true;
}

void ShutdownUi(UiWindow& ui)
{
    if (!ui.context)
        return;
    ImGui::SetCurrentContext(ui.context);
    vkDeviceWaitIdle(gpu.device);
    for (auto& [key, picture] : pictureSets[ui.context])
        DestroyPicture(picture);
    pictureSets.erase(ui.context);
    kit::ForgetContext(ui.context);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(ui.context);
    ui.context = nullptr;
}

void SetWindowIcon([[maybe_unused]] GLFWwindow* window)
{
#ifndef __APPLE__
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(kLogoPng, static_cast<int>(kLogoPngSize), &width, &height, &channels, 4);
    if (!pixels)
        return;
    // A few sizes, for the title bar, the taskbar and the window switcher.
    std::vector<std::vector<uint8_t>> sizes;
    std::vector<GLFWimage> images;
    for (int size : { 32, 64, 256 })
    {
        sizes.push_back(Shrink(pixels, width, height, size));
        images.push_back({ size, size, sizes.back().data() });
    }
    stbi_image_free(pixels);
    glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
#endif
}

void BeginUi(UiWindow& ui)
{
    ImGui::SetCurrentContext(ui.context);
    // Every frame sizes its text and its layout by the scale, so a new one needs nothing else.
    if (std::exchange(ui.rescale, false))
        ui.scale = WindowScale(ui.window);
    if (ui.surface.recreated)
    {
        ImGui_ImplVulkan_SetMinImageCount(ui.surface.minImageCount);
        ui.surface.recreated = false;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void EndUi(UiWindow& ui)
{
    ImGui::Render();
    ui.surface.BeginRenderPass();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ui.surface.commands);
    ui.surface.EndFrame();
}

void DrawLauncher(App& app)
{
    // Logos saved since, such as the icon of a game that just started, show now.
    if (glfwGetTime() - launcherState.picturesChecked > 2)
    {
        launcherState.picturesChecked = glfwGetTime();
        ForgetPictures([](const std::string&) { return true; });
    }
    scale = app.launcher.scale;
    TakeShortcut(app);
    Host host(app);
    launcher::Draw(DescribeLauncher(app), host, app.launcher.scale);
}

void ResetMenu(App& app)
{
    StopRecording(app);
    app.compareButton = false;
    menu.active.clear();
    // A switch still waiting for an answer about unsaved changes is dropped.
    menu.pendingPreset.clear();
    menu.openUnsavedPopup = false;
}

void DrawOverlay(App& app)
{
    app.menuActiveUniform = nullptr;
    app.menuHoveredUniform = nullptr;
    // The start hint, the only message with a key, has done its job once the menu opens.
    if (app.menuOpen && !app.toastKey.empty())
        app.toastUntil = 0;
    // The menu follows the size of the game's window, as on Windows, counted in the monitor's own units.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    scale = menu_layout::Scale(display.x / app.overlay.scale, display.y / app.overlay.scale) * app.overlay.scale;
    if (scale <= 0)
        return;
    ApplyStyle(ImGui::GetStyle());
    PushSize(14.5f);
    if (app.menuOpen)
        DrawMenuFrame(app);
    DrawToast(app);
    ImGui::PopFont();
}
