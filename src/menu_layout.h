#pragma once

#include <algorithm>

namespace menu_layout
{
// Layout in pixels at 1080p.
constexpr float kWidth = 460;
constexpr float kMargin = 16;
constexpr float kHeader = 74;
constexpr float kTabs = 42;
constexpr float kFooter = 64;
// The least room the tabs get. A window too short for it scrolls the whole menu.
constexpr float kMinContent = 160;
// Text gets hard to read below this, so small windows scroll the menu instead of shrinking it further.
constexpr float kMinScale = 0.75f;

// userScale is the menu size picked in Settings, on top of the size that follows the window.
constexpr float Scale(float width, float height, float userScale = 1)
{
    if (width <= 0 || height <= 0)
        return 0;
    // Height sets the scale; narrow windows must also fit the menu and its margins.
    return std::max(std::min(height / 1080.0f, width / (kWidth + kMargin * 2)), kMinScale) * userScale;
}

// Below this, text drawn in the frame is unreadable once scaled up to the window.
constexpr float kMinFrameScale = 0.5f;

// The scale in the frame's pixels when a frame of frameHeight is scaled to a window of width and height. The menu has
// the same size in the window at every effect resolution, unless that would draw it below kMinFrameScale.
constexpr float FrameScale(float width, float height, float frameHeight, float userScale = 1)
{
    if (width <= 0 || height <= 0)
        return 0;
    return std::max(Scale(width, height, userScale) * frameHeight / height, kMinFrameScale * userScale);
}

// The menu's height at 1080p when the window is too short for it.
constexpr float MinHeight()
{
    return kHeader + kTabs + kMinContent + kFooter;
}
} // namespace menu_layout
