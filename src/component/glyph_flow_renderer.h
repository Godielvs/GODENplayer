#pragma once

#include "glyph_flow_service.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <span>

namespace nekorox::component {

struct GlyphFlowDrawStyle {
    D2D1_COLOR_F color = D2D1::ColorF(0.0f, 0.847f, 0.816f, 1.0f);
    float strength = 0.12f;
    float maximumTravelDip = 1.0f;
};

// Lightweight, caller-authored glyphs which reuse the same atlas and
// rendering path as artwork GlyphFlow. Coordinates are normalized to the
// supplied bounds while size remains in DIP.
struct GlyphFlowOverlayCell {
    float xNormalized = 0.0f;
    float yNormalized = 0.0f;
    float sizeDip = 1.0f;
    float opacity = 1.0f;
    std::uint8_t glyphIndex = 0;
};

class GlyphFlowRenderer final {
public:
    bool draw(
        ID2D1RenderTarget* target,
        const D2D1_RECT_F& bounds,
        MotionMode mode,
        const GlyphFlowDrawStyle& style = {});

    bool drawOverlay(
        ID2D1RenderTarget* target,
        const D2D1_RECT_F& bounds,
        std::span<const GlyphFlowOverlayCell> cells,
        const D2D1_COLOR_F& color);

    void reset() noexcept;
    void presented(bool accepted) noexcept;

private:
    bool ensureResources(ID2D1RenderTarget* target);
    bool createAtlas(ID2D1Factory* factory);

    std::array<::nekorox::GlyphFlowCellV1, kGlyphFlowFieldCellCount>
        cells_{};
    std::array<Microsoft::WRL::ComPtr<ID2D1Geometry>, 6> atlas_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1Factory* atlasOwner_ = nullptr;
    ID2D1RenderTarget* brushOwner_ = nullptr;
    std::uint64_t pendingGeneration_ = 0;
};

} // namespace nekorox::component
