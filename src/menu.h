#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

// Draws the host's menu over the game through ReShade's ImGui, after the effects, so they do not apply to it.
// Does nothing without the add-on.
void InitMenu();

// Shows which key opens the menu over the game for a few seconds, the first time capture starts after the host
// launches.
void ShowStartHint();

// Shortcut actions from the overlay window. They happen on the next frame the overlay shows.
void RequestScreenshot(bool beforeAfter);
void RequestPresetStep(int step);

// Shows the game without effects until key is let go. UpdateHeldCompare notices that, every loop.
void StartHeldCompare(UINT key);
void UpdateHeldCompare();

// Ends what only lasts while the menu is open, such as waiting for a shortcut or comparing with the game's
// own picture. Called when input goes back to the game.
void ResetMenu();

// The cursor the menu wants, such as a hand over a button.
LPCWSTR MenuCursor();

// Whether the active preset has changes that are not saved, which happens with auto-save off. They are lost when the
// host exits, so it asks about them first.
bool MenuHasUnsavedChanges();

// The name of the active preset, for asking about its unsaved changes. Empty while ReShade runs no effects.
std::wstring ActivePresetName();

// The presets a saved game can use, by its presets folder's name, for the launcher: those in its own folder and those
// for all games, as the presets folder was last read, and the one it is played with or starts with next.
struct GamePresetList
{
    std::vector<std::filesystem::path> own;
    std::vector<std::filesystem::path> shared;
    std::filesystem::path inUse;
};
GamePresetList GamePresets(const std::wstring& game);
// Goes up whenever the presets folder was read again, which RequestPresetScan asks for.
unsigned PresetScanVersion();
void RequestPresetScan();
// Makes a preset the one a saved game starts with. While the game is being played, switches to it on the next frame,
// unless there are unsaved changes.
void UseGamePreset(const std::wstring& game, const std::filesystem::path& preset);

// Writes preset changes to disk now. ReShade writes them from its present a second after they happen, so they are
// lost when the host exits or stops showing frames before that. saveUnsaved saves changes that are not saved yet
// first; without it they stay unsaved. Call it from the host's thread outside ReShade's present: every loop while
// the overlay is hidden, and before the host exits. Does nothing when nothing is waiting.
void FlushPresets(bool saveUnsaved = false);
