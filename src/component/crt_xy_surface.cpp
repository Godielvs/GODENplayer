#include "crt_xy_surface.h"

#include "display_environment.h"

#include "crt_beam_ps.h"
#include "crt_beam_vs.h"
#include "crt_composite_ps.h"
#include "crt_decay_ps.h"
#include "crt_fullscreen_vs.h"

#include "frame_clock.h"
#include "interaction_latency_guard.h"
#include "render_device_hub.h"

#include "nekorox/presentation_timing.h"
#include "nekorox/visual_sample_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nekorox::component {
#if defined(NEKOROX_CRT_SURFACE_TEST)
void crtTestBeforeLatencyClose(CrtXySurface& surface);
void crtTestWaitDrained(CrtXySurface& surface);
void crtTestCallbackBeforePost();
bool crtTestFailComposition();
bool crtTestFailSwapChain();
#endif
namespace {

bool failedDevice(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_HUNG ||
        result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

} // namespace

CrtXySurface::CrtXySurface()
{
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    }
    targetFrameTicks_ = std::max<std::uint64_t>(1, qpcFrequency_ / 60U);
    framePaceTimer_ = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (framePaceTimer_ == nullptr) {
        framePaceTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    frameWait_ = CreateThreadpoolWait(
        &CrtXySurface::frameWaitCallback, this, nullptr);
}

CrtXySurface::~CrtXySurface()
{
    shutdown();
    if (frameWait_ != nullptr) {
        CloseThreadpoolWait(frameWait_);
        frameWait_ = nullptr;
    }
    if (framePaceTimer_ != nullptr) {
        CloseHandle(framePaceTimer_);
        framePaceTimer_ = nullptr;
    }
}

void CrtXySurface::attach(HWND parent, UINT frameReadyMessage)
{
    if (parent_ == parent && frameReadyMessage_ == frameReadyMessage) return;
    shutdown();
    parent_ = parent;
    frameReadyMessage_ = frameReadyMessage;
    updateTargetFramePeriod();
}

void CrtXySurface::displayEnvironmentChanged(bool forceAdapterCheck) noexcept
{
    lastRefreshQueryQpc_ = 0;
    lastTopologyGeneration_ = 0;
    if (forceAdapterCheck) adapterCheckRequired_ = true;
    nextPresentQpc_ = 0;
    if (active_ && parent_ != nullptr && frameReadyMessage_ != 0) {
        PostMessageW(
            parent_, frameReadyMessage_, generation(), 0);
    }
}

void CrtXySurface::setViewport(const RECT& pixelRect)
{
    const auto width = std::max<LONG>(0, pixelRect.right - pixelRect.left);
    const auto height = std::max<LONG>(0, pixelRect.bottom - pixelRect.top);
    const bool sizeChanged =
        outputWidth_ != static_cast<std::uint32_t>(width) ||
        outputHeight_ != static_cast<std::uint32_t>(height);
    const bool positionChanged =
        viewport_.left != pixelRect.left || viewport_.top != pixelRect.top;
    if (sizeChanged || positionChanged) idleSettled_ = false;
    viewport_ = pixelRect;
    outputWidth_ = static_cast<std::uint32_t>(width);
    outputHeight_ = static_cast<std::uint32_t>(height);
    stats_.outputWidth = outputWidth_;
    stats_.outputHeight = outputHeight_;

    if (outputWidth_ == 0 || outputHeight_ == 0) {
        disarmFrameWait(true);
        resourceRetryQpc_ = 0;
        if (compositionVisual_) {
            compositionVisual_->SetContent(nullptr);
            compositionDevice_->Commit();
        }
        releaseSwapChainResources();
        return;
    }

    if (!device_) {
        if (active_) {
            if (ensureResources()) armFrameWait();
            else retryResourcesLater();
        }
        return;
    }

    if (sizeChanged) {
        // ResizeBuffers is a buffer revision on the same swapchain. Drain the
        // callback while releasing backbuffer references, but keep the
        // frame-latency handle for the lifetime of the swapchain.
        suspendSizeWait();
        releaseOutputResources();
        if (swapChain_) {
            const auto result = swapChain_->ResizeBuffers(
                2,
                outputWidth_,
                outputHeight_,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            if (FAILED(result)) {
                if (failedDevice(result)) {
                    handleDeviceLoss(result);
                } else {
                    releaseSwapChainResources();
                    if (active_) {
                        if (ensureResources()) armFrameWait();
                        else retryResourcesLater();
                    }
                }
                return;
            }
        }
        if (!resizeResources()) {
            if (device_) {
                const auto reason = device_->GetDeviceRemovedReason();
                if (failedDevice(reason)) {
                    handleDeviceLoss(reason);
                    return;
                }
            }
            retryResourcesLater();
            return;
        }
        // Same simulation survives output resize; no phosphor clear.
    }
    if ((sizeChanged || positionChanged) && !updateComposition()) {
        releaseDeviceResources();
        if (active_) {
            if (ensureResources()) armFrameWait();
            else retryResourcesLater();
        }
        return;
    }
    if (active_ && (!idleSettled_ || pendingCount_ != 0)) armFrameWait();
}

void CrtXySurface::setActive(bool active)
{
    if(active_==active){
        if(active_&&(!idleSettled_||pendingCount_!=0))armFrameWait();
        return;
    }
    active_=active;
    lifecycleGeneration_.fetch_add(1,std::memory_order_acq_rel);
    if(!active_){
        disarmFrameWait(true);
        resourceRetryQpc_=0;
        pendingCount_=0;nextPresentQpc_=0;lastPresentQpc_=0;
        if(compositionVisual_){compositionVisual_->SetContent(nullptr);compositionDevice_->Commit();}
        // A real swap-chain retirement safely retires its wait, but the two
        // phosphor textures belong to the simulation, not to this HWND epoch.
        releaseSwapChainResources();
        return;
    }
    if(!ensureResources()){retryResourcesLater();return;}
    lastPresentQpc_=0;nextPresentQpc_=0;idleSettled_=false;
    if(!updateComposition()){
        releaseDeviceResources();
        if(!ensureResources()){retryResourcesLater();return;}
    }
    armFrameWait();
}

void CrtXySurface::setOrientation(CrtXyOrientation orientation) noexcept
{
    if (orientation_ == orientation) return;
    orientation_ = orientation;
    // The phosphor history is spatial. Keeping it across an orientation
    // change would briefly superimpose two unrelated coordinate systems.
    reset();
}

void CrtXySurface::reset() noexcept
{
    pendingCount_ = 0;
    clearPhosphor_ = true;
    idleSettled_ = false;
    estimatedPhosphorEnergy_ = 0.0f;
    lastSegmentQpc_ = 0;
    lastPresentQpc_ = 0;
    nextPresentQpc_ = 0;
    if (active_) armFrameWait();
}

void CrtXySurface::submit(
    std::span<const CrtBeamSegment> segments) noexcept
{
    if (!active_ || segments.empty()) return;
    const auto available = kMaxPendingSegments - pendingCount_;
    const auto accepted = std::min(available, segments.size());
    if (accepted > 0) {
        const auto submittedQpc = qpcNow();
        for (std::size_t index = 0; index < accepted; ++index) {
            pendingSegments_[pendingCount_ + index] = {
                segments[index],
                submittedQpc,
            };
        }
        pendingCount_ += accepted;
        lastSegmentQpc_ = submittedQpc;
        estimatedPhosphorEnergy_ = 1.0f;
        idleSettled_ = false;
        stats_.submittedSegments += accepted;
    }
    if (accepted < segments.size()) {
        stats_.droppedSegments += segments.size() - accepted;
    }
    if (accepted > 0) armFrameWait();
}

void CrtXySurface::present(
    std::uint32_t lifecycleGeneration,
    std::uint32_t armSerial)
{
    if (!active_ || lifecycleGeneration != generation()) return;
    if (armSerial != 0) {
        std::lock_guard<std::mutex> lock(frameWaitMutex_);
        if (!readyMessagePending_ ||
            deliveredGeneration_ != lifecycleGeneration ||
            deliveredSerial_ != armSerial) {
            return;
        }
        readyMessagePending_ = false;
        deliveredGeneration_ = 0;
        deliveredSerial_ = 0;
    }
    if (InteractionLatencyGuard::instance().shouldYieldRender()) {
        // The CRT is decorative; transport, caption drag and pointer input
        // win this pulse. A short one-shot avoids immediately reposting from
        // an already-signalled latency object while preserving all segments.
        if (!armPaceTimer(std::max<std::uint64_t>(1, qpcFrequency_ / 500U))) {
            armFrameWait();
        }
        return;
    }
    auto& telemetry = PerformanceTelemetry::instance();
    updateTargetFramePeriod();
    const auto resourceSpan = telemetry.beginSpan(
        TelemetrySurface::listenCrt, 1U);
    const auto resourcesReady = ensureResources();
    telemetry.endSpan(TelemetrySurface::listenCrt, 1U, resourceSpan);
    if (!resourcesReady) {
        retryResourcesLater();
        return;
    }
    const auto now = qpcNow();
    if (nextPresentQpc_ != 0 && now < nextPresentQpc_) {
        if (armPaceTimer(nextPresentQpc_ - now)) {
            ++stats_.pacedWaits;
            return;
        }
    }

    auto elapsed = kCrtNominalFrameSeconds;
    if (lastPresentQpc_ != 0 && now > lastPresentQpc_) {
        elapsed = static_cast<float>(
            static_cast<double>(now - lastPresentQpc_) /
            static_cast<double>(qpcFrequency_));
        elapsed = std::clamp(elapsed, 0.0f, 0.1f);
    }
    estimatedPhosphorEnergy_ *= std::exp(
        -elapsed / std::max(0.001f, kCrtTailTauSeconds));
    if (pendingCount_ != 0) estimatedPhosphorEnergy_ = 1.0f;
    lastPresentQpc_ = now;

    const auto renderStage = [&](std::uint64_t phase, auto&& action) {
        const auto pending = pendingCount_;
        const auto started = telemetry.beginSpan(
            TelemetrySurface::listenCrt, phase, pending);
        const auto succeeded = action();
        telemetry.endSpan(
            TelemetrySurface::listenCrt, phase, started, pending);
        return succeeded;
    };
    if (!renderStage(2U, [&] { return updateConstants(elapsed); }) ||
        !renderStage(3U, [&] { return renderPhosphor(); }) ||
        !renderStage(4U, [&] { return renderBeam(); }) ||
        !renderStage(5U, [&] { return renderComposite(); })) {
        if (device_) {
            const auto reason = device_->GetDeviceRemovedReason();
            if (failedDevice(reason)) {
                handleDeviceLoss(reason);
                return;
            }
        }
        armFrameWait();
        return;
    }

    const auto presentSpan = telemetry.beginSpan(
        TelemetrySurface::listenCrt, 6U);
    const auto result = swapChain_->Present(
        0, DXGI_PRESENT_DO_NOT_WAIT);
    telemetry.endSpan(
        TelemetrySurface::listenCrt, 6U, presentSpan,
        static_cast<std::uint32_t>(result));
    telemetry.present(
        TelemetrySurface::listenCrt,
        result,
        result == DXGI_ERROR_WAS_STILL_DRAWING);
    WindowFrameClock::reportPresentOutcome(
        result == DXGI_ERROR_WAS_STILL_DRAWING);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++stats_.skippedPresents;
    } else if (failedDevice(result)) {
        handleDeviceLoss(result);
        return;
    } else if (SUCCEEDED(result)) {
        ++stats_.presentedFrames;
    }
    if (nextPresentQpc_ == 0 ||
        now > nextPresentQpc_ + targetFrameTicks_ * 4U) {
        nextPresentQpc_ = now + targetFrameTicks_;
    } else {
        do {
            nextPresentQpc_ += targetFrameTicks_;
        } while (nextPresentQpc_ <= now);
    }
    const auto silentSeconds = lastSegmentQpc_ != 0 && now > lastSegmentQpc_
        ? static_cast<double>(now - lastSegmentQpc_) /
            static_cast<double>(qpcFrequency_)
        : std::numeric_limits<double>::infinity();
    if (pendingCount_ == 0 &&
        estimatedPhosphorEnergy_ < kCrtSettledEnergy &&
        silentSeconds >= kCrtMinimumSettleSeconds) {
        idleSettled_ = true;
        nextPresentQpc_ = 0;
        ++stats_.settledStops;
        return;
    }
    armFrameWait();
}

bool CrtXySurface::captureBgra(CrtSurfaceCapture& capture)
{
    capture = {};
    if (!active_ || outputWidth_ == 0 || outputHeight_ == 0) {
        return false;
    }
    if (!ensureResources()) {
        retryResourcesLater();
        return false;
    }

    const auto fail = [&]() {
        if (device_) {
            const auto reason = device_->GetDeviceRemovedReason();
            if (failedDevice(reason)) handleDeviceLoss(reason);
        }
        return false;
    };
    if (!updateConstants(kCrtNominalFrameSeconds)) return fail();

    D3D11_TEXTURE2D_DESC renderDescription{};
    renderDescription.Width = outputWidth_;
    renderDescription.Height = outputHeight_;
    renderDescription.MipLevels = 1;
    renderDescription.ArraySize = 1;
    renderDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    renderDescription.SampleDesc.Count = 1;
    renderDescription.Usage = D3D11_USAGE_DEFAULT;
    renderDescription.BindFlags = D3D11_BIND_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> renderTexture;
    auto result = device_->CreateTexture2D(
        &renderDescription,
        nullptr,
        renderTexture.ReleaseAndGetAddressOf());
    if (FAILED(result)) return fail();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
    result = device_->CreateRenderTargetView(
        renderTexture.Get(),
        nullptr,
        renderTarget.ReleaseAndGetAddressOf());
    if (FAILED(result) ||
        !renderCompositeTo(
            renderTarget.Get(), outputWidth_, outputHeight_)) {
        return fail();
    }

    auto stagingDescription = renderDescription;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
    result = device_->CreateTexture2D(
        &stagingDescription,
        nullptr,
        stagingTexture.ReleaseAndGetAddressOf());
    if (FAILED(result)) return fail();

    context_->CopyResource(stagingTexture.Get(), renderTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context_->Map(
        stagingTexture.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    if (FAILED(result)) return fail();

    bool copied = false;
    try {
        const auto rowBytes =
            static_cast<std::size_t>(outputWidth_) * 4U;
        capture.bgra.resize(
            rowBytes * static_cast<std::size_t>(outputHeight_));
        const auto* source =
            static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t row = 0; row < outputHeight_; ++row) {
            std::memcpy(
                capture.bgra.data() +
                    static_cast<std::size_t>(row) * rowBytes,
                source + static_cast<std::size_t>(row) * mapped.RowPitch,
                rowBytes);
        }
        capture.pixelRect = viewport_;
        capture.width = outputWidth_;
        capture.height = outputHeight_;
        copied = true;
    } catch (...) {
        capture = {};
    }
    context_->Unmap(stagingTexture.Get(), 0);

    // Keep the normal swap-chain target bound for the next paced present.
    if (outputTarget_) {
        context_->OMSetRenderTargets(
            1, outputTarget_.GetAddressOf(), nullptr);
    }
    return copied;
}

void CrtXySurface::shutdown()
{
    active_ = false;
    lifecycleGeneration_.fetch_add(1, std::memory_order_acq_rel);
    disarmFrameWait(true);
    pendingCount_ = 0;
    if (compositionVisual_) {
        compositionVisual_->SetContent(nullptr);
    }
    if (compositionTarget_) compositionTarget_->SetRoot(nullptr);
    if (compositionDevice_) compositionDevice_->Commit();
    releaseDeviceResources();
    parent_ = nullptr;
    frameReadyMessage_ = 0;
    viewport_ = {};
    outputWidth_ = 0;
    outputHeight_ = 0;
    renderWidth_ = 0;
    renderHeight_ = 0;
    compositionAttached_ = false;
    clearPhosphor_ = true;
    idleSettled_ = false;
    estimatedPhosphorEnergy_ = 0.0f;
    lastSegmentQpc_ = 0;
    lastPresentQpc_ = 0;
    nextPresentQpc_ = 0;
    lastRefreshQueryQpc_ = 0;
    resourceRetryQpc_ = 0;
    lastTopologyGeneration_ = 0;
}

VOID CALLBACK CrtXySurface::frameWaitCallback(
    PTP_CALLBACK_INSTANCE,
    void* context,
    PTP_WAIT,
    TP_WAIT_RESULT)
{
    if (context == nullptr) return;
    auto* surface = static_cast<CrtXySurface*>(context);
    HWND parent = nullptr;
    UINT message = 0;
    std::uint32_t generation = 0;
    std::uint32_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(surface->frameWaitMutex_);
        surface->frameWaitArmed_ = false;
        generation = surface->armedGeneration_;
        serial = surface->armedSerial_;
        if ((!surface->waitAdmissionOpen_ && !surface->preservingSizeWait_) ||
            generation != surface->generation() ||
            surface->readyMessagePending_) {
            return;
        }
        surface->readyMessagePending_ = true;
        surface->deliveredGeneration_ = generation;
        surface->deliveredSerial_ = serial;
        parent = surface->parent_;
        message = surface->frameReadyMessage_;
    }
#if defined(NEKOROX_CRT_SURFACE_TEST)
    crtTestCallbackBeforePost();
#endif
    if (parent != nullptr && message != 0) {
        if (PostMessageW(
            parent,
            message,
            static_cast<WPARAM>(generation),
            static_cast<LPARAM>(serial))) {
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(surface->frameWaitMutex_);
        if (surface->deliveredGeneration_ == generation &&
            surface->deliveredSerial_ == serial) {
            surface->readyMessagePending_ = false;
            surface->deliveredGeneration_ = 0;
            surface->deliveredSerial_ = 0;
        }
    }
}

bool CrtXySurface::ensureResources()
{
    if (!active_ || parent_ == nullptr ||
        outputWidth_ == 0 || outputHeight_ == 0) {
        return false;
    }
    // The owner may repeat setViewport/capture while a recovery is pending.
    // Those calls share the same retry deadline instead of probing at the
    // owner's presentation cadence or postponing recovery on every paint.
    if (resourceRetryQpc_ != 0 && qpcNow() < resourceRetryQpc_) return false;
    resourceRetryQpc_ = 0;
    const auto recoveryEpoch = RenderDeviceHub::instance().recoveryEpoch();
    const auto currentMonitor =
        MonitorFromWindow(parent_, MONITOR_DEFAULTTONEAREST);
    if (device_ &&
        (renderRecoveryEpoch_ != recoveryEpoch ||
         adapterCheckRequired_ || currentMonitor != renderMonitor_)) {
        if (renderRecoveryEpoch_ != recoveryEpoch ||
            !RenderDeviceHub::instance().isDeviceCompatibleWithWindow(
                device_.Get(), parent_)) {
            releaseDeviceResources();
        }
    }
    renderMonitor_ = currentMonitor;
    adapterCheckRequired_ = false;
    const auto deviceReady =
        device_ && context_ && compositionDevice_ &&
        compositionTarget_ && compositionVisual_ &&
        fullscreenVertexShader_ && beamVertexShader_ &&
        decayPixelShader_ && beamPixelShader_ &&
        compositePixelShader_ && beamInputLayout_ &&
        segmentBuffer_ && constantsBuffer_ &&
        linearSampler_ && additiveBlend_ &&
        opaqueBlend_ && rasterizer_;
    if (!deviceReady) {
        releaseDeviceResources();
        if (!createDeviceResources()) return false;
    }
    if (!swapChain_ && !createSwapChain()) return false;
    const auto sizeReady =
        outputTarget_ &&
        phosphorTextures_[0] && phosphorTextures_[1] &&
        phosphorTargets_[0] && phosphorTargets_[1] &&
        phosphorViews_[0] && phosphorViews_[1];
    if (!sizeReady) {
        releaseOutputResources();
        if (!resizeResources()) return false;
    }
    return true;
}

bool CrtXySurface::createDeviceResources()
{
    const auto fail = [&]() {
        releaseDeviceResources();
        return false;
    };
    bool softwareRasterizer = false;
    auto result = RenderDeviceHub::instance().acquire(
        parent_, device_, context_, softwareRasterizer)
        ? S_OK
        : E_FAIL;
    if (FAILED(result)) return fail();
    renderRecoveryEpoch_ = RenderDeviceHub::instance().recoveryEpoch();
    renderMonitor_ = MonitorFromWindow(
        parent_, MONITOR_DEFAULTTONEAREST);
    adapterCheckRequired_ = false;
    softwareRasterizer_ = softwareRasterizer;
    stats_.softwareRasterizer = softwareRasterizer_;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) return fail();
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1))) {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }
    result = DCompositionCreateDevice(
        dxgiDevice.Get(),
        __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(
            compositionDevice_.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return fail();
    result = compositionDevice_->CreateTargetForHwnd(
        parent_,
        TRUE,
        compositionTarget_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return fail();
    result = compositionDevice_->CreateVisual(
        compositionVisual_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return fail();
    result = compositionTarget_->SetRoot(compositionVisual_.Get());
    if (FAILED(result)) return fail();
    compositionAttached_ = true;

    if (!createShaders() || !createStates()) return fail();
    return true;
}

bool CrtXySurface::createSwapChain()
{
#if defined(NEKOROX_CRT_SURFACE_TEST)
    if (crtTestFailSwapChain()) return false;
#endif
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf())) ||
        FAILED(adapter->GetParent(
            __uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = outputWidth_;
    description.Height = outputHeight_;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    auto result = factory->CreateSwapChainForComposition(
        device_.Get(),
        &description,
        nullptr,
        swapChain.ReleaseAndGetAddressOf());
    if (FAILED(result) || FAILED(swapChain.As(&swapChain2))) return false;
    swapChain2->SetMaximumFrameLatency(1);
    const auto latencyHandle =
        swapChain2->GetFrameLatencyWaitableObject();
    if (latencyHandle == nullptr) return false;
    result = compositionVisual_->SetContent(swapChain2.Get());
    if (FAILED(result)) {
        CloseHandle(latencyHandle);
        return false;
    }
    swapChain_ = std::move(swapChain2);
    frameLatencyHandle_ = latencyHandle;
    if (!updateComposition()) {
        releaseSwapChainResources();
        return false;
    }
    return true;
}

bool CrtXySurface::createShaders()
{
    auto result = device_->CreateVertexShader(
        g_crtFullscreenVs,
        sizeof(g_crtFullscreenVs),
        nullptr,
        fullscreenVertexShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;
    result = device_->CreateVertexShader(
        g_crtBeamVs,
        sizeof(g_crtBeamVs),
        nullptr,
        beamVertexShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;
    result = device_->CreatePixelShader(
        g_crtDecayPs,
        sizeof(g_crtDecayPs),
        nullptr,
        decayPixelShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;
    result = device_->CreatePixelShader(
        g_crtBeamPs,
        sizeof(g_crtBeamPs),
        nullptr,
        beamPixelShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;
    result = device_->CreatePixelShader(
        g_crtCompositePs,
        sizeof(g_crtCompositePs),
        nullptr,
        compositePixelShader_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4> inputElements{{
        {"BEAM_START", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BEAM_END", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BEAM_CHARGE", 0, DXGI_FORMAT_R32_FLOAT, 0, 16,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"BEAM_AGE", 0, DXGI_FORMAT_R32_FLOAT, 0, 20,
            D3D11_INPUT_PER_INSTANCE_DATA, 1},
    }};
    result = device_->CreateInputLayout(
        inputElements.data(),
        static_cast<UINT>(inputElements.size()),
        g_crtBeamVs,
        sizeof(g_crtBeamVs),
        beamInputLayout_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    D3D11_BUFFER_DESC segmentDescription{};
    segmentDescription.ByteWidth = static_cast<UINT>(
        sizeof(CrtBeamSegment) * kMaxPendingSegments);
    segmentDescription.Usage = D3D11_USAGE_DYNAMIC;
    segmentDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    segmentDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device_->CreateBuffer(
        &segmentDescription,
        nullptr,
        segmentBuffer_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    D3D11_BUFFER_DESC constantDescription{};
    constantDescription.ByteWidth = sizeof(CrtXyShaderConstants);
    constantDescription.Usage = D3D11_USAGE_DYNAMIC;
    constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device_->CreateBuffer(
        &constantDescription,
        nullptr,
        constantsBuffer_.ReleaseAndGetAddressOf()));
}

bool CrtXySurface::createStates()
{
    D3D11_SAMPLER_DESC samplerDescription{};
    samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
    auto result = device_->CreateSamplerState(
        &samplerDescription,
        linearSampler_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    D3D11_BLEND_DESC blendDescription{};
    blendDescription.RenderTarget[0].BlendEnable = TRUE;
    blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    result = device_->CreateBlendState(
        &blendDescription,
        additiveBlend_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    blendDescription = {};
    blendDescription.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    result = device_->CreateBlendState(
        &blendDescription,
        opaqueBlend_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    D3D11_RASTERIZER_DESC rasterizerDescription{};
    rasterizerDescription.FillMode = D3D11_FILL_SOLID;
    rasterizerDescription.CullMode = D3D11_CULL_NONE;
    rasterizerDescription.DepthClipEnable = TRUE;
    return SUCCEEDED(device_->CreateRasterizerState(
        &rasterizerDescription,
        rasterizer_.ReleaseAndGetAddressOf()));
}

bool CrtXySurface::resizeResources()
{
    if (!swapChain_ || outputWidth_ == 0 || outputHeight_ == 0) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputTarget;
    auto result = swapChain_->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(outputTexture.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return false;
    result = device_->CreateRenderTargetView(
        outputTexture.Get(),
        nullptr,
        outputTarget.ReleaseAndGetAddressOf());
    if (FAILED(result)) return false;

    if(phosphorTextures_[0]&&phosphorTextures_[1]&&
       phosphorTargets_[0]&&phosphorTargets_[1]&&phosphorViews_[0]&&phosphorViews_[1]){
        outputTarget_=std::move(outputTarget);
        return true;
    }
    const auto renderWidth = crtXyRenderWidth(
        outputWidth_, outputHeight_, softwareRasterizer_);
    const auto renderHeight = crtXyRenderHeight(
        outputWidth_, outputHeight_, softwareRasterizer_);

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = renderWidth;
    textureDescription.Height = renderHeight;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R16G16_FLOAT;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> textures;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 2> targets;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> views;
    for (std::size_t index = 0; index < textures.size(); ++index) {
        result = device_->CreateTexture2D(
            &textureDescription,
            nullptr,
            textures[index].ReleaseAndGetAddressOf());
        if (FAILED(result)) return false;
        result = device_->CreateRenderTargetView(
            textures[index].Get(),
            nullptr,
            targets[index].ReleaseAndGetAddressOf());
        if (FAILED(result)) return false;
        result = device_->CreateShaderResourceView(
            textures[index].Get(),
            nullptr,
            views[index].ReleaseAndGetAddressOf());
        if (FAILED(result)) return false;
        constexpr float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
        context_->ClearRenderTargetView(targets[index].Get(), clear);
    }

    outputTarget_ = std::move(outputTarget);
    phosphorTextures_ = std::move(textures);
    phosphorTargets_ = std::move(targets);
    phosphorViews_ = std::move(views);
    renderWidth_ = renderWidth;
    renderHeight_ = renderHeight;
    stats_.renderWidth = renderWidth_;
    stats_.renderHeight = renderHeight_;
    phosphorIndex_ = 0;
    clearPhosphor_ = false;
    return true;
}

bool CrtXySurface::updateConstants(double elapsedSeconds)
{
    const auto constants = makeCrtXyShaderConstants(
        renderWidth_, renderHeight_, outputWidth_, outputHeight_,
        elapsedSeconds, orientation_);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result = context_->Map(
        constantsBuffer_.Get(),
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(result)) return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constantsBuffer_.Get(), 0);
    return true;
}

bool CrtXySurface::renderPhosphor()
{
    const auto next = phosphorIndex_ ^ 1U;
    D3D11_VIEWPORT viewport{
        0.0f, 0.0f,
        static_cast<float>(renderWidth_),
        static_cast<float>(renderHeight_),
        0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetRenderTargets(
        1,
        phosphorTargets_[next].GetAddressOf(),
        nullptr);
    constexpr float blendFactor[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(
        opaqueBlend_.Get(), blendFactor, 0xffffffffU);
    if (clearPhosphor_) {
        constexpr float clear[4]{0.0f, 0.0f, 0.0f, 0.0f};
        context_->ClearRenderTargetView(phosphorTargets_[next].Get(), clear);
        clearPhosphor_ = false;
    } else {
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(fullscreenVertexShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(
            0, 1, constantsBuffer_.GetAddressOf());
        context_->PSSetShader(decayPixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(
            0, 1, constantsBuffer_.GetAddressOf());
        context_->PSSetShaderResources(
            0, 1, phosphorViews_[phosphorIndex_].GetAddressOf());
        context_->PSSetSamplers(0, 1, linearSampler_.GetAddressOf());
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
    }
    phosphorIndex_ = next;
    return true;
}

bool CrtXySurface::renderBeam()
{
    if (pendingCount_ == 0) return true;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto result = context_->Map(
        segmentBuffer_.Get(),
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(result)) return false;
    const auto uploadQpc = qpcNow();
    auto* output = static_cast<CrtBeamSegment*>(mapped.pData);
    for (std::size_t index = 0; index < pendingCount_; ++index) {
        output[index] = pendingSegments_[index].beam;
        // The audio rack is intentionally free to exceed 0 dBFS.  Project
        // only the GPU geometry into the finite CRT viewport; this does not
        // normalize, limit, or otherwise alter the audible PCM.
        output[index].x0 = boundedVisualSample(output[index].x0);
        output[index].y0 = boundedVisualSample(output[index].y0);
        output[index].x1 = boundedVisualSample(output[index].x1);
        output[index].y1 = boundedVisualSample(output[index].y1);
        if (uploadQpc > pendingSegments_[index].submittedQpc) {
            output[index].ageSeconds += static_cast<float>(
                static_cast<double>(
                    uploadQpc - pendingSegments_[index].submittedQpc) /
                static_cast<double>(qpcFrequency_));
        }
    }
    context_->Unmap(segmentBuffer_.Get(), 0);

    const UINT stride = sizeof(CrtBeamSegment);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer = segmentBuffer_.Get();
    context_->IASetInputLayout(beamInputLayout_.Get());
    context_->IASetVertexBuffers(
        0, 1, &vertexBuffer, &stride, &offset);
    context_->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(beamVertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->PSSetShader(beamPixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    constexpr float blendFactor[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(
        additiveBlend_.Get(), blendFactor, 0xffffffffU);
    context_->DrawInstanced(
        4, static_cast<UINT>(pendingCount_), 0, 0);
    pendingCount_ = 0;
    return true;
}

bool CrtXySurface::renderComposite()
{
    return renderCompositeTo(
        outputTarget_.Get(), outputWidth_, outputHeight_);
}

bool CrtXySurface::renderCompositeTo(
    ID3D11RenderTargetView* target,
    std::uint32_t width,
    std::uint32_t height)
{
    if (target == nullptr || width == 0 || height == 0) return false;
    D3D11_VIEWPORT viewport{
        0.0f, 0.0f,
        static_cast<float>(width),
        static_cast<float>(height),
        0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetRenderTargets(1, &target, nullptr);
    constexpr float clear[4]{0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(target, clear);
    constexpr float blendFactor[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(
        opaqueBlend_.Get(), blendFactor, 0xffffffffU);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(fullscreenVertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->PSSetShader(compositePixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(
        0, 1, constantsBuffer_.GetAddressOf());
    context_->PSSetShaderResources(
        0, 1, phosphorViews_[phosphorIndex_].GetAddressOf());
    context_->PSSetSamplers(0, 1, linearSampler_.GetAddressOf());
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    return true;
}

void CrtXySurface::suspendSizeWait()
{
    if(!frameWait_)return;
    {
        std::lock_guard<std::mutex> lock(frameWaitMutex_);
        preservingSizeWait_=true;
        waitAdmissionOpen_=false;
        SetThreadpoolWait(frameWait_,nullptr,nullptr);
    }
    // Keep a queued same-swapchain callback: it may have consumed the latency
    // signal already. Its message will be processed after this WM_SIZE returns.
    WaitForThreadpoolWaitCallbacks(frameWait_,FALSE);
    {
        std::lock_guard<std::mutex> lock(frameWaitMutex_);
        preservingSizeWait_=false;
        const bool resumePacing = frameWaitArmed_ && frameWaitPaced_;
        frameWaitArmed_=false;
        if (resumePacing) {
            // Readiness was already consumed before this pacing interval.
            // Reuse the timer's original due time and serial; waiting on DXGI
            // here instead can deadlock until another Present occurs.
            waitAdmissionOpen_=true;
            frameWaitArmed_=true;
            SetThreadpoolWait(frameWait_,framePaceTimer_,nullptr);
        }
    }
}
void CrtXySurface::releaseOutputResources()
{
    if(context_){context_->OMSetRenderTargets(0,nullptr,nullptr);}
    outputTarget_.Reset();
}

void CrtXySurface::releaseSizeResources()
{
    if (context_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->ClearState();
        context_->Flush();
    }
    outputTarget_.Reset();
    for (auto& view : phosphorViews_) view.Reset();
    for (auto& target : phosphorTargets_) target.Reset();
    for (auto& texture : phosphorTextures_) texture.Reset();
}

void CrtXySurface::releaseSwapChainResources()
{
    if (swapChain_ || frameLatencyHandle_ != nullptr) {
        // Exceptional composition/adapter paths can retire a binding while
        // its wait or posted message is still outstanding. Retire the epoch
        // and drain here, at the owner of the handle, before closing it.
        // Successful ResizeBuffers never enters this path.
        lifecycleGeneration_.fetch_add(1, std::memory_order_acq_rel);
        disarmFrameWait(true);
    }
    releaseOutputResources();
    if (frameLatencyHandle_ != nullptr) {
#if defined(NEKOROX_CRT_SURFACE_TEST)
        crtTestBeforeLatencyClose(*this);
#endif
        CloseHandle(frameLatencyHandle_);
        frameLatencyHandle_ = nullptr;
    }
    swapChain_.Reset();
}

void CrtXySurface::releaseDeviceResources()
{
    releaseSwapChainResources();
    releaseSizeResources();
    rasterizer_.Reset();
    opaqueBlend_.Reset();
    additiveBlend_.Reset();
    linearSampler_.Reset();
    constantsBuffer_.Reset();
    segmentBuffer_.Reset();
    beamInputLayout_.Reset();
    compositePixelShader_.Reset();
    beamPixelShader_.Reset();
    decayPixelShader_.Reset();
    beamVertexShader_.Reset();
    fullscreenVertexShader_.Reset();
    swapChain_.Reset();
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    context_.Reset();
    device_.Reset();
    renderRecoveryEpoch_ = 0;
    renderMonitor_ = nullptr;
    adapterCheckRequired_ = true;
    softwareRasterizer_ = false;
    stats_.softwareRasterizer = false;
}

bool CrtXySurface::updateComposition()
{
#if defined(NEKOROX_CRT_SURFACE_TEST)
    if (crtTestFailComposition()) return false;
#endif
    if (!compositionVisual_ || !compositionDevice_) return false;
    auto result =
        compositionVisual_->SetOffsetX(static_cast<float>(viewport_.left));
    if (FAILED(result)) return false;
    result =
        compositionVisual_->SetOffsetY(static_cast<float>(viewport_.top));
    if (FAILED(result)) return false;
    result = compositionVisual_->SetContent(
        active_ && swapChain_ ? swapChain_.Get() : nullptr);
    if (FAILED(result)) return false;
    return SUCCEEDED(compositionDevice_->Commit());
}

void CrtXySurface::updateTargetFramePeriod()
{
    if (parent_ == nullptr) return;
    const auto now = qpcNow();
    const auto topologyGeneration =
        DisplayEnvironmentService::instance().topologyGeneration();
    if (lastRefreshQueryQpc_ != 0 &&
        topologyGeneration == lastTopologyGeneration_ &&
        now - lastRefreshQueryQpc_ < qpcFrequency_) {
        return;
    }
    lastRefreshQueryQpc_ = now;
    lastTopologyGeneration_ = topologyGeneration;

    const auto environment =
        DisplayEnvironmentService::instance().snapshot(parent_);
    const auto numerator = (std::max)(1U, environment.refreshNumerator);
    const auto denominator = (std::max)(1U, environment.refreshDenominator);
    const auto nextPeriod = nekorox::presentationPeriodTicks(
        qpcFrequency_, numerator, denominator);
    if (targetFrameTicks_ != nextPeriod) {
        targetFrameTicks_ = nextPeriod;
        // A monitor/refresh transition starts a new presentation epoch. Do
        // not carry a deadline from the previous display or replay missed
        // frames at the new cadence.
        nextPresentQpc_ = 0;
    }
    stats_.targetRefreshMilliHz = environment.refreshMilliHz();
}

void CrtXySurface::armFrameWait()
{
    if (!active_ || frameWait_ == nullptr || parent_ == nullptr) {
        return;
    }
    if (resourceRetryQpc_ != 0) {
        const auto now = qpcNow();
        armPaceTimer(resourceRetryQpc_ > now ? resourceRetryQpc_ - now : 1U);
        return;
    }
    if (frameLatencyHandle_ == nullptr) return;
    std::lock_guard<std::mutex> lock(frameWaitMutex_);
    if (frameWaitArmed_ || readyMessagePending_) return;
    waitAdmissionOpen_ = true;
    armedGeneration_ = generation();
    armedSerial_ = ++nextArmSerial_;
    if (armedSerial_ == 0) armedSerial_ = ++nextArmSerial_;
    frameWaitArmed_ = true;
    frameWaitPaced_ = false;
    SetThreadpoolWait(frameWait_, frameLatencyHandle_, nullptr);
}

bool CrtXySurface::armPaceTimer(std::uint64_t delayTicks)
{
    if (!active_ || frameWait_ == nullptr ||
        framePaceTimer_ == nullptr || parent_ == nullptr ||
        delayTicks == 0) {
        return false;
    }
    const auto delayHundredNanoseconds = std::max<LONGLONG>(
        1,
        static_cast<LONGLONG>(std::ceil(
            static_cast<double>(delayTicks) * 10000000.0 /
            static_cast<double>(qpcFrequency_))));
    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -delayHundredNanoseconds;
    // An unsolicited redraw (move/resize/display change) can arrive while
    // the pacing wait is already armed. Never reset or cancel that timer:
    // doing so leaves frameWaitArmed_ true with no future callback, freezing
    // the CRT while its PCM queue continues filling.
    std::lock_guard<std::mutex> lock(frameWaitMutex_);
    if (frameWaitArmed_ || readyMessagePending_) return true;
    if (!SetWaitableTimer(
            framePaceTimer_, &dueTime, 0, nullptr, nullptr, FALSE)) {
        return false;
    }

    waitAdmissionOpen_ = true;
    armedGeneration_ = generation();
    armedSerial_ = ++nextArmSerial_;
    if (armedSerial_ == 0) armedSerial_ = ++nextArmSerial_;
    frameWaitArmed_ = true;
    frameWaitPaced_ = true;
    SetThreadpoolWait(frameWait_, framePaceTimer_, nullptr);
    return true;
}

void CrtXySurface::retryResourcesLater()
{
    // A cache miss or transient allocation failure must not strand an active
    // surface. Use the existing one-shot timer, bounded to four attempts per
    // second. Hide/shutdown drain it with the normal readiness wait.
    if (active_ && outputWidth_ != 0 && outputHeight_ != 0) {
        const auto now = qpcNow();
        if (resourceRetryQpc_ == 0) {
            disarmFrameWait(true);
            resourceRetryQpc_ = now + std::max<std::uint64_t>(1, qpcFrequency_ / 4U);
        }
        armPaceTimer(resourceRetryQpc_ > now ? resourceRetryQpc_ - now : 1U);
    }
}

void CrtXySurface::disarmFrameWait(bool waitForCallback)
{
    const auto wait = frameWait_;
    if (wait == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(frameWaitMutex_);
        SetThreadpoolWait(wait, nullptr, nullptr);
        waitAdmissionOpen_ = false;
        frameWaitArmed_ = false;
        frameWaitPaced_ = false;
        readyMessagePending_ = false;
        deliveredGeneration_ = 0;
        deliveredSerial_ = 0;
    }
    if (waitForCallback) {
        WaitForThreadpoolWaitCallbacks(wait, TRUE);
#if defined(NEKOROX_CRT_SURFACE_TEST)
        crtTestWaitDrained(*this);
#endif
    }
    if (framePaceTimer_ != nullptr) {
        CancelWaitableTimer(framePaceTimer_);
    }
}

void CrtXySurface::handleDeviceLoss(HRESULT reason)
{
    if (!failedDevice(reason)) return;
    ++stats_.deviceLosses;
    lifecycleGeneration_.fetch_add(1, std::memory_order_acq_rel);
    disarmFrameWait(true);
    RenderDeviceHub::instance().invalidate(device_.Get());
    releaseDeviceResources();
    compositionAttached_ = false;
    clearPhosphor_ = true;
    idleSettled_ = false;
    estimatedPhosphorEnergy_ = 0.0f;
    lastSegmentQpc_ = 0;
    lastPresentQpc_ = 0;
    nextPresentQpc_ = 0;
    if (active_) {
        if (ensureResources()) armFrameWait();
        else retryResourcesLater();
    }
}

std::uint64_t CrtXySurface::qpcNow() const noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

static_assert(sizeof(CrtBeamSegment) == 24);

} // namespace nekorox::component
