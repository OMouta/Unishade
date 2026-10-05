#pragma once

#include "config.h"
#include "gpu.h"
#include "image.h"
#include "preset_ini.h"

#include <effect_module.hpp>

#include <array>
#include <atomic>
#include <chrono>
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
struct Uniform
{
    std::string name;
    std::string label;
    std::string tooltip;
    std::string uiType; // slider, drag, combo, radio, list, color, input or empty
    std::string category;
    std::string items; // separated by '\0' and ending in two, as ImGui::Combo takes them
    std::string text;  // ui_text, shown above the control
    std::string source; // set by the host each frame, such as the timer, and not shown
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
    bool hidden = false;
    bool enabledByDefault = false;
    bool enabledInScreenshot = true;
    int timeout = 0;     // milliseconds the technique stays on once turned on, or 0 for as long as it is on
    float timeLeft = 0;
    // The key that turns it on and off: a virtual-key code, then Ctrl, Shift and Alt, as presets write it.
    std::array<unsigned, 4> toggleKey{};
    bool toggleKeyInPreset = false; // rather than from the effect's own toggle annotations
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
    std::vector<fs::path> includePaths;
    fs::path cacheDirectory; // empty to compile without the cache
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

    // The size of the game's picture. Effects are compiled for it, so a new size compiles them again once it
    // has stayed the same for a moment. Until then the picture is shown without effects.
    void SetSize(uint32_t width, uint32_t height);
    uint32_t Width() const { return width; }
    uint32_t Height() const { return height; }

    // Finds the effects again and compiles them on worker threads, starting at the next Update, since the frame
    // being recorded may still use the current ones. Effects the current preset uses come first.
    void Reload() { reloadRequested = true; }
    bool Loading() const { return loaderRunning || reloadRequested || resizePending; }
    // How many effect files are compiled out of how many were found.
    std::pair<size_t, size_t> LoadingProgress() const { return { loadedCount.load(), totalCount.load() }; }
    // Takes in compiled effects and prepares the ones that are on. Call once per frame on the main thread.
    void Update();

    // The game's picture: pixels in memory, or the part at (x, y) of an image on the graphics card, in
    // VK_IMAGE_LAYOUT_GENERAL. Either way it is the size given to SetSize.
    struct Source
    {
        const uint32_t* pixels = nullptr;
        VkImage image = VK_NULL_HANDLE;
        uint32_t x = 0;
        uint32_t y = 0;
        bool foreign = false; // owned by something outside Vulkan, such as the X server
    };
    // Copies the game's picture in and runs every enabled technique on it, or none when effects is false.
    void Render(VkCommandBuffer commands, const Source& source, bool effects);
    // The picture after Render, in VK_IMAGE_LAYOUT_GENERAL. BGRA.
    const GpuImage& Output() const { return backbuffer; }
    // Reads the output back, as RGBA pixels. Waits for the graphics card.
    std::vector<uint8_t> ReadOutput();
    // Reads the game's own picture from an image source, as RGBA pixels, for before and after screenshots.
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

    // A preprocessor definition's value for an effect: from the preset's section for the effect, the preset's own
    // definitions or the global ones in Unishade.ini. Empty when none sets it, so the effect uses its default.
    std::string DefinitionValue(const std::string& file, const std::string& name) const;
    // Sets a definition in the preset's section for the effect, or removes it there with an empty value, and
    // compiles the effects again. When the effect no longer compiles, the definition goes back to what it was.
    void SetDefinition(const std::string& file, const std::string& name, const std::string& value);

    std::vector<Effect>& Effects() { return effects; }
    // In the order they run.
    std::vector<Technique>& Techniques() { return techniques; }
    void SetEnabled(size_t technique, bool enabled);
    void MoveTechnique(size_t from, size_t to);

    // Values as floats or ints, converted from the variable's own type. Arrays and matrices keep ReShade's
    // 16-byte rows.
    void GetValue(const Effect& effect, const Uniform& uniform, float* values, size_t count) const;
    void GetValue(const Effect& effect, const Uniform& uniform, int* values, size_t count) const;
    void SetValue(Effect& effect, const Uniform& uniform, const float* values, size_t count);
    void SetValue(Effect& effect, const Uniform& uniform, const int* values, size_t count);
    void ResetValue(Effect& effect, const Uniform& uniform);

    // For uniforms with a source.
    bool menuOpen = false;
    float mouseX = 0;
    float mouseY = 0;
    EffectInput input;

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

    void StopLoader();
    void JoinLoaders(bool wait);
    void ReloadNow();
    void AddEffect(Effect&& effect);
    void ShareTextures(Effect& effect, size_t index);
    void SortTechniques();
    void ApplyPreset(Effect& effect, size_t effectIndex);
    // Sets or, with an empty value, removes a definition in the preset's section for the effect.
    void PutDefinition(const std::string& file, const std::string& name, const std::string& value);
    bool WritePreset(const fs::path& path, PresetIni preset);
    void Enable(Technique& technique, bool enabled);
    bool ImagesReady(Effect& effect);
    bool CreateGpu(Effect& effect);
    void PrepareEffects(std::chrono::steady_clock::duration budget);
    void DestroyGpu(Effect& effect);
    void DestroyAllGpu();
    bool CreateTargets();
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
    std::vector<uint8_t> ReadImage(VkImage image, uint32_t x, uint32_t y, bool foreign);

    Settings settings;
    uint32_t width = 0;
    uint32_t height = 0;
    // Effects are compiled for one size. When the picture changes size they wait until it has settled.
    uint32_t compiledWidth = 0;
    uint32_t compiledHeight = 0;
    bool resizePending = false;
    std::chrono::steady_clock::time_point resizeTime;

    GpuImage backbuffer; // what passes without a render target write to
    GpuImage color;      // a copy of backbuffer, which effects read as COLOR
    GpuImage depth;      // effects read DEPTH from here, which stays empty: the game's depth is out of reach
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
    bool texturesRemade = false;
    std::vector<VkFormat> mipmapWarnings;

    std::vector<Effect> effects;
    std::vector<Technique> techniques;
    std::vector<std::string> sorting; // technique keys in the order they run
    fs::path presetPath;
    PresetIni presetIni; // as loaded or last saved
    PresetDefinitions presetDefinitions;
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
    bool reloadRequested = false;
    bool summaryPending = false;
    std::atomic<size_t> loadedCount = 0;
    std::atomic<size_t> totalCount = 0;
    std::mutex finishedMutex;
    std::deque<Effect> finished;

    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastFrame = startTime;
    float frameTime = 0;
    uint32_t frameCount = 0;
};
} // namespace fx
