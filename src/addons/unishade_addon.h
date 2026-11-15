#pragma once

// Unishade add-ons: native libraries Unishade loads from the Addons folder in its data folder, which on Windows is the
// folder of Unishade.exe. This header is the whole interface. It is plain C, so an add-on can be built with any
// compiler and C runtime.
//
// Writing one
//
// Build a shared library, a .dll on Windows, .dylib on macOS or .so on Linux, that exports this function, inside
// extern "C" in C++, and put it in the Addons folder:
//
//     UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void);
//
// It returns what the add-on is and what to call. Anything else the add-on needs goes in a folder beside it named after
// it without the extension, such as Addons/MyAddon/ for Addons/MyAddon.dll, which UnishadeHost.folder names. Unishade
// asks once before it loads a library that is not an official add-on and remembers the answer for that path, so a
// rebuilt library loads without asking again. The user turns add-ons on and off in Unishade, and each one's settings
// show there.
//
// Unishade calls the add-on's functions on its main thread. Add-ons may run threads of their own.
//
// Frames
//
// An add-on with UNISHADE_ADDON_WRITES_DEPTH or UNISHADE_ADDON_CHANGES_PICTURE in its flags gets every frame the overlay
// draws with effects on, before the effects run. Effects read what it writes to depth as DEPTH, and get output instead
// of the game's picture. Add-ons that write depth see the picture before add-ons that change it. When more than one
// changes the picture, each gets the one before's result as color.
//
// Frames are on the graphics card, on Windows only for now. The images and the fence are Direct3D 11 objects on the
// graphics card UnishadeHost.adapter names, shared through NT handles. Make the add-on's own Direct3D 11 or 12 device
// on that adapter and open them there: ID3D12Device::OpenSharedHandle, or ID3D11Device1::OpenSharedResource1 and
// ID3D11Device5::OpenSharedFence. The textures can be read by shaders, written as render targets and through
// unordered access, and copied. In Direct3D 12 they are in the COMMON state and have to be left in it. Open them
// again whenever the frame's generation changes.
//
// Wait for the fence to reach wait before touching the images, and set it to signal once done with them, either on
// the graphics card with ID3D12CommandQueue or ID3D11DeviceContext4's Wait and Signal, or from the processor. Signal it
// even when there is nothing to do. On the graphics card it is enough to queue the signal before frame returns.
// Effects wait for it, and Unishade does not write the images again before then, so work that takes longer than a
// frame has to copy what it needs and write its results in a later frame.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The version of this interface. An add-on says which one it was built for, and Unishade only loads add-ons built for
// one it knows.
#define UNISHADE_ADDON_VERSION 1

#ifdef _WIN32
#define UNISHADE_ADDON_EXPORT __declspec(dllexport)
#else
#define UNISHADE_ADDON_EXPORT __attribute__((visibility("default")))
#endif

// What an add-on does with frames, for UnishadeAddon.flags.
#define UNISHADE_ADDON_WRITES_DEPTH 0x1u
#define UNISHADE_ADDON_CHANGES_PICTURE 0x2u

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum UnishadeLogLevel
{
    UNISHADE_LOG_INFO = 0,
    // Warnings and errors also show in Unishade's launcher and menu.
    UNISHADE_LOG_WARNING = 1,
    UNISHADE_LOG_ERROR = 2,
} UnishadeLogLevel;

// What Unishade tells an add-on it starts. Valid until destroy.
typedef struct UnishadeHost
{
    uint32_t version; // the UNISHADE_ADDON_VERSION Unishade was built with
    // UTF-8 and ending in a separator. The add-on's own folder may not exist.
    const char* folder;
    const char* dataFolder; // Unishade's data folder, which holds Unishade.log
    // Windows: the LUID of the graphics card Unishade draws with, as the bytes of a LUID. Frames can only be shared
    // with a device on it.
    uint8_t adapter[8];
    // Writes a line to Unishade.log, after the add-on's name. Pass the host the add-on was given. Safe from any thread.
    void (*log)(const struct UnishadeHost* host, UnishadeLogLevel level, const char* text);
} UnishadeHost;

typedef enum UnishadeSettingType
{
    UNISHADE_SETTING_TOGGLE = 0,  // 0 or 1
    UNISHADE_SETTING_SLIDER = 1,  // a number from min to max
    UNISHADE_SETTING_INTEGER = 2, // a whole number from min to max
    UNISHADE_SETTING_CHOICE = 3,  // which of choices, from 0
} UnishadeSettingType;

// A setting Unishade shows for the add-on and keeps with each preset, so it changes with the preset.
typedef struct UnishadeSetting
{
    const char* key; // in the add-on's section of the preset, which is named after the add-on
    const char* label;
    const char* tooltip; // or NULL
    UnishadeSettingType type;
    float defaultValue;
    float min;
    float max;
    const char* choices; // CHOICE: the labels, each ending in '\0', with one more '\0' after the last
} UnishadeSetting;

typedef enum UnishadeFormat
{
    UNISHADE_FORMAT_BGRA8 = 0,     // DXGI_FORMAT_B8G8R8A8_UNORM: color as games draw it, in the sRGB encoding
    UNISHADE_FORMAT_R32_FLOAT = 1, // DXGI_FORMAT_R32_FLOAT
} UnishadeFormat;

typedef struct UnishadeImage
{
    void* handle; // Windows: an NT handle to a Direct3D 11 texture. Unishade closes it. NULL when the add-on gets none.
    UnishadeFormat format;
} UnishadeImage;

typedef struct UnishadeFrame
{
    uint32_t width; // of every image
    uint32_t height;
    // Changes whenever the images and the fence are made again, such as when the game's window changes size. New
    // images hold nothing yet.
    uint64_t generation;
    // Changes when the game's picture does. With the same picture and generation as the frame before, the add-on can
    // skip its work: Unishade keeps output and depth as they are.
    uint64_t picture;
    // The game's picture as effects get it, BGRA8. Only read it.
    UnishadeImage color;
    // UNISHADE_ADDON_CHANGES_PICTURE: the changed picture, BGRA8, which effects get instead of color.
    UnishadeImage output;
    // UNISHADE_ADDON_WRITES_DEPTH: R32_FLOAT, which effects read as DEPTH. As games give ReShade depth with Unishade's
    // preprocessor definitions: 0 nearest and 1 at the far plane, not reversed, not upside down, not logarithmic, and
    // non-linear for a far plane of 1000.
    UnishadeImage depth;
    void* fence; // Windows: an NT handle to a Direct3D 11 fence. Unishade closes it.
    uint64_t wait;   // the images are ready once the fence reaches this
    uint64_t signal; // set the fence to this once done with them
    const float* settings; // the settings' values, in the order UnishadeAddon lists them
} UnishadeFrame;

typedef struct UnishadeAddon
{
    uint32_t version; // UNISHADE_ADDON_VERSION
    // As Unishade shows it. It also names the add-on's section in presets, so it has to stay the same.
    const char* name;
    const char* description;
    uint32_t flags;
    const UnishadeSetting* settings;
    uint32_t settingCount;
    // Starts the add-on. Returns what the other functions are given, or NULL with the reason in error.
    void* (*create)(const UnishadeHost* host, char* error, size_t errorSize);
    // Works on a frame, as the comment at the top says. Returns false with the reason in error to stop the add-on.
    bool (*frame)(void* addon, const UnishadeFrame* frame, char* error, size_t errorSize);
    // Stops the add-on once whatever it still has on the graphics card is done. Unishade may unload it afterwards.
    void (*destroy)(void* addon);
} UnishadeAddon;

typedef const UnishadeAddon* (*UnishadeGetAddonFunction)(void);

#ifdef __cplusplus
}
#endif
