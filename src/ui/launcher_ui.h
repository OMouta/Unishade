#pragma once

// The launcher, written once for every platform. A host fills in a Model with what the launcher shows and carries out
// what the user does through Host. launcher_ui.cpp draws it with Dear ImGui: on Windows in UnishadeUi.dll, on macOS and
// Linux in the host itself. Text is UTF-8.
//
// Draw only reads the model, and the host must not change it until Draw returns: a Host call that changes something
// shows in the model the host makes for the next frame.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher
{
// In the order of LogLevel on every platform.
enum class Level
{
    Info,
    Ok,
    Warning,
    Error,
};

struct Notice
{
    Level level;
    std::string text;
};

// Something on Home that needs the user, such as an update or effects that are not installed yet.
struct Card
{
    std::string title;
    std::string text;
    bool attention = false;           // tinted with the accent color
    float progress = -1;              // a bar from 0 to 1 under the text
    std::vector<std::string> buttons; // the first is the primary one
};

enum class Tone
{
    Waiting,
    Running,
    Off,
};

// What Unishade is doing: waiting for a game, running on one, or turned off.
struct Status
{
    Tone tone = Tone::Waiting;
    std::string title;
    std::string detail;
    std::string warning;  // such as why the game could not be captured
    std::string game;     // the game shown on the icon, or empty for a dot
    std::string picture;  // the game's picture, or empty for its first letter
    bool canPick = false;   // offers Pick a window
    bool canDetect = false; // offers Detect automatically
    bool effectsShown = true;
};

// A preset by the host's own name for it, such as its path.
struct Preset
{
    std::string id;
    std::string name;
    std::string folder; // empty for the game's own folder, or the name of the folder it is in
};

// A game's performance settings, or the defaults. Only on Windows.
struct Performance
{
    int frameRate = 0; // 0 shows every frame the game draws
    int effectResolution = 100;
    std::optional<int> depthSize; // while depth estimation is installed
};

enum class PerformanceSetting
{
    FrameRate,
    EffectResolution,
    DepthSize,
};

// The choices Performance offers, besides a custom frame rate between the slowest and fastest.
inline constexpr int kFrameRates[] = { 0, 120, 60 };
inline constexpr int kEffectResolutions[] = { 100, 75, 50 };
inline constexpr int kDepthSizes[] = { 924, 518, 392, 266 };
inline constexpr int kSlowestFrameRate = 30;
inline constexpr int kFastestFrameRate = 500;

struct Game
{
    std::string id; // stays the same when the game is renamed
    std::string name;
    std::string executable;
    std::string picture; // or empty for the name's first letter
    bool enabled = true;
    bool running = false;
    std::vector<Preset> presets; // its own folder's first
    std::string preset;          // the id of the preset it starts with, or empty
    std::optional<Performance> performance;
};

// A game removed a moment ago, which can be put back.
struct Removed
{
    size_t index = 0; // where it was in the list
    std::string name;
};

struct Shortcut
{
    std::string title;
    std::string description;
    std::string keys; // empty when unassigned
    bool required = false;
};

struct Settings
{
    std::optional<bool> startWithWindows;
    bool autoSavePresets = true;
    std::optional<int> menuSize; // in percent
    bool canShrinkMenu = false;
    bool canGrowMenu = false;
    std::optional<bool> keepEffectsVisible;
    std::optional<bool> updateChecks;
    std::optional<Performance> performance; // for every game without its own
    std::optional<bool> debugInfo;
    std::vector<Shortcut> shortcuts;
    int recording = -1; // the shortcut waiting for its keys
    std::string shortcutError;
    std::vector<std::string> shortcutWarnings;
};

struct Discord
{
    bool on = true;
    std::string status;
    Level level = Level::Info;
};

// An open window, in the picker.
struct Window
{
    std::string title;
    std::string detail;
    std::string picture;
};

struct Picker
{
    bool add = true; // adds the window's game, or uses the window until Detect automatically
    std::vector<Window> windows;
};

struct Model
{
    std::string version;
    Status status;
    std::vector<Card> cards;
    std::vector<Game> games;
    std::optional<Removed> removed;
    std::vector<Notice> notices; // oldest first
    std::vector<std::string> links;
    Settings settings;
    std::optional<Discord> discord;
    std::optional<Picker> picker;
    // Why the last name given to RenameGame was not taken.
    std::string renameProblem;
    // Goes up when pictures may have changed, such as when a game's icon was found.
    unsigned pictures = 0;
};

enum class Switch
{
    EffectsShown,
    StartWithWindows,
    AutoSavePresets,
    KeepEffectsVisible,
    UpdateChecks,
    DebugInfo,
    DiscordPresence,
};

class Host
{
public:
    virtual void SetSwitch(Switch which, bool on) = 0;
    virtual void PressCard(size_t card, size_t button) = 0;
    virtual void OpenLink(size_t link) = 0;
    virtual void OpenUrl(std::string_view url) = 0;
    virtual void DismissNotices() = 0;

    // The picker lists the open windows until a window is chosen or it is closed.
    virtual void OpenPicker(bool add) = 0;
    virtual void ChooseWindow(size_t window) = 0;
    virtual void ClosePicker() = 0;
    virtual void DetectAutomatically() = 0;

    virtual void SetGameEnabled(size_t game, bool enabled) = 0;
    // Returns false when the name is not taken, with the reason in the next model.
    virtual bool RenameGame(size_t game, std::string_view name) = 0;
    virtual void RemoveGame(size_t game) = 0;
    virtual void UndoRemove() = 0;
    virtual void UsePreset(size_t game, std::string_view preset) = 0;
    virtual void OpenPresetsFolder(size_t game) = 0;
    // For a game, or the defaults without one.
    virtual void SetPerformance(std::optional<size_t> game, PerformanceSetting setting, int value) = 0;

    virtual void StepMenuSize(int step) = 0;
    // The next keys pressed become the shortcut. -1 stops waiting for them.
    virtual void RecordShortcut(int shortcut) = 0;
    virtual void ClearShortcut(size_t shortcut) = 0;
    virtual void ResetShortcuts() = 0;

protected:
    ~Host() = default;
};

// Draws the launcher over the whole window into the current Dear ImGui frame. scale is the window's scaling.
void Draw(const Model& model, Host& host, float scale);
} // namespace launcher
