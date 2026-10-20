#pragma once

#include "nekorox/crt_xy_model.h"
#include "nekorox/crt_xy_render_spec.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace nekorox::component {

struct CrtSurfaceStats {
    std::uint64_t submittedSegments = 0;
    std::uint64_t droppedSegments = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t skippedPresents = 0;
    std::uint64_t pacedWaits = 0;
    std::uint64_t deviceLosses = 0;
    std::uint64_t settledStops = 0;
    std::uint32_t targetRefreshMilliHz = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    bool softwareRasterizer = false;
};

struct CrtSurfaceCapture {
    RECT pixelRect{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra;
};

class CrtXySurface final {
public:
    CrtXySurface();
    ~CrtXySurface();

    CrtXySurface(const CrtXySurface&) = delete;
    CrtXySurface& operator=(const CrtXySurface&) = delete;

    void attach(HWND parent, UINT frameReadyMessage);
    void setViewport(const RECT& pixelRect);
    void setActive(bool active);
    void setOrientation(CrtXyOrientation orientation) noexcept;
    void displayEnvironmentChanged(bool forceAdapterCheck = false) noexcept;
    void reset() noexcept;
    void submit(std::span<const CrtBeamSegment> segments) noexcept;
    void present(
        std::uint32_t lifecycleGeneration,
        std::uint32_t armSerial = 0);
    [[nodiscard]] bool captureBgra(CrtSurfaceCapture& capture);
    void shutdown();

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::uint32_t generation() const noexcept
    {
        return lifecycleGeneration_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const CrtSurfaceStats& stats() const noexcept
    {
        return stats_;
    }

private:
#if defined(NEKOROX_CRT_SURFACE_TEST)
    friend struct CrtSurfaceTestPeer;
#endif
    static constexpr std::size_t kMaxPendingSegments = 65536;

    struct PendingSegment {
        CrtBeamSegment beam{};
        std::uint64_t submittedQpc = 0;
    };
    static VOID CALLBACK frameWaitCallback(
        PTP_CALLBACK_INSTANCE instance,
        void* context,
        PTP_WAIT wait,
        TP_WAIT_RESULT result);

    bool ensureResources();
    bool createDeviceResources();
    bool createSwapChain();
    bool createShaders();
    bool createStates();
    bool resizeResources();
    bool updateConstants(double elapsedSeconds);
    bool renderPhosphor();
    bool renderBeam();
    bool renderComposite();
    bool renderCompositeTo(
        ID3D11RenderTargetView* target,
        std::uint32_t width,
        std::uint32_t height);
    void releaseSwapChainResources();
    void releaseSizeResources();
    void releaseOutputResources();
    void suspendSizeWait();
    bool preservingSizeWait_=false;
    void releaseDeviceResources();
    bool updateComposition();
    void updateTargetFramePeriod();
    void armFrameWait();
    bool armPaceTimer(std::uint64_t delayTicks);
    void retryResourcesLater();
    // Drain only at a lifecycle/binding boundary. Stable frames reuse the
    // persistent PTP_WAIT and never synchronously unregister per present.
    void disarmFrameWait(bool waitForCallback = false);
    void handleDeviceLoss(HRESULT reason);
    std::uint64_t qpcNow() const noexcept;

    HWND parent_ = nullptr;
    UINT frameReadyMessage_ = 0;
    RECT viewport_{};
    bool active_ = false;
    bool compositionAttached_ = false;
    bool clearPhosphor_ = true;
    bool softwareRasterizer_ = false;
    bool idleSettled_ = false;
    std::uint64_t renderRecoveryEpoch_ = 0;
    CrtXyOrientation orientation_ = CrtXyOrientation::directLeftRight;
    std::uint32_t outputWidth_ = 0;
    std::uint32_t outputHeight_ = 0;
    std::uint32_t renderWidth_ = 0;
    std::uint32_t renderHeight_ = 0;
    std::uint32_t phosphorIndex_ = 0;

    std::array<PendingSegment, kMaxPendingSegments> pendingSegments_{};
    std::size_t pendingCount_ = 0;

    std::atomic<std::uint32_t> lifecycleGeneration_{1};
    HANDLE frameLatencyHandle_ = nullptr;
    HANDLE framePaceTimer_ = nullptr;
    std::mutex frameWaitMutex_;
    PTP_WAIT frameWait_ = nullptr;
    bool waitAdmissionOpen_ = false;
    bool frameWaitArmed_ = false;
    bool frameWaitPaced_ = false;
    bool readyMessagePending_ = false;
    std::uint32_t armedGeneration_ = 0;
    std::uint32_t armedSerial_ = 0;
    std::uint32_t deliveredGeneration_ = 0;
    std::uint32_t deliveredSerial_ = 0;
    std::uint32_t nextArmSerial_ = 0;
    std::uint64_t qpcFrequency_ = 1;
    std::uint64_t lastPresentQpc_ = 0;
    std::uint64_t lastSegmentQpc_ = 0;
    std::uint64_t nextPresentQpc_ = 0;
    std::uint64_t resourceRetryQpc_ = 0;
    std::uint64_t targetFrameTicks_ = 1;
    std::uint64_t lastRefreshQueryQpc_ = 0;
    std::uint64_t lastTopologyGeneration_ = 0;
    HMONITOR renderMonitor_ = nullptr;
    bool adapterCheckRequired_ = true;
    float estimatedPhosphorEnergy_ = 0.0f;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual_;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputTarget_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2>
        phosphorTextures_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 2>
        phosphorTargets_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>
        phosphorViews_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreenVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> beamVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> decayPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> beamPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> compositePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> beamInputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> segmentBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantsBuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> additiveBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;

    CrtSurfaceStats stats_{};
};

} // namespace nekorox::component
