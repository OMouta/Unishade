#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// What macOS's window list and ScreenCaptureKit's frames say about a game's window, made into decisions: which
// windows can be games, whether a window covers its screen, and which pixels of a frame are the game's. Kept out of
// platform_macos.mm so tests can check them without a window.
namespace window_rules
{
// CGWindowLevel.h's levels for normal windows and for pop-up menus. Everything from pop-up menus up is the system's
// own: menus, tooltips, the cursor and alerts.
constexpr int kNormalLevel = 0;
constexpr int kPopUpMenuLevel = 101;

// Whether a window on this layer can be a game. Most games are on the normal layer. Wine raises a window that covers
// the screen while active to the status level plus one, a topmost window to the floating or Dock level plus one, and
// with the displays captured to the shielding level plus one. Those layers also hold the Dock, the menu bar, status
// items, Control Center and other programs' overlays, which belong to programs without a Dock icon, so a window above
// the normal layer counts only when its program is a regular one.
constexpr bool GameLayer(int layer, bool regularProgram, int shieldingLevel)
{
    if (layer == kNormalLevel)
        return true;
    if (!regularProgram)
        return false;
    return (layer > kNormalLevel && layer < kPopUpMenuLevel) || layer == shieldingLevel || layer == shieldingLevel + 1;
}

// A rectangle in CoreGraphics' global coordinates: points, from the top left corner of the main screen down.
struct Box
{
    double x = 0, y = 0, width = 0, height = 0;
};

// Whether a window covers a screen, as a game in full screen does. On screens with a camera housing, macOS puts
// full-screen windows below it, so the top may start as far down as safeTop. A zoomed window starts below the menu
// bar, so the top may start no further down than one point above the menu bar's bottom, when the screen shows a menu
// bar of menuBar points. While the menu bar hides itself, a zoomed window reaches the top of the screen and cannot be
// told from one in full screen. On some screens with a camera housing the menu bar is a point taller than the housing,
// and a window in full screen starts below the menu bar, exactly where a zoomed one does. There the Dock tells them
// apart: macOS takes it off the screen in a full-screen Space, so without a Dock the top may start at the menu bar's
// bottom.
constexpr bool CoversScreen(const Box& window, const Box& screen, double safeTop, double menuBar, bool dockShown = true)
{
    constexpr double kSlack = 0.5;
    const double top = !dockShown ? std::max(safeTop, menuBar) : menuBar > 0 ? std::min(safeTop, menuBar - 1) : safeTop;
    return window.x <= screen.x + kSlack && window.x + window.width >= screen.x + screen.width - kSlack &&
           window.y <= screen.y + std::max(top, 0.0) + kSlack && window.y + window.height >= screen.y + screen.height - kSlack;
}

// Pixels of a frame, from its top left corner.
struct Crop
{
    size_t x = 0, y = 0, width = 0, height = 0;
};

// The rows of a title bar titlePoints high, in a frame where each of the window's points is pixelsPerPoint pixels.
// Rounded up, so no row of the title bar is left.
inline size_t TitleRows(double titlePoints, double pixelsPerPoint)
{
    if (titlePoints <= 0 || pixelsPerPoint <= 0)
        return 0;
    return size_t(std::ceil(titlePoints * pixelsPerPoint - 1e-3));
}

// The window's part of a frame of bufferWidth by bufferHeight pixels. content is where ScreenCaptureKit put the
// window, in points that are scale pixels each. The edges are rounded inward, so the crop never takes pixels from
// outside the window, then titleRows are dropped from the top. At least one pixel is left each way.
inline Crop ContentCrop(size_t bufferWidth, size_t bufferHeight, const Box& content, double scale, size_t titleRows)
{
    Crop crop{ 0, 0, bufferWidth, bufferHeight };
    if (bufferWidth == 0 || bufferHeight == 0 || scale <= 0)
        return crop;
    // Products such as 0.5 times 2 come out a hair off whole numbers, which must not cost a row.
    constexpr double kSlack = 1e-3;
    const auto inward = [&](double start, double size, size_t limit, size_t& at, size_t& length) {
        const double first = std::clamp(std::ceil(start * scale - kSlack), 0.0, double(limit - 1));
        const double end = std::clamp(std::floor((start + size) * scale + kSlack), first + 1, double(limit));
        at = size_t(first);
        length = size_t(end - first);
    };
    inward(content.x, content.width, bufferWidth, crop.x, crop.width);
    inward(content.y, content.height, bufferHeight, crop.y, crop.height);
    if (titleRows < crop.height)
    {
        crop.y += titleRows;
        crop.height -= titleRows;
    }
    return crop;
}
} // namespace window_rules
