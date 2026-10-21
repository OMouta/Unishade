#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

// One Vulkan device for everything: effects, the overlay and the launcher. MoltenVK provides it on macOS.
struct GpuImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1; // for 3D images
    uint32_t levels = 1;
    VkImageView view = VK_NULL_HANDLE;     // every level
    VkImageView srgbView = VK_NULL_HANDLE; // every level read as sRGB, or view when the format has no sRGB variant
    VkImageView target = VK_NULL_HANDLE;     // level 0, for rendering
    VkImageView srgbTarget = VK_NULL_HANDLE; // level 0 written as sRGB
    std::vector<VkImageView> storage;        // one per level when the image allows storage
};

struct GpuBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr; // host-visible buffers stay mapped
    bool coherent = true;   // otherwise the graphics card's writes need Gpu::Invalidate before reading
};

class Gpu
{
public:
    // Headless skips everything windows need, for rendering effects into files.
    bool Init(bool headless, std::string& error);
    void Shutdown();

    // Every image the host makes is kept in VK_IMAGE_LAYOUT_GENERAL, so passes need only memory barriers. Depth
    // and stencil formats make depth-stencil attachments.
    bool CreateImage(GpuImage& image, uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageUsageFlags usage,
                     VkImageType type = VK_IMAGE_TYPE_2D, uint32_t depth = 1);
    void DestroyImage(GpuImage& image);
    // Host-visible buffers are mapped. readback prefers memory the processor reads quickly, for results read back.
    bool CreateBuffer(GpuBuffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback = false);
    void DestroyBuffer(GpuBuffer& buffer);
    // Makes what the graphics card wrote to a mapped buffer visible to the processor.
    void Invalidate(const GpuBuffer& buffer);

    // Records commands and waits for them to finish. For uploads outside a frame. BeginCommands returns
    // VK_NULL_HANDLE when the graphics card is out of memory.
    VkCommandBuffer BeginCommands();
    bool SubmitAndWait(VkCommandBuffer commands);
    // Says that the graphics card stopped responding, once, and stops drawing.
    void ReportLost();

    bool Supports(VkFormat format, VkFormatFeatureFlags features) const;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    uint32_t apiVersion = VK_API_VERSION_1_1;
    bool independentBlend = false;
    bool anisotropy = false;
    bool metalObjects = false; // IOSurfaces can back images (macOS)
    bool dmaBuf = false;       // dma-bufs can be imported as images (Linux)
    bool foreignQueue = false; // VK_EXT_queue_family_foreign, for handing dma-bufs back and forth
    bool lost = false;         // the device was lost, so nothing can draw until Unishade starts again

    // Allocates device memory of the given type bits for an image or import. UINT32_MAX when none fits.
    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const { return MemoryType(bits, flags); }

private:
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
};

extern Gpu gpu;

// The sRGB variant of a format, or the format itself when it has none.
VkFormat SrgbFormat(VkFormat format);
// Makes everything written before visible to everything after. Effects are short passes over whole images, so
// finer barriers would not gain much.
void FullBarrier(VkCommandBuffer commands);
// Moves a fresh image into VK_IMAGE_LAYOUT_GENERAL.
void InitLayout(VkCommandBuffer commands, const GpuImage& image);
// The aspects of a format: color, or depth and stencil.
VkImageAspectFlags Aspects(VkFormat format);

// A window's swapchain, with one frame in flight.
class Surface
{
public:
    bool Create(GLFWwindow* window, bool lowLatency, std::string& error);
    void Destroy();

    // Waits for the previous frame, acquires an image and starts recording. The image is ready to be cleared or
    // copied into (VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL). Returns false when there is nothing to draw on, such
    // as a minimized window. With wait=false, skips the frame if the previous frame or an image is not ready.
    bool BeginFrame(bool wait = true);
    // Starts the render pass Dear ImGui draws in, keeping what was copied into the image.
    void BeginRenderPass();
    void EndFrame();

    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t minImageCount = 2;
    uint32_t imageCount = 0;
    // Set when the swapchain was made again, so Dear ImGui's backend can be told the new image count.
    bool recreated = false;

private:
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateSync();
    void DestroySync();
    void Recover();

    GLFWwindow* window = nullptr;
    bool lowLatency = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderDone; // per image, since presenting holds it until the image returns
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t index = 0;
    bool needsRecreate = false;
    bool lostShown = false;
};
