#pragma once

#include "config.h"
#include "effects.h"
#include "games.h"
#include "gpu.h"
#include "platform.h"
#include "setup.h"

#include <future>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct GLFWwindow;
struct ImGuiContext;

// The presets folder holds presets for all games, and one level of folders in it. A folder named after a saved
// game holds that game's presets.
struct PresetFolder
{
    fs::path path;
    std::string name;
    bool all = false;
    bool game = false;
    bool playing = false;
    std::vector<fs::path> presets;
};

// The presets in a folder, sorted by name.
std::vector<fs::path> PresetsIn(const fs::path& folder);

// Why a preset or folder name can't be used, or empty when it can. Presets move between Windows, macOS and Linux, so
// names follow Windows' rules too, and a leading dot would hide the file.
std::string PresetNameProblem(const std::string& name);

// A window with its own swapchain and Dear ImGui context: the launcher, or the overlay over the game.
struct UiWindow
{
    GLFWwindow* window = nullptr;
    Surface surface;
    ImGuiContext* context = nullptr;
    float scale = 1.0f;
    bool rescale = false; // the window's content scale changed
};

// The host: the launcher outside the game, and the overlay that redraws the game with effects and shows the
// menu. The same design as on Windows, where ReShade draws on the host's own swapchain.
class App
{
public:
    bool Init(std::string& error);
    void Run();
    void Shutdown();

    // For the launcher and the menu.
    void OpenMenu();
    // Gives input back to the game, unless the user went to another window.
    void CloseMenu(bool returnToGame = true);
    void ToggleOverlay();
    // An empty window goes back to finding saved games automatically.
    void Select(std::optional<platform::Window> window);
    void SaveGames();
    // As the menu lists them: the game being played, all games, then every other folder with presets.
    std::vector<PresetFolder> PresetFolders() const;
    // Other games' folders start closed, unless the active preset is in one. Folders opened or closed by hand
    // stay that way.
    bool FolderOpen(const PresetFolder& folder) const;
    // Where new presets go: the folder of the game being played, or the presets folder without one.
    fs::path NewPresetFolder() const;
    // Switches presets, saving or dropping unsaved changes as told. Returns false when there are unsaved changes
    // and neither was asked for.
    bool SwitchPreset(const fs::path& path, bool save, bool discard);
    bool NewPreset(const std::string& name, bool copyCurrent, std::string& error);
    // Moves a preset into a folder, creating it.
    bool MovePreset(const fs::path& preset, const fs::path& folder, std::string& error);
    void StepPreset(int step);
    void RequestScreenshot(bool beforeAfter);
    // Registers the new shortcuts and saves them. Keeps the old ones and returns false when one is taken.
    bool ChangeHotkeys(const InputHotkeys& hotkeys, std::string& error);
    // While the menu records a shortcut, every shortcut is let go so its keys reach the menu.
    void SuspendHotkeys(bool suspended);
    bool EffectsInstalled();
    // A short message at the bottom of the game, with an optional key before it.
    void ShowToast(std::string text, double seconds = 3.0, std::string key = {});
    std::string HotkeyText(int id) const;
    const std::vector<platform::Window>& Windows();

    Settings settings;
    fx::Runtime runtime;
    EffectSetup setup;
    std::vector<AutoGame> autoGames;
    std::optional<platform::Window> selected;
    std::optional<platform::Window> active;
    // How the active window's process was started, read once for matching saved games.
    std::string activeExecutable;
    std::string activeCommand;
    // The saved game being played, by its folder's name. Empty for a window picked for this session only.
    std::string game;
    // Folders opened or closed by hand, by path.
    std::map<std::string, bool> folderOpen;
    bool captureEnabled = true;
    bool menuOpen = false;
    bool effectsEnabled = true;
    bool comparing = false;     // effects off while the compare shortcut is held
    bool compareButton = false; // or the menu's compare button
    bool overlayVisible = false;
    // The effect variables whose controls the menu's last frame used and had under the cursor, for effects.
    const fx::Uniform* menuActiveUniform = nullptr;
    const fx::Uniform* menuHoveredUniform = nullptr;
    std::string toast;
    std::string toastKey;
    double toastUntil = 0;
    std::string lastCaptureError;
    UiWindow launcher;
    UiWindow overlay;

private:
    void OnHotkey(int id, bool pressed);
    void UpdateTarget();
    std::optional<platform::Window> GameInFront();
    void NoticeWindowless(const std::vector<GameProcess>& processes);
    void UpdateOverlay();
    void UpdateGameHotkeys();
    void UpdateInput();
    void RenderOverlay();
    void RenderLauncher();
    void SaveScreenshot();
    void TakeScreenshots();
    void StartCapture(const platform::Window& window);
    void StopCapture();
    void FollowGame();
    // Makes a preset the active one in Unishade.ini and for the game being played.
    void UsePreset(const fs::path& path);
    bool RegisterShortcut(const Shortcut& shortcut, const Hotkey& hotkey);
    bool HasFrame() const;
    fx::Runtime::Source Source() const;

    bool inFront = false; // the game or the overlay has focus
    double effectsCheckTime = -10;
    bool effectsInstalled = false;

    platform::Frame frame;
    std::shared_ptr<void> inFlight; // the buffer of the frame the graphics card is drawing
    platform::Rect overlayBounds;
    bool gameHotkeys = false;
    bool hotkeysSuspended = false;
    bool menuFocused = false; // the overlay had focus since the menu opened
    bool startHintShown = false;
    bool screenshotRequested = false;
    bool beforeAfterRequested = false;
    std::vector<std::future<std::string>> screenshots; // being written, each to the message it shows
    std::string screenshotStamp;                        // the second of the last screenshot
    int screenshotsInStamp = 0;
    // A picture size the effects could not make room for. The overlay stays hidden until the size changes.
    uint32_t failedWidth = 0;
    uint32_t failedHeight = 0;
    // For effects: the cursor in the previous frame, and the menu's state in its last frame.
    float lastMouseX = 0;
    float lastMouseY = 0;
    bool cursorKnown = false;
    float menuWheel = 0;
    bool menuActive = false;
    bool menuHovered = false;
    bool menuTyping = false; // a text field has the keyboard
    double nextSearch = 0;
    double nextScan = 0;                   // of every process
    platform::WindowId notGame = 0;        // the window in front, when it was not a saved game's
    std::map<int, double> windowlessSince; // saved games' processes without a window, since when
    std::set<int> windowlessNoticed;
    double lastOverlayFrame = 0;
    double lastLauncherFrame = 0;
    double lastAutoSave = 0;
    double captureRetry = 0;
    std::vector<platform::Window> windows;
    double windowsTime = -10;
};

extern App app;
