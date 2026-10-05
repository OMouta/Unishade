#include "addon.h"
#include "reshade_imgui.h"
#include "state.h"
#include "log.h"

// Shown in ReShade's add-on list.
extern "C" __declspec(dllexport) const char* NAME = "Unishade";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Draws the Unishade menu and supplies estimated depth when depth estimation is installed.";

namespace
{
bool registered = false;
bool tooOld = false;
bool reshadeMenuOpen = false;
bool allowReShadeMenu = false;
// The host has one swapchain, so ReShade creates one runtime for it.
reshade::api::effect_runtime* runtime = nullptr;
// When ReShade may have started making effects, 0 when it is done.
ULONGLONG loadingSince = 0;
// In case the end of loading is missed, such as after a preset switch that loads nothing.
constexpr ULONGLONG kLoadingLimit = 10000;
// While ReShade compiles effects on its own threads.
bool compiling = false;

// A new runtime compiles its effects from its first frame.
void OnInitRuntime(reshade::api::effect_runtime* created)
{
    runtime = created;
    loadingSince = GetTickCount64();
    compiling = true;
    uint32_t width = 0, height = 0;
    created->get_screenshot_width_and_height(&width, &height);
    Log(LogLevel::Info, L"ReShade runtime created: runtime=%p, size=%ux%u.", created, width, height);
}

// ReShade compiles effects in the background, applies the preset, then makes one effect per frame and reports reloaded
// effects once all are made. It reports reloaded effects before compiling too, which needs no frames.
void OnSetPresetPath(reshade::api::effect_runtime* changed, const char* preset)
{
    if (changed != runtime)
        return;
    loadingSince = GetTickCount64();
    compiling = false;
    Log(LogLevel::Info, L"ReShade preset selected: %hs.", preset ? preset : "(none)");
}

void OnReloadedEffects(reshade::api::effect_runtime* reloaded)
{
    if (reloaded != runtime)
        return;
    loadingSince = 0;
    // Before compiling, ReShade has dropped every effect. Once all are made, it lists them again.
    size_t count = 0;
    reloaded->enumerate_techniques(
        nullptr, [](reshade::api::effect_runtime*, reshade::api::effect_technique, void* found) { ++*static_cast<size_t*>(found); }, &count);
    compiling = !count;
    Log(LogLevel::Info, L"ReShade effects reload: runtime=%p, techniques=%zu, compiling=%d, effects_enabled=%d.", reloaded, count, compiling,
        reloaded->get_effects_state());
}

void OnDestroyRuntime(reshade::api::effect_runtime* destroyed)
{
    if (runtime != destroyed)
        return;
    runtime = nullptr;
    Log(LogLevel::Info, L"ReShade runtime destroyed: runtime=%p.", destroyed);
    // Its menu goes with it, such as when the device is lost, and the next runtime starts with it closed.
    reshadeMenuOpen = false;
    loadingSince = 0;
    compiling = false;
}

bool OnOpenOverlay(reshade::api::effect_runtime*, bool open, reshade::api::input_source)
{
    // The host's menu replaces ReShade's, which only opens from the host's menu.
    if (open && !allowReShadeMenu)
        return true;
    reshadeMenuOpen = open;
    return false;
}
} // namespace

bool ReShadeLoaded()
{
    return reshade::internal::get_reshade_module_handle() != nullptr;
}

bool InitAddon()
{
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!reshade::register_addon(module))
    {
        // register_addon also fails when ReShade does not export the ImGui version the menu is built with, but
        // leaves the add-on registered then. Unregistering treats that like a ReShade without add-on support.
        reshade::unregister_addon(module);
        using GetTable = const void* (*)(uint32_t);
        const HMODULE reshadeModule = reshade::internal::get_reshade_module_handle();
        const auto getTable = reshadeModule ? reinterpret_cast<GetTable>(GetProcAddress(reshadeModule, "ReShadeGetImGuiFunctionTable")) : nullptr;
        tooOld = reshadeModule && (!getTable || !getTable(IMGUI_VERSION_NUM));
        Log(LogLevel::Info, L"ReShade add-on registration failed: module=%p, incompatible_imgui=%d.", reshadeModule, tooOld);
        return false;
    }
    registered = true;
    Log(LogLevel::Info, L"ReShade add-on registered.");
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitRuntime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
    reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
    reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(OnSetPresetPath);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloadedEffects);
    return true;
}

bool AddonRegistered()
{
    return registered;
}

bool ReShadeTooOld()
{
    return tooOld;
}

void OpenReShadeMenu(bool open)
{
    if (!runtime)
        return;
    allowReShadeMenu = open;
    runtime->open_overlay(open, reshade::api::input_source::keyboard);
    allowReShadeMenu = false;
}

bool ReShadeMenuOpen()
{
    return reshadeMenuOpen;
}

bool ReShadeLoadingEffects()
{
    return loadingSince && GetTickCount64() - loadingSince < kLoadingLimit;
}

bool ReShadeCompilingEffects()
{
    return compiling;
}

void ShutdownAddon()
{
    if (registered)
        reshade::unregister_addon(GetModuleHandleW(nullptr));
}
