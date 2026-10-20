#pragma once

#include "sdk_compat.h"
#include "native_d2d_surface.h"
#include "monitor_text_rendering.h"
#include "pixel_ui/pixel_icon_painter.hpp"
#include "pixel_ui/ui_glyph_map.hpp"
#include "ui_design_system.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <array>
#include <optional>
#include <string_view>

namespace nekorox::component {

enum class PanelColor {
    background,
    panel,
    selected,
    border,
    text,
    muted,
    bright,
    accent,
    secondary,
    highlight,
    amber,
};

enum class PanelText {
    caption,
    normal,
    strong,
    right,
    center,
};

using PanelGlyph = UiGlyph;

class PanelD2D final {
    MonitorTextRendering textRendering_;
    bool ensureBackend(HWND window);
public:
    // Establishes the surface/backend without beginning a draw. Owners bind
    // the returned readiness handle to WindowFrameClock before painting.
    bool prepare(HWND window) { return ensure(window); }
    // Disposable-profile QA is gated by the surface owner. Observe every
    // entry, including a second prepare/begin in the same WM_PAINT.
    void queuePreparationFaultsForQa(std::uint32_t count) noexcept
    { qaPreparationTracking_ = true; qaPreparationFailuresRemaining_ = count; }
    [[nodiscard]] std::uint64_t qaPreparationAttempts() const noexcept { return qaPreparationAttempts_; }
    [[nodiscard]] std::uint64_t qaPreparationFaults() const noexcept { return qaPreparationFaults_; }
    [[nodiscard]] std::uint32_t qaPreparationRemaining() const noexcept { return qaPreparationFailuresRemaining_; }
    bool begin(HWND window, HDC paintDc);
    // Complete frames whose owner never reuses the panel backing can draw
    // directly. Partial/cached owners must continue to use begin().
    bool beginDirect(HWND window, HDC paintDc);
    bool beginPartial(HWND window, HDC paintDc, const RECT& dirtyPixels);
    // Recompose an unchanged retained frame and redraw its live region in a
    // single batch. The owner must supply the complete live region, including
    // when WM_PAINT reports only a smaller exposed rectangle.
    bool beginCachedOverlay(HWND window, HDC paintDc, const RECT& livePixels);
    // A static scene excludes all live overlays. Restore it without erasing
    // the background, then let the owner draw meters/cursors over it.
    bool beginCachedScene(HWND window, std::uint64_t expectedThemeGeneration);
    bool finishStaticSceneAndBeginOverlay();
    [[nodiscard]] std::uint64_t appliedThemeGeneration() const noexcept
    {
        return themeGeneration_;
    }
    // Revalidates the backend and reports whether a partial paint can safely
    // preserve every pixel outside its dirty rectangle. Surface recreation
    // deliberately invalidates this state until a complete frame is drawn.
    [[nodiscard]] bool retainedContentsReady(HWND window);
    HRESULT end();
    void resize();
    void reset();

    D2D1_SIZE_F sizeDip() const;
    float pixelsToDip(float pixels) const noexcept;
    ID2D1RenderTarget* target() const noexcept { return target_.Get(); }
    [[nodiscard]] ID3D11Device* d3dDevice() const noexcept
    {
        return native_.d3dDevice();
    }
    [[nodiscard]] ID3D11DeviceContext* immediateContext() const noexcept
    {
        return native_.immediateContext();
    }
    [[nodiscard]] ID2D1Bitmap1* retainedBacking() const noexcept
    {
        return partialBackingValid_ ? partialBacking_.Get() : nullptr;
    }
    [[nodiscard]] HANDLE readinessHandle() const noexcept
    {
        return native_.readinessHandle();
    }
    [[nodiscard]] SurfaceBindingKey bindingKey() const noexcept
    {
        return native_.bindingKey();
    }
    [[nodiscard]] SurfaceBackend backend() const noexcept { return backend_; }
    [[nodiscard]] FrameResult lastFrameResult() const noexcept
    {
        return lastFrameResult_;
    }
    [[nodiscard]] const FrameOutcome& lastFrameOutcome() const noexcept
    {
        return lastFrameOutcome_;
    }
    [[nodiscard]] std::uint64_t resourceEpoch() const noexcept
    {
        return resourceEpoch_;
    }
    ID2D1SolidColorBrush* brush(PanelColor color) const noexcept;

    void fill(
        const D2D1_RECT_F& rectangle,
        PanelColor color,
        float opacity = 1.0f);
    void fill(
        const D2D1_RECT_F& rectangle,
        UiColor color,
        float opacity = 1.0f);
    void line(
        float x1,
        float y1,
        float x2,
        float y2,
        PanelColor color,
        float width = 1.0f);
    void fillGeometry(ID2D1Geometry* geometry, UiColor color);
    void text(
        std::wstring_view value,
        const D2D1_RECT_F& rectangle,
        PanelText style,
        PanelColor color,
        float opacity = 1.0f);
    void text(
        std::wstring_view value,
        const D2D1_RECT_F& rectangle,
        PanelText style,
        UiColor color,
        float opacity = 1.0f);
    void glyph(
        PanelGlyph glyph,
        const D2D1_RECT_F& rectangle,
        PanelColor color);
    void repeatGlyph(
        bool repeatOne,
        const D2D1_RECT_F& rectangle,
        PanelColor color);
    void nekoLogo(const D2D1_RECT_F& rectangle);
    void godenWordmark(const D2D1_RECT_F& rectangle);

private:
    static constexpr std::size_t kPixelGlyphCount = 26U;

    bool beginFrame(HWND window, HDC paintDc, bool retain);
    bool ensure(HWND window);
    bool ensurePartialBacking();
    HRESULT finishRetainedDraw();
    void ensurePixelResources();
    void createFormats();
    void createBrushes();
    void syncTheme();
    HWND window_ = nullptr;
    float dpi_ = 96.0f;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    NativeD2DSurface native_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> partialBacking_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> normal_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> strong_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> right_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> center_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panel_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selected_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bright_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlight_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> amber_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> custom_;
    goden::pixel::PixelIconPainter pixelPainter_;
    std::array<goden::pixel::PixelIconPainter::Handle, kPixelGlyphCount>
        pixelGlyphs_{};
    goden::pixel::PixelIconPainter::Handle pixelWordmarkStandard_{};
    goden::pixel::PixelIconPainter::Handle pixelWordmarkCompact_{};
    goden::pixel::PixelIconPainter::Handle pixelMonogram_{};
    goden::pixel::PixelIconPainter::Handle pixelRepeatOne_{};
    float pixelPreparedDpi_ = 0.0f;
    std::uint64_t themeGeneration_ = 0;
    std::uint64_t appliedThemeResourceEpoch_ = 0;
    std::uint64_t resourceEpoch_ = 0;
    std::uint64_t sizeRevision_ = 0;
    std::uint64_t observedDeviceEpoch_ = 0;
    SurfaceBackend backend_ = SurfaceBackend::gdiFallback;
    FrameResult lastFrameResult_ = FrameResult::notReady;
    FrameOutcome lastFrameOutcome_{};
    bool partialClipActive_ = false;
    bool drawingRetained_ = false;
    bool partialBackingValid_ = false;
    bool qaPreparationTracking_ = false;
    std::uint32_t qaPreparationFailuresRemaining_ = 0;
    std::uint64_t qaPreparationAttempts_ = 0;
    std::uint64_t qaPreparationFaults_ = 0;
};

} // namespace nekorox::component
