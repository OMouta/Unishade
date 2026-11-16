#include "../src/menu_layout.h"

#include <cmath>
#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.001f;
}
} // namespace

int main()
{
    bool ok = true;
    const struct
    {
        float width;
        float height;
        float expectedScale;
    } resolutions[] = {
        { 1280, 720, 0.75f },
        { 1600, 900, 5.0f / 6 },
        { 1920, 1080, 1 },
        { 2560, 1440, 4.0f / 3 },
        { 3840, 2160, 2 },
        { 7680, 4320, 4 },
        { 3440, 1440, 4.0f / 3 },
        { 5120, 1440, 4.0f / 3 },
        { 640, 480, 0.75f },
        { 320, 1080, 0.75f },
        { 400, 1080, 400.0f / 492 },
    };
    for (const auto& resolution : resolutions)
    {
        const float scale = menu_layout::Scale(resolution.width, resolution.height);
        if (!Near(scale, resolution.expectedScale))
        {
            std::printf("Failed: scale for %.0fx%.0f is %f, expected %f\n",
                        resolution.width, resolution.height, scale, resolution.expectedScale);
            ok = false;
        }
        ok &= Check(scale >= menu_layout::kMinScale, "small windows keep the menu readable");
        // Windows that are wide enough at the smallest scale fit the menu and its margins.
        if (resolution.width >= (menu_layout::kWidth + menu_layout::kMargin * 2) * menu_layout::kMinScale)
            ok &= Check((menu_layout::kWidth + menu_layout::kMargin * 2) * scale <= resolution.width + 0.001f,
                        "menu and margins fit the viewport width");
        ok &= Check((menu_layout::kMargin * 2 + menu_layout::MinHeight()) * scale <= resolution.height,
                    "header, tabs and footer leave space for scrollable content");
    }

    ok &= Check(Near(menu_layout::Scale(1920, 1080, 1.5f), 1.5f), "the menu size from Settings applies on top");
    ok &= Check(Near(menu_layout::Scale(640, 480, 2), 1.5f), "the menu size also applies to small windows");
    ok &= Check(Near(menu_layout::Scale(1280, 720, 0.75f), 0.5625f), "a smaller menu size can go below the minimum");
    // A window shorter than the menu at its smallest scales it no further; the menu scrolls instead.
    ok &= Check(Near(menu_layout::Scale(800, 200), menu_layout::kMinScale), "short windows keep the smallest scale");
    ok &= Check((menu_layout::kMargin * 2 + menu_layout::MinHeight()) * menu_layout::Scale(800, 200) > 200,
                "short windows need scrolling");

    ok &= Check(menu_layout::Scale(0, 1080) == 0 && menu_layout::Scale(1920, 0) == 0,
                "empty viewports do not draw");
    ok &= Check(menu_layout::Scale(-1, 1080) == 0 && menu_layout::Scale(1920, -1) == 0,
                "invalid viewports do not draw");
    return ok ? 0 : 1;
}
