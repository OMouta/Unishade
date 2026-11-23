#include "addons.h"
#include "backdrop.h"
#include "config.h"
#include "gpu.h"
#include "ini_text.h"
#include "log.h"
#include "setup.h"

#ifdef _WIN32
#include <unknwn.h>
#include "net.h"
#include "text.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan_win32.h>
#include <winrt/base.h>
#else
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <CoreVideo/CVPixelBuffer.h>
#include <IOSurface/IOSurfaceRef.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <vulkan/vulkan_metal.h>
#elif !defined(_WIN32)
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>

// What the build puts into the program of the official add-ons: src/addons/official.ini, with the SHA-256 of the
// add-ons this build made, and each add-on's addon.ini and icon, in the same order, each ending with an entry that is
// no add-on's.
extern const char kOfficialAddons[];
extern const char* const kOfficialManifests[];
extern const unsigned char* const kOfficialIcons[];
extern const unsigned kOfficialIconSizes[];

namespace
{
#ifndef _WIN32
constexpr uintmax_t kAddonLimit = 1ull << 30; // as net.h has it for Windows
#endif
constexpr uint32_t kBeforeEffects = UNISHADE_ADDON_WRITES_DEPTH | UNISHADE_ADDON_CHANGES_PICTURE;
constexpr uint32_t kAfterEffects = UNISHADE_ADDON_READS_RESULT | UNISHADE_ADDON_CHANGES_RESULT;
constexpr uint32_t kFrameFlags = kBeforeEffects | kAfterEffects;
// Where Unishade.ini keeps the settings of an add-on that leaves the picture alone: this, then the add-on's Id.
const std::string kSettingsSection = "ADDON:";
// On the tile of an add-on that sits out an HDR picture.
constexpr char kSitsOutHdr[] = "It doesn't take HDR pictures, so it's off while the game's picture is HDR.";

bool TakesHdr(const UnishadeAddon& addon)
{
    return addon.version >= 2 && (addon.flags & UNISHADE_ADDON_TAKES_HDR);
}

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

// Where a setting's values start in the ones frames hand the add-on.
size_t ValueOffset(const Addons::Addon& addon, size_t setting)
{
    size_t offset = 0;
    for (size_t i = 0; i < setting; ++i)
        offset += addon.settings[i].count;
    return offset;
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

void CloseLibrary(void* module)
{
    FreeLibrary(static_cast<HMODULE>(module));
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

void CloseLibrary(void* module)
{
    dlclose(module);
}
#endif

// UnishadeHost first, so the host an add-on passes back to log leads to its name.
struct HostContext
{
    UnishadeHost host;
    char name[128];
    fx::Runtime* runtime;
};

void HostLog(const UnishadeHost* host, UnishadeLogLevel level, const char* text)
{
    const char* name = host ? reinterpret_cast<const HostContext*>(host)->name : "An add-on";
    const LogLevel kind = level == UNISHADE_LOG_ERROR ? LogLevel::Error : level == UNISHADE_LOG_WARNING ? LogLevel::Warning : LogLevel::Info;
    Log(kind, "%s: %s", name, text ? text : "");
}

// The host's effect functions, on fx::Runtime.

fx::Runtime& RuntimeOf(const UnishadeHost* host)
{
    return *reinterpret_cast<const HostContext*>(host)->runtime;
}

fx::Effect* EffectAt(const UnishadeHost* host, uint32_t effect)
{
    std::vector<fx::Effect>& effects = RuntimeOf(host).Effects();
    return effect < effects.size() ? &effects[effect] : nullptr;
}

// The uniforms presets keep, which are the ones the host numbers.
bool Kept(const fx::Uniform& uniform)
{
    return uniform.source.empty() && uniform.type.is_numeric();
}

const fx::Uniform* UniformAt(const UnishadeHost* host, uint32_t effect, uint32_t index, fx::Effect*& owner)
{
    owner = EffectAt(host, effect);
    if (owner)
        for (const fx::Uniform& uniform : owner->uniforms)
            if (Kept(uniform) && index-- == 0)
                return &uniform;
    return nullptr;
}

uint64_t HostEffectsGeneration(const UnishadeHost* host)
{
    return RuntimeOf(host).EffectsGeneration();
}

uint32_t HostTechniqueCount(const UnishadeHost* host)
{
    return static_cast<uint32_t>(RuntimeOf(host).Techniques().size());
}

bool HostGetTechnique(const UnishadeHost* host, uint32_t index, UnishadeTechnique* description)
{
    fx::Runtime& runtime = RuntimeOf(host);
    if (!description || index >= runtime.Techniques().size())
        return false;
    const fx::Technique& technique = runtime.Techniques()[index];
    description->name = technique.name.c_str();
    description->file = runtime.Effects()[technique.effect].file.c_str();
    description->effect = static_cast<uint32_t>(technique.effect);
    description->enabled = technique.enabled;
    return true;
}

void HostSetTechnique(const UnishadeHost* host, uint32_t index, bool enabled)
{
    RuntimeOf(host).SetEnabled(index, enabled);
}

uint32_t HostUniformCount(const UnishadeHost* host, uint32_t effect)
{
    const fx::Effect* owner = EffectAt(host, effect);
    return owner ? static_cast<uint32_t>(std::count_if(owner->uniforms.begin(), owner->uniforms.end(), Kept)) : 0;
}

bool HostGetUniform(const UnishadeHost* host, uint32_t effect, uint32_t index, UnishadeUniform* description)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform || !description)
        return false;
    description->name = uniform->name.c_str();
    description->type = uniform->type.is_boolean()        ? UNISHADE_VALUE_BOOL
                        : uniform->type.is_floating_point() ? UNISHADE_VALUE_FLOAT
                        : uniform->type.is_signed()         ? UNISHADE_VALUE_INT
                                                            : UNISHADE_VALUE_UINT;
    description->count = static_cast<uint32_t>(fx::ComponentCount(*uniform));
    description->min = uniform->min;
    description->max = uniform->max;
    return true;
}

uint32_t HostGetValue(const UnishadeHost* host, uint32_t effect, uint32_t index, float* values, uint32_t count)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return 0;
    if (values)
        RuntimeOf(host).GetValue(*owner, *uniform, values, count);
    return static_cast<uint32_t>(fx::ComponentCount(*uniform));
}

bool HostSetValue(const UnishadeHost* host, uint32_t effect, uint32_t index, const float* values, uint32_t count)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return false;
    if (values && count)
    {
        RuntimeOf(host).SetValue(*owner, *uniform, values, count);
        RuntimeOf(host).SetDirty();
    }
    return true;
}
} // namespace

struct Addons::Library
{
    void* module = nullptr;
    const UnishadeAddon* addon = nullptr;
    void* instance = nullptr;
    HostContext context{};
    std::string folder;
    std::string dataFolder;
    std::vector<float> values;
    // frame failed, so Update stops it
    bool stopping = false;

    ~Library()
    {
        if (module)
            CloseLibrary(module);
    }
};

namespace
{
// One that changes the picture and takes SDR pictures only, so it sits out an HDR one.
bool SitsOutHdr(const Addons::Addon& addon)
{
    const UnishadeAddon* described = addon.library->addon;
    return described && (described->flags & (UNISHADE_ADDON_CHANGES_PICTURE | UNISHADE_ADDON_CHANGES_RESULT)) && !TakesHdr(*described);
}

// A button press is for one call, of frame or update.
void ClearPresses(Addons::Addon& addon)
{
    std::vector<float>& values = addon.library->values;
    for (size_t i = 0, offset = 0; i < addon.settings.size(); offset += addon.settings[i++].count)
        if (addon.settings[i].type == UNISHADE_SETTING_BUTTON && offset < values.size())
            values[offset] = 0;
}

// Makes the image an HDR picture is drawn into as SDR, before it is copied to add-ons.
void MakeConvert(backdrop::Target& target, uint32_t width, uint32_t height)
{
    VkCommandBuffer setup = gpu.BeginCommands();
    if (!setup)
        throw std::runtime_error("The graphics card is out of memory.");
    const bool made = backdrop::Make(setup, target, width, height);
    if (!gpu.SubmitAndWait(setup) || !made)
        throw std::runtime_error("The graphics card could not prepare an SDR picture for add-ons.");
}
} // namespace

// What add-ons share frames through.
#ifdef _WIN32
// Direct3D 11 textures and a fence on the graphics card Vulkan runs on, imported into Vulkan. The fence is a timeline
// semaphore in Vulkan, so neither side waits on the processor. Where Vulkan cannot import it, the processor waits.
struct Addons::Exchange
{
    struct Image
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        HANDLE handle = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    winrt::com_ptr<ID3D11Device5> device;
    winrt::com_ptr<ID3D11DeviceContext4> context;
    winrt::com_ptr<ID3D11Fence> fence;
    HANDLE fenceHandle = nullptr;
    winrt::handle event;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t value = 0; // the fence's last value handed out
    // In the picture's format: BGRA8, or R16G16B16A16_SFLOAT while hdr.
    Image color, output, depth;
    Image after; // what add-ons that change the result write, apart from output, which add-ons before effects keep
    // While hdr: the picture in SDR, BGRA8, for add-ons that do not take HDR. It is drawn in convert, since the images
    // add-ons share can only be copied.
    Image sdr;
    backdrop::Target convert;
    bool hdr = false;
    uint32_t width = 0, height = 0;
    uint64_t generation = 0;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    // Effects, when add-ons work after them as well as before, since they run between the two.
    VkCommandBuffer effectsCommands = VK_NULL_HANDLE;
    VkFence effectsDone = VK_NULL_HANDLE;

    ~Exchange()
    {
        Release();
        if (semaphore)
            vkDestroySemaphore(gpu.device, semaphore, nullptr);
        for (VkFence fence : { done, effectsDone })
            if (fence)
                vkDestroyFence(gpu.device, fence, nullptr);
        for (VkCommandBuffer buffer : { commands, effectsCommands })
            if (buffer)
                vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &buffer);
        if (fenceHandle)
            CloseHandle(fenceHandle);
    }

    void CreateDevice()
    {
        VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties);
        winrt::com_ptr<IDXGIFactory1> factory;
        winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; id.deviceLUIDValid && !adapter; ++i)
        {
            winrt::com_ptr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, candidate.put()) != S_OK)
                break;
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(candidate->GetDesc1(&description)) && !memcmp(&description.AdapterLuid, id.deviceLUID, sizeof(LUID)))
                adapter = candidate;
        }
        if (!adapter)
            throw std::runtime_error("Direct3D has no graphics card that is the one Vulkan draws with, so frames cannot be shared.");
        winrt::com_ptr<ID3D11Device> created;
        winrt::com_ptr<ID3D11DeviceContext> createdContext;
        winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, created.put(),
                                               nullptr, createdContext.put()));
        device = created.as<ID3D11Device5>();
        context = createdContext.as<ID3D11DeviceContext4>();
        winrt::check_hresult(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.put())));
        winrt::check_hresult(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle));
        event.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        value = 0;

        if (!gpu.win32Fences)
        {
            Log(LogLevel::Info, "Add-ons: Vulkan has no timeline semaphores, so the processor waits for add-ons.");
            return;
        }
        VkSemaphoreTypeCreateInfoKHR timeline{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR };
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
        VkPhysicalDeviceExternalSemaphoreInfo info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
        info.pNext = &timeline;
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        VkExternalSemaphoreProperties supported{ VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
        vkGetPhysicalDeviceExternalSemaphoreProperties(gpu.physicalDevice, &info, &supported);
        const auto import = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(gpu.device, "vkImportSemaphoreWin32HandleKHR"));
        VkSemaphoreCreateInfo create{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        create.pNext = &timeline;
        VkImportSemaphoreWin32HandleInfoKHR handle{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        handle.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        handle.handle = fenceHandle;
        if (!(supported.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) || !import ||
            vkCreateSemaphore(gpu.device, &create, nullptr, &semaphore) != VK_SUCCESS || (handle.semaphore = semaphore, import(gpu.device, &handle)) != VK_SUCCESS)
        {
            if (semaphore)
                vkDestroySemaphore(gpu.device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
            Log(LogLevel::Info, "Add-ons: Vulkan cannot use a Direct3D fence, so the processor waits for add-ons.");
        }
    }

    // Takes an image from Direct3D, or gives it back, keeping the general layout and the contents.
    static void Transfer(VkCommandBuffer commands, VkImage image, bool acquire)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = acquire ? 0 : VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = acquire ? VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT : 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : gpu.queueFamily;
        barrier.dstQueueFamilyIndex = acquire ? gpu.queueFamily : VK_QUEUE_FAMILY_EXTERNAL;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    static void Copy(VkCommandBuffer commands, VkImage from, VkImage to, uint32_t copyWidth, uint32_t copyHeight)
    {
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.extent = { copyWidth, copyHeight, 1 };
        vkCmdCopyImage(commands, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }

    void Create(Image& image, DXGI_FORMAT format, VkFormat vulkanFormat)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc = { 1, 0 };
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        UINT support = 0;
        if (SUCCEEDED(device->CheckFormatSupport(format, &support)) && (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
            desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, image.texture.put()));
        winrt::check_hresult(image.texture.as<IDXGIResource1>()->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                                                     nullptr, &image.handle));

        VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext = &external;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = vulkanFormat;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(gpu.device, &info, nullptr, &image.image) != VK_SUCCESS)
            throw std::runtime_error("Vulkan could not make an image of a Direct3D texture.");
        const auto properties =
            reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(gpu.device, "vkGetMemoryWin32HandlePropertiesKHR"));
        VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(gpu.device, image.image, &requirements);
        uint32_t type = UINT32_MAX;
        if (properties && properties(gpu.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, image.handle, &handleProperties) == VK_SUCCESS)
            type = gpu.FindMemoryType(requirements.memoryTypeBits & handleProperties.memoryTypeBits, 0);
        VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = image.image;
        VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        import.handle = image.handle;
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.pNext = &import;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(gpu.device, &allocation, nullptr, &image.memory) != VK_SUCCESS ||
            vkBindImageMemory(gpu.device, image.image, image.memory, 0) != VK_SUCCESS)
            throw std::runtime_error("Vulkan could not import a Direct3D texture.");

        // Into the general layout once, which every use keeps.
        VkCommandBuffer setup = gpu.BeginCommands();
        if (!setup)
            throw std::runtime_error("The graphics card is out of memory.");
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        barrier.dstQueueFamilyIndex = gpu.queueFamily;
        barrier.image = image.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(setup, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        Transfer(setup, image.image, false);
        if (!gpu.SubmitAndWait(setup))
            throw std::runtime_error("The graphics card could not prepare an image for add-ons.");
        ++generation;
    }

    void Destroy(Image& image)
    {
        if (image.image)
            vkDestroyImage(gpu.device, image.image, nullptr);
        if (image.memory)
            vkFreeMemory(gpu.device, image.memory, nullptr);
        if (image.handle)
            CloseHandle(image.handle);
        image = {};
    }

    void Release()
    {
        if (!color.texture && !output.texture && !depth.texture && !after.texture && !sdr.texture && !convert.framebuffer)
            return;
        vkDeviceWaitIdle(gpu.device);
        for (Image* image : { &color, &output, &depth, &after, &sdr })
            Destroy(*image);
        backdrop::Destroy(convert);
        width = height = 0;
    }

    // A new size or format makes every image again.
    void Prepare(uint32_t newWidth, uint32_t newHeight, bool newHdr, bool wantOutput, bool wantDepth, bool wantAfter, bool wantSdr)
    {
        if (device && FAILED(device->GetDeviceRemovedReason()))
        {
            Release();
            if (semaphore)
                vkDestroySemaphore(gpu.device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
            CloseHandle(std::exchange(fenceHandle, nullptr));
            fence = nullptr;
            context = nullptr;
            device = nullptr;
        }
        if (!device)
        {
            CreateDevice();
            ++generation;
        }
        if (newWidth != width || newHeight != height || newHdr != hdr)
        {
            Release();
            width = newWidth;
            height = newHeight;
            hdr = newHdr;
        }
        const DXGI_FORMAT format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        const VkFormat vulkanFormat = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_UNORM;
        if (!color.texture)
            Create(color, format, vulkanFormat);
        if (wantOutput && !output.texture)
            Create(output, format, vulkanFormat);
        if (wantDepth && !depth.texture)
            Create(depth, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT);
        if (wantAfter && !after.texture)
            Create(after, format, vulkanFormat);
        if (hdr && wantSdr && !sdr.texture)
        {
            MakeConvert(convert, width, height);
            Create(sdr, DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
        }
    }

    // Begins commands or effectsCommands, with the fence that goes with them.
    VkCommandBuffer Begin(VkCommandBuffer& buffer, VkFence& finished)
    {
        if (!buffer)
        {
            VkCommandBufferAllocateInfo info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            info.commandPool = gpu.commandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            VkFenceCreateInfo signaled{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            signaled.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkAllocateCommandBuffers(gpu.device, &info, &buffer) != VK_SUCCESS || vkCreateFence(gpu.device, &signaled, nullptr, &finished) != VK_SUCCESS)
                return VK_NULL_HANDLE;
        }
        // The last frame's copy, which the overlay's last frame waited for.
        if (vkWaitForFences(gpu.device, 1, &finished, VK_TRUE, 2'000'000'000) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(buffer, 0) != VK_SUCCESS || vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        vkResetFences(gpu.device, 1, &finished);
        return buffer;
    }

    // Copies the picture into color and hands it over, in what Begin began, and with an sdrWhite into sdr as SDR too.
    // With a timeline semaphore the commands first wait for the fence to reach wait, unless it is 0. Returns the fence
    // value add-ons wait for, or 0 when nothing was submitted.
    uint64_t Submit(VkCommandBuffer& buffer, VkFence& finished, const GpuImage& picture, uint64_t wait, float sdrWhite)
    {
        Transfer(buffer, color.image, true);
        Copy(buffer, picture.image, color.image, width, height);
        Transfer(buffer, color.image, false);
        if (sdrWhite > 0 && sdr.image)
            CopySdr(buffer, picture, sdrWhite, &buffer == &effectsCommands ? 1 : 0);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &buffer;
        const uint64_t ready = value + 1;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfoKHR timeline{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR };
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &ready;
        if (semaphore)
        {
            submit.pNext = &timeline;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &semaphore;
            if (wait)
            {
                timeline.waitSemaphoreValueCount = 1;
                timeline.pWaitSemaphoreValues = &wait;
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &semaphore;
                submit.pWaitDstStageMask = &stage;
            }
        }
        if (vkEndCommandBuffer(buffer) != VK_SUCCESS || vkQueueSubmit(gpu.queue, 1, &submit, finished) != VK_SUCCESS)
        {
            // Nothing waits for it, and the next Begin cannot wait for it either.
            vkDestroyFence(gpu.device, finished, nullptr);
            vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &buffer);
            finished = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return 0;
        }
        value = ready;
        if (!semaphore)
        {
            if (vkWaitForFences(gpu.device, 1, &finished, VK_TRUE, 2'000'000'000) != VK_SUCCESS)
                return 0;
            winrt::check_hresult(context->Signal(fence.get(), ready));
            context->Flush();
        }
        return ready;
    }

    // The picture as SDR into sdr, drawn in convert first. slot is backdrop::ToSdr's, one for each set of commands.
    void CopySdr(VkCommandBuffer buffer, const GpuImage& picture, float sdrWhite, int slot)
    {
        FullBarrier(buffer);
        backdrop::ToSdr(buffer, picture, convert, sdrWhite, slot);
        Transfer(buffer, sdr.image, true);
        Copy(buffer, convert.image.image, sdr.image, width, height);
        Transfer(buffer, sdr.image, false);
    }

    // Sets the fence to a value for an add-on that could not, after the value before it.
    void Signal(uint64_t after, uint64_t next)
    {
        context->Wait(fence.get(), after);
        context->Signal(fence.get(), next);
        context->Flush();
        value = next;
    }

    // Waits on the processor for the add-ons, for where Vulkan cannot wait for the fence.
    bool WaitHere()
    {
        return SUCCEEDED(fence->SetEventOnCompletion(value, event.get())) && WaitForSingleObject(event.get(), 2000) == WAIT_OBJECT_0;
    }

    // Has the overlay's frame wait for the add-ons, then copies the picture and the depth they wrote, if any, to the
    // effects.
    bool Finish(Surface& surface, const Image* picture, bool copyDepth, const GpuImage& backbuffer, const GpuImage& effectsDepth)
    {
        if (semaphore)
            surface.WaitFor(semaphore, value);
        else if (!WaitHere())
            return false;
        Take(surface.commands, picture, copyDepth, backbuffer, effectsDepth);
        return true;
    }

    // Copies the picture and the depth the add-ons wrote, if any, to the effects, in commands that run after the
    // add-ons are done.
    void Take(VkCommandBuffer frame, const Image* picture, bool copyDepth, const GpuImage& backbuffer, const GpuImage& effectsDepth)
    {
        FullBarrier(frame);
        if (picture)
        {
            Transfer(frame, picture->image, true);
            Copy(frame, picture->image, backbuffer.image, width, height);
            Transfer(frame, picture->image, false);
        }
        if (copyDepth && effectsDepth.width == width && effectsDepth.height == height)
        {
            Transfer(frame, depth.image, true);
            Copy(frame, depth.image, effectsDepth.image, width, height);
            Transfer(frame, depth.image, false);
        }
        FullBarrier(frame);
    }
};
#else
// Images Vulkan makes and hands out: IOSurfaces on macOS, which MoltenVK keeps the images' contents in, and dma-bufs
// on Linux. Nothing shares a fence there, so Unishade's copy is done before an add-on gets the frame, and the add-on's
// work is done when it returns it.
struct Addons::Exchange
{
    struct Image
    {
        void* handle = nullptr; // what add-ons get
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
#ifdef __APPLE__
        IOSurfaceRef surface = nullptr;
#else
        UnishadeDmaBuf dmaBuf{ 0, 0, 0, -1 };
#endif
    };

    // In the picture's format: BGRA8, or R16G16B16A16_SFLOAT while hdr.
    Image color, output, depth;
    Image after; // what add-ons that change the result write, apart from output, which add-ons before effects keep
    // While hdr: the picture in SDR, BGRA8, for add-ons that do not take HDR, drawn in convert first.
    Image sdr;
    backdrop::Target convert;
    bool hdr = false;
    uint32_t width = 0, height = 0;
    uint64_t generation = 0;
    // Counts hand-overs as the fence does on Windows. Nothing waits for it here.
    uint64_t value = 0;
    void* fenceHandle = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    // Effects, when add-ons work after them as well as before, since they run between the two.
    VkCommandBuffer effectsCommands = VK_NULL_HANDLE;
    VkFence effectsDone = VK_NULL_HANDLE;

    ~Exchange()
    {
        Release();
        for (VkFence fence : { done, effectsDone })
            if (fence)
                vkDestroyFence(gpu.device, fence, nullptr);
        for (VkCommandBuffer buffer : { commands, effectsCommands })
            if (buffer)
                vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &buffer);
    }

    // Takes an image back from add-ons, or hands it to them, keeping the general layout and the contents.
    static void Transfer(VkCommandBuffer commands, VkImage image, bool acquire)
    {
        const uint32_t outside = gpu.foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = acquire ? 0 : VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = acquire ? VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT : 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = acquire ? outside : gpu.queueFamily;
        barrier.dstQueueFamilyIndex = acquire ? gpu.queueFamily : outside;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    static void Copy(VkCommandBuffer commands, VkImage from, VkImage to, uint32_t copyWidth, uint32_t copyHeight)
    {
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = region.srcSubresource;
        region.extent = { copyWidth, copyHeight, 1 };
        vkCmdCopyImage(commands, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }

#ifdef __APPLE__
    void Make(Image& image, VkFormat format)
    {
        // CoreVideo's names for the formats, so add-ons can wrap the surfaces in CVPixelBuffers. Rows are aligned as
        // IOSurface aligns them for the graphics card.
        const bool half = format == VK_FORMAT_R16G16B16A16_SFLOAT;
        const int32_t bytes = half ? 8 : 4;
        const int32_t values[] = { int32_t(width), int32_t(height), bytes, int32_t(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, size_t(width) * bytes)),
                                   int32_t(format == VK_FORMAT_R32_SFLOAT ? kCVPixelFormatType_OneComponent32Float
                                           : half                     ? kCVPixelFormatType_64RGBAHalf
                                                                      : kCVPixelFormatType_32BGRA) };
        const void* keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow, kIOSurfacePixelFormat };
        const void* numbers[std::size(values)];
        for (size_t i = 0; i < std::size(values); ++i)
            numbers[i] = CFNumberCreate(nullptr, kCFNumberSInt32Type, &values[i]);
        CFDictionaryRef properties =
            CFDictionaryCreate(nullptr, keys, numbers, CFIndex(std::size(values)), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        for (const void* number : numbers)
            CFRelease(number);
        image.surface = IOSurfaceCreate(properties);
        CFRelease(properties);
        if (!image.surface)
            throw std::runtime_error("macOS could not make an IOSurface for add-ons.");

        VkImportMetalIOSurfaceInfoEXT import{ VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT };
        import.ioSurface = image.surface;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext = &import;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(gpu.device, &info, nullptr, &image.image) != VK_SUCCESS)
            throw std::runtime_error("Vulkan could not make an image of an IOSurface.");
        // Vulkan wants memory bound to every image. MoltenVK keeps using the IOSurface for its contents.
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(gpu.device, image.image, &requirements);
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = gpu.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocation.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.device, &allocation, nullptr, &image.memory) != VK_SUCCESS ||
            vkBindImageMemory(gpu.device, image.image, image.memory, 0) != VK_SUCCESS)
            throw std::runtime_error("The graphics card is out of memory.");
    }
#else
    // The modifiers of one plane that the format can be made, exported and used with at this size.
    std::vector<uint64_t> Modifiers(VkFormat format, VkImageUsageFlags usage, VkFormatFeatureFlags features) const
    {
        VkDrmFormatModifierPropertiesListEXT list{ VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
        VkFormatProperties2 properties{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
        properties.pNext = &list;
        vkGetPhysicalDeviceFormatProperties2(gpu.physicalDevice, format, &properties);
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties = modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(gpu.physicalDevice, format, &properties);
        modifiers.resize(list.drmFormatModifierCount);

        std::vector<uint64_t> usable;
        for (const VkDrmFormatModifierPropertiesEXT& entry : modifiers)
        {
            if (entry.drmFormatModifierPlaneCount != 1 || (entry.drmFormatModifierTilingFeatures & features) != features)
                continue;
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT };
            modifierInfo.drmFormatModifier = entry.drmFormatModifier;
            modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkPhysicalDeviceExternalImageFormatInfo externalInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
            externalInfo.pNext = &modifierInfo;
            externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkPhysicalDeviceImageFormatInfo2 info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
            info.pNext = &externalInfo;
            info.format = format;
            info.type = VK_IMAGE_TYPE_2D;
            info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            info.usage = usage;
            VkExternalImageFormatProperties external{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
            VkImageFormatProperties2 result{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
            result.pNext = &external;
            if (vkGetPhysicalDeviceImageFormatProperties2(gpu.physicalDevice, &info, &result) == VK_SUCCESS &&
                (external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
                result.imageFormatProperties.maxExtent.width >= width && result.imageFormatProperties.maxExtent.height >= height)
                usable.push_back(entry.drmFormatModifier);
        }
        return usable;
    }

    void Make(Image& image, VkFormat format)
    {
        // What add-ons do with the images, and storage too where the driver allows it. The driver picks the layout
        // from the modifiers that allow all of it.
        VkImageUsageFlags usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const VkFormatFeatureFlags features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        std::vector<uint64_t> modifiers = Modifiers(format, usage | VK_IMAGE_USAGE_STORAGE_BIT, features | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if (!modifiers.empty())
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        else
            modifiers = Modifiers(format, usage, features);
        if (modifiers.empty())
            throw std::runtime_error("The graphics card cannot share images of this format as dma-bufs (Vulkan format " + std::to_string(format) + ").");

        VkImageDrmFormatModifierListCreateInfoEXT modifierList{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT };
        modifierList.drmFormatModifierCount = uint32_t(modifiers.size());
        modifierList.pDrmFormatModifiers = modifiers.data();
        VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.pNext = &modifierList;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext = &external;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(gpu.device, &info, nullptr, &image.image) != VK_SUCCESS)
            throw std::runtime_error("Vulkan could not make an image to share as a dma-buf.");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(gpu.device, image.image, &requirements);
        VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = image.image;
        VkExportMemoryAllocateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exportInfo.pNext = &dedicated;
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.pNext = &exportInfo;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = gpu.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocation.memoryTypeIndex == UINT32_MAX)
            allocation.memoryTypeIndex = gpu.FindMemoryType(requirements.memoryTypeBits, 0);
        if (allocation.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.device, &allocation, nullptr, &image.memory) != VK_SUCCESS ||
            vkBindImageMemory(gpu.device, image.image, image.memory, 0) != VK_SUCCESS)
            throw std::runtime_error("The graphics card is out of memory.");

        const auto getFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(gpu.device, "vkGetMemoryFdKHR"));
        const auto getModifier = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(gpu.device, "vkGetImageDrmFormatModifierPropertiesEXT"));
        VkMemoryGetFdInfoKHR fdInfo{ VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
        fdInfo.memory = image.memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        int fd = -1;
        if (!getFd || getFd(gpu.device, &fdInfo, &fd) != VK_SUCCESS)
            throw std::runtime_error("Vulkan could not share an image as a dma-buf.");
        image.dmaBuf.fd = fd;
        VkImageDrmFormatModifierPropertiesEXT chosen{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
        if (!getModifier || getModifier(gpu.device, image.image, &chosen) != VK_SUCCESS)
            throw std::runtime_error("Vulkan did not say how it laid out an image it shares as a dma-buf.");
        // The layout of the one plane, which an image with a modifier has as memory plane 0.
        const VkImageSubresource plane{ VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(gpu.device, image.image, &plane, &layout);
        image.dmaBuf.modifier = chosen.drmFormatModifier;
        image.dmaBuf.offset = layout.offset;
        image.dmaBuf.stride = layout.rowPitch;
    }
#endif

    // Makes an image and hands it to add-ons, in the general layout, which every use keeps. One that failed part way
    // is let go of whole.
    void Create(Image& image, VkFormat format)
    {
        try
        {
            Make(image, format);
            Hand(image);
        }
        catch (...)
        {
            // Commands that failed to finish may still use it.
            vkDeviceWaitIdle(gpu.device);
            Destroy(image);
            throw;
        }
#ifdef __APPLE__
        image.handle = image.surface;
#else
        image.handle = &image.dmaBuf;
#endif
        ++generation;
    }

    void Hand(Image& image)
    {
        VkCommandBuffer setup = gpu.BeginCommands();
        if (!setup)
            throw std::runtime_error("The graphics card is out of memory.");
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(setup, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        Transfer(setup, image.image, false);
        if (!gpu.SubmitAndWait(setup))
            throw std::runtime_error("The graphics card could not prepare an image for add-ons.");
    }

    void Destroy(Image& image)
    {
        if (image.image)
            vkDestroyImage(gpu.device, image.image, nullptr);
        if (image.memory)
            vkFreeMemory(gpu.device, image.memory, nullptr);
#ifdef __APPLE__
        if (image.surface)
            CFRelease(image.surface);
#else
        if (image.dmaBuf.fd >= 0)
            close(image.dmaBuf.fd);
#endif
        image = {};
    }

    void Release()
    {
        if (!color.handle && !output.handle && !depth.handle && !after.handle && !sdr.handle && !convert.framebuffer)
            return;
        vkDeviceWaitIdle(gpu.device);
        for (Image* image : { &color, &output, &depth, &after, &sdr })
            Destroy(*image);
        backdrop::Destroy(convert);
        width = height = 0;
    }

    // A new size or format makes every image again.
    void Prepare(uint32_t newWidth, uint32_t newHeight, bool newHdr, bool wantOutput, bool wantDepth, bool wantAfter, bool wantSdr)
    {
        if (newWidth != width || newHeight != height || newHdr != hdr)
        {
            Release();
            width = newWidth;
            height = newHeight;
            hdr = newHdr;
        }
        const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_UNORM;
        if (!color.handle)
            Create(color, format);
        if (wantOutput && !output.handle)
            Create(output, format);
        if (wantDepth && !depth.handle)
            Create(depth, VK_FORMAT_R32_SFLOAT);
        if (wantAfter && !after.handle)
            Create(after, format);
        if (hdr && wantSdr && !sdr.handle)
        {
            MakeConvert(convert, width, height);
            Create(sdr, VK_FORMAT_B8G8R8A8_UNORM);
        }
    }

    // Begins commands or effectsCommands, with the fence that goes with them.
    VkCommandBuffer Begin(VkCommandBuffer& buffer, VkFence& finished)
    {
        if (!buffer)
        {
            VkCommandBufferAllocateInfo info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            info.commandPool = gpu.commandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            VkFenceCreateInfo signaled{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            signaled.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkAllocateCommandBuffers(gpu.device, &info, &buffer) != VK_SUCCESS || vkCreateFence(gpu.device, &signaled, nullptr, &finished) != VK_SUCCESS)
                return VK_NULL_HANDLE;
        }
        if (vkWaitForFences(gpu.device, 1, &finished, VK_TRUE, 2'000'000'000) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(buffer, 0) != VK_SUCCESS || vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        vkResetFences(gpu.device, 1, &finished);
        return buffer;
    }

    // Copies the picture into color, in what Begin began, and waits for it. The fence also covers every frame submitted
    // before, so the graphics card is done with the images when this returns. Returns 0 when it could not. The value
    // to wait for is Windows' and has no use here, since add-ons are done once they return.
    uint64_t Submit(VkCommandBuffer& buffer, VkFence& finished, const GpuImage& picture, uint64_t, float sdrWhite)
    {
        Transfer(buffer, color.image, true);
        Copy(buffer, picture.image, color.image, width, height);
        Transfer(buffer, color.image, false);
        if (sdrWhite > 0 && sdr.image)
            CopySdr(buffer, picture, sdrWhite, &buffer == &effectsCommands ? 1 : 0);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &buffer;
        if (vkEndCommandBuffer(buffer) != VK_SUCCESS || vkQueueSubmit(gpu.queue, 1, &submit, finished) != VK_SUCCESS)
        {
            // Nothing waits for it, and the next Begin cannot wait for it either.
            vkDestroyFence(gpu.device, finished, nullptr);
            vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &buffer);
            finished = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return 0;
        }
        if (vkWaitForFences(gpu.device, 1, &finished, VK_TRUE, 2'000'000'000) != VK_SUCCESS)
            return 0;
        return ++value;
    }

    // As on Windows.
    void CopySdr(VkCommandBuffer buffer, const GpuImage& picture, float sdrWhite, int slot)
    {
        FullBarrier(buffer);
        backdrop::ToSdr(buffer, picture, convert, sdrWhite, slot);
        Transfer(buffer, sdr.image, true);
        Copy(buffer, convert.image.image, sdr.image, width, height);
        Transfer(buffer, sdr.image, false);
    }

    void Signal(uint64_t, uint64_t next)
    {
        value = next;
    }

    // Add-ons are done with the images once they return.
    bool WaitHere()
    {
        return true;
    }

    // Copies the picture and the depth add-ons wrote, if any, to the effects, in the overlay's frame.
    bool Finish(Surface& surface, const Image* picture, bool copyDepth, const GpuImage& backbuffer, const GpuImage& effectsDepth)
    {
        Take(surface.commands, picture, copyDepth, backbuffer, effectsDepth);
        return true;
    }

    void Take(VkCommandBuffer frame, const Image* picture, bool copyDepth, const GpuImage& backbuffer, const GpuImage& effectsDepth)
    {
        FullBarrier(frame);
        if (picture)
        {
            Transfer(frame, picture->image, true);
            Copy(frame, picture->image, backbuffer.image, width, height);
            Transfer(frame, picture->image, false);
        }
        if (copyDepth && effectsDepth.width == width && effectsDepth.height == height)
        {
            Transfer(frame, depth.image, true);
            Copy(frame, depth.image, effectsDepth.image, width, height);
            Transfer(frame, depth.image, false);
        }
        FullBarrier(frame);
    }
};
#endif

Addons::Addons() = default;

Addons::~Addons() = default;

void Addons::Init(fx::Runtime& effects)
{
    runtime = &effects;
    official = ReadOfficialList();
    for (const Official& addon : official)
        Log(LogLevel::Info, "Official add-on: %s %s, %s.", addon.manifest.id.c_str(), addon.manifest.version.c_str(), addon.manifest.library.c_str());
    std::error_code error;
    fs::create_directories(AddonsDirectory(), error);
    Scan();
    Update();
}

void Addons::Shutdown()
{
    for (Addon& addon : addons)
        Stop(addon, true);
    exchange.reset();
    addons.clear();
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
        else if (known->manifest.api > UNISHADE_ADDON_VERSION)
            known->problem = "It was built for version " + std::to_string(known->manifest.api) + " of Unishade's add-ons, and this Unishade has versions 1 to " +
                             std::to_string(UNISHADE_ADDON_VERSION) + ". Update Unishade to use it.";
        else if (!fs::is_regular_file(folder / fs::path(known->manifest.library), error))
            known->problem = "Its library, " + known->manifest.library + ", is missing.";
        else if (const Official* listed = OfficialFor(known->manifest.id))
        {
            known->official = Sha256(ReadFile(folder / fs::path(known->manifest.library))) == listed->sha256;
            known->replaceable = !known->official;
        }
        if (added)
            Log(LogLevel::Info, "Add-on found: %s %s in %s, official=%d%s%s.", known->manifest.id.c_str(), known->manifest.version.c_str(),
                entry.folder.c_str(), known->official, known->problem.empty() ? "" : ", ", known->problem.c_str());
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

void Addons::SetSetting(const std::string& folder, size_t index, const float* values)
{
    Addon* addon = Find(folder);
    if (!addon || index >= addon->settings.size())
        return;
    Setting& setting = addon->settings[index];
    std::vector<float>& handed = addon->library->values;
    const size_t offset = ValueOffset(*addon, index);
    // A press is only for the add-on's next frame or update call, which clears it.
    if (setting.type == UNISHADE_SETTING_BUTTON)
    {
        if (offset < handed.size())
            handed[offset] = 1;
        return;
    }
    for (size_t i = 0; i < setting.count; ++i)
    {
        setting.value[i] = ValidValue(setting, values[i], i);
        if (offset + i < handed.size())
            handed[offset + i] = setting.value[i];
    }
    if (ChangesPicture(*addon))
        return WritePreset(*addon);
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set(kSettingsSection + addon->manifest.id, setting.key, FormatValue(setting));
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
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
    library.values.clear();
    const bool look = ChangesPicture(addon);
    if (look)
        addon.bypassed = !PresetIni::Listed(runtime->PresetValue("", "Addons"), addon.manifest.id);
    const IniText ini(look ? std::string() : ReadFile(DataDirectory() / "Unishade.ini"));
    for (Setting& setting : addon.settings)
    {
        // Nothing keeps a button, which takes its default, 0.
        std::string text;
        if (setting.type != UNISHADE_SETTING_BUTTON && look)
            text = runtime->PresetValue(addon.manifest.id, setting.key);
        else if (setting.type != UNISHADE_SETTING_BUTTON)
            ini.Get(kSettingsSection + addon.manifest.id, setting.key, text);
        ParseValue(setting, text);
        library.values.insert(library.values.end(), setting.value, setting.value + setting.count);
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
    const UnishadeAddon* described = get ? get() : nullptr;
    if (!described)
        return refuse("It is not a Unishade add-on: it has no UnishadeGetAddon.");
    if (described->version < 1 || described->version > UNISHADE_ADDON_VERSION)
        return refuse("It was built for version " + std::to_string(described->version) + " of Unishade's add-ons, and this Unishade has versions 1 to " +
                      std::to_string(UNISHADE_ADDON_VERSION) + ".");
    if (!described->create || !described->destroy || ((described->flags & kFrameFlags) && !described->frame) ||
        (described->settingCount && !described->settings))
        return refuse("It does not say what to call.");
    library.addon = described;
    addon.settings.clear();
    // Version 1's settings end where defaultColor starts, so its array is read with that size.
    const bool colors = described->version >= 2;
    const size_t size = colors ? sizeof(UnishadeSetting) : offsetof(UnishadeSetting, defaultColor);
    for (uint32_t i = 0; i < described->settingCount; ++i)
    {
        const UnishadeSetting& declared = *reinterpret_cast<const UnishadeSetting*>(reinterpret_cast<const char*>(described->settings) + i * size);
        Setting setting;
        setting.key = declared.key ? declared.key : "";
        setting.label = declared.label ? declared.label : setting.key;
        setting.tooltip = declared.tooltip ? declared.tooltip : "";
        setting.type = declared.type;
        setting.min = declared.min;
        setting.max = declared.max;
        for (const char* choice = declared.choices; choice && *choice; choice += std::strlen(choice) + 1)
            setting.choices.emplace_back(choice);
        if (IsColor(setting.type) && colors)
        {
            setting.count = setting.type == UNISHADE_SETTING_COLOR_RGBA ? 4 : 3;
            for (size_t c = 0; c < setting.count; ++c)
                setting.defaultValue[c] = ValidValue(setting, declared.defaultColor[c], c);
        }
        else
            setting.defaultValue[0] = ValidValue(setting, declared.defaultValue);
        addon.settings.push_back(std::move(setting));
    }
    ReadSettings(addon);
    Log(LogLevel::Info, "Add-on loaded: %s from %s, flags=%u, settings=%u.", addon.manifest.id.c_str(), path.string().c_str(), described->flags,
        described->settingCount);
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
    std::snprintf(library.context.name, sizeof(library.context.name), "%s", addon.manifest.name.c_str());
    UnishadeHost& host = library.context.host;
    host.version = UNISHADE_ADDON_VERSION;
    host.folder = library.folder.c_str();
    host.dataFolder = library.dataFolder.c_str();
    host.log = HostLog;
    host.effectsGeneration = HostEffectsGeneration;
    host.techniqueCount = HostTechniqueCount;
    host.getTechnique = HostGetTechnique;
    host.setTechnique = HostSetTechnique;
    host.uniformCount = HostUniformCount;
    host.getUniform = HostGetUniform;
    host.getValue = HostGetValue;
    host.setValue = HostSetValue;
    library.context.runtime = runtime;
#ifdef _WIN32
    VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties);
    if (id.deviceLUIDValid)
        std::memcpy(host.adapter, id.deviceLUID, sizeof(host.adapter));
#elif defined(__APPLE__)
    // The registryID of the MTLDevice MoltenVK runs on, asked through the Objective-C runtime.
    const auto exportObjects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(vkGetDeviceProcAddr(gpu.device, "vkExportMetalObjectsEXT"));
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
    vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
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
    {
        Library& library = *addon.library;
        if (presetChanged && library.addon)
            ReadSettings(addon);
        // Version 1 add-ons end before update.
        const auto update = library.addon && library.addon->version >= 2 ? library.addon->update : nullptr;
        if (update && addon.state == State::Enabled && !addon.bypassed && library.instance && !library.stopping)
        {
            char error[512] = "";
            if (!update(library.instance, library.values.data(), error, sizeof(error)))
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
        depth |= addon.state == State::Enabled && !skipped && (addon.library->addon->flags & UNISHADE_ADDON_WRITES_DEPTH);
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
            const addon::Download& file = addon.files[i];
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
        if (!WriteFile(target / addon::kManifestFile, addon.manifestText))
            throw std::runtime_error("Could not write " + (target / addon::kManifestFile).string() + ".");
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
    return addon.library && addon.library->addon && (addon.library->addon->flags & (UNISHADE_ADDON_CHANGES_PICTURE | UNISHADE_ADDON_CHANGES_RESULT)) != 0;
}

bool Addons::Frame(Surface& surface, const fx::Runtime::Source& source, uint64_t picture)
{
    const bool hdrPicture = runtime->Hdr();
    std::vector<Addon*> running, before, after;
    for (Addon& addon : addons)
        if (addon.state == State::Enabled && !addon.bypassed && addon.library->instance && (addon.library->addon->flags & kFrameFlags) &&
            !(hdrPicture && SitsOutHdr(addon)))
        {
            running.push_back(&addon);
            if (addon.library->addon->flags & kBeforeEffects)
                before.push_back(&addon);
            if (addon.library->addon->flags & kAfterEffects)
                after.push_back(&addon);
        }
    const auto flagged = [](const std::vector<Addon*>& list, uint32_t flag) {
        return std::any_of(list.begin(), list.end(), [flag](const Addon* addon) { return (addon->library->addon->flags & flag) != 0; });
    };
    // Add-ons that do not take HDR get the picture as SDR while it is HDR.
    const auto getsSdr = [hdrPicture](const Addon* addon) { return hdrPicture && !TakesHdr(*addon->library->addon); };
    const auto anySdr = [&](const std::vector<Addon*>& list) { return std::any_of(list.begin(), list.end(), getsSdr); };
    // In each stage, add-ons that only read the picture or write depth see it before add-ons change it.
    std::stable_partition(before.begin(), before.end(),
                          [](const Addon* addon) { return !(addon->library->addon->flags & UNISHADE_ADDON_CHANGES_PICTURE); });
    std::stable_partition(after.begin(), after.end(),
                          [](const Addon* addon) { return !(addon->library->addon->flags & UNISHADE_ADDON_CHANGES_RESULT); });
    if (running.empty())
    {
        if (exchange)
            exchange->Release();
        return false;
    }
    if (!exchange)
        exchange = std::make_unique<Exchange>();
    try
    {
        exchange->Prepare(runtime->Width(), runtime->Height(), hdrPicture, flagged(before, UNISHADE_ADDON_CHANGES_PICTURE),
                          flagged(before, UNISHADE_ADDON_WRITES_DEPTH), flagged(after, UNISHADE_ADDON_CHANGES_RESULT), anySdr(running));
    }
    catch (const std::exception& e)
    {
        for (Addon* addon : running)
            Fail(*addon, e.what());
        return false;
    }
#ifdef _WIN32
    catch (const winrt::hresult_error& e)
    {
        for (Addon* addon : running)
            Fail(*addon, "Direct3D failed: " + winrt::to_string(e.message()));
        return false;
    }
#endif

    UnishadeFrame frame{};
    frame.width = exchange->width;
    frame.height = exchange->height;
    frame.generation = exchange->generation;
    frame.picture = picture;
    frame.fence = exchange->fenceHandle;
    const float white = runtime->SdrWhite();
    // Has one stage's add-ons work on the picture in color, each on the one before's result. Returns the image that
    // holds the result: null while it is the one Unishade already has. spare is what add-ons that change it write
    // while color holds it. An add-on that gets the picture as SDR only reads it, since the others sit out, and those
    // go before the ones that change it, so the SDR copy of the stage's picture is the picture it would get.
    bool depthWritten = false;
    const auto run = [&](const std::vector<Addon*>& stage, UnishadeStage which, Exchange::Image& spare) {
        Exchange::Image* result = nullptr;
        for (Addon* addon : stage)
        {
            // One that failed before effects is not called after them.
            if (addon->state != State::Enabled)
                continue;
            Library& library = *addon->library;
            const uint32_t flags = library.addon->flags;
            const bool changes = (flags & (which == UNISHADE_STAGE_BEFORE_EFFECTS ? UNISHADE_ADDON_CHANGES_PICTURE : UNISHADE_ADDON_CHANGES_RESULT)) != 0;
            const bool writesDepth = which == UNISHADE_STAGE_BEFORE_EFFECTS && (flags & UNISHADE_ADDON_WRITES_DEPTH);
            const bool sdr = getsSdr(addon);
            const UnishadeFormat format = exchange->hdr && !sdr ? UNISHADE_FORMAT_RGBA16_FLOAT : UNISHADE_FORMAT_BGRA8;
            Exchange::Image& in = result ? *result : exchange->color;
            Exchange::Image& out = &in == &exchange->color ? spare : exchange->color;
            frame.color = { sdr ? exchange->sdr.handle : in.handle, format };
            frame.output = changes ? UnishadeImage{ out.handle, format } : UnishadeImage{};
            frame.depth = writesDepth ? UnishadeImage{ exchange->depth.handle, UNISHADE_FORMAT_R32_FLOAT } : UnishadeImage{};
            frame.wait = exchange->value;
            frame.signal = exchange->value + 1;
            frame.settings = library.values.data();
            frame.stage = which;
            frame.sdrWhite = format == UNISHADE_FORMAT_RGBA16_FLOAT ? white : 1;
            char error[512] = "";
            if (library.addon->frame(library.instance, &frame, error, sizeof(error)))
            {
                exchange->value = frame.signal;
                if (changes)
                    result = &out;
                depthWritten |= writesDepth;
            }
            else
            {
                exchange->Signal(frame.wait, frame.signal);
                Fail(*addon, error[0] ? error : "It failed on a frame, and did not say why.");
            }
            ClearPresses(*addon);
        }
        return result;
    };
    const char* const late = "Add-ons took more than two seconds on a frame, so it shows without them.";

    VkCommandBuffer commands = exchange->Begin(exchange->commands, exchange->done);
    if (!commands)
        return false;
    runtime->CopyIn(commands, source, true);
    if (!before.empty())
    {
        if (!exchange->Submit(exchange->commands, exchange->done, runtime->Output(), 0, anySdr(before) ? white : 0))
            return false;
        Exchange::Image* result = run(before, UNISHADE_STAGE_BEFORE_EFFECTS, exchange->output);
        if (after.empty())
        {
            if (!exchange->Finish(surface, result, depthWritten, runtime->Output(), runtime->Depth()))
                Log(LogLevel::Warning, "%s", late);
            runtime->RenderEffects(surface.commands, true);
            return true;
        }
        // Effects then run in commands of their own, which wait for these add-ons, so the add-ons after them have
        // their result in this frame.
        if (!exchange->semaphore && !exchange->WaitHere())
        {
            Log(LogLevel::Warning, "%s", late);
            return false;
        }
        commands = exchange->Begin(exchange->effectsCommands, exchange->effectsDone);
        if (!commands)
            return false;
        exchange->Take(commands, result, depthWritten, runtime->Output(), runtime->Depth());
    }
    runtime->RenderEffects(commands, true);
    FullBarrier(commands);
    const bool between = !before.empty();
    if (!exchange->Submit(between ? exchange->effectsCommands : exchange->commands, between ? exchange->effectsDone : exchange->done, runtime->Output(),
                          between ? exchange->value : 0, anySdr(after) ? white : 0))
    {
        // The caller renders the effects again, which must not take the same key and button presses twice: a
        // technique's shortcut would turn it on and off again.
        runtime->input.keysPressed = {};
        runtime->input.buttonsPressed = {};
        return false;
    }
    Exchange::Image* result = run(after, UNISHADE_STAGE_AFTER_EFFECTS, exchange->after);
    if (!exchange->Finish(surface, result, false, runtime->Output(), runtime->Depth()))
        Log(LogLevel::Warning, "%s", late);
    return true;
}
