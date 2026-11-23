#pragma once

// What the files of the add-on host share: an add-on's library as it is loaded, and what Unishade hands it. addons.cpp
// finds, loads and keeps add-ons, addon_host.cpp has the functions an add-on calls, and addon_frames.cpp has add-ons
// work on frames.

#include "addons.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace addon
{
inline constexpr uint32_t kBeforeEffects = UNISHADE_READS_PICTURE | UNISHADE_WRITES_PICTURE | UNISHADE_WRITES_DEPTH;
inline constexpr uint32_t kAfterEffects = UNISHADE_READS_RESULT | UNISHADE_WRITES_RESULT;
inline constexpr uint32_t kFrameFlags = kBeforeEffects | kAfterEffects;
// What makes an add-on part of a preset's look.
inline constexpr uint32_t kWrites = UNISHADE_WRITES_PICTURE | UNISHADE_WRITES_RESULT;

inline bool TakesHdr(const UnishadeAddon& addon)
{
    return (addon.flags & UNISHADE_TAKES_HDR) != 0;
}

// UnishadeHost first, so the host an add-on passes back leads to everything else. What the functions that describe
// something hand out is kept here until the add-on asks again.
struct HostContext
{
    UnishadeHost host;
    char name[128];
    fx::Runtime* runtime;
    AddonHost* app;
    Addons* addons;
    std::string folder; // the add-on's, in the Addons folder
    std::string unishade;
    UnishadeState state;
    std::string gameName, executable, presetName, presetPath;
    std::vector<fs::path> presets;
    UnishadePreset preset;
    std::string listedName, listedPath;
    UnishadeTechnique technique;
    UnishadeUniform uniform;
};

// Sets the functions of a host, in addon_host.cpp.
void FillHost(UnishadeHost& host);
// Has a library's add-on draw its controls into the library's, in addon_host.cpp.
void DrawControls(Addons::Library& library);
// Lets go of a library that was loaded, in addons.cpp.
void CloseLibrary(void* module);
} // namespace addon

struct Addons::Library
{
    void* module = nullptr;
    // What the library says it is, once it loaded: a copy, in which whatever an older add-on does not have is empty.
    const UnishadeAddon* addon = nullptr;
    UnishadeAddon described{};
    void* instance = nullptr;
    ::addon::HostContext context{};
    std::string folder;
    std::string dataFolder;
    // Four values for each setting, as frames and updates hand them over.
    std::vector<float> values;
    // The controls the add-on drew last, and what the user did with them since, by label, for its next drawing.
    std::vector<Control> controls;
    std::map<std::string, std::array<float, 4>> used;
    // frame failed, so Update stops it
    bool stopping = false;

    const float (*Values() const)[4] { return reinterpret_cast<const float (*)[4]>(values.data()); }

    ~Library()
    {
        if (module)
            ::addon::CloseLibrary(module);
    }
};

namespace addon
{
// One that writes the picture or the result and takes SDR pictures only, so it sits out an HDR one.
inline bool SitsOutHdr(const Addons::Addon& addon)
{
    const UnishadeAddon* described = addon.library->addon;
    return described && (described->flags & kWrites) && !TakesHdr(*described);
}

// A button press is for one call, of frame or update.
inline void ClearPresses(Addons::Addon& addon)
{
    std::vector<float>& values = addon.library->values;
    for (size_t i = 0; i < addon.settings.size() && i * 4 < values.size(); ++i)
        if (addon.settings[i].type == UNISHADE_SETTING_BUTTON)
            values[i * 4] = 0;
}
} // namespace addon
