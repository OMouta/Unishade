// macOS: windows come from CoreGraphics, pictures from ScreenCaptureKit, which copies one window even where
// other windows cover it, and shortcuts from Carbon's hot keys, which work without accessibility permission.

// For VK_EXT_metal_objects, which lets an IOSurface back a Vulkan image.
#define VK_USE_PLATFORM_METAL_EXT
#include "platform.h"
#include "gpu.h"
#include "log.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <libproc.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>

extern char** environ;

namespace
{
// What the capture queue hands the main thread. A stopped stream's callbacks can still run for a moment, so
// generation tells them apart: it changes under the lock whenever capture starts or stops, and a callback only hands
// over a frame or an error while it holds the lock and its generation is the current one.
struct CaptureState
{
    std::mutex mutex;
    platform::Frame ready;
    uint64_t serial = 0;
    std::string error;
    std::atomic<bool> running = false;
    std::atomic<uint64_t> generation = 0;
    uint32_t configWidth = 0;
    uint32_t configHeight = 0;
    bool reconfiguring = false;
    std::atomic<bool> onGpu = false; // frames go to the main thread as IOSurfaces instead of pixels
    std::atomic<bool> idle = false;  // nobody sees the frames, so a few a second do
};
CaptureState capture;

void FailCapture(uint64_t generation, const std::string& message)
{
    {
        std::lock_guard lock(capture.mutex);
        if (generation != capture.generation)
            return;
        capture.error = message;
        capture.running = false;
    }
    glfwPostEmptyEvent();
}
} // namespace

@interface UnishadeStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) uint64_t generation;
@property(nonatomic, strong) SCStream* stream;
@end

@implementation UnishadeStreamOutput
{
    // The frame being filled. Only this stream's callbacks use it, one at a time on the capture queue.
    platform::Frame _back;
    std::chrono::steady_clock::time_point _handedOver;
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || self.generation != capture.generation || !CMSampleBufferIsValid(sampleBuffer))
        return;
    // While the overlay is hidden, a few frames a second keep it from showing an old one when it comes back.
    const auto now = std::chrono::steady_clock::now();
    if (capture.idle && now - _handedOver < std::chrono::milliseconds(200))
        return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (!attachments || CFArrayGetCount(attachments) == 0)
        return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    // Idle frames repeat the last one, which the host already has.
    if (!status || status.integerValue != SCFrameStatusComplete)
        return;
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!image)
        return;

    const size_t bufferWidth = CVPixelBufferGetWidth(image), bufferHeight = CVPixelBufferGetHeight(image);
    // The window's part of the frame. It is smaller than the frame while the stream catches up with a resize.
    size_t x0 = 0, y0 = 0, width = bufferWidth, height = bufferHeight;
    CGRect content = CGRectZero;
    NSDictionary* rect = info[SCStreamFrameInfoContentRect];
    const double scaleFactor = [info[SCStreamFrameInfoScaleFactor] doubleValue];
    const double contentScale = [info[SCStreamFrameInfoContentScale] doubleValue];
    if (rect && scaleFactor > 0 && CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)rect, &content))
    {
        x0 = std::min<size_t>(bufferWidth - 1, size_t(std::lround(content.origin.x * scaleFactor)));
        y0 = std::min<size_t>(bufferHeight - 1, size_t(std::lround(content.origin.y * scaleFactor)));
        width = std::clamp<size_t>(size_t(std::lround(content.size.width * scaleFactor)), 1, bufferWidth - x0);
        height = std::clamp<size_t>(size_t(std::lround(content.size.height * scaleFactor)), 1, bufferHeight - y0);

        // The window's real size in pixels. A stream the wrong size scales the window, so it is set again.
        if (contentScale > 0)
        {
            const uint32_t wantedWidth = uint32_t(std::lround(content.size.width / contentScale * scaleFactor));
            const uint32_t wantedHeight = uint32_t(std::lround(content.size.height / contentScale * scaleFactor));
            bool reconfigure = false;
            {
                std::lock_guard lock(capture.mutex);
                reconfigure = self.generation == capture.generation && !capture.reconfiguring && wantedWidth > 0 && wantedHeight > 0 &&
                              (wantedWidth != capture.configWidth || wantedHeight != capture.configHeight);
                capture.reconfiguring |= reconfigure;
            }
            if (reconfigure)
            {
                SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
                config.width = wantedWidth;
                config.height = wantedHeight;
                config.pixelFormat = kCVPixelFormatType_32BGRA;
                config.showsCursor = NO;
                config.minimumFrameInterval = CMTimeMake(1, 240);
                config.queueDepth = 8;
                config.colorSpaceName = kCGColorSpaceSRGB;
                if (@available(macOS 14.0, *))
                    config.ignoreShadowsSingleWindow = YES;
                const uint64_t generation = self.generation;
                [stream updateConfiguration:config
                          completionHandler:^(NSError* error) {
                            std::lock_guard lock(capture.mutex);
                            if (generation != capture.generation)
                                return;
                            capture.reconfiguring = false;
                            if (!error)
                            {
                                capture.configWidth = wantedWidth;
                                capture.configHeight = wantedHeight;
                            }
                          }];
            }
        }
    }

    platform::Frame& back = _back;
    back.width = uint32_t(width);
    back.height = uint32_t(height);
    if (capture.onGpu && CVPixelBufferGetIOSurface(image))
    {
        // The frame stays on the graphics card. The main thread wraps its IOSurface in a Vulkan image, and the
        // pixel buffer is held until the frame is drawn, so ScreenCaptureKit does not reuse it before then.
        back.pixels.clear();
        back.image = VK_NULL_HANDLE; // set by TakeFrame
        back.x = uint32_t(x0);
        back.y = uint32_t(y0);
        back.hold = std::shared_ptr<void>(const_cast<void*>(CFRetain(image)), [](void* buffer) { CFRelease(buffer); });
    }
    else
    {
        back.image = VK_NULL_HANDLE;
        back.x = back.y = 0;
        back.hold.reset();
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
        const size_t stride = CVPixelBufferGetBytesPerRow(image);
        back.pixels.clear();
        if (base && CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_32BGRA)
        {
            back.pixels.resize(width * height);
            for (size_t y = 0; y < height; ++y)
            {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(base + (y0 + y) * stride) + x0;
                uint32_t* out = back.pixels.data() + y * width;
                for (size_t x = 0; x < width; ++x)
                    out[x] = row[x] | 0xFF000000u;
            }
        }
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        if (back.pixels.empty())
            return;
    }
    bool current = false;
    {
        std::lock_guard lock(capture.mutex);
        current = self.generation == capture.generation;
        if (current)
        {
            back.serial = ++capture.serial;
            std::swap(back, capture.ready);
        }
    }
    // A frame the main thread skipped goes back to ScreenCaptureKit's pool now, as does one of a stopped capture.
    back.hold.reset();
    if (current)
    {
        _handedOver = now;
        glfwPostEmptyEvent();
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    FailCapture(self.generation, std::string("Capture stopped: ") + error.localizedDescription.UTF8String);
}
@end

namespace platform
{
namespace
{
UnishadeStreamOutput* output = nil;
dispatch_queue_t captureQueue = nullptr;

// Stops a stream without waiting for it. The block keeps the stream and its output alive until it has stopped and
// the callbacks it queued have run, which find their generation stale and hand nothing over.
void StopStream(UnishadeStreamOutput* handler)
{
    SCStream* stream = handler.stream;
    handler.stream = nil;
    [stream stopCaptureWithCompletionHandler:^(NSError* error) {
      (void)error;
      dispatch_async(captureQueue, ^{
        (void)handler;
        (void)stream;
      });
    }];
}

// IOSurfaces wrapped in Vulkan images. ScreenCaptureKit cycles through a few surfaces, so each is wrapped once.
struct Imported
{
    IOSurfaceRef surface = nullptr; // retained
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
std::map<IOSurfaceID, Imported> imported;

// The caller makes sure the graphics card no longer uses them.
void ReleaseImported()
{
    for (auto& [id, entry] : imported)
    {
        vkDestroyImage(gpu.device, entry.image, nullptr);
        vkFreeMemory(gpu.device, entry.memory, nullptr);
        CFRelease(entry.surface);
    }
    imported.clear();
}

VkImage Import(IOSurfaceRef surface)
{
    const IOSurfaceID id = IOSurfaceGetID(surface);
    // Retained surfaces keep their IDs, so an ID names one surface for as long as it is here.
    if (const auto found = imported.find(id); found != imported.end())
        return found->second.image;
    // A resize brings new surfaces, so the old ones are dropped instead of piling up.
    if (imported.size() >= 12)
    {
        vkDeviceWaitIdle(gpu.device);
        ReleaseImported();
    }

    VkImportMetalIOSurfaceInfoEXT import{ VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT };
    import.ioSurface = surface;
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.pNext = &import;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_B8G8R8A8_UNORM;
    info.extent = { uint32_t(IOSurfaceGetWidth(surface)), uint32_t(IOSurfaceGetHeight(surface)), 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Imported entry;
    if (vkCreateImage(gpu.device, &info, nullptr, &entry.image) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    // Vulkan wants memory bound to every image. MoltenVK keeps using the IOSurface for its contents.
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(gpu.device, entry.image, &requirements);
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = gpu.FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.device, &allocation, nullptr, &entry.memory) != VK_SUCCESS ||
        vkBindImageMemory(gpu.device, entry.image, entry.memory, 0) != VK_SUCCESS)
    {
        vkDestroyImage(gpu.device, entry.image, nullptr);
        if (entry.memory)
            vkFreeMemory(gpu.device, entry.memory, nullptr);
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer commands = gpu.BeginCommands();
    GpuImage view;
    view.image = entry.image;
    if (commands)
        InitLayout(commands, view);
    if (!gpu.SubmitAndWait(commands))
    {
        // Commands that failed to finish may still use the image.
        vkDeviceWaitIdle(gpu.device);
        vkDestroyImage(gpu.device, entry.image, nullptr);
        vkFreeMemory(gpu.device, entry.memory, nullptr);
        return VK_NULL_HANDLE;
    }
    entry.surface = static_cast<IOSurfaceRef>(const_cast<void*>(CFRetain(surface)));
    imported[id] = entry;
    return entry.image;
}

HotkeyCallback hotkeyCallback;
std::map<int, EventHotKeyRef> hotkeys;
std::vector<std::pair<int, bool>> pendingHotkeys;
EventHandlerRef hotkeyHandler = nullptr;

NSArray* WindowList(CGWindowListOption options, CGWindowID window)
{
    return CFBridgingRelease(CGWindowListCopyWindowInfo(options, window));
}

// The main loop asks about the game's window more than once in each pass, which one answer serves.
NSDictionary* WindowInfo(CGWindowID window)
{
    static CGWindowID cachedWindow = kCGNullWindowID;
    static NSDictionary* cached = nil;
    static std::chrono::steady_clock::time_point cachedTime;
    const auto now = std::chrono::steady_clock::now();
    if (window != cachedWindow || now - cachedTime > std::chrono::milliseconds(10))
    {
        NSArray* list = WindowList(kCGWindowListOptionIncludingWindow, window);
        cached = list.count > 0 ? list[0] : nil;
        cachedWindow = window;
        cachedTime = now;
    }
    return cached;
}

bool Bounds(NSDictionary* info, Rect& bounds)
{
    CGRect rect;
    if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &rect))
        return false;
    bounds = { int(std::lround(rect.origin.x)), int(std::lround(rect.origin.y)), int(std::lround(rect.size.width)),
               int(std::lround(rect.size.height)) };
    return true;
}

// The window as ListWindows lists it.
std::optional<Window> Listed(NSDictionary* info)
{
    const int pid = [info[(id)kCGWindowOwnerPID] intValue];
    Rect bounds;
    if ([info[(id)kCGWindowLayer] intValue] != 0 || pid == getpid() || [info[(id)kCGWindowAlpha] doubleValue] <= 0 || !Bounds(info, bounds) ||
        bounds.width < 64 || bounds.height < 64)
        return std::nullopt;
    // Window titles need the screen recording permission. The program's name stands in without it.
    NSString* name = info[(id)kCGWindowName];
    NSString* owner = info[(id)kCGWindowOwnerName];
    NSString* title = name.length ? name : owner;
    if (!title.length)
        return std::nullopt;
    return Window{ [info[(id)kCGWindowNumber] unsignedLongLongValue], title.UTF8String, pid };
}

std::string ProcessArgument(int pid)
{
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid };
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int))
        return {};
    std::vector<char> buffer(size);
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0)
        return {};
    // argc, then the executable path, padding, then the arguments.
    size_t at = sizeof(int);
    while (at < size && buffer[at])
        ++at;
    while (at < size && !buffer[at])
        ++at;
    size_t end = at;
    while (end < size && buffer[end])
        ++end;
    return at < size ? std::string(buffer.data() + at, end - at) : std::string();
}

// Carbon key codes follow the keyboard's layout, not the alphabet.
UInt32 KeyCode(ImGuiKey key)
{
    static const std::map<ImGuiKey, UInt32> codes = {
        { ImGuiKey_A, kVK_ANSI_A }, { ImGuiKey_B, kVK_ANSI_B }, { ImGuiKey_C, kVK_ANSI_C }, { ImGuiKey_D, kVK_ANSI_D },
        { ImGuiKey_E, kVK_ANSI_E }, { ImGuiKey_F, kVK_ANSI_F }, { ImGuiKey_G, kVK_ANSI_G }, { ImGuiKey_H, kVK_ANSI_H },
        { ImGuiKey_I, kVK_ANSI_I }, { ImGuiKey_J, kVK_ANSI_J }, { ImGuiKey_K, kVK_ANSI_K }, { ImGuiKey_L, kVK_ANSI_L },
        { ImGuiKey_M, kVK_ANSI_M }, { ImGuiKey_N, kVK_ANSI_N }, { ImGuiKey_O, kVK_ANSI_O }, { ImGuiKey_P, kVK_ANSI_P },
        { ImGuiKey_Q, kVK_ANSI_Q }, { ImGuiKey_R, kVK_ANSI_R }, { ImGuiKey_S, kVK_ANSI_S }, { ImGuiKey_T, kVK_ANSI_T },
        { ImGuiKey_U, kVK_ANSI_U }, { ImGuiKey_V, kVK_ANSI_V }, { ImGuiKey_W, kVK_ANSI_W }, { ImGuiKey_X, kVK_ANSI_X },
        { ImGuiKey_Y, kVK_ANSI_Y }, { ImGuiKey_Z, kVK_ANSI_Z },
        { ImGuiKey_0, kVK_ANSI_0 }, { ImGuiKey_1, kVK_ANSI_1 }, { ImGuiKey_2, kVK_ANSI_2 }, { ImGuiKey_3, kVK_ANSI_3 },
        { ImGuiKey_4, kVK_ANSI_4 }, { ImGuiKey_5, kVK_ANSI_5 }, { ImGuiKey_6, kVK_ANSI_6 }, { ImGuiKey_7, kVK_ANSI_7 },
        { ImGuiKey_8, kVK_ANSI_8 }, { ImGuiKey_9, kVK_ANSI_9 },
        { ImGuiKey_F1, kVK_F1 }, { ImGuiKey_F2, kVK_F2 }, { ImGuiKey_F3, kVK_F3 }, { ImGuiKey_F4, kVK_F4 }, { ImGuiKey_F5, kVK_F5 },
        { ImGuiKey_F6, kVK_F6 }, { ImGuiKey_F7, kVK_F7 }, { ImGuiKey_F8, kVK_F8 }, { ImGuiKey_F9, kVK_F9 }, { ImGuiKey_F10, kVK_F10 },
        { ImGuiKey_F11, kVK_F11 }, { ImGuiKey_F12, kVK_F12 }, { ImGuiKey_F13, kVK_F13 }, { ImGuiKey_F14, kVK_F14 }, { ImGuiKey_F15, kVK_F15 },
        { ImGuiKey_F16, kVK_F16 }, { ImGuiKey_F17, kVK_F17 }, { ImGuiKey_F18, kVK_F18 }, { ImGuiKey_F19, kVK_F19 }, { ImGuiKey_F20, kVK_F20 },
        { ImGuiKey_Home, kVK_Home }, { ImGuiKey_End, kVK_End }, { ImGuiKey_Insert, kVK_Help }, { ImGuiKey_Delete, kVK_ForwardDelete },
        { ImGuiKey_PageUp, kVK_PageUp }, { ImGuiKey_PageDown, kVK_PageDown }, { ImGuiKey_Space, kVK_Space }, { ImGuiKey_Tab, kVK_Tab },
        { ImGuiKey_Escape, kVK_Escape }, { ImGuiKey_LeftArrow, kVK_LeftArrow }, { ImGuiKey_RightArrow, kVK_RightArrow },
        { ImGuiKey_UpArrow, kVK_UpArrow }, { ImGuiKey_DownArrow, kVK_DownArrow },
        { ImGuiKey_Keypad0, kVK_ANSI_Keypad0 }, { ImGuiKey_Keypad1, kVK_ANSI_Keypad1 }, { ImGuiKey_Keypad2, kVK_ANSI_Keypad2 },
        { ImGuiKey_Keypad3, kVK_ANSI_Keypad3 }, { ImGuiKey_Keypad4, kVK_ANSI_Keypad4 }, { ImGuiKey_Keypad5, kVK_ANSI_Keypad5 },
        { ImGuiKey_Keypad6, kVK_ANSI_Keypad6 }, { ImGuiKey_Keypad7, kVK_ANSI_Keypad7 }, { ImGuiKey_Keypad8, kVK_ANSI_Keypad8 },
        { ImGuiKey_Keypad9, kVK_ANSI_Keypad9 }, { ImGuiKey_KeypadMultiply, kVK_ANSI_KeypadMultiply }, { ImGuiKey_KeypadAdd, kVK_ANSI_KeypadPlus },
        { ImGuiKey_KeypadSubtract, kVK_ANSI_KeypadMinus }, { ImGuiKey_KeypadDecimal, kVK_ANSI_KeypadDecimal },
        { ImGuiKey_KeypadDivide, kVK_ANSI_KeypadDivide },
    };
    const auto found = codes.find(key);
    return found == codes.end() ? UINT32_MAX : found->second;
}

// Windows virtual-key codes, which ReShade's effects and presets use, by key code.
constexpr std::pair<CGKeyCode, uint8_t> kVirtualKeys[] = {
    { kVK_ANSI_A, 'A' }, { kVK_ANSI_B, 'B' }, { kVK_ANSI_C, 'C' }, { kVK_ANSI_D, 'D' }, { kVK_ANSI_E, 'E' }, { kVK_ANSI_F, 'F' },
    { kVK_ANSI_G, 'G' }, { kVK_ANSI_H, 'H' }, { kVK_ANSI_I, 'I' }, { kVK_ANSI_J, 'J' }, { kVK_ANSI_K, 'K' }, { kVK_ANSI_L, 'L' },
    { kVK_ANSI_M, 'M' }, { kVK_ANSI_N, 'N' }, { kVK_ANSI_O, 'O' }, { kVK_ANSI_P, 'P' }, { kVK_ANSI_Q, 'Q' }, { kVK_ANSI_R, 'R' },
    { kVK_ANSI_S, 'S' }, { kVK_ANSI_T, 'T' }, { kVK_ANSI_U, 'U' }, { kVK_ANSI_V, 'V' }, { kVK_ANSI_W, 'W' }, { kVK_ANSI_X, 'X' },
    { kVK_ANSI_Y, 'Y' }, { kVK_ANSI_Z, 'Z' },
    { kVK_ANSI_0, '0' }, { kVK_ANSI_1, '1' }, { kVK_ANSI_2, '2' }, { kVK_ANSI_3, '3' }, { kVK_ANSI_4, '4' },
    { kVK_ANSI_5, '5' }, { kVK_ANSI_6, '6' }, { kVK_ANSI_7, '7' }, { kVK_ANSI_8, '8' }, { kVK_ANSI_9, '9' },
    { kVK_F1, 0x70 }, { kVK_F2, 0x71 }, { kVK_F3, 0x72 }, { kVK_F4, 0x73 }, { kVK_F5, 0x74 }, { kVK_F6, 0x75 }, { kVK_F7, 0x76 },
    { kVK_F8, 0x77 }, { kVK_F9, 0x78 }, { kVK_F10, 0x79 }, { kVK_F11, 0x7A }, { kVK_F12, 0x7B }, { kVK_F13, 0x7C }, { kVK_F14, 0x7D },
    { kVK_F15, 0x7E }, { kVK_F16, 0x7F }, { kVK_F17, 0x80 }, { kVK_F18, 0x81 }, { kVK_F19, 0x82 }, { kVK_F20, 0x83 },
    { kVK_Delete, 0x08 }, { kVK_Tab, 0x09 }, { kVK_Return, 0x0D }, { kVK_ANSI_KeypadEnter, 0x0D }, { kVK_CapsLock, 0x14 },
    { kVK_Escape, 0x1B }, { kVK_Space, 0x20 }, { kVK_PageUp, 0x21 }, { kVK_PageDown, 0x22 }, { kVK_End, 0x23 }, { kVK_Home, 0x24 },
    { kVK_LeftArrow, 0x25 }, { kVK_UpArrow, 0x26 }, { kVK_RightArrow, 0x27 }, { kVK_DownArrow, 0x28 }, { kVK_Help, 0x2D },
    { kVK_ForwardDelete, 0x2E }, { kVK_Command, 0x5B }, { kVK_RightCommand, 0x5C },
    { kVK_ANSI_Keypad0, 0x60 }, { kVK_ANSI_Keypad1, 0x61 }, { kVK_ANSI_Keypad2, 0x62 }, { kVK_ANSI_Keypad3, 0x63 }, { kVK_ANSI_Keypad4, 0x64 },
    { kVK_ANSI_Keypad5, 0x65 }, { kVK_ANSI_Keypad6, 0x66 }, { kVK_ANSI_Keypad7, 0x67 }, { kVK_ANSI_Keypad8, 0x68 }, { kVK_ANSI_Keypad9, 0x69 },
    { kVK_ANSI_KeypadMultiply, 0x6A }, { kVK_ANSI_KeypadPlus, 0x6B }, { kVK_ANSI_KeypadMinus, 0x6D }, { kVK_ANSI_KeypadDecimal, 0x6E },
    { kVK_ANSI_KeypadDivide, 0x6F },
    { kVK_Shift, 0xA0 }, { kVK_RightShift, 0xA1 }, { kVK_Control, 0xA2 }, { kVK_RightControl, 0xA3 }, { kVK_Option, 0xA4 },
    { kVK_RightOption, 0xA5 },
    { kVK_ANSI_Semicolon, 0xBA }, { kVK_ANSI_Equal, 0xBB }, { kVK_ANSI_Comma, 0xBC }, { kVK_ANSI_Minus, 0xBD }, { kVK_ANSI_Period, 0xBE },
    { kVK_ANSI_Slash, 0xBF }, { kVK_ANSI_Grave, 0xC0 }, { kVK_ANSI_LeftBracket, 0xDB }, { kVK_ANSI_Backslash, 0xDC },
    { kVK_ANSI_RightBracket, 0xDD }, { kVK_ANSI_Quote, 0xDE },
};

OSStatus OnHotkey(EventHandlerCallRef, EventRef event, void*)
{
    EventHotKeyID id{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id) != noErr)
        return eventNotHandledErr;
    // Handled from the main loop, outside of AppKit's event dispatch.
    pendingHotkeys.emplace_back(int(id.id), GetEventKind(event) == kEventHotKeyPressed);
    glfwPostEmptyEvent();
    return noErr;
}

void Spawn(const char* program, const std::string& argument)
{
    const char* argv[] = { program, argument.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, program, nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0)
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}
} // namespace

bool Init(std::string&)
{
    captureQueue = dispatch_queue_create("me.unishade.capture", DISPATCH_QUEUE_SERIAL);
    const EventTypeSpec types[] = { { kEventClassKeyboard, kEventHotKeyPressed }, { kEventClassKeyboard, kEventHotKeyReleased } };
    InstallApplicationEventHandler(&OnHotkey, 2, types, nullptr, &hotkeyHandler);
    return true;
}

void Shutdown()
{
    StopCapture();
    for (const auto& [id, ref] : std::map<int, EventHotKeyRef>(hotkeys))
        UnregisterHotkey(id);
    if (hotkeyHandler)
        RemoveEventHandler(hotkeyHandler);
    hotkeyHandler = nullptr;
}

std::vector<Window> ListWindows()
{
    std::vector<Window> windows;
    for (NSDictionary* info in WindowList(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
        if (std::optional<Window> window = Listed(info))
            windows.push_back(std::move(*window));
    std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
    return windows;
}

std::optional<Window> ListedWindow(WindowId window)
{
    NSDictionary* info = WindowInfo(CGWindowID(window));
    return info && [info[(id)kCGWindowIsOnscreen] boolValue] ? Listed(info) : std::nullopt;
}

std::set<int> WindowOwners()
{
    std::set<int> owners;
    for (NSDictionary* info in WindowList(kCGWindowListOptionAll, kCGNullWindowID))
        owners.insert([info[(id)kCGWindowOwnerPID] intValue]);
    return owners;
}

bool WindowExists(const Window& window)
{
    NSDictionary* info = WindowInfo(CGWindowID(window.id));
    return info && [info[(id)kCGWindowOwnerPID] intValue] == window.pid;
}

// The whole window, title bar included, since that is what ScreenCaptureKit copies of a single window.
bool WindowBounds(WindowId window, Rect& bounds)
{
    NSDictionary* info = WindowInfo(CGWindowID(window));
    return info && [info[(id)kCGWindowIsOnscreen] boolValue] && Bounds(info, bounds) && bounds.width > 0 && bounds.height > 0;
}

WindowId ForegroundWindow()
{
    const int pid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    // The list is in front-to-back order, so the first window of the program in front is its front window.
    for (NSDictionary* info in WindowList(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
        if ([info[(id)kCGWindowOwnerPID] intValue] == pid && [info[(id)kCGWindowLayer] intValue] == 0)
            return [info[(id)kCGWindowNumber] unsignedLongLongValue];
    return 0;
}

bool ProcessInFront(int pid)
{
    return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == pid;
}

void Activate(const Window& window)
{
    NSRunningApplication* target = [NSRunningApplication runningApplicationWithProcessIdentifier:window.pid];
    if (!target)
        return;
    if (@available(macOS 14.0, *))
    {
        [NSApp yieldActivationToApplication:target];
        [target activateWithOptions:0];
    }
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [target activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
    }
}

std::string ProcessExecutable(int pid)
{
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const int size = pid > 0 ? proc_pidpath(pid, path, sizeof(path)) : 0;
    return size > 0 ? std::string(path, size) : std::string();
}

std::string ProcessCommand(int pid)
{
    return ProcessArgument(pid);
}

std::vector<Process> ListProcesses()
{
    std::vector<pid_t> pids(4096);
    const int count = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    std::vector<Process> processes;
    for (int i = 0; i < count && i < int(pids.size()); ++i)
    {
        std::string executable = ProcessExecutable(pids[i]);
        if (executable.empty())
            continue;
        // Only Wine starts programs whose arguments name another executable, so the arguments are read only there.
        const std::string name = executable.substr(executable.find_last_of('/') + 1);
        std::string command = name.find("wine") != std::string::npos || name.find("preloader") != std::string::npos ? ProcessArgument(pids[i]) : "";
        processes.push_back({ pids[i], std::move(executable), std::move(command) });
    }
    return processes;
}

bool SaveWindowIcon(const Window& window, const std::string& path)
{
    @autoreleasepool
    {
        NSImage* icon = [NSRunningApplication runningApplicationWithProcessIdentifier:window.pid].icon;
        if (!icon)
            return false;
        // Drawn at 256 pixels from whichever of the icon's sizes fits best.
        const NSInteger size = 256;
        NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                           pixelsWide:size
                                                                           pixelsHigh:size
                                                                        bitsPerSample:8
                                                                      samplesPerPixel:4
                                                                             hasAlpha:YES
                                                                             isPlanar:NO
                                                                       colorSpaceName:NSCalibratedRGBColorSpace
                                                                          bytesPerRow:0
                                                                         bitsPerPixel:0];
        NSGraphicsContext* context = bitmap ? [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap] : nil;
        if (!context)
            return false;
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = context;
        [icon drawInRect:NSMakeRect(0, 0, size, size) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];
        NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        NSString* file = [NSString stringWithUTF8String:path.c_str()];
        return png && file && [png writeToFile:file atomically:YES];
    }
}

void SetupOverlayWindow(GLFWwindow* window)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    // On every Space, beside full-screen games too, and never in the window cycle.
    overlay.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
                                 NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    overlay.level = NSStatusWindowLevel;
    overlay.hasShadow = NO;
    overlay.hidesOnDeactivate = NO;
}

void ShowOverlay(GLFWwindow* window, bool visible)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    if (visible)
        [overlay orderFrontRegardless];
    else
        [overlay orderOut:nil];
}

void FocusOverlay(GLFWwindow* window)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
    [overlay makeKeyAndOrderFront:nil];
}

bool StartCapture(const Window& window, std::string& error)
{
    StopCapture();
    Rect bounds;
    if (!WindowBounds(window.id, bounds))
    {
        error = "The window is not visible.";
        return false;
    }
    capture.onGpu = gpu.metalObjects;
    uint64_t generation = 0;
    {
        std::lock_guard lock(capture.mutex);
        generation = ++capture.generation;
        capture.error.clear();
        capture.ready = {};
        capture.reconfiguring = false;
        capture.running = true;
    }
    const CGWindowID windowId = CGWindowID(window.id);

    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:NO
                                 completionHandler:^(SCShareableContent* content, NSError* contentError) {
                                   if (generation != capture.generation)
                                       return;
                                   if (!content)
                                   {
                                       FailCapture(generation, std::string("Could not list windows to capture: ") +
                                                                   (contentError ? contentError.localizedDescription.UTF8String : "unknown error"));
                                       return;
                                   }
                                   SCWindow* target = nil;
                                   for (SCWindow* candidate in content.windows)
                                       if (candidate.windowID == windowId)
                                           target = candidate;
                                   if (!target)
                                   {
                                       FailCapture(generation, "The game's window cannot be captured.");
                                       return;
                                   }
                                   SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
                                   double scale = NSScreen.mainScreen.backingScaleFactor;
                                   if (@available(macOS 14.0, *))
                                       scale = filter.pointPixelScale;
                                   SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
                                   config.width = size_t(std::lround(target.frame.size.width * scale));
                                   config.height = size_t(std::lround(target.frame.size.height * scale));
                                   config.pixelFormat = kCVPixelFormatType_32BGRA;
                                   config.showsCursor = NO;
                                   // As fast as the game draws, so the overlay adds no delay of its own.
                                   config.minimumFrameInterval = CMTimeMake(1, 240);
                                   config.queueDepth = 8;
                                   config.colorSpaceName = kCGColorSpaceSRGB;
                                   if (@available(macOS 14.0, *))
                                       config.ignoreShadowsSingleWindow = YES;
                                   {
                                       std::lock_guard lock(capture.mutex);
                                       capture.configWidth = uint32_t(config.width);
                                       capture.configHeight = uint32_t(config.height);
                                   }

                                   UnishadeStreamOutput* handler = [[UnishadeStreamOutput alloc] init];
                                   handler.generation = generation;
                                   SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:handler];
                                   NSError* addError = nil;
                                   if (![stream addStreamOutput:handler type:SCStreamOutputTypeScreen sampleHandlerQueue:captureQueue error:&addError])
                                   {
                                       FailCapture(generation, std::string("Could not capture the game: ") + addError.localizedDescription.UTF8String);
                                       return;
                                   }
                                   handler.stream = stream;
                                   dispatch_async(dispatch_get_main_queue(), ^{
                                     // Stopped or replaced before it started.
                                     if (generation != capture.generation)
                                     {
                                         StopStream(handler);
                                         return;
                                     }
                                     output = handler;
                                   });
                                   [stream startCaptureWithCompletionHandler:^(NSError* startError) {
                                     if (startError)
                                         FailCapture(generation, std::string("Could not capture the game: ") + startError.localizedDescription.UTF8String);
                                   }];
                                 }];
    return true;
}

void StopCapture()
{
    {
        std::lock_guard lock(capture.mutex);
        ++capture.generation;
        capture.running = false;
        capture.ready = {};
    }
    if (output)
    {
        StopStream(output);
        output = nil;
    }
    // The host waited for the graphics card before stopping.
    ReleaseImported();
}

bool Capturing()
{
    return capture.running;
}

void SetCaptureIdle(bool idle)
{
    capture.idle = idle;
}

bool TakeFrame(Frame& frame)
{
    {
        std::lock_guard lock(capture.mutex);
        if (capture.ready.serial <= frame.serial || (capture.ready.pixels.empty() && !capture.ready.hold))
            return false;
        std::swap(frame, capture.ready);
    }
    if (frame.hold)
    {
        frame.image = Import(CVPixelBufferGetIOSurface(static_cast<CVPixelBufferRef>(frame.hold.get())));
        if (!frame.image)
        {
            // Stay with copies through memory from the next frame on.
            Log(LogLevel::Warning, "Could not use captured frames on the graphics card. Copying them instead.");
            capture.onGpu = false;
            return false;
        }
    }
    return true;
}

std::string CaptureError()
{
    std::lock_guard lock(capture.mutex);
    return capture.error;
}

bool HasCapturePermission()
{
    return CGPreflightScreenCaptureAccess();
}

void RequestCapturePermission()
{
    // Asks once. After that, only System Settings can change it.
    if (!CGRequestScreenCaptureAccess())
        Open("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture");
}

void SetHotkeyCallback(HotkeyCallback callback)
{
    hotkeyCallback = std::move(callback);
}

bool RegisterHotkey(int id, const Hotkey& hotkey)
{
    UnregisterHotkey(id);
    const UInt32 code = KeyCode(hotkey.key);
    if (code == UINT32_MAX)
        return false;
    const UInt32 modifiers = ((hotkey.modifiers & kCtrl) ? controlKey : 0) | ((hotkey.modifiers & kAlt) ? optionKey : 0) |
                             ((hotkey.modifiers & kShift) ? shiftKey : 0) | ((hotkey.modifiers & kSuper) ? cmdKey : 0);
    EventHotKeyRef ref = nullptr;
    const EventHotKeyID hotkeyId{ 'UNSH', UInt32(id) };
    if (RegisterEventHotKey(code, modifiers, hotkeyId, GetApplicationEventTarget(), 0, &ref) != noErr || !ref)
        return false;
    hotkeys[id] = ref;
    return true;
}

void UnregisterHotkey(int id)
{
    const auto found = hotkeys.find(id);
    if (found == hotkeys.end())
        return;
    UnregisterEventHotKey(found->second);
    hotkeys.erase(found);
}

void PollHotkeys()
{
    std::vector<std::pair<int, bool>> events;
    events.swap(pendingHotkeys);
    for (const auto& [id, pressed] : events)
        if (hotkeyCallback && hotkeys.count(id))
            hotkeyCallback(id, pressed);
}

void Open(const std::string& target)
{
    Spawn("open", target);
}

std::string UiFont(bool bold)
{
    if (bold)
    {
        for (const char* path : { "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf" })
            if (access(path, R_OK) == 0)
                return path;
        return {};
    }
    for (const char* path : { "/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial.ttf" })
        if (access(path, R_OK) == 0)
            return path;
    return {};
}

std::vector<std::pair<std::string, int>> UiFallbackFonts(bool)
{
    // Symbols, then Chinese, Japanese and Korean.
    std::vector<std::pair<std::string, int>> fonts;
    for (const char* path : { "/System/Library/Fonts/Apple Symbols.ttf", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                              "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc" })
        if (access(path, R_OK) == 0)
            fonts.emplace_back(path, 0);
    return fonts;
}

void ReadInput(std::array<bool, 256>& keys, std::array<bool, 5>& buttons)
{
    keys = {};
    for (const auto& [code, key] : kVirtualKeys)
        if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, code))
            keys[key] = true;
    const CGMouseButton mouseButtons[] = { kCGMouseButtonLeft, kCGMouseButtonRight, kCGMouseButtonCenter, CGMouseButton(3), CGMouseButton(4) };
    for (size_t i = 0; i < buttons.size(); ++i)
        buttons[i] = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, mouseButtons[i]);
}

bool WaylandDesktop()
{
    return false;
}

bool DisplayDrmDevice(int64_t&, int64_t&)
{
    return false;
}
} // namespace platform
