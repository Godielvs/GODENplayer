#pragma once

#include "nekorox/realtime_visual_model.h"

#include <d2d1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace nekorox::component {

struct ScopeLightDiagnostics {
    std::uint64_t submissions = 0;
    std::uint64_t successes = 0;
    std::uint64_t resourceBytes = 0;
    bool gpuMeasured = false;
    double gpuMicroseconds = 0.0;
};

// Spatial optical skin for the authoritative CompactScopePresenter curve.
// It borrows Listen's device/context/swap chain and never owns a Present,
// DirectComposition target, PCM stream or prior-frame history texture.
class ScopeLightPass final {
public:
    [[nodiscard]] bool draw(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        IDXGISwapChain* swapChain,
        const D2D1_RECT_F& plotDip,
        float dpi,
        const std::array<float, kScopeSamples>& samples,
        const std::array<float, kScopeSamples>& minimum,
        const std::array<float, kScopeSamples>& maximum,
        std::uint64_t visualRevision,
        const D2D1_COLOR_F& accent) noexcept;

    void resetSurface() noexcept;
    void reset() noexcept;
    [[nodiscard]] const ScopeLightDiagnostics& diagnostics() const noexcept
    {
        return diagnostics_;
    }

private:
    struct MaskVertex {
        float x = 0.0f;
        float y = 0.0f;
        float core = 0.0f;
        float envelope = 0.0f;
    };

    static constexpr std::size_t kMaximumVertices =
        (kScopeSamples + (kScopeSamples - 1U)) * 6U;

    [[nodiscard]] bool ensureDevice(ID3D11Device* device) noexcept;
    [[nodiscard]] bool ensureTargets(
        IDXGISwapChain* swapChain,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] bool buildMask(
        ID3D11DeviceContext* context,
        const std::array<float, kScopeSamples>& samples,
        const std::array<float, kScopeSamples>& minimum,
        const std::array<float, kScopeSamples>& maximum,
        float dpi,
        float scale,
        std::uint64_t visualRevision) noexcept;
    [[nodiscard]] bool blur(
        ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination,
        float deltaX,
        float deltaY) noexcept;
    [[nodiscard]] bool composite(
        ID3D11DeviceContext* context,
        const D2D1_RECT_F& plotDip,
        float dpi,
        const D2D1_COLOR_F& accent) noexcept;
    void releaseTargets() noexcept;
    void releaseTimingQueries() noexcept;
    void collectGpuTiming(ID3D11DeviceContext* context) noexcept;
    [[nodiscard]] std::size_t beginGpuTiming(
        ID3D11DeviceContext* context) noexcept;
    void endGpuTiming(
        ID3D11DeviceContext* context, std::size_t slot) noexcept;
    void appendSegment(float x1, float y1, float x2, float y2,
        float thickness, float core, float envelope) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> maskVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> maskPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreenVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> blurPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> compositePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> maskInputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> maximumBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> screenBlend_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> maskTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> temporaryTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> temporaryTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> temporaryView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nearTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> nearTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nearView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> wideTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> wideTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> wideView_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> surfaceTarget_;

    std::array<MaskVertex, kMaximumVertices> vertices_{};
    std::size_t vertexCount_ = 0;
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    std::uint32_t surfaceWidth_ = 0;
    std::uint32_t surfaceHeight_ = 0;
    bool surfaceSrgb_ = false;
    std::uint64_t maskRevision_ = 0;
    struct TimingSlot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };
    std::array<TimingSlot, 4> timing_{};
    std::size_t nextTimingSlot_ = 0;
    ScopeLightDiagnostics diagnostics_{};
};

} // namespace nekorox::component
