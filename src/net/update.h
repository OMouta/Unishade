#pragma once

#include <string>

// A newer Unishade on GitHub, which Setup installs.
struct Update
{
    std::string version; // such as 0.7.0, or 0.7.0-beta.2
    std::string url;     // the release page
    // Unishade can install it itself, with InstallUpdate: this build is signed, or a beta. Otherwise the user downloads
    // Setup from the release page.
    bool installable = false;
};

// Asks GitHub for the newest release with Setup on a background thread. A beta is offered newer betas too.
void CheckForUpdate();

// The newer release, once found. Empty version while checking, when up to date, or when the check failed.
Update AvailableUpdate();

enum class UpdateStage
{
    None,
    Downloading, // fraction says how far it is
    Installing,  // Setup runs, and closes this Unishade
    Failed,      // error says why
};

struct UpdateInstall
{
    UpdateStage stage = UpdateStage::None;
    float fraction = 0;
    std::string error;
};

// Installs the available update on a background thread: downloads its Setup from the release, runs it only when it is
// the file the release's SHA256SUMS.txt lists and the Setup of that version, and has it install over this Unishade
// without a window. A version's Setup also has to be signed, by the signer of this Unishade's own file when that is
// signed. A beta's is not signed. Setup closes this Unishade and starts the new one. Does nothing unless the update is
// installable, or while it is already being installed.
void InstallUpdate();
// Stops a download. Setup, once started, runs to its end.
void CancelUpdate();
UpdateInstall InstallingUpdate();
