#pragma once

// motion_shared.h's estimator on a Direct3D 11 device.

#include "motion/motion_shared.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

namespace motion
{
// Makes everything an estimate at one picture size needs on its device, and its results. A new size needs a new one.
class Estimator
{
public:
    Estimator(ID3D11Device* device, UINT width, UINT height);

    // Estimates the motion from previous to current, views of two pictures of the estimator's size in any format the
    // device can read as color. White is the value of SDR white in them. Leaves the results in motion, confidence and
    // keys, and the device's compute bindings empty.
    void Estimate(ID3D11DeviceContext* context, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* previous, float white);
    // Only says whether it is a cut, in keys, as Estimate would for the same pictures, for a caller that has the motion
    // from elsewhere. Leaves motion and confidence as they were.
    void Cut(ID3D11DeviceContext* context, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* previous, float white);

    // Each pixel's vector to where it was in the previous picture, in pixels, as R16G16_FLOAT.
    winrt::com_ptr<ID3D11Texture2D> motion;
    winrt::com_ptr<ID3D11ShaderResourceView> motionView;
    // How sure each vector is, from 0 to 1, as R8_UNORM. 0 where the pixel came into view or the match failed.
    winrt::com_ptr<ID3D11Texture2D> confidence;
    winrt::com_ptr<ID3D11ShaderResourceView> confidenceView;
    // A raw buffer whose first unsigned number is 1 after a cut to another scene and 0 otherwise. After a cut, every
    // vector and confidence is 0.
    winrt::com_ptr<ID3D11Buffer> keys;

    // Compiles the shaders, which takes seconds, nearly all of it Search's and Refine's. It is done once for every
    // estimator a program makes, each shader on a thread of its own, the first time this or the constructor runs. A
    // caller that cannot wait that long calls this on another thread first.
    static void Compile() { Code(); }

private:
    enum Shader
    {
        kReduce,
        kDownsample,
        kResolve,
        kSearch,
        kRefine,
        kFinish,
        kShaderCount,
    };
    struct Level
    {
        UINT width = 0;
        UINT height = 0;
        winrt::com_ptr<ID3D11Buffer> constants;
        winrt::com_ptr<ID3D11ShaderResourceView> lumaView;
        winrt::com_ptr<ID3D11UnorderedAccessView> lumaTarget;
        winrt::com_ptr<ID3D11ShaderResourceView> motionView;
        winrt::com_ptr<ID3D11UnorderedAccessView> motionTarget;
    };

    static const std::array<std::vector<uint8_t>, kShaderCount>& Code();
    // Runs a shader with views in t0 to t5 and targets in u0 to u5, and unbinds them after.
    void Dispatch(ID3D11DeviceContext* context, Shader shader, ID3D11Buffer* constants, std::array<ID3D11ShaderResourceView*, 6> views,
                  std::array<ID3D11UnorderedAccessView*, 6> targets, UINT x, UINT y);

    std::array<winrt::com_ptr<ID3D11ComputeShader>, kShaderCount> shaders;
    Level picture;             // the luma at the picture's size, and the constants of Reduce and Finish
    std::vector<Level> levels; // from half the picture's size down
    size_t finest = 0;         // the level Refine matches
    winrt::com_ptr<ID3D11Buffer> frame;
    winrt::com_ptr<ID3D11SamplerState> linear;
    winrt::com_ptr<ID3D11UnorderedAccessView> keysTarget;
    winrt::com_ptr<ID3D11UnorderedAccessView> stillTarget;
    winrt::com_ptr<ID3D11UnorderedAccessView> motionTarget;
    winrt::com_ptr<ID3D11UnorderedAccessView> confidenceTarget;
};

// As kShaders' cbuffers lay them out.
struct PassConstants
{
    UINT size[2];
    UINT pictureSize[2];
    float scale;
    UINT coarsest;
    UINT padding[2];
};
struct FrameConstants
{
    float white;
    float padding[3];
};

inline winrt::com_ptr<ID3D11Texture2D> CreateTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
    return texture;
}

inline winrt::com_ptr<ID3D11Buffer> CreateConstants(ID3D11Device* device, const void* data, UINT size, D3D11_USAGE usage)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = size;
    desc.Usage = usage;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA initial{ data };
    winrt::com_ptr<ID3D11Buffer> buffer;
    winrt::check_hresult(device->CreateBuffer(&desc, &initial, buffer.put()));
    return buffer;
}

inline const std::array<std::vector<uint8_t>, Estimator::kShaderCount>& Estimator::Code()
{
    static const std::array<std::vector<uint8_t>, kShaderCount> code = [] {
        const std::string source = std::string("#define CONSTANT static const\n") + depth::kShaderShared + kShaderShared + kShaders;
        const char* entryPoints[] = { "Reduce", "Downsample", "Resolve", "Search", "Refine", "Finish" };
        std::array<std::future<std::vector<uint8_t>>, kShaderCount> compiling;
        for (size_t i = 0; i < compiling.size(); ++i)
            compiling[i] = std::async(std::launch::async, [&source, entryPoint = entryPoints[i]] {
                winrt::com_ptr<ID3DBlob> blob, errors;
                if (FAILED(D3DCompile(source.data(), source.size(), "motion", nullptr, nullptr, entryPoint, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      blob.put(), errors.put())))
                    throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "The motion shaders did not compile.");
                const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
                return std::vector<uint8_t>(bytes, bytes + blob->GetBufferSize());
            });
        std::array<std::vector<uint8_t>, kShaderCount> compiled;
        for (size_t i = 0; i < compiled.size(); ++i)
            compiled[i] = compiling[i].get();
        return compiled;
    }();
    return code;
}

inline Estimator::Estimator(ID3D11Device* device, UINT width, UINT height)
{
    const auto& code = Code();
    for (size_t i = 0; i < shaders.size(); ++i)
        winrt::check_hresult(device->CreateComputeShader(code[i].data(), code[i].size(), nullptr, shaders[i].put()));

    const auto createLuma = [&](Level& level) {
        const auto luma = CreateTexture(device, level.width, level.height, DXGI_FORMAT_R16G16_FLOAT);
        winrt::check_hresult(device->CreateShaderResourceView(luma.get(), nullptr, level.lumaView.put()));
        winrt::check_hresult(device->CreateUnorderedAccessView(luma.get(), nullptr, level.lumaTarget.put()));
    };
    // Each level is half the size of the one below, rounded up. Only the finest and those above it are matched.
    bool found = false;
    for (UINT levelWidth = (width + 1) / 2, levelHeight = (height + 1) / 2, scale = 2;; levelWidth = (levelWidth + 1) / 2,
              levelHeight = (levelHeight + 1) / 2, scale *= 2)
    {
        Level& level = levels.emplace_back();
        level.width = levelWidth;
        level.height = levelHeight;
        const bool coarsest = levelWidth <= kCoarsestWidth || levelHeight <= 2;
        const PassConstants constants{ { levelWidth, levelHeight }, { width, height }, static_cast<float>(scale), coarsest };
        level.constants = CreateConstants(device, &constants, sizeof(constants), D3D11_USAGE_IMMUTABLE);
        createLuma(level);
        if (!found && (levelWidth <= kFinestWidth || coarsest))
        {
            found = true;
            finest = levels.size() - 1;
        }
        if (found)
        {
            const auto levelMotion = CreateTexture(device, levelWidth, levelHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
            winrt::check_hresult(device->CreateShaderResourceView(levelMotion.get(), nullptr, level.motionView.put()));
            winrt::check_hresult(device->CreateUnorderedAccessView(levelMotion.get(), nullptr, level.motionTarget.put()));
        }
        if (coarsest)
            break;
    }
    picture.width = width;
    picture.height = height;
    const PassConstants constants{ { width, height }, { width, height }, static_cast<float>(2u << finest), 0 };
    picture.constants = CreateConstants(device, &constants, sizeof(constants), D3D11_USAGE_IMMUTABLE);
    createLuma(picture);
    const FrameConstants white{ 1 };
    frame = CreateConstants(device, &white, sizeof(white), D3D11_USAGE_DEFAULT);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device->CreateSamplerState(&sampler, linear.put()));

    const auto still = CreateTexture(device, levels[finest].width, levels[finest].height, DXGI_FORMAT_R32_UINT);
    winrt::check_hresult(device->CreateUnorderedAccessView(still.get(), nullptr, stillTarget.put()));
    motion = CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT);
    winrt::check_hresult(device->CreateShaderResourceView(motion.get(), nullptr, motionView.put()));
    winrt::check_hresult(device->CreateUnorderedAccessView(motion.get(), nullptr, motionTarget.put()));
    confidence = CreateTexture(device, width, height, DXGI_FORMAT_R8_UNORM);
    winrt::check_hresult(device->CreateShaderResourceView(confidence.get(), nullptr, confidenceView.put()));
    winrt::check_hresult(device->CreateUnorderedAccessView(confidence.get(), nullptr, confidenceTarget.put()));

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 4 * kKeyCount;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, keys.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements = kKeyCount;
    raw.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    winrt::check_hresult(device->CreateUnorderedAccessView(keys.get(), &raw, keysTarget.put()));

    // The counts, the still pixels and the last estimate start out zero.
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    const UINT zero[4] = {};
    context->ClearUnorderedAccessViewUint(keysTarget.get(), zero);
    context->ClearUnorderedAccessViewUint(stillTarget.get(), zero);
    context->ClearUnorderedAccessViewUint(motionTarget.get(), zero);
    context->ClearUnorderedAccessViewUint(confidenceTarget.get(), zero);
}

inline void Estimator::Cut(ID3D11DeviceContext* context, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* previous, float white)
{
    const FrameConstants constants{ white };
    context->UpdateSubresource(frame.get(), 0, nullptr, &constants, 0, 0);
    Dispatch(context, kReduce, picture.constants.get(), { current, previous }, { picture.lumaTarget.get(), nullptr, keysTarget.get() },
             (picture.width + 15) / 16, (picture.height + 15) / 16);
    Dispatch(context, kResolve, picture.constants.get(), {}, { nullptr, nullptr, keysTarget.get() }, 1, 1);
}

inline void Estimator::Estimate(ID3D11DeviceContext* context, ID3D11ShaderResourceView* current, ID3D11ShaderResourceView* previous, float white)
{
    const FrameConstants constants{ white };
    context->UpdateSubresource(frame.get(), 0, nullptr, &constants, 0, 0);
    const auto groups = [](UINT size, UINT group) { return (size + group - 1) / group; };
    Dispatch(context, kReduce, picture.constants.get(), { current, previous }, { picture.lumaTarget.get(), nullptr, keysTarget.get() },
             groups(picture.width, 16), groups(picture.height, 16));
    for (size_t i = 0; i < levels.size(); ++i)
        Dispatch(context, kDownsample, levels[i].constants.get(), { nullptr, nullptr, i ? levels[i - 1].lumaView.get() : picture.lumaView.get() },
                 { levels[i].lumaTarget.get() }, groups(levels[i].width, 16), groups(levels[i].height, 16));
    Dispatch(context, kResolve, picture.constants.get(), {}, { nullptr, nullptr, keysTarget.get() }, 1, 1);
    for (size_t i = levels.size(); i-- > finest;)
    {
        const Level& level = levels[i];
        ID3D11ShaderResourceView* coarser = i + 1 < levels.size() ? levels[i + 1].motionView.get() : nullptr;
        if (i > finest)
            Dispatch(context, kSearch, level.constants.get(), { nullptr, nullptr, level.lumaView.get(), coarser, motionView.get() },
                     { nullptr, level.motionTarget.get() }, groups(level.width, 8), groups(level.height, 8));
        else
            Dispatch(context, kRefine, level.constants.get(), { nullptr, nullptr, nullptr, coarser, motionView.get(), picture.lumaView.get() },
                     { nullptr, level.motionTarget.get(), keysTarget.get(), stillTarget.get() }, groups(level.width, 8), groups(level.height, 8));
    }
    Dispatch(context, kFinish, picture.constants.get(), { current, previous, nullptr, levels[finest].motionView.get() },
             { nullptr, nullptr, keysTarget.get(), nullptr, motionTarget.get(), confidenceTarget.get() }, groups(picture.width, 16),
             groups(picture.height, 16));
}

inline void Estimator::Dispatch(ID3D11DeviceContext* context, Shader shader, ID3D11Buffer* constants, std::array<ID3D11ShaderResourceView*, 6> views,
                                std::array<ID3D11UnorderedAccessView*, 6> targets, UINT x, UINT y)
{
    ID3D11Buffer* buffers[] = { constants, frame.get() };
    ID3D11SamplerState* samplers[] = { linear.get() };
    context->CSSetShader(shaders[shader].get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 2, buffers);
    context->CSSetSamplers(0, 1, samplers);
    context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(targets.size()), targets.data(), nullptr);
    context->Dispatch(x, y, 1);
    views.fill(nullptr);
    targets.fill(nullptr);
    context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(targets.size()), targets.data(), nullptr);
}
} // namespace motion
