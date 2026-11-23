#pragma once

#include <string>

// Rich Presence: while Unishade runs on a game, the user's Discord profile shows the game, its icon and the preset,
// unless turned off in the launcher. The Discord app is talked to on a thread of its own, so a Discord that is slow
// or closed never holds up the host. Windows only, through discord_win32.cpp.

// Tells Discord what to show, or to show nothing. Called every loop.
void UpdateDiscord();

enum class DiscordState
{
    Idle, // nothing to show
    Connecting,
    Closed, // Discord is not running
    Showing,
    Refused, // Discord did not take it, for the reason in error
};

struct DiscordStatus
{
    DiscordState state = DiscordState::Idle;
    std::string error;
};

DiscordStatus CurrentDiscordStatus();
