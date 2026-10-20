#pragma once

#include "art_cache.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace nekorox::component {

// Draws a deterministic, colour-sampled ASCII reconstruction of an artwork.
// DirectWrite glyph runs are reused directly; no text layout or heap work is
// performed per cell or per frame.
class AsciiArtworkTransitionRenderer final {
public:
    bool prepare(ID2D1RenderTarget* target);

    bool draw(
        ID2D1RenderTarget* target,
        ID2D1Bitmap* currentBitmap,
        const ArtPixels& pixels,
        const D2D1_RECT_F& bounds,
        float progress,
        std::uint64_t seed,
        float opacity = 1.0f);

    void resetDeviceResources() noexcept;

private:
    static constexpr std::size_t glyphCount = 10;
    static constexpr std::size_t scrambleStepCount = 12;

    struct PreparedCell {
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float alpha = 0.0f;
        float luminance = 0.0f;
        float order = 0.0f;
        std::uint8_t targetGlyph = 0;
        std::array<std::uint8_t, scrambleStepCount> scrambledGlyphs{};
    };

    bool ensureResources(ID2D1RenderTarget* target);
    bool createFontFace();
    bool ensurePreparedCells(
        const ArtPixels& pixels,
        std::uint32_t columns,
        std::uint32_t rows,
        std::uint64_t seed);

    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteFontFace> fontFace_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    std::array<UINT16, glyphCount> glyphIndices_{};
    std::array<DWRITE_GLYPH_METRICS, glyphCount> glyphMetrics_{};
    DWRITE_FONT_METRICS fontMetrics_{};
    ID2D1RenderTarget* brushOwner_ = nullptr;
    std::vector<PreparedCell> preparedCells_;
    std::uint64_t preparedContentHash_ = 0;
    std::uint64_t preparedSeed_ = 0;
    std::uint32_t preparedSourceWidth_ = 0;
    std::uint32_t preparedSourceHeight_ = 0;
    std::uint32_t preparedColumns_ = 0;
    std::uint32_t preparedRows_ = 0;
};

} // namespace nekorox::component
