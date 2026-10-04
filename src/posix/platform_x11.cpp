// Linux: X11, which also covers games that run through XWayland, such as Wine and Proton games under a Wayland
// desktop. Windows come from the window manager's _NET_CLIENT_LIST, pictures from XComposite, which keeps a
// window's picture even where other windows cover it, and shortcuts from passive key grabs on the root window.
// XDamage says when the game drew, so frames are only copied when there is something new.

#include "platform.h"
#include "gpu.h"
#include "log.h"

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/keysym.h>
#ifdef UNISHADE_HAVE_XRES
#include <X11/extensions/XRes.h>
#endif
#ifdef UNISHADE_HAVE_DRI3
#include <X11/Xlib-xcb.h>
#include <xcb/dri3.h>
#endif
// Only the header is needed, from libxdamage-dev, since the library is loaded when present.
#if __has_include(<X11/extensions/Xdamage.h>)
#include <X11/extensions/Xdamage.h>
#define UNISHADE_HAVE_XDAMAGE
#endif
#include <dlfcn.h>
#include <stb_image_write.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <thread>

extern char** environ;

namespace platform
{
namespace
{
Display* display = nullptr; // the main thread's own connection, apart from GLFW's
::Window root = 0;
bool haveXRes = false;

#ifdef UNISHADE_HAVE_XRES
// Loaded when present rather than linked, so the host also starts on systems without libXRes.
struct XResFunctions
{
    decltype(&XResQueryExtension) queryExtension = nullptr;
    decltype(&XResQueryVersion) queryVersion = nullptr;
    decltype(&XResQueryClientIds) queryClientIds = nullptr;
    decltype(&XResGetClientIdType) getClientIdType = nullptr;
    decltype(&XResGetClientPid) getClientPid = nullptr;
    decltype(&XResClientIdsDestroy) clientIdsDestroy = nullptr;

    bool Load()
    {
        void* library = dlopen("libXRes.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            return false;
        queryExtension = reinterpret_cast<decltype(queryExtension)>(dlsym(library, "XResQueryExtension"));
        queryVersion = reinterpret_cast<decltype(queryVersion)>(dlsym(library, "XResQueryVersion"));
        queryClientIds = reinterpret_cast<decltype(queryClientIds)>(dlsym(library, "XResQueryClientIds"));
        getClientIdType = reinterpret_cast<decltype(getClientIdType)>(dlsym(library, "XResGetClientIdType"));
        getClientPid = reinterpret_cast<decltype(getClientPid)>(dlsym(library, "XResGetClientPid"));
        clientIdsDestroy = reinterpret_cast<decltype(clientIdsDestroy)>(dlsym(library, "XResClientIdsDestroy"));
        return queryExtension && queryVersion && queryClientIds && getClientIdType && getClientPid && clientIdsDestroy;
    }
};
XResFunctions xres;
#endif

// Xlib's default error handler ends the process. Errors are expected here, since windows close at any time, so
// they are recorded and checked instead.
thread_local int lastError = 0;
int OnXError(Display*, XErrorEvent* event)
{
    lastError = event->error_code;
    return 0;
}

struct ErrorTrap
{
    Display* connection;
    explicit ErrorTrap(Display* d) : connection(d)
    {
        XSync(connection, False);
        lastError = 0;
    }
    bool Failed()
    {
        XSync(connection, False);
        return lastError != 0;
    }
};

Atom GetAtom(Display* d, const char* name)
{
    return XInternAtom(d, name, False);
}

// Reads a property of 32-bit items, such as window IDs or cardinals.
std::vector<unsigned long> Property32(Display* d, ::Window window, Atom property, Atom type)
{
    Atom actualType;
    int format;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    std::vector<unsigned long> result;
    if (XGetWindowProperty(d, window, property, 0, 4096, False, type, &actualType, &format, &count, &remaining, &data) == Success && data)
    {
        if (format == 32)
            result.assign(reinterpret_cast<unsigned long*>(data), reinterpret_cast<unsigned long*>(data) + count);
        XFree(data);
    }
    return result;
}

std::string Title(::Window window)
{
    Atom actualType;
    int format;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    std::string title;
    if (XGetWindowProperty(display, window, GetAtom(display, "_NET_WM_NAME"), 0, 1024, False, GetAtom(display, "UTF8_STRING"), &actualType,
                           &format, &count, &remaining, &data) == Success && data)
    {
        title.assign(reinterpret_cast<char*>(data), count);
        XFree(data);
    }
    if (title.empty())
    {
        char* name = nullptr;
        if (XFetchName(display, window, &name) && name)
        {
            title = name;
            XFree(name);
        }
    }
    return title;
}

// The process that owns a window. The X server knows it from the connection, which also works for sandboxed
// programs like Flatpak apps, whose own idea of their process ID differs from everyone else's.
int WindowPid(::Window window)
{
#ifdef UNISHADE_HAVE_XRES
    if (haveXRes)
    {
        XResClientIdSpec spec{ window, XRES_CLIENT_ID_PID_MASK };
        long count = 0;
        XResClientIdValue* values = nullptr;
        int pid = 0;
        if (xres.queryClientIds(display, 1, &spec, &count, &values) == Success)
        {
            for (long i = 0; i < count && !pid; ++i)
                if (xres.getClientIdType(&values[i]) == XRES_CLIENT_ID_PID)
                    pid = xres.getClientPid(&values[i]);
            xres.clientIdsDestroy(count, values);
        }
        if (pid > 0)
            return pid;
    }
#endif
    const auto pid = Property32(display, window, GetAtom(display, "_NET_WM_PID"), XA_CARDINAL);
    return pid.empty() ? 0 : static_cast<int>(pid[0]);
}

bool Hidden(::Window window)
{
    const Atom hidden = GetAtom(display, "_NET_WM_STATE_HIDDEN");
    for (unsigned long state : Property32(display, window, GetAtom(display, "_NET_WM_STATE"), XA_ATOM))
        if (state == hidden)
            return true;
    return false;
}

std::string ReadLink(const std::string& path)
{
    char buffer[4096];
    const ssize_t size = readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    return size > 0 ? std::string(buffer, size) : std::string();
}

// Capture

// The window's picture as the X server keeps it on the graphics card, from DRI3. Closes its file descriptors when
// the last frame using it is gone.
struct DmaBuffer
{
    std::vector<int> fds;
    std::vector<uint32_t> strides;
    std::vector<uint32_t> offsets;
    uint64_t modifier = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    ~DmaBuffer()
    {
        for (int fd : fds)
            close(fd);
    }
};

#ifdef UNISHADE_HAVE_DRI3
// DRI3 through XCB, loaded when present rather than linked, so the host starts where it is missing.
struct Dri3Functions
{
    decltype(&XGetXCBConnection) getConnection = nullptr;
    decltype(&xcb_get_extension_data) extensionData = nullptr;
    xcb_extension_t* extension = nullptr;
    decltype(&xcb_dri3_query_version) queryVersion = nullptr;
    decltype(&xcb_dri3_query_version_reply) queryVersionReply = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap) buffersFromPixmap = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_reply) buffersFromPixmapReply = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_strides) strides = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_offsets) offsets = nullptr;
    decltype(&xcb_dri3_buffers_from_pixmap_buffers) buffers = nullptr;
    decltype(&xcb_dri3_open) open = nullptr;
    decltype(&xcb_dri3_open_reply) openReply = nullptr;
    decltype(&xcb_dri3_open_reply_fds) openReplyFds = nullptr;

    bool Load()
    {
        void* xlibXcb = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void* xcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        void* dri3 = dlopen("libxcb-dri3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!xlibXcb || !xcb || !dri3)
            return false;
        getConnection = reinterpret_cast<decltype(getConnection)>(dlsym(xlibXcb, "XGetXCBConnection"));
        extensionData = reinterpret_cast<decltype(extensionData)>(dlsym(xcb, "xcb_get_extension_data"));
        extension = static_cast<xcb_extension_t*>(dlsym(dri3, "xcb_dri3_id"));
        queryVersion = reinterpret_cast<decltype(queryVersion)>(dlsym(dri3, "xcb_dri3_query_version"));
        queryVersionReply = reinterpret_cast<decltype(queryVersionReply)>(dlsym(dri3, "xcb_dri3_query_version_reply"));
        buffersFromPixmap = reinterpret_cast<decltype(buffersFromPixmap)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap"));
        buffersFromPixmapReply = reinterpret_cast<decltype(buffersFromPixmapReply)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_reply"));
        strides = reinterpret_cast<decltype(strides)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_strides"));
        offsets = reinterpret_cast<decltype(offsets)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_offsets"));
        buffers = reinterpret_cast<decltype(buffers)>(dlsym(dri3, "xcb_dri3_buffers_from_pixmap_buffers"));
        open = reinterpret_cast<decltype(open)>(dlsym(dri3, "xcb_dri3_open"));
        openReply = reinterpret_cast<decltype(openReply)>(dlsym(dri3, "xcb_dri3_open_reply"));
        openReplyFds = reinterpret_cast<decltype(openReplyFds)>(dlsym(dri3, "xcb_dri3_open_reply_fds"));
        return getConnection && extensionData && extension && queryVersion && queryVersionReply && buffersFromPixmap && buffersFromPixmapReply &&
               strides && offsets && buffers && open && openReply && openReplyFds;
    }
};

// Null when DRI3 is missing, here or in the X server.
const Dri3Functions* Dri3(Display* d)
{
    static Dri3Functions dri3;
    static const bool loaded = dri3.Load();
    if (!loaded)
        return nullptr;
    const xcb_query_extension_reply_t* present = dri3.extensionData(dri3.getConnection(d), dri3.extension);
    return present && present->present ? &dri3 : nullptr;
}

// The pixmap's buffers, with DRI3 1.2's modifiers. Empty where the X server cannot share them, such as without
// a GPU or with drivers that do not support DRI3.
std::shared_ptr<DmaBuffer> BuffersFromPixmap(Display* d, Pixmap pixmap)
{
    const Dri3Functions* functions = Dri3(d);
    if (!functions)
        return nullptr;
    const Dri3Functions& dri3 = *functions;
    xcb_connection_t* connection = dri3.getConnection(d);
    xcb_dri3_query_version_reply_t* version = dri3.queryVersionReply(connection, dri3.queryVersion(connection, 1, 2), nullptr);
    const bool modifiers = version && (version->major_version > 1 || version->minor_version >= 2);
    free(version);
    if (!modifiers)
        return nullptr;
    xcb_dri3_buffers_from_pixmap_reply_t* reply = dri3.buffersFromPixmapReply(connection, dri3.buffersFromPixmap(connection, pixmap), nullptr);
    if (!reply)
        return nullptr;
    auto buffer = std::make_shared<DmaBuffer>();
    const int32_t* fds = dri3.buffers(reply);
    for (int i = 0; i < reply->nfd; ++i)
    {
        buffer->fds.push_back(fds[i]);
        buffer->strides.push_back(dri3.strides(reply)[i]);
        buffer->offsets.push_back(dri3.offsets(reply)[i]);
    }
    buffer->modifier = reply->modifier;
    buffer->width = reply->width;
    buffer->height = reply->height;
    const bool usable = reply->nfd > 0 && reply->bpp == 32 && (reply->depth == 24 || reply->depth == 32);
    free(reply);
    return usable ? buffer : nullptr;
}
#else
std::shared_ptr<DmaBuffer> BuffersFromPixmap(Display*, Pixmap)
{
    return nullptr;
}
#endif

// Frees the dma-buf imported into Vulkan. Defined with TakeFrame.
void ReleaseImported();

#ifdef UNISHADE_HAVE_XDAMAGE
// XDamage, loaded when present rather than linked, like XRes.
struct DamageFunctions
{
    decltype(&XDamageQueryExtension) queryExtension = nullptr;
    decltype(&XDamageCreate) create = nullptr;
    decltype(&XDamageSubtract) subtract = nullptr;

    bool Load()
    {
        void* library = dlopen("libXdamage.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            return false;
        queryExtension = reinterpret_cast<decltype(queryExtension)>(dlsym(library, "XDamageQueryExtension"));
        create = reinterpret_cast<decltype(create)>(dlsym(library, "XDamageCreate"));
        subtract = reinterpret_cast<decltype(subtract)>(dlsym(library, "XDamageSubtract"));
        return queryExtension && create && subtract;
    }
};
#endif

// Tells when the game drew into its window, through XDamage on the capture thread's connection, which frees it when
// it closes. Inactive without XDamage.
class DamageWatch
{
public:
    DamageWatch([[maybe_unused]] Display* d, [[maybe_unused]] ::Window window)
    {
#ifdef UNISHADE_HAVE_XDAMAGE
        static DamageFunctions functions;
        static const bool loaded = functions.Load();
        int event = 0, error = 0;
        if (!loaded || !functions.queryExtension(d, &event, &error))
            return;
        ErrorTrap trap(d);
        damage = functions.create(d, window, XDamageReportNonEmpty);
        if (trap.Failed())
            return;
        connection = d;
        subtract = functions.subtract;
        notify = event + XDamageNotify;
#endif
    }

    bool Active() const { return notify >= 0; }

    // Starts collecting again, so what the game draws from here on brings a new event.
    void Clear()
    {
#ifdef UNISHADE_HAVE_XDAMAGE
        if (subtract)
            subtract(connection, damage, None, None);
#endif
    }

private:
    int notify = -1;
#ifdef UNISHADE_HAVE_XDAMAGE
    Display* connection = nullptr;
    Damage damage = 0;
    decltype(&XDamageSubtract) subtract = nullptr;
#endif
};

// While the overlay is hidden.
constexpr std::chrono::milliseconds kIdleInterval(200);

struct Capture
{
    std::thread thread;
    std::atomic<bool> stop = false;
    std::atomic<bool> running = false;
    std::atomic<bool> onGpu = false; // frames go to the main thread as dma-bufs instead of pixels
    std::atomic<bool> reset = false; // the capture thread names the pixmap again, to change how it copies
    std::atomic<bool> idle = false;
    std::atomic<int> refresh = 60; // of the monitor the game is on
    int wake[2] = { -1, -1 };      // a pipe StopCapture writes to, so the capture thread stops waiting
    std::mutex mutex;
    Frame ready;
    std::string error;
};
Capture capture;

// Waits until the time or until capture stops. With a connection, also until it has events, and returns whether
// it has.
bool WaitUntil(Display* d, std::chrono::steady_clock::time_point until)
{
    while (!capture.stop)
    {
        if (d && XPending(d))
            return true;
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            break;
        pollfd fds[] = { { capture.wake[0], POLLIN, 0 }, { d ? ConnectionNumber(d) : -1, POLLIN, 0 } };
        poll(fds, 2, int(std::min<long long>(left, 1000)));
    }
    return false;
}

// Gives the image shared memory for the X server to copy into. False where the X server cannot reach it, such as
// over the network or from another container.
bool AttachShm(Display* d, XImage* image, XShmSegmentInfo& segment)
{
    segment = {};
    segment.shmid = shmget(IPC_PRIVATE, size_t(image->bytes_per_line) * image->height, IPC_CREAT | 0600);
    if (segment.shmid < 0)
        return false;
    void* address = shmat(segment.shmid, nullptr, 0);
    bool attached = false;
    if (address != reinterpret_cast<void*>(-1))
    {
        segment.shmaddr = image->data = static_cast<char*>(address);
        segment.readOnly = False;
        ErrorTrap trap(d);
        attached = XShmAttach(d, &segment) && !trap.Failed();
    }
    // Removed once the X server has attached it, so it goes away with the process even if it crashes.
    shmctl(segment.shmid, IPC_RMID, nullptr);
    if (!attached)
    {
        if (address != reinterpret_cast<void*>(-1))
            shmdt(address);
        image->data = nullptr;
        segment = {};
    }
    return attached;
}

void CaptureThread(::Window target)
{
    Display* d = XOpenDisplay(nullptr);
    if (!d)
    {
        std::lock_guard lock(capture.mutex);
        capture.error = "Could not connect to the X server to copy the game's picture.";
        capture.running = false;
        return;
    }
    bool shm = XShmQueryExtension(d);
    {
        ErrorTrap trap(d);
        XCompositeRedirectWindow(d, target, CompositeRedirectAutomatic);
        // So the waits below also end when the window is resized, hidden or closed.
        XSelectInput(d, target, StructureNotifyMask);
        trap.Failed();
    }
    // Without XDamage, frames are copied as often as the display refreshes.
    DamageWatch damage(d, target);

    Pixmap pixmap = 0;
    std::shared_ptr<DmaBuffer> buffer;
    XImage* image = nullptr;
    XShmSegmentInfo segment{};
    int width = 0, height = 0;
    Frame back;
    uint64_t serial = 0;
    std::string failure;
    bool changed = true; // drawn into since the last copy
    int failures = 0;    // copies in a row that failed

    const auto release = [&] {
        if (image)
        {
            if (segment.shmaddr)
            {
                XShmDetach(d, &segment);
                shmdt(segment.shmaddr);
                segment = {};
            }
            XDestroyImage(image);
            image = nullptr;
        }
        if (pixmap)
            XFreePixmap(d, pixmap);
        pixmap = 0;
        buffer.reset();
    };
    const auto publish = [&] {
        {
            std::lock_guard lock(capture.mutex);
            back.serial = ++serial;
            std::swap(back, capture.ready);
        }
        back.hold.reset();
        glfwPostEmptyEvent();
    };

    auto last = std::chrono::steady_clock::now() - std::chrono::hours(1); // when the last copy started
    while (!capture.stop)
    {
        // At most once per refresh of the game's monitor, and a few times a second while nobody sees the frames.
        const std::chrono::nanoseconds interval = capture.idle ? std::chrono::nanoseconds(kIdleInterval)
                                                               : std::chrono::nanoseconds(1'000'000'000 / capture.refresh);
        if (damage.Active())
        {
            // A little sooner than once per refresh, so a game drawing at the display's rate never waits a frame.
            WaitUntil(nullptr, last + interval * 3 / 4);
            // Then until the game draws. The window is looked at now and then anyway, since hiding its window
            // manager frame, as on another desktop, sends it no event.
            if (!changed)
                WaitUntil(d, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
        }
        else
        {
            WaitUntil(nullptr, last + interval);
            changed = true;
        }
        // Every event here is about the window: its damage, or a change of its size or state.
        while (XPending(d))
        {
            XEvent event;
            XNextEvent(d, &event);
            changed = true;
        }
        if (capture.stop)
            break;

        XWindowAttributes attributes{};
        {
            ErrorTrap trap(d);
            const Status ok = XGetWindowAttributes(d, target, &attributes);
            if (trap.Failed() || !ok)
            {
                failure = "The game's window closed.";
                break;
            }
        }
        if (attributes.map_state != IsViewable || attributes.width <= 0 || attributes.height <= 0)
        {
            release();
            changed = true;
            WaitUntil(d, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
            continue;
        }
        if (attributes.width != width || attributes.height != height || !pixmap || capture.reset.exchange(false))
        {
            release();
            changed = true;
            width = attributes.width;
            height = attributes.height;
            ErrorTrap trap(d);
            pixmap = XCompositeNameWindowPixmap(d, target);
            if (trap.Failed())
            {
                pixmap = 0;
                WaitUntil(nullptr, std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
                continue;
            }
            if (capture.onGpu)
            {
                buffer = BuffersFromPixmap(d, pixmap);
                if (!buffer || buffer->width != unsigned(width) || buffer->height != unsigned(height))
                {
                    buffer.reset();
                    capture.onGpu = false;
                    Log(LogLevel::Info, "The X server cannot share the game's picture on the graphics card. Copying it instead.");
                }
            }
            if (shm && !buffer)
            {
                image = XShmCreateImage(d, attributes.visual, attributes.depth, ZPixmap, nullptr, &segment, width, height);
                if (image && !AttachShm(d, image, segment))
                {
                    XDestroyImage(image);
                    image = nullptr;
                }
                if (!image)
                {
                    shm = false;
                    Log(LogLevel::Info, "The X server cannot share memory with Unishade, so copying the game's picture takes longer.");
                }
            }
            if (image && (image->bits_per_pixel != 32 || image->red_mask != 0xFF0000 || image->blue_mask != 0xFF))
            {
                failure = "The game's window uses a pixel format Unishade cannot read.";
                break;
            }
        }
        if (!changed)
            continue;
        // Before the copy, so what the game draws meanwhile brings a new event.
        damage.Clear();
        changed = false;
        last = std::chrono::steady_clock::now();

        if (buffer)
        {
            // The X server keeps drawing into the same buffer, so each frame only says there is something new in
            // it. Nothing makes the graphics card wait until that drawing is done before reading it: XDamage only
            // says the drawing was sent, and Vulkan does not wait for the dma-buf's implicit fences. So a frame can
            // now and then show the game's picture half drawn.
            XFlush(d);
            back.pixels.clear();
            back.width = width;
            back.height = height;
            back.hold = buffer;
            publish();
            continue;
        }

        XImage* got = nullptr;
        {
            ErrorTrap trap(d);
            if (image)
                got = XShmGetImage(d, pixmap, image, 0, 0, AllPlanes) ? image : nullptr;
            else
                got = XGetImage(d, pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
            if (trap.Failed())
            {
                if (got && got != image)
                    XDestroyImage(got);
                got = nullptr;
            }
        }
        if (!got)
        {
            // Usually the window changed between the size check and the copy, which naming its pixmap again
            // fixes. Shared memory that keeps failing, as with some remote X servers, is given up.
            release();
            changed = true;
            if (++failures >= 3 && shm)
            {
                shm = false;
                Log(LogLevel::Info, "Copying the game's picture through shared memory keeps failing, so Unishade copies it without.");
            }
            WaitUntil(nullptr, std::chrono::steady_clock::now() + std::min(std::chrono::milliseconds(20) * failures, std::chrono::milliseconds(1000)));
            continue;
        }
        failures = 0;
        if (got->bits_per_pixel != 32)
        {
            failure = "The game's window uses a pixel format Unishade cannot read.";
            if (got != image)
                XDestroyImage(got);
            break;
        }

        back.width = width;
        back.height = height;
        back.hold.reset();
        back.pixels.resize(size_t(width) * height);
        for (int y = 0; y < height; ++y)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(got->data + size_t(y) * got->bytes_per_line);
            uint32_t* out = back.pixels.data() + size_t(y) * width;
            // Windows without an alpha channel leave it undefined.
            for (int x = 0; x < width; ++x)
                out[x] = row[x] | 0xFF000000u;
        }
        if (got != image)
            XDestroyImage(got);
        publish();
    }

    release();
    {
        ErrorTrap trap(d);
        XCompositeUnredirectWindow(d, target, CompositeRedirectAutomatic);
        trap.Failed();
    }
    XCloseDisplay(d);
    if (!failure.empty())
    {
        std::lock_guard lock(capture.mutex);
        capture.error = failure;
    }
    capture.running = false;
    glfwPostEmptyEvent();
}

// Hotkeys

struct Grab
{
    KeyCode code;
    unsigned modifiers;
};
std::map<int, Grab> grabs;
std::set<int> held;
HotkeyCallback hotkeyCallback;
std::thread waker;
std::atomic<bool> wakerStop = false;

KeySym Keysym(ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return XK_a + (key - ImGuiKey_A);
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return XK_0 + (key - ImGuiKey_0);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return XK_F1 + (key - ImGuiKey_F1);
    // The number pad's digits are one key with Num Lock on or off, so the shortcut works either way.
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return XK_KP_0 + (key - ImGuiKey_Keypad0);
    switch (key)
    {
    case ImGuiKey_Home: return XK_Home;
    case ImGuiKey_End: return XK_End;
    case ImGuiKey_Insert: return XK_Insert;
    case ImGuiKey_Delete: return XK_Delete;
    case ImGuiKey_PageUp: return XK_Prior;
    case ImGuiKey_PageDown: return XK_Next;
    case ImGuiKey_Pause: return XK_Pause;
    case ImGuiKey_ScrollLock: return XK_Scroll_Lock;
    case ImGuiKey_Space: return XK_space;
    case ImGuiKey_Tab: return XK_Tab;
    case ImGuiKey_Escape: return XK_Escape;
    case ImGuiKey_LeftArrow: return XK_Left;
    case ImGuiKey_RightArrow: return XK_Right;
    case ImGuiKey_UpArrow: return XK_Up;
    case ImGuiKey_DownArrow: return XK_Down;
    case ImGuiKey_KeypadMultiply: return XK_KP_Multiply;
    case ImGuiKey_KeypadAdd: return XK_KP_Add;
    case ImGuiKey_KeypadSubtract: return XK_KP_Subtract;
    case ImGuiKey_KeypadDecimal: return XK_KP_Decimal;
    case ImGuiKey_KeypadDivide: return XK_KP_Divide;
    default: return NoSymbol;
    }
}

unsigned XModifiers(unsigned modifiers)
{
    return ((modifiers & kCtrl) ? ControlMask : 0) | ((modifiers & kAlt) ? Mod1Mask : 0) | ((modifiers & kShift) ? ShiftMask : 0) |
           ((modifiers & kSuper) ? Mod4Mask : 0);
}

// Num Lock and Caps Lock are modifiers to X11, so each shortcut is grabbed with and without them.
constexpr unsigned kLockVariants[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
constexpr unsigned kRelevantModifiers = ControlMask | Mod1Mask | ShiftMask | Mod4Mask;

// The Windows virtual-key code of a keysym, or 0.
int VirtualKey(KeySym sym)
{
    if (sym >= XK_a && sym <= XK_z)
        return 'A' + int(sym - XK_a);
    if (sym >= XK_A && sym <= XK_Z)
        return 'A' + int(sym - XK_A);
    if (sym >= XK_0 && sym <= XK_9)
        return '0' + int(sym - XK_0);
    if (sym >= XK_KP_0 && sym <= XK_KP_9)
        return 0x60 + int(sym - XK_KP_0);
    if (sym >= XK_F1 && sym <= XK_F24)
        return 0x70 + int(sym - XK_F1);
    switch (sym)
    {
    case XK_BackSpace: return 0x08;
    case XK_Tab: case XK_ISO_Left_Tab: return 0x09;
    case XK_Return: case XK_KP_Enter: return 0x0D;
    case XK_Pause: return 0x13;
    case XK_Caps_Lock: return 0x14;
    case XK_Escape: return 0x1B;
    case XK_space: return 0x20;
    case XK_Prior: return 0x21;
    case XK_Next: return 0x22;
    case XK_End: return 0x23;
    case XK_Home: return 0x24;
    case XK_Left: return 0x25;
    case XK_Up: return 0x26;
    case XK_Right: return 0x27;
    case XK_Down: return 0x28;
    case XK_Print: return 0x2C;
    case XK_Insert: return 0x2D;
    case XK_Delete: return 0x2E;
    case XK_Super_L: return 0x5B;
    case XK_Super_R: return 0x5C;
    case XK_Menu: return 0x5D;
    case XK_KP_Multiply: return 0x6A;
    case XK_KP_Add: return 0x6B;
    case XK_KP_Separator: return 0x6C;
    case XK_KP_Subtract: return 0x6D;
    case XK_KP_Decimal: return 0x6E;
    case XK_KP_Divide: return 0x6F;
    case XK_Num_Lock: return 0x90;
    case XK_Scroll_Lock: return 0x91;
    case XK_Shift_L: return 0xA0;
    case XK_Shift_R: return 0xA1;
    case XK_Control_L: return 0xA2;
    case XK_Control_R: return 0xA3;
    case XK_Alt_L: return 0xA4;
    case XK_Alt_R: case XK_ISO_Level3_Shift: return 0xA5;
    case XK_semicolon: return 0xBA;
    case XK_equal: return 0xBB;
    case XK_comma: return 0xBC;
    case XK_minus: return 0xBD;
    case XK_period: return 0xBE;
    case XK_slash: return 0xBF;
    case XK_grave: return 0xC0;
    case XK_bracketleft: return 0xDB;
    case XK_backslash: return 0xDC;
    case XK_bracketright: return 0xDD;
    case XK_apostrophe: return 0xDE;
    case XK_less: return 0xE2;
    default: return 0;
    }
}

// Virtual-key codes by X11 keycode, for ReadInput. Made again when the keyboard mapping changes.
std::array<uint8_t, 256> virtualKeys{};
bool virtualKeysKnown = false;

void MapVirtualKeys()
{
    virtualKeys = {};
    int first = 0, last = 0;
    XDisplayKeycodes(display, &first, &last);
    for (int code = std::max(first, 0); code <= std::min(last, 255); ++code)
    {
        // A key's code comes from its first level, such as a for the A key, or from its second where only that has
        // one, such as the digits of a French keyboard. Number pad keys count as such with Num Lock off too.
        const int base = VirtualKey(XkbKeycodeToKeysym(display, KeyCode(code), 0, 0));
        const int shifted = VirtualKey(XkbKeycodeToKeysym(display, KeyCode(code), 0, 1));
        virtualKeys[code] = uint8_t(shifted >= 0x60 && shifted <= 0x6F ? shifted : base ? base : shifted);
    }
    virtualKeysKnown = true;
}

// The game's window, which the main loop asks about on every pass. The answers are kept until the X server sends an
// event about the window or the window manager's frame around it, and asked for again every second in case a change
// sends none.
struct Tracked
{
    ::Window window = 0;
    ::Window frame = 0; // the root window's child that holds it
    bool destroyed = false;
    bool known = false;
    bool visible = false;
    Rect bounds;
    std::chrono::steady_clock::time_point checked;
};
Tracked tracked;

// The window in front, kept until the window manager changes _NET_ACTIVE_WINDOW. Without a window manager that sets
// it, the focus is asked for again after a moment.
struct Foreground
{
    bool known = false;
    bool fromProperty = false;
    WindowId window = 0;
    int pid = -1; // its process, -1 until asked for
    std::chrono::steady_clock::time_point checked;
};
Foreground foreground;

// The root window's child that holds the window: the window manager's frame, or the window itself without one.
::Window TopLevel(::Window window)
{
    ErrorTrap trap(display);
    ::Window current = window;
    for (;;)
    {
        ::Window rootReturn = 0, parent = 0, *children = nullptr;
        unsigned int count = 0;
        if (!XQueryTree(display, current, &rootReturn, &parent, &children, &count))
            return 0;
        if (children)
            XFree(children);
        if (!parent || parent == root)
            break;
        current = parent;
    }
    return trap.Failed() ? 0 : current;
}

void Untrack()
{
    if (!tracked.window)
        return;
    ErrorTrap trap(display);
    if (!tracked.destroyed)
        XSelectInput(display, tracked.window, NoEventMask);
    if (tracked.frame && tracked.frame != tracked.window)
        XSelectInput(display, tracked.frame, NoEventMask);
    trap.Failed();
    tracked = {};
}

void Track(::Window window)
{
    Untrack();
    tracked.window = window;
    tracked.frame = TopLevel(window);
    {
        ErrorTrap trap(display);
        XSelectInput(display, window, StructureNotifyMask);
        tracked.destroyed = trap.Failed();
    }
    if (tracked.frame && tracked.frame != window)
    {
        ErrorTrap trap(display);
        XSelectInput(display, tracked.frame, StructureNotifyMask);
        if (trap.Failed())
            tracked.frame = 0;
    }
}

// The window manager put the window into another frame, or took it out of one.
void Reframe()
{
    const ::Window old = tracked.frame;
    tracked.frame = TopLevel(tracked.window);
    if (tracked.frame == old)
        return;
    ErrorTrap trap(display);
    if (old && old != tracked.window)
        XSelectInput(display, old, NoEventMask);
    if (tracked.frame && tracked.frame != tracked.window)
        XSelectInput(display, tracked.frame, StructureNotifyMask);
    trap.Failed();
}

// Marks what an event from the X server changed: the tracked window, the window in front or the keyboard mapping.
void HandleEvent(XEvent& event)
{
    static const Atom activeWindow = GetAtom(display, "_NET_ACTIVE_WINDOW");
    switch (event.type)
    {
    case PropertyNotify:
        if (event.xproperty.window == root && event.xproperty.atom == activeWindow)
            foreground.known = false;
        break;
    case DestroyNotify:
        if (event.xdestroywindow.window == tracked.window)
            tracked.destroyed = true;
        else if (event.xdestroywindow.window == tracked.frame)
            tracked.frame = 0;
        tracked.known = false;
        break;
    case ReparentNotify:
        if (event.xreparent.window == tracked.window && !tracked.destroyed)
            Reframe();
        tracked.known = false;
        break;
    case ConfigureNotify:
    case MapNotify:
    case UnmapNotify:
    case GravityNotify:
        tracked.known = false;
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&event.xmapping);
        virtualKeysKnown = false;
        break;
    }
}

bool QueryBounds(WindowId window, Rect& bounds)
{
    ErrorTrap trap(display);
    XWindowAttributes attributes{};
    int x = 0, y = 0;
    ::Window child;
    if (!XGetWindowAttributes(display, window, &attributes) || attributes.map_state != IsViewable ||
        !XTranslateCoordinates(display, window, root, 0, 0, &x, &y, &child) || trap.Failed() || Hidden(window))
        return false;
    bounds = { x, y, attributes.width, attributes.height };
    return true;
}

// The refresh rate of the monitor that shows most of the window, which GLFW reads from XRandR's CRTCs.
int RefreshRate(const Rect& bounds)
{
    int count = 0, rate = 60;
    long largest = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i)
    {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        int x = 0, y = 0;
        glfwGetMonitorPos(monitors[i], &x, &y);
        if (!mode || mode->refreshRate <= 0)
            continue;
        const long width = std::min(bounds.x + bounds.width, x + mode->width) - std::max(bounds.x, x);
        const long height = std::min(bounds.y + bounds.height, y + mode->height) - std::max(bounds.y, y);
        if (width > 0 && height > 0 && width * height > largest)
        {
            largest = width * height;
            rate = mode->refreshRate;
        }
    }
    return std::clamp(rate, 30, 360);
}

// Top-level windows, from the window manager or, without one, the root window's children.
std::vector<unsigned long> Clients()
{
    std::vector<unsigned long> clients = Property32(display, root, GetAtom(display, "_NET_CLIENT_LIST"), XA_WINDOW);
    if (clients.empty())
    {
        ::Window parent, rootReturn, *children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(display, root, &rootReturn, &parent, &children, &count) && children)
        {
            clients.assign(children, children + count);
            XFree(children);
        }
    }
    return clients;
}

// The window as ListWindows lists it. Call inside an ErrorTrap, since windows close at any time.
std::optional<Window> Listed(::Window client)
{
    static const std::set<Atom> skippedTypes = [] {
        std::set<Atom> types;
        for (const char* name : { "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
                                  "_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_TOOLTIP" })
            types.insert(GetAtom(display, name));
        return types;
    }();
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, client, &attributes) || attributes.map_state != IsViewable || attributes.override_redirect ||
        attributes.width < 64 || attributes.height < 64)
        return std::nullopt;
    for (unsigned long type : Property32(display, client, GetAtom(display, "_NET_WM_WINDOW_TYPE"), XA_ATOM))
        if (skippedTypes.count(type))
            return std::nullopt;
    if (Hidden(client))
        return std::nullopt;
    Window window{ client, Title(client), WindowPid(client) };
    if (window.title.empty() || window.pid == getpid())
        return std::nullopt;
    return window;
}
} // namespace

bool Init(std::string& error)
{
    XInitThreads();
    XSetErrorHandler(OnXError);
    display = XOpenDisplay(nullptr);
    if (!display)
    {
        error = "Unishade needs an X11 or XWayland display.";
        return false;
    }
    root = DefaultRootWindow(display);
    // For _NET_ACTIVE_WINDOW, so the window in front is only asked for when it changes.
    XSelectInput(display, root, PropertyChangeMask);
    int event, errorBase, major = 0, minor = 2;
    if (!XCompositeQueryExtension(display, &event, &errorBase) || !XCompositeQueryVersion(display, &major, &minor) || (major == 0 && minor < 2))
    {
        error = "The X server has no Composite extension, which Unishade needs to copy the game's picture.";
        return false;
    }
#ifdef UNISHADE_HAVE_XRES
    int xresMajor = 0, xresMinor = 0;
    haveXRes = xres.Load() && xres.queryExtension(display, &event, &errorBase) && xres.queryVersion(display, &xresMajor, &xresMinor) &&
               (xresMajor > 1 || (xresMajor == 1 && xresMinor >= 2));
#endif
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(display, True, &detectable);

    // GLFW only wakes for its own connection, so shortcuts and window changes on this one wake the loop from here.
    waker = std::thread([] {
        pollfd fd{ ConnectionNumber(display), POLLIN, 0 };
        while (!wakerStop)
            if (poll(&fd, 1, 200) > 0 && (fd.revents & POLLIN))
            {
                glfwPostEmptyEvent();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
    });

    if (WaylandDesktop())
        Log(LogLevel::Info, "Running through XWayland. Games that draw to Wayland directly cannot be captured.");
    return true;
}

void Shutdown()
{
    StopCapture();
    wakerStop = true;
    if (waker.joinable())
        waker.join();
    for (const auto& [id, grab] : std::map<int, Grab>(grabs))
        UnregisterHotkey(id);
    if (display)
        XCloseDisplay(display);
    display = nullptr;
}

std::vector<Window> ListWindows()
{
    std::vector<Window> windows;
    ErrorTrap trap(display);
    for (unsigned long client : Clients())
        if (std::optional<Window> window = Listed(client))
            windows.push_back(std::move(*window));
    trap.Failed();
    std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
    return windows;
}

std::optional<Window> ListedWindow(WindowId id)
{
    ErrorTrap trap(display);
    std::optional<Window> window = Listed(id);
    return trap.Failed() ? std::nullopt : window;
}

std::set<int> WindowOwners()
{
    std::set<int> owners;
    ErrorTrap trap(display);
    for (unsigned long client : Clients())
        if (const int pid = WindowPid(client); pid > 0)
            owners.insert(pid);
    trap.Failed();
    return owners;
}

bool WindowExists(const Window& window)
{
    // The game's window is there until the X server says it was destroyed.
    if (window.id == tracked.window)
        return !tracked.destroyed;
    ErrorTrap trap(display);
    XWindowAttributes attributes{};
    const bool exists = XGetWindowAttributes(display, window.id, &attributes) && !trap.Failed();
    return exists && WindowPid(window.id) == window.pid;
}

bool WindowBounds(WindowId window, Rect& bounds)
{
    if (window != tracked.window)
        return QueryBounds(window, bounds);
    const auto now = std::chrono::steady_clock::now();
    if (!tracked.known || now - tracked.checked > std::chrono::seconds(1))
    {
        tracked.visible = !tracked.destroyed && QueryBounds(window, tracked.bounds);
        tracked.known = true;
        tracked.checked = now;
        // The game can move to another monitor.
        if (tracked.visible)
            capture.refresh = RefreshRate(tracked.bounds);
    }
    bounds = tracked.bounds;
    return tracked.visible;
}

WindowId ForegroundWindow()
{
    const auto now = std::chrono::steady_clock::now();
    if (foreground.known && now - foreground.checked < std::chrono::milliseconds(foreground.fromProperty ? 1000 : 250))
        return foreground.window;
    const Foreground previous = foreground;
    foreground = {};
    foreground.known = true;
    foreground.checked = now;
    const auto active = Property32(display, root, GetAtom(display, "_NET_ACTIVE_WINDOW"), XA_WINDOW);
    if (!active.empty())
    {
        foreground.fromProperty = true;
        foreground.window = active[0];
    }
    else
    {
        // Without a window manager that tracks it, the window with input focus, up to its top-level window.
        ::Window focus = 0;
        int revert;
        XGetInputFocus(display, &focus, &revert);
        ErrorTrap trap(display);
        while (focus && focus != root && focus != PointerRoot)
        {
            ::Window parent = 0, rootReturn, *children = nullptr;
            unsigned int count = 0;
            if (!XQueryTree(display, focus, &rootReturn, &parent, &children, &count))
                break;
            if (children)
                XFree(children);
            if (parent == root)
            {
                foreground.window = trap.Failed() ? 0 : focus;
                break;
            }
            focus = parent;
        }
    }
    if (foreground.window == previous.window)
        foreground.pid = previous.pid;
    return foreground.window;
}

bool ProcessInFront(int pid)
{
    const WindowId window = ForegroundWindow();
    if (!window)
        return false;
    if (foreground.pid < 0)
    {
        ErrorTrap trap(display);
        const int owner = WindowPid(window);
        foreground.pid = trap.Failed() ? 0 : owner;
    }
    return foreground.pid == pid;
}

void Activate(const Window& window)
{
    // Source 2 says a pager asks, which window managers follow without their focus stealing prevention.
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window.id;
    event.xclient.message_type = GetAtom(display, "_NET_ACTIVE_WINDOW");
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

std::string ProcessExecutable(int pid)
{
    return pid > 0 ? ReadLink("/proc/" + std::to_string(pid) + "/exe") : std::string();
}

std::string ProcessCommand(int pid)
{
    std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string first;
    std::getline(input, first, '\0');
    return first;
}

std::vector<Process> ListProcesses()
{
    std::vector<Process> processes;
    DIR* proc = opendir("/proc");
    if (!proc)
        return processes;
    while (dirent* entry = readdir(proc))
    {
        const int pid = atoi(entry->d_name);
        if (pid <= 0)
            continue;
        Process process{ pid, ProcessExecutable(pid), ProcessCommand(pid) };
        if (!process.executable.empty() || !process.command.empty())
            processes.push_back(std::move(process));
    }
    closedir(proc);
    return processes;
}

void SetupOverlayWindow(GLFWwindow* window)
{
    Display* d = glfwGetX11Display();
    const ::Window x = glfwGetX11Window(window);
    const Atom states[] = { GetAtom(d, "_NET_WM_STATE_SKIP_TASKBAR"), GetAtom(d, "_NET_WM_STATE_SKIP_PAGER") };
    XChangeProperty(d, x, GetAtom(d, "_NET_WM_STATE"), XA_ATOM, 32, PropModeAppend, reinterpret_cast<const unsigned char*>(states), 2);
    // A utility window, in place of GLFW's normal one: it can still take focus for the menu and stay above the game,
    // while GNOME and KDE leave such windows out of the animations they play when a window opens or closes, which
    // would fade the overlay in and out over the game. Docks would suit too, but GNOME puts them below full-screen
    // windows.
    const Atom utility = GetAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY");
    XChangeProperty(d, x, GetAtom(d, "_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&utility), 1);
    // GNOME draws no shadow around a window whose frame extents say it draws its own. picom users can leave the
    // overlay out of shadows and fading by its class, unishade-overlay.
    const long extents[4] = {};
    XChangeProperty(d, x, GetAtom(d, "_GTK_FRAME_EXTENTS"), XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(extents), 4);
    XClassHint hint{ const_cast<char*>("unishade-overlay"), const_cast<char*>("Unishade") };
    XSetClassHint(d, x, &hint);
    XFlush(d);
}

void ShowOverlay(GLFWwindow* window, bool visible)
{
    if (!visible)
    {
        glfwHideWindow(window);
        return;
    }
    // A user time of zero asks the window manager not to focus the window when it appears.
    Display* d = glfwGetX11Display();
    const unsigned long zero = 0;
    XChangeProperty(d, glfwGetX11Window(window), GetAtom(d, "_NET_WM_USER_TIME"), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&zero), 1);
    glfwShowWindow(window);
    // Window managers can drop the above state while a window is hidden.
    glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_TRUE);
}

void FocusOverlay(GLFWwindow* window)
{
    Display* d = glfwGetX11Display();
    const ::Window x = glfwGetX11Window(window);
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = x;
    event.xclient.message_type = GetAtom(d, "_NET_ACTIVE_WINDOW");
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    // Without a window manager nobody answers that, so focus is set directly too.
    ErrorTrap trap(d);
    XSetInputFocus(d, x, RevertToParent, CurrentTime);
    trap.Failed();
}

bool StartCapture(const Window& window, std::string& error)
{
    StopCapture();
    {
        std::lock_guard lock(capture.mutex);
        capture.error.clear();
        capture.ready = {};
    }
    // Also sets the refresh rate of the window's monitor.
    Track(window.id);
    Rect bounds;
    if (!WindowBounds(window.id, bounds))
    {
        Untrack();
        error = "The window is not visible.";
        return false;
    }
    if (pipe2(capture.wake, O_CLOEXEC) != 0)
    {
        Untrack();
        error = "Could not start a thread to copy the game's picture.";
        return false;
    }
    capture.stop = false;
    capture.running = true;
    capture.onGpu = gpu.dmaBuf;
    capture.reset = false;
    capture.thread = std::thread(CaptureThread, static_cast<::Window>(window.id));
    return true;
}

void StopCapture()
{
    capture.stop = true;
    if (capture.wake[1] >= 0)
    {
        // Without it, the capture thread still sees stop within a second.
        [[maybe_unused]] const ssize_t woken = write(capture.wake[1], "", 1);
    }
    if (capture.thread.joinable())
        capture.thread.join();
    for (int& fd : capture.wake)
    {
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    capture.running = false;
    Untrack();
    // The host waited for the graphics card before stopping.
    std::lock_guard lock(capture.mutex);
    capture.ready = {};
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

namespace
{
// The dma-buf imported into Vulkan, for as long as the X server uses the same buffer for the window.
struct Imported
{
    std::shared_ptr<void> buffer;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
Imported imported;

// The caller makes sure the graphics card no longer uses it.
void ReleaseImported()
{
    if (imported.image)
        vkDestroyImage(gpu.device, imported.image, nullptr);
    if (imported.memory)
        vkFreeMemory(gpu.device, imported.memory, nullptr);
    imported = {};
}

// Whether the graphics card can read the X server's buffer as it is laid out: its modifier with as many planes, for
// copying from, imported from a dma-buf at its size.
bool CanImport(const DmaBuffer& buffer, VkFormat format)
{
    VkDrmFormatModifierPropertiesListEXT list{ VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 properties{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
    properties.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(gpu.physicalDevice, format, &properties);
    std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = modifiers.data();
    vkGetPhysicalDeviceFormatProperties2(gpu.physicalDevice, format, &properties);
    modifiers.resize(list.drmFormatModifierCount);
    const auto modifier = std::find_if(modifiers.begin(), modifiers.end(),
                                       [&](const VkDrmFormatModifierPropertiesEXT& entry) { return entry.drmFormatModifier == buffer.modifier; });
    if (modifier == modifiers.end() || modifier->drmFormatModifierPlaneCount != buffer.fds.size() ||
        !(modifier->drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        return false;

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT };
    modifierInfo.drmFormatModifier = buffer.modifier;
    modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
    externalInfo.pNext = &modifierInfo;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
    info.pNext = &externalInfo;
    info.format = format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties external{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 result{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    result.pNext = &external;
    return vkGetPhysicalDeviceImageFormatProperties2(gpu.physicalDevice, &info, &result) == VK_SUCCESS &&
           (external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           result.imageFormatProperties.maxExtent.width >= buffer.width && result.imageFormatProperties.maxExtent.height >= buffer.height;
}

VkImage Import(const std::shared_ptr<void>& held)
{
    if (imported.buffer == held)
        return imported.image;
    vkDeviceWaitIdle(gpu.device);
    ReleaseImported();
    const DmaBuffer& buffer = *static_cast<const DmaBuffer*>(held.get());
    // XRGB8888 and ARGB8888, the formats of 24 and 32-bit windows.
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    if (buffer.fds.size() > 4 || !CanImport(buffer, format))
    {
        Log(LogLevel::Info, "The graphics card cannot read the X server's layout of the game's picture (modifier 0x%llx).",
            static_cast<unsigned long long>(buffer.modifier));
        return VK_NULL_HANDLE;
    }

    // Every plane must be in one buffer object, which is how drivers lay out RGB images.
    struct stat first{}, other{};
    if (fstat(buffer.fds[0], &first) != 0)
        return VK_NULL_HANDLE;
    for (int fd : buffer.fds)
        if (fstat(fd, &other) != 0 || other.st_ino != first.st_ino || other.st_dev != first.st_dev)
            return VK_NULL_HANDLE;

    std::vector<VkSubresourceLayout> planes;
    for (size_t i = 0; i < buffer.fds.size(); ++i)
        planes.push_back({ buffer.offsets[i], 0, buffer.strides[i], 0, 0 });
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
    modifier.drmFormatModifier = buffer.modifier;
    modifier.drmFormatModifierPlaneCount = uint32_t(planes.size());
    modifier.pPlaneLayouts = planes.data();
    VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.pNext = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { buffer.width, buffer.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Imported result;
    if (vkCreateImage(gpu.device, &info, nullptr, &result.image) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    const auto getFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(gpu.device, "vkGetMemoryFdPropertiesKHR"));
    VkMemoryFdPropertiesKHR fdProperties{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(gpu.device, result.image, &requirements);
    // Importing hands the file descriptor to Vulkan, so it gets its own copy.
    const int fd = dup(buffer.fds[0]);
    uint32_t type = UINT32_MAX;
    if (fd >= 0 && getFdProperties &&
        getFdProperties(gpu.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProperties) == VK_SUCCESS)
        type = gpu.FindMemoryType(requirements.memoryTypeBits & fdProperties.memoryTypeBits, 0);
    VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = result.image;
    VkImportMemoryFdInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = fd;
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.pNext = &import;
    // A dedicated allocation is the image's size, which the dma-buf must hold. Where the kernel cannot tell the
    // dma-buf's size, the driver checks it.
    const off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (type == UINT32_MAX || (size >= 0 && VkDeviceSize(size) < requirements.size) ||
        vkAllocateMemory(gpu.device, &allocation, nullptr, &result.memory) != VK_SUCCESS)
    {
        if (fd >= 0)
            close(fd);
        vkDestroyImage(gpu.device, result.image, nullptr);
        return VK_NULL_HANDLE;
    }
    if (vkBindImageMemory(gpu.device, result.image, result.memory, 0) != VK_SUCCESS)
    {
        vkDestroyImage(gpu.device, result.image, nullptr);
        vkFreeMemory(gpu.device, result.memory, nullptr);
        return VK_NULL_HANDLE;
    }

    // Takes the image from the X server once to move it into the general layout, keeping its contents, then
    // hands it back. From here on each use takes and returns it in that layout.
    VkCommandBuffer commands = gpu.BeginCommands();
    if (!commands)
    {
        vkDestroyImage(gpu.device, result.image, nullptr);
        vkFreeMemory(gpu.device, result.memory, nullptr);
        return VK_NULL_HANDLE;
    }
    const uint32_t outside = gpu.foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = outside;
    barrier.dstQueueFamilyIndex = gpu.queueFamily;
    barrier.image = result.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = gpu.queueFamily;
    barrier.dstQueueFamilyIndex = outside;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if (!gpu.SubmitAndWait(commands))
    {
        // Commands that failed to finish may still use the image.
        vkDeviceWaitIdle(gpu.device);
        vkDestroyImage(gpu.device, result.image, nullptr);
        vkFreeMemory(gpu.device, result.memory, nullptr);
        return VK_NULL_HANDLE;
    }

    result.buffer = held;
    imported = result;
    return imported.image;
}
} // namespace

bool TakeFrame(Frame& frame)
{
    {
        std::lock_guard lock(capture.mutex);
        if (capture.ready.serial <= frame.serial || (capture.ready.pixels.empty() && !capture.ready.hold))
            return false;
        std::swap(frame, capture.ready);
    }
    frame.image = VK_NULL_HANDLE;
    frame.foreign = false;
    if (frame.hold)
    {
        frame.image = Import(frame.hold);
        frame.foreign = true;
        if (!frame.image)
        {
            // Copies through memory from the next frame on, which the capture thread notices when it names the
            // window's pixmap again.
            Log(LogLevel::Warning, "Could not use the game's picture on the graphics card. Copying it instead.");
            capture.onGpu = false;
            capture.reset = true;
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
    return true;
}

void RequestCapturePermission()
{
}

void SetHotkeyCallback(HotkeyCallback callback)
{
    hotkeyCallback = std::move(callback);
}

bool RegisterHotkey(int id, const Hotkey& hotkey)
{
    UnregisterHotkey(id);
    const KeySym sym = Keysym(hotkey.key);
    const KeyCode code = sym == NoSymbol ? 0 : XKeysymToKeycode(display, sym);
    if (!code)
        return false;
    const unsigned modifiers = XModifiers(hotkey.modifiers);
    ErrorTrap trap(display);
    for (unsigned variant : kLockVariants)
        XGrabKey(display, code, modifiers | variant, root, False, GrabModeAsync, GrabModeAsync);
    if (trap.Failed())
    {
        for (unsigned variant : kLockVariants)
            XUngrabKey(display, code, modifiers | variant, root);
        XSync(display, False);
        return false;
    }
    grabs[id] = { code, modifiers };
    return true;
}

void UnregisterHotkey(int id)
{
    const auto found = grabs.find(id);
    if (found == grabs.end())
        return;
    for (unsigned variant : kLockVariants)
        XUngrabKey(display, found->second.code, found->second.modifiers | variant, root);
    XFlush(display);
    grabs.erase(found);
    held.erase(id);
}

void PollHotkeys()
{
    while (XPending(display))
    {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type != KeyPress && event.type != KeyRelease)
        {
            HandleEvent(event);
            continue;
        }
        const bool pressed = event.type == KeyPress;
        for (const auto& [id, grab] : grabs)
        {
            if (grab.code != event.xkey.keycode)
                continue;
            // A key let go ends its shortcut even when the modifiers were let go first.
            if (pressed ? (event.xkey.state & kRelevantModifiers) != grab.modifiers : !held.count(id))
                continue;
            if (pressed)
            {
                if (!held.insert(id).second)
                    continue; // held down, repeating
            }
            else
                held.erase(id);
            if (hotkeyCallback)
                hotkeyCallback(id, pressed);
        }
    }
}

bool SaveWindowIcon(const Window& window, const std::string& path)
{
    // _NET_WM_ICON holds each size as its width, its height and then ARGB pixels, one per 32-bit item.
    ErrorTrap trap(display);
    Atom actualType;
    int format = 0;
    unsigned long count = 0, remaining;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window.id, GetAtom(display, "_NET_WM_ICON"), 0, 1 << 22, False, XA_CARDINAL, &actualType, &format, &count,
                           &remaining, &data) != Success || !data)
    {
        trap.Failed();
        return false;
    }
    const unsigned long* items = reinterpret_cast<const unsigned long*>(data);
    size_t largest = 0, largestSize = 0;
    for (size_t i = 0; format == 32 && i + 2 <= count;)
    {
        const size_t size = items[i] * items[i + 1];
        if (!size || size > count - i - 2)
            break;
        if (size > largestSize)
        {
            largest = i;
            largestSize = size;
        }
        i += 2 + size;
    }
    bool saved = false;
    if (largestSize)
    {
        const int width = static_cast<int>(items[largest]), height = static_cast<int>(items[largest + 1]);
        std::vector<uint8_t> rgba(largestSize * 4);
        for (size_t p = 0; p < largestSize; ++p)
        {
            const unsigned long argb = items[largest + 2 + p];
            rgba[p * 4] = (argb >> 16) & 0xFF;
            rgba[p * 4 + 1] = (argb >> 8) & 0xFF;
            rgba[p * 4 + 2] = argb & 0xFF;
            rgba[p * 4 + 3] = (argb >> 24) & 0xFF;
        }
        saved = stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
    }
    XFree(data);
    return !trap.Failed() && saved;
}

void Open(const std::string& target)
{
    const char* argv[] = { "xdg-open", target.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0)
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}

std::string UiFont(bool bold)
{
    const char* match = bold ? "fc-match -f '%{file}' 'sans-serif:bold' 2>/dev/null" : "fc-match -f '%{file}' 'sans-serif:style=Regular' 2>/dev/null";
    if (FILE* pipe = popen(match, "r"))
    {
        char buffer[1024]{};
        const size_t size = fread(buffer, 1, sizeof(buffer) - 1, pipe);
        pclose(pipe);
        const std::string path(buffer, size);
        const std::string extension = path.size() > 4 ? path.substr(path.size() - 4) : "";
        if ((extension == ".ttf" || extension == ".otf") && access(path.c_str(), R_OK) == 0)
            return path;
    }
    const std::initializer_list<const char*> regularFonts = { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                                              "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                                              "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" };
    const std::initializer_list<const char*> boldFonts = { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                                           "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                                           "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf" };
    for (const char* path : bold ? boldFonts : regularFonts)
        if (access(path, R_OK) == 0)
            return path;
    return {};
}

std::vector<std::pair<std::string, int>> UiFallbackFonts(bool bold)
{
    // fontconfig's font for each language, which may be one collection holding all of them.
    std::vector<std::string> languages = { "zh-cn", "ja", "ko", "zh-tw" };
    const char* lang = getenv("LANG");
    const std::string user = lang ? lang : "";
    const size_t first = !user.compare(0, 2, "ja") ? 1 : !user.compare(0, 2, "ko") ? 2 : !user.compare(0, 5, "zh_TW") || !user.compare(0, 5, "zh_HK") ? 3 : 0;
    std::rotate(languages.begin(), languages.begin() + static_cast<std::ptrdiff_t>(first), languages.begin() + static_cast<std::ptrdiff_t>(first) + 1);
    std::vector<std::pair<std::string, int>> fonts;
    for (const std::string& language : languages)
    {
        const std::string match = "fc-match -f '%{file}\\n%{index}' 'sans-serif:lang=" + language + (bold ? ":bold" : "") + "' 2>/dev/null";
        FILE* pipe = popen(match.c_str(), "r");
        if (!pipe)
            continue;
        char buffer[1024]{};
        const size_t size = fread(buffer, 1, sizeof(buffer) - 1, pipe);
        pclose(pipe);
        const std::string output(buffer, size);
        const size_t newline = output.find('\n');
        if (newline == std::string::npos)
            continue;
        std::pair<std::string, int> font{ output.substr(0, newline), atoi(output.c_str() + newline + 1) };
        const std::string extension = font.first.size() > 4 ? font.first.substr(font.first.size() - 4) : "";
        if ((extension == ".ttf" || extension == ".otf" || extension == ".ttc") && access(font.first.c_str(), R_OK) == 0 &&
            std::find(fonts.begin(), fonts.end(), font) == fonts.end())
            fonts.push_back(std::move(font));
    }
    return fonts;
}

void ReadInput(std::array<bool, 256>& keys, std::array<bool, 5>& buttons)
{
    keys = {};
    buttons = {};
    if (!virtualKeysKnown)
        MapVirtualKeys();
    char state[32] = {};
    XQueryKeymap(display, state);
    for (int code = 0; code < 256; ++code)
        if (((static_cast<unsigned char>(state[code / 8]) >> (code % 8)) & 1) && virtualKeys[code])
            keys[virtualKeys[code]] = true;
    ::Window rootReturn = 0, child = 0;
    int rootX = 0, rootY = 0, x = 0, y = 0;
    unsigned int mask = 0;
    if (XQueryPointer(display, root, &rootReturn, &child, &rootX, &rootY, &x, &y, &mask))
    {
        buttons[0] = mask & Button1Mask;
        buttons[1] = mask & Button3Mask;
        buttons[2] = mask & Button2Mask;
    }
}

bool WaylandDesktop()
{
    const char* wayland = getenv("WAYLAND_DISPLAY");
    const char* session = getenv("XDG_SESSION_TYPE");
    return (wayland && *wayland) || (session && !strcmp(session, "wayland"));
}

bool DisplayDrmDevice([[maybe_unused]] int64_t& deviceMajor, [[maybe_unused]] int64_t& deviceMinor)
{
#ifdef UNISHADE_HAVE_DRI3
    const Dri3Functions* dri3 = display ? Dri3(display) : nullptr;
    if (!dri3)
        return false;
    // The device the X server hands its clients for drawing, the same one its own buffers live on.
    xcb_connection_t* connection = dri3->getConnection(display);
    xcb_dri3_open_reply_t* reply = dri3->openReply(connection, dri3->open(connection, root, 0), nullptr);
    if (!reply)
        return false;
    const int fd = reply->nfd == 1 ? dri3->openReplyFds(connection, reply)[0] : -1;
    free(reply);
    struct stat device{};
    const bool found = fd >= 0 && fstat(fd, &device) == 0 && S_ISCHR(device.st_mode);
    if (fd >= 0)
        close(fd);
    if (!found)
        return false;
    deviceMajor = major(device.st_rdev);
    deviceMinor = minor(device.st_rdev);
    return true;
#else
    return false;
#endif
}
} // namespace platform
