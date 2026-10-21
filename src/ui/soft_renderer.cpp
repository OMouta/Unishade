#include "soft_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace soft
{
namespace
{
// Texels are 0xAABBGGRR, as Dear ImGui packs colors.
struct Texture
{
    int width = 0;
    int height = 0;
    std::vector<uint32_t> texels;
};

struct Target
{
    uint32_t* pixels;
    int width;
    // The clip rectangle, right and bottom excluded.
    int left, top, right, bottom;
};

struct Color
{
    float r, g, b, a;
};

Color Unpack(uint32_t packed)
{
    return { static_cast<float>(packed & 0xFF), static_cast<float>((packed >> 8) & 0xFF), static_cast<float>((packed >> 16) & 0xFF),
             static_cast<float>(packed >> 24) };
}

// Bilinear, with the edges clamped, as texture coordinates from 0 to 1 cover the whole texture.
uint32_t Sample(const Texture& texture, float u, float v)
{
    const float x = u * static_cast<float>(texture.width) - 0.5f;
    const float y = v * static_cast<float>(texture.height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const int x0 = std::clamp(static_cast<int>(fx), 0, texture.width - 1), x1 = std::clamp(static_cast<int>(fx) + 1, 0, texture.width - 1);
    const int y0 = std::clamp(static_cast<int>(fy), 0, texture.height - 1), y1 = std::clamp(static_cast<int>(fy) + 1, 0, texture.height - 1);
    const int wx = static_cast<int>((x - fx) * 256), wy = static_cast<int>((y - fy) * 256);
    const uint32_t* row0 = texture.texels.data() + static_cast<size_t>(y0) * texture.width;
    const uint32_t* row1 = texture.texels.data() + static_cast<size_t>(y1) * texture.width;
    const uint32_t a = row0[x0], b = row0[x1], c = row1[x0], d = row1[x1];
    // Exact texels, such as for text drawn at the size it was made, need no mixing.
    if ((wx == 0 || a == b) && (wy == 0 || a == c) && (wx == 0 || wy == 0 || a == d))
        return a;
    uint32_t result = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const int top = static_cast<int>((a >> shift) & 0xFF) * (256 - wx) + static_cast<int>((b >> shift) & 0xFF) * wx;
        const int bottom = static_cast<int>((c >> shift) & 0xFF) * (256 - wx) + static_cast<int>((d >> shift) & 0xFF) * wx;
        result |= static_cast<uint32_t>(((top * (256 - wy) + bottom * wy) >> 16) & 0xFF) << shift;
    }
    return result;
}

// Mixes a color over a pixel by its alpha. Channels go from 0 to 255.
inline void Blend(uint32_t& pixel, int r, int g, int b, int a)
{
    if (a <= 0)
        return;
    if (a >= 255)
    {
        pixel = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        return;
    }
    const int keep = 255 - a;
    const int pr = static_cast<int>((pixel >> 16) & 0xFF), pg = static_cast<int>((pixel >> 8) & 0xFF), pb = static_cast<int>(pixel & 0xFF);
    pixel = (static_cast<uint32_t>((r * a + pr * keep + 127) / 255) << 16) | (static_cast<uint32_t>((g * a + pg * keep + 127) / 255) << 8) |
            static_cast<uint32_t>((b * a + pb * keep + 127) / 255);
}

// A vertex color times a texel, both 0xAABBGGRR.
inline void BlendModulated(uint32_t& pixel, uint32_t color, uint32_t texel)
{
    const auto channel = [](uint32_t x, uint32_t y, int shift) { return static_cast<int>((((x >> shift) & 0xFF) * ((y >> shift) & 0xFF) + 127) / 255); };
    if (texel == 0xFFFFFFFF)
        Blend(pixel, static_cast<int>(color & 0xFF), static_cast<int>((color >> 8) & 0xFF), static_cast<int>((color >> 16) & 0xFF), static_cast<int>(color >> 24));
    else
        Blend(pixel, channel(color, texel, 0), channel(color, texel, 8), channel(color, texel, 16), channel(color, texel, 24));
}

// An axis-aligned rectangle, which most of what Dear ImGui draws is: backgrounds, buttons and every letter. Covers the
// pixels whose centers are inside, like the graphics card.
void FillRect(const Target& target, ImVec2 min, ImVec2 max, uint32_t color, const Texture* texture, ImVec2 uv0, ImVec2 uv1)
{
    const int left = std::max(target.left, static_cast<int>(std::ceil(min.x - 0.5f)));
    const int right = std::min(target.right, static_cast<int>(std::ceil(max.x - 0.5f)));
    const int top = std::max(target.top, static_cast<int>(std::ceil(min.y - 0.5f)));
    const int bottom = std::min(target.bottom, static_cast<int>(std::ceil(max.y - 0.5f)));
    if (left >= right || top >= bottom)
        return;
    const bool flat = !texture || (uv0.x == uv1.x && uv0.y == uv1.y);
    if (flat)
    {
        const uint32_t texel = texture ? Sample(*texture, uv0.x, uv0.y) : 0xFFFFFFFF;
        const auto channel = [&](int shift) { return static_cast<int>((((color >> shift) & 0xFF) * ((texel >> shift) & 0xFF) + 127) / 255); };
        const int r = channel(0), g = channel(8), b = channel(16), a = channel(24);
        if (a <= 0)
            return;
        const uint32_t opaque = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        for (int y = top; y < bottom; ++y)
        {
            uint32_t* row = target.pixels + static_cast<size_t>(y) * target.width;
            if (a >= 255)
                std::fill(row + left, row + right, opaque);
            else
                for (int x = left; x < right; ++x)
                    Blend(row[x], r, g, b, a);
        }
        return;
    }
    const float du = (uv1.x - uv0.x) / (max.x - min.x);
    const float dv = (uv1.y - uv0.y) / (max.y - min.y);
    for (int y = top; y < bottom; ++y)
    {
        uint32_t* row = target.pixels + static_cast<size_t>(y) * target.width;
        const float v = uv0.y + (static_cast<float>(y) + 0.5f - min.y) * dv;
        for (int x = left; x < right; ++x)
        {
            const float u = uv0.x + (static_cast<float>(x) + 0.5f - min.x) * du;
            BlendModulated(row[x], color, Sample(*texture, u, v));
        }
    }
}

// Any other triangle: rounded corners, circles, lines and the soft edges around them. Pixels on an edge two triangles
// share are covered by one of them only, so see-through shapes show no seams.
void FillTriangle(const Target& target, const ImDrawVert* a, const ImDrawVert* b, const ImDrawVert* c, const Texture* texture)
{
    float area = (b->pos.x - a->pos.x) * (c->pos.y - a->pos.y) - (b->pos.y - a->pos.y) * (c->pos.x - a->pos.x);
    if (area < 0)
    {
        std::swap(b, c);
        area = -area;
    }
    if (area < 1e-6f)
        return;
    const int left = std::max(target.left, static_cast<int>(std::floor(std::min({ a->pos.x, b->pos.x, c->pos.x }))));
    const int right = std::min(target.right - 1, static_cast<int>(std::ceil(std::max({ a->pos.x, b->pos.x, c->pos.x }))));
    const int top = std::max(target.top, static_cast<int>(std::floor(std::min({ a->pos.y, b->pos.y, c->pos.y }))));
    const int bottom = std::min(target.bottom - 1, static_cast<int>(std::ceil(std::max({ a->pos.y, b->pos.y, c->pos.y }))));
    if (left > right || top > bottom)
        return;

    // Each edge's function is the weight of the vertex across from it, times area.
    struct Edge
    {
        float dx, dy, c; // value = dx * x + dy * y + c
        bool inclusive;   // covers pixels exactly on it
    };
    const auto edge = [](const ImVec2& from, const ImVec2& to) {
        const float ex = to.x - from.x, ey = to.y - from.y;
        return Edge{ -ey, ex, ey * from.x - ex * from.y, ey > 0 || (ey == 0 && ex < 0) };
    };
    const Edge edges[3] = { edge(b->pos, c->pos), edge(c->pos, a->pos), edge(a->pos, b->pos) };
    const auto inside = [&](const float (&values)[3]) {
        for (int i = 0; i < 3; ++i)
            if (values[i] < 0 || (values[i] == 0 && !edges[i].inclusive))
                return false;
        return true;
    };

    const bool sameColor = a->col == b->col && a->col == c->col;
    const bool sameUv = a->uv.x == b->uv.x && a->uv.x == c->uv.x && a->uv.y == b->uv.y && a->uv.y == c->uv.y;
    const uint32_t flatTexel = !texture ? 0xFFFFFFFF : sameUv ? Sample(*texture, a->uv.x, a->uv.y) : 0;
    const Color ca = Unpack(a->col), cb = Unpack(b->col), cc = Unpack(c->col);

    for (int y = top; y <= bottom; ++y)
    {
        const float py = static_cast<float>(y) + 0.5f;
        float start[3];
        for (int i = 0; i < 3; ++i)
            start[i] = edges[i].dx * (static_cast<float>(left) + 0.5f) + edges[i].dy * py + edges[i].c;
        // The span where every edge is on the inside, found from each edge's line and checked pixel by pixel at its ends.
        float from = static_cast<float>(left), to = static_cast<float>(right);
        bool empty = false;
        for (int i = 0; i < 3 && !empty; ++i)
        {
            if (edges[i].dx > 0)
                from = std::max(from, static_cast<float>(left) + std::floor(-start[i] / edges[i].dx) - 1);
            else if (edges[i].dx < 0)
                to = std::min(to, static_cast<float>(left) + std::ceil(start[i] / -edges[i].dx) + 1);
            else
                empty = start[i] < 0 || (start[i] == 0 && !edges[i].inclusive);
        }
        if (empty || from > to)
            continue;
        int x0 = std::max(left, static_cast<int>(from)), x1 = std::min(right, static_cast<int>(to));
        const auto valuesAt = [&](int x, float (&values)[3]) {
            for (int i = 0; i < 3; ++i)
                values[i] = start[i] + edges[i].dx * static_cast<float>(x - left);
        };
        float values[3];
        for (valuesAt(x0, values); x0 <= x1 && !inside(values); valuesAt(++x0, values))
        {
        }
        for (valuesAt(x1, values); x1 >= x0 && !inside(values); valuesAt(--x1, values))
        {
        }
        if (x0 > x1)
            continue;

        uint32_t* row = target.pixels + static_cast<size_t>(y) * target.width;
        // Most triangles are one color, such as the inside of a rounded card.
        if (sameColor && (!texture || sameUv))
        {
            for (int x = x0; x <= x1; ++x)
                BlendModulated(row[x], a->col, flatTexel);
            continue;
        }
        valuesAt(x0, values);
        for (int x = x0; x <= x1; ++x)
        {
            const float wa = values[0] / area, wb = values[1] / area, wc = 1 - wa - wb;
            for (int i = 0; i < 3; ++i)
                values[i] += edges[i].dx;
            uint32_t color = a->col;
            if (!sameColor)
            {
                const auto mix = [&](float Color::*channel) {
                    return static_cast<uint32_t>(std::clamp(ca.*channel * wa + cb.*channel * wb + cc.*channel * wc + 0.5f, 0.0f, 255.0f));
                };
                color = mix(&Color::r) | (mix(&Color::g) << 8) | (mix(&Color::b) << 16) | (mix(&Color::a) << 24);
            }
            uint32_t texel = flatTexel;
            if (texture && !sameUv)
                texel = Sample(*texture, a->uv.x * wa + b->uv.x * wb + c->uv.x * wc, a->uv.y * wa + b->uv.y * wb + c->uv.y * wc);
            BlendModulated(row[x], color, texel);
        }
    }
}

// Two triangles Dear ImGui makes a rectangle of: corners a, b, c and d clockwise from the top left, with one color and
// texture coordinates that line up with the corners.
bool IsRect(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImDrawVert& d)
{
    return a.pos.y == b.pos.y && b.pos.x == c.pos.x && c.pos.y == d.pos.y && d.pos.x == a.pos.x && a.pos.x < b.pos.x && a.pos.y < d.pos.y &&
           a.col == b.col && a.col == c.col && a.col == d.col && a.uv.y == b.uv.y && b.uv.x == c.uv.x && c.uv.y == d.uv.y && d.uv.x == a.uv.x;
}

void UpdateTexture(ImTextureData* data)
{
    if (data->Status == ImTextureStatus_WantDestroy && data->UnusedFrames > 0)
    {
        delete static_cast<Texture*>(data->BackendUserData);
        data->BackendUserData = nullptr;
        data->SetTexID(ImTextureID_Invalid);
        data->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (data->Status != ImTextureStatus_WantCreate && data->Status != ImTextureStatus_WantUpdates)
        return;
    auto* texture = static_cast<Texture*>(data->BackendUserData);
    if (!texture)
    {
        texture = new Texture;
        data->BackendUserData = texture;
        data->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)));
    }
    // A new texture copies everything, an update only what changed.
    ImTextureRect whole{ 0, 0, static_cast<unsigned short>(data->Width), static_cast<unsigned short>(data->Height) };
    if (texture->width != data->Width || texture->height != data->Height)
    {
        texture->width = data->Width;
        texture->height = data->Height;
        texture->texels.assign(static_cast<size_t>(data->Width) * data->Height, 0);
        data->Updates.clear();
    }
    const bool everything = data->Status == ImTextureStatus_WantCreate || data->Updates.empty();
    const ImTextureRect* rects = everything ? &whole : data->Updates.Data;
    const int count = everything ? 1 : data->Updates.Size;
    for (int i = 0; i < count; ++i)
        for (int y = rects[i].y; y < rects[i].y + rects[i].h; ++y)
            for (int x = rects[i].x; x < rects[i].x + rects[i].w; ++x)
            {
                const unsigned char* source = static_cast<const unsigned char*>(data->GetPixelsAt(x, y));
                texture->texels[static_cast<size_t>(y) * data->Width + x] =
                    data->Format == ImTextureFormat_Alpha8
                        ? 0x00FFFFFFu | (static_cast<uint32_t>(source[0]) << 24)
                        : static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8) | (static_cast<uint32_t>(source[2]) << 16) |
                              (static_cast<uint32_t>(source[3]) << 24);
            }
    data->SetStatus(ImTextureStatus_OK);
}
} // namespace

void Init()
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "unishade_soft";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
}

void Shutdown()
{
    for (ImTextureData* data : ImGui::GetPlatformIO().Textures)
        if (data->RefCount == 1 && data->BackendUserData)
        {
            delete static_cast<Texture*>(data->BackendUserData);
            data->BackendUserData = nullptr;
            data->SetTexID(ImTextureID_Invalid);
            data->SetStatus(ImTextureStatus_Destroyed);
        }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
}

void Render(ImDrawData* data, uint32_t* pixels, int width, int height)
{
    if (data->Textures)
        for (ImTextureData* texture : *data->Textures)
            if (texture->Status != ImTextureStatus_OK)
                UpdateTexture(texture);

    const ImVec2 offset = data->DisplayPos;
    const ImVec2 scale = data->FramebufferScale;
    std::vector<ImDrawVert> vertices;
    for (const ImDrawList* list : data->CmdLists)
    {
        // In pixels, from the top left of the window.
        vertices.assign(list->VtxBuffer.begin(), list->VtxBuffer.end());
        for (ImDrawVert& vertex : vertices)
            vertex.pos = ImVec2((vertex.pos.x - offset.x) * scale.x, (vertex.pos.y - offset.y) * scale.y);
        for (const ImDrawCmd& command : list->CmdBuffer)
        {
            if (command.UserCallback)
            {
                if (command.UserCallback != ImDrawCallback_ResetRenderState)
                    command.UserCallback(list, &command);
                continue;
            }
            Target target{ pixels, width };
            target.left = std::max(0, static_cast<int>((command.ClipRect.x - offset.x) * scale.x));
            target.top = std::max(0, static_cast<int>((command.ClipRect.y - offset.y) * scale.y));
            target.right = std::min(width, static_cast<int>((command.ClipRect.z - offset.x) * scale.x));
            target.bottom = std::min(height, static_cast<int>((command.ClipRect.w - offset.y) * scale.y));
            if (target.left >= target.right || target.top >= target.bottom)
                continue;
            const ImTextureID id = command.GetTexID();
            const Texture* texture = id == ImTextureID_Invalid ? nullptr : reinterpret_cast<const Texture*>(static_cast<intptr_t>(id));
            const ImDrawIdx* indices = list->IdxBuffer.Data + command.IdxOffset;
            const ImDrawVert* base = vertices.data() + command.VtxOffset;
            for (unsigned i = 0; i + 2 < command.ElemCount; i += 3)
            {
                const ImDrawVert& a = base[indices[i]];
                const ImDrawVert& b = base[indices[i + 1]];
                const ImDrawVert& c = base[indices[i + 2]];
                // Dear ImGui's rectangles: a, b, c then a, c, d.
                if (i + 5 < command.ElemCount && indices[i + 3] == indices[i] && indices[i + 4] == indices[i + 2])
                {
                    const ImDrawVert& d = base[indices[i + 5]];
                    if (IsRect(a, b, c, d))
                    {
                        FillRect(target, a.pos, c.pos, a.col, texture, a.uv, c.uv);
                        i += 3;
                        continue;
                    }
                }
                FillTriangle(target, &a, &b, &c, texture);
            }
        }
    }
}

ImTextureID CreateTexture(const uint8_t* rgba, int width, int height)
{
    auto* texture = new Texture{ width, height, std::vector<uint32_t>(static_cast<size_t>(width) * height) };
    std::memcpy(texture->texels.data(), rgba, texture->texels.size() * 4);
    return static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture));
}

void DestroyTexture(ImTextureID texture)
{
    delete reinterpret_cast<Texture*>(static_cast<intptr_t>(texture));
}
} // namespace soft
