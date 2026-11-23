// Add-ons working on frames: the images they share with Unishade, and the order they and the effects run in.

#include "addons/addon_library.h"
#include "gpu/backdrop.h"
#include "app/config.h"
#include "gpu/gpu.h"
#include "app/log.h"

#ifdef _WIN32
#include <unknwn.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan_win32.h>
#include <winrt/base.h>
#else
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <CoreVideo/CVPixelBuffer.h>
#include <IOSurface/IOSurfaceRef.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <vulkan/vulkan_metal.h>
#elif !defined(_WIN32)
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>

using addon::kFrameFlags;
using addon::TakesHdr;
using addon::SitsOutHdr;
using addon::ClearPresses;
using addon::kBeforeEffects;
using addon::kAfterEffects;

namespace
{
// Makes the image an HDR picture is drawn into as SDR, before it is copied to add-ons.
void MakeConvert(backdrop::Target& target, uint32_t width, uint32_t height)
{
    VkCommandBuffer setup = gpu.BeginCommands();
    if (!setup)
        throw std::runtime_error("The graphics card is out of memory.");
    const bool made = backdrop::Make(setup, target, width, height);
    if (!gpu.SubmitAndWait(setup) || !made)
        throw std::runtime_error("The graphics card could not prepare an SDR picture for add-ons.");
}
} // namespace

#ifdef _WIN32
#include "addons/addon_exchange_win32.h"
#else
#include "addons/addon_exchange_unix.h"
#endif

Addons::Addons() = default;

Addons::~Addons() = default;

void Addons::Shutdown()
{
    for (Addon& addon : addons)
        Stop(addon, true);
    exchange.reset();
    addons.clear();
}

bool Addons::Frame(Surface& surface, const fx::Runtime::Source& source, uint64_t picture)
{
    const bool hdrPicture = runtime->Hdr();
    std::vector<Addon*> running, before, after;
    for (Addon& addon : addons)
        if (addon.state == State::Enabled && !addon.bypassed && addon.library->instance && (addon.library->addon->flags & kFrameFlags) &&
            !(hdrPicture && SitsOutHdr(addon)))
        {
            running.push_back(&addon);
            if (addon.library->addon->flags & kBeforeEffects)
                before.push_back(&addon);
            if (addon.library->addon->flags & kAfterEffects)
                after.push_back(&addon);
        }
    const auto flagged = [](const std::vector<Addon*>& list, uint32_t flag) {
        return std::any_of(list.begin(), list.end(), [flag](const Addon* addon) { return (addon->library->addon->flags & flag) != 0; });
    };
    // Add-ons that do not take HDR get the picture as SDR while it is HDR.
    const auto getsSdr = [hdrPicture](const Addon* addon) { return hdrPicture && !TakesHdr(*addon->library->addon); };
    const auto anySdr = [&](const std::vector<Addon*>& list) { return std::any_of(list.begin(), list.end(), getsSdr); };
    // In each stage, add-ons that write depth go first, so the ones that read it have it, then those that only read,
    // then those that write the picture or the result.
    const auto order = [](std::vector<Addon*>& stage, uint32_t writes) {
        const auto rank = [writes](const Addon* addon) {
            const uint32_t flags = addon->library->addon->flags;
            return flags & writes ? 2 : flags & UNISHADE_WRITES_DEPTH ? 0 : 1;
        };
        std::stable_sort(stage.begin(), stage.end(), [&](const Addon* a, const Addon* b) { return rank(a) < rank(b); });
    };
    order(before, UNISHADE_WRITES_PICTURE);
    order(after, UNISHADE_WRITES_RESULT);
    if (running.empty())
    {
        if (exchange)
            exchange->Release();
        return false;
    }
    if (!exchange)
        exchange = std::make_unique<Exchange>();
    try
    {
        exchange->Prepare(runtime->Width(), runtime->Height(), hdrPicture, flagged(before, UNISHADE_WRITES_PICTURE),
                          flagged(before, UNISHADE_WRITES_DEPTH), flagged(after, UNISHADE_WRITES_RESULT), anySdr(running));
    }
    catch (const std::exception& e)
    {
        for (Addon* addon : running)
            Fail(*addon, e.what());
        return false;
    }
#ifdef _WIN32
    catch (const winrt::hresult_error& e)
    {
        for (Addon* addon : running)
            Fail(*addon, "Direct3D failed: " + winrt::to_string(e.message()));
        return false;
    }
#endif

    UnishadeFrame frame{};
    frame.size = sizeof(frame);
    frame.time = told.time;
    frame.width = exchange->width;
    frame.height = exchange->height;
    frame.generation = exchange->generation;
    frame.picture = picture;
    frame.fence = exchange->fenceHandle;
    const float white = runtime->SdrWhite();
    // Has one stage's add-ons work on the picture in color, each on the one before's result. Returns the image that
    // holds the result: null while it is the one Unishade already has. spare is what add-ons that change it write
    // while color holds it. An add-on that gets the picture as SDR only reads it, since the others sit out, and those
    // go before the ones that change it, so the SDR copy of the stage's picture is the picture it would get.
    bool depthWritten = false;
    const auto run = [&](const std::vector<Addon*>& stage, UnishadeStage which, Exchange::Image& spare) {
        Exchange::Image* result = nullptr;
        for (Addon* addon : stage)
        {
            // One that failed before effects is not called after them.
            if (addon->state != State::Enabled)
                continue;
            Library& library = *addon->library;
            const uint32_t flags = library.addon->flags;
            const bool changes = (flags & (which == UNISHADE_STAGE_BEFORE_EFFECTS ? UNISHADE_WRITES_PICTURE : UNISHADE_WRITES_RESULT)) != 0;
            const bool writesDepth = which == UNISHADE_STAGE_BEFORE_EFFECTS && (flags & UNISHADE_WRITES_DEPTH);
            // Depth an add-on wrote for this picture, for one that reads it.
            const bool readsDepth = !writesDepth && depthWritten && (flags & UNISHADE_READS_DEPTH);
            const bool sdr = getsSdr(addon);
            const UnishadeFormat format = exchange->hdr && !sdr ? UNISHADE_FORMAT_RGBA16_FLOAT : UNISHADE_FORMAT_BGRA8;
            Exchange::Image& in = result ? *result : exchange->color;
            Exchange::Image& out = &in == &exchange->color ? spare : exchange->color;
            frame.color = { sdr ? exchange->sdr.handle : in.handle, format };
            frame.output = changes ? UnishadeImage{ out.handle, format } : UnishadeImage{};
            frame.depth = writesDepth || readsDepth ? UnishadeImage{ exchange->depth.handle, UNISHADE_FORMAT_R32_FLOAT } : UnishadeImage{};
            frame.wait = exchange->value;
            frame.signal = exchange->value + 1;
            frame.settings = library.Values();
            frame.stage = which;
            frame.sdrWhite = format == UNISHADE_FORMAT_RGBA16_FLOAT ? white : 1;
            char error[512] = "";
            if (library.addon->frame(library.instance, &frame, error, sizeof(error)))
            {
                exchange->value = frame.signal;
                if (changes)
                    result = &out;
                depthWritten |= writesDepth;
            }
            else
            {
                exchange->Signal(frame.wait, frame.signal);
                Fail(*addon, error[0] ? error : "It failed on a frame, and did not say why.");
            }
            ClearPresses(*addon);
        }
        return result;
    };
    const char* const late = "Add-ons took more than two seconds on a frame, so it shows without them.";

    VkCommandBuffer commands = exchange->Begin(exchange->commands, exchange->done);
    if (!commands)
        return false;
    runtime->CopyIn(commands, source, true);
    if (!before.empty())
    {
        if (!exchange->Submit(exchange->commands, exchange->done, runtime->Output(), 0, anySdr(before) ? white : 0))
            return false;
        Exchange::Image* result = run(before, UNISHADE_STAGE_BEFORE_EFFECTS, exchange->output);
        if (after.empty())
        {
            if (!exchange->Finish(surface, result, depthWritten, runtime->Output(), runtime->Depth()))
                Log(LogLevel::Warning, "%s", late);
            runtime->RenderEffects(surface.commands, true);
            return true;
        }
        // Effects then run in commands of their own, which wait for these add-ons, so the add-ons after them have
        // their result in this frame.
        if (!exchange->semaphore && !exchange->WaitHere())
        {
            Log(LogLevel::Warning, "%s", late);
            return false;
        }
        commands = exchange->Begin(exchange->effectsCommands, exchange->effectsDone);
        if (!commands)
            return false;
        exchange->Take(commands, result, depthWritten, runtime->Output(), runtime->Depth());
    }
    runtime->RenderEffects(commands, true);
    FullBarrier(commands);
    const bool between = !before.empty();
    if (!exchange->Submit(between ? exchange->effectsCommands : exchange->commands, between ? exchange->effectsDone : exchange->done, runtime->Output(),
                          between ? exchange->value : 0, anySdr(after) ? white : 0))
    {
        // The caller renders the effects again, which must not take the same key and button presses twice: a
        // technique's shortcut would turn it on and off again.
        runtime->input.keysPressed = {};
        runtime->input.buttonsPressed = {};
        return false;
    }
    Exchange::Image* result = run(after, UNISHADE_STAGE_AFTER_EFFECTS, exchange->after);
    if (!exchange->Finish(surface, result, false, runtime->Output(), runtime->Depth()))
        Log(LogLevel::Warning, "%s", late);
    return true;
}
