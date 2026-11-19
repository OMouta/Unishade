#pragma once

// The launcher, written once for every platform. A host fills in a Model with what the launcher shows and carries out
// what the user does through Host. launcher_ui.cpp draws it with Dear ImGui: in the launcher's window, and over the game
// as its menu. Text is UTF-8.
//
// Draw only reads the model, and the host must not change it until Draw returns: a Host call that changes something
// shows in the model the host makes for the next frame.

#include <array>
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
    bool canDownload = false; // offers Download effects and presets, since none are installed
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
    bool kept = false;   // turned off, and still one of the preset's until it is removed
    std::string description; // what it does, in a line, where its file says
    std::string pack;        // the folder of effects it came in, or empty
};

// One setting of an effect, or of an add-on, with its value as floats or as ints.
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
    std::string units; // after the value
    int spacing = 0;   // lines of space above it
    bool categoryClosed = false; // its category starts closed
    bool resettable = true;
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

// A game's performance settings, or the defaults.
struct Performance
{
    int frameRate = 0; // 0 shows every frame the game draws
    std::optional<int> effectResolution; // where effects can run on a smaller picture
};

enum class PerformanceSetting
{
    FrameRate,
    EffectResolution,
};

// The choices Performance offers, besides a custom frame rate between the slowest and fastest.
inline constexpr int kFrameRates[] = { 0, 120, 60 };
inline constexpr int kEffectResolutions[] = { 100, 75, 50 };
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
    // The parts of its picture marked as its own interface, which effects leave as the game drew them: the left, top,
    // width and height of each, in fractions of the picture's.
    std::vector<std::array<float, 4>> uiMask;
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
    // Where the menu over the game was left: across the game's window from 0 to 1, where exactly 0 and 1 are docked at
    // its left and right sides, and down it to the middle of the menu's buttons. In the top half the menu opens
    // downward. Below 0 until the menu is moved.
    float menuX = -1;
    float menuY = -1;
    // How solid the tray is, from 0.2 to 1, and whether its tiles are small.
    float menuOpacity = 1;
    bool smallTiles = false;
    // The tray, and the launcher's blocks and windows, in near-white type on near-black.
    bool darkMenu = false;
    // The tray says how its tiles work, until the user says they have it.
    bool menuHints = false;
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

enum class AddonState
{
    Available,  // official, and not installed or with files missing
    Installing, // progress says how far it is
    Pending,    // not official, and the user has not said whether to load it
    On,
    Off,
    Failed, // error says why
};

// An add-on in the Addons folder, or an official one that can be installed.
struct Addon
{
    std::string id; // the host's own name for it, which stays the same while it installs
    std::string name;
    std::string description;
    std::string file; // as it is named in the Addons folder
    bool official = false;
    AddonState state = AddonState::Off;
    // Why it failed, or why the last install did.
    std::string error;
    float progress = 0; // from 0 to 1, while it installs
    // While it is on: it changes the game's picture, as effects do.
    bool changesPicture = false;
    // It stays loaded and leaves the picture alone, as an effect that is turned off. The preset keeps that.
    bool bypassed = false;
    // While it is on: its settings. Each preset keeps them for one that changes the picture.
    std::vector<EffectSetting> settings;
};

// An open window, in the picker.
struct Window
{
    std::string title;
    std::string detail;
    std::string picture;
};

// A game found installed on this computer, offered the first time Unishade starts.
struct FoundGame
{
    std::string name;
    std::string store; // such as Steam, or empty
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
    // A newer version Setup can install, such as 0.7.0, or empty. Its card is among cards.
    std::string update;
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
    // Over the game: the menu was closed and is on its way out, without taking input. See Draw.
    bool closing = false;
    // The preset whose unsaved changes the launcher asks about before Unishade quits, or empty.
    std::string quitting;
    std::vector<Notice> notices; // oldest first
    std::vector<std::string> links;
    Settings settings;
    std::optional<Discord> discord;
    // The Add-ons page shows, where the host has add-ons. Official ones first.
    std::optional<std::vector<Addon>> addons;
    std::optional<Picker> picker;
    // The games found installed, which the window offers to add until AddFoundGames answers.
    std::vector<FoundGame> found;
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
    SmallTiles,
    MenuHints,
    DarkMenu,
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
    // The answer to Model::found: the ones to add by their place in it, or none.
    virtual void AddFoundGames(const std::vector<size_t>& games) = 0;

    virtual void SetGameEnabled(size_t game, bool enabled) = 0;
    // Returns false when the name is not taken, with the reason in the next model.
    virtual bool RenameGame(size_t game, std::string_view name) = 0;
    virtual void RemoveGame(size_t game) = 0;
    virtual void UndoRemove() = 0;
    virtual void UsePreset(size_t game, std::string_view preset) = 0;
    virtual void OpenPresetsFolder(size_t game) = 0;
    // Where the model offers to edit presets. New ones and copies go to the game's own folder, and the game uses them.
    // Each returns false when the name is not taken, with the reason in the next model.
    virtual bool NewPreset(size_t game, std::string_view name) = 0;
    virtual bool CopyPreset(size_t game, std::string_view preset, std::string_view name) = 0;
    virtual bool RenamePreset(std::string_view preset, std::string_view name) = 0;
    virtual void MovePreset(std::string_view preset, std::string_view folder) = 0;
    virtual void DeletePreset(std::string_view preset) = 0;
    // The changes to the preset of the game being played, with auto-save off.
    virtual void SaveChanges() = 0;
    virtual void DiscardChanges() = 0;
    // The effects of the game being played, by their place in Effects::list. One turned off stays the preset's, as
    // Effect::kept says, until it is removed.
    virtual void SetEffect(size_t effect, bool on) = 0;
    virtual void RemoveEffect(size_t effect) = 0;
    virtual void MoveEffect(size_t from, size_t to) = 0;
    // An effect's settings while they show. Draw is done with them before it returns, and asks again in the next frame.
    virtual const EffectSettings* DescribeSettings(size_t effect) = 0;
    virtual void SetSetting(size_t effect, size_t setting, const EffectSetting& value) = 0;
    // One setting, or every setting of the effect without one.
    virtual void ResetSettings(size_t effect, std::optional<size_t> setting) = 0;
    virtual void SetDefinition(size_t effect, size_t definition, std::string_view value) = 0;
    // The setting being changed and the one under the cursor, which effects can read, in every frame its settings show.
    virtual void PointAtSettings(size_t effect, std::optional<size_t> active, std::optional<size_t> hovered) = 0;
    // Over the game. Compare is called in every frame, with whether its button is held.
    virtual void Compare(bool holding) = 0;
    virtual void TakeScreenshot(bool beforeAfter) = 0;
    virtual void OpenScreenshots() = 0;
    virtual void CloseMenu() = 0;
    // The answer to the question about unsaved changes before quitting.
    virtual void Quit(bool save) = 0;
    virtual void KeepRunning() = 0;
    // For a game, or the defaults without one.
    virtual void SetPerformance(std::optional<size_t> game, PerformanceSetting setting, int value) = 0;

    // As Game::uiMask.
    virtual void SetUiMask(size_t game, const std::vector<std::array<float, 4>>& areas) = 0;

    virtual void StepMenuSize(int step) = 0;
    // Over the game: the menu size in percent that dragging the menu's corner left, and where the menu was moved to,
    // as Settings::menuX and menuY say.
    virtual void SetMenuSize(int percent) = 0;
    virtual void PlaceMenu(float x, float y) = 0;
    // As Settings::menuOpacity, once its bar is let go.
    virtual void SetMenuOpacity(float opacity) = 0;
    // The next keys pressed become the shortcut. -1 stops waiting for them.
    virtual void RecordShortcut(int shortcut) = 0;
    virtual void ClearShortcut(size_t shortcut) = 0;
    virtual void ResetShortcuts() = 0;
    // Only where Settings offers it.
    virtual void DownloadEffects() = 0;
    virtual void ReloadEffects() = 0;

    // Add-ons, by their place in Model::addons.
    virtual void InstallAddon(size_t addon) = 0;
    virtual void CancelInstall(size_t addon) = 0;
    // The answer for a Pending add-on.
    virtual void LoadAddon(size_t addon, bool load) = 0;
    virtual void SetAddon(size_t addon, bool on) = 0;
    // Has an add-on that is on leave the picture alone, or work on it again.
    virtual void BypassAddon(size_t addon, bool bypassed) = 0;
    virtual void SetAddonSetting(size_t addon, size_t setting, const EffectSetting& value) = 0;
    virtual void OpenAddonsFolder() = 0;

protected:
    ~Host() = default;
};

// Where Draw puts the launcher. Each place keeps its own page, scroll and dialogs.
enum class Place
{
    Window, // over the whole launcher window
    Game,   // in a tray over the game, which can be moved and resized and leaves the rest of the game in sight
};

// Draws the launcher into the current Dear ImGui frame. scale is the window's scaling, or the menu's over the game.
// While Model::closing, the menu goes away the way it came, and Draw returns false once it is gone: the host then stops
// drawing it and calls Reset. Otherwise it returns true.
bool Draw(const Model& model, Host& host, float scale, Place place = Place::Window);
// Starts the place over on the page of the game being played, the next time it is drawn.
void Reset(Place place);
} // namespace launcher
