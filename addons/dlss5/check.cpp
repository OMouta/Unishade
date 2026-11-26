// Runs an image through Unishade_nvngx.dll, writes the result as PNG and says how far it is from the input.
// dlss5_check <nvngx_dlssnr.dll> <image> <output folder> [--module <dll>] [--frames N] [--passes N] [--style N]
//     [--intensity X] [--local-tone X] [--local-structure X] [--skin-structure X] [--no-auto-mask]
// It also runs the top-left quarter of the image, so the feature is made again at a new size.

#include "dlss5.h"

#include <d3d12.h>
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    std::vector<uint8_t> pixels; // RGBA, rows packed
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

    winrt::com_ptr<ID3D12Resource> Texture(UINT width, UINT height, D3D12_RESOURCE_FLAGS flags)
    {
        const D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC description{ D3D12_RESOURCE_DIMENSION_TEXTURE2D };
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        winrt::com_ptr<ID3D12Resource> texture = Texture(image.width, image.height, D3D12_RESOURCE_FLAG_NONE);
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

Image Crop(const Image& image, UINT width, UINT height)
{
    Image crop{ width, height };
    crop.pixels.resize(size_t(width) * height * 4);
    for (UINT row = 0; row < height; ++row)
        std::copy_n(image.pixels.data() + size_t(row) * image.width * 4, size_t(width) * 4, crop.pixels.data() + size_t(row) * width * 4);
    return crop;
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
        if (!api.evaluate(dlss5, color.get(), nullptr, output.get(), &look))
            throw std::runtime_error(std::string("Dlss5Evaluate failed: ") + api.error(dlss5));
        const auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("Evaluated frame %d at %ux%u with %d pass(es) in %.1f ms\n", frame, input.width, input.height, look.passCount, took);
    }
    Image result = gpu.Read(output.get());
    Compare(input, result);
    return result;
}

int Main(int argc, wchar_t** argv)
{
    if (argc < 4)
    {
        std::printf("dlss5_check <nvngx_dlssnr.dll> <image> <output folder> [--module <dll>] [--frames N] [--passes N] [--style N]\n"
                    "    [--intensity X] [--local-tone X] [--local-structure X] [--skin-structure X] [--no-auto-mask]\n");
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
        else
            throw std::runtime_error("Unknown option.");
    }
    std::printf("Look: style %d, intensity %.2f, local tone %.2f, local structure %.2f, skin structure %.2f, auto mask %d, passes %d\n",
                look.style, look.intensity, look.localTone, look.localStructure, look.skinStructure, look.autoMask, look.passCount);

    Image input;
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(std::filesystem::path(argv[2]).string().c_str(), &width, &height, &channels, 4);
    if (!pixels)
        throw std::runtime_error(std::string("Could not read the image: ") + stbi_failure_reason());
    input.width = static_cast<UINT>(width);
    input.height = static_cast<UINT>(height);
    input.pixels.assign(pixels, pixels + size_t(width) * height * 4);
    stbi_image_free(pixels);
    std::printf("Read %ls: %dx%d\n", argv[2], width, height);
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
    FreeLibrary(library);
    return 0;
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
