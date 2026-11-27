// Runs a moving picture through the Depth add-on as Unishade does, without a window, and says how steady its depth is
// and how sharp its edges are.
// depth_check <Depth.dll> <add-on folder> <image> <output folder> [--size WxH] [--frames N] [--hold N] [--scroll X]
//     [--fps N] [--detail N] [--part <image>] [--cut <image>] [--cut-at N] [--hud 1] [--dark F] [--grain N]
// The picture is a part of the image, the size given, that moves across it by scroll pixels a frame for frames frames
// and then stays for hold frames. With --part, the picture stays still and a rectangle from the other image moves
// across it instead. With --cut, the picture comes from the other image from moving frame cut-at on. With --hud, still
// bars and stripes like a game's HUD are drawn over every frame, and left out of the figures. With --dark, the images
// are made as dark and flat as a night scene: each value, from 0 to 1, is squared and multiplied by F. With --grain,
// every frame gets noise of its own, with a deviation of N levels of 255, and the add-on is told the picture changed,
// as a game's picture does even where nothing in it moves. The add-on folder holds the model and ONNX Runtime. The
// depth of the last moving frame and of the last frame are written as PNG, with the normals effects would make of them,
// and the figures of every frame to frames.csv.

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
#include <functional>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

// As depth_shared.h maps the model's output to DEPTH.
constexpr float kFarPlane = 1000.0f;
constexpr float kNearest = 0.01f;
// A background pixel the moving part covered in this many frames before is counted as just uncovered.
constexpr int kWake = 8;

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

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

Image Load(const wchar_t* file)
{
    Image image;
    int channels = 0;
    stbi_uc* loaded = stbi_load(std::filesystem::path(file).string().c_str(), &image.width, &image.height, &channels, 4);
    if (!loaded)
        throw std::runtime_error(std::string("Could not read the image: ") + stbi_failure_reason());
    image.rgba.assign(loaded, loaded + size_t(image.width) * image.height * 4);
    stbi_image_free(loaded);
    return image;
}

// What a frame shows: which image the picture comes from and where, and where the moving part is, if there is one.
struct Layout
{
    int offset = 0; // of the picture in its image
    bool second = false;
    int part = -1;  // the moving part's left edge in the picture
};

struct Rect
{
    int x, y, width, height;
    bool Contains(int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
};

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

// How depth looks to effects that differentiate it, which no real depth is at hand to compare with. A pixel is on an
// edge where the closeness within kEdgeReach pixels across or down differs by more than kEdgeStep of the highest, and
// floats when it lies more than kFloating of that difference from both sides, between two surfaces. An edge pixel is a
// speck when it is an edge's step nearer or farther than at least three of the four next to it, a pixel of one surface
// left in another. Floating counts soft edges and specks count edges that snap to the wrong side, and an upsample can
// lower either by trading it for the other, so the two are read together, and with the normals. Elsewhere a
// surface is smooth, and the share of neighbors with exactly the same depth says how much of it is in steps, and the
// second differences say how much it ripples, which normals turn into embossing. Only sloped surfaces count, whose
// depth changes somewhere within reach, since the model puts all of the sky at one value.
constexpr int kEdgeReach = 3;
constexpr float kEdgeStep = 0.25f;
constexpr float kFloating = 0.2f;

void Shape(const std::vector<float>& depth, int width, int height, const char* name)
{
    const std::vector<float> closeness = Closeness(depth);
    const auto at = [&](int x, int y) { return closeness[size_t(y) * width + x]; };
    size_t edges = 0, floating = 0, specks = 0, slopes = 0, equal = 0;
    double ripple = 0;
    for (int y = kEdgeReach; y < height - kEdgeReach; ++y)
        for (int x = kEdgeReach; x < width - kEdgeReach; ++x)
        {
            const float value = at(x, y);
            bool edge = false, between = false, sloped = false;
            for (const auto& [dx, dy] : { std::pair{ 1, 0 }, std::pair{ 0, 1 } })
            {
                float low = value, high = value;
                for (int k = -kEdgeReach; k <= kEdgeReach; ++k)
                {
                    low = std::min(low, at(x + k * dx, y + k * dy));
                    high = std::max(high, at(x + k * dx, y + k * dy));
                }
                const float spread = high - low;
                sloped |= spread > 0;
                if (spread > kEdgeStep * high)
                {
                    edge = true;
                    between |= value > low + kFloating * spread && value < high - kFloating * spread;
                }
            }
            if (edge)
            {
                ++edges;
                floating += between;
                int nearer = 0, farther = 0;
                for (const float next : { at(x - 1, y), at(x + 1, y), at(x, y - 1), at(x, y + 1) })
                    if (std::abs(value - next) > kEdgeStep * std::max(value, next))
                        ++(value > next ? nearer : farther);
                specks += nearer >= 3 || farther >= 3;
                continue;
            }
            if (!sloped)
                continue;
            ++slopes;
            const size_t index = size_t(y) * width + x;
            equal += depth[index] == depth[index + 1];
            ripple += (std::abs(at(x - 1, y) - 2 * value + at(x + 1, y)) + std::abs(at(x, y - 1) - 2 * value + at(x, y + 1))) / value;
        }
    std::printf("In the %s depth, %.2f%% of pixels are on an edge, %.2f%% of those float between its sides and %.2f%% are specks. Of the sloped "
                "ones, %.3f%% have the same depth as the next, and they ripple by %.4f%%.\n",
                name, 100.0 * edges / std::max<size_t>(1, (size_t(width) - 2 * kEdgeReach) * (height - 2 * kEdgeReach)),
                100.0 * floating / std::max<size_t>(1, edges), 100.0 * specks / std::max<size_t>(1, edges), 100.0 * equal / std::max<size_t>(1, slopes),
                100 * ripple / std::max<size_t>(1, slopes));
}

// The mean of the worst tenth of values, which is where the frames an estimate landed in show.
double Worst(std::vector<double> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end(), std::greater<>());
    const size_t count = std::max<size_t>(1, values.size() / 10);
    double sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += values[i];
    return sum / count;
}

double Mean(const std::vector<double>& values)
{
    double sum = 0;
    for (double value : values)
        sum += value;
    return values.empty() ? 0 : sum / values.size();
}

int Main(int argc, wchar_t** argv)
{
    if (argc < 5)
    {
        std::printf("depth_check <Depth.dll> <add-on folder> <image> <output folder> [--size WxH] [--frames N] [--hold N] [--scroll X]\n"
                    "    [--fps N] [--detail N] [--part <image>] [--cut <image>] [--cut-at N] [--hud 1] [--dark F] [--grain N]\n");
        return 2;
    }
    const std::filesystem::path module = std::filesystem::absolute(argv[1]), output = argv[4];
    std::string folder = std::filesystem::absolute(argv[2]).string();
    if (folder.back() != '\\' && folder.back() != '/')
        folder += '\\';
    int width = 1280, height = 720, frames = 120, hold = 60, scroll = 4, fps = 60, cutAt = -1;
    float detail = 1, dark = 0, grain = 0;
    bool hud = false;
    const wchar_t* partFile = nullptr;
    const wchar_t* cutFile = nullptr;
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
        else if (option == L"--part")
            partFile = value;
        else if (option == L"--cut")
            cutFile = value;
        else if (option == L"--cut-at")
            cutAt = _wtoi(value);
        else if (option == L"--hud")
            hud = _wtoi(value) != 0;
        else if (option == L"--dark")
            dark = static_cast<float>(_wtof(value));
        else if (option == L"--grain")
            grain = static_cast<float>(_wtof(value));
        else
            throw std::runtime_error("Unknown option.");
    }

    const auto darken = [&](Image loaded) {
        if (dark > 0)
            for (size_t i = 0; i < loaded.rgba.size(); ++i)
                if (i % 4 != 3)
                {
                    const float v = loaded.rgba[i] / 255.0f;
                    loaded.rgba[i] = static_cast<uint8_t>(std::lround(std::clamp(255 * dark * v * v, 0.0f, 255.0f)));
                }
        return loaded;
    };
    const Image image = darken(Load(argv[3]));
    const std::optional<Image> other = partFile ? darken(Load(partFile)) : cutFile ? darken(Load(cutFile)) : std::optional<Image>();
    if (width > image.width || height > image.height || (other && (width > other->width || height > other->height)))
        throw std::runtime_error("An image is smaller than the picture.");
    if (cutFile && cutAt < 1)
        throw std::runtime_error("--cut needs --cut-at.");
    // The moving part is a quarter of the picture wide and half as high, from the middle of the other image.
    const Rect partRect{ 0, height / 4, width / 4, height / 2 };
    // The picture and the part never leave their image, so the motion stops short where it would.
    if (partFile)
        frames = std::min(frames, (width - partRect.width) / std::max(1, std::abs(scroll)) + 1);
    else if (scroll)
        frames = std::min(frames, (std::min(image.width, other ? other->width : image.width) - width) / std::abs(scroll) + 1);
    const int total = frames + hold;
    std::vector<Layout> layouts(total);
    for (int index = 0; index < total; ++index)
    {
        const int step = std::min(index, frames - 1);
        Layout& layout = layouts[index];
        if (partFile)
            layout.part = scroll < 0 ? width - partRect.width + step * scroll : step * scroll;
        else
        {
            layout.offset = step * scroll + (scroll < 0 ? image.width - width : 0);
            layout.second = cutFile && index >= cutAt;
        }
    }
    const std::vector<Rect> hudRects = hud ? std::vector<Rect>{ { 24, 24, 320, 28 }, { width - 260, height - 60, 236, 36 }, { width / 2 - 12, height / 2 - 12, 24, 24 } }
                                           : std::vector<Rect>{};
    const auto inHud = [&](int x, int y) {
        for (const Rect& rect : hudRects)
            if (rect.Contains(x, y))
                return true;
        return false;
    };

    std::printf("Picture %dx%d of %ls (%dx%d), %d frames moving %d pixels each, then %d still, at %d a second, detail %d%s%s%s\n", width, height,
                argv[3], image.width, image.height, frames, scroll, hold, fps, static_cast<int>(detail), partFile ? ", a part moving over it" : "",
                cutFile ? ", cut to another image" : "", hud ? ", with a HUD" : "");
    if (dark > 0 || grain > 0)
        std::printf("Darkened by %.2f, with grain of %.1f levels\n", dark, grain);
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

    const auto loadStarted = Clock::now();
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
    const double createMs = std::chrono::duration<double, std::milli>(Clock::now() - loadStarted).count();

    std::vector<std::array<float, 4>> settings(addon->settingCount);
    for (uint32_t i = 0; i < addon->settingCount; ++i)
        std::copy_n(addon->settings[i].defaultValue, 4, settings[i].data());
    settings[0][0] = detail;

    std::vector<uint8_t> picture(size_t(width) * height * 4);
    // Drawing new noise for every frame would take longer than a frame, so each frame reads this from a place of its
    // own instead.
    constexpr size_t kGrainReach = 1 << 16;
    std::vector<float> noise;
    std::mt19937 random(1);
    if (grain > 0)
    {
        std::normal_distribution<float> normal(0.0f, grain);
        noise.resize(picture.size() + kGrainReach);
        for (float& sample : noise)
            sample = normal(random);
    }
    std::vector<std::vector<float>> closeness(total);
    std::vector<float> lastMoving, last;
    uint64_t value = 0;
    double callMs = 0, frameMs = 0, worstMs = 0, firstCallsMs = 0;
    std::vector<double> frameTimes, frameTime(total);
    int warmFrames = 0;
    const auto start = Clock::now();
    // The model takes seconds to load, so the picture stays where it starts until depth arrives, and five seconds more
    // for depth to settle.
    const int kWarm = 5 * fps;
    for (int index = 0, tick = 0, warm = 0; index < total; ++tick)
    {
        std::this_thread::sleep_until(start + tick * std::chrono::microseconds(1000000 / fps));
        if (tick > 60 * fps && warm < kWarm)
            throw std::runtime_error("The add-on wrote no depth in a minute.");
        const Layout& layout = layouts[index];
        const Image& source = layout.second ? *other : image;
        const float* frameNoise = grain > 0 ? &noise[random() % kGrainReach] : nullptr;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const uint8_t* from = &source.rgba[(size_t(y) * source.width + x + layout.offset) * 4];
                if (layout.part >= 0 && partRect.Contains(x - layout.part, y))
                {
                    const int px = x - layout.part + (other->width - partRect.width) / 2;
                    const int py = y - partRect.y + (other->height - partRect.height) / 2;
                    from = &other->rgba[(size_t(py) * other->width + px) * 4];
                }
                uint8_t* to = &picture[(size_t(y) * width + x) * 4];
                to[0] = from[2];
                to[1] = from[1];
                to[2] = from[0];
                to[3] = 255;
                if (frameNoise)
                    for (int c = 0; c < 3; ++c)
                        to[c] = static_cast<uint8_t>(std::clamp(std::lround(to[c] + frameNoise[(size_t(y) * width + x) * 4 + c]), 0l, 255l));
            }
        // Bars with a border and stripes like lettering, the same in every frame.
        for (const Rect& rect : hudRects)
            for (int y = rect.y; y < rect.y + rect.height; ++y)
                for (int x = rect.x; x < rect.x + rect.width; ++x)
                {
                    const bool border = y < rect.y + 3 || y >= rect.y + rect.height - 3 || x < rect.x + 3 || x >= rect.x + rect.width - 3;
                    const bool stripe = ((x - rect.x) / 4) % 2 == 0;
                    uint8_t* to = &picture[(size_t(y) * width + x) * 4];
                    to[0] = to[1] = to[2] = border ? 20 : stripe ? 240 : 60;
                    if (!border && !stripe)
                        to[2] = 200;
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
        // The picture only stops changing once it stops moving, unless its grain changes it.
        frame.picture = grain > 0 ? static_cast<uint64_t>(tick) + 1 : static_cast<uint64_t>(std::min(index, frames - 1)) + 1;
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
            frameTimes.push_back(took);
            frameTime[index] = took;
        }
        else if (tick < 10)
            firstCallsMs = std::max(firstCallsMs, took);

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
            ++warmFrames;
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

    // Where a pixel of frame i shows in frame j, or -1 where it does not. Only the part or the picture moves, and only
    // sideways.
    const auto where = [&](int i, int j, int x, int y) {
        if (inHud(x, y))
            return -1;
        const Layout& a = layouts[i];
        const Layout& b = layouts[j];
        int to = x;
        if (a.part >= 0)
        {
            if (partRect.Contains(x - a.part, y))
                to = x - a.part + b.part;
            else if (partRect.Contains(x - b.part, y))
                return -1;
        }
        else
        {
            if (a.second != b.second)
                return -1;
            to = x + a.offset - b.offset;
        }
        return to >= 0 && to < width && !inHud(to, y) ? to : -1;
    };
    // The background pixels of frame i that the part covered in one of the kWake frames before.
    const auto uncovered = [&](int i, int x, int y) {
        const Layout& a = layouts[i];
        if (a.part < 0 || partRect.Contains(x - a.part, y))
            return false;
        for (int k = 1; k <= kWake && i - k >= 0; ++k)
            if (partRect.Contains(x - layouts[i - k].part, y))
                return true;
        return false;
    };
    // The mean difference between frame i's closeness and frame j's where i's pixels show in j, as a share of j's mean
    // there, so it says the same whatever the closeness is measured against. And the middle one of the ratios, which is
    // 1 when depth as a whole is the same in both. Only the pixels select takes count.
    struct Comparison
    {
        double difference = 0;
        double scale = 0;
        size_t pixels = 0;
    };
    const auto compare = [&](int i, int j, auto select) {
        double sum = 0, mean = 0;
        std::vector<float> ratios;
        Comparison result;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                if (!select(x, y))
                    continue;
                const int to = where(i, j, x, y);
                if (to < 0)
                    continue;
                const float a = closeness[i][size_t(y) * width + x], b = closeness[j][size_t(y) * width + to];
                sum += std::abs(a - b);
                mean += b;
                ++result.pixels;
                if (x % 4 == 0 && y % 4 == 0)
                    ratios.push_back(a / b);
            }
        result.difference = mean > 0 ? sum / mean : 0;
        if (!ratios.empty())
        {
            std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
            result.scale = std::abs(ratios[ratios.size() / 2] - 1);
        }
        return result;
    };
    const auto all = [](int, int) { return true; };
    // How many pixels frame i's depth trails its picture by, along the motion: the shift of the rest frame's depth, as
    // a whole brought to the same scale, that matches it best. The moving part is seen differently against each part
    // of the background, so this is only measured on a pan.
    const int direction = partFile ? (scroll < 0 ? -1 : 1) : (scroll < 0 ? 1 : -1);
    const auto trail = [&](int i) {
        constexpr int kReach = 48;
        std::optional<double> best;
        int bestShift = 0;
        for (int shift = -kReach; shift <= kReach; ++shift)
        {
            double a = 0, b = 0;
            std::vector<std::pair<float, float>> pairs;
            for (int y = 0; y < height; y += 4)
                for (int x = 0; x < width; x += 4)
                {
                    if (partFile && !partRect.Contains(x - layouts[i].part, y))
                        continue;
                    const int at = where(i, total - 1, x, y);
                    if (at < 0 || at + shift < 0 || at + shift >= width)
                        continue;
                    pairs.emplace_back(closeness[i][size_t(y) * width + x], closeness[total - 1][size_t(y) * width + at + shift]);
                    a += pairs.back().first;
                    b += pairs.back().second;
                }
            if (pairs.empty() || b <= 0)
                continue;
            const double scale = a / b;
            double sum = 0;
            for (const auto& [here, there] : pairs)
                sum += std::abs(here - scale * there);
            const double difference = sum / a;
            if (!best || difference < *best)
            {
                best = difference;
                bestShift = shift;
            }
        }
        return bestShift * direction;
    };

    // Depth takes a few estimates to follow the picture once it starts to move, so the first half of the moving frames
    // does not count, and nor do the frames after a cut, which have figures of their own.
    const int from = frames / 2;
    const int to = cutFile ? std::min(frames, cutAt) : frames;
    std::vector<double> flicker, pulse, late, off, wake, part, lag, resting, jumps;
    // At rest, a second after the picture stops, depth should only change as much as grain makes it. A pixel that jumps
    // by an edge's step from one frame to the next is one that went from one side of an edge to the other, which
    // effects show as edges that crawl.
    const int settled = frames + fps;
    const auto jumped = [&](int i) {
        size_t count = 0, pixels = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (!inHud(x, y))
                {
                    const float a = closeness[i][size_t(y) * width + x], b = closeness[i - 1][size_t(y) * width + x];
                    count += std::abs(a - b) > kEdgeStep * std::max(a, b);
                    ++pixels;
                }
        return pixels ? double(count) / pixels : 0;
    };
    FILE* csv = nullptr;
    _wfopen_s(&csv, (output / "frames.csv").c_str(), L"w");
    if (csv)
        std::fprintf(csv, "frame,flicker,pulse,late,off,ms\n");
    for (int index = 1; index < total; ++index)
    {
        const Comparison change = compare(index, index - 1, all);
        const Comparison rest = compare(index, total - 1, all);
        if (csv)
            std::fprintf(csv, "%d,%.5f,%.5f,%.5f,%.5f,%.2f\n", index, change.difference, change.scale, rest.difference, rest.scale, frameTime[index]);
        if (index > settled)
        {
            resting.push_back(change.difference);
            jumps.push_back(jumped(index));
        }
        if (index < from || index >= to)
            continue;
        flicker.push_back(change.difference);
        pulse.push_back(change.scale);
        if (!partFile)
            lag.push_back(trail(index));
        if (rest.pixels)
        {
            late.push_back(rest.difference);
            off.push_back(rest.scale);
        }
        if (partFile)
        {
            const Comparison opened = compare(index, total - 1, [&](int x, int y) { return uncovered(index, x, y); });
            if (opened.pixels)
                wake.push_back(opened.difference);
            part.push_back(compare(index, total - 1, [&](int x, int y) { return partRect.Contains(x - layouts[index].part, y); }).difference);
        }
    }
    if (csv)
        std::fclose(csv);
    std::printf("Frame to frame, depth changed by %.3f%% on average while the picture moved (%.3f%% in the worst tenth of frames), and as a whole "
                "by %.3f%% (%.3f%%).\n",
                100 * Mean(flicker), 100 * Worst(flicker), 100 * Mean(pulse), 100 * Worst(pulse));
    if (!late.empty())
        std::printf("It was %.3f%% from the depth the picture came to rest at (%.3f%% at worst), and as a whole %.3f%% (%.3f%%).\n", 100 * Mean(late),
                    100 * Worst(late), 100 * Mean(off), 100 * Worst(off));
    if (!resting.empty())
        std::printf("At rest, it changed by %.3f%% frame to frame (%.3f%%), and %.3f%% of pixels jumped by an edge's step (%.3f%%).\n",
                    100 * Mean(resting), 100 * Worst(resting), 100 * Mean(jumps), 100 * Worst(jumps));
    if (!lag.empty())
        std::printf("Depth trailed the picture by %.1f pixels on average, and %.0f at most.\n", Mean(lag), *std::max_element(lag.begin(), lag.end()));
    if (partFile)
        std::printf("On the moving part it was %.3f%% from rest, and %.3f%% where the part had just uncovered the background.\n", 100 * Mean(part),
                    100 * Mean(wake));
    if (cutFile)
    {
        // How far from the new scene's rest depth each frame after the cut was, in groups of frames.
        const int groups[][2] = { { 0, 5 }, { 5, 10 }, { 10, 20 }, { 20, 40 } };
        std::printf("After the cut, depth was from rest");
        const char* separator = " ";
        for (const auto& group : groups)
        {
            std::vector<double> values;
            for (int index = cutAt + group[0]; index < std::min(cutAt + group[1], frames); ++index)
                values.push_back(compare(index, total - 1, all).difference);
            if (!values.empty())
                std::printf("%s%.3f%% in frames %d to %d", std::exchange(separator, ", "), 100 * Mean(values), group[0] + 1, group[1]);
        }
        std::printf(".\n");
    }
    std::sort(frameTimes.begin(), frameTimes.end());
    std::printf("A frame took the add-on %.2f ms on the processor and %.2f ms until the graphics card was done, %.2f ms in 99%% of frames and "
                "%.2f ms at worst.\n",
                callMs / total, frameMs / total, frameTimes[frameTimes.size() * 99 / 100], worstMs);
    std::printf("Starting took %.0f ms, the slowest of the first frames %.0f ms, and depth came after %d frames.\n", createMs, firstCallsMs,
                warmFrames - kWarm + 1);
    Shape(lastMoving, width, height, "moving");
    Shape(last, width, height, "still");
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
