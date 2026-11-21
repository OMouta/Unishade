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
// Frames are on the graphics card UnishadeHost.adapter names, in images the add-on opens with a device of its own
// there. Open them again whenever the frame's generation changes.
//
// Windows: the images and the fence are Direct3D 11 objects, shared through NT handles. Make the add-on's own
// Direct3D 11 or 12 device on that adapter and open them there: ID3D12Device::OpenSharedHandle, or
// ID3D11Device1::OpenSharedResource1 and ID3D11Device5::OpenSharedFence. The textures can be read by shaders, written
// as render targets and through unordered access, and copied. In Direct3D 12 they are in the COMMON state and have to
// be left in it.
//
// macOS: the images are IOSurfaces. Wrap them in Metal textures on that MTLDevice with
// newTextureWithDescriptor:iosurface:plane:, or in CVPixelBuffers with CVPixelBufferCreateWithIOSurface, for Core ML
// or Vision. The textures can be read, written and rendered to.
//
// Linux: the images are dma-bufs, each described by a UnishadeDmaBuf. Import them on that device in Vulkan, with
// VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier, or in EGL, with
// EGL_EXT_image_dma_buf_import_modifiers. They can be sampled, rendered to and copied, and written as storage images
// where the driver allows it. In Vulkan their contents are in VK_IMAGE_LAYOUT_GENERAL: acquire them from
// VK_QUEUE_FAMILY_FOREIGN_EXT in that layout before using them, and release them to it afterwards.
//
// On Windows, wait for the fence to reach wait before touching the images, and set it to signal once done with them,
// either on the graphics card with ID3D12CommandQueue or ID3D11DeviceContext4's Wait and Signal, or from the
// processor. Signal it even when there is nothing to do. On the graphics card it is enough to queue the signal before
// frame returns. Effects wait for it.
//
// On macOS and Linux there is no fence. The images are ready when frame is called, and whatever the add-on does with
// them on the graphics card has to be finished when frame returns, whether it succeeds or not: wait for it, such as
// with waitUntilCompleted on the last Metal command buffer, a Vulkan fence, or glFinish.
//
// Unishade does not write the images again before the add-on is done with them, so work that takes longer than a
// frame has to copy what it needs and write its results in a later frame.
//
// After effects
//
// An add-on with UNISHADE_ADDON_READS_RESULT or UNISHADE_ADDON_CHANGES_RESULT gets the picture again once effects ran on
// it, before it shows and before screenshots read it, with the same images, fence and rules. Add-ons that read it see
// it before add-ons that change it, and each one that changes it gets the one before's result as color. An add-on can
// ask for both stages: then frame is called twice for the same picture, and UnishadeFrame.stage says which call it is.
//
// HDR
//
// From version 2 on, a game's picture can be HDR, in RGBA16_FLOAT. An add-on with UNISHADE_ADDON_TAKES_HDR gets it as it
// is, in color and output in both stages, and what it writes to output stays HDR. Any other add-on gets an SDR copy in
// BGRA8, as screenshots make it: divided by SDR white, clamped and encoded as sRGB. One that only reads the picture or
// writes depth then works as it does with an SDR game. One that changes the picture or the result sits out while the
// picture is HDR, since what it writes could not replace an HDR picture, and runs again once it is SDR. A game can
// switch at any time, which makes the images again and changes the generation, so look at color.format on every frame.
//
// Effects
//
// From version 2 on, UnishadeHost has functions that read and change the preset's effects as the user does in the
// menu. Call them on the main thread only, from create, frame or update.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The version of this interface. An add-on says which one it was built for, and Unishade only loads add-ons built for
// one it knows. What is marked version 2 below is new in version 2. Structs only grow at their end, so add-ons built
// for version 1 still load.
#define UNISHADE_ADDON_VERSION 2

#ifdef _WIN32
#define UNISHADE_ADDON_EXPORT __declspec(dllexport)
#else
#define UNISHADE_ADDON_EXPORT __attribute__((visibility("default")))
#endif

// What an add-on does with frames, for UnishadeAddon.flags.
#define UNISHADE_ADDON_WRITES_DEPTH 0x1u
#define UNISHADE_ADDON_CHANGES_PICTURE 0x2u
// Version 2: the picture after effects, as the comment at the top says.
#define UNISHADE_ADDON_READS_RESULT 0x4u
#define UNISHADE_ADDON_CHANGES_RESULT 0x8u
// Version 2: takes HDR pictures, as the comment at the top says.
#define UNISHADE_ADDON_TAKES_HDR 0x10u

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

// Version 2: a technique, as UnishadeHost.getTechnique describes it. The strings stay valid until
// UnishadeHost.effectsGeneration changes.
typedef struct UnishadeTechnique
{
    const char* name; // as its effect declares it
    const char* file; // the effect file's name, such as "Bloom.fx"
    uint32_t effect;  // the effect's index, for the uniform functions
    bool enabled;
} UnishadeTechnique;

typedef enum UnishadeValueType
{
    UNISHADE_VALUE_BOOL = 0,
    UNISHADE_VALUE_INT = 1,
    UNISHADE_VALUE_UINT = 2,
    UNISHADE_VALUE_FLOAT = 3,
} UnishadeValueType;

// Version 2: one of an effect's uniforms that presets keep, as UnishadeHost.getUniform describes it. The name stays
// valid until UnishadeHost.effectsGeneration changes.
typedef struct UnishadeUniform
{
    const char* name; // as the effect declares it, which is its key in the preset
    UnishadeValueType type;
    // Its values: rows times columns, times the length of an array. getValue and setValue take them in that order.
    uint32_t count;
    float min; // ui_min and ui_max, or -FLT_MAX and FLT_MAX when the effect gives none
    float max;
} UnishadeUniform;

// What Unishade tells an add-on it starts. Valid until destroy.
typedef struct UnishadeHost
{
    uint32_t version; // the UNISHADE_ADDON_VERSION Unishade was built with
    // UTF-8 and ending in a separator. The add-on's own folder may not exist.
    const char* folder;
    const char* dataFolder; // Unishade's data folder, which holds Unishade.log
    // The graphics card Unishade draws with. Frames can only be shared with a device on it. Windows: its LUID, as the
    // bytes of a LUID. macOS: the registryID of its MTLDevice, as the bytes of a uint64_t. Linux: the dev_t of its DRM
    // render node, as the bytes of a uint64_t: st_rdev of its /dev/dri/renderD* file, or makedev of
    // VkPhysicalDeviceDrmPropertiesEXT's renderMajor and renderMinor. All zero when Vulkan does not say.
    uint8_t adapter[8];
    // Writes a line to Unishade.log, after the add-on's name. Pass the host the add-on was given. Safe from any thread.
    void (*log)(const struct UnishadeHost* host, UnishadeLogLevel level, const char* text);

    // Version 2: the preset's effects. Main thread only, from create, frame or update. Techniques are listed in the order
    // they run, every one that loaded, on or off. Effects, techniques and uniforms are numbered from 0. The numbers and
    // the strings from before stop being valid whenever effectsGeneration changes, which it does when a preset loads,
    // effects compile and the user moves a technique. Each function takes the host the add-on was given.
    uint64_t (*effectsGeneration)(const struct UnishadeHost* host);
    uint32_t (*techniqueCount)(const struct UnishadeHost* host);
    // False when there is no such technique.
    bool (*getTechnique)(const struct UnishadeHost* host, uint32_t technique, UnishadeTechnique* description);
    // Turns a technique on or off, as the menu does: the preset then has changes to save.
    void (*setTechnique)(const struct UnishadeHost* host, uint32_t technique, bool enabled);
    // The uniforms an effect's section in the preset keeps, which leaves out those Unishade sets, such as the timer.
    uint32_t (*uniformCount)(const struct UnishadeHost* host, uint32_t effect);
    // False when there is no such uniform.
    bool (*getUniform)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, UnishadeUniform* description);
    // Writes up to count of the uniform's values to values, as floats. Returns how many it has, or 0 when there is no
    // such uniform.
    uint32_t (*getValue)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, float* values, uint32_t count);
    // Sets the uniform's first count values, converted to its type, as the menu does: the preset then has changes to
    // save. min and max do not limit them. False when there is no such uniform.
    bool (*setValue)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, const float* values, uint32_t count);
} UnishadeHost;

typedef enum UnishadeSettingType
{
    UNISHADE_SETTING_TOGGLE = 0,  // 0 or 1
    UNISHADE_SETTING_SLIDER = 1,  // a number from min to max
    UNISHADE_SETTING_INTEGER = 2, // a whole number from min to max
    UNISHADE_SETTING_CHOICE = 3,  // which of choices, from 0
    // Version 2.
    UNISHADE_SETTING_COLOR_RGB = 4,  // three values: red, green and blue from 0 to 1
    UNISHADE_SETTING_COLOR_RGBA = 5, // four, with alpha
    // A button with the label on it, which presets do not keep: 1 in the add-on's first frame or update call after it
    // was pressed, whichever comes first, otherwise 0.
    UNISHADE_SETTING_BUTTON = 6,
} UnishadeSettingType;

// A setting Unishade shows for the add-on and keeps with each preset, so it changes with the preset. A color is kept as
// presets keep the colors of effects, such as Tint=1.000000,0.500000,0.000000.
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
    // Version 2. COLOR_RGB and COLOR_RGBA: the default, red, green, blue and alpha. Colors ignore defaultValue, min and
    // max.
    float defaultColor[4];
} UnishadeSetting;

typedef enum UnishadeFormat
{
    // Color as games draw it, in the sRGB encoding. DXGI_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm and
    // kCVPixelFormatType_32BGRA, VK_FORMAT_B8G8R8A8_UNORM and DRM_FORMAT_ARGB8888.
    UNISHADE_FORMAT_BGRA8 = 0,
    // DXGI_FORMAT_R32_FLOAT, MTLPixelFormatR32Float and kCVPixelFormatType_OneComponent32Float, VK_FORMAT_R32_SFLOAT.
    // There is no DRM format for it, so on Linux import it with Vulkan.
    UNISHADE_FORMAT_R32_FLOAT = 1,
    // Version 2: an HDR game's color, in scRGB: linear, with sRGB's primaries, and 1 is 80 nits. Values can be above 1,
    // and below 0 for colors outside sRGB's. DXGI_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float and
    // kCVPixelFormatType_64RGBAHalf, VK_FORMAT_R16G16B16A16_SFLOAT and DRM_FORMAT_ABGR16161616F.
    UNISHADE_FORMAT_RGBA16_FLOAT = 2,
} UnishadeFormat;

typedef struct UnishadeImage
{
    // Windows: an NT handle to a Direct3D 11 texture. macOS: an IOSurfaceRef. Linux: a const UnishadeDmaBuf*.
    // Unishade closes or releases it once the generation changes. NULL when the add-on gets none.
    void* handle;
    UnishadeFormat format;
} UnishadeImage;

typedef enum UnishadeStage
{
    UNISHADE_STAGE_BEFORE_EFFECTS = 0, // for UNISHADE_ADDON_WRITES_DEPTH and UNISHADE_ADDON_CHANGES_PICTURE
    UNISHADE_STAGE_AFTER_EFFECTS = 1,  // for UNISHADE_ADDON_READS_RESULT and UNISHADE_ADDON_CHANGES_RESULT
} UnishadeStage;

// Linux: a dma-buf holding an image in one plane.
typedef struct UnishadeDmaBuf
{
    uint64_t modifier; // the DRM format modifier, which says how the image is laid out
    uint64_t offset;   // of the image in the dma-buf, in bytes
    uint64_t stride;   // bytes from one row to the next
    // Unishade closes it. Importing it in Vulkan hands the descriptor over, so import a dup of it there.
    int fd;
} UnishadeDmaBuf;

typedef struct UnishadeFrame
{
    uint32_t width; // of every image
    uint32_t height;
    // Changes whenever the images and the fence are made again, such as when the game's window changes size. New
    // images hold nothing yet.
    uint64_t generation;
    // Changes when the game's picture does. With the same picture and generation as the frame before, the add-on can
    // skip its work before effects: Unishade keeps output and depth as they are. After effects it cannot, since
    // effects can change their result in every frame.
    uint64_t picture;
    // The game's picture as effects get it, or after effects their result, BGRA8, or RGBA16_FLOAT as the comment at the
    // top says. Only read it.
    UnishadeImage color;
    // UNISHADE_ADDON_CHANGES_PICTURE: the changed picture, in color's format, which effects get instead of color.
    // UNISHADE_ADDON_CHANGES_RESULT: the changed result, in color's format, which shows instead of color.
    UnishadeImage output;
    // UNISHADE_ADDON_WRITES_DEPTH: R32_FLOAT, which effects read as DEPTH. As games give ReShade depth with Unishade's
    // preprocessor definitions: 0 nearest and 1 at the far plane, not reversed, not upside down, not logarithmic, and
    // non-linear for a far plane of 1000.
    UnishadeImage depth;
    void* fence; // Windows: an NT handle to a Direct3D 11 fence. Unishade closes it. NULL elsewhere.
    uint64_t wait;   // Windows: the images are ready once the fence reaches this
    uint64_t signal; // Windows: set the fence to this once done with them
    // The settings' values, in the order UnishadeAddon lists them: one for each setting and three or four for a color,
    // so a setting's values come after all the values of the settings before it.
    const float* settings;
    // Version 2: which call this is, for an add-on that works on both.
    UnishadeStage stage;
    // Version 2: SDR white in an RGBA16_FLOAT color, in its units, such as 2.5 for 200 nits. 1 with BGRA8.
    float sdrWhite;
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
    // Version 2, or NULL: called once per pass of Unishade's main loop, whether a game runs or not, with the settings'
    // values as UnishadeFrame.settings has them. An add-on that does not work on frames, such as one that sets effects'
    // values from a controller, gets its settings and button presses here. Returns false with the reason in error to
    // stop the add-on.
    bool (*update)(void* addon, const float* settings, char* error, size_t errorSize);
} UnishadeAddon;

typedef const UnishadeAddon* (*UnishadeGetAddonFunction)(void);

#ifdef __cplusplus
}
#endif
