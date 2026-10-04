// The host's menu. ReShade runs the reshade_overlay event every frame after the effects, so the menu is drawn
// with ReShade's own ImGui on top of the processed picture. ReShade's menu stays one click away for
// everything this one leaves out.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "menu.h"
#include "menu_layout.h"
#include "preset_ini.h"
#include "addon.h"
#include "config.h"
#include "depth/depth.h"
#include "log.h"
#include "names.h"
#include "reshade_imgui.h"
#include "resource.h"
#include "sharing.h"
#include "shell.h"
#include "state.h"
#include "text.h"
#include "theme.h"
#include "update.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace reshade::api;

namespace
{
constexpr ImU32 Color(unsigned rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

constexpr ImU32 kBackground = Color(theme::kBackground);
constexpr ImU32 kInset = Color(theme::kSidebar);
constexpr ImU32 kCard = Color(theme::kCard);
constexpr ImU32 kCardHover = Color(theme::kCardHover);
constexpr ImU32 kBorder = Color(theme::kBorder);
constexpr ImU32 kBorderStrong = Color(theme::kBorderStrong);
constexpr ImU32 kText = Color(theme::kText);
constexpr ImU32 kDim = Color(theme::kDim);
constexpr ImU32 kAccent = Color(theme::kAccent);
constexpr ImU32 kAccentHover = Color(theme::kAccentHover);
constexpr ImU32 kAccentActive = Color(theme::kAccentActive);
constexpr ImU32 kWarning = Color(theme::kWarning);
constexpr ImU32 kError = Color(theme::kError);
constexpr ImU32 kSuccess = Color(theme::kSuccess);

// Layout in pixels at 1080p.
using menu_layout::kWidth;
using menu_layout::kMargin;
using menu_layout::kHeader;
using menu_layout::kTabs;
using menu_layout::kFooter;
constexpr float kPadding = 18;
constexpr ULONGLONG kHintDuration = 6000;
constexpr ULONGLONG kToastDuration = 3000;

// The DLSS5 add-on as ReShade loads it, and the title of its window in ReShade's menu.
constexpr wchar_t kDlssModule[] = L"renodx-dlss.addon64";
constexpr char kDlssWindow[] = "RenoDX DLSS";

// RenoDX's DLSS5 add-on keeps the settings that change its look in this section of ReShade.ini, and reads them when it
// loads. Its other settings are about how it runs on that PC, so shared presets only carry these.
constexpr char kDlss5Section[] = "RENODX-DLSS-preset1";
constexpr const char* kDlss5Keys[] = {
    "DirectNeuralRenderingStyle",
    "DirectNeuralRenderingIntensity",
    "DirectNeuralRenderingLocalToneStrength",
    "DirectNeuralRenderingLocalStructureStrength",
    "DirectNeuralRenderingSkinStructureStrength",
    "DirectNeuralRenderingGlobalToneStrength",
    "DirectNeuralRenderingAutoMask",
    "DirectNeuralRenderingPassCount",
};

enum class Tab
{
    Presets,
    Browse,
    Effects,
    Settings,
    Status,
};

// The parts of the Settings tab, in the order their buttons show.
enum class SettingsPage
{
    General,
    Performance,
    Shortcuts,
};

struct Technique
{
    effect_technique handle{};
    std::string key; // name@file, like ReShade's presets, stays the same across reloads
    std::string label;
    std::string effect;
    std::string tooltip;
    std::string search; // lowercase label and file name
    bool failed = false;
};

struct Parameter
{
    effect_uniform_variable handle{};
    format base = format::unknown;
    uint32_t rows = 0;
    uint32_t columns = 0;
    uint32_t arrayLength = 0;
    std::string label;
    std::string tooltip;
    std::string category;
    std::string type;
    std::string text;
    std::string units;
    std::string items; // separated by '\0', as ImGui::Combo takes them
    float min = 0;
    float max = 0;
    float step = 0;
    int spacing = 0;
    bool categoryClosed = false;
    bool noReset = false;
};

// Decoded once with every mipmap level, largest first, so the GPU can draw it smoothly at any size.
using Pixels = std::shared_ptr<std::vector<BYTE>>;

struct Texture
{
    resource image{};
    resource_view view{};
    // The pixels it was made from. Others, or none after the device was lost, make it again.
    Pixels source;
};

// The presets folder holds presets for all games, and one level of folders in it. A folder named after a saved
// game holds that game's presets.
struct PresetFolder
{
    fs::path path;
    std::string name;
    bool all = false;
    bool game = false;
    bool playing = false;
    // Outside the presets folder, holding only the active preset.
    bool other = false;
    std::vector<fs::path> presets;
    // The folder's logo.png, such as the icon saved for a game.
    Pixels logo;
};

// A folder as the scanner last read it from disk.
struct ScannedFolder
{
    fs::path path;
    std::vector<fs::path> presets;
    Pixels logo;
};

enum class NameAction
{
    New,
    Duplicate,
    Rename,
    SaveAsNew,
    NewFolder,
};

struct MenuNotice
{
    LogLevel level;
    std::string text;
};

// ReShade writes a preset saved into its cache from its present, once more than a second has passed since.
constexpr ULONGLONG kReShadeWriteDelay = 1100;

// A preset ReShade has yet to write, and when it was saved into the cache.
struct PendingWrite
{
    fs::path preset;
    ULONGLONG since = 0;
};

// What the confirmation dialog asks about.
enum class Confirmation
{
    ResetEffect,
    ResetShortcuts,
    DeleteShared,
};

// A screenshot of a shared preset, decoded at the size the menu shows it.
struct SharedPicture
{
    bool requested = false;
    // RGBA, one level. Empty until it downloaded, and when it could not be.
    Pixels pixels;
    UINT width = 0;
    UINT height = 0;
    Texture texture;
};

// The back buffer as Unishade's swap chain holds it: BGRA rows.
struct Frame
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

enum class BrowseView
{
    List,
    Preset,
    Own,
};

// Publishing takes the picture before the effects, then the one after them, in the same frame.
enum class Capture
{
    None,
    Before,
    After,
};

// The Browse tab: presets others shared for the game being played.
struct Browse
{
    BrowseView view = BrowseView::List;

    // The game being played as the presets API knows it. Looked up again when the tab shows after a while, since
    // Roblox can move to another experience without the game changing.
    DWORD resolvedProcess = 0;
    ULONGLONG resolvedAt = 0;
    bool resolving = false;
    bool resolved = false;
    std::optional<sharing::Game> game;
    std::optional<sharing::Game> experience;
    // The Roblox place the experience came from, which publishing for the experience sends.
    std::optional<int64_t> place;
    bool allOfRoblox = false;
    bool newest = false;
    std::string effect;

    // The presets listed. Answers to requests made before the game, sorting or filter changed are dropped.
    unsigned generation = 0;
    std::vector<sharing::Preset> presets;
    std::optional<int64_t> next;
    bool loading = false;
    std::string error;
    // By "<id>/<kind>".
    std::map<std::string, SharedPicture> pictures;
    // Textures of pictures dropped this frame, which ImGui may still draw. Destroyed at the start of the next one.
    std::vector<Texture> retired;

    // The preset opened from the list, empty while it loads.
    int64_t openId = 0;
    std::optional<sharing::Preset> open;
    bool showBefore = false;
    bool useDlss5 = true;
    // A preset being tried, from a file outside the presets folder.
    int64_t tryingId = 0;
    fs::path tryingPath;

    std::vector<sharing::OwnPreset> own;
    bool ownLoading = false;

    bool accountRead = false;
    std::string token;
    std::string account;
    bool signingIn = false;
    // Discord's sign-in page, to open again when the browser tab was closed.
    std::string signInUrl;

    bool openPublishPopup = false;
    bool publishDialogOpen = false;
    bool published = false;
    char publishName[64]{};
    char publishDescription[512]{};
    bool publishForExperience = true;
    bool publishNeedsDepth = false;
    std::string publishError;
    bool publishing = false;
    std::string publishIni;
    Capture capture = Capture::None;
    ULONGLONG captureAt = 0;
    Frame before;

    bool openReportPopup = false;
    int64_t reportId = 0;
    std::string reportName;
    char reportReason[300]{};

    int64_t deleteId = 0;
    std::string deleteName;
};

// What to do with unsaved changes before switching presets.
enum class UnsavedChoice
{
    Ask,
    Save,
    Discard,
};

struct Menu
{
    effect_runtime* runtime = nullptr;
    // The runtime's device, which the logos are made on.
    reshade::api::device* device = nullptr;
    float scale = 1;
    ImGuiMouseCursor cursor = ImGuiMouseCursor_Arrow;
    Tab tab = Tab::Presets;
    // Errors caught on the way back to ReShade are logged once.
    bool errorReported = false;

    // A short message at the bottom of the screen, with an optional key before it.
    std::string toastText;
    std::string toastKey;
    ULONGLONG toastStart = 0;
    ULONGLONG toastDuration = 0;
    bool hintShown = false;

    // Shortcut requests, carried out on the next frame the overlay draws.
    bool screenshotRequested = false;
    bool beforeAfterRequested = false;
    bool beforeTaken = false;
    ULONGLONG lastScreenshot = 0;
    int presetStep = 0;
    ULONGLONG presetStepAt = 0;
    // The key that keeps effects off, while the compare shortcut is held.
    UINT compareKey = 0;

    // The effect files the active preset uses, read from it when switching to it. ReShade drops effects that are
    // not installed from a preset the next time it saves it, so the file is read before that can happen.
    fs::path effectsOf;
    std::vector<std::string> presetEffects;

    // Handles become invalid when ReShade reloads effects, so everything is read again after a reload.
    bool techniquesDirty = true;
    // ReShade lists no effects while it loads them, so the list is tried again now and then until it has some, or
    // until it is clear none loaded: ReShade finished compiling a while ago, or there are no effect files at all.
    ULONGLONG techniquesTried = 0;
    ULONGLONG compiledAt = 0;
    bool effectsEmpty = false;
    bool effectCheckRequested = false;
    std::optional<bool> effectFiles;
    std::vector<Technique> techniques;
    // Lowercase names of the effect files that loaded, hidden techniques included.
    std::set<std::string> effects;
    std::vector<size_t> byName;
    std::string expanded;
    std::string parametersEffect;
    std::vector<Parameter> parameters;
    char search[128]{};
    char presetSearch[128]{};
    bool showAll = false;
    // Keys of the effects listed under Active since the menu opened or the preset changed.
    std::set<std::string> active;
    bool presetChanged = false;
    // With auto-save off, changes wait for the save icon. ReShade saves the current preset whenever it switches
    // to another one, so a switch first asks what to do with them.
    bool unsaved = false;
    UnsavedChoice unsavedChoice = UnsavedChoice::Ask;
    bool openUnsavedPopup = false;
    bool askingUnsaved = false;
    // Set when the changes went into the preset being switched to, so the one left behind is reverted.
    bool pendingKeepsEdits = false;
    // Saved changes to the active preset that only ReShade's cache holds yet, and the preset switched away from,
    // which ReShade saves on the switch. Renaming, moving or deleting that one waits for ReShade's write, or ReShade
    // would write it back where it was.
    PendingWrite unwritten;
    PendingWrite leftBehind;
    ULONGLONG lastFrame = 0;
    // Loads the active preset again once ReShade tried to write the one left behind, which clears its error.
    bool reloadAfterWrite = false;

    // The active preset, read from ReShade at the start of every frame and after every switch.
    fs::path current;
    // As the Presets tab lists them: the game being played, all games, then every other folder with presets.
    std::vector<PresetFolder> folders;
    // Built again from the latest scan when it, the active preset or the game changes.
    bool foldersDirty = true;
    std::vector<ScannedFolder> scan;
    unsigned scanVersion = 0;
    // When the scan was asked for that the folders come from, and when one was last asked for.
    ULONGLONG scannedAt = 0;
    ULONGLONG scanRequested = 0;
    // Folders opened or closed by hand, by path.
    std::map<std::wstring, bool> folderOpen;
    // The saved game being played, by its folder's name. Empty for a window picked for this session only.
    DWORD gameProcess = 0;
    std::wstring game;
    // The game as saved, to notice when the launcher renames it.
    fs::path gameExecutable;
    fs::path gamePreset;
    // The game's preset, when switching to it waits for the changes to the preset in followFrom.
    fs::path followPreset;
    fs::path followFrom;
    // Switching presets can reload effects, so it waits until the frame is drawn.
    fs::path pendingPreset;
    bool saveNewPreset = false;
    bool openNamePopup = false;
    NameAction nameAction = NameAction::New;
    fs::path nameTarget;
    char name[128]{};
    std::string nameError;
    bool openDeletePopup = false;
    bool deleteDialogOpen = false;
    fs::path deleteTarget;
    std::string deleteError;
    // Set while the preset goes to the Recycle Bin, and when it went, to close the dialog.
    std::shared_ptr<std::atomic<RecycleResult>> deleting;
    bool deleteDone = false;

    bool comparing = false;
    bool effectsBeforeCompare = true;
    bool openReShade = false;
    bool openDlss = false;
    bool focusDlss = false;

    SettingsPage settingsPage = SettingsPage::General;
    int capturing = -1;
    std::wstring shortcutError;
    // The frame rate typed for Custom, and whether Custom was picked for a rate that another choice also gives.
    int customRate = 0;
    bool customRatePicked = false;
    // The limit these two go with. The launcher changes it too.
    int frameRate = 0;

    bool openConfirmPopup = false;
    Confirmation confirm = Confirmation::ResetEffect;
    std::string confirmEffect;

    // Read once a frame while the menu shows, and the notices only when they change.
    Update update;
    bool noticesRead = false;
    unsigned noticeVersion = 0;
    std::vector<MenuNotice> notices;
    bool problems = false;

    Pixels logoPixels;
    Texture logo;
    // By folder.
    std::map<std::wstring, Texture> folderLogos;

    Browse browse;
};
Menu m;

// The import dialog runs on a thread of its own, so the menu keeps drawing. Never destroyed, since the dialog may
// still be open when the host exits.
struct ImportDialog
{
    std::mutex mutex;
    std::vector<fs::path> files;
    std::atomic<bool> open = false;
    // The game the presets are for, and when the dialog closed.
    DWORD process = 0;
    ULONGLONG closedAt = 0;
};
ImportDialog& importDialog = *new ImportDialog;

// The presets folder is read on a thread of its own, so the frame never waits for the disk. Never destroyed, since
// the thread may still be reading when the host exits.
struct PresetScanner
{
    std::mutex mutex;
    std::condition_variable wake;
    bool started = false;
    bool requested = false;
    ULONGLONG requestedAt = 0;
    // Icons to save as logos: the game's executable, then the logo's path.
    std::vector<std::pair<fs::path, fs::path>> icons;
    // ReShade's effect search paths, to find out whether they hold any effect files.
    std::optional<std::vector<std::string>> effectPaths;
    // What the last scan found, and when the request was made that it answers.
    std::vector<ScannedFolder> folders;
    ULONGLONG scannedAt = 0;
    unsigned version = 0;
    std::optional<bool> effectFiles;
};
PresetScanner& scanner = *new PresetScanner;

// Folders are read again this often while the Presets tab shows them.
constexpr ULONGLONG kScanInterval = 2000;
// How long picked presets wait for the menu to open again after the import dialog.
constexpr ULONGLONG kImportWait = 3000;
// How often the effect list is tried while ReShade loads effects, and how long ReShade gets to create them after
// compiling before an empty list counts as no effects.
constexpr ULONGLONG kLoadRetry = 250;
constexpr ULONGLONG kCreateWait = 5000;

float S(float value)
{
    return value * m.scale;
}

std::string Lower(std::string text)
{
    for (char& character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

// Logos

// Logos are decoded once at these sizes, and the GPU scales them to the size the menu draws them.
constexpr UINT kHeaderLogoSize = 256;
constexpr UINT kFolderLogoSize = 128;

// Scales with premultiplied alpha, which keeps the transparent edge from darkening.
std::vector<BYTE> ScaledPixels(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, UINT size)
{
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> premultiplied;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> straight;
    std::vector<BYTE> pixels(size * size * 4);
    if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(factory->CreateFormatConverter(premultiplied.put())) ||
        FAILED(premultiplied->Initialize(frame.get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(premultiplied.get(), size, size, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(straight.put())) ||
        FAILED(straight->Initialize(scaler.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(straight->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return {};
    return pixels;
}

// Every mipmap level from size down to one pixel, one after the other. size is a power of two.
Pixels MipmapPixels(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, UINT size)
{
    auto pixels = std::make_shared<std::vector<BYTE>>();
    for (UINT level = size; level; level /= 2)
    {
        const std::vector<BYTE> scaled = ScaledPixels(factory, decoder, level);
        if (scaled.empty())
            return std::make_shared<std::vector<BYTE>>();
        pixels->insert(pixels->end(), scaled.begin(), scaled.end());
    }
    return pixels;
}

Pixels LogoPixels(UINT size)
{
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    const HGLOBAL resource = info ? LoadResource(nullptr, info) : nullptr;
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (!resource || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(resource)), SizeofResource(nullptr, info))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())))
        return std::make_shared<std::vector<BYTE>>();
    return MipmapPixels(factory.get(), decoder.get(), size);
}

Pixels ImagePixels(const fs::path& file, UINT size)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put())))
        return std::make_shared<std::vector<BYTE>>();
    return MipmapPixels(factory.get(), decoder.get(), size);
}

// Saves an executable's icon as a PNG.
void SaveExecutableIcon(const fs::path& executable, const fs::path& file)
{
    HICON icon = nullptr;
    if (SHDefExtractIconW(executable.c_str(), 0, 0, &icon, nullptr, 256) != S_OK)
        return;
    bool saved = false;
    {
        winrt::com_ptr<IWICImagingFactory> factory;
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::com_ptr<IWICStream> stream;
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        saved = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) &&
                SUCCEEDED(factory->CreateBitmapFromHICON(icon, bitmap.put())) && SUCCEEDED(factory->CreateStream(stream.put())) &&
                SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) &&
                SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(frame.put(), nullptr)) &&
                SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->WriteSource(bitmap.get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
                SUCCEEDED(encoder->Commit());
    }
    DestroyIcon(icon);
    std::error_code ignored;
    if (!saved)
        fs::remove(file, ignored);
}

void DestroyTexture(Texture& texture)
{
    if (m.device && texture.view.handle)
        m.device->destroy_resource_view(texture.view);
    if (m.device && texture.image.handle)
        m.device->destroy_resource(texture.image);
    texture = {};
}

// Without pixels, or when the device cannot make it, the texture stays empty until it gets other pixels.
void CreateTexture(Texture& texture, const Pixels& pixels, UINT size)
{
    DestroyTexture(texture);
    texture.source = pixels;
    if (!m.device || !pixels || pixels->empty())
        return;
    std::vector<subresource_data> levels;
    size_t offset = 0;
    for (UINT level = size; level; level /= 2)
    {
        levels.push_back({ pixels->data() + offset, level * 4, level * level * 4 });
        offset += static_cast<size_t>(level) * level * 4;
    }
    if (offset != pixels->size())
        return;
    const uint16_t count = static_cast<uint16_t>(levels.size());
    const resource_desc desc(size, size, 1, count, format::r8g8b8a8_unorm, 1, memory_heap::default_, resource_usage::shader_resource);
    if (m.device->create_resource(desc, levels.data(), resource_usage::shader_resource, &texture.image))
        m.device->create_resource_view(texture.image, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm, 0, count, 0, 1),
                                       &texture.view);
}

// A picture of any size with one level, such as a shared preset's screenshot.
void CreatePictureTexture(Texture& texture, const Pixels& pixels, UINT width, UINT height)
{
    DestroyTexture(texture);
    texture.source = pixels;
    if (!m.device || !pixels || pixels->size() != static_cast<size_t>(width) * height * 4)
        return;
    const subresource_data data{ pixels->data(), width * 4, width * height * 4 };
    const resource_desc desc(width, height, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::default_, resource_usage::shader_resource);
    if (m.device->create_resource(desc, &data, resource_usage::shader_resource, &texture.image))
        m.device->create_resource_view(texture.image, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm, 0, 1, 0, 1),
                                       &texture.view);
}

void DestroyTextures()
{
    DestroyTexture(m.logo);
    for (auto& [folder, texture] : m.folderLogos)
        DestroyTexture(texture);
    m.folderLogos.clear();
    for (auto& [key, picture] : m.browse.pictures)
        DestroyTexture(picture.texture);
    for (Texture& texture : m.browse.retired)
        DestroyTexture(texture);
    m.browse.retired.clear();
}

uint64_t HeaderLogo()
{
    if (!m.logoPixels)
        m.logoPixels = LogoPixels(kHeaderLogoSize);
    if (m.logo.source != m.logoPixels)
        CreateTexture(m.logo, m.logoPixels, kHeaderLogoSize);
    return m.logo.view.handle;
}

// A folder's logo.png, such as the icon saved for a game. 0 when it has none.
uint64_t FolderLogo(const PresetFolder& folder)
{
    if (!folder.logo || folder.logo->empty())
        return 0;
    Texture& texture = m.folderLogos[folder.path.wstring()];
    if (texture.source != folder.logo)
        CreateTexture(texture, folder.logo, kFolderLogoSize);
    return texture.view.handle;
}

// Scanning

// What the scanner knows about a file, so it only reads the file again when it changes.
struct ScannedFile
{
    fs::file_time_type time{};
    uintmax_t size = 0;
    unsigned seen = 0;
    bool preset = false;
    Pixels logo;
};
using ScanCache = std::map<std::wstring, ScannedFile>;

bool IsPreset(const fs::path& path);
bool LoadablePreset(const fs::path& path);
const fs::path& PresetsRoot();

ScannedFolder ScanFolder(const fs::path& folder, ScanCache& cache, unsigned generation)
{
    ScannedFolder scanned{ .path = folder };
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end; entry.increment(error))
    {
        std::error_code ignored;
        const fs::path& path = entry->path();
        const bool logo = _wcsicmp(path.filename().c_str(), L"logo.png") == 0;
        if (!entry->is_regular_file(ignored) || !(logo || LoadablePreset(path)))
            continue;
        // Directory listings carry the time and size, so files that did not change are not opened.
        const fs::file_time_type time = entry->last_write_time(ignored);
        const uintmax_t size = entry->file_size(ignored);
        ScannedFile& file = cache[path.native()];
        if (!file.seen || file.time != time || file.size != size)
        {
            file = { .time = time, .size = size };
            if (logo)
                file.logo = ImagePixels(path, kFolderLogoSize);
            else
                file.preset = IsPreset(path);
        }
        file.seen = generation;
        if (logo)
            scanned.logo = file.logo;
        else if (file.preset)
            scanned.presets.push_back(path);
    }
    std::sort(scanned.presets.begin(), scanned.presets.end(),
              [](const fs::path& a, const fs::path& b) { return _wcsicmp(a.stem().c_str(), b.stem().c_str()) < 0; });
    return scanned;
}

// Whether ReShade's effect search paths hold any effect files. A path ending in ** includes its folders, as in
// ReShade.
bool AnyEffectFiles(const std::vector<std::string>& searchPaths)
{
    const auto effect = [](const fs::directory_entry& entry) {
        std::error_code ignored;
        return !entry.is_directory(ignored) && (entry.path().extension() == L".fx" || entry.path().extension() == L".addonfx");
    };
    for (const std::string& searchPath : searchPaths)
    {
        fs::path path(Wide(searchPath));
        const bool recursive = path.filename() == L"**";
        if (recursive)
            path = path.parent_path();
        if (path.is_relative())
            path = fs::path(ExeDirectory()) / path;
        std::error_code error;
        if (recursive)
        {
            for (fs::recursive_directory_iterator entry(path, fs::directory_options::skip_permission_denied, error), end; !error && entry != end;
                 entry.increment(error))
                if (effect(*entry))
                    return true;
        }
        else
            for (fs::directory_iterator entry(path, error), end; !error && entry != end; entry.increment(error))
                if (effect(*entry))
                    return true;
    }
    return false;
}

// The presets folder itself, then each folder in it.
std::vector<ScannedFolder> ScanLibrary(ScanCache& cache, unsigned generation)
{
    const fs::path& root = PresetsRoot();
    std::vector<ScannedFolder> folders{ ScanFolder(root, cache, generation) };
    std::error_code error;
    for (fs::directory_iterator entry(root, error), end; !error && entry != end; entry.increment(error))
        if (std::error_code ignored; entry->is_directory(ignored))
            folders.push_back(ScanFolder(entry->path(), cache, generation));
    std::erase_if(cache, [generation](const auto& file) { return file.second.seen != generation; });
    return folders;
}

void ScanThread()
{
    // WIC and the shell's icon extraction both work in a single-threaded apartment.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ScanCache cache;
    for (unsigned generation = 1;; ++generation)
    {
        std::vector<std::pair<fs::path, fs::path>> icons;
        std::optional<std::vector<std::string>> effectPaths;
        ULONGLONG requestedAt = 0;
        {
            std::unique_lock lock(scanner.mutex);
            scanner.wake.wait(lock, [] { return scanner.requested; });
            scanner.requested = false;
            requestedAt = scanner.requestedAt;
            icons.swap(scanner.icons);
            effectPaths.swap(scanner.effectPaths);
        }
        try
        {
            for (const auto& [executable, logo] : icons)
            {
                std::error_code error;
                if (fs::exists(logo, error))
                    continue;
                fs::create_directories(logo.parent_path(), error);
                SaveExecutableIcon(executable, logo);
            }
            std::vector<ScannedFolder> folders = ScanLibrary(cache, generation);
            const std::optional<bool> effectFiles = effectPaths ? std::optional(AnyEffectFiles(*effectPaths)) : std::nullopt;
            std::lock_guard lock(scanner.mutex);
            scanner.folders = std::move(folders);
            scanner.scannedAt = requestedAt;
            if (effectFiles)
                scanner.effectFiles = effectFiles;
            ++scanner.version;
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Warning, L"Could not read the presets folder: %hs", e.what());
        }
    }
}

// Asks the scanner to read the presets folder again, and to save a game's icon first when given one.
void RequestScan(const fs::path& executable = {}, const fs::path& logo = {})
{
    {
        std::lock_guard lock(scanner.mutex);
        if (!logo.empty())
            scanner.icons.emplace_back(executable, logo);
        scanner.requested = true;
        scanner.requestedAt = GetTickCount64();
        if (!scanner.started)
        {
            std::thread(ScanThread).detach();
            scanner.started = true;
        }
    }
    scanner.wake.notify_one();
    m.scanRequested = GetTickCount64();
}

// Takes what the scanner found since the last frame.
void TakeScan()
{
    std::lock_guard lock(scanner.mutex);
    if (scanner.version == m.scanVersion)
        return;
    m.scan = std::move(scanner.folders);
    scanner.folders.clear();
    m.scanVersion = scanner.version;
    m.scannedAt = scanner.scannedAt;
    m.foldersDirty = true;
    if (scanner.effectFiles)
    {
        m.effectFiles = std::exchange(scanner.effectFiles, std::nullopt);
        m.techniquesTried = 0;
    }
}

// Asks the scanner whether ReShade's effect search paths hold any effect files.
void RequestEffectCheck()
{
    std::vector<std::string> paths;
    size_t size = 0;
    if (reshade::get_config_value(m.runtime, "GENERAL", "EffectSearchPaths", nullptr, &size) && size)
    {
        std::string value(size, '\0');
        reshade::get_config_value(m.runtime, "GENERAL", "EffectSearchPaths", value.data(), &size);
        value.resize(std::min(size, value.size()));
        // One path after another, each ended by a zero.
        for (size_t start = 0, end = 0; start < value.size(); start = end + 1)
        {
            end = std::min(value.find('\0', start), value.size());
            if (end > start)
                paths.push_back(value.substr(start, end - start));
        }
    }
    // ReShade looks beside itself when no path is set.
    if (paths.empty())
        paths.push_back(".\\");
    {
        std::lock_guard lock(scanner.mutex);
        scanner.effectPaths = std::move(paths);
    }
    m.effectCheckRequested = true;
    RequestScan();
}

// Drawing helpers

void ApplyStyle(ImGuiStyle& style)
{
    style.Alpha = 1;
    style.DisabledAlpha = 0.45f;
    style.WindowPadding = ImVec2(0, 0);
    style.WindowRounding = S(12);
    style.WindowBorderSize = 1;
    style.WindowMinSize = ImVec2(1, 1);
    style.ChildRounding = S(8);
    style.ChildBorderSize = 1;
    style.PopupRounding = S(10);
    style.PopupBorderSize = 1;
    style.FramePadding = ImVec2(S(10), S(6));
    style.FrameRounding = S(6);
    style.FrameBorderSize = 0;
    style.ItemSpacing = ImVec2(S(8), S(8));
    style.ItemInnerSpacing = ImVec2(S(8), S(6));
    style.IndentSpacing = S(16);
    style.ScrollbarSize = S(8);
    style.ScrollbarRounding = S(4);
    style.GrabMinSize = S(10);
    style.GrabRounding = S(4);
    style.TabRounding = S(6);
    // ReShade scales its own text by resolution. The menu sizes its text itself.
    style.FontScaleMain = 1;
    style.FontScaleDpi = 1;

    const auto color = [&](ImGuiCol index, ImU32 value) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(value); };
    for (ImGuiCol index = 0; index < ImGuiCol_COUNT; ++index)
        color(index, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_Text, kText);
    color(ImGuiCol_TextDisabled, kDim);
    color(ImGuiCol_WindowBg, kBackground);
    color(ImGuiCol_PopupBg, Color(0x16171D, 252));
    color(ImGuiCol_Border, kBorder);
    color(ImGuiCol_FrameBg, kCard);
    color(ImGuiCol_FrameBgHovered, kCardHover);
    color(ImGuiCol_FrameBgActive, kCardHover);
    color(ImGuiCol_ScrollbarGrab, kBorder);
    color(ImGuiCol_ScrollbarGrabHovered, kBorderStrong);
    color(ImGuiCol_ScrollbarGrabActive, kBorderStrong);
    color(ImGuiCol_CheckMark, IM_COL32_WHITE);
    color(ImGuiCol_SliderGrab, kAccent);
    color(ImGuiCol_SliderGrabActive, kAccentHover);
    color(ImGuiCol_Button, kCard);
    color(ImGuiCol_ButtonHovered, kCardHover);
    color(ImGuiCol_ButtonActive, kBorder);
    color(ImGuiCol_Header, kCard);
    color(ImGuiCol_HeaderHovered, kCardHover);
    color(ImGuiCol_HeaderActive, kBorder);
    color(ImGuiCol_Separator, kBorder);
    color(ImGuiCol_TextLink, kAccentHover);
    color(ImGuiCol_TextSelectedBg, Color(theme::kAccent, 90));
    color(ImGuiCol_InputTextCursor, kText);
    color(ImGuiCol_DragDropTarget, kAccent);
    color(ImGuiCol_NavCursor, kAccent);
    color(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 120));
}

void PushSize(float size)
{
    ImGui::PushFont(nullptr, S(size));
}

// Wraps at the edge of the window, at wrap in window coordinates when given, or not at all when wrap is negative.
void Text(const std::string& text, ImU32 color = kText, float size = 14.5f, float wrap = 0.0f)
{
    PushSize(size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Heading(const char* text)
{
    ImGui::Dummy(ImVec2(0, S(2)));
    Text(text, kDim, 12);
}

void HandOnHover()
{
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

bool Button(const char* label, ImVec2 size, bool primary = false, bool enabled = true)
{
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? kAccent : kCard);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? kAccentHover : kCardHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? kAccentActive : kBorder);
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? IM_COL32_WHITE : kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, primary ? 0.0f : 1.0f);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    if (enabled)
        HandOnHover();
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

bool Link(const char* label, ImU32 color = kAccentHover)
{
    ImGui::PushStyleColor(ImGuiCol_TextLink, color);
    const bool clicked = ImGui::TextLink(label);
    HandOnHover();
    ImGui::PopStyleColor();
    return clicked;
}

// An on/off switch. Returns true when clicked.
bool Switch(const char* id, bool on)
{
    const ImVec2 size(S(32), S(18));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, position + size, on ? (hovered ? kAccentHover : kAccent) : (hovered ? kBorderStrong : kBorder), size.y / 2);
    const float x = on ? position.x + size.x - size.y / 2 : position.x + size.y / 2;
    draw->AddCircleFilled(ImVec2(x, position.y + size.y / 2), size.y / 2 - S(3), on ? IM_COL32_WHITE : kDim);
    return clicked;
}

// Choices side by side in one control, each as wide as the others, with the selected one filled. Returns the index
// of the choice clicked, or -1.
int Segmented(const char* id, std::initializer_list<const char*> choices, int selected, ImU32 fill)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 whole(ImGui::GetContentRegionAvail().x, S(30));
    const ImVec2 size(whole.x / static_cast<float>(choices.size()), whole.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + whole, kInset, S(7));
    draw->AddRect(start, start + whole, kBorder, S(7));
    int clicked = -1;
    int index = 0;
    ImGui::PushID(id);
    PushSize(13.5f);
    for (const char* choice : choices)
    {
        const ImVec2 min = start + ImVec2(size.x * index, 0);
        ImGui::SetCursorScreenPos(min);
        if (ImGui::InvisibleButton(choice, size, ImGuiButtonFlags_EnableNav))
            clicked = index;
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        if (index == selected)
            draw->AddRectFilled(min + ImVec2(S(3), S(3)), min + size - ImVec2(S(3), S(3)), fill, S(5));
        draw->AddText(min + (size - ImGui::CalcTextSize(choice)) * 0.5f, index == selected ? IM_COL32_WHITE : hovered ? kText : kDim, choice);
        ++index;
    }
    ImGui::PopFont();
    ImGui::PopID();
    return clicked;
}

void KeyCap(const std::string& key, float size = 13)
{
    PushSize(size);
    const ImVec2 text = ImGui::CalcTextSize(key.c_str());
    const ImVec2 box(std::max(text.x + S(14), S(28)), text.y + S(6));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, position + box, kCard, S(5));
    draw->AddRect(position, position + box, kBorderStrong, S(5));
    draw->AddText(position + (box - text) * 0.5f, kText, key.c_str());
    ImGui::Dummy(box);
    ImGui::PopFont();
}

void Spinner(float radius)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(radius, radius);
    const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(center, radius - S(2), start, start + 4.2f, 32);
    draw->PathStroke(kAccent, 0, S(2.5f));
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

// A floppy disk: an outline with one corner cut, the shutter at the top and the label below.
void SaveIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 min = center - ImVec2(size, size) * 0.5f;
    const ImVec2 max = center + ImVec2(size, size) * 0.5f;
    const float corner = size * 0.25f;
    const ImVec2 outline[] = { min, ImVec2(max.x - corner, min.y), ImVec2(max.x, min.y + corner), max, ImVec2(min.x, max.y) };
    draw->AddPolyline(outline, 5, color, ImDrawFlags_Closed, S(1.5f));
    draw->AddRectFilled(ImVec2(min.x + size * 0.25f, min.y), ImVec2(max.x - size * 0.35f, min.y + size * 0.3f), color);
    draw->AddRect(ImVec2(min.x + size * 0.2f, center.y + size * 0.1f), ImVec2(max.x - size * 0.2f, max.y), color, 0, 0, S(1.5f));
}

// A camera: the body, the bump on top and the lens.
void CameraIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 min = center + ImVec2(-size * 0.5f, -size * 0.3f);
    const ImVec2 max = center + ImVec2(size * 0.5f, size * 0.4f);
    draw->AddRect(min, max, color, S(2), 0, S(1.5f));
    draw->AddRectFilled(ImVec2(center.x - size * 0.2f, min.y - size * 0.15f), ImVec2(center.x + size * 0.2f, min.y), color, S(1));
    draw->AddCircle(ImVec2(center.x, center.y + size * 0.05f), size * 0.2f, color, 0, S(1.5f));
}

// Points down when open and right when closed.
void Chevron(ImDrawList* draw, ImVec2 center, bool open, ImU32 color)
{
    const float r = S(3.5f);
    if (open)
    {
        const ImVec2 points[] = { center + ImVec2(-r, -r / 2), center + ImVec2(0, r / 2), center + ImVec2(r, -r / 2) };
        draw->AddPolyline(points, 3, color, 0, S(1.6f));
    }
    else
    {
        const ImVec2 points[] = { center + ImVec2(-r / 2, -r), center + ImVec2(r / 2, 0), center + ImVec2(-r / 2, r) };
        draw->AddPolyline(points, 3, color, 0, S(1.6f));
    }
}

void Rainbow(ImDrawList* draw, ImVec2 min, ImVec2 max)
{
    constexpr int count = static_cast<int>(std::size(theme::kRainbow));
    for (int i = 0; i + 1 < count; ++i)
    {
        const float left = min.x + (max.x - min.x) * i / (count - 1);
        const float right = min.x + (max.x - min.x) * (i + 1) / (count - 1);
        const ImU32 from = Color(theme::kRainbow[i]);
        const ImU32 to = Color(theme::kRainbow[i + 1]);
        draw->AddRectFilledMultiColor(ImVec2(left, min.y), ImVec2(right, max.y), from, to, to, from);
    }
}

// A check, exclamation mark or cross in a tinted circle, for the status list.
void NoticeIcon(ImDrawList* draw, ImVec2 center, LogLevel level)
{
    const float r = S(8);
    const ImU32 color = level == LogLevel::Ok ? kSuccess : level == LogLevel::Warning ? kWarning : level == LogLevel::Error ? kError : kDim;
    draw->AddCircleFilled(center, r, (color & 0x00FFFFFF) | 0x30000000);
    const float t = S(1.6f);
    switch (level)
    {
    case LogLevel::Ok:
    {
        const ImVec2 points[] = { center + ImVec2(-S(3.5f), 0), center + ImVec2(-S(1), S(2.8f)), center + ImVec2(S(3.8f), -S(2.8f)) };
        draw->AddPolyline(points, 3, color, 0, t);
        break;
    }
    case LogLevel::Warning:
        draw->AddLine(center + ImVec2(0, -S(4)), center + ImVec2(0, S(1)), color, t);
        draw->AddCircleFilled(center + ImVec2(0, S(3.8f)), S(1.1f), color);
        break;
    case LogLevel::Error:
        draw->AddLine(center + ImVec2(-S(3), -S(3)), center + ImVec2(S(3), S(3)), color, t);
        draw->AddLine(center + ImVec2(S(3), -S(3)), center + ImVec2(-S(3), S(3)), color, t);
        break;
    default:
        draw->AddCircleFilled(center, S(2.2f), color);
        break;
    }
}

// Notices are copied from the log only when they change.
void UpdateNotices()
{
    const unsigned version = NoticeVersion();
    if (m.noticesRead && version == m.noticeVersion)
        return;
    m.noticesRead = true;
    m.noticeVersion = version;
    m.notices.clear();
    for (const Notice& notice : Notices())
        m.notices.push_back({ notice.level, Utf8(notice.text) });
    m.problems = std::any_of(m.notices.begin(), m.notices.end(), [](const MenuNotice& notice) { return notice.level >= LogLevel::Warning; });
}

void ShowToast(std::string text, std::string key = {}, ULONGLONG duration = kToastDuration)
{
    m.toastText = std::move(text);
    m.toastKey = std::move(key);
    m.toastStart = GetTickCount64();
    m.toastDuration = duration;
}

// Where ReShade saves screenshots, which can be relative to the host's folder.
fs::path ScreenshotFolder()
{
    char value[2048] = "";
    size_t size = sizeof(value);
    fs::path folder = ExeDirectory();
    if (reshade::get_config_value(m.runtime, "SCREENSHOT", "SavePath", value, &size) && value[0])
        folder /= fs::path(Wide(value));
    return folder.lexically_normal();
}

// Presets

fs::path CurrentPreset()
{
    size_t size = 0;
    m.runtime->get_current_preset_path(nullptr, &size);
    std::string path(size, '\0');
    m.runtime->get_current_preset_path(path.data(), &size);
    path.resize(size);
    return fs::path(Wide(path)).lexically_normal();
}

bool SamePath(const fs::path& a, const fs::path& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// The presets folder beside the exe, where Unishade keeps its presets and creates folders, logos and new presets.
// Spelled the way ReShade spells the paths it loads, which resolves links.
const fs::path& PresetsRoot()
{
    static const fs::path root = [] {
        const fs::path path = (fs::path(ExeDirectory()) / L"presets").lexically_normal();
        std::error_code error;
        fs::path resolved = fs::canonical(path, error);
        return error ? path : resolved;
    }();
    return root;
}

// Whether a preset is in the presets folder or one of its folders, the ones the Presets tab lists.
bool InPresets(const fs::path& preset)
{
    const fs::path folder = preset.parent_path();
    return SamePath(folder, PresetsRoot()) || SamePath(folder.parent_path(), PresetsRoot());
}

// The preset's path in the presets folder, such as Roblox\Warm.ini, or empty when it is somewhere else.
std::wstring LibraryPath(const fs::path& preset)
{
    const std::wstring& root = PresetsRoot().native();
    const std::wstring& path = preset.native();
    if (path.size() <= root.size() + 1 || _wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0 || path[root.size()] != L'\\')
        return {};
    return path.substr(root.size() + 1);
}

// Names

// Why a name typed for a preset or folder cannot be used, or empty when it can. Trims spaces from both ends first.
std::string NameProblem(std::wstring& name)
{
    name.erase(0, name.find_first_not_of(L' '));
    name.erase(name.find_last_not_of(L' ') + 1);
    switch (CheckName(name, false))
    {
    case NameIssue::None:
        break;
    case NameIssue::Empty:
        return "Enter a name.";
    case NameIssue::Control:
    case NameIssue::Character:
        return "A name cannot contain \\ / : * ? \" < > | or control characters.";
    case NameIssue::End:
        return "A name cannot end with a dot.";
    case NameIssue::Device:
        return "Windows keeps " + Utf8(name) + " for devices. Pick another name.";
    }
    return {};
}

// Copies UTF-8 text into a fixed buffer, cut between characters when it does not fit.
template <size_t N>
void CopyText(char (&buffer)[N], const std::string& text)
{
    size_t length = std::min(text.size(), N - 1);
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        --length;
    text.copy(buffer, length);
    buffer[length] = '\0';
}

// Where new and imported presets go: the folder of the game being played, or the presets folder without one.
fs::path NewPresetFolder()
{
    return m.game.empty() ? PresetsRoot() : PresetsRoot() / m.game;
}

// The preset a game was last played with. Empty when there is none, or when the setting points outside the presets
// folder, which only editing the file by hand can do.
fs::path RememberedPreset(const std::wstring& game)
{
    const fs::path relative = fs::path(GamePreset(game)).lexically_normal();
    if (relative.empty() || relative.has_root_path() || relative.extension() != L".ini" ||
        std::any_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == L".."; }))
        return {};
    return PresetsRoot() / relative;
}

// Points the games that remember a preset at where it went, or forgets it for them when it is gone.
void UpdateGamePresets(const fs::path& from, const fs::path& to)
{
    const std::wstring moved = LibraryPath(to);
    std::set<std::wstring> games;
    for (const AutoGame& game : g.autoGames)
    {
        const std::wstring name = FolderName(game.name);
        if (name.empty() || !games.insert(name).second || !SamePath(RememberedPreset(name), from))
            continue;
        if (moved.empty())
            RemoveGamePreset(name);
        else
            SetGamePreset(name, moved);
    }
}

// Saves the runtime's values into ReShade's cache of the active preset.
void SaveToCache()
{
    m.runtime->save_current_preset();
    m.unwritten = { m.current, GetTickCount64() };
}

void SavePreset()
{
    SaveToCache();
    m.presetChanged = false;
    m.unsaved = false;
}

// Whether ReShade has effects loaded. It lists none while loading them, when a saved preset would lose them.
bool EffectsLoaded()
{
    bool any = false;
    m.runtime->enumerate_techniques(nullptr, [&any](effect_runtime*, effect_technique) { any = true; });
    return any;
}

// Writes the active preset to disk now instead of from a later present. ReShade only writes at once when exporting
// to a file outside its cache, so the preset is exported to another file and moved over it. Only done while the
// values on screen are the saved ones.
void WriteActivePreset()
{
    if (m.unwritten.preset.empty() || !SamePath(m.unwritten.preset, m.current) || m.unsaved || m.presetChanged || !EffectsLoaded())
        return;
    const fs::path& preset = m.current;
    std::error_code error;
    // Beside the preset when it is in the presets folder, so the move replaces it in one step. Nothing is created
    // next to a preset elsewhere.
    fs::path temporary = InPresets(preset) ? preset.parent_path() : fs::temp_directory_path(error);
    temporary /= preset.filename();
    temporary += L".unishade";
    // Starting from the file keeps what ReShade only writes for effects that are on, like settings of others.
    if (!error && fs::exists(preset, error))
        fs::copy_file(preset, temporary, fs::copy_options::overwrite_existing, error);
    else if (!error)
        fs::remove(temporary, error);
    if (error)
        return;
    m.runtime->export_current_preset(Utf8(temporary.wstring()).c_str());
    fs::rename(temporary, preset, error);
    // Moving fails from a temporary folder on another drive, so the file is copied there instead.
    if (error)
    {
        error.clear();
        fs::copy_file(temporary, preset, fs::copy_options::overwrite_existing, error);
        std::error_code ignored;
        fs::remove(temporary, ignored);
    }
    if (error)
        return;
    m.unwritten = {};
    // ReShade still writes the same values from its cache later. Saving into the cache again dates them after this
    // file, which ReShade would otherwise take for a change made elsewhere and report that it could not save.
    m.runtime->save_current_preset();
}

// Whether ReShade has yet to write a preset that is not the active one.
bool WritePending(const fs::path& preset)
{
    return SamePath(m.leftBehind.preset, preset) || (SamePath(m.unwritten.preset, preset) && !SamePath(preset, m.current));
}

// Switches ReShade to a preset. Returns false when ReShade did not, such as for a file that is not a preset.
bool SetPreset(const fs::path& preset)
{
    // ReShade saves the preset it leaves into its cache. Writing it first keeps the file right meanwhile.
    WriteActivePreset();
    const fs::path left = m.current;
    m.runtime->set_current_preset_path(Utf8(preset.wstring()).c_str());
    m.current = CurrentPreset();
    m.foldersDirty = true;
    if (!SamePath(m.current, left))
    {
        m.leftBehind = { left, GetTickCount64() };
        // ReShade cannot write the preset it left when its folder is gone, such as after the launcher renamed the
        // game, and reports that until it loads a preset again.
        if (std::error_code error; !left.empty() && !fs::is_directory(left.parent_path(), error))
            m.reloadAfterWrite = true;
    }
    return SamePath(m.current, preset);
}

// Loads the active preset again as it was last saved. ReShade only saves the preset it leaves when switching to
// a different one, so switching to the same one discards the changes.
void DiscardChanges()
{
    SetPreset(m.current);
    m.presetChanged = false;
    m.unsaved = false;
    m.active.clear();
}

std::string ReadText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

bool IsPreset(const fs::path& path)
{
    std::string value;
    return PresetIni(ReadText(path)).Get("", "Techniques", value);
}

// ReShade only loads presets whose extension is exactly .ini, so X.INI is left out.
bool LoadablePreset(const fs::path& path)
{
    return path.extension() == L".ini";
}

void ReadPresetEffects(const fs::path& path)
{
    m.effectsOf = path;
    m.presetEffects = PresetEffectFiles(PresetIni(ReadText(path)));
}

// Effects the active preset uses that did not load, once ReShade has loaded effects.
std::vector<std::string> MissingEffects(const fs::path& current)
{
    std::vector<std::string> missing;
    if (m.techniquesDirty || !SamePath(m.effectsOf, current))
        return missing;
    for (const std::string& effect : m.presetEffects)
        if (!m.effects.count(Lower(effect)))
            missing.push_back(effect);
    return missing;
}

enum class SwitchResult
{
    Switched,
    Unsaved,
    Failed,
};

// Switches presets right away, for shortcuts, which cannot ask about unsaved changes.
SwitchResult SwitchNow(const fs::path& target)
{
    if (m.unsaved || (m.presetChanged && !AutoSavePresets()))
        return SwitchResult::Unsaved;
    // ReShade would take a preset deleted since the last scan for a new one.
    if (std::error_code error; !fs::exists(target, error))
        return SwitchResult::Failed;
    if (m.presetChanged)
        SavePreset();
    ReadPresetEffects(target);
    const bool switched = SetPreset(target);
    m.active.clear();
    return switched ? SwitchResult::Switched : SwitchResult::Failed;
}

// Whether the launcher renamed or removed the saved game being played since the menu followed it.
bool GameRenamed()
{
    if (m.game.empty())
        return false;
    const auto game = std::find_if(g.autoGames.begin(), g.autoGames.end(), [](const AutoGame& saved) { return saved.executable == m.gameExecutable; });
    return game == g.autoGames.end() || FolderName(game->name) != m.game;
}

// Renaming a game in the launcher renames its folder. When the active preset was in it, ReShade moves to the same
// preset in the new folder, so it does not write the preset back to the old one.
void FollowRenamedFolder(const std::wstring& previous)
{
    if (!SamePath(m.current.parent_path(), PresetsRoot() / previous))
        return;
    const fs::path moved = PresetsRoot() / m.game / m.current.filename();
    std::error_code error;
    if (SamePath(moved, m.current) || !fs::exists(moved, error))
        return;
    // With auto-save on, the values on screen are the saved ones, including any ReShade had yet to write. Unsaved
    // changes cannot come along, since switching loads the preset from its file.
    if (AutoSavePresets() && EffectsLoaded())
        m.runtime->export_current_preset(Utf8(moved.wstring()).c_str());
    else if (m.unsaved || m.presetChanged)
        ShowToast("Unsaved changes to " + Utf8(moved.stem().wstring()) + " were dropped, since its game was renamed");
    m.unsaved = false;
    m.presetChanged = false;
    m.unwritten = {};
    m.active.clear();
    ReadPresetEffects(moved);
    SetPreset(moved);
}

// Switching to a game switches to the preset last used in it, and switching presets while playing a saved game
// is remembered for it. Only presets in the presets folder are remembered.
void FollowGame()
{
    const fs::path current = m.current;
    const DWORD process = g.activeGame ? g.activeGame->processId : 0;
    const bool renamed = process == m.gameProcess && GameRenamed();
    if (process == m.gameProcess && !renamed)
    {
        if (!m.game.empty() && !SamePath(current, m.gamePreset))
        {
            m.gamePreset = current;
            if (const std::wstring relative = LibraryPath(current); !relative.empty())
                SetGamePreset(m.game, relative);
        }
        // The game's preset follows once the changes that held it up are saved, unless another preset was picked.
        if (!m.followPreset.empty() && !SamePath(current, m.followFrom))
            m.followPreset.clear();
        else if (!m.followPreset.empty() && !m.unsaved && !(m.presetChanged && !AutoSavePresets()))
        {
            const fs::path preset = std::exchange(m.followPreset, {});
            if (SwitchNow(preset) == SwitchResult::Switched)
                ShowToast(Utf8(preset.stem().wstring()));
        }
        return;
    }

    // A renamed game is followed again under its new name, in its renamed folder.
    const std::wstring renamedFrom = renamed ? m.game : std::wstring();
    m.gameProcess = process;
    m.game.clear();
    m.gameExecutable.clear();
    m.followPreset.clear();
    m.foldersDirty = true;
    fs::path executable;
    try
    {
        executable = ProcessExecutable(process);
    }
    catch (const std::system_error&)
    {
        return;
    }
    for (const AutoGame& game : g.autoGames)
        if (MatchesExecutable(game, executable))
        {
            m.game = FolderName(game.name);
            m.gameExecutable = game.executable;
            break;
        }
    if (m.game.empty())
        return;
    if (!renamedFrom.empty())
        FollowRenamedFolder(renamedFrom);

    // Games saved by filename, like Roblox, move with every update, so their icon is kept while they run.
    RequestScan(executable, PresetsRoot() / m.game / L"logo.png");

    const fs::path preset = RememberedPreset(m.game);
    const fs::path active = m.current;
    if (preset.empty())
    {
        if (const std::wstring relative = LibraryPath(active); !relative.empty())
            SetGamePreset(m.game, relative);
    }
    else if (std::error_code error; !SamePath(preset, active) && fs::exists(preset, error) && SwitchNow(preset) == SwitchResult::Unsaved)
    {
        ShowToast("Save or discard the changes to " + Utf8(active.stem().wstring()) + " to switch to " + Utf8(preset.stem().wstring()));
        m.followPreset = preset;
        m.followFrom = active;
    }
    m.gamePreset = m.current;
}

// Drops a preset that was renamed, moved or deleted from the list until the next scan, so it cannot be clicked.
void ForgetPreset(const fs::path& preset)
{
    for (ScannedFolder& folder : m.scan)
        std::erase_if(folder.presets, [&](const fs::path& listed) { return SamePath(listed, preset); });
    m.foldersDirty = true;
}

// Moves a preset into a folder, creating it. Returns what went wrong, if anything.
std::string MovePreset(const fs::path& preset, const fs::path& folder)
{
    if (WritePending(preset))
        return "ReShade is still saving " + Utf8(preset.stem().wstring()) + ". Try again in a moment.";
    const fs::path target = folder / preset.filename();
    std::error_code error;
    if (fs::exists(target, error))
        return "There is already a preset named " + Utf8(preset.stem().wstring()) + " there.";
    fs::create_directories(folder, error);
    if (!error)
        fs::rename(preset, target, error);
    if (error)
        return "Windows could not move " + Utf8(preset.stem().wstring()) + ".";
    UpdateGamePresets(preset, target);
    ForgetPreset(preset);
    RequestScan();
    return {};
}

// Copies presets picked in the import dialog into the folder for new presets and switches to the first, asking
// about unsaved changes like any other switch.
void ImportPresets(const std::vector<fs::path>& files)
{
    const fs::path& current = m.current;
    const fs::path folder = NewPresetFolder();
    std::error_code error;
    fs::create_directories(folder, error);
    for (const fs::path& file : files)
    {
        const std::string name = Utf8(file.filename().wstring());
        if (!IsPreset(file))
        {
            ShowToast(name + " is not a ReShade preset");
            continue;
        }
        fs::path target = file;
        // A preset picked from the presets folder is already there, unless ReShade cannot load it as it is named.
        if (!InPresets(file) || !LoadablePreset(file))
        {
            target = folder / (file.stem().wstring() + L".ini");
            for (int copy = 2; fs::exists(target, error); ++copy)
                target = folder / (file.stem().wstring() + L" (" + std::to_wstring(copy) + L").ini");
            if (!fs::copy_file(file, target, error))
            {
                ShowToast("Windows could not copy " + name);
                continue;
            }
        }
        if (m.pendingPreset.empty() && !SamePath(target, current))
            m.pendingPreset = target;
    }
    m.tab = Tab::Presets;
    RequestScan();
}

std::vector<fs::path> PickPresets()
{
    std::vector<fs::path> files;
    winrt::com_ptr<IFileOpenDialog> dialog;
    winrt::com_ptr<IShellItemArray> items;
    FILEOPENDIALOGOPTIONS options = 0;
    const COMDLG_FILTERSPEC types[] = { { L"ReShade presets", L"*.ini" } };
    DWORD count = 0;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) ||
        FAILED(dialog->GetOptions(&options)) ||
        FAILED(dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST)) ||
        FAILED(dialog->SetFileTypes(1, types)) || FAILED(dialog->SetTitle(L"Import presets")) || FAILED(dialog->Show(nullptr)) ||
        FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count)))
        return files;
    for (DWORD i = 0; i < count; ++i)
    {
        winrt::com_ptr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            files.emplace_back(path);
        CoTaskMemFree(path);
    }
    return files;
}

// The dialog takes focus from the menu, which closes it, so the menu opens again when presets were picked.
void OpenImportDialog()
{
    if (importDialog.open.exchange(true))
        return;
    {
        std::lock_guard lock(importDialog.mutex);
        importDialog.process = m.gameProcess;
    }
    std::thread([] {
        // The main thread's apartment is multithreaded, and the shell's dialogs need a single-threaded one.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::vector<fs::path> files = PickPresets();
        if (SUCCEEDED(com))
            CoUninitialize();
        {
            std::lock_guard lock(importDialog.mutex);
            importDialog.files.insert(importDialog.files.end(), files.begin(), files.end());
            importDialog.closedAt = GetTickCount64();
        }
        importDialog.open = false;
        if (!files.empty())
            PostMessageW(g.overlay, kOpenMenuMessage, 0, 0);
    }).detach();
}

// Imports picked presets once the menu is open again. They are dropped when the game they were picked for is gone,
// or when the menu could not open for them, so they never end up in another game's folder later.
void TakeImports(bool menu)
{
    std::vector<fs::path> files;
    bool sameGame = false;
    {
        std::lock_guard lock(importDialog.mutex);
        if (importDialog.files.empty())
            return;
        sameGame = importDialog.process == m.gameProcess;
        if (sameGame && !menu && GetTickCount64() - importDialog.closedAt < kImportWait)
            return;
        files.swap(importDialog.files);
    }
    if (sameGame && menu)
        ImportPresets(files);
    else
    {
        const char* reason = sameGame ? "the menu could not open" : "the game changed";
        Log(LogLevel::Info, L"Did not import %zu presets, since %hs.", files.size(), reason);
        ShowToast(std::string("The presets were not imported, since ") + reason);
    }
}

// Lists the latest scan the way the Presets tab shows it.
void BuildFolders()
{
    m.foldersDirty = false;
    const fs::path& root = PresetsRoot();
    const fs::path& current = m.current;
    // The game being played is listed even before it has presets, since new ones go there.
    PresetFolder playing{ .path = root / m.game, .name = Utf8(m.game), .game = true, .playing = true };
    PresetFolder all{ .path = root, .name = "All games", .all = true };
    std::vector<PresetFolder> others;
    for (const ScannedFolder& scanned : m.scan)
    {
        if (SamePath(scanned.path, root))
        {
            all.presets = scanned.presets;
            all.logo = scanned.logo;
            continue;
        }
        const std::wstring name = scanned.path.filename().wstring();
        if (!m.game.empty() && _wcsicmp(name.c_str(), m.game.c_str()) == 0)
        {
            playing.path = scanned.path;
            playing.name = Utf8(name);
            playing.presets = scanned.presets;
            playing.logo = scanned.logo;
            continue;
        }
        if (scanned.presets.empty())
            continue;
        PresetFolder folder{ .path = scanned.path, .name = Utf8(name), .presets = scanned.presets, .logo = scanned.logo };
        folder.game = std::any_of(g.autoGames.begin(), g.autoGames.end(),
                                  [&](const AutoGame& game) { return _wcsicmp(FolderName(game.name).c_str(), name.c_str()) == 0; });
        others.push_back(std::move(folder));
    }
    std::sort(others.begin(), others.end(), [](const PresetFolder& a, const PresetFolder& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });

    m.folders.clear();
    if (!m.game.empty())
        m.folders.push_back(std::move(playing));
    m.folders.push_back(std::move(all));
    std::move(others.begin(), others.end(), std::back_inserter(m.folders));
    const auto listed = [&](const PresetFolder& folder) {
        return std::any_of(folder.presets.begin(), folder.presets.end(), [&](const fs::path& preset) { return SamePath(preset, current); });
    };
    if (!current.empty() && std::none_of(m.folders.begin(), m.folders.end(), listed))
    {
        // ReShade writes a new preset a moment after switching to it.
        const auto parent = std::find_if(m.folders.begin(), m.folders.end(), [&](const PresetFolder& folder) { return SamePath(folder.path, current.parent_path()); });
        if (parent != m.folders.end() || InPresets(current))
            (parent != m.folders.end() ? *parent : m.folders.front()).presets.push_back(current);
        // A preset picked elsewhere in ReShade's menu is shown, but nothing is created or read next to it.
        else
            m.folders.push_back({ .path = current.parent_path(), .name = "Other location", .other = true, .presets = { current } });
    }

    // Logos of folders no longer listed, or replaced since.
    for (auto texture = m.folderLogos.begin(); texture != m.folderLogos.end();)
    {
        const bool used = std::any_of(m.folders.begin(), m.folders.end(), [&](const PresetFolder& folder) {
            return folder.logo && folder.logo == texture->second.source && SamePath(folder.path, texture->first);
        });
        if (used)
            ++texture;
        else
        {
            DestroyTexture(texture->second);
            texture = m.folderLogos.erase(texture);
        }
    }
}

void RefreshFolders()
{
    if (m.foldersDirty)
        BuildFolders();
}

// Other games' folders start closed, unless the active preset is in one. Folders opened or closed by hand stay
// that way.
bool FolderOpen(const PresetFolder& folder, const fs::path& current)
{
    if (const auto found = m.folderOpen.find(folder.path.wstring()); found != m.folderOpen.end())
        return found->second;
    return !folder.game || folder.playing ||
           std::any_of(folder.presets.begin(), folder.presets.end(), [&](const fs::path& preset) { return SamePath(preset, current); });
}

void OpenNamePopup(NameAction action, const fs::path& target)
{
    m.nameAction = action;
    m.nameTarget = target;
    m.nameError.clear();
    const std::string stem = Utf8(target.stem().wstring());
    const std::string name = action == NameAction::New         ? "New preset"
                             : action == NameAction::NewFolder ? "New folder"
                             : action == NameAction::Rename    ? stem
                                                               : stem + " copy";
    CopyText(m.name, name);
    m.openNamePopup = true;
}

// Whether the dialog's preset must wait for ReShade to write it first. The active one is copied from the screen.
bool NameWaits(const fs::path& current)
{
    return m.nameAction != NameAction::New && !SamePath(m.nameTarget, current) && WritePending(m.nameTarget);
}

bool ApplyName(const fs::path& current)
{
    if (NameWaits(current))
    {
        m.nameError = "ReShade is still saving " + Utf8(m.nameTarget.stem().wstring()) + ". Try again in a moment.";
        return false;
    }
    std::wstring name = Wide(m.name);
    m.nameError = NameProblem(name);
    if (!m.nameError.empty())
        return false;
    if (m.nameAction == NameAction::NewFolder)
    {
        // A folder that exists already, such as one hidden since its presets moved out, is used as it is.
        m.nameError = MovePreset(m.nameTarget, PresetsRoot() / name);
        return m.nameError.empty();
    }

    // Other presets stay in the folder of the one they come from, unless that is outside the presets folder.
    const fs::path folder = m.nameAction == NameAction::New || !InPresets(m.nameTarget) ? NewPresetFolder() : m.nameTarget.parent_path();
    const fs::path path = folder / (name + L".ini");
    std::error_code error;
    if (fs::exists(path, error) && !(m.nameAction == NameAction::Rename && SamePath(path, m.nameTarget)))
    {
        m.nameError = "A preset with that name already exists.";
        return false;
    }

    switch (m.nameAction)
    {
    case NameAction::New:
        // Switching to a preset that does not exist yet starts with every effect off.
        fs::create_directories(folder, error);
        m.pendingPreset = path;
        m.saveNewPreset = true;
        break;
    case NameAction::Duplicate:
    case NameAction::SaveAsNew:
        fs::create_directories(folder, error);
        // The active preset is copied as it is on screen, unsaved changes included.
        if (SamePath(m.nameTarget, current))
        {
            m.runtime->export_current_preset(Utf8(path.wstring()).c_str());
            m.pendingKeepsEdits = true;
        }
        else if (!error)
            fs::copy_file(m.nameTarget, path, error);
        m.pendingPreset = path;
        break;
    case NameAction::Rename:
        fs::rename(m.nameTarget, path, error);
        if (!error)
        {
            UpdateGamePresets(m.nameTarget, path);
            ForgetPreset(m.nameTarget);
        }
        break;
    case NameAction::NewFolder:
        break;
    }
    if (error)
    {
        m.nameError = "Windows could not save the preset.";
        m.pendingPreset.clear();
        return false;
    }
    RequestScan();
    return true;
}

// A dialog in the middle of the screen, opened when open is set. Call EndDialog when this returns true.
bool BeginDialog(const char* id, bool& open, float width)
{
    if (open)
    {
        ImGui::OpenPopup(id);
        open = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(width), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(18)));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
        return true;
    ImGui::PopStyleVar();
    return false;
}

void EndDialog()
{
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}

// The dialog's title and the line below it.
void DialogText(const std::string& title, const std::string& detail)
{
    Text(title, kText, 16.5f);
    if (!detail.empty())
        Text(detail, kDim, 13.5f);
}

// The dialog's buttons, on the right with the last one primary. Returns the index of the one clicked, or -1.
int DialogButtons(std::initializer_list<const char*> labels, bool primaryEnabled = true)
{
    ImGui::Dummy(ImVec2(0, S(2)));
    const float buttonWidth = S(100);
    const int count = static_cast<int>(labels.size());
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(20) - buttonWidth * count - S(8) * (count - 1));
    int clicked = -1;
    int index = 0;
    for (const char* label : labels)
    {
        if (index)
            ImGui::SameLine(0, S(8));
        const bool primary = index == count - 1;
        if (Button(label, ImVec2(buttonWidth, S(32)), primary, !primary || primaryEnabled))
            clicked = index;
        ++index;
    }
    return clicked;
}

void NameDialog(const fs::path& current)
{
    if (!BeginDialog("##name", m.openNamePopup, 380))
        return;
    const char* titles[] = { "New preset", "Duplicate preset", "Rename preset", "Save as new preset", "Move to a new folder" };
    const char* actions[] = { "Create", "Duplicate", "Rename", "Save", "Move" };
    const int action = static_cast<int>(m.nameAction);
    DialogText(titles[action], m.nameAction == NameAction::New ? "Starts with every effect off." : "");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputText("##value", m.name, sizeof(m.name), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!m.nameError.empty())
        Text(m.nameError, kError, 13.5f);
    const int clicked = DialogButtons({ "Cancel", actions[action] }, !NameWaits(current));
    if ((clicked == 1 || enter) && ApplyName(current))
        ImGui::CloseCurrentPopup();
    if (clicked == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

void DeleteDialog()
{
    m.deleteDialogOpen = BeginDialog("##delete", m.openDeletePopup, 380);
    if (!m.deleteDialogOpen)
        return;
    DialogText("Delete " + Utf8(m.deleteTarget.stem().wstring()) + "?", "The preset goes to the Recycle Bin.");
    if (!m.deleteError.empty())
        Text(m.deleteError, kError, 13.5f);
    const int clicked = DialogButtons({ "Cancel", m.deleting ? "Deleting..." : "Delete" }, !m.deleting && !WritePending(m.deleteTarget));
    if (clicked == 1)
    {
        m.deleteError.clear();
        // When Windows asks before deleting for good, its question takes focus from the menu, which closes it.
        m.deleting = Recycle(m.deleteTarget.wstring(), [] { PostMessageW(g.overlay, kOpenMenuMessage, 0, 0); });
    }
    if (m.deleteDone || clicked == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m.deleteDone = false;
        ImGui::CloseCurrentPopup();
    }
    EndDialog();
}

// Picks up the end of a deletion, which runs on a thread of its own.
void FinishDelete()
{
    if (!m.deleting || m.deleting->load() == RecycleResult::Pending)
        return;
    const RecycleResult result = m.deleting->load();
    m.deleting.reset();
    if (result == RecycleResult::Recycled)
    {
        UpdateGamePresets(m.deleteTarget, {});
        ForgetPreset(m.deleteTarget);
        RequestScan();
        m.deleteDone = true;
    }
    else if (result == RecycleResult::Failed)
    {
        m.deleteError = "Windows could not delete the preset.";
        if (!m.deleteDialogOpen)
            ShowToast("Windows could not delete " + Utf8(m.deleteTarget.stem().wstring()));
    }
}

// The folders a preset can move to: the ones listed, then saved games that have no presets yet.
void MoveMenu(const fs::path& preset)
{
    const fs::path& root = PresetsRoot();
    std::vector<std::pair<std::string, fs::path>> targets;
    for (const PresetFolder& folder : m.folders)
        if (!folder.other)
            targets.emplace_back(folder.name, folder.path);
    for (const AutoGame& game : g.autoGames)
    {
        const std::wstring name = FolderName(game.name);
        if (!name.empty() && std::none_of(targets.begin(), targets.end(), [&](const auto& target) { return SamePath(target.second, root / name); }))
            targets.emplace_back(Utf8(name), root / name);
    }
    for (const auto& [name, folder] : targets)
        if (!SamePath(folder, preset.parent_path()) && ImGui::MenuItem(name.c_str()))
            if (const std::string error = MovePreset(preset, folder); !error.empty())
                ShowToast(error);
    ImGui::Separator();
    if (ImGui::MenuItem("New folder..."))
        OpenNamePopup(NameAction::NewFolder, preset);
}

// The preset's menu, opened from the three dots on its row.
void PresetActions(const fs::path& path, bool active)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("actions"))
    {
        // The active preset is duplicated from what is on screen. Others wait until ReShade has written them.
        const bool writing = !active && WritePending(path);
        if (ImGui::MenuItem("Duplicate", nullptr, false, !writing))
            OpenNamePopup(NameAction::Duplicate, path);
        if (ImGui::MenuItem("Rename", nullptr, false, !active && !writing))
            OpenNamePopup(NameAction::Rename, path);
        if (ImGui::BeginMenu("Move to", !active && !writing))
        {
            MoveMenu(path);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Delete", nullptr, false, !active && !writing && !m.deleting))
        {
            m.deleteTarget = path;
            m.deleteError.clear();
            m.deleteDone = false;
            m.openDeletePopup = true;
        }
        if (active)
        {
            ImGui::Separator();
            PushSize(12.5f);
            ImGui::TextDisabled("Switch to another preset to\nrename, move or delete this one.");
            ImGui::PopFont();
        }
        else if (writing)
        {
            ImGui::Separator();
            PushSize(12.5f);
            ImGui::TextDisabled("ReShade is still saving this preset.");
            ImGui::PopFont();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void PresetRow(const fs::path& path, bool active)
{
    const std::string name = Utf8(path.stem().wstring());
    ImGui::PushID(name.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(44));
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav) && !active)
        m.pendingPreset = path;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));
    draw->AddRect(start, start + size, active ? kAccent : kBorder, S(8), 0, active ? S(1.5f) : 1.0f);

    // The menu button, drawn as three dots.
    const ImVec2 dots = start + ImVec2(size.x - S(38), (size.y - S(28)) / 2);
    ImGui::SetCursorScreenPos(dots);
    if (ImGui::InvisibleButton("actions", ImVec2(S(28), S(28)), ImGuiButtonFlags_EnableNav))
        ImGui::OpenPopup("actions");
    const bool dotsHovered = ImGui::IsItemHovered();
    HandOnHover();
    if (dotsHovered)
        draw->AddRectFilled(dots, dots + ImVec2(S(28), S(28)), kBorder, S(6));
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(dots + ImVec2(S(14) + i * S(5), S(14)), S(1.6f), hovered || dotsHovered ? kText : kDim);

    PushSize(14.5f);
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(dots.x - S(70), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(14), textY), kText, name.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    if (active)
    {
        PushSize(11.5f);
        const char* tag = "ACTIVE";
        const ImVec2 tagSize = ImGui::CalcTextSize(tag) + ImVec2(S(14), S(6));
        const ImVec2 tagStart(dots.x - S(8) - tagSize.x, start.y + (size.y - tagSize.y) / 2);
        draw->AddRectFilled(tagStart, tagStart + tagSize, Color(theme::kAccent, 50), tagSize.y / 2);
        draw->AddText(tagStart + ImVec2(S(7), S(3)), kAccentHover, tag);
        ImGui::PopFont();
    }

    PresetActions(path, active);
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    ImGui::PopID();
}

// The folder's logo.png, such as a game's icon, or a stand-in: a globe for all games, a game's first letter, or a
// folder.
void FolderIcon(ImDrawList* draw, const PresetFolder& folder, ImVec2 min, float size)
{
    const ImVec2 max = min + ImVec2(size, size);
    if (const uint64_t logo = FolderLogo(folder))
    {
        draw->AddImageRounded(ImTextureRef(logo), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(4));
        return;
    }
    const ImVec2 center = (min + max) * 0.5f;
    const float t = S(1.5f);
    if (folder.all)
    {
        const float r = size * 0.4f;
        draw->AddCircle(center, r, kDim, 0, t);
        draw->AddEllipse(center, ImVec2(r * 0.45f, r), kDim, 0, 0, t);
        draw->AddLine(center - ImVec2(r, 0), center + ImVec2(r, 0), kDim, t);
    }
    else if (folder.game)
    {
        draw->AddRectFilled(min, max, kBorder, S(5));
        size_t length = 1;
        while (length < folder.name.size() && (static_cast<unsigned char>(folder.name[length]) & 0xC0) == 0x80)
            ++length;
        const std::string letter = length == 1 ? std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(folder.name[0]))))
                                               : folder.name.substr(0, length);
        PushSize(13);
        draw->AddText(center - ImGui::CalcTextSize(letter.c_str()) * 0.5f, kText, letter.c_str());
        ImGui::PopFont();
    }
    else
    {
        const ImVec2 body(min.x + size * 0.1f, min.y + size * 0.32f);
        draw->AddRectFilled(ImVec2(body.x, min.y + size * 0.2f), ImVec2(body.x + size * 0.35f, body.y + t), kDim, S(1.5f));
        draw->AddRect(body, ImVec2(max.x - size * 0.1f, max.y - size * 0.18f), kDim, S(2), 0, t);
    }
}

// A folder's logo, name and preset count. Clicking it opens or closes the folder.
bool FolderHeader(const PresetFolder& folder, bool open, size_t presets)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(34));
    const bool clicked = ImGui::InvisibleButton("folder", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(start, start + size, kCard, S(8));

    const float icon = std::round(S(22));
    const ImVec2 iconStart(std::round(start.x + S(6)), std::round(start.y + (size.y - icon) / 2));
    FolderIcon(draw, folder, iconStart, icon);

    const float chevron = start.x + size.x - S(20);
    PushSize(13);
    const std::string count = std::to_string(presets);
    const ImVec2 countSize = ImGui::CalcTextSize(count.c_str());
    const float countX = chevron - S(16) - countSize.x;
    draw->AddText(ImVec2(countX, start.y + (size.y - countSize.y) / 2), kDim, count.c_str());
    ImGui::PopFont();
    PushSize(14.5f);
    draw->PushClipRect(start, ImVec2(countX - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(iconStart.x + icon + S(10), start.y + (size.y - ImGui::GetFontSize()) / 2), kText, folder.name.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    Chevron(draw, ImVec2(chevron, start.y + size.y / 2), open, open || hovered ? kText : kDim);
    return clicked;
}

// Returns false when searching found nothing in the folder, which is then left out.
bool FolderSection(const PresetFolder& folder, const fs::path& current, const std::string& filter)
{
    // While searching, folders are open and show the presets that match, or all of them when the folder's name does.
    const bool searching = !filter.empty();
    const bool folderMatches = searching && Lower(folder.name).find(filter) != std::string::npos;
    std::vector<const fs::path*> shown;
    for (const fs::path& preset : folder.presets)
        if (!searching || folderMatches || Lower(Utf8(preset.stem().wstring())).find(filter) != std::string::npos)
            shown.push_back(&preset);
    if (searching && shown.empty())
        return false;

    ImGui::PushID(Utf8(folder.path.wstring()).c_str());
    const bool open = searching || FolderOpen(folder, current);
    if (FolderHeader(folder, open, shown.size()) && !searching)
        m.folderOpen[folder.path.wstring()] = !open;
    if (open)
    {
        for (const fs::path* preset : shown)
            PresetRow(*preset, SamePath(*preset, current));
        if (folder.presets.empty() && SamePath(folder.path, NewPresetFolder()))
            Text("New presets and imports go here.", kDim, 13.5f);
    }
    ImGui::PopID();
    return true;
}

// Asks what to do with unsaved changes when switching presets. The switch waits for the answer.
void UnsavedDialog(const fs::path& current)
{
    if (!BeginDialog("##unsaved", m.openUnsavedPopup, 420))
    {
        // ImGui closes the dialog while the menu is hidden, so a switch still waiting asks again.
        m.askingUnsaved = false;
        return;
    }
    DialogText("Save your changes to " + Utf8(current.stem().wstring()) + "?", "Discarding goes back to how the preset was last saved.");
    const int clicked = DialogButtons({ "Discard", "Cancel", "Save" });
    if (clicked == 0)
        m.unsavedChoice = UnsavedChoice::Discard;
    else if (clicked == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m.pendingPreset.clear();
        m.saveNewPreset = false;
        m.pendingKeepsEdits = false;
    }
    else if (clicked == 2)
        m.unsavedChoice = UnsavedChoice::Save;
    // Answered, or the menu closed and dropped the switch.
    if (m.unsavedChoice != UnsavedChoice::Ask || m.pendingPreset.empty())
    {
        m.askingUnsaved = false;
        ImGui::CloseCurrentPopup();
    }
    EndDialog();
}

void UpdateTechniques();

void PresetsTab()
{
    const fs::path& current = m.current;
    if (GetTickCount64() - m.scanRequested > kScanInterval)
        RequestScan();

    if (Button("New preset", ImVec2(S(130), S(32)), true))
        OpenNamePopup(NameAction::New, {});
    ImGui::SameLine(0, S(8));
    if (Button("Import", ImVec2(S(90), S(32)), false, !importDialog.open))
        OpenImportDialog();
    if (!AutoSavePresets())
    {
        ImGui::SameLine(0, S(8));
        if (Button("Save as new", ImVec2(S(120), S(32))))
            OpenNamePopup(NameAction::SaveAsNew, current);
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    PushSize(14.5f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##presetsearch", "Search presets", m.presetSearch, sizeof(m.presetSearch));
    ImGui::PopFont();

    UpdateTechniques();
    const std::vector<std::string> missing = MissingEffects(current);
    if (!missing.empty())
    {
        std::string names;
        for (const std::string& effect : missing)
            names += (names.empty() ? "" : ", ") + effect;
        Text("This preset uses effects that are not installed: " + names +
                 ". They stay off, and saving or switching presets removes them from it.",
             kWarning, 13.5f);
        ImGui::Dummy(ImVec2(0, S(2)));
    }

    const std::string filter = Lower(m.presetSearch);
    bool any = false;
    for (const PresetFolder& folder : m.folders)
        if (FolderSection(folder, current, filter))
            any = true;
    if (!any && !filter.empty())
        Text("No presets match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    Text(AutoSavePresets() ? "Changes save to the active preset as you make them."
                    : "Changes apply right away. Save them with the icon at the top.",
         kDim, 13);
    PushSize(13.5f);
    if (Link("Open presets folder"))
        ShellOpen(PresetsRoot().wstring());
    ImGui::PopFont();
}

// Effects

std::string TechniqueString(effect_runtime* runtime, effect_technique technique, const char* name)
{
    size_t size = 0;
    if (!runtime->get_annotation_string_from_technique(technique, name, nullptr, &size) || size <= 1)
        return {};
    std::string value(size, '\0');
    runtime->get_annotation_string_from_technique(technique, name, value.data(), &size);
    value.resize(size);
    return value;
}

std::string UniformString(effect_runtime* runtime, effect_uniform_variable variable, const char* name)
{
    size_t size = 0;
    if (!runtime->get_annotation_string_from_uniform_variable(variable, name, nullptr, &size) || size <= 1)
        return {};
    std::string value(size, '\0');
    runtime->get_annotation_string_from_uniform_variable(variable, name, value.data(), &size);
    value.resize(size);
    return value;
}

void LoadTechniques()
{
    m.techniques.clear();
    m.effects.clear();
    m.parameters.clear();
    m.parametersEffect.clear();
    // Returns nothing while ReShade compiles effects.
    m.runtime->enumerate_techniques(nullptr, [](effect_runtime* runtime, effect_technique handle) {
        char name[256] = "";
        char effect[256] = "";
        runtime->get_technique_effect_name(handle, effect);
        m.effects.insert(Lower(effect));
        int32_t hidden = 0;
        if (runtime->get_annotation_int_from_technique(handle, "hidden", &hidden, 1) && hidden)
            return;
        runtime->get_technique_name(handle, name);
        Technique technique;
        technique.handle = handle;
        technique.key = std::string(name) + "@" + effect;
        technique.label = TechniqueString(runtime, handle, "ui_label");
        if (technique.label.empty())
            technique.label = name;
        technique.effect = effect;
        technique.tooltip = TechniqueString(runtime, handle, "ui_tooltip");
        technique.search = Lower(technique.label + " " + technique.effect);
        m.techniques.push_back(std::move(technique));
    });
    m.byName.resize(m.techniques.size());
    for (size_t i = 0; i < m.byName.size(); ++i)
        m.byName[i] = i;
    std::sort(m.byName.begin(), m.byName.end(),
              [](size_t a, size_t b) { return _stricmp(m.techniques[a].label.c_str(), m.techniques[b].label.c_str()) < 0; });
    m.techniquesTried = GetTickCount64();
    m.techniquesDirty = m.effects.empty();
    m.effectsEmpty = m.techniques.empty();
    if (!m.techniquesDirty)
        return;
    m.effectsEmpty = false;
    // After compiling, ReShade still creates the effects that are on, one a frame, and lists nothing until done.
    if ((m.compiledAt && m.techniquesTried - m.compiledAt > kCreateWait) || m.effectFiles == false)
    {
        m.effectsEmpty = true;
        m.techniquesDirty = false;
    }
    else if (!m.effectCheckRequested)
        RequestEffectCheck();
}

// Effects are listed again after a reload, and tried this often while ReShade loads them.
void UpdateTechniques()
{
    if (m.techniquesDirty && GetTickCount64() - m.techniquesTried >= kLoadRetry)
        LoadTechniques();
}

void LoadParameters(const std::string& effect)
{
    m.parameters.clear();
    m.parametersEffect = effect;
    m.runtime->enumerate_uniform_variables(effect.c_str(), [](effect_runtime* runtime, effect_uniform_variable handle) {
        // Variables with a source, such as the frame time, are set by ReShade itself.
        size_t size = 0;
        int32_t flag = 0;
        if (runtime->get_annotation_string_from_uniform_variable(handle, "source", nullptr, &size) ||
            (runtime->get_annotation_int_from_uniform_variable(handle, "hidden", &flag, 1) && flag))
            return;
        Parameter parameter;
        parameter.handle = handle;
        runtime->get_uniform_variable_type(handle, &parameter.base, &parameter.rows, &parameter.columns, &parameter.arrayLength);
        char name[256] = "";
        runtime->get_uniform_variable_name(handle, name);
        parameter.label = UniformString(runtime, handle, "ui_label");
        if (parameter.label.empty())
            parameter.label = name;
        parameter.tooltip = UniformString(runtime, handle, "ui_tooltip");
        parameter.category = UniformString(runtime, handle, "ui_category");
        parameter.type = UniformString(runtime, handle, "ui_type");
        parameter.text = UniformString(runtime, handle, "ui_text");
        parameter.units = UniformString(runtime, handle, "ui_units");
        parameter.items = UniformString(runtime, handle, "ui_items");
        if (!parameter.items.empty() && parameter.items.back() != '\0')
            parameter.items.push_back('\0');
        runtime->get_annotation_float_from_uniform_variable(handle, "ui_min", &parameter.min, 1);
        runtime->get_annotation_float_from_uniform_variable(handle, "ui_max", &parameter.max, 1);
        runtime->get_annotation_float_from_uniform_variable(handle, "ui_step", &parameter.step, 1);
        runtime->get_annotation_int_from_uniform_variable(handle, "ui_spacing", &parameter.spacing, 1);
        parameter.categoryClosed = runtime->get_annotation_int_from_uniform_variable(handle, "ui_category_closed", &flag, 1) && flag;
        parameter.noReset = runtime->get_annotation_int_from_uniform_variable(handle, "noreset", &flag, 1) && flag;
        m.parameters.push_back(std::move(parameter));
    });
}

// The base types that have a control. ReShade keeps min16 types at 32 bits for add-ons, so they share one.
bool HasControl(format base)
{
    switch (base)
    {
    case format::r32_typeless:
    case format::r32_float:
    case format::r16_float:
    case format::r32_sint:
    case format::r16_sint:
    case format::r32_uint:
    case format::r16_uint:
        return true;
    default:
        return false;
    }
}

bool FloatControl(const Parameter& p, const std::string& units, bool list)
{
    const int count = static_cast<int>(p.rows);
    float values[4]{};
    m.runtime->get_uniform_value_float(p.handle, values, count);
    const std::string format = "%.3f" + units;
    const bool range = p.min < p.max;
    bool changed = false;
    if (p.type == "color" && count == 3)
        changed = ImGui::ColorEdit3("##value", values);
    else if (p.type == "color" && count == 4)
        changed = ImGui::ColorEdit4("##value", values, ImGuiColorEditFlags_AlphaBar);
    else if (list && count == 1)
    {
        int index = static_cast<int>(values[0]);
        changed = ImGui::Combo("##value", &index, p.items.c_str());
        values[0] = static_cast<float>(index);
    }
    else if (p.type == "input")
        changed = ImGui::InputScalarN("##value", ImGuiDataType_Float, values, count, nullptr, nullptr, format.c_str());
    else if (range && p.type != "drag")
        changed = ImGui::SliderScalarN("##value", ImGuiDataType_Float, values, count, &p.min, &p.max, format.c_str());
    else
    {
        const float speed = p.step > 0 ? p.step : range ? (p.max - p.min) / 300 : 0.01f;
        changed = ImGui::DragScalarN("##value", ImGuiDataType_Float, values, count, speed, range ? &p.min : nullptr, range ? &p.max : nullptr,
                                     format.c_str());
    }
    if (changed)
        m.runtime->set_uniform_value_float(p.handle, values, count);
    return changed;
}

// For signed and unsigned values, with min and max already in their type.
template <typename T>
bool IntegerControl(const Parameter& p, ImGuiDataType type, T* values, T min, T max, const std::string& format, bool list)
{
    const int count = static_cast<int>(p.rows);
    if (list && count == 1)
    {
        int index = static_cast<int>(values[0]);
        const bool changed = ImGui::Combo("##value", &index, p.items.c_str());
        values[0] = static_cast<T>(index);
        return changed;
    }
    const bool range = min < max;
    if (p.type == "input")
        return ImGui::InputScalarN("##value", type, values, count, nullptr, nullptr, format.c_str());
    if (range && p.type != "drag")
        return ImGui::SliderScalarN("##value", type, values, count, &min, &max, format.c_str());
    return ImGui::DragScalarN("##value", type, values, count, std::max(p.step, 1.0f), range ? &min : nullptr, range ? &max : nullptr, format.c_str());
}

// Draws the control for one variable of an effect. Returns true when its value changed.
bool DrawParameter(const Parameter& p)
{
    const bool list = p.type == "combo" || p.type == "list" || p.type == "radio";
    const bool supported = HasControl(p.base) && p.arrayLength == 0 && p.columns <= 1 && p.rows >= 1 && p.rows <= 4 && !(list && p.items.empty());
    if (p.spacing > 0)
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * p.spacing));
    if (!p.text.empty())
        Text(p.text, kDim, 13.5f);
    // Arrays, matrices and other types are left to ReShade's menu, like variables that only carry text.
    if (!supported)
        return false;

    const float x = ImGui::GetCursorPosX();
    const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x + labelWidth - S(10));
    ImGui::PushStyleColor(ImGuiCol_Text, p.tooltip.empty() ? kText : Color(0xD8D9E6));
    ImGui::TextUnformatted(p.label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    if (!p.tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", p.tooltip.c_str());
    ImGui::SameLine(x + labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    ImGui::PushID(reinterpret_cast<void*>(p.handle.handle));
    std::string units = p.units;
    for (size_t at = units.find('%'); at != std::string::npos; at = units.find('%', at + 2))
        units.insert(at, 1, '%');
    const int count = static_cast<int>(p.rows);
    bool changed = false;
    switch (p.base)
    {
    case format::r32_typeless:
    {
        bool value = false;
        m.runtime->get_uniform_value_bool(p.handle, &value, 1);
        if (ImGui::Checkbox("##value", &value))
        {
            m.runtime->set_uniform_value_bool(p.handle, &value, 1);
            changed = true;
        }
        break;
    }
    case format::r32_float:
    case format::r16_float:
        changed = FloatControl(p, units, list);
        break;
    case format::r32_sint:
    case format::r16_sint:
    {
        int32_t values[4]{};
        m.runtime->get_uniform_value_int(p.handle, values, count);
        changed = IntegerControl(p, ImGuiDataType_S32, values, static_cast<int32_t>(p.min), static_cast<int32_t>(p.max), "%d" + units, list);
        if (changed)
            m.runtime->set_uniform_value_int(p.handle, values, count);
        break;
    }
    case format::r32_uint:
    case format::r16_uint:
    {
        uint32_t values[4]{};
        m.runtime->get_uniform_value_uint(p.handle, values, count);
        // A negative ui_min would wrap around to the largest value.
        const uint32_t min = static_cast<uint32_t>(std::max(p.min, 0.0f));
        const uint32_t max = static_cast<uint32_t>(std::max(p.max, 0.0f));
        changed = IntegerControl(p, ImGuiDataType_U32, values, min, max, "%u" + units, list);
        if (changed)
            m.runtime->set_uniform_value_uint(p.handle, values, count);
        break;
    }
    default:
        break;
    }
    if (!p.noReset && ImGui::BeginPopupContextItem("reset"))
    {
        if (ImGui::MenuItem("Reset to default"))
        {
            m.runtime->reset_uniform_value(p.handle);
            changed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void ResetEffect(const std::string& effect)
{
    if (m.parametersEffect != effect)
        LoadParameters(effect);
    for (const Parameter& parameter : m.parameters)
        if (!parameter.noReset)
            m.runtime->reset_uniform_value(parameter.handle);
    m.presetChanged = true;
}

void DrawParameters(const Technique& technique)
{
    if (m.parametersEffect != technique.effect)
        LoadParameters(technique.effect);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    PushSize(13.5f);
    if (technique.failed)
        Text(technique.effect + " did not compile, so it cannot be turned on. ReShade's menu shows why.", kWarning, 13.5f);
    if (!technique.tooltip.empty())
        Text(technique.tooltip, kDim, 13.5f);
    if (m.parameters.empty())
        Text("This effect has no settings.", kDim, 13.5f);

    std::string category;
    bool open = true;
    for (const Parameter& parameter : m.parameters)
    {
        if (parameter.category != category)
        {
            category = parameter.category;
            open = category.empty() ||
                   ImGui::CollapsingHeader(category.c_str(), parameter.categoryClosed ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen);
        }
        if (open && DrawParameter(parameter))
            m.presetChanged = true;
    }
    if (!m.parameters.empty())
    {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (Link("Reset all", kDim))
        {
            m.confirm = Confirmation::ResetEffect;
            m.confirmEffect = technique.effect;
            m.openConfirmPopup = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Right-click a setting to reset only that one.");
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Moves an effect to where another one runs, as dragging it there does. The other effects keep their order.
void MoveTechnique(size_t from, size_t to)
{
    std::vector<effect_technique> order;
    m.runtime->enumerate_techniques(nullptr, [&order](effect_runtime*, effect_technique handle) { order.push_back(handle); });
    const auto source = std::find(order.begin(), order.end(), m.techniques[from].handle);
    const auto target = std::find(order.begin(), order.end(), m.techniques[to].handle);
    if (source == order.end() || target == order.end())
        return;
    if (source < target)
        std::rotate(source, source + 1, target + 1);
    else
        std::rotate(target, source, source + 1);
    m.runtime->reorder_techniques(order.size(), order.data());
    m.presetChanged = true;
    LoadTechniques();
}

// Effects that are on can be dragged onto each other to change the order they run in, which from and to take.
void TechniqueRow(size_t index, size_t& moveFrom, size_t& moveTo)
{
    Technique& technique = m.techniques[index];
    const bool on = m.runtime->get_technique_state(technique.handle);
    const bool expanded = m.expanded == technique.key;
    ImGui::PushID(technique.key.c_str());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(38));
    if (!expanded && !ImGui::IsRectVisible(size))
    {
        ImGui::Dummy(size);
        ImGui::PopID();
        return;
    }

    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("row", size, ImGuiButtonFlags_EnableNav))
        m.expanded = expanded ? std::string() : technique.key;
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    if (on)
    {
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("technique", &index, sizeof(index));
            ImGui::TextUnformatted(technique.label.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("technique"))
            {
                moveFrom = *static_cast<const size_t*>(payload->Data);
                moveTo = index;
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || expanded)
        draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));

    ImGui::SetCursorScreenPos(start + ImVec2(S(10), (size.y - S(18)) / 2));
    if (Switch("on", on))
    {
        m.runtime->set_technique_state(technique.handle, !on);
        technique.failed = !on && !m.runtime->get_technique_state(technique.handle);
        m.presetChanged = true;
    }

    const float chevron = start.x + size.x - S(20);
    PushSize(14.5f);
    const ImVec2 labelSize = ImGui::CalcTextSize(technique.label.c_str());
    const float textY = start.y + (size.y - ImGui::GetFontSize()) / 2;
    draw->PushClipRect(start, ImVec2(chevron - S(10), start.y + size.y), true);
    draw->AddText(ImVec2(start.x + S(52), textY), technique.failed ? kDim : kText, technique.label.c_str());
    ImGui::PopFont();
    PushSize(12.5f);
    draw->AddText(ImVec2(start.x + S(52) + labelSize.x + S(8), textY + S(1.5f)), technique.failed ? kWarning : kDim,
                  technique.failed ? "did not compile" : technique.effect.c_str());
    ImGui::PopFont();
    draw->PopClipRect();

    Chevron(draw, ImVec2(chevron, start.y + size.y / 2), expanded, expanded || hovered ? kText : kDim);

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(size);
    if (expanded)
        DrawParameters(technique);
    ImGui::PopID();
}

void EffectsTab()
{
    UpdateTechniques();

    PushSize(14.5f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search effects", m.search, sizeof(m.search));
    ImGui::PopFont();

    if (m.techniques.empty() && m.effectsEmpty)
    {
        ImGui::Dummy(ImVec2(0, S(6)));
        Text(m.effectFiles == false ? "No effects are installed. Run Unishade Setup again to download them."
                                    : "No effects loaded. The ReShade button below opens ReShade's menu, which shows why.",
             kDim, 14);
        return;
    }
    if (m.techniques.empty())
    {
        ImGui::Dummy(ImVec2(0, S(6)));
        Spinner(S(10));
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
        Text("Loading effects. The first time can take a minute.", kDim, 14);
        return;
    }

    const std::string filter = Lower(m.search);
    const auto matches = [&](const Technique& technique) { return filter.empty() || technique.search.find(filter) != std::string::npos; };

    // An effect turned off stays in Active until the menu closes, so it can be turned back on where it was.
    for (const Technique& technique : m.techniques)
        if (m.runtime->get_technique_state(technique.handle))
            m.active.insert(technique.key);
    const auto active = [&](const Technique& technique) { return m.active.count(technique.key) != 0; };

    size_t moveFrom = 0, moveTo = 0;
    Heading("ACTIVE");
    bool any = false;
    for (size_t i = 0; i < m.techniques.size(); ++i)
        if (active(m.techniques[i]) && matches(m.techniques[i]))
        {
            TechniqueRow(i, moveFrom, moveTo);
            any = true;
        }
    if (!any)
        Text(filter.empty() ? "No effects are on. Pick a preset or turn effects on below." : "No active effects match.", kDim, 13.5f);

    ImGui::Dummy(ImVec2(0, S(4)));
    size_t others = 0;
    for (const Technique& technique : m.techniques)
        others += !active(technique) && matches(technique);
    const bool searching = !filter.empty();
    const std::string title = "ALL EFFECTS (" + std::to_string(others) + ")";
    PushSize(12);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::SetNextItemOpen(searching || m.showAll);
    const bool open = ImGui::TreeNodeEx((title + "###all").c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::IsItemToggledOpen() && !searching)
        m.showAll = !m.showAll;
    HandOnHover();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (open)
        for (size_t index : m.byName)
            if (!active(m.techniques[index]) && matches(m.techniques[index]))
                TechniqueRow(index, moveFrom, moveTo);

    if (moveFrom != moveTo)
        MoveTechnique(moveFrom, moveTo);
}

// Settings

void StopCapture()
{
    m.capturing = -1;
    SuspendHotkeys(false);
}

// While the menu waits for a shortcut, the next key pressed with any modifiers becomes it.
void CaptureShortcut()
{
    if (m.capturing < 0)
        return;
    const uint32_t key = m.runtime->last_key_pressed();
    // Mouse buttons and modifiers on their own.
    if (key < 8 || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN || (key >= VK_LSHIFT && key <= VK_RMENU))
        return;
    Hotkey hotkey;
    if (m.runtime->is_key_down(VK_CONTROL))
        hotkey.modifiers |= MOD_CONTROL;
    if (m.runtime->is_key_down(VK_MENU))
        hotkey.modifiers |= MOD_ALT;
    if (m.runtime->is_key_down(VK_SHIFT))
        hotkey.modifiers |= MOD_SHIFT;
    if (m.runtime->is_key_down(VK_LWIN) || m.runtime->is_key_down(VK_RWIN))
        hotkey.modifiers |= MOD_WIN;
    if (key == VK_ESCAPE && hotkey.modifiers == MOD_NOREPEAT)
    {
        StopCapture();
        return;
    }
    hotkey.key = key;
    if (FormatHotkey(hotkey).empty())
    {
        m.shortcutError = UnusableKeyText();
        return;
    }
    InputHotkeys hotkeys = g.hotkeys;
    hotkeys.*kShortcuts[m.capturing].member = hotkey;
    StopCapture();
    m.shortcutError = ChangeHotkeys(hotkeys);
}

void ShortcutRow(int index)
{
    const Shortcut& shortcut = kShortcuts[index];
    const char* title = shortcut.title;
    const char* description = shortcut.description;
    const Hotkey& hotkey = g.hotkeys.*shortcut.member;
    // Without the menu shortcut there would be no way back into the menu.
    const bool clearable = shortcut.member != &InputHotkeys::input;
    ImGui::PushID(index);
    const float x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const float buttonWidth = S(130);
    const float controls = buttonWidth + (clearable ? S(36) : 0);

    ImGui::BeginGroup();
    Text(title, kText, 14.5f, x + width - controls - S(14));
    Text(description, kDim, 13, x + width - controls - S(14));
    ImGui::EndGroup();
    const float bottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos(ImVec2(x + width - controls, top + S(2)));
    const bool capturing = m.capturing == index;
    const std::string label = capturing ? "Press keys..." : hotkey.key ? Utf8(FormatHotkey(hotkey)) : "Not set";
    ImGui::PushStyleColor(ImGuiCol_Border, capturing ? kAccent : kBorderStrong);
    if (Button((label + "##key").c_str(), ImVec2(buttonWidth, S(32))))
    {
        if (capturing)
            StopCapture();
        else
        {
            m.capturing = index;
            m.shortcutError.clear();
            SuspendHotkeys(true);
        }
    }
    ImGui::PopStyleColor();
    if (clearable)
    {
        ImGui::SameLine(0, S(4));
        if (Button("x", ImVec2(S(32), S(32)), false, hotkey.key != 0))
        {
            InputHotkeys hotkeys = g.hotkeys;
            hotkeys.*shortcut.member = {};
            StopCapture();
            m.shortcutError = ChangeHotkeys(hotkeys);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Leave unassigned");
    }
    ImGui::SetCursorPos(ImVec2(x, std::max(bottom, top + S(40)) + S(6)));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

// A switch with its title and description beside it. Returns true when clicked.
bool SwitchRow(const char* id, bool on, const char* title, const char* description)
{
    const bool clicked = Switch(id, on);
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    Text(title, kText, 14.5f);
    Text(description, kDim, 13);
    ImGui::EndGroup();
    return clicked;
}

// The menu's size in quarter steps.
void MenuSizeRow()
{
    const float x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const float button = S(32);
    const float valueWidth = S(58);
    const float controls = button * 2 + valueWidth;

    ImGui::BeginGroup();
    Text("Menu size", kText, 14.5f, x + width - controls - S(14));
    Text("On top of the size that follows the game's window. Changes right away.", kDim, 13, x + width - controls - S(14));
    ImGui::EndGroup();
    const float bottom = ImGui::GetCursorPosY();

    ImGui::SetCursorPos(ImVec2(x + width - controls, top + S(2)));
    float step = 0;
    if (Button("-##smaller", ImVec2(button, button), false, MenuScale() > kSmallestMenuScale + 0.01f))
        step = -0.25f;
    ImGui::SameLine(0, 0);
    const std::string value = std::to_string(std::lround(MenuScale() * 100)) + "%";
    PushSize(14);
    const ImVec2 valueSize = ImGui::CalcTextSize(value.c_str());
    const ImVec2 valueStart = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(valueStart + ImVec2((valueWidth - valueSize.x) / 2, (button - valueSize.y) / 2), kText, value.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(valueWidth, button));
    ImGui::SameLine(0, 0);
    if (Button("+##larger", ImVec2(button, button), false, MenuScale() < kLargestMenuScale - 0.01f))
        step = 0.25f;
    if (step != 0)
        SetMenuScale(std::round((MenuScale() + step) * 4) / 4);
    ImGui::SetCursorPos(ImVec2(x, std::max(bottom, top + S(40)) + S(6)));
    ImGui::Dummy(ImVec2(0, 0));
}

// A setting with its choices under it. Returns the index of the choice clicked, or -1.
int ChoiceRow(const char* title, const char* description, std::initializer_list<const char*> choices, int selected)
{
    Text(title, kText, 14.5f);
    Text(description, kDim, 13);
    return Segmented(title, choices, selected, kAccent);
}

// The place of value among a setting's choices, or past the last one for a value that is none of them.
template <size_t N>
int ChoiceOf(const int (&values)[N], int value)
{
    return static_cast<int>(std::find(std::begin(values), std::end(values), value) - std::begin(values));
}

void FrameRateRow()
{
    constexpr int kCustom = static_cast<int>(std::size(kFrameRates));
    const int limit = FrameRateLimit();
    // Changed in the launcher, so what was picked and typed here no longer goes with it.
    if (limit != m.frameRate)
    {
        m.frameRate = m.customRate = limit;
        m.customRatePicked = false;
    }
    const int choice = ChoiceOf(kFrameRates, limit);
    const bool custom = m.customRatePicked || choice == kCustom;
    const int clicked = ChoiceRow("Frame rate", "The most frames a second Unishade shows. A lower limit leaves more of the GPU to the game.",
                                  { "Follow game", "120 FPS", "60 FPS", "Custom" }, custom ? kCustom : choice);
    if (clicked == kCustom)
    {
        m.customRatePicked = true;
        m.frameRate = m.customRate = limit ? limit : 60;
        SetFrameRateLimit(m.customRate);
    }
    else if (clicked >= 0)
    {
        m.customRatePicked = false;
        m.frameRate = kFrameRates[clicked];
        SetFrameRateLimit(m.frameRate);
    }
    if (!custom)
        return;

    ImGui::SetNextItemWidth(S(84));
    ImGui::InputInt("##custom_rate", &m.customRate, 0, 0);
    // Applies once typing ends, so a number is not limited while it is still being typed.
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        m.frameRate = m.customRate = std::clamp(m.customRate, kSlowestFrameRate, kFastestFrameRate);
        SetFrameRateLimit(m.customRate);
    }
    ImGui::SameLine(0, S(10));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6));
    Text("frames a second, from " + std::to_string(kSlowestFrameRate) + " to " + std::to_string(kFastestFrameRate), kDim, 13);
}

void PerformanceSettings()
{
    const std::wstring& game = PerformanceGame();
    Text(game.empty() ? "For every game without its own settings." : "For " + Utf8(game) + " only.", kDim, 13);
    ImGui::Dummy(ImVec2(0, S(4)));
    FrameRateRow();

    ImGui::Dummy(ImVec2(0, S(8)));
    int clicked = ChoiceRow("Effect resolution",
                            "Effects run on a smaller picture, stretched back to the game's size. Lower is faster and blurrier. The menu "
                            "blurs too, and screenshots get smaller.",
                            { "100%", "75%", "50%" }, ChoiceOf(kEffectResolutions, EffectResolution()));
    if (clicked >= 0)
        SetEffectResolution(kEffectResolutions[clicked]);

    if (DepthEnabled())
    {
        ImGui::Dummy(ImVec2(0, S(8)));
        clicked = ChoiceRow("Depth detail", "Depth is estimated from a smaller picture. Lower is faster, and effects that use depth lose fine detail.",
                            { "Ultra", "High", "Medium", "Low" }, ChoiceOf(kDepthSizes, DepthSize()));
        if (clicked >= 0)
            SetDepthSize(kDepthSizes[clicked]);
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    Heading("DEBUG");
    if (SwitchRow("debug_info", DebugInfoEnabled(), "Show debug info", "Captured game FPS, output FPS and frame loss."))
        SetDebugInfoEnabled(!DebugInfoEnabled());
}

void ShortcutSettings()
{
    Text("Click a shortcut, then press the keys you want. They work right away.", kDim, 13);
    ImGui::Dummy(ImVec2(0, S(4)));
    for (int i = 0; i < static_cast<int>(std::size(kShortcuts)); ++i)
        ShortcutRow(i);

    if (!m.shortcutError.empty())
        Text(Utf8(m.shortcutError), kError, 13.5f);
    for (const std::wstring& warning : ShortcutWarnings())
        Text(Utf8(warning), kWarning, 13.5f);
    if (Link("Reset to defaults", kDim))
    {
        StopCapture();
        m.confirm = Confirmation::ResetShortcuts;
        m.openConfirmPopup = true;
    }
}

void GeneralSettings()
{
    Heading("PRESETS");
    if (SwitchRow("autosave", AutoSavePresets(), "Save changes automatically", "Turn off to try changes first and save them with the icon at the top."))
    {
        SetAutoSavePresets(!AutoSavePresets());
        // From here on every change saves as it happens, so changes that were waiting are saved too.
        if (AutoSavePresets() && m.unsaved)
            SavePreset();
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    Heading("DISPLAY");
    MenuSizeRow();
    if (SwitchRow("keep_effects", KeepEffectsVisible(), "Keep effects visible when another window is in front",
                  "Effects stay over the game while another window, such as a chat or a browser, is in front of it."))
        SetKeepEffectsVisible(!KeepEffectsVisible());

    ImGui::Dummy(ImVec2(0, S(14)));
    Heading("UPDATES");
    if (SwitchRow("update_checks", UpdateChecksEnabled(), "Check for updates", "Asks GitHub for a newer version when Unishade starts. Applies from the next start."))
        SetUpdateChecksEnabled(!UpdateChecksEnabled());
}

void SettingsTab()
{
    const int clicked = Segmented("page", { "General", "Performance", "Shortcuts" }, static_cast<int>(m.settingsPage), kBorder);
    if (clicked >= 0)
    {
        // A shortcut left waiting for keys on its page would take the next key pressed on another.
        StopCapture();
        m.settingsPage = static_cast<SettingsPage>(clicked);
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    switch (m.settingsPage)
    {
    case SettingsPage::General: GeneralSettings(); break;
    case SettingsPage::Performance: PerformanceSettings(); break;
    case SettingsPage::Shortcuts: ShortcutSettings(); break;
    }
}

// Asks before resetting what cannot be got back, such as an effect's settings with auto-save on.
void DeleteShared();

void ConfirmDialog()
{
    if (!BeginDialog("##confirm", m.openConfirmPopup, 380))
        return;
    if (m.confirm == Confirmation::ResetEffect)
        DialogText("Reset every setting of " + m.confirmEffect + "?",
                   AutoSavePresets() ? "They go back to their defaults and the preset is saved." : "They go back to their defaults.");
    else if (m.confirm == Confirmation::DeleteShared)
        DialogText("Delete " + m.browse.deleteName + "?", "It leaves Browse for everyone, and its saves are gone.");
    else
        DialogText("Reset every shortcut?", "They go back to the keys Unishade starts with.");
    const int clicked = DialogButtons({ "Cancel", m.confirm == Confirmation::DeleteShared ? "Delete" : "Reset" });
    if (clicked == 1)
    {
        if (m.confirm == Confirmation::ResetEffect)
            ResetEffect(m.confirmEffect);
        else if (m.confirm == Confirmation::DeleteShared)
            DeleteShared();
        else
            m.shortcutError = ChangeHotkeys(DefaultHotkeys());
    }
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Status

// Wraps a notice at the edge of the window, with the web addresses in it as links. Wrapped lines start where the
// first one does.
void NoticeText(const std::string& text)
{
    if (text.find("http://") == std::string::npos && text.find("https://") == std::string::npos)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
        ImGui::PopTextWrapPos();
        return;
    }
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float space = ImGui::CalcTextSize(" ").x;
    ImGui::BeginGroup();
    bool first = true;
    int index = 0;
    for (size_t start = 0; start < text.size(); ++index)
    {
        const size_t end = std::min(text.find(' ', start), text.size());
        std::string word = text.substr(start, end - start);
        start = end + 1;
        if (word.empty())
            continue;
        const size_t address = WebAddressLength(word);
        std::string after;
        if (address)
        {
            after = word.substr(address);
            word.resize(address);
        }
        if (!first && ImGui::GetItemRectMax().x + space + ImGui::CalcTextSize((word + after).c_str()).x <= right)
            ImGui::SameLine(0, space);
        first = false;
        if (address)
        {
            ImGui::PushID(index);
            if (Link(word.c_str()))
                ShellOpen(Wide(word));
            ImGui::PopID();
            if (!after.empty())
            {
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(after.c_str());
            }
        }
        else
            ImGui::TextUnformatted(word.c_str());
    }
    ImGui::EndGroup();
}

void StatusTab()
{
    const Update& update = m.update;
    if (!update.version.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(theme::kAccent, 30));
        ImGui::PushStyleColor(ImGuiCol_Border, Color(theme::kAccent, 120));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
        ImGui::BeginChild("update", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        Text("Unishade " + Utf8(update.version) + " is available", kText, 14.5f);
        Text("Download the new Setup and run it. Your presets and settings stay.", kDim, 13);
        if (Button("Download", ImVec2(S(110), S(30)), true))
            ShellOpen(update.url);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0, S(2)));
    }

    const float x = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    Heading("STATUS");
    if (!m.notices.empty())
    {
        // The log keeps them.
        PushSize(12);
        const char* dismiss = "Dismiss all";
        ImGui::SameLine(x + width - ImGui::CalcTextSize(dismiss).x);
        if (Link(dismiss, kDim))
            ClearNotices();
        ImGui::PopFont();
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PushSize(13.5f);
    for (size_t i = 0; i < m.notices.size(); ++i)
    {
        const MenuNotice& notice = m.notices[i];
        const ImVec2 start = ImGui::GetCursorScreenPos();
        NoticeIcon(draw, start + ImVec2(S(8), ImGui::GetFontSize() / 2 + S(1)), notice.level);
        ImGui::SetCursorScreenPos(start + ImVec2(S(26), 0));
        ImGui::PushID(static_cast<int>(i));
        NoticeText(notice.text);
        ImGui::PopID();
    }
    if (m.notices.empty())
        ImGui::TextDisabled("No messages.");
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, S(4)));
    PushSize(13.5f);
    if (Link("Open log"))
        ShellOpen(LogPath());
    ImGui::SameLine(0, S(16));
    if (Link("Get help on Discord"))
        ShellOpen(kHelpUrl);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(6)));
    Text("Unishade " UNISHADE_VERSION ". Effects run on ReShade by crosire.", kDim, 12.5f);
}

// Browse

// The game is looked up again when the tab shows after this long, since Roblox moves between experiences.
constexpr ULONGLONG kResolveInterval = 30000;
// Screenshots are decoded at most this wide, about twice what the menu shows them at.
constexpr UINT kThumbnailWidth = 480;
constexpr UINT kScreenshotWidth = 960;
// ReShade only takes the picture before the effects while they are on, so publishing waits this long for it.
constexpr ULONGLONG kCaptureWait = 1000;

// Calls to the presets API and screenshot downloads run one after another on a thread of their own, so the frame
// never waits for the server. A job returns what to do with its answer, which runs on the menu's thread before the
// next frame. Never destroyed, since a call may still be waiting for the server when the host exits.
struct SharingQueue
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<std::function<void()>()>> jobs;
    std::vector<std::function<void()>> answers;
    bool started = false;
    // Never set. Calls give up on their own after the server's timeouts.
    std::atomic<bool> cancel = false;
};
SharingQueue& sharingQueue = *new SharingQueue;

void Answer(std::function<void()> answer)
{
    std::lock_guard lock(sharingQueue.mutex);
    sharingQueue.answers.push_back(std::move(answer));
}

void SharingThread()
{
    // WIC decodes the screenshots shown and encodes the ones published.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;)
    {
        std::function<std::function<void()>()> job;
        {
            std::unique_lock lock(sharingQueue.mutex);
            sharingQueue.wake.wait(lock, [] { return !sharingQueue.jobs.empty(); });
            job = std::move(sharingQueue.jobs.front());
            sharingQueue.jobs.pop_front();
        }
        std::function<void()> answer;
        try
        {
            answer = job();
        }
        catch (const std::exception& e)
        {
            answer = [message = std::string(e.what())] { ShowToast(message); };
        }
        if (answer)
            Answer(std::move(answer));
    }
}

void Share(std::function<std::function<void()>()> job)
{
    {
        std::lock_guard lock(sharingQueue.mutex);
        sharingQueue.jobs.push_back(std::move(job));
        if (!std::exchange(sharingQueue.started, true))
            std::thread(SharingThread).detach();
    }
    sharingQueue.wake.notify_one();
}

void TakeSharingAnswers()
{
    for (Texture& texture : m.browse.retired)
        DestroyTexture(texture);
    m.browse.retired.clear();
    std::vector<std::function<void()>> answers;
    {
        std::lock_guard lock(sharingQueue.mutex);
        answers.swap(sharingQueue.answers);
    }
    for (const std::function<void()>& answer : answers)
        answer();
}

// What went wrong in a call, and whether the server no longer knows the sign-in.
struct Failure
{
    std::string message;
    bool signedOut = false;
};

template <typename F>
Failure Attempt(F&& call)
{
    try
    {
        call();
        return {};
    }
    catch (const sharing::Refused& refused)
    {
        return { refused.what(), refused.status == 401 };
    }
    catch (const std::exception& e)
    {
        return { e.what() };
    }
}

void ForgetToken()
{
    m.browse.token.clear();
    m.browse.account.clear();
    SetSharingToken({});
}

std::string ConfigValue(const char* section, const char* key)
{
    char value[256] = "";
    size_t size = sizeof(value);
    return reshade::get_config_value(m.runtime, section, key, value, &size) ? value : std::string();
}

// Whether the DLSS5 add-on is loaded and not turned off with its hook point.
bool Dlss5On()
{
    return GetModuleHandleW(kDlssModule) && ConfigValue("RENODX-DLSS", "DirectNeuralRenderingHookPoint") != "0";
}

// The DLSS5 settings to share with a preset, or none when DLSS5 is off.
sharing::Settings Dlss5Settings()
{
    sharing::Settings settings;
    if (!Dlss5On())
        return settings;
    for (const char* key : kDlss5Keys)
        if (std::string value = ConfigValue(kDlss5Section, key); !value.empty())
            settings.emplace_back(key, std::move(value));
    return settings;
}

// The file name of the game being played, which the presets API knows games by. Empty when it has exited.
std::wstring GameExecutable()
{
    try
    {
        return m.gameProcess ? ProcessExecutable(m.gameProcess).filename().wstring() : std::wstring();
    }
    catch (const std::system_error&)
    {
        return {};
    }
}

// What Unishade calls the game being played: its saved name, or its window's.
std::string GameName()
{
    if (!m.game.empty())
        return Utf8(m.game);
    return g.activeGame ? Utf8(g.activeGame->name) : std::string();
}

std::optional<int64_t> ListedGame()
{
    const Browse& b = m.browse;
    if (b.experience && !b.allOfRoblox)
        return b.experience->id;
    if (b.game)
        return b.game->id;
    return std::nullopt;
}

std::string Saves(int64_t saves)
{
    return saves == 1 ? "1 save" : std::to_string(saves) + " saves";
}

// An effect file's name without .fx.
std::string EffectName(const std::string& file)
{
    return file.size() > 3 && Lower(file.substr(file.size() - 3)) == ".fx" ? file.substr(0, file.size() - 3) : file;
}

// Text cut between characters to at most length bytes, with an ellipsis when it was cut.
std::string Shortened(const std::string& text, size_t length)
{
    if (text.size() <= length)
        return text;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        --length;
    return text.substr(0, length) + "...";
}

std::string Trimmed(const char* text)
{
    std::string trimmed = text;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
    return trimmed;
}

// Pictures

struct Decoded
{
    Pixels pixels;
    UINT width = 0;
    UINT height = 0;
};

// Decodes a screenshot at most maxWidth wide, as RGBA. Empty when it is not a picture WIC can read.
Decoded DecodePicture(const std::string& data, UINT maxWidth)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> converter;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data.data())), static_cast<DWORD>(data.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(frame->GetSize(&width, &height)) || !width || !height)
        return {};
    const UINT targetWidth = std::min(width, maxWidth);
    const UINT targetHeight = std::max(1u, static_cast<UINT>(static_cast<uint64_t>(height) * targetWidth / width));
    auto pixels = std::make_shared<std::vector<BYTE>>(static_cast<size_t>(targetWidth) * targetHeight * 4);
    if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(frame.get(), targetWidth, targetHeight, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->CopyPixels(nullptr, targetWidth * 4, static_cast<UINT>(pixels->size()), pixels->data())))
        return {};
    return { pixels, targetWidth, targetHeight };
}

// A frame as a JPEG to publish. The swap chain's alpha means nothing, so it is left out.
std::string EncodeJpeg(const Frame& frame)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::com_ptr<IWICBitmap> bitmap;
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::com_ptr<IStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::com_ptr<IWICBitmapFrameEncode> target;
    winrt::com_ptr<IPropertyBag2> options;
    stream.attach(SHCreateMemStream(nullptr, 0));
    PROPBAG2 quality{};
    quality.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{};
    value.vt = VT_R4;
    value.fltVal = 0.92f;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateBitmapFromMemory(frame.width, frame.height, GUID_WICPixelFormat32bppBGRA, frame.width * 4,
                                               static_cast<UINT>(frame.pixels.size()), const_cast<BYTE*>(frame.pixels.data()), bitmap.put())) ||
        FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(target.put(), options.put())) ||
        FAILED(options->Write(1, &quality, &value)) || FAILED(target->Initialize(options.get())) ||
        FAILED(target->SetSize(frame.width, frame.height)) || FAILED(target->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR ||
        FAILED(target->WriteSource(converter.get(), nullptr)) || FAILED(target->Commit()) || FAILED(encoder->Commit()))
        throw std::runtime_error("Unishade could not save the screenshots.");
    STATSTG stat{};
    const LARGE_INTEGER start{};
    std::string data;
    ULONG read = 0;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)))
        throw std::runtime_error("Unishade could not save the screenshots.");
    data.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    if (FAILED(stream->Read(data.data(), static_cast<ULONG>(data.size()), &read)) || read != data.size())
        throw std::runtime_error("Unishade could not save the screenshots.");
    return data;
}

// The back buffer as BGRA rows. Unishade's swap chain is BGRA, and ReShade hands its pixels over as they are.
Frame CaptureFrame(effect_runtime* runtime)
{
    Frame frame;
    const resource_desc desc = runtime->get_device()->get_resource_desc(runtime->get_current_back_buffer());
    if (format_to_default_typed(desc.texture.format, 0) != format::b8g8r8a8_unorm)
        return frame;
    runtime->get_screenshot_width_and_height(&frame.width, &frame.height);
    frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4);
    if (!runtime->capture_screenshot(frame.pixels.data()))
        frame.pixels.clear();
    return frame;
}

void ForgetPictures(const std::function<bool(const std::string&)>& which)
{
    for (auto picture = m.browse.pictures.begin(); picture != m.browse.pictures.end();)
        if (which(picture->first))
        {
            m.browse.retired.push_back(picture->second.texture);
            picture = m.browse.pictures.erase(picture);
        }
        else
            ++picture;
}

// A shared preset's screenshot, downloaded the first time it is asked for. Its texture stays empty until it arrives.
const SharedPicture& Picture(int64_t id, const char* kind, const std::string& url, UINT maxWidth)
{
    const std::string key = std::to_string(id) + "/" + kind;
    SharedPicture& picture = m.browse.pictures[key];
    if (!picture.requested && !url.empty())
    {
        picture.requested = true;
        Share([key, url, maxWidth] {
            Decoded decoded;
            try
            {
                decoded = DecodePicture(sharing::Image(url, sharingQueue.cancel), maxWidth);
            }
            catch (const std::exception& e)
            {
                Log(LogLevel::Info, L"Could not download a shared preset's screenshot: %hs", e.what());
            }
            return std::function<void()>([key, decoded] {
                // Gone when the list changed meanwhile.
                if (const auto found = m.browse.pictures.find(key); found != m.browse.pictures.end())
                {
                    found->second.pixels = decoded.pixels;
                    found->second.width = decoded.width;
                    found->second.height = decoded.height;
                }
            });
        });
    }
    if (picture.pixels && picture.texture.source != picture.pixels)
        CreatePictureTexture(picture.texture, picture.pixels, picture.width, picture.height);
    return picture;
}

// Fills min to max with a picture, cutting its sides or its top and bottom to keep its shape, or with a dark box
// until it arrives.
void DrawPicture(ImDrawList* draw, const SharedPicture& picture, ImVec2 min, ImVec2 max, ImDrawFlags corners)
{
    if (!picture.texture.view.handle || !picture.width || !picture.height)
    {
        draw->AddRectFilled(min, max, kInset, S(8), corners);
        return;
    }
    const float box = (max.x - min.x) / std::max(max.y - min.y, 1.0f);
    const float shape = static_cast<float>(picture.width) / static_cast<float>(picture.height);
    ImVec2 uv0(0, 0);
    ImVec2 uv1(1, 1);
    if (shape > box)
    {
        uv0.x = (1 - box / shape) / 2;
        uv1.x = 1 - uv0.x;
    }
    else
    {
        uv0.y = (1 - shape / box) / 2;
        uv1.y = 1 - uv0.y;
    }
    draw->AddImageRounded(ImTextureRef(picture.texture.view.handle), min, max, uv0, uv1, IM_COL32_WHITE, S(8), corners);
}

// Requests

void LoadPresets(bool more)
{
    Browse& b = m.browse;
    if (!more)
    {
        ++b.generation;
        b.presets.clear();
        b.next.reset();
        ForgetPictures([](const std::string& key) { return key.ends_with("/thumbnail"); });
    }
    b.error.clear();
    const std::optional<int64_t> game = ListedGame();
    b.loading = game.has_value();
    if (!game)
        return;
    const int64_t offset = more && b.next ? *b.next : 0;
    Share([game = *game, effect = b.effect, newest = b.newest, offset, generation = b.generation] {
        sharing::Page page;
        const Failure failure = Attempt([&] { page = sharing::List(game, effect, newest, offset, sharingQueue.cancel); });
        return std::function<void()>([page, failure, generation] {
            Browse& b = m.browse;
            if (generation != b.generation)
                return;
            b.loading = false;
            b.error = failure.message;
            b.presets.insert(b.presets.end(), page.presets.begin(), page.presets.end());
            b.next = page.next;
        });
    });
}

// Looks up the game being played, and the Roblox experience in it, when the tab shows for a new game or after a while.
void ResolveGame()
{
    Browse& b = m.browse;
    const ULONGLONG now = GetTickCount64();
    if (b.resolving || (b.resolved && b.resolvedProcess == m.gameProcess && now - b.resolvedAt < kResolveInterval))
        return;
    const std::wstring executable = GameExecutable();
    if (executable.empty())
    {
        b.resolved = true;
        b.resolvedAt = now;
        b.resolvedProcess = m.gameProcess;
        b.error = "Unishade can't tell which game this is.";
        return;
    }
    b.resolving = true;
    Share([executable, process = m.gameProcess] {
        // Only Roblox's Windows client writes the logs the place comes from.
        const std::optional<int64_t> place = _wcsicmp(executable.c_str(), L"RobloxPlayerBeta.exe") == 0 ? sharing::RobloxPlace() : std::nullopt;
        sharing::Place found;
        const Failure failure = Attempt([&] { found = sharing::Resolve(executable, place, sharingQueue.cancel); });
        return std::function<void()>([process, place, found, failure] {
            Browse& b = m.browse;
            const auto id = [](const std::optional<sharing::Game>& game) { return game ? game->id : 0; };
            const bool changed = !b.resolved || b.resolvedProcess != process || id(b.game) != id(found.game) || id(b.experience) != id(found.experience);
            const bool failedBefore = !b.error.empty();
            b.resolving = false;
            b.resolved = true;
            b.resolvedAt = GetTickCount64();
            b.resolvedProcess = process;
            if (!failure.message.empty())
            {
                b.error = failure.message;
                return;
            }
            b.game = found.game;
            b.experience = found.experience;
            b.place = found.experience ? place : std::nullopt;
            if (!changed && !failedBefore)
                return;
            b.allOfRoblox = false;
            b.effect.clear();
            if (b.view == BrowseView::Preset)
                b.view = BrowseView::List;
            LoadPresets(false);
        });
    });
}

void ClosePreset()
{
    Browse& b = m.browse;
    const std::string prefix = std::to_string(b.openId) + "/";
    ForgetPictures([&](const std::string& key) { return key.starts_with(prefix) && !key.ends_with("/thumbnail"); });
    b.open.reset();
    b.openId = 0;
    b.view = BrowseView::List;
}

void OpenPreset(int64_t id)
{
    Browse& b = m.browse;
    b.view = BrowseView::Preset;
    b.open.reset();
    b.openId = id;
    b.showBefore = false;
    b.useDlss5 = true;
    Share([id] {
        sharing::Preset preset;
        const Failure failure = Attempt([&] { preset = sharing::Get(id, sharingQueue.cancel); });
        return std::function<void()>([id, preset, failure] {
            Browse& b = m.browse;
            if (b.view != BrowseView::Preset || b.openId != id)
                return;
            if (!failure.message.empty())
            {
                ShowToast(failure.message);
                ClosePreset();
                return;
            }
            b.open = preset;
        });
    });
}

void ReadAccount()
{
    Browse& b = m.browse;
    b.accountRead = true;
    b.token = SharingToken();
    if (b.token.empty())
        return;
    Share([token = b.token] {
        std::string name;
        const Failure failure = Attempt([&] { name = sharing::AccountName(token, sharingQueue.cancel); });
        // The list says when the server can't be reached.
        return std::function<void()>([token, name, failure] {
            if (m.browse.token != token)
                return;
            if (failure.signedOut)
                ForgetToken();
            else
                m.browse.account = name;
        });
    });
}

// Opens Discord's sign-in in the browser and waits for it on a thread of its own, since it can take minutes. While it
// waits, the page opens again.
void SignIn()
{
    if (m.browse.signingIn)
    {
        if (!m.browse.signInUrl.empty())
            ShellOpen(Wide(m.browse.signInUrl));
        return;
    }
    m.browse.signingIn = true;
    m.browse.signInUrl.clear();
    std::thread([] {
        std::function<void()> answer = [] { m.browse.signingIn = false; };
        try
        {
            const auto [url, poll] = sharing::StartSignIn(sharingQueue.cancel);
            ShellOpen(Wide(url));
            Answer([url] { m.browse.signInUrl = url; });
            // The API forgets a sign-in after 10 minutes.
            for (int attempt = 0; attempt < 300; ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (const std::optional<sharing::Account> account = sharing::PollSignIn(poll, sharingQueue.cancel))
                {
                    answer = [account = *account] {
                        Browse& b = m.browse;
                        b.signingIn = false;
                        b.token = account.token;
                        b.account = account.name;
                        SetSharingToken(account.token);
                        ShowToast("Signed in as " + account.name);
                    };
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            answer = [message = std::string(e.what())] {
                m.browse.signingIn = false;
                ShowToast(message);
            };
        }
        Answer(std::move(answer));
    }).detach();
}

void SignOut()
{
    Browse& b = m.browse;
    // The token is forgotten here either way.
    Share([token = b.token] {
        Attempt([&] { sharing::SignOut(token, sharingQueue.cancel); });
        return std::function<void()>();
    });
    ForgetToken();
    if (b.view == BrowseView::Own)
        b.view = BrowseView::List;
}

void LoadOwn()
{
    Browse& b = m.browse;
    b.ownLoading = true;
    Share([token = b.token] {
        std::vector<sharing::OwnPreset> own;
        const Failure failure = Attempt([&] { own = sharing::OwnPresets(token, sharingQueue.cancel); });
        return std::function<void()>([own, failure] {
            Browse& b = m.browse;
            b.ownLoading = false;
            if (failure.signedOut)
                ForgetToken();
            if (!failure.message.empty())
                ShowToast(failure.message);
            else
                b.own = own;
        });
    });
}

void DeleteShared()
{
    Browse& b = m.browse;
    Share([token = b.token, id = b.deleteId, name = b.deleteName] {
        const Failure failure = Attempt([&] { sharing::Delete(token, id, sharingQueue.cancel); });
        return std::function<void()>([failure, name] {
            if (failure.signedOut)
                ForgetToken();
            ShowToast(failure.message.empty() ? "Deleted " + name : failure.message);
            if (m.browse.view == BrowseView::Own && !m.browse.token.empty())
                LoadOwn();
        });
    });
}

// The name a shared preset's file gets, or one Windows allows when its own isn't.
std::wstring SharedFileName(const sharing::Preset& preset)
{
    std::wstring name = Wide(preset.name);
    if (!NameProblem(name).empty())
        name = L"Shared preset " + std::to_wstring(preset.id);
    return name;
}

bool Trying(const sharing::Preset& preset)
{
    return m.browse.tryingId == preset.id && SamePath(m.current, m.browse.tryingPath);
}

// Switches to a shared preset from a file outside the presets folder, so it is not kept unless Keep copies it in.
void TryPreset(const sharing::Preset& preset)
{
    std::error_code error;
    fs::path folder = fs::temp_directory_path(error) / L"Unishade" / std::to_wstring(preset.id);
    if (!error)
        fs::create_directories(folder, error);
    // Spelled the way ReShade spells the preset it loads, which resolves links and short names.
    if (!error)
        folder = fs::canonical(folder, error);
    const fs::path path = folder / (SharedFileName(preset) + L".ini");
    if (!error)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << preset.ini;
        file.close();
        if (!file)
            error = std::make_error_code(std::errc::io_error);
    }
    if (error)
    {
        ShowToast("Windows could not save the preset to try it");
        return;
    }
    m.browse.tryingId = preset.id;
    m.browse.tryingPath = path;
    m.pendingPreset = path;
}

// Saves a shared preset to the game's folder and switches to it, with its DLSS5 settings when asked to.
void KeepPreset(const sharing::Preset& preset)
{
    Browse& b = m.browse;
    const fs::path folder = NewPresetFolder();
    const std::wstring name = SharedFileName(preset);
    std::error_code error;
    fs::path target = folder / (name + L".ini");
    for (int copy = 2; fs::exists(target, error); ++copy)
        target = folder / (name + L" (" + std::to_wstring(copy) + L").ini");
    fs::create_directories(folder, error);
    if (!error && Trying(preset))
    {
        // Changes made while trying it come along, as with Save as new.
        m.runtime->export_current_preset(Utf8(target.wstring()).c_str());
        m.pendingKeepsEdits = true;
    }
    else if (!error)
    {
        std::ofstream file(target, std::ios::binary);
        file << preset.ini;
        file.close();
        if (!file)
            error = std::make_error_code(std::errc::io_error);
    }
    if (error || !fs::exists(target, error))
    {
        m.pendingKeepsEdits = false;
        ShowToast("Windows could not save the preset");
        return;
    }
    m.pendingPreset = target;
    RequestScan();

    std::string message = "Kept " + Utf8(target.stem().wstring());
    if (b.useDlss5 && !preset.dlss5.empty() && GetModuleHandleW(kDlssModule))
    {
        for (const auto& [key, value] : preset.dlss5)
            if (std::any_of(std::begin(kDlss5Keys), std::end(kDlss5Keys), [&key](const char* shared) { return key == shared; }))
                reshade::set_config_value(m.runtime, kDlss5Section, key.c_str(), value.c_str());
        message += ". Its DLSS5 settings apply after Unishade restarts";
    }
    ShowToast(message, {}, 5000);
    Share([id = preset.id] {
        // Saves only sort the list, so a failed count doesn't matter.
        Attempt([&] { sharing::CountSave(id, sharingQueue.cancel); });
        return std::function<void()>();
    });
}

void OpenPublish()
{
    Browse& b = m.browse;
    if (b.token.empty())
    {
        SignIn();
        ShowToast("Sign in with Discord in your browser, then publish", {}, 5000);
        return;
    }
    CopyText(b.publishName, Utf8(m.current.stem().wstring()));
    b.publishDescription[0] = '\0';
    b.publishForExperience = true;
    b.publishNeedsDepth = DepthEnabled();
    b.publishError.clear();
    b.published = false;
    b.openPublishPopup = true;
}

// Checks what can be checked now, then takes the screenshots in the next frame.
void StartPublish()
{
    Browse& b = m.browse;
    b.publishError.clear();
    if (Trimmed(b.publishName).empty())
    {
        b.publishError = "Give the preset a name.";
        return;
    }
    if (m.techniquesDirty || !EffectsLoaded())
    {
        b.publishError = "Wait for the effects to finish loading.";
        return;
    }
    if (const std::vector<std::string> missing = MissingEffects(m.current); !missing.empty())
    {
        std::string names;
        for (const std::string& effect : missing)
            names += (names.empty() ? "" : ", ") + effect;
        b.publishError = "It uses effects that are not installed: " + names + ".";
        return;
    }
    if (!m.runtime->get_effects_state() || m.comparing)
    {
        b.publishError = "Turn effects on, so the screenshots show them.";
        return;
    }
    // The preset as it is on screen, unsaved changes included.
    std::error_code error;
    const fs::path file = fs::temp_directory_path(error) / L"Unishade" / L"Publish.ini";
    if (!error)
        fs::create_directories(file.parent_path(), error);
    b.publishIni.clear();
    if (!error)
    {
        m.runtime->export_current_preset(Utf8(file.wstring()).c_str());
        b.publishIni = ReadText(file);
        fs::remove(file, error);
    }
    if (b.publishIni.empty())
    {
        b.publishError = "Unishade could not read the preset.";
        return;
    }
    b.publishing = true;
    b.capture = Capture::Before;
    b.captureAt = GetTickCount64();
}

// Others only have the effects Setup installs, so a preset that uses any other could not work for them.
void CheckSetupEffects(const std::string& ini)
{
    const fs::path shaders = fs::path(ExeDirectory()) / L"reshade-shaders" / L"Shaders";
    std::set<std::string> installed;
    std::error_code error;
    for (fs::recursive_directory_iterator entry(shaders, error), end; !error && entry != end; entry.increment(error))
        installed.insert(Lower(Utf8(entry->path().filename().wstring())));
    for (const std::string& effect : PresetEffectFiles(PresetIni(ini)))
        if (!installed.count(Lower(effect)))
            throw std::runtime_error(effect + " is not one of the effects Setup installs, so others couldn't use the preset.");
}

// Sends the preset with the screenshots taken for it.
void FinishPublish(Frame after)
{
    Browse& b = m.browse;
    Frame before = std::exchange(b.before, {});
    if (before.pixels.empty() || after.pixels.empty())
    {
        b.publishing = false;
        b.publishError = "Unishade could not take the screenshots.";
        return;
    }
    const sharing::Publication publication{
        .name = Trimmed(b.publishName),
        .description = Trimmed(b.publishDescription),
        .executable = Utf8(GameExecutable()),
        .gameName = GameName(),
        .place = b.publishForExperience && b.experience ? b.place : std::nullopt,
        .ini = b.publishIni,
        .needsDepth = b.publishNeedsDepth,
        .dlss5 = Dlss5Settings(),
    };
    auto frames = std::make_shared<std::pair<Frame, Frame>>(std::move(before), std::move(after));
    Share([publication, frames, token = b.token] {
        const Failure failure = Attempt([&] {
            CheckSetupEffects(publication.ini);
            sharing::Publication sent = publication;
            sent.before = EncodeJpeg(frames->first);
            sent.after = EncodeJpeg(frames->second);
            sharing::Publish(token, sent, sharingQueue.cancel);
        });
        return std::function<void()>([failure, name = publication.name] {
            Browse& b = m.browse;
            b.publishing = false;
            if (failure.signedOut)
                ForgetToken();
            if (failure.message.empty())
            {
                b.published = true;
                ShowToast("Sent " + name + " for review", {}, 5000);
                if (b.view == BrowseView::Own)
                    LoadOwn();
                return;
            }
            b.publishError = failure.message;
            if (!b.publishDialogOpen)
                ShowToast(failure.message);
        });
    });
}

void OpenReport(const sharing::Preset& preset)
{
    Browse& b = m.browse;
    if (b.token.empty())
    {
        SignIn();
        ShowToast("Sign in with Discord in your browser, then report it", {}, 5000);
        return;
    }
    b.reportId = preset.id;
    b.reportName = preset.name;
    b.reportReason[0] = '\0';
    b.openReportPopup = true;
}

void SendReport()
{
    Browse& b = m.browse;
    Share([token = b.token, id = b.reportId, reason = Trimmed(b.reportReason)] {
        const Failure failure = Attempt([&] { sharing::Report(token, id, reason, sharingQueue.cancel); });
        return std::function<void()>([failure] {
            if (failure.signedOut)
                ForgetToken();
            ShowToast(failure.message.empty() ? "Sent the report to the team" : failure.message);
        });
    });
}

// Drawing

bool BackLink()
{
    PushSize(13.5f);
    const bool clicked = Link("Back");
    ImGui::PopFont();
    return clicked;
}

bool PresetCard(const sharing::Preset& preset, float width)
{
    ImGui::PushID(static_cast<int>(preset.id));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float imageHeight = std::round(width * 9 / 16);
    const ImVec2 size(width, imageHeight + S(48));
    const bool clicked = ImGui::InvisibleButton("card", size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + size, hovered ? kCardHover : kCard, S(8));
    DrawPicture(draw, Picture(preset.id, "thumbnail", preset.thumbnail, kThumbnailWidth), start, start + ImVec2(width, imageHeight),
                ImDrawFlags_RoundCornersTop);
    draw->AddRect(start, start + size, hovered ? kBorderStrong : kBorder, S(8));

    draw->PushClipRect(start, start + size - ImVec2(S(8), 0), true);
    PushSize(14);
    draw->AddText(start + ImVec2(S(10), imageHeight + S(7)), kText, preset.name.c_str());
    ImGui::PopFont();
    PushSize(12.5f);
    const std::string saves = Saves(preset.saves);
    const float savesX = start.x + width - S(10) - ImGui::CalcTextSize(saves.c_str()).x;
    draw->AddText(ImVec2(savesX, start.y + imageHeight + S(27)), kDim, saves.c_str());
    draw->PushClipRect(start, ImVec2(savesX - S(8), start.y + size.y), true);
    draw->AddText(start + ImVec2(S(10), imageHeight + S(27)), kDim, preset.author.c_str());
    draw->PopClipRect();
    ImGui::PopFont();
    draw->PopClipRect();
    ImGui::PopID();
    return clicked;
}

// The preset's effects as small buttons that wrap. Returns the one clicked, or null.
const std::string* EffectChips(const std::vector<std::string>& effects)
{
    const std::string* clicked = nullptr;
    const float left = ImGui::GetCursorScreenPos().x;
    const float right = left + ImGui::GetContentRegionAvail().x;
    float x = left;
    float y = ImGui::GetCursorScreenPos().y;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    PushSize(13);
    const float height = ImGui::GetFontSize() + S(10);
    for (const std::string& effect : effects)
    {
        const std::string name = EffectName(effect);
        const ImVec2 size(ImGui::CalcTextSize(name.c_str()).x + S(18), height);
        if (x > left && x + size.x > right)
        {
            x = left;
            y += height + S(6);
        }
        const ImVec2 min(x, y);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(effect.c_str());
        if (ImGui::InvisibleButton("effect", size, ImGuiButtonFlags_EnableNav))
            clicked = &effect;
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        if (hovered)
            ImGui::SetTooltip("Show presets with %s", name.c_str());
        draw->AddRectFilled(min, min + size, hovered ? kCardHover : kCard, size.y / 2);
        draw->AddRect(min, min + size, kBorder, size.y / 2);
        draw->AddText(min + ImVec2(S(9), S(5)), kText, name.c_str());
        ImGui::PopID();
        x += size.x + S(6);
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(left, y + height));
    ImGui::Dummy(ImVec2(0, 0));
    return clicked;
}

void BrowseList()
{
    Browse& b = m.browse;
    if (Button(b.publishing ? "Publishing..." : "Publish", ImVec2(S(120), S(32)), true, !b.publishing))
        OpenPublish();
    if (!b.token.empty())
    {
        ImGui::SameLine(0, S(8));
        if (Button("Your presets", ImVec2(S(120), S(32))))
        {
            b.view = BrowseView::Own;
            LoadOwn();
        }
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    if (b.experience)
    {
        const std::string experience = Shortened(b.experience->name, 28);
        const int clicked = Segmented("scope", { experience.c_str(), "All of Roblox" }, b.allOfRoblox ? 1 : 0, kAccent);
        if (clicked >= 0 && (clicked == 1) != b.allOfRoblox)
        {
            b.allOfRoblox = clicked == 1;
            LoadPresets(false);
        }
    }
    const int sort = Segmented("sort", { "Popular", "New" }, b.newest ? 1 : 0, kAccent);
    if (sort >= 0 && (sort == 1) != b.newest)
    {
        b.newest = sort == 1;
        LoadPresets(false);
    }
    if (!b.effect.empty())
    {
        Text("Only presets with " + EffectName(b.effect) + ".", kDim, 13);
        ImGui::SameLine(0, S(6));
        PushSize(13);
        if (Link("Show all"))
        {
            b.effect.clear();
            LoadPresets(false);
        }
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    const std::string game = b.experience && !b.allOfRoblox ? b.experience->name : b.game ? b.game->name : GameName();
    if (!b.error.empty())
    {
        Text(b.error, kWarning, 13.5f);
        PushSize(13.5f);
        if (Link("Try again"))
        {
            if (b.game)
                LoadPresets(false);
            b.resolved = false;
        }
        ImGui::PopFont();
    }
    else if (!b.resolved || (b.loading && b.presets.empty()))
        Spinner(S(10));
    else if (b.presets.empty())
        Text(b.effect.empty() ? "Nobody has shared a preset for " + game + " yet." : "No presets for " + game + " use " + EffectName(b.effect) + ".", kDim,
             13.5f);
    else
    {
        const float gap = S(10);
        const float width = std::floor((ImGui::GetContentRegionAvail().x - gap) / 2);
        for (size_t i = 0; i < b.presets.size(); ++i)
        {
            if (i % 2)
                ImGui::SameLine(0, gap);
            if (PresetCard(b.presets[i], width))
                OpenPreset(b.presets[i].id);
        }
        if (b.next)
        {
            ImGui::Dummy(ImVec2(0, S(2)));
            if (Button(b.loading ? "Loading..." : "Show more", ImVec2(ImGui::GetContentRegionAvail().x, S(32)), false, !b.loading))
                LoadPresets(true);
        }
    }

    ImGui::Dummy(ImVec2(0, S(6)));
    PushSize(13);
    if (b.token.empty())
    {
        if (Link(b.signingIn ? "Waiting for Discord in your browser..." : "Sign in with Discord"))
            SignIn();
        if (b.signingIn && ImGui::IsItemHovered())
            ImGui::SetTooltip("Opens the sign-in page again");
    }
    else
    {
        Text(b.account.empty() ? "Signed in with Discord." : "Signed in as " + b.account + ".", kDim, 13);
        ImGui::SameLine(0, S(6));
        if (Link("Sign out"))
            SignOut();
    }
    ImGui::PopFont();
}

void PresetPage()
{
    Browse& b = m.browse;
    if (BackLink())
    {
        ClosePreset();
        return;
    }
    if (!b.open)
    {
        Spinner(S(10));
        return;
    }
    const sharing::Preset& preset = *b.open;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, std::round(width * 9 / 16));
    // Both download at once, so holding to compare doesn't wait.
    const SharedPicture& after = Picture(preset.id, "after", preset.after, kScreenshotWidth);
    const SharedPicture& before = Picture(preset.id, "before", preset.before, kScreenshotWidth);
    DrawPicture(draw, b.showBefore ? before : after, start, start + size, ImDrawFlags_RoundCornersAll);
    ImGui::Dummy(size);
    PushSize(13.5f);
    Button("Hold to compare", ImVec2(width, S(30)));
    b.showBefore = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() && !b.showBefore)
        ImGui::SetTooltip("Hold to see the screenshot without effects");
    ImGui::PopFont();

    Text(preset.name, kText, 16.5f);
    Text("by " + preset.author + ", " + Saves(preset.saves), kDim, 13);
    if (!preset.description.empty())
        Text(preset.description, kText, 13.5f);
    Heading("EFFECTS");
    if (const std::string* effect = EffectChips(preset.effects))
    {
        const std::string chosen = *effect;
        ClosePreset();
        b.effect = chosen;
        LoadPresets(false);
        return;
    }
    if (preset.needsDepth)
        Text(DepthEnabled() ? "Needs depth estimation." : "Needs depth estimation, which isn't running. Add it in Unishade Setup.",
             DepthEnabled() ? kDim : kWarning, 13);
    if (!preset.dlss5.empty())
    {
        if (!GetModuleHandleW(kDlssModule))
            Text("Made with DLSS5, so it looks different without it.", kDim, 13);
        else if (SwitchRow("dlss5", b.useDlss5, "Use its DLSS5 settings", "They apply after Unishade restarts."))
            b.useDlss5 = !b.useDlss5;
    }

    ImGui::Dummy(ImVec2(0, S(2)));
    const bool trying = Trying(preset);
    if (Button(trying ? "Trying" : "Try", ImVec2(S(100), S(32)), false, !trying))
        TryPreset(preset);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Switches to it without adding it to your presets");
    ImGui::SameLine(0, S(8));
    if (Button("Keep", ImVec2(S(100), S(32)), true))
        KeepPreset(preset);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds it to your presets for %s and switches to it", GameName().c_str());
    ImGui::Dummy(ImVec2(0, S(4)));
    PushSize(13);
    if (Link("Report", kDim))
        OpenReport(preset);
    ImGui::PopFont();
}

void OwnPresetsPage()
{
    Browse& b = m.browse;
    if (BackLink())
    {
        b.view = BrowseView::List;
        return;
    }
    Text("Your presets", kText, 16.5f);
    if (b.ownLoading && b.own.empty())
    {
        Spinner(S(10));
        return;
    }
    if (b.own.empty())
    {
        Text("You haven't published a preset yet.", kDim, 13.5f);
        return;
    }
    for (const sharing::OwnPreset& preset : b.own)
    {
        ImGui::PushID(static_cast<int>(preset.id));
        ImGui::Dummy(ImVec2(0, S(2)));
        Text(preset.name, kText, 14.5f);
        if (preset.status == "approved")
            Text(preset.game + ". Shared, " + Saves(preset.saves) + ".", kDim, 13);
        else if (preset.status == "pending")
            Text(preset.game + ". Waiting for review.", kDim, 13);
        else
            Text(preset.game + ". Rejected" + (preset.rejectReason.empty() ? "." : ": " + preset.rejectReason), kWarning, 13);
        PushSize(13);
        if (Link("Delete", kDim))
        {
            b.deleteId = preset.id;
            b.deleteName = preset.name;
            m.confirm = Confirmation::DeleteShared;
            m.openConfirmPopup = true;
        }
        ImGui::PopFont();
        ImGui::PopID();
    }
}

void BrowseTab()
{
    Browse& b = m.browse;
    if (!b.accountRead)
        ReadAccount();
    if (!m.gameProcess)
    {
        Text("Browse shows presets for the game Unishade runs on.", kDim, 13.5f);
        return;
    }
    ResolveGame();
    switch (b.view)
    {
    case BrowseView::List: BrowseList(); break;
    case BrowseView::Preset: PresetPage(); break;
    case BrowseView::Own: OwnPresetsPage(); break;
    }
}

void PublishDialog()
{
    Browse& b = m.browse;
    b.publishDialogOpen = BeginDialog("##publish", b.openPublishPopup, 420);
    if (!b.publishDialogOpen)
        return;
    DialogText("Publish " + Utf8(m.current.stem().wstring()), "The team reviews it before others can find it.");
    PushSize(14.5f);
    Text("Name", kDim, 12.5f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##name", b.publishName, sizeof(b.publishName));
    Text("Description", kDim, 12.5f);
    ImGui::InputTextMultiline("##description", b.publishDescription, sizeof(b.publishDescription), ImVec2(-FLT_MIN, S(64)));
    if (b.experience)
        ImGui::Checkbox(("Only for " + Shortened(b.experience->name, 40)).c_str(), &b.publishForExperience);
    ImGui::Checkbox("Needs depth estimation", &b.publishNeedsDepth);
    ImGui::PopFont();
    if (Dlss5On())
        Text("Your DLSS5 settings are shared with it.", kDim, 13);
    Text("Its screenshots are the game behind the menu, with and without effects.", kDim, 13);
    if (!b.publishError.empty())
        Text(b.publishError, kError, 13.5f);
    const int clicked = DialogButtons({ "Cancel", b.publishing ? "Publishing..." : "Publish" }, !b.publishing);
    if (clicked == 1)
        StartPublish();
    if (b.published || clicked == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        b.published = false;
        ImGui::CloseCurrentPopup();
    }
    EndDialog();
}

void ReportDialog()
{
    Browse& b = m.browse;
    if (!BeginDialog("##report", b.openReportPopup, 400))
        return;
    DialogText("Report " + b.reportName, "Tell the team what's wrong with it.");
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    PushSize(14.5f);
    ImGui::InputTextMultiline("##reason", b.reportReason, sizeof(b.reportReason), ImVec2(-FLT_MIN, S(64)));
    ImGui::PopFont();
    const int clicked = DialogButtons({ "Cancel", "Send" }, !Trimmed(b.reportReason).empty());
    if (clicked == 1)
        SendReport();
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Frame

void Header(ImVec2 origin, float width)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float logo = std::round(S(38));
    const ImVec2 logoPosition = origin + ImVec2(std::round(S(kPadding)), std::round(S(18)));
    if (const uint64_t texture = HeaderLogo())
        draw->AddImage(ImTextureRef(texture), logoPosition, logoPosition + ImVec2(logo, logo));
    const float textX = S(kPadding) + logo + S(12);
    PushSize(16.5f);
    draw->AddText(origin + ImVec2(textX, S(17)), kText, "Unishade");
    const float titleWidth = ImGui::CalcTextSize("Unishade").x;
    ImGui::PopFont();
    // The right side holds the effects switch, its label and, with auto-save off, the save icon.
    PushSize(13);
    const ImVec2 labelSize = ImGui::CalcTextSize("Effects");
    const float switchX = width - S(kPadding) - S(32);
    const float labelX = switchX - S(8) - labelSize.x;
    const ImVec2 saveSize(S(30), S(30));
    const float saveX = labelX - S(14) - saveSize.x;

    const std::string preset = Utf8(m.current.stem().wstring());
    const ImVec2 nameSize = ImGui::CalcTextSize(preset.c_str());
    draw->PushClipRect(origin, origin + ImVec2((AutoSavePresets() ? labelX : saveX) - S(12), S(kHeader)), true);
    draw->AddText(origin + ImVec2(textX, S(39)), kDim, preset.c_str());
    if (m.unsaved)
        draw->AddCircleFilled(origin + ImVec2(textX + nameSize.x + S(7), S(39) + ImGui::GetFontSize() / 2 + S(1)), S(3), kWarning);
    draw->PopClipRect();
    ImGui::PopFont();

    if (!AutoSavePresets())
    {
        const ImVec2 start = origin + ImVec2(saveX, S(22));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton("save", saveSize, ImGuiButtonFlags_EnableNav) && m.unsaved)
            SavePreset();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(m.unsaved ? "Save changes to %s" : "No unsaved changes in %s", preset.c_str());
            if (m.unsaved)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                draw->AddRectFilled(start, start + saveSize, kBorder, S(6));
            }
        }
        SaveIcon(draw, start + saveSize * 0.5f, S(14), m.unsaved ? kAccentHover : kBorderStrong);
    }

    if (!m.update.version.empty())
    {
        PushSize(11.5f);
        const char* tag = "UPDATE";
        const ImVec2 tagSize = ImGui::CalcTextSize(tag) + ImVec2(S(12), S(5));
        const ImVec2 tagStart = origin + ImVec2(textX + titleWidth + S(8), S(19));
        ImGui::SetCursorScreenPos(tagStart);
        if (ImGui::InvisibleButton("update", tagSize, ImGuiButtonFlags_EnableNav))
            m.tab = Tab::Status;
        HandOnHover();
        draw->AddRectFilled(tagStart, tagStart + tagSize, Color(theme::kAccent, ImGui::IsItemHovered() ? 90 : 50), tagSize.y / 2);
        draw->AddText(tagStart + ImVec2(S(6), S(2.5f)), kAccentHover, tag);
        ImGui::PopFont();
    }

    // Effects on and off for everything, like ReShade's own shortcut.
    const bool on = m.runtime->get_effects_state() || m.comparing;
    ImGui::SetCursorScreenPos(origin + ImVec2(switchX, S(28)));
    if (Switch("effects", on) && !m.comparing)
        m.runtime->set_effects_state(!on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(on ? "Turn all effects off" : "Turn effects back on");
    PushSize(13);
    draw->AddText(origin + ImVec2(labelX, S(28) + (S(18) - labelSize.y) / 2), kDim, "Effects");
    ImGui::PopFont();

    Rainbow(draw, origin + ImVec2(0, S(kHeader) - S(2)), origin + ImVec2(width, S(kHeader)));
}

void Tabs(ImVec2 origin, float width)
{
    struct Entry
    {
        const char* name;
        // Empty for DLSS5. RenoDX's add-on only draws its settings in ReShade's menu, so its tab opens that.
        std::optional<Tab> tab;
    };
    Entry entries[6];
    int count = 0;
    entries[count++] = { "Presets", Tab::Presets };
    if (sharing::Available())
        entries[count++] = { "Browse", Tab::Browse };
    entries[count++] = { "Effects", Tab::Effects };
    if (GetModuleHandleW(kDlssModule))
        entries[count++] = { "DLSS5", std::nullopt };
    entries[count++] = { "Settings", Tab::Settings };
    entries[count++] = { "Status", Tab::Status };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tabWidth = (width - S(kPadding) * 2) / count;
    const bool problems = m.problems;
    PushSize(14);
    for (int i = 0; i < count; ++i)
    {
        const Entry& entry = entries[i];
        const ImVec2 start = origin + ImVec2(S(kPadding) + i * tabWidth, S(kHeader));
        ImGui::SetCursorScreenPos(start);
        if (ImGui::InvisibleButton(entry.name, ImVec2(tabWidth, S(kTabs)), ImGuiButtonFlags_EnableNav))
        {
            if (entry.tab)
                m.tab = *entry.tab;
            else
                m.openDlss = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        const bool active = entry.tab == m.tab;
        const ImVec2 text = ImGui::CalcTextSize(entry.name);
        const ImVec2 textStart = start + ImVec2((tabWidth - text.x) / 2, (S(kTabs) - text.y) / 2);
        draw->AddText(textStart, active || hovered ? kText : kDim, entry.name);
        if (active)
            draw->AddRectFilled(ImVec2(textStart.x - S(6), start.y + S(kTabs) - S(2)), ImVec2(textStart.x + text.x + S(6), start.y + S(kTabs)),
                                kAccent, S(1));
        if (entry.tab == Tab::Status && problems)
            draw->AddCircleFilled(textStart + ImVec2(text.x + S(6), S(3)), S(3), kWarning);
    }
    ImGui::PopFont();
    draw->AddLine(origin + ImVec2(0, S(kHeader + kTabs)), origin + ImVec2(width, S(kHeader + kTabs)), kBorder);
}

// Effects stay off while the compare button is held.
void Compare(bool holding)
{
    // The compare shortcut, while held, decides instead.
    if (holding == m.comparing || m.compareKey)
        return;
    if (holding)
    {
        m.effectsBeforeCompare = m.runtime->get_effects_state();
        m.runtime->set_effects_state(false);
    }
    else
        m.runtime->set_effects_state(m.effectsBeforeCompare);
    m.comparing = holding;
}

void Footer(ImVec2 origin, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float top = size.y - S(kFooter);
    draw->AddLine(origin + ImVec2(0, top), origin + ImVec2(size.x, top), kBorder);
    const float buttonY = top + (S(kFooter) - S(34)) / 2;

    ImGui::SetCursorScreenPos(origin + ImVec2(S(kPadding), buttonY));
    PushSize(14);
    Button("Compare", ImVec2(S(100), S(34)));
    Compare(ImGui::IsItemActive());
    if (ImGui::IsItemHovered() && !m.comparing)
        ImGui::SetTooltip("Hold to see the game without effects");
    ImGui::SameLine(0, S(8));
    if (Button("ReShade", ImVec2(S(90), S(34))))
        m.openReShade = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open ReShade's own menu for everything else");
    ImGui::PopFont();

    // Screenshots leave out the menu, since they are taken before it is drawn.
    ImGui::SameLine(0, S(8));
    const ImVec2 cameraStart = ImGui::GetCursorScreenPos();
    const ImVec2 cameraSize(S(34), S(34));
    if (ImGui::InvisibleButton("camera", cameraSize, ImGuiButtonFlags_EnableNav))
        ImGui::OpenPopup("screenshots");
    const bool cameraHovered = ImGui::IsItemHovered();
    HandOnHover();
    draw->AddRectFilled(cameraStart, cameraStart + cameraSize, cameraHovered ? kCardHover : kCard, S(6));
    draw->AddRect(cameraStart, cameraStart + cameraSize, kBorder, S(6));
    CameraIcon(draw, cameraStart + cameraSize * 0.5f, S(15), cameraHovered ? kText : kDim);
    if (cameraHovered)
        ImGui::SetTooltip("Screenshots");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("screenshots"))
    {
        if (ImGui::MenuItem("Screenshot"))
            RequestScreenshot(false);
        if (ImGui::MenuItem("Before and after"))
            RequestScreenshot(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Open screenshots folder"))
        {
            const fs::path folder = ScreenshotFolder();
            std::error_code ignored;
            fs::create_directories(folder, ignored);
            ShellOpen(folder.wstring());
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    // The menu shortcut, which also leaves.
    PushSize(13.5f);
    const char* label = "Back to the game";
    const std::string key = Utf8(g.inputHotkey);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    PushSize(13);
    const float keyWidth = std::max(ImGui::CalcTextSize(key.c_str()).x + S(14), S(28));
    ImGui::PopFont();
    const ImVec2 backSize(keyWidth + S(8) + labelSize.x, S(34));
    const ImVec2 backStart = origin + ImVec2(size.x - S(kPadding) - backSize.x, buttonY);
    ImGui::SetCursorScreenPos(backStart);
    if (ImGui::InvisibleButton("back", backSize, ImGuiButtonFlags_EnableNav))
        PostMessageW(g.overlay, kLeaveMenuMessage, 0, 0);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImGui::SetCursorScreenPos(backStart + ImVec2(0, (backSize.y - S(22)) / 2));
    KeyCap(key);
    PushSize(13.5f);
    draw->AddText(backStart + ImVec2(keyWidth + S(8), (backSize.y - labelSize.y) / 2), hovered ? kText : kDim, label);
    ImGui::PopFont();
}

void DrawMenu()
{
    m.update = AvailableUpdate();
    UpdateNotices();
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(S(kWidth), io.DisplaySize.x - S(kMargin) * 2);
    const float available = std::max(io.DisplaySize.y - S(kMargin) * 2, 1.0f);
    // A window too short for the menu scrolls all of it, so the text keeps a readable size.
    const ImVec2 size(width, std::max(available, S(menu_layout::MinHeight())));
    const bool scrolls = size.y > available;
    ImGui::SetNextWindowPos(ImVec2(S(kMargin), S(kMargin)));
    ImGui::SetNextWindowSize(ImVec2(width, available));
    ImGui::Begin("Unishade##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     (scrolls ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollWithMouse));
    const ImVec2 origin = ImGui::GetWindowPos() - ImVec2(0, ImGui::GetScrollY());
    Header(origin, width);
    Tabs(origin, width);

    const float top = S(kHeader + kTabs) + 1;
    ImGui::SetCursorScreenPos(origin + ImVec2(0, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(kPadding), S(16)));
    ImGui::BeginChild("content", ImVec2(width, size.y - top - S(kFooter)), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar();
    switch (m.tab)
    {
    case Tab::Presets: PresetsTab(); break;
    case Tab::Browse: BrowseTab(); break;
    case Tab::Effects: EffectsTab(); break;
    case Tab::Settings: SettingsTab(); break;
    case Tab::Status: StatusTab(); break;
    }
    ImGui::EndChild();
    Footer(origin, size);
    // Dialogs show over any tab.
    NameDialog(m.current);
    DeleteDialog();
    UnsavedDialog(m.current);
    ConfirmDialog();
    PublishDialog();
    ReportDialog();
    if (scrolls)
    {
        // Lets the window scroll down to the footer.
        ImGui::SetCursorScreenPos(origin + ImVec2(0, size.y));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::End();
}

void DrawToast(ULONGLONG elapsed)
{
    const float seconds = elapsed / 1000.0f;
    const float alpha = std::clamp(std::min(seconds / 0.25f, (m.toastDuration / 1000.0f - seconds) / 0.6f), 0.0f, 1.0f);
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y - S(48)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    if (!m.toastKey.empty())
    {
        KeyCap(m.toastKey, 14);
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
    }
    Text(m.toastText, kText, 14.5f, -1.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DrawFpsGraph(const FrameStatistics& stats)
{
    Text("Game capture", kAccent, 11.5f, -1);
    ImGui::SameLine(0, S(14));
    Text("Unishade", kWarning, 11.5f, -1);
    ImGui::SameLine(0, S(14));
    Text("Fresh output", kSuccess, 11.5f, -1);

    const auto now = FrameStatistics::Clock::now();
    constexpr double seconds = 60;
    double maximum = 60;
    for (size_t i = 0; i < stats.HistorySize(); ++i)
    {
        const auto& sample = stats.HistoryAt(i);
        if (std::chrono::duration<double>(now - sample.at).count() <= seconds)
            maximum = std::max({ maximum, sample.captureFps, sample.programFps, sample.freshFps });
    }
    maximum = std::ceil(maximum / 30) * 30;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(112));
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, origin + size, kInset, S(6));
    const ImVec2 min = origin + ImVec2(S(8), S(10));
    const ImVec2 max = origin + size - ImVec2(S(38), S(22));
    PushSize(10.5f);
    for (int i = 0; i <= 2; ++i)
    {
        const float y = min.y + (max.y - min.y) * i / 2;
        draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), kBorder);
        char label[32];
        snprintf(label, sizeof(label), "%.0f", maximum * (2 - i) / 2);
        draw->AddText(ImVec2(max.x + S(6), y - S(5)), kDim, label);
    }
    draw->AddText(ImVec2(min.x, max.y + S(6)), kDim, "60s ago");
    const ImVec2 nowSize = ImGui::CalcTextSize("Now");
    draw->AddText(ImVec2(max.x - nowSize.x, max.y + S(6)), kDim, "Now");
    ImGui::PopFont();

    const auto line = [&](double FrameStatistics::Sample::*rate, ImU32 color) {
        ImVec2 points[FrameStatistics::kHistory];
        int count = 0;
        for (size_t i = 0; i < stats.HistorySize(); ++i)
        {
            const auto& sample = stats.HistoryAt(i);
            const double age = std::chrono::duration<double>(now - sample.at).count();
            if (age > seconds)
                continue;
            points[count++] = ImVec2(min.x + (max.x - min.x) * static_cast<float>(1 - age / seconds),
                                    max.y - (max.y - min.y) * static_cast<float>(sample.*rate / maximum));
        }
        if (count > 1)
            draw->AddPolyline(points, count, color, 0, S(1.5f));
        if (count)
            draw->AddCircleFilled(points[count - 1], S(2), color);
    };
    // The fresh-output line stays visible when it overlaps the presentation rate.
    line(&FrameStatistics::Sample::captureFps, kAccent);
    line(&FrameStatistics::Sample::programFps, kWarning);
    line(&FrameStatistics::Sample::freshFps, kSuccess);
}

void DrawDebugInfo()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - S(kMargin), S(kMargin)), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(S(350), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::Begin("##debug_info", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    PushSize(13.5f);
    const FrameStatistics& stats = g.frameStatistics;
    Text("Performance", kText, 14.5f, -1);
    ImGui::SameLine();
    const std::string resolution = std::to_string(g.poolSize.Width) + " x " + std::to_string(g.poolSize.Height);
    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - S(14) - ImGui::CalcTextSize(resolution.c_str()).x);
    ImGui::TextDisabled("%s", resolution.c_str());
    DrawFpsGraph(stats);
    if (stats.ready)
    {
        if (ImGui::BeginTable("debug_metrics", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            const auto row = [](const char* label) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
            };
            row("Game capture");
            ImGui::Text("%.1f FPS", stats.captureFps);
            row("Unishade");
            ImGui::Text("%.1f FPS", stats.programFps);
            row("Fresh output");
            ImGui::Text("%.1f FPS", stats.freshFps);
            row("Frame loss");
            ImGui::Text("%.1f FPS / %.1f%%", stats.LostFps(), stats.LossPercent());
            row("Repeated frames");
            ImGui::Text("%.1f FPS", stats.RepeatedFps());
            row("Processing avg");
            ImGui::Text("%.2f ms", stats.processingMs);
            row("Processing peak");
            ImGui::Text("%.2f ms", stats.peakProcessingMs);
            ImGui::EndTable();
        }
    }
    else
        ImGui::TextUnformatted("Measuring FPS...");
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
}

// Saving writes the whole preset, so it waits until a slider is let go. With auto-save off, changes only mark the
// preset as unsaved.
void SaveChanges()
{
    if (!m.presetChanged || (ImGui::IsAnyItemActive() && m.pendingPreset.empty()))
        return;
    if (AutoSavePresets())
        SavePreset();
    else
        m.unsaved = true;
    m.presetChanged = false;
}

// Switches to the preset picked this frame, once it is clear what happens to unsaved changes.
void SwitchToPending()
{
    if (m.pendingPreset.empty())
        return;
    // ReShade would take a preset deleted or moved since the last scan for a new one, and write it back.
    if (std::error_code error; !m.askingUnsaved && !m.saveNewPreset && !fs::exists(m.pendingPreset, error))
    {
        ShowToast(Utf8(m.pendingPreset.stem().wstring()) + " is no longer there");
        ForgetPreset(m.pendingPreset);
        RequestScan();
        m.pendingPreset.clear();
        m.pendingKeepsEdits = false;
        m.unsavedChoice = UnsavedChoice::Ask;
        return;
    }
    if (m.unsaved && !m.pendingKeepsEdits && m.unsavedChoice == UnsavedChoice::Ask)
    {
        if (!m.askingUnsaved)
        {
            m.askingUnsaved = true;
            m.openUnsavedPopup = true;
        }
        return;
    }
    // Changes that went into the new preset, or were discarded, must not be saved into this one.
    if (m.unsaved && (m.pendingKeepsEdits || m.unsavedChoice == UnsavedChoice::Discard))
        DiscardChanges();
    else if (m.unsaved)
        SavePreset();
    ReadPresetEffects(m.pendingPreset);
    if (!SetPreset(m.pendingPreset))
        ShowToast("ReShade could not load " + Utf8(m.pendingPreset.stem().wstring()));
    else if (m.saveNewPreset)
        SaveToCache();
    m.pendingPreset.clear();
    m.saveNewPreset = false;
    m.pendingKeepsEdits = false;
    m.unsavedChoice = UnsavedChoice::Ask;
    m.active.clear();
}

void DrawMenuFrame()
{
    // Escape leaves the menu, unless it closes a popup, ends typing or cancels waiting for a shortcut.
    if (m.capturing < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        PostMessageW(g.overlay, kLeaveMenuMessage, 0, 0);
    CaptureShortcut();

    DrawMenu();

    if (m.capturing >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        StopCapture();
    SaveChanges();
    SwitchToPending();
    if (m.openReShade || m.openDlss)
    {
        m.focusDlss = m.openDlss;
        m.openReShade = m.openDlss = false;
        OpenReShadeMenu(true);
    }
}

// Runs before anything is drawn, so screenshots leave out the menu and ReShade's messages.
void CarryOutRequests()
{
    if (m.screenshotRequested)
    {
        // Without a picture from before the effects, which ReShade skips while they are off, there is only one.
        m.runtime->save_screenshot(m.beforeTaken ? "After" : nullptr);
        m.screenshotRequested = m.beforeAfterRequested = m.beforeTaken = false;
    }
    // Publishing takes the picture after the effects in the frame it took the one before them.
    if (m.browse.capture == Capture::After)
    {
        m.browse.capture = Capture::None;
        FinishPublish(CaptureFrame(m.runtime));
    }
    else if (m.browse.capture == Capture::Before && GetTickCount64() - m.browse.captureAt > kCaptureWait)
    {
        m.browse.capture = Capture::None;
        m.browse.publishing = false;
        m.browse.publishError = "Turn effects on, so the screenshots show them.";
    }
    // Steps go by presets read at most a couple of seconds before they were asked for, so new ones count.
    if (m.presetStep && (!m.scanVersion || m.scannedAt + kScanInterval < m.presetStepAt))
    {
        if (m.scanRequested < m.presetStepAt)
            RequestScan();
    }
    else if (m.presetStep)
    {
        RefreshFolders();
        const fs::path current = m.current;
        // The presets in open folders, in the order the Presets tab shows them.
        std::vector<fs::path> presets;
        for (const PresetFolder& folder : m.folders)
            if (FolderOpen(folder, current))
                presets.insert(presets.end(), folder.presets.begin(), folder.presets.end());
        const int count = static_cast<int>(presets.size());
        const auto found = std::find_if(presets.begin(), presets.end(), [&](const fs::path& preset) { return SamePath(preset, current); });
        // From a preset in a closed folder, the next one is the first shown and the previous one the last.
        const int index = found != presets.end() ? static_cast<int>(found - presets.begin()) : m.presetStep > 0 ? -1 : count;
        const fs::path target = count ? presets[((index + m.presetStep) % count + count) % count] : current;
        m.presetStep = 0;
        const std::string name = Utf8(target.stem().wstring());
        if (!SamePath(target, current))
            switch (SwitchNow(target))
            {
            case SwitchResult::Switched: ShowToast(name); break;
            case SwitchResult::Unsaved: ShowToast("Save or discard the changes to " + Utf8(current.stem().wstring()) + " first"); break;
            case SwitchResult::Failed: ShowToast("ReShade could not load " + name); break;
            }
    }
}

// ReShade calls the menu from its own DLL, which nothing may be thrown into. An error is logged once, and the frame
// goes on without the rest of the menu.
template <typename F>
void Guarded(F&& callback) noexcept
{
    try
    {
        callback();
    }
    catch (const std::exception& e)
    {
        if (!std::exchange(m.errorReported, true))
            Log(LogLevel::Error, L"The Unishade menu ran into an error: %hs", e.what());
    }
    catch (...)
    {
        if (!std::exchange(m.errorReported, true))
            Log(LogLevel::Error, L"The Unishade menu ran into an unknown error.");
    }
}

void OnBeginEffects(effect_runtime* runtime, command_list*, resource_view, resource_view)
{
    if (runtime == m.runtime && m.beforeAfterRequested && !m.beforeTaken)
    {
        runtime->save_screenshot("Before");
        m.beforeTaken = true;
    }
    if (runtime == m.runtime && m.browse.capture == Capture::Before)
    {
        m.browse.before = CaptureFrame(runtime);
        m.browse.capture = Capture::After;
    }
}

// What happens every frame before anything is drawn.
void UpdateFrame(bool menu)
{
    // ReShade draws the add-on's window before this runs, from the frame after its menu opens.
    if (m.focusDlss && ReShadeMenuOpen())
    {
        ImGui::SetWindowFocus(kDlssWindow);
        m.focusDlss = false;
    }
    // ReShade writes its cache from the end of a present a second after the last change, so once a frame has passed
    // that point, the files are written.
    for (PendingWrite* write : { &m.unwritten, &m.leftBehind })
        if (!write->preset.empty() && m.lastFrame > write->since + kReShadeWriteDelay)
            *write = {};
    m.lastFrame = GetTickCount64();
    // ReShade's own menu and shortcuts can switch presets too, saving the one they leave.
    if (const fs::path current = CurrentPreset(); !SamePath(current, m.current))
    {
        m.leftBehind = { m.current, m.lastFrame };
        m.current = current;
        m.foldersDirty = true;
    }
    // The values on screen are the ones loading gives, unless there are changes the reload would drop.
    if (m.reloadAfterWrite && m.leftBehind.preset.empty() && !m.unsaved && !m.presetChanged)
    {
        m.reloadAfterWrite = false;
        m.runtime->set_current_preset_path(Utf8(m.current.wstring()).c_str());
    }
    TakeScan();
    TakeSharingAnswers();
    FinishDelete();
    FollowGame();
    CarryOutRequests();
    TakeImports(menu);
}

void DrawOverlay(bool menu)
{
    // The start hint, the only toast with a key, has done its job once the menu opens.
    if (menu && !m.toastKey.empty())
        m.toastStart = 0;
    const ULONGLONG elapsed = GetTickCount64() - m.toastStart;
    const bool toast = m.toastStart && elapsed < m.toastDuration;
    if (!menu && !toast && !DebugInfoEnabled())
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    m.scale = menu_layout::Scale(display.x, display.y, MenuScale());
    if (m.scale <= 0)
        return;
    ApplyStyle(ImGui::GetStyle());
    PushSize(14.5f);
    if (menu)
    {
        RefreshFolders();
        DrawMenuFrame();
    }
    if (toast)
        DrawToast(elapsed);
    if (DebugInfoEnabled())
        DrawDebugInfo();
    ImGui::PopFont();
    m.cursor = menu ? ImGui::GetMouseCursor() : ImGuiMouseCursor_Arrow;
}

void OnOverlay(effect_runtime* runtime)
{
    if (runtime != m.runtime)
        return;
    // The menu's look only applies to its own windows, so ReShade's is restored after, also after an error.
    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiStyle saved = style;
    Guarded([] {
        const bool menu = g.editMode && !ReShadeMenuOpen();
        UpdateFrame(menu);
        DrawOverlay(menu);
    });
    style = saved;
}

void OnInitRuntime(effect_runtime* runtime)
{
    m.runtime = runtime;
    m.device = runtime->get_device();
    // A new runtime loads its effects from the start.
    m.techniquesDirty = true;
    m.techniquesTried = 0;
    m.compiledAt = 0;
    m.effectsEmpty = false;
    m.effectFiles.reset();
    m.effectCheckRequested = false;
    m.current = CurrentPreset();
    m.foldersDirty = true;
    ReadPresetEffects(m.current);
    // Ready for the first preset shortcut.
    RequestScan();
}

// ReShade destroys the runtime when the swapchain is resized or released, such as after the graphics card was
// reset, and creates a new one after. Logos are made again on the new device when they are drawn.
void OnDestroyRuntime(effect_runtime* runtime)
{
    if (runtime != m.runtime)
        return;
    DestroyTextures();
    // Effects load again from the preset, without the changes that were not saved.
    if (m.unsaved || (m.presetChanged && !AutoSavePresets()))
        ShowToast("ReShade loaded the effects again, so the unsaved changes to " + Utf8(m.current.stem().wstring()) + " are gone");
    m.unsaved = false;
    m.presetChanged = false;
    m.device = nullptr;
    m.runtime = nullptr;
    m.comparing = false;
    m.compareKey = 0;
    m.techniques.clear();
    m.parameters.clear();
    m.parametersEffect.clear();
}

// Only after the runtime is gone, which released the logos made on the device already.
void OnDestroyDevice(reshade::api::device* destroyed)
{
    if (destroyed != m.device)
        return;
    m.logo = {};
    m.folderLogos.clear();
    for (auto& [key, picture] : m.browse.pictures)
        picture.texture = {};
    m.browse.retired.clear();
    m.device = nullptr;
}

// Runs when ReShade starts loading effects again, and when it has created them all.
void OnReloadedEffects(effect_runtime*)
{
    // The handles are no longer valid, and an empty list tells when ReShade finished compiling.
    m.techniques.clear();
    m.effects.clear();
    m.byName.clear();
    m.techniquesDirty = true;
    m.techniquesTried = 0;
    m.compiledAt = 0;
    m.effectFiles.reset();
    m.effectCheckRequested = false;
    m.parameters.clear();
    m.parametersEffect.clear();
}

// Runs when ReShade finished compiling effects, and when its own menu switches presets.
void OnSetPresetPath(effect_runtime* runtime, const char*)
{
    if (runtime != m.runtime || !m.techniques.empty())
        return;
    m.compiledAt = GetTickCount64();
    m.techniquesDirty = true;
    m.techniquesTried = 0;
}

// Keys pressed while the menu waits for a shortcut still reach ReShade. This keeps End, ReShade's effects
// key, from turning effects off when it is picked as a shortcut.
bool OnSetEffectsState(effect_runtime*, bool)
{
    return m.capturing >= 0;
}
} // namespace

void InitMenu()
{
    if (!AddonRegistered())
        return;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(
        [](effect_runtime* runtime) { Guarded([runtime] { OnInitRuntime(runtime); }); });
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(
        [](effect_runtime* runtime) { Guarded([runtime] { OnDestroyRuntime(runtime); }); });
    reshade::register_event<reshade::addon_event::destroy_device>(
        [](reshade::api::device* destroyed) { Guarded([destroyed] { OnDestroyDevice(destroyed); }); });
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(
        [](effect_runtime* runtime) { Guarded([runtime] { OnReloadedEffects(runtime); }); });
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(
        [](effect_runtime* runtime, const char* path) { Guarded([runtime, path] { OnSetPresetPath(runtime, path); }); });
    reshade::register_event<reshade::addon_event::reshade_set_effects_state>(OnSetEffectsState);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(
        [](effect_runtime* runtime, command_list* commands, resource_view rtv, resource_view rtvSrgb) {
            Guarded([=] { OnBeginEffects(runtime, commands, rtv, rtvSrgb); });
        });
    reshade::register_event<reshade::addon_event::reshade_overlay>(OnOverlay);
}

void ShowStartHint()
{
    // Capture also restarts whenever the overlay is turned back on, often while taking comparison shots.
    if (!AddonRegistered() || m.hintShown)
        return;
    m.hintShown = true;
    ShowToast("opens the Unishade menu", Utf8(g.inputHotkey), kHintDuration);
}

void RequestScreenshot(bool beforeAfter)
{
    // ReShade names screenshots by the second, so a second one within it would replace the first.
    if (!m.runtime || !g.overlayVisible || GetTickCount64() - m.lastScreenshot < 1000)
        return;
    m.lastScreenshot = GetTickCount64();
    m.screenshotRequested = true;
    m.beforeAfterRequested = beforeAfter;
    m.beforeTaken = false;
}

void RequestPresetStep(int step)
{
    if (!m.runtime || !g.overlayVisible)
        return;
    if (!m.presetStep)
        m.presetStepAt = GetTickCount64();
    m.presetStep += step;
}

void StartHeldCompare(UINT key)
{
    if (!m.runtime || m.comparing)
        return;
    Compare(true);
    m.compareKey = key;
}

void UpdateHeldCompare()
{
    if (!m.compareKey || (GetAsyncKeyState(static_cast<int>(m.compareKey)) & 0x8000))
        return;
    m.compareKey = 0;
    if (m.runtime)
        Compare(false);
}

void ResetMenu()
{
    if (m.capturing >= 0)
        StopCapture();
    m.focusDlss = false;
    m.active.clear();
    if (!m.runtime)
        return;
    // The compare shortcut keeps effects off until it is let go, even outside the menu.
    if (m.comparing && !m.compareKey)
    {
        m.runtime->set_effects_state(m.effectsBeforeCompare);
        m.comparing = false;
    }
    if (m.presetChanged && AutoSavePresets())
        SavePreset();
    else if (m.presetChanged)
        m.unsaved = true;
    m.presetChanged = false;
    // A switch still waiting for an answer about unsaved changes is dropped.
    m.pendingPreset.clear();
    m.saveNewPreset = false;
    m.pendingKeepsEdits = false;
    m.unsavedChoice = UnsavedChoice::Ask;
    m.askingUnsaved = false;
    m.openUnsavedPopup = false;
    // Hiding the menu closes its dialogs.
    m.deleteDialogOpen = false;
}

LPCWSTR MenuCursor()
{
    if (ReShadeMenuOpen())
        return IDC_ARROW;
    switch (m.cursor)
    {
    case ImGuiMouseCursor_TextInput: return IDC_IBEAM;
    case ImGuiMouseCursor_Hand: return IDC_HAND;
    case ImGuiMouseCursor_ResizeAll: return IDC_SIZEALL;
    case ImGuiMouseCursor_ResizeEW: return IDC_SIZEWE;
    case ImGuiMouseCursor_ResizeNS: return IDC_SIZENS;
    case ImGuiMouseCursor_NotAllowed: return IDC_NO;
    default: return IDC_ARROW;
    }
}

bool MenuHasUnsavedChanges()
{
    return m.runtime && (m.unsaved || (m.presetChanged && !AutoSavePresets()));
}

std::wstring ActivePresetName()
{
    return m.runtime ? m.current.stem().wstring() : std::wstring();
}

void FlushPresets(bool saveUnsaved)
{
    if (!m.runtime)
        return;
    // Changes made while a slider was still held are saved with auto-save on. Unsaved ones only when asked to.
    const bool save = (saveUnsaved && (m.unsaved || m.presetChanged)) || (AutoSavePresets() && m.presetChanged);
    if (!save && m.unwritten.preset.empty())
        return;
    try
    {
        if (save)
            SavePreset();
        WriteActivePreset();
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Error, L"Could not write %ls: %hs", m.current.filename().c_str(), e.what());
    }
}
