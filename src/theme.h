#pragma once

// Unishade's colors, shared by Setup, the launcher, the menu and the macOS and Linux host. 0xRRGGBB.
namespace theme
{
// Black, like the space around the logo, so the ring's colors are the only bright ones.
constexpr unsigned kBackground = 0x000000;
constexpr unsigned kSidebar = 0x000000;
constexpr unsigned kCard = 0x0D0D16;
constexpr unsigned kCardHover = 0x181926;
constexpr unsigned kPopup = 0x0D0D16;
constexpr unsigned kBorder = 0x22233A;
constexpr unsigned kBorderStrong = 0x4A4D6E;
constexpr unsigned kText = 0xF2F2FB;
constexpr unsigned kDim = 0xA2A5C8;
// The main button of a window is white, like the cube, with dark text.
constexpr unsigned kPrimary = 0xF2F2FB;
constexpr unsigned kPrimaryHover = 0xFFFFFF;
constexpr unsigned kPrimaryActive = 0xD4D4E2;
constexpr unsigned kOnPrimary = 0x000000;
constexpr unsigned kAccent = 0x707AFF;
constexpr unsigned kAccentHover = 0x848DFF;
constexpr unsigned kWarning = 0xF5C05C;
constexpr unsigned kError = 0xFF7676;
constexpr unsigned kSuccess = 0x68D694;
// The ring in the logo.
constexpr unsigned kRainbow[] = { 0xFF4860, 0xFF9E36, 0xF8E44C, 0x54DE7A, 0x3EC6FF, 0x546EFF, 0xBA5CFF };
} // namespace theme
