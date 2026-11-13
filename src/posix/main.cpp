// Unishade for macOS and Linux redraws the game's window with ReShade effects in a window of its own over the
// game, the same design as on Windows. The game is only observed from outside, through its window and
// ScreenCaptureKit or XComposite. No game memory is read and no code is injected.

#include "app.h"
#include "log.h"
#include "setup.h"

#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace
{
// Settings, effects and the log need a home folder.
bool HaveDataDirectory()
{
    if (!DataDirectory().empty())
        return true;
    fprintf(stderr, "Unishade cannot find your home folder. Set HOME and try again.\n");
    return false;
}

// Ctrl-C, kill and logging out end Unishade the same way closing the launcher does, so App::Shutdown still saves
// the preset and the pipeline cache and stops a download.
constexpr int kStopSignals[] = { SIGINT, SIGTERM, SIGHUP };
volatile sig_atomic_t stopSignal = 0;
int signalPipe[2] = { -1, -1 };

void OnStopSignal(int signal)
{
    const int savedErrno = errno;
    // A second signal ends Unishade at once, in case quitting is stuck.
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    for (int stop : kStopSignals)
        sigaction(stop, &fallback, nullptr);
    stopSignal = signal;
    if (signalPipe[1] >= 0)
    {
        const unsigned char byte = static_cast<unsigned char>(signal);
        [[maybe_unused]] const ssize_t written = write(signalPipe[1], &byte, 1);
    }
    errno = savedErrno;
}

// A signal the shell set to be ignored, such as SIGHUP under nohup, stays ignored. A handler, unlike a blocked
// signal, is not passed on to the programs Unishade starts.
void CatchStopSignals()
{
    struct sigaction action{};
    action.sa_handler = OnStopSignal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    for (int stop : kStopSignals)
    {
        struct sigaction previous{};
        if (sigaction(stop, nullptr, &previous) == 0 && previous.sa_handler != SIG_IGN)
            sigaction(stop, &action, nullptr);
    }
}

// The launcher a stop signal closes. Never destroyed, since the thread that uses it may still wait while the
// process exits.
struct QuitRequest
{
    std::mutex mutex;
    GLFWwindow* window = nullptr;
    bool requested = false;
};
QuitRequest& quitRequest = *new QuitRequest;

// Turns a stop signal into a request to close the launcher, from a thread of its own, since a signal handler may
// not call GLFW. The thread is detached and only waits, so it does not keep the process alive.
bool WatchStopSignals()
{
    if (pipe(signalPipe) != 0)
        return false;
    for (int fd : signalPipe)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    // The handler must never block on a full pipe. One byte is enough anyway.
    fcntl(signalPipe[1], F_SETFL, fcntl(signalPipe[1], F_GETFL) | O_NONBLOCK);
    std::thread([] {
        unsigned char byte = 0;
        ssize_t count;
        while ((count = read(signalPipe[0], &byte, 1)) < 0 && errno == EINTR) {}
        if (count != 1)
            return;
        std::lock_guard lock(quitRequest.mutex);
        quitRequest.requested = true;
        if (quitRequest.window)
        {
            glfwSetWindowShouldClose(quitRequest.window, GLFW_TRUE);
            glfwPostEmptyEvent();
        }
    }).detach();
    CatchStopSignals();
    return true;
}

// The launcher a stop signal closes, or none once it is about to be destroyed. A signal that came before there was
// a launcher closes it at once.
void SetQuitWindow(GLFWwindow* window)
{
    std::lock_guard lock(quitRequest.mutex);
    quitRequest.window = window;
    if (window && quitRequest.requested)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

// --install-effects. A stop signal cancels the download, which also removes its temporary folder.
int InstallEffects()
{
    CatchStopSignals();
    EffectSetup setup;
    setup.Start();
    std::string last;
    for (EffectSetup::State state = setup.Read(); state.running && !stopSignal; state = setup.Read())
    {
        const std::string line = state.status + (state.detail.empty() ? "" : " - " + state.detail);
        if (line != last)
            printf("%3d%%  %s\n", int(state.fraction * 100), line.c_str());
        last = line;
        usleep(200'000);
    }
    // Waits for the setup thread, and stops it first after a signal.
    setup.Cancel();
    const EffectSetup::State state = setup.Read();
    if (stopSignal)
    {
        Log(LogLevel::Info, "Signal %d received. The install was cancelled.", int(stopSignal));
        fprintf(stderr, "Cancelled.\n");
        return 128 + stopSignal;
    }
    for (const std::string& note : state.notes)
        printf("Skipped %s\n", note.c_str());
    if (!state.error.empty())
    {
        fprintf(stderr, "%s\n", state.error.c_str());
        return 1;
    }
    printf("%s\n", state.status.c_str());
    return 0;
}

void Usage()
{
    printf("Unishade %s\n\n"
           "  unishade                      Starts the launcher.\n"
           "  unishade --install-effects    Downloads effects and presets into %s.\n"
           "  unishade --render IN.png PRESET.ini OUT.png [--gpu-source]\n"
           "                                Applies a preset to an image without a window, to check effects.\n"
           "                                --gpu-source hands the image over on the graphics card, as zero-copy\n"
           "                                capture does.\n",
           UNISHADE_VERSION, DataDirectory().c_str());
}

// Applies a preset to an image with the same code the overlay uses, without any window. gpuSource hands the
// picture over the way zero-copy capture does: from an image on the graphics card, at an offset inside it.
int Render(const char* input, const char* preset, const char* output, bool gpuSource)
{
    std::string error;
    if (!gpu.Init(true, error))
    {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc* rgba = stbi_load(input, &width, &height, &channels, 4);
    if (!rgba)
    {
        fprintf(stderr, "Could not read %s.\n", input);
        return 1;
    }
    std::vector<uint32_t> pixels(size_t(width) * height);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = 0xFF000000u | uint32_t(rgba[i * 4]) << 16 | uint32_t(rgba[i * 4 + 1]) << 8 | rgba[i * 4 + 2];
    stbi_image_free(rgba);

    fx::Runtime runtime;
    if (!runtime.Init(LoadSettings()))
        return 1;
    runtime.LoadPreset(fs::absolute(preset));
    runtime.SetSize(width, height);
    while (runtime.Loading())
    {
        runtime.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runtime.Update();
    int failed = 0;
    for (const std::string& missing : runtime.MissingTechniques())
    {
        fprintf(stderr, "Missing: %s\n", missing.c_str());
        ++failed;
    }
    fx::Runtime::Source source{ pixels.data() };
    GpuImage sourceImage;
    GpuBuffer upload;
    if (gpuSource)
    {
        constexpr uint32_t kX = 7, kY = 3;
        if (!gpu.CreateImage(sourceImage, width + 16, height + 8, 1, VK_FORMAT_B8G8R8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !gpu.CreateBuffer(upload, pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
            return 1;
        std::memcpy(upload.mapped, pixels.data(), pixels.size() * 4);
        VkCommandBuffer commands = gpu.BeginCommands();
        if (!commands)
        {
            fprintf(stderr, "The graphics card is out of memory.\n");
            return 1;
        }
        InitLayout(commands, sourceImage);
        // Something else around the picture, which must not end up in it.
        VkClearColorValue magenta{};
        magenta.float32[0] = magenta.float32[2] = magenta.float32[3] = 1.0f;
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(commands, sourceImage.image, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &range);
        FullBarrier(commands);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageOffset = { int32_t(kX), int32_t(kY), 0 };
        copy.imageExtent = { uint32_t(width), uint32_t(height), 1 };
        vkCmdCopyBufferToImage(commands, upload.buffer, sourceImage.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        if (!gpu.SubmitAndWait(commands))
        {
            fprintf(stderr, "Could not hand %s to the graphics card.\n", input);
            return 1;
        }
        source = { nullptr, sourceImage.image, kX, kY, false };
    }
    // A few frames, so effects that build on earlier frames settle.
    for (int frame = 0; frame < 3; ++frame)
    {
        VkCommandBuffer commands = gpu.BeginCommands();
        if (!commands)
        {
            fprintf(stderr, "The graphics card is out of memory.\n");
            return 1;
        }
        runtime.Render(commands, source, true);
        if (!gpu.SubmitAndWait(commands))
        {
            fprintf(stderr, "The graphics card could not apply %s.\n", preset);
            return 1;
        }
    }
    if (gpuSource)
    {
        // The picture read back from the source image must be the input, exactly.
        const std::vector<uint8_t> original = runtime.ReadSource(source);
        for (size_t i = 0; i < pixels.size(); ++i)
            if (original[i * 4] != ((pixels[i] >> 16) & 0xFF) || original[i * 4 + 1] != ((pixels[i] >> 8) & 0xFF) ||
                original[i * 4 + 2] != (pixels[i] & 0xFF))
            {
                fprintf(stderr, "The picture read from the source image differs from the input.\n");
                ++failed;
                break;
            }
    }
    for (const fx::Technique& technique : runtime.Techniques())
        if (technique.enabled && (!runtime.Effects()[technique.effect].gpu || runtime.Effects()[technique.effect].gpuFailed))
        {
            fprintf(stderr, "Failed: %s\n", fx::TechniqueKey(technique, runtime.Effects()[technique.effect]).c_str());
            ++failed;
        }
    const std::vector<uint8_t> result = runtime.ReadOutput();
    const bool written = stbi_write_png(output, width, height, 4, result.data(), width * 4);
    gpu.DestroyImage(sourceImage);
    gpu.DestroyBuffer(upload);
    runtime.Shutdown();
    gpu.Shutdown();
    if (!written)
    {
        fprintf(stderr, "Could not write %s.\n", output);
        return 1;
    }
    return failed ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    bool relaunched = false;
    for (int i = 1; i < argc; ++i)
    {
        // Started by platform::Relaunch, while the Unishade that asked for it quits.
        if (!strcmp(argv[i], "--relaunch"))
            relaunched = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            Usage();
            return 0;
        }
        if (!strcmp(argv[i], "--version"))
        {
            printf("%s\n", UNISHADE_VERSION);
            return 0;
        }
        // Tests replace the download locations, as with Setup on Windows.
        if (!strcmp(argv[i], "--effects-url") && i + 1 < argc)
        {
            setupSources.effects = argv[++i];
            setupSources.fromCommandLine = true;
        }
        else if (!strcmp(argv[i], "--presets-url") && i + 1 < argc)
        {
            setupSources.presets = argv[++i];
            setupSources.fromCommandLine = true;
        }
        else if (!strcmp(argv[i], "--install-effects"))
        {
            if (!HaveDataDirectory())
                return 1;
            // Setup takes a lock of its own, so this cannot mix with an install the launcher runs. The single-instance
            // lock is the launcher's alone, so the launcher can start while this runs.
            InitTerminalLog();
            const int result = InstallEffects();
            Log(LogLevel::Info, "Session ended: exit_code=%d.", result);
            FlushLog();
            return result;
        }
        else if (!strcmp(argv[i], "--render"))
        {
            if (i + 3 >= argc)
            {
                Usage();
                return 2;
            }
            if (!HaveDataDirectory())
                return 1;
            InitTerminalLog();
            const bool gpuSource = i + 4 < argc && !strcmp(argv[i + 4], "--gpu-source");
            const int result = Render(argv[i + 1], argv[i + 2], argv[i + 3], gpuSource);
            Log(LogLevel::Info, "Session ended: exit_code=%d.", result);
            FlushLog();
            return result;
        }
    }
    if (!HaveDataDirectory())
    {
        platform::ShowStartupError("Unishade cannot find your home folder.");
        return 1;
    }

    // One host at a time, like the mutex on Windows. A restarted one waits a few seconds for the one before it to quit.
    const fs::path lockPath = DataDirectory() / "unishade.lock";
    std::optional<FileLock> instance;
    instance.emplace(lockPath);
    for (int wait = 0; relaunched && instance->Busy() && wait < 100; ++wait)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        instance.emplace(lockPath);
    }
    if (instance->Busy())
    {
        fprintf(stderr, "Unishade is already running.\n");
        platform::ShowAlreadyRunning();
        return 0;
    }

    InitLog();
    // Where locks do not work, such as on some network file systems, a second Unishade cannot be told apart.
    if (!instance->Locked())
        Log(LogLevel::Warning, "Could not lock %s: %s.", lockPath.c_str(), instance->Error().c_str());
    glfwSetErrorCallback([](int code, const char* description) { Log(LogLevel::Warning, "GLFW error %d: %s", code, description); });
#ifndef __APPLE__
    // Only X11 lets the overlay sit over another program's window.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    // MoltenVK on macOS is linked directly, without a loader for GLFW to find.
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (!glfwInit())
    {
        Log(LogLevel::Error, "Could not initialize GLFW. The host needs a supported display.");
        fprintf(stderr, "Unishade could not open a window. It needs an X11 or XWayland display on Linux.\n");
        platform::ShowStartupError("Unishade could not open a window.");
        return 1;
    }

    if (!WatchStopSignals())
        Log(LogLevel::Warning, "Could not watch for signals to quit: %s.", strerror(errno));

    std::string error;
    int result = 0;
    if (app.Init(error))
    {
        SetQuitWindow(app.launcher.window);
        app.Run();
    }
    else
    {
        Log(LogLevel::Error, "%s", error.c_str());
        fprintf(stderr, "Unishade cannot start: %s\n", error.c_str());
        platform::ShowStartupError(error);
        result = 1;
    }
    // The launcher is destroyed next, so the signal thread must leave it alone from here.
    SetQuitWindow(nullptr);
    if (stopSignal)
        Log(LogLevel::Info, "Signal %d received. Quitting.", int(stopSignal));
    app.Shutdown();
    glfwTerminate();
    Log(LogLevel::Info, "Session ended: exit_code=%d.", result);
    FlushLog();
    return result;
}
