#pragma once

#include <string>

// A newer Unishade on GitHub, which Setup installs.
struct Update
{
    std::string version; // such as 0.7.0
    std::string url;     // the release page
};

// Asks GitHub for the newest release with Setup on a background thread.
void CheckForUpdate();

// The newer release, once found. Empty version while checking, when up to date, or when the check failed.
Update AvailableUpdate();
