#pragma once

// Unishade's look in Dear ImGui: its colors, text sizes and controls. Used by the launcher on every platform and by the
// menu on macOS and Linux. Sizes are given at 100% scaling and drawn at scale.

#include "launcher_ui.h"
#include "theme.h"

#include <imgui.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace kit
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

// The launcher's text sizes. Windows gives them as the height of the letters' box, three quarters of the line height
// Dear ImGui sizes text by.
constexpr float kTitle = 19 * 1.33f;
constexpr float kSemibold = 15 * 1.33f;
constexpr float kBody = 13.5f * 1.33f;
constexpr float kNote = 12 * 1.33f;

// The scale of the window being drawn.
extern float scale;

inline float S(float value)
{
    return value * scale;
}

// A font file, and which font of a collection (.ttc) to use.
struct FontFile
{
    std::string path;
    int index = 0;
};

// Loads the current Dear ImGui context's fonts, regular and bold, each followed by fallbacks for the characters it
// lacks, such as Chinese, Japanese, Korean or emoji. Files are mapped into memory, so only the parts that are drawn
// get read, and missing files are left out. Without a regular font, Dear ImGui's own is used.
void LoadFonts(const FontFile& regular, const std::vector<FontFile>& fallbacks, const FontFile& bold, const std::vector<FontFile>& boldFallbacks);
// The bold font of the current Dear ImGui context. Without one, bold text uses the regular font.
void SetBoldFont(ImFont* bold);
// Forgets what was kept for a context that is being destroyed.
void ForgetContext(ImGuiContext* context);

void PushSize(float size, bool bold = false);
ImVec2 Measure(const std::string& text, float size, bool bold = false, float wrap = -1.0f);
void ApplyStyle(ImGuiStyle& style);

// Wraps at the edge of the window, at wrap in window coordinates when given, or not at all when wrap is negative.
void Text(const std::string& text, ImU32 color = kText, float size = 14.5f, float wrap = 0.0f, bool bold = false);
void Heading(const char* text);
void HandOnHover();
void Tooltip(const std::string& text);
bool Button(const char* label, ImVec2 size, bool primary = false, bool enabled = true);
bool Link(const char* label, ImU32 color = kAccentHover);
void DrawSwitch(ImDrawList* draw, ImVec2 position, ImVec2 size, bool on, bool hovered);
// An on/off switch. Returns true when clicked.
bool Switch(const char* id, bool on, ImVec2 size = ImVec2(0, 0));
// Choices side by side in one control, each as wide as the others, with the selected one filled. Returns the index
// of the choice clicked, or -1. As wide as width, or the rest of the line without one.
int Segmented(const char* id, std::initializer_list<const char*> choices, int selected, ImU32 fill, float width = 0);
void KeyCap(const std::string& key, float size = 13);
void Spinner(float radius);
void ProgressBar(float fraction);
// Points down when open and right when closed.
void Chevron(ImDrawList* draw, ImVec2 center, bool open, ImU32 color);
// The colors of the logo's ring, side by side.
void Rainbow(ImDrawList* draw, ImVec2 min, ImVec2 max);
// A check, exclamation mark or cross in a tinted circle, for messages.
void NoticeIcon(ImDrawList* draw, ImVec2 center, launcher::Level level);
// The first letter of a name, which stands in for a missing icon.
std::string Initial(const std::string& name);

// A dialog in the middle of the window, opened when open is set. Call EndDialog when this returns true.
bool BeginDialog(const char* id, bool& open, float width);
void EndDialog();
// The dialog's title and the line below it.
void DialogText(const std::string& title, const std::string& detail);
// The dialog's buttons, on the right with the last one primary. Returns the index of the one clicked, or -1.
int DialogButtons(std::initializer_list<const char*> labels);

// One line of text between left and right, cut off at right, centered up and down around middle.
void Line(ImDrawList* draw, float left, float right, float middle, const std::string& text, ImU32 color, float size, bool bold = false,
          bool centered = false);
// Text wrapped between position and right. Returns its height.
float Paragraph(ImDrawList* draw, ImVec2 position, float right, const std::string& text, ImU32 color, float size, bool bold = false);
float ButtonWidth(const char* label, bool bold = false);
// A button at a place. Returns true when clicked.
bool PlacedButton(const char* label, ImVec2 min, ImVec2 size, bool primary = false, bool enabled = true);
// Text to click at a place, as high as height. Returns true when clicked.
bool PlacedLink(const char* label, ImVec2 position, float height, float size = kBody);
void RoundedCard(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 fill = kCard, ImU32 border = kBorder);

// A picture the host names, such as a game's icon, as a texture drawn size wide and high in the window's units.
// ImTextureID_Invalid when there is none. Each platform makes its own textures.
ImTextureID Picture(const std::string& key, float size);
// The average color of a picture's opaque pixels, or 0 when there is none. Only after Picture made it.
ImU32 PictureTint(const std::string& key);
} // namespace kit
