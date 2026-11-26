#pragma once

// What add-ons share frames through on macOS and Linux. Part of addon_frames.cpp.

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
    Image motion; // R16G16_FLOAT, which only add-ons write and read
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
                                   int32_t(format == VK_FORMAT_R32_SFLOAT      ? kCVPixelFormatType_OneComponent32Float
                                           : format == VK_FORMAT_R16G16_SFLOAT ? kCVPixelFormatType_TwoComponent16Half
                                           : half                              ? kCVPixelFormatType_64RGBAHalf
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
        if (!color.handle && !output.handle && !depth.handle && !motion.handle && !after.handle && !sdr.handle && !convert.framebuffer)
            return;
        vkDeviceWaitIdle(gpu.device);
        for (Image* image : { &color, &output, &depth, &motion, &after, &sdr })
            Destroy(*image);
        backdrop::Destroy(convert);
        width = height = 0;
    }

    // A new size or format makes every image again.
    void Prepare(uint32_t newWidth, uint32_t newHeight, bool newHdr, bool wantOutput, bool wantDepth, bool wantMotion, bool wantAfter, bool wantSdr)
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
        if (wantMotion && !motion.handle)
            Create(motion, VK_FORMAT_R16G16_SFLOAT);
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
