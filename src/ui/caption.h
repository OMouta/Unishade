#pragma once

// The launcher window's title bar, the row above the shelf of games. Where the platform lets the window draw into its
// title bar, the host says what is left of the window's own, and the launcher says each frame where its row and what
// in it takes clicks are, so the host can tell the system which parts move the window. Both sides run on the main
// thread. Positions are in the window's client coordinates, as Dear ImGui's: pixels on Windows, points on macOS.

#include <vector>

namespace caption
{
enum class Button
{
    None,
    Minimize,
    Maximize, // restore while maximized
    Close,
};

struct Box
{
    float left = 0, top = 0, right = 0, bottom = 0;
    bool Contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

struct Bar
{
    // From the host.
    bool drawButtons = false; // the window has no caption of its own, so the launcher draws minimize, maximize and close
    float inset = 0;          // room on the left for the window's own buttons, as macOS's
    bool maximized = false;
    Button hovered = Button::None; // the system sends the mouse over the buttons to the host, not to Dear ImGui
    Button pressed = Button::None;

    // From the launcher, each frame it draws the window.
    float height = 0;          // the bar, from the top of the window
    Box buttons[3];            // minimize, maximize and close, where drawButtons
    std::vector<Box> controls; // what in the bar takes clicks; the rest of it moves the window
};

// The launcher window's. The menu over the game has none.
inline Bar bar;
} // namespace caption
