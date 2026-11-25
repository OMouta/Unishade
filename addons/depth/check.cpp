// Runs a picture through the Depth add-on as Unishade does, without a window, and says how steady its depth is.
// depth_check <Depth.dll> <add-on folder> <image> <output folder> [--size WxH] [--frames N] [--hold N] [--scroll X]
//     [--fps N] [--detail N]
// The picture is a part of the image, the size given, that moves across it by scroll pixels a frame for frames frames
// and then stays for hold frames. The add-on folder holds the model and ONNX Runtime. The depth of the last moving
// frame and of the last frame are written as PNG, with the normals effects would make of them.

#include <unishade_addon.h>

#include <d3d11_4.h>
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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

// As depth_shared.h maps the model's output to DEPTH.
constexpr float kFarPlane = 1000.0f;
constexpr float kNearest = 0.01f;

void Log(const UnishadeHost*, UnishadeLogLevel, const char* text)
{
    std::printf("[Depth] %s\n", text);
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

// How close each pixel is, as the model's output over the nearest thing's: 1 for the nearest, kNearest at the far
// plane. Depth is steady when this is.
std::vector<float> Closeness(const std::vector<float>& depth)
{
    std::vector<float> closeness(depth.size());
    for (size_t i = 0; i < depth.size(); ++i)
    {
        const float distance = depth[i] / (kFarPlane - depth[i] * (kFarPlane - 1));
        closeness[i] = kNearest / std::max(distance, kNearest * kNearest);
    }
    return closeness;
}

// The mean difference between a and b moved shift pixels to the left, over the pixels both have, as a share of b's
// mean there, so it says the same whatever the closeness is measured against.
double Difference(const std::vector<float>& a, const std::vector<float>& b, int width, int height, int shift)
{
    double sum = 0, mean = 0;
    for (int y = 0; y < height; ++y)
        for (int x = std::max(0, -shift); x < std::min(width, width - shift); ++x)
        {
            sum += std::abs(a[size_t(y) * width + x] - b[size_t(y) * width + x + shift]);
            mean += b[size_t(y) * width + x + shift];
        }
    return mean > 0 ? sum / mean : 0;
}

// How much closer everything in a is than in b moved shift pixels to the left: the middle one of the ratios, over a
// part of the pixels both have. 1 when depth as a whole is the same in both.
double Scale(const std::vector<float>& a, const std::vector<float>& b, int width, int height, int shift)
{
    std::vector<float> ratios;
    for (int y = 0; y < height; y += 4)
        for (int x = std::max(0, -shift); x < std::min(width, width - shift); x += 4)
            ratios.push_back(a[size_t(y) * width + x] / b[size_t(y) * width + x + shift]);
    if (ratios.empty())
        return 1;
    std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
    return ratios[ratios.size() / 2];
}

void SaveDepth(const std::vector<float>& depth, int width, int height, const std::filesystem::path& folder, const std::string& name)
{
    const std::vector<float> closeness = Closeness(depth);
    std::vector<uint8_t> gray(closeness.size());
    for (size_t i = 0; i < gray.size(); ++i)
        gray[i] = static_cast<uint8_t>(std::lround(255 * std::sqrt(std::clamp(closeness[i], 0.0f, 1.0f))));
    std::filesystem::path path = folder / (name + ".png");
    if (!stbi_write_png(path.string().c_str(), width, height, 1, gray.data(), width))
        throw std::runtime_error("Could not write " + path.string());

    // Normals as effects find them: from the positions of a pixel and its neighbors to the right and below, with the
    // linear depth as distance. Stairs in a flat surface and wide silhouettes show here.
    std::vector<uint8_t> normals(size_t(width) * height * 3);
    const auto position = [&](int x, int y) {
        x = std::min(x, width - 1);
        y = std::min(y, height - 1);
        const float z = depth[size_t(y) * width + x];
        const float distance = z / (kFarPlane - z * (kFarPlane - 1)) * kFarPlane;
        struct { float x, y, z; } point{ (float(x) / width * 2 - 1) * distance, (float(y) / height * 2 - 1) * distance * height / width, distance };
        return point;
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const auto center = position(x, y), right = position(x + 1, y), below = position(x, y + 1);
            const float ax = right.x - center.x, ay = right.y - center.y, az = right.z - center.z;
            const float bx = below.x - center.x, by = below.y - center.y, bz = below.z - center.z;
            float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            const float length = std::max(std::sqrt(nx * nx + ny * ny + nz * nz), 1e-12f);
            uint8_t* pixel = &normals[(size_t(y) * width + x) * 3];
            pixel[0] = static_cast<uint8_t>(std::lround(255 * (0.5f + 0.5f * nx / length)));
            pixel[1] = static_cast<uint8_t>(std::lround(255 * (0.5f + 0.5f * ny / length)));
            pixel[2] = static_cast<uint8_t>(std::lround(255 * (0.5f + 0.5f * nz / length)));
        }
    path = folder / (name + "-normals.png");
    if (!stbi_write_png(path.string().c_str(), width, height, 3, normals.data(), width * 3))
        throw std::runtime_error("Could not write " + path.string());
    std::printf("Wrote %s.png and %s-normals.png\n", name.c_str(), name.c_str());
}

int Main(int argc, wchar_t** argv)
{
    if (argc < 5)
    {
        std::printf("depth_check <Depth.dll> <add-on folder> <image> <output folder> [--size WxH] [--frames N] [--hold N] [--scroll X]\n"
                    "    [--fps N] [--detail N]\n");
        return 2;
    }
    const std::filesystem::path module = std::filesystem::absolute(argv[1]), output = argv[4];
    std::string folder = std::filesystem::absolute(argv[2]).string();
    if (folder.back() != '\\' && folder.back() != '/')
        folder += '\\';
    int width = 1280, height = 720, frames = 120, hold = 60, scroll = 4, fps = 60;
    float detail = 1;
    for (int index = 5; index + 1 < argc; index += 2)
    {
        const std::wstring option = argv[index];
        const wchar_t* value = argv[index + 1];
        if (option == L"--size")
        {
            if (swscanf_s(value, L"%dx%d", &width, &height) != 2)
                throw std::runtime_error("The size is not WxH.");
        }
        else if (option == L"--frames")
            frames = std::max(1, _wtoi(value));
        else if (option == L"--hold")
            hold = std::max(1, _wtoi(value));
        else if (option == L"--scroll")
            scroll = _wtoi(value);
        else if (option == L"--fps")
            fps = std::max(1, _wtoi(value));
        else if (option == L"--detail")
            detail = static_cast<float>(_wtoi(value));
        else
            throw std::runtime_error("Unknown option.");
    }

    int imageWidth = 0, imageHeight = 0, channels = 0;
    stbi_uc* loaded = stbi_load(std::filesystem::path(argv[3]).string().c_str(), &imageWidth, &imageHeight, &channels, 4);
    if (!loaded)
        throw std::runtime_error(std::string("Could not read the image: ") + stbi_failure_reason());
    std::vector<uint8_t> image(loaded, loaded + size_t(imageWidth) * imageHeight * 4);
    stbi_image_free(loaded);
    if (width > imageWidth || height > imageHeight)
        throw std::runtime_error("The image is smaller than the picture.");
    // The picture never leaves the image, so the scroll stops short where it would.
    frames = scroll ? std::min(frames, (imageWidth - width) / std::abs(scroll) + 1) : frames;
    std::printf("Picture %dx%d of %ls (%dx%d), %d frames moving %d pixels each, then %d still, at %d a second, detail %d\n", width, height, argv[3],
                imageWidth, imageHeight, frames, scroll, hold, fps, static_cast<int>(detail));
    std::filesystem::create_directories(output);

    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description{};
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.put()) == S_OK; ++index, adapter = nullptr)
        if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            break;
    if (!adapter)
        throw std::runtime_error("There is no graphics card.");
    std::printf("On %ls\n", description.Description);
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
    const Shared depth = CreateShared(device.get(), width, height, DXGI_FORMAT_R32_FLOAT);
    D3D11_TEXTURE2D_DESC staging{};
    depth.texture->GetDesc(&staging);
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.MiscFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> readable;
    winrt::check_hresult(device->CreateTexture2D(&staging, nullptr, readable.put()));

    const HMODULE library = LoadLibraryW(module.c_str());
    if (!library)
        throw std::runtime_error("Could not load " + module.string() + ": error " + std::to_string(GetLastError()));
    const auto getAddon = reinterpret_cast<UnishadeGetAddonFunction>(GetProcAddress(library, "UnishadeGetAddon"));
    if (!getAddon)
        throw std::runtime_error("The module is not an add-on.");
    const UnishadeAddon* addon = getAddon();
    UnishadeHost host{};
    host.size = sizeof(host);
    host.version = UNISHADE_API_VERSION;
    host.unishade = "check";
    host.folder = folder.c_str();
    host.dataFolder = folder.c_str();
    std::memcpy(host.adapter, &description.AdapterLuid, sizeof(LUID));
    host.log = Log;
    char error[512] = "";
    void* instance = addon->create(&host, error, sizeof(error));
    if (!instance)
        throw std::runtime_error(std::string("The add-on did not start: ") + error);

    std::vector<std::array<float, 4>> settings(addon->settingCount);
    for (uint32_t i = 0; i < addon->settingCount; ++i)
        std::copy_n(addon->settings[i].defaultValue, 4, settings[i].data());
    settings[0][0] = detail;

    const int total = frames + hold;
    std::vector<uint8_t> picture(size_t(width) * height * 4);
    std::vector<std::vector<float>> closeness(total);
    std::vector<float> lastMoving, last;
    std::vector<int> offsets(total);
    uint64_t value = 0;
    double callMs = 0, frameMs = 0, worstMs = 0;
    const auto start = Clock::now();
    // The model takes seconds to load, so the picture stays where it starts until depth arrives and settles.
    constexpr int kWarm = 30;
    for (int index = 0, tick = 0, warm = 0; index < total; ++tick)
    {
        std::this_thread::sleep_until(start + tick * std::chrono::microseconds(1000000 / fps));
        if (tick > 60 * fps && warm < kWarm)
            throw std::runtime_error("The add-on wrote no depth in a minute.");
        const int offset = offsets[index] = std::min(index, frames - 1) * scroll + (scroll < 0 ? imageWidth - width : 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const uint8_t* from = &image[(size_t(y) * imageWidth + x + offset) * 4];
                uint8_t* to = &picture[(size_t(y) * width + x) * 4];
                to[0] = from[2];
                to[1] = from[1];
                to[2] = from[0];
                to[3] = 255;
            }
        context->UpdateSubresource(color.texture.get(), 0, nullptr, picture.data(), width * 4, 0);
        winrt::check_hresult(context->Signal(fence.get(), ++value));
        context->Flush();

        UnishadeFrame frame{};
        frame.size = sizeof(frame);
        frame.stage = UNISHADE_STAGE_BEFORE_EFFECTS;
        frame.width = width;
        frame.height = height;
        frame.generation = 1;
        // The picture only stops changing once it stops moving.
        frame.picture = static_cast<uint64_t>(std::min(index, frames - 1)) + 1;
        frame.color = { color.handle.get(), UNISHADE_FORMAT_BGRA8 };
        frame.depth = { depth.handle.get(), UNISHADE_FORMAT_R32_FLOAT };
        frame.fence = fenceHandle.get();
        frame.wait = value;
        frame.signal = ++value;
        frame.settings = reinterpret_cast<const float (*)[4]>(settings.data());
        frame.sdrWhite = 1;
        frame.time = std::chrono::duration<double>(Clock::now() - start).count();
        const auto called = Clock::now();
        if (!addon->frame(instance, &frame, error, sizeof(error)))
            throw std::runtime_error(std::string("The add-on failed on a frame: ") + error);
        const auto returned = Clock::now();
        winrt::check_hresult(fence->SetEventOnCompletion(value, reached.get()));
        if (WaitForSingleObject(reached.get(), 5000) != WAIT_OBJECT_0)
            throw std::runtime_error("The add-on did not signal the fence.");
        const double took = std::chrono::duration<double, std::milli>(Clock::now() - called).count();
        if (warm == kWarm)
        {
            callMs += std::chrono::duration<double, std::milli>(returned - called).count();
            frameMs += took;
            worstMs = std::max(worstMs, took);
        }

        context->CopyResource(readable.get(), depth.texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(readable.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<float> values(size_t(width) * height);
        for (int y = 0; y < height; ++y)
            std::memcpy(&values[size_t(y) * width], static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(width) * 4);
        context->Unmap(readable.get(), 0);
        if (warm < kWarm)
        {
            if (warm || *std::min_element(values.begin(), values.end()) < 1)
                ++warm;
            continue;
        }
        closeness[index] = Closeness(values);
        if (index == frames - 1)
            lastMoving = values;
        if (index == total - 1)
            last = values;
        ++index;
    }
    addon->destroy(instance);
    FreeLibrary(library);

    // Depth takes a few estimates to follow the picture once it starts to move, so the first half of the moving frames
    // does not count.
    const int from = frames / 2;
    double flicker = 0, late = 0, pulse = 0, off = 0;
    for (int index = from; index < frames; ++index)
    {
        // What is at x in this frame was scroll pixels to the right in the one before, and is where the picture came
        // to rest in the last frame.
        flicker += Difference(closeness[index], closeness[index - 1], width, height, scroll);
        late += Difference(closeness[index], closeness[total - 1], width, height, offsets[index] - offsets[total - 1]);
        pulse += std::abs(Scale(closeness[index], closeness[index - 1], width, height, scroll) - 1);
        off += std::abs(Scale(closeness[index], closeness[total - 1], width, height, offsets[index] - offsets[total - 1]) - 1);
    }
    const int counted = std::max(1, frames - from);
    std::printf("Frame to frame, depth changed by %.3f%% on average while the picture moved, and as a whole by %.3f%%.\n", 100 * flicker / counted,
                100 * pulse / counted);
    std::printf("It was %.3f%% from the depth the picture came to rest at, and as a whole %.3f%%.\n", 100 * late / counted, 100 * off / counted);
    std::printf("A frame took the add-on %.2f ms on the processor and %.2f ms until the graphics card was done, %.2f ms at worst.\n",
                callMs / total, frameMs / total, worstMs);
    SaveDepth(lastMoving, width, height, output, "moving");
    SaveDepth(last, width, height, output, "still");
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
    }
    catch (const winrt::hresult_error& exception)
    {
        std::printf("Direct3D failed: %s\n", winrt::to_string(exception.message()).c_str());
    }
    return 1;
}
