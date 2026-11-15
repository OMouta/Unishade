#include "addons.h"
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

// src/addons/official.ini, with the SHA-256 of the add-ons this build made, which the build puts into the program.
extern const char kOfficialAddons[];

namespace
{
#ifdef _WIN32
constexpr char kExtension[] = ".dll";
#elif defined(__APPLE__)
constexpr char kExtension[] = ".dylib";
#else
constexpr char kExtension[] = ".so";
#endif
constexpr uint32_t kFrameFlags = UNISHADE_ADDON_WRITES_DEPTH | UNISHADE_ADDON_CHANGES_PICTURE;

fs::path AddonsDirectory()
{
    return DataDirectory() / "Addons";
}

bool IsSha256(const std::string& text)
{
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string Trim(const std::string& text)
{
    const size_t start = text.find_first_not_of(" \t\r");
    return start == std::string::npos ? std::string() : text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
}

// The add-ons whose binary is for this system and whose files all have a SHA-256 and an address.
std::vector<Addons::Official> ReadOfficialList(const std::string& text)
{
    std::vector<Addons::Official> list;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);)
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            list.push_back({ line.substr(1, line.size() - 2), {} });
            continue;
        }
        const size_t equals = line.find('=');
        if (list.empty() || equals == std::string::npos)
            continue;
        std::istringstream value(line.substr(equals + 1));
        Addons::Official::File file{ Trim(line.substr(0, equals)) };
        value >> file.sha256 >> file.url;
        file.sha256 = Lowercase(file.sha256);
        // The add-on itself is the file directly in the Addons folder, which goes first.
        std::vector<Addons::Official::File>& files = list.back().files;
        files.insert(file.path.find('/') == std::string::npos ? files.begin() : files.end(), file);
    }
    std::erase_if(list, [](const Addons::Official& addon) {
        return addon.files.empty() || addon.files[0].path.find('/') != std::string::npos ||
               Lowercase(fs::path(addon.files[0].path).extension().string()) != kExtension ||
               std::any_of(addon.files.begin(), addon.files.end(), [](const auto& file) { return !IsSha256(file.sha256) || file.url.empty(); });
    });
    return list;
}

std::string FormatValue(UnishadeSettingType type, float value)
{
    return type == UNISHADE_SETTING_SLIDER ? std::to_string(value) : std::to_string(static_cast<int>(std::lround(value)));
}

float ValidValue(const Addons::Setting& setting, float value)
{
    switch (setting.type)
    {
    case UNISHADE_SETTING_TOGGLE:
        return value != 0 ? 1.0f : 0.0f;
    case UNISHADE_SETTING_CHOICE:
        return std::clamp(std::round(value), 0.0f, std::max(0.0f, static_cast<float>(setting.choices.size()) - 1));
    case UNISHADE_SETTING_INTEGER:
        return std::clamp(std::round(value), std::round(setting.min), std::round(setting.max));
    default:
        return std::isfinite(value) ? std::clamp(value, setting.min, setting.max) : setting.defaultValue;
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
};

void HostLog(const UnishadeHost* host, UnishadeLogLevel level, const char* text)
{
    const char* name = host ? reinterpret_cast<const HostContext*>(host)->name : "An add-on";
    const LogLevel kind = level == UNISHADE_LOG_ERROR ? LogLevel::Error : level == UNISHADE_LOG_WARNING ? LogLevel::Warning : LogLevel::Info;
    Log(kind, "%s: %s", name, text ? text : "");
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
    Image color, output, depth;
    uint32_t width = 0, height = 0;
    uint64_t generation = 0;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;

    ~Exchange()
    {
        Release();
        if (semaphore)
            vkDestroySemaphore(gpu.device, semaphore, nullptr);
        if (done)
            vkDestroyFence(gpu.device, done, nullptr);
        if (commands)
            vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &commands);
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
        if (!color.texture && !output.texture && !depth.texture)
            return;
        vkDeviceWaitIdle(gpu.device);
        for (Image* image : { &color, &output, &depth })
            Destroy(*image);
        width = height = 0;
    }

    void Prepare(uint32_t newWidth, uint32_t newHeight, bool wantOutput, bool wantDepth)
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
        if (newWidth != width || newHeight != height)
        {
            Release();
            width = newWidth;
            height = newHeight;
        }
        if (!color.texture)
            Create(color, DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
        if (wantOutput && !output.texture)
            Create(output, DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM);
        if (wantDepth && !depth.texture)
            Create(depth, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT);
    }

    VkCommandBuffer Begin()
    {
        if (!commands)
        {
            VkCommandBufferAllocateInfo info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            info.commandPool = gpu.commandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            VkFenceCreateInfo signaled{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            signaled.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkAllocateCommandBuffers(gpu.device, &info, &commands) != VK_SUCCESS || vkCreateFence(gpu.device, &signaled, nullptr, &done) != VK_SUCCESS)
                return VK_NULL_HANDLE;
        }
        // The last frame's copy, which the overlay's last frame waited for.
        if (vkWaitForFences(gpu.device, 1, &done, VK_TRUE, 2'000'000'000) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(commands, 0) != VK_SUCCESS || vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        vkResetFences(gpu.device, 1, &done);
        return commands;
    }

    // Copies the picture into color and hands it over. Returns the fence value add-ons wait for, or 0 when nothing was
    // submitted.
    uint64_t Submit(const GpuImage& picture)
    {
        Transfer(commands, color.image, true);
        Copy(commands, picture.image, color.image, width, height);
        Transfer(commands, color.image, false);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        const uint64_t ready = value + 1;
        VkTimelineSemaphoreSubmitInfoKHR timeline{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR };
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &ready;
        if (semaphore)
        {
            submit.pNext = &timeline;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &semaphore;
        }
        if (vkEndCommandBuffer(commands) != VK_SUCCESS || vkQueueSubmit(gpu.queue, 1, &submit, done) != VK_SUCCESS)
        {
            // Nothing waits for it, and the next Begin cannot wait for it either.
            vkDestroyFence(gpu.device, done, nullptr);
            vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &commands);
            done = VK_NULL_HANDLE;
            commands = VK_NULL_HANDLE;
            return 0;
        }
        value = ready;
        if (!semaphore)
        {
            if (vkWaitForFences(gpu.device, 1, &done, VK_TRUE, 2'000'000'000) != VK_SUCCESS)
                return 0;
            winrt::check_hresult(context->Signal(fence.get(), ready));
            context->Flush();
        }
        return ready;
    }

    // Sets the fence to a value for an add-on that could not, after the value before it.
    void Signal(uint64_t after, uint64_t next)
    {
        context->Wait(fence.get(), after);
        context->Signal(fence.get(), next);
        context->Flush();
        value = next;
    }

    // Has the overlay's frame wait for the add-ons, then copies the picture and the depth they wrote, if any, to the
    // effects.
    bool Finish(Surface& surface, const Image* picture, bool copyDepth, const GpuImage& backbuffer, const GpuImage& effectsDepth)
    {
        if (semaphore)
            surface.WaitFor(semaphore, value);
        else if (FAILED(fence->SetEventOnCompletion(value, event.get())) || WaitForSingleObject(event.get(), 2000) != WAIT_OBJECT_0)
            return false;
        VkCommandBuffer frame = surface.commands;
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
        return true;
    }
};
#else
struct Addons::Exchange
{
};
#endif

Addons::Addons() = default;

Addons::~Addons() = default;

void Addons::Init(fx::Runtime& effects)
{
    runtime = &effects;
    official = ReadOfficialList(kOfficialAddons);
    for (const Official& addon : official)
        Log(LogLevel::Info, "Official add-on: %s, %s.", addon.name.c_str(), addon.files[0].path.c_str());
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
    std::vector<std::string> found;
    std::error_code error;
    for (fs::directory_iterator it(AddonsDirectory(), error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error) && Lowercase(it->path().extension().string()) == kExtension)
            found.push_back(it->path().filename().string());
    std::sort(found.begin(), found.end());

    for (Addon& addon : addons)
        if (std::find(found.begin(), found.end(), addon.file) == found.end())
        {
            Log(LogLevel::Info, "Add-on removed: %s.", addon.file.c_str());
            Stop(addon, true);
        }
    std::erase_if(addons, [&](const Addon& addon) { return std::find(found.begin(), found.end(), addon.file) == found.end(); });

    for (const std::string& file : found)
    {
        Addon* known = Find(file);
        // A loaded library stays as it was loaded.
        if (known && known->library->module)
            continue;
        const std::string hash = Sha256(ReadFile(AddonsDirectory() / file));
        const Official* listed = nullptr;
        for (const Official& entry : official)
            if (entry.files[0].sha256 == hash)
                listed = &entry;
        const bool added = !known;
        if (added)
        {
            addons.push_back({ file, fs::path(file).stem().string() });
            addons.back().library = std::make_shared<Library>();
            known = &addons.back();
            Log(LogLevel::Info, "Add-on found: %s, official=%d, sha256=%s.", file.c_str(), listed != nullptr, hash.c_str());
        }
        known->official = listed != nullptr;
        if (listed)
            known->name = listed->name;
        // Update starts the ones that are wanted.
        if (known->state == State::Pending || known->state == State::Disabled)
            known->state = !known->official && Answer(file) < 0 ? State::Pending : State::Disabled;
        if (added && known->state == State::Pending)
            Report(LogLevel::Info, "%s in the Addons folder is not an official add-on. Unishade loads it once you allow it in Settings, Add-ons.",
                   file.c_str());
    }
    changed = true;
}

int Addons::Answer(const std::string& file) const
{
    std::string value;
    if (!IniText(ReadFile(DataDirectory() / "Unishade.ini")).Get("ADDONS", file, value))
        return -1;
    return value == "1" ? 1 : 0;
}

void Addons::SetAnswer(const std::string& file, bool load)
{
    const fs::path path = DataDirectory() / "Unishade.ini";
    IniText ini(ReadFile(path));
    ini.Set("ADDONS", file, load ? "1" : "0");
    if (!WriteFile(path, ini.Text()))
        Log(LogLevel::Warning, "Could not write %s.", path.string().c_str());
    changed = true;
}

Addons::Addon* Addons::Find(const std::string& file)
{
    for (Addon& addon : addons)
        if (addon.file == file)
            return &addon;
    return nullptr;
}

void Addons::Approve(const std::string& file, bool load)
{
    Addon* addon = Find(file);
    if (!addon || addon->state != State::Pending)
        return;
    Log(LogLevel::Info, "Add-on %s: the user said to %s.", file.c_str(), load ? "load it" : "leave it");
    addon->state = State::Disabled;
    SetAnswer(file, load);
}

void Addons::SetEnabled(const std::string& file, bool enabled)
{
    Addon* addon = Find(file);
    if (!addon)
        return;
    if (addon->state == State::Failed || addon->state == State::Pending)
        addon->state = State::Disabled;
    SetAnswer(file, enabled);
}

void Addons::SetSetting(const std::string& file, size_t index, float value)
{
    Addon* addon = Find(file);
    if (!addon || index >= addon->settings.size())
        return;
    Setting& setting = addon->settings[index];
    setting.value = ValidValue(setting, value);
    if (index < addon->library->values.size())
        addon->library->values[index] = setting.value;
    runtime->SetPresetValue(addon->name, setting.key, FormatValue(setting.type, setting.value));
}

void Addons::ReadSettings(Addon& addon)
{
    Library& library = *addon.library;
    library.values.resize(addon.settings.size());
    for (size_t i = 0; i < addon.settings.size(); ++i)
    {
        Setting& setting = addon.settings[i];
        const std::string text = runtime->PresetValue(addon.name, setting.key);
        setting.value = text.empty() ? setting.defaultValue : ValidValue(setting, std::strtof(text.c_str(), nullptr));
        library.values[i] = setting.value;
    }
}

void Addons::Load(Addon& addon)
{
    Library& library = *addon.library;
    const fs::path path = AddonsDirectory() / addon.file;
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
    if (described->version != UNISHADE_ADDON_VERSION)
        return refuse("It was built for version " + std::to_string(described->version) + " of Unishade's add-ons, and this Unishade has version " +
                      std::to_string(UNISHADE_ADDON_VERSION) + ".");
    if (!described->name || !*described->name || !described->create || !described->destroy || ((described->flags & kFrameFlags) && !described->frame) ||
        (described->settingCount && !described->settings))
        return refuse("It does not say its name or what to call.");
    library.addon = described;
    addon.name = described->name;
    addon.description = described->description ? described->description : "";
    addon.settings.clear();
    for (uint32_t i = 0; i < described->settingCount; ++i)
    {
        const UnishadeSetting& declared = described->settings[i];
        Setting setting;
        setting.key = declared.key ? declared.key : "";
        setting.label = declared.label ? declared.label : setting.key;
        setting.tooltip = declared.tooltip ? declared.tooltip : "";
        setting.type = declared.type;
        setting.min = declared.min;
        setting.max = declared.max;
        for (const char* choice = declared.choices; choice && *choice; choice += std::strlen(choice) + 1)
            setting.choices.emplace_back(choice);
        setting.defaultValue = ValidValue(setting, declared.defaultValue);
        addon.settings.push_back(std::move(setting));
    }
    ReadSettings(addon);
    Log(LogLevel::Info, "Add-on loaded: %s from %s, flags=%u, settings=%u.", addon.name.c_str(), addon.file.c_str(), described->flags,
        described->settingCount);
}

void Addons::Start(Addon& addon)
{
    Library& library = *addon.library;
    if (!library.module)
        Load(addon);
    if (!library.addon)
        return;
#ifndef _WIN32
    if (library.addon->flags & kFrameFlags)
        return Fail(addon, "Add-ons that work on the game's picture only run on Windows for now.");
#endif
    library.folder = (AddonsDirectory() / fs::path(addon.file).stem()).string() + static_cast<char>(fs::path::preferred_separator);
    library.dataFolder = DataDirectory().string() + static_cast<char>(fs::path::preferred_separator);
    std::snprintf(library.context.name, sizeof(library.context.name), "%s", addon.name.c_str());
    UnishadeHost& host = library.context.host;
    host.version = UNISHADE_ADDON_VERSION;
    host.folder = library.folder.c_str();
    host.dataFolder = library.dataFolder.c_str();
    host.log = HostLog;
    VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(gpu.physicalDevice, &properties);
    if (id.deviceLUIDValid)
        std::memcpy(host.adapter, id.deviceLUID, sizeof(host.adapter));

    char error[512] = "";
    library.instance = library.addon->create(&host, error, sizeof(error));
    if (!library.instance)
        return Fail(addon, error[0] ? error : "It did not start, and did not say why.");
    addon.state = State::Enabled;
    addon.error.clear();
    Log(LogLevel::Info, "Add-on started: %s.", addon.name.c_str());
}

void Addons::Stop(Addon& addon, bool unload)
{
    Library& library = *addon.library;
    if (library.instance)
    {
        library.addon->destroy(library.instance);
        library.instance = nullptr;
        Log(LogLevel::Info, "Add-on stopped: %s.", addon.name.c_str());
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
    Report(LogLevel::Error, "%s stopped: %s", addon.name.c_str(), error.c_str());
}

void Addons::Update()
{
    const bool presetChanged = runtime->PresetGeneration() != presetGeneration;
    presetGeneration = runtime->PresetGeneration();
    bool work = std::exchange(changed, false);
    for (Addon& addon : addons)
    {
        work |= addon.library->stopping;
        if (presetChanged && addon.library->addon)
            ReadSettings(addon);
    }
    if (!work)
        return;

    for (Addon& addon : addons)
    {
        const int answer = Answer(addon.file);
        const bool wanted = addon.official ? answer != 0 : answer == 1;
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
    for (const Addon& addon : addons)
        depth |= addon.state == State::Enabled && (addon.library->addon->flags & UNISHADE_ADDON_WRITES_DEPTH);
    runtime->ProvideDepth(depth);
}

void Addons::Install(const Official& addon, const std::atomic<bool>& cancel,
                     const std::function<void(size_t file, uint64_t received, uint64_t total)>& progress)
{
#ifdef _WIN32
    for (size_t i = 0; i < addon.files.size(); ++i)
    {
        const Official::File& file = addon.files[i];
        const fs::path path = AddonsDirectory() / fs::path(file.path);
        fs::create_directories(path.parent_path());
        // Under another name until it is whole and checked, so a broken file is never taken for the add-on.
        fs::path part = path;
        part += ".part";
        Download(Wide(file.url), part, file.sha256, kAddonLimit, cancel, [&](uint64_t received, uint64_t total) {
            if (progress)
                progress(i, received, total);
        });
        fs::rename(part, path);
        Log(LogLevel::Info, "Installed %s from %s.", file.path.c_str(), file.url.c_str());
    }
#else
    (void)cancel;
    (void)progress;
    throw std::runtime_error(addon.name + " is for Windows only.");
#endif
}

bool Addons::Frame(Surface& surface, const fx::Runtime::Source& source, uint64_t picture)
{
#ifdef _WIN32
    std::vector<Addon*> running;
    for (Addon& addon : addons)
        if (addon.state == State::Enabled && addon.library->instance && (addon.library->addon->flags & kFrameFlags))
            running.push_back(&addon);
    // Add-ons that write depth see the picture before add-ons change it.
    std::stable_partition(running.begin(), running.end(),
                          [](const Addon* addon) { return !(addon->library->addon->flags & UNISHADE_ADDON_CHANGES_PICTURE); });
    if (running.empty())
    {
        if (exchange)
            exchange->Release();
        return false;
    }
    bool changes = false, writesDepth = false;
    for (const Addon* addon : running)
    {
        changes |= (addon->library->addon->flags & UNISHADE_ADDON_CHANGES_PICTURE) != 0;
        writesDepth |= (addon->library->addon->flags & UNISHADE_ADDON_WRITES_DEPTH) != 0;
    }
    if (!exchange)
        exchange = std::make_unique<Exchange>();
    try
    {
        exchange->Prepare(runtime->Width(), runtime->Height(), changes, writesDepth);
    }
    catch (const std::exception& e)
    {
        for (Addon* addon : running)
            Fail(*addon, e.what());
        return false;
    }
    catch (const winrt::hresult_error& e)
    {
        for (Addon* addon : running)
            Fail(*addon, "Direct3D failed: " + winrt::to_string(e.message()));
        return false;
    }

    VkCommandBuffer commands = exchange->Begin();
    if (!commands)
        return false;
    runtime->CopyIn(commands, source);
    if (!exchange->Submit(runtime->Output()))
        return false;

    UnishadeFrame frame{};
    frame.width = exchange->width;
    frame.height = exchange->height;
    frame.generation = exchange->generation;
    frame.picture = picture;
    frame.fence = exchange->fenceHandle;
    // Which image holds the picture: null while it is the one the effects already have.
    Exchange::Image* result = nullptr;
    bool depthWritten = false;
    for (Addon* addon : running)
    {
        Library& library = *addon->library;
        const uint32_t flags = library.addon->flags;
        Exchange::Image& in = result ? *result : exchange->color;
        Exchange::Image& out = &in == &exchange->color ? exchange->output : exchange->color;
        frame.color = { in.handle, UNISHADE_FORMAT_BGRA8 };
        frame.output = (flags & UNISHADE_ADDON_CHANGES_PICTURE) ? UnishadeImage{ out.handle, UNISHADE_FORMAT_BGRA8 } : UnishadeImage{};
        frame.depth = (flags & UNISHADE_ADDON_WRITES_DEPTH) ? UnishadeImage{ exchange->depth.handle, UNISHADE_FORMAT_R32_FLOAT } : UnishadeImage{};
        frame.wait = exchange->value;
        frame.signal = exchange->value + 1;
        frame.settings = library.values.data();
        char error[512] = "";
        if (library.addon->frame(library.instance, &frame, error, sizeof(error)))
        {
            exchange->value = frame.signal;
            if (flags & UNISHADE_ADDON_CHANGES_PICTURE)
                result = &out;
            depthWritten |= (flags & UNISHADE_ADDON_WRITES_DEPTH) != 0;
        }
        else
        {
            exchange->Signal(frame.wait, frame.signal);
            Fail(*addon, error[0] ? error : "It failed on a frame, and did not say why.");
        }
    }
    if (!exchange->Finish(surface, result, depthWritten, runtime->Output(), runtime->Depth()))
        Log(LogLevel::Warning, "Add-ons took more than two seconds on a frame, so it shows without them.");
    return true;
#else
    (void)surface;
    (void)source;
    (void)picture;
    return false;
#endif
}
