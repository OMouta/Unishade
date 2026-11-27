#pragma once

#include "addons/addons.h"

#include <functional>
#include <string>
#include <vector>

// The official add-ons: Unishade's own, which the Add-ons page offers to install. They are not in the program. Each
// is released on its own, as src/addons/addon_releases.h describes, and Unishade asks GitHub which there are. What it
// finds it keeps in the cache folder, so the page lists them at once the next time, and without a connection.

// Downloads an HTTPS address into memory. Throws std::runtime_error with a message for the log.
using FetchText = std::function<std::string(const std::string& url)>;

// As Unishade downloads everything else: through WinHTTP on Windows, and the system's curl on macOS and Linux.
std::string FetchFromTheNet(const std::string& url);

// The official add-ons the last look found for this system and this build, from the cache folder.
std::vector<Addons::Official> KeptOfficialAddons();

// Asks GitHub for the add-ons' releases and returns, for each add-on, the newest one this build is offered and can
// run on this system: one for this Unishade's version of the add-on interface or an older one, with a library for the
// system. Every file it reads of a release has to have the SHA-256 the release's SHA256SUMS.txt lists. An add-on that
// could not be looked at keeps its entry in known, the list so far. Throws std::runtime_error when GitHub's list
// itself could not be read. Blocks, so call it from a thread of its own.
std::vector<Addons::Official> FindOfficialAddons(const FetchText& fetch, const std::vector<Addons::Official>& known);
