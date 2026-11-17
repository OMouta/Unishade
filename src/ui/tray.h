#pragma once

// The tray: the effects of the preset in use as tiles, the menu's buttons along the edge it is anchored by, and sheets
// that grow out of it for an effect's settings, adding effects, presets, and performance, the menu and shortcuts.
// Over the game it floats, moves, docks at a side and resizes. In the launcher's window it sits on the page of the game
// being played. launcher_ui.cpp draws it over the game, and its window can place it on a page.

#include "launcher_ui.h"

#include <imgui.h>

namespace launcher::tray
{
// The launcher's own windows, which the tray's More opens over the game.
enum class Open
{
    None,
    Games,
    Settings,
    Addons,
    Messages,
};

// Over the game, as the whole menu. scale is the menu's, which already has the menu size of Settings in it. covered
// says one of the launcher's windows is open over the tray, which takes Escape. Returns the window the user asked for.
Open DrawOverGame(const Model& model, Host& host, float scale, bool covered);
// In the current window at min, as wide as width, for the game being played, with kit::scale as the window's. Returns
// how high it is, which changes while it animates.
float DrawInPage(const Model& model, Host& host, ImVec2 min, float width);
// Whether the tray over the game went out of sight, which it does while Model::closing.
bool Gone();
// The tray over the game opens anew the next time it is drawn, closed to its tiles.
void Reset();
} // namespace launcher::tray
