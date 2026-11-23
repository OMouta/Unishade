#pragma once

#include "base/ini_text.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <string_view>

// The settings of the ReShade host Unishade 0.6.3 and earlier were, which Unishade.ini starts from when it is made in a
// folder that has them. That host kept its own in RobloxShadeHost.ini and ReShade's in ReShade.ini.

// RobloxShadeHost.ini as UTF-8. The ReShade host wrote it in UTF-16 after a byte order mark, in the system's code page
// before that, and Notepad may have saved it as UTF-8.
inline std::string OldHostIniText(const std::string& bytes)
{
    if (bytes.size() >= 2 && bytes[0] == '\xFF' && bytes[1] == '\xFE')
    {
        const std::wstring_view text(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        std::string utf8(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
        return utf8;
    }
    if (bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0))
        return bytes;
    std::wstring wide(MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), static_cast<int>(wide.size()));
    std::string utf8(WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    return utf8;
}

// Unishade.ini's text with what carries over from RobloxShadeHost.ini, as UTF-8, and ReShade.ini: the shortcuts, the
// menu's switches and size, each game's preset and performance settings, the preset in use, and where effects are.
// Depth detail and ReShade's own settings do not carry over.
inline std::string CarryOldSettings(const std::string& hostIni, const std::string& reshadeIni)
{
    IniText settings("");
    const auto lower = [](std::string text) {
        for (char& c : text)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return text;
    };
    const auto trim = [](std::string text) {
        text.erase(0, text.find_first_not_of(" \t\r"));
        text.erase(text.find_last_not_of(" \t\r") + 1);
        return text;
    };

    std::string header, section; // as written, and in lower case
    size_t start = hostIni.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (start < hostIni.size())
    {
        const size_t end = std::min(hostIni.find('\n', start), hostIni.size());
        const std::string line = trim(hostIni.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && line[0] == '[')
        {
            header = trim(line.substr(1, line.find(']') - 1));
            section = lower(header);
            continue;
        }
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == ';' || equals == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string name = lower(key);
        const std::string value = trim(line.substr(equals + 1));
        if (section == "input")
            settings.Set("INPUT", key, value);
        else if (section == "menu" && name == "scale")
            settings.Set("HOST", "MenuScale", value);
        else if (section == "menu" && (name == "autosavepresets" || name == "keepeffectsvisible" || name == "discordpresence" || name == "checkforupdates"))
            settings.Set("HOST", key, value);
        else if ((section == "performance" || section.rfind("performance.", 0) == 0) && (name == "frameratelimit" || name == "effectresolution"))
            settings.Set("PERFORMANCE" + header.substr(11), key, value);
        // Relative to the presets folder there, and to the data folder here. The ReShade host used no other.
        else if (section == "gamepresets" && value.find(':') == std::string::npos && value.find("..") == std::string::npos &&
                 value[0] != '\\' && value[0] != '/' && lower(value).ends_with(".ini"))
            settings.Set("GAMEPRESETS", key, "presets\\" + value);
    }

    const IniText reshade(reshadeIni);
    std::string value;
    for (const char* key : { "EffectSearchPaths", "TextureSearchPaths", "PresetPath", "PreprocessorDefinitions" })
        if (reshade.Get("GENERAL", key, value))
            settings.Set("GENERAL", key, value);
    return settings.Changed() ? settings.Text() : std::string();
}
