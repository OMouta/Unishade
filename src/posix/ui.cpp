// The launcher, in its window and over the game as the menu. It is the one every platform shares, in src/ui. This fills
// in what it shows and carries out what the user does, and draws the short messages at the bottom of the game.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"
#ifdef _WIN32
#include "discord.h"
#include "update.h"
#endif
#ifdef __APPLE__
#include "emoji_macos.h"
#endif
#include "backdrop.h"
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
#include <atomic>
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
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

// assets/Unishade.png, which the build puts into the program.
extern const unsigned char kLogoPng[];
extern const unsigned kLogoPngSize;
// assets/fonts, which the build puts into the program too.
extern const unsigned char kBricolageRegular[];
extern const unsigned kBricolageRegularSize;
extern const unsigned char kBricolageSemiBold[];
extern const unsigned kBricolageSemiBoldSize;
extern const unsigned char kBricolageExtraBold[];
extern const unsigned kBricolageExtraBoldSize;

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

// The game's picture for the interface, which the overlay's frames make with backdrop: frost behind the tray, the
// preview on the launcher's page and the last screenshot. Each is a texture in the one context that asks for it.
struct GamePicture
{
    backdrop::Target target;
    unsigned made = 0;  // goes up whenever the image is made again
    bool ready = false; // drawn into since it was made
    platform::WindowId window = 0; // the game's window it shows
    double asked = -10;
    ImGuiContext* context = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    unsigned setMade = 0;
};
GamePicture frost;
GamePicture preview;
GamePicture shot;
backdrop::Target frostScratch; // the blur's way between its two passes
double nextPreview = 0;
// The launcher refreshes its preview this often, at most.
constexpr double kPreviewInterval = 1.0 / 30;
// The preview at most this wide, and a screenshot's thumbnail half the picture's size.
constexpr uint32_t kPreviewWidth = 1280;
// CSS's backdrop-filter of the design's tray: blur(30px) saturate(1.7).
constexpr float kFrostBlur = 30;
constexpr float kFrostSaturation = 1.7f;

ImTextureID GameTexture(GamePicture& picture, ImVec2* size)
{
    picture.asked = glfwGetTime();
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!picture.ready || !app.active || app.active->id != picture.window || (picture.context && picture.context != context))
        return ImTextureID_Invalid;
    // The old set was last drawn by this context's previous frame, which is done.
    if (picture.set && picture.setMade != picture.made)
    {
        ImGui_ImplVulkan_RemoveTexture(picture.set);
        picture.set = VK_NULL_HANDLE;
    }
    if (!picture.set)
    {
        picture.set = ImGui_ImplVulkan_AddTexture(picture.target.image.view, VK_IMAGE_LAYOUT_GENERAL);
        picture.context = context;
        picture.setMade = picture.made;
    }
    if (size)
        *size = ImVec2(static_cast<float>(picture.target.image.width), static_cast<float>(picture.target.image.height));
    return (ImTextureID)picture.set;
}

// Gives a picture's image the size, making it again when it has another. shared is for one that another window's
// frame may still be reading.
bool Fit(VkCommandBuffer commands, GamePicture& picture, uint32_t width, uint32_t height, bool shared)
{
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (picture.target.framebuffer && picture.target.image.width == width && picture.target.image.height == height)
        return true;
    if (shared && picture.target.framebuffer)
        vkDeviceWaitIdle(gpu.device);
    picture.ready = false;
    ++picture.made;
    return backdrop::Make(commands, picture.target, width, height);
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
    // When this message came up, which the host does not keep.
    static double since = 0, until = 0;
    static std::string shownText;
    if (until != app.toastUntil || shownText != app.toast)
    {
        since = glfwGetTime();
        until = app.toastUntil;
        shownText = app.toast;
    }
    // A black pill that springs up over half a second and sinks away over the last 0.3, as the design's toast. It
    // stands at the top while the menu is open, whose tray opens at the bottom.
    const float in = Ease(Curve::Spring, std::clamp(static_cast<float>((glfwGetTime() - since) / 0.5), 0.0f, 1.0f));
    const float out = 1 - std::clamp(static_cast<float>(left / 0.3), 0.0f, 1.0f);
    const ImGuiIO& io = ImGui::GetIO();
    const bool top = app.menuOpen;
    const float height = S(44.8f), pad = S(20.8f), gap = S(9.6f);
    const float keyWidth = app.toastKey.empty() ? 0 : std::max(Measure(app.toastKey, spectrum::kTextSmall, true).x + S(19.2f), S(30.4f));
    const float room = std::max(io.DisplaySize.x * 0.8f - pad * 2 - (keyWidth > 0 ? keyWidth + gap : 0), S(120));
    const float textWidth = std::min(Measure(app.toast, spectrum::kTextBody, true).x, room);
    const ImVec2 size(pad * 2 + (keyWidth > 0 ? keyWidth + gap - S(8) : 0) + textWidth, height);
    const ImVec2 min(std::round((io.DisplaySize.x - size.x) / 2), top ? S(48) : io.DisplaySize.y - S(48) - height);
    const float middle = min.y + height / 2;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min + ImVec2(0, S(6.4f)), min + size + ImVec2(0, S(6.4f)), IM_COL32(0, 0, 0, 40), height / 2);
    draw->AddRectFilled(min, min + size, spectrum::kInk, height / 2);
    float x = min.x + pad;
    if (keyWidth > 0)
    {
        x -= S(8);
        const float keyHeight = S(28.8f);
        draw->AddRectFilled(ImVec2(x, middle - keyHeight / 2), ImVec2(x + keyWidth, middle + keyHeight / 2), IM_COL32(255, 255, 255, 46), S(9.6f));
        Line(draw, x, x + keyWidth, middle, app.toastKey, IM_COL32_WHITE, spectrum::kTextSmall, true, true);
        x += keyWidth + gap;
    }
    Line(draw, x, x + textWidth + 1, middle, app.toast, IM_COL32_WHITE, spectrum::kTextBody, true);
    const float away = S(24) * (1 - in) + S(9.6f) * out;
    Transform(draw, start, min + size * 0.5f, 0.8f + 0.2f * in, 0, ImVec2(0, top ? -away : away), std::min(in, 1.0f) * (1 - out));
}


// Launcher

// An official add-on installing on a thread of its own, which starts once the add-on let go of its files.
struct AddonInstall
{
    Addons::Official official;
    std::thread thread;
    std::atomic<bool> cancel = false;
    std::atomic<bool> done = false;
    std::atomic<float> progress = 0;
    std::string error; // written by the thread before done

    ~AddonInstall()
    {
        cancel = true;
        if (thread.joinable())
            thread.join();
    }
};

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
    // The file of each add-on the model lists, the installs under way, why the last install of each official add-on
    // failed, and whether each has all its files, which is read with the presets.
    std::vector<std::string> addonFiles;
    std::vector<std::unique_ptr<AddonInstall>> installs;
    std::map<std::string, std::string> installErrors;
    std::map<std::string, bool> addonsWhole;
    std::string addonsFolder; // its files as last seen, with their sizes and times
#ifdef _WIN32
    bool startsWithWindows = false; // read with the presets, since Task Manager can turn it off meanwhile
#endif
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
    launcherState.addonsWhole.clear();
    launcherState.gamesStale = true;
#ifdef _WIN32
    launcherState.startsWithWindows = platform::StartsWithWindows();
#endif
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
        else if (which == launcher::Switch::SmallTiles)
        {
            app.settings.smallTiles = on;
            SaveSettings(app.settings);
        }
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
#ifdef _WIN32
        else if (which == launcher::Switch::UpdateChecks)
        {
            app.settings.updateChecks = on;
            SaveSettings(app.settings);
            Log(LogLevel::Info, "Settings: update_checks=%d.", on);
        }
        else if (which == launcher::Switch::StartWithWindows)
        {
            platform::SetStartWithWindows(on);
            RefreshLauncher();
        }
#endif
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
        // The thumbnail waits for the frame that takes it, and stays away when none does.
        shot.ready = false;
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

    void SetMenuSize(int percent) override
    {
        app.settings.menuScale = std::clamp(static_cast<float>(percent) / 100, kSmallestMenuScale, kLargestMenuScale);
        SaveSettings(app.settings);
    }

    void PlaceMenu(float x, float y) override
    {
        app.settings.menuX = std::clamp(x, 0.0f, 1.0f);
        app.settings.menuY = std::clamp(y, 0.0f, 1.0f);
        SaveSettings(app.settings);
    }

    void SetMenuOpacity(float opacity) override
    {
        app.settings.menuOpacity = std::clamp(opacity, 0.4f, 1.0f);
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

    void InstallAddon(size_t addon) override
    {
        const Addons::Official* official = OfficialOf(addon);
        if (!official || std::any_of(launcherState.installs.begin(), launcherState.installs.end(),
                                     [&](const auto& install) { return install->official.name == official->name; }))
            return;
        Log(LogLevel::Info, "Installing the add-on %s.", official->name.c_str());
        launcherState.installs.push_back(std::make_unique<AddonInstall>());
        launcherState.installs.back()->official = *official;
        launcherState.installErrors.erase(official->name);
        // Windows does not let a library that is loaded be replaced.
        app.addons.Pause(official->files[0].path);
    }

    void CancelInstall(size_t addon) override
    {
        if (const Addons::Official* official = OfficialOf(addon))
            for (const auto& install : launcherState.installs)
                if (install->official.name == official->name)
                    install->cancel = true;
    }

    void LoadAddon(size_t addon, bool load) override
    {
        if (addon < launcherState.addonFiles.size())
            app.addons.Approve(launcherState.addonFiles[addon], load);
    }

    void SetAddon(size_t addon, bool on) override
    {
        if (addon < launcherState.addonFiles.size())
            app.addons.SetEnabled(launcherState.addonFiles[addon], on);
    }

    void BypassAddon(size_t addon, bool bypassed) override
    {
        if (addon < launcherState.addonFiles.size())
            app.addons.SetBypassed(launcherState.addonFiles[addon], bypassed);
    }

    void SetAddonSetting(size_t addon, size_t setting, const launcher::EffectSetting& value) override
    {
        if (addon < launcherState.addonFiles.size())
            app.addons.SetSetting(launcherState.addonFiles[addon], setting, value.floating ? value.floats[0] : static_cast<float>(value.ints[0]));
    }

    void OpenAddonsFolder() override
    {
        const fs::path folder = DataDirectory() / "Addons";
        std::error_code error;
        fs::create_directories(folder, error);
        platform::Open(folder.string());
    }

private:
    // The model lists the official add-ons first, in the official list's order.
    const Addons::Official* OfficialOf(size_t addon) const
    {
        const std::vector<Addons::Official>& list = app.addons.OfficialList();
        return addon < list.size() ? &list[addon] : nullptr;
    }

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
    // A new installation outside Windows has no effects, and without them nothing shows in a game. Messages has the
    // same download as a card, which few would look for there first.
    if (!app.setup.Read().running && !app.EffectsInstalled())
    {
        status.canDownload = true;
        if (!app.active && !app.selected && app.captureEnabled)
        {
            status.title = "Effects are not installed yet";
            status.detail = "Unishade runs ReShade's effects. Download them and the presets made for Unishade.";
        }
    }
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
#ifdef _WIN32
    if (Update update = AvailableUpdate(); !update.version.empty())
    {
        model.update = update.version;
        card({ .title = "Unishade " + update.version + " is available",
               .text = "Download the new Setup and run it. Your presets and settings stay.",
               .attention = true,
               .buttons = { "Download" } },
             { [url = std::move(update.url)] { platform::Open(url); } });
    }
#endif
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

// Starts each install once its add-on let go of its files, and finishes the ones that are done.
void FollowInstalls(App& app)
{
    std::vector<std::unique_ptr<AddonInstall>>& installs = launcherState.installs;
    for (auto it = installs.begin(); it != installs.end();)
    {
        AddonInstall& install = **it;
        const std::string& file = install.official.files[0].path;
        if (!install.thread.joinable() && !install.done && install.cancel)
            install.done = true;
        else if (!install.thread.joinable() && !install.done && !app.addons.Loaded(file))
            install.thread = std::thread([&install] {
                try
                {
                    // The add-on's own library is small next to the files it needs, so the bar follows those.
                    const size_t needed = install.official.files.size() - 1;
                    Addons::Install(install.official, install.cancel, [&](size_t index, uint64_t received, uint64_t total) {
                        const float part = total ? static_cast<float>(received) / static_cast<float>(total) : 0.0f;
                        install.progress = !needed ? part : index ? (static_cast<float>(index - 1) + part) / static_cast<float>(needed) : 0.0f;
                    });
                }
                catch (const std::exception& e)
                {
                    install.error = e.what();
                }
                // Cancelled.
                catch (...)
                {
                }
                install.done = true;
                glfwPostEmptyEvent();
            });
        if (!install.done)
        {
            ++it;
            continue;
        }
        if (install.thread.joinable())
            install.thread.join();
        const std::string& name = install.official.name;
        app.addons.Resume(file);
        if (install.cancel)
            Log(LogLevel::Info, "Installing the add-on %s was cancelled.", name.c_str());
        else if (!install.error.empty())
        {
            Log(LogLevel::Warning, "Could not install the add-on %s: %s", name.c_str(), install.error.c_str());
            launcherState.installErrors[name] = install.error;
        }
        else
        {
            Log(LogLevel::Info, "Installed the add-on %s.", name.c_str());
            app.addons.SetEnabled(file, true);
        }
        it = installs.erase(it);
        RefreshLauncher();
    }
}

launcher::AddonState StateOf(Addons::State state)
{
    switch (state)
    {
    case Addons::State::Pending: return launcher::AddonState::Pending;
    case Addons::State::Enabled: return launcher::AddonState::On;
    case Addons::State::Disabled: return launcher::AddonState::Off;
    case Addons::State::Failed: return launcher::AddonState::Failed;
    }
    return launcher::AddonState::Off;
}

// An add-on's setting as the launcher shows the settings of effects.
launcher::EffectSetting SettingOf(const Addons::Setting& setting)
{
    using Control = launcher::EffectSetting::Control;
    launcher::EffectSetting shown;
    shown.label = setting.label;
    shown.tooltip = setting.tooltip;
    shown.resettable = false;
    shown.range = true;
    shown.min = setting.min;
    shown.max = setting.max;
    shown.floating = setting.type == UNISHADE_SETTING_SLIDER;
    shown.floats[0] = setting.value;
    shown.ints[0] = static_cast<int>(std::lround(setting.value));
    switch (setting.type)
    {
    case UNISHADE_SETTING_TOGGLE: shown.control = Control::Check; break;
    case UNISHADE_SETTING_CHOICE:
        shown.control = Control::List;
        for (const std::string& choice : setting.choices)
            shown.items += choice + '\0';
        break;
    case UNISHADE_SETTING_SLIDER:
    case UNISHADE_SETTING_INTEGER: shown.control = Control::Slider; break;
    }
    return shown;
}

// Reads the Addons folder again when its files changed, so one dropped there shows without a restart. Installs write
// there, and read it again once they are done.
void LookForAddons(App& app)
{
    if (!launcherState.installs.empty())
        return;
    std::string files;
    std::error_code error;
    for (fs::directory_iterator it(DataDirectory() / "Addons", error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error))
            // libc++ counts file times in a 128-bit integer, which std::to_string does not take.
            files += it->path().filename().string() + ' ' + std::to_string(it->file_size(error)) + ' ' +
                     std::to_string(static_cast<long long>(it->last_write_time(error).time_since_epoch().count())) + '\n';
    if (files != launcherState.addonsFolder)
    {
        launcherState.addonsFolder = std::move(files);
        app.addons.Scan();
    }
}

// Whether every file of an official add-on is in the Addons folder, read once until the launcher reads the disk again.
bool Whole(const Addons::Official& official)
{
    auto [entry, added] = launcherState.addonsWhole.try_emplace(official.name);
    if (added)
        entry->second = std::all_of(official.files.begin(), official.files.end(), [](const Addons::Official::File& file) {
            std::error_code error;
            return fs::is_regular_file(DataDirectory() / "Addons" / fs::path(file.path), error);
        });
    return entry->second;
}

// The official add-ons, installed or not, then every other file in the Addons folder.
void DescribeAddons(App& app, launcher::Model& model)
{
    FollowInstalls(app);
    std::vector<launcher::Addon>& list = model.addons.emplace();
    std::vector<std::string>& files = launcherState.addonFiles;
    files.clear();
    const std::vector<Addons::Addon>& found = app.addons.List();
    std::vector<bool> listed(found.size());
    const auto installing = [](const std::string& file) {
        return std::find_if(launcherState.installs.begin(), launcherState.installs.end(),
                            [&](const auto& install) { return install->official.files[0].path == file; });
    };
    for (const Addons::Official& official : app.addons.OfficialList())
    {
        launcher::Addon addon{ .id = official.name, .name = official.name, .description = official.description, .file = official.files[0].path, .official = true };
        const auto installed =
            std::find_if(found.begin(), found.end(), [&](const Addons::Addon& candidate) { return candidate.official && candidate.file == addon.file; });
        if (installed != found.end())
            listed[installed - found.begin()] = true;
        if (const auto install = installing(addon.file); install != launcherState.installs.end())
        {
            addon.state = launcher::AddonState::Installing;
            addon.progress = (*install)->progress;
        }
        // One with files missing fails to start for that reason, which installing it fixes.
        else if (installed == found.end() || !Whole(official))
        {
            addon.state = launcher::AddonState::Available;
            if (const auto error = launcherState.installErrors.find(official.name); error != launcherState.installErrors.end())
                addon.error = error->second;
        }
        else
        {
            addon.state = StateOf(installed->state);
            addon.error = installed->error;
            addon.changesPicture = addon.state == launcher::AddonState::On && app.addons.ChangesPicture(*installed);
            addon.bypassed = installed->bypassed;
            if (addon.state == launcher::AddonState::On)
                for (const Addons::Setting& setting : installed->settings)
                    addon.settings.push_back(SettingOf(setting));
        }
        files.push_back(addon.file);
        list.push_back(std::move(addon));
    }
    for (size_t i = 0; i < found.size(); ++i)
    {
        const Addons::Addon& other = found[i];
        // One that is being installed over goes once it is.
        if (listed[i] || installing(other.file) != launcherState.installs.end())
            continue;
        files.push_back(other.file);
        launcher::Addon& addon = list.emplace_back(launcher::Addon{ .id = other.file,
                                                                    .name = other.name,
                                                                    .description = other.description,
                                                                    .file = other.file,
                                                                    .official = other.official,
                                                                    .state = StateOf(other.state),
                                                                    .error = other.error });
        addon.changesPicture = addon.state == launcher::AddonState::On && app.addons.ChangesPicture(other);
        addon.bypassed = other.bypassed;
        if (addon.state == launcher::AddonState::On)
            for (const Addons::Setting& setting : other.settings)
                addon.settings.push_back(SettingOf(setting));
    }
}

// The effect files the preset uses that are not installed, once every effect has loaded. Looked for again when the
// preset, its changes or the effects change, since that compares every effect's name.
std::vector<std::string> MissingEffects(App& app)
{
    std::vector<std::string>& files = launcherState.missing;
    // Effects load with the game's first picture. Until then none has arrived, which says nothing about the preset's.
    const bool loading = app.runtime.Loading() || !app.runtime.EffectsComplete();
    const std::tuple<fs::path, size_t, bool, bool> now{ app.runtime.PresetPath(), app.runtime.Effects().size(), loading, app.runtime.Dirty() };
    if (now == launcherState.missingChecked)
        return files;
    launcherState.missingChecked = now;
    files.clear();
    if (loading)
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
        LookForAddons(app);
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
    DescribeAddons(app, model);
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

#ifdef _WIN32
    model.settings.startWithWindows = launcherState.startsWithWindows;
    model.settings.updateChecks = app.settings.updateChecks;
#endif
    model.settings.autoSavePresets = app.settings.autoSavePresets;
    if (platform::OverlayCanStayBehind())
        model.settings.keepEffectsVisible = app.settings.keepEffectsVisible;
    model.settings.menuSize = static_cast<int>(std::lround(app.settings.menuScale * 100));
    model.settings.canShrinkMenu = app.settings.menuScale > kSmallestMenuScale + 0.01f;
    model.settings.canGrowMenu = app.settings.menuScale < kLargestMenuScale - 0.01f;
    model.settings.menuX = app.settings.menuX;
    model.settings.menuY = app.settings.menuY;
    model.settings.menuOpacity = app.settings.menuOpacity;
    model.settings.smallTiles = app.settings.smallTiles;
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
// The menu's scale over the game, which follows the size of the game's window, as on Windows, counted in the monitor's
// own units. display is the overlay's size in Dear ImGui's units.
float MenuScale(const App& app, ImVec2 display)
{
    return menu_layout::Scale(display.x / app.overlay.scale, display.y / app.overlay.scale, app.settings.menuScale) * app.overlay.scale;
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

ImTextureID GameImage(GameView view, ImVec2* size)
{
    switch (view)
    {
    case GameView::Frost: return GameTexture(frost, size);
    case GameView::Preview:
        // Hidden while the game is played, when nobody looks at the launcher. It is still asked for, so a picture
        // from the last second is there when the launcher comes to the front.
        if (app.inFront)
        {
            preview.asked = glfwGetTime();
            return ImTextureID_Invalid;
        }
        return GameTexture(preview, size);
    case GameView::Shot: return GameTexture(shot, size);
    }
    return ImTextureID_Invalid;
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

    // Bricolage Grotesque, then the system's own font and the fonts for what that lacks, such as Cyrillic, Chinese or
    // emoji. Headings fall back to the regular font.
    const auto fallbacks = [](bool bold) {
        std::vector<kit::FontFile> fonts;
        if (std::string system = platform::UiFont(bold); !system.empty())
            fonts.push_back({ std::move(system) });
        for (auto& [path, index] : platform::UiFallbackFonts(bold))
            fonts.push_back({ std::move(path), index });
#ifdef __APPLE__
        // macOS has its emoji as pictures, which Core Text draws.
        fonts.push_back({ .loader = EmojiLoader() });
#endif
        return fonts;
    };
    kit::LoadFonts({ .data = { kBricolageRegular, kBricolageRegularSize } }, fallbacks(false),
                   { .data = { kBricolageSemiBold, kBricolageSemiBoldSize } }, fallbacks(true),
                   { .data = { kBricolageExtraBold, kBricolageExtraBoldSize } });

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
    // Installs started there stop with the launcher.
    if (&ui == &app.launcher)
        launcherState.installs.clear();
    if (!ui.context)
        return;
    ImGui::SetCurrentContext(ui.context);
    vkDeviceWaitIdle(gpu.device);
    for (auto& [key, picture] : pictureSets[ui.context].pictures)
        DestroyPicture(picture);
    pictureSets.erase(ui.context);
    // Their sets go with the context's descriptor pool.
    for (GamePicture* picture : { &frost, &preview, &shot })
        if (picture->context == ui.context)
        {
            picture->context = nullptr;
            picture->set = VK_NULL_HANDLE;
        }
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
    app.launcherAnimating = kit::Animating();
    GiveBack(model);
}

void ResetMenu(App& app)
{
    StopShortcut(app);
    app.compareButton = false;
}

void ForgetMenu(App& app)
{
    app.menuClosing = false;
    launcher::Reset(launcher::Place::Game);
}

void DrawGamePictures(App& app, VkCommandBuffer commands, const GpuImage& output, bool screenshot)
{
    const double now = glfwGetTime();
    const bool frosted = app.menuOpen || app.menuClosing;
    const bool previewing = now - preview.asked < 1 && now >= nextPreview;
    if ((!frosted && !previewing && !screenshot) || !app.active || !output.image)
        return;
    backdrop::Begin(commands);
    const platform::WindowId window = app.active->id;
    if (frosted)
    {
        int width = 1, height = 1;
        glfwGetWindowSize(app.overlay.window, &width, &height);
        const ImVec2 display(static_cast<float>(std::max(width, 1)), static_cast<float>(std::max(height, 1)));
        // The design's pixels at the menu's scale, in the picture's pixels, then in those of its quarter.
        const float radius = kFrostBlur * MenuScale(app, display) * static_cast<float>(output.width) / display.x / 4;
        const uint32_t width4 = output.width / 4, height4 = output.height / 4;
        frost.ready = Fit(commands, frost, width4, height4, false) && backdrop::Make(commands, frostScratch, width4, height4) &&
                      backdrop::Shrink(commands, output, frost.target) && backdrop::Blur(commands, frost.target, frostScratch, radius, kFrostSaturation);
        frost.window = window;
    }
    if (previewing)
    {
        // Once a second while the game is played, so it costs the game next to nothing.
        nextPreview = now + (app.inFront ? 1.0 : kPreviewInterval);
        const uint32_t divisor = std::max(2u, (output.width + kPreviewWidth - 1) / kPreviewWidth);
        preview.ready = Fit(commands, preview, output.width / divisor, output.height / divisor, true) && backdrop::Shrink(commands, output, preview.target);
        preview.window = window;
    }
    if (screenshot)
    {
        shot.ready = Fit(commands, shot, output.width / 2, output.height / 2, false) && backdrop::Shrink(commands, output, shot.target);
        shot.window = window;
    }
}

void ReleaseGamePictures()
{
    for (GamePicture* picture : { &frost, &preview, &shot })
    {
        backdrop::Destroy(picture->target);
        *picture = GamePicture{};
    }
    backdrop::Destroy(frostScratch);
    backdrop::Release();
}

void DrawOverlay(App& app)
{
    app.menuActiveUniform = nullptr;
    app.menuHoveredUniform = nullptr;
    // The start hint, the only message with a key, has done its job once the menu opens.
    if (app.menuOpen && !app.toastKey.empty())
        app.toastUntil = 0;
    const float menuScale = MenuScale(app, ImGui::GetIO().DisplaySize);
    if (menuScale <= 0)
    {
        if (app.menuClosing)
            ForgetMenu(app);
        return;
    }
    if (app.menuOpen || app.menuClosing)
    {
        if (app.menuOpen)
            TakeShortcut(app, app.overlay.window);
        Host host(app);
        launcher::Model model = DescribeLauncher(app);
        model.menuKey = app.HotkeyText(kEditModeHotkey);
        model.closing = app.menuClosing;
        // How to open the menu says nothing in it.
        if (app.active)
            model.status.detail.clear();
        launcherState.overGame = true;
        const bool shown = launcher::Draw(model, host, menuScale, launcher::Place::Game);
        launcherState.overGame = false;
        GiveBack(model);
        if (!shown)
            ForgetMenu(app);
    }
    scale = menuScale;
    ApplyStyle(ImGui::GetStyle());
    PushSize(14.5f);
    DrawToast(app);
    ImGui::PopFont();
}
