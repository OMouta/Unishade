#pragma once

// The host's window outside the game: a sidebar of the saved games, a page for each with its presets and performance
// settings, and pages for the settings and Discord. UnishadeUi.dll draws it, with the launcher every platform shares.
// Closing it leaves the host running from its notification area icon, whose Quit ends it.
void CreateLauncher();

// Describes the launcher again when what it shows has changed, unless it is hidden or minimized. Called every loop.
void UpdateLauncher();

// Also removes the notification area icon. Does nothing more when called again.
void DestroyLauncher();

// A second copy of the host finds the running one's launcher by this class and brings it to the front.
// Shared with earlier hosts when a second instance brings the launcher forward.
inline constexpr wchar_t kLauncherClass[] = L"RobloxShadeHostLauncher";
