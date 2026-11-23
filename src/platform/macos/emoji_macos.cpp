#include "platform/macos/emoji_macos.h"

#include <imgui_internal.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
// Characters that join the emoji around them or pick between their forms. Dear ImGui draws one character at a time,
// so they take no room instead of showing as question marks.
bool Joins(ImWchar character)
{
    return character == 0x200D || character == 0xFE0E || character == 0xFE0F;
}

CGGlyph GlyphOf(CTFontRef font, ImWchar character)
{
    UniChar units[2] = { static_cast<UniChar>(character), 0 };
    CFIndex count = 1;
    if (character >= 0x10000)
    {
        units[0] = static_cast<UniChar>(0xD800 + ((character - 0x10000) >> 10));
        units[1] = static_cast<UniChar>(0xDC00 + ((character - 0x10000) & 0x3FF));
        count = 2;
    }
    CGGlyph glyphs[2] = {};
    return CTFontGetGlyphsForCharacters(font, units, glyphs, count) ? glyphs[0] : 0;
}

// Core Text answers with another font when the one asked for is missing, which would draw text the fonts before this
// one already cover, so only the emoji font itself counts.
bool SourceInit(ImFontAtlas*, ImFontConfig* source)
{
    CTFontRef font = CTFontCreateWithName(CFSTR("AppleColorEmoji"), 16, nullptr);
    if (!font)
        return false;
    CFStringRef name = CTFontCopyPostScriptName(font);
    const bool emoji = name && CFStringCompare(name, CFSTR("AppleColorEmoji"), 0) == kCFCompareEqualTo;
    if (name)
        CFRelease(name);
    if (!emoji)
    {
        CFRelease(font);
        return false;
    }
    source->FontLoaderData = const_cast<void*>(static_cast<const void*>(font));
    return true;
}

void SourceDestroy(ImFontAtlas*, ImFontConfig* source)
{
    if (source->FontLoaderData)
        CFRelease(static_cast<CTFontRef>(source->FontLoaderData));
    source->FontLoaderData = nullptr;
}

bool SourceContainsGlyph(ImFontAtlas*, ImFontConfig* source, ImWchar character)
{
    return Joins(character) || GlyphOf(static_cast<CTFontRef>(source->FontLoaderData), character) != 0;
}

// Each size has the font at the size its pixels are drawn at, kept where Dear ImGui makes room for it.
bool BakedInit(ImFontAtlas*, ImFontConfig* source, ImFontBaked* baked, void* data)
{
    const float density = source->RasterizerDensity * baked->RasterizerDensity;
    CTFontRef font = CTFontCreateCopyWithAttributes(static_cast<CTFontRef>(source->FontLoaderData), baked->Size * density, nullptr, nullptr);
    *static_cast<CTFontRef*>(data) = font;
    if (font && !source->MergeMode)
    {
        baked->Ascent = std::ceil(static_cast<float>(CTFontGetAscent(font)) / density);
        baked->Descent = -std::ceil(static_cast<float>(CTFontGetDescent(font)) / density);
    }
    return font != nullptr;
}

void BakedDestroy(ImFontAtlas*, ImFontConfig*, ImFontBaked*, void* data)
{
    if (CTFontRef font = *static_cast<CTFontRef*>(data))
        CFRelease(font);
    *static_cast<CTFontRef*>(data) = nullptr;
}

bool BakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* source, ImFontBaked* baked, void* data, ImWchar character, ImFontGlyph* out, float* advanceOnly)
{
    CTFontRef font = *static_cast<CTFontRef*>(data);
    if (!font)
        return false;
    const bool joins = Joins(character);
    CGGlyph glyph = joins ? 0 : GlyphOf(font, character);
    if (!joins && !glyph)
        return false;
    const float density = source->RasterizerDensity * baked->RasterizerDensity;
    const float advance = joins ? 0 : static_cast<float>(CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, nullptr, 1)) / density;
    // Large sizes ask for the width first and the picture when it is drawn.
    if (advanceOnly)
    {
        *advanceOnly = advance;
        return true;
    }
    out->Codepoint = character;
    out->AdvanceX = advance;
    if (joins)
        return true;

    // The picture's box around the baseline, in pixels, with y pointing up as Core Text has it.
    const CGRect box = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
    const int left = static_cast<int>(std::floor(CGRectGetMinX(box))), right = static_cast<int>(std::ceil(CGRectGetMaxX(box)));
    const int bottom = static_cast<int>(std::floor(CGRectGetMinY(box))), top = static_cast<int>(std::ceil(CGRectGetMaxY(box)));
    const int width = right - left, height = top - bottom;
    if (width <= 0 || height <= 0)
        return true;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, static_cast<size_t>(width) * 4, space,
                                                 static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (!context)
        return false;
    const CGPoint origin = CGPointMake(-left, -bottom);
    CTFontDrawGlyphs(font, &glyph, &origin, 1, context);
    CGContextRelease(context);
    // Core Graphics multiplies colors by their alpha, and Dear ImGui's textures do not.
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (const unsigned alpha = pixels[i + 3]; alpha > 0 && alpha < 255)
            for (size_t channel = 0; channel < 3; ++channel)
                pixels[i + channel] = static_cast<unsigned char>(std::min(255u, (pixels[i + channel] * 255u + alpha / 2) / alpha));

    const ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, width, height);
    if (pack == ImFontAtlasRectId_Invalid)
        return false;
    ImTextureRect* rect = ImFontAtlasPackGetRect(atlas, pack);
    // Glyphs are placed from the top of the line, with y pointing down.
    const float baseline = IM_ROUND(baked->Ascent);
    out->X0 = static_cast<float>(left) / density;
    out->X1 = static_cast<float>(right) / density;
    out->Y0 = baseline - static_cast<float>(top) / density;
    out->Y1 = baseline - static_cast<float>(bottom) / density;
    out->Visible = true;
    out->Colored = true;
    out->PackId = pack;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, source, out, rect, pixels.data(), ImTextureFormat_RGBA32, width * 4);
    return true;
}
} // namespace

const ImFontLoader* EmojiLoader()
{
    static const ImFontLoader loader = [] {
        ImFontLoader made;
        made.Name = "Apple Color Emoji";
        made.FontSrcInit = SourceInit;
        made.FontSrcDestroy = SourceDestroy;
        made.FontSrcContainsGlyph = SourceContainsGlyph;
        made.FontBakedInit = BakedInit;
        made.FontBakedDestroy = BakedDestroy;
        made.FontBakedLoadGlyph = BakedLoadGlyph;
        made.FontBakedSrcLoaderDataSize = sizeof(CTFontRef);
        return made;
    }();
    return &loader;
}
