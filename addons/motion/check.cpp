// Runs the motion estimator on pictures moved by known amounts, without a window, and says how far its vectors are
// from the truth, how sure it is where it should and should not be, and how long it takes.
// motion_check <image> <other image> <output folder>
// The image is moved as a whole by whole and by fractional pixels, held still, and has a part of itself moved over it.
// The other image, upside down, is what the picture cuts to. Each case's last motion and confidence are written as PNG.

#include "motion/motion_d3d11.h"

#include <DirectXPackedVector.h>
#include <dxgi1_2.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4996) // sprintf in the HDR writer
#include <stb_image_write.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
};

Image Load(const std::filesystem::path& path)
{
    Image image;
    int channels = 0;
    stbi_uc* loaded = stbi_load(path.string().c_str(), &image.width, &image.height, &channels, 3);
    if (!loaded)
        throw std::runtime_error("Could not read " + path.string() + ": " + stbi_failure_reason());
    image.rgb.assign(loaded, loaded + size_t(image.width) * image.height * 3);
    stbi_image_free(loaded);
    for (float& value : image.rgb)
        value /= 255;
    return image;
}

// Catmull-Rom weights for a sample t of the way from the second of four pixels to the third. At a whole pixel they
// give that pixel alone, so a picture moved by whole pixels is the image itself.
void Weights(float t, float weights[4])
{
    const float t2 = t * t, t3 = t2 * t;
    weights[0] = (-t3 + 2 * t2 - t) / 2;
    weights[1] = (3 * t3 - 5 * t2 + 2) / 2;
    weights[2] = (-3 * t3 + 4 * t2 + t) / 2;
    weights[3] = (t3 - t2) / 2;
}

// The image's color at x, y, between pixels with Catmull-Rom, at the edge with the edge's pixels.
void Sample(const Image& image, float x, float y, float color[3])
{
    const float fx = std::floor(x), fy = std::floor(y);
    float wx[4], wy[4];
    Weights(x - fx, wx);
    Weights(y - fy, wy);
    color[0] = color[1] = color[2] = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
        {
            const int sx = std::clamp(static_cast<int>(fx) - 1 + i, 0, image.width - 1);
            const int sy = std::clamp(static_cast<int>(fy) - 1 + j, 0, image.height - 1);
            const float* pixel = &image.rgb[(size_t(sy) * image.width + sx) * 3];
            for (int c = 0; c < 3; ++c)
                color[c] += wx[i] * wy[j] * pixel[c];
        }
}

// A picture as BGRA8, as Unishade's SDR pictures are.
struct Picture
{
    std::vector<uint8_t> bgra;
};

Picture Render(int width, int height, const std::function<void(int, int, float[3])>& color)
{
    Picture picture{ std::vector<uint8_t>(size_t(width) * height * 4) };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            float rgb[3];
            color(x, y, rgb);
            uint8_t* pixel = &picture.bgra[(size_t(y) * width + x) * 4];
            for (int c = 0; c < 3; ++c)
                pixel[2 - c] = static_cast<uint8_t>(std::lround(255 * std::clamp(rgb[c], 0.0f, 1.0f)));
            pixel[3] = 255;
        }
    return picture;
}

// What a pixel of a picture truly did since the picture before.
struct Truth
{
    float x = 0;
    float y = 0;
    // Nothing in the picture before is where it came from: it came into view from outside the picture or from behind
    // something that moved.
    bool unseen = false;
};

struct Case
{
    std::string name;
    std::vector<Picture> pictures;
    // The truth for a pixel of a picture after the first.
    std::function<Truth(int x, int y, int picture)> truth;
};

struct Device
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
};

Device CreateDevice()
{
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
    Device device;
    winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.device.put(),
                                           nullptr, device.context.put()));
    return device;
}

struct Input
{
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
};

Input CreateInput(ID3D11Device* device, int width, int height)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Input input;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, input.texture.put()));
    winrt::check_hresult(device->CreateShaderResourceView(input.texture.get(), nullptr, input.view.put()));
    return input;
}

winrt::com_ptr<ID3D11Texture2D> CreateReadable(ID3D11Device* device, ID3D11Texture2D* like)
{
    D3D11_TEXTURE2D_DESC desc{};
    like->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
    return texture;
}

// One estimate as read back.
struct Result
{
    std::vector<float> x, y, confidence;
    std::vector<bool> zero; // exactly zero in both halves
    bool cut = false;
};

Result Read(const Device& device, motion::Estimator& estimator, int width, int height)
{
    const auto motion = CreateReadable(device.device.get(), estimator.motion.get());
    const auto confidence = CreateReadable(device.device.get(), estimator.confidence.get());
    D3D11_BUFFER_DESC desc{};
    estimator.keys->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Buffer> keys;
    winrt::check_hresult(device.device->CreateBuffer(&desc, nullptr, keys.put()));
    device.context->CopyResource(motion.get(), estimator.motion.get());
    device.context->CopyResource(confidence.get(), estimator.confidence.get());
    device.context->CopyResource(keys.get(), estimator.keys.get());

    Result result;
    const size_t pixels = size_t(width) * height;
    result.x.resize(pixels);
    result.y.resize(pixels);
    result.confidence.resize(pixels);
    result.zero.resize(pixels);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(device.context->Map(motion.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (int y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
        for (int x = 0; x < width; ++x)
        {
            const size_t i = size_t(y) * width + x;
            result.x[i] = DirectX::PackedVector::XMConvertHalfToFloat(row[2 * x]);
            result.y[i] = DirectX::PackedVector::XMConvertHalfToFloat(row[2 * x + 1]);
            result.zero[i] = (row[2 * x] & 0x7FFF) == 0 && (row[2 * x + 1] & 0x7FFF) == 0;
        }
    }
    device.context->Unmap(motion.get(), 0);
    winrt::check_hresult(device.context->Map(confidence.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            result.confidence[size_t(y) * width + x] = static_cast<const uint8_t*>(mapped.pData)[size_t(y) * mapped.RowPitch + x] / 255.0f;
    device.context->Unmap(confidence.get(), 0);
    winrt::check_hresult(device.context->Map(keys.get(), 0, D3D11_MAP_READ, 0, &mapped));
    result.cut = *static_cast<const uint32_t*>(mapped.pData) != 0;
    device.context->Unmap(keys.get(), 0);
    return result;
}

// Prints how an estimate compares with the truth.
void Report(const char* label, const Result& result, const Case& test, int picture, int width, int height)
{
    double error = 0, x = 0, y = 0, sureRight = 0, sureWrong = 0, sureUnseen = 0;
    size_t seen = 0, wrong = 0, right = 0, unseen = 0, zero = 0, still = 0, stillZero = 0, over1 = 0;
    std::vector<float> errors;
    for (int py = 0; py < height; ++py)
        for (int px = 0; px < width; ++px)
        {
            const size_t i = size_t(py) * width + px;
            const Truth truth = test.truth(px, py, picture);
            zero += result.zero[i];
            if (truth.unseen)
            {
                ++unseen;
                sureUnseen += result.confidence[i];
                continue;
            }
            if (truth.x == 0 && truth.y == 0)
            {
                ++still;
                stillZero += result.zero[i];
            }
            const float e = std::hypot(result.x[i] - truth.x, result.y[i] - truth.y);
            errors.push_back(e);
            error += e;
            x += result.x[i];
            y += result.y[i];
            ++seen;
            over1 += e > 1;
            if (e > 1)
            {
                ++wrong;
                sureWrong += result.confidence[i];
            }
            else if (e <= 0.5f)
            {
                ++right;
                sureRight += result.confidence[i];
            }
        }
    std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end());
    const auto share = [](size_t part, size_t whole) { return whole ? 100.0 * part / whole : 0.0; };
    const auto mean = [](double sum, size_t count) { return count ? sum / count : 0.0; };
    std::printf("  %-6s endpoint error %.3f px mean, %.3f median, %.2f%% over 1 px; mean vector (%.3f, %.3f); cut %s\n", label, mean(error, seen),
                errors.empty() ? 0.0 : errors[errors.size() / 2], share(over1, seen), mean(x, seen), mean(y, seen), result.cut ? "yes" : "no");
    std::printf("         confidence %.3f where right (within 0.5 px, %.2f%%), %.3f where wrong (over 1 px, %.2f%%), %.3f where unseen before (%.2f%%)\n",
                mean(sureRight, right), share(right, seen), mean(sureWrong, wrong), share(wrong, seen), mean(sureUnseen, unseen),
                share(unseen, size_t(width) * height));
    std::printf("         exactly zero: %.2f%% of all pixels, %.2f%% of the pixels that did not move\n", share(zero, size_t(width) * height),
                share(stillZero, still));
}

void Save(const Result& result, const Picture& picture, int width, int height, float largest, const std::filesystem::path& folder, const std::string& name)
{
    // Direction as hue and length as saturation, full at largest pixels.
    std::vector<uint8_t> colors(size_t(width) * height * 3), sure(size_t(width) * height), shown(size_t(width) * height * 3);
    for (size_t i = 0; i < sure.size(); ++i)
    {
        const float angle = std::atan2(result.y[i], result.x[i]) / (2 * std::numbers::pi_v<float>) + 0.5f;
        const float saturation = std::min(std::hypot(result.x[i], result.y[i]) / largest, 1.0f);
        for (int c = 0; c < 3; ++c)
        {
            const float k = std::fmod(5 - 2 * c + angle * 6, 6.0f);
            const float channel = 1 - saturation * std::clamp(std::min(k, 4 - k), 0.0f, 1.0f);
            colors[i * 3 + c] = static_cast<uint8_t>(std::lround(255 * channel));
        }
        sure[i] = static_cast<uint8_t>(std::lround(255 * result.confidence[i]));
        for (int c = 0; c < 3; ++c)
            shown[i * 3 + c] = picture.bgra[i * 4 + 2 - c];
    }
    for (const auto& [suffix, data, channels] : { std::tuple{ "-motion.png", colors.data(), 3 }, std::tuple{ "-confidence.png", sure.data(), 1 },
                                                  std::tuple{ "-picture.png", shown.data(), 3 } })
    {
        const std::filesystem::path path = folder / (name + suffix);
        if (!stbi_write_png(path.string().c_str(), width, height, channels, data, width * channels))
            throw std::runtime_error("Could not write " + path.string());
    }
}

// The middle of the image moving by step pixels from each picture to the next.
Case Pan(const Image& image, int width, int height, float stepX, float stepY, int count, const std::string& name)
{
    Case test{ name };
    const float left = (image.width - width) / 2.0f, top = (image.height - height) / 2.0f;
    for (int f = 0; f < count; ++f)
        test.pictures.push_back(Render(width, height, [&](int x, int y, float rgb[3]) { Sample(image, x + left - f * stepX, y + top - f * stepY, rgb); }));
    test.truth = [=](int x, int y, int) {
        const float fromX = x - stepX, fromY = y - stepY;
        return Truth{ -stepX, -stepY, fromX < 0 || fromY < 0 || fromX > width - 1 || fromY > height - 1 };
    };
    return test;
}

int Main(int argc, wchar_t** argv)
{
    if (argc < 4)
    {
        std::printf("motion_check <image> <other image> <output folder>\n");
        return 2;
    }
    const Image image = Load(argv[1]), other = Load(argv[2]);
    const std::filesystem::path output = argv[3];
    std::filesystem::create_directories(output);
    const Device device = CreateDevice();

    // Pictures smaller than the image, so a moving picture never runs out of it.
    constexpr int kWidth = 1280, kHeight = 720, kCount = 5;
    if (image.width < kWidth + 400 || image.height < kHeight + 300)
        throw std::runtime_error("The image is too small.");
    std::vector<Case> cases;
    cases.push_back(Pan(image, kWidth, kHeight, 3, 0, kCount, "right-3"));
    cases.push_back(Pan(image, kWidth, kHeight, 2.5f, -1.25f, kCount, "fraction"));
    cases.push_back(Pan(image, kWidth, kHeight, 0.25f, 0.5f, kCount, "slow"));
    cases.push_back(Pan(image, kWidth, kHeight, -12.75f, 7.5f, kCount, "fast"));
    cases.push_back(Pan(image, kWidth, kHeight, 40, -24, kCount, "faster"));
    cases.push_back(Pan(image, kWidth, kHeight, 0, 0, kCount, "still"));

    // A part of the image, from its upper right, moving over the still picture.
    {
        constexpr float kStepX = 6.5f, kStepY = 2.25f, kPartLeft = 1150, kPartTop = 40, kStartX = 400, kStartY = 240;
        constexpr int kPartWidth = 320, kPartHeight = 240;
        Case test{ "part" };
        const float left = (image.width - kWidth) / 2.0f, top = (image.height - kHeight) / 2.0f;
        const auto inside = [=](float x, float y, int f) {
            const float u = x - (kStartX + f * kStepX), v = y - (kStartY + f * kStepY);
            return u >= 0 && v >= 0 && u < kPartWidth && v < kPartHeight;
        };
        for (int f = 0; f < kCount; ++f)
            test.pictures.push_back(Render(kWidth, kHeight, [&](int x, int y, float rgb[3]) {
                if (inside(static_cast<float>(x), static_cast<float>(y), f))
                    Sample(image, kPartLeft + x - (kStartX + f * kStepX), kPartTop + y - (kStartY + f * kStepY), rgb);
                else
                    Sample(image, x + left, y + top, rgb);
            }));
        test.truth = [=](int x, int y, int f) {
            if (inside(static_cast<float>(x), static_cast<float>(y), f))
                return Truth{ -kStepX, -kStepY, false };
            return Truth{ 0, 0, inside(static_cast<float>(x), static_cast<float>(y), f - 1) };
        };
        cases.push_back(std::move(test));
    }

    // A pan that cuts to the other image upside down for its last picture.
    {
        Case test = Pan(image, kWidth, kHeight, 3, 0, kCount, "cut");
        const float left = (other.width - kWidth) / 2.0f, top = (other.height - kHeight) / 2.0f;
        test.pictures.back() = Render(kWidth, kHeight, [&](int x, int y, float rgb[3]) { Sample(other, x + left, other.height - 1 - (y + top), rgb); });
        const auto pan = test.truth;
        test.truth = [=](int x, int y, int f) { return f == kCount - 1 ? Truth{ 0, 0, true } : pan(x, y, f); };
        cases.push_back(std::move(test));
    }

    // The same view after a change of lighting: the other image as it is, which is the same scene graded differently.
    {
        Case test = Pan(image, kWidth, kHeight, 0, 0, 2, "lighting");
        const float left = (other.width - kWidth) / 2.0f, top = (other.height - kHeight) / 2.0f;
        test.pictures.back() = Render(kWidth, kHeight, [&](int x, int y, float rgb[3]) { Sample(other, x + left, y + top, rgb); });
        cases.push_back(std::move(test));
    }

    const Input current = CreateInput(device.device.get(), kWidth, kHeight), previous = CreateInput(device.device.get(), kWidth, kHeight);
    for (const Case& test : cases)
    {
        std::printf("%s:\n", test.name.c_str());
        motion::Estimator estimator(device.device.get(), kWidth, kHeight);
        std::vector<int> cuts;
        for (int f = 1; f < static_cast<int>(test.pictures.size()); ++f)
        {
            device.context->UpdateSubresource(previous.texture.get(), 0, nullptr, test.pictures[f - 1].bgra.data(), kWidth * 4, 0);
            device.context->UpdateSubresource(current.texture.get(), 0, nullptr, test.pictures[f].bgra.data(), kWidth * 4, 0);
            estimator.Estimate(device.context.get(), current.view.get(), previous.view.get(), 1);
            const bool last = f + 1 == static_cast<int>(test.pictures.size());
            if (f != 1 && !last)
            {
                // Only the cut flag of the estimates between.
                if (Read(device, estimator, kWidth, kHeight).cut)
                    cuts.push_back(f);
                continue;
            }
            const Result result = Read(device, estimator, kWidth, kHeight);
            if (result.cut)
                cuts.push_back(f);
            if (test.truth)
                Report(f == 1 ? "first" : "last", result, test, f, kWidth, kHeight);
            if (last)
            {
                float largest = 1;
                if (test.truth)
                    for (int y = 0; y < kHeight; y += 8)
                        for (int x = 0; x < kWidth; x += 8)
                        {
                            const Truth truth = test.truth(x, y, f);
                            largest = std::max(largest, std::hypot(truth.x, truth.y));
                        }
                Save(result, test.pictures[f], kWidth, kHeight, largest, output, test.name);
            }
        }
        std::printf("  cut flagged on estimate");
        for (int f : cuts)
            std::printf(" %d", f);
        std::printf("%s of %zu\n", cuts.empty() ? " none" : "", test.pictures.size() - 1);
    }

    // How long an estimate takes on the graphics card, from the image moving 3 pixels at each size.
    for (const auto& [width, height] : { std::pair{ 1920, 1080 }, std::pair{ 3840, 2160 } })
    {
        const float scale = static_cast<float>(image.width) / width;
        const Picture before = Render(width, height, [&](int x, int y, float rgb[3]) { Sample(image, x * scale, y * scale, rgb); });
        const Picture after = Render(width, height, [&](int x, int y, float rgb[3]) { Sample(image, (x - 3) * scale, y * scale, rgb); });
        const Input timedCurrent = CreateInput(device.device.get(), width, height), timedPrevious = CreateInput(device.device.get(), width, height);
        device.context->UpdateSubresource(timedPrevious.texture.get(), 0, nullptr, before.bgra.data(), width * 4, 0);
        device.context->UpdateSubresource(timedCurrent.texture.get(), 0, nullptr, after.bgra.data(), width * 4, 0);
        motion::Estimator estimator(device.device.get(), width, height);
        D3D11_QUERY_DESC query{ D3D11_QUERY_TIMESTAMP_DISJOINT };
        winrt::com_ptr<ID3D11Query> disjoint, begin, end;
        winrt::check_hresult(device.device->CreateQuery(&query, disjoint.put()));
        query.Query = D3D11_QUERY_TIMESTAMP;
        winrt::check_hresult(device.device->CreateQuery(&query, begin.put()));
        winrt::check_hresult(device.device->CreateQuery(&query, end.put()));
        // Estimates run in batches, as they would every frame, since a card left idle between them slows its clock.
        constexpr int kWarm = 5, kTimed = 50, kBatch = 10;
        std::vector<double> times;
        for (int run = 0; run < kWarm + kTimed; ++run)
        {
            device.context->Begin(disjoint.get());
            device.context->End(begin.get());
            for (int i = 0; i < kBatch; ++i)
                estimator.Estimate(device.context.get(), timedCurrent.view.get(), timedPrevious.view.get(), 1);
            device.context->End(end.get());
            device.context->End(disjoint.get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
            while (device.context->GetData(disjoint.get(), &frequency, sizeof(frequency), 0) != S_OK)
                ;
            UINT64 from = 0, to = 0;
            while (device.context->GetData(begin.get(), &from, sizeof(from), 0) != S_OK)
                ;
            while (device.context->GetData(end.get(), &to, sizeof(to), 0) != S_OK)
                ;
            if (run >= kWarm && !frequency.Disjoint)
                times.push_back(1000.0 * (to - from) / frequency.Frequency / kBatch);
        }
        std::sort(times.begin(), times.end());
        if (times.empty())
            throw std::runtime_error("The graphics card gave no times.");
        double sum = 0;
        for (double time : times)
            sum += time;
        std::printf("%dx%d: an estimate took %.3f ms on the graphics card on average, %.3f ms median, %.3f ms at best, over %zu batches of %d\n", width,
                    height, sum / times.size(), times[times.size() / 2], times.front(), times.size(), kBatch);
    }
    std::printf("Wrote the motion, confidence and picture of each case's last estimate to %s\n", output.string().c_str());
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
