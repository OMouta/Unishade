#include "update.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "text.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace
{
// Never destroyed, since the check's thread may still run while the host exits.
struct Shared
{
    std::mutex mutex;
    Update available;
    std::atomic<unsigned> version = 0;
};
Shared& shared = *new Shared;

constexpr size_t kMissing = std::string_view::npos;
constexpr std::string_view kSpace = " \t\r\n";

// Reads 1.2.3 from the start of text. Returns false for anything else, such as the depth-assets release.
bool ParseVersion(std::string_view text, std::array<int, 3>& version)
{
    size_t position = 0;
    for (int part = 0; part < 3; ++part)
    {
        if (position >= text.size() || text[position] < '0' || text[position] > '9')
            return false;
        version[part] = 0;
        // Nine digits always fit in an int.
        for (int digits = 0; position < text.size() && text[position] >= '0' && text[position] <= '9'; ++digits)
        {
            if (digits == 9)
                return false;
            version[part] = version[part] * 10 + (text[position++] - '0');
        }
        if (part < 2 && (position >= text.size() || text[position++] != '.'))
            return false;
    }
    return position == text.size();
}

// Where the value of "key": starts in GitHub's JSON, searching from from. kMissing when there is none.
size_t JsonValue(std::string_view json, std::string_view key, size_t from)
{
    const std::string pattern = "\"" + std::string(key) + "\"";
    for (size_t position = json.find(pattern, from); position != kMissing; position = json.find(pattern, position + 1))
    {
        // The same text can be a value, which no colon follows.
        const size_t colon = json.find_first_not_of(kSpace, position + pattern.size());
        if (colon != kMissing && json[colon] == ':')
            return json.find_first_not_of(kSpace, colon + 1);
    }
    return kMissing;
}

// The string starting at start. Empty when there is none, such as for null.
std::string_view StringAt(std::string_view json, size_t start)
{
    if (start == kMissing || json[start] != '"')
        return {};
    const size_t end = json.find('"', start + 1);
    return end == kMissing ? std::string_view{} : json.substr(start + 1, end - start - 1);
}

std::string_view JsonString(std::string_view json, std::string_view key, size_t from)
{
    return StringAt(json, JsonValue(json, key, from));
}

std::optional<bool> JsonBool(std::string_view json, std::string_view key, size_t from)
{
    const size_t start = JsonValue(json, key, from);
    if (start == kMissing)
        return std::nullopt;
    if (json.substr(start, 4) == "true")
        return true;
    if (json.substr(start, 5) == "false")
        return false;
    return std::nullopt;
}

// Only releases with Setup can be installed, like on the website. The depth and DLSS5 files are releases too.
bool HasSetup(std::string_view release)
{
    const size_t assets = JsonValue(release, "assets", 0);
    for (size_t name = JsonValue(release, "name", assets); assets != kMissing && name != kMissing; name = JsonValue(release, "name", name))
        if (const std::string_view file = StringAt(release, name); file.starts_with("Unishade-Setup-") && file.ends_with(".exe"))
            return true;
    return false;
}

void Check()
{
    std::array<int, 3> current{};
    std::array<int, 3> newest{};
    ParseVersion(UNISHADE_VERSION, current);
    newest = current;
    std::string url;

    const std::atomic<bool> cancel = false;
    const std::string json = Fetch(L"https://api.github.com/repos/OMouta/Unishade/releases?per_page=30", cancel);
    // Each release lists its "tag_name", then "draft", "prerelease" and its "assets", before the next release's
    // "tag_name".
    const std::string_view releases = json;
    for (size_t position = releases.find("\"tag_name\""); position != kMissing;)
    {
        const size_t next = releases.find("\"tag_name\"", position + 1);
        const std::string_view release = releases.substr(position, next == kMissing ? kMissing : next - position);
        position = next;
        // Only digits after the v, since the tag goes into the release page's address.
        const std::string_view tag = JsonString(release, "tag_name", 0);
        std::array<int, 3> version{};
        if (tag.size() < 2 || tag[0] != 'v' || !ParseVersion(tag.substr(1), version) || version <= newest)
            continue;
        // A prerelease flag that is missing or unreadable counts as set.
        if (JsonBool(release, "prerelease", 0) == false && JsonBool(release, "draft", 0) != true && HasSetup(release))
        {
            newest = version;
            url = "https://github.com/OMouta/Unishade/releases/tag/" + std::string(tag);
        }
    }
    if (newest == current)
    {
        Log(LogLevel::Info, L"Unishade is up to date.");
        return;
    }
    const std::wstring version =
        std::to_wstring(newest[0]) + L"." + std::to_wstring(newest[1]) + L"." + std::to_wstring(newest[2]);
    Log(LogLevel::Info, L"Unishade %ls is available.", version.c_str());
    std::lock_guard lock(shared.mutex);
    shared.available = { version, Wide(url) };
    ++shared.version;
}
} // namespace

void CheckForUpdate()
{
    if (!UpdateChecksEnabled())
    {
        Log(LogLevel::Info, L"Update checks are off.");
        return;
    }
    std::thread([] {
        InitThreadLog();
        try
        {
            Check();
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Info, L"Could not check for updates: %hs", e.what());
        }
    }).detach();
}

Update AvailableUpdate()
{
    std::lock_guard lock(shared.mutex);
    return shared.available;
}

unsigned AvailableUpdateVersion()
{
    return shared.version;
}
