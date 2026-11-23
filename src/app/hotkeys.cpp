#include "app/hotkeys.h"
#include "app/hotkey_text.h"

#include <algorithm>

namespace
{
struct NamedKey
{
    const char* name;
    ImGuiKey key;
};

// Keys other than letters, digits and function keys, named as on Windows.
constexpr NamedKey kNamedKeys[] = {
    { "Home", ImGuiKey_Home }, { "End", ImGuiKey_End }, { "Insert", ImGuiKey_Insert },
    { "Delete", ImGuiKey_Delete }, { "PageUp", ImGuiKey_PageUp }, { "PageDown", ImGuiKey_PageDown },
    { "Pause", ImGuiKey_Pause }, { "ScrollLock", ImGuiKey_ScrollLock }, { "Space", ImGuiKey_Space },
    { "Tab", ImGuiKey_Tab }, { "Escape", ImGuiKey_Escape }, { "Left", ImGuiKey_LeftArrow },
    { "Right", ImGuiKey_RightArrow }, { "Up", ImGuiKey_UpArrow }, { "Down", ImGuiKey_DownArrow },
    { "Numpad0", ImGuiKey_Keypad0 }, { "Numpad1", ImGuiKey_Keypad1 }, { "Numpad2", ImGuiKey_Keypad2 },
    { "Numpad3", ImGuiKey_Keypad3 }, { "Numpad4", ImGuiKey_Keypad4 }, { "Numpad5", ImGuiKey_Keypad5 },
    { "Numpad6", ImGuiKey_Keypad6 }, { "Numpad7", ImGuiKey_Keypad7 }, { "Numpad8", ImGuiKey_Keypad8 },
    { "Numpad9", ImGuiKey_Keypad9 }, { "NumpadMultiply", ImGuiKey_KeypadMultiply }, { "NumpadAdd", ImGuiKey_KeypadAdd },
    { "NumpadSubtract", ImGuiKey_KeypadSubtract }, { "NumpadDecimal", ImGuiKey_KeypadDecimal }, { "NumpadDivide", ImGuiKey_KeypadDivide },
};

// macOS has no hot key codes past F20.
#ifdef __APPLE__
constexpr ImGuiKey kLastFunctionKey = ImGuiKey_F20;
#else
constexpr ImGuiKey kLastFunctionKey = ImGuiKey_F24;
#endif
} // namespace

bool IsShortcutKey(ImGuiKey key)
{
#ifdef _WIN32
    // Windows keeps F12 for the debugger.
    if (key == ImGuiKey_F12)
        return false;
#endif
    if ((key >= ImGuiKey_A && key <= ImGuiKey_Z) || (key >= ImGuiKey_0 && key <= ImGuiKey_9) || (key >= ImGuiKey_F1 && key <= kLastFunctionKey))
        return true;
    return std::any_of(std::begin(kNamedKeys), std::end(kNamedKeys), [key](const NamedKey& named) { return named.key == key; });
}

// The rules for writing shortcuts are shared with Windows. macOS names are accepted everywhere, so settings move
// between systems.
bool ParseHotkey(std::string_view text, Hotkey& result)
{
    const auto modifierBit = [](std::string_view name) -> unsigned {
        using hotkey_text::SameName;
        if (SameName(name, "Ctrl") || SameName(name, "Control"))
            return kCtrl;
        if (SameName(name, "Alt") || SameName(name, "Option") || SameName(name, "Opt"))
            return kAlt;
        if (SameName(name, "Shift"))
            return kShift;
        if (SameName(name, "Win") || SameName(name, "Super") || SameName(name, "Cmd") || SameName(name, "Command"))
            return kSuper;
        return 0;
    };
    unsigned modifiers = 0;
    std::string name;
    if (!hotkey_text::Split(text, modifierBit, modifiers, name))
        return false;

    ImGuiKey key = ImGuiKey_None;
    if (const char c = hotkey_text::Character(name))
        key = static_cast<ImGuiKey>(c >= 'A' ? ImGuiKey_A + (c - 'A') : ImGuiKey_0 + (c - '0'));
    else if (const int number = hotkey_text::FunctionKey(name))
        key = static_cast<ImGuiKey>(ImGuiKey_F1 + number - 1);
    else
        for (const NamedKey& named : kNamedKeys)
            if (hotkey_text::SameName(name, named.name))
                key = named.key;
    if (key == ImGuiKey_None)
        return false;
#ifdef _WIN32
    if (key == ImGuiKey_F12)
        return false;
#endif
    result = { modifiers, key };
    return true;
}

std::string FormatHotkey(const Hotkey& hotkey)
{
    std::string key;
    if (hotkey.key >= ImGuiKey_A && hotkey.key <= ImGuiKey_Z)
        key = static_cast<char>('A' + (hotkey.key - ImGuiKey_A));
    else if (hotkey.key >= ImGuiKey_0 && hotkey.key <= ImGuiKey_9)
        key = static_cast<char>('0' + (hotkey.key - ImGuiKey_0));
    else if (hotkey.key >= ImGuiKey_F1 && hotkey.key <= ImGuiKey_F24)
        key = "F" + std::to_string(hotkey.key - ImGuiKey_F1 + 1);
    for (const NamedKey& named : kNamedKeys)
        if (hotkey.key == named.key)
            key = named.name;
    if (key.empty())
        return {};

    std::string text;
    if (hotkey.modifiers & kCtrl)
        text += "Ctrl+";
#ifdef __APPLE__
    if (hotkey.modifiers & kAlt)
        text += "Option+";
    if (hotkey.modifiers & kShift)
        text += "Shift+";
    if (hotkey.modifiers & kSuper)
        text += "Cmd+";
#else
    if (hotkey.modifiers & kAlt)
        text += "Alt+";
    if (hotkey.modifiers & kShift)
        text += "Shift+";
#ifdef _WIN32
    if (hotkey.modifiers & kSuper)
        text += "Win+";
#else
    if (hotkey.modifiers & kSuper)
        text += "Super+";
#endif
#endif
    return text + key;
}
