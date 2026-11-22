#pragma once

#include "config.h"
#include "gpu.h"
#include "image.h"
#include "preset_ini.h"

#include <effect_module.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Runs ReShade effects on the game's picture. ReShade's own compiler turns each .fx file into SPIR-V, the same
// way ReShade does for Vulkan games, and this runs the passes it describes. Presets are ReShade's .ini files,
// so they move between Windows and here unchanged.
namespace fx
{
// The values the host sets in uniforms, by their source annotation.
enum class UniformSource : uint8_t
{
    None,
    Other, // one the host cannot provide, which stays zero
    FrameTime,
    FrameCount,
    Timer,
    Date,
    Random,
    PingPong,
    Key,
    MouseButton,
    MousePoint,
    MouseDelta,
    MouseWheel,
    OverlayOpen,
    OverlayActive,
    OverlayHovered,
    Screenshot,
    DepthReady,
};

// The annotations a source reads, taken once, so setting the values every frame needs no lookups.
struct SourceSettings
{
    int randomMin = 0;
    int randomMax = 32767;
    float min = 0;     // pingpong and mousewheel
    float max = 0;
    float step[2]{};   // pingpong's range of steps, or mousewheel's step
    float smoothing = 0;
    int keycode = 0;   // key and mousebutton
    bool toggle = false;
    bool press = false;
};

struct Uniform
{
    std::string name;
    std::string label;
    std::string tooltip;
    std::string uiType; // slider, drag, combo, radio, list, color, input or empty
    std::string category;
    std::string items; // separated by '\0' and ending in two, as ImGui::Combo takes them
    std::string text;  // ui_text, shown above the control
    std::string units; // ui_units, after the value
    int spacing = 0;   // ui_spacing, lines of space above the control
    bool categoryClosed = false; // ui_category_closed: its category starts closed
    bool noReset = false;        // noreset: resetting the effect's settings leaves it alone
    std::string source; // set by the host each frame, such as the timer, and not shown
    UniformSource sourceKind = UniformSource::None;
    SourceSettings sourceSettings;
    reshadefx::type type;
    uint32_t offset = 0;
    uint32_t size = 0;
    float min = 0;
    float max = 0;
    float step = 0;
    bool hidden = false;
    size_t declaration = 0; // index into the module's uniforms, for annotations
};

struct Technique
{
    std::string name;
    std::string label;
    std::string tooltip;
    size_t effect = 0; // index into Runtime::Effects
    size_t index = 0;  // index into the effect's module
    bool enabled = false;
    // Turned off, and still one of the preset's, which lists it in TechniquesOff until it is removed.
    bool kept = false;
    bool hidden = false;
    bool enabledByDefault = false;
    bool enabledInScreenshot = true;
    int timeout = 0;     // milliseconds the technique stays on once turned on, or 0 for as long as it is on
    float timeLeft = 0;
    // The key that turns it on and off: a virtual-key code, then Ctrl, Shift and Alt, as presets write it.
    std::array<unsigned, 4> toggleKey{};
    bool toggleKeyInPreset = false; // rather than from the effect's own toggle annotations
    // Milliseconds the graphics card spends on it, smoothed, while Runtime::timeTechniques is on. 0 when it did not
    // run a few frames ago, or the graphics card has no timestamps.
    float gpuMs = 0;
};

struct EffectGpu;

// An image a texture loads from its source annotation, decoded and sized for it.
struct TextureImage
{
    image::Memory memory;
    std::vector<uint8_t> pixels; // every level after the first is made on the graphics card
    std::string error;
};

struct Effect
{
    std::filesystem::path path;
    std::string file; // the filename, which presets use as the section name
    bool compiled = false;
    bool cached = false; // taken from the compile cache
    std::string errors; // the compiler's errors and warnings
    std::vector<fs::path> files; // the effect's file and every file it included, which are watched for changes
    // The preprocessor definitions the effect checks that can be set, with the value it was compiled with, by name.
    // ReShade's menu lists the same ones.
    std::vector<std::pair<std::string, std::string>> definitions;
    reshadefx::effect_module module;
    std::unordered_map<std::string, std::vector<uint32_t>> spirv; // SPIR-V code by entry point
    std::vector<Uniform> uniforms;
    std::vector<uint8_t> uniformData;
    std::unordered_map<std::string, TextureImage> images; // by texture, decoded while the effect compiled
    std::unordered_map<std::string, std::string> textures; // the shared texture each of its textures uses
    std::shared_ptr<EffectGpu> gpu;
    bool gpuFailed = false;

    Effect();
    Effect(Effect&&) noexcept;
    Effect& operator=(Effect&&) noexcept;
    ~Effect();
};

using Definitions = std::vector<std::pair<std::string, std::string>>;

// Effect files and preset sections compare without case, as on Windows.
struct NoCase
{
    bool operator()(const std::string& a, const std::string& b) const { return Lowercase(a) < Lowercase(b); }
};

// A preset's preprocessor definitions: the ones for every effect and the ones in each effect's section.
struct PresetDefinitions
{
    Definitions global;
    std::map<std::string, Definitions, NoCase> effects;
    bool operator==(const PresetDefinitions&) const = default;
};

// What effects can read through uniform sources such as "key", "mousebutton" and "overlay_active", and what
// technique shortcuts react to. Key codes are Windows virtual-key codes, as ReShade's annotations and presets
// write them. The host fills this in every frame before Render.
struct EffectInput
{
    std::array<bool, 256> keysDown{};
    std::array<bool, 256> keysPressed{}; // went down since the previous frame
    std::array<bool, 5> buttonsDown{};   // left, right, middle, back, forward
    std::array<bool, 5> buttonsPressed{};
    float cursorDeltaX = 0; // in frame pixels since the previous frame
    float cursorDeltaY = 0;
    float wheelDelta = 0;
    bool overlayActive = false;  // a menu control is being used
    bool overlayHovered = false; // the cursor is over the menu
    // The variable whose control is being used or is under the cursor, when the menu knows it. ReShade tells an
    // effect which of its own variables that is.
    const Uniform* activeUniform = nullptr;
    const Uniform* hoveredUniform = nullptr;
    bool screenshot = false; // the frame being rendered is saved as a screenshot
};

// "Name@File.fx", as presets list techniques.
std::string TechniqueKey(const Technique& technique, const Effect& effect);

// Puts techniques in the order they run: as sorting lists them, by key or by name. The rest go by label, each
// effect's together in the order the file declares them, as in ReShade.
void OrderTechniques(std::vector<Technique>& techniques, const std::vector<Effect>& effects, const std::vector<std::string>& sorting);

// The order techniques run in, as a preset saves it while effects are still loading: order lists the techniques
// that are loaded, and the keys of previous that pending accepts, because their effect is not loaded yet, keep
// their places among them.
std::vector<std::string> MergeSorting(const std::vector<std::string>& order, const std::vector<std::string>& previous,
                                      const std::function<bool(const std::string&)>& pending);

// Where a uniform's value i is in the effect's uniform data, in ReShade's layout: every array element and every
// matrix row starts on 16 bytes.
size_t ComponentOffset(const Uniform& uniform, size_t i);
size_t ComponentCount(const Uniform& uniform);

// Compiling, which needs no graphics card.
struct CompileOptions
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vendor = 0;
    uint32_t device = 0;
    bool hdr = false; // the picture is scRGB in R16G16B16A16_SFLOAT, rather than sRGB in B8G8R8A8_UNORM
    std::vector<fs::path> includePaths;
    fs::path cacheDirectory; // empty to compile without the cache
    // Also searched. An entry found only there moves to cacheDirectory, so an effect that changes folders keeps it.
    fs::path otherCacheDirectory;
};

// The macros an effect is compiled with: ReShade's own, such as BUFFER_WIDTH, then the given definitions.
Definitions EffectMacros(const Definitions& definitions, const CompileOptions& options);
// What the compiled effect depends on: the compiler, the macros it starts with and the text of the effect and of
// every file it includes. The cache keeps an effect for as long as this stays the same.
uint64_t CompileKey(const Definitions& macros, const std::vector<std::string>& files);
// Compiles an effect into SPIR-V, or reads it from the cache. cancelled is checked between steps, so a compile
// nobody waits for anymore stops early.
bool CompileEffect(Effect& effect, const fs::path& path, const Definitions& definitions, const CompileOptions& options,
                   const std::function<bool()>& cancelled = {});

class Runtime
{
public:
    ~Runtime();

    bool Init(const Settings& settings);
    void Shutdown();

    // The size of the game's picture, and the size effects run at, smaller where they run below the game's resolution:
    // the picture is then scaled down to it as it comes in. Effects are compiled for their size, so a new one compiles
    // them again once it has stayed the same for a moment. Until the effects the preset uses have compiled for it, the
    // ones for the size before run on the picture scaled to that size.
    void SetSize(uint32_t pictureWidth, uint32_t pictureHeight, uint32_t effectsWidth, uint32_t effectsHeight);
    void SetSize(uint32_t width, uint32_t height) { SetSize(width, height, width, height); }
    // The size effects run at and Output has, which stays behind SetSize while a new size waits for its effects.
    uint32_t Width() const { return width; }
    uint32_t Height() const { return height; }
    // Whether the picture is an HDR game's, in scRGB as R16G16B16A16_SFLOAT, with SDR white at sdrWhite in scRGB's units,
    // where 1 is 80 nits. Effects are compiled for one or the other, so a change compiles them again at once, and the
    // picture shows without effects until they are in.
    void SetHdr(bool hdr, float sdrWhite);
    bool Hdr() const { return hdr; }
    float SdrWhite() const { return sdrWhite; }

    // Finds the effects again and compiles them on worker threads, starting at the next Update, since the frame
    // being recorded may still use the current ones. Effects the current preset uses come first. The rest follow
    // on fewer threads at low priority, so a game keeps its cores.
    void Reload() { reloadRequested = true; }
    // The same, keeping the changes made since the preset was loaded or saved, which Reload would drop: the effects
    // that arrive take them, and they stay unsaved. For new effects installed while the preset had changes.
    void ReloadKeepingChanges();
    // Includes the loader's final results until Update takes them and prepares the enabled effects.
    bool Loading() const { return loaderRunning || summaryPending || PresetLoading(); }
    // Every effect that was found has arrived. False until effects were looked for, which waits for the game's
    // first picture.
    bool EffectsComplete() const { return effectsComplete; }
    // The same for the effects the preset uses, which are done before the rest. Presets can be saved meanwhile:
    // techniques and sections of effects not loaded yet stay as they are.
    bool PresetLoading() const
    {
        return presetLoaderRunning || presetSummaryPending || reloadRequested || resizePending || (preparingEffects && !gpu.lost);
    }
    // How many effect files are compiled out of how many were found.
    std::pair<size_t, size_t> LoadingProgress() const { return { loadedCount.load(), totalCount.load() }; }
    // Takes in compiled effects and prepares the ones that are on. Call once per frame on the main thread.
    void Update();

    // The game's picture: pixels or rows in memory, or the part at (x, y) of an image on the graphics card, in
    // VK_IMAGE_LAYOUT_GENERAL. Either way it is the size given to SetSize.
    struct Source
    {
        const uint32_t* pixels = nullptr;
        VkImage image = VK_NULL_HANDLE;
        uint32_t x = 0;
        uint32_t y = 0;
        bool foreign = false; // owned by something outside Vulkan, such as the X server
        // Or rows in memory stride bytes apart, such as the capture's own buffer, whose alpha is made opaque as
        // they are copied, since windows without an alpha channel leave it undefined.
        const uint8_t* memory = nullptr;
        size_t stride = 0;
    };
    // Copies the game's picture in and runs every enabled technique on it, or none when effects is false.
    void Render(VkCommandBuffer commands, const Source& source, bool effects)
    {
        CopyIn(commands, source, effects);
        RenderEffects(commands, effects);
    }
    // Render in two steps, for add-ons that work on the picture in between, in commands of their own: the picture is
    // in the output after CopyIn. RenderEffects starts with nothing between it and the commands before it. With
    // effects, the parts of the picture marked as the game's interface are filled in, as uimask.h says, and
    // RenderEffects puts them back.
    void CopyIn(VkCommandBuffer commands, const Source& source, bool effects);
    void RenderEffects(VkCommandBuffer commands, bool effects);
    // Gives effects a DEPTH the size of the picture, R32_SFLOAT, which something else fills, such as an add-on, and
    // tells them through bufready_depth. Otherwise DEPTH is empty. A change prepares the effects again.
    void ProvideDepth(bool provided);
    const GpuImage& Depth() const { return depth; }
    // The picture after Render, in VK_IMAGE_LAYOUT_GENERAL. BGRA, or R16G16B16A16_SFLOAT while Hdr.
    const GpuImage& Output() const { return backbuffer; }
    // Reads the output back, as RGBA pixels, an HDR picture turned into SDR as capture does. Waits for the graphics card.
    std::vector<uint8_t> ReadOutput();
    // Reads the game's own picture at the output's size, as RGBA pixels, for before and after screenshots: from an image
    // source, or for a picture of another size, as CopyIn scaled it last. Empty for a source in memory at the output's
    // size, which the caller reads itself. An image source has the output's format.
    std::vector<uint8_t> ReadSource(const Source& source);

    // Presets. Switching compiles effects again when the preset has other preprocessor definitions.
    bool LoadPreset(const fs::path& path);
    bool SavePreset();
    // Saves the effects as they are to a new preset and switches to it.
    bool SavePresetAs(const fs::path& path);
    const fs::path& PresetPath() const { return presetPath; }
    // Changes since the preset was loaded or saved.
    bool Dirty() const { return dirty; }
    void SetDirty() { dirty = true; }
    // Techniques the preset turns on whose effect file was not found.
    std::vector<std::string> MissingTechniques() const;
    // Keys of the preset outside the effects' sections, such as add-ons' settings. Empty when the preset has none.
    // Setting one is a change to the preset, which saving writes.
    std::string PresetValue(const std::string& section, const std::string& key) const;
    void SetPresetValue(const std::string& section, const std::string& key, const std::string& value);
    // Goes up whenever a preset loads, the same one again included.
    uint64_t PresetGeneration() const { return presetGeneration; }

    // A preprocessor definition's value for an effect: from the preset's section for the effect, the preset's own
    // definitions or the global ones in Unishade.ini. Empty when none sets it, so the effect uses its default.
    std::string DefinitionValue(const std::string& file, const std::string& name) const;
    // Sets a definition in the preset's section for the effect, or removes it there with an empty value, and
    // compiles the effects again. When the effect no longer compiles, the definition goes back to what it was.
    void SetDefinition(const std::string& file, const std::string& name, const std::string& value);

    std::vector<Effect>& Effects() { return effects; }
    // In the order they run.
    std::vector<Technique>& Techniques() { return techniques; }
    // Goes up whenever effects or techniques may have moved, been added or gone, so indices into them and pointers
    // to their strings from before may be stale: when effects load, a preset loads or a technique moves.
    uint64_t EffectsGeneration() const { return effectsGeneration; }
    // A technique turned off stays one of the preset's, to be turned back on. Removing it takes it out.
    void SetEnabled(size_t technique, bool enabled);
    void RemoveTechnique(size_t technique);
    void MoveTechnique(size_t from, size_t to);

    // Values as floats or ints, converted from the variable's own type. Arrays and matrices keep ReShade's
    // 16-byte rows.
    void GetValue(const Effect& effect, const Uniform& uniform, float* values, size_t count) const;
    void GetValue(const Effect& effect, const Uniform& uniform, int* values, size_t count) const;
    void SetValue(Effect& effect, const Uniform& uniform, const float* values, size_t count);
    void SetValue(Effect& effect, const Uniform& uniform, const int* values, size_t count);
    void ResetValue(Effect& effect, const Uniform& uniform);

    // Whether anything loaded reads the keys and mouse buttons in input: a technique shortcut, which works for
    // techniques that are off too, or a key or mousebutton uniform of an effect with a technique on. Reading them
    // for the whole system takes a hundred calls or a round trip to the X server, so the host skips it otherwise.
    bool ReadsInput() const;

    // Effect files that change on disk load again by themselves, unless this is set, as while the installer writes
    // them and reloads once it is done.
    bool holdFileChanges = false;

    // For uniforms with a source.
    bool menuOpen = false;
    float mouseX = 0;
    float mouseY = 0;
    EffectInput input;
    // Times each technique on the graphics card, for Technique::gpuMs. Only while something shows it.
    bool timeTechniques = false;

private:
    // A texture effects share by name, or through the pooled annotation, as in ReShade. It is made with every
    // use any of them has for it.
    struct SharedTexture
    {
        GpuImage image;
        reshadefx::texture_desc desc;
        std::string source; // the image it loads
        bool renderTarget = false;
        bool storage = false;
        bool pooled = false;
        std::vector<size_t> users; // effects
        TextureImage loaded;       // decoded and waiting to be uploaded
        std::future<TextureImage> decoding;
    };

    // Uploads and first layouts, recorded outside the frame and submitted together.
    struct Setup
    {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::vector<GpuBuffer> buffers;
    };

    struct Loader
    {
        std::thread thread;
        std::atomic<bool> done = false;
    };

    // What the watcher looks at: the effect folders, the effect files they had when the effects loaded, and every
    // file the effects were compiled from. A file written after since may not be what was compiled.
    struct WatchList
    {
        uint64_t generation = 0; // 0 while there is nothing to watch
        std::vector<fs::path> roots;
        std::vector<fs::path> found;
        std::vector<fs::path> files;
        fs::file_time_type since;
    };

    void StopLoader();
    void JoinLoaders(bool wait);
    // With replace, the effects there are go on until the new ones the preset uses have arrived, and Update swaps
    // them in at once, with the changes made to the old ones.
    void ReloadNow(bool replace = false);
    void KeepChanges();
    void ClearEffects();
    bool FitPicture();
    bool CreateScaled();
    void WatchEffects();
    void WatchFiles();
    void StopWatcher();
    void AddEffect(Effect&& effect);
    void ShareTextures(Effect& effect, size_t index);
    void SortTechniques();
    // The order of the techniques for the preset, with those of effects not loaded yet in their places.
    std::vector<std::string> SortingKeys() const;
    void ApplyPreset(Effect& effect, size_t effectIndex);
    // Sets or, with an empty value, removes a definition in the preset's section for the effect.
    void PutDefinition(const std::string& file, const std::string& name, const std::string& value);
    bool WritePreset(const fs::path& path, PresetIni preset);
    // The preset with the effects as they are, as WritePreset saves it, in the given order.
    PresetIni ComposePreset(PresetIni preset, const std::vector<std::string>& order) const;
    void Enable(Technique& technique, bool enabled);
    bool ImagesReady(Effect& effect);
    bool CreateGpu(Effect& effect);
    void PrepareEffects(std::chrono::steady_clock::duration budget);
    void DestroyGpu(Effect& effect);
    void DestroyAllGpu();
    bool CreateTargets();
    bool CreateEmpty(GpuImage& image, uint32_t imageWidth, uint32_t imageHeight, VkFormat format);
    bool CreateDepth();
    void HandleToggleKeys();
    void UpdateSpecialUniforms(Effect& effect);
    VkSampler Sampler(const reshadefx::sampler_desc& desc);
    GpuImage* Texture(const reshadefx::texture& texture, Effect& effect);
    fs::path FindTexture(const std::string& source);
    void GenerateMipmaps(VkCommandBuffer commands, const GpuImage& image);
    VkCommandBuffer SetupCommands();
    void SubmitSetup();
    void ReleaseSetups(bool wait);
    void SavePipelineCache();
    void ReadTimestamps(uint32_t slot);
    std::vector<uint8_t> ReadImage(VkImage image, uint32_t x, uint32_t y, bool foreign);

    Settings settings;
    uint32_t width = 0;
    uint32_t height = 0;
    // Effects are compiled for one size, width by height. When the size they should have, pictureWidth by
    // pictureHeight, changes they wait until it has settled, and the picture is scaled to theirs meanwhile.
    uint32_t pictureWidth = 0;
    uint32_t pictureHeight = 0;
    // The size the picture comes in at, smaller or larger than the effects' while they run below the game's
    // resolution or wait for a new size.
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    bool resizePending = false;
    std::chrono::steady_clock::time_point resizeTime;
    // The picture at its own size while that differs from the effects', which CopyIn scales from.
    GpuImage scaled;
    GpuBuffer scaledStaging;
    bool hdr = false;
    float sdrWhite = 1;

    GpuImage backbuffer; // what passes without a render target write to
    GpuImage color;      // a copy of backbuffer, which effects read as COLOR
    GpuImage depth;      // effects read DEPTH from here, empty unless ProvideDepth: the game's depth is out of reach
    bool depthProvided = false;
    GpuImage blank;      // bound where an effect reads a texture it is writing in the same pass
    GpuImage stencil;    // the size of backbuffer, for passes that use stencil
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    GpuBuffer staging;
    GpuBuffer readback;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::map<std::string, SharedTexture> sharedTextures;
    std::vector<std::pair<std::vector<uint8_t>, VkSampler>> samplers;
    std::vector<std::pair<std::string, fs::path>> textureFiles; // lowercase relative path, full path
    bool textureFilesScanned = false;
    Setup setup;
    std::vector<Setup> setups; // submitted, until the graphics card is done with them
    std::vector<VkFormat> mipmapWarnings;

    // Timestamps before and after each technique, in a slot per frame. A slot is read when its frame comes round
    // again, by when the graphics card is done with it, so reading never waits. No pool without timestamps.
    static constexpr uint32_t kTimedFrames = 3;
    static constexpr uint32_t kTimedTechniques = 128;
    struct TimedTechnique
    {
        size_t technique = 0; // index into techniques when it ran, checked against effect and name when read
        size_t effect = 0;
        std::string name;
    };
    VkQueryPool timestamps = VK_NULL_HANDLE;
    uint64_t timestampMask = 0;
    double tickMs = 0;
    std::array<std::vector<TimedTechnique>, kTimedFrames> timedTechniques;

    std::vector<Effect> effects;
    std::vector<Technique> techniques;
    std::vector<std::string> sorting; // technique keys in the order they run
    fs::path presetPath;
    PresetIni presetIni; // as loaded or last saved
    PresetDefinitions presetDefinitions;
    // SetPresetValue's keys since the preset loaded, which saving writes into the file.
    std::vector<std::array<std::string, 3>> presetValues;
    uint64_t presetGeneration = 0;
    uint64_t effectsGeneration = 0;
    bool dirty = false;
    // The definition SetDefinition changed last, until the effects compiled with it, and the value it had in the
    // effect's section, if any.
    struct DefinitionChange
    {
        std::string file;
        std::string name;
        std::optional<std::string> previous;
    };
    std::optional<DefinitionChange> definitionChange;

    // Compiling happens on a loader thread with workers of its own. Compiled effects wait in finished until
    // Update takes them. A new load does not wait for the one before: that one stops after its current step,
    // and what it still finishes is dropped, since it belongs to an older generation.
    std::vector<std::unique_ptr<Loader>> loaders;
    std::atomic<uint64_t> generation = 0;
    std::atomic<bool> loaderRunning = false;
    std::atomic<bool> presetLoaderRunning = false; // still compiling the effects the preset uses
    bool reloadRequested = false;
    bool replacing = false; // a load with replace runs, whose effects wait in finished until the preset's have arrived
    bool preparingEffects = false;
    bool summaryPending = false;
    bool presetSummaryPending = false;
    size_t presetEffectCount = 0; // the first effects to arrive are the preset's
    // Every effect found has arrived. Until then a saved preset keeps the order of techniques not loaded yet.
    bool effectsComplete = false;
    std::atomic<size_t> loadedCount = 0;
    std::atomic<size_t> totalCount = 0;
    std::mutex finishedMutex;
    std::deque<Effect> finished;

    // A thread looks at the effect files once a second, and sets filesChanged once they changed and settled.
    fs::file_time_type loadStarted;
    std::vector<fs::path> foundEffects; // by the last load
    std::thread watcher;
    std::mutex watchMutex;
    std::condition_variable watchWake;
    bool watchStop = false;
    WatchList watching;
    uint64_t watchGeneration = 0;
    std::atomic<bool> filesChanged = false;

    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastFrame = startTime;
    float frameTime = 0;
    uint32_t frameCount = 0;
};
} // namespace fx
