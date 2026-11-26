// Runs an image through Unishade_nvngx.dll, writes the result as PNG and says how far it is from the input.
// dlss5_check <nvngx_dlssnr.dll> <image> <output folder> [--module <dll>] [--frames N] [--passes N] [--style N]
//     [--intensity X] [--local-tone X] [--local-structure X] [--skin-structure X] [--no-auto-mask] [--keys <image>]
// It also runs the top-left quarter of the image, so the feature is made again at a new size. With --keys it then
// shows that each input Unishade_nvngx.dll sets reaches the network, since NGX ignores a parameter it does not know
// without saying so: it runs the image with only that input changed and compares the outputs. Then it runs the add-on
// as Unishade does, on the same picture twice, a cut and a small move. The image after --keys is the scene the cut
// goes to. It returns 1 when an input changed nothing or the add-on did the wrong thing. With --keys it also runs a
// pan with its true motion vectors, with them negated and with none, and says which is closest to a still picture's
// result, since nothing NVIDIA published says which way round DLSS5 takes them.

#include "dlss5.h"
#include <unishade_addon.h>

#include <DirectXPackedVector.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4996) // sprintf in the HDR writer
#include <stb_image_write.h>
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Api
{
    decltype(&Dlss5Create) create;
    decltype(&Dlss5Device) device;
    decltype(&Dlss5Evaluate) evaluate;
    decltype(&Dlss5Error) error;
    decltype(&Dlss5Destroy) destroy;
};

struct Image
{
    UINT width = 0, height = 0;
    std::vector<uint8_t> pixels; // four bytes for each pixel, rows packed
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
};

void Print(const char* line)
{
    std::printf("%s\n", line);
    std::fflush(stdout);
}

void Check(HRESULT result, const char* call)
{
    if (FAILED(result))
    {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(result));
        throw std::runtime_error(std::string(call) + " failed: " + code);
    }
}

template <typename Function>
void Resolve(HMODULE module, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!function)
        throw std::runtime_error(std::string("The module has no export ") + name);
}

// Uploads, copies and reads back on its own queue, waiting for each submission.
struct Gpu
{
    ID3D12Device* device;
    winrt::com_ptr<ID3D12CommandQueue> queue;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;

    explicit Gpu(ID3D12Device* device) : device(device)
    {
        const D3D12_COMMAND_QUEUE_DESC description{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        Check(device->CreateCommandQueue(&description, IID_PPV_ARGS(queue.put())), "CreateCommandQueue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())), "CreateCommandAllocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put())), "CreateCommandList");
        Check(list->Close(), "Close");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())), "CreateFence");
    }

    template <typename Record>
    void Submit(Record record)
    {
        Check(allocator->Reset(), "ID3D12CommandAllocator::Reset");
        Check(list->Reset(allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset");
        record(list.get());
        Check(list->Close(), "ID3D12GraphicsCommandList::Close");
        ID3D12CommandList* lists[] = { list.get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.get(), ++fenceValue), "Signal");
        Check(fence->SetEventOnCompletion(fenceValue, nullptr), "SetEventOnCompletion");
        Check(device->GetDeviceRemovedReason(), "The D3D12 device");
    }

    winrt::com_ptr<ID3D12Resource> Texture(UINT width, UINT height, D3D12_RESOURCE_FLAGS flags, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        const D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC description{ D3D12_RESOURCE_DIMENSION_TEXTURE2D };
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Flags = flags;
        winrt::com_ptr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(texture.put())), "CreateCommittedResource");
        return texture;
    }

    winrt::com_ptr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE type, UINT64 size)
    {
        const D3D12_HEAP_PROPERTIES heap{ type };
        D3D12_RESOURCE_DESC description{ D3D12_RESOURCE_DIMENSION_BUFFER };
        description.Width = size;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        winrt::com_ptr<ID3D12Resource> buffer;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                              type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(buffer.put())), "CreateCommittedResource");
        return buffer;
    }

    void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, D3D12_RESOURCE_STATES state, auto copy)
    {
        D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        barrier.Transition = { texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, state };
        list->ResourceBarrier(1, &barrier);
        copy();
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    winrt::com_ptr<ID3D12Resource> Upload(const Image& image)
    {
        winrt::com_ptr<ID3D12Resource> texture = Texture(image.width, image.height, D3D12_RESOURCE_FLAG_NONE, image.format);
        const D3D12_RESOURCE_DESC description = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT64 size = 0;
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        winrt::com_ptr<ID3D12Resource> buffer = Buffer(D3D12_HEAP_TYPE_UPLOAD, size);
        uint8_t* mapped = nullptr;
        Check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map");
        for (UINT row = 0; row < image.height; ++row)
            std::copy_n(image.pixels.data() + size_t(row) * image.width * 4, size_t(image.width) * 4, mapped + size_t(row) * footprint.Footprint.RowPitch);
        buffer->Unmap(0, nullptr);
        Submit([&](ID3D12GraphicsCommandList* list) {
            Copy(list, texture.get(), D3D12_RESOURCE_STATE_COPY_DEST, [&] {
                const D3D12_TEXTURE_COPY_LOCATION to{ texture.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { .SubresourceIndex = 0 } };
                const D3D12_TEXTURE_COPY_LOCATION from{ buffer.get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, { .PlacedFootprint = footprint } };
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            });
        });
        return texture;
    }

    Image Read(ID3D12Resource* texture)
    {
        const D3D12_RESOURCE_DESC description = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT64 size = 0;
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        winrt::com_ptr<ID3D12Resource> buffer = Buffer(D3D12_HEAP_TYPE_READBACK, size);
        Submit([&](ID3D12GraphicsCommandList* list) {
            Copy(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, [&] {
                const D3D12_TEXTURE_COPY_LOCATION to{ buffer.get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, { .PlacedFootprint = footprint } };
                const D3D12_TEXTURE_COPY_LOCATION from{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { .SubresourceIndex = 0 } };
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            });
        });
        Image image{ static_cast<UINT>(description.Width), description.Height };
        image.pixels.resize(size_t(image.width) * image.height * 4);
        uint8_t* mapped = nullptr;
        Check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map");
        for (UINT row = 0; row < image.height; ++row)
            std::copy_n(mapped + size_t(row) * footprint.Footprint.RowPitch, size_t(image.width) * 4, image.pixels.data() + size_t(row) * image.width * 4);
        const D3D12_RANGE written{};
        buffer->Unmap(0, &written);
        return image;
    }
};

Image Crop(const Image& image, UINT width, UINT height, UINT left = 0, UINT top = 0)
{
    Image crop{ width, height };
    crop.pixels.resize(size_t(width) * height * 4);
    for (UINT row = 0; row < height; ++row)
        std::copy_n(image.pixels.data() + (size_t(top + row) * image.width + left) * 4, size_t(width) * 4, crop.pixels.data() + size_t(row) * width * 4);
    return crop;
}

Image Load(const std::filesystem::path& path)
{
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!pixels)
        throw std::runtime_error("Could not read " + path.string() + ": " + stbi_failure_reason());
    Image image{ static_cast<UINT>(width), static_cast<UINT>(height) };
    image.pixels.assign(pixels, pixels + size_t(width) * height * 4);
    stbi_image_free(pixels);
    std::printf("Read %s: %dx%d\n", path.string().c_str(), width, height);
    return image;
}

void Save(const Image& image, const std::filesystem::path& path)
{
    if (!stbi_write_png(path.string().c_str(), static_cast<int>(image.width), static_cast<int>(image.height), 4, image.pixels.data(),
                        static_cast<int>(image.width * 4)))
        throw std::runtime_error("Could not write " + path.string());
    std::printf("Wrote %s\n", path.string().c_str());
}

void Compare(const Image& input, const Image& output)
{
    double sum[3]{};
    size_t changed = 0;
    int largest = 0;
    const size_t count = size_t(input.width) * input.height;
    for (size_t pixel = 0; pixel < count; ++pixel)
    {
        int most = 0;
        for (int channel = 0; channel < 3; ++channel)
        {
            const int difference = std::abs(int(output.pixels[pixel * 4 + channel]) - int(input.pixels[pixel * 4 + channel]));
            sum[channel] += difference;
            most = std::max(most, difference);
        }
        largest = std::max(largest, most);
        changed += most > 8;
    }
    std::printf("Mean difference from the input, out of 255: R %.2f G %.2f B %.2f. Largest %d. Pixels off by more than 8: %.1f%%\n",
                sum[0] / count, sum[1] / count, sum[2] / count, largest, 100.0 * changed / count);
}

// Evaluates the same image for a number of frames, as a still scene would give it, and reads the last one back.
Image Run(const Api& api, Dlss5* dlss5, Gpu& gpu, const Image& input, const Dlss5Look& look, int frames)
{
    const winrt::com_ptr<ID3D12Resource> color = gpu.Upload(input);
    const winrt::com_ptr<ID3D12Resource> output = gpu.Texture(input.width, input.height, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (int frame = 1; frame <= frames; ++frame)
    {
        const auto start = std::chrono::steady_clock::now();
        if (!api.evaluate(dlss5, color.get(), nullptr, nullptr, 1, 1, output.get(), &look, false))
            throw std::runtime_error(std::string("Dlss5Evaluate failed: ") + api.error(dlss5));
        const auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("Evaluated frame %d at %ux%u with %d pass(es) in %.1f ms\n", frame, input.width, input.height, look.passCount, took);
    }
    Image result = gpu.Read(output.get());
    Compare(input, result);
    return result;
}

// Depth as Unishade gives it, 0 nearest and 1 at the far plane, non-linear for a far plane of 1000. A floor reaches
// from near at the bottom to the far plane at the top. Middle is half as far as the far plane everywhere.
enum class Shape
{
    Floor,
    Middle,
    Zero,
};

Image Depth(UINT width, UINT height, Shape shape)
{
    constexpr float kFarPlane = 1000;
    Image depth{ width, height, std::vector<uint8_t>(size_t(width) * height * 4), DXGI_FORMAT_R32_FLOAT };
    for (UINT row = 0; row < height; ++row)
    {
        const float distance = shape == Shape::Floor ? std::max(0.01f, 1.0f - float(row) / height) : 0.5f;
        const float stored = shape == Shape::Zero ? 0.0f : distance * kFarPlane / (1 + distance * (kFarPlane - 1));
        for (UINT column = 0; column < width; ++column)
            std::memcpy(depth.pixels.data() + (size_t(row) * width + column) * 4, &stored, 4);
    }
    return depth;
}

// The same vector for every pixel, as R16G16_FLOAT.
Image Motion(UINT width, UINT height, float x, float y)
{
    Image motion{ width, height, std::vector<uint8_t>(size_t(width) * height * 4), DXGI_FORMAT_R16G16_FLOAT };
    const DirectX::PackedVector::HALF vector[2] = { DirectX::PackedVector::XMConvertFloatToHalf(x), DirectX::PackedVector::XMConvertFloatToHalf(y) };
    for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel)
        std::memcpy(motion.pixels.data() + pixel * 4, vector, 4);
    return motion;
}

// A pan: the image's content moves kPanX pixels right and kPanY down each frame, so every pixel's vector to where it
// was in the previous picture is (-kPanX, -kPanY). Whole pixels, so each frame is the image itself, only cropped.
constexpr UINT kPanX = 4, kPanY = 2, kPanFrames = 8;

std::vector<Image> Pan(const Image& image, UINT width, UINT height)
{
    std::vector<Image> frames;
    for (UINT frame = 0; frame < kPanFrames; ++frame)
        frames.push_back(Crop(image, width, height, (kPanFrames - 1 - frame) * kPanX, (kPanFrames - 1 - frame) * kPanY));
    return frames;
}

struct Step
{
    const Image* color;
    const Image* depth; // null for none
    bool reset;
    const Image* motion = nullptr; // null for none
    float scaleX = 1, scaleY = 1;
};

// Runs the steps on a DLSS5 of its own, so no trial gets another's history, and reads back the last output, and every
// output into each if it is given.
Image Trial(const Api& api, const std::filesystem::path& runtime, const std::wstring& data, const Dlss5Look& look, const std::vector<Step>& steps,
            std::vector<Image>* each = nullptr)
{
    char error[1024] = "";
    Dlss5* dlss5 = api.create(runtime.c_str(), data.c_str(), nullptr, error, sizeof(error));
    if (!dlss5)
        throw std::runtime_error(std::string("Dlss5Create failed: ") + error);
    try
    {
        Gpu gpu(api.device(dlss5));
        std::vector<std::pair<const Image*, winrt::com_ptr<ID3D12Resource>>> textures;
        const auto texture = [&](const Image* image) -> ID3D12Resource* {
            if (!image)
                return nullptr;
            for (const auto& [uploaded, resource] : textures)
                if (uploaded == image)
                    return resource.get();
            textures.emplace_back(image, gpu.Upload(*image));
            return textures.back().second.get();
        };
        const UINT width = steps.front().color->width, height = steps.front().color->height;
        const winrt::com_ptr<ID3D12Resource> output = gpu.Texture(width, height, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for (const Step& step : steps)
        {
            if (!api.evaluate(dlss5, texture(step.color), texture(step.depth), texture(step.motion), step.scaleX, step.scaleY, output.get(), &look,
                              step.reset))
                throw std::runtime_error(std::string("Dlss5Evaluate failed: ") + api.error(dlss5));
            if (each)
                each->push_back(gpu.Read(output.get()));
        }
        Image result = gpu.Read(output.get());
        textures.clear();
        api.destroy(dlss5);
        return result;
    }
    catch (...)
    {
        api.destroy(dlss5);
        throw;
    }
}

struct Difference
{
    double mean; // over every channel but alpha, out of 255
    int largest;
};

Difference Differ(const Image& a, const Image& b)
{
    double sum = 0;
    int largest = 0;
    const size_t count = size_t(a.width) * a.height;
    for (size_t pixel = 0; pixel < count; ++pixel)
        for (int channel = 0; channel < 3; ++channel)
        {
            const int difference = std::abs(int(a.pixels[pixel * 4 + channel]) - int(b.pixels[pixel * 4 + channel]));
            sum += difference;
            largest = std::max(largest, difference);
        }
    return { sum / (count * 3), largest };
}

// The mean difference between each output and the one before it moved by the pan, over the second half of the pan, out
// of 255. Where the history holds, each output is the last one moved, as the input is.
double Restlessness(const std::vector<Image>& outputs, UINT x, UINT y)
{
    double sum = 0;
    size_t count = 0;
    for (size_t frame = outputs.size() / 2; frame < outputs.size(); ++frame)
    {
        const Image& now = outputs[frame];
        const Image& before = outputs[frame - 1];
        for (UINT row = y; row < now.height; ++row)
            for (UINT column = x; column < now.width; ++column)
                for (int channel = 0; channel < 3; ++channel)
                {
                    sum += std::abs(int(now.pixels[(size_t(row) * now.width + column) * 4 + channel]) -
                                    int(before.pixels[(size_t(row - y) * now.width + column - x) * 4 + channel]));
                    ++count;
                }
    }
    return sum / count;
}

// The mean difference between neighbouring pixels, out of 255, which falls as a picture blurs.
double Sharpness(const Image& image)
{
    double sum = 0;
    for (UINT row = 0; row + 1 < image.height; ++row)
        for (UINT column = 0; column + 1 < image.width; ++column)
            for (int channel = 0; channel < 3; ++channel)
            {
                const int here = image.pixels[(size_t(row) * image.width + column) * 4 + channel];
                sum += std::abs(here - int(image.pixels[(size_t(row) * image.width + column + 1) * 4 + channel])) +
                       std::abs(here - int(image.pixels[(size_t(row + 1) * image.width + column) * 4 + channel]));
            }
    return sum / (double(image.width - 1) * (image.height - 1) * 6);
}

// Runs the pan with each kind of vector and says how far its last output is from a still picture's result, how restless
// its outputs are and how sharp. The still result is the last picture of the pan evaluated as often with no motion,
// which is what a pan whose history follows it exactly would give.
void CheckSign(const Api& api, const std::filesystem::path& runtime, const std::wstring& data, const Dlss5Look& look, const std::vector<Image>& pan)
{
    const UINT width = pan.front().width, height = pan.front().height;
    std::printf("\nA pan of %u, %u pixels a frame over %u frames at %ux%u, against the last picture held still:\n", kPanX, kPanY, kPanFrames, width,
                height);
    std::vector<Image> stillOutputs;
    const Image still = Trial(api, runtime, data, look, std::vector<Step>(kPanFrames, Step{ &pan.back(), nullptr, false }), &stillOutputs);
    const double stillSharpness = Sharpness(still);
    std::printf("  %-24s restless %.3f, sharpness %.3f\n", "Held still", Restlessness(stillOutputs, 0, 0), stillSharpness);
    const Image truth = Motion(width, height, -float(kPanX), -float(kPanY)), negated = Motion(width, height, float(kPanX), float(kPanY));
    const std::pair<const char*, const Image*> kinds[] = { { "True vectors (-4, -2)", &truth }, { "Negated vectors (4, 2)", &negated },
                                                           { "No vectors", nullptr } };
    for (const auto& [name, motion] : kinds)
    {
        std::vector<Step> steps;
        for (const Image& frame : pan)
            steps.push_back({ &frame, nullptr, false, motion });
        std::vector<Image> outputs;
        const Difference difference = Differ(Trial(api, runtime, data, look, steps, &outputs), still);
        std::printf("  %-24s from still mean %.3f, largest %d, restless %.3f, sharpness %.3f of still's\n", name, difference.mean, difference.largest,
                    Restlessness(outputs, kPanX, kPanY), Sharpness(outputs.back()) / stillSharpness);
    }
}

// Changes one input at a time and says whether the output changed more than between two runs of the same trial.
// Returns how many inputs changed nothing.
int CheckKeys(const Api& api, const std::filesystem::path& runtime, const std::wstring& data, const Image& input, const Image& cut,
              const std::vector<Image>& pan, const Dlss5Look& look, int frames)
{
    std::printf("\nEach input, changed alone, over %d frame(s) of the image on a new feature:\n", frames);
    const std::vector<Step> still(frames, Step{ &input, nullptr, false });
    const Image baseline = Trial(api, runtime, data, look, still);
    const Difference noise = Differ(baseline, Trial(api, runtime, data, look, still));
    std::printf("  %-33s mean %.3f, largest %d\n", "The same trial again", noise.mean, noise.largest);
    int missing = 0;
    const auto report = [&](const char* key, const Image& a, const Image& b) {
        const Difference difference = Differ(a, b);
        const bool reached = difference.largest > noise.largest;
        std::printf("  %-33s mean %.3f, largest %d: %s\n", key, difference.mean, difference.largest, reached ? "changes the output" : "NO EFFECT");
        missing += !reached;
    };
    const auto changed = [&](const char* key, auto change) {
        Dlss5Look other = look;
        change(other);
        report(key, baseline, Trial(api, runtime, data, other, still));
    };
    changed("DLSSNR.Style", [](Dlss5Look& other) { other.style = other.style == 0 ? 1 : 0; });
    changed("DLSSNR.Intensity", [](Dlss5Look& other) { other.intensity = other.intensity > 0.5f ? 0.5f : 1.0f; });
    changed("DLSSNR.LocalToneStrength", [](Dlss5Look& other) { other.localTone = other.localTone > 0.5f ? 0.0f : 1.0f; });
    changed("DLSSNR.LocalStructureStrength", [](Dlss5Look& other) { other.localStructure = other.localStructure > 0.5f ? 0.0f : 1.0f; });
    changed("DLSSNR.SkinStructureStrength", [](Dlss5Look& other) { other.skinStructure = other.skinStructure > 0.5f ? 0.0f : 1.0f; });
    changed("DLSSNR.UseAutoMask", [](Dlss5Look& other) { other.autoMask = !other.autoMask; });

    std::vector<Step> cutting = still;
    cutting.push_back({ &cut, nullptr, false });
    // The last step can get other depth, as a scene whose depth changes gives it.
    const auto withDepth = [&](std::vector<Step> steps, const Image* depth, const Image* last) {
        for (Step& step : steps)
            step.depth = depth;
        steps.back().depth = last;
        return Trial(api, runtime, data, look, steps);
    };
    const Image floor = Depth(input.width, input.height, Shape::Floor), middle = Depth(input.width, input.height, Shape::Middle),
                zero = Depth(input.width, input.height, Shape::Zero);
    report("DLSSNR.Depth", withDepth(still, &floor, &floor), withDepth(still, &middle, &middle));
    report("DLSSNR.Depth, then a cut", withDepth(cutting, &floor, &floor), withDepth(cutting, &middle, &middle));
    report("DLSSNR.Depth, changing", withDepth(still, &floor, &floor), withDepth(still, &floor, &middle));
    // No depth binds a texture of 0 with DepthInverted set. Depth of 0 given binds the same values without it.
    report("DLSSNR.DepthInverted", baseline, withDepth(still, &zero, &zero));
    report("DLSSNR.DepthInverted, then a cut", withDepth(cutting, nullptr, nullptr), withDepth(cutting, &zero, &zero));

    // The pan, with its true vectors unless a row says otherwise. Its own noise is the threshold, since history that
    // moves may vary more from run to run.
    const UINT panWidth = pan.front().width, panHeight = pan.front().height;
    const Image truth = Motion(panWidth, panHeight, -float(kPanX), -float(kPanY));
    const Image halved = Motion(panWidth, panHeight, -float(kPanX) / 2, -float(kPanY) / 2);
    const auto panning = [&](const Image* motion, const Image* depth, float scaleX = 1, float scaleY = 1) {
        std::vector<Step> steps;
        for (const Image& frame : pan)
            steps.push_back({ &frame, depth, false, motion, scaleX, scaleY });
        return Trial(api, runtime, data, look, steps);
    };
    const Image panned = panning(&truth, nullptr);
    const Difference panNoise = Differ(panned, panning(&truth, nullptr));
    std::printf("  %-33s mean %.3f, largest %d\n", "The same pan again", panNoise.mean, panNoise.largest);
    const auto reportPan = [&](const char* key, const Image& a, const Image& b) {
        const Difference difference = Differ(a, b);
        const bool reached = difference.largest > panNoise.largest;
        std::printf("  %-33s mean %.3f, largest %d: %s\n", key, difference.mean, difference.largest, reached ? "changes the output" : "NO EFFECT");
        missing += !reached;
    };
    reportPan("DLSSNR.MVec", panned, panning(nullptr, nullptr));
    reportPan("DLSSNR.MVecScaleX", panned, panning(&truth, nullptr, 0, 1));
    reportPan("DLSSNR.MVecScaleY", panned, panning(&truth, nullptr, 1, 0));
    const Difference units = Differ(panned, panning(&halved, nullptr, 2, 2));
    std::printf("  %-33s mean %.3f, largest %d: %s\n", "Half the vectors at scale 2", units.mean, units.largest,
                units.largest <= panNoise.largest ? "the same, so the scale multiplies them" : "different");
    const Image panFloor = Depth(panWidth, panHeight, Shape::Floor), panMiddle = Depth(panWidth, panHeight, Shape::Middle),
                panZero = Depth(panWidth, panHeight, Shape::Zero);
    reportPan("DLSSNR.Depth, in the pan", panning(&truth, &panFloor), panning(&truth, &panMiddle));
    reportPan("DLSSNR.DepthInverted, in the pan", panned, panning(&truth, &panZero));

    const Image kept = Trial(api, runtime, data, look, cutting);
    cutting.back().reset = true;
    const Image reset = Trial(api, runtime, data, look, cutting);
    report("DLSSNR.Reset", kept, reset);
    const Difference fresh = Differ(reset, Trial(api, runtime, data, look, { { &cut, nullptr, false } }));
    std::printf("  %-33s mean %.3f, largest %d: %s\n", "Reset against a new feature", fresh.mean, fresh.largest,
                fresh.largest <= noise.largest ? "the same, so it drops the history" : "different, so some history is kept");
    std::printf("  Not checked: Color, Output, their subrects, Width, Height, ScalingRatio and Enabled, since every output above\n"
                "  needs them.\n");
    return missing;
}

void AddonLog(const UnishadeHost*, UnishadeLogLevel, const char* text)
{
    std::printf("[DLSS5] %s\n", text);
    std::fflush(stdout);
}

struct Shared
{
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::handle handle;
};

Shared CreateShared(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    // As Unishade makes the images add-ons get.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    Shared shared;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, shared.texture.put()));
    winrt::check_hresult(shared.texture.as<IDXGIResource1>()->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                                                 nullptr, shared.handle.put()));
    return shared;
}

// Runs the add-on in the module as Unishade does, with depth, on a picture, the same picture again, a cut and a small
// change, and says whether it kept its output for the repeat and dropped the history at the cut only. Returns how
// many of those it got wrong.
int CheckAddon(HMODULE library, const Api& api, const std::filesystem::path& runtime, const std::wstring& data, const Image& first,
               const Image& cut, const Image& nudged, const Image& depthImage, const Dlss5Look& look)
{
    std::printf("\nThe add-on, as Unishade runs it:\n");
    const UINT width = first.width, height = first.height;
    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.put()) == S_OK; ++index, adapter = nullptr)
        if (SUCCEEDED(adapter->GetDesc1(&description)) && description.VendorId == 0x10DE && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            break;
    if (!adapter)
        throw std::runtime_error("There is no NVIDIA graphics card.");
    winrt::com_ptr<ID3D11Device> created;
    winrt::com_ptr<ID3D11DeviceContext> createdContext;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, created.put(), nullptr,
                                           createdContext.put()));
    const auto device = created.as<ID3D11Device5>();
    const auto context = createdContext.as<ID3D11DeviceContext4>();
    winrt::com_ptr<ID3D11Fence> fence;
    winrt::check_hresult(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.put())));
    winrt::handle fenceHandle;
    winrt::check_hresult(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put()));
    winrt::handle reached{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    const Shared color = CreateShared(device.get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    const Shared output = CreateShared(device.get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    const Shared depth = CreateShared(device.get(), width, height, DXGI_FORMAT_R32_FLOAT);
    context->UpdateSubresource(depth.texture.get(), 0, nullptr, depthImage.pixels.data(), width * 4, 0);
    D3D11_TEXTURE2D_DESC staging{};
    output.texture->GetDesc(&staging);
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> readable;
    winrt::check_hresult(device->CreateTexture2D(&staging, nullptr, readable.put()));

    const auto getAddon = reinterpret_cast<UnishadeGetAddonFunction>(GetProcAddress(library, "UnishadeGetAddon"));
    if (!getAddon)
        throw std::runtime_error("The module is not an add-on.");
    const UnishadeAddon* addon = getAddon();
    if (!(addon->flags & UNISHADE_READS_DEPTH))
        throw std::runtime_error("The add-on does not ask for depth.");
    // The add-on's folder holds nvngx_dlssnr.dll, and its name ends in a separator, as Unishade gives it.
    const std::string folder = runtime.parent_path().string() + "\\";
    const std::string dataFolder = std::filesystem::path(data).string();
    UnishadeHost host{};
    host.size = sizeof(host);
    host.version = UNISHADE_API_VERSION;
    host.unishade = "check";
    host.folder = folder.c_str();
    host.dataFolder = dataFolder.c_str();
    std::memcpy(host.adapter, &description.AdapterLuid, sizeof(LUID));
    host.log = AddonLog;
    char error[512] = "";
    void* instance = addon->create(&host, error, sizeof(error));
    if (!instance)
        throw std::runtime_error(std::string("The add-on did not start: ") + error);
    // Its settings are in the order of Dlss5Look.
    std::vector<std::array<float, 4>> settings(addon->settingCount);
    const float values[] = { float(look.style), look.intensity, look.localTone, look.localStructure, look.skinStructure, float(look.autoMask),
                             float(look.passCount) };
    for (uint32_t i = 0; i < addon->settingCount; ++i)
        settings[i] = { values[i] };

    uint64_t value = 0;
    const auto run = [&](const Image& picture, uint64_t number) {
        std::vector<uint8_t> pixels = picture.pixels;
        for (size_t pixel = 0; pixel < pixels.size(); pixel += 4)
            std::swap(pixels[pixel], pixels[pixel + 2]);
        context->UpdateSubresource(color.texture.get(), 0, nullptr, pixels.data(), width * 4, 0);
        winrt::check_hresult(context->Signal(fence.get(), ++value));
        context->Flush();
        UnishadeFrame frame{};
        frame.size = sizeof(frame);
        frame.stage = UNISHADE_STAGE_BEFORE_EFFECTS;
        frame.width = width;
        frame.height = height;
        frame.generation = 1;
        frame.picture = number;
        frame.color = { color.handle.get(), UNISHADE_FORMAT_BGRA8 };
        frame.output = { output.handle.get(), UNISHADE_FORMAT_BGRA8 };
        frame.depth = { depth.handle.get(), UNISHADE_FORMAT_R32_FLOAT };
        frame.fence = fenceHandle.get();
        frame.wait = value;
        frame.signal = ++value;
        frame.settings = reinterpret_cast<const float (*)[4]>(settings.data());
        frame.sdrWhite = 1;
        const auto start = std::chrono::steady_clock::now();
        if (!addon->frame(instance, &frame, error, sizeof(error)))
            throw std::runtime_error(std::string("The add-on failed on a frame: ") + error);
        winrt::check_hresult(fence->SetEventOnCompletion(value, reached.get()));
        if (WaitForSingleObject(reached.get(), 5000) != WAIT_OBJECT_0)
            throw std::runtime_error("The add-on did not signal the fence.");
        std::printf("  Frame %llu took the add-on %.1f ms\n", static_cast<unsigned long long>(number),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        context->CopyResource(readable.get(), output.texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(readable.get(), 0, D3D11_MAP_READ, 0, &mapped));
        Image result{ width, height, std::vector<uint8_t>(size_t(width) * height * 4) };
        for (UINT row = 0; row < height; ++row)
            std::memcpy(result.pixels.data() + size_t(row) * width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch,
                        size_t(width) * 4);
        context->Unmap(readable.get(), 0);
        for (size_t pixel = 0; pixel < result.pixels.size(); pixel += 4)
            std::swap(result.pixels[pixel], result.pixels[pixel + 2]);
        return result;
    };
    const Image once = run(first, 1), again = run(first, 2), afterCut = run(cut, 3), afterNudge = run(nudged, 4);
    settings[1][0] = look.intensity > 0.5f ? 0.5f : 1.0f;
    const Image otherLook = run(nudged, 5);
    addon->destroy(instance);

    // The module on its own, for what the add-on should match.
    const Image twice = Trial(api, runtime, data, look, { { &first, &depthImage, false }, { &first, &depthImage, false } });
    const Image freshCut = Trial(api, runtime, data, look, { { &cut, &depthImage, false } });
    const Image freshNudge = Trial(api, runtime, data, look, { { &nudged, &depthImage, false } });
    int wrong = 0;
    const auto report = [&](const char* what, const Image& a, const Image& b, bool same, const char* meaning) {
        const Difference difference = Differ(a, b);
        const bool right = same == (difference.largest == 0);
        std::printf("  %-44s mean %.3f, largest %d: %s\n", what, difference.mean, difference.largest, right ? meaning : "WRONG");
        wrong += !right;
    };
    report("Evaluating the same picture twice, against once", twice, once, false, "it changes, so a repeat would flicker");
    report("The same picture under a new number", again, once, true, "the output is kept");
    report("A cut, against a new feature", afterCut, freshCut, true, "the history is dropped");
    report("The cut picture moved 4 pixels, against new", afterNudge, freshNudge, false, "not a cut, so the history is kept");
    report("That picture again with another intensity", otherLook, afterNudge, false, "evaluated again");
    return wrong;
}

int Main(int argc, wchar_t** argv)
{
    if (argc < 4)
    {
        std::printf("dlss5_check <nvngx_dlssnr.dll> <image> <output folder> [--module <dll>] [--frames N] [--passes N] [--style N]\n"
                    "    [--intensity X] [--local-tone X] [--local-structure X] [--skin-structure X] [--no-auto-mask] [--keys <image>]\n");
        return 2;
    }
    // nvngx_dlssnr.dll does not log through the callback NGX is given, only to a file in the data folder and to the
    // console, and only when this is set.
    if (!GetEnvironmentVariableW(L"__NGX_LOG_LEVEL", nullptr, 0))
        SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", L"1");
    const std::filesystem::path runtime = std::filesystem::absolute(argv[1]), folder = argv[3];
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path module = std::filesystem::path(exe).replace_filename(L"Unishade_nvngx.dll");
    Dlss5Look look{ 0, 1.0f, 1.0f, 1.0f, 1.0f, true, 1 };
    int frames = 1;
    std::filesystem::path keys;
    for (int index = 4; index < argc; ++index)
    {
        const std::wstring option = argv[index];
        if (option == L"--no-auto-mask")
        {
            look.autoMask = false;
            continue;
        }
        if (index + 1 == argc)
            throw std::runtime_error("An option is missing its value.");
        const wchar_t* value = argv[++index];
        if (option == L"--module")
            module = std::filesystem::absolute(value);
        else if (option == L"--frames")
            frames = std::max(1, _wtoi(value));
        else if (option == L"--passes")
            look.passCount = _wtoi(value);
        else if (option == L"--style")
            look.style = _wtoi(value);
        else if (option == L"--intensity")
            look.intensity = static_cast<float>(_wtof(value));
        else if (option == L"--local-tone")
            look.localTone = static_cast<float>(_wtof(value));
        else if (option == L"--local-structure")
            look.localStructure = static_cast<float>(_wtof(value));
        else if (option == L"--skin-structure")
            look.skinStructure = static_cast<float>(_wtof(value));
        else if (option == L"--keys")
            keys = value;
        else
            throw std::runtime_error("Unknown option.");
    }
    std::printf("Look: style %d, intensity %.2f, local tone %.2f, local structure %.2f, skin structure %.2f, auto mask %d, passes %d\n",
                look.style, look.intensity, look.localTone, look.localStructure, look.skinStructure, look.autoMask, look.passCount);

    const Image input = Load(argv[2]);
    std::filesystem::create_directories(folder / "ngx");
    Save(input, folder / "input.png");

    HMODULE library = LoadLibraryW(module.c_str());
    if (!library)
        throw std::runtime_error("Could not load " + module.string() + ": error " + std::to_string(GetLastError()));
    std::printf("Loaded %s\n", module.string().c_str());
    Api api;
    Resolve(library, "Dlss5Create", api.create);
    Resolve(library, "Dlss5Device", api.device);
    Resolve(library, "Dlss5Evaluate", api.evaluate);
    Resolve(library, "Dlss5Error", api.error);
    Resolve(library, "Dlss5Destroy", api.destroy);

    char error[1024] = "";
    const std::wstring data = std::filesystem::absolute(folder / "ngx").wstring();
    Dlss5* dlss5 = api.create(runtime.c_str(), data.c_str(), Print, error, sizeof(error));
    if (!dlss5)
        throw std::runtime_error(std::string("Dlss5Create failed: ") + error);
    {
        Gpu gpu(api.device(dlss5));
        Save(Run(api, dlss5, gpu, input, look, frames), folder / "output.png");
        const Image quarter = Crop(input, input.width / 2, input.height / 2);
        Save(quarter, folder / "quarter-input.png");
        Save(Run(api, dlss5, gpu, quarter, look, frames), folder / "quarter-output.png");
    }
    api.destroy(dlss5);
    int missing = 0;
    if (!keys.empty())
    {
        const Image cut = Load(keys);
        const UINT width = std::min(input.width, cut.width), height = std::min(input.height, cut.height);
        // Each picture of the pan is a part of the image, smaller by how far the pan goes.
        const std::vector<Image> pan = Pan(input, width - kPanX * (kPanFrames - 1), height - kPanY * (kPanFrames - 1));
        CheckSign(api, runtime, data, look, pan);
        missing = CheckKeys(api, runtime, data, Crop(input, width, height), Crop(cut, width, height), pan, look, frames);
        // A part of each, so the cut picture can also move a little.
        const UINT part = width - 4;
        missing += CheckAddon(library, api, runtime, data, Crop(input, part, height), Crop(cut, part, height), Crop(cut, part, height, 4),
                              Depth(part, height, Shape::Floor), look);
    }
    FreeLibrary(library);
    return missing ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        return Main(argc, argv);
    }
    catch (const std::exception& exception)
    {
        std::printf("%s\n", exception.what());
        return 1;
    }
}
