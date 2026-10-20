#pragma once

#include "render_surface_contracts.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <span>

namespace nekorox::component {

struct CompactScopeColumn {
    float x, top, bottom, midpoint;
};
static_assert(sizeof(CompactScopeColumn) == 16);

// Zero-input effect constants, in physical pixels. The projection supplied by
// the owner remains in DIPs and includes every presented sample/extremum.
struct CompactScopeGpuConstants {
    std::array<float, 4> bounds{}, controls{}, accent{}, bright{};
    std::array<CompactScopeColumn, 512> columns{};
};
static_assert(sizeof(CompactScopeGpuConstants) == 8256);

class CompactScopeGpu final {
public:
    struct Statistics {
        std::uint64_t submitted = 0, fallback = 0, creations = 0;
        std::uint64_t failures = 0, completedDraws = 0;
    };
    enum class FaultForQa : unsigned { none, create, update, render };

    CompactScopeGpu() = default;
    ~CompactScopeGpu() { reset(); }
    CompactScopeGpu(const CompactScopeGpu&) = delete;
    CompactScopeGpu& operator=(const CompactScopeGpu&) = delete;

    // True means the command was queued inside the owner's existing draw.
    // The owner must report its EndDraw result through finishFrame().
    bool tryDraw(ID2D1RenderTarget* target, ID3D11Device* device,
        SurfaceBackend backend, std::span<const CompactScopeColumn> columns,
        D2D1_COLOR_F accent, D2D1_COLOR_F bright) noexcept;
    // True requests one full repaint using scalar drawing after a deferred
    // effect failure. Present/occlusion failures do not blacklist the effect.
    bool finishFrame(HRESULT drawResult) noexcept;

    void releaseTargetResources() noexcept;
    void reset() noexcept;
    const Statistics& statistics() const noexcept { return statistics_; }
    HRESULT lastFailure() const noexcept { return lastFailure_; }

    // Entry points are exposed only by the owner's opt-in QA message gate.
    void setScalarForQa(bool scalar) noexcept;
    void queueFaultForQa(FaultForQa fault) noexcept;

private:
    bool bind(ID2D1RenderTarget* target, ID3D11Device* device) noexcept;
    void fail(HRESULT result) noexcept;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Effect> effect_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_, failedDevice_;
    ID2D1RenderTarget* observedTarget_ = nullptr; // retained by context_
    CompactScopeGpuConstants constants_{};
    Statistics statistics_{};
    HRESULT lastFailure_ = S_OK;
    FaultForQa fault_ = FaultForQa::none;
    bool registered_ = false, submitted_ = false, scalarForQa_ = false;
};

} // namespace nekorox::component
