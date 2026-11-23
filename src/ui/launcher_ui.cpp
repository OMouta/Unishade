// The launcher's design, the same on every platform. See launcher_ui.h.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/launcher_ui.h"
#include "ui/caption.h"
#include "ui/kit.h"
#include "ui/tray.h"
#include "base/web_address.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace launcher
{
namespace
{
using namespace kit;
using namespace kit::spectrum;

// The design's sizes at 100%, in CSS pixels.
constexpr float kBar = 52;         // the title bar: the logo, the window's buttons and its own
constexpr float kShelfRow = 83.2f; // the games, in a row of their own under it
constexpr float kShelf = kBar + kShelfRow + 3.2f;
constexpr float kSide = 48;        // the page's margin on the left and right
constexpr float kColumn = 720;     // the page beside the game's picture, at most
constexpr float kColumnLeast = 560;
constexpr float kPictureGap = 40;
constexpr float kPictureLeast = 320; // the game's picture, narrower than which the page goes without it
constexpr float kSquare = 54.4f;   // a game on the shelf
constexpr float kSquareOn = 70.4f; // the game whose page shows
constexpr float kRingPad = 3.2f;
constexpr float kShelfGap = 11.2f;
constexpr float kPanelWidth = 736;
constexpr float kPanelPad = 22.4f;
constexpr float kSettingsBody = 560; // what Settings shows of a tab, at most
constexpr float kBlockRounding = 35.2f;
constexpr float kBlockPad = 19.2f;
constexpr float kField = 52.8f;
constexpr float kFieldGap = 8.8f;
constexpr float kFieldWidth = 288;
constexpr float kCardWidth = 216;
constexpr float kCardHeight = 73.6f;
constexpr float kButton = 41.6f;
constexpr float kRowRounding = 17.6f;
constexpr float kWhole = 1e6f; // the narrowest column of a flow with one
constexpr float kPageTitle = Css(51.2f);
constexpr float kStatusText = Css(17.6f);

// On the black of the window.
constexpr ImU32 kWhite = IM_COL32_WHITE;
constexpr ImU32 kSoft = Color(0xB8B8C6);
constexpr ImU32 kGo = Color(0x54DE7A);
constexpr ImU32 kOutline = Color(0x4A4A58);
constexpr ImU32 kOutlineText = Color(0x8A8A98);
constexpr ImU32 kBadge = Color(0xF8E44C);


constexpr const char* kRates[] = { "Follow game", "120 FPS", "60 FPS", "Custom" };
constexpr const char* kResolutions[] = { "100%", "75%", "50%" };
constexpr const char* kThemes[] = { "Light", "Dark" };
// The frame rate choice that is typed in a box.
constexpr int kCustom = static_cast<int>(std::size(kFrameRates));
constexpr char kRateNote[] = "The most frames a second Unishade shows. A lower limit leaves more of the GPU to the game.";
constexpr char kResolutionNote[] =
    "Effects run on a smaller picture that FSR upscales to the game's size. Lower is faster and softer, and screenshots get smaller.";
constexpr char kHdrNote[] =
    "Keeps an HDR game in HDR. Most effects are made for SDR and can look wrong in HDR, and add-ons that work on the picture pause.";

// What shows over the page: the launcher's own windows, the picker, and the questions that wait for an answer.
enum class Modal
{
    None,
    Games,
    Settings,
    Addons,
    Messages,
    Picker,
    Found,
    Question,
    Quit,
};

enum class Tab
{
    General,
    Performance,
    Shortcuts,
    Discord,
    About,
};

enum class Question
{
    DeletePreset,
    ResetShortcuts,
};

enum class Naming
{
    None,
    New,
    Copy,
    Rename,
};

struct State
{
    std::string game; // the id of the game whose page shows
    // Every game seen, so one that was just added gets its page, and the ones on the shelf that grow in.
    std::set<std::string> known;
    std::set<std::string> fresh;
    bool started = false;

    Modal modal = Modal::None; // what the user opened
    Modal shown = Modal::None; // what shows, which stays while it fades away
    bool open = false;
    bool focus = false;
    float header = 0; // the shown modal's header and its body's contents last frame, at 100%
    float body = 0;
    Modal measured = Modal::None;
    std::optional<Picker> picker; // kept while the picker fades away
    std::vector<FoundGame> found; // kept while the list of them fades away
    std::set<size_t> chosen;      // the found games picked to add
    std::string quitting;
    Tab tab = Tab::General;
    Question question = Question::DeletePreset;
    Modal back = Modal::None; // where the question goes back to
    std::string deleting;
    bool recordingHere = false;
    bool pictured = false; // the game's picture showed beside the page last frame

    // Naming a preset of a game, by its id.
    Naming naming = Naming::None;
    std::string namingGame;
    std::string namingTarget;
    char name[128] = {};
    bool focusName = false;
    bool nameFailed = false;

    // Renaming a game, by its id.
    std::string renaming;
    char gameName[128] = {};
    bool focusGameName = false;
    bool renameFailed = false; // shows the model's renameProblem until the name changes

    // The custom frame rate box, for a game by its id, or the defaults with an empty id.
    std::optional<std::string> rateFor;
    char rate[8] = {};
    bool focusRate = false;
    bool rateActive = false;
};
State states[2];
// The place being drawn, and its state.
Place place = Place::Window;
State* state = &states[0];

// An id that keeps animations apart from the tray's and between the places.
ImGuiID Key(const char* name)
{
    return ImHashStr(name, 0, place == Place::Game ? 0x4D454E55u : 0x57494E44u);
}

template <size_t N>
int ChoiceOf(const int (&values)[N], int value)
{
    return static_cast<int>(std::find(std::begin(values), std::end(values), value) - std::begin(values));
}

std::optional<size_t> Played(const Model& model)
{
    for (size_t i = 0; i < model.games.size(); ++i)
        if (model.games[i].running)
            return i;
    return std::nullopt;
}

std::optional<size_t> IndexOf(const Model& model, const std::string& id)
{
    for (size_t i = 0; i < model.games.size(); ++i)
        if (model.games[i].id == id)
            return i;
    return std::nullopt;
}

std::string PresetName(const Model& model, const std::string& id)
{
    for (const Game& game : model.games)
        for (const Preset& preset : game.presets)
            if (preset.id == id)
                return preset.name;
    return {};
}

void ShowGame(const std::string& id)
{
    state->game = id;
    state->renaming.clear();
    state->rateFor.reset();
    state->naming = Naming::None;
    // The page rises in.
    Snap(Key("page"), 0);
}

void CloseModal()
{
    state->modal = Modal::None;
}

void Ask(Question question)
{
    state->question = question;
    state->back = state->modal == Modal::Question ? Modal::None : state->modal;
    state->modal = Modal::Question;
}

// Follows the model: a game added since the last frame shows its page, and a page whose game is gone shows the game
// being played, or the first.
void Follow(const Model& model)
{
    std::string added;
    for (const Game& game : model.games)
        if (state->known.insert(game.id).second && state->started)
        {
            added = game.id;
            state->fresh.insert(game.id);
        }
    const bool first = !state->started;
    state->started = true;
    if (!added.empty())
        ShowGame(added);
    else if (first || !IndexOf(model, state->game))
    {
        const std::optional<size_t> played = Played(model);
        const std::string shown = played ? model.games[*played].id : model.games.empty() ? std::string() : model.games.front().id;
        // With no game on the shelf there is no page to turn to, and showing the same one again in every frame would
        // start its rising in over and over, which left the window empty.
        if (first || shown != state->game)
            ShowGame(shown);
    }
    if (!state->renaming.empty() && !IndexOf(model, state->renaming))
        state->renaming.clear();
}

// Drawing

// Moves and scales what a window and the windows in it drew so far in this frame around pivot, their clip rectangles
// too, as a CSS transform does. Clicks still go where the window was laid out.
void MoveWindow(ImGuiWindow* window, ImVec2 pivot, float by, ImVec2 offset)
{
    if (!window || (by == 1 && offset.x == 0 && offset.y == 0))
        return;
    const auto move = [&](ImVec2 point) { return pivot + (point - pivot) * by + offset; };
    for (ImDrawVert& vertex : window->DrawList->VtxBuffer)
        vertex.pos = move(vertex.pos);
    for (ImDrawCmd& command : window->DrawList->CmdBuffer)
    {
        const ImVec2 min = move(ImVec2(command.ClipRect.x, command.ClipRect.y)), max = move(ImVec2(command.ClipRect.z, command.ClipRect.w));
        command.ClipRect = ImVec4(min.x, min.y, max.x, max.y);
    }
    for (ImGuiWindow* child : window->DC.ChildWindows)
        MoveWindow(child, pivot, by, offset);
}

// The ring's colors in a rounded box, as CSS's conic-gradient(from 200deg) fills it.
void RingBox(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float alpha)
{
    if (alpha <= 0.01f)
        return;
    constexpr int kSegments = 72;
    constexpr int kColors = static_cast<int>(std::size(theme::kRainbow));
    const ImVec2 center = (min + max) * 0.5f;
    const float radius = std::sqrt(ImLengthSqr(max - min)); // past the corners
    for (int i = 0; i < kSegments; ++i)
    {
        const float a0 = 2 * IM_PI * static_cast<float>(i) / kSegments, a1 = 2 * IM_PI * static_cast<float>(i + 1) / kSegments;
        // CSS angles start at the top and go clockwise.
        float css = std::fmod((a0 + a1) / 2 + IM_PI / 2 - 200.0f * IM_PI / 180, 2 * IM_PI);
        if (css < 0)
            css += 2 * IM_PI;
        const float along = css / (2 * IM_PI) * kColors;
        const int stop = static_cast<int>(along) % kColors;
        const ImU32 color = Mix(Color(theme::kRainbow[stop]), Color(theme::kRainbow[(stop + 1) % kColors]), along - std::floor(along));
        const ImVec2 slice[] = { center, center + ImVec2(std::cos(a0), std::sin(a0)) * radius, center + ImVec2(std::cos(a1), std::sin(a1)) * radius };
        FillInside(draw, min, max, rounding, slice, Alpha(color, alpha));
    }
}

// The ring's colors from the left of a rounded box to fraction of its width.
void RainbowFill(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float fraction)
{
    constexpr int kSlices = 24;
    constexpr int kColors = static_cast<int>(std::size(theme::kRainbow));
    const float width = (max.x - min.x) / kSlices;
    const float end = min.x + (max.x - min.x) * std::clamp(fraction, 0.0f, 1.0f);
    for (int i = 0; i < kSlices; ++i)
    {
        const float left = min.x + width * static_cast<float>(i);
        if (left >= end)
            break;
        const float right = std::min(left + width + 0.5f, end);
        const float along = (static_cast<float>(i) + 0.5f) / kSlices * (kColors - 1);
        const int stop = std::min(static_cast<int>(along), kColors - 2);
        const ImU32 color = Mix(Color(theme::kRainbow[stop]), Color(theme::kRainbow[stop + 1]), along - static_cast<float>(stop));
        const ImVec2 slice[] = { ImVec2(left, min.y), ImVec2(right, min.y), ImVec2(right, max.y), ImVec2(left, max.y) };
        FillInside(draw, min, max, rounding, slice, color);
    }
}

// The design's icons for the buttons at the top, which the tray does not use.
enum class Glyph
{
    None,
    Bell,
    Puzzle,
    Sliders,
};

void DrawGlyph(ImDrawList* draw, Glyph glyph, ImVec2 center, float size, ImU32 color)
{
    // 24 units wide, drawn with lines 2 units thick, as the design's.
    const float unit = size / 24, thickness = 2 * unit;
    const auto at = [&](float x, float y) { return center + ImVec2((x - 12) * unit, (y - 12) * unit); };
    switch (glyph)
    {
    case Glyph::Bell:
        draw->PathLineTo(at(6, 16));
        draw->PathLineTo(at(6, 11));
        draw->PathArcTo(at(12, 11), 6 * unit, IM_PI, 2 * IM_PI, 12);
        draw->PathLineTo(at(18, 16));
        draw->PathLineTo(at(20, 18));
        draw->PathLineTo(at(4, 18));
        draw->PathStroke(color, thickness, ImDrawFlags_Closed);
        draw->AddLine(at(10, 21), at(14, 21), color, thickness);
        break;
    case Glyph::Puzzle:
    {
        const ImVec2 points[] = { at(9, 4), at(15, 4), at(15, 8), at(19, 8), at(19, 14), at(15, 14), at(15, 20), at(9, 20), at(9, 16), at(5, 16), at(5, 8), at(9, 8) };
        draw->AddPolyline(points, static_cast<int>(std::size(points)), color, thickness, ImDrawFlags_Closed);
        break;
    }
    case Glyph::Sliders: DrawIcon(draw, Icon::Sliders, center, size, color); break;
    case Glyph::None: break;
    }
}

// A picture's color made bright enough to stand behind it, or a color of the ring by key without one.
ImU32 Backdrop(ImU32 tint, const std::string& key)
{
    if (!tint)
        return Ring(std::hash<std::string>{}(key));
    float h = 0, s = 0, v = 0;
    ImGui::ColorConvertRGBtoHSV(((tint >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, ((tint >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                                ((tint >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, h, s, v);
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB(h, std::min(s * 1.2f, 1.0f), std::max(v, 0.85f), r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
}

// A game's square: its picture on its color, or the start of its name on a color of the ring.
void GameSquare(ImDrawList* draw, ImVec2 min, float size, float rounding, const std::string& name, const std::string& picture, const std::string& key)
{
    const ImVec2 max = min + ImVec2(size, size);
    const float inset = std::round(size * 0.14f);
    // Picture makes the texture, and only then is its tint known.
    const ImTextureID texture = picture.empty() ? ImTextureID_Invalid : Picture(picture, size - inset * 2);
    draw->AddRectFilled(min, max, Backdrop(texture != ImTextureID_Invalid ? PictureTint(picture) : 0, key), rounding);
    if (texture != ImTextureID_Invalid)
        draw->AddImageRounded(ImTextureRef(texture), min + ImVec2(inset, inset), max - ImVec2(inset, inset), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                              rounding / 2);
    else
        Line(draw, min.x, max.x, (min.y + max.y) / 2, Initial(name), kOnColor, size > S(44) ? kTextName : kTextBody, true, true);
}

// A button on the black of the window: grey text, white with a faint white behind it under the mouse.
float DarkWidth(const std::string& label, Glyph glyph, const std::string& badge)
{
    float width = S(12.8f) * 2 + Measure(label, kTextBody, true).x;
    if (glyph != Glyph::None)
        width += S(16.8f) + S(7.2f);
    if (!badge.empty())
        width += S(7.2f) + Measure(badge, Css(12), true).x + S(12.8f);
    return width;
}

bool DarkButton(const char* id, const std::string& label, ImVec2 min, Glyph glyph, const std::string& badge = {})
{
    const ImVec2 size(DarkWidth(label, glyph, badge), S(32));
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const ImGuiID key = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float lit = Animate(SubId(key, "lit"), hovered ? 1.0f : 0.0f, 0.15f, Curve::Linear);
    if (lit > 0)
        draw->AddRectFilled(min, min + size, Alpha(IM_COL32(255, 255, 255, 26), lit), size.y / 2);
    const ImU32 color = Mix(kSoft, kWhite, lit);
    const float middle = min.y + size.y / 2;
    float x = min.x + S(12.8f);
    if (glyph != Glyph::None)
    {
        DrawGlyph(draw, glyph, ImVec2(x + S(8.4f), middle), S(16.8f), color);
        x += S(16.8f) + S(7.2f);
    }
    const float width = Measure(label, kTextBody, true).x;
    Line(draw, x, x + width + 1, middle, label, color, kTextBody, true);
    x += width;
    if (!badge.empty())
    {
        const float badgeWidth = Measure(badge, Css(12), true).x + S(12.8f);
        const ImVec2 badgeMin(x + S(7.2f), middle - S(8.8f));
        draw->AddRectFilled(badgeMin, badgeMin + ImVec2(badgeWidth, S(17.6f)), kBadge, S(8.8f));
        Line(draw, badgeMin.x, badgeMin.x + badgeWidth, middle, badge, kOnColor, Css(12), true, true);
    }
    return clicked;
}

// Fields

// Lays fields out in rows of columns at least minimum wide, as the design's grids do.
struct Flow
{
    ImVec2 origin;
    float width = 0;
    int columns = 1;
    float column = 0;
    float y = 0;
    int used = 0;   // columns taken in the row
    float row = 0;  // its height

    Flow(ImVec2 at, float across, float minimum) : origin(at), width(across)
    {
        const float gap = S(kFieldGap);
        columns = std::max(1, static_cast<int>((width + gap) / (S(minimum) + gap)));
        column = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    }

    void Break()
    {
        if (used == 0)
            return;
        y += row + S(kFieldGap);
        used = 0;
        row = 0;
    }

    // Where the next field goes, span columns wide, or as wide as the flow with 0.
    ImRect Next(float height, int span = 1)
    {
        span = span == 0 ? columns : std::min(span, columns);
        if (used > 0 && used + span > columns)
            Break();
        const ImVec2 min = origin + ImVec2(static_cast<float>(used) * (column + S(kFieldGap)), y);
        const float across = span == columns ? width : static_cast<float>(span) * column + static_cast<float>(span - 1) * S(kFieldGap);
        used += span;
        row = std::max(row, height);
        if (used >= columns)
            Break();
        return ImRect(min, min + ImVec2(across, height));
    }

    float Height()
    {
        Break();
        return std::max(y - S(kFieldGap), 0.0f);
    }
};

struct Action
{
    const char* label;
    PillStyle style;
    bool enabled = true;
};

void Heading(Flow& flow, const std::string& text, float height = 30)
{
    const ImRect at = flow.Next(S(height), 0);
    Line(ImGui::GetWindowDrawList(), at.Min.x + S(4.8f), at.Max.x, at.Max.y - S(11), text, kMist, kTextHeading, true);
}

void Note(Flow& flow, const std::string& text, ImU32 color = kMist)
{
    const ImRect at = flow.Next(Measure(text, kTextBody, false, flow.width - S(9.6f)).y + S(4), 0);
    Paragraph(ImGui::GetWindowDrawList(), at.Min + ImVec2(S(4.8f), S(2)), at.Max.x - S(4.8f), text, color, kTextBody);
}

// A grey row as wide as the flow: a label with what it does under it, and room on its right for controls that wide.
// Draws the text and returns the row, which grows with it.
ImRect Described(Flow& flow, const std::string& label, const std::string& description, float controls)
{
    const float pad = S(17.6f), vertical = S(11.2f);
    const float textWidth = std::max(flow.width - pad - S(12.8f) - controls - S(11.2f), S(120));
    const float title = Measure(label, kTextBody, true, textWidth).y;
    const float detail = description.empty() ? 0 : S(2) + Measure(description, kTextSmall, false, textWidth).y;
    const ImRect at = flow.Next(std::max(S(kField), title + detail + vertical * 2), 0);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const float left = at.Min.x + pad;
    float y = at.Min.y + (at.GetHeight() - title - detail) / 2;
    y += Paragraph(draw, ImVec2(left, y), left + textWidth, label, kInk, kTextBody, true);
    if (!description.empty())
        Paragraph(draw, ImVec2(left, y + S(2)), left + textWidth, description, kMist, kTextSmall);
    return at;
}

// A grey row with a label on its left and choices on its right: one column where they fit beside the label, the whole
// width otherwise. Returns the choice clicked when it is not the selected one, or -1.
int Choices(Flow& flow, const char* id, const std::string& label, std::span<const char* const> choices, int selected)
{
    const float width = SegmentsWidth(choices);
    const float need = S(17.6f) + Measure(label, kTextBody, true).x + S(9.6f) + width + S(5.6f);
    const ImRect at = flow.Next(S(kField), need <= flow.column ? 1 : 0);
    ImGui::PushID(id);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const ImVec2 spot(at.Max.x - S(5.6f) - width, at.Min.y + (S(kField) - S(kSegmentHeight)) / 2);
    Line(draw, at.Min.x + S(17.6f), spot.x - S(9.6f), at.GetCenter().y, label, kInk, kTextBody, true);
    const int clicked = Segments("choices", spot, choices, selected);
    ImGui::PopID();
    return clicked >= 0 && clicked != selected ? clicked : -1;
}

// A grey row with a label and a switch, and what the switch does under the label when given. Returns what a click
// turns it to, or nothing.
std::optional<bool> OnOff(Flow& flow, const char* id, const std::string& label, bool on, const std::string& description = {})
{
    const float width = S(kSwitchWidth);
    if (!description.empty())
    {
        const ImRect at = Described(flow, label, description, width);
        ImGui::PushID(id);
        const bool clicked = SwitchButton("switch", ImVec2(at.Max.x - S(11.2f) - width, at.Min.y + (at.GetHeight() - S(kSwitchHeight)) / 2), on);
        ImGui::PopID();
        if (!clicked)
            return std::nullopt;
        return !on;
    }
    const float need = S(17.6f) + Measure(label, kTextBody, true).x + S(9.6f) + width + S(11.2f);
    const ImRect at = flow.Next(S(kField), need <= flow.column ? 1 : 0);
    ImGui::PushID(id);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const ImVec2 spot(at.Max.x - S(11.2f) - width, at.Min.y + (S(kField) - S(kSwitchHeight)) / 2);
    Line(draw, at.Min.x + S(17.6f), spot.x - S(9.6f), at.GetCenter().y, label, kInk, kTextBody, true);
    const bool clicked = SwitchButton("switch", spot, on);
    ImGui::PopID();
    if (!clicked)
        return std::nullopt;
    return !on;
}

// A grey row with a label and buttons on its right, or under the label where they do not fit beside it. With what the
// buttons do, that goes under the label and the buttons stay beside it. Returns the index of the button clicked, or -1.
int Buttons(Flow& flow, const char* id, const std::string& label, const std::vector<Action>& actions, const std::string& description = {})
{
    float buttons = 0;
    for (const Action& action : actions)
        buttons += PillWidth(action.label, action.style) + S(6.4f);
    if (!description.empty())
    {
        const ImRect at = Described(flow, label, description, buttons);
        ImGui::PushID(id);
        float x = at.Max.x - buttons;
        int clicked = -1;
        for (size_t i = 0; i < actions.size(); ++i)
        {
            if (PillButton(actions[i].label, ImVec2(x, at.Min.y + (at.GetHeight() - S(kButton)) / 2), S(kButton), actions[i].style, actions[i].enabled))
                clicked = static_cast<int>(i);
            x += PillWidth(actions[i].label, actions[i].style) + S(6.4f);
        }
        ImGui::PopID();
        return clicked;
    }
    const float need = S(17.6f) + (label.empty() ? 0 : Measure(label, kTextBody, true).x) + S(9.6f) + buttons;
    const bool beside = need <= flow.width;
    const ImRect at = flow.Next(beside ? S(kField) : S(kField) + S(kButton) + S(4), beside && need <= flow.column ? 1 : 0);
    ImGui::PushID(id);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const float labelRight = at.Max.x - (beside ? buttons : 0) - S(9.6f);
    if (!label.empty())
        Line(draw, at.Min.x + S(17.6f), labelRight, at.Min.y + S(kField) / 2, label, kInk, kTextBody, true);
    float x = beside ? at.Max.x - buttons : at.Min.x + S(5.6f);
    const float y = beside ? at.Min.y + (S(kField) - S(kButton)) / 2 : at.Min.y + S(kField) - S(4);
    int clicked = -1;
    for (size_t i = 0; i < actions.size(); ++i)
    {
        if (PillButton(actions[i].label, ImVec2(x, y), S(kButton), actions[i].style, actions[i].enabled))
            clicked = static_cast<int>(i);
        x += PillWidth(actions[i].label, actions[i].style) + S(6.4f);
    }
    ImGui::PopID();
    return clicked;
}

// The box a custom frame rate is typed in, which applies when it is left, unless Escape left it.
void RateBox(Flow& flow, Host& host, const Performance& performance, std::optional<size_t> game)
{
    const ImRect at = flow.Next(S(kField), 0);
    ImGui::PushID("custom rate");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const float box = S(84);
    // The box shows the limit unless it is being typed in.
    if (!state->rateActive && !state->focusRate)
        std::snprintf(state->rate, sizeof(state->rate), "%d", performance.frameRate ? performance.frameRate : 60);
    PushSize(kTextBody, true);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), std::max((S(kButton) - ImGui::GetFontSize()) / 2, 0.0f)));
    ImGui::SetCursorScreenPos(ImVec2(at.Min.x + S(5.6f), at.Min.y + (S(kField) - ImGui::GetFrameHeight()) / 2));
    ImGui::SetNextItemWidth(box);
    if (std::exchange(state->focusRate, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##rate", state->rate, sizeof(state->rate), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
    const bool left = ImGui::IsItemDeactivated();
    state->rateActive = ImGui::IsItemActive();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    const std::string range = "frames a second, from " + std::to_string(kSlowestFrameRate) + " to " + std::to_string(kFastestFrameRate);
    Line(draw, at.Min.x + S(5.6f) + box + S(12), at.Max.x - S(12), at.GetCenter().y, range, kMist, kTextBody);
    if (left && !ImGui::IsKeyPressed(ImGuiKey_Escape))
        if (const int value = std::atoi(state->rate); value > 0)
        {
            const int limit = std::clamp(value, kSlowestFrameRate, kFastestFrameRate);
            host.SetPerformance(game, PerformanceSetting::FrameRate, limit);
            // The box goes once the limit is one of the choices.
            if (ChoiceOf(kFrameRates, limit) < kCustom)
                state->rateFor.reset();
        }
    ImGui::PopID();
}

// Frame rate and effect resolution, for a game by its index or the defaults. owner is the game's id, or empty for the
// defaults.
void PerformanceFields(Flow& flow, Host& host, const Performance& performance, std::optional<size_t> game, const std::string& owner)
{
    const int preset = ChoiceOf(kFrameRates, performance.frameRate);
    // A limit that is none of the choices is a custom one, typed in the box, which also shows while Custom is picked.
    const bool custom = state->rateFor == owner || preset >= kCustom;
    const int rate = Choices(flow, "rate", "Frame rate", kRates, custom ? kCustom : preset);
    if (rate >= 0 && rate < kCustom)
    {
        state->rateFor.reset();
        host.SetPerformance(game, PerformanceSetting::FrameRate, kFrameRates[rate]);
    }
    else if (rate == kCustom)
    {
        state->rateFor = owner;
        state->focusRate = true;
    }
    if ((custom && rate < 0) || rate == kCustom)
        RateBox(flow, host, performance, game);
    Note(flow, kRateNote);
    if (!performance.effectResolution)
        return;
    const int resolution = Choices(flow, "resolution", "Effect resolution", kResolutions, ChoiceOf(kEffectResolutions, *performance.effectResolution));
    if (resolution >= 0)
        host.SetPerformance(game, PerformanceSetting::EffectResolution, kEffectResolutions[resolution]);
    Note(flow, kResolutionNote);
    if (performance.hdrOutput)
        if (const std::optional<bool> on = OnOff(flow, "hdr", "HDR output", *performance.hdrOutput, kHdrNote))
            host.SetPerformance(game, PerformanceSetting::HdrOutput, *on ? 1 : 0);
}

void ShortcutFields(Flow& flow, Host& host, const Settings& settings)
{
    Note(flow, "Click a shortcut, then press the keys you want. They work right away.");
    bool recordHovered = false;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < settings.shortcuts.size(); ++i)
    {
        const Shortcut& shortcut = settings.shortcuts[i];
        const bool recording = settings.recording == static_cast<int>(i);
        const bool clearable = !shortcut.required && !shortcut.keys.empty();
        const float clear = clearable ? S(32) + S(6.4f) : 0;
        const ImVec2 keySize(S(170), S(40));
        const ImRect at = Described(flow, shortcut.title, shortcut.description, keySize.x + clear);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 keyMin(at.Max.x - S(5.6f) - clear - keySize.x, at.Min.y + (at.GetHeight() - keySize.y) / 2);
        ImGui::SetCursorScreenPos(keyMin);
        if (ImGui::InvisibleButton("keys", keySize, ImGuiButtonFlags_EnableNav))
        {
            host.RecordShortcut(recording ? -1 : static_cast<int>(i));
            state->recordingHere = !recording;
        }
        const bool hovered = ImGui::IsItemHovered();
        recordHovered |= hovered;
        HandOnHover();
        const int start = draw->VtxBuffer.Size;
        if (recording)
            RainbowFill(draw, keyMin, keyMin + keySize, S(11.2f), 1);
        else
            draw->AddRectFilled(keyMin, keyMin + keySize, hovered ? kInkHover : kInk, S(11.2f));
        const std::string label = recording ? "Press keys..." : shortcut.keys.empty() ? "Not set" : shortcut.keys;
        Line(draw, keyMin.x + S(8), keyMin.x + keySize.x - S(8), keyMin.y + keySize.y / 2, label,
             recording ? kOnColor : shortcut.keys.empty() ? Alpha(kOnInk, 0.6f) : kOnInk, kTextBody, true, true);
        if (recording)
        {
            // Breathes while it waits, as the design's does.
            const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 1.8) / 1.8);
            Transform(draw, start, keyMin + keySize * 0.5f, 1 + 0.03f * (1 - std::cos(2 * IM_PI * t)));
        }
        if (clearable && RoundButton("clear", ImVec2(at.Max.x - S(5.6f) - S(32), at.Min.y + (at.GetHeight() - S(32)) / 2), Icon::Close, "Leave unassigned", false, 32))
            host.ClearShortcut(i);
        ImGui::PopID();
    }
    if (!settings.shortcutError.empty())
        Note(flow, settings.shortcutError, kDanger);
    for (const std::string& warning : settings.shortcutWarnings)
        Note(flow, warning, kCaution);
    if (Buttons(flow, "reset shortcuts", "", { { "Reset to defaults", PillStyle::Text } }) == 0)
        Ask(Question::ResetShortcuts);
    // Clicking anything but the shortcut that waits for keys stops the wait.
    if (settings.recording >= 0 && state->recordingHere && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !recordHovered)
    {
        host.RecordShortcut(-1);
        state->recordingHere = false;
    }
}

// The game's presets

void StartNaming(Naming naming, const std::string& game, const std::string& target, const std::string& name)
{
    state->naming = naming;
    state->namingGame = game;
    state->namingTarget = target;
    state->nameFailed = false;
    state->focusName = true;
    std::snprintf(state->name, sizeof(state->name), "%s", name.c_str());
}

// The box that names a preset, in place of its card or the card for a new one.
void NameInput(Host& host, size_t game, const ImRect& at)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const float ring = S(2.88f);
    draw->AddRect(at.Min + ImVec2(ring, ring) * 0.5f, at.Max - ImVec2(ring, ring) * 0.5f, kInk, S(kRowRounding) - ring / 2, ring);
    PushSize(kTextBody, true);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
    ImGui::SetCursorScreenPos(ImVec2(at.Min.x + S(16), at.Min.y + (at.GetHeight() - ImGui::GetFrameHeight()) / 2));
    ImGui::SetNextItemWidth(at.GetWidth() - S(32));
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
        case Naming::New: done = host.NewPreset(game, state->name); break;
        case Naming::Copy: done = host.CopyPreset(game, state->namingTarget, state->name); break;
        case Naming::Rename: done = host.RenamePreset(state->namingTarget, state->name); break;
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
void PresetMenu(const Model& model, Host& host, size_t index, const Preset& preset)
{
    const Game& game = model.games[index];
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6.4f), S(6.4f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(1.6f)));
    if (ImGui::BeginPopup("actions"))
    {
        // The folders take the place of the actions, rather than opening beside them.
        static bool moving = false;
        if (ImGui::IsWindowAppearing())
            moving = false;
        // To every folder but its own.
        const std::string& current = preset.folder.empty() ? game.name : preset.folder;
        if (moving)
        {
            if (MenuRow("Back", true, kMist))
                moving = false;
            for (const Folder& folder : model.folders)
                if (folder.name != current && MenuRow(folder.name.c_str()))
                {
                    host.MovePreset(preset.id, folder.id);
                    ImGui::CloseCurrentPopup();
                }
        }
        else
        {
            if (MenuRow("Duplicate"))
            {
                StartNaming(Naming::Copy, game.id, preset.id, preset.name + " copy");
                ImGui::CloseCurrentPopup();
            }
            if (MenuRow("Rename", !preset.loaded))
            {
                StartNaming(Naming::Rename, game.id, preset.id, preset.name);
                ImGui::CloseCurrentPopup();
            }
            if (MenuRow("Move to", !preset.loaded, kInk, true))
                moving = true;
            if (MenuRow("Delete", !preset.loaded, kDanger))
            {
                state->deleting = preset.id;
                Ask(Question::DeletePreset);
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

// The presets a game that is not being played can start with, as cards, the one it starts with black.
void PresetCards(Flow& flow, const Model& model, Host& host, size_t index)
{
    const Game& game = model.games[index];
    const bool edit = model.editPresets;
    const bool naming = state->naming != Naming::None && state->namingGame == game.id;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const Preset& preset : game.presets)
    {
        const ImRect at = flow.Next(S(kCardHeight));
        ImGui::PushID(preset.id.c_str());
        if (naming && state->naming == Naming::Rename && state->namingTarget == preset.id)
        {
            NameInput(host, index, at);
            ImGui::PopID();
            continue;
        }
        const bool used = preset.id == game.preset;
        ImGui::SetCursorScreenPos(at.Min);
        if (edit)
            ImGui::SetNextItemAllowOverlap();
        const bool clicked = ImGui::InvisibleButton("preset", at.GetSize(), ImGuiButtonFlags_EnableNav);
        const ImGuiID key = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (!used)
            HandOnHover();
        const float on = Animate(SubId(key, "on"), used ? 1.0f : 0.0f, 0.25f, Curve::Out);
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(at.Min, at.Max, Mix(hovered && !used ? kWellHover : kWell, kInk, on), S(kRowRounding));
        const ImU32 text = Mix(kInk, kOnInk, on);
        Line(draw, at.Min.x + S(16), at.Max.x - S(edit ? 44.0f : 16.0f), at.Min.y + S(22), preset.name, text, kTextBody, true);
        Line(draw, at.Min.x + S(16), at.Max.x - S(16), at.Max.y - S(18), preset.folder.empty() ? game.name : preset.folder, Alpha(text, 0.6f), kTextSmall);
        Transform(draw, start, at.GetCenter(), Animate(SubId(key, "press"), held && !used ? 0.96f : 1.0f, 0.25f, Curve::Spring));
        if (clicked && !used)
            host.UsePreset(index, preset.id);
        if (edit)
        {
            const float button = S(28);
            const ImVec2 dots(at.Max.x - S(8) - button, at.Min.y + S(8));
            ImGui::SetCursorScreenPos(dots);
            if (ImGui::InvisibleButton("##actions", ImVec2(button, button), ImGuiButtonFlags_EnableNav))
                ImGui::OpenPopup("actions");
            const bool over = ImGui::IsItemHovered();
            HandOnHover();
            if (over)
                draw->AddCircleFilled(dots + ImVec2(button, button) * 0.5f, button / 2, Alpha(text, 0.18f));
            DrawIcon(draw, Icon::Dots, dots + ImVec2(button, button) * 0.5f, S(20), text);
            PresetMenu(model, host, index, preset);
        }
        ImGui::PopID();
    }
    if (!edit)
        return;
    // A new preset, or a copy being named.
    const ImRect at = flow.Next(S(kCardHeight));
    ImGui::PushID("##new");
    if (naming && (state->naming == Naming::New || state->naming == Naming::Copy))
        NameInput(host, index, at);
    else
    {
        ImGui::SetCursorScreenPos(at.Min);
        const bool clicked = ImGui::InvisibleButton("new", at.GetSize(), ImGuiButtonFlags_EnableNav);
        const ImGuiID key = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        const int start = draw->VtxBuffer.Size;
        DashedBox(draw, at.Min, at.Max, S(kRowRounding), hovered ? Alpha(kInk, 0.5f) : kFaint, S(2.4f));
        Line(draw, at.Min.x + S(12), at.Max.x - S(12), at.GetCenter().y, "New preset", hovered ? kInk : kMist, kTextBody, true, true);
        Transform(draw, start, at.GetCenter(), Animate(SubId(key, "press"), held ? 0.96f : 1.0f, 0.25f, Curve::Spring));
        if (clicked)
            StartNaming(Naming::New, game.id, {}, "New preset");
    }
    ImGui::PopID();
}

// The game's file, with Rename and Remove, or the box that renames it.
void ExecutableRow(Flow& flow, const Model& model, Host& host, size_t index)
{
    const Game& game = model.games[index];
    const bool renaming = state->renaming == game.id;
    const bool editable = !game.picked && !renaming;
    const float buttons = editable ? PillWidth("Rename", PillStyle::Text) + S(6.4f) + PillWidth("Remove", PillStyle::Text) + S(5.6f) : 0;
    const float need = S(17.6f) + Measure(game.executable, kTextBody, true).x + S(9.6f) + buttons;
    const ImRect at = flow.Next(S(kField), renaming || need > flow.column ? 0 : 1);
    ImGui::PushID("executable");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    if (renaming)
    {
        const float ring = S(2.88f);
        draw->AddRect(at.Min + ImVec2(ring, ring) * 0.5f, at.Max - ImVec2(ring, ring) * 0.5f, kInk, S(kRowRounding) - ring / 2, ring);
        PushSize(kTextBody, true);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
        ImGui::SetCursorScreenPos(ImVec2(at.Min.x + S(17.6f), at.Min.y + (S(kField) - ImGui::GetFrameHeight()) / 2));
        ImGui::SetNextItemWidth(at.GetWidth() - S(35.2f));
        if (std::exchange(state->focusGameName, false))
            ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##name", state->gameName, sizeof(state->gameName), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemEdited())
            state->renameFailed = false;
        const bool left = ImGui::IsItemDeactivated();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
        // Enter keeps the box open when the name is not taken. Leaving it drops such a name, and Escape any name.
        if (entered)
        {
            if (host.RenameGame(index, state->gameName))
                state->renaming.clear();
            else
            {
                state->renameFailed = true;
                state->focusGameName = true;
            }
        }
        else if (left)
        {
            if (!ImGui::IsKeyPressed(ImGuiKey_Escape))
                host.RenameGame(index, state->gameName);
            state->renaming.clear();
        }
    }
    else
    {
        Line(draw, at.Min.x + S(17.6f), at.Max.x - buttons - S(9.6f), at.GetCenter().y, game.executable, kInk, kTextBody, true);
        if (editable)
        {
            float x = at.Max.x - buttons;
            const float top = at.Min.y + (S(kField) - S(kButton)) / 2;
            if (PillButton("Rename", ImVec2(x, top), S(kButton), PillStyle::Text))
            {
                state->renaming = game.id;
                state->renameFailed = false;
                state->focusGameName = true;
                std::snprintf(state->gameName, sizeof(state->gameName), "%s", game.name.c_str());
            }
            x += PillWidth("Rename", PillStyle::Text) + S(6.4f);
            if (PillButton("Remove", ImVec2(x, top), S(kButton), PillStyle::Text))
                host.RemoveGame(index);
            Tip("Remove from list");
        }
    }
    ImGui::PopID();
    if (renaming)
        Note(flow, state->renameFailed && !model.renameProblem.empty() ? model.renameProblem : "Enter saves the name. Esc cancels.",
             state->renameFailed && !model.renameProblem.empty() ? kDanger : kMist);
}

// What can be set for a game: the preset it starts with while it is not being played, whether Unishade runs on it, its
// performance, and its name. The tray has the rest for the game being played. Returns the height taken.
float GameFields(const Model& model, Host& host, size_t index, ImVec2 min, float width, bool played)
{
    const Game& game = model.games[index];
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float y = 0;
    if (!played)
    {
        const float folder = PillWidth("Open folder", PillStyle::Text);
        Line(draw, min.x + S(4.8f), min.x + width - folder - S(8), min.y + S(kButton) / 2, "Starts with", kMist, kTextHeading, true);
        if (PillButton("Open folder", ImVec2(min.x + width - folder, min.y), S(kButton), PillStyle::Text))
            host.OpenPresetsFolder(index);
        y += S(kButton) + S(6.4f);
        Flow cards(min + ImVec2(0, y), width, kCardWidth);
        PresetCards(cards, model, host, index);
        y += cards.Height();
        Flow notes(min + ImVec2(0, y + S(kFieldGap)), width, kFieldWidth);
        if (game.presets.empty())
            Note(notes, model.editPresets ? "No presets yet." : "No presets yet. Make one with New preset in the menu while the game runs.");
        if (state->naming == Naming::New && state->namingGame == game.id)
            Note(notes, "Starts with every effect off.");
        if (state->nameFailed && state->naming != Naming::None && state->namingGame == game.id && !model.presetProblem.empty())
            Note(notes, model.presetProblem, kDanger);
        if (!game.missing.empty())
        {
            std::string names;
            for (const std::string& effect : game.missing)
                names += (names.empty() ? "" : ", ") + effect;
            Note(notes, "Uses effects that are not installed: " + names + ".", kCaution);
        }
        if (const float height = notes.Height(); height > 0)
            y += S(kFieldGap) + height;
        y += S(6.4f);
    }
    Flow rows(min + ImVec2(0, y), width, kFieldWidth);
    Heading(rows, "For this game", played ? 22.0f : 30.0f);
    if (!game.picked)
        if (const std::optional<bool> on = OnOff(rows, "enabled", "Use Unishade", game.enabled, "Unishade skips it while it's off."))
            host.SetGameEnabled(index, *on);
    if (!played && game.performance)
        PerformanceFields(rows, host, *game.performance, index, game.id);
    ExecutableRow(rows, model, host, index);
    return y + rows.Height();
}

// Unishade's state where it asks something of the user: effects turned off, a window used for now, no effects
// installed, a problem with the game's window, or no game to run on.
bool StatusShown(const Model& model)
{
    const Status& status = model.status;
    return status.tone == Tone::Off || status.canDetect || status.canDownload || !status.warning.empty() || !Played(model);
}

// Returns its height.
float StatusBlock(const Model& model, Host& host, ImVec2 min, float width, ImU32 fill)
{
    enum class Do
    {
        Show,
        Download,
        Pick,
        Detect,
    };
    const Status& status = model.status;
    std::vector<std::pair<Action, Do>> buttons;
    if (status.tone == Tone::Off)
        buttons.push_back({ { "Show effects", PillStyle::Black }, Do::Show });
    if (status.canDownload)
        buttons.push_back({ { "Download effects and presets", PillStyle::Black }, Do::Download });
    if (status.canPick)
        buttons.push_back({ { "Pick a window", PillStyle::Well }, Do::Pick });
    if (status.canDetect)
        buttons.push_back({ { "Detect automatically", PillStyle::Well }, Do::Detect });
    const float pad = S(kBlockPad), gap = S(6.4f), dot = S(22);
    float buttonsWidth = 0;
    for (const auto& [action, what] : buttons)
        buttonsWidth += PillWidth(action.label, action.style) + gap;
    buttonsWidth = std::max(buttonsWidth - gap, 0.0f);
    const bool beside = buttons.empty() || width - pad * 2 - dot - buttonsWidth - S(12.8f) >= S(220);
    const float textLeft = min.x + pad + dot;
    const float textRight = min.x + width - pad - (beside && !buttons.empty() ? buttonsWidth + S(12.8f) : 0);
    const float wrap = textRight - textLeft;
    const float titleHeight = Measure(status.title, kTextBody, true, wrap).y;
    const float detailHeight = status.detail.empty() ? 0 : S(2) + Measure(status.detail, kTextBody, false, wrap).y;
    const float warningHeight = status.warning.empty() ? 0 : S(4.8f) + Measure(status.warning, kTextBody, false, wrap).y;
    const float text = titleHeight + detailHeight + warningHeight;
    const float controls = buttons.empty() ? 0 : S(kButton);
    const float inside = beside ? std::max(text, controls) : text + S(9.6f) + controls;
    const float height = pad * 2 + inside;

    ImGui::PushID("status");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, min + ImVec2(width, height), fill, S(kBlockRounding));
    float y = min.y + pad + (beside ? (inside - text) / 2 : 0);
    const ImU32 tone = status.tone == Tone::Running ? kGo : status.tone == Tone::Off ? kQuiet : Ring(5);
    draw->AddCircleFilled(ImVec2(min.x + pad + S(6), y + titleHeight / 2), S(6), tone);
    y += Paragraph(draw, ImVec2(textLeft, y), textRight, status.title, kInk, kTextBody, true);
    if (!status.detail.empty())
        y += S(2) + Paragraph(draw, ImVec2(textLeft, y + S(2)), textRight, status.detail, kMist, kTextBody);
    if (!status.warning.empty())
        Paragraph(draw, ImVec2(textLeft, y + S(4.8f)), textRight, status.warning, kCaution, kTextBody);
    float x = beside ? min.x + width - pad - buttonsWidth : textLeft;
    const float top = beside ? min.y + (height - controls) / 2 : min.y + height - pad - controls;
    for (const auto& [action, what] : buttons)
    {
        if (PillButton(action.label, ImVec2(x, top), S(kButton), action.style))
            switch (what)
            {
            case Do::Show: host.SetSwitch(Switch::EffectsShown, true); break;
            case Do::Download: host.DownloadEffects(); break;
            case Do::Pick: host.OpenPicker(false); break;
            case Do::Detect: host.DetectAutomatically(); break;
            }
        x += PillWidth(action.label, action.style) + gap;
    }
    ImGui::PopID();
    return height;
}

// Marks the item just added as taking clicks in the window's title bar, as far as it shows. The rest of the bar moves
// the window.
void InTitleBar()
{
    ImRect box(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    box.ClipWith(ImGui::GetCurrentWindow()->ClipRect);
    if (box.GetWidth() > 0 && box.GetHeight() > 0)
        caption::bar.controls.push_back({ box.Min.x, box.Min.y, box.Max.x, box.Max.y });
}

// The games along the top: the one whose page shows is larger in the ring's colors, and a dot marks the one being
// played. Then the button that adds a game, and the game removed a moment ago, which can be put back. dark is on the
// black of the window.
void Shelf(const Model& model, Host& host, ImVec2 min, float width, bool dark)
{
    const float s = kit::scale;
    const float height = S(kShelfRow);
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild("##shelf", ImVec2(std::max(width, 1.0f), height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // The wheel scrolls it sideways when the games do not fit.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel != 0)
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * S(60));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float middle = origin.y + height / 2;
    // Places are kept at 100%, and glide with the same spring as the sizes, as the design's row of squares reflows.
    float x = 6;
    for (const Game& game : model.games)
    {
        const bool selected = game.id == state->game;
        ImGui::PushID(game.id.c_str());
        const float side = Animate(ImGui::GetID("side"), selected ? kSquareOn : kSquare, 0.45f, Curve::Spring);
        const float left = Animate(ImGui::GetID("x"), x, 0.45f, Curve::Spring);
        x += (selected ? kSquareOn : kSquare) + kRingPad * 2 + kShelfGap;
        const float wrap = (side + kRingPad * 2) * s;
        const ImVec2 wrapMin(origin.x + left * s, middle - wrap / 2), wrapMax = wrapMin + ImVec2(wrap, wrap);
        ImGui::SetCursorScreenPos(wrapMin);
        const bool clicked = ImGui::InvisibleButton("game", ImVec2(wrap, wrap), ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        Tip(game.name);
        if (clicked && !selected)
            ShowGame(game.id);
        // A game added since grows in.
        const ImGuiID appearKey = ImGui::GetID("appear");
        if (state->fresh.erase(game.id))
            Snap(appearKey, 0);
        const float appear = Animate(appearKey, 1, 0.65f, Curve::Spring);
        const float ring = Animate(ImGui::GetID("ring"), selected ? 1.0f : 0.0f, 0.3f, Curve::Out);
        const float press = Animate(ImGui::GetID("press"), held ? 0.94f : hovered ? 1.06f : 1.0f, 0.3f, Curve::Spring);

        const int start = draw->VtxBuffer.Size;
        RingBox(draw, wrapMin, wrapMax, S(19.2f), ring);
        const ImVec2 squareMin = wrapMin + ImVec2(S(kRingPad), S(kRingPad));
        const float square = side * s;
        const float grown = std::clamp((side - kSquare) / (kSquareOn - kSquare), 0.0f, 1.0f);
        const float rounding = S(14.4f + 1.92f * grown);
        const int squareStart = draw->VtxBuffer.Size;
        GameSquare(draw, squareMin, square, rounding, game.name, game.picture, game.id);
        if (ring > 0.01f)
        {
            const ImVec2 inset(S(1.6f), S(1.6f));
            draw->AddRect(squareMin + inset, squareMin + ImVec2(square, square) - inset, Alpha(IM_COL32_BLACK, ring), rounding - S(1.6f), S(3.2f));
        }
        Transform(draw, squareStart, squareMin + ImVec2(square, square) * 0.5f, press, 0, ImVec2(0, 0), game.enabled ? 1.0f : 0.45f);
        if (game.running)
        {
            const ImVec2 dot = wrapMax - ImVec2(S(1.6f) + S(6), S(1.6f) + S(6));
            draw->AddCircleFilled(dot, S(6) + S(3.2f), IM_COL32_BLACK);
            draw->AddCircleFilled(dot, S(6), kGo);
        }
        Transform(draw, start, (wrapMin + wrapMax) * 0.5f, 0.2f + 0.8f * appear, 0, ImVec2(0, 0), std::clamp(appear, 0.0f, 1.0f));
        ImGui::PopID();
    }

    // Adds a game.
    {
        ImGui::PushID("##add");
        const float left = Animate(ImGui::GetID("x"), x, 0.45f, Curve::Spring);
        const ImVec2 addMin(origin.x + (left + kRingPad) * s, middle - S(kSquare) / 2), addMax = addMin + ImVec2(S(kSquare), S(kSquare));
        ImGui::SetCursorScreenPos(addMin);
        const bool clicked = ImGui::InvisibleButton("add", addMax - addMin, ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        Tip("Add game");
        const int start = draw->VtxBuffer.Size;
        DashedBox(draw, addMin, addMax, S(14.4f), dark ? kOutline : kFaint, S(2.4f));
        DrawIcon(draw, Icon::Plus, (addMin + addMax) * 0.5f, S(20.8f), dark ? kOutlineText : kMist);
        Transform(draw, start, (addMin + addMax) * 0.5f, Animate(ImGui::GetID("press"), held ? 0.94f : hovered ? 1.06f : 1.0f, 0.3f, Curve::Spring));
        if (clicked)
            host.OpenPicker(true);
        x += kSquare + kRingPad * 2 + kShelfGap;
        ImGui::PopID();
    }

    // Undo follows the text, away from where the game was clicked.
    if (model.removed)
    {
        const std::string text = "Removed " + model.removed->name + ".";
        const float textWidth = Measure(text, kTextBody).x;
        const float textLeft = origin.x + x * s;
        Line(draw, textLeft, textLeft + textWidth + 1, middle, text, dark ? kSoft : kMist, kTextBody);
        const ImVec2 undo(textLeft + textWidth + S(6.4f), middle - (dark ? S(16) : S(kButton) / 2));
        if (dark ? DarkButton("undo", "Undo", undo, Glyph::None) : PillButton("Undo", undo, S(kButton), PillStyle::Text))
            host.UndoRemove();
        x +=(textWidth + S(6.4f) + (dark ? DarkWidth("Undo", Glyph::None, {}) : PillWidth("Undo", PillStyle::Text))) / s + kShelfGap;
    }
    ImGui::SetCursorScreenPos(origin + ImVec2(x * s, 0));
    ImGui::Dummy(ImVec2(1, height));
    // Games that do not fit fade out at the side they go on to, which the wheel scrolls to.
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImRect box = window->InnerRect;
    const ImU32 solid = dark ? kBackground : kPaper, clear = Alpha(solid, 0);
    const float fade = S(36);
    if (window->Scroll.x > 0.5f)
        draw->AddRectFilledMultiColor(box.Min, ImVec2(box.Min.x + fade, box.Max.y), solid, clear, clear, solid);
    if (window->Scroll.x < window->ScrollMax.x - 0.5f)
        draw->AddRectFilledMultiColor(ImVec2(box.Max.x - fade, box.Min.y), box.Max, clear, solid, solid, clear);
    ImGui::EndChild();
}

// The page of a game, or what the Games window shows of it: its name and what Unishade does with it, the tray while it
// is being played, and what can be set for it. dark is on the black of the window, with the tray. Returns its height.
float GameBody(const Model& model, Host& host, size_t index, ImVec2 min, float width, bool dark)
{
    const Game& game = model.games[index];
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool played = game.running && Played(model) == index;
    float y = 0;
    const float titleSize = dark ? kPageTitle : kTextTitle;
    const float titleHeight = Measure(game.name, titleSize, true).y;
    Line(draw, min.x, min.x + width, min.y + titleHeight / 2, game.name, dark ? kWhite : kInk, titleSize, true);
    y += titleHeight * 0.9f + S(dark ? 11.2f : 4.8f);

    std::string lead, rest;
    if (game.running)
    {
        lead = "Running.";
        if (model.status.tone == Tone::Running)
            rest = model.status.detail;
    }
    else if (game.enabled)
    {
        lead = "Not running.";
        rest = "Effects start when you open it.";
    }
    else
    {
        lead = "Off.";
        rest = "Unishade skips it while it's off.";
    }
    const float statusSize = dark ? kStatusText : kTextBody;
    const ImVec2 leadSize = Measure(lead, statusSize, game.running);
    Line(draw, min.x, min.x + leadSize.x + 1, min.y + y + leadSize.y / 2, lead, game.running ? (dark ? kGo : kInk) : (dark ? kSoft : kMist), statusSize,
         game.running);
    if (!rest.empty())
        Line(draw, min.x + leadSize.x + S(5), min.x + width, min.y + y + leadSize.y / 2, rest, dark ? kSoft : kMist, statusSize);
    y += leadSize.y + S(dark ? 22.4f : 14.4f);

    if (played && dark)
        y += tray::DrawInPage(model, host, min + ImVec2(0, y), width) + S(14.4f);

    // On the page, in a block of the tray's white.
    const ImVec2 blockMin = min + ImVec2(0, y);
    const float pad = dark ? S(kBlockPad) : 0;
    if (dark)
    {
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
    }
    const float inside = GameFields(model, host, index, blockMin + ImVec2(pad, pad), width - pad * 2, played);
    if (dark)
    {
        draw->ChannelsSetCurrent(0);
        const ImVec2 blockMax = blockMin + ImVec2(width, inside + pad * 2);
        draw->AddRectFilled(blockMin, blockMax, kTray, S(kBlockRounding));
        draw->AddRect(blockMin + ImVec2(0.5f, 0.5f), blockMax - ImVec2(0.5f, 0.5f), kEdge, S(kBlockRounding), 1.0f);
        draw->ChannelsMerge();
    }
    return y + inside + pad * 2;
}

// The window's buttons at the right of the title bar. Returns how wide they are, and draws them unless measuring.
float TopButtons(const Model& model, float right, float middle, bool measuring)
{
    struct Top
    {
        const char* id;
        const char* label;
        Glyph glyph;
        std::string badge;
        Modal modal;
    };
    const size_t count = model.cards.size() +
                         static_cast<size_t>(std::count_if(model.notices.begin(), model.notices.end(), [](const Notice& notice) { return notice.level >= Level::Warning; }));
    std::vector<Top> tops;
    tops.push_back({ "messages", "Messages", Glyph::Bell, count ? std::to_string(count) : std::string(), Modal::Messages });
    if (model.addons)
        tops.push_back({ "addons", "Add-ons", Glyph::Puzzle, {}, Modal::Addons });
    tops.push_back({ "settings", "Settings", Glyph::Sliders, {}, Modal::Settings });
    const float gap = S(9.6f);
    float width = -gap;
    for (const Top& top : tops)
        width += DarkWidth(top.label, top.glyph, top.badge) + gap;
    if (measuring)
        return width;
    float x = right - width;
    for (const Top& top : tops)
    {
        if (DarkButton(top.id, top.label, ImVec2(x, middle - S(16)), top.glyph, top.badge))
            state->modal = top.modal;
        InTitleBar();
        x += DarkWidth(top.label, top.glyph, top.badge) + gap;
    }
    return width;
}

// Minimize, maximize and close at the right end of the title bar, as Windows 11's, where the window has none of its
// own. The host gets the mouse over them, as the system does over its own buttons, and says which is hovered and
// pressed. They stay above the modals, which do not cover them. Returns how wide they are.
float CaptionButtons(float /*height*/, float middle)
{
    caption::Bar& bar = caption::bar;
    if (!bar.drawButtons)
        return 0;
    // As high as the pills beside them and on their line, so the row reads as one set of controls.
    const float width = std::round(S(40));
    const float half = std::round(S(20));
    const float right = ImGui::GetIO().DisplaySize.x - std::round(S(14));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    // Thin lines on whole pixels, 10 wide at 100%.
    const float thickness = std::max(1.0f, std::round(S(1)));
    const float glyph = std::round(S(10));
    constexpr caption::Button kButtons[] = { caption::Button::Minimize, caption::Button::Maximize, caption::Button::Close };
    constexpr const char* kKeys[] = { "caption minimize", "caption maximize", "caption close" };
    constexpr ImU32 kRed = Color(0xC42B1C);
    for (int i = 0; i < 3; ++i)
    {
        const caption::Button button = kButtons[i];
        const ImVec2 min(right - width * static_cast<float>(3 - i), std::round(middle) - half), max(min.x + width, std::round(middle) + half);
        bar.buttons[i] = { min.x, min.y, max.x, max.y };
        const bool hovered = bar.hovered == button, pressed = hovered && bar.pressed == button;
        const bool close = button == caption::Button::Close;
        const float lit = Animate(Key(kKeys[i]), hovered ? 1.0f : 0.0f, 0.15f, Curve::Linear);
        if (lit > 0)
            draw->AddRectFilled(min, max, close ? Alpha(kRed, pressed ? 0.9f * lit : lit) : Alpha(IM_COL32(255, 255, 255, pressed ? 15 : 26), lit),
                                half);
        const ImU32 color = pressed && !close ? Alpha(kWhite, 0.8f) : Mix(kSoft, kWhite, lit);

        // Positions in the glyph from 0 to 1, with the lines' centers inside its box.
        const ImVec2 origin(std::round(min.x + (width - glyph) / 2), std::round(middle - glyph / 2));
        const auto at = [&](float x, float y) {
            return origin + ImVec2(std::round(x * (glyph - thickness)), std::round(y * (glyph - thickness))) + ImVec2(thickness, thickness) * 0.5f;
        };
        const auto lines = [&](std::initializer_list<ImVec2> points, bool closed) {
            draw->AddPolyline(points.begin(), static_cast<int>(points.size()), color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
        };
        switch (button)
        {
        case caption::Button::Minimize: lines({ at(0, 0.5f), at(1, 0.5f) }, false); break;
        case caption::Button::Maximize:
            if (bar.maximized)
            {
                // Restore: a square in front of another.
                lines({ at(0, 0.2f), at(0.8f, 0.2f), at(0.8f, 1), at(0, 1) }, true);
                lines({ at(0.2f, 0.2f), at(0.2f, 0), at(1, 0), at(1, 0.8f), at(0.8f, 0.8f) }, false);
            }
            else
                lines({ at(0, 0), at(1, 0), at(1, 1), at(0, 1) }, true);
            break;
        case caption::Button::Close:
            lines({ at(0, 0), at(1, 1) }, false);
            lines({ at(1, 0), at(0, 1) }, false);
            break;
        case caption::Button::None: break;
        }
    }
    return ImGui::GetIO().DisplaySize.x - (right - width * 3);
}

// The launcher's window: the title bar, the shelf of games under it, and the page of the game picked on the shelf.
void WindowView(const Model& model, Host& host)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // The title bar, where the platform lets the window draw it: the logo after the system's buttons at its left, or
    // at the left with the buttons the launcher draws at its right. What is between the logo and the buttons moves the
    // window.
    const float side = S(kSide);
    const float middle = S(kBar) / 2;
    caption::bar.height = S(kBar);
    caption::bar.controls.clear();
    const float captions = CaptionButtons(S(kBar), middle);
    const float right = captions > 0 ? display.x - captions - S(9.6f) : display.x - side;
    TopButtons(model, right, middle, false);
    const float logo = S(25.6f);
    const ImVec2 logoMin(std::round(std::max(side, caption::bar.inset)), std::round(middle - logo / 2));
    if (const ImTextureID picture = Picture("logo", logo); picture != ImTextureID_Invalid)
        ImGui::GetWindowDrawList()->AddImage(picture, logoMin, logoMin + ImVec2(logo, logo));

    // The games, across the window.
    Shelf(model, host, ImVec2(side - S(6), S(kBar)), display.x - side * 2 + S(12), true);

    // The page of the game being played has the game's picture on its right, as the design's .gameview, where the host
    // has it and the window has room for both. Otherwise the page takes the whole width.
    const std::optional<size_t> shownIndex = IndexOf(model, state->game);
    const bool playedPage = shownIndex && Played(model) == shownIndex;
    const float available = std::max(display.x - side * 2, S(200));
    const bool room = available >= S(kColumnLeast + kPictureGap + kPictureLeast);
    ImVec2 pictureSize;
    const ImTextureID picture = playedPage && room ? GameImage(GameView::Preview, &pictureSize) : ImTextureID_Invalid;
    const bool pictured = picture != ImTextureID_Invalid;
    // Without a picture of the game, a banner in its icon's colors stands there.
    const bool banner = !pictured && shownIndex && room;
    const bool beside = pictured || banner;
    // It comes in as a new page does, and the page makes room for it.
    if (beside && !state->pictured)
        Snap(Key("picture"), 0);
    state->pictured = beside;
    const float split = Animate(Key("split"), beside ? 1.0f : 0.0f, 0.5f, Curve::Out);
    const float narrow = std::clamp(available * 0.6f, S(kColumnLeast), S(kColumn));
    const float column = std::max(available + (narrow - available) * split, S(200));
    const float enter = Animate(Key("page"), 1, 0.5f, Curve::Out);
    if (pictured)
    {
        const float shown = std::min(enter, Animate(Key("picture"), 1, 0.5f, Curve::Out, 0.06f));
        const ImVec2 min(side + narrow + S(kPictureGap), S(kShelf) + S(9.6f));
        const ImVec2 max(display.x - side, std::max(display.y - S(38.4f), min.y + S(100)));
        // Cut to the box's shape from the middle, as CSS's background-size: cover.
        const float box = (max.x - min.x) / (max.y - min.y);
        const float image = pictureSize.y > 0 ? pictureSize.x / pictureSize.y : box;
        const ImVec2 cut = image > box ? ImVec2(box / image, 1) : ImVec2(1, image / box);
        const ImVec2 uv0 = (ImVec2(1, 1) - cut) * 0.5f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const int start = draw->VtxBuffer.Size;
        draw->AddImageRounded(picture, min, max, uv0, uv0 + cut, IM_COL32_WHITE, S(kBlockRounding));
        Transform(draw, start, (min + max) * 0.5f, 1, 0, ImVec2(0, S(24) * (1 - shown)), shown);
    }
    else if (banner)
    {
        const Game& game = model.games[*shownIndex];
        const float shown = std::min(enter, Animate(Key("picture"), 1, 0.5f, Curve::Out, 0.06f));
        const ImVec2 min(side + narrow + S(kPictureGap), S(kShelf) + S(9.6f));
        const ImVec2 max(display.x - side, std::max(display.y - S(38.4f), min.y + S(100)));
        const float square = S(88);
        // Picture makes the texture, and only then is its tint known.
        const bool has = !game.picture.empty() && Picture(game.picture, square - std::round(square * 0.14f) * 2) != ImTextureID_Invalid;
        const ImU32 tint = Backdrop(has ? PictureTint(game.picture) : 0, game.id);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(min, max, kWhite, S(kBlockRounding));
        // One color getting darker, so the square stands out on it. A second color mixed in turns it to mud.
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, min, max, Mix(tint, IM_COL32_BLACK, 0.3f),
                                                      Mix(tint, IM_COL32_BLACK, 0.72f));
        const ImVec2 center = (min + max) * 0.5f;
        const ImVec2 squareMin(std::round(center.x - square / 2), std::round(center.y - square / 2));
        GameSquare(draw, squareMin, square, S(22), game.name, game.picture, game.id);
        Transform(draw, start, center, 1, 0, ImVec2(0, S(24) * (1 - shown)), shown);
    }

    // The page scrolls under the shelf. Each game's page keeps its own scroll.
    ImGui::SetCursorScreenPos(ImVec2(0, S(kShelf)));
    ImGui::BeginChild(("##page " + state->game).c_str(), ImVec2(display.x, std::max(display.y - S(kShelf), 1.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 at = origin + ImVec2(side, S(9.6f));
    float y = 0;
    if (StatusShown(model))
        y += StatusBlock(model, host, at, column, kTray) + S(19.2f);
    if (shownIndex)
        y += GameBody(model, host, *shownIndex, at + ImVec2(0, y), column, true);
    ImGui::SetCursorScreenPos(at + ImVec2(0, y + S(38.4f)));
    ImGui::Dummy(ImVec2(1, 1));
    // A new page rises in.
    ImGuiWindow* page = ImGui::GetCurrentWindow();
    MoveWindow(page, origin, 1, ImVec2(0, S(24) * (1 - enter)));
    FadeWindow(page, enter);
    ImGui::EndChild();
    ScrollCue(page, Alpha(kWhite, 0.3f));
    ImGui::End();
}

// The modals

// A title with any line under it, and buttons on the right level with the bottom of the text, or under it where they
// do not fit. Returns the index of the button clicked, or -1.
int Header(ImVec2 min, float width, const std::string& title, const std::string& detail, const std::vector<Action>& actions, float& height)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float gap = S(6.4f), button = S(kButton);
    float buttons = 0;
    for (const Action& action : actions)
        buttons += PillWidth(action.label, action.style) + gap;
    buttons = std::max(buttons - gap, 0.0f);
    const bool beside = width - buttons - S(12.8f) >= S(200);
    const float textWidth = beside ? width - buttons - S(12.8f) : width;
    const float titleHeight = title.empty() ? 0 : Measure(title, kTextTitle, true, textWidth).y;
    const float detailHeight = detail.empty() ? 0 : Measure(detail, kTextBody, false, textWidth).y;
    const float textHeight = titleHeight + (titleHeight > 0 && detailHeight > 0 ? S(4.8f) : 0) + detailHeight;
    height = beside ? std::max(textHeight, button) : textHeight + (actions.empty() ? 0 : S(9.6f) + button);
    float y = min.y + (beside ? height - textHeight : 0);
    if (!title.empty())
    {
        Paragraph(draw, ImVec2(min.x, y), min.x + textWidth, title, kInk, kTextTitle, true);
        y += titleHeight + S(4.8f);
    }
    if (!detail.empty())
        Paragraph(draw, ImVec2(min.x, y), min.x + textWidth, detail, kMist, kTextBody);
    float x = min.x + width - buttons;
    const float top = min.y + height - button;
    int clicked = -1;
    for (size_t i = 0; i < actions.size(); ++i)
    {
        if (PillButton(actions[i].label, ImVec2(x, top), button, actions[i].style, actions[i].enabled))
            clicked = static_cast<int>(i);
        x += PillWidth(actions[i].label, actions[i].style) + gap;
    }
    return clicked;
}

// Settings' pages as tabs, in as many rows as they need beside Done.
void SettingsHeader(const Model& model, Host& host, ImVec2 min, float width, float& height)
{
    std::vector<Tab> tabs{ Tab::General };
    std::vector<const char*> labels{ "General" };
    if (model.settings.performance)
    {
        tabs.push_back(Tab::Performance);
        labels.push_back("Performance");
    }
    if (!model.settings.shortcuts.empty())
    {
        tabs.push_back(Tab::Shortcuts);
        labels.push_back("Shortcuts");
    }
    if (model.discord)
    {
        tabs.push_back(Tab::Discord);
        labels.push_back("Discord");
    }
    tabs.push_back(Tab::About);
    labels.push_back("About");
    if (std::find(tabs.begin(), tabs.end(), state->tab) == tabs.end())
        state->tab = Tab::General;
    const size_t selected = static_cast<size_t>(std::find(tabs.begin(), tabs.end(), state->tab) - tabs.begin());
    const float done = PillWidth("Done", PillStyle::Black);
    const float room = std::max(width - done - S(12.8f), S(120));
    std::optional<size_t> clicked;
    float rows = 0;
    for (size_t first = 0; first < labels.size();)
    {
        size_t last = first + 1;
        while (last < labels.size() && SegmentsWidth(std::span<const char* const>(labels.data() + first, last + 1 - first), true) <= room)
            ++last;
        ImGui::PushID(static_cast<int>(first));
        const int picked = Segments("tabs", min + ImVec2(0, rows), std::span<const char* const>(labels.data() + first, last - first),
                                    selected >= first && selected < last ? static_cast<int>(selected - first) : -1, true);
        ImGui::PopID();
        if (picked >= 0)
            clicked = first + static_cast<size_t>(picked);
        rows += S(kSegmentHeight) + S(6.4f) * 2;
        first = last;
    }
    rows -= S(6.4f);
    height = std::max(rows, S(kButton));
    if (PillButton("Done", ImVec2(min.x + width - done, min.y + height - S(kButton)), S(kButton), PillStyle::Black))
        CloseModal();
    if (clicked && tabs[*clicked] != state->tab)
    {
        // A shortcut left waiting for keys on its page would take the next keys pressed on another.
        if (state->recordingHere)
        {
            host.RecordShortcut(-1);
            state->recordingHere = false;
        }
        state->tab = tabs[*clicked];
        state->rateFor.reset();
    }
}

void MenuSizeRow(Flow& flow, Host& host, const Settings& settings)
{
    const float minus = PillWidth("-##menu", PillStyle::Well), plus = PillWidth("+##menu", PillStyle::Well), value = S(72);
    const float controls = minus + value + plus;
    const ImRect at = Described(flow, "Menu size", "On top of the size that follows the game's window. Changes right away.", controls);
    ImGui::PushID("menu size");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float left = at.Max.x - S(5.6f) - controls;
    const float top = at.Min.y + (at.GetHeight() - S(kButton)) / 2;
    if (PillButton("-##menu", ImVec2(left, top), S(kButton), PillStyle::Well, settings.canShrinkMenu))
        host.StepMenuSize(-1);
    Line(draw, left + minus, left + minus + value, at.GetCenter().y, std::to_string(*settings.menuSize) + "%", kInk, kTextBody, true, true);
    if (PillButton("+##menu", ImVec2(left + minus + value, top), S(kButton), PillStyle::Well, settings.canGrowMenu))
        host.StepMenuSize(1);
    ImGui::PopID();
}

// The links, side by side in as many lines as they need.
void Links(Flow& flow, const Model& model, Host& host)
{
    if (model.links.empty())
        return;
    const float gap = S(6.4f);
    std::vector<ImVec2> places;
    float x = 0, y = 0;
    for (const std::string& link : model.links)
    {
        const float width = PillWidth(link.c_str(), PillStyle::Well);
        if (x > 0 && x + width > flow.width)
        {
            x = 0;
            y += S(kButton) + gap;
        }
        places.push_back(ImVec2(x, y));
        x += width + gap;
    }
    const ImRect at = flow.Next(y + S(kButton), 0);
    for (size_t i = 0; i < model.links.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        if (PillButton(model.links[i].c_str(), at.Min + places[i], S(kButton), PillStyle::Well))
            host.OpenLink(i);
        ImGui::PopID();
    }
}

float SettingsBody(const Model& model, Host& host, ImVec2 min, float width)
{
    const Settings& settings = model.settings;
    Flow flow(min, width, kFieldWidth);
    ImGui::PushID(static_cast<int>(state->tab));
    switch (state->tab)
    {
    case Tab::General:
        if (settings.startWithWindows)
            if (const std::optional<bool> on = OnOff(flow, "start", "Start with Windows", *settings.startWithWindows, "Starts in the notification area when you sign in."))
                host.SetSwitch(Switch::StartWithWindows, *on);
        if (const std::optional<bool> on = OnOff(flow, "shown", "Show effects", model.status.effectsShown, "Turn off to see the game without them and stop capturing it."))
            host.SetSwitch(Switch::EffectsShown, *on);
        if (const std::optional<bool> on =
                OnOff(flow, "autosave", "Save changes automatically", settings.autoSavePresets, "Turn off to try changes first, then save or discard them."))
            host.SetSwitch(Switch::AutoSavePresets, *on);
        if (settings.skipTitleBar)
            if (const std::optional<bool> on = OnOff(flow, "titlebar", "Skip the title bar", *settings.skipTitleBar,
                                                     "Effects leave a windowed game's title bar alone. Turn off for borderless games in a window."))
                host.SetSwitch(Switch::SkipTitleBar, *on);
        if (settings.keepEffectsVisible)
            if (const std::optional<bool> on = OnOff(flow, "keep", "Keep effects visible when another window is in front", *settings.keepEffectsVisible,
                                                     "Effects stay over the game while another window, such as a chat or a browser, is in front of it."))
                host.SetSwitch(Switch::KeepEffectsVisible, *on);
        if (settings.updateChecks)
            if (const std::optional<bool> on = OnOff(flow, "updates", "Check for updates", *settings.updateChecks,
                                                     "Asks GitHub for a newer version when Unishade starts. Applies from the next start."))
                host.SetSwitch(Switch::UpdateChecks, *on);
        if (const int theme = Choices(flow, "theme", "Theme", kThemes, settings.darkMenu ? 1 : 0); theme >= 0)
            host.SetSwitch(Switch::DarkMenu, theme == 1);
        if (settings.menuSize)
            MenuSizeRow(flow, host, settings);
        if (settings.reloadEffects &&
            Buttons(flow, "reload", "Reload effects", { { "Reload", PillStyle::Text } }, "Compiles every effect again, such as after you change one.") == 0)
            host.ReloadEffects();
        if (settings.downloadEffects &&
            Buttons(flow, "download", "Download effects and presets again", { { "Download", PillStyle::Text, *settings.downloadEffects } },
                    "Gets every package from ReShade's list again, such as ones that were skipped. Your presets are kept.") == 0)
            host.DownloadEffects();
        break;
    case Tab::Performance:
        Note(flow, "For every game without its own settings. A game's page changes its own.");
        if (settings.performance)
            PerformanceFields(flow, host, *settings.performance, std::nullopt, {});
        if (settings.debugInfo)
            if (const std::optional<bool> on = OnOff(flow, "debug", "Show debug info", *settings.debugInfo, "Frame rates and frame times, at the top of the game."))
                host.SetSwitch(Switch::DebugInfo, *on);
        if (settings.uiMaskShown)
            if (const std::optional<bool> on = OnOff(flow, "uimask", "Show the interface mask", *settings.uiMaskShown, "Tints what effects leave as the game drew it."))
                host.SetSwitch(Switch::UiMaskShown, *on);
        break;
    case Tab::Shortcuts: ShortcutFields(flow, host, settings); break;
    case Tab::Discord:
        if (model.discord)
        {
            const Discord& discord = *model.discord;
            if (const std::optional<bool> on = OnOff(flow, "presence", "Show on Discord", discord.on,
                                                     "Your Discord profile shows the game Unishade is running on, with its icon and your preset."))
                host.SetSwitch(Switch::DiscordPresence, *on);
            if (discord.on && !discord.status.empty())
                Note(flow, discord.status,
                     discord.level == Level::Warning ? kCaution
                     : discord.level == Level::Error ? kDanger
                     : discord.level == Level::Ok    ? kInk
                                                     : kMist);
        }
        break;
    case Tab::About:
        Note(flow, "Unishade " + model.version, kInk);
        if (!model.update.empty())
            Note(flow, "Unishade " + model.update + " is available.");
        Links(flow, model, host);
        break;
    }
    ImGui::PopID();
    return flow.Height();
}

// An add-on: its icon, what it is and who made it, and its switch or what it waits for.
void AddonRow(Flow& flow, Host& host, const Addon& addon, size_t index)
{
    std::string description = addon.description;
    std::vector<Action> actions;
    const bool switched = addon.state == AddonState::On || addon.state == AddonState::Off || addon.state == AddonState::Failed;
    switch (addon.state)
    {
    case AddonState::Available: actions = { { addon.error.empty() ? "Install" : "Try again", PillStyle::Black } }; break;
    case AddonState::Installing: actions = { { "Cancel", PillStyle::Text } }; break;
    case AddonState::Pending:
        description = "This isn't an official Unishade add-on. Add-ons run with the same access as Unishade.";
        actions = { { "Don't load", PillStyle::Text }, { "Load", PillStyle::Black } };
        break;
    case AddonState::On:
    case AddonState::Off:
    case AddonState::Failed:
        if (addon.update)
            actions = { { "Update", PillStyle::Well } };
        break;
    }
    // Its version and who made it, as it says itself, and its folder unless it is one of Unishade's.
    std::string byline = addon.version;
    for (const std::string& part : { addon.author, addon.official ? std::string() : addon.folder })
        if (!part.empty())
            byline += (byline.empty() ? "" : " \xC2\xB7 ") + part;
    const bool installing = addon.state == AddonState::Installing;
    const float gap = S(6.4f), progressWidth = S(96);
    float controls = installing ? progressWidth + gap : 0;
    for (const Action& action : actions)
        controls += PillWidth(action.label, action.style) + gap;
    controls += switched ? S(kSwitchWidth) : -gap;
    const float pad = S(17.6f), vertical = S(14.4f), icon = S(44), iconGap = S(12.8f);
    const float room = flow.width - pad * 2 - icon - iconGap;
    const bool beside = room - controls - S(12.8f) >= S(200);
    const float textWidth = beside ? room - controls - S(12.8f) : room;
    const std::string error = installing ? std::string() : addon.error;
    const float titleHeight = Measure(addon.name, kTextBody, true, textWidth).y;
    const float bylineHeight = byline.empty() ? 0 : S(2) + Measure(byline, kTextSmall, false, textWidth).y;
    const float descriptionHeight = description.empty() ? 0 : S(2) + Measure(description, kTextSmall, false, textWidth).y;
    const float errorHeight = error.empty() ? 0 : S(4.8f) + Measure(error, kTextSmall, false, textWidth).y;
    const float text = std::max(titleHeight + bylineHeight + descriptionHeight + errorHeight, icon);
    const float inside = beside ? std::max(text, S(kButton)) : text + S(9.6f) + S(kButton);
    const ImRect at = flow.Next(inside + vertical * 2, 0);

    ImGui::PushID(addon.id.c_str());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
    const float top = at.Min.y + vertical + (beside ? (inside - text) / 2 : 0);
    GameSquare(draw, ImVec2(at.Min.x + pad, top), icon, S(11.2f), addon.name, addon.icon, addon.id);
    const float left = at.Min.x + pad + icon + iconGap, right = left + textWidth;
    float y = top + std::max(0.0f, (icon - titleHeight - bylineHeight - descriptionHeight - errorHeight) / 2);
    y += Paragraph(draw, ImVec2(left, y), right, addon.name, kInk, kTextBody, true);
    if (!byline.empty())
        y += S(2) + Paragraph(draw, ImVec2(left, y + S(2)), right, byline, kMist, kTextSmall);
    if (!description.empty())
        y += S(2) + Paragraph(draw, ImVec2(left, y + S(2)), right, description, kMist, kTextSmall);
    if (!error.empty())
        Paragraph(draw, ImVec2(left, y + S(4.8f)), right, error, kDanger, kTextSmall);

    float x = beside ? at.Max.x - pad - controls : left;
    const float buttons = beside ? at.Min.y + (at.GetHeight() - S(kButton)) / 2 : at.Max.y - vertical - S(kButton);
    if (installing)
    {
        // How far it is, in the ring's colors across a black pill.
        const ImVec2 barMin(x, buttons), barMax(x + progressWidth, buttons + S(kButton));
        draw->AddRectFilled(barMin, barMax, IM_COL32_BLACK, S(kButton) / 2);
        const float progress = std::clamp(Animate(ImGui::GetID("progress"), addon.progress, 0.2f, Curve::Linear), 0.0f, 1.0f);
        RainbowFill(draw, barMin, barMax, S(kButton) / 2, progress);
        Line(draw, barMin.x, barMax.x, (barMin.y + barMax.y) / 2, std::to_string(static_cast<int>(std::lround(addon.progress * 100))) + "%", IM_COL32_WHITE,
             kTextBody, true, true);
        x += progressWidth + gap;
    }
    for (size_t i = 0; i < actions.size(); ++i)
    {
        if (PillButton(actions[i].label, ImVec2(x, buttons), S(kButton), actions[i].style))
        {
            if (addon.state == AddonState::Available || switched)
                host.InstallAddon(index);
            else if (installing)
                host.CancelInstall(index);
            else
                host.LoadAddon(index, i == 1);
        }
        x += PillWidth(actions[i].label, actions[i].style) + gap;
    }
    if (switched)
    {
        const bool on = addon.state == AddonState::On;
        const bool clicked = SwitchButton("switch", ImVec2(x, buttons + (S(kButton) - S(kSwitchHeight)) / 2), on);
        if (addon.state == AddonState::Failed)
            Tip("Turn on to try again");
        if (clicked)
            host.SetAddon(index, !on);
    }
    ImGui::PopID();
}

// Official add-ons, then the others in the Addons folder. Their settings are in the tray's More, since each preset
// keeps them.
float AddonsBody(const Model& model, Host& host, ImVec2 min, float width)
{
    Flow flow(min, width, kWhole);
    if (!model.addons || model.addons->empty())
        Note(flow, "No add-ons yet.");
    else
        for (size_t i = 0; i < model.addons->size(); ++i)
            AddonRow(flow, host, (*model.addons)[i], i);
    return flow.Height();
}

// A notice's words one by one, so the web addresses in it can be links. Draws it unless draw is null. Returns its
// height.
float NoticeText(ImDrawList* draw, Host& host, ImVec2 at, float right, const std::string& text, int index)
{
    PushSize(kTextBody);
    const float space = ImGui::CalcTextSize(" ").x;
    const float line = ImGui::GetTextLineHeight();
    float x = at.x, top = at.y;
    int link = 0;
    for (size_t start = 0; start < text.size();)
    {
        const size_t end = std::min(text.find_first_of(" \n", start), text.size());
        if (std::string word = text.substr(start, end - start); !word.empty())
        {
            const std::string address = word.substr(0, WebAddressLengthOf(std::string_view(word)));
            word.erase(0, address.size());
            const float addressWidth = address.empty() ? 0 : ImGui::CalcTextSize(address.c_str()).x;
            const float wordWidth = addressWidth + (word.empty() ? 0 : ImGui::CalcTextSize(word.c_str()).x);
            if (x > at.x && x + wordWidth > right)
            {
                x = at.x;
                top += line;
            }
            if (draw && !address.empty())
            {
                ImGui::PushID(index * 64 + link++);
                ImGui::SetCursorScreenPos(ImVec2(x, top));
                if (ImGui::InvisibleButton("link", ImVec2(std::max(addressWidth, 1.0f), line)))
                    host.OpenUrl(address);
                const bool hovered = ImGui::IsItemHovered();
                HandOnHover();
                draw->AddText(ImVec2(x, top), kInk, address.c_str());
                draw->AddLine(ImVec2(x, top + line - S(2)), ImVec2(x + addressWidth, top + line - S(2)), hovered ? kInk : kFaint, S(1.5f));
                ImGui::PopID();
            }
            if (draw && !word.empty())
                draw->AddText(ImVec2(x + addressWidth, top), kInk, word.c_str());
            x += wordWidth + space;
        }
        if (end < text.size() && text[end] == '\n')
        {
            x = at.x;
            top += line;
        }
        start = end + 1;
    }
    ImGui::PopFont();
    return top + line - at.y;
}

// What needs the user, such as an update, then the notices, newest first.
float MessagesBody(const Model& model, Host& host, ImVec2 min, float width)
{
    Flow flow(min, width, kWhole);
    if (model.cards.empty() && model.notices.empty())
        Note(flow, "No messages.");
    const float pad = S(17.6f), vertical = S(14.4f), gap = S(6.4f);
    for (size_t i = 0; i < model.cards.size(); ++i)
    {
        const Card& card = model.cards[i];
        float buttons = -gap;
        for (size_t button = 0; button < card.buttons.size(); ++button)
            buttons += PillWidth(card.buttons[button].c_str(), button == 0 ? PillStyle::Black : PillStyle::Text) + gap;
        buttons = std::max(buttons, 0.0f);
        const bool beside = card.buttons.empty() || width - buttons - pad * 2 - S(12.8f) >= S(220);
        const float textWidth = beside ? width - pad * 2 - (card.buttons.empty() ? 0 : buttons + S(12.8f)) : width - pad * 2;
        const float titleHeight = Measure(card.title, kTextBody, true, textWidth).y;
        const float textHeight = card.text.empty() ? 0 : S(2) + Measure(card.text, kTextSmall, false, textWidth).y;
        const float barHeight = card.progress >= 0 ? S(9.6f) + S(4.48f) : 0;
        const float text = titleHeight + textHeight + barHeight;
        const float controls = card.buttons.empty() ? 0 : S(kButton);
        const float inside = beside ? std::max(text, controls) : text + S(9.6f) + controls;
        const ImRect at = flow.Next(inside + vertical * 2, 0);
        ImGui::PushID(static_cast<int>(i));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
        if (card.attention)
        {
            const float ring = S(2.4f);
            draw->AddRect(at.Min + ImVec2(ring, ring) * 0.5f, at.Max - ImVec2(ring, ring) * 0.5f, kInk, S(kRowRounding) - ring / 2, ring);
        }
        const float left = at.Min.x + pad;
        float y = at.Min.y + vertical + (beside ? (inside - text) / 2 : 0);
        y += Paragraph(draw, ImVec2(left, y), left + textWidth, card.title, kInk, kTextBody, true);
        if (!card.text.empty())
            y += S(2) + Paragraph(draw, ImVec2(left, y + S(2)), left + textWidth, card.text, kMist, kTextSmall);
        if (card.progress >= 0)
        {
            const ImVec2 barMin(left, y + S(9.6f)), barMax(left + textWidth, y + S(9.6f) + S(4.48f));
            draw->AddRectFilled(barMin, barMax, kWell, S(2.24f));
            RainbowLine(draw, barMin, barMax, std::clamp(Animate(ImGui::GetID("progress"), card.progress, 0.2f, Curve::Linear), 0.0f, 1.0f), 1);
        }
        float x = beside ? at.Max.x - pad - buttons : left;
        const float top = beside ? at.Min.y + (at.GetHeight() - controls) / 2 : at.Max.y - vertical - controls;
        for (size_t button = 0; button < card.buttons.size(); ++button)
        {
            const PillStyle style = button == 0 ? PillStyle::Black : PillStyle::Text;
            const std::string label = card.buttons[button] + "##" + std::to_string(button);
            if (PillButton(label.c_str(), ImVec2(x, top), S(kButton), style))
                host.PressCard(i, button);
            x += PillWidth(label.c_str(), style) + gap;
        }
        ImGui::PopID();
    }
    for (size_t i = model.notices.size(); i-- > 0;)
    {
        const Notice& notice = model.notices[i];
        const float textLeft = pad + S(20);
        const float textHeight = NoticeText(nullptr, host, ImVec2(0, 0), width - textLeft - pad, notice.text, static_cast<int>(i));
        const ImRect at = flow.Next(textHeight + vertical * 2, 0);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at.Min, at.Max, kWell, S(kRowRounding));
        const ImU32 color = notice.level == Level::Ok        ? kGo
                            : notice.level == Level::Warning ? kCaution
                            : notice.level == Level::Error   ? kDanger
                                                             : kQuiet;
        draw->AddCircleFilled(ImVec2(at.Min.x + pad + S(5), at.Min.y + vertical + Measure("A", kTextBody).y / 2), S(5), color);
        ImGui::PushID("notice");
        NoticeText(draw, host, ImVec2(at.Min.x + textLeft, at.Min.y + vertical), at.Max.x - pad, notice.text, static_cast<int>(i));
        ImGui::PopID();
    }
    return flow.Height();
}

// The open windows, to add a game by or to use for now. Adding a game, the games installed and the way to a game's
// file are under them, so every way to add one is on the one page.
float PickerBody(const Picker& picker, Host& host, ImVec2 min, float width)
{
    Flow flow(min, width, kCardWidth);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // A window or a game: its icon, its name and a line under it. Returns whether it was clicked.
    const auto row = [&](const char* id, const std::string& name, const std::string& detail, const std::string& picture) {
        const ImRect at = flow.Next(S(64));
        ImGui::SetCursorScreenPos(at.Min);
        const bool clicked = ImGui::InvisibleButton(id, at.GetSize(), ImGuiButtonFlags_EnableNav);
        const ImGuiID key = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(at.Min, at.Max, hovered ? kWellHover : kWell, S(kRowRounding));
        const float icon = S(36);
        const ImVec2 iconMin(at.Min.x + S(14), at.GetCenter().y - icon / 2);
        GameSquare(draw, iconMin, icon, S(9.6f), name, picture, name);
        const float left = iconMin.x + icon + S(12);
        Line(draw, left, at.Max.x - S(14), at.GetCenter().y - (detail.empty() ? 0 : S(9)), name, kInk, kTextBody, true);
        if (!detail.empty())
            Line(draw, left, at.Max.x - S(14), at.GetCenter().y + S(11), detail, kMist, kTextSmall);
        Transform(draw, start, at.GetCenter(), Animate(SubId(key, "press"), held ? 0.96f : 1.0f, 0.25f, Curve::Spring));
        return clicked;
    };

    const bool more = picker.add && (picker.browse || !picker.installed.empty());
    if (more)
        Heading(flow, "Open now");
    if (picker.windows.empty())
        Note(flow, "No open windows.");
    for (size_t i = 0; i < picker.windows.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        if (row("window", picker.windows[i].title, picker.windows[i].detail, picker.windows[i].picture))
            host.ChooseWindow(i);
        ImGui::PopID();
    }
    if (!more)
        return flow.Height();

    flow.Break();
    Heading(flow, picker.installed.empty() ? "Not open" : "Installed");
    ImGui::PushID("##installed");
    for (size_t i = 0; i < picker.installed.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        if (row("installed", picker.installed[i].name, picker.installed[i].store, picker.installed[i].picture))
            host.AddInstalledGame(i);
        ImGui::PopID();
    }
    ImGui::PopID();
    if (picker.browse)
    {
        const ImRect at = flow.Next(S(64));
        ImGui::SetCursorScreenPos(at.Min);
        const bool clicked = ImGui::InvisibleButton("browse", at.GetSize(), ImGuiButtonFlags_EnableNav);
        const ImGuiID key = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        const int start = draw->VtxBuffer.Size;
        DashedBox(draw, at.Min, at.Max, S(kRowRounding), hovered ? Alpha(kInk, 0.5f) : kFaint, S(2.4f));
        Line(draw, at.Min.x + S(12), at.Max.x - S(12), at.GetCenter().y, "Choose a file", hovered ? kInk : kMist, kTextBody, true, true);
        Transform(draw, start, at.GetCenter(), Animate(SubId(key, "press"), held ? 0.96f : 1.0f, 0.25f, Curve::Spring));
        if (clicked)
            host.BrowseForGame();
    }
    return flow.Height();
}

// The games found installed, each picked or not with a click.
float FoundBody(ImVec2 min, float width)
{
    Flow flow(min, width, kCardWidth);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < state->found.size(); ++i)
    {
        const FoundGame& game = state->found[i];
        const bool chosen = state->chosen.count(i) > 0;
        const ImRect at = flow.Next(S(64));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(at.Min);
        const bool clicked = ImGui::InvisibleButton("found", at.GetSize(), ImGuiButtonFlags_EnableNav);
        const ImGuiID key = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        HandOnHover();
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(at.Min, at.Max, hovered ? kWellHover : kWell, S(kRowRounding));
        const float icon = S(36);
        const ImVec2 iconMin(at.Min.x + S(14), at.GetCenter().y - icon / 2);
        GameSquare(draw, iconMin, icon, S(9.6f), game.name, game.picture, game.name);
        const float left = iconMin.x + icon + S(12), right = at.Max.x - S(48);
        Line(draw, left, right, at.GetCenter().y - (game.store.empty() ? 0 : S(9)), game.name, kInk, kTextBody, true);
        if (!game.store.empty())
            Line(draw, left, right, at.GetCenter().y + S(11), game.store, kMist, kTextSmall);
        // A ring that fills with a check once the game is picked.
        const ImVec2 mark(at.Max.x - S(14) - S(12), at.GetCenter().y);
        const float fill = Animate(SubId(key, "fill"), chosen ? 1.0f : 0.0f, 0.25f, Curve::Out);
        draw->AddCircle(mark, S(11), kFaint, 0, S(2));
        if (fill > 0.01f)
        {
            draw->AddCircleFilled(mark, S(12) * fill, kInk);
            DrawIcon(draw, Icon::Check, mark, S(14), Alpha(kOnInk, fill));
        }
        Transform(draw, start, at.GetCenter(), Animate(SubId(key, "press"), held ? 0.96f : 1.0f, 0.25f, Curve::Spring));
        if (clicked && !state->chosen.erase(i))
            state->chosen.insert(i);
        ImGui::PopID();
    }
    return flow.Height();
}

// The games, as on the window's shelf, with what can be set for the one picked on it.
float GamesBody(const Model& model, Host& host, ImVec2 min, float width)
{
    float y = 0;
    if (StatusShown(model))
        y += StatusBlock(model, host, min, width, kWell) + S(14.4f);
    Shelf(model, host, min + ImVec2(0, y), width, false);
    y += S(kShelfRow) + S(9.6f);
    if (const std::optional<size_t> index = IndexOf(model, state->game))
        y += GameBody(model, host, *index, min + ImVec2(0, y), width, false);
    return y;
}

// The modal's title and buttons.
void ModalHeader(const Model& model, Host& host, ImVec2 min, float width, float& height)
{
    switch (state->shown)
    {
    case Modal::Games:
        if (Header(min, width, "Games", "", { { "Done", PillStyle::Black } }, height) == 0)
            CloseModal();
        break;
    case Modal::Settings: SettingsHeader(model, host, min, width, height); break;
    case Modal::Addons:
        switch (Header(min, width, "Add-ons", "", { { "Open folder", PillStyle::Text }, { "Done", PillStyle::Black } }, height))
        {
        case 0: host.OpenAddonsFolder(); break;
        case 1: CloseModal(); break;
        }
        break;
    case Modal::Messages:
        if (model.notices.empty())
        {
            if (Header(min, width, "Messages", "", { { "Done", PillStyle::Black } }, height) == 0)
                CloseModal();
        }
        else
            switch (Header(min, width, "Messages", "", { { "Clear all", PillStyle::Text }, { "Done", PillStyle::Black } }, height))
            {
            case 0: host.DismissNotices(); break;
            case 1: CloseModal(); break;
            }
        break;
    case Modal::Picker:
    {
        const bool add = state->picker && state->picker->add;
        // Adding from the games installed leaves the picker open for the next one.
        const bool more = add && (state->picker->browse || !state->picker->installed.empty());
        if (Header(min, width, add ? "Add a game" : "Pick a window",
                   more  ? ""
                   : add ? "Pick your game's window. If it isn't here, open the game first."
                         : "Unishade uses this window until you click Detect automatically.",
                   { { more ? "Done" : "Cancel", PillStyle::Text } }, height) == 0)
            host.ClosePicker();
        break;
    }
    case Modal::Found:
    {
        const int clicked = Header(min, width, "Pick your games",
                                   "Unishade found these on this PC, and turns on when one you pick starts. The + adds any other game later.",
                                   { { "Skip", PillStyle::Text }, { "Add", PillStyle::Black, !state->chosen.empty() } }, height);
        if (clicked >= 0)
        {
            host.AddFoundGames(clicked == 1 ? std::vector<size_t>(state->chosen.begin(), state->chosen.end()) : std::vector<size_t>());
            state->chosen.clear();
        }
        break;
    }
    case Modal::Question:
    {
        int clicked = -1;
        if (state->question == Question::DeletePreset)
        {
#ifdef _WIN32
            constexpr char kTrash[] = "It moves to the Recycle Bin.";
#else
            constexpr char kTrash[] = "It moves to the Trash.";
#endif
            clicked = Header(min, width, "Delete " + PresetName(model, state->deleting) + "?", kTrash,
                             { { "Cancel", PillStyle::Text }, { "Delete", PillStyle::Black } }, height);
            if (clicked == 1)
                host.DeletePreset(state->deleting);
        }
        else
        {
            clicked = Header(min, width, "Reset every shortcut?", "They go back to the keys Unishade starts with.",
                             { { "Cancel", PillStyle::Text }, { "Reset", PillStyle::Black } }, height);
            if (clicked == 1)
                host.ResetShortcuts();
        }
        if (clicked >= 0)
            state->modal = state->back;
        break;
    }
    case Modal::Quit:
        // Unishade quits once the changes to the preset in use are saved or dropped.
        switch (Header(min, width, "Save the changes to " + state->quitting + " before quitting?", "Without saving, they are lost.",
                       { { "Don't save", PillStyle::Text }, { "Cancel", PillStyle::Text }, { "Save", PillStyle::Black } }, height))
        {
        case 0: host.Quit(false); break;
        case 1: host.KeepRunning(); break;
        case 2: host.Quit(true); break;
        }
        break;
    case Modal::None: break;
    }
}

float ModalBody(const Model& model, Host& host, ImVec2 min, float width)
{
    switch (state->shown)
    {
    case Modal::Games: return GamesBody(model, host, min, width);
    case Modal::Settings: return SettingsBody(model, host, min, width);
    case Modal::Addons: return AddonsBody(model, host, min, width);
    case Modal::Messages: return MessagesBody(model, host, min, width);
    case Modal::Picker: return state->picker ? PickerBody(*state->picker, host, min, width) : 0;
    case Modal::Found: return FoundBody(min, width);
    case Modal::Question:
    case Modal::Quit:
    case Modal::None: break;
    }
    return 0;
}

// What the user opened, or what the host asks: the picker, and over the window the question before quitting.
Modal Wanted(const Model& model)
{
    if (place == Place::Game && model.closing)
        return Modal::None;
    if (place == Place::Window && !model.quitting.empty())
        return Modal::Quit;
    if (model.picker)
        return Modal::Picker;
    // The first thing a new user sees, in the launcher's window.
    if (place == Place::Window && !model.found.empty())
        return Modal::Found;
    return state->modal;
}

// The modal over everything else, on a dark backdrop: it rises in a little bigger with the spring and fades away.
// Clicking the backdrop or Escape closes it.
void Modals(const Model& model, Host& host)
{
    const Modal want = Wanted(model);
    if (model.picker)
        state->picker = *model.picker;
    if (!model.found.empty())
        state->found = model.found;
    if (!model.quitting.empty())
        state->quitting = model.quitting;
    // A shortcut left waiting for keys when its page went would take the next keys pressed.
    if (model.settings.recording < 0)
        state->recordingHere = false;
    else if (state->recordingHere && !(want == Modal::Settings && state->tab == Tab::Shortcuts))
    {
        host.RecordShortcut(-1);
        state->recordingHere = false;
    }

    const bool open = want != Modal::None;
    if (open)
    {
        if (!state->open)
        {
            Snap(Key("modal fade"), 0);
            Snap(Key("modal lift"), 1);
            state->focus = true;
        }
        state->shown = want;
    }
    state->open = open;
    if (state->shown == Modal::None)
        return;
    const float fade = Animate(Key("modal fade"), open ? 1.0f : 0.0f, 0.25f, Curve::Out);
    const float lift = Animate(Key("modal lift"), open ? 0.0f : 1.0f, 0.5f, Curve::Spring);
    if (!open && fade < 0.01f)
    {
        state->shown = Modal::None;
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float s = kit::scale;
    const float width = std::min(S(kPanelWidth), display.x * 0.9f);
    const float inner = width - S(kPanelPad) * 2;
    const bool hasBody = state->shown != Modal::Question && state->shown != Modal::Quit;
    const float header = state->header * s;
    const float most = std::max(display.y * 0.86f - header - S(kPanelPad) * 2 - S(14.4f), S(80));
    // Settings is as high on every tab, so nothing moves when the tab changes, and a longer tab scrolls. The others
    // are as high as their contents. Contents not measured yet get all the room for a frame, while the modal is still
    // on its way in.
    const float body = !hasBody                           ? 0
                       : state->shown == Modal::Settings  ? std::min(S(kSettingsBody), most)
                       : state->measured == state->shown ? std::min(state->body * s, most)
                                                         : most;
    const float height = S(kPanelPad) * 2 + header + (hasBody ? S(14.4f) + body : 0);
    const ImVec2 min(std::round((display.x - width) / 2), std::round((display.y - height) / 2));
    const ImVec2 max = min + ImVec2(width, height);
    // A small window can bring the panel up into the title bar.
    if (place == Place::Window && open)
        caption::bar.controls.push_back({ min.x, min.y, max.x, max.y });

    if (std::exchange(state->focus, false))
        ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##modal", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollWithMouse | (open ? 0 : ImGuiWindowFlags_NoInputs));
    ImGui::PopStyleVar(3);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* draw = window->DrawList;
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 153));
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, kPaper, S(kBlockRounding));
    draw->AddRect(min + ImVec2(0.5f, 0.5f), max - ImVec2(0.5f, 0.5f), kEdge, S(kBlockRounding), 1.0f);

    const ImVec2 inside = min + ImVec2(S(kPanelPad), S(kPanelPad));
    float headerHeight = 0;
    ModalHeader(model, host, inside, inner, headerHeight);
    state->header = headerHeight / s;
    ImGuiWindow* bodyWindow = nullptr;
    if (hasBody)
    {
        ImGui::SetCursorScreenPos(inside + ImVec2(0, header + S(14.4f)));
        ImGui::BeginChild("##body", ImVec2(inner, std::max(body, 1.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        bodyWindow = ImGui::GetCurrentWindow();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float used = ModalBody(model, host, origin, inner);
        ImGui::SetCursorScreenPos(origin + ImVec2(0, used));
        ImGui::Dummy(ImVec2(1, 0));
        ImGui::EndChild();
        ScrollCue(bodyWindow);
        state->body = used / s;
        state->measured = state->shown;
    }
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(0, 0));

    const bool outside = open && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImRect(min, max).Contains(ImGui::GetIO().MousePos);
    const bool escape = open && model.settings.recording < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
                        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (outside || escape)
        switch (state->shown)
        {
        case Modal::Picker: host.ClosePicker(); break;
        // A stray click would answer for good, so only Skip and Add do.
        case Modal::Found: break;
        case Modal::Quit: host.KeepRunning(); break;
        case Modal::Question: state->modal = state->back; break;
        default: CloseModal(); break;
        }

    // From a little below and smaller, as the design's translateY(2rem) scale(.92).
    const ImVec2 pivot = (min + max) * 0.5f;
    const float grow = 1 - 0.08f * lift;
    const ImVec2 offset(0, S(32) * lift);
    Transform(draw, start, pivot, grow, 0, offset);
    MoveWindow(bodyWindow, pivot, grow, offset);
    FadeWindow(window, fade);
    ImGui::End();
}
} // namespace

bool Draw(const Model& model, Host& host, float windowScale, Place where)
{
    spectrum::SetDark(model.settings.darkMenu);
    place = where;
    state = &states[static_cast<size_t>(where)];
    if (where == Place::Game)
    {
        // The tray is the menu. The launcher's own windows open over it from its More.
        const bool covered = state->open || state->modal != Modal::None || model.picker.has_value();
        const tray::Open asked = tray::DrawOverGame(model, host, windowScale, covered);
        scale = windowScale;
        Follow(model);
        switch (asked)
        {
        case tray::Open::Games: state->modal = Modal::Games; break;
        case tray::Open::Settings: state->modal = Modal::Settings; break;
        case tray::Open::Addons: state->modal = Modal::Addons; break;
        case tray::Open::Messages: state->modal = Modal::Messages; break;
        case tray::Open::None: break;
        }
        const ImGuiStyle saved = ImGui::GetStyle();
        spectrum::ApplyStyle(ImGui::GetStyle());
        PushSize(kTextBody);
        Modals(model, host);
        ImGui::PopFont();
        ImGui::GetStyle() = saved;
        return !model.closing || !tray::Gone() || state->shown != Modal::None;
    }
    scale = windowScale;
    spectrum::ApplyStyle(ImGui::GetStyle());
    Follow(model);
    PushSize(kTextBody);
    WindowView(model, host);
    Modals(model, host);
    ImGui::PopFont();
    return true;
}

void Reset(Place where)
{
    if (where == Place::Game)
        tray::Reset();
    states[static_cast<size_t>(where)] = State{};
}
} // namespace launcher
