#include "sharing.h"
#include "json.h"
#include "net.h"
#include "text.h"

#include <windows.h>
#include <shlobj.h>

#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace sharing
{
namespace
{
// The address CMake builds in, without a slash at the end.
const std::wstring kApi = Wide(UNISHADE_SHARING_API);

std::string Encode(std::string_view text)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded += c;
        else
            encoded += std::string("%") + kHex[byte >> 4] + kHex[byte & 15];
    }
    return encoded;
}

json::Value Call(const wchar_t* method, const std::string& path, const std::string& token, const std::string& contentType, const std::string& body,
                 const std::atomic<bool>& cancel)
{
    std::wstring headers;
    if (!token.empty())
        headers += L"Authorization: Bearer " + Wide(token) + L"\r\n";
    if (!contentType.empty())
        headers += L"Content-Type: " + Wide(contentType) + L"\r\n";
    const HttpResponse response = Request(method, kApi + Wide(path), headers, body, cancel);
    if (response.status >= 200 && response.status < 300)
        return response.body.empty() ? json::Value{} : json::Parse(response.body);
    // The API explains in {"error": "..."}. Anything else, such as a proxy's page, gets the status.
    std::string message;
    try
    {
        message = json::Parse(response.body)["error"].String();
    }
    catch (const std::runtime_error&)
    {
    }
    throw Refused(response.status, message.empty() ? "The presets server answered " + std::to_string(response.status) + "." : message);
}

json::Value Call(const wchar_t* method, const std::string& path, const std::atomic<bool>& cancel)
{
    return Call(method, path, {}, {}, {}, cancel);
}

std::optional<Game> GameFrom(const json::Value& value)
{
    if (value.IsNull())
        return std::nullopt;
    return Game{ value["id"].Integer(), value["name"].String() };
}

Preset PresetFrom(const json::Value& value)
{
    Preset preset{
        .id = value["id"].Integer(),
        .name = value["name"].String(),
        .description = value["description"].String(),
        .author = value["author"].String(),
        .needsDepth = value["needsDepth"].Bool(),
        .saves = value["saves"].Integer(),
        .thumbnail = value["thumbnail"].String(),
        .ini = value["ini"].String(),
        .before = value["before"].String(),
        .after = value["after"].String(),
    };
    for (const json::Value& effect : value["effects"].items)
        preset.effects.push_back(effect.String());
    for (const auto& [key, setting] : value["dlss5"].members)
        preset.dlss5.emplace_back(key, setting.String());
    return preset;
}

std::string JsonObject(const std::vector<std::pair<std::string, std::string>>& members)
{
    std::string object = "{";
    for (const auto& [key, value] : members)
        object += (object.size() > 1 ? "," : "") + json::Quote(key) + ":" + json::Quote(value);
    return object + "}";
}

// A form as browsers send one with files, which is how the API takes screenshots.
class Form
{
public:
    Form()
    {
        std::random_device random;
        for (int i = 0; i < 4; ++i)
            boundary += std::to_string(random());
    }

    void Field(const std::string& name, const std::string& value)
    {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
    }

    void File(const std::string& name, const std::string& data)
    {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + name +
                ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n" + data + "\r\n";
    }

    std::string ContentType() const { return "multipart/form-data; boundary=" + boundary; }
    std::string Body() const { return body + "--" + boundary + "--\r\n"; }

private:
    std::string boundary = "UnishadeForm";
    std::string body;
};
} // namespace

bool Available()
{
    return !kApi.empty();
}

Place Resolve(const std::wstring& executable, std::optional<int64_t> place, const std::atomic<bool>& cancel)
{
    std::string path = "/games/resolve?executable=" + Encode(Utf8(executable));
    if (place)
        path += "&place=" + std::to_string(*place);
    const json::Value answer = Call(L"GET", path, cancel);
    return { GameFrom(answer["game"]), GameFrom(answer["experience"]) };
}

Page List(int64_t game, const std::string& effect, bool newest, int64_t offset, const std::atomic<bool>& cancel)
{
    std::string path = "/presets?game=" + std::to_string(game) + "&sort=" + (newest ? "new" : "popular") + "&offset=" + std::to_string(offset);
    if (!effect.empty())
        path += "&effect=" + Encode(effect);
    const json::Value answer = Call(L"GET", path, cancel);
    Page page;
    for (const json::Value& preset : answer["presets"].items)
        page.presets.push_back(PresetFrom(preset));
    if (!answer["next"].IsNull())
        page.next = answer["next"].Integer();
    return page;
}

Preset Get(int64_t id, const std::atomic<bool>& cancel)
{
    return PresetFrom(Call(L"GET", "/presets/" + std::to_string(id), cancel));
}

void CountSave(int64_t id, const std::atomic<bool>& cancel)
{
    Call(L"POST", "/presets/" + std::to_string(id) + "/saves", cancel);
}

std::pair<std::string, std::string> StartSignIn(const std::atomic<bool>& cancel)
{
    const json::Value answer = Call(L"POST", "/auth/start", cancel);
    return { answer["url"].String(), answer["poll"].String() };
}

std::optional<Account> PollSignIn(const std::string& poll, const std::atomic<bool>& cancel)
{
    const json::Value answer = Call(L"POST", "/auth/poll", {}, "application/json", JsonObject({ { "poll", poll } }), cancel);
    if (answer.IsNull())
        return std::nullopt;
    return Account{ answer["token"].String(), answer["user"]["name"].String() };
}

std::string AccountName(const std::string& token, const std::atomic<bool>& cancel)
{
    return Call(L"GET", "/me", token, {}, {}, cancel)["name"].String();
}

void SignOut(const std::string& token, const std::atomic<bool>& cancel)
{
    Call(L"POST", "/auth/logout", token, {}, {}, cancel);
}

int64_t Publish(const std::string& token, const Publication& publication, const std::atomic<bool>& cancel)
{
    Form form;
    form.Field("name", publication.name);
    form.Field("description", publication.description);
    form.Field("executable", publication.executable);
    form.Field("gameName", publication.gameName);
    if (publication.place)
        form.Field("place", std::to_string(*publication.place));
    form.Field("ini", publication.ini);
    form.Field("needsDepth", publication.needsDepth ? "true" : "false");
    if (!publication.dlss5.empty())
        form.Field("dlss5", JsonObject(publication.dlss5));
    form.File("before", publication.before);
    form.File("after", publication.after);
    return Call(L"POST", "/presets", token, form.ContentType(), form.Body(), cancel)["id"].Integer();
}

std::vector<OwnPreset> OwnPresets(const std::string& token, const std::atomic<bool>& cancel)
{
    std::vector<OwnPreset> presets;
    for (const json::Value& preset : Call(L"GET", "/me/presets", token, {}, {}, cancel)["presets"].items)
        presets.push_back({
            .id = preset["id"].Integer(),
            .name = preset["name"].String(),
            .game = preset["game"].String(),
            .status = preset["status"].String(),
            .rejectReason = preset["rejectReason"].String(),
            .saves = preset["saves"].Integer(),
        });
    return presets;
}

void Delete(const std::string& token, int64_t id, const std::atomic<bool>& cancel)
{
    Call(L"DELETE", "/presets/" + std::to_string(id), token, {}, {}, cancel);
}

void Report(const std::string& token, int64_t id, const std::string& reason, const std::atomic<bool>& cancel)
{
    Call(L"POST", "/presets/" + std::to_string(id) + "/reports", token, "application/json", JsonObject({ { "reason", reason } }), cancel);
}

std::string Image(const std::string& url, const std::atomic<bool>& cancel)
{
    return Fetch(Wide(url), cancel);
}

std::optional<int64_t> RobloxPlace()
{
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        return std::nullopt;
    const fs::path logs = fs::path(local) / L"Roblox" / L"logs";
    CoTaskMemFree(local);

    // The game client's logs are named like 0.741.0.7411058_20261004T031821Z_Player_38D67_last.log, beside the
    // crash handler's, which have CrashHandler in the name too.
    fs::path newest;
    fs::file_time_type newestTime{};
    std::error_code error;
    for (fs::directory_iterator entry(logs, error), end; !error && entry != end; entry.increment(error))
    {
        const std::wstring name = entry->path().filename().wstring();
        if (name.find(L"_Player_") == std::wstring::npos || name.find(L"CrashHandler") != std::wstring::npos || entry->path().extension() != L".log")
            continue;
        std::error_code ignored;
        const fs::file_time_type time = entry->last_write_time(ignored);
        if (!ignored && (newest.empty() || time > newestTime))
        {
            newest = entry->path();
            newestTime = time;
        }
    }
    if (newest.empty())
        return std::nullopt;

    // Each join writes a line like "! Joining game '7a39ee76-...' place 292439477 at 10.34.9.221". The last one is
    // the game being played, or the last one played.
    std::ifstream file(newest, std::ios::binary);
    const std::string text(std::istreambuf_iterator<char>(file), {});
    const size_t join = text.rfind("! Joining game '");
    const size_t place = join == std::string::npos ? std::string::npos : text.find("' place ", join);
    if (place == std::string::npos)
        return std::nullopt;
    int64_t id = 0;
    const char* start = text.data() + place + 8;
    const auto [end, parsed] = std::from_chars(start, text.data() + text.size(), id);
    if (parsed != std::errc() || end == start || id <= 0)
        return std::nullopt;
    return id;
}
} // namespace sharing
