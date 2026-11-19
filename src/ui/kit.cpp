#define IMGUI_DEFINE_MATH_OPERATORS
#include "kit.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>

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
// An animation from one value to another. See Animate.
struct Tween
{
    ImVec2 from;
    ImVec2 to;
    double start = 0;
    float seconds = 0;
    Curve curve = Curve::Linear;
    double used = 0;
};

// What each Dear ImGui context keeps.
struct Context
{
    ImFont* bold = nullptr;
    ImFont* heavy = nullptr;
    // How much larger than asked text is drawn. See FontScale.
    float fontScale = 0;
    std::unordered_map<ImGuiID, Tween> tweens;
    double swept = 0;
    // The fill bar whose value is being typed, and the text typed.
    ImGuiID typing = 0;
    char typed[64] = {};
    bool focusTyping = false;
    std::string toast;
    double toastAt = -100;
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
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.FontNo = static_cast<ImU32>(file.index);
    config.MergeMode = merge;
    config.Flags |= ImFontFlags_NoLoadError;
    if (file.loader)
    {
        config.FontLoader = file.loader;
        return ImGui::GetIO().Fonts->AddFont(&config);
    }
    const std::span<const unsigned char> data = file.data.empty() ? MapFont(file.path) : file.data;
    if (data.empty())
        return nullptr;
    // Dear ImGui only reads font data it does not own.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), 0.0f, &config);
}
} // namespace

void LoadFonts(const FontFile& regular, const std::vector<FontFile>& fallbacks, const FontFile& bold, const std::vector<FontFile>& boldFallbacks,
               const FontFile& heavy)
{
    const auto merge = [](const std::vector<FontFile>& files) {
        for (const FontFile& file : files)
            AddFont(file, true);
    };
    if ((regular.path.empty() && regular.data.empty()) || !AddFont(regular, false))
        ImGui::GetIO().Fonts->AddFontDefault();
    merge(fallbacks);
    ImFont* boldFont = bold.path.empty() && bold.data.empty() ? nullptr : AddFont(bold, false);
    if (boldFont)
        merge(boldFallbacks);
    SetBoldFont(boldFont);
    ImFont* heavyFont = heavy.path.empty() && heavy.data.empty() ? nullptr : AddFont(heavy, false);
    if (heavyFont)
        merge(boldFallbacks);
    Current().heavy = heavyFont;
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
    // The design sets its titles, from the preset's name up, in the heaviest weight and everything smaller in semibold.
    const Context& context = Current();
    ImFont* font = !bold ? nullptr : size >= spectrum::kTextName && context.heavy ? context.heavy : context.bold;
    ImGui::PushFont(font, S(size) * FontScale());
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
    color(ImGuiCol_PopupBg, Color(theme::kPopup, 252));
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

void HandOnHover()
{
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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

std::string Initial(const std::string& name)
{
    // From the first letter or digit, so a name in quotes or brackets still shows one.
    size_t start = 0;
    while (start < name.size() && static_cast<unsigned char>(name[start]) < 0x80 && !std::isalnum(static_cast<unsigned char>(name[start])))
        ++start;
    if (start == name.size())
        return "?";
    size_t length = 1;
    while (start + length < name.size() && (static_cast<unsigned char>(name[start + length]) & 0xC0) == 0x80)
        ++length;
    if (length > 1)
        return name.substr(start, length);
    // With the letter or digit after it, so Roblox and R.E.P.O. are Ro and RE rather than both R.
    std::string initial(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[start]))));
    for (size_t next = start + 1; next < name.size() && static_cast<unsigned char>(name[next]) < 0x80; ++next)
        if (std::isalnum(static_cast<unsigned char>(name[next])))
        {
            initial += name[next];
            break;
        }
    return initial;
}

void Line(ImDrawList* draw, float left, float right, float middle, const std::string& text, ImU32 color, float size, bool bold, bool centered)
{
    PushSize(size, bold);
    const ImVec2 measured = ImGui::CalcTextSize(text.c_str());
    const float x = centered ? left + (right - left - measured.x) / 2 : left;
    // Callers often give right as left plus the measured width, which rounding can leave a hair short.
    if (measured.x <= right - left + 0.5f)
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

// Animation

namespace
{
// CSS's cubic-bezier(x1, y1, x2, y2) at x.
float Bezier(float x1, float y1, float x2, float y2, float x)
{
    const auto sample = [](float a, float b, float t) {
        const float u = 1 - t;
        return 3 * a * u * u * t + 3 * b * u * t * t + t * t * t;
    };
    float low = 0, high = 1, t = x;
    for (int i = 0; i < 24; ++i)
    {
        const float value = sample(x1, x2, t);
        if (std::abs(value - x) < 1e-5f)
            break;
        (value < x ? low : high) = t;
        t = (low + high) / 2;
    }
    return sample(y1, y2, t);
}

// The design's spring, as the stops of its CSS linear(): percent of the time, and the value there. Stops the CSS
// leaves without a time are spread evenly between their neighbors, as CSS does.
constexpr float kSpring[][2] = { { 0, 0 },          { 1.05f, 0.009f },  { 2.1f, 0.035f },   { 4.4f, 0.141f },  { 12.9f, 0.723f },
                                 { 16.7f, 0.938f }, { 18.6f, 1.017f },  { 20.5f, 1.077f },  { 22.4f, 1.121f }, { 24.3f, 1.149f },
                                 { 25.7f, 1.159f }, { 27.1f, 1.163f },  { 28.5f, 1.161f },  { 29.9f, 1.154f }, { 32.8f, 1.129f },
                                 { 39.6f, 1.051f }, { 43.1f, 1.017f },  { 47.05f, 0.991f }, { 51, 0.977f },    { 53.8f, 0.974f },
                                 { 57.1f, 0.975f }, { 69.8f, 0.997f },  { 76.9f, 1.003f },  { 83.8f, 1.004f }, { 100, 1 } };

ImVec2 ValueAt(const Tween& tween, double now)
{
    if (now >= tween.start + tween.seconds)
        return tween.to;
    if (now <= tween.start)
        return tween.from;
    const float t = Ease(tween.curve, static_cast<float>((now - tween.start) / tween.seconds));
    return tween.from + (tween.to - tween.from) * t;
}

// Forgets the animations nobody asked for in a second.
void Sweep(Context& context, double now)
{
    if (now - context.swept < 1)
        return;
    context.swept = now;
    std::erase_if(context.tweens, [&](const auto& entry) { return now - entry.second.used > 1; });
}
} // namespace

float Ease(Curve curve, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (curve)
    {
    case Curve::Out: return Bezier(0.2f, 0.9f, 0.2f, 1, t);
    case Curve::In: return Bezier(0.5f, 0, 0.8f, 0.3f, t);
    case Curve::EaseIn: return Bezier(0.42f, 0, 1, 1, t);
    case Curve::Leave: return Bezier(0.5f, 0, 0.9f, 0.4f, t);
    case Curve::Spring:
    {
        const float percent = t * 100;
        for (size_t i = 1; i < std::size(kSpring); ++i)
            if (percent <= kSpring[i][0])
            {
                const float span = kSpring[i][0] - kSpring[i - 1][0];
                const float along = span > 0 ? (percent - kSpring[i - 1][0]) / span : 1;
                return kSpring[i - 1][1] + (kSpring[i][1] - kSpring[i - 1][1]) * along;
            }
        return 1;
    }
    case Curve::Linear: break;
    }
    return t;
}

ImVec2 Animate(ImGuiID id, ImVec2 target, float seconds, Curve curve, float delay)
{
    const double now = ImGui::GetTime();
    Context& context = Current();
    Sweep(context, now);
    const auto [found, created] = context.tweens.try_emplace(id);
    Tween& tween = found->second;
    tween.used = now;
    if (created)
    {
        tween.from = tween.to = target;
        tween.start = now;
        return target;
    }
    if (tween.to.x != target.x || tween.to.y != target.y)
    {
        tween.from = ValueAt(tween, now);
        tween.to = target;
        tween.start = now + delay;
        tween.seconds = seconds;
        tween.curve = curve;
    }
    return ValueAt(tween, now);
}

float Animate(ImGuiID id, float target, float seconds, Curve curve, float delay)
{
    return Animate(id, ImVec2(target, 0), seconds, curve, delay).x;
}

void Snap(ImGuiID id, ImVec2 value)
{
    const double now = ImGui::GetTime();
    Tween& tween = Current().tweens[id];
    tween.from = tween.to = value;
    tween.start = now;
    tween.seconds = 0;
    tween.used = now;
}

void Snap(ImGuiID id, float value)
{
    Snap(id, ImVec2(value, 0));
}

bool Animating()
{
    const double now = ImGui::GetTime();
    for (const auto& [id, tween] : Current().tweens)
        if (now - tween.used < 0.1 && now < tween.start + tween.seconds)
            return true;
    return false;
}

bool Animated(ImGuiID id)
{
    const Context& context = Current();
    const auto found = context.tweens.find(id);
    return found != context.tweens.end() && ImGui::GetTime() - found->second.used <= 1;
}

ImGuiID SubId(ImGuiID id, const char* part)
{
    return ImHashStr(part, 0, id);
}

void Transform(ImDrawList* draw, int start, ImVec2 pivot, float scaleBy, float angle, ImVec2 offset, float alpha)
{
    if (scaleBy == 1 && angle == 0 && offset.x == 0 && offset.y == 0 && alpha >= 1)
        return;
    const float c = std::cos(angle) * scaleBy, s = std::sin(angle) * scaleBy;
    for (int i = start; i < draw->VtxBuffer.Size; ++i)
    {
        ImDrawVert& vertex = draw->VtxBuffer[i];
        const ImVec2 d = vertex.pos - pivot;
        vertex.pos = pivot + ImVec2(d.x * c - d.y * s, d.x * s + d.y * c) + offset;
        if (alpha < 1)
            vertex.col = spectrum::Alpha(vertex.col, alpha);
    }
}

void FadeWindow(ImGuiWindow* window, float alpha)
{
    if (!window || alpha >= 1)
        return;
    for (ImDrawVert& vertex : window->DrawList->VtxBuffer)
        vertex.col = spectrum::Alpha(vertex.col, alpha);
    for (ImGuiWindow* child : window->DC.ChildWindows)
        FadeWindow(child, alpha);
}

// Spectrum

namespace spectrum
{
namespace
{
// The outline of a rounded box, clockwise on the screen, where y points down.
std::vector<ImVec2> Outline(ImVec2 min, ImVec2 max, float rounding)
{
    rounding = std::min({ rounding, (max.x - min.x) / 2, (max.y - min.y) / 2 });
    if (rounding < 0.5f)
        return { min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y) };
    constexpr int kSteps = 8;
    const ImVec2 centers[] = { min + ImVec2(rounding, rounding), ImVec2(max.x - rounding, min.y + rounding), max - ImVec2(rounding, rounding),
                               ImVec2(min.x + rounding, max.y - rounding) };
    std::vector<ImVec2> points;
    points.reserve(4 * (kSteps + 1));
    for (int corner = 0; corner < 4; ++corner)
        for (int step = 0; step <= kSteps; ++step)
        {
            const float angle = IM_PI + (static_cast<float>(corner) + static_cast<float>(step) / kSteps) * IM_PI / 2;
            points.push_back(centers[corner] + ImVec2(std::cos(angle), std::sin(angle)) * rounding);
        }
    return points;
}

// The part of subject inside the convex clip outline, both clockwise on the screen.
std::vector<ImVec2> Clip(std::span<const ImVec2> subject, const std::vector<ImVec2>& clip)
{
    std::vector<ImVec2> output(subject.begin(), subject.end()), input;
    for (size_t i = 0; i < clip.size() && !output.empty(); ++i)
    {
        const ImVec2 a = clip[i], b = clip[(i + 1) % clip.size()];
        // Inside is on the right of a to b, which is positive here with y pointing down.
        const auto side = [&](ImVec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
        input.swap(output);
        output.clear();
        for (size_t j = 0; j < input.size(); ++j)
        {
            const ImVec2 p = input[j], q = input[(j + 1) % input.size()];
            const float sp = side(p), sq = side(q);
            if (sp >= 0)
                output.push_back(p);
            if ((sp >= 0) != (sq >= 0))
                output.push_back(p + (q - p) * (sp / (sp - sq)));
        }
    }
    return output;
}

std::string LabelOf(const char* label)
{
    std::string text = label;
    return text.substr(0, text.find("##"));
}

float PillPadding(PillStyle style)
{
    return style == PillStyle::Well ? 17.6f : style == PillStyle::Black ? 20.8f : 14.4f;
}
} // namespace

ImU32 Ring(size_t index)
{
    return Color(theme::kRainbow[index % std::size(theme::kRainbow)]);
}

ImU32 Alpha(ImU32 color, float alpha)
{
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
    return (color & ~static_cast<ImU32>(IM_COL32_A_MASK)) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 from, ImU32 to, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    ImU32 result = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const float a = static_cast<float>((from >> shift) & 0xFF), b = static_cast<float>((to >> shift) & 0xFF);
        result |= static_cast<ImU32>(a + (b - a) * t + 0.5f) << shift;
    }
    return result;
}

namespace
{
bool darkTheme = false;
}

void SetDark(bool dark)
{
    darkTheme = dark;
    kTray = dark ? IM_COL32(20, 20, 26, 219) : IM_COL32(247, 247, 251, 219);
    kPaper = dark ? Color(0x16161C) : Color(0xF7F7FB);
    kInk = dark ? Color(0xF2F2FB) : IM_COL32(0, 0, 0, 255);
    kOnInk = dark ? IM_COL32(0, 0, 0, 255) : IM_COL32_WHITE;
    kInkHover = dark ? Color(0xD4D4E2) : Color(0x2A2A33);
    kMist = dark ? Color(0xA0A0B0) : Color(0x62626F);
    kWell = dark ? IM_COL32(255, 255, 255, 20) : IM_COL32(0, 0, 0, 18);
    kWellHover = dark ? IM_COL32(255, 255, 255, 36) : IM_COL32(0, 0, 0, 33);
    kFaint = dark ? IM_COL32(255, 255, 255, 64) : IM_COL32(0, 0, 0, 64);
    kQuiet = dark ? Color(0x4A4A58) : Color(0xC9C9D3);
    kDanger = dark ? Color(0xFF7676) : Color(0xC8243A);
    kCaution = dark ? Color(0xF5C05C) : Color(0x9A5B00);
    kEdge = dark ? IM_COL32(255, 255, 255, 36) : IM_COL32(255, 255, 255, 178);
    kToast = dark ? Color(0x16161C) : IM_COL32(0, 0, 0, 255);
    kOnToast = dark ? Color(0xF2F2FB) : IM_COL32_WHITE;
    kToastEdge = dark ? IM_COL32(255, 255, 255, 36) : 0;
}

bool Dark()
{
    return darkTheme;
}

void ApplyStyle(ImGuiStyle& style)
{
    kit::ApplyStyle(style);
    style.FrameRounding = S(12.8f);
    style.PopupRounding = S(17.6f);
    style.GrabRounding = S(6);
    style.ScrollbarSize = S(6);
    const auto color = [&](ImGuiCol index, ImU32 value) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(value); };
    color(ImGuiCol_Text, kInk);
    color(ImGuiCol_TextDisabled, kMist);
    color(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_PopupBg, kPaper);
    color(ImGuiCol_Border, Alpha(kInk, 0.1f));
    color(ImGuiCol_FrameBg, kWell);
    color(ImGuiCol_FrameBgHovered, kWellHover);
    color(ImGuiCol_FrameBgActive, kWellHover);
    color(ImGuiCol_ScrollbarGrab, kFaint);
    color(ImGuiCol_ScrollbarGrabHovered, Alpha(kInk, 0.43f));
    color(ImGuiCol_ScrollbarGrabActive, Alpha(kInk, 0.43f));
    color(ImGuiCol_CheckMark, kInk);
    color(ImGuiCol_SliderGrab, kInk);
    color(ImGuiCol_SliderGrabActive, kInk);
    color(ImGuiCol_Button, kWell);
    color(ImGuiCol_ButtonHovered, kWellHover);
    color(ImGuiCol_ButtonActive, kFaint);
    color(ImGuiCol_Header, kWell);
    color(ImGuiCol_HeaderHovered, kWellHover);
    color(ImGuiCol_HeaderActive, kFaint);
    color(ImGuiCol_Separator, Alpha(kInk, 0.12f));
    color(ImGuiCol_TextLink, kInk);
    color(ImGuiCol_TextSelectedBg, Alpha(kInk, 0.18f));
    color(ImGuiCol_InputTextCursor, kInk);
    color(ImGuiCol_DragDropTarget, kInk);
    color(ImGuiCol_NavCursor, kInk);
}

void Tip(const std::string& text)
{
    if (text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(8)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPaper);
    if (ImGui::BeginTooltip())
    {
        Text(text, kInk, kTextSmall, S(320));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void ScrollCue(const ImGuiWindow* child, ImU32 color)
{
    if (!child || child->ScrollMax.y < 1)
        return;
    const ImRect box = child->Rect();
    const float width = S(3.2f), inset = S(1.6f);
    const float track = box.GetHeight() - inset * 2;
    const float thumb = std::max(track * box.GetHeight() / (box.GetHeight() + child->ScrollMax.y), std::min(S(24), track));
    const float top = box.Min.y + inset + (track - thumb) * std::clamp(child->Scroll.y / child->ScrollMax.y, 0.0f, 1.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(box.Max.x - width, top), ImVec2(box.Max.x, top + thumb), color, width / 2);
}

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color, float angle)
{
    // The design's icons are 24 units wide, drawn with lines 2 units thick.
    const float unit = size / 24, thickness = 2 * unit;
    const float c = std::cos(angle), s = std::sin(angle);
    const auto at = [&](float x, float y) {
        const ImVec2 d((x - 12) * unit, (y - 12) * unit);
        return center + ImVec2(d.x * c - d.y * s, d.x * s + d.y * c);
    };
    const auto line = [&](float x1, float y1, float x2, float y2) { draw->AddLine(at(x1, y1), at(x2, y2), color, thickness); };
    const auto polyline = [&](std::initializer_list<ImVec2> points, bool closed) {
        ImVec2 placed[8];
        int count = 0;
        for (const ImVec2 point : points)
            if (count < 8)
                placed[count++] = at(point.x, point.y);
        draw->AddPolyline(placed, count, color, thickness, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    };
    switch (icon)
    {
    case Icon::Left: polyline({ ImVec2(15, 6), ImVec2(9, 12), ImVec2(15, 18) }, false); break;
    case Icon::Right: polyline({ ImVec2(9, 6), ImVec2(15, 12), ImVec2(9, 18) }, false); break;
    case Icon::Plus:
        line(12, 5, 12, 19);
        line(5, 12, 19, 12);
        break;
    case Icon::Close:
        line(6, 6, 18, 18);
        line(18, 6, 6, 18);
        break;
    case Icon::Check: polyline({ ImVec2(5, 12.5f), ImVec2(10, 17.5f), ImVec2(19, 7) }, false); break;
    case Icon::Camera:
        polyline({ ImVec2(4, 8), ImVec2(7, 8), ImVec2(9, 5), ImVec2(15, 5), ImVec2(17, 8), ImVec2(20, 8), ImVec2(20, 19), ImVec2(4, 19) }, true);
        draw->AddCircle(at(12, 13.5f), 3.5f * unit, color, 0, thickness);
        break;
    case Icon::Power:
        line(12, 3, 12, 12);
        // From the top left round the bottom to the top right.
        draw->PathArcTo(center, 8 * unit, -0.75f * IM_PI + angle, -2.25f * IM_PI + angle, 32);
        draw->PathStroke(color, thickness);
        break;
    case Icon::Sliders:
        line(4, 6, 14, 6);
        line(18, 6, 20, 6);
        line(4, 12, 6, 12);
        line(10, 12, 20, 12);
        line(4, 18, 12, 18);
        line(16, 18, 20, 18);
        line(16, 4, 16, 8);
        line(8, 10, 8, 14);
        line(14, 16, 14, 20);
        break;
    case Icon::Dots:
        for (const float x : { 6.0f, 12.0f, 18.0f })
            draw->AddCircleFilled(at(x, 12), 1.6f * unit, color);
        break;
    }
}

void Shadow(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float y, float blur, float alpha)
{
    // Layers that each spread a little further, so the shadow is darkest under the box and fades out around it.
    constexpr int kLayers = 10;
    const int each = std::max(1, static_cast<int>(alpha * 255 / kLayers + 0.5f));
    for (int i = kLayers; i >= 1; --i)
    {
        const float spread = blur * 0.6f * static_cast<float>(i) / kLayers;
        const ImVec2 grow(spread, spread);
        draw->AddRectFilled(min - grow + ImVec2(0, y), max + grow + ImVec2(0, y), IM_COL32(0, 0, 0, each), rounding + spread);
    }
}

void FillInside(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, std::span<const ImVec2> polygon, ImU32 color)
{
    const std::vector<ImVec2> inside = Clip(polygon, Outline(min, max, rounding));
    if (inside.size() >= 3)
        draw->AddConvexPolyFilled(inside.data(), static_cast<int>(inside.size()), color);
}

void CircleInside(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, ImVec2 center, float radius, ImU32 color)
{
    if (radius <= 0)
        return;
    const auto covered = [&](ImVec2 corner) { return ImLengthSqr(corner - center) <= radius * radius; };
    if (covered(min) && covered(max) && covered(ImVec2(min.x, max.y)) && covered(ImVec2(max.x, min.y)))
    {
        draw->AddRectFilled(min, max, color, rounding);
        return;
    }
    constexpr int kSegments = 64;
    ImVec2 circle[kSegments];
    for (int i = 0; i < kSegments; ++i)
    {
        const float angle = 2 * IM_PI * static_cast<float>(i) / kSegments;
        circle[i] = center + ImVec2(std::cos(angle), std::sin(angle)) * radius;
    }
    FillInside(draw, min, max, rounding, circle, color);
}

void DashedBox(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, ImU32 color, float thickness)
{
    // Inset by half the line, so it stays inside the box as a CSS border does.
    const ImVec2 half(thickness / 2, thickness / 2);
    const std::vector<ImVec2> points = Outline(min + half, max - half, rounding - thickness / 2);
    const float dash = thickness * 3, gap = thickness * 2;
    bool on = true;
    float left = dash;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const ImVec2 p = points[i], q = points[(i + 1) % points.size()];
        const float length = std::sqrt(ImLengthSqr(q - p));
        if (length <= 0)
            continue;
        const ImVec2 direction = (q - p) / length;
        for (float at = 0; at < length;)
        {
            const float step = std::min(left, length - at);
            if (on)
                draw->AddLine(p + direction * at, p + direction * (at + step), color, thickness);
            at += step;
            left -= step;
            if (left <= 0)
            {
                on = !on;
                left = on ? dash : gap;
            }
        }
    }
}

void RingDisc(ImDrawList* draw, ImVec2 center, float radius, float angle, float alpha)
{
    // CSS's conic-gradient(from 200deg, ...) with the ring's colors and the first again at the end. CSS angles start
    // at the top and go clockwise.
    constexpr int kSegments = 96;
    constexpr int kColors = static_cast<int>(std::size(theme::kRainbow));
    for (int i = 0; i < kSegments; ++i)
    {
        const float a0 = 2 * IM_PI * static_cast<float>(i) / kSegments, a1 = 2 * IM_PI * static_cast<float>(i + 1) / kSegments;
        float css = (a0 + a1) / 2 + IM_PI / 2 - angle - 200.0f * IM_PI / 180;
        css = std::fmod(css, 2 * IM_PI);
        if (css < 0)
            css += 2 * IM_PI;
        const float along = css / (2 * IM_PI) * kColors;
        const int stop = static_cast<int>(along) % kColors;
        const ImU32 color = Mix(Color(theme::kRainbow[stop]), Color(theme::kRainbow[(stop + 1) % kColors]), along - std::floor(along));
        draw->AddTriangleFilled(center, center + ImVec2(std::cos(a0), std::sin(a0)) * radius, center + ImVec2(std::cos(a1), std::sin(a1)) * radius,
                                Alpha(color, alpha));
    }
}

void RainbowLine(ImDrawList* draw, ImVec2 min, ImVec2 max, float fraction, float alpha)
{
    if (fraction <= 0 || alpha <= 0)
        return;
    draw->PushClipRect(min, ImVec2(min.x + (max.x - min.x) * std::min(fraction, 1.0f), max.y), true);
    constexpr int count = static_cast<int>(std::size(theme::kRainbow));
    for (int i = 0; i + 1 < count; ++i)
    {
        const float left = min.x + (max.x - min.x) * static_cast<float>(i) / (count - 1);
        const float right = min.x + (max.x - min.x) * static_cast<float>(i + 1) / (count - 1);
        const ImU32 from = Alpha(Color(theme::kRainbow[i]), alpha);
        const ImU32 to = Alpha(Color(theme::kRainbow[i + 1]), alpha);
        draw->AddRectFilledMultiColor(ImVec2(left, min.y), ImVec2(right, max.y), from, to, to, from);
    }
    draw->PopClipRect();
}

std::vector<std::string> WrapName(const std::string& name, float size, bool bold, float width)
{
    std::vector<std::string> pieces;
    std::string piece;
    for (size_t i = 0; i < name.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        piece += name[i];
        const bool last = i + 1 == name.size();
        const unsigned char next = last ? 0 : static_cast<unsigned char>(name[i + 1]);
        if (last || c == ' ' || c == '_' || c == '-' || (std::islower(c) && std::isupper(next)))
        {
            pieces.push_back(piece);
            piece.clear();
        }
    }
    const auto trim = [](std::string text) {
        text.erase(text.find_last_not_of(' ') + 1);
        text.erase(0, std::min(text.find_first_not_of(' '), text.size()));
        return text;
    };
    std::vector<std::string> lines;
    std::string line;
    PushSize(size, bold);
    for (const std::string& part : pieces)
    {
        const std::string longer = trim(line + part);
        if (!trim(line).empty() && ImGui::CalcTextSize(longer.c_str()).x > width)
        {
            lines.push_back(trim(line));
            line = part;
        }
        else
            line += part;
    }
    if (!trim(line).empty())
        lines.push_back(trim(line));
    ImGui::PopFont();
    return lines;
}

float KeyWidth(const std::string& key)
{
    return std::max(Measure(key, kTextBody, true).x + S(22.4f), S(38.4f));
}

void KeyLabel(ImDrawList* draw, ImVec2 min, float height, const std::string& key, ImU32 fill, ImU32 text)
{
    const float width = KeyWidth(key);
    draw->AddRectFilled(min, min + ImVec2(width, height), fill, S(9.6f));
    Line(draw, min.x, min.x + width, min.y + height / 2, key, text, kTextBody, true, true);
}

bool RoundButton(const char* id, ImVec2 min, Icon icon, const char* tip, bool active, float size, float turn)
{
    const float side = S(size);
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side), ImGuiButtonFlags_EnableNav);
    const ImGuiID key = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int start = draw->VtxBuffer.Size;
    const ImVec2 center = min + ImVec2(side, side) * 0.5f;
    draw->AddCircleFilled(center, side / 2, active ? kInk : hovered ? kWellHover : kWell);
    const float angle = Animate(SubId(key, "turn"), active ? turn : 0, 0.5f, Curve::Spring);
    DrawIcon(draw, icon, center, S(20.8f), active ? kOnInk : kInk, angle);
    Transform(draw, start, center, Animate(SubId(key, "press"), held ? 0.86f : 1, 0.25f, Curve::Spring));
    if (tip)
        Tip(tip);
    return clicked;
}

bool PowerButton(const char* id, ImVec2 min, bool on, const char* tip)
{
    const float side = S(kPowerSize);
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side), ImGuiButtonFlags_EnableNav);
    const ImGuiID key = ImGui::GetItemID();
    const bool held = ImGui::IsItemActive();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int start = draw->VtxBuffer.Size;
    const ImVec2 center = min + ImVec2(side, side) * 0.5f;
    draw->AddCircleFilled(center, side / 2, kQuiet, 48);
    // The ring fades in as it turns half round into place.
    const float ring = Animate(SubId(key, "ring"), on ? 1.0f : 0.0f, 0.4f, Curve::Out);
    const float turn = Animate(SubId(key, "turn"), on ? 0.0f : -IM_PI, 0.9f, Curve::Out);
    if (ring > 0)
        RingDisc(draw, center, side / 2, turn, ring);
    draw->AddCircleFilled(center, side / 2 - S(4), kPaper, 48);
    const float ink = Animate(SubId(key, "ink"), on ? 1.0f : 0.0f, 0.3f, Curve::Out);
    DrawIcon(draw, Icon::Power, center, S(20.8f), Mix(kMist, kInk, ink));
    Transform(draw, start, center, Animate(SubId(key, "press"), held ? 0.86f : 1, 0.25f, Curve::Spring));
    if (tip)
        Tip(tip);
    return clicked;
}

float PillWidth(const char* label, PillStyle style)
{
    return Measure(LabelOf(label), kTextBody, true).x + S(PillPadding(style)) * 2;
}

bool PillButton(const char* label, ImVec2 min, float height, PillStyle style, bool enabled)
{
    const ImVec2 size(PillWidth(label, style), height);
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const ImGuiID key = ImGui::GetItemID();
    const bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    if (enabled)
        HandOnHover();
    ImU32 fill = 0, text = kInk;
    float pressed = 1;
    switch (style)
    {
    case PillStyle::Well:
        fill = held ? kInk : hovered ? kWellHover : kWell;
        text = held ? kOnInk : kInk;
        pressed = 0.94f;
        break;
    case PillStyle::Black:
        fill = hovered ? kInkHover : kInk;
        text = kOnInk;
        pressed = 0.92f;
        break;
    case PillStyle::Text:
        fill = hovered || held ? kWell : 0;
        text = hovered || held ? kInk : kMist;
        pressed = 0.93f;
        break;
    }
    if (!enabled)
        text = Alpha(text, 0.4f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int start = draw->VtxBuffer.Size;
    if (fill)
        draw->AddRectFilled(min, min + size, fill, height / 2);
    Line(draw, min.x, min.x + size.x, min.y + height / 2, LabelOf(label), text, kTextBody, true, true);
    Transform(draw, start, min + size * 0.5f, Animate(SubId(key, "press"), held ? pressed : 1, 0.25f, Curve::Spring));
    return clicked;
}

float SegmentsWidth(std::span<const char* const> choices, bool tabs)
{
    float width = tabs ? S(6.4f) : 0;
    for (const char* choice : choices)
        width += Measure(choice, kTextBody, true).x + S(27.2f);
    return width;
}

int Segments(const char* id, ImVec2 min, std::span<const char* const> choices, int selected, bool tabs)
{
    ImGui::PushID(id);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pad = tabs ? S(3.2f) : 0, height = S(kSegmentHeight);
    if (tabs)
        draw->AddRectFilled(min, min + ImVec2(SegmentsWidth(choices, true), height + pad * 2), kWell, S(16));
    std::vector<float> lefts, widths;
    float x = pad;
    for (const char* choice : choices)
    {
        lefts.push_back(x);
        widths.push_back(Measure(choice, kTextBody, true).x + S(27.2f));
        x += widths.back();
    }
    // The pill slides from choice to choice. It is kept in units of the control, so it stays with it when the control
    // moves, and placed at once the first time.
    if (selected >= 0 && static_cast<size_t>(selected) < choices.size())
    {
        const ImVec2 target(lefts[static_cast<size_t>(selected)] / scale, widths[static_cast<size_t>(selected)] / scale);
        const ImVec2 thumb = Animate(ImGui::GetID("thumb"), target, 0.45f, Curve::Spring) * scale;
        draw->AddRectFilled(min + ImVec2(thumb.x, pad), min + ImVec2(thumb.x + thumb.y, pad + height), kInk, S(12.8f));
    }
    int clicked = -1;
    for (size_t i = 0; i < choices.size(); ++i)
    {
        const ImVec2 at = min + ImVec2(lefts[i], pad);
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton(choices[i], ImVec2(widths[i], height), ImGuiButtonFlags_EnableNav))
            clicked = static_cast<int>(i);
        const bool hovered = ImGui::IsItemHovered();
        HandOnHover();
        ImGui::PopID();
        const ImU32 color = static_cast<int>(i) == selected ? kOnInk : hovered ? kInk : kMist;
        Line(draw, at.x, at.x + widths[i], at.y + height / 2, choices[i], color, kTextBody, true, true);
    }
    ImGui::PopID();
    return clicked;
}

bool SwitchButton(const char* id, ImVec2 min, bool on)
{
    const ImVec2 size(S(kSwitchWidth), S(kSwitchHeight));
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const ImGuiID key = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    HandOnHover();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int start = draw->VtxBuffer.Size;
    const float fill = Animate(SubId(key, "fill"), on ? 1.0f : 0.0f, 0.2f, Curve::Out);
    const float slide = Animate(SubId(key, "slide"), on ? 1.0f : 0.0f, 0.4f, Curve::Spring);
    draw->AddRectFilled(min, min + size, Mix(hovered ? Alpha(kInk, 0.3f) : kFaint, hovered ? kInkHover : kInk, fill), size.y / 2);
    const float inset = S(3.2f), radius = size.y / 2 - inset;
    const float left = min.x + inset + radius, right = min.x + size.x - inset - radius;
    draw->AddCircleFilled(ImVec2(left + (right - left) * slide, min.y + size.y / 2), radius, kPaper, 32);
    Transform(draw, start, min + size * 0.5f, Animate(SubId(key, "press"), held ? 0.92f : 1, 0.25f, Curve::Spring));
    return clicked;
}

bool FillBar(const char* id, ImVec2 min, ImVec2 size, const Bar& bar, float& value, bool* reset, bool* active)
{
    ImGui::PushID(id);
    const ImGuiID key = ImGui::GetID("bar");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Context& context = Current();
    const float rounding = S(bar.rounding);
    const float pad = S(17.6f);
    const float range = bar.max - bar.min;
    const auto settle = [&](float v) {
        v = std::clamp(v, std::min(bar.min, bar.max), std::max(bar.min, bar.max));
        if (bar.step > 0)
            v = bar.min + std::round((v - bar.min) / bar.step) * bar.step;
        if (bar.integer)
            v = std::round(v);
        return v;
    };
    const auto text = [&](float v) {
        char buffer[64];
        if (bar.integer)
            std::snprintf(buffer, sizeof(buffer), bar.format.c_str(), static_cast<int>(std::lround(v)));
        else
            std::snprintf(buffer, sizeof(buffer), bar.format.c_str(), static_cast<double>(v));
        return std::string(buffer);
    };
    bool changed = false;
    if (context.typing == key)
    {
        // The value typed in place of the one shown, which applies on Enter or when the box is left, unless Escape
        // left it.
        draw->AddRectFilled(min, min + size, kWell, rounding);
        Line(draw, min.x + pad, min.x + size.x / 2, min.y + size.y / 2, bar.label, kInk, kTextBody, true);
        const float width = S(80);
        PushSize(kTextBody, true);
        const float height = ImGui::GetFontSize() + S(8);
        const ImVec2 at(min.x + size.x - pad - width, min.y + (size.y - height) / 2);
        ImGui::SetCursorScreenPos(at);
        ImGui::PushItemWidth(width);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
        if (std::exchange(context.focusTyping, false))
            ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##typed", context.typed, sizeof(context.typed),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsScientific);
        const bool left = ImGui::IsItemDeactivated();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopItemWidth();
        ImGui::PopFont();
        draw->AddLine(ImVec2(at.x, at.y + height), ImVec2(at.x + width, at.y + height), kInk, S(2));
        if (entered || (left && !ImGui::IsKeyPressed(ImGuiKey_Escape)))
        {
            char* end = nullptr;
            const float typed = std::strtof(context.typed, &end);
            if (end != context.typed && std::isfinite(typed))
            {
                const float settled = settle(typed);
                changed = settled != value;
                value = settled;
            }
            context.typing = 0;
        }
        else if (left)
            context.typing = 0;
        if (active)
            *active = true;
        ImGui::PopID();
        return changed;
    }

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("bar", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (hovered || held)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (active)
        *active = held;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        context.typing = key;
        context.focusTyping = true;
        std::snprintf(context.typed, sizeof(context.typed), "%s", text(value).c_str());
        // The units after the number are not typed.
        for (char* c = context.typed; *c; ++c)
            if (!std::isdigit(static_cast<unsigned char>(*c)) && *c != '.' && *c != '-' && *c != '+' && *c != 'e' && *c != 'E')
            {
                *c = 0;
                break;
            }
    }
    else if (held && range != 0)
    {
        const float settled = settle(bar.min + std::clamp((ImGui::GetIO().MousePos.x - min.x) / size.x, 0.0f, 1.0f) * range);
        changed = settled != value;
        value = settled;
    }
    else if ((hovered || ImGui::IsItemFocused()) && range != 0 && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)))
    {
        // The arrows change the bar the keyboard is on, as they do a slider, rather than move to the control beside it.
        if (ImGui::IsItemFocused())
            ImGui::NavMoveRequestCancel();
        const float direction = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1.0f : -1.0f;
        float stepBy = range * (ImGui::GetIO().KeyShift ? 0.002f : 0.02f);
        if (bar.integer || bar.step > 0)
            stepBy = std::max(std::abs(stepBy), bar.integer ? std::max(bar.step, 1.0f) : bar.step) * (range < 0 ? -1.0f : 1.0f);
        const float settled = settle(value + direction * stepBy);
        changed = settled != value;
        value = settled;
    }
    if (hovered && bar.resettable && reset && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        *reset = true;

    // The fill follows the value with the spring, and at once while it is dragged.
    const float fraction = range != 0 ? std::clamp((value - bar.min) / range, 0.0f, 1.0f) : 0.0f;
    const ImGuiID fillKey = SubId(key, "fill");
    float shown = fraction;
    if (held)
        Snap(fillKey, fraction);
    else
        shown = std::clamp(Animate(fillKey, fraction, 0.5f, Curve::Spring), 0.0f, 1.0f);
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, min + size, kWell, rounding);
    if (shown > 0)
    {
        const ImVec2 fill[] = { min, ImVec2(min.x + size.x * shown, min.y), ImVec2(min.x + size.x * shown, min.y + size.y), ImVec2(min.x, min.y + size.y) };
        // The label and the value lie on the fill in the theme's type, which a bright color would hide in the dark.
        FillInside(draw, min, min + size, rounding, fill, Dark() && bar.color != kQuiet ? Mix(bar.color, kPaper, 0.5f) : bar.color);
    }
    const std::string shownValue = text(value);
    const float valueWidth = Measure(shownValue, kTextBody, true).x;
    if (!bar.label.empty())
        Line(draw, min.x + pad, min.x + size.x - pad - valueWidth - S(9.6f), min.y + size.y / 2, bar.label, kInk, kTextBody, true);
    Line(draw, min.x + std::max(pad, size.x - pad - valueWidth), min.x + size.x - pad + 1, min.y + size.y / 2, shownValue, kInk, kTextBody, true,
         bar.label.empty() && valueWidth < size.x - pad * 2);
    Transform(draw, start, min + size * 0.5f, Animate(SubId(key, "live"), held ? 1.015f : 1, 0.3f, Curve::Spring));
    Tip(bar.tip);
    ImGui::PopID();
    return changed;
}

bool SearchField(const char* id, const char* hint, char* text, size_t size, ImVec2 min, float width, bool focus)
{
    const float height = S(kSearchHeight);
    PushSize(kTextName, true);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kWell);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kWellHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kWell);
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kMist);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(19.2f), std::max((height - ImGui::GetFontSize()) / 2, 0.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(17.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (focus)
        ImGui::SetKeyboardFocusHere();
    const bool edited = ImGui::InputTextWithHint(id, hint, text, size);
    if (ImGui::IsItemActive())
    {
        const float ring = S(2.88f);
        ImGui::GetWindowDrawList()->AddRect(min + ImVec2(ring, ring) * 0.5f, min + ImVec2(width, height) - ImVec2(ring, ring) * 0.5f, kInk,
                                            S(17.6f) - ring / 2, ring);
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopItemWidth();
    ImGui::PopFont();
    return edited;
}

ImVec2 TileDot(ImVec2 min, ImVec2 max, bool small)
{
    const float radius = TileDotRadius(small);
    const float inset = S(small ? 5.6f : 7.2f);
    return ImVec2(max.x - inset - radius, min.y + inset + radius);
}

float TileDotRadius(bool small)
{
    return S(small ? 9.2f : 12);
}

void DrawTile(ImDrawList* draw, ImVec2 min, ImVec2 max, const TileLook& look)
{
    const float rounding = S(kTileRounding);
    const float width = max.x - min.x, height = max.y - min.y;
    draw->AddRectFilled(min, max, kWell, rounding);
    const ImVec2 dot = TileDot(min, max, look.small);
    if (look.cooking)
    {
        // A white shine, slanted like the design's 110 degree gradient, that runs across while the effects compile.
        draw->AddRectFilled(min, max, Alpha(kInk, 0.06f), rounding);
        const float peak = min.x - 0.55f * width + 3.75f * width * look.phase;
        const float slant = 0.364f;
        const float middle = (min.y + max.y) / 2;
        constexpr int kSlices = 10;
        for (int i = 0; i < kSlices; ++i)
        {
            const float o0 = -0.5f * width + width * static_cast<float>(i) / kSlices;
            const float o1 = o0 + width / kSlices;
            const float strength = 0.9f * (1 - std::abs((o0 + o1) / 2) / (0.5f * width));
            const ImVec2 band[] = { ImVec2(peak + o0 - (min.y - middle) * slant, min.y), ImVec2(peak + o1 - (min.y - middle) * slant, min.y),
                                    ImVec2(peak + o1 - (max.y - middle) * slant, max.y), ImVec2(peak + o0 - (max.y - middle) * slant, max.y) };
            FillInside(draw, min, max, rounding, band, Alpha(IM_COL32_WHITE, Dark() ? strength * 0.25f : strength));
        }
    }
    else if (look.reveal > 0)
    {
        // Grayed by its brightness and faded, while every effect is off.
        const float r = static_cast<float>((look.color >> IM_COL32_R_SHIFT) & 0xFF), g = static_cast<float>((look.color >> IM_COL32_G_SHIFT) & 0xFF),
                    b = static_cast<float>((look.color >> IM_COL32_B_SHIFT) & 0xFF);
        const int luma = static_cast<int>(0.3f * r + 0.59f * g + 0.11f * b);
        const ImU32 color = Alpha(Mix(look.color, IM_COL32(luma, luma, luma, 255), look.gray), 1 - 0.7f * look.gray);
        const float full = 1.5f * std::sqrt(width * width + height * height) / std::sqrt(2.0f);
        CircleInside(draw, min, max, rounding, dot, full * look.reveal, color);
    }

    // The name at the bottom left, under any detail line.
    // Black on the tile's color, and the theme's own type once the color is grayed and faded away.
    const ImU32 solid = Mix(kOnColor, kInk, look.gray);
    const ImU32 text = look.cooking ? kMist : Mix(kMist, solid, look.ink);
    const ImVec2 pad(S(look.small ? 8 : 10.4f), S(look.small ? 6.4f : 8));
    const float size = look.small ? kTextTileSmall : kTextTile;
    PushSize(size, true);
    const float line = ImGui::GetFontSize() * 1.1f;
    ImGui::PopFont();
    float bottom = max.y - pad.y;
    draw->PushClipRect(min, max, true);
    if (!look.detail.empty())
    {
        Line(draw, min.x + pad.x, max.x - pad.x, bottom - line / 2, look.detail, look.detailColor, size);
        bottom -= line;
    }
    std::vector<std::string> lines = WrapName(look.name, size, true, width - pad.x * 2);
    // A name with more lines than the tile holds ends its last line with what is left, which Line cuts off.
    const size_t room = static_cast<size_t>(std::max((bottom - min.y - pad.y) / line, 1.0f));
    while (lines.size() > room)
    {
        const std::string rest = lines.back();
        lines.pop_back();
        lines.back() += " " + rest;
    }
    const float dotLeft = dot.x - TileDotRadius(look.small) - S(4);
    const float dotBottom = dot.y + TileDotRadius(look.small) + S(2);
    for (size_t i = lines.size(); i-- > 0;)
    {
        // A line as high as the dot stops before it.
        const float right = bottom - line < dotBottom ? std::min(max.x - pad.x, dotLeft) : max.x - pad.x;
        Line(draw, min.x + pad.x, right, bottom - line / 2, lines[i], text, size, true);
        bottom -= line;
    }
    draw->PopClipRect();

    // The dot: an outline while off, black with the tile's color in the middle while on.
    const float radius = TileDotRadius(look.small) * look.dotScale;
    const float faint = look.cooking ? 0.15f : 1.0f;
    draw->AddCircleFilled(dot, radius, Alpha(solid, look.dot * faint));
    draw->AddCircle(dot, radius - S(1.2f), Alpha(Mix(text, solid, look.dot), (0.45f + 0.55f * look.dot) * faint), 0, S(2.4f));
    if (look.spot > 0)
        draw->AddCircleFilled(dot, (radius - S(look.small ? 3.2f : 4.8f)) * look.spot, look.color);
}

bool MenuRow(const char* label, bool enabled, ImU32 color, bool arrow)
{
    const ImVec2 size(S(kMenuRowWidth), S(35.2f));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("row", size) && enabled;
    const ImGuiID id = ImGui::GetItemID();
    ImGui::PopID();
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float lit = Animate(id, hovered ? 1.0f : 0.0f, 0.12f, Curve::Linear);
    if (lit > 0)
        draw->AddRectFilled(min, min + size, Alpha(hovered && ImGui::IsItemActive() ? kWellHover : kWell, lit), S(11.2f));
    const float middle = min.y + size.y / 2;
    Line(draw, min.x + S(12.8f), min.x + size.x - S(arrow ? 32 : 12.8f), middle, label, enabled ? color : kFaint, kTextBody, true);
    if (arrow)
        Chevron(draw, ImVec2(min.x + size.x - S(17.6f), middle), false, enabled ? kMist : kFaint);
    return clicked;
}

float MenuAppear()
{
    const ImGuiID id = ImGui::GetID("appear");
    if (ImGui::IsWindowAppearing())
        Snap(id, 0.0f);
    const float shown = Animate(id, 1, 0.16f, Curve::Out);
    FadeWindow(ImGui::GetCurrentWindow(), shown);
    return shown;
}

void Toast(const std::string& text)
{
    Context& context = Current();
    context.toast = text;
    context.toastAt = ImGui::GetTime();
}

void DrawToast(ImDrawList* draw, ImVec2 top)
{
    Context& context = Current();
    const float age = static_cast<float>(ImGui::GetTime() - context.toastAt);
    if (context.toast.empty() || age > 1.9f)
        return;
    // In with the spring over half a second, and out over the last 0.3.
    const float in = Ease(Curve::Spring, age / 0.5f);
    const float out = age > 1.6f ? (age - 1.6f) / 0.3f : 0;
    const ImVec2 size(Measure(context.toast, kTextBody, true).x + S(41.6f), S(41.6f));
    const ImVec2 min(top.x - size.x / 2, top.y);
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, min + size, kToast, size.y / 2);
    draw->AddRect(min + ImVec2(0.5f, 0.5f), min + size - ImVec2(0.5f, 0.5f), kToastEdge, size.y / 2);
    Line(draw, min.x, min.x + size.x, min.y + size.y / 2, context.toast, kOnToast, kTextBody, true, true);
    Transform(draw, start, min + size * 0.5f, 0.8f + 0.2f * in, 0, ImVec2(0, -S(24) * (1 - in) - S(9.6f) * out), std::min(in, 1.0f) * (1 - out));
}
} // namespace spectrum
} // namespace kit
