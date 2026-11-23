#include "app/old_settings.h"

#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

bool Has(const IniText& ini, const char* section, const char* key, const char* expected)
{
    std::string value;
    return ini.Get(section, key, value) && value == expected;
}

bool Missing(const IniText& ini, const char* section, const char* key)
{
    std::string value;
    return !ini.Get(section, key, value);
}

// As the ReShade host saved it: UTF-16 after a byte order mark.
std::string Utf16(std::wstring_view text)
{
    std::string bytes = "\xFF\xFE";
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return bytes;
}
} // namespace

int main()
{
    bool ok = true;
    const std::string host = OldHostIniText(Utf16(L"[Input]\r\nToggleKey=F8\r\nCompareKey=\r\n"
                                                  L"[Menu]\r\nScale=1.25\r\nAutoSavePresets=0\r\nShowDebugInfo=1\r\nCheckForUpdates=0\r\n"
                                                  L"[Performance]\r\nFrameRateLimit=60\r\nDepthSize=392\r\n"
                                                  L"[Performance.Café Racer]\r\nEffectResolution=75\r\nDepthSize=266\r\n"
                                                  L"[GamePresets]\r\nRoblox=Roblox\\Warm.ini\r\nCafé Racer=Café.ini\r\nOutside=..\\Warm.ini\r\n"
                                                  L"Elsewhere=C:\\Presets\\Warm.ini\r\n"
                                                  L"[Launcher]\r\nPlace=0,0,800,600,0\r\n"));
    const std::string reshade = "[GENERAL]\r\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\**\r\nTextureSearchPaths=.\\reshade-shaders\\Textures\\**\r\n"
                                "PresetPath=.\\presets\\Roblox\\Warm.ini\r\nPreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=0\r\nPerformanceMode=1\r\n"
                                "[INPUT]\r\nKeyOverlay=36,0,0,0\r\n[RENODX-DLSS-preset1]\r\nPassCount=2\r\n";
    const IniText settings(CarryOldSettings(host, reshade));

    ok &= Check(Has(settings, "INPUT", "ToggleKey", "F8"), "shortcuts carry over");
    ok &= Check(Has(settings, "INPUT", "CompareKey", ""), "a shortcut left unassigned stays unassigned");
    ok &= Check(Missing(settings, "INPUT", "KeyOverlay"), "ReShade's own shortcuts stay behind");
    ok &= Check(Has(settings, "HOST", "MenuScale", "1.25"), "the menu's size carries over");
    ok &= Check(Has(settings, "HOST", "AutoSavePresets", "0") && Has(settings, "HOST", "CheckForUpdates", "0"), "the menu's switches carry over");
    ok &= Check(Missing(settings, "HOST", "ShowDebugInfo"), "the debug info switch stays behind");
    ok &= Check(Has(settings, "PERFORMANCE", "FrameRateLimit", "60"), "the default performance settings carry over");
    ok &= Check(Has(settings, "PERFORMANCE.Café Racer", "EffectResolution", "75"), "a game's performance settings carry over");
    ok &= Check(settings.Text().find("[PERFORMANCE.Café Racer]") != std::string::npos, "a game's name keeps its case and characters");
    ok &= Check(Missing(settings, "PERFORMANCE", "DepthSize") && Missing(settings, "PERFORMANCE.Café Racer", "DepthSize"),
                "depth detail stays behind");
    ok &= Check(Has(settings, "GAMEPRESETS", "Roblox", "presets\\Roblox\\Warm.ini"), "a game's preset moves under the presets folder");
    ok &= Check(Has(settings, "GAMEPRESETS", "Café Racer", "presets\\Café.ini"), "names outside ASCII carry over");
    ok &= Check(Missing(settings, "GAMEPRESETS", "Outside") && Missing(settings, "GAMEPRESETS", "Elsewhere"),
                "presets outside the presets folder stay behind");
    ok &= Check(Missing(settings, "Launcher", "Place"), "the launcher's place stays behind");
    ok &= Check(Has(settings, "GENERAL", "EffectSearchPaths", ".\\reshade-shaders\\Shaders\\**") &&
                    Has(settings, "GENERAL", "TextureSearchPaths", ".\\reshade-shaders\\Textures\\**"),
                "where effects are carries over");
    ok &= Check(Has(settings, "GENERAL", "PresetPath", ".\\presets\\Roblox\\Warm.ini"), "the preset in use carries over");
    ok &= Check(Has(settings, "GENERAL", "PreprocessorDefinitions", "RESHADE_DEPTH_INPUT_IS_REVERSED=0"), "definitions carry over");
    ok &= Check(Missing(settings, "GENERAL", "PerformanceMode") && Missing(settings, "RENODX-DLSS-preset1", "PassCount"),
                "ReShade's own settings stay behind");

    ok &= Check(OldHostIniText("[Input]\r\nToggleKey=F8\r\n") == "[Input]\r\nToggleKey=F8\r\n", "text that is UTF-8 stays as it is");
    ok &= Check(CarryOldSettings("", "").empty(), "a folder without either file starts empty");
    const IniText reshadeOnly(CarryOldSettings("", reshade));
    ok &= Check(Has(reshadeOnly, "GENERAL", "PresetPath", ".\\presets\\Roblox\\Warm.ini"), "ReShade.ini carries over on its own");
    return ok ? 0 : 1;
}
