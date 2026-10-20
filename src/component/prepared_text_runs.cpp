#include "prepared_text_runs.h"
#include <wrl.h>
#include <dwrite_1.h>
#include <algorithm>
#include <cmath>

namespace nekorox::component {
namespace {
class RunCollector final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteTextRenderer> {
public:
    PreparedTextRuns result;
    struct Line { UINT32 end; float baseline; };
    std::vector<Line> lines;
    std::function<bool()> cancelled;
    HRESULT status = S_OK;
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override {
        *matrix = {1, 0, 0, 1, 0, 0}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* scale) override {
        *scale = 1.f; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y,
        DWRITE_MEASURING_MODE measuring, const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown*) override {
        if (FAILED(status)) return status;
        try {
            if (cancelled && cancelled()) return status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        } catch (...) { return status = E_ABORT; }
        if (!run || !run->fontFace) return status = E_INVALIDARG;
        Microsoft::WRL::ComPtr<IDWriteFontFace1> extended;
        if (!run->isSideways && SUCCEEDED(run->fontFace->QueryInterface(IID_PPV_ARGS(&extended)))) {
            DWRITE_FONT_METRICS1 metrics{};
            extended->GetMetrics(&metrics);
            if (metrics.designUnitsPerEm != 0U) {
                float lowOffset = 0.f, highOffset = 0.f;
                if (run->glyphOffsets) for (UINT32 i = 0; i < run->glyphCount; ++i) {
                    lowOffset = std::min(lowOffset, run->glyphOffsets[i].ascenderOffset);
                    highOffset = std::max(highOffset, run->glyphOffsets[i].ascenderOffset);
                }
                const auto scale = run->fontEmSize / metrics.designUnitsPerEm;
                // Font-wide ink bounds plus every shaping offset, with room for
                // antialiasing. Unknown/sideways fonts conservatively keep runs.
                const auto inkTop = y - metrics.glyphBoxTop * scale - highOffset - 2.f;
                const auto inkBottom = y - metrics.glyphBoxBottom * scale - lowOffset + 2.f;
                if (inkBottom < result.top || inkTop > result.bottom) return S_OK;
            }
        }
        try {
            PreparedGlyphRun copy;
            copy.face = run->fontFace;
            copy.indices.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
            if (run->glyphAdvances) copy.advances.assign(run->glyphAdvances, run->glyphAdvances + run->glyphCount);
            if (run->glyphOffsets) copy.offsets.assign(run->glyphOffsets, run->glyphOffsets + run->glyphCount);
            copy.x = x; copy.y = y; copy.size = run->fontEmSize;
            copy.lineBaseline = y;
            if (description) {
                const auto line = std::upper_bound(lines.begin(), lines.end(), description->textPosition,
                    [](UINT32 position, const Line& line){return position < line.end;});
                if (line != lines.end()) copy.lineBaseline = line->baseline;
            }
            copy.bidi = run->bidiLevel; copy.sideways = run->isSideways; copy.measuring = measuring;
            result.glyphs.push_back(std::move(copy));
            return S_OK;
        } catch (...) { return status = E_OUTOFMEMORY; }
    }
    // The value-only reading styles expose neither decorations nor inline
    // objects. Fail explicitly if that contract is extended without support.
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return status = E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return status = E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return status = E_NOTIMPL; }
};
}

std::shared_ptr<const PreparedTextRuns> PreparedTextRuns::capture(
    IDWriteTextLayout* layout, float top, float bottom,
    const std::function<bool()>& cancelled)
{
    if (!layout || (cancelled && cancelled())) return {};
    auto collector = Microsoft::WRL::Make<RunCollector>();
    if (!collector) return {};
    collector->cancelled = cancelled;
    collector->result.top = top;
    collector->result.bottom = bottom;
    UINT32 count = 0;
    (void)layout->GetLineMetrics(nullptr, 0, &count);
    std::vector<DWRITE_LINE_METRICS> metrics(count);
    if (FAILED(layout->GetLineMetrics(metrics.data(), count, &count))) return {};
    float lineTop = 0.f;
    UINT32 end = 0;
    for (const auto& line : metrics) {
        if (cancelled && cancelled()) return {};
        end += line.length;
        collector->lines.push_back({end, lineTop + line.baseline});
        lineTop += line.height;
    }
    // DirectWrite can continue callbacks and return S_OK after a renderer
    // failure. Latch it ourselves; never publish partial glyphs as complete.
    if (FAILED(layout->Draw(nullptr, collector.Get(), 0.f, 0.f)) || FAILED(collector->status) ||
        (cancelled && cancelled())) return {};
    return std::make_shared<const PreparedTextRuns>(std::move(collector->result));
}

void PreparedTextRuns::draw(ID2D1RenderTarget* target, ID2D1Brush* brush,
    float x, float y) const
{
    D2D1_MATRIX_3X2_F transform{};
    target->GetTransform(&transform);
    float dpiX = 96.f, dpiY = 96.f;
    target->GetDpi(&dpiX, &dpiY);
    for (const auto& run : glyphs) {
        const DWRITE_GLYPH_RUN glyphs{run.face.Get(), run.size,
            static_cast<UINT32>(run.indices.size()), run.indices.data(),
            run.advances.empty() ? nullptr : run.advances.data(),
            run.offsets.empty() ? nullptr : run.offsets.data(), run.sideways, run.bidi};
        auto baseline = y + run.y;
        if (transform._12 == 0.f && transform._21 == 0.f && transform._22 != 0.f && dpiY > 0.f) {
            const auto lineBaseline = y + run.lineBaseline;
            const auto scale = transform._22 * dpiY / 96.f;
            const auto deviceY = lineBaseline * scale + transform._32;
            baseline += (std::floor(deviceY + .5f) - transform._32) / scale - lineBaseline;
        }
        target->DrawGlyphRun(D2D1::Point2F(x + run.x, baseline), &glyphs, brush, run.measuring);
    }
}
}
