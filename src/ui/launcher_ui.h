#pragma once

// The launcher, written once for every platform. A host fills in a Model with what the launcher shows and carries out
// what the user does through Host. launcher_ui.cpp draws it with Dear ImGui: in the launcher's window, and over the game
// as its menu, or on Windows with ReShade in UnishadeUi.dll. Text is UTF-8.
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
    // The effects use it now, so it is not renamed, moved or deleted.
    bool loaded = false;
};

// A folder presets can move to, by the host's own name for it.
struct Folder
{
    std::string id;
    std::string name;
};

// An effect the preset can turn on: one technique of an effect file, as ReShade calls them.
struct Effect
{
    std::string key; // as presets name it, which stays the same when the effects load again
    std::string name;
    std::string file;
    bool on = false;
    bool failed = false; // could not start on the graphics card
};

// One setting of an effect, with its value as floats or as ints.
struct EffectSetting
{
    enum class Control
    {
        Check,
        List,
        Color,
        Slider,
        Drag,
        Input,
    };
    Control control = Control::Drag;
    bool floating = true;
    int count = 1; // values, up to four
    std::string label;
    std::string tooltip;
    std::string category;
    std::string text;  // shown above it
    std::string items; // a List's choices, each ending in '\0'
    bool range = false;
    float min = 0;
    float max = 0;
    float step = 0;
    float floats[4] = {};
    int ints[4] = {};
};

// A preprocessor definition an effect checks, such as a quality level. Empty while the effect uses its own value.
struct EffectDefinition
{
    std::string name;
    std::string value;
    std::string fallback;
};

// The settings of one effect, described while they show.
struct EffectSettings
{
    std::string tooltip; // what the effect does
    std::vector<EffectSetting> settings;
    std::vector<EffectDefinition> definitions;
};

// An effect file that did not compile, with the compiler's errors.
struct BrokenEffect
{
    std::string file;
    std::string errors;
};

// The effects of the game being played.
struct Effects
{
    std::vector<Effect> list; // in the order they run
    // Goes up whenever the list changes, other than an effect turned on or off.
    unsigned revision = 0;
    bool on = true; // off shows the game without any of them
    // While effect files compile: how many are done out of how many.
    bool loading = false;
    size_t loaded = 0;
    size_t total = 0;
    std::vector<BrokenEffect> broken;
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
    bool picked = false; // a window picked for now rather than a saved game, so it cannot be renamed, turned off or removed
    std::vector<Preset> presets; // its own folder's first
    std::string preset;          // the id of the preset it starts with, or empty
    // While it is being played: the preset has changes that are not saved, with auto-save off, and uses effects that
    // are not installed.
    bool changed = false;
    std::vector<std::string> missing;
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
    // Offers to download effects and presets again, where the launcher installs them. False while they download.
    std::optional<bool> downloadEffects;
    // Offers to compile the effects again, such as after one was changed.
    bool reloadEffects = false;
    // macOS: effects leave out the title bar of a game in a window.
    std::optional<bool> skipTitleBar;
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
    // Offers to make, copy, rename, move and delete presets on a game's page, which can move to these folders.
    bool editPresets = false;
    std::vector<Folder> folders;
    // Why the last name given for a preset was not taken.
    std::string presetProblem;
    // The effects of the game being played, on its page.
    std::optional<Effects> effects;
    // Over the game: the shortcut that opens and closes the menu.
    std::string menuKey;
    // The preset whose unsaved changes the launcher asks about before Unishade quits, or empty.
    std::string quitting;
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
    SkipTitleBar,
    Effects, // the effects of the game being played, without stopping the overlay
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
    // Where the model offers to edit presets. New ones and copies go to the game's own folder, and the game uses them.
    // Each returns false when the name is not taken, with the reason in the next model.
    virtual bool NewPreset(size_t /*game*/, std::string_view /*name*/) { return false; }
    virtual bool CopyPreset(size_t /*game*/, std::string_view /*preset*/, std::string_view /*name*/) { return false; }
    virtual bool RenamePreset(std::string_view /*preset*/, std::string_view /*name*/) { return false; }
    virtual void MovePreset(std::string_view /*preset*/, std::string_view /*folder*/) {}
    virtual void DeletePreset(std::string_view /*preset*/) {}
    // The changes to the preset of the game being played, with auto-save off.
    virtual void SaveChanges() {}
    virtual void DiscardChanges() {}
    // The effects of the game being played, by their place in Effects::list.
    virtual void SetEffect(size_t /*effect*/, bool /*on*/) {}
    virtual void MoveEffect(size_t /*from*/, size_t /*to*/) {}
    // An effect's settings while they show. Draw is done with them before it returns, and asks again in the next frame.
    virtual const EffectSettings* DescribeSettings(size_t /*effect*/) { return nullptr; }
    virtual void SetSetting(size_t /*effect*/, size_t /*setting*/, const EffectSetting& /*value*/) {}
    // One setting, or every setting of the effect without one.
    virtual void ResetSettings(size_t /*effect*/, std::optional<size_t> /*setting*/) {}
    virtual void SetDefinition(size_t /*effect*/, size_t /*definition*/, std::string_view /*value*/) {}
    // The setting being changed and the one under the cursor, which effects can read, in every frame its settings show.
    virtual void PointAtSettings(size_t /*effect*/, std::optional<size_t> /*active*/, std::optional<size_t> /*hovered*/) {}
    // Over the game. Compare is called in every frame, with whether its button is held.
    virtual void Compare(bool /*holding*/) {}
    virtual void TakeScreenshot(bool /*beforeAfter*/) {}
    virtual void OpenScreenshots() {}
    virtual void CloseMenu() {}
    // The answer to the question about unsaved changes before quitting.
    virtual void Quit(bool /*save*/) {}
    virtual void KeepRunning() {}
    // For a game, or the defaults without one.
    virtual void SetPerformance(std::optional<size_t> game, PerformanceSetting setting, int value) = 0;

    virtual void StepMenuSize(int step) = 0;
    // The next keys pressed become the shortcut. -1 stops waiting for them.
    virtual void RecordShortcut(int shortcut) = 0;
    virtual void ClearShortcut(size_t shortcut) = 0;
    virtual void ResetShortcuts() = 0;
    // Only where Settings offers it.
    virtual void DownloadEffects() {}
    virtual void ReloadEffects() {}

protected:
    ~Host() = default;
};

// Where Draw puts the launcher. Each place keeps its own page, scroll and dialogs.
enum class Place
{
    Window, // over the whole launcher window
    Game,   // in a panel at the side of the game, which leaves the rest of the game in sight
};

// Draws the launcher into the current Dear ImGui frame. scale is the window's scaling, or the menu's over the game.
void Draw(const Model& model, Host& host, float scale, Place place = Place::Window);
// Starts the place over on the page of the game being played, the next time it is drawn.
void Reset(Place place);
} // namespace launcher
