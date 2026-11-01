#pragma once

// DSL MODS Visual Engine palette, shared by Setup, the launcher and the in-game menu. 0xRRGGBB.
namespace theme
{
constexpr unsigned kBackground = 0x090B0E;
constexpr unsigned kSidebar = 0x050608;
constexpr unsigned kCard = 0x15191E;
constexpr unsigned kCardHover = 0x222830;
constexpr unsigned kBorder = 0x343B44;
constexpr unsigned kBorderStrong = 0x78828D;
constexpr unsigned kText = 0xFFFFFF;
constexpr unsigned kDim = 0xB9C1CA;
constexpr unsigned kAccent = 0xAEB8C2;
constexpr unsigned kAccentHover = 0xE1E6EB;
constexpr unsigned kAccentActive = 0x7D8791;
constexpr unsigned kWarning = 0xE4C77A;
constexpr unsigned kError = 0xF07878;
constexpr unsigned kSuccess = 0x83C9A1;
// Kept for compatibility with upstream UI drawing helpers.
constexpr unsigned kRainbow[] = { 0x7D8791, 0xAEB8C2, 0xE1E6EB, 0xFFFFFF, 0xC5CDD5, 0x89939E, 0x7D8791 };
} // namespace theme
