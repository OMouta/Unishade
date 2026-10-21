#define IMGUI_DEFINE_MATH_OPERATORS
#include "kit.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <iterator>
#include <map>
#include <span>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kit
{
float scale = 1;

namespace
{
// What each Dear ImGui context keeps.
struct Context
{
    ImFont* bold = nullptr;
    // How much larger than asked text is drawn. See FontScale.
    float fontScale = 0;
};
std::map<ImGuiContext*, Context> contexts;

Context& Current()
{
    return contexts[ImGui::GetCurrentContext()];
}

// Text sizes are the Windows host's GDI sizes, whose fonts have capitals 0.53 of the size tall. Fonts differ in that,
// DejaVu Sans and Noto Sans by a fifth, so each font is drawn at the size that makes its capitals as tall.
float FontScale()
{
    Context& context = Current();
    if (context.fontScale == 0)
    {
        context.fontScale = 1;
        const ImFontGlyph* capital = ImGui::GetFont()->GetFontBaked(100.0f)->FindGlyphNoFallback('H');
        if (capital && capital->Y1 > capital->Y0)
            context.fontScale = 53.0f / (capital->Y1 - capital->Y0);
    }
    return context.fontScale;
}

// A font file mapped into memory until the program exits, since Dear ImGui reads glyphs from it whenever it first
// draws them. Contexts that load the same file share the mapping. Empty when the file cannot be read.
std::span<const unsigned char> MapFont(const std::string& path)
{
    static std::map<std::string, std::span<const unsigned char>> mapped;
    if (const auto found = mapped.find(path); found != mapped.end())
        return found->second;
    std::span<const unsigned char> view;
#ifdef _WIN32
    std::wstring wide(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size{};
        const HANDLE mapping =
            GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < INT_MAX ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        if (mapping)
        {
            // The view keeps the mapping open.
            if (const void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0))
                view = { static_cast<const unsigned char*>(data), static_cast<size_t>(size.QuadPart) };
            CloseHandle(mapping);
        }
        CloseHandle(file);
    }
#else
    const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat info{};
    if (file >= 0 && fstat(file, &info) == 0 && info.st_size > 0 && info.st_size < INT_MAX)
        if (void* data = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, file, 0); data != MAP_FAILED)
            view = { static_cast<const unsigned char*>(data), static_cast<size_t>(info.st_size) };
    if (file >= 0)
        close(file);
#endif
    mapped[path] = view;
    return view;
}

// Adds a font, or merges it into the one added before.
ImFont* AddFont(const FontFile& file, bool merge)
{
    const std::span<const unsigned char> data = MapFont(file.path);
    if (data.empty())
        return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.FontNo = static_cast<ImU32>(file.index);
    config.MergeMode = merge;
    config.Flags |= ImFontFlags_NoLoadError;
    // Dear ImGui only reads font data it does not own.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), 0.0f, &config);
}
} // namespace

void LoadFonts(const FontFile& regular, const std::vector<FontFile>& fallbacks, const FontFile& bold, const std::vector<FontFile>& boldFallbacks)
{
    const auto merge = [](const std::vector<FontFile>& files) {
        for (const FontFile& file : files)
            AddFont(file, true);
    };
    if (regular.path.empty() || !AddFont(regular, false))
        ImGui::GetIO().Fonts->AddFontDefault();
    merge(fallbacks);
    ImFont* boldFont = bold.path.empty() ? nullptr : AddFont(bold, false);
    if (boldFont)
        merge(boldFallbacks);
    SetBoldFont(boldFont);
}

void SetBoldFont(ImFont* bold)
{
    Current().bold = bold;
}

void ForgetContext(ImGuiContext* context)
{
    contexts.erase(context);
}

void PushSize(float size, bool bold)
{
    ImGui::PushFont(bold ? Current().bold : nullptr, S(size) * FontScale());
}

ImVec2 Measure(const std::string& text, float size, bool bold, float wrap)
{
    PushSize(size, bold);
    const ImVec2 result = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(), false, wrap);
    ImGui::PopFont();
    return result;
}

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
    // Text is sized by PushSize alone.
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

void Text(const std::string& text, ImU32 color, float size, float wrap, bool bold)
{
    PushSize(size, bold);
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

void Tooltip(const std::string& text)
{
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(7)));
    if (ImGui::BeginTooltip())
    {
        Text(text, kText, 13, S(300));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
}

bool Button(const char* label, ImVec2 size, bool primary, bool enabled)
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

bool Link(const char* label, ImU32 color)
{
    ImGui::PushStyleColor(ImGuiCol_TextLink, color);
    const bool clicked = ImGui::TextLink(label);
    HandOnHover();
    ImGui::PopStyleColor();
    return clicked;
}

void DrawSwitch(ImDrawList* draw, ImVec2 position, ImVec2 size, bool on, bool hovered)
{
    draw->AddRectFilled(position, position + size, on ? (hovered ? kAccentHover : kAccent) : (hovered ? kBorderStrong : kBorder), size.y / 2);
    const float x = on ? position.x + size.x - size.y / 2 : position.x + size.y / 2;
    draw->AddCircleFilled(ImVec2(x, position.y + size.y / 2), size.y / 2 - S(3), on ? IM_COL32_WHITE : kDim);
}

bool Switch(const char* id, bool on, ImVec2 size)
{
    if (size.x <= 0)
        size = ImVec2(S(32), S(18));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    HandOnHover();
    DrawSwitch(ImGui::GetWindowDrawList(), position, size, on, ImGui::IsItemHovered());
    return clicked;
}

int Segmented(const char* id, std::initializer_list<const char*> choices, int selected, ImU32 fill, float width)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 whole(width > 0 ? width : ImGui::GetContentRegionAvail().x, S(30));
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

void KeyCap(const std::string& key, float size)
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

void ProgressBar(float fraction)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, S(4));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, start + size, kBorder, S(2));
    draw->AddRectFilled(start, start + ImVec2(size.x * std::clamp(fraction, 0.0f, 1.0f), size.y), kAccent, S(2));
    ImGui::Dummy(size);
}

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

void NoticeIcon(ImDrawList* draw, ImVec2 center, launcher::Level level)
{
    using launcher::Level;
    const float r = S(8);
    const ImU32 color = level == Level::Ok ? kSuccess : level == Level::Warning ? kWarning : level == Level::Error ? kError : kDim;
    draw->AddCircleFilled(center, r, (color & 0x00FFFFFF) | 0x30000000);
    const float t = S(1.6f);
    switch (level)
    {
    case Level::Ok:
    {
        const ImVec2 points[] = { center + ImVec2(-S(3.5f), 0), center + ImVec2(-S(1), S(2.8f)), center + ImVec2(S(3.8f), -S(2.8f)) };
        draw->AddPolyline(points, 3, color, 0, t);
        break;
    }
    case Level::Warning:
        draw->AddLine(center + ImVec2(0, -S(4)), center + ImVec2(0, S(1)), color, t);
        draw->AddCircleFilled(center + ImVec2(0, S(3.8f)), S(1.1f), color);
        break;
    case Level::Error:
        draw->AddLine(center + ImVec2(-S(3), -S(3)), center + ImVec2(S(3), S(3)), color, t);
        draw->AddLine(center + ImVec2(S(3), -S(3)), center + ImVec2(-S(3), S(3)), color, t);
        break;
    default:
        draw->AddCircleFilled(center, S(2.2f), color);
        break;
    }
}

std::string Initial(const std::string& name)
{
    // The first letter or digit, so a name in quotes or brackets still shows one.
    size_t start = 0;
    while (start < name.size() && static_cast<unsigned char>(name[start]) < 0x80 && !std::isalnum(static_cast<unsigned char>(name[start])))
        ++start;
    if (start == name.size())
        return "?";
    size_t length = 1;
    while (start + length < name.size() && (static_cast<unsigned char>(name[start + length]) & 0xC0) == 0x80)
        ++length;
    return length == 1 ? std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[start])))) : name.substr(start, length);
}

bool BeginDialog(const char* id, bool& open, float width)
{
    if (open)
    {
        ImGui::OpenPopup(id);
        open = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(S(width), io.DisplaySize.x - S(16)), 0));
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

void DialogText(const std::string& title, const std::string& detail)
{
    Text(title, kText, 16.5f);
    if (!detail.empty())
        Text(detail, kDim, 13.5f);
}

int DialogButtons(std::initializer_list<const char*> labels)
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
        if (Button(label, ImVec2(buttonWidth, S(32)), index == count - 1))
            clicked = index;
        ++index;
    }
    return clicked;
}

void Line(ImDrawList* draw, float left, float right, float middle, const std::string& text, ImU32 color, float size, bool bold, bool centered)
{
    PushSize(size, bold);
    const ImVec2 measured = ImGui::CalcTextSize(text.c_str());
    const float x = centered ? left + (right - left - measured.x) / 2 : left;
    if (measured.x <= right - left)
        draw->AddText(ImVec2(x, middle - measured.y / 2), color, text.c_str());
    else
    {
        // Cut off with an ellipsis at right.
        const float ellipsis = ImGui::CalcTextSize("...").x;
        const char* end = text.c_str();
        const char* last = text.c_str() + text.size();
        while (end < last)
        {
            const char* next = end + 1;
            while (next < last && (static_cast<unsigned char>(*next) & 0xC0) == 0x80)
                ++next;
            if (ImGui::CalcTextSize(text.c_str(), next).x + ellipsis > right - left)
                break;
            end = next;
        }
        const std::string cut = std::string(text.c_str(), end) + "...";
        draw->AddText(ImVec2(left, middle - measured.y / 2), color, cut.c_str());
    }
    ImGui::PopFont();
}

float Paragraph(ImDrawList* draw, ImVec2 position, float right, const std::string& text, ImU32 color, float size, bool bold)
{
    PushSize(size, bold);
    const float wrap = right - position.x;
    const float height = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).y;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, color, text.c_str(), nullptr, wrap);
    ImGui::PopFont();
    return height;
}

float ButtonWidth(const char* label, bool bold)
{
    return Measure(label, kBody, bold).x + S(28);
}

bool PlacedButton(const char* label, ImVec2 min, ImVec2 size, bool primary, bool enabled)
{
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (enabled)
        HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (primary)
        draw->AddRectFilled(min, min + size, hovered ? kAccentHover : kAccent, S(8));
    else
    {
        draw->AddRectFilled(min, min + size, hovered ? kBorder : kCardHover, S(8));
        draw->AddRect(min, min + size, hovered ? kBorderStrong : kBorder, S(8));
    }
    // The label stops at ##, which keeps buttons with the same label apart.
    std::string text = label;
    text = text.substr(0, text.find("##"));
    Line(draw, min.x, min.x + size.x, min.y + size.y / 2, text, !enabled ? kBorderStrong : primary ? IM_COL32_WHITE : kText, kBody, primary, true);
    return clicked;
}

bool PlacedLink(const char* label, ImVec2 position, float height, float size)
{
    const float width = Measure(label, size).x;
    ImGui::SetCursorScreenPos(position);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    HandOnHover();
    Line(ImGui::GetWindowDrawList(), position.x, position.x + width + 1, position.y + height / 2, label, hovered ? kText : kAccentHover, size);
    return clicked;
}

void RoundedCard(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 fill, ImU32 border)
{
    draw->AddRectFilled(min, max, fill, S(10));
    draw->AddRect(min, max, border, S(10));
}
} // namespace kit
