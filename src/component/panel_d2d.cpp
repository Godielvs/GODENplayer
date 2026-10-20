#include "panel_d2d.h"
#include "telemetry_span.h"

#include "render_device_hub.h"
#include "theme_service.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nekorox::component {
namespace {

using Microsoft::WRL::ComPtr;

void configureFormat(IDWriteTextFormat* format, DWRITE_TEXT_ALIGNMENT alignment)
{
    if (format == nullptr) return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
}

D2D1_COLOR_F d2dColor(UiColor color) noexcept
{
    return D2D1::ColorF(
        color.red,
        color.green,
        color.blue,
        color.alpha);
}

D2D1_RECT_F centeredPixelSlot(
    const D2D1_RECT_F& rectangle, float width, float height) noexcept
{
    const auto centerX = (rectangle.left + rectangle.right) * 0.5f;
    const auto centerY = (rectangle.top + rectangle.bottom) * 0.5f;
    return D2D1::RectF(
        centerX - width * 0.5f,
        centerY - height * 0.5f,
        centerX + width * 0.5f,
        centerY + height * 0.5f);
}

} // namespace

bool PanelD2D::begin(HWND window, HDC paintDc)
{
    return beginFrame(window, paintDc, true);
}

bool PanelD2D::beginDirect(HWND window, HDC paintDc)
{
    return beginFrame(window, paintDc, false);
}

bool PanelD2D::beginFrame(HWND window, HDC paintDc, bool retain)
{
    (void)paintDc;
    if (!ensure(window)) {
        lastFrameResult_ = FrameResult::notReady;
        lastFrameOutcome_ = {lastFrameResult_, E_PENDING, E_PENDING,
            resourceEpoch_, sizeRevision_, partialBackingValid_};
        return false;
    }
    syncTheme();
    partialClipActive_ = false;
    drawingRetained_ = native_.active() && retain;
    if (!retain) {
        partialBackingValid_ = false;
        if (native_.active()) {
            native_.target()->SetTarget(native_.swapChainTarget());
        }
    }
    if (drawingRetained_) {
        // Every complete native frame must populate the retained backing,
        // including the very first frame after create/resize/device recovery.
        // Otherwise the next monitor-only paint copies an empty bitmap over
        // the header and leaves only the compact scope visible.
        if (ensurePartialBacking()) {
            native_.target()->SetTarget(partialBacking_.Get());
            partialBackingValid_ = false;
        } else {
            drawingRetained_ = false;
            native_.target()->SetTarget(native_.swapChainTarget());
        }
    }
    target_->BeginDraw();
    target_->Clear(d2dColor(
        UiDesignSystemV1::color(UiColorToken::canvas)));
    return true;
}

bool PanelD2D::beginPartial(
    HWND window,
    HDC paintDc,
    const RECT& dirtyPixels)
{
    (void)paintDc;
    if (!ensure(window)) {
        lastFrameResult_ = FrameResult::notReady;
        lastFrameOutcome_ = {lastFrameResult_, E_PENDING, E_PENDING,
            resourceEpoch_, sizeRevision_, partialBackingValid_};
        return false;
    }
    syncTheme();
    partialClipActive_ = false;
    drawingRetained_ = native_.active();
    if (drawingRetained_) {
        if (!ensurePartialBacking()) {
            lastFrameResult_ = FrameResult::notReady;
            lastFrameOutcome_ = {lastFrameResult_, E_PENDING, E_PENDING,
                resourceEpoch_, sizeRevision_, partialBackingValid_};
            return false;
        }
        native_.target()->SetTarget(partialBacking_.Get());
    }
    target_->BeginDraw();
    if (drawingRetained_ && !partialBackingValid_) {
        target_->Clear(d2dColor(
            UiDesignSystemV1::color(UiColorToken::canvas)));
    }
    const auto dirty = D2D1::RectF(
        pixelsToDip(static_cast<float>(dirtyPixels.left)),
        pixelsToDip(static_cast<float>(dirtyPixels.top)),
        pixelsToDip(static_cast<float>(dirtyPixels.right)),
        pixelsToDip(static_cast<float>(dirtyPixels.bottom)));
    target_->PushAxisAlignedClip(dirty, D2D1_ANTIALIAS_MODE_ALIASED);
    partialClipActive_ = true;
    target_->FillRectangle(dirty, background_.Get());
    return true;
}

bool PanelD2D::beginCachedOverlay(
    HWND window,
    HDC paintDc,
    const RECT& livePixels)
{
    (void)paintDc;
    if (!ensure(window) || !native_.active() || !partialBackingValid_ ||
            !partialBacking_ || !native_.swapChainTarget()) {
        return false;
    }
    const auto retainedSize = partialBacking_->GetPixelSize();
    const auto currentSize = native_.swapChainTarget()->GetPixelSize();
    if (retainedSize.width != currentSize.width ||
            retainedSize.height != currentSize.height) {
        partialBackingValid_ = false;
        return false;
    }
    syncTheme();
    partialClipActive_ = false;
    drawingRetained_ = false;
    auto* context = native_.target();
    context->SetTarget(native_.swapChainTarget());
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->BeginDraw();
    context->DrawBitmap(partialBacking_.Get(), nullptr, 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
    const auto live = D2D1::RectF(
        pixelsToDip(static_cast<float>(livePixels.left)),
        pixelsToDip(static_cast<float>(livePixels.top)),
        pixelsToDip(static_cast<float>(livePixels.right)),
        pixelsToDip(static_cast<float>(livePixels.bottom)));
    context->PushAxisAlignedClip(live, D2D1_ANTIALIAS_MODE_ALIASED);
    partialClipActive_ = true;
    // Erase all of the old live snapshot, not only rcPaint. Otherwise an
    // exposed subrectangle could restore stale scope pixels from the cache.
    context->FillRectangle(live, background_.Get());
    return true;
}

bool PanelD2D::beginCachedScene(HWND window, std::uint64_t expectedThemeGeneration)
{
    if (!ensure(window) || !native_.active() || !partialBackingValid_ ||
            !partialBacking_ || !native_.swapChainTarget()) return false;
    const auto retained = partialBacking_->GetPixelSize();
    const auto current = native_.swapChainTarget()->GetPixelSize();
    if (retained.width != current.width || retained.height != current.height) {
        partialBackingValid_ = false;
        return false;
    }
    syncTheme();
    if (themeGeneration_ != expectedThemeGeneration) return false;
    partialClipActive_ = false;
    drawingRetained_ = false;
    auto* context = native_.target();
    context->SetTarget(native_.swapChainTarget());
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->BeginDraw();
    context->DrawBitmap(partialBacking_.Get(), nullptr, 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
    return true;
}

bool PanelD2D::finishStaticSceneAndBeginOverlay()
{
    // Software/recovery keeps the complete scalar paint in its open draw.
    if (!native_.active() || !drawingRetained_) return true;
    const auto result = target_->EndDraw();
    if (FAILED(result)) {
        // Use the normal device-loss classifier before releasing the surface;
        // it invalidates the shared device hub and wakes even paused owners.
        const auto frame = native_.present(result);
        const FrameOutcome outcome{frame, result, native_.lastPresentHr(),
            native_.resourceEpoch(), native_.sizeRevision(), false};
        reset();
        lastFrameResult_ = frame;
        lastFrameOutcome_ = outcome;
        return false;
    }
    partialBackingValid_ = true;
    drawingRetained_ = false;
    auto* context = native_.target();
    context->SetTarget(native_.swapChainTarget());
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->BeginDraw();
    context->DrawBitmap(partialBacking_.Get(), nullptr, 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
    return true;
}

HRESULT PanelD2D::end()
{
    TelemetrySpan span(TelemetrySurface::graphics,203,reinterpret_cast<std::uintptr_t>(window_));
    if (!target_) return E_FAIL;
    if (partialClipActive_) {
        target_->PopAxisAlignedClip();
        partialClipActive_ = false;
    }
    const auto result = target_->EndDraw();
    lastFrameOutcome_.drawHr = result;
    if (native_.active()) {
        if (drawingRetained_) partialBackingValid_ = SUCCEEDED(result);
        const auto presentDrawResult = drawingRetained_ && SUCCEEDED(result)
            ? finishRetainedDraw() : result;
        drawingRetained_ = false;
        lastFrameResult_ = native_.present(presentDrawResult);
        resourceEpoch_ = native_.resourceEpoch();
        sizeRevision_ = native_.sizeRevision();
        lastFrameOutcome_ = {lastFrameResult_, result,
            native_.lastPresentHr(), resourceEpoch_, sizeRevision_,
            partialBackingValid_};
        switch (lastFrameResult_) {
        case FrameResult::accepted: return S_OK;
        case FrameResult::occluded: return DXGI_STATUS_OCCLUDED;
        case FrameResult::notReady: return DXGI_ERROR_WAS_STILL_DRAWING;
        case FrameResult::deviceLost: {
            const auto saved=lastFrameOutcome_;reset();lastFrameOutcome_=saved;lastFrameResult_=saved.state;
            return D2DERR_RECREATE_TARGET;
        }
        case FrameResult::failed: {
            const auto saved=lastFrameOutcome_;reset();lastFrameOutcome_=saved;lastFrameResult_=saved.state;
            return E_FAIL;
        }
        }
    }
    lastFrameResult_ = SUCCEEDED(result)
        ? FrameResult::accepted : FrameResult::failed;
    lastFrameOutcome_ = {lastFrameResult_, result, result,
        resourceEpoch_, sizeRevision_, SUCCEEDED(result)};
    // Any failed EndDraw leaves the target's state undefined. Recreating only
    // for D2DERR_RECREATE_TARGET stranded panels after driver-reset and remote
    // session errors that use different HRESULTs.
    if(FAILED(result)){const auto saved=lastFrameOutcome_;reset();lastFrameOutcome_=saved;lastFrameResult_=saved.state;}
    return result;
}

bool PanelD2D::retainedContentsReady(HWND window)
{
    if (!ensure(window)) return false;
    if (native_.active()) {
        return partialBacking_ != nullptr && partialBackingValid_;
    }
    return lastFrameResult_ == FrameResult::accepted;
}

void PanelD2D::resize()
{
    if (!target_ || window_ == nullptr) return;
    if (native_.active()) {
        const auto previousSizeRevision = native_.sizeRevision();
        if (!native_.resize()) reset();
        else {
            resourceEpoch_ = native_.resourceEpoch();
            sizeRevision_ = native_.sizeRevision();
            if (sizeRevision_ != previousSizeRevision) {
                partialBacking_.Reset();
                partialBackingValid_ = false;
                drawingRetained_ = false;
            }
        }
        return;
    }
    RECT rectangle{};
    GetClientRect(window_, &rectangle);
    const auto width = static_cast<UINT32>(std::max(
        0L, rectangle.right - rectangle.left));
    const auto height = static_cast<UINT32>(std::max(
        0L, rectangle.bottom - rectangle.top));
    if (width == 0 || height == 0) return;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> hwndTarget;
    if (FAILED(target_.As(&hwndTarget)) ||
        FAILED(hwndTarget->Resize(D2D1::SizeU(width, height)))) reset();
}

void PanelD2D::reset()
{
    textRendering_.reset();
    partialClipActive_ = false;
    drawingRetained_ = false;
    partialBackingValid_ = false;
    partialBacking_.Reset();
    target_.Reset();
    native_.reset();
    background_.Reset();
    panel_.Reset();
    selected_.Reset();
    border_.Reset();
    text_.Reset();
    muted_.Reset();
    bright_.Reset();
    accent_.Reset();
    secondary_.Reset();
    highlight_.Reset();
    amber_.Reset();
    custom_.Reset();
    themeGeneration_ = 0;
    appliedThemeResourceEpoch_ = 0;
    observedDeviceEpoch_ = 0;
    backend_ = SurfaceBackend::gdiFallback;
    lastFrameResult_ = FrameResult::notReady;
    lastFrameOutcome_ = {FrameResult::notReady, E_PENDING, E_PENDING,
        resourceEpoch_, sizeRevision_, false};
}

bool PanelD2D::ensurePartialBacking()
{
    auto* context = native_.target();
    if (context == nullptr) return false;
    RECT client{};
    GetClientRect(window_, &client);
    const auto size = D2D1::SizeU(
        static_cast<UINT32>(std::max(1L, client.right - client.left)),
        static_cast<UINT32>(std::max(1L, client.bottom - client.top)));
    if (partialBacking_) {
        const auto existing = partialBacking_->GetPixelSize();
        if (existing.width == size.width && existing.height == size.height)
            return true;
        partialBacking_.Reset();
        partialBackingValid_ = false;
    }
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        dpi_, dpi_);
    return SUCCEEDED(context->CreateBitmap(size, nullptr, 0, &properties,
        partialBacking_.ReleaseAndGetAddressOf()));
}

HRESULT PanelD2D::finishRetainedDraw()
{
    auto* context = native_.target();
    auto* swapTarget = native_.swapChainTarget();
    if (context == nullptr || swapTarget == nullptr || !partialBacking_)
        return E_FAIL;
    context->SetTarget(swapTarget);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->BeginDraw();
    context->DrawBitmap(partialBacking_.Get(), nullptr, 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
    return context->EndDraw();
}

D2D1_SIZE_F PanelD2D::sizeDip() const
{
    RECT rectangle{};
    if (window_ != nullptr) GetClientRect(window_, &rectangle);
    return D2D1::SizeF(
        pixelsToDip(static_cast<float>(rectangle.right - rectangle.left)),
        pixelsToDip(static_cast<float>(rectangle.bottom - rectangle.top)));
}

float PanelD2D::pixelsToDip(float pixels) const noexcept
{
    return pixels * 96.0f / std::max(1.0f, dpi_);
}

ID2D1SolidColorBrush* PanelD2D::brush(PanelColor color) const noexcept
{
    switch (color) {
    case PanelColor::background: return background_.Get();
    case PanelColor::panel: return panel_.Get();
    case PanelColor::selected: return selected_.Get();
    case PanelColor::border: return border_.Get();
    case PanelColor::text: return text_.Get();
    case PanelColor::muted: return muted_.Get();
    case PanelColor::bright: return bright_.Get();
    case PanelColor::accent: return accent_.Get();
    case PanelColor::secondary: return secondary_.Get();
    case PanelColor::highlight: return highlight_.Get();
    case PanelColor::amber: return amber_.Get();
    }
    return text_.Get();
}

void PanelD2D::fill(
    const D2D1_RECT_F& rectangle,
    PanelColor color,
    float opacity)
{
    auto* selectedBrush = brush(color);
    if (selectedBrush == nullptr) return;
    const auto previousOpacity = selectedBrush->GetOpacity();
    selectedBrush->SetOpacity(
        std::clamp(opacity, 0.0f, 1.0f));
    target_->FillRectangle(rectangle, selectedBrush);
    selectedBrush->SetOpacity(previousOpacity);
}

void PanelD2D::fill(
    const D2D1_RECT_F& rectangle,
    UiColor color,
    float opacity)
{
    if (!target_ || !custom_) return;
    custom_->SetColor(d2dColor(color));
    const auto previousOpacity = custom_->GetOpacity();
    custom_->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
    target_->FillRectangle(rectangle, custom_.Get());
    custom_->SetOpacity(previousOpacity);
}

void PanelD2D::line(
    float x1,
    float y1,
    float x2,
    float y2,
    PanelColor color,
    float width)
{
    target_->DrawLine(
        D2D1::Point2F(x1, y1),
        D2D1::Point2F(x2, y2),
        brush(color),
        width);
}

void PanelD2D::fillGeometry(ID2D1Geometry* geometry, UiColor color)
{
    if (target_ == nullptr || geometry == nullptr || custom_ == nullptr) return;
    custom_->SetColor(d2dColor(color));
    target_->FillGeometry(geometry, custom_.Get());
}

void PanelD2D::text(
    std::wstring_view value,
    const D2D1_RECT_F& rectangle,
    PanelText style,
    PanelColor color,
    float opacity)
{
    IDWriteTextFormat* format = normal_.Get();
    switch (style) {
    case PanelText::caption: format = small_.Get(); break;
    case PanelText::normal: format = normal_.Get(); break;
    case PanelText::strong: format = strong_.Get(); break;
    case PanelText::right: format = right_.Get(); break;
    case PanelText::center: format = center_.Get(); break;
    }
    if (format == nullptr || value.empty()) return;
    auto* textBrush = brush(color);
    const auto previousOpacity = textBrush->GetOpacity();
    textBrush->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
    target_->DrawTextW(
        value.data(),
        static_cast<UINT32>(value.size()),
        format,
        rectangle,
        textBrush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);
    textBrush->SetOpacity(previousOpacity);
}

void PanelD2D::text(
    std::wstring_view value,
    const D2D1_RECT_F& rectangle,
    PanelText style,
    UiColor color,
    float opacity)
{
    if (custom_ == nullptr) return;
    custom_->SetColor(d2dColor(color));
    IDWriteTextFormat* format = normal_.Get();
    switch (style) {
    case PanelText::caption: format = small_.Get(); break;
    case PanelText::normal: format = normal_.Get(); break;
    case PanelText::strong: format = strong_.Get(); break;
    case PanelText::right: format = right_.Get(); break;
    case PanelText::center: format = center_.Get(); break;
    }
    if (format == nullptr || value.empty()) return;
    custom_->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
    target_->DrawTextW(
        value.data(),
        static_cast<UINT32>(value.size()),
        format,
        rectangle,
        custom_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);
    custom_->SetOpacity(1.0f);
}

void PanelD2D::glyph(
    PanelGlyph glyphValue,
    const D2D1_RECT_F& rectangle,
    PanelColor color)
{
    static_assert(static_cast<std::size_t>(PanelGlyph::menuSubmenu) + 1U ==
        kPixelGlyphCount);
    if (!target_) return;
    const auto index = static_cast<std::size_t>(glyphValue);
    if (index >= pixelGlyphs_.size()) return;
    const auto slot = centeredPixelSlot(rectangle, 16.0f, 16.0f);
    (void)pixelPainter_.drawPrepared(
        target_.Get(),
        pixelGlyphs_[index],
        {slot.left, slot.top, slot.right, slot.bottom},
        brush(color));
}

void PanelD2D::repeatGlyph(
    bool repeatOne,
    const D2D1_RECT_F& rectangle,
    PanelColor color)
{
    if (!repeatOne) {
        glyph(PanelGlyph::repeat, rectangle, color);
        return;
    }
    if (!target_) return;
    const auto slot = centeredPixelSlot(rectangle, 16.0f, 16.0f);
    (void)pixelPainter_.drawPrepared(
        target_.Get(), pixelRepeatOne_,
        {slot.left, slot.top, slot.right, slot.bottom}, brush(color));
}

void PanelD2D::nekoLogo(const D2D1_RECT_F& rectangle)
{
    if (!target_) return;
    const auto slot = centeredPixelSlot(rectangle, 16.0f, 16.0f);
    (void)pixelPainter_.drawPrepared(
        target_.Get(),
        pixelMonogram_,
        {slot.left, slot.top, slot.right, slot.bottom},
        brush(PanelColor::bright));
}

void PanelD2D::godenWordmark(const D2D1_RECT_F& rectangle)
{
    if (!target_ || rectangle.right <= rectangle.left ||
            rectangle.bottom <= rectangle.top) return;
    const bool standard = rectangle.right - rectangle.left >= 128.0f &&
        rectangle.bottom - rectangle.top >= 20.0f;
    const auto width = standard ? 128.0f : 92.0f;
    const auto height = standard ? 20.0f : 14.0f;
    const auto slot = centeredPixelSlot(rectangle, width, height);
    (void)pixelPainter_.drawPrepared(
        target_.Get(),
        standard ? pixelWordmarkStandard_ : pixelWordmarkCompact_,
        {slot.left, slot.top, slot.right, slot.bottom},
        brush(PanelColor::accent),
        brush(PanelColor::secondary));
}

bool PanelD2D::ensure(HWND window)
{
    if (!ensureBackend(window)) return false;
    if (textRendering_.apply(target_.Get(), writeFactory_.Get(),
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST))) {
        // A retained bitmap still contains the previous monitor's raster.
        partialBackingValid_ = false;
    }
    return true;
}

bool PanelD2D::ensureBackend(HWND window)
{
    if (qaPreparationTracking_) {
        ++qaPreparationAttempts_;
        if (qaPreparationFailuresRemaining_ != 0) {
            --qaPreparationFailuresRemaining_;
            ++qaPreparationFaults_;
            reset();
            return false;
        }
    }
    const auto currentDpi = window != nullptr
        ? static_cast<float>((std::max)(1U, GetDpiForWindow(window)))
        : 96.0f;
    if (window_ != window) {
        window_ = window;
        dpi_ = currentDpi;
        reset();
    } else if (std::abs(dpi_ - currentDpi) > 0.5f) {
        // A target is tied to the DPI at which it was created.  WM_DPICHANGED
        // handlers reset the target, but the HWND itself remains identical;
        // refresh the cached scale here as well so rendering and hit testing
        // cannot diverge after a cross-monitor move.
        dpi_ = currentDpi;
        reset();
    }
    if (!factory_) {
        factory_ = RenderDeviceHub::instance().d2dFactory();
    }
    ensurePixelResources();
    if (!writeFactory_) {
        writeFactory_ = RenderDeviceHub::instance().writeFactory();
    }
    createFormats();
    const auto hubEpoch = RenderDeviceHub::instance().recoveryEpoch();
    if (target_) {
        if (native_.active()) {
            if (native_.ensure(window_, dpi_)) {
                resourceEpoch_ = native_.resourceEpoch();
                sizeRevision_ = native_.sizeRevision();
                if (target_.Get() != native_.target()) {
                    target_ = native_.target();
                    backend_ = native_.backend();
                    partialBacking_.Reset();
                    partialBackingValid_ = false;
                    createBrushes();
                }
                observedDeviceEpoch_ = native_.deviceEpoch();
                return true;
            }
            reset();
            factory_ = RenderDeviceHub::instance().d2dFactory();
            writeFactory_ = RenderDeviceHub::instance().writeFactory();
            ensurePixelResources();
            createFormats();
        } else if (observedDeviceEpoch_ == hubEpoch) {
            return true;
        }
        reset();
        factory_ = RenderDeviceHub::instance().d2dFactory();
        writeFactory_ = RenderDeviceHub::instance().writeFactory();
        ensurePixelResources();
        createFormats();
    }
    if (!factory_ || window_ == nullptr) return false;

    observedDeviceEpoch_ = hubEpoch;
    if (native_.ensure(window_, dpi_)) {
        target_ = native_.target();
        backend_ = native_.backend();
        resourceEpoch_ = native_.resourceEpoch();
        sizeRevision_ = native_.sizeRevision();
        observedDeviceEpoch_ = native_.deviceEpoch();
        createBrushes();
        return true;
    }

    RECT rectangle{};
    GetClientRect(window_, &rectangle);
    const auto pixelSize = D2D1::SizeU(
        static_cast<UINT32>(std::max(
            1L, rectangle.right - rectangle.left)),
        static_cast<UINT32>(std::max(
            1L, rectangle.bottom - rectangle.top)));
    // ID2D1HwndRenderTarget creates an implicit D3D device synchronously.
    // On the NVIDIA user-mode driver that call can remain inside
    // D3D11CreateDevice indefinitely after a surface reset, freezing the
    // foobar UI thread (transport, caption drag and every panel together).
    // These native chrome/list surfaces are small retained-mode surfaces;
    // keep the explicitly managed Spectrum/CRT paths on the shared GPU hub,
    // and render this UI tier in software so device creation and device loss
    // can never stall interaction.
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE),
        dpi_, dpi_);
    const auto windowProperties = D2D1::HwndRenderTargetProperties(
        window_,
        pixelSize,
        static_cast<D2D1_PRESENT_OPTIONS>(
            D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS |
            D2D1_PRESENT_OPTIONS_IMMEDIATELY));
    ComPtr<ID2D1HwndRenderTarget> softwareTarget;
    if (FAILED(factory_->CreateHwndRenderTarget(
            properties,
            windowProperties,
            softwareTarget.ReleaseAndGetAddressOf()))) {
        return false;
    }
    target_ = softwareTarget;
    ++resourceEpoch_;
    ++sizeRevision_;
    backend_ = SurfaceBackend::gdiFallback;
    createBrushes();
    return true;
}

void PanelD2D::ensurePixelResources()
{
    if (!factory_) return;
    pixelPainter_.bindFactory(factory_.Get());
    if (std::abs(pixelPreparedDpi_ - dpi_) <= 0.01f &&
            pixelGlyphs_.front().generation != 0 &&
            pixelWordmarkStandard_.generation != 0 &&
            pixelWordmarkCompact_.generation != 0 &&
            pixelMonogram_.generation != 0 &&
            pixelRepeatOne_.generation != 0) return;

    auto prepare = [this](const goden::pixel::GlyphView& glyph,
                          goden::pixel::PixelIconPainter::Handle& handle) {
        const auto width = static_cast<std::int32_t>(std::lround(
            static_cast<float>(glyph.width) * dpi_ / 96.0f));
        const auto height = static_cast<std::int32_t>(std::lround(
            static_cast<float>(glyph.height) * dpi_ / 96.0f));
        if (FAILED(pixelPainter_.prepare(glyph,
                std::max<std::int32_t>(glyph.width, width),
                std::max<std::int32_t>(glyph.height, height), handle))) {
            handle = {};
        }
    };

    for (std::size_t index = 0; index < pixelGlyphs_.size(); ++index) {
        const auto glyph = static_cast<PanelGlyph>(index);
        if (const auto* art = goden::pixel::icon(pixelIconFor(glyph), 16))
            prepare(*art, pixelGlyphs_[index]);
        else
            pixelGlyphs_[index] = {};
    }
    prepare(goden::pixel::kWordmarkStandard, pixelWordmarkStandard_);
    prepare(goden::pixel::kWordmarkCompact, pixelWordmarkCompact_);
    prepare(goden::pixel::kMonogram16, pixelMonogram_);
    if (const auto* repeatOne = goden::pixel::icon(
            goden::pixel::IconId::RepeatOne, 16)) {
        prepare(*repeatOne, pixelRepeatOne_);
    } else {
        pixelRepeatOne_ = {};
    }
    pixelPreparedDpi_ = dpi_;
}

void PanelD2D::createFormats()
{
    if (!writeFactory_ || normal_) return;
    auto create = [&](UiFontRole role, ComPtr<IDWriteTextFormat>& output) {
        const auto& descriptor = UiDesignSystemV1::font(role);
        const auto family = UiDesignSystemV1::resolveFontFamily(
            writeFactory_.Get(),
            descriptor);
        writeFactory_->CreateTextFormat(
            family.data(),
            nullptr,
            descriptor.weight,
            descriptor.style,
            descriptor.stretch,
            descriptor.sizeDip,
            L"en-us",
            output.ReleaseAndGetAddressOf());
    };
    create(UiFontRole::caption, small_);
    create(UiFontRole::body, normal_);
    create(UiFontRole::bodyStrong, strong_);
    create(UiFontRole::time, right_);
    create(UiFontRole::caption, center_);
    configureFormat(small_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
    configureFormat(normal_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
    configureFormat(strong_.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
    configureFormat(right_.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    configureFormat(center_.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    const auto applyEllipsis = [&](IDWriteTextFormat* format) {
        if (format == nullptr) return;
        DWRITE_TRIMMING trimming{
            DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(
                format, sign.ReleaseAndGetAddressOf()))) {
            format->SetTrimming(&trimming, sign.Get());
        }
    };
    applyEllipsis(small_.Get());
    applyEllipsis(normal_.Get());
    applyEllipsis(strong_.Get());
    applyEllipsis(right_.Get());
    applyEllipsis(center_.Get());
}

void PanelD2D::createBrushes()
{
    // A recreated target owns new brushes even when the semantic palette did
    // not advance. Force the current snapshot onto this resource epoch.
    appliedThemeResourceEpoch_ = 0;
    auto create = [&](UiColorToken token,
                      ComPtr<ID2D1SolidColorBrush>& output) {
        target_->CreateSolidColorBrush(
            d2dColor(UiDesignSystemV1::color(token)),
            output.ReleaseAndGetAddressOf());
    };
    create(UiColorToken::canvas, background_);
    create(UiColorToken::surface, panel_);
    create(UiColorToken::surfaceSelected, selected_);
    create(UiColorToken::separator, border_);
    create(UiColorToken::text, text_);
    create(UiColorToken::textMuted, muted_);
    create(UiColorToken::textBright, bright_);
    create(UiColorToken::accentPrimary, accent_);
    create(UiColorToken::accentSecondary, secondary_);
    create(UiColorToken::focus, highlight_);
    target_->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.63f, 0.24f),
        amber_.ReleaseAndGetAddressOf());
    create(UiColorToken::text, custom_);
}

void PanelD2D::syncTheme()
{
    const auto theme = ThemeService::instance().snapshot();
    if (theme.generation == themeGeneration_ &&
        appliedThemeResourceEpoch_ == resourceEpoch_) return;
    themeGeneration_ = theme.generation;
    appliedThemeResourceEpoch_ = resourceEpoch_;
    if (accent_) {
        accent_->SetColor(D2D1::ColorF(
            theme.accent.red,
            theme.accent.green,
            theme.accent.blue));
    }
    if (secondary_) {
        secondary_->SetColor(D2D1::ColorF(
            theme.secondary.red,
            theme.secondary.green,
            theme.secondary.blue));
    }
    if (highlight_) {
        highlight_->SetColor(D2D1::ColorF(
            theme.highlight.red,
            theme.highlight.green,
            theme.highlight.blue));
    }
    if (selected_) {
        const auto base = interpolateLinearColor(
            UiDesignSystemV1::color(UiColorToken::surfaceRaised),
            UiDesignSystemV1::color(UiColorToken::surfaceSelected),
            0.72f);
        const UiColor accent{
            theme.accent.red,
            theme.accent.green,
            theme.accent.blue,
            1.0f,
        };
        selected_->SetColor(d2dColor(interpolateLinearColor(
            base,
            accent,
            0.012f)));
    }
}

} // namespace nekorox::component
