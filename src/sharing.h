#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The presets API, where players share presets for the game they play. Every call waits for the server, so the menu
// makes them from a thread of its own. Calls throw std::runtime_error with a message to show when the server can't
// be reached, and Refused when it turns the request away. Text is UTF-8.
namespace sharing
{
struct Refused : std::runtime_error
{
    unsigned status;
    Refused(unsigned code, const std::string& message) : std::runtime_error(message), status(code) {}
};

// Whether this build knows where the API is. Sharing stays hidden without it.
bool Available();

struct Game
{
    int64_t id = 0;
    std::string name;
};

// The game an executable belongs to and the Roblox experience a place belongs to, each when the API knows it.
struct Place
{
    std::optional<Game> game;
    std::optional<Game> experience;
};

// Keys and values from the [RENODX-DLSS-preset1] section of ReShade.ini.
using Settings = std::vector<std::pair<std::string, std::string>>;

struct Preset
{
    int64_t id = 0;
    std::string name;
    std::string description;
    std::string author;
    std::vector<std::string> effects;
    bool needsDepth = false;
    // Empty when it was made without DLSS5.
    Settings dlss5;
    int64_t saves = 0;
    std::string thumbnail;
    // Only with a single preset.
    std::string ini;
    std::string before;
    std::string after;
};

struct Page
{
    std::vector<Preset> presets;
    std::optional<int64_t> next;
};

struct Account
{
    std::string token;
    std::string name;
};

// One of the signed-in player's presets, with how its review went.
struct OwnPreset
{
    int64_t id = 0;
    std::string name;
    std::string game;
    std::string status;
    std::string rejectReason;
    int64_t saves = 0;
};

struct Publication
{
    std::string name;
    std::string description;
    std::string executable;
    // Names the game when nobody has published for it yet.
    std::string gameName;
    // Publishes for this Roblox experience instead of all of Roblox.
    std::optional<int64_t> place;
    std::string ini;
    bool needsDepth = false;
    Settings dlss5;
    std::string before; // JPEG
    std::string after;
};

Place Resolve(const std::wstring& executable, std::optional<int64_t> place, const std::atomic<bool>& cancel);
Page List(int64_t game, const std::string& effect, bool newest, int64_t offset, const std::atomic<bool>& cancel);
Preset Get(int64_t id, const std::atomic<bool>& cancel);
void CountSave(int64_t id, const std::atomic<bool>& cancel);

// Starts signing in with Discord: the address to open in the browser and the secret to poll with.
std::pair<std::string, std::string> StartSignIn(const std::atomic<bool>& cancel);
// The account once the browser has signed in, nothing until then. Throws Refused when the sign-in expired.
std::optional<Account> PollSignIn(const std::string& poll, const std::atomic<bool>& cancel);
// The name of the account a token signs in as. Throws Refused with status 401 when the token is no longer valid.
std::string AccountName(const std::string& token, const std::atomic<bool>& cancel);
void SignOut(const std::string& token, const std::atomic<bool>& cancel);

// Sends a preset for review and returns its ID.
int64_t Publish(const std::string& token, const Publication& publication, const std::atomic<bool>& cancel);
std::vector<OwnPreset> OwnPresets(const std::string& token, const std::atomic<bool>& cancel);
void Delete(const std::string& token, int64_t id, const std::atomic<bool>& cancel);
void Report(const std::string& token, int64_t id, const std::string& reason, const std::atomic<bool>& cancel);

// Downloads a screenshot from an address the API gave.
std::string Image(const std::string& url, const std::atomic<bool>& cancel);

// The place the newest Roblox client log last joined, from %LOCALAPPDATA%\Roblox\logs. Empty without one.
std::optional<int64_t> RobloxPlace();
} // namespace sharing
