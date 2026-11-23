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

// The menu's height at 1080p when the window is too short for it.
constexpr float MinHeight()
{
    return kHeader + kTabs + kMinContent + kFooter;
}
} // namespace menu_layout
