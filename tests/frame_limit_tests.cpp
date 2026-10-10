#include "../src/frame_limit.h"

#include <cmath>
#include <cstdio>

namespace
{
constexpr int64_t kSecond = 1'000'000'000;

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

// How many of the frames a game draws in a second are shown. frameTime gives the time of each frame.
template <typename FrameTime>
int Shown(FrameLimit& limit, int frames, int rate, FrameTime frameTime)
{
    int shown = 0;
    for (int frame = 0; frame < frames; ++frame)
        shown += limit.Allow(frameTime(frame), kSecond / rate);
    return shown;
}
} // namespace

int main()
{
    bool ok = true;
    const auto steady = [](int rate) { return [rate](int frame) { return kSecond + frame * kSecond / rate; }; };

    FrameLimit fast;
    ok &= Check(std::abs(Shown(fast, 240, 60, steady(240)) - 60) <= 1, "a faster game is held to the limit");
    FrameLimit uneven;
    ok &= Check(std::abs(Shown(uneven, 144, 60, steady(144)) - 60) <= 1, "the limit is met when it does not divide the game's rate");
    FrameLimit slow;
    ok &= Check(Shown(slow, 30, 60, steady(30)) == 30, "a slower game keeps every frame");

    // 60 frames a second as a 144 Hz screen shows them: each on the next refresh, so two or three refreshes apart.
    FrameLimit quantized;
    const auto onRefresh = [](int frame) { return kSecond + static_cast<int64_t>(std::ceil(frame * 144 / 60.0)) * kSecond / 144; };
    ok &= Check(Shown(quantized, 60, 60, onRefresh) == 60, "a game at the limit keeps every frame although they arrive unevenly");

    // After a pause, the frames that were not shown are not made up for.
    FrameLimit paused;
    Shown(paused, 240, 60, steady(240));
    const auto resumed = [](int frame) { return 10 * kSecond + frame * kSecond / 240; };
    ok &= Check(Shown(paused, 24, 60, resumed) <= 7, "a pause is not followed by a burst");

    FrameLimit capacity;
    ok &= Check(capacity.Ready(kSecond, kSecond / 60), "a new schedule allows its first frame");
    ok &= Check(capacity.Ready(kSecond + 1, kSecond / 60), "checking a frame held by the GPU does not consume the FPS schedule");
    ok &= Check(capacity.Allow(kSecond + 1, kSecond / 60), "a ready frame can be recorded after GPU capacity becomes available");
    ok &= Check(!capacity.Ready(kSecond + 2, kSecond / 60), "a successful submission advances the FPS schedule");
    return ok ? 0 : 1;
}
