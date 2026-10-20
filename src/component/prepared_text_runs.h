#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <memory>
#include <functional>
#include <vector>

namespace nekorox::component {
struct PreparedGlyphRun {
    Microsoft::WRL::ComPtr<IDWriteFontFace> face;
    std::vector<UINT16> indices;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    float x = 0, y = 0, size = 0, lineBaseline = 0;
    UINT32 bidi = 0;
    BOOL sideways = FALSE;
    DWRITE_MEASURING_MODE measuring = DWRITE_MEASURING_MODE_NATURAL;
};
struct PreparedTextRuns {
    float top = 0, bottom = 0;
    std::vector<PreparedGlyphRun> glyphs;
    static std::shared_ptr<const PreparedTextRuns> capture(
        IDWriteTextLayout* layout, float top, float bottom,
        const std::function<bool()>& cancelled = {});
    void draw(ID2D1RenderTarget* target, ID2D1Brush* brush, float x, float y) const;
};
}
