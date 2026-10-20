#pragma once

#include "nekorox/crt_xy_model.h"
#include "nekorox/crt_xy_render_spec.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nekorox::tools {

struct CrtOffscreenStats {
    std::uint64_t submittedFrames = 0;
    std::uint64_t submittedSegments = 0;
    std::uint64_t readbackWaits = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    bool softwareRasterizer = false;
    std::wstring adapterName;
};

struct CrtReadbackFrame {
    std::uint64_t index = 0;
    std::vector<std::uint8_t> bgra;
};

class CrtOffscreenRenderer final {
public:
    CrtOffscreenRenderer() = default;
    ~CrtOffscreenRenderer();

    CrtOffscreenRenderer(const CrtOffscreenRenderer&) = delete;
    CrtOffscreenRenderer& operator=(const CrtOffscreenRenderer&) = delete;

    bool initialize(
        std::uint32_t width,
        std::uint32_t height,
        CrtXyOrientation orientation,
        bool forceWarp,
        std::wstring& error);
    bool reset(std::wstring& error);

    // Returns the oldest completed frame when the four-slot staging ring is
    // full. The current frame is always submitted before the method returns.
    bool render(
        std::span<const CrtBeamSegment> segments,
        double deltaSeconds,
        CrtReadbackFrame& completed,
        bool& hasCompleted,
        std::wstring& error);
    bool drain(
        CrtReadbackFrame& completed,
        bool& hasCompleted,
        std::wstring& error);
    void shutdown() noexcept;

    [[nodiscard]] const CrtOffscreenStats& stats() const noexcept {
        return stats_;
    }

private:
    static constexpr std::size_t kMaximumSegments = 65536;
    static constexpr std::size_t kReadbackRingSize = 4;

    struct ReadbackSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Query> completion;
        std::uint64_t frameIndex = 0;
        bool pending = false;
    };

    bool createDevice(bool forceWarp, std::wstring& error);
    bool createShaders(std::wstring& error);
    bool createStates(std::wstring& error);
    bool createSizeResources(std::wstring& error);
    bool updateConstants(double deltaSeconds, std::wstring& error);
    bool decay(std::wstring& error);
    bool beam(
        std::span<const CrtBeamSegment> segments,
        std::wstring& error);
    bool composite(std::wstring& error);
    bool enqueueReadback(std::wstring& error);
    bool consumeOldest(
        CrtReadbackFrame& completed,
        bool wait,
        bool& available,
        std::wstring& error);
    static std::wstring hresultMessage(const wchar_t* operation, HRESULT hr);

    std::uint32_t outputWidth_ = 0;
    std::uint32_t outputHeight_ = 0;
    std::uint32_t renderWidth_ = 0;
    std::uint32_t renderHeight_ = 0;
    std::uint32_t phosphorIndex_ = 0;
    CrtXyOrientation orientation_ = CrtXyOrientation::midSide;
    bool clearPhosphor_ = true;
    std::uint64_t nextFrameIndex_ = 0;
    std::size_t readbackHead_ = 0;
    std::size_t readbackCount_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
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
    std::array<ReadbackSlot, kReadbackRingSize> readback_{};
    CrtOffscreenStats stats_{};
};

} // namespace nekorox::tools
