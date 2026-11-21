#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// What the debug view shows: how many frames a second capture gets from the game and Unishade shows, and how long
// Unishade works on each. Counted from the frames Unishade shows, each with the serial of the game's frame in it,
// which goes up by one for every frame capture gets, shown or not. Frames the system drops before capture gets them
// are not counted, so the game may draw more than this says.
struct FrameStats
{
    static constexpr size_t kSeconds = 60; // of frame rates kept for the graph
    static constexpr size_t kFrames = 120; // frame times kept

    struct Second
    {
        float game = 0;
        float shown = 0;
    };

    // False until the first second is counted.
    bool ready = false;
    double gameFps = 0;
    double shownFps = 0;
    // Shown frames with a new frame of the game in them. The rest show the same one again, as the menu's renders do.
    double freshFps = 0;
    double averageMs = 0;
    double worstMs = 0;

    // A frame was shown at time, in seconds, after milliseconds of work.
    void Shown(double time, uint64_t serial, double milliseconds)
    {
        // Capture started over, or nothing was shown for a while, as when the game is behind another window: the
        // time in between says nothing about either rate.
        if (!counting || serial < lastSerial || time - lastTime > 1.0)
        {
            counting = true;
            start = time;
            startSerial = serial;
            shown = fresh = 0;
            sumMs = peakMs = 0;
        }
        else
        {
            ++shown;
            fresh += serial != lastSerial;
            sumMs += milliseconds;
            peakMs = std::max(peakMs, milliseconds);
        }
        lastSerial = serial;
        lastTime = time;
        frames[frameCount++ % kFrames] = static_cast<float>(milliseconds);

        const double elapsed = time - start;
        if (elapsed < 1.0)
            return;
        ready = true;
        gameFps = static_cast<double>(serial - startSerial) / elapsed;
        shownFps = shown / elapsed;
        freshFps = fresh / elapsed;
        averageMs = shown ? sumMs / shown : 0;
        worstMs = peakMs;
        seconds[secondCount++ % kSeconds] = { static_cast<float>(gameFps), static_cast<float>(shownFps) };
        start = time;
        startSerial = serial;
        shown = fresh = 0;
        sumMs = peakMs = 0;
    }

    // The share of the game's frames that were never shown, in percent.
    double SkippedPercent() const
    {
        return gameFps > 0 ? std::clamp((gameFps - freshFps) / gameFps * 100.0, 0.0, 100.0) : 0.0;
    }

    size_t Seconds() const
    {
        return std::min(secondCount, kSeconds);
    }

    // The frame rates of a second, the oldest kept at 0.
    Second SecondAt(size_t index) const
    {
        return seconds[(secondCount - Seconds() + index) % kSeconds];
    }

    size_t Frames() const
    {
        return std::min(frameCount, kFrames);
    }

    // The milliseconds of a frame, the oldest kept at 0.
    float FrameAt(size_t index) const
    {
        return frames[(frameCount - Frames() + index) % kFrames];
    }

private:
    bool counting = false;
    double start = 0;
    double lastTime = 0;
    uint64_t startSerial = 0;
    uint64_t lastSerial = 0;
    int shown = 0;
    int fresh = 0;
    double sumMs = 0;
    double peakMs = 0;
    std::array<Second, kSeconds> seconds{};
    size_t secondCount = 0;
    std::array<float, kFrames> frames{};
    size_t frameCount = 0;
};
