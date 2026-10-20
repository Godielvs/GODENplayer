#pragma once

#include "render_surface_contracts.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>

namespace nekorox::component {

// Native DXGI/D2D surface with surface-owned context, swap chain and target
// bitmap. Device creation is obtained only from RenderDeviceHub's async cache.
class NativeD2DSurface final {
public:
    [[nodiscard]] bool ensure(HWND window, float dpi) noexcept;
    [[nodiscard]] bool resize() noexcept;
    [[nodiscard]] FrameResult present(HRESULT drawResult) noexcept;
    void reset() noexcept;

    [[nodiscard]] ID2D1DeviceContext* target() const noexcept
    {
        return context_.Get();
    }
    [[nodiscard]] ID2D1Bitmap1* swapChainTarget() const noexcept
    {
        return targetBitmap_.Get();
    }
    [[nodiscard]] ID3D11Device* d3dDevice() const noexcept
    {
        return d3dDevice_.Get();
    }
    [[nodiscard]] ID3D11DeviceContext* immediateContext() const noexcept
    {
        return immediateContext_.Get();
    }
    [[nodiscard]] HANDLE readinessHandle() const noexcept
    {
        return readiness_;
    }
    [[nodiscard]] SurfaceBindingKey bindingKey() const noexcept
    {
        return {lifetimeToken_, swapchainEpoch_, readiness_};
    }
    [[nodiscard]] SurfaceBackend backend() const noexcept { return backend_; }
    [[nodiscard]] std::uint64_t deviceEpoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t resourceEpoch() const noexcept
    {
        return resourceEpoch_;
    }
    [[nodiscard]] std::uint64_t sizeRevision() const noexcept
    {
        return sizeRevision_;
    }
    [[nodiscard]] HRESULT lastPresentHr() const noexcept
    {
        return lastPresentHr_;
    }
    [[nodiscard]] HRESULT lastDrawHr() const noexcept { return lastDrawHr_; }
    [[nodiscard]] bool active() const noexcept
    {
        return context_ != nullptr && swapChain_ != nullptr;
    }

private:
    [[nodiscard]] bool createTargetBitmap() noexcept;
    [[nodiscard]] D2D1_SIZE_U clientPixelSize() const noexcept;

    HWND window_ = nullptr;
    float dpi_ = 96.0f;
    D2D1_SIZE_U pixelSize_{};
    SurfaceBackend backend_ = SurfaceBackend::gdiFallback;
    std::uint64_t epoch_ = 0;
    std::uint64_t observedRecoveryEpoch_ = 0;
    HMONITOR recoveryMonitor_ = nullptr;
    std::uint64_t resourceEpoch_ = 0;
    std::uint64_t sizeRevision_ = 0;
    std::uint64_t topologyGeneration_ = 0;
    HRESULT lastPresentHr_ = E_PENDING;
    HRESULT lastDrawHr_ = E_PENDING;
    HANDLE readiness_ = nullptr;
    std::uint64_t lifetimeToken_ = 0;
    std::uint64_t swapchainEpoch_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual_;
};

} // namespace nekorox::component
