// What an add-on calls: the functions of UnishadeHost and UnishadeUi, as addons/unishade_addon.h describes them.

#include "addons/addon_library.h"
#include "app/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
using addon::HostContext;

// A label as it shows: without what follows ##, which only tells controls apart.
std::string Shown(const char* label)
{
    const std::string text = label ? label : "";
    return text.substr(0, text.find("##"));
}

HostContext& ContextOf(const UnishadeHost* host)
{
    return *const_cast<HostContext*>(reinterpret_cast<const HostContext*>(host));
}

void HostLog(const UnishadeHost* host, UnishadeLogLevel level, const char* text)
{
    const char* name = host ? reinterpret_cast<const HostContext*>(host)->name : "An add-on";
    const LogLevel kind = level == UNISHADE_LOG_ERROR ? LogLevel::Error : level == UNISHADE_LOG_WARNING ? LogLevel::Warning : LogLevel::Info;
    Log(kind, "%s: %s", name, text ? text : "");
}

// Unishade itself.

const UnishadeState* HostGetState(const UnishadeHost* host)
{
    HostContext& context = ContextOf(host);
    const AddonHost::State now = context.app->DescribeForAddons();
    const fx::Runtime& runtime = *context.runtime;
    context.gameName = now.gameName;
    context.executable = now.executable;
    context.presetName = runtime.PresetPath().stem().string();
    context.presetPath = runtime.PresetPath().string();
    UnishadeState& state = context.state;
    state = {};
    state.size = sizeof(state);
    state.playing = now.playing;
    state.effectsOn = now.effectsOn;
    state.menuOpen = now.menuOpen;
    state.hdr = now.playing && runtime.Hdr();
    state.width = now.playing ? runtime.Width() : 0;
    state.height = now.playing ? runtime.Height() : 0;
    state.frameRate = now.frameRate;
    state.gameName = context.gameName.c_str();
    state.executable = context.executable.c_str();
    state.presetName = context.presetName.c_str();
    state.presetPath = context.presetPath.c_str();
    state.time = now.time;
    return &state;
}

void HostShowMessage(const UnishadeHost* host, const char* text)
{
    if (text && *text)
        ContextOf(host).app->ShowMessage(text);
}

void HostSetEffectsOn(const UnishadeHost* host, bool on)
{
    ContextOf(host).app->SetEffectsOn(on);
}

void HostTakeScreenshot(const UnishadeHost* host)
{
    ContextOf(host).app->TakeScreenshot();
}

bool HostKeyDown(const UnishadeHost* host, uint32_t key)
{
    const fx::EffectInput& input = ContextOf(host).runtime->input;
    return key < input.keysDown.size() && input.keysDown[key];
}

void HostSetSetting(const UnishadeHost* host, uint32_t setting, const float* values)
{
    HostContext& context = ContextOf(host);
    if (values)
        context.addons->SetSetting(context.folder, setting, values, false);
}

uint32_t HostPresetCount(const UnishadeHost* host)
{
    HostContext& context = ContextOf(host);
    context.presets = context.app->ListPresets();
    return static_cast<uint32_t>(context.presets.size());
}

const UnishadePreset* HostGetPreset(const UnishadeHost* host, uint32_t index)
{
    HostContext& context = ContextOf(host);
    if (index >= context.presets.size())
        return nullptr;
    context.listedName = context.presets[index].stem().string();
    context.listedPath = context.presets[index].string();
    context.preset = { sizeof(UnishadePreset), context.presets[index] == context.runtime->PresetPath(), context.listedName.c_str(),
                       context.listedPath.c_str() };
    return &context.preset;
}

bool HostUsePreset(const UnishadeHost* host, uint32_t index)
{
    HostContext& context = ContextOf(host);
    return index < context.presets.size() && context.app->UsePreset(context.presets[index]);
}

// The host's effect functions, on fx::Runtime.

fx::Runtime& RuntimeOf(const UnishadeHost* host)
{
    return *reinterpret_cast<const HostContext*>(host)->runtime;
}

fx::Effect* EffectAt(const UnishadeHost* host, uint32_t effect)
{
    std::vector<fx::Effect>& effects = RuntimeOf(host).Effects();
    return effect < effects.size() ? &effects[effect] : nullptr;
}

// The uniforms presets keep, which are the ones the host numbers.
bool Kept(const fx::Uniform& uniform)
{
    return uniform.source.empty() && uniform.type.is_numeric();
}

const fx::Uniform* UniformAt(const UnishadeHost* host, uint32_t effect, uint32_t index, fx::Effect*& owner)
{
    owner = EffectAt(host, effect);
    if (owner)
        for (const fx::Uniform& uniform : owner->uniforms)
            if (Kept(uniform) && index-- == 0)
                return &uniform;
    return nullptr;
}

uint64_t HostEffectsGeneration(const UnishadeHost* host)
{
    return RuntimeOf(host).EffectsGeneration();
}

uint32_t HostTechniqueCount(const UnishadeHost* host)
{
    return static_cast<uint32_t>(RuntimeOf(host).Techniques().size());
}

const UnishadeTechnique* HostGetTechnique(const UnishadeHost* host, uint32_t index)
{
    fx::Runtime& runtime = RuntimeOf(host);
    if (index >= runtime.Techniques().size())
        return nullptr;
    const fx::Technique& technique = runtime.Techniques()[index];
    UnishadeTechnique& description = ContextOf(host).technique;
    description = { sizeof(UnishadeTechnique), static_cast<uint32_t>(technique.effect), technique.name.c_str(),
                    runtime.Effects()[technique.effect].file.c_str(), technique.enabled };
    return &description;
}

uint32_t HostFindTechnique(const UnishadeHost* host, const char* name, const char* file)
{
    fx::Runtime& runtime = RuntimeOf(host);
    const std::vector<fx::Technique>& techniques = runtime.Techniques();
    for (size_t i = 0; name && i < techniques.size(); ++i)
        if (techniques[i].name == name && (!file || runtime.Effects()[techniques[i].effect].file == file))
            return static_cast<uint32_t>(i);
    return UNISHADE_NONE;
}

void HostSetTechnique(const UnishadeHost* host, uint32_t index, bool enabled)
{
    if (index < RuntimeOf(host).Techniques().size())
        RuntimeOf(host).SetEnabled(index, enabled);
}

uint32_t HostFindEffect(const UnishadeHost* host, const char* file)
{
    const std::vector<fx::Effect>& effects = RuntimeOf(host).Effects();
    for (size_t i = 0; file && i < effects.size(); ++i)
        if (effects[i].file == file)
            return static_cast<uint32_t>(i);
    return UNISHADE_NONE;
}

uint32_t HostUniformCount(const UnishadeHost* host, uint32_t effect)
{
    const fx::Effect* owner = EffectAt(host, effect);
    return owner ? static_cast<uint32_t>(std::count_if(owner->uniforms.begin(), owner->uniforms.end(), Kept)) : 0;
}

const UnishadeUniform* HostGetUniform(const UnishadeHost* host, uint32_t effect, uint32_t index)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return nullptr;
    UnishadeUniform& description = ContextOf(host).uniform;
    description = {};
    description.size = sizeof(UnishadeUniform);
    description.name = uniform->name.c_str();
    description.type = uniform->type.is_boolean()        ? UNISHADE_VALUE_BOOL
                       : uniform->type.is_floating_point() ? UNISHADE_VALUE_FLOAT
                       : uniform->type.is_signed()         ? UNISHADE_VALUE_INT
                                                           : UNISHADE_VALUE_UINT;
    description.count = static_cast<uint32_t>(fx::ComponentCount(*uniform));
    description.min = uniform->min;
    description.max = uniform->max;
    return &description;
}

uint32_t HostFindUniform(const UnishadeHost* host, uint32_t effect, const char* name)
{
    const fx::Effect* owner = EffectAt(host, effect);
    uint32_t index = 0;
    if (owner && name)
        for (const fx::Uniform& uniform : owner->uniforms)
            if (Kept(uniform))
            {
                if (uniform.name == name)
                    return index;
                ++index;
            }
    return UNISHADE_NONE;
}

uint32_t HostGetValue(const UnishadeHost* host, uint32_t effect, uint32_t index, float* values, uint32_t count)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return 0;
    if (values)
        RuntimeOf(host).GetValue(*owner, *uniform, values, count);
    return static_cast<uint32_t>(fx::ComponentCount(*uniform));
}

uint32_t HostGetIntValue(const UnishadeHost* host, uint32_t effect, uint32_t index, int32_t* values, uint32_t count)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return 0;
    if (values)
    {
        std::vector<int> read(count);
        RuntimeOf(host).GetValue(*owner, *uniform, read.data(), count);
        std::copy(read.begin(), read.end(), values);
    }
    return static_cast<uint32_t>(fx::ComponentCount(*uniform));
}

bool HostSetValue(const UnishadeHost* host, uint32_t effect, uint32_t index, const float* values, uint32_t count)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return false;
    if (values && count)
    {
        RuntimeOf(host).SetValue(*owner, *uniform, values, count);
        RuntimeOf(host).SetDirty();
    }
    return true;
}

bool HostSetIntValue(const UnishadeHost* host, uint32_t effect, uint32_t index, const int32_t* values, uint32_t count)
{
    fx::Effect* owner = nullptr;
    const fx::Uniform* uniform = UniformAt(host, effect, index, owner);
    if (!uniform)
        return false;
    if (values && count)
    {
        const std::vector<int> given(values, values + count);
        RuntimeOf(host).SetValue(*owner, *uniform, given.data(), count);
        RuntimeOf(host).SetDirty();
    }
    return true;
}

// Frames. addon_frames.cpp reads and clears what this sets around each frame call.

void HostWroteMotion(const UnishadeHost* host)
{
    ContextOf(host).wroteMotion = true;
}

// The controls an add-on draws, which UnishadeUi's functions add to its library's. UnishadeUi first, so the ui an
// add-on passes back leads to it.
struct UiContext
{
    UnishadeUi ui;
    Addons::Library* library;
};

Addons::Control& Add(const UnishadeUi* ui, Addons::Control::Kind kind, const char* label)
{
    std::vector<Addons::Control>& controls = reinterpret_cast<const UiContext*>(ui)->library->controls;
    controls.push_back({ kind, Shown(label), label ? label : "" });
    return controls.back();
}

// What the user did with the control with a label since the add-on last drew it, if anything.
bool Used(const UnishadeUi* ui, const char* label, float* values)
{
    auto& used = reinterpret_cast<const UiContext*>(ui)->library->used;
    const auto entry = used.find(label ? label : "");
    if (entry == used.end())
        return false;
    std::copy(entry->second.begin(), entry->second.end(), values);
    used.erase(entry);
    return true;
}

void UiHeading(const UnishadeUi* ui, const char* text)
{
    Add(ui, Addons::Control::Kind::Heading, text);
}

void UiText(const UnishadeUi* ui, const char* text)
{
    // Text is not a label: all of it shows.
    Add(ui, Addons::Control::Kind::Text, nullptr).label = text ? text : "";
}

void UiProgress(const UnishadeUi* ui, const char* label, float fraction)
{
    Add(ui, Addons::Control::Kind::Progress, label).value[0] = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 0;
}

bool UiButton(const UnishadeUi* ui, const char* label)
{
    float values[4];
    Add(ui, Addons::Control::Kind::Button, label);
    return Used(ui, label, values);
}

bool UiToggle(const UnishadeUi* ui, const char* label, bool* value)
{
    float values[4];
    const bool changed = value && Used(ui, label, values);
    if (changed)
        *value = values[0] != 0;
    Add(ui, Addons::Control::Kind::Toggle, label).value[0] = value && *value ? 1.0f : 0.0f;
    return changed;
}

bool UiSlider(const UnishadeUi* ui, const char* label, float* value, float min, float max)
{
    float values[4];
    const bool changed = value && Used(ui, label, values) && std::isfinite(values[0]);
    if (changed)
        *value = std::clamp(values[0], std::min(min, max), std::max(min, max));
    Addons::Control& control = Add(ui, Addons::Control::Kind::Slider, label);
    control.min = std::min(min, max);
    control.max = std::max(min, max);
    control.value[0] = value ? *value : 0;
    return changed;
}

bool UiInteger(const UnishadeUi* ui, const char* label, int32_t* value, int32_t min, int32_t max)
{
    float values[4];
    const bool changed = value && Used(ui, label, values) && std::isfinite(values[0]);
    if (changed)
        *value = std::clamp(static_cast<int32_t>(std::lround(values[0])), std::min(min, max), std::max(min, max));
    Addons::Control& control = Add(ui, Addons::Control::Kind::Integer, label);
    control.min = static_cast<float>(std::min(min, max));
    control.max = static_cast<float>(std::max(min, max));
    control.value[0] = value ? static_cast<float>(*value) : 0;
    return changed;
}

bool UiChoice(const UnishadeUi* ui, const char* label, int32_t* value, const char* choices)
{
    std::vector<std::string> list;
    for (const char* choice = choices; choice && *choice; choice += std::strlen(choice) + 1)
        list.emplace_back(choice);
    float values[4];
    const bool changed = value && Used(ui, label, values) && std::isfinite(values[0]) && !list.empty();
    if (changed)
        *value = std::clamp(static_cast<int32_t>(std::lround(values[0])), 0, static_cast<int32_t>(list.size()) - 1);
    Addons::Control& control = Add(ui, Addons::Control::Kind::Choice, label);
    control.choices = std::move(list);
    control.value[0] = value ? static_cast<float>(*value) : 0;
    return changed;
}

bool UiColor(const UnishadeUi* ui, const char* label, float* value, bool alpha)
{
    const size_t count = alpha ? 4 : 3;
    float values[4];
    const bool changed = value && Used(ui, label, values);
    for (size_t i = 0; changed && i < count; ++i)
        value[i] = std::isfinite(values[i]) ? std::clamp(values[i], 0.0f, 1.0f) : value[i];
    Addons::Control& control = Add(ui, Addons::Control::Kind::Color, label);
    control.count = count;
    for (size_t i = 0; value && i < count; ++i)
        control.value[i] = value[i];
    return changed;
}

void UiTooltip(const UnishadeUi* ui, const char* text)
{
    std::vector<Addons::Control>& controls = reinterpret_cast<const UiContext*>(ui)->library->controls;
    if (!controls.empty() && text)
        controls.back().tooltip = text;
}
} // namespace

void addon::FillHost(UnishadeHost& host)
{
    host.log = HostLog;
    host.getState = HostGetState;
    host.showMessage = HostShowMessage;
    host.setEffectsOn = HostSetEffectsOn;
    host.takeScreenshot = HostTakeScreenshot;
    host.keyDown = HostKeyDown;
    host.setSetting = HostSetSetting;
    host.presetCount = HostPresetCount;
    host.getPreset = HostGetPreset;
    host.usePreset = HostUsePreset;
    host.effectsGeneration = HostEffectsGeneration;
    host.techniqueCount = HostTechniqueCount;
    host.getTechnique = HostGetTechnique;
    host.findTechnique = HostFindTechnique;
    host.setTechnique = HostSetTechnique;
    host.findEffect = HostFindEffect;
    host.uniformCount = HostUniformCount;
    host.getUniform = HostGetUniform;
    host.findUniform = HostFindUniform;
    host.getValue = HostGetValue;
    host.getIntValue = HostGetIntValue;
    host.setValue = HostSetValue;
    host.setIntValue = HostSetIntValue;
    host.wroteMotion = HostWroteMotion;
}

void addon::DrawControls(Addons::Library& library)
{
    UiContext context{};
    context.library = &library;
    UnishadeUi& ui = context.ui;
    ui.size = sizeof(ui);
    ui.heading = UiHeading;
    ui.text = UiText;
    ui.progress = UiProgress;
    ui.button = UiButton;
    ui.toggle = UiToggle;
    ui.slider = UiSlider;
    ui.integer = UiInteger;
    ui.choice = UiChoice;
    ui.color = UiColor;
    ui.tooltip = UiTooltip;
    library.addon->ui(library.instance, &ui);
}
