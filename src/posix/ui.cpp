// The launcher, in its window and over the game as the menu. It is the one every platform shares, in src/ui. This fills
// in what it shows and carries out what the user does, and draws the short messages at the bottom of the game.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"
#ifdef _WIN32
#include "discord.h"
#endif
#include "log.h"
#include "image.h"
#include "menu_layout.h"
#include "theme.h"
#include "ui/kit.h"
#include "ui/launcher_ui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

// assets/Unishade.png, which the build puts into the program.
extern const unsigned char kLogoPng[];
extern const unsigned kLogoPngSize;

namespace
{
using namespace kit;

// How long a removed game can be put back.
constexpr double kUndoSeconds = 8;
// The presets folder is read this often while the launcher shows.
constexpr double kPresetsInterval = 2;

// Reads the presets folder and Unishade.ini again for the launcher's next frame, after the user changed them.
void RefreshLauncher();

// Help is given on the Unishade Discord server.
constexpr char kHelpUrl[] = "https://discord.gg/wVbVUdENas";

// A picture drawn in a window, made at the size it is drawn. Each takes a set from Dear ImGui's descriptor pool,
// which also holds its fonts, so a window has at most kMaxPictures.
constexpr size_t kMaxPictures = 192;
constexpr uint32_t kDescriptorPoolSize = kMaxPictures + 64;
constexpr char kLogoPicture[] = "logo";

struct DecodedPicture
{
    image::Memory memory;
    std::vector<uint8_t> rgba;
    ImU32 tint = 0;
};

struct GpuPicture
{
    GpuImage image;
    GpuBuffer upload; // kept until this window's frame fence completes
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::future<DecodedPicture> decoding;
    bool requested = false;
    bool used = false; // drawn since ForgetPictures last looked
    ImU32 tint = 0;    // the average color of the pixels that show
};

// The logo, and folders' logos, of each window's Dear ImGui context, by folder and size in pixels: the launcher draws
// a game's icon at more than one size. A folder without a logo keeps an empty entry until it is looked for again.
struct PictureSet
{
    std::map<std::pair<std::string, int>, GpuPicture> pictures;
    VkCommandBuffer commands = VK_NULL_HANDLE;
};
std::map<ImGuiContext*, PictureSet> pictureSets;

std::map<std::pair<std::string, int>, GpuPicture>& Pictures()
{
    return pictureSets[ImGui::GetCurrentContext()].pictures;
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
    gpu.DestroyBuffer(picture.upload);
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
    const auto entry = pictures.try_emplace({ key, size }).first;
    GpuPicture& picture = entry->second;
    picture.used = true;
    if (picture.set || size <= 0 || size > 1024)
        return (ImTextureID)picture.set;
    if (std::count_if(pictures.begin(), pictures.end(), [](const auto& other) { return other.second.set != VK_NULL_HANDLE; }) >=
        std::ptrdiff_t(kMaxPictures))
        return ImTextureID_Invalid;
    if (!picture.requested)
    {
        size_t pending = 0;
        for (const auto& [context, set] : pictureSets)
            for (const auto& [name, other] : set.pictures)
                if (other.decoding.valid() && other.decoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    ++pending;
        if (pending >= 2)
            return ImTextureID_Invalid;
        picture.requested = true;
        try
        {
            picture.decoding = std::async(std::launch::async, [key, size] {
                DecodedPicture result;
                image::Pixels pixels = key == kLogoPicture
                    ? image::Decode(std::string_view(reinterpret_cast<const char*>(kLogoPng), kLogoPngSize))
                    : image::Read(fs::path(key) / "logo.png");
                if (!pixels.data || !result.memory.Resize(size_t(size) * size * 4))
                    return result;
                result.rgba = Shrink(static_cast<const uint8_t*>(pixels.data.get()), pixels.width, pixels.height, size);
                uint64_t sums[3] = {}, weight = 0;
                for (size_t i = 0; i < result.rgba.size(); i += 4)
                {
                    for (int c = 0; c < 3; ++c)
                        sums[c] += uint64_t(result.rgba[i + c]) * result.rgba[i + 3];
                    weight += result.rgba[i + 3];
                }
                if (weight)
                    result.tint = IM_COL32(sums[0] / weight, sums[1] / weight, sums[2] / weight, 255);
                return result;
            });
        }
        catch (const std::exception&)
        {
            return ImTextureID_Invalid;
        }
    }
    if (!picture.decoding.valid() || picture.decoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return ImTextureID_Invalid;
    DecodedPicture decoded;
    try
    {
        decoded = picture.decoding.get();
    }
    catch (const std::exception&)
    {
        return ImTextureID_Invalid;
    }
    const VkCommandBuffer commands = pictureSets[ImGui::GetCurrentContext()].commands;
    if (decoded.rgba.empty() || !commands)
        return ImTextureID_Invalid;
    picture.tint = decoded.tint;
    if (gpu.CreateImage(picture.image, size, size, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        gpu.CreateBuffer(picture.upload, decoded.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
    {
        std::memcpy(picture.upload.mapped, decoded.rgba.data(), decoded.rgba.size());
        InitLayout(commands, picture.image);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { uint32_t(size), uint32_t(size), 1 };
        vkCmdCopyBufferToImage(commands, picture.upload.buffer, picture.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        FullBarrier(commands);
        picture.set = ImGui_ImplVulkan_AddTexture(picture.image.view, VK_IMAGE_LAYOUT_GENERAL);
    }
    else
    {
        gpu.DestroyBuffer(picture.upload);
        gpu.DestroyImage(picture.image);
    }
    return (ImTextureID)picture.set;
}

// Drops the pictures not drawn since the last call, the folder logos that are not kept, and the folders found without
// one, so a logo saved since, such as the icon of a game that just started, shows the next time it is drawn.
void ForgetPictures(const std::function<bool(const std::string& folder)>& keep)
{
    auto& pictures = Pictures();
    for (auto entry = pictures.begin(); entry != pictures.end();)
    {
        const std::string& folder = entry->first.first;
        GpuPicture& picture = entry->second;
        const bool used = std::exchange(picture.used, false);
        const bool pending = picture.decoding.valid() && picture.decoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        if (picture.upload.buffer || pending || (used && picture.decoding.valid()) ||
            (used && picture.set && (folder == kLogoPicture || keep(folder))))
        {
            ++entry;
            continue;
        }
        DestroyPicture(entry->second);
        entry = pictures.erase(entry);
    }
}


// Settings lists the shortcuts in the order of kShortcuts.
constexpr const char* kShortcutDescriptions[] = {
    "Press it again, or Escape, to go back to the game.",
    "Shows the game without effects and stops capturing it.",
    "Shows the game without effects for as long as you hold it.",
    "Saves what you see, without the menu.",
    "Saves the same moment with and without effects.",
    "Switches to the next preset in the game's list.",
    "Switches to the preset before it.",
};
static_assert(std::size(kShortcutDescriptions) == std::size(kShortcuts));

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
    std::string presetProblem;
    // The model is being drawn over the game rather than in the launcher's window.
    bool overGame = false;
    // The effects the preset uses that are not installed, and what they were looked for with.
    std::vector<std::string> missing;
    std::tuple<fs::path, size_t, bool, bool> missingChecked;
    // The effects as last listed, each with its technique's index, made again when the techniques change, and the
    // effect files that did not compile. DescribeLauncher lends them to the model.
    std::vector<launcher::Effect> effects;
    std::vector<size_t> effectTechniques;
    unsigned effectsRevision = 0;
    std::vector<launcher::BrokenEffect> broken;
    size_t brokenFrom = SIZE_MAX; // the number of effect files they were found among
    // The settings described last, for the effect at settingsEffect, with the variable behind each.
    launcher::EffectSettings settings;
    size_t settingsEffect = SIZE_MAX;
    std::vector<const fx::Uniform*> settingUniforms;
    int recording = -1; // the shortcut waiting for its keys
    std::string shortcutError;
    // Screen recording was asked for, so a restart can make it count.
    bool permissionRequested = false;
    // Whether the picked window is still open, asked now and then.
    bool selectedOpen = true;
    double selectedChecked = -10;
    double picturesChecked = 0;
    // What the launcher shows of the saved games comes from the presets folder and Unishade.ini, too slow to read on
    // every frame. They are read again every kPresetsInterval and right after the user changes them. Each folder's
    // presets by path, and each game's folder by name.
    std::map<std::string, std::vector<fs::path>> presets;
    std::map<std::string, fs::path> folders;
    double presetsRead = -10;
    // The games as last shown, made again when what they were made from changes: the saved games, the game being
    // played and its preset, or what was read from disk. DrawLauncher lends them to the model.
    std::vector<launcher::Game> games;
    launcher::Performance defaults; // read with the games
    std::string gamesPlaying;
    fs::path gamesInUse;
    bool gamesStale = true;
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
        if (entry.is_directory(error) && !strcasecmp(entry.path().filename().string().c_str(), folder.c_str()))
            return entry.path();
    return root / folder;
}

// GameFolder, read once until the launcher reads the disk again.
const fs::path& CachedGameFolder(const std::string& folder)
{
    auto [entry, added] = launcherState.folders.try_emplace(folder);
    if (added)
        entry->second = GameFolder(folder);
    return entry->second;
}

void RefreshLauncher()
{
    launcherState.presets.clear();
    launcherState.folders.clear();
    launcherState.gamesStale = true;
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

// While a shortcut waits for its keys, the next key pressed in the window with any modifiers becomes it.
void TakeShortcut(App& app, GLFWwindow* window)
{
    if (launcherState.recording < 0)
        return;
    // Space would otherwise also press the control the keyboard is on.
    ImGui::SetNavCursorVisible(false);
    // Modifiers straight from GLFW: Dear ImGui swaps Cmd and Ctrl on macOS.
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
        if (currentMoves && app.runtime.Dirty() && app.settings.autoSavePresets)
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
    RenamePerformance(key, FolderName(name));
    if (playing)
        app.game = FolderName(name);
    // The preset still loaded after the game closed moves too, or a save would make the old folder again.
    if (moveFolder && currentMoves)
    {
        if (app.runtime.Dirty())
        {
            const std::string dropped = "Unsaved changes to " + current.stem().string() + " were dropped, since its game was renamed";
            // The launcher is in front when the game is not running.
            if (playing)
                app.ShowToast(dropped);
            else
                Report(LogLevel::Warning, "%s.", dropped.c_str());
        }
        app.SwitchPreset(to / current.filename(), false, true);
    }
    RefreshLauncher();
    Log(LogLevel::Info, "Renamed %s to %s.", key.c_str(), name.c_str());
    return {};
}

// Once a removed game can no longer be put back, its own performance settings go, unless a game with its name was
// added since.
void ForgetRemoved(App& app)
{
    if (!launcherState.removed)
        return;
    const std::string key = FolderName(launcherState.removed->name);
    if (std::none_of(app.autoGames.begin(), app.autoGames.end(),
                     [&](const AutoGame& game) { return !strcasecmp(FolderName(game.name).c_str(), key.c_str()); }))
        RemovePerformance(key);
    launcherState.removed.reset();
}

// The launcher lists a window picked for now after the saved games, when it is not one of them.
bool Picked(App& app, size_t game)
{
    return game == app.autoGames.size() && app.active && app.game.empty();
}

bool Listed(App& app, size_t game)
{
    return game < app.autoGames.size() || Picked(app, game);
}

// Whether the game at an index of the launcher's list is the one being played.
bool Playing(App& app, size_t game)
{
    return Picked(app, game) ||
           (game < app.autoGames.size() && !app.game.empty() && !strcasecmp(FolderName(app.autoGames[game].name).c_str(), app.game.c_str()));
}

// The folder of a game's own presets, or the presets folder itself for a window picked for now.
fs::path FolderOf(App& app, size_t game)
{
    return game < app.autoGames.size() ? GameFolder(FolderName(app.autoGames[game].name)) : PresetsDirectory();
}

// The game is played with the preset from now on: at once for the game being played, and otherwise when it starts.
void UseGamePreset(App& app, size_t game, const fs::path& path)
{
    RefreshLauncher();
    if (!Playing(app, game))
        SetGamePreset(FolderName(app.autoGames[game].name), path);
    else if (!app.SwitchPreset(path, app.settings.autoSavePresets, false))
        Report(LogLevel::Warning, "Save or discard the changes to %s to switch to %s.", app.runtime.PresetPath().stem().string().c_str(),
               path.stem().string().c_str());
}

// Points the games that use a preset at where it went, or at nothing once it is gone.
void FollowPreset(App& app, const fs::path& from, const fs::path& to)
{
    for (const AutoGame& saved : app.autoGames)
        if (const std::string folder = FolderName(saved.name); !folder.empty() && GamePreset(folder) == from)
            SetGamePreset(folder, to);
    RefreshLauncher();
}

// Where a preset of that name goes in the game's own folder, which is made, or empty when there is one already.
fs::path NewGamePreset(App& app, size_t game, const std::string& name)
{
    const fs::path folder = FolderOf(app, game);
    const fs::path path = folder / (name + ".ini");
    std::error_code error;
    if (fs::exists(path, error))
    {
        launcherState.presetProblem = "A preset with that name already exists.";
        return {};
    }
    fs::create_directories(folder, error);
    return path;
}

// A preset name as typed, without spaces around it, or why it cannot be used.
bool PresetName(std::string_view typed, std::string& name, std::string& problem)
{
    name = std::string(typed);
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    problem = PresetNameProblem(name);
    return problem.empty();
}

// Downloads effects and presets, over what is installed. Effects load again once it finishes, which goes back to how
// the preset was saved, so unsaved changes have to be saved or discarded first.
void DownloadEffects(App& app)
{
    if (app.runtime.Dirty() && !app.settings.autoSavePresets)
        Report(LogLevel::Warning, "Save or discard the changes to %s in the menu before downloading effects.", app.runtime.PresetPath().stem().string().c_str());
    else
        app.setup.Start();
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
        RefreshLauncher();
    }
    else
        app.Select(window);
    platform::Activate(window);
}

// The effects of the game being played. The list is made again only when the techniques change, which is checked on
// every frame without allocating.
launcher::Effects DescribeEffects(App& app)
{
    const std::vector<fx::Technique>& techniques = app.runtime.Techniques();
    const std::vector<fx::Effect>& effects = app.runtime.Effects();
    std::vector<launcher::Effect>& list = launcherState.effects;
    std::vector<size_t>& indices = launcherState.effectTechniques;
    const auto name = [](const fx::Technique& technique) -> const std::string& { return technique.label.empty() ? technique.name : technique.label; };
    size_t listed = 0;
    bool same = true;
    for (size_t i = 0; i < techniques.size() && same; ++i)
    {
        const fx::Technique& technique = techniques[i];
        if (technique.hidden)
            continue;
        const fx::Effect& effect = effects[technique.effect];
        same = listed < list.size() && indices[listed] == i && list[listed].name == name(technique) && list[listed].file == effect.file &&
               list[listed].failed == effect.gpuFailed;
        ++listed;
    }
    if (!same || listed != list.size())
    {
        list.clear();
        indices.clear();
        for (size_t i = 0; i < techniques.size(); ++i)
        {
            const fx::Technique& technique = techniques[i];
            if (technique.hidden)
                continue;
            const fx::Effect& effect = effects[technique.effect];
            list.push_back({ fx::TechniqueKey(technique, effect), name(technique), effect.file, false, effect.gpuFailed });
            indices.push_back(i);
        }
        ++launcherState.effectsRevision;
        launcherState.settingsEffect = SIZE_MAX;
    }
    for (size_t i = 0; i < list.size(); ++i)
        list[i].on = techniques[indices[i]].enabled;
    if (launcherState.brokenFrom != effects.size())
    {
        launcherState.brokenFrom = effects.size();
        launcherState.broken.clear();
        for (const fx::Effect& effect : effects)
            if (!effect.compiled)
                launcherState.broken.push_back({ effect.file, effect.errors });
    }

    // Lent to the model, and given back once it is drawn.
    launcher::Effects described{ .list = std::move(list), .revision = launcherState.effectsRevision, .on = app.effectsEnabled, .loading = app.runtime.Loading() };
    std::tie(described.loaded, described.total) = app.runtime.LoadingProgress();
    described.broken = std::move(launcherState.broken);
    return described;
}

// One effect's settings with their values, as the launcher shows them. Each setting keeps the variable behind it.
const launcher::EffectSettings* SettingsOf(App& app, size_t index)
{
    if (index >= launcherState.effectTechniques.size())
        return nullptr;
    const fx::Technique& technique = app.runtime.Techniques()[launcherState.effectTechniques[index]];
    const fx::Effect& effect = app.runtime.Effects()[technique.effect];
    launcher::EffectSettings& settings = launcherState.settings;
    launcherState.settingsEffect = index;
    launcherState.settingUniforms.clear();
    settings = { .tooltip = technique.tooltip };
    using Control = launcher::EffectSetting::Control;
    for (const fx::Uniform& uniform : effect.uniforms)
    {
        const int components = static_cast<int>(uniform.type.components());
        if (uniform.hidden || uniform.type.is_array() || uniform.type.is_matrix() || components > 4)
            continue;
        const bool floating = uniform.type.is_floating_point();
        launcher::EffectSetting setting{ .floating = floating,
                                         .count = components,
                                         .label = uniform.label,
                                         .tooltip = uniform.tooltip,
                                         .category = uniform.category,
                                         .text = uniform.text,
                                         .items = uniform.items,
                                         .units = uniform.units,
                                         .spacing = uniform.spacing,
                                         .categoryClosed = uniform.categoryClosed,
                                         .resettable = !uniform.noReset,
                                         .range = uniform.min > std::numeric_limits<float>::lowest() && uniform.max < std::numeric_limits<float>::max(),
                                         .min = uniform.min,
                                         .max = uniform.max,
                                         .step = std::max(uniform.step, floating ? 0.0001f : 0.2f) };
        if (uniform.type.is_boolean())
            setting.control = Control::Check;
        else if (!uniform.items.empty() && !floating && components == 1)
            setting.control = Control::List;
        else if (uniform.uiType == "color" && floating && components >= 3)
            setting.control = Control::Color;
        else if (uniform.uiType == "slider" && setting.range)
            setting.control = Control::Slider;
        else if (uniform.uiType == "input")
            setting.control = Control::Input;
        if (floating)
            app.runtime.GetValue(effect, uniform, setting.floats, components);
        else
            app.runtime.GetValue(effect, uniform, setting.ints, components);
        settings.settings.push_back(std::move(setting));
        launcherState.settingUniforms.push_back(&uniform);
    }
    for (const auto& [name, compiled] : effect.definitions)
        settings.definitions.push_back({ name, app.runtime.DefinitionValue(effect.file, name), compiled });
    return &settings;
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
        else if (which == launcher::Switch::KeepEffectsVisible)
        {
            app.settings.keepEffectsVisible = on;
            SaveSettings(app.settings);
            Log(LogLevel::Info, "Settings: keep_effects_visible=%d.", on);
        }
        else if (which == launcher::Switch::Effects)
            app.effectsEnabled = on;
        else if (which == launcher::Switch::SkipTitleBar)
        {
            app.settings.skipTitleBar = on;
            platform::SetSkipTitleBar(on);
            SaveSettings(app.settings);
            Log(LogLevel::Info, "Settings: skip_title_bar=%d.", on);
        }
        else if (which == launcher::Switch::DiscordPresence)
        {
            app.settings.discordPresence = on;
            SaveSettings(app.settings);
            Log(LogLevel::Info, "Settings: discord_presence=%d.", on);
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
        ForgetRemoved(app);
        launcherState.removed = app.autoGames[game];
        launcherState.removedIndex = game;
        launcherState.removedAt = glfwGetTime();
        app.autoGames.erase(app.autoGames.begin() + static_cast<std::ptrdiff_t>(game));
        app.SaveGames();
        RefreshLauncher();
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
        RefreshLauncher();
    }

    void UsePreset(size_t game, std::string_view preset) override
    {
        if (Listed(app, game))
            UseGamePreset(app, game, fs::path(std::string(preset)));
    }

    bool NewPreset(size_t game, std::string_view typed) override
    {
        std::string name;
        if (!Listed(app, game) || !PresetName(typed, name, launcherState.presetProblem))
            return false;
        RefreshLauncher();
        if (Playing(app, game))
            return app.NewPreset(name, false, launcherState.presetProblem);
        const fs::path path = NewGamePreset(app, game, name);
        if (path.empty())
            return false;
        if (!WriteFile(path, "Techniques=\n"))
        {
            launcherState.presetProblem = "Could not write the preset.";
            return false;
        }
        UseGamePreset(app, game, path);
        return true;
    }

    // Copies go to the game's own folder, and the game uses them.
    bool CopyPreset(size_t game, std::string_view preset, std::string_view typed) override
    {
        std::string name;
        if (!Listed(app, game) || !PresetName(typed, name, launcherState.presetProblem))
            return false;
        RefreshLauncher();
        const fs::path source{ std::string(preset) };
        // The preset on screen is copied as it is, unsaved changes included.
        if (Playing(app, game) && source == app.runtime.PresetPath())
            return app.NewPreset(name, true, launcherState.presetProblem);
        const fs::path path = NewGamePreset(app, game, name);
        if (path.empty())
            return false;
        std::error_code error;
        if (!fs::copy_file(source, path, error))
        {
            launcherState.presetProblem = "Could not write the preset.";
            return false;
        }
        UseGamePreset(app, game, path);
        return true;
    }

    bool RenamePreset(std::string_view preset, std::string_view typed) override
    {
        std::string name;
        if (!PresetName(typed, name, launcherState.presetProblem))
            return false;
        const fs::path source{ std::string(preset) };
        const fs::path path = source.parent_path() / (name + ".ini");
        std::error_code error;
        if (path == source)
            return true;
        // A name that only changes case is the same file on Windows and macOS.
        if (fs::exists(path, error) && strcasecmp(path.filename().string().c_str(), source.filename().string().c_str()))
        {
            launcherState.presetProblem = "A preset with that name already exists.";
            return false;
        }
        fs::rename(source, path, error);
        if (error)
        {
            launcherState.presetProblem = "Could not rename the preset.";
            return false;
        }
        FollowPreset(app, source, path);
        return true;
    }

    void MovePreset(std::string_view preset, std::string_view folder) override
    {
        const fs::path source{ std::string(preset) };
        std::string error;
        if (!app.MovePreset(source, fs::path(std::string(folder)), error))
            Report(LogLevel::Warning, "%s", error.c_str());
        else
            FollowPreset(app, source, fs::path(std::string(folder)) / source.filename());
    }

    void DeletePreset(std::string_view preset) override
    {
        const fs::path path{ std::string(preset) };
        if (!platform::MoveToTrash(path.string()))
            Report(LogLevel::Warning, "Could not delete %s.", path.stem().string().c_str());
        else
            FollowPreset(app, path, fs::path());
    }

    void SetEffect(size_t effect, bool on) override
    {
        if (effect < launcherState.effectTechniques.size())
            app.runtime.SetEnabled(launcherState.effectTechniques[effect], on);
    }

    void MoveEffect(size_t from, size_t to) override
    {
        if (from < launcherState.effectTechniques.size() && to < launcherState.effectTechniques.size())
            app.runtime.MoveTechnique(launcherState.effectTechniques[from], launcherState.effectTechniques[to]);
    }

    const launcher::EffectSettings* DescribeSettings(size_t effect) override
    {
        return SettingsOf(app, effect);
    }

    void SetSetting(size_t effect, size_t setting, const launcher::EffectSetting& value) override
    {
        if (const auto [target, uniform] = Setting(effect, setting); uniform)
        {
            if (value.floating)
                app.runtime.SetValue(*target, *uniform, value.floats, static_cast<size_t>(value.count));
            else
                app.runtime.SetValue(*target, *uniform, value.ints, static_cast<size_t>(value.count));
            app.runtime.SetDirty();
        }
    }

    void ResetSettings(size_t effect, std::optional<size_t> setting) override
    {
        if (setting)
        {
            if (const auto [target, uniform] = Setting(effect, *setting); uniform)
                app.runtime.ResetValue(*target, *uniform);
        }
        else if (fx::Effect* target = EffectOf(effect))
            for (const fx::Uniform& uniform : target->uniforms)
                if (uniform.source.empty() && !uniform.noReset)
                    app.runtime.ResetValue(*target, uniform);
        app.runtime.SetDirty();
    }

    void SetDefinition(size_t effect, size_t definition, std::string_view value) override
    {
        if (const fx::Effect* target = EffectOf(effect); target && effect == launcherState.settingsEffect &&
                                                         definition < launcherState.settings.definitions.size())
            app.runtime.SetDefinition(target->file, launcherState.settings.definitions[definition].name, std::string(value));
    }

    void PointAtSettings(size_t effect, std::optional<size_t> active, std::optional<size_t> hovered) override
    {
        // Effects only read them while the menu is open.
        if (!launcherState.overGame)
            return;
        app.menuActiveUniform = active ? Setting(effect, *active).second : nullptr;
        app.menuHoveredUniform = hovered ? Setting(effect, *hovered).second : nullptr;
    }

    void Compare(bool holding) override
    {
        app.compareButton = holding;
    }

    void TakeScreenshot(bool beforeAfter) override
    {
        app.RequestScreenshot(beforeAfter);
    }

    void OpenScreenshots() override
    {
        const fs::path folder = ScreenshotDirectory();
        std::error_code error;
        fs::create_directories(folder, error);
        platform::Open(folder.string());
    }

    void CloseMenu() override
    {
        app.CloseMenu();
    }

    void Quit(bool save) override
    {
        if (save)
            app.runtime.SavePreset();
        app.quitAsked = false;
        glfwSetWindowShouldClose(app.launcher.window, GLFW_TRUE);
    }

    void KeepRunning() override
    {
        app.quitAsked = false;
    }

    void SaveChanges() override
    {
        app.runtime.SavePreset();
    }

    void DiscardChanges() override
    {
        app.SwitchPreset(app.runtime.PresetPath(), false, true);
    }

    void OpenPresetsFolder(size_t game) override
    {
        if (!Listed(app, game))
            return;
        const fs::path folder = FolderOf(app, game);
        std::error_code error;
        fs::create_directories(folder, error);
        platform::Open(folder.string());
        RefreshLauncher();
    }

    // A window picked for now uses and changes the settings for every game, as does Settings.
    void SetPerformance(std::optional<size_t> game, launcher::PerformanceSetting setting, int value) override
    {
        if (game && !Listed(app, *game))
            return;
        const std::string folder = game && *game < app.autoGames.size() ? FolderName(app.autoGames[*game].name) : std::string();
        if (setting == launcher::PerformanceSetting::FrameRate)
        {
            SetFrameRateLimit(folder, value);
            app.UpdateFrameRateLimit();
        }
#ifdef _WIN32
        else if (setting == launcher::PerformanceSetting::EffectResolution)
        {
            SetEffectResolution(folder, value);
            app.UpdateEffectResolution();
        }
#endif
        else
            return;
        RefreshLauncher();
    }
    void StepMenuSize(int step) override
    {
        app.settings.menuScale = std::clamp(app.settings.menuScale + 0.25f * static_cast<float>(step), kSmallestMenuScale, kLargestMenuScale);
        SaveSettings(app.settings);
    }

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

    void ClearShortcut(size_t shortcut) override
    {
        StopShortcut(app);
        if (shortcut >= std::size(kShortcuts) || kShortcuts[shortcut].member == &InputHotkeys::input)
            return;
        InputHotkeys hotkeys = app.settings.hotkeys;
        hotkeys.*kShortcuts[shortcut].member = {};
        launcherState.shortcutError.clear();
        app.ChangeHotkeys(hotkeys, launcherState.shortcutError);
    }

    void ResetShortcuts() override
    {
        InputHotkeys hotkeys;
        for (const Shortcut& shortcut : kShortcuts)
            ParseHotkey(shortcut.fallback, hotkeys.*shortcut.member);
        launcherState.shortcutError.clear();
        app.ChangeHotkeys(hotkeys, launcherState.shortcutError);
    }

    void DownloadEffects() override
    {
        ::DownloadEffects(app);
    }

    void ReloadEffects() override
    {
        app.runtime.Reload();
    }

private:
    fx::Effect* EffectOf(size_t effect)
    {
        if (effect >= launcherState.effectTechniques.size())
            return nullptr;
        return &app.runtime.Effects()[app.runtime.Techniques()[launcherState.effectTechniques[effect]].effect];
    }

    // A setting the last description listed, with its effect.
    std::pair<fx::Effect*, const fx::Uniform*> Setting(size_t effect, size_t setting)
    {
        if (effect != launcherState.settingsEffect || setting >= launcherState.settingUniforms.size())
            return {};
        return { EffectOf(effect), launcherState.settingUniforms[setting] };
    }

    App& app;
};

launcher::Status DescribeStatus(App& app)
{
    launcher::Status status;
    const std::optional<platform::Window>& game = app.active ? app.active : app.selected;
    status.game = game ? game->title : std::string();
    status.picture = app.active && !app.game.empty() ? CachedGameFolder(app.game).string() : std::string();
    status.warning = app.lastCaptureError;
    status.effectsShown = app.captureEnabled;
    bool windowClosed = false;
    if (!app.captureEnabled)
    {
        status.tone = launcher::Tone::Off;
        status.title = "Effects off";
        const std::string key = app.HotkeyText(kOverlayToggleHotkey);
        status.detail = key.empty() ? "Turn them back on below." : "Turn them back on below, or press " + key + ".";
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
    {
        const auto request = [] {
            platform::RequestCapturePermission();
            launcherState.permissionRequested = true;
        };
        // macOS counts the permission only once Unishade starts again.
        const auto restart = [&app] {
            if (platform::Relaunch())
                glfwSetWindowShouldClose(app.launcher.window, GLFW_TRUE);
            else
                Report(LogLevel::Warning, "Could not start Unishade again. Quit it and open it again.");
        };
        launcher::Card permission{ .title = "Unishade needs to record the screen",
                                   .text = "macOS asks before a program can copy another program's window. Allow Unishade under Privacy & Security, "
                                           "Screen & System Audio Recording, then restart Unishade.",
                                   .attention = true,
                                   .buttons = { "Allow screen recording" } };
        if (launcherState.permissionRequested)
        {
            permission.buttons = { "Restart Unishade", "Allow screen recording" };
            card(std::move(permission), { restart, request });
        }
        else
            card(std::move(permission), { request });
    }
    if (app.runtime.Loading())
    {
        const auto [loaded, total] = app.runtime.LoadingProgress();
        card({ .title = "Compiling effects",
               .text = std::to_string(loaded) + " of " + std::to_string(total),
               .progress = total ? float(loaded) / float(total) : 0.0f });
    }
    const EffectSetup::State setup = app.setup.Read();
    model.settings.downloadEffects = !setup.running;
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
        // Packages that were skipped, such as ones that timed out, can be downloaded again.
        if (!setup.error.empty() || !setup.notes.empty())
            finished.buttons = { "Try again" };
        card(std::move(finished), { [&app] { DownloadEffects(app); } });
    }
    else if (!app.EffectsInstalled())
        card({ .title = "Effects are not installed yet",
               .text = "Unishade runs ReShade's effects. Download every package from ReShade's official list and the presets made for Unishade into "
                       "the data folder.",
               .attention = true,
               .buttons = { "Download effects and presets" } },
             { [&app] { DownloadEffects(app); } });
}

// A saved game's performance settings by its folder's name, or the defaults for every game with an empty one.
launcher::Performance PerformanceOf(const std::string& folder)
{
    launcher::Performance performance{ .frameRate = FrameRateLimit(folder) };
#ifdef _WIN32
    performance.effectResolution = EffectResolution(folder);
#endif
    return performance;
}

// The effect files the preset uses that are not installed, once every effect has loaded. Looked for again when the
// preset, its changes or the effects change, since that compares every effect's name.
std::vector<std::string> MissingEffects(App& app)
{
    std::vector<std::string>& files = launcherState.missing;
    const std::tuple<fs::path, size_t, bool, bool> now{ app.runtime.PresetPath(), app.runtime.Effects().size(), app.runtime.Loading(), app.runtime.Dirty() };
    if (now == launcherState.missingChecked)
        return files;
    launcherState.missingChecked = now;
    files.clear();
    if (app.runtime.Loading())
        return files;
    for (const std::string& key : app.runtime.MissingTechniques())
    {
        const size_t at = key.find('@');
        std::string file = at == std::string::npos ? key : key.substr(at + 1);
        if (std::find(files.begin(), files.end(), file) == files.end())
            files.push_back(std::move(file));
    }
    return files;
}

launcher::Model DescribeLauncher(App& app)
{
    launcher::Model model;
    model.version = UNISHADE_VERSION;
    model.status = DescribeStatus(app);
    DescribeCards(app, model);

    if (glfwGetTime() - launcherState.presetsRead > kPresetsInterval)
    {
        RefreshLauncher();
        launcherState.presetsRead = glfwGetTime();
    }
    const auto running = [&app](const AutoGame& saved) {
        return saved.enabled && app.active && MatchesProcess(saved, app.activeExecutable, app.activeCommand);
    };
    std::vector<launcher::Game>& games = launcherState.games;
    bool same = !launcherState.gamesStale && games.size() == app.autoGames.size() && launcherState.gamesPlaying == app.game &&
                launcherState.gamesInUse == app.runtime.PresetPath();
    for (size_t i = 0; same && i < games.size(); ++i)
    {
        const AutoGame& saved = app.autoGames[i];
        same = games[i].id == saved.executable && games[i].name == saved.name && games[i].enabled == saved.enabled && games[i].running == running(saved);
    }
    if (!same)
    {
        games.clear();
        launcherState.gamesStale = false;
        launcherState.gamesPlaying = app.game;
        launcherState.gamesInUse = app.runtime.PresetPath();
        launcherState.defaults = PerformanceOf({});
        const fs::path root = PresetsDirectory();
        for (const AutoGame& saved : app.autoGames)
        {
            const std::string folder = FolderName(saved.name);
            const fs::path path = folder.empty() ? fs::path() : CachedGameFolder(folder);
            launcher::Game game{ .id = saved.executable,
                                 .name = saved.name,
                                 .executable = saved.executable,
                                 .picture = path.string(),
                                 .enabled = saved.enabled,
                                 .running = running(saved),
                                 .performance = PerformanceOf(folder) };
            const bool playing = !folder.empty() && !strcasecmp(app.game.c_str(), folder.c_str());
            const fs::path inUse = playing ? app.runtime.PresetPath() : folder.empty() ? fs::path() : GamePreset(folder);
            const auto add = [&](const fs::path& preset, const std::string& from) {
                game.presets.push_back({ preset.string(), preset.stem().string(), from, preset == app.runtime.PresetPath() });
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
            games.push_back(std::move(game));
        }
    }
    // Lent to the model, and given back once it is drawn.
    model.games = std::move(games);
    if (Picked(app, app.autoGames.size()))
    {
        // Its own presets are the ones for all games.
        launcher::Game picked{ .id = "picked",
                               .name = app.active->title,
                               .executable = fs::path(app.activeExecutable).filename().string(),
                               .running = true,
                               .picked = true,
                               .performance = launcherState.defaults };
        const fs::path& inUse = app.runtime.PresetPath();
        for (const fs::path& preset : PresetsOf(PresetsDirectory()))
            picked.presets.push_back({ preset.string(), preset.stem().string(), "All games", preset == inUse });
        if (inUse.parent_path() != PresetsDirectory())
            picked.presets.push_back({ inUse.string(), inUse.stem().string(), inUse.parent_path().filename().string(), true });
        picked.preset = inUse.string();
        model.games.push_back(std::move(picked));
    }
    for (size_t i = 0; i < model.games.size(); ++i)
    {
        const bool playing = Playing(app, i);
        model.games[i].changed = playing && app.runtime.Dirty() && !app.settings.autoSavePresets;
        model.games[i].missing = playing ? MissingEffects(app) : std::vector<std::string>();
    }
    model.editPresets = true;
    model.folders.push_back({ PresetsDirectory().string(), "All games" });
    for (const AutoGame& saved : app.autoGames)
        if (const std::string folder = FolderName(saved.name); !folder.empty())
            model.folders.push_back({ CachedGameFolder(folder).string(), saved.name });
    model.presetProblem = launcherState.presetProblem;
    if (app.active)
        model.effects = DescribeEffects(app);
    if (launcherState.removed && glfwGetTime() - launcherState.removedAt > kUndoSeconds)
        ForgetRemoved(app);
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

#ifdef _WIN32
    launcher::Discord discord{ .on = app.settings.discordPresence };
    if (!app.active)
        discord.status = "Shows up once Unishade is running on a game.";
    else
        switch (const DiscordStatus status = CurrentDiscordStatus(); status.state)
        {
        case DiscordState::Idle:
        case DiscordState::Connecting: discord.status = "Connecting to Discord..."; break;
        case DiscordState::Closed: discord.status = "Waiting for Discord to open."; break;
        case DiscordState::Showing:
            discord.status = "Showing " + app.active->title + " on Discord.";
            discord.level = launcher::Level::Ok;
            break;
        case DiscordState::Refused:
            discord.status = "Discord refused it: " + status.error;
            discord.level = launcher::Level::Warning;
            break;
        }
    model.discord = std::move(discord);
#endif

    model.settings.autoSavePresets = app.settings.autoSavePresets;
    if (platform::OverlayCanStayBehind())
        model.settings.keepEffectsVisible = app.settings.keepEffectsVisible;
    model.settings.menuSize = static_cast<int>(std::lround(app.settings.menuScale * 100));
    model.settings.canShrinkMenu = app.settings.menuScale > kSmallestMenuScale + 0.01f;
    model.settings.canGrowMenu = app.settings.menuScale < kLargestMenuScale - 0.01f;
    model.settings.reloadEffects = true;
    model.settings.performance = launcherState.defaults;
#ifdef __APPLE__
    model.settings.skipTitleBar = app.settings.skipTitleBar;
#endif
    for (size_t i = 0; i < std::size(kShortcuts); ++i)
        model.settings.shortcuts.push_back({ kShortcuts[i].label, kShortcutDescriptions[i], FormatHotkey(app.settings.hotkeys.*kShortcuts[i].member),
                                             kShortcuts[i].member == &InputHotkeys::input });
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
    if (app.quitAsked && app.runtime.Dirty())
        model.quitting = app.runtime.PresetPath().stem().string();
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
#ifdef NDEBUG
    // Dear ImGui outlines controls that share an ID in red, with a message for programmers, which users took for a
    // border Unishade draws. Debug builds keep it.
    io.ConfigDebugHighlightIdConflicts = false;
#endif

    ui.scale = WindowScale(ui.window);
    // A monitor with another scale, or a new scale in the system's settings. BeginUi applies it.
    glfwSetWindowUserPointer(ui.window, &ui);
    glfwSetWindowContentScaleCallback(ui.window, [](GLFWwindow* window, float, float) {
        static_cast<UiWindow*>(glfwGetWindowUserPointer(window))->rescale = true;
    });
    // A shortcut waiting for its keys in the launcher lets go of every shortcut, so it stops waiting once the user
    // goes elsewhere, such as to the game. The launcher may not be drawn again before then, as when it is minimized.
    // Set before Dear ImGui's, which calls it.
    glfwSetWindowFocusCallback(ui.window, [](GLFWwindow* window, int focused) {
        if (!focused && window == app.launcher.window)
            StopShortcut(app);
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
    for (auto& [key, picture] : pictureSets[ui.context].pictures)
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
    PictureSet& pictures = pictureSets[ui.context];
    pictures.commands = ui.surface.commands;
    // BeginFrame completed this window's previous submission before its upload buffers are released.
    for (auto& [key, picture] : pictures.pictures)
        gpu.DestroyBuffer(picture.upload);
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
    pictureSets[ui.context].commands = VK_NULL_HANDLE;
}

// What the model was lent, once it is drawn.
void GiveBack(launcher::Model& model)
{
    if (!model.games.empty() && model.games.back().picked)
        model.games.pop_back();
    launcherState.games = std::move(model.games);
    if (model.effects)
    {
        launcherState.effects = std::move(model.effects->list);
        launcherState.broken = std::move(model.effects->broken);
    }
}

void DrawLauncher(App& app)
{
    // Logos saved since, such as the icon of a game that just started, show now.
    if (glfwGetTime() - launcherState.picturesChecked > 2)
    {
        launcherState.picturesChecked = glfwGetTime();
        ForgetPictures([](const std::string&) { return true; });
    }
    TakeShortcut(app, app.launcher.window);
    Host host(app);
    launcher::Model model = DescribeLauncher(app);
    launcher::Draw(model, host, app.launcher.scale);
    GiveBack(model);
}

void ResetMenu(App& app)
{
    StopShortcut(app);
    app.compareButton = false;
    launcher::Reset(launcher::Place::Game);
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
    const float menuScale = menu_layout::Scale(display.x / app.overlay.scale, display.y / app.overlay.scale, app.settings.menuScale) * app.overlay.scale;
    if (menuScale <= 0)
        return;
    if (app.menuOpen)
    {
        TakeShortcut(app, app.overlay.window);
        Host host(app);
        launcher::Model model = DescribeLauncher(app);
        model.menuKey = app.HotkeyText(kEditModeHotkey);
        // How to open the menu says nothing in it.
        if (app.active)
            model.status.detail.clear();
        launcherState.overGame = true;
        launcher::Draw(model, host, menuScale, launcher::Place::Game);
        launcherState.overGame = false;
        GiveBack(model);
    }
    scale = menuScale;
    ApplyStyle(ImGui::GetStyle());
    PushSize(14.5f);
    DrawToast(app);
    ImGui::PopFont();
}
