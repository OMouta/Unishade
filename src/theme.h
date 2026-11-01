#pragma once

// DSL MODS Visual Engine palette, shared by Setup, the launcher and the in-game menu. 0xRRGGBB.
namespace theme
{
constexpr unsigned kBackground = 0x100B16;
constexpr unsigned kSidebar = 0x0B0710;
constexpr unsigned kCard = 0x191222;
constexpr unsigned kCardHover = 0x241832;
constexpr unsigned kBorder = 0x332440;
constexpr unsigned kBorderStrong = 0x62427D;
constexpr unsigned kText = 0xF4ECFA;
constexpr unsigned kDim = 0xA99AB5;
constexpr unsigned kAccent = 0xA855F7;
constexpr unsigned kAccentHover = 0xC084FC;
constexpr unsigned kAccentActive = 0x9333EA;
constexpr unsigned kWarning = 0xF5C05C;
constexpr unsigned kError = 0xFF7676;
constexpr unsigned kSuccess = 0x68D694;
// Kept for compatibility with upstream UI drawing helpers.
constexpr unsigned kRainbow[] = { 0xA855F7, 0xC084FC, 0x7C3AED, 0x6D28D9, 0x9333EA, 0xD8B4FE, 0xA855F7 };
} // namespace theme
