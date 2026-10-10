#include "capture.h"
#include "addon.h"
#include "depth/depth.h"
#include "hdr.h"
#include "log.h"
#include "menu.h"
#include "overlay.h"
#include "state.h"

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <utility>

using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace
{
constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";
// Set when Windows does not allow capture without its border, so capture keeps it instead of failing to start.
std::atomic<bool> borderRequired = false;
std::atomic<bool> captureIdle = false;

winrt::com_ptr<ID3D11RenderTargetView> scaleTarget;

// Draws a texture over the whole target with one triangle. tonemap does it for an HDR frame.
constexpr char kScaleShader[] = R"(
Texture2D Frame : register(t0);
SamplerState Smooth : register(s0);
cbuffer Hdr : register(b0)
{
    float SdrWhite; // in scRGB, where 1 is 80 nits
};
struct Corner
{
    float4 position : SV_Position;
    float2 at : TEXCOORD;
};

Corner vs(uint id : SV_VertexID)
{
    Corner corner;
    corner.at = float2((id << 1) & 2, id & 2);
    corner.position = float4(corner.at * float2(2, -2) + float2(-1, 1), 0, 1);
    return corner;
}

float4 ps(Corner corner) : SV_Target
{
    return Frame.Sample(Smooth, corner.at);
}

// Turns linear scRGB back into the sRGB an SDR frame has, with SDR white at 1.
float4 tonemap(Corner corner) : SV_Target
{
    float3 color = saturate(Frame.Sample(Smooth, corner.at).rgb / SdrWhite);
    return float4(color <= 0.0031308 ? color * 12.92 : 1.055 * pow(color, 1 / 2.4) - 0.055, 1);
}
)";

// What drawing the frame at another size or from HDR takes, made on first use and again after the device was lost.
struct Scaler
{
    winrt::com_ptr<ID3D11VertexShader> vertexShader;
    winrt::com_ptr<ID3D11PixelShader> pixelShader;
    winrt::com_ptr<ID3D11PixelShader> tonemapShader;
    winrt::com_ptr<ID3D11Buffer> sdrWhite;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    winrt::com_ptr<ID3D11Texture2D> frame;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    std::optional<int64_t> lastInput;
};
Scaler scaler;

// Draws the frame into a back buffer of another size, or an HDR frame as SDR. Capture's textures are not promised to
// be readable by shaders, so the frame is copied into one that is.
void DrawFrame(ID3D11Texture2D* frame, const D3D11_TEXTURE2D_DESC& size, ID3D11Texture2D* backBuffer, UINT width, UINT height, int64_t frameTimestamp)
{
    if (!scaler.sampler)
    {
        winrt::com_ptr<ID3DBlob> vertex, pixel, tonemap;
        winrt::check_hresult(D3DCompile(kScaleShader, sizeof(kScaleShader) - 1, "scale", nullptr, nullptr, "vs", "vs_4_0", 0, 0, vertex.put(), nullptr));
        winrt::check_hresult(D3DCompile(kScaleShader, sizeof(kScaleShader) - 1, "scale", nullptr, nullptr, "ps", "ps_4_0", 0, 0, pixel.put(), nullptr));
        winrt::check_hresult(D3DCompile(kScaleShader, sizeof(kScaleShader) - 1, "scale", nullptr, nullptr, "tonemap", "ps_4_0", 0, 0, tonemap.put(), nullptr));
        scaler = {};
        winrt::check_hresult(g.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, scaler.vertexShader.put()));
        winrt::check_hresult(g.device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, scaler.pixelShader.put()));
        winrt::check_hresult(g.device->CreatePixelShader(tonemap->GetBufferPointer(), tonemap->GetBufferSize(), nullptr, scaler.tonemapShader.put()));
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 16;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        winrt::check_hresult(g.device->CreateBuffer(&constants, nullptr, scaler.sdrWhite.put()));
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        winrt::check_hresult(g.device->CreateSamplerState(&desc, scaler.sampler.put()));
    }

    D3D11_TEXTURE2D_DESC copy{};
    if (scaler.frame)
        scaler.frame->GetDesc(&copy);
    if (copy.Width != size.Width || copy.Height != size.Height || copy.Format != size.Format)
    {
        copy = size;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        copy.SampleDesc = { 1, 0 };
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = 0;
        scaler.view = nullptr;
        scaler.frame = nullptr;
        winrt::check_hresult(g.device->CreateTexture2D(&copy, nullptr, scaler.frame.put()));
        winrt::check_hresult(g.device->CreateShaderResourceView(scaler.frame.get(), nullptr, scaler.view.put()));
        scaler.lastInput.reset();
    }
    if (!scaler.lastInput || *scaler.lastInput != frameTimestamp)
    {
        g.context->CopyResource(scaler.frame.get(), frame);
        scaler.lastInput = frameTimestamp;
    }
    if (g.hdrWhiteLevel)
    {
        const float sdrWhite[4] = { *g.hdrWhiteLevel / 80 };
        g.context->UpdateSubresource(scaler.sdrWhite.get(), 0, nullptr, sdrWhite, 0, 0);
    }

    if (!scaleTarget)
        winrt::check_hresult(g.device->CreateRenderTargetView(backBuffer, nullptr, scaleTarget.put()));
    const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    ID3D11RenderTargetView* targets[] = { scaleTarget.get() };
    ID3D11ShaderResourceView* views[] = { scaler.view.get() };
    ID3D11SamplerState* samplers[] = { scaler.sampler.get() };
    ID3D11Buffer* constants[] = { scaler.sdrWhite.get() };
    g.context->IASetInputLayout(nullptr);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(scaler.vertexShader.get(), nullptr, 0);
    g.context->PSSetShader(g.hdrWhiteLevel ? scaler.tonemapShader.get() : scaler.pixelShader.get(), nullptr, 0);
    g.context->PSSetShaderResources(0, 1, views);
    g.context->PSSetSamplers(0, 1, samplers);
    g.context->PSSetConstantBuffers(0, 1, constants);
    g.context->RSSetState(nullptr);
    g.context->RSSetViewports(1, &viewport);
    g.context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g.context->OMSetDepthStencilState(nullptr, 0);
    g.context->OMSetRenderTargets(1, targets, nullptr);
    g.context->Draw(3, 0);
    // Neither stays bound: the back buffer must be free to be resized, and the copy to be written again.
    views[0] = nullptr;
    g.context->PSSetShaderResources(0, 1, views);
    g.context->OMSetRenderTargets(0, nullptr, nullptr);
}
void CloseCapture()
{
    g.frameArrived.revoke();
    {
        const std::lock_guard lock(g.frameMutex);
        g.takingFrames = false;
        g.arrivedFrame = nullptr;
    }
    g.latestFrame = nullptr;
    scaler.lastInput.reset();
    ResetDepthInput();
    if (g.session)
        g.session.Close();
    g.session = nullptr;
    if (g.pool)
        g.pool.Close();
    g.pool = nullptr;
}
} // namespace

void CreateDevice()
{
    Log(LogLevel::Info, L"Creating a D3D11 graphics device.");
    // Made in locals, so a failure leaves no half-made device behind.
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature{};
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, device.put(), &feature, context.put()));

    // The capture pool uses the device from its own worker threads.
    device.as<ID3D11Multithread>()->SetMultithreadProtected(TRUE);

    auto dxgiDevice = device.as<IDXGIDevice1>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())))
    {
        DXGI_ADAPTER_DESC description{};
        adapter->GetDesc(&description);
        LARGE_INTEGER driver{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        Log(LogLevel::Info, L"Graphics device: %ls, vendor=0x%04X, device=0x%04X, luid=%08lX:%08lX, "
                           L"vram=%zu MB, shared=%zu MB, feature=0x%04X, driver=%u.%u.%u.%u.",
            description.Description, description.VendorId, description.DeviceId, static_cast<DWORD>(description.AdapterLuid.HighPart),
            description.AdapterLuid.LowPart, description.DedicatedVideoMemory / (1024 * 1024), description.SharedSystemMemory / (1024 * 1024),
            feature, HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart));
    }
    dxgiDevice->SetMaximumFrameLatency(1);

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    g.captureDevice = inspectable.as<IDirect3DDevice>();
    g.device = std::move(device);
    g.context = std::move(context);
}

bool DeviceLost(HRESULT error)
{
    return error == DXGI_ERROR_DEVICE_REMOVED || error == DXGI_ERROR_DEVICE_RESET || error == DXGI_ERROR_DEVICE_HUNG ||
           (g.device && FAILED(g.device->GetDeviceRemovedReason()));
}

void ReleaseDevice()
{
    Log(LogLevel::Info, L"Releasing capture, swapchain, depth resources and graphics device.");
    StopCapture();
    scaleTarget = nullptr;
    g.swapchain = nullptr;
    scaler = {};
    ReleaseDepthDevice();
    g.captureDevice = nullptr;
    if (g.context)
    {
        g.context->ClearState();
        g.context->Flush();
    }
    g.context = nullptr;
    g.device = nullptr;
}

void RequestBorderlessCapture()
{
    // Kept local, so they cannot clash with the global AsyncStatus of the Windows SDK's C headers.
    using winrt::Windows::Foundation::AsyncStatus;
    using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
    if (!ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired"))
    {
        Log(LogLevel::Info, L"Borderless capture is unavailable on this Windows version.");
        return;
    }
    try
    {
        // Windows answers in the background. Programs that are not packaged are allowed without a prompt.
        GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless)
            .Completed([](const auto& request, AsyncStatus status) {
                if (status == AsyncStatus::Completed && request.GetResults() == AppCapabilityAccessStatus::Allowed)
                {
                    Log(LogLevel::Info, L"Borderless capture permission granted.");
                    return;
                }
                borderRequired = true;
                Log(LogLevel::Info, L"Windows did not allow capture without a border, so it may draw one around the game.");
            });
    }
    catch (const winrt::hresult_error& e)
    {
        borderRequired = true;
        Log(LogLevel::Info, L"Could not ask to capture without a border: %ls (0x%08X)", e.message().c_str(), static_cast<unsigned>(e.code()));
    }
}

void StartCapture(HWND target)
{
    const bool resumed = captureIdle.exchange(false);
    ResetDepthInput();
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(target, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));

    g.hdrWhiteLevel = HdrWhiteLevel(target);
    g.poolSize = item.Size();
    Log(LogLevel::Info, L"Creating capture pool: hwnd=%p, size=%dx%d, buffers=%d, format=%d, SDR_white=%.1f nits, border_required=%d.",
        target, g.poolSize.Width, g.poolSize.Height, kFrameBuffers, static_cast<int>(g.hdrWhiteLevel ? kHdrPixelFormat : kPixelFormat),
        g.hdrWhiteLevel.value_or(80), borderRequired.load());
    g.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(g.captureDevice, g.hdrWhiteLevel ? kHdrPixelFormat : kPixelFormat, kFrameBuffers,
                                                            g.poolSize);
    g.capturedFrames.store(0, std::memory_order_relaxed);
    g.frameStatistics.Reset(FrameStatistics::Clock::now(), 0);
    {
        const std::lock_guard lock(g.frameMutex);
        g.takingFrames = true;
    }
    g.frameArrived = g.pool.FrameArrived(winrt::auto_revoke, [](const Direct3D11CaptureFramePool& pool, auto&&) {
        InitThreadLog();
        try
        {
            while (Direct3D11CaptureFrame frame = pool.TryGetNextFrame())
            {
                g.capturedFrames.fetch_add(1, std::memory_order_relaxed);
                // The frame it replaces goes back to the pool.
                const std::lock_guard lock(g.frameMutex);
                if (g.takingFrames)
                    g.arrivedFrame = std::move(frame);
            }
        }
        catch (const winrt::hresult_error& e)
        {
            bool taking = false;
            {
                const std::lock_guard lock(g.frameMutex);
                taking = g.takingFrames;
            }
            // Closing the pool while a callback finishes is expected.
            if (taking)
                Log(LogLevel::Error, L"Capture worker could not read a frame: %ls (0x%08X).", e.message().c_str(), static_cast<unsigned>(e.code()));
        }
        if (!captureIdle)
            SetEvent(g.frameEvent);
    });
    g.session = g.pool.CreateCaptureSession(item);

    // The real cursor is already drawn on top of the overlay.
    g.session.IsCursorCaptureEnabled(false);
    // RequestBorderlessCapture asked for this at startup.
    if (!borderRequired && ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired"))
        g.session.IsBorderRequired(false);
    // Without this, capture can be capped at 60 FPS.
    if (ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval"))
        g.session.MinUpdateInterval(std::chrono::milliseconds(1));

    g.session.StartCapture();
    Log(LogLevel::Info, L"Capture session started: hwnd=%p, resumed=%d, minimum_interval_supported=%d.", target, resumed,
        ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval"));
    g.target = target;
    if (!resumed)
    {
        Log(LogLevel::Info, L"Capturing %ls (%dx%d%ls)", g.activeGame->name.c_str(), g.poolSize.Width, g.poolSize.Height,
            g.hdrWhiteLevel ? L", HDR" : L"");
        ShowStartHint();
    }
}

void StopCapture()
{
    if (g.target)
        Log(LogLevel::Info, L"Stopping capture: hwnd=%p, captured_frames=%llu.", g.target, g.capturedFrames.load(std::memory_order_relaxed));
    SetEditMode(false);
    CloseCapture();
    captureIdle = false;
    g.target = nullptr;
    g.activeGame.reset();
    SetPerformanceGame({});
    g.frameStatistics.Reset(FrameStatistics::Clock::now(), g.capturedFrames.load(std::memory_order_relaxed));
}

void SetCaptureIdle(bool idle)
{
    if (!g.target || captureIdle == idle)
        return;
    Log(LogLevel::Info, L"Capture idle=%d, hwnd=%p.", idle, g.target);
    if (ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval"))
    {
        captureIdle = idle;
        g.session.MinUpdateInterval(std::chrono::milliseconds(idle ? 200 : 1));
    }
    else if (idle)
    {
        captureIdle = true;
        CloseCapture();
    }
    else
        StartCapture(g.target);
}

void PresentLatestFrame()
{
    const auto started = FrameStatistics::Clock::now();
    const int64_t frameTimestamp = g.latestFrame.SystemRelativeTime().count();
    winrt::com_ptr<ID3D11Texture2D> surface;
    auto access = g.latestFrame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), surface.put_void()));
    D3D11_TEXTURE2D_DESC size{};
    surface->GetDesc(&size);
    // Effects run at the swapchain's size. Windows stretches a smaller swapchain over the overlay's window.
    UINT width = std::max(size.Width * EffectResolution() / 100, 1u);
    UINT height = std::max(size.Height * EffectResolution() / 100, 1u);

    if (!g.swapchain)
    {
        Log(LogLevel::Info, L"Creating swapchain: %ux%u, source=%ux%u, source_format=%u, output_format=%u, buffers=2, effect_resolution=%d%%.",
            width, height, size.Width, size.Height, size.Format, DXGI_FORMAT_B8G8R8A8_UNORM, EffectResolution());
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(g.device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        winrt::check_hresult(factory->CreateSwapChainForHwnd(g.device.get(), g.overlay, &desc, nullptr, nullptr, g.swapchain.put()));
        factory->MakeWindowAssociation(g.overlay, DXGI_MWA_NO_ALT_ENTER);
    }
    else
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        g.swapchain->GetDesc1(&desc);
        // Resizing makes ReShade wait for the effects it is compiling, which would freeze the host, so the frame is
        // drawn at the old size until it is done.
        if (ReShadeCompilingEffects())
        {
            width = desc.Width;
            height = desc.Height;
        }
        else if (desc.Width != width || desc.Height != height)
        {
            Log(LogLevel::Info, L"Resizing swapchain: %ux%u -> %ux%u, source=%ux%u.", desc.Width, desc.Height, width, height, size.Width, size.Height);
            scaleTarget = nullptr;
            winrt::check_hresult(g.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
        }
    }

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    winrt::check_hresult(g.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
    if (width == size.Width && height == size.Height && !g.hdrWhiteLevel)
        g.context->CopyResource(backBuffer.get(), surface.get());
    else
        DrawFrame(surface.get(), size, backBuffer.get(), width, height, frameTimestamp);
    // Depth uses the same scaled SDR input as effects, including when the capture itself is SDR.
    UpdateDepth(backBuffer.get(), frameTimestamp);
    const HRESULT presented = g.swapchain->Present(0, 0);
    if (FAILED(presented))
        Log(LogLevel::Error, L"Swapchain Present failed: 0x%08X, device_reason=0x%08X, output=%ux%u, source=%ux%u, source_format=%u.",
            static_cast<unsigned>(presented), static_cast<unsigned>(g.device->GetDeviceRemovedReason()), width, height, size.Width, size.Height, size.Format);
    winrt::check_hresult(presented);
    const auto finished = FrameStatistics::Clock::now();
    g.frameStatistics.RecordPresent(frameTimestamp, std::chrono::duration<double, std::milli>(finished - started).count());
    g.frameStatistics.Update(finished, g.capturedFrames.load(std::memory_order_relaxed));
}
