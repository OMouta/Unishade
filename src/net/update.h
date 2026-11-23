#pragma once

#include <string>

// A newer Unishade on GitHub, which Setup installs.
struct Update
{
    std::string version; // such as 0.7.0, or 0.7.0-beta.2
    std::string url;     // the release page
};

// Asks GitHub for the newest release with Setup on a background thread. A beta is offered newer betas too.
void CheckForUpdate();

// The newer release, once found. Empty version while checking, when up to date, or when the check failed.
Update AvailableUpdate();
