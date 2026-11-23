#pragma once

// Unishade add-ons: native libraries that work on a game's picture, change effects, and add settings and controls of
// their own to Unishade's menu. This header is the whole interface. It is plain C, so an add-on can be built with any
// compiler and C runtime.
//
// Writing one
//
// An add-on is a folder in the Addons folder of Unishade's data folder, which on Windows is the folder of Unishade.exe.
// The folder has an addon.ini that says what the add-on is: its Id, name, version, author, description and icon, the
// version of this interface it was built for, and its library for each system. src/addon_manifest.h describes the
// file, and src/addons/example is a whole add-on to start from. Unishade reads addon.ini without running anything, so
// it can show the add-on before the user decides whether to load it.
//
// The library is a .dll on Windows, a .dylib on macOS or a .so on Linux that exports this function, inside extern "C"
// in C++:
//
//     UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void);
//
// It returns what the add-on does and what to call. Everything else the add-on needs goes in its folder with it, which
// UnishadeHost.folder names. Unishade asks once before it loads a library that is not an official add-on's and
// remembers the answer for the add-on's Id, so a rebuilt library loads without asking again. The user turns add-ons on
// and off in Unishade.
//
// Unishade calls the add-on's functions on its main thread. Add-ons may run threads of their own, and may call
// UnishadeHost.log from them. Every other function of UnishadeHost and UnishadeUi is for the main thread only, from
// inside one of the add-on's own functions.
//
// Growing
//
// Every struct that can get more members starts with its size. Unishade fills in size for the structs it hands out, and
// the add-on for the ones it hands over, as sizeof says for the header each was built with. Members are only ever added
// at the end, so a member exists when the struct's size reaches past it: UNISHADE_HAS says so. An add-on built for an
// older version of this interface keeps working in a newer Unishade, and one that wants to run in an older Unishade
// than it was built for checks before it uses what is newer.
//
// Settings
//
// The add-on lists its settings, and Unishade shows them in its menu and keeps them: with each preset for an add-on
// that writes the picture or the result, since they are part of the preset's look, and in Unishade.ini for any other.
// Their values come with every call to frame and update, four floats for each setting, in the order the add-on lists
// them, so settings[2][0] is the third setting's value and a color's channels are settings[i][0] to [3].
//
// Controls
//
// An add-on with a ui function draws controls of its own under its settings, for what settings cannot do: text that
// changes, progress, buttons, and values the add-on keeps itself. Unishade calls ui for every frame those controls
// show, and the add-on calls UnishadeUi's functions for the controls it wants at that moment, in order, as in Dear
// ImGui: a function for a control returns true in the call after the user used it, with the new value written where
// the add-on pointed. Unishade draws them as it draws its own. A control is told apart by its label, so labels are
// different within an add-on. What follows ## in a label is not shown.
//
// Frames
//
// An add-on that asks for the picture in its flags gets every frame the overlay draws with effects on. Before effects
// run, UNISHADE_READS_PICTURE gives it the game's picture in color, UNISHADE_WRITES_PICTURE also an output to write the
// picture effects get instead, and UNISHADE_WRITES_DEPTH a depth to write, which effects read as DEPTH. After effects,
// UNISHADE_READS_RESULT gives it their result in color, before it shows and before screenshots read it, and
// UNISHADE_WRITES_RESULT also an output to write what shows instead. UNISHADE_READS_DEPTH adds the depth another
// add-on wrote to every call, where one did. An add-on that asks for both stages is called twice for the same picture,
// and UnishadeFrame.stage says which call it is.
//
// In each stage, add-ons that write depth go first, then those that only read, then those that write the picture or
// the result, each of which gets the one before's output as color.
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
// HDR
//
// A game's picture can be HDR, in RGBA16_FLOAT. An add-on with UNISHADE_TAKES_HDR gets it as it is, in color and output
// in both stages, and what it writes to output stays HDR. Any other add-on gets an SDR copy in BGRA8, as screenshots
// make it: divided by SDR white, clamped and encoded as sRGB. One that only reads or writes depth then works as it
// does with an SDR game. One that writes the picture or the result sits out while the picture is HDR, since what it
// writes could not replace an HDR picture, and runs again once it is SDR. A game can switch at any time, which makes
// the images again and changes the generation, so look at color.format on every frame.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The version of this interface, which an add-on's addon.ini gives as Api and its UnishadeAddon as api. Unishade loads
// add-ons built for this version or an older one.
#define UNISHADE_API_VERSION 1

#ifdef _WIN32
#define UNISHADE_ADDON_EXPORT __declspec(dllexport)
#else
#define UNISHADE_ADDON_EXPORT __attribute__((visibility("default")))
#endif

// Whether a struct that starts with its size has a member, as the comment at the top says under Growing, such as
// UNISHADE_HAS(UnishadeHost, host, setIntValue).
#define UNISHADE_HAS(type, pointer, member) ((pointer)->size >= offsetof(type, member) + sizeof(((type*)0)->member))

// For functions that find something by name: there is none.
#define UNISHADE_NONE 0xFFFFFFFFu

// What an add-on does, for UnishadeAddon.flags, as the comment at the top says under Frames and HDR.
#define UNISHADE_READS_PICTURE 0x1u
#define UNISHADE_WRITES_PICTURE 0x2u
#define UNISHADE_WRITES_DEPTH 0x4u
#define UNISHADE_READS_DEPTH 0x8u
#define UNISHADE_READS_RESULT 0x10u
#define UNISHADE_WRITES_RESULT 0x20u
#define UNISHADE_TAKES_HDR 0x40u
// Has Unishade read the keyboard and the mouse buttons for UnishadeHost.keyDown, while the game or the menu has them.
// Reading them takes time on every frame, so only an add-on that asks gets them.
#define UNISHADE_READS_KEYS 0x80u

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

// A technique, as UnishadeHost.getTechnique describes it.
typedef struct UnishadeTechnique
{
    uint32_t size;
    uint32_t effect;  // the effect's index, for the uniform functions
    const char* name; // as its effect declares it
    const char* file; // the effect file's name, such as "Bloom.fx"
    bool enabled;
} UnishadeTechnique;

typedef enum UnishadeValueType
{
    UNISHADE_VALUE_BOOL = 0,
    UNISHADE_VALUE_INT = 1,
    UNISHADE_VALUE_UINT = 2,
    UNISHADE_VALUE_FLOAT = 3,
} UnishadeValueType;

// One of an effect's uniforms that presets keep, as UnishadeHost.getUniform describes it.
typedef struct UnishadeUniform
{
    uint32_t size;
    UnishadeValueType type;
    const char* name; // as the effect declares it, which is its key in the preset
    // Its values: rows times columns, times the length of an array. The value functions take them in that order.
    uint32_t count;
    float min; // ui_min and ui_max, or -FLT_MAX and FLT_MAX when the effect gives none
    float max;
} UnishadeUniform;

// What Unishade is doing, as UnishadeHost.getState describes it. The strings are empty rather than NULL when there is
// nothing to say.
typedef struct UnishadeState
{
    uint32_t size;
    bool playing;           // Unishade draws over a game
    bool effectsOn;         // the user has effects on, which the power button and its shortcut turn off
    bool menuOpen;          // Unishade's menu shows over the game
    bool hdr;               // the game's picture is HDR
    uint32_t width;         // of the game's picture, or 0 without a game
    uint32_t height;
    float frameRate;        // the frames a second Unishade showed lately, or 0
    const char* gameName;   // as Unishade's launcher names the game, or its window's title
    const char* executable; // the game's program, as a path where the system says
    const char* presetName; // the preset in use, without its folder and .ini
    const char* presetPath;
    double time;            // seconds since Unishade started
} UnishadeState;

// A preset the menu lists, as UnishadeHost.getPreset describes it.
typedef struct UnishadePreset
{
    uint32_t size;
    bool current;     // the one in use
    const char* name; // without its folder and .ini
    const char* path;
} UnishadePreset;

// What Unishade tells an add-on it starts, and what the add-on can ask of it. Valid until destroy. Every function takes
// the host the add-on was given. A function that describes something returns a struct of Unishade's own, which stays
// as it is, with the strings in it, until the add-on calls that function again or returns to Unishade.
typedef struct UnishadeHost
{
    uint32_t size;
    uint32_t version;     // the UNISHADE_API_VERSION Unishade was built with
    const char* unishade; // Unishade's own version, such as "0.7.0"
    // UTF-8 and ending in a separator.
    const char* folder;     // the add-on's own folder, which it may write to
    const char* dataFolder; // Unishade's data folder, which holds Unishade.log
    // The graphics card Unishade draws with. Frames can only be shared with a device on it. Windows: its LUID, as the
    // bytes of a LUID. macOS: the registryID of its MTLDevice, as the bytes of a uint64_t. Linux: the dev_t of its DRM
    // render node, as the bytes of a uint64_t: st_rdev of its /dev/dri/renderD* file, or makedev of
    // VkPhysicalDeviceDrmPropertiesEXT's renderMajor and renderMinor. All zero when Vulkan does not say.
    uint8_t adapter[8];
    // Writes a line to Unishade.log, after the add-on's name. Safe from any thread.
    void (*log)(const struct UnishadeHost* host, UnishadeLogLevel level, const char* text);

    // Unishade

    const UnishadeState* (*getState)(const struct UnishadeHost* host);
    // A short message at the bottom of the game, for a few seconds.
    void (*showMessage)(const struct UnishadeHost* host, const char* text);
    // Turns every effect on or off, as the power button does.
    void (*setEffectsOn)(const struct UnishadeHost* host, bool on);
    // Saves a screenshot of the next frame, as the user's screenshot settings say.
    void (*takeScreenshot)(const struct UnishadeHost* host);
    // Whether a key or mouse button is held, by its Windows virtual-key code on every system, such as 0x20 for Space
    // and 0x01 for the left mouse button. Only with UNISHADE_READS_KEYS, and false while neither the game nor the
    // menu has the keyboard.
    bool (*keyDown)(const struct UnishadeHost* host, uint32_t key);
    // Sets one of the add-on's own settings, by its place in UnishadeAddon.settings, as the user does in the menu:
    // four values, of which the setting takes as many as it has. The values the add-on gets from then on have it.
    void (*setSetting)(const struct UnishadeHost* host, uint32_t setting, const float* values);

    // Presets, as the menu lists them: the game's own, then the ones for all games.

    uint32_t (*presetCount)(const struct UnishadeHost* host);
    // NULL when there is no such preset.
    const UnishadePreset* (*getPreset)(const struct UnishadeHost* host, uint32_t preset);
    // Switches to a preset, as picking it in the menu does. False when there is no such preset, or the preset in use
    // has changes the user has to save or discard first.
    bool (*usePreset)(const struct UnishadeHost* host, uint32_t preset);

    // The preset's effects. Techniques are listed in the order they run, every one that loaded, on or off. Effects,
    // techniques and uniforms are numbered from 0. The numbers from before stop being valid whenever effectsGeneration
    // changes, which it does when a preset loads, effects compile and the user moves a technique.

    uint64_t (*effectsGeneration)(const struct UnishadeHost* host);
    uint32_t (*techniqueCount)(const struct UnishadeHost* host);
    // NULL when there is no such technique.
    const UnishadeTechnique* (*getTechnique)(const struct UnishadeHost* host, uint32_t technique);
    // The technique with a name, in an effect file when file is not NULL, or UNISHADE_NONE.
    uint32_t (*findTechnique)(const struct UnishadeHost* host, const char* name, const char* file);
    // Turns a technique on or off, as the menu does: the preset then has changes to save.
    void (*setTechnique)(const struct UnishadeHost* host, uint32_t technique, bool enabled);
    // The effect an effect file made, such as "Bloom.fx", or UNISHADE_NONE.
    uint32_t (*findEffect)(const struct UnishadeHost* host, const char* file);
    // The uniforms an effect's section in the preset keeps, which leaves out those Unishade sets, such as the timer.
    uint32_t (*uniformCount)(const struct UnishadeHost* host, uint32_t effect);
    // NULL when there is no such uniform.
    const UnishadeUniform* (*getUniform)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform);
    // The uniform with a name in an effect, or UNISHADE_NONE.
    uint32_t (*findUniform)(const struct UnishadeHost* host, uint32_t effect, const char* name);
    // Writes up to count of the uniform's values to values, as floats or as ints. Returns how many it has, or 0 when
    // there is no such uniform.
    uint32_t (*getValue)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, float* values, uint32_t count);
    uint32_t (*getIntValue)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, int32_t* values, uint32_t count);
    // Sets the uniform's first count values, converted to its type, as the menu does: the preset then has changes to
    // save. min and max do not limit them. False when there is no such uniform.
    bool (*setValue)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, const float* values, uint32_t count);
    bool (*setIntValue)(const struct UnishadeHost* host, uint32_t effect, uint32_t uniform, const int32_t* values, uint32_t count);
} UnishadeHost;

typedef enum UnishadeSettingType
{
    UNISHADE_SETTING_TOGGLE = 0,     // 0 or 1
    UNISHADE_SETTING_SLIDER = 1,     // a number from min to max
    UNISHADE_SETTING_INTEGER = 2,    // a whole number from min to max
    UNISHADE_SETTING_CHOICE = 3,     // which of choices, from 0
    UNISHADE_SETTING_COLOR_RGB = 4,  // three values: red, green and blue from 0 to 1
    UNISHADE_SETTING_COLOR_RGBA = 5, // four, with alpha
    // A button with the label on it, which nothing keeps: 1 in the add-on's first frame or update call after it was
    // pressed, whichever comes first, otherwise 0.
    UNISHADE_SETTING_BUTTON = 6,
} UnishadeSettingType;

// A setting Unishade shows for the add-on and keeps, as the comment at the top says under Settings. A color is kept as
// presets keep the colors of effects, such as Tint=1.000000,0.500000,0.000000. Members left out of an initializer are
// zero, which is right for every one of them.
typedef struct UnishadeSetting
{
    const char* key; // in the add-on's section of the preset or of Unishade.ini, which is named after its Id
    const char* label;
    UnishadeSettingType type;
    // The default: the first value, or a color's red, green, blue and alpha.
    float defaultValue[4];
    float min;
    float max;
    const char* choices; // CHOICE: the labels, each ending in '\0', with one more '\0' after the last
    const char* tooltip; // or NULL
    const char* group;   // a heading the setting goes under with the others that name it, or NULL
    const char* units;   // shown after a number, such as "ms", or NULL
} UnishadeSetting;

typedef enum UnishadeFormat
{
    // Color as games draw it, in the sRGB encoding. DXGI_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm and
    // kCVPixelFormatType_32BGRA, VK_FORMAT_B8G8R8A8_UNORM and DRM_FORMAT_ARGB8888.
    UNISHADE_FORMAT_BGRA8 = 0,
    // DXGI_FORMAT_R32_FLOAT, MTLPixelFormatR32Float and kCVPixelFormatType_OneComponent32Float, VK_FORMAT_R32_SFLOAT.
    // There is no DRM format for it, so on Linux import it with Vulkan.
    UNISHADE_FORMAT_R32_FLOAT = 1,
    // An HDR game's color, in scRGB: linear, with sRGB's primaries, and 1 is 80 nits. Values can be above 1, and below
    // 0 for colors outside sRGB's. DXGI_FORMAT_R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float and
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
    UNISHADE_STAGE_BEFORE_EFFECTS = 0, // for UNISHADE_READS_PICTURE, UNISHADE_WRITES_PICTURE and UNISHADE_WRITES_DEPTH
    UNISHADE_STAGE_AFTER_EFFECTS = 1,  // for UNISHADE_READS_RESULT and UNISHADE_WRITES_RESULT
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
    uint32_t size;
    UnishadeStage stage; // which call this is, for an add-on that works on both
    uint32_t width;      // of every image
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
    // UNISHADE_WRITES_PICTURE: the changed picture, in color's format, which effects get instead of color.
    // UNISHADE_WRITES_RESULT: the changed result, in color's format, which shows instead of color.
    UnishadeImage output;
    // UNISHADE_WRITES_DEPTH, before effects: R32_FLOAT to write, which effects read as DEPTH. UNISHADE_READS_DEPTH:
    // what an add-on wrote for this picture, to read only, or nothing when none did. As games give ReShade depth with
    // Unishade's preprocessor definitions: 0 nearest and 1 at the far plane, not reversed, not upside down, not
    // logarithmic, and non-linear for a far plane of 1000.
    UnishadeImage depth;
    void* fence;     // Windows: an NT handle to a Direct3D 11 fence. Unishade closes it. NULL elsewhere.
    uint64_t wait;   // Windows: the images are ready once the fence reaches this
    uint64_t signal; // Windows: set the fence to this once done with them
    // The settings' values, four for each setting in the order UnishadeAddon lists them.
    const float (*settings)[4];
    // SDR white in an RGBA16_FLOAT color, in its units, such as 2.5 for 200 nits. 1 with BGRA8.
    float sdrWhite;
    double time; // seconds since Unishade started
} UnishadeFrame;

// What update gets, once per pass of Unishade's main loop.
typedef struct UnishadeUpdate
{
    uint32_t size;
    const float (*settings)[4]; // as UnishadeFrame.settings
    double time;                // seconds since Unishade started
} UnishadeUpdate;

typedef enum UnishadeEventType
{
    UNISHADE_EVENT_GAME_STARTED = 0,     // Unishade began drawing over a game. text: its name, as UnishadeState has it
    UNISHADE_EVENT_GAME_STOPPED = 1,     // and stopped
    UNISHADE_EVENT_PRESET_LOADED = 2,    // a preset loaded, the same one again included. text: its path
    UNISHADE_EVENT_EFFECTS_CHANGED = 3,  // UnishadeHost.effectsGeneration changed
    UNISHADE_EVENT_EFFECTS_TOGGLED = 4,  // every effect was turned on or off. value: 1 for on
    UNISHADE_EVENT_MENU_TOGGLED = 5,     // the menu over the game opened or closed. value: 1 for open
    UNISHADE_EVENT_SETTING_CHANGED = 6,  // one of the add-on's settings changed. value: its place in the list
    UNISHADE_EVENT_SCREENSHOT_SAVED = 7, // a screenshot was written. text: its file
} UnishadeEventType;

// Something that happened, which an add-on with an event function is told between frames. An add-on is told of the
// game and the preset there already are when it starts.
typedef struct UnishadeEvent
{
    uint32_t size;
    UnishadeEventType type;
    uint32_t value;
    const char* text; // valid during the call, and empty rather than NULL when the event has none
} UnishadeEvent;

// The controls an add-on draws under its settings, as the comment at the top says under Controls. Every function
// takes the ui that the add-on's ui function was given.
typedef struct UnishadeUi
{
    uint32_t size;
    // A heading over the controls that follow.
    void (*heading)(const struct UnishadeUi* ui, const char* text);
    // A paragraph of text.
    void (*text)(const struct UnishadeUi* ui, const char* text);
    // A bar filled to fraction, from 0 to 1, with a label.
    void (*progress)(const struct UnishadeUi* ui, const char* label, float fraction);
    // Returns true once after it was pressed.
    bool (*button)(const struct UnishadeUi* ui, const char* label);
    // Each of these shows the value pointed at, and returns true once after the user changed it, with the new value
    // written there.
    bool (*toggle)(const struct UnishadeUi* ui, const char* label, bool* value);
    bool (*slider)(const struct UnishadeUi* ui, const char* label, float* value, float min, float max);
    bool (*integer)(const struct UnishadeUi* ui, const char* label, int32_t* value, int32_t min, int32_t max);
    // choices as UnishadeSetting.choices.
    bool (*choice)(const struct UnishadeUi* ui, const char* label, int32_t* value, const char* choices);
    // Three values, or four with alpha.
    bool (*color)(const struct UnishadeUi* ui, const char* label, float* value, bool alpha);
    // What the control before says when the mouse rests on it.
    void (*tooltip)(const struct UnishadeUi* ui, const char* text);
} UnishadeUi;

typedef struct UnishadeAddon
{
    uint32_t size; // sizeof(UnishadeAddon)
    uint32_t api;  // UNISHADE_API_VERSION
    uint32_t flags;
    uint32_t settingCount;
    const UnishadeSetting* settings;
    uint32_t settingSize; // sizeof(UnishadeSetting)
    // Starts the add-on. Returns what the other functions are given, or NULL with the reason in error.
    void* (*create)(const UnishadeHost* host, char* error, size_t errorSize);
    // Stops the add-on once whatever it still has on the graphics card is done. Unishade may unload it afterwards.
    void (*destroy)(void* addon);
    // Works on a frame, as the comment at the top says. Returns false with the reason in error to stop the add-on.
    // NULL for an add-on that asks for no picture.
    bool (*frame)(void* addon, const UnishadeFrame* frame, char* error, size_t errorSize);
    // Or NULL: called once per pass of Unishade's main loop, whether a game runs or not. An add-on that does not work
    // on frames, such as one that sets effects' values from a controller, gets its settings and button presses here.
    // Returns false with the reason in error to stop the add-on.
    bool (*update)(void* addon, const UnishadeUpdate* update, char* error, size_t errorSize);
    // Or NULL: told what happened, as UnishadeEvent says.
    void (*event)(void* addon, const UnishadeEvent* event);
    // Or NULL: draws the add-on's own controls, as the comment at the top says under Controls.
    void (*ui)(void* addon, const UnishadeUi* ui);
} UnishadeAddon;

typedef const UnishadeAddon* (*UnishadeGetAddonFunction)(void);

#ifdef __cplusplus
}
#endif
