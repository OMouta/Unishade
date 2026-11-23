#include "app/frame_stats.h"

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

bool Near(double value, double expected, double tolerance)
{
    return std::abs(value - expected) <= tolerance;
}

// Shows frames for some seconds at shownRate, while the game draws gameRate frames a second. Each of the game's
// frames is latency seconds old when first shown, or comes without a capture time at 0.
void Run(FrameStats& stats, double from, double seconds, int gameRate, int shownRate, double milliseconds, double latency = 0)
{
    uint64_t lastSerial = UINT64_MAX;
    double captured = 0;
    for (int frame = 0; frame <= static_cast<int>(seconds * shownRate); ++frame)
    {
        const double time = from + static_cast<double>(frame) / shownRate;
        const uint64_t serial = static_cast<uint64_t>(time * gameRate);
        if (serial != lastSerial)
            captured = latency > 0 ? time - latency : 0;
        lastSerial = serial;
        stats.Shown(time, serial, milliseconds, captured);
    }
}
} // namespace

int main()
{
    bool ok = true;

    FrameStats every;
    ok &= Check(!every.ready, "nothing is known before a frame is shown");
    Run(every, 10, 3, 120, 120, 2.0);
    ok &= Check(every.ready && Near(every.gameFps, 120, 2) && Near(every.shownFps, 120, 2), "a game shown frame for frame has both rates alike");
    ok &= Check(every.SkippedPercent() < 2, "no frame of it is skipped");
    ok &= Check(Near(every.averageMs, 2.0, 0.01) && Near(every.worstMs, 2.0, 0.01), "the frame times are the ones given");

    FrameStats half;
    Run(half, 10, 3, 120, 60, 1.0);
    ok &= Check(Near(half.gameFps, 120, 2) && Near(half.shownFps, 60, 2), "a limit of half the game's rate shows half its frames");
    ok &= Check(Near(half.SkippedPercent(), 50, 3), "and skips the other half");

    // The menu renders more often than the game draws.
    FrameStats menu;
    Run(menu, 10, 3, 30, 120, 1.0);
    ok &= Check(Near(menu.gameFps, 30, 2) && Near(menu.shownFps, 120, 2) && Near(menu.freshFps, 30, 2), "repeated frames count as shown, not as the game's");
    ok &= Check(menu.SkippedPercent() < 5, "repeating a frame skips none");

    // Behind another window for a while, then back.
    FrameStats away;
    Run(away, 10, 2, 60, 60, 1.0);
    Run(away, 40, 2, 60, 60, 1.0);
    ok &= Check(Near(away.gameFps, 60, 2) && Near(away.shownFps, 60, 2), "time with nothing shown is left out of the rates");

    // Capture starting over counts its frames from 0 again.
    FrameStats restarted;
    Run(restarted, 10, 2, 60, 60, 1.0);
    for (int frame = 0; frame <= 120; ++frame)
        restarted.Shown(12.1 + frame / 60.0, static_cast<uint64_t>(frame), 1.0, 0);
    ok &= Check(Near(restarted.gameFps, 60, 2), "a serial that starts over does not give a wrong rate");

    // A picture is as old as it was when first shown, however often the menu shows it again.
    FrameStats aged;
    ok &= Check(aged.latencyMs == 0, "no latency is known before a frame is shown");
    Run(aged, 10, 3, 30, 120, 1.0, 0.025);
    ok &= Check(Near(aged.latencyMs, 25, 0.5), "the latency is the age of each new picture when shown");
    FrameStats untimed;
    Run(untimed, 10, 3, 60, 60, 1.0);
    ok &= Check(untimed.ready && untimed.latencyMs == 0, "frames without a capture time give no latency");

    FrameStats kept;
    Run(kept, 0, 90, 60, 60, 1.0);
    ok &= Check(kept.Seconds() == FrameStats::kSeconds && kept.Frames() == FrameStats::kFrames, "only the newest seconds and frames are kept");
    ok &= Check(Near(kept.SecondAt(0).game, 60, 2) && Near(kept.SecondAt(FrameStats::kSeconds - 1).shown, 60, 2), "and read from the oldest to the newest");

    return ok ? 0 : 1;
}
