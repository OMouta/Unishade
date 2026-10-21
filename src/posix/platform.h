#pragma once

#include "hotkeys.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct GLFWwindow;

// What differs between macOS and Linux: finding windows, copying their picture, global shortcuts and the
// overlay window's place above the game. Implemented in platform_macos.mm and platform_x11.cpp. Everything here
// runs on the main thread unless it says otherwise.
namespace platform
{
using WindowId = uint64_t;

struct Window
{
    WindowId id = 0;
    std::string title;
    int pid = 0;
};

// The window's content area in GLFW screen coordinates: points on macOS, pixels on X11.
struct Rect
{
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};

// Call after glfwInit. Returns false with a message when the platform cannot run the host at all.
bool Init(std::string& error);
void Shutdown();

// Visible top-level windows of other programs with a title, sorted by title.
std::vector<Window> ListWindows();
// The window as ListWindows lists it, or nothing when ListWindows would leave it out.
std::optional<Window> ListedWindow(WindowId window);
// The processes that own a top-level window, shown or not.
std::set<int> WindowOwners();
bool WindowExists(const Window& window);
// False when the window is minimized, hidden or gone.
bool WindowBounds(WindowId window, Rect& bounds);
// The window that has keyboard focus, or 0. On macOS, the frontmost window of the frontmost program.
WindowId ForegroundWindow();
// Whether the program that owns the window is in front. Games can have more than one window.
bool ProcessInFront(int pid);
// Gives keyboard focus to the window.
void Activate(const Window& window);

// The full path of the program's executable and how it was started, for matching saved games. Wine games run
// as wine64-preloader or similar, so the command line's first argument names the Windows executable. Empty
// when the process is gone.
std::string ProcessExecutable(int pid);
std::string ProcessCommand(int pid);
// Every process with its identifiers, for matching saved games. Reads every process, so it is slow.
struct Process
{
    int pid;
    std::string executable;
    std::string command;
};
std::vector<Process> ListProcesses();
// Saves the icon of the window's program as a PNG. Returns false when it has none.
bool SaveWindowIcon(const Window& window, const std::string& path);

// Makes the overlay stay above the game, out of the taskbar and the window switcher, and on every Space. Call
// before the window is first shown.
void SetupOverlayWindow(GLFWwindow* window);
// Shows or hides the overlay without taking focus from the game.
void ShowOverlay(GLFWwindow* window, bool visible);
// Gives the overlay keyboard focus while the menu is open.
void FocusOverlay(GLFWwindow* window);

// The game's picture. Where the platform can, it stays on the graphics card: image is then an image in
// VK_IMAGE_LAYOUT_GENERAL whose part at (x, y) of width by height is the window, and stays valid until capture
// stops. Otherwise pixels holds it, copied by a thread of the platform's: BGRA, top row first, with opaque alpha.
struct Frame
{
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t serial = 0;
    VkImage image = VK_NULL_HANDLE;
    uint32_t x = 0;
    uint32_t y = 0;
    bool foreign = false; // written outside Vulkan, so each use acquires it from outside first
    // Keeps the platform from reusing the frame's buffer. Held until the graphics card is done with the frame.
    std::shared_ptr<void> hold;
};

// Starts copying the window. Frames arrive on another thread, which calls glfwPostEmptyEvent so the main loop
// wakes. Returns false with a message when capture cannot start.
bool StartCapture(const Window& window, std::string& error);
void StopCapture();
bool Capturing();
// Slows capture down while nobody sees its frames, such as while another program is in front. Frames keep
// coming a few times a second, so the overlay has a recent one when it shows again.
void SetCaptureIdle(bool idle);
// Swaps in the newest frame when it is newer than frame.serial. Returns whether it did.
bool TakeFrame(Frame& frame);
// Set when capture stopped by itself, such as when the window closed. Cleared by StartCapture.
std::string CaptureError();

// macOS asks for permission to record the screen. Linux always has it.
bool HasCapturePermission();
void RequestCapturePermission();

// Global shortcuts. The callback runs on the main thread from PollHotkeys, with pressed false when the key is let
// go.
using HotkeyCallback = std::function<void(int id, bool pressed)>;
void SetHotkeyCallback(HotkeyCallback callback);
// Returns false when another program holds the shortcut.
bool RegisterHotkey(int id, const Hotkey& hotkey);
void UnregisterHotkey(int id);
// Handles what happened since the last call: shortcuts, and on X11 the changes to the game's window and the window
// in front that the functions above keep track of. Call at the start of every pass of the main loop.
void PollHotkeys();

// The keys and mouse buttons held down now, wherever the focus is, for effects. Keys are indexed by Windows
// virtual-key code, as ReShade's effects and presets name them. Buttons are left, right, middle, back and
// forward. X11 cannot tell the back and forward buttons.
void ReadInput(std::array<bool, 256>& keys, std::array<bool, 5>& buttons);

// Whether this is a Wayland desktop, where games that draw to Wayland directly have no X11 window to capture.
bool WaylandDesktop();

// Opens a folder or a web page with the system's default program.
void Open(const std::string& target);

// The DRM device the display server draws with, as the major and minor numbers of its primary or render node, so
// the Vulkan device can be the same GPU. False when unknown, as on macOS.
bool DisplayDrmDevice(int64_t& major, int64_t& minor);

// A sans-serif font for the launcher and the menu, regular or bold, or empty when the system has none.
std::string UiFont(bool bold);
// Fonts for what that font lacks, such as Chinese, Japanese and Korean, the user's own language first: each file with
// the font's index in it, for collections.
std::vector<std::pair<std::string, int>> UiFallbackFonts(bool bold);
} // namespace platform
