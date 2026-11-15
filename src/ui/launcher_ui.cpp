// The launcher's design, the same on every platform. See launcher_ui.h.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "launcher_ui.h"
#include "kit.h"
#include "web_address.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>
#include <utility>

namespace launcher
{
namespace
{
using namespace kit;

constexpr float kStrip = 3;
constexpr float kSidebarWidth = 264;
constexpr float kPagePadding = 40;
constexpr float kPageWidth = 900;
// Over the game the launcher is a panel, with a rail of icons for the sidebar and the menu's buttons at the bottom.
// Its text is a fifth smaller than in the launcher's window, as the menu's was.
constexpr float kPanelScale = 0.8f;
constexpr float kPanelWidth = 640;
constexpr float kPanelMargin = 20;
constexpr float kPanelPadding = 24;
constexpr float kRailWidth = 64;
constexpr float kFooter = 76;
// The picker scrolls past its height.
constexpr float kPickerWidth = 520;
constexpr float kPickerListHeight = 420;
// Text sizes besides kit's.
constexpr float kHeroTitle = 25 * 1.33f;
constexpr float kPageTitle = 20 * 1.33f;
constexpr float kHeading = 12;
// The frame rate choice that is typed in a box.
constexpr int kCustom = static_cast<int>(std::size(kFrameRates));

enum class PageKind
{
    Home,
    Game,
    Settings,
    Discord,
};

enum class SettingsPage
{
    General,
    Performance,
    Shortcuts,
};

// What the preset name dialog does, in the order of its titles.
enum class PresetAction
{
    New,
    Copy,
    Rename,
};

struct State
{
    PageKind page = PageKind::Home;
    std::string game; // the id of the game whose page shows
    SettingsPage settingsPage = SettingsPage::General;
    // Every game seen, so one that was just added gets its page.
    std::set<std::string> known;
    bool started = false;

    // Renaming the game whose page shows.
    std::string renaming;
    char name[128] = {};
    bool focusName = false;
    bool renameFailed = false; // shows the model's renameProblem until the name changes

    // The custom frame rate box, for a game by its id, or the defaults with an empty id.
    std::optional<std::string> rateFor;
    char rate[8] = {};
    bool focusRate = false;

    bool confirmReset = false;

    // The dialog that names a preset, for the game whose page shows.
    PresetAction presetAction = PresetAction::New;
    std::string presetTarget; // the preset copied or renamed
    size_t presetGame = 0;
    char presetName[128] = {};
    bool openPresetName = false;
    bool presetFailed = false; // shows the model's presetProblem until the name changes
    bool focusPresetName = false;
    // A preset to switch to once the changes to the one in use are saved or discarded.
    std::string pendingPreset;
    size_t pendingGame = 0;
    bool openUnsaved = false;
    std::string deleting; // the preset the delete dialog asks about
    bool openDelete = false;

    // The effects of the game being played.
    char search[128] = {};
    std::string expanded;    // the effect whose settings show, by key
    std::string shownErrors; // the effect file whose compiler errors show
    bool showAll = false;
    // The effects listed under Active since the page showed or the preset changed, by key. An effect turned off stays
    // there, so it can be turned back on where it was.
    std::set<std::string> active;
    std::string activePreset;
    // The rows, made again when the list, the search, or which effects are on changes.
    std::vector<size_t> activeRows; // in the order they run
    std::vector<size_t> otherRows;  // by name
    unsigned rowsRevision = 0;
    std::string rowsSearch;
    std::vector<bool> rowsOn;
    bool rowsStale = true;
    // Reset all asks first.
    std::string resetting; // the effect's key
    bool openResetEffect = false;
};
State states[2];
// The place being drawn, and its state.
Place place = Place::Window;
State* state = &states[0];

// Where a page is drawn. It scrolls, so places are given from its top left corner.
struct Page
{
    ImDrawList* draw;
    ImVec2 origin;
    float left;
    float right;

    ImVec2 At(float x, float y) const
    {
        return origin + ImVec2(x, y);
    }
};

ImU32 Fade(ImU32 color, int alpha)
{
    return (color & 0x00FFFFFF) | (static_cast<ImU32>(alpha) << 24);
}

// Icons, drawn around center for a 16 pixel box.

void HomeIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    const float t = S(1.5f);
    const ImVec2 roof[] = { center + ImVec2(-S(7), -S(0.5f)), center + ImVec2(0, -S(7)), center + ImVec2(S(7), -S(0.5f)) };
    draw->AddPolyline(roof, 3, color, 0, t);
    const ImVec2 body[] = { center + ImVec2(-S(5), -S(2)), center + ImVec2(-S(5), S(6)), center + ImVec2(S(5), S(6)), center + ImVec2(S(5), -S(2)) };
    draw->AddPolyline(body, 4, color, 0, t);
}

void SettingsIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    // Three sliders.
    const float t = S(1.5f);
    const float knobs[] = { -S(2.5f), S(3), -S(0.5f) };
    for (int i = 0; i < 3; ++i)
    {
        const float y = center.y + S(5) * static_cast<float>(i - 1);
        draw->AddLine(ImVec2(center.x - S(7), y), ImVec2(center.x + S(7), y), color, t);
        draw->AddCircleFilled(ImVec2(center.x + knobs[i], y), S(2.4f), color);
    }
}

void DiscordIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    // A speech bubble.
    const float t = S(1.5f);
    draw->AddRect(center + ImVec2(-S(7), -S(6)), center + ImVec2(S(7), S(4)), color, S(3), 0, t);
    const ImVec2 tail[] = { center + ImVec2(-S(3), S(4)), center + ImVec2(-S(4), S(7.5f)), center + ImVec2(S(1), S(4)) };
    draw->AddPolyline(tail, 3, color, 0, t);
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(center + ImVec2(S(3.5f) * static_cast<float>(i), -S(1)), S(1.1f), color);
}

// A camera: the body, the bump on top and the lens.
void CameraIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    const float t = S(1.5f);
    draw->AddRect(center + ImVec2(-S(8), -S(5)), center + ImVec2(S(8), S(6)), color, S(2), t);
    draw->AddRectFilled(center + ImVec2(-S(3), -S(7.5f)), center + ImVec2(S(3), -S(5)), color, S(1));
    draw->AddCircle(center + ImVec2(0, S(0.5f)), S(3.2f), color, 0, t);
}

// A floppy disk: an outline with one corner cut, the shutter at the top and the label below.
void SaveIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    const float size = S(15);
    const ImVec2 min = center - ImVec2(size, size) * 0.5f;
    const ImVec2 max = center + ImVec2(size, size) * 0.5f;
    const float corner = size * 0.25f;
    const ImVec2 outline[] = { min, ImVec2(max.x - corner, min.y), ImVec2(max.x, min.y + corner), max, ImVec2(min.x, max.y) };
    draw->AddPolyline(outline, 5, color, ImDrawFlags_Closed, S(1.5f));
    draw->AddRectFilled(ImVec2(min.x + size * 0.25f, min.y), ImVec2(max.x - size * 0.35f, min.y + size * 0.3f), color);
    draw->AddRect(ImVec2(min.x + size * 0.2f, center.y + size * 0.1f), ImVec2(max.x - size * 0.2f, max.y), color, 0, S(1.5f));
}

void PlusIcon(ImDrawList* draw, ImVec2 center, ImU32 color, float radius)
{
    draw->AddLine(center - ImVec2(radius, 0), center + ImVec2(radius, 0), color, S(1.6f));
    draw->AddLine(center - ImVec2(0, radius), center + ImVec2(0, radius), color, S(1.6f));
}

void PencilIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    // From the tip at the bottom left to the end at the top right, with a band below the end.
    const ImVec2 outline[] = { center + ImVec2(-S(5), S(5)), center + ImVec2(-S(4.5f), S(2.5f)), center + ImVec2(S(2.5f), -S(4.5f)),
                               center + ImVec2(S(4.5f), -S(2.5f)), center + ImVec2(-S(2.5f), S(4.5f)) };
    draw->AddPolyline(outline, 5, color, ImDrawFlags_Closed, S(1.4f));
    draw->AddLine(center + ImVec2(S(1), -S(3)), center + ImVec2(S(3), -S(1)), color, S(1.4f));
}

void CrossIcon(ImDrawList* draw, ImVec2 center, ImU32 color)
{
    const float s = S(4.5f);
    draw->AddLine(center + ImVec2(-s, -s), center + ImVec2(s, s), color, S(1.6f));
    draw->AddLine(center + ImVec2(s, -s), center + ImVec2(-s, s), color, S(1.6f));
}

// A round button with an icon, which shows its tip once the mouse rests on it.
bool IconButton(const char* id, ImVec2 min, float size, void (*icon)(ImDrawList*, ImVec2, ImU32), const char* tip, bool danger = false)
{
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    Tooltip(tip);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = min + ImVec2(size, size) * 0.5f;
    if (hovered)
        draw->AddCircleFilled(center, size / 2, danger ? Color(theme::kError, 0x24) : kBorder);
    icon(draw, center, hovered ? (danger ? kError : kText) : kDim);
    return clicked;
}

// The game's picture, or its name's first letter on a tile. Faded with the color behind it while turned off.
void GamePicture(ImDrawList* draw, ImVec2 min, float size, const std::string& name, const std::string& picture, float letterSize, float rounding,
                 ImU32 fade = 0)
{
    const ImVec2 max = min + ImVec2(size, size);
    if (const ImTextureID texture = picture.empty() ? ImTextureID_Invalid : Picture(picture, size); texture != ImTextureID_Invalid)
        draw->AddImageRounded(ImTextureRef(texture), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
    else
    {
        draw->AddRectFilled(min, max, kCardHover, rounding);
        draw->AddRect(min, max, kBorder, rounding);
        Line(draw, min.x, max.x, (min.y + max.y) / 2, Initial(name), kText, letterSize, true, true);
    }
    if (fade)
        draw->AddRectFilled(min, max, fade, rounding);
}

// A small rounded label, such as Running.
float Pill(ImDrawList* draw, ImVec2 min, const std::string& text, ImU32 color)
{
    const float width = Measure(text, kNote).x + S(18);
    draw->AddRectFilled(min, min + ImVec2(width, S(22)), Fade(color, 0x26), S(11));
    Line(draw, min.x, min.x + width, min.y + S(11), text, color, kNote, false, true);
    return width;
}

// A heading above a section, with a link on the right when given. Returns true when the link is clicked.
bool SectionHeading(const Page& page, float& y, const char* text, const char* link = nullptr)
{
    Line(page.draw, page.origin.x + page.left, page.origin.x + page.right, page.origin.y + y + S(10), text, kDim, kHeading, true);
    bool clicked = false;
    if (link)
        clicked = PlacedLink(link, page.At(page.right - Measure(link, kBody).x, y), S(20));
    y += S(20) + S(10);
    return clicked;
}

// A card around what is drawn between BeginCard and EndCard, which tell its height.
void BeginCard(const Page& page)
{
    page.draw->ChannelsSplit(2);
    page.draw->ChannelsSetCurrent(1);
}

void EndCard(const Page& page, float top, float bottom, ImU32 fill = kCard, ImU32 border = kBorder)
{
    page.draw->ChannelsSetCurrent(0);
    RoundedCard(page.draw, page.At(page.left, top), page.At(page.right, bottom), fill, border);
    page.draw->ChannelsMerge();
}

void RowSeparator(const Page& page, float y)
{
    page.draw->AddLine(page.At(page.left + S(20), y), page.At(page.right - S(20), y), kBorder);
}

// A setting's title and description between left and right. Returns their height.
float SettingText(const Page& page, float left, float right, float y, const std::string& title, const std::string& description)
{
    float height = Paragraph(page.draw, page.At(left, y), page.origin.x + right, title, kText, kBody);
    if (!description.empty())
        height += S(3) + Paragraph(page.draw, page.At(left, y + height + S(3)), page.origin.x + right, description, kDim, kNote);
    return height;
}

// A card row with a switch on the right. Returns true when the switch is clicked.
bool SwitchRow(const Page& page, float& y, const char* id, bool on, const std::string& title, const std::string& description)
{
    const float pad = S(20);
    const ImVec2 size(S(40), S(22));
    const float textHeight = SettingText(page, page.left + pad, page.right - pad - size.x - S(20), y + S(16), title, description);
    const float height = std::max(textHeight, size.y) + S(32);
    ImGui::SetCursorScreenPos(page.At(page.right - pad - size.x, y + (height - size.y) / 2));
    const bool clicked = kit::Switch(id, on, size);
    y += height;
    return clicked;
}

// A card row with a button on the right. Returns true when the button is clicked.
bool ButtonRow(const Page& page, float& y, const char* label, bool enabled, const std::string& title, const std::string& description)
{
    const float pad = S(20);
    std::string text = label;
    const ImVec2 size(Measure(text.substr(0, text.find("##")), kBody).x + S(32), S(32));
    const float textHeight = SettingText(page, page.left + pad, page.right - pad - size.x - S(20), y + S(16), title, description);
    const float height = std::max(textHeight, size.y) + S(32);
    const bool clicked = PlacedButton(label, page.At(page.right - pad - size.x, y + (height - size.y) / 2), size, false, enabled);
    y += height;
    return clicked;
}

// The value the choices show as selected: the place of value among them, or past the last for one that is none.
template <size_t N>
int ChoiceOf(const int (&values)[N], int value)
{
    return static_cast<int>(std::find(std::begin(values), std::end(values), value) - std::begin(values));
}

// A card row with choices under its text. Returns the index of the choice clicked, or -1.
int ChoicesRow(const Page& page, float& y, const char* id, const std::string& title, const std::string& description,
               std::initializer_list<const char*> choices, int selected)
{
    const float pad = S(20);
    y += S(16);
    y += SettingText(page, page.left + pad, page.right - pad, y, title, description) + S(12);
    ImGui::SetCursorScreenPos(page.At(page.left + pad, y));
    const int clicked = Segmented(id, choices, selected, kAccent, std::min(page.right - page.left - pad * 2, S(520)));
    y += S(30) + S(18);
    return clicked;
}

// Frame rate, effect resolution and depth detail, for a game by its index or the defaults.
void PerformanceRows(const Page& page, float& y, Host& host, const Performance& performance, std::optional<size_t> game, const std::string& rateFor)
{
    const float pad = S(20);
    const auto preset = [](int fps) { return ChoiceOf(kFrameRates, fps) < kCustom; };
    const auto showRate = [](int fps) { std::snprintf(state->rate, sizeof(state->rate), "%d", fps ? fps : 60); };
    // A limit that is none of the choices is a custom one, typed in the box, which also shows while Custom is picked.
    if (!preset(performance.frameRate) && state->rateFor != rateFor)
    {
        state->rateFor = rateFor;
        showRate(performance.frameRate);
    }
    const int rate = ChoicesRow(page, y, "rate", "Frame rate", "The most frames a second Unishade shows. A lower limit leaves more of the GPU to the game.",
                                { "Follow game", "120 FPS", "60 FPS", "Custom" },
                                state->rateFor == rateFor ? kCustom : ChoiceOf(kFrameRates, performance.frameRate));
    if (rate >= 0 && rate < kCustom)
    {
        state->rateFor.reset();
        host.SetPerformance(game, PerformanceSetting::FrameRate, kFrameRates[rate]);
    }
    else if (rate == kCustom && state->rateFor != rateFor)
    {
        state->rateFor = rateFor;
        showRate(performance.frameRate);
        state->focusRate = true;
    }
    if (state->rateFor == rateFor)
    {
        y -= S(6);
        const float width = S(84);
        ImGui::SetCursorScreenPos(page.At(page.left + pad, y));
        ImGui::PushItemWidth(width);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kCardHover);
        ImGui::PushStyleColor(ImGuiCol_Border, kBorderStrong);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(7)));
        PushSize(kBody);
        if (std::exchange(state->focusRate, false))
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##rate", state->rate, sizeof(state->rate), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
        const bool done = ImGui::IsItemDeactivated();
        const bool active = ImGui::IsItemActive();
        const float height = ImGui::GetItemRectSize().y;
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopItemWidth();
        const std::string range = "frames a second, from " + std::to_string(kSlowestFrameRate) + " to " + std::to_string(kFastestFrameRate);
        Line(page.draw, page.origin.x + page.left + pad + width + S(12), page.origin.x + page.right - pad, page.origin.y + y + height / 2, range, kDim, kNote);
        // Leaving the box uses the number in it, and Escape leaves it with the limit as it was. The box goes once the
        // limit is one of the choices again.
        int limit = performance.frameRate;
        if (done && !ImGui::IsKeyPressed(ImGuiKey_Escape))
            if (const int value = std::atoi(state->rate); value > 0)
            {
                limit = std::clamp(value, kSlowestFrameRate, kFastestFrameRate);
                host.SetPerformance(game, PerformanceSetting::FrameRate, limit);
            }
        if (done)
        {
            showRate(limit);
            if (preset(limit))
                state->rateFor.reset();
        }
        // Custom picked and then left alone.
        else if (!active && preset(limit) && rate < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered())
            state->rateFor.reset();
        y += height + S(18);
    }
    RowSeparator(page, y);

    const int resolution = ChoicesRow(page, y, "resolution", "Effect resolution",
                                      "Effects run on a smaller picture that FSR upscales to the game's size. Lower is faster and softer. The menu gets "
                                      "softer too, and screenshots get smaller.",
                                      { "100%", "75%", "50%" }, ChoiceOf(kEffectResolutions, performance.effectResolution));
    if (resolution >= 0)
        host.SetPerformance(game, PerformanceSetting::EffectResolution, kEffectResolutions[resolution]);
    if (performance.depthSize)
    {
        RowSeparator(page, y);
        const int depth = ChoicesRow(page, y, "depth", "Depth detail",
                                     "Depth is estimated from a smaller picture. Lower is faster, and effects that use depth lose fine detail.",
                                     { "Ultra", "High", "Medium", "Low" }, ChoiceOf(kDepthSizes, *performance.depthSize));
        if (depth >= 0)
            host.SetPerformance(game, PerformanceSetting::DepthSize, kDepthSizes[depth]);
    }
}

// Sidebar

// A row of the sidebar. Returns true when clicked.
bool NavRow(const char* id, ImVec2 min, float width, const std::string& label, void (*icon)(ImDrawList*, ImVec2, ImU32), bool selected, int badge = 0)
{
    const float height = S(38);
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (selected || hovered)
        draw->AddRectFilled(min, min + ImVec2(width, height), selected ? kCard : Fade(kCard, 0x90), S(8));
    if (selected)
        draw->AddRectFilled(min + ImVec2(0, S(10)), min + ImVec2(S(3), height - S(10)), kAccent, S(1.5f));
    const ImU32 color = selected || hovered ? kText : kDim;
    icon(draw, min + ImVec2(S(22), height / 2), color);
    Line(draw, min.x + S(42), min.x + width - S(40), min.y + height / 2, label, color, kBody, selected);
    if (badge)
    {
        const std::string count = std::to_string(badge);
        const float badgeWidth = std::max(Measure(count, kNote).x + S(12), S(20));
        const ImVec2 badgeMin = min + ImVec2(width - S(12) - badgeWidth, height / 2 - S(10));
        draw->AddRectFilled(badgeMin, badgeMin + ImVec2(badgeWidth, S(20)), Fade(kWarning, 0x30), S(10));
        Line(draw, badgeMin.x, badgeMin.x + badgeWidth, badgeMin.y + S(10), count, kWarning, kNote, true, true);
    }
    return clicked;
}

void ShowPage(PageKind page, const std::string& game = {})
{
    state->page = page;
    state->game = game;
    state->renaming.clear();
    state->rateFor.reset();
}

// The pages at the bottom of the sidebar, for every game. An Add-ons page would go here.
struct BottomPage
{
    PageKind page;
    const char* id;
    const char* label;
    void (*icon)(ImDrawList*, ImVec2, ImU32);
};

std::vector<BottomPage> BottomPages(const Model& model)
{
    std::vector<BottomPage> pages{ { PageKind::Settings, "settings", "Settings", SettingsIcon } };
    if (model.discord)
        pages.push_back({ PageKind::Discord, "discord", "Discord", DiscordIcon });
    return pages;
}

int Problems(const Model& model)
{
    return static_cast<int>(std::count_if(model.notices.begin(), model.notices.end(), [](const Notice& notice) { return notice.level >= Level::Warning; }));
}

void Sidebar(const Model& model, Host& host, ImVec2 at, float height)
{
    const float width = S(kSidebarWidth);
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::BeginChild("sidebar", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    draw->AddLine(origin + ImVec2(width - 1, 0), origin + ImVec2(width - 1, height), kBorder);
    const float pad = S(14);
    const float rowWidth = width - pad * 2;

    // The logo, the name and the version.
    float y = S(kStrip) + S(22);
    const float logo = std::round(S(36));
    if (const ImTextureID texture = Picture("logo", logo); texture != ImTextureID_Invalid)
        draw->AddImage(ImTextureRef(texture), origin + ImVec2(S(22), y), origin + ImVec2(S(22) + logo, y + logo));
    const float textX = origin.x + S(22) + logo + S(12);
    Line(draw, textX, origin.x + width - pad, origin.y + y + S(10), "Unishade", kText, kSemibold, true);
    Line(draw, textX, origin.x + width - pad, origin.y + y + S(28), "Version " + model.version, kDim, kNote);
    y += logo + S(26);

    if (NavRow("home", origin + ImVec2(pad, y), rowWidth, "Home", HomeIcon, state->page == PageKind::Home, Problems(model)))
        ShowPage(PageKind::Home);
    y += S(38) + S(22);

    // The games, with a button to add one.
    const std::string heading = model.games.empty() ? "GAMES" : "GAMES  " + std::to_string(model.games.size());
    Line(draw, origin.x + pad + S(8), origin.x + width - pad - S(30), origin.y + y + S(12), heading, kDim, kHeading, true);
    if (IconButton("add", origin + ImVec2(width - pad - S(26), y - S(1)), S(26), [](ImDrawList* d, ImVec2 c, ImU32 color) { PlusIcon(d, c, color, S(5)); },
                   "Add game"))
        host.OpenPicker(true);
    y += S(24) + S(6);

    // The bottom rows stay at the bottom, and the games scroll between.
    const std::vector<BottomPage> bottomPages = BottomPages(model);
    const float bottomRows = static_cast<float>(bottomPages.size()) * (S(38) + S(4)) + S(14) + S(12);
    const float listHeight = std::max(height - y - bottomRows, S(40));
    ImGui::SetCursorScreenPos(origin + ImVec2(0, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6));
    ImGui::BeginChild("games", ImVec2(width - 1, listHeight), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float rowHeight = S(44);
        const size_t removedRow = model.removed ? std::min(model.removed->index, model.games.size()) : SIZE_MAX;
        const size_t rows = model.games.size() + (model.removed ? 1 : 0);
        if (!rows)
            Line(list, top.x + pad + S(8), top.x + width - pad, top.y + S(14), "No games yet.", kDim, kBody);
        for (size_t row = 0; row < rows; ++row)
        {
            const ImVec2 min = top + ImVec2(pad, static_cast<float>(row) * (rowHeight + S(2)));
            if (row == removedRow)
            {
                // Undo follows the text, away from where the game's row was clicked.
                const std::string text = "Removed " + model.removed->name + ".";
                const float undoWidth = Measure("Undo", kBody).x;
                const float textEnd = std::min(min.x + S(12) + Measure(text, kBody).x, min.x + rowWidth - undoWidth - S(18));
                Line(list, min.x + S(12), textEnd, min.y + rowHeight / 2, text, kDim, kBody);
                ImGui::PushID("removed");
                if (PlacedLink("Undo", ImVec2(textEnd + S(8), min.y + rowHeight / 2 - S(10)), S(20)))
                    host.UndoRemove();
                ImGui::PopID();
                continue;
            }
            const size_t index = row > removedRow ? row - 1 : row;
            const Game& game = model.games[index];
            ImGui::PushID(game.id.c_str());
            ImGui::SetCursorScreenPos(min);
            const bool selected = state->page == PageKind::Game && state->game == game.id;
            if (ImGui::InvisibleButton("game", ImVec2(rowWidth, rowHeight), ImGuiButtonFlags_EnableNav))
                ShowPage(PageKind::Game, game.id);
            const bool hovered = ImGui::IsItemHovered();
            HandOnHover();
            if (selected || hovered)
                list->AddRectFilled(min, min + ImVec2(rowWidth, rowHeight), selected ? kCard : Fade(kCard, 0x90), S(8));
            if (selected)
                list->AddRectFilled(min + ImVec2(0, S(13)), min + ImVec2(S(3), rowHeight - S(13)), kAccent, S(1.5f));
            const float icon = std::round(S(28));
            const ImVec2 iconMin = min + ImVec2(S(12), (rowHeight - icon) / 2);
            GamePicture(list, iconMin, icon, game.name, game.picture, kBody, S(6), game.enabled ? 0 : Fade(selected ? kCard : kInset, 0xA0));
            const float nameRight = min.x + rowWidth - (game.running ? S(26) : S(10));
            Line(list, iconMin.x + icon + S(12), nameRight, min.y + rowHeight / 2, game.name, game.enabled ? (selected || hovered ? kText : Color(0xC9CAD3)) : kDim,
                 kBody, selected);
            if (game.running)
            {
                const ImVec2 dot = min + ImVec2(rowWidth - S(16), rowHeight / 2);
                list->AddCircleFilled(dot, S(7), Fade(kSuccess, 0x30));
                list->AddCircleFilled(dot, S(3.5f), kSuccess);
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(top + ImVec2(0, static_cast<float>(rows) * (rowHeight + S(2))));
        ImGui::Dummy(ImVec2(1, S(8)));
    }
    ImGui::EndChild();

    float bottom = height - bottomRows + S(14);
    draw->AddLine(origin + ImVec2(pad, bottom - S(8)), origin + ImVec2(width - pad, bottom - S(8)), kBorder);
    for (const BottomPage& page : bottomPages)
    {
        if (NavRow(page.id, origin + ImVec2(pad, bottom), rowWidth, page.label, page.icon, state->page == page.page))
            ShowPage(page.page);
        bottom += S(38) + S(4);
    }
    ImGui::EndChild();
}

// A square button of the rail with an icon or a game's picture, and its name as the tip.
bool RailButton(const char* id, ImVec2 min, float size, bool selected, const std::string& tip)
{
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    Tooltip(tip);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (selected || hovered)
        draw->AddRectFilled(min, min + ImVec2(size, size), selected ? kCard : Fade(kCard, 0x90), S(8));
    if (selected)
        draw->AddRectFilled(min + ImVec2(-S(8), S(10)), min + ImVec2(-S(5), size - S(10)), kAccent, S(1.5f));
    return clicked;
}

// The sidebar over the game: icons, with the logo on top and the pages for every game at the bottom.
void Rail(const Model& model, ImVec2 at, float height)
{
    const float width = S(kRailWidth);
    ImGui::SetCursorScreenPos(at);
    ImGui::BeginChild("rail", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    draw->AddLine(origin + ImVec2(width - 1, S(kStrip)), origin + ImVec2(width - 1, height), kBorder);
    const float button = S(44);
    const float left = std::round((width - button) / 2);
    float y = S(kStrip) + S(18);
    const float logo = std::round(S(30));
    if (const ImTextureID texture = Picture("logo", logo); texture != ImTextureID_Invalid)
        draw->AddImage(ImTextureRef(texture), origin + ImVec2(std::round((width - logo) / 2), y), origin + ImVec2(std::round((width - logo) / 2) + logo, y + logo));
    y += logo + S(18);

    const bool home = state->page == PageKind::Home;
    if (RailButton("home", origin + ImVec2(left, y), button, home, "Home"))
        ShowPage(PageKind::Home);
    HomeIcon(draw, origin + ImVec2(width / 2, y + button / 2), home ? kText : kDim);
    if (Problems(model))
        draw->AddCircleFilled(origin + ImVec2(left + button - S(10), y + S(10)), S(3.5f), kWarning);
    y += button + S(12);
    draw->AddLine(origin + ImVec2(left + S(8), y), origin + ImVec2(left + button - S(8), y), kBorder);
    y += S(12);

    // The games scroll between the top and the bottom.
    const std::vector<BottomPage> bottomPages = BottomPages(model);
    const float bottomHeight = static_cast<float>(bottomPages.size()) * (button + S(6)) + S(18);
    ImGui::SetCursorScreenPos(origin + ImVec2(0, y));
    ImGui::BeginChild("games", ImVec2(width - 1, std::max(height - y - bottomHeight, button)), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        for (size_t i = 0; i < model.games.size(); ++i)
        {
            const Game& game = model.games[i];
            const ImVec2 min = top + ImVec2(left, static_cast<float>(i) * (button + S(6)));
            ImGui::PushID(game.id.c_str());
            const bool selected = state->page == PageKind::Game && state->game == game.id;
            if (RailButton("game", min, button, selected, game.name))
                ShowPage(PageKind::Game, game.id);
            const float icon = std::round(S(28));
            const ImVec2 iconMin(std::round(min.x + (button - icon) / 2), std::round(min.y + (button - icon) / 2));
            GamePicture(list, iconMin, icon, game.name, game.picture, kNote, S(6), game.enabled ? 0 : Fade(selected ? kCard : kBackground, 0xA0));
            if (game.running)
            {
                const ImVec2 dot = iconMin + ImVec2(icon, icon) - ImVec2(S(1), S(1));
                list->AddCircleFilled(dot, S(5), selected ? kCard : kBackground);
                list->AddCircleFilled(dot, S(3), kSuccess);
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(top + ImVec2(0, static_cast<float>(model.games.size()) * (button + S(6))));
        ImGui::Dummy(ImVec2(1, 1));
    }
    ImGui::EndChild();

    float bottom = height - bottomHeight + S(12);
    for (const BottomPage& page : bottomPages)
    {
        const bool selected = state->page == page.page;
        if (RailButton(page.id, origin + ImVec2(left, bottom), button, selected, page.label))
            ShowPage(page.page);
        page.icon(draw, origin + ImVec2(width / 2, bottom + button / 2), selected ? kText : kDim);
        bottom += button + S(6);
    }
    ImGui::EndChild();
}

// A square button with an icon, outlined as the menu's were.
bool SquareButton(const char* id, ImVec2 min, float size, void (*icon)(ImDrawList*, ImVec2, ImU32), const char* tip, bool bright = false)
{
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    Tooltip(tip);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, min + ImVec2(size, size), hovered ? kBorder : kCardHover, S(8));
    draw->AddRect(min, min + ImVec2(size, size), hovered ? kBorderStrong : kBorder, S(8));
    icon(draw, min + ImVec2(size, size) * 0.5f, hovered || bright ? kText : kDim);
    return clicked;
}

// The menu's buttons, under the page over the game.
void Footer(const Model& model, Host& host, ImVec2 min, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddLine(min, min + ImVec2(size.x, 0), kBorder);
    const float pad = S(kPanelPadding);
    const float height = S(40);
    const float top = min.y + std::round((size.y - height) / 2);
    float x = min.x + pad;

    // Effects stay off while Compare is held.
    const float compareWidth = ButtonWidth("Compare");
    PlacedButton("Compare", ImVec2(x, top), ImVec2(compareWidth, height));
    const bool comparing = ImGui::IsItemActive();
    host.Compare(comparing);
    if (!comparing)
        Tooltip("Hold to see the game without effects");
    x += compareWidth + S(8);

    // Screenshots leave out the menu.
    if (SquareButton("camera", ImVec2(x, top), height, CameraIcon, "Screenshots"))
        ImGui::OpenPopup("screenshots");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("screenshots"))
    {
        if (ImGui::MenuItem("Screenshot"))
            host.TakeScreenshot(false);
        if (ImGui::MenuItem("Before and after"))
            host.TakeScreenshot(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Open screenshots folder"))
            host.OpenScreenshots();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    x += height + S(8);

    // With auto-save off, the changes to the preset in use.
    const auto played = std::find_if(model.games.begin(), model.games.end(), [](const Game& game) { return game.running; });
    if (played != model.games.end() && played->changed && SquareButton("save", ImVec2(x, top), height, SaveIcon, "Save changes", true))
        host.SaveChanges();

    // The menu's shortcut, which also leaves.
    const char* label = "Back to the game";
    const float labelWidth = Measure(label, kBody).x;
    const float keyWidth = std::max(Measure(model.menuKey, kNote).x + S(14), S(28));
    const ImVec2 backSize(keyWidth + S(10) + labelWidth, height);
    const ImVec2 backMin(min.x + size.x - pad - backSize.x, top);
    ImGui::SetCursorScreenPos(backMin);
    if (ImGui::InvisibleButton("back", backSize, ImGuiButtonFlags_EnableNav))
        host.CloseMenu();
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    PushSize(kNote);
    const float keyHeight = ImGui::GetFontSize() + S(6);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(backMin + ImVec2(0, std::round((height - keyHeight) / 2)));
    KeyCap(model.menuKey, kNote);
    Line(draw, backMin.x + keyWidth + S(10), backMin.x + backSize.x + 1, backMin.y + height / 2, label, hovered ? kText : kDim, kBody);
}

// Home

ImU32 ToneColor(Tone tone)
{
    return tone == Tone::Running ? kSuccess : tone == Tone::Off ? kDim : kAccent;
}

// What Unishade is doing, switching between detection and a picked window, and the effects on and off.
void StatusCard(const Model& model, Host& host, const Page& page, float& y)
{
    const Status& status = model.status;
    const float pad = S(24);
    const float icon = std::round(S(60));
    std::vector<std::pair<const char*, bool>> buttons; // the label, and whether it opens the picker
    if (status.canPick)
        buttons.emplace_back("Pick a window", true);
    if (status.canDetect)
        buttons.emplace_back("Detect automatically", false);
    float buttonWidth = 0;
    for (const auto& [label, pick] : buttons)
        buttonWidth = std::max(buttonWidth, ButtonWidth(label));
    const float buttonsHeight = buttons.empty() ? 0 : static_cast<float>(buttons.size()) * (S(32) + S(8)) - S(8);

    const float top = y;
    BeginCard(page);
    const float textLeft = page.left + pad + icon + S(20);
    const float textRight = page.right - pad - (buttons.empty() ? 0 : buttonWidth + S(20));
    const float wrap = textRight - textLeft;
    const float titleHeight = Measure(status.title, kTitle, true, wrap).y;
    const float detailHeight = status.detail.empty() ? 0 : S(4) + Measure(status.detail, kBody, false, wrap).y;
    const float warningHeight = status.warning.empty() ? 0 : S(6) + Measure(status.warning, kBody, false, wrap).y;
    const float textHeight = titleHeight + detailHeight + warningHeight;
    const float height = std::max({ icon, textHeight, buttonsHeight });
    const float middle = y + pad + height / 2;

    // The game's icon with the state as a dot on its corner, or a glowing dot while there is no game.
    const ImVec2 iconMin = page.At(page.left + pad, middle - icon / 2);
    const ImU32 color = ToneColor(status.tone);
    if (status.game.empty())
    {
        const ImVec2 center = iconMin + ImVec2(icon, icon) * 0.5f;
        page.draw->AddCircleFilled(center, icon / 2, Fade(color, 0x18));
        page.draw->AddCircleFilled(center, S(15), Fade(color, 0x40));
        page.draw->AddCircleFilled(center, S(7.5f), color);
    }
    else
    {
        GamePicture(page.draw, iconMin, icon, status.game, status.picture, kTitle, S(12));
        const ImVec2 dot = iconMin + ImVec2(icon - S(4), icon - S(4));
        page.draw->AddCircleFilled(dot, S(9.5f), kCard);
        page.draw->AddCircleFilled(dot, S(6), color);
    }

    float textY = middle - textHeight / 2;
    textY += Paragraph(page.draw, page.At(textLeft, textY), page.origin.x + textRight, status.title, kText, kTitle, true);
    if (!status.detail.empty())
        textY += S(4) + Paragraph(page.draw, page.At(textLeft, textY + S(4)), page.origin.x + textRight, status.detail, kDim, kBody);
    if (!status.warning.empty())
        Paragraph(page.draw, page.At(textLeft, textY + S(6)), page.origin.x + textRight, status.warning, kWarning, kBody);

    float buttonTop = middle - buttonsHeight / 2;
    for (const auto& [label, pick] : buttons)
    {
        if (PlacedButton(label, page.At(page.right - pad - buttonWidth, buttonTop), ImVec2(buttonWidth, S(32))))
        {
            if (pick)
                host.OpenPicker(false);
            else
                host.DetectAutomatically();
        }
        buttonTop += S(32) + S(8);
    }

    // The effects on and off for every game.
    y += pad + height + pad;
    page.draw->AddLine(page.At(page.left, y), page.At(page.right, y), kBorder);
    y += S(14);
    ImGui::SetCursorScreenPos(page.At(page.left + pad, y));
    if (kit::Switch("effects", status.effectsShown, ImVec2(S(40), S(22))))
        host.SetSwitch(Switch::EffectsShown, !status.effectsShown);
    Line(page.draw, page.origin.x + page.left + pad + S(52), page.origin.x + page.right - pad, page.origin.y + y + S(11), "Show effects", kText, kBody);
    y += S(22) + S(14);
    EndCard(page, top, y);
    y += S(16);
}

// Cards for what needs the user, such as an update.
void Cards(const Model& model, Host& host, const Page& page, float& y)
{
    for (size_t i = 0; i < model.cards.size(); ++i)
    {
        const Card& card = model.cards[i];
        ImGui::PushID(static_cast<int>(i));
        const float pad = S(20);
        const float top = y;
        BeginCard(page);
        y += pad;
        // Buttons on the right of the text when there is room.
        float buttonsWidth = 0;
        for (const std::string& label : card.buttons)
            buttonsWidth += ButtonWidth(label.c_str(), &label == &card.buttons.front()) + S(8);
        const bool side = page.right - page.left - buttonsWidth > S(420);
        const float textRight = page.right - pad - (side ? buttonsWidth + S(12) : 0);
        const float textTop = y;
        y += Paragraph(page.draw, page.At(page.left + pad, y), page.origin.x + textRight, card.title, kText, kSemibold, true);
        if (!card.text.empty())
            y += S(4) + Paragraph(page.draw, page.At(page.left + pad, y + S(4)), page.origin.x + textRight, card.text, kDim, kBody);
        if (card.progress >= 0)
        {
            y += S(12);
            const ImVec2 bar = page.At(page.left + pad, y);
            const float width = textRight - page.left - pad;
            page.draw->AddRectFilled(bar, bar + ImVec2(width, S(4)), kBorder, S(2));
            page.draw->AddRectFilled(bar, bar + ImVec2(width * std::clamp(card.progress, 0.0f, 1.0f), S(4)), kAccent, S(2));
            y += S(4);
        }
        float x = side ? page.right - pad - buttonsWidth + S(8) : page.left + pad;
        const float buttonTop = side ? textTop + std::max((y - textTop - S(32)) / 2, 0.0f) : y + S(14);
        for (size_t button = 0; button < card.buttons.size(); ++button)
        {
            const std::string& label = card.buttons[button];
            const float width = ButtonWidth(label.c_str(), button == 0);
            if (PlacedButton((label + "##" + std::to_string(button)).c_str(), page.At(x, buttonTop), ImVec2(width, S(32)), button == 0))
                host.PressCard(i, button);
            x += width + S(8);
        }
        if (!card.buttons.empty())
            y = std::max(y, buttonTop + S(32));
        y += pad;
        EndCard(page, top, y, card.attention ? Color(theme::kAccent, 0x1C) : kCard, card.attention ? Color(theme::kAccent, 0x80) : kBorder);
        y += S(16);
        ImGui::PopID();
    }
}

// The games as tiles, and one to add a game.
void GameTiles(const Model& model, Host& host, const Page& page, float& y)
{
    y += S(12);
    SectionHeading(page, y, "GAMES");
    const float gap = S(14);
    const float available = page.right - page.left;
    const int columns = std::max(1, static_cast<int>((available + gap) / (S(156) + gap)));
    const float width = (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float height = S(150);
    const size_t count = model.games.size() + 1;
    for (size_t i = 0; i < count; ++i)
    {
        const ImVec2 min = page.At(page.left + static_cast<float>(i % columns) * (width + gap), y + static_cast<float>(i / columns) * (height + gap));
        const ImVec2 max = min + ImVec2(width, height);
        const bool add = i == model.games.size();
        ImGui::PushID(add ? "add" : model.games[i].id.c_str());
        ImGui::SetCursorScreenPos(min);
        const bool clicked = ImGui::InvisibleButton("tile", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        ImGui::PopID();
        if (add)
        {
            page.draw->AddRectFilled(min, max, hovered ? kCard : kBackground, S(12));
            page.draw->AddRect(min, max, hovered ? kBorderStrong : kBorder, S(12));
            const ImVec2 center = min + ImVec2(width / 2, height / 2 - S(12));
            page.draw->AddCircleFilled(center, S(22), hovered ? kBorder : kCard);
            PlusIcon(page.draw, center, hovered ? kText : kDim, S(7));
            Line(page.draw, min.x, max.x, center.y + S(40), "Add game", hovered ? kText : kDim, kBody, false, true);
            if (clicked)
                host.OpenPicker(true);
            continue;
        }
        const Game& game = model.games[i];
        page.draw->AddRectFilled(min, max, hovered ? kCardHover : kCard, S(12));
        page.draw->AddRect(min, max, hovered ? kBorderStrong : kBorder, S(12));
        const float icon = std::round(S(56));
        const ImVec2 iconMin = min + ImVec2((width - icon) / 2, S(22));
        GamePicture(page.draw, iconMin, icon, game.name, game.picture, kTitle, S(10), game.enabled ? 0 : Fade(hovered ? kCardHover : kCard, 0xA0));
        Line(page.draw, min.x + S(12), max.x - S(12), iconMin.y + icon + S(20), game.name, game.enabled ? kText : kDim, kBody, true, true);
        if (game.running)
            Line(page.draw, min.x + S(12), max.x - S(12), iconMin.y + icon + S(40), "Running", kSuccess, kNote, false, true);
        else if (!game.enabled)
            Line(page.draw, min.x + S(12), max.x - S(12), iconMin.y + icon + S(40), "Off", kDim, kNote, false, true);
        if (clicked)
            ShowPage(PageKind::Game, game.id);
    }
    const size_t rows = (count + columns - 1) / columns;
    y += static_cast<float>(rows) * (height + gap) - gap + S(28);
}

// A notice's words one by one, so the web addresses in it can be links. Returns its height.
float NoticeText(const Page& page, Host& host, float left, float y, const std::string& text, int index)
{
    PushSize(kBody);
    const float space = ImGui::CalcTextSize(" ").x;
    const float line = ImGui::GetTextLineHeight();
    const float right = page.right;
    float x = left;
    float top = y;
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
            if (x > left && x + wordWidth > right)
            {
                x = left;
                top += line;
            }
            if (!address.empty())
            {
                ImGui::PushID(index * 64 + link++);
                ImGui::SetCursorScreenPos(page.At(x, top));
                if (ImGui::InvisibleButton("link", ImVec2(std::max(addressWidth, 1.0f), line)))
                    host.OpenUrl(address);
                const bool hovered = ImGui::IsItemHovered();
                HandOnHover();
                page.draw->AddText(page.At(x, top), hovered ? kText : kAccentHover, address.c_str());
                ImGui::PopID();
            }
            if (!word.empty())
                page.draw->AddText(page.At(x + addressWidth, top), kText, word.c_str());
            x += wordWidth + space;
        }
        if (end < text.size() && text[end] == '\n')
        {
            x = left;
            top += line;
        }
        start = end + 1;
    }
    ImGui::PopFont();
    return top + line - y;
}

void Messages(const Model& model, Host& host, const Page& page, float& y)
{
    if (model.notices.empty())
        return;
    if (SectionHeading(page, y, "MESSAGES", "Dismiss"))
        host.DismissNotices();
    const float top = y;
    BeginCard(page);
    y += S(16);
    // Newest first.
    for (size_t i = model.notices.size(); i-- > 0;)
    {
        const Notice& notice = model.notices[i];
        NoticeIcon(page.draw, page.At(page.left + S(28), y + S(10)), notice.level);
        y += std::max(NoticeText(page, host, page.left + S(48), y, notice.text, static_cast<int>(i)), S(20)) + S(10);
    }
    y += S(6);
    EndCard(page, top, y);
    y += S(28);
}

void Links(const Model& model, Host& host, const Page& page, float& y)
{
    float x = page.left;
    for (size_t i = 0; i < model.links.size(); ++i)
    {
        const std::string& label = model.links[i];
        const float width = Measure(label, kBody).x;
        if (x > page.left && x + width > page.right)
        {
            x = page.left;
            y += S(28);
        }
        if (PlacedLink(label.c_str(), page.At(x, y), S(20)))
            host.OpenLink(i);
        x += width + S(24);
    }
    y += S(20);
}

void HomePage(const Model& model, Host& host, const Page& page, float& y)
{
    StatusCard(model, host, page, y);
    Cards(model, host, page, y);
    GameTiles(model, host, page, y);
    Messages(model, host, page, y);
    Links(model, host, page, y);
}

// A game's page

// The color a picture tints the top of its page with, made bright enough to show.
ImU32 Tint(const std::string& picture)
{
    ImU32 color = picture.empty() ? 0 : PictureTint(picture);
    if (!color)
        return kAccent;
    float h = 0, s = 0, v = 0;
    ImGui::ColorConvertRGBtoHSV(((color >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, ((color >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                                ((color >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, h, s, v);
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB(h, std::min(s * 1.2f, 1.0f), std::max(v, 0.85f), r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
}

void GameHero(const Model& model, Host& host, size_t index, const Page& page, float& y, float windowLeft, float windowWidth)
{
    const Game& game = model.games[index];
    // The game's color from the top of the page, fading into the background.
    const ImU32 tint = Tint(game.picture);
    const ImVec2 bandMin(windowLeft, page.origin.y);
    page.draw->AddRectFilledMultiColor(bandMin, bandMin + ImVec2(windowWidth, S(250)), Fade(tint, 0x2C), Fade(tint, 0x2C), Fade(tint, 0), Fade(tint, 0));

    const float icon = std::round(S(96));
    const ImVec2 iconMin = page.At(page.left, y);
    GamePicture(page.draw, iconMin, icon, game.name, game.picture, kHeroTitle, S(16), game.enabled ? 0 : Fade(kBackground, 0x90));

    // The switch and the remove button on the right.
    const float controlsWidth = S(40) + S(16) + S(32);
    const float textLeft = page.left + icon + S(24);
    const float textRight = page.right - controlsWidth - S(24);
    float textY = y + S(6);
    const bool renaming = state->renaming == game.id;
    if (renaming)
    {
        ImGui::SetCursorScreenPos(page.At(textLeft - S(8), textY - S(4)));
        ImGui::PushItemWidth(std::min(textRight - textLeft + S(8), S(460)));
        const bool problem = state->renameFailed && !model.renameProblem.empty();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kCardHover);
        ImGui::PushStyleColor(ImGuiCol_Border, problem ? kError : kAccent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(4)));
        PushSize(kHeroTitle, true);
        if (std::exchange(state->focusName, false))
            ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##name", state->name, sizeof(state->name), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemEdited())
            state->renameFailed = false;
        const bool left = ImGui::IsItemDeactivated();
        const float height = ImGui::GetItemRectSize().y;
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopItemWidth();
        // Enter keeps the box open when the name is not taken. Leaving it drops such a name, and Escape any name.
        if (entered)
        {
            if (host.RenameGame(index, state->name))
                state->renaming.clear();
            else
            {
                state->renameFailed = true;
                state->focusName = true;
            }
        }
        else if (left)
        {
            if (!ImGui::IsKeyPressed(ImGuiKey_Escape))
                host.RenameGame(index, state->name);
            state->renaming.clear();
        }
        textY += height - S(4);
        Line(page.draw, page.origin.x + textLeft, page.origin.x + textRight, page.origin.y + textY + S(14), problem ? model.renameProblem : "Enter saves the name. Esc cancels.",
             problem ? kError : kDim, kNote);
    }
    else
    {
        const float nameWidth = std::min(Measure(game.name, kHeroTitle, true).x, textRight - textLeft - S(40));
        Line(page.draw, page.origin.x + textLeft, page.origin.x + textLeft + nameWidth, page.origin.y + textY + S(18), game.name, kText, kHeroTitle, true);
        if (!game.picked && IconButton("rename", page.At(textLeft + nameWidth + S(10), textY + S(4)), S(28), PencilIcon, "Rename"))
        {
            state->renaming = game.id;
            state->renameFailed = false;
            state->focusName = true;
            std::snprintf(state->name, sizeof(state->name), "%s", game.name.c_str());
        }
        textY += S(36);
        Line(page.draw, page.origin.x + textLeft, page.origin.x + textRight, page.origin.y + textY + S(10), game.executable, kDim, kNote);
    }
    textY += S(30);

    // What Unishade does with it.
    const ImVec2 pillMin = page.At(textLeft, textY);
    const float pillWidth = Pill(page.draw, pillMin, game.running ? "Running" : game.enabled ? "Not running" : "Off", game.running ? kSuccess : kDim);
    const std::string detail = game.running ? model.status.detail : game.enabled ? "Effects start when you open it." : "Unishade skips it while it's off.";
    Line(page.draw, pillMin.x + pillWidth + S(12), page.origin.x + textRight, pillMin.y + S(11), detail, kDim, kBody);

    if (!game.picked)
    {
        ImGui::SetCursorScreenPos(page.At(page.right - controlsWidth, y + S(10)));
        if (kit::Switch("enabled", game.enabled, ImVec2(S(40), S(22))))
            host.SetGameEnabled(index, !game.enabled);
        Tooltip(game.enabled ? "Turn off" : "Turn on");
        if (IconButton("remove", page.At(page.right - S(32), y + S(5)), S(32), CrossIcon, "Remove from list", true))
            host.RemoveGame(index);
    }

    y += std::max(icon, textY + S(22) - y) + S(40);
}

// Links on the right of a section's heading at y, the last one rightmost. Returns the index of the one clicked, or -1.
int HeadingLinks(const Page& page, float y, std::initializer_list<const char*> links)
{
    int clicked = -1;
    float x = page.right;
    for (int i = static_cast<int>(links.size()) - 1; i >= 0; --i)
    {
        const char* link = links.begin()[i];
        x -= Measure(link, kBody).x;
        if (PlacedLink(link, page.At(x, y), S(20)))
            clicked = i;
        x -= S(20);
    }
    return clicked;
}

// Three dots, which open a menu.
bool DotsButton(const char* id, ImVec2 min, float size, bool bright)
{
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered)
        draw->AddRectFilled(min, min + ImVec2(size, size), kBorder, S(6));
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(min + ImVec2(size / 2 + static_cast<float>(i) * S(5), size / 2), S(1.6f), hovered || bright ? kText : kDim);
    return clicked;
}

void OpenPresetName(PresetAction action, size_t game, const std::string& target, const std::string& name)
{
    state->presetAction = action;
    state->presetGame = game;
    state->presetTarget = target;
    state->presetFailed = false;
    std::snprintf(state->presetName, sizeof(state->presetName), "%s", name.c_str());
    state->openPresetName = true;
}

// The menu of a preset's row.
void PresetMenu(const Model& model, Host& host, size_t index, const Preset& preset)
{
    const Game& game = model.games[index];
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    if (ImGui::BeginPopup("actions"))
    {
        PushSize(kBody);
        if (ImGui::MenuItem("Duplicate"))
            OpenPresetName(PresetAction::Copy, index, preset.id, preset.name + " copy");
        if (ImGui::MenuItem("Rename", nullptr, false, !preset.loaded))
            OpenPresetName(PresetAction::Rename, index, preset.id, preset.name);
        // To every folder but its own.
        const std::string& current = preset.folder.empty() ? game.name : preset.folder;
        if (ImGui::BeginMenu("Move to", !preset.loaded))
        {
            for (const Folder& folder : model.folders)
                if (folder.name != current && ImGui::MenuItem(folder.name.c_str()))
                    host.MovePreset(preset.id, folder.id);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Delete", nullptr, false, !preset.loaded))
        {
            state->deleting = preset.id;
            state->openDelete = true;
        }
        ImGui::PopFont();
        if (preset.loaded)
        {
            ImGui::Separator();
            Text("Switch to another preset to rename,\nmove or delete this one.", kDim, kNote, -1.0f);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// Switches the game to a preset, asking first about the changes to the one in use.
void UsePreset(const Game& game, Host& host, size_t index, const std::string& preset)
{
    if (!game.changed)
    {
        host.UsePreset(index, preset);
        return;
    }
    state->pendingPreset = preset;
    state->pendingGame = index;
    state->openUnsaved = true;
}

// The top of a game's page over the game: its picture, name and the preset in use. Renaming, turning off and removing
// it are in the launcher's window.
void CompactHero(const Model& model, size_t index, const Page& page, float& y, float windowLeft, float windowWidth)
{
    const Game& game = model.games[index];
    const ImU32 tint = Tint(game.picture);
    const ImVec2 bandMin(windowLeft, page.origin.y);
    page.draw->AddRectFilledMultiColor(bandMin, bandMin + ImVec2(windowWidth, S(170)), Fade(tint, 0x2C), Fade(tint, 0x2C), Fade(tint, 0), Fade(tint, 0));
    const float icon = std::round(S(60));
    GamePicture(page.draw, page.At(page.left, y), icon, game.name, game.picture, kTitle, S(12));
    const float textLeft = page.origin.x + page.left + icon + S(18);
    const float middle = page.origin.y + y + icon / 2;
    Line(page.draw, textLeft, page.origin.x + page.right, middle - S(11), game.name, kText, kTitle, true);
    const auto preset = std::find_if(game.presets.begin(), game.presets.end(), [&](const Preset& preset) { return preset.id == game.preset; });
    if (preset != game.presets.end())
        Line(page.draw, textLeft, page.origin.x + page.right, middle + S(15), preset->name, kDim, kBody);
    y += icon + S(36);
}

void PresetsSection(const Model& model, Host& host, size_t index, const Page& page, float& y)
{
    const Game& game = model.games[index];
    const float headingY = y;
    SectionHeading(page, y, "PRESETS");
    const int link = model.editPresets ? HeadingLinks(page, headingY, { "New preset", "Open folder" }) : HeadingLinks(page, headingY, { "Open folder" });
    if (link == 0 && model.editPresets)
        OpenPresetName(PresetAction::New, index, {}, "New preset");
    else if (link >= 0)
        host.OpenPresetsFolder(index);
    const float top = y;
    BeginCard(page);
    if (game.presets.empty())
    {
        y += S(18);
        y += Paragraph(page.draw, page.At(page.left + S(20), y), page.origin.x + page.right - S(20),
                       model.editPresets ? "No presets yet." : "No presets yet. Make one with New preset in the menu while the game runs.", kDim, kBody);
        y += S(18);
    }
    const float rowHeight = S(46);
    const float dots = S(28);
    std::string folder = game.presets.empty() ? std::string() : game.presets.front().folder;
    for (size_t i = 0; i < game.presets.size(); ++i)
    {
        const Preset& preset = game.presets[i];
        // Presets from other folders, such as the ones for all games, under the folder's name.
        if (preset.folder != folder || (i == 0 && !preset.folder.empty()))
        {
            if (i)
                RowSeparator(page, y);
            folder = preset.folder;
            Line(page.draw, page.origin.x + page.left + S(20), page.origin.x + page.right - S(20), page.origin.y + y + S(20), preset.folder, kDim, kNote, true);
            y += S(32);
        }
        const bool used = preset.id == game.preset;
        ImGui::PushID(preset.id.c_str());
        const ImVec2 min = page.At(page.left + S(6), y);
        const ImVec2 size(page.right - page.left - S(12), rowHeight);
        ImGui::SetCursorScreenPos(min);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("preset", size, ImGuiButtonFlags_EnableNav) && !used)
            UsePreset(game, host, index, preset.id);
        const bool hovered = ImGui::IsItemHovered();
        if (!used)
            HandOnHover();
        if (hovered && !used)
            page.draw->AddRectFilled(min, min + size, kCardHover, S(8));
        // The menu's three dots on the right, and what the row is to its left.
        float right = min.x + size.x - S(12);
        if (model.editPresets)
        {
            const ImVec2 dotsMin(right - dots, min.y + (rowHeight - dots) / 2);
            if (DotsButton("dots", dotsMin, dots, hovered))
                ImGui::OpenPopup("actions");
            PresetMenu(model, host, index, preset);
            right = dotsMin.x - S(8);
        }
        const float nameRight = right - S(90);
        if (used)
        {
            const float width = Measure("In use", kNote).x + S(18);
            Pill(page.draw, ImVec2(right - width, min.y + rowHeight / 2 - S(11)), "In use", kAccentHover);
        }
        else if (hovered && !ImGui::IsPopupOpen("actions"))
        {
            const float width = Measure("Use", kBody).x;
            Line(page.draw, right - S(4) - width, right, min.y + rowHeight / 2, "Use", kAccentHover, kBody, true);
        }
        const float nameWidth = std::min(Measure(preset.name, kBody, used).x, nameRight - min.x - S(14) - (used && game.changed ? S(14) : 0));
        Line(page.draw, min.x + S(14), min.x + S(14) + nameWidth, min.y + rowHeight / 2, preset.name, kText, kBody, used);
        if (used && game.changed)
            page.draw->AddCircleFilled(ImVec2(min.x + S(14) + nameWidth + S(9), min.y + rowHeight / 2 + S(1)), S(3), kWarning);
        ImGui::PopID();
        y += rowHeight;
    }
    if (!game.presets.empty())
        y += S(6);
    const float pad = S(20);
    if (!game.missing.empty())
    {
        std::string names;
        for (const std::string& effect : game.missing)
            names += (names.empty() ? "" : ", ") + effect;
        RowSeparator(page, y);
        y += S(14);
        y += Paragraph(page.draw, page.At(page.left + pad, y), page.origin.x + page.right - pad, "Uses effects that are not installed: " + names + ".",
                       kWarning, kNote);
        y += S(14);
    }
    // Changes waiting to be saved, with auto-save off.
    if (game.changed)
    {
        RowSeparator(page, y);
        const float height = S(56);
        const float middle = y + height / 2;
        const float saveWidth = ButtonWidth("Save", true);
        const float saveLeft = page.right - pad - saveWidth;
        const float discardLeft = saveLeft - S(16) - Measure("Discard", kBody).x;
        Line(page.draw, page.origin.x + page.left + pad, page.origin.x + discardLeft - S(12), page.origin.y + middle, "Changes are not saved.", kDim, kBody);
        if (PlacedLink("Discard", page.At(discardLeft, middle - S(10)), S(20)))
            host.DiscardChanges();
        if (PlacedButton("Save##changes", page.At(saveLeft, middle - S(16)), ImVec2(saveWidth, S(32)), true))
            host.SaveChanges();
        y += height;
    }
    EndCard(page, top, y);
    y += S(28);
}

// Effects

std::string Lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A text box on the page, as wide as it, with a hint while empty.
void SearchBox(const Page& page, float& y, const char* id, const char* hint, char* text, size_t size)
{
    ImGui::SetCursorScreenPos(page.At(page.left, y));
    ImGui::PushItemWidth(page.right - page.left);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kCardHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kCardHover);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(10));
    PushSize(kBody);
    ImGui::InputTextWithHint(id, hint, text, size);
    y += ImGui::GetItemRectSize().y;
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    ImGui::PopItemWidth();
}

// Lists the effects again when the list, the search or which are on changed. Effects on are listed under Active, in
// the order they run, and stay there once turned off. The rest go by name.
void ListEffects(const Effects& effects, const std::string& preset)
{
    const std::vector<Effect>& list = effects.list;
    if (state->activePreset != preset)
    {
        state->active.clear();
        state->activePreset = preset;
        state->rowsStale = true;
    }
    bool same = !state->rowsStale && state->rowsRevision == effects.revision && state->rowsSearch == state->search && state->rowsOn.size() == list.size();
    for (size_t i = 0; same && i < list.size(); ++i)
        same = state->rowsOn[i] == list[i].on;
    if (same)
        return;
    state->rowsStale = false;
    state->rowsRevision = effects.revision;
    state->rowsSearch = state->search;
    state->rowsOn.resize(list.size());
    for (size_t i = 0; i < list.size(); ++i)
    {
        state->rowsOn[i] = list[i].on;
        if (list[i].on)
            state->active.insert(list[i].key);
    }
    const std::string filter = Lower(state->search);
    state->activeRows.clear();
    state->otherRows.clear();
    for (size_t i = 0; i < list.size(); ++i)
        if (filter.empty() || Lower(list[i].name).find(filter) != std::string::npos || Lower(list[i].file).find(filter) != std::string::npos)
            (state->active.count(list[i].key) ? state->activeRows : state->otherRows).push_back(i);
    std::sort(state->otherRows.begin(), state->otherRows.end(), [&](size_t a, size_t b) { return Lower(list[a].name) < Lower(list[b].name); });
}

// A heading that opens and closes the settings under it, with the launcher's chevron. Remembers whether it is open.
bool SettingsHeading(const char* label, bool openFirst)
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    bool open = storage->GetBool(id, openFirst);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    if (ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav))
        storage->SetBool(id, open = !open);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, min + size, hovered ? kCardHover : kCard, S(6));
    // Effects put line breaks around names for the space they leave in ReShade's menu.
    std::string text = label;
    text.erase(0, text.find_first_not_of(" \r\n\t"));
    text.erase(text.find_last_not_of(" \r\n\t") + 1);
    Line(draw, min.x + S(10), min.x + size.x - S(30), min.y + size.y / 2, text, kText, kNote, true);
    Chevron(draw, ImVec2(min.x + size.x - S(14), min.y + size.y / 2), open, hovered || open ? kText : kDim);
    return open;
}

// A choice from a list, with the launcher's chevron. items end in '\0' each.
bool ListControl(const char* id, int& value, const std::string& items)
{
    std::vector<const char*> choices;
    for (size_t at = 0; at < items.size() && items[at]; at += std::strlen(items.c_str() + at) + 1)
        choices.push_back(items.c_str() + at);
    bool changed = false;
    const char* preview = value >= 0 && static_cast<size_t>(value) < choices.size() ? choices[static_cast<size_t>(value)] : "";
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
    const ImVec2 max = ImGui::GetItemRectMax();
    Chevron(ImGui::GetWindowDrawList(), ImVec2(max.x - S(14), (ImGui::GetItemRectMin().y + max.y) / 2), true, kDim);
    if (open)
    {
        for (size_t i = 0; i < choices.size(); ++i)
            if (ImGui::Selectable(choices[i], static_cast<int>(i) == value))
            {
                value = static_cast<int>(i);
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

// One setting's label and control. Returns true when its value changed, in value.
bool SettingRow(Host& host, size_t effect, size_t index, const EffectSetting& setting, EffectSetting& value)
{
    if (setting.spacing > 0)
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * static_cast<float>(setting.spacing)));
    if (!setting.text.empty())
        Text(setting.text, kDim, kNote);
    ImGui::PushID(static_cast<int>(index));
    const float x = ImGui::GetCursorPosX();
    const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
    // One group for the label and the control, so effects can tell which of their settings is in use or under the
    // cursor.
    ImGui::BeginGroup();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x + labelWidth - S(10));
    ImGui::PushStyleColor(ImGuiCol_Text, setting.tooltip.empty() ? kText : Color(0xD8D9E6));
    ImGui::TextUnformatted(setting.label.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    Tooltip(setting.tooltip);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x + labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);

    float floats[4];
    int ints[4];
    std::copy(std::begin(setting.floats), std::end(setting.floats), floats);
    std::copy(std::begin(setting.ints), std::end(setting.ints), ints);
    const ImGuiDataType type = setting.floating ? ImGuiDataType_Float : ImGuiDataType_S32;
    void* values = setting.floating ? static_cast<void*>(floats) : static_cast<void*>(ints);
    const int low = static_cast<int>(setting.min), high = static_cast<int>(setting.max);
    const void* min = !setting.range ? nullptr : setting.floating ? static_cast<const void*>(&setting.min) : &low;
    const void* max = !setting.range ? nullptr : setting.floating ? static_cast<const void*>(&setting.max) : &high;
    // The units follow the number, with any % in them doubled for the format.
    std::string format = setting.floating ? "%.3f" : "%d";
    for (const char c : setting.units)
        format += c == '%' ? std::string("%%") : std::string(1, c);
    bool changed = false;
    switch (setting.control)
    {
    case EffectSetting::Control::Check:
    {
        // As high as the other controls, which are frames.
        const ImVec2 size(S(36), S(20));
        ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0, (ImGui::GetFrameHeight() - size.y) / 2));
        if ((changed = kit::Switch("##value", ints[0] != 0, size)))
            ints[0] = !ints[0];
        break;
    }
    case EffectSetting::Control::List:
        changed = ListControl("##value", ints[0], setting.items);
        break;
    case EffectSetting::Control::Color:
        changed = setting.count == 3 ? ImGui::ColorEdit3("##value", floats) : ImGui::ColorEdit4("##value", floats, ImGuiColorEditFlags_AlphaBar);
        break;
    case EffectSetting::Control::Slider:
        changed = ImGui::SliderScalarN("##value", type, values, setting.count, min, max, format.c_str());
        break;
    case EffectSetting::Control::Input:
        changed = ImGui::InputScalarN("##value", type, values, setting.count, nullptr, nullptr, format.c_str());
        break;
    case EffectSetting::Control::Drag:
        changed = ImGui::DragScalarN("##value", type, values, setting.count, setting.step, min, max, format.c_str());
        break;
    }
    if (setting.resettable && ImGui::BeginPopupContextItem("reset"))
    {
        if (ImGui::MenuItem("Reset to default"))
            host.ResetSettings(effect, index);
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    ImGui::PopID();
    if (changed)
    {
        value = setting;
        std::copy(std::begin(floats), std::end(floats), value.floats);
        std::copy(std::begin(ints), std::end(ints), value.ints);
    }
    return changed;
}

// The preprocessor definitions the effect checks. A change compiles the effects again, so it applies on Enter, as in
// ReShade's menu.
void DefinitionRows(Host& host, size_t effect, const EffectSettings& settings)
{
    if (settings.definitions.empty() || !SettingsHeading("Preprocessor definitions", false))
        return;
    for (size_t i = 0; i < settings.definitions.size(); ++i)
    {
        const EffectDefinition& definition = settings.definitions[i];
        ImGui::PushID(definition.name.c_str());
        const float x = ImGui::GetCursorPosX();
        const float labelWidth = std::floor(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(x + labelWidth - S(10));
        ImGui::TextUnformatted(definition.name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        ImGui::SetCursorPosX(x + labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        char value[256] = {};
        definition.value.copy(value, sizeof(value) - 1);
        if (ImGui::InputTextWithHint("##value", definition.fallback.c_str(), value, sizeof(value),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) &&
            definition.value != value)
            host.SetDefinition(effect, i, value);
        if (ImGui::BeginPopupContextItem("reset"))
        {
            if (ImGui::MenuItem("Reset to default"))
                host.SetDefinition(effect, i, "");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

// An effect's settings, under its row.
void SettingsArea(Host& host, const Effect& effect, size_t index, const Page& page, float& y)
{
    const EffectSettings* settings = host.DescribeSettings(index);
    if (!settings)
        return;
    const float left = page.left + S(6);
    ImGui::SetCursorScreenPos(page.At(left, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInset);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGui::BeginChild("settings", ImVec2(page.right - S(6) - left, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    PushSize(kNote);
    if (!settings->tooltip.empty())
        Text(settings->tooltip, kDim, kNote);
    // Grouped by category in the order they first appear, as in ReShade, each open unless its first setting asks
    // otherwise.
    std::vector<const EffectSetting*> categories;
    for (const EffectSetting& setting : settings->settings)
        if (std::none_of(categories.begin(), categories.end(), [&](const EffectSetting* first) { return first->category == setting.category; }))
            categories.push_back(&setting);
    if (categories.empty() && settings->definitions.empty())
        Text("This effect has no settings.", kDim, kNote);
    std::optional<size_t> active, hovered;
    for (const EffectSetting* first : categories)
    {
        const std::string& category = first->category;
        if (!category.empty() && !SettingsHeading(category.c_str(), !first->categoryClosed))
            continue;
        for (size_t i = 0; i < settings->settings.size(); ++i)
        {
            const EffectSetting& setting = settings->settings[i];
            if (setting.category != category)
                continue;
            EffectSetting value;
            if (SettingRow(host, index, i, setting, value))
                host.SetSetting(index, i, value);
            if (ImGui::IsItemActive())
                active = i;
            if (ImGui::IsItemHovered())
                hovered = i;
        }
    }
    host.PointAtSettings(index, active, hovered);
    if (!categories.empty())
    {
        ImGui::Dummy(ImVec2(0, S(2)));
        PushSize(kNote);
        if (Link("Reset all", kDim))
        {
            state->resetting = effect.key;
            state->openResetEffect = true;
        }
        ImGui::PopFont();
        Tooltip("Right-click a setting to reset only that one.");
    }
    DefinitionRows(host, index, *settings);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    y = ImGui::GetItemRectMax().y - page.origin.y + S(8);
}

// Effects that are on can be dragged onto each other to change the order they run in, which from and to take.
void EffectRow(const Effects& effects, Host& host, size_t index, const Page& page, float& y, std::optional<size_t>& from, std::optional<size_t>& to)
{
    const Effect& effect = effects.list[index];
    const bool expanded = state->expanded == effect.key;
    const ImVec2 min = page.At(page.left + S(6), y);
    const ImVec2 size(page.right - page.left - S(12), S(44));
    y += size.y;
    if (!expanded && !ImGui::IsRectVisible(min, min + size))
        return;
    ImGui::PushID(effect.key.c_str());
    ImGui::SetCursorScreenPos(min);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("effect", size, ImGuiButtonFlags_EnableNav))
        state->expanded = expanded ? std::string() : effect.key;
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    if (effect.on)
    {
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("effect", &index, sizeof(index));
            Text(effect.name, kText, kBody);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("effect"))
            {
                from = *static_cast<const size_t*>(payload->Data);
                to = index;
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || expanded)
        draw->AddRectFilled(min, min + size, kCardHover, S(8));
    const ImVec2 switchSize(S(40), S(22));
    ImGui::SetCursorScreenPos(min + ImVec2(S(14), (size.y - switchSize.y) / 2));
    if (kit::Switch("on", effect.on, switchSize))
        host.SetEffect(index, !effect.on);
    const float chevron = min.x + size.x - S(18);
    const float textLeft = min.x + S(14) + switchSize.x + S(14);
    const float nameWidth = std::min(Measure(effect.name, kBody).x, chevron - S(16) - textLeft);
    Line(draw, textLeft, textLeft + nameWidth, min.y + size.y / 2, effect.name, effect.failed ? kDim : kText, kBody);
    Line(draw, textLeft + nameWidth + S(8), chevron - S(16), min.y + size.y / 2 + S(1), effect.failed ? "failed" : effect.file, effect.failed ? kWarning : kDim,
         kNote);
    Chevron(draw, ImVec2(chevron, min.y + size.y / 2), expanded, expanded || hovered ? kText : kDim);
    if (expanded)
        SettingsArea(host, effect, index, page, y);
    ImGui::PopID();
}

// A heading inside a card that opens and closes what is under it, with a count.
bool GroupHeading(const Page& page, float& y, const char* id, const std::string& text, size_t count, bool open)
{
    const ImVec2 min = page.At(page.left + S(6), y);
    const ImVec2 size(page.right - page.left - S(12), S(36));
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    if (hovered)
        page.draw->AddRectFilled(min, min + size, kCardHover, S(8));
    const std::string label = text + "  " + std::to_string(count);
    Line(page.draw, min.x + S(14), min.x + size.x - S(40), min.y + size.y / 2, label, hovered ? kText : kDim, kNote, true);
    Chevron(page.draw, ImVec2(min.x + size.x - S(18), min.y + size.y / 2), open, open || hovered ? kText : kDim);
    y += size.y;
    return clicked;
}

void EffectsSection(const Model& model, Host& host, size_t index, const Page& page, float& y)
{
    const Effects& effects = *model.effects;
    const float headingY = y;
    SectionHeading(page, y, "EFFECTS");
    // All of them on and off.
    ImGui::SetCursorScreenPos(page.At(page.right - S(40), headingY - S(1)));
    if (kit::Switch("all", effects.on, ImVec2(S(40), S(22))))
        host.SetSwitch(Switch::Effects, !effects.on);
    Tooltip(effects.on ? "Turn all effects off" : "Turn effects back on");
    SearchBox(page, y, "##search", "Search effects", state->search, sizeof(state->search));
    y += S(12);
    ListEffects(effects, model.games[index].preset);

    const float top = y;
    BeginCard(page);
    y += S(6);
    const float pad = S(20);
    if (effects.loading)
    {
        const ImVec2 center = page.At(page.left + pad + S(8), y + S(22));
        const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
        page.draw->PathArcTo(center, S(8), start, start + 4.2f, 32);
        page.draw->PathStroke(kAccent, 0, S(2.5f));
        const std::string text = effects.list.empty() ? "Loading effects. The first time can take a minute."
                                                      : "Compiling effects " + std::to_string(effects.loaded) + " of " + std::to_string(effects.total);
        Line(page.draw, center.x + S(20), page.origin.x + page.right - pad, center.y, text, kDim, kBody);
        y += S(44);
    }
    else if (effects.list.empty() && effects.broken.empty())
    {
        y += S(12);
        y += Paragraph(page.draw, page.At(page.left + pad, y), page.origin.x + page.right - pad, "No effects are installed.", kDim, kBody);
        y += S(18);
    }
    std::optional<size_t> from, to;
    const bool searching = state->search[0] != 0;
    if (!effects.list.empty())
    {
        Line(page.draw, page.origin.x + page.left + pad, page.origin.x + page.right - pad, page.origin.y + y + S(16), "Active", kDim, kNote, true);
        y += S(30);
        for (size_t row : state->activeRows)
            EffectRow(effects, host, row, page, y, from, to);
        if (state->activeRows.empty())
        {
            y += S(4);
            y += Paragraph(page.draw, page.At(page.left + pad, y), page.origin.x + page.right - pad,
                           searching ? "No active effects match." : "No effects are on. Pick a preset or turn effects on below.", kDim, kBody);
            y += S(12);
        }
        RowSeparator(page, y + S(6));
        y += S(12);
        const bool open = searching || state->showAll;
        if (GroupHeading(page, y, "all", "All effects", state->otherRows.size(), open) && !searching)
            state->showAll = !state->showAll;
        if (open)
            for (size_t row : state->otherRows)
                EffectRow(effects, host, row, page, y, from, to);
    }
    if (!effects.broken.empty())
    {
        if (!effects.list.empty())
        {
            RowSeparator(page, y + S(6));
            y += S(12);
        }
        const bool open = !state->shownErrors.empty();
        if (GroupHeading(page, y, "broken", "Did not compile", effects.broken.size(), open))
            state->shownErrors = open ? std::string() : effects.broken.front().file;
        for (const BrokenEffect& broken : effects.broken)
        {
            if (!open)
                break;
            ImGui::PushID(broken.file.c_str());
            const bool shown = state->shownErrors == broken.file;
            const ImVec2 min = page.At(page.left + S(6), y);
            const ImVec2 size(page.right - page.left - S(12), S(40));
            ImGui::SetCursorScreenPos(min);
            if (ImGui::InvisibleButton("errors", size, ImGuiButtonFlags_EnableNav))
                state->shownErrors = broken.file;
            const bool hovered = ImGui::IsItemHovered();
            HandOnHover();
            if (hovered || shown)
                page.draw->AddRectFilled(min, min + size, kCardHover, S(8));
            Line(page.draw, min.x + S(14), min.x + size.x - S(14), min.y + size.y / 2, broken.file, kText, kBody);
            y += size.y;
            if (shown)
            {
                y += S(4);
                y += Paragraph(page.draw, page.At(page.left + pad, y), page.origin.x + page.right - pad, broken.errors, kDim, kNote);
                y += S(10);
            }
            ImGui::PopID();
        }
    }
    y += S(8);
    EndCard(page, top, y);
    y += S(28);
    if (from && to && *from != *to)
        host.MoveEffect(*from, *to);
}

void GamePage(const Model& model, Host& host, size_t index, const Page& page, float& y, float windowLeft, float windowWidth)
{
    const Game& game = model.games[index];
    if (place == Place::Game)
        CompactHero(model, index, page, y, windowLeft, windowWidth);
    else
        GameHero(model, host, index, page, y, windowLeft, windowWidth);
    PresetsSection(model, host, index, page, y);
    if (model.effects && game.running)
        EffectsSection(model, host, index, page, y);
    if (game.performance)
    {
        SectionHeading(page, y, "PERFORMANCE");
        const float top = y;
        BeginCard(page);
        PerformanceRows(page, y, host, *game.performance, index, game.id);
        EndCard(page, top, y);
        y += S(28);
    }
}

// Settings

void PageTitle(const Page& page, float& y, const char* title)
{
    Line(page.draw, page.origin.x + page.left, page.origin.x + page.right, page.origin.y + y + S(16), title, kText, kPageTitle, true);
    y += S(32) + S(18);
}

void GeneralSettings(const Settings& settings, Host& host, const Page& page, float& y)
{
    const auto section = [&](const char* heading, auto&& rows) {
        SectionHeading(page, y, heading);
        const float top = y;
        BeginCard(page);
        rows();
        EndCard(page, top, y);
        y += S(24);
    };
    if (settings.startWithWindows)
        section("STARTUP", [&] {
            if (SwitchRow(page, y, "start", *settings.startWithWindows, "Start with Windows", "Starts in the notification area when you sign in."))
                host.SetSwitch(Switch::StartWithWindows, !*settings.startWithWindows);
        });
    section("PRESETS", [&] {
        if (SwitchRow(page, y, "autosave", settings.autoSavePresets, "Save changes automatically",
                      "Turn off to try changes first, then save or discard them."))
            host.SetSwitch(Switch::AutoSavePresets, !settings.autoSavePresets);
    });
    if (settings.skipTitleBar)
        section("GAME WINDOW", [&] {
            if (SwitchRow(page, y, "titlebar", *settings.skipTitleBar, "Skip the title bar",
                          "Effects leave a windowed game's title bar alone. Turn off for borderless games in a window."))
                host.SetSwitch(Switch::SkipTitleBar, !*settings.skipTitleBar);
        });
    if (settings.downloadEffects || settings.reloadEffects)
        section("EFFECTS", [&] {
            if (settings.reloadEffects && ButtonRow(page, y, "Reload##effects", true, "Reload effects", "Compiles every effect again, such as after you change one."))
                host.ReloadEffects();
            if (settings.reloadEffects && settings.downloadEffects)
                RowSeparator(page, y);
            if (settings.downloadEffects &&
                ButtonRow(page, y, "Download again##effects", *settings.downloadEffects, "Download effects and presets again",
                          "Gets every package from ReShade's list again, such as ones that were skipped. Your presets are kept."))
                host.DownloadEffects();
        });
    if (settings.menuSize || settings.keepEffectsVisible)
        section("DISPLAY", [&] {
            if (settings.menuSize)
            {
                const float pad = S(20);
                const float button = S(32);
                const float value = S(64);
                const float controls = button * 2 + value;
                const float textHeight = SettingText(page, page.left + pad, page.right - pad - controls - S(20), y + S(16), "Menu size",
                                                     "On top of the size that follows the game's window. Changes right away.");
                const float height = std::max(textHeight, button) + S(32);
                const float controlTop = y + (height - button) / 2;
                const float controlLeft = page.right - pad - controls;
                if (PlacedButton("-##menu", page.At(controlLeft, controlTop), ImVec2(button, button), false, settings.canShrinkMenu))
                    host.StepMenuSize(-1);
                Line(page.draw, page.origin.x + controlLeft + button, page.origin.x + controlLeft + button + value, page.origin.y + controlTop + button / 2,
                     std::to_string(*settings.menuSize) + "%", kText, kBody, false, true);
                if (PlacedButton("+##menu", page.At(controlLeft + button + value, controlTop), ImVec2(button, button), false, settings.canGrowMenu))
                    host.StepMenuSize(1);
                y += height;
            }
            if (settings.keepEffectsVisible)
            {
                if (settings.menuSize)
                    RowSeparator(page, y);
                if (SwitchRow(page, y, "keep", *settings.keepEffectsVisible, "Keep effects visible when another window is in front",
                              "Effects stay over the game while another window, such as a chat or a browser, is in front of it."))
                    host.SetSwitch(Switch::KeepEffectsVisible, !*settings.keepEffectsVisible);
            }
        });
    if (settings.updateChecks)
        section("UPDATES", [&] {
            if (SwitchRow(page, y, "updates", *settings.updateChecks, "Check for updates",
                          "Asks GitHub for a newer version when Unishade starts. Applies from the next start."))
                host.SetSwitch(Switch::UpdateChecks, !*settings.updateChecks);
        });
}

void PerformanceSettings(const Settings& settings, Host& host, const Page& page, float& y)
{
    y += Paragraph(page.draw, page.At(page.left, y), page.origin.x + page.right,
                   "For every game without its own settings. A game's page changes its own.", kDim, kBody) +
         S(18);
    const float top = y;
    BeginCard(page);
    PerformanceRows(page, y, host, *settings.performance, std::nullopt, {});
    EndCard(page, top, y);
    y += S(24);
    if (settings.debugInfo)
    {
        SectionHeading(page, y, "DEBUG");
        const float debugTop = y;
        BeginCard(page);
        if (SwitchRow(page, y, "debug", *settings.debugInfo, "Show debug info", "Captured game FPS, output FPS and frame loss."))
            host.SetSwitch(Switch::DebugInfo, !*settings.debugInfo);
        EndCard(page, debugTop, y);
        y += S(24);
    }
}

void ShortcutSettings(const Settings& settings, Host& host, const Page& page, float& y)
{
    y += Paragraph(page.draw, page.At(page.left, y), page.origin.x + page.right, "Click a shortcut, then press the keys you want. They work right away.",
                   kDim, kBody) +
         S(18);
    const float top = y;
    BeginCard(page);
    const float pad = S(20);
    const float keyWidth = S(150);
    const float clearWidth = S(28);
    bool recordHovered = false;
    for (size_t i = 0; i < settings.shortcuts.size(); ++i)
    {
        const Shortcut& shortcut = settings.shortcuts[i];
        ImGui::PushID(static_cast<int>(i));
        if (i)
            RowSeparator(page, y);
        const float keyLeft = page.right - pad - clearWidth - S(8) - keyWidth;
        const float textHeight = SettingText(page, page.left + pad, keyLeft - S(20), y + S(14), shortcut.title, shortcut.description);
        const float height = std::max(textHeight, S(34)) + S(28);
        const float buttonTop = y + (height - S(34)) / 2;
        const bool recording = settings.recording == static_cast<int>(i);
        const ImVec2 keyMin = page.At(keyLeft, buttonTop);
        ImGui::SetCursorScreenPos(keyMin);
        if (ImGui::InvisibleButton("keys", ImVec2(keyWidth, S(34)), ImGuiButtonFlags_EnableNav))
            host.RecordShortcut(recording ? -1 : static_cast<int>(i));
        const bool hovered = ImGui::IsItemHovered();
        recordHovered |= hovered && settings.recording >= 0;
        HandOnHover();
        page.draw->AddRectFilled(keyMin, keyMin + ImVec2(keyWidth, S(34)), hovered ? kBorder : kCardHover, S(8));
        page.draw->AddRect(keyMin, keyMin + ImVec2(keyWidth, S(34)), recording ? kAccent : hovered ? kBorderStrong : kBorder, S(8));
        const std::string label = recording ? "Press keys..." : shortcut.keys.empty() ? "Not set" : shortcut.keys;
        Line(page.draw, keyMin.x + S(8), keyMin.x + keyWidth - S(8), keyMin.y + S(17), label, recording || !shortcut.keys.empty() ? kText : kDim, kBody, false,
             true);
        if (!shortcut.required && !shortcut.keys.empty())
        {
            if (IconButton("clear", page.At(page.right - pad - clearWidth, buttonTop + S(3)), clearWidth, CrossIcon, "Leave unassigned", true))
                host.ClearShortcut(i);
        }
        y += height;
        ImGui::PopID();
    }
    EndCard(page, top, y);
    // Clicking anything but the shortcut that waits for keys stops the wait.
    if (settings.recording >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !recordHovered)
        host.RecordShortcut(-1);
    y += S(14);
    const auto line = [&](const std::string& text, ImU32 color) { y += Paragraph(page.draw, page.At(page.left, y), page.origin.x + page.right, text, color, kBody) + S(6); };
    if (!settings.shortcutError.empty())
        line(settings.shortcutError, kError);
    for (const std::string& warning : settings.shortcutWarnings)
        line(warning, kWarning);
    if (PlacedLink("Reset to defaults", page.At(page.left, y + S(4)), S(20)))
        state->confirmReset = true;
    y += S(24) + S(24);
}

void SettingsPageView(const Model& model, Host& host, const Page& page, float& y)
{
    const Settings& settings = model.settings;
    PageTitle(page, y, "Settings");
    std::vector<SettingsPage> pages{ SettingsPage::General };
    if (settings.performance)
        pages.push_back(SettingsPage::Performance);
    if (!settings.shortcuts.empty())
        pages.push_back(SettingsPage::Shortcuts);
    if (std::find(pages.begin(), pages.end(), state->settingsPage) == pages.end())
        state->settingsPage = SettingsPage::General;
    if (pages.size() > 1)
    {
        const int selected = static_cast<int>(std::find(pages.begin(), pages.end(), state->settingsPage) - pages.begin());
        ImGui::SetCursorScreenPos(page.At(page.left, y));
        const int clicked = pages.size() == 3 ? Segmented("pages", { "General", "Performance", "Shortcuts" }, selected, kBorder, S(420))
                                              : Segmented("pages", { "General", "Shortcuts" }, selected, kBorder, S(300));
        if (clicked >= 0 && pages[clicked] != state->settingsPage)
        {
            // A shortcut left waiting for keys on its page would take the next key pressed on another.
            host.RecordShortcut(-1);
            state->settingsPage = pages[clicked];
            state->rateFor.reset();
        }
        y += S(30) + S(26);
    }
    switch (state->settingsPage)
    {
    case SettingsPage::General: GeneralSettings(settings, host, page, y); break;
    case SettingsPage::Performance: PerformanceSettings(settings, host, page, y); break;
    case SettingsPage::Shortcuts: ShortcutSettings(settings, host, page, y); break;
    }
}

void DiscordPage(const Model& model, Host& host, const Page& page, float& y)
{
    const Discord& discord = *model.discord;
    PageTitle(page, y, "Discord");
    const float top = y;
    BeginCard(page);
    if (SwitchRow(page, y, "presence", discord.on, "Show on Discord", "Your Discord profile shows the game Unishade is running on, with its icon and your preset."))
        host.SetSwitch(Switch::DiscordPresence, !discord.on);
    if (discord.on && !discord.status.empty())
    {
        RowSeparator(page, y);
        const ImU32 color = discord.level == Level::Ok ? kSuccess : discord.level == Level::Warning ? kWarning : discord.level == Level::Error ? kError : kDim;
        y += S(16);
        page.draw->AddCircleFilled(page.At(page.left + S(26), y + S(10)), S(4), color);
        y += Paragraph(page.draw, page.At(page.left + S(40), y), page.origin.x + page.right - S(20), discord.status, discord.level == Level::Info ? kDim : kText, kBody);
        y += S(16);
    }
    EndCard(page, top, y);
    y += S(24);
}

// Dialogs

void PickerDialog(const Model& model, Host& host)
{
    const bool open = ImGui::IsPopupOpen("##picker");
    if (model.picker && !open)
        ImGui::OpenPopup("##picker");
    const ImGuiIO& io = ImGui::GetIO();
    const float width = std::min(S(kPickerWidth), io.DisplaySize.x - S(32));
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (!ImGui::BeginPopupModal("##picker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();
    if (!model.picker)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Picker& picker = *model.picker;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float pad = S(24);
    float y = S(22);
    Line(draw, origin.x + pad, origin.x + width - pad, origin.y + y + S(12), picker.add ? "Add a game" : "Pick a window", kText, kSemibold, true);
    y += S(24) + S(6);
    y += Paragraph(draw, origin + ImVec2(pad, y), origin.x + width - pad,
                   picker.add ? "Pick your game's window. If it isn't here, open the game first." : "Unishade uses this window until you click Detect automatically.",
                   kDim, kBody) +
         S(16);

    const float rowHeight = S(52);
    const float listHeight = std::min(static_cast<float>(std::max<size_t>(picker.windows.size(), 1)) * rowHeight + S(8), S(kPickerListHeight));
    const ImVec2 listMin = origin + ImVec2(pad, y);
    RoundedCard(draw, listMin, listMin + ImVec2(width - pad * 2, listHeight));
    ImGui::SetCursorScreenPos(listMin + ImVec2(1, S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6));
    ImGui::BeginChild("windows", ImVec2(width - pad * 2 - 2, listHeight - S(8)), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    std::optional<size_t> chosen;
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float rowWidth = width - pad * 2 - 2;
        if (picker.windows.empty())
            Line(list, top.x + S(18), top.x + rowWidth, top.y + rowHeight / 2, "No open windows.", kDim, kBody);
        for (size_t i = 0; i < picker.windows.size(); ++i)
        {
            const Window& window = picker.windows[i];
            const ImVec2 min = top + ImVec2(0, static_cast<float>(i) * rowHeight);
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetCursorScreenPos(min);
            if (ImGui::InvisibleButton("window", ImVec2(rowWidth, rowHeight), ImGuiButtonFlags_EnableNav))
                chosen = i;
            HandOnHover();
            if (ImGui::IsItemHovered())
                list->AddRectFilled(min + ImVec2(S(4), S(2)), min + ImVec2(rowWidth - S(4), rowHeight - S(2)), kCardHover, S(8));
            else if (i)
                list->AddLine(min + ImVec2(S(14), 0), min + ImVec2(rowWidth - S(14), 0), kBorder);
            const float icon = std::round(S(32));
            const ImVec2 iconMin = min + ImVec2(S(14), (rowHeight - icon) / 2);
            GamePicture(list, iconMin, icon, window.title, window.picture, kSemibold, S(6));
            const float nameLeft = iconMin.x + icon + S(12);
            Line(list, nameLeft, min.x + rowWidth - S(14), min.y + rowHeight / 2 - S(9), window.title, kText, kBody, true);
            Line(list, nameLeft, min.x + rowWidth - S(14), min.y + rowHeight / 2 + S(10), window.detail, kDim, kNote);
            ImGui::PopID();
        }
        // Scrolls only when the rows do not fit.
        ImGui::SetCursorScreenPos(top + ImVec2(0, static_cast<float>(picker.windows.size()) * rowHeight));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::EndChild();
    y += listHeight + S(16);
    const float cancelWidth = ButtonWidth("Cancel");
    const bool cancel = PlacedButton("Cancel", origin + ImVec2(width - pad - cancelWidth, y), ImVec2(cancelWidth, S(32)));
    y += S(32) + S(20);
    ImGui::SetCursorScreenPos(origin + ImVec2(0, y));
    ImGui::Dummy(ImVec2(width, 0));

    const bool outside = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                         !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (chosen)
        host.ChooseWindow(*chosen);
    else if (cancel || outside || ImGui::IsKeyPressed(ImGuiKey_Escape))
        host.ClosePicker();
    if (chosen || cancel || outside || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ResetDialog(Host& host)
{
    if (!BeginDialog("##reset", state->confirmReset, 400))
        return;
    DialogText("Reset every shortcut?", "They go back to the keys Unishade starts with.");
    const int clicked = DialogButtons({ "Cancel", "Reset" });
    if (clicked == 1)
        host.ResetShortcuts();
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

std::string PresetName(const Model& model, const std::string& id)
{
    for (const Game& game : model.games)
        for (const Preset& preset : game.presets)
            if (preset.id == id)
                return preset.name;
    return {};
}

void PresetNameDialog(const Model& model, Host& host)
{
    if (!BeginDialog("##presetname", state->openPresetName, 400))
        return;
    const char* titles[] = { "New preset", "Duplicate preset", "Rename preset" };
    const char* actions[] = { "Create", "Duplicate", "Rename" };
    const int action = static_cast<int>(state->presetAction);
    DialogText(titles[action], state->presetAction == PresetAction::New ? "Starts with every effect off." : "");
    if (ImGui::IsWindowAppearing() || std::exchange(state->focusPresetName, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputText("##name", state->presetName, sizeof(state->presetName), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
        state->presetFailed = false;
    if (state->presetFailed && !model.presetProblem.empty())
        Text(model.presetProblem, kError, kNote);
    const int clicked = DialogButtons({ "Cancel", actions[action] });
    if (clicked == 1 || enter)
    {
        bool done = false;
        switch (state->presetAction)
        {
        case PresetAction::New: done = host.NewPreset(state->presetGame, state->presetName); break;
        case PresetAction::Copy: done = host.CopyPreset(state->presetGame, state->presetTarget, state->presetName); break;
        case PresetAction::Rename: done = host.RenamePreset(state->presetTarget, state->presetName); break;
        }
        if (done)
            ImGui::CloseCurrentPopup();
        else
        {
            state->presetFailed = true;
            state->focusPresetName = true;
        }
    }
    if (clicked == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Asks what to do with the changes to the preset in use before switching. The switch waits for the answer.
void UnsavedDialog(const Model& model, Host& host)
{
    if (!BeginDialog("##unsaved", state->openUnsaved, 420))
        return;
    std::string name;
    if (state->pendingGame < model.games.size())
        name = PresetName(model, model.games[state->pendingGame].preset);
    DialogText("Save your changes to " + name + "?", "Discarding goes back to how the preset was last saved.");
    const int clicked = DialogButtons({ "Discard", "Cancel", "Save" });
    if (clicked == 0)
        host.DiscardChanges();
    else if (clicked == 2)
        host.SaveChanges();
    if (clicked == 0 || clicked == 2)
        host.UsePreset(state->pendingGame, state->pendingPreset);
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        state->pendingPreset.clear();
        ImGui::CloseCurrentPopup();
    }
    EndDialog();
}

// Unishade quits once the changes to the preset in use are saved or dropped.
void QuitDialog(const Model& model, Host& host)
{
    if (place != Place::Window)
        return;
    bool open = !model.quitting.empty() && !ImGui::IsPopupOpen("##quit");
    if (!BeginDialog("##quit", open, 420))
        return;
    if (model.quitting.empty())
    {
        ImGui::CloseCurrentPopup();
        EndDialog();
        return;
    }
    DialogText("Save the changes to " + model.quitting + " before quitting?", "Without saving, they are lost.");
    const int clicked = DialogButtons({ "Don't save", "Cancel", "Save" });
    if (clicked == 0 || clicked == 2)
        host.Quit(clicked == 2);
    else if (clicked == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        host.KeepRunning();
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

void DeleteDialog(const Model& model, Host& host)
{
    if (!BeginDialog("##delete", state->openDelete, 380))
        return;
#ifdef _WIN32
    constexpr char kTrash[] = "It moves to the Recycle Bin.";
#else
    constexpr char kTrash[] = "It moves to the Trash.";
#endif
    DialogText("Delete " + PresetName(model, state->deleting) + "?", kTrash);
    const int clicked = DialogButtons({ "Cancel", "Delete" });
    if (clicked == 1)
        host.DeletePreset(state->deleting);
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Asks before resetting an effect's settings, which cannot be got back with auto-save on.
void ResetEffectDialog(const Model& model, Host& host)
{
    if (!BeginDialog("##reseteffect", state->openResetEffect, 400))
        return;
    std::optional<size_t> effect;
    if (model.effects)
        for (size_t i = 0; i < model.effects->list.size() && !effect; ++i)
            if (model.effects->list[i].key == state->resetting)
                effect = i;
    DialogText("Reset every setting of " + (effect ? model.effects->list[*effect].name : std::string()) + "?", "They go back to their defaults.");
    const int clicked = DialogButtons({ "Cancel", "Reset" });
    if (clicked == 1 && effect)
        host.ResetSettings(*effect, std::nullopt);
    if (clicked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    EndDialog();
}

// Follows the model: a game added since the last frame shows its page, and a page whose game is gone goes Home.
void Follow(const Model& model)
{
    if (!state->started && place == Place::Game)
    {
        ImGui::ClosePopupsOverWindow(nullptr, false);
        const auto played = std::find_if(model.games.begin(), model.games.end(), [](const Game& game) { return game.running; });
        if (played != model.games.end())
            ShowPage(PageKind::Game, played->id);
        else
            ShowPage(PageKind::Home);
    }
    std::string added;
    for (const Game& game : model.games)
        if (state->known.insert(game.id).second && state->started)
            added = game.id;
    state->started = true;
    if (!added.empty())
        ShowPage(PageKind::Game, added);
    const auto exists = [&](const std::string& id) { return std::any_of(model.games.begin(), model.games.end(), [&](const Game& game) { return game.id == id; }); };
    if (state->page == PageKind::Game && !exists(state->game))
        ShowPage(PageKind::Home);
    if (state->page == PageKind::Discord && !model.discord)
        ShowPage(PageKind::Home);
    if (!state->renaming.empty() && !exists(state->renaming))
        state->renaming.clear();
}
} // namespace

void Draw(const Model& model, Host& host, float windowScale, Place where)
{
    place = where;
    state = &states[static_cast<size_t>(where)];
    const bool panel = where == Place::Game;
    scale = windowScale * (panel ? kPanelScale : 1.0f);
    ApplyStyle(ImGui::GetStyle());
    ImGui::GetStyle().FrameRounding = S(8);
    // Over the game Escape goes back to it, unless it closes a popup, ends typing or stops waiting for a shortcut.
    const bool leave = panel && model.settings.recording < 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
                       !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    Follow(model);
    const ImGuiIO& io = ImGui::GetIO();
    PushSize(kBody);

    // Over the game, a panel at its side with square top corners, so the strip of colors runs along its top.
    const float margin = panel ? S(kPanelMargin) : 0;
    const ImVec2 origin(margin, margin);
    const ImVec2 size = panel ? ImVec2(std::min(S(kPanelWidth), io.DisplaySize.x - margin * 2), io.DisplaySize.y - margin * 2) : io.DisplaySize;
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, panel ? IM_COL32(0, 0, 0, 0) : kBackground);
    ImGui::Begin(panel ? "panel" : "launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (panel)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, origin + size, kBackground, S(12), ImDrawFlags_RoundCornersBottom);
        draw->AddRect(origin, origin + size, kBorder, S(12), 1.0f, ImDrawFlags_RoundCornersBottom);
    }

    const float height = size.y - (panel ? S(kFooter) : 0);
    if (panel)
        Rail(model, origin, size.y);
    else
        Sidebar(model, host, origin, size.y);

    // The page scrolls under the strip.
    const float left = origin.x + (panel ? S(kRailWidth) : S(kSidebarWidth));
    const float width = std::max(origin.x + size.x - left, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(left, origin.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(8));
    // Each page keeps its own scroll.
    const std::string id = state->page == PageKind::Game ? "page " + state->game : "page " + std::to_string(static_cast<int>(state->page));
    ImGui::BeginChild(id.c_str(), ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    {
        // In the middle of a window wider than the page.
        const float padding = S(panel ? kPanelPadding : kPagePadding);
        const float pageWidth = std::min(width - padding * 2, S(kPageWidth));
        const float pageLeft = std::max(padding, std::floor((width - pageWidth) / 2));
        const Page page{ ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), pageLeft, pageLeft + pageWidth };
        float y = S(kStrip) + (panel ? S(24) : S(32));
        switch (state->page)
        {
        case PageKind::Home: HomePage(model, host, page, y); break;
        case PageKind::Game:
        {
            const auto game = std::find_if(model.games.begin(), model.games.end(), [](const Game& game) { return game.id == state->game; });
            GamePage(model, host, static_cast<size_t>(game - model.games.begin()), page, y, left, width);
            break;
        }
        case PageKind::Settings: SettingsPageView(model, host, page, y); break;
        case PageKind::Discord: DiscordPage(model, host, page, y); break;
        }
        ImGui::SetCursorScreenPos(page.At(0, y + S(24)));
        ImGui::Dummy(ImVec2(1, 1));
    }
    ImGui::EndChild();

    if (panel)
        Footer(model, host, ImVec2(left, origin.y + height), ImVec2(width, size.y - height));
    // Over the pages, which scroll under it.
    Rainbow(ImGui::GetForegroundDrawList(), origin, origin + ImVec2(size.x, S(kStrip)));
    PickerDialog(model, host);
    ResetDialog(host);
    PresetNameDialog(model, host);
    UnsavedDialog(model, host);
    DeleteDialog(model, host);
    QuitDialog(model, host);
    ResetEffectDialog(model, host);
    ImGui::End();
    ImGui::PopFont();
    if (leave)
        host.CloseMenu();
}

void Reset(Place where)
{
    State& reset = states[static_cast<size_t>(where)];
    reset.started = false;
    reset.known.clear();
    reset.renaming.clear();
    reset.rateFor.reset();
    reset.pendingPreset.clear();
    reset.openPresetName = reset.openUnsaved = reset.openDelete = reset.openResetEffect = reset.confirmReset = false;
    reset.active.clear();
    reset.rowsStale = true;
}
} // namespace launcher
