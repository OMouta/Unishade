#pragma once

// What add-ons share frames through on Windows. Part of addon_frames.cpp.

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
    Image motion; // R16G16_FLOAT, which only add-ons write and read
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
        if (!color.texture && !output.texture && !depth.texture && !motion.texture && !after.texture && !sdr.texture && !convert.framebuffer)
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
        if (wantMotion && !motion.texture)
            Create(motion, DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT);
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
