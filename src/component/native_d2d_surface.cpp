#include "native_d2d_surface.h"
#include "device_failure.h"

#include "render_device_hub.h"
#include "display_environment.h"
#include "telemetry_span.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace nekorox::component {
namespace {

constexpr UINT kSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
std::atomic<std::uint64_t> gNextSurfaceLifetime{1};

} // namespace

D2D1_SIZE_U NativeD2DSurface::clientPixelSize() const noexcept
{
    RECT client{};
    if (window_ != nullptr) GetClientRect(window_, &client);
    return D2D1::SizeU(
        static_cast<UINT32>(std::max(1L, client.right - client.left)),
        static_cast<UINT32>(std::max(1L, client.bottom - client.top)));
}

bool NativeD2DSurface::ensure(HWND window, float dpi) noexcept
{
    TelemetrySpan span(TelemetrySurface::graphics,200,reinterpret_cast<std::uintptr_t>(window));
    if (window == nullptr || !IsWindow(window)) return false;
    auto& hub = RenderDeviceHub::instance();
    const auto wantedTopology =
        DisplayEnvironmentService::instance().topologyGeneration();
    if (active() && window_ == window &&
        SUCCEEDED(d3dDevice_->GetDeviceRemovedReason()) &&
        hub.isDeviceCompatibleWithWindow(d3dDevice_.Get(), window)) {
        bool hardwareRecovered = false;
        if (backend_ == SurfaceBackend::warpDxgi) {
            const auto recovery = hub.recoveryEpoch();
            const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            if (observedRecoveryEpoch_ != recovery ||
                    recoveryMonitor_ != monitor ||
                    topologyGeneration_ != wantedTopology) {
                // WARP remains compatible with every monitor even after the
                // recovery worker publishes hardware. Probe only on change,
                // and keep the working target if hardware is still absent.
                DeviceLease preferred;
                hardwareRecovered = hub.tryAcquireExisting(window, preferred) &&
                    preferred.backend == SurfaceBackend::hardwareDxgi;
                observedRecoveryEpoch_ = recovery;
                recoveryMonitor_ = monitor;
            }
        }
        if (!hardwareRecovered) {
            topologyGeneration_ = wantedTopology;
            if (std::abs(dpi_ - dpi) >= 0.5f) {
                dpi_ = std::max(1.0f, dpi);
                context_->SetTarget(nullptr);
                targetBitmap_.Reset();
                if (!createTargetBitmap()) {
                    reset();
                    return false;
                }
                ++resourceEpoch_;
                ++sizeRevision_;
            }
            return true;
        }
    }

    reset();
    DeviceLease lease;
    if (!hub.tryAcquireExisting(window, lease)) return false;

    window_ = window;
    lifetimeToken_ = gNextSurfaceLifetime.fetch_add(
        1, std::memory_order_relaxed);
    dpi_ = std::max(1.0f, dpi);
    pixelSize_ = clientPixelSize();
    backend_ = lease.backend;
    epoch_ = lease.epoch;
    observedRecoveryEpoch_ = lease.epoch;
    recoveryMonitor_ = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    topologyGeneration_ = lease.topologyGeneration;
    d3dDevice_ = lease.device;
    immediateContext_ = lease.immediateContext;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(d3dDevice_.As(&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf())) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
        reset();
        return false;
    }

    auto* d2dFactory = hub.d2dFactory();
    if (d2dFactory == nullptr ||
        FAILED(d2dFactory->CreateDevice(
            dxgiDevice.Get(), d2dDevice_.ReleaseAndGetAddressOf())) ||
        FAILED(d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            context_.ReleaseAndGetAddressOf()))) {
        reset();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = pixelSize_.width;
    description.Height = pixelSize_.height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Every GODENplayer panel owns and paints its complete client area.  A
    // premultiplied composition swap-chain briefly exposes a transparent,
    // uninitialised back-buffer after ResizeBuffers; DWM then shows the host
    // (often white or grey) until the first present.  Treat the panel as the
    // opaque surface it is so resize/fullscreen transitions retain a valid
    // frame instead of flashing the parent through it.
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = kSwapChainFlags;
    if (FAILED(factory->CreateSwapChainForComposition(d3dDevice_.Get(),
            &description, nullptr,
            swapChain_.ReleaseAndGetAddressOf())) ||
        FAILED(DCompositionCreateDevice(dxgiDevice.Get(),
            __uuidof(IDCompositionDevice), reinterpret_cast<void**>(
                compositionDevice_.ReleaseAndGetAddressOf()))) ||
        FAILED(compositionDevice_->CreateTargetForHwnd(window_, FALSE,
            compositionTarget_.ReleaseAndGetAddressOf())) ||
        FAILED(compositionDevice_->CreateVisual(
            compositionVisual_.ReleaseAndGetAddressOf())) ||
        FAILED(compositionVisual_->SetContent(swapChain_.Get())) ||
        FAILED(compositionTarget_->SetRoot(compositionVisual_.Get())) ||
        FAILED(compositionDevice_->Commit())) {
        reset();
        return false;
    }
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain_.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(1);
        readiness_ = swapChain2->GetFrameLatencyWaitableObject();
    }
    if (!createTargetBitmap()) {
        reset();
        return false;
    }
    ++swapchainEpoch_;
    ++resourceEpoch_;
    ++sizeRevision_;
    context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

bool NativeD2DSurface::createTargetBitmap() noexcept
{
    if (!swapChain_ || !context_) return false;
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, __uuidof(IDXGISurface),
            reinterpret_cast<void**>(surface.ReleaseAndGetAddressOf())))) {
        return false;
    }
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        dpi_, dpi_);
    if (FAILED(context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
            targetBitmap_.ReleaseAndGetAddressOf()))) {
        return false;
    }
    context_->SetTarget(targetBitmap_.Get());
    context_->SetDpi(dpi_, dpi_);
    return true;
}

bool NativeD2DSurface::resize() noexcept
{
    TelemetrySpan span(TelemetrySurface::graphics,201,reinterpret_cast<std::uintptr_t>(window_));
    if (!active()) return false;
    const auto wanted = clientPixelSize();
    if (wanted.width == pixelSize_.width && wanted.height == pixelSize_.height)
        return true;
    context_->SetTarget(nullptr);
    targetBitmap_.Reset();
    const auto result = swapChain_->ResizeBuffers(
        0, wanted.width, wanted.height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (FAILED(result)) {
        if (confirmedDeviceLost(d3dDevice_.Get(), result))
            RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
        reset();
        return false;
    }
    pixelSize_ = wanted;
    if (!createTargetBitmap()) {
        reset();
        return false;
    }
    // ResizeBuffers changes the buffers, not the identity of the swapchain or
    // its frame-latency waitable object. Keep the readiness binding stable;
    // sizeRevision_/resourceEpoch_ invalidate only size-dependent scenes.
    ++resourceEpoch_;
    ++sizeRevision_;
    return true;
}

FrameResult NativeD2DSurface::present(HRESULT drawResult) noexcept
{
    TelemetrySpan span(TelemetrySurface::graphics,202,reinterpret_cast<std::uintptr_t>(window_));
    lastDrawHr_ = drawResult;
    if (FAILED(drawResult)) {
        lastPresentHr_ = E_PENDING;
        if (confirmedDeviceLost(d3dDevice_.Get(), drawResult)) {
            RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
            reset();
            return FrameResult::deviceLost;
        }
        if (drawResult == D2DERR_RECREATE_TARGET) {
            // The D3D lease is healthy. Retire only this target/swapchain;
            // other panels sharing the adapter remain untouched.
            reset();
        }
        return FrameResult::failed;
    }
    if (!swapChain_) {
        lastPresentHr_ = E_HANDLE;
        return FrameResult::failed;
    }
    DXGI_PRESENT_PARAMETERS parameters{};
    // FrameClock supplies the pacing, and DWM synchronizes composition to the
    // display. A second vblank wait here serializes every child panel on the
    // input thread; some drivers sleep inside Present even with DO_NOT_WAIT.
    const auto result = swapChain_->Present1(
        0, DXGI_PRESENT_DO_NOT_WAIT, &parameters);
    lastPresentHr_ = result;
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) return FrameResult::notReady;
    if (result == DXGI_STATUS_OCCLUDED) return FrameResult::occluded;
    if (confirmedDeviceLost(d3dDevice_.Get(), result)) {
        RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
        reset();
        return FrameResult::deviceLost;
    }
    return SUCCEEDED(result) ? FrameResult::accepted : FrameResult::failed;
}

void NativeD2DSurface::reset() noexcept
{
    if (context_) context_->SetTarget(nullptr);
    targetBitmap_.Reset();
    if (compositionTarget_) (void)compositionTarget_->SetRoot(nullptr);
    if (compositionVisual_) (void)compositionVisual_->SetContent(nullptr);
    if (compositionDevice_) (void)compositionDevice_->Commit();
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    swapChain_.Reset();
    context_.Reset();
    d2dDevice_.Reset();
    d3dDevice_.Reset();
    immediateContext_.Reset();
    if (readiness_ != nullptr) CloseHandle(readiness_);
    readiness_ = nullptr;
    window_ = nullptr;
    dpi_ = 96.0f;
    pixelSize_ = {};
    backend_ = SurfaceBackend::gdiFallback;
    epoch_ = 0;
    observedRecoveryEpoch_ = 0;
    recoveryMonitor_ = nullptr;
    topologyGeneration_ = 0;
    lastPresentHr_ = E_PENDING;
    lastDrawHr_ = E_PENDING;
}

} // namespace nekorox::component
