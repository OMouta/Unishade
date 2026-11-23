#include "addon_library.h"
#include "config.h"
#include "gpu.h"
#include "ini_text.h"
#include "log.h"
#include "setup.h"

#ifdef _WIN32
#include <unknwn.h>
#include "net.h"
#include "text.h"
#else
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include <vulkan/vulkan_metal.h>
#elif !defined(_WIN32)
#include <sys/sysmacros.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>

// What the build puts into the program of the official add-ons: addons/official.ini, with the SHA-256 of the
// add-ons this build made, and each add-on's addon.ini and icon, in the same order, each ending with an entry that is
// no add-on's.
extern const char kOfficialAddons[];
extern const char* const kOfficialManifests[];
extern const unsigned char* const kOfficialIcons[];
extern const unsigned kOfficialIconSizes[];

using addon::CloseLibrary;
using addon::kFrameFlags;
using addon::kWrites;
using addon::SitsOutHdr;
using addon::ClearPresses;
using addon::HostContext;

namespace
{
#ifndef _WIN32
constexpr uintmax_t kAddonLimit = 1ull << 30; // as net.h has it for Windows
#endif
// What an add-on and each of its settings have to say: what version 1 of the interface has. Later members are optional.
constexpr size_t kSmallestAddon = offsetof(UnishadeAddon, draw) + sizeof(void*);
constexpr size_t kSmallestSetting = offsetof(UnishadeSetting, units) + sizeof(char*);
// Where Unishade.ini keeps the settings of an add-on that leaves the picture alone: this, then the add-on's Id.
const std::string kSettingsSection = "ADDON:";
// On the tile of an add-on that sits out an HDR picture.
constexpr char kSitsOutHdr[] = "It doesn't take HDR pictures, so it's off while the game's picture is HDR.";

fs::path AddonsDirectory()
{
    return DataDirectory() / "Addons";
}

// The official add-ons that have a library for this system, and a list of files with it.
std::vector<Addons::Official> ReadOfficialList()
{
    const std::vector<addon::Files> lists = addon::ReadFiles(kOfficialAddons, addon::Platform());
    std::vector<Addons::Official> list;
    for (size_t i = 0; kOfficialManifests[i]; ++i)
    {
        Addons::Official official;
        official.manifestText = kOfficialManifests[i];
        if (!addon::ReadManifest(official.manifestText, addon::Platform(), official.manifest).empty() || official.manifest.library.empty())
            continue;
        official.icon = { kOfficialIcons[i], kOfficialIconSizes[i] };
        for (const addon::Files& files : lists)
            if (files.id == official.manifest.id)
                official.files = files.files;
        for (const addon::Download& file : official.files)
            if (file.path == official.manifest.library)
                official.sha256 = file.sha256;
        if (!official.sha256.empty())
            list.push_back(std::move(official));
    }
    return list;
}

bool IsColor(UnishadeSettingType type)
{
    return type == UNISHADE_SETTING_COLOR_RGB || type == UNISHADE_SETTING_COLOR_RGBA;
}

// As presets keep it: a color's values separated by commas, as effects' colors are.
std::string FormatValue(const Addons::Setting& setting)
{
    std::string text;
    for (size_t i = 0; i < setting.count; ++i)
    {
        const float value = setting.value[i];
        text += (i ? "," : "") + (setting.type == UNISHADE_SETTING_SLIDER || IsColor(setting.type) ? std::to_string(value)
                                                                                                  : std::to_string(static_cast<int>(std::lround(value))));
    }
    return text;
}

// Value i of a setting, as it may be.
float ValidValue(const Addons::Setting& setting, float value, size_t i = 0)
{
    switch (setting.type)
    {
    case UNISHADE_SETTING_TOGGLE:
        return value != 0 ? 1.0f : 0.0f;
    case UNISHADE_SETTING_CHOICE:
        return std::clamp(std::round(value), 0.0f, std::max(0.0f, static_cast<float>(setting.choices.size()) - 1));
    case UNISHADE_SETTING_INTEGER:
        return std::clamp(std::round(value), std::round(setting.min), std::round(setting.max));
    case UNISHADE_SETTING_COLOR_RGB:
    case UNISHADE_SETTING_COLOR_RGBA:
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : setting.defaultValue[i];
    case UNISHADE_SETTING_BUTTON:
        return 0;
    default:
        return std::isfinite(value) ? std::clamp(value, setting.min, setting.max) : setting.defaultValue[0];
    }
}

// Reads what FormatValue wrote. Values it does not have take their defaults.
void ParseValue(Addons::Setting& setting, const std::string& text)
{
    size_t start = 0;
    for (size_t i = 0; i < setting.count; ++i)
    {
        setting.value[i] = start < text.size() ? ValidValue(setting, std::strtof(text.c_str() + start, nullptr), i) : setting.defaultValue[i];
        start = std::min(text.find(',', start), text.size()) + 1;
    }
}

#ifdef _WIN32
void* OpenLibrary(const fs::path& path, std::string& error)
{
    // Windows would otherwise show a box of its own for a file it cannot load, and wait until it is closed.
    DWORD mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &mode);
    // The add-on's own imports resolve from its folder first.
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD code = GetLastError();
    SetThreadErrorMode(mode, nullptr);
    if (!module)
        error = "Windows could not load it (error " + std::to_string(code) + ").";
    return module;
}

void* LibrarySymbol(void* module, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}

#else
void* OpenLibrary(const fs::path& path, std::string& error)
{
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
        error = dlerror();
    return module;
}

void* LibrarySymbol(void* module, const char* name)
{
    return dlsym(module, name);
}

#endif

} // namespace

void addon::CloseLibrary(void* module)
{
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
}

void Addons::Init(fx::Runtime& effects, AddonHost& app)
{
    runtime = &effects;
    host = &app;
    told = app.DescribeForAddons();
    effectsGeneration = effects.EffectsGeneration();
    official = ReadOfficialList();
    for (const Official& addon : official)
        Log(LogLevel::Info, "Official add-on: %s %s, %s.", addon.manifest.id.c_str(), addon.manifest.version.c_str(), addon.manifest.library.c_str());
    std::error_code error;
    fs::create_directories(AddonsDirectory(), error);
    Scan();
    Update();
}

void Addons::Scan()
{
    struct Found
    {
        std::string folder;
        addon::Manifest manifest;
        std::string problem;
    };
    std::vector<Found> found;
    std::error_code error;
    for (fs::directory_iterator it(AddonsDirectory(), error), end; !error && it != end; it.increment(error))
    {
        const fs::path manifest = it->path() / addon::kManifestFile;
        if (!it->is_directory(error) || !fs::is_regular_file(manifest, error))
            continue;
        Found entry{ it->path().filename().string(), {}, {} };
        entry.problem = addon::ReadManifest(ReadFile(manifest), addon::Platform(), entry.manifest);
        found.push_back(std::move(entry));
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.folder < b.folder; });
    // Of two with the same Id, the first keeps it.
    for (size_t i = 0; i < found.size(); ++i)
        for (size_t other = 0; other < i && found[i].problem.empty(); ++other)
            if (found[other].problem.empty() && found[other].manifest.id == found[i].manifest.id)
                found[i].problem = "The add-on in the folder " + found[other].folder + " has the same Id, " + found[i].manifest.id + ".";
    const auto has = [&](const std::string& folder) {
        return std::any_of(found.begin(), found.end(), [&](const Found& entry) { return entry.folder == folder; });
    };

    for (Addon& addon : addons)
        if (!has(addon.folder))
        {
            Log(LogLevel::Info, "Add-on removed: %s.", addon.folder.c_str());
            Stop(addon, true);
        }
    std::erase_if(addons, [&](const Addon& addon) { return !has(addon.folder); });

    for (Found& entry : found)
    {
        Addon* known = Find(entry.folder);
        // A loaded library stays as it was loaded.
        if (known && known->library->module)
            continue;
        const bool added = !known;
        if (added)
        {
            addons.push_back({ entry.folder });
            addons.back().library = std::make_shared<Library>();
            known = &addons.back();
        }
        const bool unusable = !known->problem.empty();
        const fs::path folder = AddonsDirectory() / fs::path(entry.folder);
        known->manifest = std::move(entry.manifest);
        known->problem = std::move(entry.problem);
        known->official = false;
        known->replaceable = false;
        known->icon.clear();
        if (!known->manifest.icon.empty() && fs::is_regular_file(folder / fs::path(known->manifest.icon), error))
            known->icon = (folder / fs::path(known->manifest.icon)).string();
        if (!known->problem.empty())
        {
            // Nothing is kept for an add-on that does not say which it is.
            known->manifest.id.clear();
            if (known->manifest.name.empty())
                known->manifest.name = entry.folder;
        }
        else if (known->manifest.library.empty())
            known->problem = "It isn't made for this system.";
        else if (known->manifest.api > UNISHADE_API_VERSION)
            known->problem = "It was built for version " + std::to_string(known->manifest.api) + " of Unishade's add-ons, and this Unishade has versions 1 to " +
                             std::to_string(UNISHADE_API_VERSION) + ". Update Unishade to use it.";
        else if (!fs::is_regular_file(folder / fs::path(known->manifest.library), error))
            known->problem = "Its library, " + known->manifest.library + ", is missing.";
        else if (const Official* listed = OfficialFor(known->manifest.id))
        {
            known->official = Sha256(ReadFile(folder / fs::path(known->manifest.library))) == listed->sha256;
            known->replaceable = !known->official;
        }
        if (added && known->problem.empty())
            Log(LogLevel::Info, "Add-on found: %s %s in %s, official=%d.", known->manifest.id.c_str(), known->manifest.version.c_str(),
                entry.folder.c_str(), known->official);
        else if (added)
            Log(LogLevel::Info, "The add-on in %s cannot be used: %s", entry.folder.c_str(), known->problem.c_str());
        // Update starts the ones that are wanted, and fails the ones that cannot be used.
        if (known->state == State::Pending || known->state == State::Disabled || (unusable && known->problem.empty()))
            known->state = !known->official && Answer(*known) < 0 ? State::Pending : State::Disabled;
        if (added && known->state == State::Pending && known->problem.empty())
            Report(LogLevel::Info, "%s in the Addons folder isn't an official add-on. It loads once you allow it on the Add-ons page.",
                   known->manifest.name.c_str());
    }
    changed = true;
}

int Addons::Answer(const Addon& addon) const
{
    if (!answersRead)
    {
        // Every key of [ADDONS] is an Id, which the add-ons found are looked up by.
        answersRead = true;
        answersText = ReadFile(DataDirectory() / "Unishade.ini");
    }
    if (addon.manifest.id.empty())
        return -1;
    const auto known = answers.find(addon.manifest.id);
    if (known != answers.end())
        return known->second;
    std::string value;
    const int answer = !IniText(answersText).Get("ADDONS", addon.manifest.id, value) ? -1 : value == "1" ? 1 : 0;
    answers[addon.manifest.id] = answer;
    return answer;
}

void Addons::SetAnswer(const Addon& addon, bool load)
{
    if (addon.manifest.id.empty())
        return;
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set("ADDONS", addon.manifest.id, load ? "1" : "0");
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
    answers[addon.manifest.id] = load ? 1 : 0;
    changed = true;
}

Addons::Addon* Addons::Find(const std::string& folder)
{
    for (Addon& addon : addons)
        if (addon.folder == folder)
            return &addon;
    return nullptr;
}

const Addons::Official* Addons::OfficialFor(const std::string& id) const
{
    for (const Official& entry : official)
        if (entry.manifest.id == id)
            return &entry;
    return nullptr;
}

std::string Addons::FolderOf(const Official& addon) const
{
    for (const Addon& installed : addons)
        if (installed.manifest.id == addon.manifest.id)
            return installed.folder;
    return addon.manifest.id;
}

void Addons::Approve(const std::string& folder, bool load)
{
    Addon* addon = Find(folder);
    if (!addon || addon->state != State::Pending)
        return;
    Log(LogLevel::Info, "Add-on %s: the user said to %s.", addon->manifest.id.c_str(), load ? "load it" : "leave it");
    addon->state = State::Disabled;
    SetAnswer(*addon, load);
}

void Addons::SetEnabled(const std::string& folder, bool enabled)
{
    Addon* addon = Find(folder);
    if (!addon)
        return;
    if (addon->state == State::Failed || addon->state == State::Pending)
        addon->state = State::Disabled;
    SetAnswer(*addon, enabled);
    changed = true;
}

void Addons::Pause(const std::string& folder)
{
    paused.insert(folder);
    changed = true;
}

void Addons::Resume(const std::string& folder)
{
    paused.erase(folder);
    Scan();
}

bool Addons::Loaded(const std::string& folder) const
{
    return std::any_of(addons.begin(), addons.end(), [&](const Addon& addon) { return addon.folder == folder && addon.library->module; });
}

void Addons::SetSetting(const std::string& folder, size_t index, const float* values, bool tell)
{
    Addon* addon = Find(folder);
    if (!addon || index >= addon->settings.size())
        return;
    Setting& setting = addon->settings[index];
    std::vector<float>& handed = addon->library->values;
    // A press is only for the add-on's next frame or update call, which clears it.
    if (setting.type == UNISHADE_SETTING_BUTTON)
    {
        if (index * 4 < handed.size())
            handed[index * 4] = 1;
        return;
    }
    for (size_t i = 0; i < setting.count; ++i)
    {
        setting.value[i] = ValidValue(setting, values[i], i);
        if (index * 4 + i < handed.size())
            handed[index * 4 + i] = setting.value[i];
    }
    if (ChangesPicture(*addon))
        WritePreset(*addon);
    else
    {
        const fs::path path = DataDirectory() / "Unishade.ini";
        IniText ini(ReadFile(path));
        ini.Set(kSettingsSection + addon->manifest.id, setting.key, FormatValue(setting));
        if (!WriteFile(path, ini.Text()))
            Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
    }
    if (tell)
        Tell(*addon, UNISHADE_EVENT_SETTING_CHANGED, static_cast<uint32_t>(index), setting.key);
}

void Addons::WritePreset(const Addon& addon)
{
    // Addons lists the ones on in the preset by Id, as Techniques does for effects. One it does not name is off.
    // A list the preset does not have is not started empty.
    const std::string was = runtime->PresetValue("", "Addons");
    if (!addon.bypassed || !was.empty())
        runtime->SetPresetValue("", "Addons", addon.bypassed ? PresetIni::Without(was, addon.manifest.id) : PresetIni::With(was, addon.manifest.id));
    for (const Setting& setting : addon.settings)
        if (setting.type != UNISHADE_SETTING_BUTTON)
            runtime->SetPresetValue(addon.manifest.id, setting.key, FormatValue(setting));
}

std::vector<std::string> Addons::Missing() const
{
    std::vector<std::string> missing;
    for (const std::string& id : PresetIni::Split(runtime->PresetValue("", "Addons")))
    {
        const auto installed = std::find_if(addons.begin(), addons.end(), [&](const Addon& addon) { return PresetIni::Same(addon.manifest.id, id); });
        if (installed != addons.end() && installed->state == State::Enabled)
            continue;
        const Official* listed = OfficialFor(Lowercase(id));
        missing.push_back(installed != addons.end() ? installed->manifest.name : listed ? listed->manifest.name : id);
    }
    return missing;
}

void Addons::ReadSettings(Addon& addon)
{
    Library& library = *addon.library;
    library.values.assign(addon.settings.size() * 4, 0.0f);
    const bool look = ChangesPicture(addon);
    if (look)
        addon.bypassed = !PresetIni::Listed(runtime->PresetValue("", "Addons"), addon.manifest.id);
    const IniText ini(look ? std::string() : ReadFile(DataDirectory() / "Unishade.ini"));
    for (size_t i = 0; i < addon.settings.size(); ++i)
    {
        Setting& setting = addon.settings[i];
        // Nothing keeps a button, which takes its default, 0.
        std::string text;
        if (setting.type != UNISHADE_SETTING_BUTTON && look)
            text = runtime->PresetValue(addon.manifest.id, setting.key);
        else if (setting.type != UNISHADE_SETTING_BUTTON)
            ini.Get(kSettingsSection + addon.manifest.id, setting.key, text);
        ParseValue(setting, text);
        std::copy(setting.value, setting.value + setting.count, library.values.begin() + i * 4);
    }
}

void Addons::Load(Addon& addon)
{
    Library& library = *addon.library;
    const fs::path path = AddonsDirectory() / fs::path(addon.folder) / fs::path(addon.manifest.library);
    std::string error;
    library.module = OpenLibrary(path, error);
    if (!library.module)
        return Fail(addon, error);
    const auto refuse = [&](const std::string& reason) {
        CloseLibrary(std::exchange(library.module, nullptr));
        Fail(addon, reason);
    };
    const auto get = reinterpret_cast<UnishadeGetAddonFunction>(LibrarySymbol(library.module, "UnishadeGetAddon"));
    const UnishadeAddon* given = get ? get() : nullptr;
    if (!given)
        return refuse("It is not a Unishade add-on: it has no UnishadeGetAddon.");
    if (given->size < kSmallestAddon)
        return refuse("It is not an add-on this Unishade can use: what it says of itself is too short.");
    // A newer add-on's later members are left out, and an older one's are empty.
    UnishadeAddon& described = library.described;
    described = {};
    std::memcpy(&described, given, std::min<size_t>(given->size, sizeof(described)));
    if (described.api < 1 || described.api > UNISHADE_API_VERSION)
        return refuse("It was built for version " + std::to_string(described.api) + " of Unishade's add-ons, and this Unishade has versions 1 to " +
                      std::to_string(UNISHADE_API_VERSION) + ".");
    if (!described.create || !described.destroy || ((described.flags & kFrameFlags) && !described.frame) ||
        (described.settingCount && (!described.settings || described.settingSize < kSmallestSetting)))
        return refuse("It does not say what to call.");
    library.addon = &described;
    addon.settings.clear();
    for (uint32_t i = 0; i < described.settingCount; ++i)
    {
        // Each is as long as the add-on's header had it.
        UnishadeSetting declared{};
        std::memcpy(&declared, reinterpret_cast<const char*>(described.settings) + size_t(i) * described.settingSize,
                    std::min<size_t>(described.settingSize, sizeof(declared)));
        Setting setting;
        setting.key = declared.key ? declared.key : "";
        setting.label = declared.label ? declared.label : setting.key;
        setting.tooltip = declared.tooltip ? declared.tooltip : "";
        setting.group = declared.group ? declared.group : "";
        setting.units = declared.units ? declared.units : "";
        setting.type = declared.type;
        setting.min = declared.min;
        setting.max = declared.max;
        for (const char* choice = declared.choices; choice && *choice; choice += std::strlen(choice) + 1)
            setting.choices.emplace_back(choice);
        setting.count = setting.type == UNISHADE_SETTING_COLOR_RGBA ? 4 : setting.type == UNISHADE_SETTING_COLOR_RGB ? 3 : 1;
        for (size_t c = 0; c < setting.count; ++c)
            setting.defaultValue[c] = ValidValue(setting, declared.defaultValue[c], c);
        addon.settings.push_back(std::move(setting));
    }
    ReadSettings(addon);
    Log(LogLevel::Info, "Add-on loaded: %s from %s, api=%u, flags=%u, settings=%u.", addon.manifest.id.c_str(), path.string().c_str(), described.api,
        described.flags, described.settingCount);
}

void Addons::Start(Addon& addon)
{
    Library& library = *addon.library;
    if (!library.module)
        Load(addon);
    if (!library.addon)
        return;
#ifdef __APPLE__
    if ((library.addon->flags & kFrameFlags) && !gpu.metalObjects)
        return Fail(addon, "It works on the game's picture, which needs Vulkan's VK_EXT_metal_objects, and this MoltenVK does not have it.");
#elif !defined(_WIN32)
    if ((library.addon->flags & kFrameFlags) && !gpu.dmaBuf)
        return Fail(addon, "It works on the game's picture, which needs the graphics card to share images as dma-bufs, and its Vulkan driver cannot "
                           "(VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier).");
#endif
    library.folder = (AddonsDirectory() / fs::path(addon.folder)).string() + static_cast<char>(fs::path::preferred_separator);
    library.dataFolder = DataDirectory().string() + static_cast<char>(fs::path::preferred_separator);
    HostContext& context = library.context;
    std::snprintf(context.name, sizeof(context.name), "%s", addon.manifest.name.c_str());
    context.runtime = runtime;
    context.app = this->host;
    context.addons = this;
    context.folder = addon.folder;
    context.unishade = UNISHADE_VERSION;
    UnishadeHost& host = context.host;
    host = {};
    host.size = sizeof(host);
    host.version = UNISHADE_API_VERSION;
    host.unishade = context.unishade.c_str();
    host.folder = library.folder.c_str();
    host.dataFolder = library.dataFolder.c_str();
    ::addon::FillHost(host);
#ifdef _WIN32
    VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &id;
    if (gpu.physicalDevice)
        vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties);
    if (id.deviceLUIDValid)
        std::memcpy(host.adapter, id.deviceLUID, sizeof(host.adapter));
#elif defined(__APPLE__)
    // The registryID of the MTLDevice MoltenVK runs on, asked through the Objective-C runtime.
    const auto exportObjects =
        gpu.device ? reinterpret_cast<PFN_vkExportMetalObjectsEXT>(vkGetDeviceProcAddr(gpu.device, "vkExportMetalObjectsEXT")) : nullptr;
    VkExportMetalDeviceInfoEXT metalDevice{ VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT };
    VkExportMetalObjectsInfoEXT objects{ VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT };
    objects.pNext = &metalDevice;
    if (gpu.metalObjects && exportObjects)
        exportObjects(gpu.device, &objects);
    if (metalDevice.mtlDevice)
    {
        const uint64_t registry =
            reinterpret_cast<uint64_t (*)(id, SEL)>(objc_msgSend)(static_cast<id>(metalDevice.mtlDevice), sel_registerName("registryID"));
        std::memcpy(host.adapter, &registry, sizeof(host.adapter));
    }
#elif defined(VK_EXT_physical_device_drm)
    // The device number of the render node, where Vulkan says which it is.
    uint32_t count = 0;
    if (gpu.physicalDevice)
        vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    if (gpu.physicalDevice)
        vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &count, extensions.data());
    extensions.resize(count);
    if (std::any_of(extensions.begin(), extensions.end(),
                    [](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME); }))
    {
        VkPhysicalDeviceDrmPropertiesEXT drm{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &drm;
        vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties);
        if (drm.hasRender)
        {
            const uint64_t node = makedev(static_cast<unsigned>(drm.renderMajor), static_cast<unsigned>(drm.renderMinor));
            std::memcpy(host.adapter, &node, sizeof(host.adapter));
        }
    }
#endif

    char error[512] = "";
    library.instance = library.addon->create(&host, error, sizeof(error));
    if (!library.instance)
        return Fail(addon, error[0] ? error : "It did not start, and did not say why.");
    addon.state = State::Enabled;
    addon.error.clear();
    Log(LogLevel::Info, "Add-on started: %s.", addon.manifest.id.c_str());
    // What there already is, which the others were told of as it came.
    if (told.playing)
        Tell(addon, UNISHADE_EVENT_GAME_STARTED, 0, told.gameName);
    if (!runtime->PresetPath().empty())
        Tell(addon, UNISHADE_EVENT_PRESET_LOADED, 0, runtime->PresetPath().string());
}

void Addons::Tell(Addon& addon, UnishadeEventType type, uint32_t value, const std::string& text)
{
    const Library& library = *addon.library;
    if (addon.state != State::Enabled || !library.instance || library.stopping || !library.addon->event)
        return;
    const UnishadeEvent event{ sizeof(UnishadeEvent), type, value, text.c_str() };
    library.addon->event(library.instance, &event);
}

void Addons::Tell(UnishadeEventType type, uint32_t value, const std::string& text)
{
    for (Addon& addon : addons)
        Tell(addon, type, value, text);
}

bool Addons::HasControls(const Addon& addon) const
{
    return addon.library && addon.library->addon && addon.library->addon->ui;
}

const std::vector<Addons::Control>& Addons::Controls(const std::string& folder)
{
    static const std::vector<Control> none;
    Addon* addon = Find(folder);
    if (!addon || addon->state != State::Enabled || !addon->library->instance || addon->library->stopping || !HasControls(*addon))
        return none;
    Library& library = *addon->library;
    library.controls.clear();
    ::addon::DrawControls(library);
    // What was done with a control the add-on no longer draws is done with nothing.
    library.used.clear();
    return library.controls;
}

void Addons::UseControl(const std::string& folder, size_t control, const float* values)
{
    Addon* addon = Find(folder);
    if (!addon || control >= addon->library->controls.size())
        return;
    std::array<float, 4> used{};
    std::copy(values, values + 4, used.begin());
    addon->library->used[addon->library->controls[control].id] = used;
}

void Addons::Draw(UnishadeDraw& draw)
{
    for (Addon& addon : addons)
    {
        const Library& library = *addon.library;
        if (addon.state != State::Enabled || addon.bypassed || !library.instance || library.stopping || !library.addon->draw)
            continue;
        draw.settings = library.Values();
        draw.time = told.time;
        library.addon->draw(library.instance, &draw);
    }
}

bool Addons::ReadsKeys() const
{
    return std::any_of(addons.begin(), addons.end(), [](const Addon& addon) {
        return addon.state == State::Enabled && addon.library->addon && (addon.library->addon->flags & UNISHADE_READS_KEYS);
    });
}

void Addons::Stop(Addon& addon, bool unload)
{
    Library& library = *addon.library;
    if (library.instance)
    {
        library.addon->destroy(library.instance);
        library.instance = nullptr;
        Log(LogLevel::Info, "Add-on stopped: %s.", addon.manifest.id.c_str());
    }
    library.stopping = false;
    if (unload && library.module)
    {
        CloseLibrary(library.module);
        library.module = nullptr;
        library.addon = nullptr;
    }
}

void Addons::Fail(Addon& addon, const std::string& error)
{
    addon.state = State::Failed;
    addon.error = error;
    addon.library->stopping = addon.library->instance != nullptr;
    Report(LogLevel::Error, "%s stopped: %s", addon.manifest.name.c_str(), error.c_str());
}

void Addons::Update()
{
    const bool presetChanged = runtime->PresetGeneration() != presetGeneration;
    presetGeneration = runtime->PresetGeneration();
    bool work = std::exchange(changed, false);
    // Which add-ons sit out and which provide DEPTH depends on it.
    work |= runtime->Hdr() != hdr;
    hdr = runtime->Hdr();
    for (Addon& addon : addons)
        if (presetChanged && addon.library->addon)
            ReadSettings(addon);

    // What happened since the last pass.
    const AddonHost::State now = host->DescribeForAddons();
    if (now.playing != told.playing)
        Tell(now.playing ? UNISHADE_EVENT_GAME_STARTED : UNISHADE_EVENT_GAME_STOPPED, 0, now.playing ? now.gameName : told.gameName);
    if (presetChanged)
        Tell(UNISHADE_EVENT_PRESET_LOADED, 0, runtime->PresetPath().string());
    if (runtime->EffectsGeneration() != effectsGeneration)
        Tell(UNISHADE_EVENT_EFFECTS_CHANGED);
    effectsGeneration = runtime->EffectsGeneration();
    if (now.effectsOn != told.effectsOn)
        Tell(UNISHADE_EVENT_EFFECTS_TOGGLED, now.effectsOn);
    if (now.menuOpen != told.menuOpen)
        Tell(UNISHADE_EVENT_MENU_TOGGLED, now.menuOpen);
    told = now;

    for (Addon& addon : addons)
    {
        Library& library = *addon.library;
        const auto update = library.addon ? library.addon->update : nullptr;
        if (update && addon.state == State::Enabled && !addon.bypassed && library.instance && !library.stopping)
        {
            const UnishadeUpdate given{ sizeof(UnishadeUpdate), library.Values(), now.time };
            char error[512] = "";
            if (!update(library.instance, &given, error, sizeof(error)))
                Fail(addon, error[0] ? error : "Its update failed, and it did not say why.");
            ClearPresses(addon);
        }
        work |= library.stopping;
    }
    if (!work)
        return;

    for (Addon& addon : addons)
    {
        const int answer = Answer(addon);
        // One that cannot be used says why, and is not asked about.
        if (!addon.problem.empty())
        {
            addon.state = State::Failed;
            addon.error = addon.problem;
            continue;
        }
        const bool wanted = !paused.count(addon.folder) && (addon.official ? answer != 0 : answer == 1);
        if (addon.library->stopping || (!wanted && addon.library->module))
        {
            if (gpu.device)
                vkDeviceWaitIdle(gpu.device);
            Stop(addon, !wanted);
        }
        if (!wanted)
            addon.state = !addon.official && answer < 0 ? State::Pending : State::Disabled;
        else if (addon.state == State::Disabled)
            Start(addon);
    }
    bool depth = false;
    for (Addon& addon : addons)
    {
        const bool skipped = hdr && addon.state == State::Enabled && SitsOutHdr(addon);
        // Its tile says why it is off meanwhile. Messages say so once it starts to.
        if (skipped != addon.hdrSkipped)
        {
            addon.hdrSkipped = skipped;
            if (skipped)
            {
                addon.error = kSitsOutHdr;
                Report(LogLevel::Info, "%s doesn't take HDR pictures, so it's off while the game's picture is HDR.", addon.manifest.name.c_str());
            }
            else if (addon.state != State::Failed)
                addon.error.clear();
        }
        depth |= addon.state == State::Enabled && !skipped && (addon.library->addon->flags & UNISHADE_WRITES_DEPTH);
    }
    runtime->ProvideDepth(depth);
}

void Addons::Install(const Official& addon, const std::string& folder, const std::atomic<bool>& cancel,
                     const std::function<void(size_t file, uint64_t received, uint64_t total)>& progress)
{
    // Each file downloads under another name, and they take their places once all of them are whole and checked, so a
    // failed install leaves what was there. The manifest is written last: a folder is an add-on once it has one.
    const fs::path target = AddonsDirectory() / fs::path(folder);
    std::vector<fs::path> parts;
    try
    {
        for (size_t i = 0; i < addon.files.size(); ++i)
        {
            const ::addon::Download& file = addon.files[i];
            const fs::path path = target / fs::path(file.path);
            fs::create_directories(path.parent_path());
            fs::path part = path;
            part += ".part";
            parts.push_back(part);
#ifdef _WIN32
            Download(Wide(file.url), part, file.sha256, kAddonLimit, cancel, [&](uint64_t received, uint64_t total) {
                if (progress)
                    progress(i, received, total);
            });
#else
            // curl does not say how large the file is, so the bar moves on as each file finishes.
            DownloadFile(file.url, part, cancel, kAddonLimit, [&](uintmax_t received) {
                if (progress)
                    progress(i, received, 0);
            });
            if (Sha256(ReadFile(part)) != file.sha256)
                throw std::runtime_error(file.url + " is not the file Unishade expects. Try again later.");
#endif
        }
        for (size_t i = 0; i < addon.files.size(); ++i)
        {
            fs::rename(parts[i], target / fs::path(addon.files[i].path));
            Log(LogLevel::Info, "Installed %s from %s.", (fs::path(folder) / fs::path(addon.files[i].path)).string().c_str(), addon.files[i].url.c_str());
        }
        if (!addon.manifest.icon.empty() && !addon.icon.empty())
        {
            const fs::path icon = target / fs::path(addon.manifest.icon);
            fs::create_directories(icon.parent_path());
            WriteFile(icon, std::string(reinterpret_cast<const char*>(addon.icon.data()), addon.icon.size()));
        }
        if (!WriteFile(target / ::addon::kManifestFile, addon.manifestText))
            throw std::runtime_error("Could not write " + (target / ::addon::kManifestFile).string() + ".");
    }
    catch (...)
    {
        std::error_code ignored;
        for (const fs::path& part : parts)
            fs::remove(part, ignored);
        throw;
    }
}

void Addons::SetBypassed(const std::string& folder, bool bypassed)
{
    Addon* addon = Find(folder);
    if (!addon)
        return;
    addon->bypassed = bypassed;
    if (ChangesPicture(*addon))
        WritePreset(*addon);
}

bool Addons::ChangesPicture(const Addon& addon) const
{
    return addon.library && addon.library->addon && (addon.library->addon->flags & kWrites) != 0;
}
