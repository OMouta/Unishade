#include "gpu.h"
#include "log.h"
#include "platform.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

Gpu gpu;

namespace
{
bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& e) { return !strcmp(e.extensionName, name); });
}

// How long drawing waits for the graphics card before it skips a frame, rather than hanging when the card or the
// window system stops answering.
constexpr uint64_t kFenceTimeout = 2'000'000'000;
constexpr uint64_t kAcquireTimeout = 250'000'000;

int DeviceRank(VkPhysicalDeviceType type)
{
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    default:
        return 3;
    }
}

// Whether the device is the DRM device with the given primary or render node.
bool IsDrmDevice(VkPhysicalDevice device, int64_t major, int64_t minor)
{
#ifdef VK_EXT_physical_device_drm
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS ||
        !HasExtension(extensions, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
        return false;
    VkPhysicalDeviceDrmPropertiesEXT drm{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &drm;
    vkGetPhysicalDeviceProperties2(device, &properties);
    return (drm.hasPrimary && drm.primaryMajor == major && drm.primaryMinor == minor) ||
           (drm.hasRender && drm.renderMajor == major && drm.renderMinor == minor);
#else
    (void)device, (void)major, (void)minor;
    return false;
#endif
}
} // namespace

bool Gpu::Init(bool headless, std::string& error)
{
    if (vkEnumerateInstanceVersion(&apiVersion) != VK_SUCCESS || apiVersion < VK_API_VERSION_1_1)
    {
        error = "Vulkan 1.1 is not available. Update the graphics driver.";
        return false;
    }
    apiVersion = VK_API_VERSION_1_1;

    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    std::vector<const char*> extensions;
    if (!headless)
    {
        const char** required = glfwGetRequiredInstanceExtensions(&count);
        if (!required)
        {
            error = "Vulkan cannot draw to windows on this system.";
            return false;
        }
        extensions.assign(required, required + count);
    }
    VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_portability_enumeration
    // A loader in front of MoltenVK only lists it with this extension.
    if (HasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    std::vector<const char*> layers;
    if (const char* validate = getenv("UNISHADE_VALIDATION"); validate && *validate == '1')
        layers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Unishade";
    app.apiVersion = apiVersion;
    VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.flags = flags;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    if (VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance); result != VK_SUCCESS)
    {
        error = "Could not start Vulkan (error " + std::to_string(result) + ").";
        return false;
    }

    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    // Frames from the display server can only be imported on the graphics card it draws with, which matters on
    // laptops with two. That card comes first when the system says which it is, otherwise the fastest kind.
    int64_t displayMajor = 0, displayMinor = 0;
    const bool displayKnown = !headless && devices.size() > 1 && platform::DisplayDrmDevice(displayMajor, displayMinor);
    int bestRank = 99;
    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.apiVersion < VK_API_VERSION_1_1)
            continue;
        const int rank = displayKnown && IsDrmDevice(device, displayMajor, displayMinor) ? -1 : DeviceRank(props.deviceType);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t family = 0; family < familyCount; ++family)
        {
            // Effects can use compute passes, so the queue must do both.
            if ((families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) ||
                (!headless && !glfwGetPhysicalDevicePresentationSupport(instance, device, family)))
                continue;
            if (rank < bestRank)
            {
                bestRank = rank;
                physicalDevice = device;
                queueFamily = family;
                properties = props;
            }
            break;
        }
    }
    if (!physicalDevice)
    {
        error = "No graphics card with Vulkan 1.1 can draw to windows here.";
        return false;
    }
    if (bestRank < 0)
        Log(LogLevel::Info, "The display is drawn by %s, so effects run there too.", properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceAvailable(count);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, deviceAvailable.data());
    std::vector<const char*> deviceExtensions;
    if (!headless)
        deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // MoltenVK lists what Metal cannot do through this extension, which must then be enabled.
    if (HasExtension(deviceAvailable, "VK_KHR_portability_subset"))
        deviceExtensions.push_back("VK_KHR_portability_subset");
    // What lets captured frames stay on the graphics card: IOSurfaces through MoltenVK on macOS, dma-bufs on
    // Linux. Without them frames are copied through memory.
    const auto enableAll = [&](std::initializer_list<const char*> names) {
        for (const char* name : names)
            if (!HasExtension(deviceAvailable, name))
                return false;
        deviceExtensions.insert(deviceExtensions.end(), names);
        return true;
    };
    if (!headless)
    {
#ifdef __APPLE__
        metalObjects = enableAll({ "VK_EXT_metal_objects" });
#else
        dmaBuf = enableAll({ "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_image_format_list",
                             "VK_EXT_image_drm_format_modifier" });
        foreignQueue = dmaBuf && enableAll({ "VK_EXT_queue_family_foreign" });
#endif
    }

    // What ReShade enables for effects, where the device has it.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physicalDevice, &supported);
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = supported.samplerAnisotropy;
    features.shaderImageGatherExtended = supported.shaderImageGatherExtended;
    features.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
    features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    features.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
    features.independentBlend = supported.independentBlend;
    features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    features.imageCubeArray = supported.imageCubeArray;
    independentBlend = supported.independentBlend;
    anisotropy = supported.samplerAnisotropy;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &features;
    if (VkResult result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device); result != VK_SUCCESS)
    {
        error = "Could not start the graphics card (Vulkan error " + std::to_string(result) + ").";
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool); result != VK_SUCCESS)
    {
        error = "The graphics card is out of memory (Vulkan error " + std::to_string(result) + ").";
        return false;
    }

    Log(LogLevel::Info, "Frames stay on the graphics card: %s", metalObjects || dmaBuf ? "yes" : "no, they are copied through memory");
    Log(LogLevel::Info, "Graphics: %s (Vulkan %u.%u.%u)", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
        VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
    return true;
}

void Gpu::Shutdown()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        if (commandPool)
            vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    commandPool = VK_NULL_HANDLE;
    if (instance)
        vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

uint32_t Gpu::MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

bool Gpu::Supports(VkFormat format, VkFormatFeatureFlags features) const
{
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

VkImageAspectFlags Aspects(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkFormat SrgbFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_SRGB;
    default:
        return format;
    }
}

bool Gpu::CreateImage(GpuImage& image, uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageUsageFlags usage,
                      VkImageType type, uint32_t depth)
{
    image = {};
    image.type = type;
    image.format = format;
    image.width = width;
    image.height = type == VK_IMAGE_TYPE_1D ? 1 : height;
    image.depth = type == VK_IMAGE_TYPE_3D ? depth : 1;
    image.levels = levels;
    const VkFormat srgb = SrgbFormat(format);

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = srgb != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    info.imageType = type;
    info.format = format;
    info.extent = { image.width, image.height, image.depth };
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &image.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &allocation, nullptr, &image.memory) != VK_SUCCESS)
    {
        DestroyImage(image);
        return false;
    }
    if (vkBindImageMemory(device, image.image, image.memory, 0) != VK_SUCCESS)
    {
        DestroyImage(image);
        return false;
    }

    bool viewsMade = true;
    const auto makeView = [&](VkFormat viewFormat, uint32_t base, uint32_t count) {
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = image.image;
        view.viewType = type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        view.format = viewFormat;
        view.subresourceRange = { Aspects(format), base, count, 0, 1 };
        VkImageViewUsageCreateInfo viewUsage{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
        // An sRGB view cannot be used for storage, which the image as a whole may allow.
        if (viewFormat != format && (usage & VK_IMAGE_USAGE_STORAGE_BIT))
        {
            viewUsage.usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT;
            view.pNext = &viewUsage;
        }
        VkImageView result = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &view, nullptr, &result) != VK_SUCCESS)
        {
            viewsMade = false;
            return VkImageView(VK_NULL_HANDLE);
        }
        return result;
    };
    const bool sampled = usage & VK_IMAGE_USAGE_SAMPLED_BIT;
    const bool target = usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (sampled)
    {
        image.view = makeView(format, 0, levels);
        image.srgbView = srgb != format ? makeView(srgb, 0, levels) : image.view;
    }
    if (target)
    {
        image.target = makeView(format, 0, 1);
        image.srgbTarget = srgb != format ? makeView(srgb, 0, 1) : image.target;
    }
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
        for (uint32_t level = 0; level < levels; ++level)
            image.storage.push_back(makeView(format, level, 1));
    if (!viewsMade)
    {
        DestroyImage(image);
        return false;
    }
    return true;
}

void Gpu::DestroyImage(GpuImage& image)
{
    std::vector<VkImageView> views = image.storage;
    for (VkImageView view : { image.view, image.srgbView, image.target, image.srgbTarget })
        if (view && std::find(views.begin(), views.end(), view) == views.end())
            views.push_back(view);
    for (VkImageView view : views)
        vkDestroyImageView(device, view, nullptr);
    if (image.image)
        vkDestroyImage(device, image.image, nullptr);
    if (image.memory)
        vkFreeMemory(device, image.memory, nullptr);
    image = {};
}

bool Gpu::CreateBuffer(GpuBuffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback)
{
    buffer = {};
    buffer.size = size;
    VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    if (vkCreateBuffer(device, &info, nullptr, &buffer.buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    // Memory the graphics card writes and the processor reads is fastest cached, which is not always coherent.
    allocation.memoryTypeIndex = UINT32_MAX;
    if (readback)
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        allocation.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                                          : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &allocation, nullptr, &buffer.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0) != VK_SUCCESS ||
        (hostVisible && vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped) != VK_SUCCESS))
    {
        DestroyBuffer(buffer);
        return false;
    }
    buffer.coherent = memoryProperties.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    return true;
}

void Gpu::Invalidate(const GpuBuffer& buffer)
{
    if (buffer.coherent || !buffer.memory)
        return;
    VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    range.memory = buffer.memory;
    range.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(device, 1, &range);
}

void Gpu::DestroyBuffer(GpuBuffer& buffer)
{
    if (buffer.buffer)
        vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory)
        vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

VkCommandBuffer Gpu::BeginCommands()
{
    VkCommandBufferAllocateInfo info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    info.commandPool = commandPool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &info, &commands) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, commandPool, 1, &commands);
        return VK_NULL_HANDLE;
    }
    return commands;
}

bool Gpu::SubmitAndWait(VkCommandBuffer commands)
{
    if (!commands)
        return false;
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    bool submitted = false;
    VkResult result = vkEndCommandBuffer(commands);
    if (result == VK_SUCCESS)
        result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    if (result == VK_SUCCESS && (result = vkQueueSubmit(queue, 1, &submit, fence)) == VK_SUCCESS)
    {
        submitted = true;
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    if (result == VK_ERROR_DEVICE_LOST)
        ReportLost();
    // Commands still running keep their buffer and fence.
    if (!submitted || result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
    {
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &commands);
    }
    return result == VK_SUCCESS;
}

void Gpu::ReportLost()
{
    if (lost)
        return;
    lost = true;
    Report(LogLevel::Error, "The graphics card stopped responding (Vulkan device lost), so Unishade stopped drawing. Restart Unishade.");
}

void FullBarrier(VkCommandBuffer commands)
{
    VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void InitLayout(VkCommandBuffer commands, const GpuImage& image)
{
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = { Aspects(image.format), 0, image.levels, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Surface

bool Surface::Create(GLFWwindow* glfwWindow, bool wantLowLatency, std::string& error)
{
    window = glfwWindow;
    lowLatency = wantLowLatency;
    if (VkResult result = glfwCreateWindowSurface(gpu.instance, window, nullptr, &surface); result != VK_SUCCESS)
    {
        error = "Could not draw to a window (Vulkan error " + std::to_string(result) + ").";
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physicalDevice, gpu.queueFamily, surface, &supported);
    if (!supported)
    {
        error = "The graphics card cannot draw to this window.";
        return false;
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physicalDevice, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physicalDevice, surface, &count, formats.data());
    if (formats.empty())
    {
        error = "The window has no picture formats.";
        return false;
    }
    format = formats[0].format;
    VkColorSpaceKHR colorSpace = formats[0].colorSpace;
    for (VkFormat wanted : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
    {
        const auto it = std::find_if(formats.begin(), formats.end(), [wanted](const VkSurfaceFormatKHR& f) {
            return f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end())
        {
            format = it->format;
            colorSpace = it->colorSpace;
            break;
        }
    }
    this->colorSpace = colorSpace;

    // Dear ImGui draws on top of what was copied into the image, then it is presented.
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    VkCommandBufferAllocateInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    commandInfo.commandPool = gpu.commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkCreateRenderPass(gpu.device, &passInfo, nullptr, &renderPass) != VK_SUCCESS ||
        vkAllocateCommandBuffers(gpu.device, &commandInfo, &commands) != VK_SUCCESS || !CreateSync())
    {
        error = "The graphics card is out of memory.";
        return false;
    }

    if (!CreateSwapchain())
    {
        error = "Could not create the window's swapchain.";
        return false;
    }
    return true;
}

bool Surface::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physicalDevice, surface, &caps);
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0)
        return false;

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physicalDevice, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physicalDevice, surface, &count, modes.data());
    // The overlay shows each frame as soon as it is ready, since the game already waited for the display. Without
    // mailbox it waits for the display too, in FIFO mode, which every device has and which never tears.
    const bool mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
    const VkPresentModeKHR mode = lowLatency && mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;

    minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount)
        minImageCount = std::min(minImageCount, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR option : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR })
        if (caps.supportedCompositeAlpha & option)
        {
            alpha = option;
            break;
        }

    VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = surface;
    info.minImageCount = minImageCount;
    info.imageFormat = format;
    info.imageColorSpace = colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(gpu.device, &info, nullptr, &created);
    DestroySwapchain();
    if (result != VK_SUCCESS)
        return false;
    swapchain = created;

    vkGetSwapchainImagesKHR(gpu.device, swapchain, &imageCount, nullptr);
    images.resize(imageCount);
    vkGetSwapchainImagesKHR(gpu.device, swapchain, &imageCount, images.data());
    for (VkImage swapImage : images)
    {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = swapImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        const VkResult viewMade = vkCreateImageView(gpu.device, &viewInfo, nullptr, &view);
        views.push_back(view);

        VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        const bool made = viewMade == VK_SUCCESS && vkCreateFramebuffer(gpu.device, &framebufferInfo, nullptr, &framebuffer) == VK_SUCCESS &&
                          vkCreateSemaphore(gpu.device, &semaphoreInfo, nullptr, &semaphore) == VK_SUCCESS;
        framebuffers.push_back(framebuffer);
        renderDone.push_back(semaphore);
        if (!made)
        {
            DestroySwapchain();
            return false;
        }
    }
    recreated = true;
    needsRecreate = false;
    return true;
}

// Keeps the swapchain handle itself, which CreateSwapchain passes on as the old one.
void Surface::DestroySwapchain()
{
    for (VkFramebuffer framebuffer : framebuffers)
        vkDestroyFramebuffer(gpu.device, framebuffer, nullptr);
    for (VkImageView view : views)
        vkDestroyImageView(gpu.device, view, nullptr);
    for (VkSemaphore semaphore : renderDone)
        vkDestroySemaphore(gpu.device, semaphore, nullptr);
    framebuffers.clear();
    views.clear();
    renderDone.clear();
    images.clear();
    if (swapchain)
        vkDestroySwapchainKHR(gpu.device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

bool Surface::CreateSync()
{
    VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(gpu.device, &semaphoreInfo, nullptr, &acquired) == VK_SUCCESS &&
        vkCreateFence(gpu.device, &fenceInfo, nullptr, &fence) == VK_SUCCESS)
        return true;
    DestroySync();
    return false;
}

void Surface::DestroySync()
{
    if (acquired)
        vkDestroySemaphore(gpu.device, acquired, nullptr);
    if (fence)
        vkDestroyFence(gpu.device, fence, nullptr);
    acquired = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;
}

void Surface::Destroy()
{
    if (!gpu.device)
        return;
    vkDeviceWaitIdle(gpu.device);
    DestroySwapchain();
    if (renderPass)
        vkDestroyRenderPass(gpu.device, renderPass, nullptr);
    DestroySync();
    if (commands)
        vkFreeCommandBuffers(gpu.device, gpu.commandPool, 1, &commands);
    if (surface)
        vkDestroySurfaceKHR(gpu.instance, surface, nullptr);
    renderPass = VK_NULL_HANDLE;
    commands = VK_NULL_HANDLE;
    surface = VK_NULL_HANDLE;
}

bool Surface::BeginFrame(bool wait)
{
    if (gpu.lost)
    {
        // Nothing can draw the error anymore, so the window's title says it.
        if (!lostShown)
            glfwSetWindowTitle(window, "Unishade - the graphics card stopped responding, restart Unishade");
        lostShown = true;
        return false;
    }
    if (!fence && !CreateSync())
        return false;
    const VkResult waited = vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, wait ? kFenceTimeout : 0);
    if (waited == VK_ERROR_DEVICE_LOST)
        gpu.ReportLost();
    if (waited != VK_SUCCESS)
        return false;

    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return false;
    if (needsRecreate || !swapchain || static_cast<uint32_t>(width) != extent.width || static_cast<uint32_t>(height) != extent.height)
    {
        vkDeviceWaitIdle(gpu.device);
        if (!CreateSwapchain())
            return false;
    }

    VkResult result = vkAcquireNextImageKHR(gpu.device, swapchain, wait ? kAcquireTimeout : 0, acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        needsRecreate = true;
        return false;
    }
    if (result == VK_ERROR_DEVICE_LOST)
        gpu.ReportLost();
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        return false;
    if (result == VK_SUBOPTIMAL_KHR)
        needsRecreate = true;
    image = images[index];

    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(commands, 0) != VK_SUCCESS || vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS)
    {
        Recover();
        return false;
    }
    vkResetFences(gpu.device, 1, &fence);

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    return true;
}

void Surface::BeginRenderPass()
{
    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = renderPass;
    info.framebuffer = framebuffers[index];
    info.renderArea.extent = extent;
    vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
}

void Surface::EndFrame()
{
    vkCmdEndRenderPass(commands);
    const VkResult ended = vkEndCommandBuffer(commands);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone[index];
    if (const VkResult submitted = ended == VK_SUCCESS ? vkQueueSubmit(gpu.queue, 1, &submit, fence) : ended; submitted != VK_SUCCESS)
    {
        if (submitted == VK_ERROR_DEVICE_LOST)
            gpu.ReportLost();
        else
        {
            Log(LogLevel::Error, "The graphics card could not draw a frame (Vulkan error %d).", submitted);
            Recover();
        }
        return;
    }

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    const VkResult result = vkQueuePresentKHR(gpu.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        needsRecreate = true;
    else if (result == VK_ERROR_DEVICE_LOST)
        gpu.ReportLost();
}

// After a frame was acquired but not submitted: its fence will never be signalled and the acquired semaphore
// stays signalled, so both are made again, and the swapchain too, which takes back the image.
void Surface::Recover()
{
    vkDeviceWaitIdle(gpu.device);
    DestroySync();
    CreateSync();
    needsRecreate = true;
}
