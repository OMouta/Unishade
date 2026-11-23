#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Discord's Rich Presence, as the Discord app takes it from other programs on the same computer: through a named pipe
// on Windows and a socket on macOS and Linux. What goes through it is the same everywhere. Each message is a frame:
// an opcode and a length, both 32-bit little-endian, then that many bytes of JSON.
namespace discord
{
enum Opcode : uint32_t
{
    kHandshake = 0,
    kFrame = 1,
    kClose = 2,
    kPing = 3,
    kPong = 4,
};

// Discord's frames are a few kilobytes. A longer one means the connection is broken.
constexpr uint32_t kMaxFrame = 1 << 16;

// Unishade's logo, beside the game's icon. Discord shows pictures from other servers by their address.
constexpr char kLogo[] = "https://raw.githubusercontent.com/OMouta/Unishade/main/assets/Unishade.png";

// What Discord shows while Unishade runs on a game. Text is UTF-8.
struct Activity
{
    std::string game;       // its name in Unishade
    std::string preset;     // the active preset's name, or empty
    std::string executable; // the game's path, which finds its icon
    int64_t start = 0;      // when Unishade started running on it, in seconds since 1970

    bool operator==(const Activity&) const = default;
};

struct Message
{
    uint32_t opcode = 0;
    std::string json;
};

inline std::string Frame(uint32_t opcode, std::string_view json)
{
    std::string frame;
    for (const uint32_t value : { opcode, static_cast<uint32_t>(json.size()) })
        for (int shift = 0; shift < 32; shift += 8)
            frame += static_cast<char>((value >> shift) & 0xFF);
    frame += json;
    return frame;
}

// Cuts the first whole frame off the front of received. False until all of it has arrived. Throws
// std::runtime_error for a frame longer than kMaxFrame.
inline bool TakeFrame(std::string& received, Message& message)
{
    if (received.size() < 8)
        return false;
    const auto value = [&](size_t at) {
        uint32_t result = 0;
        for (size_t i = 4; i-- > 0;)
            result = (result << 8) | static_cast<unsigned char>(received[at + i]);
        return result;
    };
    const uint32_t size = value(4);
    if (size > kMaxFrame)
        throw std::runtime_error("Discord sent a frame longer than any it sends");
    if (received.size() < 8 + size)
        return false;
    message.opcode = value(0);
    message.json = received.substr(8, size);
    received.erase(0, 8 + size);
    return true;
}

// text as a JSON string, quotes included.
inline std::string Quote(std::string_view text)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string quoted = "\"";
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            quoted += '\\';
        if (byte < 0x20)
            quoted += std::string("\\u00") + kHex[byte >> 4] + kHex[byte & 15];
        else
            quoted += c;
    }
    return quoted + "\"";
}

// Discord takes at most 128 characters of text, so longer text is cut to 128 bytes, between two characters.
inline std::string Fit(std::string text)
{
    if (text.size() <= 128)
        return text;
    size_t end = 128;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    text.resize(end);
    return text;
}

inline std::string HandshakeJson(std::string_view clientId)
{
    return "{\"v\":1,\"client_id\":" + Quote(clientId) + "}";
}

// The game's name and icon, or Unishade's logo when Discord has no icon for it, with the preset under the name.
// pid is Unishade's, so Discord clears the activity when Unishade exits.
inline std::string SetActivityJson(uint32_t pid, const Activity& activity, const std::string& icon, unsigned nonce)
{
    // Discord refuses text under 2 characters, so that is left out.
    const auto text = [](const char* key, const std::string& value) {
        const std::string fitted = Fit(value);
        return fitted.size() < 2 ? std::string() : Quote(key) + ":" + Quote(fitted) + ",";
    };
    const std::string assets = icon.empty() ? "\"large_image\":" + Quote(kLogo)
                                            : "\"large_image\":" + Quote(icon) + "," + text("large_text", activity.game) +
                                                  "\"small_image\":" + Quote(kLogo) + ",\"small_text\":\"Unishade\"";
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":{" + text("details", activity.game) +
           (activity.preset.empty() ? "" : text("state", "Preset: " + activity.preset)) + "\"timestamps\":{\"start\":" +
           std::to_string(activity.start) + "},\"assets\":{" + assets + "}}},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

// Reads JSON from Discord for what Unishade needs of it. Every function returns false for JSON it cannot read.
class Reader
{
public:
    explicit Reader(std::string_view text) : text(text) {}

    // Whether the next character past any spaces is c.
    bool Peek(char c)
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
            ++at;
        return at < text.size() && text[at] == c;
    }

    // A string, decoded into value when given.
    bool String(std::string* value = nullptr)
    {
        if (!Take('"'))
            return false;
        while (at < text.size())
        {
            const char c = text[at++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                if (value)
                    *value += c;
                continue;
            }
            if (at == text.size())
                return false;
            uint32_t code = 0;
            switch (const char escape = text[at++])
            {
            case '"':
            case '\\':
            case '/': code = static_cast<uint32_t>(escape); break;
            case 'b': code = '\b'; break;
            case 'f': code = '\f'; break;
            case 'n': code = '\n'; break;
            case 'r': code = '\r'; break;
            case 't': code = '\t'; break;
            case 'u':
                if (!Hex(at, code))
                    return false;
                at += 4;
                // A character past U+FFFF comes as two halves. A half without the other is replaced.
                if (code >= 0xD800 && code < 0xE000)
                {
                    uint32_t low = 0;
                    if (code < 0xDC00 && text.substr(at, 2) == "\\u" && Hex(at + 2, low) && low >= 0xDC00 && low < 0xE000)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        at += 6;
                    }
                    else
                        code = 0xFFFD;
                }
                break;
            default: return false;
            }
            if (value)
                AppendUtf8(*value, code);
        }
        return false;
    }

    // Calls member with each key of an object, which must read the key's value.
    template <typename Member>
    bool Object(Member&& member)
    {
        if (depth == kMaxDepth || !Take('{'))
            return false;
        ++depth;
        bool ok = true;
        if (!Take('}'))
        {
            do
            {
                std::string key;
                ok = String(&key) && Take(':') && member(key);
            } while (ok && Take(','));
            ok = ok && Take('}');
        }
        --depth;
        return ok;
    }

    // Calls element for each value of an array, which must read it.
    template <typename Element>
    bool Array(Element&& element)
    {
        if (depth == kMaxDepth || !Take('['))
            return false;
        ++depth;
        bool ok = true;
        if (!Take(']'))
        {
            do
                ok = element();
            while (ok && Take(','));
            ok = ok && Take(']');
        }
        --depth;
        return ok;
    }

    // Any value.
    bool Skip()
    {
        if (Peek('{'))
            return Object([this](const std::string&) { return Skip(); });
        if (Peek('['))
            return Array([this] { return Skip(); });
        if (Peek('"'))
            return String();
        // A number, true, false or null.
        const size_t start = at;
        while (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '-' || text[at] == '+' || text[at] == '.'))
            ++at;
        return at > start;
    }

private:
    // Deeper JSON would run out of stack, and nothing Discord sends is close.
    static constexpr int kMaxDepth = 64;

    bool Take(char c)
    {
        if (!Peek(c))
            return false;
        ++at;
        return true;
    }

    // Four hex digits from position from.
    bool Hex(size_t from, uint32_t& code) const
    {
        if (from + 4 > text.size())
            return false;
        code = 0;
        for (size_t i = from; i < from + 4; ++i)
        {
            const char c = text[i];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0)
                return false;
            code = (code << 4) | static_cast<uint32_t>(digit);
        }
        return true;
    }

    static void AppendUtf8(std::string& text, uint32_t code)
    {
        if (code < 0x80)
            text += static_cast<char>(code);
        else if (code < 0x800)
        {
            text += static_cast<char>(0xC0 | (code >> 6));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            text += static_cast<char>(0xE0 | (code >> 12));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            text += static_cast<char>(0xF0 | (code >> 18));
            text += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::string_view text;
    size_t at = 0;
    int depth = 0;
};

// The first string named key in json, at any depth, such as the "evt" of a message or the "message" of an error.
// Empty when there is none.
inline std::string FindString(std::string_view json, std::string_view key)
{
    Reader reader(json);
    std::string found;
    const auto search = [&](const auto& self) -> bool {
        if (reader.Peek('{'))
            return reader.Object([&](const std::string& name) {
                if (found.empty() && name == key && reader.Peek('"'))
                    return reader.String(&found);
                return self(self);
            });
        if (reader.Peek('['))
            return reader.Array([&] { return self(self); });
        return reader.Skip();
    };
    search(search);
    return found;
}

// A game Discord detects by an executable, and the address of its icon.
struct GameIcon
{
    // In lowercase after a slash, with as much of its folder as Discord needs to tell games apart, such as
    // /_retail_/wow.exe.
    std::string executable;
    std::string url;
};

inline std::string LowercasePath(std::string path)
{
    for (char& c : path)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return path;
}

// The icons in Discord's list of the games it detects, from https://discord.com/api/v9/applications/detectable.
// Empty when the list cannot be read.
inline std::vector<GameIcon> ParseGameIcons(std::string_view list)
{
    // The id and the hash go into an address, so they may only hold digits.
    const auto only = [](const std::string& text, std::string_view digits) {
        return !text.empty() && text.find_first_not_of(digits) == std::string::npos;
    };
    std::vector<GameIcon> icons;
    Reader reader(list);
    const bool ok = reader.Array([&] {
        std::string id, hash;
        std::vector<std::string> executables;
        const bool read = reader.Object([&](const std::string& key) {
            if (key == "id" && reader.Peek('"'))
                return reader.String(&id);
            if (key == "icon_hash" && reader.Peek('"'))
                return reader.String(&hash);
            if (key == "executables" && reader.Peek('['))
                return reader.Array([&] {
                    return reader.Object([&](const std::string& field) {
                        if (field != "name" || !reader.Peek('"'))
                            return reader.Skip();
                        return reader.String(&executables.emplace_back());
                    });
                });
            return reader.Skip();
        });
        if (read && only(id, "0123456789") && only(hash, "0123456789abcdef"))
            for (const std::string& name : executables)
                // A name starting with > matches a command line instead, such as >java. No file name holds control
                // characters, which would break the lines FormatGameIcons writes.
                if (!name.empty() && name[0] != '>' && std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
                    icons.push_back({ "/" + LowercasePath(name), "https://cdn.discordapp.com/app-icons/" + id + "/" + hash + ".png?size=512" });
        return read;
    });
    if (!ok)
        icons.clear();
    return icons;
}

// The icons as lines of an executable, a tab and an address, kept between runs.
inline std::string FormatGameIcons(const std::vector<GameIcon>& icons)
{
    std::string text;
    for (const GameIcon& icon : icons)
        text += icon.executable + "\t" + icon.url + "\n";
    return text;
}

// The icons FormatGameIcons wrote, without lines that are not one.
inline std::vector<GameIcon> ReadGameIcons(std::string_view text)
{
    std::vector<GameIcon> icons;
    while (!text.empty())
    {
        const size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        const size_t tab = line.find('\t');
        if (tab != std::string_view::npos && tab > 0 && tab + 1 < line.size())
            icons.push_back({ std::string(line.substr(0, tab)), std::string(line.substr(tab + 1)) });
    }
    return icons;
}

// The icon of the first game in icons whose executable the path ends with. Empty when there is none.
inline std::string FindGameIcon(const std::vector<GameIcon>& icons, const std::string& path)
{
    const std::string lowercase = "/" + LowercasePath(path);
    for (const GameIcon& icon : icons)
        if (lowercase.ends_with(icon.executable))
            return icon.url;
    return {};
}
} // namespace discord
