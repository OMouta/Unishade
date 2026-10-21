#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

struct FrameStatistics
{
    using Clock = std::chrono::steady_clock;
    // Samples kept for the graph, one a second.
    static constexpr size_t kHistory = 60;
    static constexpr size_t kTimingSamples = 512;

    struct Timings
    {
        double prepareMs = 0;
        double depthMs = 0;
        double presentMs = 0;
        double menuMs = 0;
        double captureAgeMs = 0;
        double capacityWaitMs = 0;
    };

    struct GpuTimings
    {
        double prepareMs = 0;
        double depthMs = 0;
        double effectsPresentMs = 0;
    };

    struct Sample
    {
        Clock::time_point at;
        double captureFps;
        double programFps;
        double freshFps;
    };

    bool ready = false;
    double captureFps = 0;
    double programFps = 0;
    double freshFps = 0;
    double processingMs = 0;
    double peakProcessingMs = 0;
    double p95ProcessingMs = 0;
    double p99ProcessingMs = 0;
    Timings cpu;
    bool gpuReady = false;
    double gpuMs = 0;
    double peakGpuMs = 0;
    double p95GpuMs = 0;
    double p99GpuMs = 0;
    GpuTimings gpu;
    double depthFps = 0;
    double depthObservedMs = 0;
    double depthWorkerMs = 0;
    double peakDepthObservedMs = 0;
    uint64_t capacityWaits = 0;

    void Reset(Clock::time_point now, uint64_t capturedFrames)
    {
        *this = {};
        sampleStart = now;
        captureStart = capturedFrames;
    }

    void RecordPresent(int64_t frameTimestamp, double elapsedMs)
    {
        RecordPresent(frameTimestamp, elapsedMs, {});
    }

    void RecordPresent(int64_t frameTimestamp, double elapsedMs, const Timings& timings)
    {
        cpuSamples[presents % cpuSamples.size()] = elapsedMs;
        ++presents;
        if (!lastFrame || frameTimestamp != *lastFrame)
            ++freshFrames;
        lastFrame = frameTimestamp;
        processingTotalMs += elapsedMs;
        processingPeakMs = std::max(processingPeakMs, elapsedMs);
        cpuTotal.prepareMs += timings.prepareMs;
        cpuTotal.depthMs += timings.depthMs;
        cpuTotal.presentMs += timings.presentMs;
        cpuTotal.menuMs += timings.menuMs;
        cpuTotal.captureAgeMs += timings.captureAgeMs;
        cpuTotal.capacityWaitMs += timings.capacityWaitMs;
    }

    // Timestamp queries arrive on later frames. They never make presentation wait for the GPU.
    void RecordGpu(const GpuTimings& timings)
    {
        const double elapsedMs = timings.prepareMs + timings.depthMs + timings.effectsPresentMs;
        gpuSamples[gpuCount % gpuSamples.size()] = elapsedMs;
        ++gpuCount;
        gpuTotal.prepareMs += timings.prepareMs;
        gpuTotal.depthMs += timings.depthMs;
        gpuTotal.effectsPresentMs += timings.effectsPresentMs;
        gpuPeakMs = std::max(gpuPeakMs, elapsedMs);
    }

    // Request-to-fence observation includes the time until the next host redraw checks completion.
    void RecordDepth(double observedMs, double workerMs)
    {
        ++depthCount;
        depthTotalMs += observedMs;
        depthWorkerTotalMs += workerMs;
        depthPeakMs = std::max(depthPeakMs, observedMs);
    }

    void RecordCapacityWait()
    {
        ++capacityWaitCount;
    }

    bool Update(Clock::time_point now, uint64_t capturedFrames)
    {
        const double seconds = std::chrono::duration<double>(now - sampleStart).count();
        if (seconds < 1)
            return false;
        captureFps = (capturedFrames - captureStart) / seconds;
        programFps = presents / seconds;
        freshFps = freshFrames / seconds;
        processingMs = presents ? processingTotalMs / presents : 0;
        peakProcessingMs = processingPeakMs;
        const auto cpuPercentiles = Percentiles(cpuSamples, presents);
        p95ProcessingMs = cpuPercentiles.first;
        p99ProcessingMs = cpuPercentiles.second;
        cpu = presents ? Timings{ cpuTotal.prepareMs / presents, cpuTotal.depthMs / presents, cpuTotal.presentMs / presents,
                                 cpuTotal.menuMs / presents, cpuTotal.captureAgeMs / presents, cpuTotal.capacityWaitMs / presents } : Timings{};
        gpuReady = gpuCount != 0;
        gpu = gpuCount ? GpuTimings{ gpuTotal.prepareMs / gpuCount, gpuTotal.depthMs / gpuCount,
                                     gpuTotal.effectsPresentMs / gpuCount } : GpuTimings{};
        gpuMs = gpu.prepareMs + gpu.depthMs + gpu.effectsPresentMs;
        peakGpuMs = gpuPeakMs;
        const auto gpuPercentiles = Percentiles(gpuSamples, gpuCount);
        p95GpuMs = gpuPercentiles.first;
        p99GpuMs = gpuPercentiles.second;
        depthFps = depthCount / seconds;
        depthObservedMs = depthCount ? depthTotalMs / depthCount : 0;
        depthWorkerMs = depthCount ? depthWorkerTotalMs / depthCount : 0;
        peakDepthObservedMs = depthPeakMs;
        capacityWaits = capacityWaitCount;
        history[historyNext] = { now, captureFps, programFps, freshFps };
        historyNext = (historyNext + 1) % history.size();
        historyCount = std::min(historyCount + 1, history.size());
        ready = true;
        sampleStart = now;
        captureStart = capturedFrames;
        presents = freshFrames = 0;
        processingTotalMs = 0;
        processingPeakMs = 0;
        cpuTotal = {};
        gpuTotal = {};
        gpuCount = depthCount = capacityWaitCount = 0;
        gpuPeakMs = depthTotalMs = depthWorkerTotalMs = depthPeakMs = 0;
        return true;
    }

    double LostFps() const
    {
        return std::max(0.0, captureFps - freshFps);
    }

    double LossPercent() const
    {
        return captureFps > 0 ? LostFps() / captureFps * 100 : 0;
    }

    double RepeatedFps() const
    {
        return std::max(0.0, programFps - freshFps);
    }

    size_t HistorySize() const
    {
        return historyCount;
    }

    // Samples run from oldest to newest, including after the buffer wraps.
    const Sample& HistoryAt(size_t index) const
    {
        return history[(historyNext + history.size() - historyCount + index) % history.size()];
    }

private:
    // Nearest-rank percentiles over the newest bounded set; averages and peaks include every frame.
    static std::pair<double, double> Percentiles(std::array<double, kTimingSamples> samples, uint64_t count)
    {
        const size_t used = static_cast<size_t>(std::min<uint64_t>(count, samples.size()));
        if (!used)
            return {};
        std::sort(samples.begin(), samples.begin() + used);
        return { samples[(used * 95 + 99) / 100 - 1], samples[(used * 99 + 99) / 100 - 1] };
    }

    Clock::time_point sampleStart{};
    uint64_t captureStart = 0;
    uint64_t presents = 0;
    uint64_t freshFrames = 0;
    double processingTotalMs = 0;
    double processingPeakMs = 0;
    Timings cpuTotal;
    GpuTimings gpuTotal;
    uint64_t gpuCount = 0;
    double gpuPeakMs = 0;
    uint64_t depthCount = 0;
    double depthTotalMs = 0;
    double depthWorkerTotalMs = 0;
    double depthPeakMs = 0;
    uint64_t capacityWaitCount = 0;
    std::array<double, kTimingSamples> cpuSamples{};
    std::array<double, kTimingSamples> gpuSamples{};
    std::optional<int64_t> lastFrame;
    std::array<Sample, kHistory> history{};
    size_t historyNext = 0;
    size_t historyCount = 0;
};
