// The tray's design. See tray.h.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/tray.h"
#include "ui/kit.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace launcher::tray
{
namespace
{
using namespace kit;
using namespace kit::spectrum;

// The design's sizes at 100%, in CSS pixels.
constexpr float kWidth = 992;     // at most, and 94% of the game's width
constexpr float kSideWidth = 304; // docked at a side
constexpr float kPad = 16;
constexpr float kRounding = 35.2f;
constexpr float kGrab = 25.6f;
constexpr float kRowHeight = 48; // a line of the menu's buttons
constexpr float kRowGap = 8;
constexpr float kTilesGap = 12.8f; // between the tiles and the buttons
constexpr float kTileGap = 8;
constexpr float kEdge = 25.6f;  // from the bottom of the game, where the tray starts
constexpr float kDock = 90;     // let go this near a side of the game and the tray docks there
constexpr float kCenter = 18;   // moved this near the middle of the game, the tray stays in the middle
constexpr float kMargin = 20;   // the tray keeps this far from the sides of the game
constexpr float kNameWidth = 240;
constexpr float kSideNameWidth = 144;
constexpr float kField = 52.8f;
constexpr float kFieldGap = 8.8f;
constexpr float kFieldWidth = 288;
constexpr float kCardWidth = 216;
constexpr float kButton = 41.6f;
// The menu sizes the corner and the Menu tab go between, in percent, as the hosts keep them.
constexpr int kSmallestSize = 75;
constexpr int kLargestSize = 200;
// The frame rate choice that is typed in a box.
constexpr int kCustom = static_cast<int>(std::size(kFrameRates));
constexpr char kHint[] = "Click a tile for its settings. The dot in its corner turns it off, and dragging it changes the order effects run in.";

enum class Sheet
{
    None,
    Tune,
    Add,
    Presets,
    More,
    Shot,
    Ask,
};

enum class Question
{
    Unsaved,
    Discard,
    Delete,
    ResetEffect,
    ResetShortcuts,
};

enum class Tab
{
    Game, // what is kept for the game being played
    Menu,
    Shortcuts,
    Addons,
};

enum class Naming
{
    None,
    New,
    Copy,
    Rename,
};

// Where a tile was drawn last, in units of the tile area at 100%, so a tile that goes shrinks away from there.
struct Memo
{
    ImVec2 at;
    ImVec2 size;
    ImU32 color = 0;
    bool on = false;
    std::string name;
};

struct Ghost
{
    Memo memo;
    double since = 0;
};

struct Frame;

struct State
{
    bool started = false;
    bool justOpened = false;
    double opened = -100;
    bool gone = false; // out of sight after the menu closed
    // The effects in the preset in use, by key: the ones that are on, and the ones turned off that the preset keeps.
    std::set<std::string> members;
    std::string preset;
    bool presetKnown = false;
    std::string presetName;
    int swapDirection = 1;
    double swapped = -100;
    std::map<std::string, Memo> memos;
    std::vector<Ghost> ghosts;
    bool plusSeen = false;
    // A tile held, which is dragged once it moved far enough, and the order the tiles have while it is.
    std::string held;
    ImVec2 heldAt;
    ImVec2 heldOffset;
    bool dragging = false;
    std::vector<std::string> dragOrder;

    Sheet sheet = Sheet::None;
    Sheet drawn = Sheet::None; // stays while the sheet closes
    Sheet measuredKind = Sheet::None;
    float measured = 0; // the drawn sheet's height in the last frame, at 100%
    float sheetTarget = 0;
    float slide = 0;
    bool fromClosed = false;
    std::string tuned; // the effect whose settings show, by key
    Tab tab = Tab::Game;
    Question question = Question::Unsaved;
    Sheet back = Sheet::None; // where the question goes back to
    std::string pendingPreset;
    int pendingDirection = 1;
    std::string deleting;
    std::string resetting;
    char search[128] = {};
    bool focusSearch = false;
    std::string pack; // the pack Add effect lists, or empty for every one
    std::set<std::string> addedHere; // added with the add sheet open, which can take them back out
    std::string shownErrors;
    Naming naming = Naming::None;
    std::string namingTarget;
    char name[128] = {};
    bool focusName = false;
    bool nameFailed = false;
    bool customRate = false;
    char rate[8] = {};
    bool focusRate = false;
    bool rateActive = false;
    bool recordingHere = false;
    bool savedShown = false;
    bool comparing = false;
    // What was just done, and how to take it back, offered at the tray's edge for a few seconds.
    std::string undoText;
    std::function<void(Frame&)> undo;

    // Where the tray is over the game, as Settings::menuX and menuY, and the menu size while it changes.
    float x = -1;
    float y = -1;
    float seenX = -2;
    float seenY = -2;
    int size = 100;
    int seenSize = 0;
    bool moving = false;
    ImVec2 moveOffset;
    // Marking the game's interface, in place of the tray, and where the area being dragged started. With detection on,
    // a drag can mark scene instead.
    bool masking = false;
    ImVec2 maskFrom;
    bool maskScene = false;
    bool sizing = false;
    int sizeFrom = 100;
    float sizeAt = 0;
    float sizeAnchor = 0;
    bool sizeBarHeld = false;
    float sizeBarValue = 100;
    double shot = -100;
    // From Settings, and the opacity while its bar is held, before the host has it.
    float opacity = 1;
    bool opacityHeld = false;
    bool small = false;
};
State states[2];
State* state = &states[0];
bool page = false; // in a page of the launcher's window rather than over the game
Open requested = Open::None;

// What one frame of the tray draws from.
struct Frame
{
    const Model& model;
    Host& host;
    const Game* game = nullptr;
    size_t gameIndex = 0;
    const Effects* effects = nullptr;
    bool side = false; // docked at a side
    bool down = false; // opens downward, with the buttons on top
    float width = 0;   // inside the tray's padding
    ImVec2 trayMin;
    float trayWidth = 0;
    float trayHeight = 0;
    float rows = 0; // the height of the buttons
    size_t tileCount = 0;
    size_t onCount = 0;
};

struct TileItem
{
    std::string key;
    size_t index; // in Effects::list
};

// A field of a sheet's grid, drawn at the place the grid gives it.
struct Field
{
    float height = 0;
    int span = 1; // columns, or 0 for a whole row
    std::function<void(ImVec2 min, ImVec2 size)> draw;
};

struct Grid
{
    int columns = 1;
    float column = 0;
};

struct Action
{
    const char* label;
    PillStyle style;
};

// An id that keeps the tray over the game and the one in a page apart.
ImGuiID Key(const char* name)
{
    return ImHashStr(name, 0, page ? 0x50414745u : 0x47414D45u);
}

std::string Lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string Trim(std::string text)
{
    text.erase(0, std::min(text.find_first_not_of(" \r\n\t"), text.size()));
    text.erase(text.find_last_not_of(" \r\n\t") + 1);
    return text;
}

template <size_t N>
int ChoiceOf(const int (&values)[N], int value)
{
    return static_cast<int>(std::find(std::begin(values), std::end(values), value) - std::begin(values));
}

const Game* Played(const Model& model, size_t& index)
{
    for (size_t i = 0; i < model.games.size(); ++i)
        if (model.games[i].running)
        {
            index = i;
            return &model.games[i];
        }
    return nullptr;
}

const Preset* FindPreset(const Game& game, const std::string& id)
{
    for (const Preset& preset : game.presets)
        if (preset.id == id)
            return &preset;
    return nullptr;
}

std::optional<size_t> IndexOf(const Effects& effects, const std::string& key)
{
    for (size_t i = 0; i < effects.list.size(); ++i)
        if (effects.list[i].key == key)
            return i;
    return std::nullopt;
}

// An effect's color of the ring, which is its own in every preset.
ImU32 TileColor(const std::string& key)
{
    return Ring(ImHashStr(key.c_str()));
}

float TitleSize(const Frame& f)
{
    return f.side ? kTextTitleSide : kTextTitle;
}

// Sheets

void OpenSheet(Sheet sheet, float slide = 0)
{
    if (sheet == Sheet::Add && state->sheet != Sheet::Add)
        state->focusSearch = true;
    if (state->sheet == Sheet::Add && sheet != Sheet::Add)
        state->addedHere.clear();
    if (sheet != Sheet::Presets)
        state->naming = Naming::None;
    state->fromClosed = state->sheet == Sheet::None;
    state->slide = slide;
    state->sheet = sheet;
    state->drawn = sheet;
    Snap(Key("sheet enter"), 0);
}

void CloseSheet()
{
    if (state->sheet == Sheet::Add)
        state->addedHere.clear();
    state->sheet = Sheet::None;
    state->naming = Naming::None;
}

void Toggle(Sheet sheet)
{
    if (state->sheet == sheet)
        CloseSheet();
    else
        OpenSheet(sheet);
}

void Ask(Question question, Sheet back)
{
    state->question = question;
    state->back = back == Sheet::Ask ? Sheet::None : back;
    OpenSheet(Sheet::Ask);
}

void Back()
{
    if (state->back == Sheet::None)
        CloseSheet();
    else
        OpenSheet(state->back);
}

// Offers to take back what was just done.
void Offer(std::string text, std::function<void(Frame&)> undo)
{
    state->undoText = std::move(text);
    state->undo = std::move(undo);
    Snap(Key("undo"), 1);
}

// Switches to a preset, asking first about the changes to the one in use. direction is the side its name comes from.
void Use(Frame& f, const std::string& preset, int direction)
{
    if (!f.game || preset == f.game->preset)
        return;
    if (f.game->changed)
    {
        state->pendingPreset = preset;
        state->pendingDirection = direction;
        Ask(Question::Unsaved, state->sheet);
        return;
    }
    state->swapDirection = direction;
    f.host.UsePreset(f.gameIndex, preset);
}

void StepPreset(Frame& f, int step)
{
    if (!f.game || f.game->presets.empty())
        return;
    const std::vector<Preset>& presets = f.game->presets;
    const auto current = std::find_if(presets.begin(), presets.end(), [&](const Preset& preset) { return preset.id == f.game->preset; });
    const int count = static_cast<int>(presets.size());
    const int at = current == presets.end() ? (step > 0 ? -1 : 0) : static_cast<int>(current - presets.begin());
    Use(f, presets[static_cast<size_t>(((at + step) % count + count) % count)].id, step);
}

// Keeps the tiles with the preset in use: a new preset starts with the effects it turns on, and effects turned on
// since join them.
void Follow(Frame& f)
{
    const std::string preset = f.game ? f.game->preset : std::string();
    if (!state->presetKnown || state->preset != preset)
    {
        state->presetKnown = true;
        state->preset = preset;
        state->dragging = false;
        state->held.clear();
        state->addedHere.clear();
        state->undo = nullptr;
        if (state->sheet == Sheet::Tune)
            CloseSheet();
    }
    state->members.clear();
    if (f.effects)
        for (const Effect& effect : f.effects->list)
            if (effect.on || effect.kept)
                state->members.insert(effect.key);
    if (state->sheet == Sheet::Tune && (!f.effects || !IndexOf(*f.effects, state->tuned) || !state->members.count(state->tuned)))
        CloseSheet();

    // The name slides in from the side it came from.
    const Preset* current = f.game ? FindPreset(*f.game, f.game->preset) : nullptr;
    const std::string name = current ? current->name : "No preset";
    if (name != state->presetName)
    {
        if (!state->presetName.empty())
            state->swapped = ImGui::GetTime();
        state->presetName = name;
    }

    // A shortcut left waiting for keys in a sheet that closed would take the next keys pressed.
    if (f.model.settings.recording < 0)
        state->recordingHere = false;
    else if (state->recordingHere && !(state->sheet == Sheet::More && state->tab == Tab::Shortcuts))
    {
        f.host.RecordShortcut(-1);
        state->recordingHere = false;
    }
}

// A grey row with a label on its left, cut off at right, with tip on the label.
void RowBack(ImVec2 min, ImVec2 size)
{
    ImGui::GetWindowDrawList()->AddRectFilled(min, min + size, kWell, S(17.6f));
}

void RowLabel(ImVec2 min, ImVec2 size, const std::string& label, float right, const std::string& tip)
{
    Line(ImGui::GetWindowDrawList(), min.x + S(17.6f), right, min.y + size.y / 2, label, kInk, kTextBody, true);
    if (tip.empty())
        return;
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##label", ImVec2(std::max(right - min.x, 1.0f), size.y));
    Tip(tip);
}

Grid GridOf(float width, float minimum)
{
    const float gap = S(kFieldGap);
    const int columns = std::max(1, static_cast<int>((width + gap) / (minimum + gap)));
    return { columns, (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns) };
}

// Lays fields out in the grid, in a child window that scrolls past maxHeight without a scroll bar, as the design's.
// Returns the height it takes.
float Fields(const char* id, const std::vector<Field>& fields, const Grid& grid, ImVec2 min, float width, float maxHeight)
{
    const float gap = S(kFieldGap);
    std::vector<ImVec2> places(fields.size());
    std::vector<float> widths(fields.size());
    float y = 0, rowHeight = 0;
    int column = 0;
    const auto next = [&] {
        y += rowHeight + gap;
        column = 0;
        rowHeight = 0;
    };
    for (size_t i = 0; i < fields.size(); ++i)
    {
        const int span = fields[i].span == 0 ? grid.columns : std::min(fields[i].span, grid.columns);
        if (column > 0 && column + span > grid.columns)
            next();
        places[i] = ImVec2(static_cast<float>(column) * (grid.column + gap), y);
        widths[i] = fields[i].span == 0 ? width : static_cast<float>(span) * grid.column + static_cast<float>(span - 1) * gap;
        rowHeight = std::max(rowHeight, fields[i].height);
        column += span;
        if (column >= grid.columns)
            next();
    }
    const float total = column > 0 ? y + rowHeight : std::max(y - gap, 0.0f);
    const float height = std::max(std::min(total, maxHeight), 0.0f);
    // A margin around the fields, so ones that grow while held are not cut off.
    const float pad = S(4);
    ImGui::SetCursorScreenPos(min - ImVec2(pad, pad));
    ImGui::BeginChild(id, ImVec2(width + pad * 2, height + pad * 2), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos() + ImVec2(pad, pad);
    for (size_t i = 0; i < fields.size(); ++i)
        fields[i].draw(origin + places[i], ImVec2(widths[i], fields[i].height));
    ImGui::SetCursorScreenPos(origin + ImVec2(0, total + pad));
    ImGui::Dummy(ImVec2(1, 0));
    const ImGuiWindow* child = ImGui::GetCurrentWindow();
    ImGui::EndChild();
    ScrollCue(child);
    return height;
}

Field Heading(const std::string& text)
{
    return { S(30), 0, [text](ImVec2 min, ImVec2 size) {
                Line(ImGui::GetWindowDrawList(), min.x + S(4.8f), min.x + size.x, min.y + S(8) + (size.y - S(8)) / 2, text, kMist, kTextHeading, true);
            } };
}

// A heading that opens and closes what is under it, which storage remembers by key.
Field Category(const std::string& text, ImGuiStorage* storage, ImGuiID key, bool open)
{
    return { S(30), 0, [=](ImVec2 min, ImVec2 size) {
                ImGui::PushID(static_cast<int>(key));
                const float width = Measure(text, kTextHeading, true).x + S(28);
                ImGui::SetCursorScreenPos(min);
                if (ImGui::InvisibleButton("##category", ImVec2(std::min(width, size.x), size.y), ImGuiButtonFlags_EnableNav))
                    storage->SetBool(key, !open);
                const bool hovered = ImGui::IsItemHovered();
                HandOnHover();
                ImGui::PopID();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float middle = min.y + S(8) + (size.y - S(8)) / 2;
                const ImU32 color = hovered ? kInk : kMist;
                Line(draw, min.x + S(4.8f), min.x + size.x, middle, text, color, kTextHeading, true);
                Chevron(draw, ImVec2(min.x + width - S(12), middle), open, color);
            } };
}

Field Note(const std::string& text, ImU32 color, float width)
{
    const float height = Measure(text, kTextBody, false, width - S(9.6f)).y + S(4);
    return { height, 0, [=](ImVec2 min, ImVec2 size) { Paragraph(ImGui::GetWindowDrawList(), min + ImVec2(S(4.8f), S(2)), min.x + size.x - S(4.8f), text, color, kTextBody); } };
}

Field Space(float height)
{
    return { height, 0, [](ImVec2, ImVec2) {} };
}

// A bar filled to fraction, after its label.
Field Progress(const std::string& label, float fraction)
{
    return { S(34), 0, [=](ImVec2 min, ImVec2 size) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float middle = min.y + size.y / 2, inset = S(4.8f), half = S(4);
                const float named = label.empty() ? 0 : std::min(Measure(label, kTextBody, true).x + S(12), size.x / 2);
                if (!label.empty())
                    Line(draw, min.x + inset, min.x + inset + named - S(12), middle, label, kInk, kTextBody, true);
                const float left = min.x + inset + named, right = min.x + size.x - inset;
                draw->AddRectFilled(ImVec2(left, middle - half), ImVec2(right, middle + half), kWell, half);
                if (fraction > 0)
                    draw->AddRectFilled(ImVec2(left, middle - half), ImVec2(left + std::max((right - left) * std::min(fraction, 1.0f), half * 2), middle + half),
                                        kInk, half);
            } };
}

// A row of choices, wide enough for its label and the choices, as one column or two.
Field Choices(const std::string& id, const std::string& label, std::vector<const char*> choices, int selected, std::function<void(int)> pick,
              const std::string& tip, const Grid& grid)
{
    const float need = S(17.6f) + Measure(label, kTextBody, true).x + S(9.6f) + SegmentsWidth(choices) + S(5.6f);
    const int span = need <= grid.column ? 1 : 2;
    return { S(kField), span, [=](ImVec2 min, ImVec2 size) {
                ImGui::PushID(id.c_str());
                RowBack(min, size);
                const float width = SegmentsWidth(choices);
                const ImVec2 at(min.x + size.x - S(5.6f) - width, min.y + (size.y - S(kSegmentHeight)) / 2);
                RowLabel(min, size, label, at.x - S(9.6f), tip);
                const int clicked = Segments("choices", at, choices, selected);
                if (clicked >= 0 && clicked != selected)
                    pick(clicked);
                ImGui::PopID();
            } };
}

// A row with a label and a switch, as one column or two.
Field Switched(const std::string& id, const std::string& label, bool on, std::function<void(bool)> set, const std::string& tip, const Grid& grid)
{
    const float need = S(17.6f) + Measure(label, kTextBody, true).x + S(9.6f) + S(kSwitchWidth) + S(11.2f);
    return { S(kField), need <= grid.column ? 1 : 2, [=](ImVec2 min, ImVec2 size) {
                ImGui::PushID(id.c_str());
                RowBack(min, size);
                const ImVec2 at(min.x + size.x - S(11.2f) - S(kSwitchWidth), min.y + (size.y - S(kSwitchHeight)) / 2);
                RowLabel(min, size, label, at.x - S(9.6f), tip);
                if (SwitchButton("switch", at, on))
                    set(!on);
                ImGui::PopID();
            } };
}

// A row with a label and buttons on its right, or under the label where they do not fit beside it.
Field Actions(const std::string& id, const std::string& label, std::vector<Action> actions, std::function<void(int)> pressed, float width)
{
    float buttons = 0;
    for (const Action& action : actions)
        buttons += PillWidth(action.label, action.style) + S(6.4f);
    const bool beside = S(17.6f) + Measure(label, kTextBody, true).x + S(9.6f) + buttons <= width;
    return { beside ? S(kField) : S(kField) + S(kButton) + S(4), 0, [=](ImVec2 min, ImVec2 size) {
                ImGui::PushID(id.c_str());
                RowBack(min, size);
                if (!label.empty())
                    Line(ImGui::GetWindowDrawList(), min.x + S(17.6f), min.x + size.x - (beside ? buttons : 0), min.y + S(kField) / 2, label, kInk, kTextBody,
                         true);
                float x = beside ? min.x + size.x - buttons : min.x + S(5.6f);
                const float y = beside ? min.y + (S(kField) - S(kButton)) / 2 : min.y + S(kField) - S(4);
                for (size_t i = 0; i < actions.size(); ++i)
                {
                    if (PillButton(actions[i].label, ImVec2(x, y), S(kButton), actions[i].style))
                        pressed(static_cast<int>(i));
                    x += PillWidth(actions[i].label, actions[i].style) + S(6.4f);
                }
                ImGui::PopID();
            } };
}

// The printf format of a setting's value, with any % in its units doubled.
std::string FormatOf(const EffectSetting& setting)
{
    std::string format = setting.floating ? "%.3f" : "%d";
    for (const char c : setting.units)
        format += c == '%' ? std::string("%%") : std::string(1, c);
    return format;
}

// One setting as a field: a fill bar for a number in a range, choices for a check or a short list, and Dear ImGui's
// own controls for the rest. change takes the new value, and reset, when given, resets it on a right-click. active and
// hovered get the setting's index while it is in use or under the mouse.
Field SettingField(const std::string& scope, const EffectSetting& setting, size_t index, ImU32 color, const Grid& grid,
                   std::function<void(const EffectSetting&)> change, std::function<void()> reset, std::optional<size_t>* active,
                   std::optional<size_t>* hovered)
{
    const EffectSetting* s = &setting;
    const std::string id = scope + "/" + std::to_string(index);
    const bool resettable = setting.resettable && static_cast<bool>(reset);
    const auto point = [=](ImVec2 min, ImVec2 size, bool inUse) {
        const bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, min + size);
        if (over && hovered)
            *hovered = index;
        if (active && (inUse || (over && ImGui::IsMouseDown(ImGuiMouseButton_Left))))
            *active = index;
        if (over && resettable && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            reset();
    };

    // What an add-on shows without taking anything.
    const float whole = grid.column * static_cast<float>(grid.columns) + S(kFieldGap) * static_cast<float>(grid.columns - 1);
    if (setting.control == EffectSetting::Control::Heading)
        return Heading(setting.label);
    if (setting.control == EffectSetting::Control::Text)
        return Note(setting.label, kMist, whole);
    if (setting.control == EffectSetting::Control::Progress)
        return Progress(setting.label, setting.floats[0]);

    if (setting.control == EffectSetting::Control::Check)
    {
        Field field = Switched(id, setting.label, setting.ints[0] != 0,
                               [s, change](bool on) {
                                   EffectSetting value = *s;
                                   value.ints[0] = on ? 1 : 0;
                                   change(value);
                               },
                               setting.tooltip, grid);
        field.draw = [draw = field.draw, point](ImVec2 min, ImVec2 size) {
            draw(min, size);
            point(min, size, false);
        };
        return field;
    }

    if (setting.control == EffectSetting::Control::Button)
        return Actions(id, "", { { s->label.c_str(), PillStyle::Well } }, [s, change](int) { change(*s); },
                       grid.column * static_cast<float>(grid.columns) + S(kFieldGap) * static_cast<float>(grid.columns - 1));

    if (setting.control == EffectSetting::Control::List)
    {
        std::vector<const char*> choices;
        for (size_t at = 0; at < setting.items.size() && setting.items[at]; at += std::strlen(setting.items.c_str() + at) + 1)
            choices.push_back(setting.items.c_str() + at);
        const float need = S(17.6f) + Measure(setting.label, kTextBody, true).x + S(9.6f) + SegmentsWidth(choices) + S(5.6f);
        const float most = grid.columns >= 2 ? grid.column * 2 + S(kFieldGap) : grid.column;
        if (need <= most && !choices.empty())
        {
            Field field = Choices(id, setting.label, choices, setting.ints[0],
                                  [s, change](int choice) {
                                      EffectSetting value = *s;
                                      value.ints[0] = choice;
                                      change(value);
                                  },
                                  setting.tooltip, grid);
            field.draw = [draw = field.draw, point](ImVec2 min, ImVec2 size) {
                draw(min, size);
                point(min, size, false);
            };
            return field;
        }
        // Too many choices to show side by side: a list that opens.
        return { S(kField), 1, [=](ImVec2 min, ImVec2 size) {
                    ImGui::PushID(id.c_str());
                    RowBack(min, size);
                    const float width = std::floor(size.x * 0.55f);
                    const float left = min.x + size.x - S(5.6f) - width;
                    RowLabel(min, size, s->label, left - S(9.6f), s->tooltip);
                    const int selected = s->ints[0];
                    const char* preview = selected >= 0 && static_cast<size_t>(selected) < choices.size() ? choices[static_cast<size_t>(selected)] : "";
                    // A pill with the choice in it, which opens the rest as a menu.
                    const ImVec2 boxMin(left, min.y + (size.y - S(kButton)) / 2), boxSize(width, S(kButton));
                    const float boxMiddle = boxMin.y + boxSize.y / 2;
                    ImGui::SetCursorScreenPos(boxMin);
                    const bool pressed = ImGui::InvisibleButton("##list", boxSize, ImGuiButtonFlags_EnableNav);
                    const bool hovered = ImGui::IsItemHovered();
                    if (hovered)
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (pressed)
                        ImGui::OpenPopup("choices");
                    const bool open = ImGui::IsPopupOpen("choices");
                    ImDrawList* list = ImGui::GetWindowDrawList();
                    list->AddRectFilled(boxMin, boxMin + boxSize, hovered || open ? kWellHover : kWell, boxSize.y / 2);
                    Line(list, boxMin.x + S(14.4f), boxMin.x + boxSize.x - S(32), boxMiddle, preview, kInk, kTextBody, true);
                    Chevron(list, ImVec2(boxMin.x + boxSize.x - S(17.6f), boxMiddle), true, kMist);

                    // Under the pill, or over it where the screen ends first.
                    const float gap = S(6.4f), most = S(326);
                    const float height = std::min(static_cast<float>(choices.size()) * S(36.8f) + S(12.8f), most);
                    const bool above = boxMin.y + boxSize.y + gap + height > ImGui::GetIO().DisplaySize.y;
                    ImGui::SetNextWindowPos(ImVec2(boxMin.x + boxSize.x, above ? boxMin.y - gap : boxMin.y + boxSize.y + gap), ImGuiCond_Appearing,
                                            ImVec2(1, above ? 1.0f : 0.0f));
                    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, most));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6.4f), S(6.4f)));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(1.6f)));
                    bool inUse = false;
                    if (ImGui::BeginPopup("choices"))
                    {
                        inUse = true;
                        for (size_t i = 0; i < choices.size(); ++i)
                        {
                            ImGui::PushID(static_cast<int>(i));
                            const bool chosen = MenuRow(choices[i]);
                            ImGui::PopID();
                            // A dot marks the choice in use.
                            if (static_cast<int>(i) == selected)
                            {
                                const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
                                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(rowMax.x - S(17.6f), (rowMin.y + rowMax.y) / 2), S(3.6f), kInk);
                                if (ImGui::IsWindowAppearing())
                                    ImGui::SetScrollHereY();
                            }
                            if (chosen)
                            {
                                if (static_cast<int>(i) != selected)
                                {
                                    EffectSetting value = *s;
                                    value.ints[0] = static_cast<int>(i);
                                    change(value);
                                }
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        MenuAppear();
                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleVar(2);
                    point(min, size, inUse);
                    ImGui::PopID();
                } };
    }

    const bool ranged = setting.range && setting.max > setting.min &&
                        (setting.control == EffectSetting::Control::Slider || setting.control == EffectSetting::Control::Drag);
    if (ranged)
    {
        const std::string hint = resettable ? "Drag, or double-click to type. Right-click resets." : "Drag, or double-click to type.";
        Bar bar;
        bar.min = setting.min;
        bar.max = setting.max;
        bar.integer = !setting.floating;
        bar.format = FormatOf(setting);
        bar.color = color;
        bar.tip = setting.tooltip.empty() ? hint : setting.tooltip;
        const int count = std::clamp(setting.count, 1, 4);
        if (count == 1)
        {
            bar.label = setting.label;
            return { S(kField), 1, [=](ImVec2 min, ImVec2 size) {
                        ImGui::PushID(id.c_str());
                        float value = s->floating ? s->floats[0] : static_cast<float>(s->ints[0]);
                        bool held = false;
                        if (FillBar("value", min, size, bar, value, nullptr, &held))
                        {
                            EffectSetting changed = *s;
                            if (s->floating)
                                changed.floats[0] = value;
                            else
                                changed.ints[0] = static_cast<int>(std::lround(value));
                            change(changed);
                        }
                        point(min, size, held);
                        ImGui::PopID();
                    } };
        }
        // Several values: a bar for each beside the label.
        return { S(kField), 2, [=](ImVec2 min, ImVec2 size) {
                    ImGui::PushID(id.c_str());
                    RowBack(min, size);
                    const float labelWidth = std::floor(size.x * 0.3f);
                    RowLabel(min, size, s->label, min.x + labelWidth - S(4), s->tooltip);
                    const float inset = S(5.6f), gap = S(4.8f);
                    const float each = (size.x - labelWidth - inset - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
                    Bar part = bar;
                    part.rounding = 12.8f;
                    bool held = false;
                    for (int i = 0; i < count; ++i)
                    {
                        ImGui::PushID(i);
                        float value = s->floating ? s->floats[i] : static_cast<float>(s->ints[i]);
                        bool partHeld = false;
                        if (FillBar("value", ImVec2(min.x + labelWidth + static_cast<float>(i) * (each + gap), min.y + inset),
                                    ImVec2(each, size.y - inset * 2), part, value, nullptr, &partHeld))
                        {
                            EffectSetting changed = *s;
                            if (s->floating)
                                changed.floats[i] = value;
                            else
                                changed.ints[i] = static_cast<int>(std::lround(value));
                            change(changed);
                        }
                        held |= partHeld;
                        ImGui::PopID();
                    }
                    point(min, size, held);
                    ImGui::PopID();
                } };
    }

    // Colors, numbers without a range, and numbers typed in: Dear ImGui's controls on the right of the row.
    const int span = setting.count > 1 || setting.control == EffectSetting::Control::Color ? 2 : 1;
    return { S(kField), span, [=](ImVec2 min, ImVec2 size) {
                ImGui::PushID(id.c_str());
                RowBack(min, size);
                const float width = std::floor(size.x * (span == 2 ? 0.65f : 0.55f));
                const float left = min.x + size.x - S(5.6f) - width;
                RowLabel(min, size, s->label, left - S(9.6f), s->tooltip);
                PushSize(kTextBody);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), std::max((S(kButton) - ImGui::GetFontSize()) / 2, 0.0f)));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(S(6), S(6)));
                ImGui::SetCursorScreenPos(ImVec2(left, min.y + (size.y - ImGui::GetFrameHeight()) / 2));
                ImGui::SetNextItemWidth(width);
                float floats[4];
                int ints[4];
                std::copy(std::begin(s->floats), std::end(s->floats), floats);
                std::copy(std::begin(s->ints), std::end(s->ints), ints);
                const ImGuiDataType type = s->floating ? ImGuiDataType_Float : ImGuiDataType_S32;
                void* values = s->floating ? static_cast<void*>(floats) : static_cast<void*>(ints);
                const int low = static_cast<int>(s->min), high = static_cast<int>(s->max);
                const void* lowest = !s->range ? nullptr : s->floating ? static_cast<const void*>(&s->min) : &low;
                const void* highest = !s->range ? nullptr : s->floating ? static_cast<const void*>(&s->max) : &high;
                const std::string format = FormatOf(*s);
                const int count = std::clamp(s->count, 1, 4);
                bool changed = false;
                switch (s->control)
                {
                case EffectSetting::Control::Color:
                    changed = count == 3 ? ImGui::ColorEdit3("##value", floats) : ImGui::ColorEdit4("##value", floats, ImGuiColorEditFlags_AlphaBar);
                    break;
                case EffectSetting::Control::Input:
                    changed = ImGui::InputScalarN("##value", type, values, count, nullptr, nullptr, format.c_str());
                    break;
                default:
                    changed = ImGui::DragScalarN("##value", type, values, count, s->step > 0 ? s->step : 0.01f, lowest, highest, format.c_str());
                    break;
                }
                const bool inUse = ImGui::IsItemActive();
                ImGui::PopStyleVar(3);
                ImGui::PopFont();
                if (changed)
                {
                    EffectSetting value = *s;
                    std::copy(std::begin(floats), std::end(floats), value.floats);
                    std::copy(std::begin(ints), std::end(ints), value.ints);
                    change(value);
                }
                point(min, size, inUse);
                ImGui::PopID();
            } };
}

// The sheet's title with its line under it, and its buttons on the right, level with the bottom of the text. The
// buttons go under the text where they do not fit beside it. Returns the index of the button clicked, or -1.
int Header(const Frame& f, ImVec2 min, float width, const std::string& title, const std::string& detail, std::initializer_list<Action> actions, float& height,
           bool wrap = false)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float gap = S(6.4f), buttonHeight = S(kButton);
    float buttons = 0;
    for (const Action& action : actions)
        buttons += PillWidth(action.label, action.style) + gap;
    buttons = std::max(buttons - gap, 0.0f);
    const bool beside = width - buttons - S(12.8f) >= S(160);
    const float textWidth = beside ? width - buttons - S(12.8f) : width;
    const float titleSize = TitleSize(f);
    const float titleHeight = title.empty() ? 0 : Measure(title, titleSize, true, wrap ? textWidth : -1.0f).y;
    const float detailHeight = detail.empty() ? 0 : Measure(detail, kTextBody, false, textWidth).y;
    const float textHeight = titleHeight + (titleHeight > 0 && detailHeight > 0 ? S(4.8f) : 0) + detailHeight;
    height = beside ? std::max(textHeight, buttonHeight) : textHeight + (actions.size() ? S(9.6f) + buttonHeight : 0);
    float y = min.y + (beside ? height - textHeight : 0);
    if (!title.empty())
    {
        if (wrap)
            Paragraph(draw, ImVec2(min.x, y), min.x + textWidth, title, kInk, titleSize, true);
        else
            Line(draw, min.x, min.x + textWidth, y + titleHeight / 2, title, kInk, titleSize, true);
        y += titleHeight + S(4.8f);
    }
    if (!detail.empty())
        Paragraph(draw, ImVec2(min.x, y), min.x + textWidth, detail, kMist, kTextBody);
    float x = min.x + width - buttons;
    const float buttonTop = min.y + height - buttonHeight;
    int clicked = -1, index = 0;
    for (const Action& action : actions)
    {
        if (PillButton(action.label, ImVec2(x, buttonTop), buttonHeight, action.style))
            clicked = index;
        x += PillWidth(action.label, action.style) + gap;
        ++index;
    }
    return clicked;
}

float MaxHeight(const Frame& f, float share, float space, float used)
{
    return std::max(std::min(ImGui::GetIO().DisplaySize.y * (f.side ? 0.26f : share), space - used - S(9.6f)), S(60));
}

// An effect's settings, its definitions, and Reset, Remove and Done.
float TuneSheet(Frame& f, ImVec2 min, float width, float space)
{
    const std::optional<size_t> found = f.effects ? IndexOf(*f.effects, state->tuned) : std::nullopt;
    if (!found)
        return 0;
    const size_t index = *found;
    const Effect& effect = f.effects->list[index];
    const EffectSettings* settings = f.host.DescribeSettings(index);
    const bool resettable = settings && std::any_of(settings->settings.begin(), settings->settings.end(), [](const EffectSetting& s) { return s.resettable; });
    const std::string detail = settings && !settings->tooltip.empty() ? settings->tooltip : effect.file;
    float height = 0;
    const int clicked = resettable ? Header(f, min, width, effect.name, detail, { { "Reset", PillStyle::Text }, { "Remove", PillStyle::Text }, { "Done", PillStyle::Black } }, height)
                                   : Header(f, min, width, effect.name, detail, { { "Remove", PillStyle::Text }, { "Done", PillStyle::Black } }, height);
    const int action = resettable ? clicked : clicked < 0 ? -1 : clicked + 1;
    if (action == 0)
    {
        state->resetting = effect.key;
        Ask(Question::ResetEffect, Sheet::Tune);
    }
    else if (action == 1)
    {
        f.host.RemoveEffect(index);
        Offer("Removed " + effect.name, [key = effect.key](Frame& frame) {
            if (const std::optional<size_t> at = frame.effects ? IndexOf(*frame.effects, key) : std::nullopt)
                frame.host.SetEffect(*at, true);
        });
        CloseSheet();
    }
    else if (action == 2)
        CloseSheet();
    float y = height + S(14.4f);
    if (!settings)
        return y;

    const Grid grid = GridOf(width, f.side ? width : S(kFieldWidth));
    const ImU32 color = TileColor(effect.key);
    std::vector<Field> fields;
    std::optional<size_t> active, hovered;
    if (effect.failed)
        fields.push_back(Note("This effect could not start on the graphics card.", kDanger, width));
    // Grouped by category in the order they first appear, as in ReShade, each open unless its first setting asks
    // otherwise.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGui::PushID(effect.key.c_str());
    std::vector<const EffectSetting*> categories;
    for (const EffectSetting& setting : settings->settings)
        if (std::none_of(categories.begin(), categories.end(), [&](const EffectSetting* first) { return first->category == setting.category; }))
            categories.push_back(&setting);
    if (categories.empty() && settings->definitions.empty())
        fields.push_back(Note("This effect has no settings.", kMist, width));
    for (const EffectSetting* first : categories)
    {
        const std::string& category = first->category;
        if (!category.empty())
        {
            const ImGuiID key = ImGui::GetID(category.c_str());
            const bool open = storage->GetBool(key, !first->categoryClosed);
            fields.push_back(Category(Trim(category), storage, key, open));
            if (!open)
                continue;
        }
        for (size_t i = 0; i < settings->settings.size(); ++i)
        {
            const EffectSetting& setting = settings->settings[i];
            if (setting.category != category)
                continue;
            if (setting.spacing > 0)
                fields.push_back(Space(S(12) * static_cast<float>(setting.spacing)));
            if (!setting.text.empty())
                fields.push_back(Note(setting.text, kMist, width));
            fields.push_back(SettingField(
                "", setting, i, color, grid, [&f, index, i](const EffectSetting& value) { f.host.SetSetting(index, i, value); },
                [&f, index, i, &setting, key = effect.key] {
                    Offer("Reset " + setting.label, [key, i, previous = setting](Frame& frame) {
                        if (const std::optional<size_t> at = frame.effects ? IndexOf(*frame.effects, key) : std::nullopt)
                            frame.host.SetSetting(*at, i, previous);
                    });
                    f.host.ResetSettings(index, i);
                },
                &active, &hovered));
        }
    }
    // The preprocessor definitions the effect checks. A change compiles the effects again, so it applies on Enter, as
    // in ReShade's menu.
    if (!settings->definitions.empty())
    {
        const ImGuiID key = ImGui::GetID("##definitions");
        const bool open = storage->GetBool(key, false);
        fields.push_back(Category("Preprocessor definitions", storage, key, open));
        if (open)
            fields.push_back(Note("Enter applies a value, and the effects compile again. Right-click one to go back to the effect's own.", kMist, width));
        for (size_t i = 0; open && i < settings->definitions.size(); ++i)
        {
            const EffectDefinition* definition = &settings->definitions[i];
            fields.push_back({ S(kField), 1, [&f, index, i, definition](ImVec2 at, ImVec2 size) {
                                  ImGui::PushID(definition->name.c_str());
                                  RowBack(at, size);
                                  const float width = std::floor(size.x * 0.5f);
                                  const float left = at.x + size.x - S(5.6f) - width;
                                  RowLabel(at, size, definition->name, left - S(9.6f), {});
                                  PushSize(kTextBody);
                                  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), std::max((S(kButton) - ImGui::GetFontSize()) / 2, 0.0f)));
                                  ImGui::SetCursorScreenPos(ImVec2(left, at.y + (size.y - ImGui::GetFrameHeight()) / 2));
                                  ImGui::SetNextItemWidth(width);
                                  char value[256] = {};
                                  definition->value.copy(value, sizeof(value) - 1);
                                  if (ImGui::InputTextWithHint("##value", definition->fallback.c_str(), value, sizeof(value),
                                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) &&
                                      definition->value != value)
                                      f.host.SetDefinition(index, i, value);
                                  ImGui::PopStyleVar();
                                  ImGui::PopFont();
                                  if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(at, at + size) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                                      f.host.SetDefinition(index, i, "");
                                  ImGui::PopID();
                              } });
        }
    }
    y += Fields("##settings", fields, grid, min + ImVec2(0, y), width, MaxHeight(f, 0.38f, space, y));
    ImGui::PopID();
    f.host.PointAtSettings(index, active, hovered);
    return y;
}

// The effects the preset does not use, to add, and the effect files that did not compile.
float AddSheet(Frame& f, ImVec2 min, float width, float space)
{
    float height = 0;
    if (Header(f, min, width, "Add an effect", "", { { "Done", PillStyle::Black } }, height) == 0)
        CloseSheet();
    float y = height + S(14.4f);
    if (!f.effects)
        return y;
    const Effects& effects = *f.effects;
    // The packs the effects came in, to list one at a time, as chips under the search box. Searching
    // finds a pack by its name too.
    std::set<std::string> packs;
    for (const Effect& effect : effects.list)
        if (!effect.pack.empty())
            packs.insert(effect.pack);
    if (!packs.count(state->pack))
        state->pack.clear();
    SearchField("##search", "Search effects", state->search, sizeof(state->search), min + ImVec2(0, y), width,
                std::exchange(state->focusSearch, false));
    // Enter adds the first effect found, and the box is ready for the next.
    const bool entered = ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    y += S(kSearchHeight) + S(9.6f);
    if (!packs.empty())
    {
        // One row, which the wheel moves along, since there can be dozens of packs. The chosen one is black, and
        // clicking it again lists every pack.
        const float chip = S(35.2f), gap = S(6.4f);
        ImGui::SetCursorScreenPos(min + ImVec2(0, y));
        ImGui::BeginChild("##packs", ImVec2(width, chip), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0)
            ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * S(160));
        const ImVec2 origin = ImGui::GetWindowPos() - ImVec2(ImGui::GetScrollX(), 0);
        float x = 0;
        const auto add = [&](const std::string& label, const std::string& pack) {
            const bool chosen = state->pack == pack;
            if (PillButton(label.c_str(), origin + ImVec2(x, 0), chip, chosen ? PillStyle::Black : PillStyle::Well))
                state->pack = chosen ? std::string() : pack;
            x += PillWidth(label.c_str(), PillStyle::Well) + gap;
        };
        add("All##every pack", "");
        for (const std::string& pack : packs)
            add(pack + "##pack", pack);
        ImGui::EndChild();
        y += chip + S(9.6f);
    }

    const std::string filter = Lower(state->search);
    std::vector<size_t> listed;
    for (size_t i = 0; i < effects.list.size(); ++i)
    {
        const Effect& effect = effects.list[i];
        if (state->members.count(effect.key) && !state->addedHere.count(effect.key))
            continue;
        if (!state->pack.empty() && effect.pack != state->pack)
            continue;
        if (filter.empty() || Lower(effect.name).find(filter) != std::string::npos || Lower(effect.file).find(filter) != std::string::npos ||
            Lower(effect.description).find(filter) != std::string::npos || Lower(effect.pack).find(filter) != std::string::npos)
            listed.push_back(i);
    }
    std::sort(listed.begin(), listed.end(), [&](size_t a, size_t b) { return Lower(effects.list[a].name) < Lower(effects.list[b].name); });
    if (entered && !filter.empty() && !listed.empty() && !state->members.count(effects.list[listed.front()].key))
    {
        f.host.SetEffect(listed.front(), true);
        state->addedHere.insert(effects.list[listed.front()].key);
        state->search[0] = 0;
        state->focusSearch = true;
    }

    const Grid grid = GridOf(width, f.side ? width : S(kCardWidth));
    std::vector<Field> fields;
    if (effects.loading && effects.list.empty())
        fields.push_back(Note("Loading effects. The first time can take a minute.", kMist, width));
    else if (effects.list.empty() && effects.broken.empty())
        fields.push_back(Note("No effects are installed.", kMist, width));
    else if (listed.empty() && !effects.list.empty())
        fields.push_back(Note(filter.empty() ? "Every effect is in the preset." : "No effects match.", kMist, width));
    for (const size_t index : listed)
    {
        const Effect* effect = &effects.list[index];
        const bool added = state->members.count(effect->key) > 0;
        fields.push_back({ S(64), 1, [&f, index, effect, added](ImVec2 at, ImVec2 size) {
                              ImGui::PushID(effect->key.c_str());
                              ImGui::SetCursorScreenPos(at);
                              const bool clicked = ImGui::InvisibleButton("add", size, ImGuiButtonFlags_EnableNav);
                              const ImGuiID key = ImGui::GetItemID();
                              const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
                              HandOnHover();
                              ImDrawList* draw = ImGui::GetWindowDrawList();
                              const int start = draw->VtxBuffer.Size;
                              draw->AddRectFilled(at, at + size, hovered ? kWellHover : kWell, S(17.6f));
                              const float right = at.x + size.x - S(51.2f);
                              Line(draw, at.x + S(16), right, at.y + size.y / 2 - S(9), effect->name, kInk, kTextBody, true);
                              Line(draw, at.x + S(16), right, at.y + size.y / 2 + S(11),
                                   effect->failed ? "Could not start on the graphics card" : effect->description.empty() ? effect->file : effect->description,
                                   effect->failed ? kDanger : Alpha(kInk, 0.65f), kTextSmall);
                              // A plus that turns into a check in the tile's color once it is added.
                              const ImVec2 center(at.x + size.x - S(11.2f) - S(16), at.y + size.y / 2);
                              const float fill = Animate(SubId(key, "fill"), added ? 1.0f : 0.0f, 0.35f, Curve::Out);
                              const float turn = Animate(SubId(key, "turn"), added ? 2 * IM_PI : 0.0f, 0.35f, Curve::Spring);
                              draw->AddCircleFilled(center, S(16), Mix(kInk, TileColor(effect->key), fill));
                              DrawIcon(draw, added ? Icon::Check : Icon::Plus, center, S(16), Mix(kOnInk, kOnColor, fill), turn);
                              Transform(draw, start, at + size * 0.5f, Animate(SubId(key, "press"), held ? 0.96f : 1, 0.25f, Curve::Spring));
                              ImGui::PopID();
                              if (!clicked)
                                  return;
                              if (added)
                              {
                                  f.host.RemoveEffect(index);
                                  state->addedHere.erase(effect->key);
                              }
                              else
                              {
                                  f.host.SetEffect(index, true);
                                  state->addedHere.insert(effect->key);
                              }
                          } });
    }
    if (!effects.broken.empty())
    {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetID("##broken");
        const bool open = storage->GetBool(key, false);
        fields.push_back(Category("Did not compile  " + std::to_string(effects.broken.size()), storage, key, open));
        for (size_t i = 0; open && i < effects.broken.size(); ++i)
        {
            const BrokenEffect* broken = &effects.broken[i];
            fields.push_back(Actions("broken " + broken->file, broken->file, { { state->shownErrors == broken->file ? "Hide errors" : "Errors", PillStyle::Text } },
                                     [broken](int) { state->shownErrors = state->shownErrors == broken->file ? std::string() : broken->file; }, width));
            if (state->shownErrors == broken->file)
                fields.push_back(Note(broken->errors, kMist, width));
        }
    }
    y += Fields("##library", fields, grid, min + ImVec2(0, y), width, MaxHeight(f, 0.36f, space, y));
    return y;
}

void StartNaming(Naming naming, const std::string& target, const std::string& name)
{
    state->naming = naming;
    state->namingTarget = target;
    state->nameFailed = false;
    state->focusName = true;
    std::snprintf(state->name, sizeof(state->name), "%s", name.c_str());
}

// The box that names a preset, in place of a preset's card or the card for a new one.
void NameInput(Frame& f, ImVec2 min, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, min + size, kWell, S(17.6f));
    const float ring = S(2.88f);
    draw->AddRect(min + ImVec2(ring, ring) * 0.5f, min + size - ImVec2(ring, ring) * 0.5f, kInk, S(17.6f) - ring / 2, ring);
    PushSize(kTextBody, true);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
    ImGui::SetCursorScreenPos(ImVec2(min.x + S(16), min.y + (size.y - ImGui::GetFrameHeight()) / 2));
    ImGui::SetNextItemWidth(size.x - S(32));
    if (std::exchange(state->focusName, false))
        ImGui::SetKeyboardFocusHere();
    const bool entered = ImGui::InputText("##name", state->name, sizeof(state->name), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemEdited())
        state->nameFailed = false;
    const bool escaped = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool away = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered() && !ImGui::IsItemActive();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    // Enter keeps the box open when the name is not taken. Escape and a click elsewhere drop the name.
    if (entered)
    {
        bool done = false;
        switch (state->naming)
        {
        case Naming::New: done = f.host.NewPreset(f.gameIndex, state->name); break;
        case Naming::Copy: done = f.host.CopyPreset(f.gameIndex, state->namingTarget, state->name); break;
        case Naming::Rename: done = f.host.RenamePreset(state->namingTarget, state->name); break;
        case Naming::None: break;
        }
        if (done)
            state->naming = Naming::None;
        else
        {
            state->nameFailed = true;
            state->focusName = true;
        }
    }
    else if (escaped || away)
        state->naming = Naming::None;
}

// The menu of a preset's card.
void PresetMenu(Frame& f, const Preset& preset)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6.4f), S(6.4f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(1.6f)));
    if (ImGui::BeginPopup("actions"))
    {
        // The folders take the place of the actions, rather than opening beside them.
        static bool moving = false;
        if (ImGui::IsWindowAppearing())
            moving = false;
        // To every folder but its own.
        const std::string& current = preset.folder.empty() ? f.game->name : preset.folder;
        if (moving)
        {
            if (MenuRow("Back", true, kMist))
                moving = false;
            for (const Folder& folder : f.model.folders)
                if (folder.name != current && MenuRow(folder.name.c_str()))
                {
                    f.host.MovePreset(preset.id, folder.id);
                    ImGui::CloseCurrentPopup();
                }
        }
        else
        {
            if (MenuRow("Duplicate"))
            {
                StartNaming(Naming::Copy, preset.id, preset.name + " copy");
                ImGui::CloseCurrentPopup();
            }
            if (MenuRow("Rename", !preset.loaded))
            {
                StartNaming(Naming::Rename, preset.id, preset.name);
                ImGui::CloseCurrentPopup();
            }
            if (MenuRow("Move to", !preset.loaded, kInk, true))
                moving = true;
            if (MenuRow("Delete", !preset.loaded, kDanger))
            {
                state->deleting = preset.id;
                Ask(Question::Delete, Sheet::Presets);
                ImGui::CloseCurrentPopup();
            }
            if (preset.loaded)
            {
                const ImVec2 at = ImGui::GetCursorScreenPos() + ImVec2(S(12.8f), S(6.4f));
                const float height = Paragraph(ImGui::GetWindowDrawList(), at, at.x + S(kMenuRowWidth - 25.6f),
                                               "Switch to another preset to rename, move or delete this one.", kMist, kTextSmall);
                ImGui::Dummy(ImVec2(S(kMenuRowWidth), height + S(12.8f)));
            }
        }
        MenuAppear();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// What a screenshot holds, and the buttons that take one. Screenshots leave out the tray.
float ShotSheet(Frame& f, ImVec2 min, float width, float space)
{
    const Settings& settings = f.model.settings;
    float height = 0;
    const bool copies = settings.copyScreenshots;
    const int clicked = copies ? Header(f, min, width, "Screenshot", "", { { "Copy", PillStyle::Text }, { "Take screenshot", PillStyle::Black } }, height)
                               : Header(f, min, width, "Screenshot", "", { { "Take screenshot", PillStyle::Black } }, height);
    if (clicked >= 0)
    {
        f.host.TakeScreenshot(copies && clicked == 0);
        state->shot = ImGui::GetTime();
    }
    float y = height + S(14.4f);

    const Grid grid = GridOf(width, f.side ? width : S(kFieldWidth));
    std::vector<Field> fields;
    // One of the two stays on: turning the last one off turns the other on.
    fields.push_back(Switched("effects", "With effects", settings.shotEffects,
                              [&f, &settings](bool on) {
                                  f.host.SetSwitch(Switch::ShotEffects, on);
                                  if (!on && !settings.shotOriginal)
                                      f.host.SetSwitch(Switch::ShotOriginal, true);
                              },
                              {}, grid));
    fields.push_back(Switched("original", "Without effects", settings.shotOriginal,
                              [&f, &settings](bool on) {
                                  f.host.SetSwitch(Switch::ShotOriginal, on);
                                  if (!on && !settings.shotEffects)
                                      f.host.SetSwitch(Switch::ShotEffects, true);
                              },
                              {}, grid));
    if (settings.shotEffects && settings.shotOriginal)
        fields.push_back(Choices("both", "Both", { "Side by side", "Two pictures" }, settings.shotTwoFiles ? 1 : 0,
                                 [&f](int choice) { f.host.SetSwitch(Switch::ShotTwoFiles, choice == 1); }, {}, grid));
    fields.push_back(Actions("folder", "Saved screenshots", { { "Open folder", PillStyle::Text } }, [&f](int) { f.host.OpenScreenshots(); }, width));
    y += Fields("##shot", fields, grid, min + ImVec2(0, y), width, MaxHeight(f, 0.38f, space, y));
    return y;
}

// The game's presets by folder, to use, and to make, copy, rename, move and delete where the model offers it.
float PresetsSheet(Frame& f, ImVec2 min, float width, float space)
{
    float height = 0;
    const bool edit = f.model.editPresets && f.game;
    const int clicked = edit ? Header(f, min, width, "Presets", "", { { "New preset", PillStyle::Text }, { "Open folder", PillStyle::Text }, { "Done", PillStyle::Black } }, height)
                             : Header(f, min, width, "Presets", "", { { "Open folder", PillStyle::Text }, { "Done", PillStyle::Black } }, height);
    const int action = edit ? clicked : clicked < 0 ? -1 : clicked + 1;
    if (action == 0)
        StartNaming(Naming::New, {}, "New preset");
    else if (action == 1 && f.game)
        f.host.OpenPresetsFolder(f.gameIndex);
    else if (action == 2)
        CloseSheet();
    float y = height + S(14.4f);
    if (!f.game)
        return y;
    const Game& game = *f.game;
    const Grid grid = GridOf(width, f.side ? width : S(kCardWidth));
    std::vector<Field> fields;
    const auto current = std::find_if(game.presets.begin(), game.presets.end(), [&](const Preset& preset) { return preset.id == game.preset; });
    const size_t currentIndex = static_cast<size_t>(current - game.presets.begin());
    std::optional<std::string> folder;
    for (size_t i = 0; i < game.presets.size(); ++i)
    {
        const Preset* preset = &game.presets[i];
        if (folder != preset->folder)
        {
            folder = preset->folder;
            fields.push_back(Heading(preset->folder.empty() ? game.name : preset->folder));
        }
        fields.push_back({ S(73.6f), 1, [&f, preset, i, currentIndex, edit](ImVec2 at, ImVec2 size) {
                              ImGui::PushID(preset->id.c_str());
                              if (state->naming == Naming::Rename && state->namingTarget == preset->id)
                              {
                                  NameInput(f, at, size);
                                  ImGui::PopID();
                                  return;
                              }
                              const bool used = preset->id == f.game->preset;
                              ImGui::SetCursorScreenPos(at);
                              if (edit)
                                  ImGui::SetNextItemAllowOverlap();
                              const bool clicked = ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav);
                              const ImGuiID key = ImGui::GetItemID();
                              const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
                              if (!used)
                                  HandOnHover();
                              ImDrawList* draw = ImGui::GetWindowDrawList();
                              const int start = draw->VtxBuffer.Size;
                              draw->AddRectFilled(at, at + size, used ? kInk : hovered ? kWellHover : kWell, S(17.6f));
                              const ImU32 text = used ? kOnInk : kInk;
                              Line(draw, at.x + S(16), at.x + size.x - S(edit ? 44.0f : 16.0f), at.y + S(22), preset->name, text, kTextBody, true);
                              // The preset in use shows its tiles' colors, grey for the ones turned off.
                              if (used && f.effects)
                              {
                                  float x = at.x + S(16) + S(5.6f);
                                  for (const Effect& effect : f.effects->list)
                                  {
                                      if (!state->members.count(effect.key) || x > at.x + size.x - S(16))
                                          continue;
                                      draw->AddCircleFilled(ImVec2(x, at.y + size.y - S(16.8f)), S(5.6f), effect.on ? TileColor(effect.key) : Color(0x9A9AA6));
                                      x += S(15.2f);
                                  }
                              }
                              Transform(draw, start, at + size * 0.5f, Animate(SubId(key, "press"), held && !used ? 0.96f : 1, 0.25f, Curve::Spring));
                              if (clicked && !used)
                                  Use(f, preset->id, i > currentIndex ? 1 : -1);
                              if (edit)
                              {
                                  const float button = S(28);
                                  const ImVec2 dots(at.x + size.x - S(8) - button, at.y + S(8));
                                  ImGui::SetCursorScreenPos(dots);
                                  if (ImGui::InvisibleButton("##actions", ImVec2(button, button), ImGuiButtonFlags_EnableNav))
                                      ImGui::OpenPopup("actions");
                                  const bool over = ImGui::IsItemHovered();
                                  HandOnHover();
                                  if (over)
                                      draw->AddCircleFilled(dots + ImVec2(button, button) * 0.5f, button / 2, Alpha(text, 0.18f));
                                  DrawIcon(draw, Icon::Dots, dots + ImVec2(button, button) * 0.5f, S(20), text);
                                  PresetMenu(f, *preset);
                              }
                              ImGui::PopID();
                          } });
    }
    // A new preset, or a copy of this one.
    if (edit)
    {
        const bool naming = state->naming == Naming::New || state->naming == Naming::Copy;
        const bool copy = current != game.presets.end();
        const std::string currentId = copy ? current->id : std::string();
        const std::string currentName = copy ? current->name : std::string();
        fields.push_back({ S(73.6f), 1, [&f, naming, copy, currentId, currentName](ImVec2 at, ImVec2 size) {
                              ImGui::PushID("##new");
                              if (naming)
                              {
                                  NameInput(f, at, size);
                                  ImGui::PopID();
                                  return;
                              }
                              ImGui::SetCursorScreenPos(at);
                              const bool clicked = ImGui::InvisibleButton("new", size, ImGuiButtonFlags_EnableNav);
                              const bool hovered = ImGui::IsItemHovered();
                              HandOnHover();
                              ImDrawList* draw = ImGui::GetWindowDrawList();
                              DashedBox(draw, at, at + size, S(17.6f), hovered ? Alpha(kInk, 0.5f) : kFaint, S(2.4f));
                              Line(draw, at.x + S(12), at.x + size.x - S(12), at.y + size.y / 2, copy ? "New preset from this one" : "New preset", hovered ? kInk : kMist,
                                   kTextBody, true, true);
                              if (clicked)
                              {
                                  if (copy)
                                      StartNaming(Naming::Copy, currentId, currentName + " copy");
                                  else
                                      StartNaming(Naming::New, {}, "New preset");
                              }
                              ImGui::PopID();
                          } });
    }
    if (game.presets.empty())
        fields.push_back(Note(edit ? "No presets yet." : "No presets yet. Make one with New preset in the menu while the game runs.", kMist, width));
    if (state->naming == Naming::New)
        fields.push_back(Note("Starts with every effect off.", kMist, width));
    if (state->nameFailed && !f.model.presetProblem.empty())
        fields.push_back(Note(f.model.presetProblem, kDanger, width));
    if (!game.missing.empty())
    {
        std::string names;
        for (const std::string& effect : game.missing)
            names += (names.empty() ? "" : ", ") + effect;
        fields.push_back(Note("Uses effects that are not installed: " + names + ".", kCaution, width));
    }
    y += Fields("##presets", fields, grid, min + ImVec2(0, y), width, MaxHeight(f, 0.36f, space, y));
    return y;
}

// A question that waits for its answer in the sheet, such as whether to save the changes before switching presets.
float AskSheet(Frame& f, ImVec2 min, float width)
{
    float height = 0;
    const auto presetName = [&](const std::string& id) {
        const Preset* preset = f.game ? FindPreset(*f.game, id) : nullptr;
        return preset ? preset->name : std::string();
    };
    switch (state->question)
    {
    case Question::Unsaved:
    {
        const int clicked = Header(f, min, width, "Save your changes to " + presetName(f.game ? f.game->preset : std::string()) + "?",
                                   "Discarding goes back to how the preset was last saved.",
                                   { { "Discard", PillStyle::Text }, { "Cancel", PillStyle::Text }, { "Save", PillStyle::Black } }, height, true);
        if (clicked == 0)
            f.host.DiscardChanges();
        else if (clicked == 2)
            f.host.SaveChanges();
        if (clicked == 0 || clicked == 2)
        {
            state->swapDirection = state->pendingDirection;
            f.host.UsePreset(f.gameIndex, state->pendingPreset);
        }
        if (clicked >= 0)
            Back();
        break;
    }
    case Question::Discard:
    {
        const int clicked = Header(f, min, width, "Discard your changes to " + presetName(f.game ? f.game->preset : std::string()) + "?",
                                   "The preset goes back to how it was last saved.", { { "Cancel", PillStyle::Text }, { "Discard", PillStyle::Black } },
                                   height, true);
        if (clicked == 1)
            f.host.DiscardChanges();
        if (clicked >= 0)
            Back();
        break;
    }
    case Question::Delete:
    {
#ifdef _WIN32
        constexpr char kTrash[] = "It moves to the Recycle Bin.";
#else
        constexpr char kTrash[] = "It moves to the Trash.";
#endif
        const int clicked = Header(f, min, width, "Delete " + presetName(state->deleting) + "?", kTrash,
                                   { { "Cancel", PillStyle::Text }, { "Delete", PillStyle::Black } }, height, true);
        if (clicked == 1)
            f.host.DeletePreset(state->deleting);
        if (clicked >= 0)
            Back();
        break;
    }
    case Question::ResetEffect:
    {
        const std::optional<size_t> effect = f.effects ? IndexOf(*f.effects, state->resetting) : std::nullopt;
        const int clicked = Header(f, min, width, "Reset every setting of " + (effect ? f.effects->list[*effect].name : std::string()) + "?",
                                   "They go back to their defaults.", { { "Cancel", PillStyle::Text }, { "Reset", PillStyle::Black } }, height, true);
        if (clicked == 1 && effect)
            f.host.ResetSettings(*effect, std::nullopt);
        if (clicked >= 0)
            Back();
        break;
    }
    case Question::ResetShortcuts:
    {
        const int clicked = Header(f, min, width, "Reset every shortcut?", "They go back to the keys Unishade starts with.",
                                   { { "Cancel", PillStyle::Text }, { "Reset", PillStyle::Black } }, height, true);
        if (clicked == 1)
            f.host.ResetShortcuts();
        if (clicked >= 0)
            Back();
        break;
    }
    }
    return height;
}

// The ring's colors across a rounded box, for a shortcut waiting for its keys.
void RainbowBox(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding)
{
    constexpr int kSlices = 24;
    constexpr int kColors = static_cast<int>(std::size(theme::kRainbow));
    const float width = (max.x - min.x) / kSlices;
    for (int i = 0; i < kSlices; ++i)
    {
        const float along = (static_cast<float>(i) + 0.5f) / kSlices * (kColors - 1);
        const int stop = std::min(static_cast<int>(along), kColors - 2);
        const ImU32 color = Mix(Color(theme::kRainbow[stop]), Color(theme::kRainbow[stop + 1]), along - static_cast<float>(stop));
        const float left = min.x + width * static_cast<float>(i);
        const ImVec2 slice[] = { ImVec2(left, min.y), ImVec2(left + width + 0.5f, min.y), ImVec2(left + width + 0.5f, max.y), ImVec2(left, max.y) };
        FillInside(draw, min, max, rounding, slice, color);
    }
}

// The game's own settings, the menu, shortcuts, and the settings of the add-ons that are on.
float MoreSheet(Frame& f, ImVec2 min, float width, float space)
{
    const Settings& settings = f.model.settings;
    const auto addonShown = [](const Addon& addon) { return addon.state == AddonState::On && (!addon.settings.empty() || addon.controls); };
    std::vector<Tab> tabs;
    std::vector<const char*> labels;
    if (f.game && f.game->performance)
    {
        tabs.push_back(Tab::Game);
        labels.push_back("Game");
    }
    tabs.push_back(Tab::Menu);
    labels.push_back("Menu");
    if (!settings.shortcuts.empty())
    {
        tabs.push_back(Tab::Shortcuts);
        labels.push_back("Shortcuts");
    }
    if (f.model.addons && std::any_of(f.model.addons->begin(), f.model.addons->end(), addonShown))
    {
        tabs.push_back(Tab::Addons);
        labels.push_back("Add-ons");
    }
    if (std::find(tabs.begin(), tabs.end(), state->tab) == tabs.end())
        state->tab = tabs.front();
    const size_t selected = static_cast<size_t>(std::find(tabs.begin(), tabs.end(), state->tab) - tabs.begin());

    // The tabs, in as many rows as they need beside Done.
    const float done = PillWidth("Done", PillStyle::Black);
    const float room = std::max(width - done - S(12.8f), S(120));
    std::optional<size_t> clicked;
    float rowsHeight = 0;
    for (size_t first = 0; first < labels.size();)
    {
        size_t last = first + 1;
        while (last < labels.size() && SegmentsWidth(std::span<const char* const>(labels.data() + first, last + 1 - first), true) <= room)
            ++last;
        ImGui::PushID(static_cast<int>(first));
        const int picked = Segments("tabs", min + ImVec2(0, rowsHeight), std::span<const char* const>(labels.data() + first, last - first),
                                    selected >= first && selected < last ? static_cast<int>(selected - first) : -1, true);
        ImGui::PopID();
        if (picked >= 0)
            clicked = first + static_cast<size_t>(picked);
        rowsHeight += S(kSegmentHeight) + S(6.4f) + S(6.4f);
        first = last;
    }
    rowsHeight -= S(6.4f);
    const float height = std::max(rowsHeight, S(kButton));
    if (PillButton("Done", ImVec2(min.x + width - done, min.y + height - S(kButton)), S(kButton), PillStyle::Black))
        CloseSheet();
    if (clicked && tabs[*clicked] != state->tab)
    {
        const float slide = *clicked > selected ? 1.0f : -1.0f;
        state->tab = tabs[*clicked];
        OpenSheet(Sheet::More, slide);
    }
    float y = height + S(14.4f);

    const Grid grid = GridOf(width, f.side ? width : S(kFieldWidth));
    std::vector<Field> fields;
    const auto debugInfo = [&] {
        return Switched("debug", "Show debug info", *settings.debugInfo, [&f](bool on) { f.host.SetSwitch(Switch::DebugInfo, on); },
                        "Frame rates and frame times, at the top of the game.", grid);
    };
    switch (state->tab)
    {
    case Tab::Game:
    {
        const Performance performance = *f.game->performance;
        const int rate = ChoiceOf(kFrameRates, performance.frameRate);
        const bool custom = state->customRate || rate >= kCustom;
        fields.push_back(Choices("rate", "Frame rate", { "Follow game", "120 FPS", "60 FPS", "Custom" }, custom ? kCustom : rate,
                                 [&f](int choice) {
                                     if (choice < kCustom)
                                     {
                                         state->customRate = false;
                                         f.host.SetPerformance(f.gameIndex, PerformanceSetting::FrameRate, kFrameRates[choice]);
                                     }
                                     else
                                     {
                                         state->customRate = true;
                                         state->focusRate = true;
                                     }
                                 },
                                 {}, grid));
        if (custom)
            fields.push_back({ S(kField), 0, [&f, performance](ImVec2 at, ImVec2 size) {
                                  ImGui::PushID("custom rate");
                                  RowBack(at, size);
                                  const float box = S(84);
                                  // The box shows the limit unless it is being typed in.
                                  if (!state->rateActive && !state->focusRate)
                                      std::snprintf(state->rate, sizeof(state->rate), "%d", performance.frameRate ? performance.frameRate : 60);
                                  PushSize(kTextBody, true);
                                  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), std::max((S(kButton) - ImGui::GetFontSize()) / 2, 0.0f)));
                                  ImGui::SetCursorScreenPos(ImVec2(at.x + S(5.6f), at.y + (size.y - ImGui::GetFrameHeight()) / 2));
                                  ImGui::SetNextItemWidth(box);
                                  if (std::exchange(state->focusRate, false))
                                      ImGui::SetKeyboardFocusHere();
                                  ImGui::InputText("##rate", state->rate, sizeof(state->rate), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
                                  const bool left = ImGui::IsItemDeactivated();
                                  state->rateActive = ImGui::IsItemActive();
                                  ImGui::PopStyleVar();
                                  ImGui::PopFont();
                                  const std::string range = "frames a second, from " + std::to_string(kSlowestFrameRate) + " to " + std::to_string(kFastestFrameRate);
                                  Line(ImGui::GetWindowDrawList(), at.x + S(5.6f) + box + S(12), at.x + size.x - S(12), at.y + size.y / 2, range, kMist, kTextBody);
                                  // Leaving the box uses the number in it, and Escape leaves it as it was.
                                  if (left && !ImGui::IsKeyPressed(ImGuiKey_Escape))
                                      if (const int value = std::atoi(state->rate); value > 0)
                                      {
                                          const int limit = std::clamp(value, kSlowestFrameRate, kFastestFrameRate);
                                          f.host.SetPerformance(f.gameIndex, PerformanceSetting::FrameRate, limit);
                                          if (ChoiceOf(kFrameRates, limit) < kCustom)
                                              state->customRate = false;
                                      }
                                  ImGui::PopID();
                              } });
        fields.push_back(Note("The most frames a second Unishade shows. A lower limit leaves more of the GPU to the game.", kMist, width));
        if (performance.effectResolution)
        {
            fields.push_back(Choices("resolution", "Effect resolution", { "100%", "75%", "50%" }, ChoiceOf(kEffectResolutions, *performance.effectResolution),
                                     [&f](int choice) { f.host.SetPerformance(f.gameIndex, PerformanceSetting::EffectResolution, kEffectResolutions[choice]); },
                                     {}, grid));
            fields.push_back(Note("Effects run on a smaller picture that FSR upscales to the game's size. Lower is faster and softer, and screenshots get smaller.",
                                  kMist, width));
        }
        if (performance.hdrOutput)
        {
            fields.push_back(Switched("hdr", "HDR output", *performance.hdrOutput,
                                      [&f](bool on) { f.host.SetPerformance(f.gameIndex, PerformanceSetting::HdrOutput, on ? 1 : 0); }, {}, grid));
            fields.push_back(Note("Keeps an HDR game in HDR. Most effects are made for SDR and can look wrong in HDR, and add-ons that work on the picture pause.",
                                  kMist, width));
        }
        // Marked over the game, so only there.
        if (!page)
        {
            const size_t count = f.game->uiMask.size();
            const std::string label = count ? "Game interface: " + std::to_string(count) + (count == 1 ? " area" : " areas") : "Game interface";
            fields.push_back(Actions("interface", label, { { "Mark", PillStyle::Text } }, [](int) { state->masking = true; }, width));
            fields.push_back(Note("Mark the game's own interface, such as a health bar or the chat, and effects leave it as the game drew it.", kMist, width));
            fields.push_back(Switched("detect", "Detect the game's interface", f.game->detectUi,
                                      [&f](bool on) { f.host.SetPerformance(f.gameIndex, PerformanceSetting::DetectUi, on ? 1 : 0); }, {}, grid));
        }
        break;
    }
    case Tab::Menu:
    {
        // The rest of the launcher, which opens over the game. The window has its own buttons for these.
        if (!page)
        {
            std::vector<Action> places = { { "Games", PillStyle::Text }, { "Settings", PillStyle::Text } };
            std::vector<Open> opens = { Open::Games, Open::Settings };
            if (f.model.addons)
            {
                places.push_back({ "Add-ons", PillStyle::Text });
                opens.push_back(Open::Addons);
            }
            places.push_back({ "Messages", PillStyle::Text });
            opens.push_back(Open::Messages);
            fields.push_back(Actions("launcher", "Unishade", places, [opens](int pressed) { requested = opens[static_cast<size_t>(pressed)]; }, width));
        }
        fields.push_back(Heading("Menu"));
        if (settings.menuSize && !page)
            fields.push_back({ S(kField), 1, [&f](ImVec2 at, ImVec2 size) {
                                  // The tray takes the new size when the bar is let go, so it does not grow under the mouse.
                                  Bar bar;
                                  bar.label = "Size";
                                  bar.min = static_cast<float>(kSmallestSize);
                                  bar.max = static_cast<float>(kLargestSize);
                                  bar.integer = true;
                                  bar.format = "%d%%";
                                  float value = state->sizeBarHeld ? state->sizeBarValue : static_cast<float>(state->size);
                                  bool held = false;
                                  const bool changed = FillBar("size", at, size, bar, value, nullptr, &held);
                                  if (held)
                                  {
                                      state->sizeBarHeld = true;
                                      state->sizeBarValue = value;
                                  }
                                  else if (state->sizeBarHeld || changed)
                                  {
                                      state->sizeBarHeld = false;
                                      state->size = static_cast<int>(std::lround(value));
                                      f.host.SetMenuSize(state->size);
                                  }
                              } });
        fields.push_back({ S(kField), 1, [&f](ImVec2 at, ImVec2 size) {
                              Bar bar;
                              bar.label = "Opacity";
                              bar.min = 20;
                              bar.max = 100;
                              bar.integer = true;
                              bar.format = "%d%%";
                              float value = state->opacity * 100;
                              bool held = false;
                              const bool changed = FillBar("opacity", at, size, bar, value, nullptr, &held);
                              if (changed)
                                  state->opacity = value / 100;
                              if (held)
                                  state->opacityHeld = true;
                              else if (state->opacityHeld || changed)
                              {
                                  state->opacityHeld = false;
                                  f.host.SetMenuOpacity(state->opacity);
                              }
                          } });
        fields.push_back(Choices("theme", "Theme", { "Light", "Dark" }, settings.darkMenu ? 1 : 0,
                                 [&f](int choice) { f.host.SetSwitch(Switch::DarkMenu, choice == 1); }, {}, grid));
        fields.push_back(Choices("tiles", "Tiles", { "Regular", "Small" }, state->small ? 1 : 0,
                                 [&f](int choice) {
                                     state->small = choice == 1;
                                     f.host.SetSwitch(Switch::SmallTiles, choice == 1);
                                 },
                                 {}, grid));
        if (settings.debugInfo)
            fields.push_back(debugInfo());
        if (settings.uiMaskShown)
            fields.push_back(Switched("uimask", "Show the interface mask", *settings.uiMaskShown,
                                      [&f](bool on) { f.host.SetSwitch(Switch::UiMaskShown, on); }, "Tints what effects leave as the game drew it.", grid));
        fields.push_back(Heading("Presets"));
        fields.push_back(Switched("autosave", "Save changes automatically", settings.autoSavePresets,
                                  [&f](bool on) { f.host.SetSwitch(Switch::AutoSavePresets, on); },
                                  "Turn off to try changes first, then save or discard them.", grid));
        if (settings.reloadEffects)
            fields.push_back(Actions("reload", "Reload effects", { { "Reload", PillStyle::Text } }, [&f](int) { f.host.ReloadEffects(); }, width));
        break;
    }
    case Tab::Shortcuts:
    {
        fields.push_back(Note("Click a shortcut, then press the keys you want. They work right away.", kMist, width));
        auto recordHovered = std::make_shared<bool>(false);
        for (size_t i = 0; i < settings.shortcuts.size(); ++i)
        {
            const Shortcut* shortcut = &settings.shortcuts[i];
            fields.push_back({ S(kField), 0, [&f, &settings, shortcut, i, recordHovered](ImVec2 at, ImVec2 size) {
                                  ImGui::PushID(static_cast<int>(i));
                                  RowBack(at, size);
                                  const bool recording = settings.recording == static_cast<int>(i);
                                  const bool clearable = !shortcut->required && !shortcut->keys.empty();
                                  const float clear = clearable ? S(32) + S(6.4f) : 0;
                                  const float keyWidth = S(170);
                                  const ImVec2 keyMin(at.x + size.x - S(5.6f) - clear - keyWidth, at.y + (size.y - S(40)) / 2);
                                  RowLabel(at, size, shortcut->title, keyMin.x - S(9.6f), shortcut->description);
                                  ImGui::SetCursorScreenPos(keyMin);
                                  if (ImGui::InvisibleButton("keys", ImVec2(keyWidth, S(40)), ImGuiButtonFlags_EnableNav))
                                  {
                                      f.host.RecordShortcut(recording ? -1 : static_cast<int>(i));
                                      state->recordingHere = !recording;
                                  }
                                  const bool hovered = ImGui::IsItemHovered();
                                  *recordHovered |= hovered;
                                  HandOnHover();
                                  ImDrawList* draw = ImGui::GetWindowDrawList();
                                  const int start = draw->VtxBuffer.Size;
                                  if (recording)
                                      RainbowBox(draw, keyMin, keyMin + ImVec2(keyWidth, S(40)), S(11.2f));
                                  else
                                      draw->AddRectFilled(keyMin, keyMin + ImVec2(keyWidth, S(40)), hovered ? kInkHover : kInk, S(11.2f));
                                  const std::string label = recording ? "Press keys..." : shortcut->keys.empty() ? "Not set" : shortcut->keys;
                                  Line(draw, keyMin.x + S(8), keyMin.x + keyWidth - S(8), keyMin.y + S(20), label,
                                       recording ? kOnColor : shortcut->keys.empty() ? Alpha(kOnInk, 0.6f) : kOnInk, kTextBody, true, true);
                                  if (recording)
                                  {
                                      // Breathes while it waits, as the design's does.
                                      const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 1.8) / 1.8);
                                      Transform(draw, start, keyMin + ImVec2(keyWidth, S(40)) * 0.5f, 1 + 0.03f * (1 - std::cos(2 * IM_PI * t)));
                                  }
                                  if (clearable && RoundButton("clear", ImVec2(at.x + size.x - S(5.6f) - S(32), at.y + (size.y - S(32)) / 2), Icon::Close,
                                                               "Leave unassigned", false, 32))
                                      f.host.ClearShortcut(i);
                                  ImGui::PopID();
                              } });
        }
        if (!settings.shortcutError.empty())
            fields.push_back(Note(settings.shortcutError, kDanger, width));
        for (const std::string& warning : settings.shortcutWarnings)
            fields.push_back(Note(warning, kCaution, width));
        fields.push_back(Actions("reset shortcuts", "", { { "Reset to defaults", PillStyle::Text } }, [](int) { Ask(Question::ResetShortcuts, Sheet::More); },
                                 width));
        y += Fields("##shortcuts", fields, grid, min + ImVec2(0, y), width, MaxHeight(f, 0.38f, space, y));
        // Clicking anything but the shortcut that waits for keys stops the wait.
        if (settings.recording >= 0 && state->recordingHere && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !*recordHovered)
        {
            f.host.RecordShortcut(-1);
            state->recordingHere = false;
        }
        return y;
    }
    case Tab::Addons:
    {
        for (size_t i = 0; i < f.model.addons->size(); ++i)
        {
            const Addon& addon = (*f.model.addons)[i];
            if (!addonShown(addon))
                continue;
            fields.push_back(Heading(addon.name));
            const std::string* group = nullptr;
            for (size_t setting = 0; setting < addon.settings.size(); ++setting)
            {
                // A heading over the settings that name the same group, where the group changes.
                const std::string& named = addon.settings[setting].category;
                if (!named.empty() && (!group || *group != named))
                    fields.push_back(Heading(named));
                group = &named;
                fields.push_back(SettingField(
                    addon.id, addon.settings[setting], setting, kQuiet, grid,
                    [&f, i, setting](const EffectSetting& value) { f.host.SetAddonSetting(i, setting, value); }, {}, nullptr, nullptr));
            }
            // Its own controls, which it draws again for every frame they show.
            const std::vector<EffectSetting>* controls = addon.controls ? f.host.DescribeAddonControls(i) : nullptr;
            for (size_t control = 0; controls && control < controls->size(); ++control)
                fields.push_back(SettingField(
                    addon.id + "/controls", (*controls)[control], control, kQuiet, grid,
                    [&f, i, control](const EffectSetting& value) { f.host.UseAddonControl(i, control, value); }, {}, nullptr, nullptr));
        }
        break;
    }
    }
    ImGui::PushID(static_cast<int>(state->tab));
    y += Fields("##more", fields, grid, min + ImVec2(0, y), width, MaxHeight(f, 0.38f, space, y));
    ImGui::PopID();
    return y;
}

// The open sheet, growing out of the tray. Its contents keep their place while it grows, so it reveals them.
void DrawSheet(Frame& f, ImVec2 min, float height, float room)
{
    const float s = kit::scale;
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild("##sheet", ImVec2(f.width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    // New contents fade in, from the side they come from or from a little below.
    const float enter = Animate(Key("sheet enter"), 1, 0.38f, Curve::Out);
    const ImVec2 offset(state->slide * S(32) * (1 - enter), state->slide == 0 && state->fromClosed ? S(16) * (1 - enter) : 0);
    const float top = f.down ? S(19.2f) : S(4.8f), bottom = f.down ? S(4.8f) : S(19.2f);
    const ImVec2 inner = min + ImVec2(S(4.8f), top) + offset;
    const float width = f.width - S(9.6f);
    const float space = room - top - bottom;
    float used = 0;
    ImGui::PushID(static_cast<int>(state->drawn));
    switch (state->drawn)
    {
    case Sheet::Tune: used = TuneSheet(f, inner, width, space); break;
    case Sheet::Add: used = AddSheet(f, inner, width, space); break;
    case Sheet::Presets: used = PresetsSheet(f, inner, width, space); break;
    case Sheet::More: used = MoreSheet(f, inner, width, space); break;
    case Sheet::Shot: used = ShotSheet(f, inner, width, space); break;
    case Sheet::Ask: used = AskSheet(f, inner, width); break;
    case Sheet::None: break;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
    FadeWindow(window, std::clamp(enter, 0.0f, 1.0f));
    state->measured = (used + top + bottom) / s;
    state->measuredKind = state->drawn;
}

// Tiles

// Moving the tray by its grab handle or the space between its buttons, over the whole game. Let go near a side, it
// docks there; in the top part of the game, it opens downward.
void MoveWith(Frame& f)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated())
    {
        state->moving = true;
        state->moveOffset = io.MousePos - f.trayMin;
    }
    if (state->moving && ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (!state->moving || !ImGui::IsItemDeactivated())
        return;
    state->moving = false;
    const ImVec2 display = io.DisplaySize;
    const bool left = io.MousePos.x < S(kDock), right = io.MousePos.x > display.x - S(kDock);
    const bool down = io.MousePos.y < display.y * 0.42f;
    const float width = left || right ? S(kSideWidth) : std::min(display.x * 0.94f, S(kWidth));
    const float x = left ? 0.0f : right ? 1.0f : std::clamp((f.trayMin.x + f.trayWidth / 2) / display.x, 0.01f, 0.99f);
    // The tray stays on the game, and the middle of its buttons is what is kept.
    const float top = std::clamp(f.trayMin.y, S(kMargin), std::max(S(kMargin), display.y - f.trayHeight - S(kMargin)));
    const float middle = down ? top + S(kPad) + f.rows / 2 : top + f.trayHeight - S(kPad) - f.rows / 2;
    float y = middle / display.y;
    y = down ? std::min(y, 0.49f) : std::clamp(y, 0.5f, 1.0f);
    state->x = state->seenX = x;
    state->y = state->seenY = y;
    f.host.PlaceMenu(x, y);
    // From where it was let go to its place.
    const float cx = x == 0 ? S(kMargin) + width / 2 : x == 1 ? display.x - S(kMargin) - width / 2 : x * display.x;
    const float cy = y * display.y;
    const ImVec2 placed(std::clamp(cx - width / 2, 0.0f, std::max(0.0f, display.x - width)),
                        down ? cy - S(kPad) - f.rows / 2 : cy + f.rows / 2 + S(kPad) - f.trayHeight);
    Snap(Key("drop"), (f.trayMin - placed) / kit::scale);
}

// The corner that makes the whole menu bigger or smaller, staying under the mouse. Docked at the right side, the
// corner is on the left.
void SizeCorner(Frame& f, ImVec2 min, ImVec2 max)
{
    if (page || !f.model.settings.menuSize)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    const bool mirrored = state->x == 1;
    const float box = S(22.4f), inset = S(6);
    const ImVec2 at = mirrored ? ImVec2(min.x + inset, max.y - inset - box) : ImVec2(max.x - inset - box, max.y - inset - box);
    ImGui::SetCursorScreenPos(at);
    // The buttons along the bottom win where the corner reaches under them, so a grab that misses it does not turn
    // every effect off. Opening downward, only the grab handle is there, which the corner wins over.
    if (!f.down)
        ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##size", ImVec2(box, box));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    if (hovered || held)
        ImGui::SetMouseCursor(mirrored ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE);
    // What stays put while the size changes: the middle, or the side the tray is docked at.
    if (ImGui::IsItemActivated())
    {
        state->sizing = true;
        state->sizeFrom = state->size;
        state->sizeAt = io.MousePos.x;
        state->sizeAnchor = state->x == 0 ? min.x : state->x == 1 ? max.x : (min.x + max.x) / 2;
    }
    if (state->sizing && held)
    {
        const float from = state->sizeAt - state->sizeAnchor;
        if (std::abs(from) > 1)
            state->size = std::clamp(static_cast<int>(std::lround(static_cast<float>(state->sizeFrom) * (io.MousePos.x - state->sizeAnchor) / from)),
                                     kSmallestSize, kLargestSize);
    }
    if (state->sizing && ImGui::IsItemDeactivated())
    {
        state->sizing = false;
        f.host.SetMenuSize(state->size);
    }
    // An L that rounds with the tray's corner.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float radius = S(22);
    const ImU32 color = hovered || held ? kInk : kFaint;
    if (mirrored)
    {
        draw->PathLineTo(at);
        draw->PathArcTo(ImVec2(at.x + radius, at.y + box - radius), radius, IM_PI, IM_PI / 2, 12);
        draw->PathLineTo(at + ImVec2(box, box));
    }
    else
    {
        draw->PathLineTo(ImVec2(at.x + box, at.y));
        draw->PathArcTo(ImVec2(at.x + box - radius, at.y + box - radius), radius, 0, IM_PI / 2, 12);
        draw->PathLineTo(ImVec2(at.x, at.y + box));
    }
    draw->PathStroke(color, S(3.2f));
    Tip("Drag to make everything bigger or smaller");
}

// Tapping a tile opens its settings, or closes them when they show; the sheet slides toward the tile's side.
void Select(const std::vector<TileItem>& tiles, const std::string& key)
{
    if (state->sheet == Sheet::Tune && state->tuned == key)
    {
        CloseSheet();
        return;
    }
    float slide = 0;
    if (state->sheet == Sheet::Tune)
    {
        const auto place = [&](const std::string& k) {
            return std::find_if(tiles.begin(), tiles.end(), [&](const TileItem& tile) { return tile.key == k; }) - tiles.begin();
        };
        slide = place(key) > place(state->tuned) ? 1.0f : -1.0f;
    }
    state->tuned = key;
    OpenSheet(Sheet::Tune, slide);
}

// A dragged tile runs where it was let go: the effect moves to the place of the effect whose tile was there.
void DropTile(Frame& f, const std::vector<TileItem>& modelTiles, const std::vector<TileItem>& tiles, const std::string& key)
{
    const auto find = [&](const std::vector<TileItem>& list) {
        return static_cast<size_t>(std::find_if(list.begin(), list.end(), [&](const TileItem& tile) { return tile.key == key; }) - list.begin());
    };
    const size_t from = find(modelTiles), to = find(tiles);
    if (from != to && from < modelTiles.size() && to < modelTiles.size())
        f.host.MoveEffect(modelTiles[from].index, modelTiles[to].index);
}

// The menu a right click on an effect's tile opens.
void EffectMenu(Frame& f, const std::vector<TileItem>& modelTiles, const TileItem& tile)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6.4f), S(6.4f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(1.6f)));
    if (ImGui::BeginPopup("actions"))
    {
        const Effect& effect = f.effects->list[tile.index];
        const auto addonOn = [](const Addon& addon) { return addon.changesPicture && !addon.bypassed; };
        const bool othersOn = std::any_of(modelTiles.begin(), modelTiles.end(), [&](const TileItem& other) {
                                  return other.key != tile.key && f.effects->list[other.index].on;
                              }) || (f.model.addons && std::any_of(f.model.addons->begin(), f.model.addons->end(), addonOn));
        bool chosen = false;
        if (MenuRow(effect.on ? "Turn off" : "Turn on"))
        {
            f.host.SetEffect(tile.index, !effect.on);
            chosen = true;
        }
        // Only this one stays on: every other effect and add-on that changes the picture is turned off.
        if (MenuRow("Solo", othersOn || !effect.on))
        {
            std::vector<std::string> effectsOff, addonsOff;
            for (const TileItem& other : modelTiles)
                if (other.key != tile.key && f.effects->list[other.index].on)
                {
                    f.host.SetEffect(other.index, false);
                    effectsOff.push_back(other.key);
                }
            for (size_t i = 0; f.model.addons && i < f.model.addons->size(); ++i)
                if (addonOn((*f.model.addons)[i]))
                {
                    f.host.BypassAddon(i, true);
                    addonsOff.push_back((*f.model.addons)[i].id);
                }
            const bool turnedOn = !effect.on;
            if (turnedOn)
                f.host.SetEffect(tile.index, true);
            Offer("Only " + effect.name + " is on", [effectsOff, addonsOff, turnedOn, key = tile.key](Frame& frame) {
                if (!frame.effects)
                    return;
                for (const std::string& other : effectsOff)
                    if (const std::optional<size_t> at = IndexOf(*frame.effects, other))
                        frame.host.SetEffect(*at, true);
                for (size_t i = 0; frame.model.addons && i < frame.model.addons->size(); ++i)
                    if (std::find(addonsOff.begin(), addonsOff.end(), (*frame.model.addons)[i].id) != addonsOff.end())
                        frame.host.BypassAddon(i, false);
                if (const std::optional<size_t> at = IndexOf(*frame.effects, key); at && turnedOn)
                    frame.host.SetEffect(*at, false);
            });
            chosen = true;
        }
        if (MenuRow("Move first", tile.key != modelTiles.front().key))
        {
            f.host.MoveEffect(tile.index, modelTiles.front().index);
            chosen = true;
        }
        if (MenuRow("Move last", tile.key != modelTiles.back().key))
        {
            f.host.MoveEffect(tile.index, modelTiles.back().index);
            chosen = true;
        }
        if (MenuRow("Reset settings"))
        {
            state->resetting = tile.key;
            Ask(Question::ResetEffect, state->sheet);
            chosen = true;
        }
        if (MenuRow("Remove", true, kDanger))
        {
            f.host.RemoveEffect(tile.index);
            Offer("Removed " + effect.name, [key = tile.key](Frame& frame) {
                if (const std::optional<size_t> at = frame.effects ? IndexOf(*frame.effects, key) : std::nullopt)
                    frame.host.SetEffect(*at, true);
            });
            if (state->sheet == Sheet::Tune && state->tuned == tile.key)
                CloseSheet();
            chosen = true;
        }
        if (chosen)
            ImGui::CloseCurrentPopup();
        MenuAppear();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// The tiles, and the one that adds an effect. Each glides to its place with the spring when the order or the size
// changes, grows in when it is new and shrinks away when it goes.
void DrawTiles(Frame& f, ImVec2 origin, const std::vector<TileItem>& modelTiles, const std::vector<TileItem>& tiles, const std::vector<size_t>& addonTiles,
               int columns, float cell, float height)
{
    // Add-ons work on the picture before effects, so their tiles come first.
    const size_t lead = addonTiles.size();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float s = kit::scale;
    const double now = ImGui::GetTime();
    const bool cooking = f.effects->loading;
    const bool opening = now - state->opened < 1.2;
    const bool small = state->small;
    const auto slot = [&](size_t i) {
        return ImVec2(static_cast<float>(i % static_cast<size_t>(columns)) * (cell + kTileGap), static_cast<float>(i / static_cast<size_t>(columns)) * (height + kTileGap));
    };

    // Tiles that went shrink away where they were.
    std::set<std::string> keys;
    for (const TileItem& tile : tiles)
        keys.insert(tile.key);
    for (const size_t index : addonTiles)
        keys.insert("addon:" + (*f.model.addons)[index].id);
    for (auto memo = state->memos.begin(); memo != state->memos.end();)
        if (!keys.count(memo->first))
        {
            state->ghosts.push_back({ memo->second, now });
            memo = state->memos.erase(memo);
        }
        else
            ++memo;
    std::erase_if(state->ghosts, [&](const Ghost& ghost) { return now - ghost.since > 0.26; });
    for (const Ghost& ghost : state->ghosts)
    {
        const float t = Ease(Curve::EaseIn, static_cast<float>((now - ghost.since) / 0.26));
        const ImVec2 min = origin + ghost.memo.at * s, max = min + ghost.memo.size * s;
        TileLook look;
        look.name = ghost.memo.name;
        look.color = ghost.memo.color;
        look.reveal = look.ink = look.dot = look.spot = ghost.memo.on ? 1.0f : 0.0f;
        look.small = small;
        const int start = draw->VtxBuffer.Size;
        DrawTile(draw, min, max, look);
        Transform(draw, start, (min + max) * 0.5f, 1 - 0.8f * t, 0, ImVec2(0, 0), 1 - t);
    }

    // An add-on that changes the picture has a tile as an effect has, so what the game looks like is all in one place.
    // It opens the add-on's settings, and its dot only has it leave the picture alone, so it is back at once. It stays
    // where it is.
    for (size_t i = 0; i < addonTiles.size(); ++i)
    {
        const Addon& addon = (*f.model.addons)[addonTiles[i]];
        const std::string key = "addon:" + addon.id;
        const bool on = !addon.bypassed;
        ImGui::PushID(key.c_str());
        const ImVec2 slotAt = slot(i);
        const ImVec2 slotMin = origin + slotAt * s, slotSize = ImVec2(cell, height) * s;
        ImGui::SetCursorScreenPos(slotMin);
        ImGui::SetNextItemAllowOverlap();
        const bool clicked = ImGui::InvisibleButton("addon", slotSize, ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 dot = TileDot(slotMin, slotMin + slotSize, small);
        // The dot takes clicks in the whole corner around it, which is easier to hit than the dot.
        const float reach = (slotMin.x + slotSize.x - dot.x) * 2;
        ImGui::SetCursorScreenPos(ImVec2(slotMin.x + slotSize.x - reach, slotMin.y));
        const bool dotClicked = ImGui::InvisibleButton("on", ImVec2(reach, reach), ImGuiButtonFlags_EnableNav);
        const bool dotHovered = ImGui::IsItemHovered();
        if (dotHovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        Tip(on ? "Turn off" : "Turn on");
        const bool shown = state->sheet == Sheet::More && state->tab == Tab::Addons;
        if (dotClicked)
            f.host.BypassAddon(addonTiles[i], on);
        else if (clicked && shown)
            CloseSheet();
        else if (clicked)
        {
            state->tab = Tab::Addons;
            OpenSheet(Sheet::More, 0);
        }

        const ImGuiID appearKey = ImGui::GetID("appear");
        const bool isNew = !state->memos.count(key);
        if (isNew)
            Snap(appearKey, 0.0f);
        const float appear = Animate(appearKey, 1, opening ? 0.6f : 0.65f, Curve::Spring, isNew && opening ? 0.12f + 0.028f * static_cast<float>(i) : 0);
        const ImVec2 at = Animate(ImGui::GetID("at"), slotAt, 0.65f, Curve::Spring);
        const ImVec2 size = Animate(ImGui::GetID("size"), ImVec2(cell, height), 0.65f, Curve::Spring);
        const float lift = Animate(ImGui::GetID("lift"), shown ? (f.down ? 5.6f : -5.6f) : hovered && !held ? -3.2f : 0.0f, 0.4f, Curve::Spring);
        const float press = Animate(ImGui::GetID("press"), held ? 0.95f : 1, 0.3f, Curve::Spring);

        TileLook look;
        look.name = addon.name;
        look.detail = "Add-on";
        look.detailColor = on && f.effects->on ? Alpha(kOnColor, 0.55f) : kMist;
        look.color = Ring(std::hash<std::string>{}(addon.id));
        look.reveal = Animate(ImGui::GetID("reveal"), on ? 1.0f : 0.0f, 0.55f, Curve::Out);
        look.ink = Animate(ImGui::GetID("ink"), on ? 1.0f : 0.0f, 0.35f, Curve::Out);
        look.gray = Animate(ImGui::GetID("gray"), f.effects->on ? 0.0f : 1.0f, 0.4f, Curve::Out, 0.03f * static_cast<float>(i));
        look.dot = Animate(ImGui::GetID("dot"), on ? 1.0f : 0.0f, 0.3f, Curve::Out);
        look.spot = std::max(0.0f, Animate(ImGui::GetID("spot"), on ? 1.0f : 0.0f, 0.4f, Curve::Spring));
        look.dotScale = Animate(ImGui::GetID("dot scale"), dotHovered ? 1.15f : 1, 0.25f, Curve::Spring);
        look.small = small;
        ImVec2 min = origin + (at + ImVec2(0, lift)) * s;
        if (opening)
            min.y += (1 - appear) * S(25.6f);
        const ImVec2 max = min + size * s;
        state->memos[key] = { at + ImVec2(0, lift), size, look.color, on, addon.name };
        const int start = draw->VtxBuffer.Size;
        DrawTile(draw, min, max, look);
        Transform(draw, start, (min + max) * 0.5f, press * (opening ? 0.7f + 0.3f * appear : 0.2f + 0.8f * appear), 0, ImVec2(0, 0),
                  std::clamp(appear, 0.0f, 1.0f));
        ImGui::PopID();
    }

    std::function<void()> lifted; // the dragged tile, drawn over the others
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const TileItem& tile = tiles[i];
        const Effect& effect = f.effects->list[tile.index];
        ImGui::PushID(tile.key.c_str());
        const ImVec2 slotAt = slot(lead + i);
        const ImVec2 slotMin = origin + slotAt * s, slotSize = ImVec2(cell, height) * s;

        // Pressing opens its settings, the dot turns it on and off, and moving it far enough drags it to another place.
        ImGui::SetCursorScreenPos(slotMin);
        ImGui::SetNextItemAllowOverlap();
        const bool clicked = ImGui::InvisibleButton("effect", slotSize, ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered() && !cooking, held = ImGui::IsItemActive();
        const bool activated = ImGui::IsItemActivated(), released = ImGui::IsItemDeactivated();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 dot = TileDot(slotMin, slotMin + slotSize, small);
        // The dot takes clicks in the whole corner around it, which is easier to hit than the dot.
        const float reach = (slotMin.x + slotSize.x - dot.x) * 2;
        ImGui::SetCursorScreenPos(ImVec2(slotMin.x + slotSize.x - reach, slotMin.y));
        const bool dotClicked = ImGui::InvisibleButton("on", ImVec2(reach, reach), ImGuiButtonFlags_EnableNav);
        const bool dotHovered = ImGui::IsItemHovered() && !cooking;
        if (dotHovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (!cooking)
            Tip(effect.on ? "Turn off" : "Turn on");
        if (dotClicked && !cooking)
            f.host.SetEffect(tile.index, !effect.on);
        if ((hovered || dotHovered) && !state->dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("actions");
        EffectMenu(f, modelTiles, tile);

        const bool isNew = !state->memos.count(tile.key);
        if (activated)
        {
            state->held = tile.key;
            state->heldAt = io.MousePos;
            state->dragging = false;
        }
        if (held && state->held == tile.key && !state->dragging && !cooking && ImLengthSqr(io.MousePos - state->heldAt) > S(7) * S(7))
        {
            state->dragging = true;
            state->dragOrder.clear();
            for (const TileItem& item : tiles)
                state->dragOrder.push_back(item.key);
            const ImVec2 shownAt = isNew ? slotAt : state->memos[tile.key].at;
            state->heldOffset = (io.MousePos - origin) / s - shownAt;
        }
        const bool dragged = state->dragging && state->held == tile.key;
        // From the keyboard, which presses without letting go in the same frame as the mouse does.
        if (clicked && !released && !cooking)
            Select(tiles, tile.key);
        if (released && state->held == tile.key)
        {
            if (state->dragging)
                DropTile(f, modelTiles, tiles, tile.key);
            else if (clicked && !cooking)
                Select(tiles, tile.key);
            state->held.clear();
            state->dragging = false;
        }

        // Where it shows: following the mouse while dragged, and gliding to its place otherwise.
        const ImGuiID atKey = ImGui::GetID("at"), dragKey = ImGui::GetID("drag"), appearKey = ImGui::GetID("appear");
        ImVec2 at;
        float drag = 1;
        if (dragged)
        {
            at = (io.MousePos - origin) / s - state->heldOffset;
            Snap(atKey, at);
            Snap(dragKey, 1);
        }
        else
        {
            at = Animate(atKey, slotAt, 0.65f, Curve::Spring);
            drag = Animate(dragKey, 0, 0.6f, Curve::Spring);
        }
        const ImVec2 size = Animate(ImGui::GetID("size"), ImVec2(cell, height), 0.65f, Curve::Spring);
        if (isNew)
            Snap(appearKey, 0);
        const float appear = Animate(appearKey, 1, opening ? 0.6f : 0.65f, Curve::Spring, isNew && opening ? 0.12f + 0.028f * static_cast<float>(i) : 0);
        const bool selected = state->sheet == Sheet::Tune && state->tuned == tile.key;
        const float lift = Animate(ImGui::GetID("lift"), selected ? (f.down ? 5.6f : -5.6f) : hovered && !held ? -3.2f : 0.0f, 0.4f, Curve::Spring);
        const float press = Animate(ImGui::GetID("press"), held && !state->dragging ? 0.95f : 1, 0.3f, Curve::Spring);
        const float chosen = Animate(ImGui::GetID("selected"), selected ? 1.0f : 0.0f, 0.25f, Curve::Out);

        TileLook look;
        look.name = effect.name;
        look.detail = effect.failed ? "Failed" : "";
        look.color = TileColor(tile.key);
        look.reveal = Animate(ImGui::GetID("reveal"), effect.on ? 1.0f : 0.0f, 0.55f, Curve::Out);
        look.ink = Animate(ImGui::GetID("ink"), effect.on ? 1.0f : 0.0f, 0.35f, Curve::Out);
        // Red does not show on a tile's color, so it is black there.
        look.detailColor = f.effects->on ? Mix(kDanger, kOnColor, look.ink) : kDanger;
        // The time the graphics card spends on it, while debug info shows.
        if (!effect.failed && effect.gpuMs > 0)
        {
            char ms[16];
            std::snprintf(ms, sizeof(ms), "%.1f ms", effect.gpuMs);
            look.detail = ms;
            look.detailColor = Alpha(kOnColor, 0.55f);
        }
        look.gray = Animate(ImGui::GetID("gray"), f.effects->on ? 0.0f : 1.0f, 0.4f, Curve::Out, 0.03f * static_cast<float>(i));
        look.dot = Animate(ImGui::GetID("dot"), effect.on ? 1.0f : 0.0f, 0.3f, Curve::Out);
        look.spot = std::max(0.0f, Animate(ImGui::GetID("spot"), effect.on ? 1.0f : 0.0f, 0.4f, Curve::Spring));
        look.dotScale = Animate(ImGui::GetID("dot scale"), dotHovered ? 1.15f : 1, 0.25f, Curve::Spring);
        look.small = small;
        look.cooking = cooking;
        look.phase = cooking ? static_cast<float>(std::fmod(now / 1.1 + 0.09 * static_cast<double>(i) / 1.1, 1.0)) : 0;
        ImVec2 min = origin + (at + ImVec2(0, lift)) * s;
        if (opening)
            min.y += (1 - appear) * S(25.6f);
        const ImVec2 max = min + size * s;
        const float grow = (1 + 0.08f * drag) * press * (opening ? 0.7f + 0.3f * appear : 0.2f + 0.8f * appear);
        const float turn = -0.0524f * drag; // three degrees
        const float alpha = std::clamp(appear, 0.0f, 1.0f);
        state->memos[tile.key] = { at + ImVec2(0, lift), size, look.color, effect.on, effect.name };

        const auto paint = [draw, min, max, look, grow, turn, alpha, chosen, drag] {
            const int start = draw->VtxBuffer.Size;
            const float rounding = S(kTileRounding);
            if (drag > 0.01f)
                Shadow(draw, min, max, rounding, S(22.4f), S(38.4f), 0.4f * drag);
            if (chosen > 0.01f)
                Shadow(draw, min, max, rounding, S(16), S(25.6f), 0.25f * chosen);
            DrawTile(draw, min, max, look);
            if (chosen > 0.01f)
            {
                const float ring = S(3.52f);
                draw->AddRect(min - ImVec2(ring, ring) * 0.5f, max + ImVec2(ring, ring) * 0.5f, Alpha(kInk, chosen), rounding + ring / 2, ring);
            }
            Transform(draw, start, (min + max) * 0.5f, grow, turn, ImVec2(0, 0), alpha);
        };
        if (dragged)
            lifted = paint;
        else
            paint();
        ImGui::PopID();
    }

    // The tile that adds an effect: a dashed outline, solid while the add sheet is open, with a plus that turns into
    // a cross.
    {
        const size_t i = lead + tiles.size();
        ImGui::PushID("+add");
        const ImVec2 slotAt = slot(i);
        ImGui::SetCursorScreenPos(origin + slotAt * s);
        const bool clicked = ImGui::InvisibleButton("add", ImVec2(cell, height) * s, ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        if (clicked)
            Toggle(Sheet::Add);
        const bool open = state->sheet == Sheet::Add;
        const ImGuiID appearKey = ImGui::GetID("appear");
        const bool isNew = !state->plusSeen;
        if (isNew)
            Snap(appearKey, 0);
        state->plusSeen = true;
        const float appear = Animate(appearKey, 1, opening ? 0.6f : 0.65f, Curve::Spring, isNew && opening ? 0.12f + 0.028f * static_cast<float>(i) : 0);
        const ImVec2 at = Animate(ImGui::GetID("at"), slotAt, 0.65f, Curve::Spring);
        const ImVec2 size = Animate(ImGui::GetID("size"), ImVec2(cell, height), 0.65f, Curve::Spring);
        const float lift = Animate(ImGui::GetID("lift"), hovered && !held ? -3.2f : 0.0f, 0.4f, Curve::Spring);
        const float press = Animate(ImGui::GetID("press"), held ? 0.95f : 1, 0.3f, Curve::Spring);
        const float turn = Animate(ImGui::GetID("turn"), open ? IM_PI / 4 : 0.0f, 0.5f, Curve::Spring);
        ImVec2 min = origin + (at + ImVec2(0, lift)) * s;
        if (opening)
            min.y += (1 - appear) * S(25.6f);
        const ImVec2 max = min + size * s;
        const int start = draw->VtxBuffer.Size;
        const float rounding = S(kTileRounding);
        if (open)
        {
            const float line = S(2.4f);
            draw->AddRect(min + ImVec2(line, line) * 0.5f, max - ImVec2(line, line) * 0.5f, kInk, rounding - line / 2, line);
        }
        else
            DashedBox(draw, min, max, rounding, kFaint, S(2.4f));
        const ImVec2 pad(S(small ? 8 : 10.4f), S(small ? 6.4f : 8));
        const float textSize = small ? kTextTileSmall : kTextTile;
        PushSize(textSize, true);
        const float line = ImGui::GetFontSize() * 1.1f;
        ImGui::PopFont();
        Line(draw, min.x + pad.x, max.x - pad.x, max.y - pad.y - line / 2, "Add effect", kInk, textSize, true);
        DrawIcon(draw, Icon::Plus, ImVec2(min.x + pad.x + S(10.4f), max.y - pad.y - line - S(12)), S(20.8f), kInk, turn);
        Transform(draw, start, (min + max) * 0.5f, press * (opening ? 0.7f + 0.3f * appear : 0.2f + 0.8f * appear), 0, ImVec2(0, 0),
                  std::clamp(appear, 0.0f, 1.0f));
        ImGui::PopID();
    }
    if (lifted)
        lifted();
}

// The menu's buttons

enum class Control
{
    Previous,
    Name,
    Next,
    Save,
    Discard,
    Compare,
    Camera,
    More,
    Close,
    Power,
};

struct Placed
{
    Control control;
    float x;
    float width;
    int line;
};

float CloseWidth(const std::string& key)
{
    return key.empty() ? S(kRoundSize) : S(6.4f) + KeyWidth(key) + S(6.4f) + S(20.8f) + S(9.6f);
}

// Back to the game, with the menu's shortcut on it.
bool CloseButton(const char* id, ImVec2 min, const std::string& key)
{
    if (key.empty())
        return RoundButton(id, min, Icon::Close, "Back to the game");
    const ImVec2 size(CloseWidth(key), S(kRoundSize));
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const ImGuiID item = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, min + size, hovered ? kWellHover : kWell, size.y / 2);
    KeyLabel(draw, min + ImVec2(S(6.4f), S(6.4f)), size.y - S(12.8f), key);
    DrawIcon(draw, Icon::Close, ImVec2(min.x + S(6.4f) + KeyWidth(key) + S(6.4f) + S(10.4f), min.y + size.y / 2), S(18), kInk);
    Transform(draw, start, min + size * 0.5f, Animate(SubId(item, "press"), held ? 0.94f : 1, 0.25f, Curve::Spring));
    Tip("Back to the game");
    return clicked;
}

std::pair<std::string, ImU32> Subtitle(const Frame& f)
{
    if (f.effects && f.effects->loading)
        return { f.effects->list.empty() ? "Loading effects"
                                         : "Compiling effects " + std::to_string(f.effects->loaded) + " of " + std::to_string(f.effects->total),
                 kMist };
    if (f.effects && !f.effects->on)
        return { "Effects are off", kMist };
    if (f.game && f.game->changed)
        return { "Changes are not saved", kMist };
    if (f.game && !f.game->missing.empty())
        return { "Uses effects that are not installed", kCaution };
    // Nothing to count before the game's first picture, when no effect has loaded yet, or in an empty preset.
    if (f.effects && f.tileCount)
        return { std::to_string(f.onCount) + " of " + std::to_string(f.tileCount) + " effects on", kMist };
    return { {}, kMist };
}

// The preset's name in a slot of its own, which opens the presets. A new name slides in from the side it came from.
void PresetName(Frame& f, ImVec2 min, float width)
{
    const ImVec2 size(width, S(kRowHeight));
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav);
    const ImGuiID item = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int start = draw->VtxBuffer.Size;
    const bool open = state->sheet == Sheet::Presets;
    if (hovered || open)
        draw->AddRectFilled(min, min + size, kWell, S(17.6f));
    const float swap = Ease(Curve::Out, static_cast<float>((ImGui::GetTime() - state->swapped) / 0.4));
    const int nameStart = draw->VtxBuffer.Size;
    Line(draw, min.x + S(14.4f), min.x + width - S(14.4f), min.y + S(16.5f), state->presetName, kInk, kTextName, true, true);
    Transform(draw, nameStart, min + size * 0.5f, 1, 0, ImVec2(static_cast<float>(state->swapDirection) * S(25.6f) * (1 - swap), 0), swap);
    const auto [text, color] = Subtitle(f);
    Line(draw, min.x + S(8), min.x + width - S(8), min.y + S(35), text, color, kTextSmall, false, true);
    Transform(draw, start, min + size * 0.5f, Animate(SubId(item, "press"), held ? 0.96f : 1, 0.25f, Curve::Spring));
    Tip("Presets");
    if (clicked)
        Toggle(Sheet::Presets);
}

// The buttons in lines: one line with the preset on the left and the rest on the right where they fit, and lines in
// the middle otherwise, as at a side.
std::vector<Placed> LayOutControls(const Frame& f)
{
    const float gap = S(8);
    std::vector<std::pair<Control, float>> left, right;
    left.push_back({ Control::Previous, S(kRoundSize) });
    left.push_back({ Control::Name, S(f.side ? kSideNameWidth : kNameWidth) });
    left.push_back({ Control::Next, S(kRoundSize) });
    if (f.game && f.game->changed)
    {
        left.push_back({ Control::Save, PillWidth("Save##changes", PillStyle::Black) });
        left.push_back({ Control::Discard, PillWidth("Discard", PillStyle::Text) });
    }
    if (!page)
    {
        right.push_back({ Control::Compare, PillWidth("Hold to compare", PillStyle::Well) });
        right.push_back({ Control::Camera, S(kRoundSize) });
    }
    right.push_back({ Control::More, S(kRoundSize) });
    if (!page)
        right.push_back({ Control::Close, CloseWidth(f.model.menuKey) });
    if (f.effects)
        right.push_back({ Control::Power, S(kPowerSize) });
    float total = -gap;
    for (const auto& [control, width] : left)
        total += width + gap;
    for (const auto& [control, width] : right)
        total += width + gap;
    std::vector<Placed> placed;
    if (!f.side && total + S(16) <= f.width)
    {
        float x = 0;
        for (const auto& [control, width] : left)
        {
            placed.push_back({ control, x, width, 0 });
            x += width + gap;
        }
        x = f.width;
        for (auto item = right.rbegin(); item != right.rend(); ++item)
        {
            x -= item->second;
            placed.push_back({ item->first, x, item->second, 0 });
            x -= gap;
        }
        return placed;
    }
    std::vector<std::pair<Control, float>> all = left;
    for (const auto& item : right)
        if (item.first != Control::Compare)
            all.push_back(item);
    // At a side, Compare goes last, on a line of its own.
    for (const auto& item : right)
        if (item.first == Control::Compare)
            all.push_back(item);
    int line = 0;
    float x = 0;
    size_t lineStart = 0;
    const auto center = [&](size_t end) {
        const float shift = (f.width - (x - gap)) / 2;
        for (size_t i = lineStart; i < end; ++i)
            placed[i].x += shift;
    };
    for (const auto& [control, width] : all)
    {
        if (x > 0 && x + width > f.width)
        {
            center(placed.size());
            ++line;
            x = 0;
            lineStart = placed.size();
        }
        placed.push_back({ control, x, width, line });
        x += width + gap;
    }
    center(placed.size());
    return placed;
}

// The buttons. Returns whether Compare is held.
bool Controls(Frame& f, ImVec2 origin, const std::vector<Placed>& placed)
{
    if (!page)
    {
        // The space between the buttons moves the tray, as the grab handle does.
        ImGui::SetCursorScreenPos(origin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##move", ImVec2(f.width, f.rows));
        MoveWith(f);
    }
    const bool saveShown = std::any_of(placed.begin(), placed.end(), [](const Placed& p) { return p.control == Control::Save; });
    if (saveShown && !state->savedShown)
        Snap(Key("save pop"), 0);
    state->savedShown = saveShown;
    bool comparing = false;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const Placed& p : placed)
    {
        const float top = origin.y + static_cast<float>(p.line) * (S(kRowHeight) + S(kRowGap));
        const float middle = top + S(kRowHeight) / 2;
        const float x = origin.x + p.x;
        const ImVec2 at(x, middle - S(kRoundSize) / 2);
        switch (p.control)
        {
        case Control::Previous:
            if (RoundButton("previous", at, Icon::Left, "Previous preset"))
                StepPreset(f, -1);
            break;
        case Control::Name: PresetName(f, ImVec2(x, top), p.width); break;
        case Control::Next:
            if (RoundButton("next", at, Icon::Right, "Next preset"))
                StepPreset(f, 1);
            break;
        case Control::Save:
        {
            // Pops in when there are changes to save.
            const int start = draw->VtxBuffer.Size;
            if (PillButton("Save##changes", at, S(kRoundSize), PillStyle::Black))
                f.host.SaveChanges();
            const float pop = Animate(Key("save pop"), 1, 0.45f, Curve::Spring);
            Transform(draw, start, ImVec2(x + p.width / 2, middle), 0.4f + 0.6f * pop, 0, ImVec2(0, 0), std::clamp(pop, 0.0f, 1.0f));
            break;
        }
        case Control::Discard:
            // Nothing brings the changes back, so it asks first.
            if (PillButton("Discard", at, S(kRoundSize), PillStyle::Text) && state->sheet != Sheet::Ask)
                Ask(Question::Discard, state->sheet);
            break;
        case Control::Compare:
            // Effects stay off while it is held.
            PillButton("Hold to compare", at, S(kRoundSize), PillStyle::Well);
            comparing = ImGui::IsItemActive();
            Tip("Hold to see the game without effects");
            break;
        case Control::Camera:
            if (RoundButton("screenshot", at, Icon::Camera, "Screenshot", state->sheet == Sheet::Shot))
                Toggle(Sheet::Shot);
            break;
        case Control::More:
            if (RoundButton("more", at, Icon::Sliders, "Performance and settings", state->sheet == Sheet::More, kRoundSize, IM_PI / 3))
                Toggle(Sheet::More);
            break;
        case Control::Close:
            if (CloseButton("back", at, f.model.menuKey))
                f.host.CloseMenu();
            break;
        case Control::Power:
            if (PowerButton("power", ImVec2(x, middle - S(kPowerSize) / 2), f.effects->on, f.effects->on ? "Turn all effects off" : "Turn effects back on"))
                f.host.SetSwitch(Switch::Effects, !f.effects->on);
            break;
        }
    }
    return comparing;
}

// What can be taken back, in a black pill across the tray's far edge, for a few seconds.
void UndoPill(Frame& f, ImVec2 min, ImVec2 max)
{
    if (!state->undo)
        return;
    const float shown = Animate(Key("undo"), 0, 0.3f, Curve::Linear, 6);
    if (shown <= 0.01f)
    {
        state->undo = nullptr;
        return;
    }
    const float height = S(kButton), pad = S(17.6f), gap = S(6.4f), inset = S(4);
    const float button = Measure("Undo", kTextBody, true).x + S(14.4f) * 2;
    const float text = std::min(Measure(state->undoText, kTextBody, true).x, std::max(max.x - min.x - S(64) - pad - gap - button - inset, S(40)));
    const ImVec2 size(pad + text + gap + button + inset, height);
    const ImVec2 at(std::round((min.x + max.x - size.x) / 2), std::round((f.down ? max.y : min.y) - height / 2));
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::Begin(page ? "##undo page" : "##undo", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    // The tray comes to the front when it is clicked, and this stays over it.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, at + size, kToast, height / 2);
    draw->AddRect(at + ImVec2(0.5f, 0.5f), at + size - ImVec2(0.5f, 0.5f), kToastEdge, height / 2);
    Line(draw, at.x + pad, at.x + pad + text + 1, at.y + height / 2, state->undoText, kOnToast, kTextBody, true);
    const ImVec2 buttonMin(at.x + size.x - inset - button, at.y + inset), buttonSize(button, height - inset * 2);
    ImGui::SetCursorScreenPos(buttonMin);
    const bool clicked = ImGui::InvisibleButton("Undo", buttonSize);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    draw->AddRectFilled(buttonMin, buttonMin + buttonSize, Mix(kToast, kOnToast, hovered ? 0.3f : 0.16f), buttonSize.y / 2);
    Line(draw, buttonMin.x, buttonMin.x + button, at.y + height / 2, "Undo", kOnToast, kTextBody, true, true);
    FadeWindow(ImGui::GetCurrentWindow(), shown);
    ImGui::End();
    if (clicked)
    {
        const std::function<void(Frame&)> undo = std::move(state->undo);
        state->undo = nullptr;
        undo(f);
    }
}

// The tray

// Where the tray goes for where it was left: x across the game, with 0 and 1 docked at its sides, and y down it to the
// middle of the buttons. Below 0, it starts at the bottom in the middle.
ImVec2 TrayMin(const Frame& f, float height)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float x = state->x, y = state->y;
    const float cx = x < 0 ? display.x / 2 : x == 0 ? S(kMargin) + f.trayWidth / 2 : x == 1 ? display.x - S(kMargin) - f.trayWidth / 2 : x * display.x;
    const float cy = y < 0 ? display.y - S(kEdge) - S(kPad) - f.rows / 2 : y * display.y;
    return ImVec2(std::clamp(cx - f.trayWidth / 2, 0.0f, std::max(0.0f, display.x - f.trayWidth)),
                  f.down ? cy - S(kPad) - f.rows / 2 : cy + f.rows / 2 + S(kPad) - height);
}

// Draws the tray, over the game or at the place in a page. Returns its height.
float Tray(Frame& f, std::optional<ImVec2> place, float pageWidth, bool& comparing)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const float s = kit::scale;
    Follow(f);

    f.side = !page && (state->x == 0 || state->x == 1);
    f.down = !page && state->y >= 0 && state->y < 0.5f;
    f.trayWidth = page ? pageWidth : f.side ? S(kSideWidth) : std::min(display.x * 0.94f, S(kWidth));
    f.width = std::max(f.trayWidth - S(kPad) * 2, S(100));

    // The tiles: the effects of the preset in the order they run, or in the order a drag gives them.
    std::vector<TileItem> modelTiles;
    if (f.effects)
        for (size_t i = 0; i < f.effects->list.size(); ++i)
            if (state->members.count(f.effects->list[i].key))
            {
                modelTiles.push_back({ f.effects->list[i].key, i });
                if (f.effects->list[i].on)
                    ++f.onCount;
            }
    f.tileCount = modelTiles.size();
    const auto has = [&](const std::string& key) { return std::any_of(modelTiles.begin(), modelTiles.end(), [&](const TileItem& tile) { return tile.key == key; }); };
    if (!state->held.empty() && !has(state->held))
    {
        state->held.clear();
        state->dragging = false;
    }

    const std::vector<Placed> controls = LayOutControls(f);
    int lines = 1;
    for (const Placed& p : controls)
        lines = std::max(lines, p.line + 1);
    f.rows = static_cast<float>(lines) * S(kRowHeight) + static_cast<float>(lines - 1) * S(kRowGap);

    const float tileWidth = state->small ? 99.2f : 121.6f, tileHeight = state->small ? 56.0f : 73.6f;
    const float inside = f.width / s;
    const int columns = std::max(1, static_cast<int>((inside + kTileGap) / (tileWidth + kTileGap)));
    const float cell = (inside - kTileGap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    // The add-ons that change the picture, while they are on.
    std::vector<size_t> addonTiles;
    if (f.effects && f.model.addons)
        for (size_t i = 0; i < f.model.addons->size(); ++i)
        {
            const Addon& addon = (*f.model.addons)[i];
            if (addon.changesPicture)
                addonTiles.push_back(i);
        }
    const size_t lead = addonTiles.size();
    const size_t count = f.effects ? lead + modelTiles.size() + 1 : 0;
    const size_t tileRows = (count + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns);
    const float tilesTarget = tileRows ? static_cast<float>(tileRows) * tileHeight + static_cast<float>(tileRows - 1) * kTileGap : 0;
    // The area eases to a new height when rows come or go.
    const float tiles = std::max(0.0f, Animate(Key("tiles height"), tilesTarget, 0.52f, Curve::Spring));
    const float gap = count ? kTilesGap : 0;

    // How the tiles work, beside them until the user has it, while no sheet is open.
    const bool hinting = f.model.settings.menuHints && f.tileCount > 0 && state->sheet == Sheet::None;
    const float gotIt = PillWidth("Got it", PillStyle::Text);
    const float hintText = Measure(kHint, kTextSmall, false, f.width - gotIt - S(17.6f)).y;
    const float hintFull = (std::max(hintText, S(kButton)) + S(8)) / s;
    const float hint = std::max(0.0f, Animate(Key("hint height"), hinting ? hintFull : 0.0f, 0.4f, Curve::Out));

    // The sheet grows to its contents' height, measured in the frame before, and shrinks away when it closes.
    if (state->sheet != Sheet::None)
    {
        state->drawn = state->sheet;
        if (state->measuredKind == state->drawn)
            state->sheetTarget = state->measured;
    }
    else
        state->sheetTarget = 0;
    const bool growing = state->sheet != Sheet::None;
    const float sheet = std::max(0.0f, Animate(Key("sheet height"), state->sheetTarget, growing ? 0.62f : 0.32f, growing ? Curve::Spring : Curve::In));
    if (!growing && sheet < 0.5f)
        state->drawn = Sheet::None;

    const float top = page ? kPad : f.down ? kPad : kGrab;
    const float bottom = page ? kPad : f.down ? kGrab : kPad;
    f.trayHeight = S(top + bottom + sheet + hint + tiles + gap) + f.rows;

    ImVec2 min;
    if (place)
        min = *place;
    else
    {
        min = state->moving ? io.MousePos - state->moveOffset : TrayMin(f, f.trayHeight);
        if (state->moving)
        {
            min.y = std::max(min.y, 0.0f);
            // Near the middle of the game it stays there, with a line down the middle to say so.
            const float middle = (display.x - f.trayWidth) / 2;
            if (std::abs(min.x - middle) < S(kCenter))
            {
                min.x = middle;
                ImDrawList* over = ImGui::GetForegroundDrawList();
                const float x = std::round(display.x / 2);
                over->AddLine(ImVec2(x, 0), ImVec2(x, display.y), IM_COL32(0, 0, 0, 90), S(4));
                over->AddLine(ImVec2(x, 0), ImVec2(x, display.y), IM_COL32(255, 255, 255, 230), S(1.6f));
            }
        }
        // It rises into place with the spring when the menu opens, and drops back out of sight when it closes: 1 is
        // its own height and 48 more beyond the edge it is anchored by.
        if (std::exchange(state->justOpened, false))
            Snap(Key("open"), 1);
        const bool closing = f.model.closing;
        const float away = Animate(Key("open"), closing ? 1.0f : 0.0f, closing ? 0.3f : 0.5f, closing ? Curve::Leave : Curve::Spring);
        state->gone = closing && away >= 1;
        min.y += away * (f.trayHeight + S(48)) * (f.down ? -1.0f : 1.0f);
        min += Animate(Key("drop"), ImVec2(0, 0), 0.65f, Curve::Spring) * s;
        min = ImVec2(std::round(min.x), std::round(min.y));
    }
    f.trayMin = min;
    const ImVec2 size(f.trayWidth, f.trayHeight);
    const ImVec2 max = min + size;

    // Hold to compare fades the tray nearly away, so the game shows.
    const float fade = Animate(Key("compare"), state->comparing ? 0.05f : 1.0f, 0.2f, Curve::Linear);
    if (place)
    {
        ImGui::SetCursorScreenPos(min);
        ImGui::BeginChild("##tray", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    }
    else
    {
        ImGui::SetNextWindowPos(min);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##tray", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(3);
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = S(kRounding);
    if (!place)
    {
        Shadow(ImGui::GetBackgroundDrawList(), min, max, rounding, S(14), S(40), 0.32f * fade);
        // The game behind it, blurred and saturated by the host, as the design's backdrop-filter. Without it, the
        // white alone.
        if (const ImTextureID frost = GameImage(GameView::Frost); frost != ImTextureID_Invalid)
            draw->AddImageRounded(frost, min, max, min / display, max / display, IM_COL32_WHITE, rounding);
    }
    // The opacity lets the game through. On the launcher's page only black is behind, which would grey the tray.
    draw->AddRectFilled(min, max, page ? kTray : Alpha(kTray, state->opacity), rounding);
    draw->AddRect(min + ImVec2(0.5f, 0.5f), max - ImVec2(0.5f, 0.5f), spectrum::kEdge, rounding, 1.0f);
    // First, so it wins over the grab handle.
    SizeCorner(f, min, max);

    // A line in the ring's colors along the top while effects compile.
    if (f.effects)
    {
        const float shown = Animate(Key("progress"), f.effects->loading ? 1.0f : 0.0f, 0.3f, Curve::Linear);
        const float fraction = f.effects->total ? static_cast<float>(f.effects->loaded) / static_cast<float>(f.effects->total) : 0.0f;
        RainbowLine(draw, ImVec2(min.x + S(35.2f), min.y), ImVec2(max.x - S(35.2f), min.y + S(4.48f)), fraction, shown);
    }

    if (!place)
    {
        const ImVec2 grab(min.x, f.down ? max.y - S(kGrab) : min.y);
        ImGui::SetCursorScreenPos(grab);
        ImGui::InvisibleButton("##grab", ImVec2(f.trayWidth, S(kGrab)));
        const bool hovered = ImGui::IsItemHovered();
        MoveWith(f);
        const float width = Animate(Key("grab"), state->moving ? 35.2f : hovered ? 67.2f : 48.0f, 0.3f, Curve::Spring) * s;
        const ImVec2 middle(min.x + f.trayWidth / 2, grab.y + S(kGrab) / 2);
        draw->AddRectFilled(middle - ImVec2(width / 2, S(2.56f)), middle + ImVec2(width / 2, S(2.56f)),
                            state->moving ? kInk : Alpha(kInk, hovered ? 0.4f : 0.2f), S(16));
    }

    // From the anchored edge: the buttons, the tiles beyond them, then the sheet.
    float sheetTop, hintTop, tilesTop, controlsTop;
    if (f.down)
    {
        controlsTop = min.y + S(kPad);
        tilesTop = controlsTop + f.rows + S(gap);
        hintTop = tilesTop + S(tiles);
        sheetTop = hintTop + S(hint);
    }
    else
    {
        sheetTop = min.y + S(top);
        hintTop = sheetTop + S(sheet);
        tilesTop = hintTop + S(hint);
        controlsTop = tilesTop + S(tiles) + S(gap);
    }
    if (hint > 0.5f)
    {
        const ImVec2 at(min.x + S(kPad), hintTop);
        draw->PushClipRect(at, at + ImVec2(f.width, S(hint)), true);
        const int start = draw->VtxBuffer.Size;
        Paragraph(draw, at + ImVec2(S(4.8f), (S(hintFull) - hintText) / 2), at.x + f.width - gotIt - S(12.8f), kHint, kMist, kTextSmall);
        ImGui::BeginDisabled(!hinting);
        if (PillButton("Got it", ImVec2(at.x + f.width - gotIt, at.y + (S(hintFull) - S(kButton)) / 2), S(kButton), PillStyle::Text))
            f.host.SetSwitch(Switch::MenuHints, false);
        ImGui::EndDisabled();
        Transform(draw, start, at, 1, 0, ImVec2(0, 0), std::clamp(hint / hintFull, 0.0f, 1.0f));
        draw->PopClipRect();
    }
    // The room the sheet has before it would leave the game.
    const float room = place ? display.y * 0.6f : f.down ? display.y - S(8) - S(kGrab) - sheetTop : hintTop - S(kGrab) - S(8);
    if (state->drawn != Sheet::None)
        DrawSheet(f, ImVec2(min.x + S(kPad), sheetTop), std::max(S(sheet), 1.0f), room);

    if (f.effects)
    {
        const ImVec2 origin(min.x + S(kPad), tilesTop);
        // While a tile is dragged, it takes the place of the tile under the mouse. Places are where tiles are laid out,
        // not where they are on their way.
        std::vector<TileItem> ordered = modelTiles;
        if (state->dragging)
        {
            std::erase_if(state->dragOrder, [&](const std::string& key) { return !has(key); });
            for (const TileItem& tile : modelTiles)
                if (std::find(state->dragOrder.begin(), state->dragOrder.end(), tile.key) == state->dragOrder.end())
                    state->dragOrder.push_back(tile.key);
            const ImVec2 mouse = (io.MousePos - origin) / s;
            const auto held = std::find(state->dragOrder.begin(), state->dragOrder.end(), state->held);
            if (held != state->dragOrder.end())
            {
                const size_t from = static_cast<size_t>(held - state->dragOrder.begin());
                for (size_t j = 0; j < state->dragOrder.size(); ++j)
                {
                    const size_t spot = lead + j;
                    const ImVec2 at(static_cast<float>(spot % static_cast<size_t>(columns)) * (cell + kTileGap),
                                    static_cast<float>(spot / static_cast<size_t>(columns)) * (tileHeight + kTileGap));
                    if (j != from && mouse.x > at.x && mouse.x < at.x + cell && mouse.y > at.y && mouse.y < at.y + tileHeight)
                    {
                        const std::string key = state->dragOrder[from];
                        state->dragOrder.erase(state->dragOrder.begin() + static_cast<std::ptrdiff_t>(from));
                        state->dragOrder.insert(state->dragOrder.begin() + static_cast<std::ptrdiff_t>(j), key);
                        break;
                    }
                }
            }
            ordered.clear();
            for (const std::string& key : state->dragOrder)
                for (const TileItem& tile : modelTiles)
                    if (tile.key == key)
                        ordered.push_back(tile);
        }
        DrawTiles(f, origin, modelTiles, ordered, addonTiles, columns, cell, tileHeight);
    }

    comparing = Controls(f, ImVec2(min.x + S(kPad), controlsTop), controls);
    state->comparing = comparing;
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(0, 0));
    FadeWindow(window, fade);
    if (place)
        ImGui::EndChild();
    else
        ImGui::End();
    if (!f.model.closing)
        UndoPill(f, min, max);
    return f.trayHeight;
}

// Starts a frame of the tray: its style, and the menu's place and size from the model unless they are changing here.
void Begin(const Model& model)
{
    if (!state->started)
    {
        state->started = true;
        state->justOpened = !page;
        state->opened = ImGui::GetTime();
        state->masking = false;
        // A list left open when the menu closed.
        if (!page)
            ImGui::ClosePopupsOverWindow(nullptr, false);
    }
    const Settings& settings = model.settings;
    if (!state->opacityHeld)
        state->opacity = settings.menuOpacity;
    state->small = settings.smallTiles;
    if (page)
        return;
    if (settings.menuSize && *settings.menuSize != state->seenSize && !state->sizing && !state->sizeBarHeld)
    {
        state->seenSize = *settings.menuSize;
        state->size = *settings.menuSize;
    }
    if ((settings.menuX != state->seenX || settings.menuY != state->seenY) && !state->moving)
    {
        state->x = state->seenX = settings.menuX;
        state->y = state->seenY = settings.menuY;
    }
}
} // namespace

// Marking the game's interface, with the tray out of the way: dragging over the game marks an area, and each area has
// a cross that takes it away. Every change goes to the host at once, so the game shows what it does.
void MaskEditor(Frame& f)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##interface", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    std::vector<std::array<float, 5>> areas = f.game->uiMask;
    bool changed = false;
    // Scene only means something to detection.
    const bool scene = state->maskScene && f.game->detectUi;
    // A line that shows on any picture: white with a dark one around it.
    const auto outline = [&](ImVec2 min, ImVec2 max, float alpha) {
        draw->AddRect(min, max, IM_COL32(0, 0, 0, static_cast<int>(120 * alpha)), S(3.2f), 0, S(4.8f));
        draw->AddRect(min, max, IM_COL32(255, 255, 255, static_cast<int>(240 * alpha)), S(3.2f), 0, S(1.6f));
    };

    // The whole game takes the drag. The areas' crosses and the bar are over it.
    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##drag", display);
    if (ImGui::IsItemActivated())
        state->maskFrom = io.MousePos;
    const bool dragging = ImGui::IsItemActive();
    const ImVec2 from(std::min(state->maskFrom.x, io.MousePos.x), std::min(state->maskFrom.y, io.MousePos.y));
    const ImVec2 to(std::max(state->maskFrom.x, io.MousePos.x), std::max(state->maskFrom.y, io.MousePos.y));
    const bool room = areas.size() < 16;
    // A click, or a slip of the mouse, marks nothing.
    if (ImGui::IsItemDeactivated() && room && to.x - from.x > S(8) && to.y - from.y > S(8))
    {
        const ImVec2 min = ImClamp(from / display, ImVec2(0, 0), ImVec2(1, 1)), max = ImClamp(to / display, ImVec2(0, 0), ImVec2(1, 1));
        areas.push_back({ min.x, min.y, max.x - min.x, max.y - min.y, scene ? 1.0f : 0.0f });
        changed = true;
    }

    for (size_t i = 0; i < areas.size(); ++i)
    {
        const ImVec2 min = ImVec2(areas[i][0], areas[i][1]) * display, max = min + ImVec2(areas[i][2], areas[i][3]) * display;
        // Scene is darkened, interface lightened.
        draw->AddRectFilled(min, max, areas[i][4] != 0 ? IM_COL32(0, 0, 0, 90) : IM_COL32(255, 255, 255, 30), S(3.2f));
        outline(min, max, 1);
        ImGui::PushID(static_cast<int>(i));
        const float button = S(25.6f);
        const ImVec2 at(std::max(max.x - button - S(4.8f), min.x), min.y + S(4.8f));
        ImGui::SetCursorScreenPos(at);
        const bool removed = ImGui::InvisibleButton("##remove", ImVec2(button, button)) && !dragging;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        draw->AddCircleFilled(at + ImVec2(button, button) * 0.5f, button / 2, hovered ? kDanger : kToast);
        DrawIcon(draw, Icon::Close, at + ImVec2(button, button) * 0.5f, S(14.4f), kOnToast);
        ImGui::PopID();
        if (removed)
        {
            areas.erase(areas.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            break;
        }
    }
    if (dragging && room)
        outline(from, to, 0.7f);

    // What to do, and the way out, at the top of the game.
    const char* hint = !room ? "That is as many areas as there can be."
                       : scene ? "Drag over what detection took for interface to keep effects on it."
                               : "Drag over the game's interface to keep effects off it.";
    // With detection on, the other kind is a click away.
    const char* other = !f.game->detectUi ? nullptr : scene ? "Mark interface" : "Mark scene";
    const float pad = S(9.6f), gap = S(6.4f), height = S(kButton) + pad * 2;
    const float text = Measure(hint, kTextBody, true).x, clear = PillWidth("Clear", PillStyle::Text), done = PillWidth("Done", PillStyle::Black);
    const float swap = other ? PillWidth(other, PillStyle::Text) + gap : 0;
    const float width = pad + S(9.6f) + text + S(16) + swap + clear + gap + done + pad;
    const ImVec2 min(std::round((display.x - width) / 2), S(25.6f)), max = min + ImVec2(width, height);
    // Clicks on the bar are not the start of an area.
    ImGui::SetCursorScreenPos(min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bar", ImVec2(width, height));
    Shadow(draw, min, max, height / 2, S(16), S(40), 0.4f);
    draw->AddRectFilled(min, max, kTray, height / 2);
    draw->AddRect(min + ImVec2(0.5f, 0.5f), max - ImVec2(0.5f, 0.5f), spectrum::kEdge, height / 2, 0, 1.0f);
    Line(draw, min.x + pad + S(9.6f), min.x + pad + S(9.6f) + text + 1, min.y + height / 2, hint, kInk, kTextBody, true);
    if (other && PillButton(other, ImVec2(max.x - pad - done - gap - clear - swap, min.y + pad), S(kButton), PillStyle::Text))
        state->maskScene = !scene;
    if (PillButton("Clear", ImVec2(max.x - pad - done - gap - clear, min.y + pad), S(kButton), PillStyle::Text, !areas.empty()))
    {
        areas.clear();
        changed = true;
    }
    if (PillButton("Done", ImVec2(max.x - pad - done, min.y + pad), S(kButton), PillStyle::Black))
        state->masking = false;
    ImGui::End();
    if (changed)
        f.host.SetUiMask(f.gameIndex, areas);
}

Open DrawOverGame(const Model& model, Host& host, float windowScale, bool covered)
{
    page = false;
    state = &states[0];
    requested = Open::None;
    Begin(model);
    // The menu's size while the corner is dragged, before the host has it.
    const float ratio = model.settings.menuSize && *model.settings.menuSize > 0 ? static_cast<float>(state->size) / static_cast<float>(*model.settings.menuSize) : 1.0f;
    kit::scale = windowScale * ratio;
    const ImGuiStyle saved = ImGui::GetStyle();
    spectrum::ApplyStyle(ImGui::GetStyle());
    // Escape closes the sheet, or goes back to the game, unless it closes a popup, ends typing or stops waiting for a
    // shortcut.
    const bool escape = !covered && !model.closing && model.settings.recording < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                        !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    PushSize(kTextBody);
    Frame f{ model, host };
    f.game = Played(model, f.gameIndex);
    f.effects = model.effects ? &*model.effects : nullptr;
    bool comparing = false;
    // On its way out, the input is the game's again, so nothing in the tray answers the mouse, and its lists close.
    // Disabled items would fade by DisabledAlpha.
    if (model.closing)
        ImGui::ClosePopupsOverWindow(nullptr, false);
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
    ImGui::BeginDisabled(model.closing);
    if (state->masking && f.game && !model.closing)
        MaskEditor(f);
    else
        Tray(f, std::nullopt, 0, comparing);
    ImGui::EndDisabled();
    ImGui::PopStyleVar();

    // Over the game while Compare is held, and the flash of a screenshot.
    ImDrawList* front = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float label = Animate(Key("without"), comparing ? 1.0f : 0.0f, 0.25f, Curve::Out);
    if (label > 0.01f)
    {
        const std::string text = "Without effects";
        const ImVec2 size(Measure(text, kTextBody, true).x + S(41.6f), S(41.6f));
        const ImVec2 min(display.x / 2 - size.x / 2, S(25.6f) - S(16) * (1 - label));
        front->AddRectFilled(min, min + size, Alpha(kInk, label), size.y / 2);
        Line(front, min.x, min.x + size.x, min.y + size.y / 2, text, Alpha(kOnInk, label), kTextBody, true, true);
    }
    // The shot itself, framed in white, shrinks a little, then flies to the top right and fades, as the design's
    // .snapshot: 0.9 seconds, scale(.92) by 15% of it, then translate(42%, -40%) scale(.12), each part on the Out curve.
    const float age = static_cast<float>(ImGui::GetTime() - state->shot) / 0.9f;
    if (age >= 0 && age < 1)
        if (const ImTextureID picture = GameImage(GameView::Shot); picture != ImTextureID_Invalid)
        {
            float size = 1, alpha = 1;
            ImVec2 offset(0, 0);
            if (age < 0.15f)
                size = 1 - 0.08f * Ease(Curve::Out, age / 0.15f);
            else
            {
                const float t = Ease(Curve::Out, (age - 0.15f) / 0.85f);
                size = 0.92f - 0.8f * t;
                offset = ImVec2(0.42f * display.x, -0.4f * display.y) * t;
                alpha = 1 - t;
            }
            const ImVec2 center = display * 0.5f + offset;
            const ImVec2 min = center - display * (0.5f * size), max = center + display * (0.5f * size);
            const float border = S(6.4f) * size, rounding = S(16) * size;
            const ImU32 white = Alpha(IM_COL32_WHITE, alpha);
            front->AddImageRounded(picture, min, max, ImVec2(0, 0), ImVec2(1, 1), white, rounding);
            front->AddRect(min + ImVec2(border, border) * 0.5f, max - ImVec2(border, border) * 0.5f, white, rounding - border / 2, 0, border);
        }
    const float flash = 1 - static_cast<float>((ImGui::GetTime() - state->shot) / 0.5);
    if (flash > 0)
        front->AddRectFilled(ImVec2(0, 0), display, Alpha(IM_COL32_WHITE, 0.75f * flash));
    ImGui::PopFont();
    ImGui::GetStyle() = saved;
    host.Compare(comparing);
    if (escape)
    {
        if (state->masking)
            state->masking = false;
        else if (state->sheet == Sheet::Ask)
            Back();
        else if (state->sheet != Sheet::None)
            CloseSheet();
        else
            host.CloseMenu();
    }
    return std::exchange(requested, Open::None);
}

float DrawInPage(const Model& model, Host& host, ImVec2 min, float width)
{
    page = true;
    state = &states[1];
    Begin(model);
    const ImGuiStyle saved = ImGui::GetStyle();
    spectrum::ApplyStyle(ImGui::GetStyle());
    PushSize(kTextBody);
    Frame f{ model, host };
    f.game = Played(model, f.gameIndex);
    f.effects = model.effects ? &*model.effects : nullptr;
    bool comparing = false;
    const float height = Tray(f, min, width, comparing);
    ImGui::PopFont();
    ImGui::GetStyle() = saved;
    return height;
}

bool Gone()
{
    return states[0].gone;
}

void Reset()
{
    states[0] = State{};
}
} // namespace launcher::tray
