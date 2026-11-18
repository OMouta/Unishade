#pragma once

// Unishade's look in Dear ImGui: its colors, text sizes and controls, for the launcher in its window and over the game.
// Sizes are given at 100% scaling and drawn at scale.

#include "launcher_ui.h"
#include "theme.h"

#include <imgui.h>

#include <initializer_list>
#include <span>
#include <string>
#include <vector>

struct ImGuiWindow;

namespace kit
{
constexpr ImU32 Color(unsigned rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

constexpr ImU32 kBackground = Color(theme::kBackground);
constexpr ImU32 kCard = Color(theme::kCard);
constexpr ImU32 kCardHover = Color(theme::kCardHover);
constexpr ImU32 kBorder = Color(theme::kBorder);
constexpr ImU32 kBorderStrong = Color(theme::kBorderStrong);
constexpr ImU32 kText = Color(theme::kText);
constexpr ImU32 kDim = Color(theme::kDim);
constexpr ImU32 kAccent = Color(theme::kAccent);
constexpr ImU32 kAccentHover = Color(theme::kAccentHover);

// The scale of the window being drawn.
extern float scale;

inline float S(float value)
{
    return value * scale;
}

// A font file, and which font of a collection (.ttc) to use, or the bytes of a font built into the program, or a
// loader that draws the characters itself, as for emoji the system only has as pictures.
struct FontFile
{
    std::string path;
    int index = 0;
    std::span<const unsigned char> data;
    const ImFontLoader* loader = nullptr;
};

// Loads the current Dear ImGui context's fonts, regular and bold, each followed by fallbacks for the characters it
// lacks, such as Chinese, Japanese, Korean or emoji. Files are mapped into memory, so only the parts that are drawn
// get read, and missing files are left out. Without a regular font, Dear ImGui's own is used.
// Bold text as large as a title uses heavy when there is one, with the bold font's fallbacks.
void LoadFonts(const FontFile& regular, const std::vector<FontFile>& fallbacks, const FontFile& bold, const std::vector<FontFile>& boldFallbacks,
               const FontFile& heavy = {});
// The bold font of the current Dear ImGui context. Without one, bold text uses the regular font.
void SetBoldFont(ImFont* bold);
// Forgets what was kept for a context that is being destroyed.
void ForgetContext(ImGuiContext* context);

void PushSize(float size, bool bold = false);
ImVec2 Measure(const std::string& text, float size, bool bold = false, float wrap = -1.0f);
void ApplyStyle(ImGuiStyle& style);

// Wraps at the edge of the window, at wrap in window coordinates when given, or not at all when wrap is negative.
void Text(const std::string& text, ImU32 color = kText, float size = 14.5f, float wrap = 0.0f, bool bold = false);
void HandOnHover();
void KeyCap(const std::string& key, float size = 13);
// Points down when open and right when closed.
void Chevron(ImDrawList* draw, ImVec2 center, bool open, ImU32 color);
// The first letter of a name, which stands in for a missing icon.
std::string Initial(const std::string& name);

// One line of text between left and right, cut off at right, centered up and down around middle.
void Line(ImDrawList* draw, float left, float right, float middle, const std::string& text, ImU32 color, float size, bool bold = false,
          bool centered = false);
// Text wrapped between position and right. Returns its height.
float Paragraph(ImDrawList* draw, ImVec2 position, float right, const std::string& text, ImU32 color, float size, bool bold = false);

// A picture the host names, such as a game's icon, as a texture drawn size wide and high in the window's units.
// ImTextureID_Invalid when there is none. Each platform makes its own textures.
ImTextureID Picture(const std::string& key, float size);
// The average color of a picture's opaque pixels, or 0 when there is none. Only after Picture made it.
ImU32 PictureTint(const std::string& key);

// The picture of the game being played, as the host has it with effects.
enum class GameView
{
    Frost,   // over the game: blurred and saturated, behind the tray, while the menu shows
    Preview, // in the launcher's window: smaller, refreshed while the overlay draws
    Shot,    // over the game: the screenshot just taken
};
// The whole picture as a texture, which stretches over the game's window, or ImTextureID_Invalid where the host has
// none. size gets its size in pixels, for its shape. Asking keeps it coming: the host makes the preview only while
// the launcher asked for it within the last second.
ImTextureID GameImage(GameView view, ImVec2* size = nullptr);

// Animation

// How an animation gets from one value to the next, as the CSS curves the design was drawn with.
enum class Curve
{
    Out,    // quick, then settling: cubic-bezier(.2, .9, .2, 1)
    In,     // slow, then quick, for things that go away: cubic-bezier(.5, 0, .8, .3)
    EaseIn, // CSS ease-in
    Leave,  // the tray dropping out of sight: cubic-bezier(.5, 0, .9, .4)
    Spring, // past the end and back, as the design's linear() spring
    Linear,
};
// Where a curve is at t from 0 to 1. The spring goes past 1 on the way.
float Ease(Curve curve, float t);
// A value that moves to each new target over seconds instead of jumping there, kept by id in the current Dear ImGui
// context. It starts at its first target. Time is Dear ImGui's, the sum of its frames' delta times, so it runs alike at
// any frame rate. An animation not asked for in a second is forgotten, and starts at its target again.
float Animate(ImGuiID id, float target, float seconds, Curve curve = Curve::Out, float delay = 0);
ImVec2 Animate(ImGuiID id, ImVec2 target, float seconds, Curve curve = Curve::Out, float delay = 0);
// Puts an animation at a value at once, such as where something appears from before it animates to its place.
void Snap(ImGuiID id, float value);
void Snap(ImGuiID id, ImVec2 value);
// Whether anything drawn lately in the current Dear ImGui context is still on its way to its target, so the next
// frame should come at once rather than with the next event.
bool Animating();
// Whether an animation was asked for within the last second.
bool Animated(ImGuiID id);
// An id for one part of something with an id, such as the press of a button.
ImGuiID SubId(ImGuiID id, const char* part);

// Changes what draw got since its vertex start: scales and turns it by angle around pivot, moves it by offset and
// multiplies its alpha. Dear ImGui cannot transform what it draws, so this is how things scale, turn and fade.
void Transform(ImDrawList* draw, int start, ImVec2 pivot, float scale, float angle = 0, ImVec2 offset = ImVec2(0, 0), float alpha = 1);
// Multiplies the alpha of what a window and the windows in it drew so far in this frame.
void FadeWindow(ImGuiWindow* window, float alpha);

// Spectrum: the design of the tray. Near-white frosted blocks with black type, and every effect owns one color of the
// ring. Sizes are the CSS pixels of the design at 100%, which S() scales.
namespace spectrum
{
// A text size from the CSS font size of the design. CSS sizes text by its em, Dear ImGui by the font's ascent plus
// descent, which is 1.2 em in Bricolage Grotesque.
constexpr float Css(float pixels)
{
    return pixels * 1.2f;
}
constexpr float kTextBody = Css(16);
constexpr float kTextSmall = Css(14.4f);
constexpr float kTextTile = Css(14.08f);
constexpr float kTextTileSmall = Css(12.16f);
constexpr float kTextHeading = Css(15.2f);
constexpr float kTextName = Css(20.8f);
constexpr float kTextTitle = Css(28.8f);
constexpr float kTextTitleSide = Css(22.4f);

// The tray's translucent white. Over the game it lies on GameView::Frost, the design's backdrop blur, where the host
// has it.
constexpr ImU32 kTray = IM_COL32(247, 247, 251, 219);
constexpr ImU32 kPaper = Color(0xF7F7FB);
constexpr ImU32 kInk = IM_COL32(0, 0, 0, 255);
constexpr ImU32 kMist = Color(0x62626F);
constexpr ImU32 kWell = IM_COL32(0, 0, 0, 18);
constexpr ImU32 kWellHover = IM_COL32(0, 0, 0, 33);
constexpr ImU32 kFaint = IM_COL32(0, 0, 0, 64);
constexpr ImU32 kQuiet = Color(0xC9C9D3);
constexpr ImU32 kDanger = Color(0xC8243A);
constexpr ImU32 kCaution = Color(0x9A5B00);
// The ring's colors, repeating after seven.
ImU32 Ring(size_t index);
// color with its alpha multiplied.
ImU32 Alpha(ImU32 color, float alpha);
ImU32 Mix(ImU32 from, ImU32 to, float t);

// Dear ImGui's colors for the tray's own controls, popups and text boxes: black on white.
void ApplyStyle(ImGuiStyle& style);
// A tip in the tray's colors, once the mouse rests on the last item.
void Tip(const std::string& text);
// A thin bar at the right edge of a child window that just ended, where it is and how much there is, while its
// contents do not fit. The windows scroll without Dear ImGui's bars, so this is what says there is more.
void ScrollCue(const ImGuiWindow* child, ImU32 color = kFaint);

enum class Icon
{
    Left,
    Right,
    Plus,
    Close,
    Check,
    Camera,
    Power,
    Sliders,
    Dots,
};
// The design's icons, drawn in a size wide box around center and turned by angle.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color, float angle = 0);

// A shadow under a rounded box, offset down by y and spread over blur, as CSS's box-shadow, faked with layers.
void Shadow(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float y, float blur, float alpha);
// A convex polygon, clockwise, filled where it is inside a rounded box. Shapes that grow inside rounded boxes use it.
void FillInside(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, std::span<const ImVec2> polygon, ImU32 color);
// A circle filled where it is inside a rounded box.
void CircleInside(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, ImVec2 center, float radius, ImU32 color);
void DashedBox(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, ImU32 color, float thickness);
// The ring's colors around a disc, turned by angle.
void RingDisc(ImDrawList* draw, ImVec2 center, float radius, float angle, float alpha);
// The ring's colors from left to right, filled to fraction.
void RainbowLine(ImDrawList* draw, ImVec2 min, ImVec2 max, float fraction, float alpha);
// A name broken into lines that fit width, between words and where a lowercase letter meets a capital, as effects
// name themselves.
std::vector<std::string> WrapName(const std::string& name, float size, bool bold, float width);

// A key's name in a black cap, as high as height. Returns its width.
float KeyWidth(const std::string& key);
void KeyLabel(ImDrawList* draw, ImVec2 min, float height, const std::string& key, ImU32 fill = kInk, ImU32 text = IM_COL32_WHITE);

constexpr float kRoundSize = 40;
// A round button with an icon, black while active. The icon turns by turn while active. Returns true when clicked.
bool RoundButton(const char* id, ImVec2 min, Icon icon, const char* tip, bool active = false, float size = kRoundSize, float turn = 0);
constexpr float kPowerSize = 44.8f;
// The button that turns every effect on and off: the ring's colors around it while on.
bool PowerButton(const char* id, ImVec2 min, bool on, const char* tip);

enum class PillStyle
{
    Well,  // grey, black while held
    Black, // the main button
    Text,  // grey text, which shows its grey only under the mouse
};
float PillWidth(const char* label, PillStyle style);
// A rounded button with a label, as high as height. The label stops at ##. Returns true when clicked.
bool PillButton(const char* label, ImVec2 min, float height, PillStyle style, bool enabled = true);

constexpr float kSegmentHeight = 41.6f;
// Choices side by side, with a black pill under the selected one that slides when the choice changes. As tabs, they
// sit in a grey well. Returns the index of the choice clicked, or -1.
float SegmentsWidth(std::span<const char* const> choices, bool tabs = false);
int Segments(const char* id, ImVec2 min, std::span<const char* const> choices, int selected, bool tabs = false);
inline int Segments(const char* id, ImVec2 min, std::initializer_list<const char*> choices, int selected, bool tabs = false)
{
    return Segments(id, min, std::span<const char* const>(choices.begin(), choices.size()), selected, tabs);
}
inline float SegmentsWidth(std::initializer_list<const char*> choices, bool tabs = false)
{
    return SegmentsWidth(std::span<const char* const>(choices.begin(), choices.size()), tabs);
}

constexpr float kSwitchWidth = 54.4f;
constexpr float kSwitchHeight = 32;
// On or off: black with its knob at the right while on, grey with it at the left while off, so a column of them reads
// at a glance. Returns true when clicked.
bool SwitchButton(const char* id, ImVec2 min, bool on);

// A value as a bar filled to it in color. Dragging across sets it, double-clicking types it, the arrow keys step it
// while the mouse is on it, and right-clicking asks to reset it.
struct Bar
{
    std::string label;
    float min = 0;
    float max = 1;
    bool integer = false;
    float step = 0;              // values go in steps of this, when given
    std::string format = "%.3f"; // printf's, given an int for an integer and a double otherwise
    ImU32 color = kQuiet;
    float rounding = 17.6f;
    bool resettable = false;
    std::string tip;
};
// Returns true when value changed. reset is set when a resettable bar is right-clicked, and active while it is held.
bool FillBar(const char* id, ImVec2 min, ImVec2 size, const Bar& bar, float& value, bool* reset = nullptr, bool* active = nullptr);

constexpr float kSearchHeight = 57.6f;
// A big grey text box with a hint while empty. Takes the keyboard when focus is set. Returns true when edited.
bool SearchField(const char* id, const char* hint, char* text, size_t size, ImVec2 min, float width, bool focus = false);

// An effect as a tile: its color spreads from the dot in its corner while it is on.
struct TileLook
{
    std::string name;
    std::string detail; // a line under the name, such as why it is not running
    ImU32 detailColor = kDanger;
    ImU32 color = kQuiet;
    float reveal = 0; // how far its color has spread from the dot, 0 to 1
    float ink = 0;    // its text, from grey at 0 to black at 1
    float gray = 0;   // its color grayed and faded, while every effect is off
    float dot = 0;    // the dot, from an outline at 0 to black at 1
    float spot = 0;   // the color in the middle of the dot, 0 to 1
    float dotScale = 1;
    bool small = false;
    bool cooking = false; // a shine runs across it while effects compile
    float phase = 0;      // where the shine is, 0 to 1
};
constexpr float kTileRounding = 17.6f;
void DrawTile(ImDrawList* draw, ImVec2 min, ImVec2 max, const TileLook& look);
ImVec2 TileDot(ImVec2 min, ImVec2 max, bool small);
float TileDotRadius(bool small);

// A row of a popup menu, as wide as every other: grey behind it under the mouse, faint while it cannot be chosen, and an
// arrow at its right when it leads to more rows. Returns true when clicked, and leaves the popup open.
bool MenuRow(const char* label, bool enabled = true, ImU32 color = kInk, bool arrow = false);
constexpr float kMenuRowWidth = 216;
// Call last in a popup, before EndPopup, which then fades in when it opens.
float MenuAppear();

// A message in a black pill at the top middle of the screen, which springs in and fades after a moment.
void Toast(const std::string& text);
void DrawToast(ImDrawList* draw, ImVec2 top);
} // namespace spectrum
} // namespace kit
