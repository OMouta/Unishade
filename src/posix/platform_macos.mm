// macOS: windows come from CoreGraphics, pictures from ScreenCaptureKit, which copies one window even where
// other windows cover it, and shortcuts from Carbon's hot keys, which work without accessibility permission.

// For VK_EXT_metal_objects, which lets an IOSurface back a Vulkan image.
#define VK_USE_PLATFORM_METAL_EXT
#include "platform.h"
#include "gpu.h"
#include "log.h"
#include "macho.h"
#include "window_rules.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// Added to GLFW by posix.cmake: the next window GLFW creates is a non-activating panel.
extern "C" void glfwCocoaPanelHint(int panel);

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
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
#include <vector>

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
    bool stoppedByUser = false;
    std::atomic<bool> running = false;
    std::atomic<uint64_t> generation = 0;
    uint32_t configWidth = 0;
    uint32_t configHeight = 0;
    bool reconfiguring = false;
    std::atomic<bool> onGpu = false; // frames go to the main thread as IOSurfaces instead of pixels
    std::atomic<bool> idle = false;  // nobody sees the frames, so a few a second do
    // The height in points of the title bar the frames leave out, or 0. Only the main thread can tell, since it needs
    // the screens, so it sets this and the capture queue reads it for each frame.
    std::atomic<int> titleInset = 0;
    // ScreenCaptureKit said the user refused to let Unishade record the screen, which CGPreflightScreenCaptureAccess
    // does not notice while Unishade runs.
    std::atomic<bool> declined = false;
};
CaptureState capture;

void FailCapture(uint64_t generation, const std::string& message, bool stoppedByUser = false)
{
    {
        std::lock_guard lock(capture.mutex);
        if (generation != capture.generation)
            return;
        capture.error = message;
        capture.stoppedByUser = stoppedByUser;
        capture.running = false;
    }
    glfwPostEmptyEvent();
}

// Stops capture for a ScreenCaptureKit error, saying what it means for Unishade. The code goes to the log too, since
// the system's descriptions of these errors are vague.
void FailWithError(uint64_t generation, NSError* error, const char* what)
{
    if (generation != capture.generation)
        return;
    const NSInteger code = error ? error.code : 0;
    Log(LogLevel::Info, "%s: domain=%s, code=%ld.", what, error ? error.domain.UTF8String : "none", long(code));
    if (error && [error.domain isEqualToString:SCStreamErrorDomain])
        switch (code)
        {
        case SCStreamErrorUserDeclined:
            capture.declined = true;
            FailCapture(generation, "Unishade needs permission to record the screen.");
            return;
        case SCStreamErrorUserStopped:
            FailCapture(generation, "Recording was stopped from the menu bar.", true);
            return;
        case SCStreamErrorNoCaptureSource:
            FailCapture(generation, "The game's window closed.");
            return;
        default:
            // Such as the connection to the capture service breaking, or macOS 15 stopping the stream itself. The
            // host starts capture again after a moment. A stop Unishade asked for never comes here, since StopStream
            // ignores its result, so even an error about stopping means this stream is dead.
            break;
        }
    FailCapture(generation, std::string(what) + ": " + (error ? error.localizedDescription.UTF8String : "unknown error"));
}

// One configuration for starting and resizing the stream, so the two never differ.
SCStreamConfiguration* StreamConfiguration(size_t width, size_t height)
{
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = width;
    config.height = height;
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.colorSpaceName = kCGColorSpaceSRGB;
    config.showsCursor = NO;
    // As fast as the game draws, so the overlay adds no delay of its own.
    config.minimumFrameInterval = CMTimeMake(1, 240);
    config.queueDepth = 8;
    // The background stays clear and shouldBeOpaque stays off. Only the rounded corners of a window are not covered
    // by it, and the host makes every pixel opaque, which turns them black. shouldBeOpaque would make them white,
    // which effects that spread light, such as bloom, would carry into the picture.
    if (@available(macOS 14.0, *))
    {
        config.ignoreShadowsSingleWindow = YES;
        // A window partly off the screen is captured whole, so the frame keeps the window's size and the overlay's
        // picture lines up with it.
        config.ignoreGlobalClipSingleWindow = YES;
        // From the screen's pixels rather than its points, so a Retina screen gives the game's full detail.
        config.captureResolution = SCCaptureResolutionBest;
    }
    // includeChildWindows stays as it is: it only applies to display and application filters, and a filter of one
    // window has none of the game's other windows in it either way.
    return config;
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
    if (bufferWidth == 0 || bufferHeight == 0)
        return;
    // The window's part of the frame. It is smaller than the frame while the stream catches up with a resize. The
    // stream copies the whole window, so its title bar is cut off here, as WindowBounds leaves it out of the overlay.
    window_rules::Crop crop{ 0, 0, bufferWidth, bufferHeight };
    CGRect content = CGRectZero;
    NSDictionary* rect = info[SCStreamFrameInfoContentRect];
    const double scaleFactor = [info[SCStreamFrameInfoScaleFactor] doubleValue];
    const double contentScale = [info[SCStreamFrameInfoContentScale] doubleValue];
    if (rect && scaleFactor > 0 && CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)rect, &content))
    {
        // Each of the window's points is contentScale of the frame's points, which are scaleFactor pixels each.
        const double pixelsPerPoint = scaleFactor * (contentScale > 0 ? contentScale : 1);
        crop = window_rules::ContentCrop(bufferWidth, bufferHeight,
                                         { content.origin.x, content.origin.y, content.size.width, content.size.height }, scaleFactor,
                                         window_rules::TitleRows(capture.titleInset, pixelsPerPoint));

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
                SCStreamConfiguration* config = StreamConfiguration(wantedWidth, wantedHeight);
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

    const size_t x0 = crop.x, y0 = crop.y, width = crop.width, height = crop.height;
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
    FailWithError(self.generation, error, "Capture stopped");
}
@end

namespace platform
{
namespace
{
UnishadeStreamOutput* output = nil;
dispatch_queue_t captureQueue = nullptr;
// The window being captured, for keeping capture.titleInset up to date. Main thread only.
CGWindowID capturedWindow = kCGNullWindowID;
bool skipTitleBar = true;
// Held while capture runs. Unishade's windows are an overlay that never has focus and a launcher that is often
// covered, which App Nap would otherwise take as a program nobody uses and slow down.
id<NSObject> activity = nil;

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
        // Waiting for the graphics card holds up a frame, which shows in the log if the game stutters.
        Log(LogLevel::Info, "Dropping %zu captured surfaces on the graphics card for new ones.", imported.size());
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

// A screen in CoreGraphics' coordinates, with its scale, how far down full-screen windows may start, the height of
// its menu bar, 0 while it has none or the menu bar hides itself, and whether the Dock is on it. The Dock keeps a
// window over each screen, also while it hides itself, and macOS takes that window away in a full-screen Space.
struct Screen
{
    window_rules::Box frame;
    double scale = 1;
    double safeTop = 0;
    double menuBar = 0;
    bool dock = true;
};

// The Dock's windows that are on a screen now.
std::vector<CGRect> DockWindows()
{
    std::vector<CGRect> docks;
    NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
    for (NSDictionary* info in windows)
    {
        CGRect bounds;
        if ([info[(id)kCGWindowLayer] intValue] == kCGDockWindowLevel && [info[(id)kCGWindowOwnerName] isEqual:@"Dock"] &&
            CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &bounds))
            docks.push_back(bounds);
    }
    return docks;
}

// Asked again at most once a second, since screens rarely change and the main loop asks often.
const std::vector<Screen>& Screens()
{
    static std::vector<Screen> screens;
    static std::chrono::steady_clock::time_point checked;
    const auto now = std::chrono::steady_clock::now();
    if (!screens.empty() && now - checked < std::chrono::seconds(1))
        return screens;
    checked = now;
    screens.clear();
    NSArray<NSScreen*>* all = NSScreen.screens;
    // AppKit counts up from the bottom left corner of the first screen, CoreGraphics down from its top left corner.
    const double top = all.count > 0 ? NSMaxY(all[0].frame) : 0;
    const std::vector<CGRect> docks = DockWindows();
    for (NSScreen* screen in all)
    {
        const NSRect frame = screen.frame;
        const CGRect area = CGRectMake(frame.origin.x, top - NSMaxY(frame), frame.size.width, frame.size.height);
        const bool dock = std::any_of(docks.begin(), docks.end(), [&](const CGRect& bounds) { return CGRectIntersectsRect(bounds, area); });
        screens.push_back({ { area.origin.x, area.origin.y, area.size.width, area.size.height }, screen.backingScaleFactor,
                            screen.safeAreaInsets.top, std::max(NSMaxY(frame) - NSMaxY(screen.visibleFrame), 0.0), dock });
    }
    return screens;
}

window_rules::Box ToBox(const Rect& rect)
{
    return { double(rect.x), double(rect.y), double(rect.width), double(rect.height) };
}

bool FillsScreen(const Rect& bounds)
{
    const window_rules::Box box = ToBox(bounds);
    // One reference, since asking again can make a new list.
    const std::vector<Screen>& screens = Screens();
    return std::any_of(screens.begin(), screens.end(),
                       [&](const Screen& screen) { return window_rules::CoversScreen(box, screen.frame, screen.safeTop, screen.menuBar, screen.dock); });
}

// The scale of the screen that shows most of the window, or of the first screen when none does.
double ScreenScale(const Rect& bounds)
{
    const CGRect window = CGRectMake(bounds.x, bounds.y, bounds.width, bounds.height);
    const std::vector<Screen>& screens = Screens();
    double scale = screens.empty() ? 2 : screens[0].scale, most = 0;
    for (const Screen& screen : screens)
    {
        const CGRect shown = CGRectIntersection(window, CGRectMake(screen.frame.x, screen.frame.y, screen.frame.width, screen.frame.height));
        const double area = CGRectIsNull(shown) ? 0 : shown.size.width * shown.size.height;
        if (area > most)
        {
            most = area;
            scale = screen.scale;
        }
    }
    return scale;
}

// Whether the process is a program with a Dock icon, as games are. Programs without one draw the Dock, the menu bar,
// status items and overlays. Kept for a few seconds, since window lists ask about many windows at a time.
bool RegularProgram(int pid)
{
    static std::map<int, std::pair<bool, std::chrono::steady_clock::time_point>> known;
    const auto now = std::chrono::steady_clock::now();
    if (const auto found = known.find(pid); found != known.end() && now - found->second.second < std::chrono::seconds(5))
        return found->second.first;
    if (known.size() > 256)
        known.clear();
    NSRunningApplication* program = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    const bool regular = program && program.activationPolicy == NSApplicationActivationPolicyRegular;
    known[pid] = { regular, now };
    return regular;
}

// Whether macOS draws the program's windows with macOS 26's look, a taller title bar and rounder corners. macOS 26
// gives programs built with an older SDK the earlier look, so this reads the SDK from the program's executable. Wine
// games are drawn by Wine, whose executable decides. An unreadable executable counts as the earlier look. Kept per
// process and checked again now and then, since process numbers are reused.
bool NewLook(int pid)
{
    // macOS 26 tells programs built with an older SDK that it is 16, which the check counts too.
    if (@available(macOS 16.0, *))
    {
        static std::map<int, std::pair<bool, std::chrono::steady_clock::time_point>> known;
        const auto now = std::chrono::steady_clock::now();
        if (const auto found = known.find(pid); found != known.end() && now - found->second.second < std::chrono::seconds(10))
            return found->second.first;
        if (known.size() > 64)
            known.clear();
        const std::string executable = ProcessExecutable(pid);
        const uint32_t sdk = macho::SdkMajorOfFile(executable);
        const bool look = sdk >= 26;
        if (known.find(pid) == known.end())
            Log(LogLevel::Info, "Window look: pid=%d, sdk=%u, new_look=%d, executable=%s.", pid, sdk, look, executable.c_str());
        known[pid] = { look, now };
        return look;
    }
    return false;
}

// The height in points of the title bar WindowBounds and the frames leave out, or 0. No public interface tells
// another program's content area without the accessibility permission, so this is a rule: a window has the standard
// title bar when it is on the normal layer and does not cover a screen. A window in full screen, native or
// borderless, covers its screen, and Wine raises its full-screen windows above the normal layer. A borderless game in
// a window would lose its top strip by this rule, which is why Settings can turn it off.
int TitleInset(NSDictionary* info)
{
    // The standard title bar's height for the game's look. Asking AppKit would answer for Unishade's own SDK instead.
    const int standard = NewLook([info[(id)kCGWindowOwnerPID] intValue]) ? 32 : 28;
    Rect bounds;
    if (!skipTitleBar || [info[(id)kCGWindowLayer] intValue] != window_rules::kNormalLevel || !Bounds(info, bounds) || bounds.height <= standard ||
        FillsScreen(bounds))
        return 0;
    return std::max(standard, 0);
}

// The window as ListWindows lists it.
std::optional<Window> Listed(NSDictionary* info)
{
    const int pid = [info[(id)kCGWindowOwnerPID] intValue];
    const int layer = [info[(id)kCGWindowLayer] intValue];
    Rect bounds;
    if (pid == getpid() || [info[(id)kCGWindowAlpha] doubleValue] <= 0 || !Bounds(info, bounds) || bounds.width < 64 || bounds.height < 64)
        return std::nullopt;
    // A game on its own full-screen Space is off the screen while another Space shows, the same as a minimized or
    // hidden window, which the window list cannot tell apart. Of those, only a window that covers a screen is listed,
    // since that is what a game on its own Space looks like and the hidden windows programs keep around are not.
    if (![info[(id)kCGWindowIsOnscreen] boolValue] && !(FillsScreen(bounds) && RegularProgram(pid)))
        return std::nullopt;
    if (!window_rules::GameLayer(layer, layer != window_rules::kNormalLevel && RegularProgram(pid), CGShieldingWindowLevel()))
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

bool Spawn(const char* program, const std::vector<std::string>& arguments)
{
    std::vector<const char*> argv{ program };
    for (const std::string& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, program, nullptr, nullptr, const_cast<char* const*>(argv.data()), environ) != 0)
        return false;
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    return true;
}
} // namespace

bool Init(std::string&)
{
    // Frames are what the user looks at, so their queue runs before other work. Each frame's Objective-C objects are
    // released when it is done, not whenever the thread's pool is drained.
    dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    attributes = dispatch_queue_attr_make_with_autorelease_frequency(attributes, DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM);
    captureQueue = dispatch_queue_create("me.unishade.capture", attributes);
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

// These functions run from the host's C++ loop, outside any autorelease pool AppKit drains, so each that makes
// Objective-C objects has a pool of its own. Without one they would pile up until GLFW next handles events.

std::vector<Window> ListWindows()
{
    @autoreleasepool
    {
        std::vector<Window> windows;
        // Every window, since a game on its own full-screen Space is off the screen while another Space shows.
        for (NSDictionary* info in WindowList(kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
            if (std::optional<Window> window = Listed(info))
                windows.push_back(std::move(*window));
        std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
        return windows;
    }
}

std::optional<Window> ListedWindow(WindowId window)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(window));
        return info ? Listed(info) : std::nullopt;
    }
}

std::set<int> WindowOwners()
{
    @autoreleasepool
    {
        std::set<int> owners;
        for (NSDictionary* info in WindowList(kCGWindowListOptionAll, kCGNullWindowID))
            owners.insert([info[(id)kCGWindowOwnerPID] intValue]);
        return owners;
    }
}

bool WindowExists(const Window& window)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(window.id));
        return info && [info[(id)kCGWindowOwnerPID] intValue] == window.pid;
    }
}

bool WindowBounds(WindowId window, Rect& bounds)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(window));
        if (!info || ![info[(id)kCGWindowIsOnscreen] boolValue] || !Bounds(info, bounds))
            return false;
        // The same title bar the frames leave out, decided at the same time.
        const int inset = TitleInset(info);
        if (window == capturedWindow)
            capture.titleInset = inset;
        bounds.y += inset;
        bounds.height -= inset;
        return bounds.width > 0 && bounds.height > 0;
    }
}

int WindowLayer(WindowId window)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(window));
        return info ? [info[(id)kCGWindowLayer] intValue] : 0;
    }
}

bool WindowFillsScreen(WindowId window)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(window));
        Rect bounds;
        return info && Bounds(info, bounds) && FillsScreen(bounds);
    }
}

bool WindowHasTitleBar(WindowId window)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(window));
        return info && TitleInset(info) > 0;
    }
}

void SetSkipTitleBar(bool skip)
{
    skipTitleBar = skip;
    // The next frame is cut to match.
    if (capturedWindow != kCGNullWindowID)
        @autoreleasepool
        {
            if (NSDictionary* info = WindowInfo(capturedWindow))
                capture.titleInset = TitleInset(info);
        }
}

WindowId ForegroundWindow()
{
    @autoreleasepool
    {
        const int pid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        // The list is in front-to-back order, so the first window of the program in front is its front window. One
        // above the normal layer, such as a Wine game in full screen, counts where ListWindows would list it.
        for (NSDictionary* info in WindowList(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID))
            if ([info[(id)kCGWindowOwnerPID] intValue] == pid &&
                ([info[(id)kCGWindowLayer] intValue] == window_rules::kNormalLevel || Listed(info)))
                return [info[(id)kCGWindowNumber] unsignedLongLongValue];
        return 0;
    }
}

bool ProcessInFront(int pid)
{
    @autoreleasepool
    {
        return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == pid;
    }
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

void PrepareOverlayWindow()
{
    glfwCocoaPanelHint(GLFW_TRUE);
}

void SetupOverlayWindow(GLFWwindow* window)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    if (NSPanel* panel = [overlay isKindOfClass:[NSPanel class]] ? (NSPanel*)overlay : nil)
    {
        // A panel acts as a palette unless told otherwise. Not floating, which would also set its level, and key
        // whenever the menu asks rather than only for controls that type. Nothing of it works during an alert.
        panel.floatingPanel = NO;
        panel.becomesKeyOnlyIfNeeded = NO;
        panel.worksWhenModal = NO;
    }
    else
        Log(LogLevel::Warning, "The overlay is not a panel, so opening the menu brings Unishade to the front.");
    // On every Space, beside full-screen games too, and never in the window cycle.
    overlay.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
                                 NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    overlay.level = NSStatusWindowLevel;
    overlay.hasShadow = NO;
    // The game stays on screen while Unishade is in the background, and while the user hides Unishade or every
    // program but the game.
    overlay.hidesOnDeactivate = NO;
    overlay.canHide = NO;
    // A window that lets what is under it show keeps macOS from taking the game for covered, which lets a program
    // draw less often and nap, and would slow capture down with it. The overlay still shows only its own pixels: the
    // swapchain's opaque composite alpha makes MoltenVK's layer opaque, and MatchOverlay keeps it so.
    overlay.opaque = NO;
    overlay.backgroundColor = NSColor.clearColor;
}

void ShowOverlay(GLFWwindow* window, bool visible)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    if (visible)
        [overlay orderFrontRegardless];
    else
        [overlay orderOut:nil];
}

void MatchOverlay(GLFWwindow* window, WindowId game)
{
    @autoreleasepool
    {
        NSWindow* overlay = glfwGetCocoaWindow(window);
        // Just above the game, which can be above the overlay's usual level, as Wine games in full screen are. Wine
        // raises and lowers its window as the game gains and loses focus, so the level follows it.
        const int layer = WindowLayer(game);
        const NSWindowLevel level = std::max<NSWindowLevel>(NSStatusWindowLevel, NSWindowLevel(layer) + 1);
        if (overlay.level != level)
        {
            Log(LogLevel::Info, "Overlay level: %ld, game layer: %d.", long(level), layer);
            overlay.level = level;
        }

        // The view's layer is MoltenVK's once the swapchain exists.
        CALayer* content = [(NSView*)glfwGetCocoaView(window) layer];
        if (!content)
            return;
        if (!content.opaque)
            content.opaque = YES;
        // A window with a title bar has round corners, where the captured picture is clear and would show black. The
        // overlay rounds its own corners to match: the bottom ones when it leaves out the title bar, all four when it
        // covers the whole window. A window that covers its screen or sits above the normal layer, as games in full
        // screen do, has square corners. macOS does not tell another program's corner radius, so these are measured
        // from a standard titled window and rounded up, since a sliver of the game without effects shows less than a
        // black notch: macOS 26's look draws 35 pixels at 2x along the bottom row, which an 18-point continuous curve
        // covers with a pixel or two to spare, and the earlier look draws about 10 points, rounded up to 12. Which
        // look the game has depends on its SDK. The layer is not flipped, so its bottom corners are the MinY ones.
        NSDictionary* info = WindowInfo(CGWindowID(game));
        const CACornerMask all = kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner | kCALayerMinXMaxYCorner | kCALayerMaxXMaxYCorner;
        CACornerMask corners = all;
        double radius = info && NewLook([info[(id)kCGWindowOwnerPID] intValue]) ? 18 : 12;
        if (WindowFillsScreen(game) || layer != window_rules::kNormalLevel)
            radius = 0;
        else if (WindowHasTitleBar(game))
            corners = kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner;
        if (content.cornerRadius != radius || content.maskedCorners != corners)
        {
            content.cornerCurve = kCACornerCurveContinuous;
            content.cornerRadius = radius;
            content.maskedCorners = corners;
            content.masksToBounds = radius > 0;
        }
    }
}

void FocusOverlay(GLFWwindow* window)
{
    NSWindow* overlay = glfwGetCocoaWindow(window);
    // The non-activating panel takes the keyboard while the game stays the active program, so macOS does not switch
    // away from the game's full-screen Space to the launcher's.
    const bool panel = overlay.styleMask & NSWindowStyleMaskNonactivatingPanel;
    if (panel)
    {
        [overlay orderFrontRegardless];
        [overlay makeKeyWindow];
    }
    // A game that hides the cursor, as games played with the mouse held do, lets go of the mouse only once it is no
    // longer the active program, so then Unishade becomes it. The panel is key first, so macOS has no other window of
    // Unishade's to go to and stays on the game's Space. CGCursorIsVisible is deprecated without a replacement and
    // answers for the whole session, so it sees a cursor another program hid.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const bool cursorHidden = !CGCursorIsVisible();
#pragma clang diagnostic pop
    if (panel && !cursorHidden)
    {
        Log(LogLevel::Info, "Menu input: the game stays in front.");
        return;
    }
    Log(LogLevel::Info, "Menu input: Unishade comes to the front, since %s.", panel ? "the game hides the cursor" : "the overlay is not a panel");
    // NSApp.active is already true once the panel is key, so the newer [NSApp activate] does nothing here and the
    // game keeps the mouse. The deprecated call still takes the front on macOS 26.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    [overlay makeKeyAndOrderFront:nil];
}

void ReturnFocus(GLFWwindow* window, const Window& game)
{
    @autoreleasepool
    {
        NSWindow* overlay = glfwGetCocoaWindow(window);
        // When the menu made Unishade the program in front, activating the game gives it the keyboard and the mouse
        // back, and takes the key window from Unishade. The panel is not ordered out then: in the active program
        // AppKit would make the launcher key, which could take the user to the launcher's Space. NSApp.active does
        // not answer this: it stays true while the game is in front and the panel is key.
        if (!ProcessInFront(game.pid))
        {
            Activate(game);
            return;
        }
        // Otherwise the game stayed the active program, and a key panel stays key until another window becomes key,
        // which the game has no reason to ask for. Ordered out, the panel gives the keyboard back to the game's
        // window at once, and ordered straight back in it is missing for a frame at most.
        if (overlay.keyWindow)
        {
            const bool shown = overlay.visible;
            [overlay orderOut:nil];
            if (shown)
                [overlay orderFrontRegardless];
        }
    }
}

bool GameWindowCovered(const Window& game)
{
    @autoreleasepool
    {
        NSDictionary* info = WindowInfo(CGWindowID(game.id));
        Rect bounds;
        if (!info || !Bounds(info, bounds))
            return false;
        const CGRect gameRect = CGRectMake(bounds.x, bounds.y, bounds.width, bounds.height);
        const int layer = [info[(id)kCGWindowLayer] intValue];
        // Windows at or above the overlay's level show over it anyway, such as the game's menus.
        const long overlayLevel = std::max<long>(NSStatusWindowLevel, long(layer) + 1);
        // Front to back, every window in front of the game's.
        for (NSDictionary* above in WindowList(kCGWindowListOptionOnScreenAboveWindow, CGWindowID(game.id)))
        {
            Rect other;
            if ([above[(id)kCGWindowOwnerPID] intValue] != game.pid || [above[(id)kCGWindowLayer] intValue] >= overlayLevel ||
                [above[(id)kCGWindowAlpha] doubleValue] <= 0 || !Bounds(above, other))
                continue;
            // Tiny windows, and windows the same size and place as the game's, are more likely parts of the game than
            // dialogs. So are windows below the normal layer. A game's see-through windows, as Wine and browsers make
            // for overlays, report full alpha anyway, so a window also needs a title, as dialogs and file pickers
            // have, or a quarter of the game's area. When unsure, the overlay stays.
            const CGRect otherRect = CGRectMake(other.x, other.y, other.width, other.height);
            const CGRect overlap = CGRectIntersection(gameRect, otherRect);
            NSString* title = above[(id)kCGWindowName];
            if (other.width < 64 || other.height < 64 || other == bounds || CGRectIsNull(overlap) || overlap.size.width < 32 ||
                overlap.size.height < 32 || [above[(id)kCGWindowLayer] intValue] < window_rules::kNormalLevel ||
                (!title.length && double(other.width) * other.height < double(bounds.width) * bounds.height / 4))
                continue;
            // Said once for each window, since the host asks a few times a second.
            static unsigned logged = 0;
            const unsigned number = [above[(id)kCGWindowNumber] unsignedIntValue];
            if (number != logged)
            {
                Log(LogLevel::Info, "Another window of the game is above it: window=%u, title=%s, layer=%d, bounds=%d,%d %dx%d.", number,
                    title.length ? title.UTF8String : "", [above[(id)kCGWindowLayer] intValue], other.x, other.y, other.width, other.height);
                logged = number;
            }
            return true;
        }
        return false;
    }
}

bool StartCapture(const Window& window, std::string& error)
{
    @autoreleasepool
    {
        StopCapture();
        // A window on another Space can be captured too. The overlay stays hidden until it shows.
        NSDictionary* info = WindowInfo(CGWindowID(window.id));
        Rect bounds;
        if (!info || [info[(id)kCGWindowOwnerPID] intValue] != window.pid || !Bounds(info, bounds))
        {
            error = "The window has closed.";
            return false;
        }
        // macOS 13 cannot ask the filter for its scale, so it is that of the screen with most of the window. Asked here,
        // since NSScreen belongs to the main thread.
        const double screenScale = ScreenScale(bounds);
        capturedWindow = CGWindowID(window.id);
        capture.titleInset = TitleInset(info);
        if (!activity)
            activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                                                                    reason:@"Copying the game's window"];
        capture.onGpu = gpu.metalObjects;
        uint64_t generation = 0;
        {
            std::lock_guard lock(capture.mutex);
            generation = ++capture.generation;
            capture.error.clear();
            capture.stoppedByUser = false;
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
                                           FailWithError(generation, contentError, "Could not list windows to capture");
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
                                       double scale = screenScale;
                                       if (@available(macOS 14.0, *))
                                           scale = filter.pointPixelScale;
                                       SCStreamConfiguration* config = StreamConfiguration(size_t(std::lround(target.frame.size.width * scale)),
                                                                                           size_t(std::lround(target.frame.size.height * scale)));
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
                                           FailWithError(generation, addError, "Could not capture the game");
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
                                             FailWithError(generation, startError, "Could not capture the game");
                                       }];
                                     }];
        return true;
    }
}

void StopCapture()
{
    @autoreleasepool
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
        capturedWindow = kCGNullWindowID;
        if (activity)
        {
            [NSProcessInfo.processInfo endActivity:activity];
            activity = nil;
        }
        // The host waited for the graphics card before stopping.
        ReleaseImported();
    }
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
    // Called on every pass of the main loop while capturing, so the title bar follows the window into and out of full
    // screen even while the overlay is hidden and WindowBounds is not asked.
    if (capturedWindow != kCGNullWindowID)
        @autoreleasepool
        {
            if (NSDictionary* info = WindowInfo(capturedWindow))
                capture.titleInset = TitleInset(info);
        }
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

bool CaptureStoppedByUser()
{
    std::lock_guard lock(capture.mutex);
    return capture.stoppedByUser;
}

bool HasCapturePermission()
{
    // CGPreflightScreenCaptureAccess answers with what macOS told Unishade when it started, so a permission given
    // later counts only after a restart, and one taken away shows only when ScreenCaptureKit refuses. The launcher
    // asks every frame, so the answer is kept for a second, and for good once it is yes.
    static bool granted = false;
    static std::chrono::steady_clock::time_point checked;
    if (capture.declined)
        return false;
    const auto now = std::chrono::steady_clock::now();
    if (!granted && (checked == std::chrono::steady_clock::time_point() || now - checked > std::chrono::seconds(1)))
    {
        granted = CGPreflightScreenCaptureAccess();
        checked = now;
    }
    return granted;
}

void RequestCapturePermission()
{
    @autoreleasepool
    {
        // macOS shows its request only the first time it is asked, and its request has a button to System Settings.
        // After that CGRequestScreenCaptureAccess shows nothing, so System Settings opens instead. macOS does not say
        // which time this is, so Unishade remembers that it asked.
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        NSString* key = @"ScreenCaptureRequested";
        const bool askedBefore = [defaults boolForKey:key] || capture.declined;
        [defaults setBool:YES forKey:key];
        if (!CGRequestScreenCaptureAccess() && askedBefore)
            Open("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture");
    }
}

bool Relaunch()
{
    @autoreleasepool
    {
        NSBundle* bundle = NSBundle.mainBundle;
        Log(LogLevel::Info, "Starting Unishade again: %s.", bundle.bundlePath.fileSystemRepresentation);
        // Through Launch Services, so the new Unishade is the app itself rather than a child of this one, which macOS
        // checks the permission against. --relaunch makes it wait for this one to let go of the lock that keeps a
        // second Unishade from starting.
        if ([bundle.bundlePath.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame)
            return Spawn("open", { "-n", bundle.bundlePath.fileSystemRepresentation, "--args", "--relaunch" });
        NSString* executable = bundle.executablePath;
        return executable && Spawn(executable.fileSystemRepresentation, { "--relaunch" });
    }
}

namespace
{
// Shows an alert in front of everything. Before glfwInit there is no NSApp yet, so this makes one.
void Alert(NSString* title, NSString* text)
{
    [NSApplication sharedApplication];
    if (NSApp.activationPolicy != NSApplicationActivationPolicyRegular)
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = text;
    [alert runModal];
}
} // namespace

void ShowStartupError(const std::string& message)
{
    @autoreleasepool
    {
        NSString* text = [NSString stringWithUTF8String:message.c_str()];
        Alert(@"Unishade cannot start", text ?: @"");
    }
}

void ShowAlreadyRunning()
{
    @autoreleasepool
    {
        // The other Unishade is the app with the same identifier, such as another copy of the app.
        NSRunningApplication* other = nil;
        if (NSString* identifier = NSBundle.mainBundle.bundleIdentifier)
            for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:identifier])
                if (app.processIdentifier != getpid())
                    other = app;
        Alert(@"Unishade is already running", other ? @"It comes to the front when you close this." : @"Only one Unishade runs at a time.");
        if (!other)
            return;
        // Only the active program can hand activation on, which this one is while its alert shows.
        if (@available(macOS 14.0, *))
        {
            [NSApp yieldActivationToApplication:other];
            [other activateWithOptions:NSApplicationActivateAllWindows];
        }
        else
        {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [other activateWithOptions:NSApplicationActivateAllWindows | NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
        }
    }
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
    Spawn("open", { target });
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
