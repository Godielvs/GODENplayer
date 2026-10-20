#include "listen_visual_renderer.h"
#include "device_failure.h"
#include "rollout_flags.h"

#include "render_device_hub.h"

#include "frame_clock.h"
#include "text_util.h"
#include "ui_design_system.h"
#include "ui_preferences.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <numbers>
#include <optional>
#include <string>

namespace nekorox::component {

namespace {

std::atomic<std::uint64_t> gNextListenSurfaceLifetime{1};

constexpr D2D1_COLOR_F designColor(UiColor color) noexcept
{
    return {
        color.red,
        color.green,
        color.blue,
        color.alpha,
    };
}

const auto kBackground = designColor(
    UiDesignSystemV1::color(UiColorToken::canvas));
const auto kPanel = designColor(
    UiDesignSystemV1::color(UiColorToken::surface));
const auto kPanelAlternate = designColor(
    UiDesignSystemV1::color(UiColorToken::surfaceRaised));
const auto kBorder = designColor(
    UiDesignSystemV1::color(UiColorToken::separator));
constexpr D2D1_COLOR_F kGrid{0.09f, 0.105f, 0.11f, 1.0f};
const auto kText = designColor(
    UiDesignSystemV1::color(UiColorToken::text));
const auto kBright = designColor(
    UiDesignSystemV1::color(UiColorToken::textBright));
const auto kMuted = designColor(
    UiDesignSystemV1::color(UiColorToken::textMuted));
constexpr D2D1_COLOR_F kMagenta{0.78f, 0.10f, 0.56f, 1.0f};
constexpr D2D1_COLOR_F kGreen{0.35f, 0.78f, 0.42f, 1.0f};
// A ribbon occupies at most 70% of its lane. The previous 112% envelope made
// loud masters cross their separators and visually merge adjacent metrics.
constexpr float kMusicMapMaximumHalfLane = 0.35f;
static_assert(kMusicMapMaximumHalfLane < 0.5f);

float width(const D2D1_RECT_F& rectangle) noexcept
{
    return std::max(0.0f, rectangle.right - rectangle.left);
}

float height(const D2D1_RECT_F& rectangle) noexcept
{
    return std::max(0.0f, rectangle.bottom - rectangle.top);
}

D2D1_RECT_F spectrumPlotFor(const D2D1_RECT_F& frame) noexcept
{
    // The scale owns its own gutters. Labels are deliberately outside this
    // rectangle so the plot clip can never cut them or let bars cover them.
    constexpr float kTitle = 30.0f;
    constexpr float kTopGap = 8.0f;
    constexpr float kLeftGutter = 44.0f;
    constexpr float kRightGutter = 8.0f;
    constexpr float kBottomGutter = 24.0f;
    const auto plot = D2D1::RectF(
        std::min(frame.right, frame.left + kLeftGutter),
        std::min(frame.bottom, frame.top + kTitle + kTopGap),
        std::max(frame.left, frame.right - kRightGutter),
        std::max(frame.top, frame.bottom - kBottomGutter));
    return validListenRect(plot) ? plot : D2D1_RECT_F{};
}

void formatDurationText(
    double seconds,
    wchar_t* buffer,
    std::size_t capacity) noexcept
{
    if (buffer == nullptr || capacity == 0) return;
    if (!std::isfinite(seconds) || seconds < 0.0) {
        wcsncpy_s(buffer, capacity, L"--:--", _TRUNCATE);
        return;
    }
    const auto total = static_cast<unsigned long long>(seconds + 0.5);
    const auto hours = total / 3600ULL;
    const auto minutes = (total / 60ULL) % 60ULL;
    const auto remaining = total % 60ULL;
    if (hours > 0) {
        std::swprintf(buffer, capacity, L"%llu:%02llu:%02llu",
            hours, minutes, remaining);
    } else {
        std::swprintf(buffer, capacity, L"%llu:%02llu",
            minutes, remaining);
    }
}

std::wstring durationText(double seconds)
{
    wchar_t buffer[32]{};
    formatDurationText(seconds, buffer, std::size(buffer));
    return buffer;
}

std::wstring percentText(float value)
{
    wchar_t buffer[16]{};
    std::swprintf(buffer, std::size(buffer), L"%d%%",
        static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * 100.0f)));
    return buffer;
}

double monotonicSeconds() noexcept
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
        Clock::now().time_since_epoch()).count();
}

D2D1_COLOR_F mix(
    const D2D1_COLOR_F& left,
    const D2D1_COLOR_F& right,
    float amount) noexcept
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    return D2D1::ColorF(
        left.r + (right.r - left.r) * amount,
        left.g + (right.g - left.g) * amount,
        left.b + (right.b - left.b) * amount,
        left.a + (right.a - left.a) * amount);
}

double spectrumNormalizedPower(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (value <= 0.0f) return 0.0;
    constexpr std::size_t kTableSteps = 1024U;
    static const auto table = [] {
        std::array<double, kTableSteps + 1U> values{};
        for (std::size_t index = 0; index <= kTableSteps; ++index) {
            const auto normalized = static_cast<double>(index) /
                static_cast<double>(kTableSteps);
            const auto decibels = normalized * 66.0 - 72.0;
            values[index] = std::pow(10.0, decibels / 10.0);
        }
        return values;
    }();
    const auto position = static_cast<double>(value) *
        static_cast<double>(kTableSteps);
    const auto lower = std::min<std::size_t>(
        kTableSteps - 1U,
        static_cast<std::size_t>(position));
    return std::lerp(
        table[lower], table[lower + 1U],
        position - static_cast<double>(lower));
}

std::uint32_t spectrogramColor(float value) noexcept
{
    value = std::pow(std::clamp((value - 0.035f) / 0.965f, 0.0f, 1.0f), 1.18f);
    D2D1_COLOR_F colorValue{};
    if (value < 0.28f) {
        colorValue = mix(
            D2D1::ColorF(0.015f, 0.016f, 0.020f),
            D2D1::ColorF(0.16f, 0.025f, 0.15f), value / 0.28f);
    } else if (value < 0.72f) {
        colorValue = mix(
            D2D1::ColorF(0.16f, 0.025f, 0.15f),
            D2D1::ColorF(0.0f, 0.55f, 0.52f), (value - 0.28f) / 0.44f);
    } else {
        colorValue = mix(
            D2D1::ColorF(0.0f, 0.55f, 0.52f),
            D2D1::ColorF(0.66f, 0.91f, 0.90f), (value - 0.72f) / 0.28f);
    }
    const auto red = static_cast<std::uint32_t>(
        std::round(std::clamp(colorValue.r, 0.0f, 1.0f) * 255.0f));
    const auto green = static_cast<std::uint32_t>(
        std::round(std::clamp(colorValue.g, 0.0f, 1.0f) * 255.0f));
    const auto blue = static_cast<std::uint32_t>(
        std::round(std::clamp(colorValue.b, 0.0f, 1.0f) * 255.0f));
    return 0xFF000000U | (red << 16U) | (green << 8U) | blue;
}

bool writeBgraBitmap(
    const std::filesystem::path& path,
    std::uint32_t widthValue,
    std::uint32_t heightValue,
    const void* pixels)
{
    const auto rowBytes64 = static_cast<std::uint64_t>(widthValue) * 4U;
    const auto pixelBytes64 = rowBytes64 * heightValue;
    if (pixels == nullptr || rowBytes64 > MAXDWORD ||
        pixelBytes64 > MAXDWORD) {
        return false;
    }
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits +
        static_cast<DWORD>(pixelBytes64);
    BITMAPINFOHEADER imageHeader{};
    imageHeader.biSize = sizeof(imageHeader);
    imageHeader.biWidth = static_cast<LONG>(widthValue);
    imageHeader.biHeight = -static_cast<LONG>(heightValue);
    imageHeader.biPlanes = 1;
    imageHeader.biBitCount = 32;
    imageHeader.biCompression = BI_RGB;
    imageHeader.biSizeImage = static_cast<DWORD>(pixelBytes64);

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto write = [&](const void* data, DWORD size) {
        DWORD written = 0;
        return WriteFile(file, data, size, &written, nullptr) != FALSE &&
            written == size;
    };
    const auto success = write(&fileHeader, sizeof(fileHeader)) &&
        write(&imageHeader, sizeof(imageHeader)) &&
        write(pixels, static_cast<DWORD>(pixelBytes64));
    CloseHandle(file);
    return success;
}

bool compositeBgraOverlay(
    std::uint8_t* destination,
    std::uint32_t destinationWidth,
    std::uint32_t destinationHeight,
    const ListenBgraOverlay& overlay)
{
    const auto rectangleWidth =
        overlay.pixelRect.right - overlay.pixelRect.left;
    const auto rectangleHeight =
        overlay.pixelRect.bottom - overlay.pixelRect.top;
    const auto requiredBytes =
        static_cast<std::uint64_t>(overlay.width) *
        overlay.height * 4U;
    if (destination == nullptr ||
        destinationWidth == 0 || destinationHeight == 0 ||
        rectangleWidth <= 0 || rectangleHeight <= 0 ||
        overlay.width == 0 || overlay.height == 0 ||
        requiredBytes > overlay.bgra.size()) {
        return false;
    }

    const auto left = std::clamp<LONG>(
        overlay.pixelRect.left, 0, static_cast<LONG>(destinationWidth));
    const auto top = std::clamp<LONG>(
        overlay.pixelRect.top, 0, static_cast<LONG>(destinationHeight));
    const auto right = std::clamp<LONG>(
        overlay.pixelRect.right, 0, static_cast<LONG>(destinationWidth));
    const auto bottom = std::clamp<LONG>(
        overlay.pixelRect.bottom, 0, static_cast<LONG>(destinationHeight));
    if (right <= left || bottom <= top) return false;

    const auto destinationStride =
        static_cast<std::size_t>(destinationWidth) * 4U;
    const auto sourceStride =
        static_cast<std::size_t>(overlay.width) * 4U;
    for (LONG y = top; y < bottom; ++y) {
        const auto sourceY = std::min<std::uint32_t>(
            overlay.height - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(y - overlay.pixelRect.top) *
                    overlay.height) /
                static_cast<std::uint32_t>(rectangleHeight)));
        for (LONG x = left; x < right; ++x) {
            const auto sourceX = std::min<std::uint32_t>(
                overlay.width - 1U,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(x - overlay.pixelRect.left) *
                        overlay.width) /
                    static_cast<std::uint32_t>(rectangleWidth)));
            const auto* source = overlay.bgra.data() +
                static_cast<std::size_t>(sourceY) * sourceStride +
                static_cast<std::size_t>(sourceX) * 4U;
            auto* target = destination +
                static_cast<std::size_t>(y) * destinationStride +
                static_cast<std::size_t>(x) * 4U;
            const auto alpha = static_cast<std::uint32_t>(source[3]);
            const auto inverse = 255U - alpha;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                target[channel] = static_cast<std::uint8_t>(
                    (static_cast<std::uint32_t>(source[channel]) * alpha +
                        static_cast<std::uint32_t>(target[channel]) * inverse +
                        127U) /
                    255U);
            }
            target[3] = 255U;
        }
    }
    return true;
}

} // namespace

FrameResult ListenVisualRenderer::draw(
    HWND window, const ListenRenderState& state)
{
    if (!ensureResources(window)) return FrameResult::failed;
    // The owner consumed the readiness permit before entering WM_PAINT. There
    // is intentionally no second HANDLE poll here: demand and GPU credit have
    // one authority, so an accepted revision cannot be skipped ambiguously.
    const auto result = drawFrame(state);
    artworkTransition_.presented(artworkTransition_.pendingPresentation(), result == FrameResult::accepted);
    glyphFlowRenderer_.presented(result == FrameResult::accepted);
    return result;
}

bool ListenVisualRenderer::drawToDc(
    HDC deviceContext,
    const RECT& pixelBounds,
    float dpi,
    const ListenRenderState& state)
{
    if (deviceContext == nullptr ||
        pixelBounds.right <= pixelBounds.left ||
        pixelBounds.bottom <= pixelBounds.top) {
        return false;
    }
    ListenVisualRenderer capture;
    if (!capture.ensureDeviceIndependentResources()) return false;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget;
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED),
        std::max(1.0f, dpi),
        std::max(1.0f, dpi));
    if (FAILED(capture.factory_->CreateDCRenderTarget(
            &properties, dcTarget.ReleaseAndGetAddressOf())) ||
        FAILED(dcTarget->BindDC(deviceContext, &pixelBounds))) {
        return false;
    }
    capture.target_ = dcTarget;
    if (FAILED(capture.target_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White),
            capture.brush_.ReleaseAndGetAddressOf()))) {
        return false;
    }
    // Deterministic exports render the currently presented artwork at full
    // opacity. The live renderer owns the semantic transition clock; creating
    // a fresh off-screen renderer must not restart a first-image fade.
    if (state.activeSpace == 0 && state.track != nullptr && state.artwork) {
        capture.prepareArtwork(state.artworkIdentity, state.artwork);
        capture.artworkTransition_.settle();
    }
    return capture.drawFrame(state) == FrameResult::accepted;
}

bool ListenVisualRenderer::captureBmp(
    const std::filesystem::path& path,
    std::uint32_t pixelWidth,
    std::uint32_t pixelHeight,
    float dpi,
    const ListenRenderState& state,
    const ListenBgraOverlay* overlay)
{
    if (pixelWidth == 0 || pixelHeight == 0 ||
        pixelWidth > static_cast<std::uint32_t>(LONG_MAX) ||
        pixelHeight > static_cast<std::uint32_t>(LONG_MAX)) {
        return false;
    }
    HDC memory = CreateCompatibleDC(nullptr);
    if (memory == nullptr) return false;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(pixelWidth);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(pixelHeight);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &bitmapInfo, DIB_RGB_COLORS,
        &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        DeleteDC(memory);
        return false;
    }
    const auto previous = SelectObject(memory, bitmap);
    const auto byteCount = static_cast<std::size_t>(pixelWidth) *
        pixelHeight * 4U;
    std::memset(pixels, 0, byteCount);
    const RECT bounds{
        0, 0, static_cast<LONG>(pixelWidth), static_cast<LONG>(pixelHeight)};
    const auto rendered = drawToDc(memory, bounds, dpi, state);
    GdiFlush();
    if (rendered && overlay != nullptr) {
        compositeBgraOverlay(
            static_cast<std::uint8_t*>(pixels),
            pixelWidth,
            pixelHeight,
            *overlay);
    }
    const auto written = rendered &&
        writeBgraBitmap(path, pixelWidth, pixelHeight, pixels);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return written;
}

FrameResult ListenVisualRenderer::drawFrame(const ListenRenderState& state)
{
    if (!target_ || !brush_) return FrameResult::failed;
    ensurePixelIcons();
    auto& telemetry = PerformanceTelemetry::instance();
    telemetry.paint(TelemetrySurface::listen);
    accent_ = color(state.theme.accent);
    secondary_ = color(state.theme.secondary);
    if (state.track != nullptr && state.artwork &&
        (glyphArtworkIdentity_ != state.artwork.get() ||
            glyphArtworkKey_ != state.artworkIdentity)) {
        glyphArtworkIdentity_ = state.artwork.get();
        glyphArtworkKey_ = state.artworkIdentity;
        GlyphFlowService::instance().observeArtwork(
            state.artworkIdentity, *state.artwork);
    }
    if (state.activeSpace == 0 && state.track != nullptr) {
        prepareArtwork(state.artworkIdentity, state.artwork);
    }
    if (state.activeSpace == 1 && state.model != nullptr &&
        (state.liveFocus == ListenLiveFocus::none ||
         state.liveFocus == ListenLiveFocus::spectrogram)) {
        prepareSpectrogram(*state.model);
    }

    const auto size = target_->GetSize();
    const auto content = D2D1::RectF(14.0f, 113.0f,
        std::max(14.0f, size.width - 14.0f),
        std::max(113.0f, size.height - 14.0f));
    musicMapRect_ = {};
    analyzeRect_ = {};
    liveModeRect_ = {};
    liveVectorPlotRect_ = {};
    const auto retainedLive =
        rolloutFlagEnabled(RolloutFlag::listenRetainedV3) &&
        state.activeSpace == 1 &&
        prepareLiveStatic(state, content);

    target_->BeginDraw();
    if (retainedLive) {
        target_->DrawBitmap(
            liveStaticBitmap_.Get(),
            D2D1::RectF(0.0f, 0.0f, size.width, size.height),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    } else {
        target_->Clear(kBackground);
    }
    drawChrome(state, size);
    switch (std::clamp(state.activeSpace, 0, 1)) {
    case 0: drawOverview(state, content); break;
    case 1:
        if (retainedLive) drawLiveDynamic(state, content);
        else drawLive(state, content);
        break;
    default: break;
    }
    GlyphFlowDrawStyle glyphStyle;
    glyphStyle.color = accent_;
    glyphStyle.strength = 0.085f;
    const auto glyphDrawn = glyphFlowRenderer_.draw(
        target_.Get(),
        D2D1::RectF(
            104.0f,
            76.0f,
            std::max(14.0f, size.width - 150.0f),
            99.0f),
        motionMode(),
        glyphStyle);
    const auto result = target_->EndDraw();
    telemetry.endDraw(TelemetrySurface::listen, result);
    if (confirmedDeviceLost(d3dDevice_.Get(), result)) {
        RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
        discardTargetResources();
        return FrameResult::deviceLost;
    }
    if (result == D2DERR_RECREATE_TARGET) {
        discardTargetResources();
        return FrameResult::deviceLost;
    }
    if (FAILED(result)) return FrameResult::failed;
    if (!swapChain_) {
        glyphOverlayPresented_ = glyphDrawn;
        return FrameResult::accepted;
    }
    ListenEffectsDiagnostics effects;
    effects.mode = listenEffectsMode();
    effects.fallback = effects.mode == ListenEffectsMode::noEffects
        ? ListenEffectsFallback::disabled : ListenEffectsFallback::none;
    const auto scopeVisible = state.activeSpace == 1 &&
        (state.liveFocus == ListenLiveFocus::none ||
         state.liveFocus == ListenLiveFocus::scope) &&
        state.scopeSamples != nullptr && state.scopeMinimum != nullptr &&
        state.scopeMaximum != nullptr && state.scopeVisualRevision != 0 &&
        liveLayout_.scope.visible;
    if (scopeVisible && effects.mode == ListenEffectsMode::softLight &&
        !softwareRasterizer_) {
        float dpiX = 96.0f;
        float dpiY = 96.0f;
        target_->GetDpi(&dpiX, &dpiY);
        // Failure is deliberately non-fatal: the native D2D core/envelope
        // already in this frame is the WARP/device-error fallback.
        const auto effectDrawn = scopeLightPass_.draw(
            d3dDevice_.Get(), d3dContext_.Get(), swapChain_.Get(),
            liveLayout_.scope.plot, dpiX,
            *state.scopeSamples, *state.scopeMinimum, *state.scopeMaximum,
            state.scopeVisualRevision, accent_);
        effects.fallback = effectDrawn
            ? ListenEffectsFallback::none
            : ListenEffectsFallback::unavailable;
    } else if (scopeVisible && effects.mode == ListenEffectsMode::softLight &&
        softwareRasterizer_) {
        effects.fallback = ListenEffectsFallback::softwareRasterizer;
    }
    const auto& scopeDiagnostics = scopeLightPass_.diagnostics();
    effects.submissions = scopeDiagnostics.submissions;
    effects.successes = scopeDiagnostics.successes;
    effects.resourceBytes = scopeDiagnostics.resourceBytes;
    effects.gpuMeasured = scopeDiagnostics.gpuMeasured;
    effects.gpuMicroseconds = scopeDiagnostics.gpuMicroseconds;
    publishListenEffectsDiagnostics(effects);

    DXGI_PRESENT_PARAMETERS parameters{};
    // The waitable frame clock already paces this composed surface. Keep the
    // UI thread out of an additional driver/vblank wait, as for native panels.
    const auto present = swapChain_->Present1(
        0, DXGI_PRESENT_DO_NOT_WAIT, &parameters);
    telemetry.present(
        TelemetrySurface::listen,
        present,
        present == DXGI_ERROR_WAS_STILL_DRAWING);
    WindowFrameClock::reportPresentOutcome(
        present == DXGI_ERROR_WAS_STILL_DRAWING);
    if (present == DXGI_ERROR_WAS_STILL_DRAWING)
        return FrameResult::notReady;
    if (present == DXGI_STATUS_OCCLUDED) return FrameResult::occluded;
    if (confirmedDeviceLost(d3dDevice_.Get(), present)) {
        RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
        discardTargetResources();
        return FrameResult::deviceLost;
    }
    // Keep demand for one clean frame after the shared animation expires.
    // Failed/occluded presentation cannot retire pixels still on screen.
    if (SUCCEEDED(present)) glyphOverlayPresented_ = glyphDrawn;
    return SUCCEEDED(present) ? FrameResult::accepted : FrameResult::failed;
}

void ListenVisualRenderer::resize(std::uint32_t widthValue, std::uint32_t heightValue)
{
    if (!swapChain_ || widthValue == 0 || heightValue == 0 ||
        (compositionWidth_ == widthValue && compositionHeight_ == heightValue)) {
        return;
    }
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    scopeLightPass_.resetSurface();
    liveStaticBitmap_.Reset();
    liveStaticThemeGeneration_ = 0;
    liveStaticMode_ = -1;
    liveStaticContent_ = {};
    portraitRadarGeometry_.Reset();
    portraitRadarIdentity_ = nullptr;
    compositionBitmap_.Reset();
    target_.Reset();
    const auto result = swapChain_->ResizeBuffers(
        2,
        widthValue,
        heightValue,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(result)) {
        if (confirmedDeviceLost(d3dDevice_.Get(), result))
            RenderDeviceHub::instance().invalidate(d3dDevice_.Get());
        discardTargetResources();
        return;
    }
    // ResizeBuffers preserves this swap-chain object and its readiness binding.
    compositionWidth_ = widthValue;
    compositionHeight_ = heightValue;
}

void ListenVisualRenderer::discard()
{
    discardTargetResources();
    artworkTransition_.clear();
    portraitRadarGeometry_.Reset();
    portraitRadarIdentity_ = nullptr;
    portraitRadarCenter_ = {};
    portraitRadarRadius_ = 0.0f;
    for (auto& geometry : musicMapRibbonGeometry_) geometry.Reset();
    musicMapWaveformIdentity_ = nullptr;
    musicMapWaveformSize_ = 0;
    musicMapProfile_ = {};
    musicMapGeometryRect_ = {};
    numericFormat_.Reset();
    heroFormat_.Reset();
    titleFormat_.Reset();
    strongFormat_.Reset();
    bodyFormat_.Reset();
    captionFormat_.Reset();
    writeFactory_.Reset();
    factory_.Reset();
    pixelIconPainter_.bindFactory(nullptr);
    favoriteOutlineIcon_ = {};
    favoriteFilledIcon_ = {};
    minimizeIcon_ = {};
    maximizeIcon_ = {};
    restoreIcon_ = {};
    closeIcon_ = {};
    pixelIconDpi_ = 0.0f;
}

void ListenVisualRenderer::displayEnvironmentChanged() noexcept
{
    adapterCheckRequired_ = true;
}

void ListenVisualRenderer::invalidateArtwork()
{
    // Keep the current image visible until its decoded replacement arrives.
}

void ListenVisualRenderer::clearLiveHistory() noexcept
{
    scopeLightPass_.resetSurface();
    spectrogramBitmap_.Reset();
    spectrogramRevision_ = 0;
    spectrogramEpoch_ = 0;
    dynamicsGeometryRevision_ = 0;
    dynamicsGeometryRect_ = {};
    dynamicsGeometryCount_ = 0;
    dynamicsBadgeText_ = L"PEAK --  \u00b7  RMS --  \u00b7  CREST --";
    dynamicsBadgeUpdated_ = {};
    spectrogramHead_ = 0;
    spectrogramPixels_.fill(0);
    spectrogramColumnPixels_.fill(0);
}

bool ListenVisualRenderer::ensureDeviceIndependentResources()
{
    if (!factory_) {
        factory_ = RenderDeviceHub::instance().d2dFactory();
        if (!factory_) return false;
    }
    if (!writeFactory_) {
        writeFactory_ = RenderDeviceHub::instance().writeFactory();
        if (!writeFactory_) return false;
        const auto createFormat = [&](
                                      UiFontRole role,
                                      Microsoft::WRL::ComPtr<IDWriteTextFormat>& output) {
            const auto& descriptor = UiDesignSystemV1::font(role);
            const auto family = UiDesignSystemV1::resolveFontFamily(
                writeFactory_.Get(),
                descriptor);
            return writeFactory_->CreateTextFormat(
                family.data(),
                nullptr,
                descriptor.weight,
                descriptor.style,
                descriptor.stretch,
                descriptor.sizeDip,
                L"en-us",
                output.ReleaseAndGetAddressOf());
        };
        if (FAILED(createFormat(UiFontRole::information, captionFormat_)) ||
            FAILED(createFormat(UiFontRole::body, bodyFormat_)) ||
            FAILED(createFormat(UiFontRole::bodyStrong, strongFormat_)) ||
            FAILED(createFormat(UiFontRole::artist, titleFormat_)) ||
            FAILED(createFormat(UiFontRole::artist, heroFormat_)) ||
            FAILED(createFormat(UiFontRole::technical, numericFormat_))) {
            return false;
        }
        for (auto* format : {captionFormat_.Get(), bodyFormat_.Get(),
                             strongFormat_.Get(), titleFormat_.Get(),
                             heroFormat_.Get(), numericFormat_.Get()}) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    return true;
}

void ListenVisualRenderer::ensurePixelIcons()
{
    if (!factory_ || !target_) return;
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target_->GetDpi(&dpiX, &dpiY);
    pixelIconPainter_.bindFactory(factory_.Get());
    if (std::abs(pixelIconDpi_ - dpiX) <= 0.01f &&
            favoriteOutlineIcon_.generation != 0 &&
            favoriteFilledIcon_.generation != 0 &&
            minimizeIcon_.generation != 0 &&
            maximizeIcon_.generation != 0 &&
            restoreIcon_.generation != 0 &&
            closeIcon_.generation != 0 &&
            focusEnterIcon_.generation != 0 &&
            focusExitIcon_.generation != 0) return;
    auto prepare = [this, dpiX, dpiY](
            goden::pixel::IconId id,
            goden::pixel::PixelIconPainter::Handle& handle) {
        const auto* glyph = goden::pixel::icon(id, 16);
        if (!glyph) { handle = {}; return; }
        const auto width = static_cast<std::int32_t>(std::lround(
            static_cast<float>(glyph->width) * dpiX / 96.0f));
        const auto height = static_cast<std::int32_t>(std::lround(
            static_cast<float>(glyph->height) * dpiY / 96.0f));
        if (FAILED(pixelIconPainter_.prepare(*glyph,
                std::max<std::int32_t>(glyph->width, width),
                std::max<std::int32_t>(glyph->height, height), handle))) {
            handle = {};
        }
    };
    prepare(goden::pixel::IconId::FavoriteOutline, favoriteOutlineIcon_);
    prepare(goden::pixel::IconId::FavoriteFilled, favoriteFilledIcon_);
    prepare(goden::pixel::IconId::WindowMinimize, minimizeIcon_);
    prepare(goden::pixel::IconId::WindowMaximize, maximizeIcon_);
    prepare(goden::pixel::IconId::WindowRestore, restoreIcon_);
    prepare(goden::pixel::IconId::WindowClose, closeIcon_);
    prepare(goden::pixel::IconId::FocusEnter, focusEnterIcon_);
    prepare(goden::pixel::IconId::FocusExit, focusExitIcon_);
    pixelIconDpi_ = dpiX;
}

bool ListenVisualRenderer::ensureResources(HWND window)
{
    if (qaPreparationFailuresRemaining_ != 0) {
        --qaPreparationFailuresRemaining_;
        ++qaPreparationFaultCount_;
        discardTargetResources();
        return false;
    }
    if (!ensureDeviceIndependentResources()) return false;
    const auto currentMonitor =
        MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    const auto recoveryEpoch = RenderDeviceHub::instance().recoveryEpoch();
    if (d3dDevice_ &&
        (renderRecoveryEpoch_ != recoveryEpoch ||
         adapterCheckRequired_ || currentMonitor != renderMonitor_)) {
        if (renderRecoveryEpoch_ != recoveryEpoch ||
            !RenderDeviceHub::instance().isDeviceCompatibleWithWindow(
                d3dDevice_.Get(), window)) {
            discardTargetResources();
        }
    }
    renderMonitor_ = currentMonitor;
    adapterCheckRequired_ = false;
    if (window_ != window) {
        discardTargetResources();
        window_ = window;
    }
    if (!d3dDevice_ && !createCompositionResources(window)) return false;

    RECT client{};
    GetClientRect(window, &client);
    const auto widthValue = static_cast<std::uint32_t>(
        std::max<LONG>(1, client.right - client.left));
    const auto heightValue = static_cast<std::uint32_t>(
        std::max<LONG>(1, client.bottom - client.top));
    if (!swapChain_) {
        if (!createCompositionSwapChain(widthValue, heightValue)) return false;
    } else if (compositionWidth_ != widthValue ||
        compositionHeight_ != heightValue) {
        resize(widthValue, heightValue);
        if (!swapChain_) return false;
    }
    if (!compositionBitmap_ && !bindCompositionBackBuffer(
            static_cast<float>(GetDpiForWindow(window)))) {
        discardTargetResources();
        return false;
    }
    if (!brush_ && FAILED(target_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White),
            brush_.ReleaseAndGetAddressOf()))) {
        discardTargetResources();
        return false;
    }
    if (textRendering_.apply(target_.Get(), writeFactory_.Get(), currentMonitor)) {
        liveStaticBitmap_.Reset();
    }
    return true;
}

bool ListenVisualRenderer::createCompositionResources(HWND window)
{
    const auto fail = [&]() {
        discardTargetResources();
        return false;
    };
    bool softwareRasterizer = false;
    auto result = RenderDeviceHub::instance().acquire(
        window, d3dDevice_, d3dContext_, softwareRasterizer)
        ? S_OK
        : E_FAIL;
    if (FAILED(result)) return fail();
    softwareRasterizer_ = softwareRasterizer;
    renderRecoveryEpoch_ = RenderDeviceHub::instance().recoveryEpoch();
    renderMonitor_ = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    adapterCheckRequired_ = false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice1;
    if (FAILED(d3dDevice_.As(&dxgiDevice)) ||
        FAILED(factory_->CreateDevice(
            dxgiDevice.Get(), d2dDevice_.ReleaseAndGetAddressOf())) ||
        FAILED(d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            d2dContext_.ReleaseAndGetAddressOf())) ||
        FAILED(DCompositionCreateDevice(
            dxgiDevice.Get(),
            __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(
                compositionDevice_.ReleaseAndGetAddressOf()))) ||
        FAILED(compositionDevice_->CreateTargetForHwnd(
            window,
            FALSE,
            compositionTarget_.ReleaseAndGetAddressOf())) ||
        FAILED(compositionDevice_->CreateVisual(
            compositionVisual_.ReleaseAndGetAddressOf())) ||
        FAILED(compositionTarget_->SetRoot(compositionVisual_.Get()))) {
        return fail();
    }
    if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1))) {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }
    return true;
}

bool ListenVisualRenderer::createCompositionSwapChain(
    std::uint32_t widthValue,
    std::uint32_t heightValue)
{
    const auto fail = [&]() {
        if (frameLatencyHandle_ != nullptr) {
            CloseHandle(frameLatencyHandle_);
            frameLatencyHandle_ = nullptr;
        }
        swapChain_.Reset();
        compositionWidth_ = 0;
        compositionHeight_ = 0;
        return false;
    };
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(d3dDevice_.As(&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf())) ||
        FAILED(adapter->GetParent(
            __uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
        return fail();
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = widthValue;
    description.Height = heightValue;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Listen paints the full client area.  Opaque composition prevents DWM
    // from exposing the parent through a fresh resize/fullscreen back-buffer.
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForComposition(
            d3dDevice_.Get(),
            &description,
            nullptr,
            swapChain.ReleaseAndGetAddressOf())) ||
        FAILED(swapChain.As(&swapChain_))) {
        return fail();
    }
    swapChain_->SetMaximumFrameLatency(1);
    frameLatencyHandle_ = swapChain_->GetFrameLatencyWaitableObject();
    if (frameLatencyHandle_ == nullptr ||
        FAILED(compositionVisual_->SetContent(swapChain_.Get())) ||
        FAILED(compositionDevice_->Commit())) {
        return fail();
    }
    surfaceLifetime_ = gNextListenSurfaceLifetime.fetch_add(
        1, std::memory_order_relaxed);
    ++swapchainEpoch_;
    compositionWidth_ = widthValue;
    compositionHeight_ = heightValue;
    return true;
}

bool ListenVisualRenderer::bindCompositionBackBuffer(float dpi)
{
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (!swapChain_ || FAILED(swapChain_->GetBuffer(
            0,
            __uuidof(IDXGISurface),
            reinterpret_cast<void**>(surface.ReleaseAndGetAddressOf())))) {
        return false;
    }
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE),
        std::max(1.0f, dpi),
        std::max(1.0f, dpi));
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(
            surface.Get(),
            &properties,
            compositionBitmap_.ReleaseAndGetAddressOf()))) {
        return false;
    }
    d2dContext_->SetTarget(compositionBitmap_.Get());
    target_ = d2dContext_;
    return true;
}

void ListenVisualRenderer::discardTargetResources()
{
    textRendering_.reset();
    scopeLightPass_.reset();
    spectrogramBitmap_.Reset();
    liveStaticBitmap_.Reset();
    artworkTransition_.discardDeviceResources();
    glyphFlowRenderer_.reset();
    glyphOverlayPresented_ = false;
    brush_.Reset();
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    compositionBitmap_.Reset();
    target_.Reset();
    if (compositionTarget_ && compositionDevice_) {
        compositionTarget_->SetRoot(nullptr);
        compositionDevice_->Commit();
    }
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    if (frameLatencyHandle_ != nullptr) {
        CloseHandle(frameLatencyHandle_);
        frameLatencyHandle_ = nullptr;
    }
    swapChain_.Reset();
    d2dContext_.Reset();
    d2dDevice_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    renderRecoveryEpoch_ = 0;
    renderMonitor_ = nullptr;
    adapterCheckRequired_ = true;
    compositionWidth_ = 0;
    compositionHeight_ = 0;
    softwareRasterizer_ = false;
    window_ = nullptr;
    spectrogramRevision_ = 0;
    spectrogramEpoch_ = 0;
    spectrogramHead_ = 0;
    liveStaticThemeGeneration_ = 0;
    liveStaticMode_ = -1;
    liveStaticContent_ = {};
}

void ListenVisualRenderer::prepareArtwork(
    std::wstring_view identity,
    const std::shared_ptr<const ArtPixels>& pixels)
{
    if (!pixels || pixels->bgra.empty()) return;
    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target_->GetDpi(&dpiX, &dpiY);
    artworkTransition_.update(target_.Get(), dpiX, identity, pixels);
}

void ListenVisualRenderer::prepareSpectrogram(
    const RealtimeAnalysisSnapshot& model)
{
    if (!model.history) return;
    const auto& history = *model.history;
    const auto revision = history.spectrogramRevision;
    const auto sameEpoch = history.epoch == spectrogramEpoch_;
    if (sameEpoch && revision == spectrogramRevision_) return;
    const auto& columns = history.spectrogram;
    const auto head = history.spectrogramHead;
    const auto incremental = sameEpoch && spectrogramBitmap_ &&
        revision == spectrogramRevision_ + 1U &&
        head == (spectrogramHead_ + 1U) % kSpectrogramColumns;
    if (!incremental) {
        for (std::size_t x = 0; x < kSpectrogramColumns; ++x) {
            for (std::size_t y = 0; y < kSpectrogramBins; ++y) {
                spectrogramPixels_[y * kSpectrogramColumns + x] =
                    spectrogramColor(columns[x][y]);
            }
        }
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE));
        if (!spectrogramBitmap_) {
            const auto result = target_->CreateBitmap(
                D2D1::SizeU(
                    static_cast<UINT32>(kSpectrogramColumns),
                    static_cast<UINT32>(kSpectrogramBins)),
                spectrogramPixels_.data(),
                static_cast<UINT32>(
                    kSpectrogramColumns * sizeof(std::uint32_t)),
                properties, spectrogramBitmap_.ReleaseAndGetAddressOf());
            if (FAILED(result)) return;
            PerformanceTelemetry::instance().upload(
                TelemetrySurface::listen,
                kSpectrogramColumns * kSpectrogramBins *
                    sizeof(std::uint32_t));
        } else {
            const auto result = spectrogramBitmap_->CopyFromMemory(
                nullptr, spectrogramPixels_.data(),
                static_cast<UINT32>(
                    kSpectrogramColumns * sizeof(std::uint32_t)));
            if (FAILED(result)) return;
            PerformanceTelemetry::instance().upload(
                TelemetrySurface::listen,
                kSpectrogramColumns * kSpectrogramBins *
                    sizeof(std::uint32_t));
        }
    } else {
        const auto written = (head + kSpectrogramColumns - 1U) %
            kSpectrogramColumns;
        for (std::size_t y = 0; y < kSpectrogramBins; ++y) {
            spectrogramColumnPixels_[y] =
                spectrogramColor(columns[written][y]);
        }
        const D2D1_RECT_U destination{
            static_cast<UINT32>(written),
            0,
            static_cast<UINT32>(written + 1U),
            static_cast<UINT32>(kSpectrogramBins),
        };
        const auto result = spectrogramBitmap_->CopyFromMemory(
            &destination,
            spectrogramColumnPixels_.data(),
            static_cast<UINT32>(sizeof(std::uint32_t)));
        if (FAILED(result)) return;
        PerformanceTelemetry::instance().upload(
            TelemetrySurface::listen,
            kSpectrogramBins * sizeof(std::uint32_t));
    }
    spectrogramRevision_ = revision;
    spectrogramEpoch_ = history.epoch;
    spectrogramHead_ = head;
}

void ListenVisualRenderer::prepareMusicMapRibbons(
    const DeepAnalysisSnapshot& analysis,
    const D2D1_RECT_F& rectangle)
{
    const auto waveform = analysis.waveform;
    if (!waveform || waveform->empty() || !factory_ ||
        width(rectangle) <= 1.0f || height(rectangle) <= 1.0f) {
        if (!waveform || waveform->empty()) {
            for (auto& geometry : musicMapRibbonGeometry_) geometry.Reset();
            musicMapWaveformIdentity_ = nullptr;
            musicMapWaveformSize_ = 0;
            musicMapProfile_ = {};
        }
        return;
    }

    const auto rectangleChanged =
        std::abs(rectangle.left - musicMapGeometryRect_.left) > 0.1f ||
        std::abs(rectangle.top - musicMapGeometryRect_.top) > 0.1f ||
        std::abs(rectangle.right - musicMapGeometryRect_.right) > 0.1f ||
        std::abs(rectangle.bottom - musicMapGeometryRect_.bottom) > 0.1f;
    if (!rectangleChanged &&
        waveform.get() == musicMapWaveformIdentity_ &&
        waveform->size() == musicMapWaveformSize_) {
        return;
    }

    const auto map = buildMusicMapProfile(*waveform);
    const auto metric = [&](std::size_t index, std::size_t lane) noexcept {
        const auto& bin = map[index];
        return lane == 0 ? bin.energy
            : lane == 1 ? bin.width
            : lane == 2 ? bin.brightness
                        : bin.pulse;
    };

    std::array<Microsoft::WRL::ComPtr<ID2D1PathGeometry>, 4> next{};
    const auto lanePitch = height(rectangle) / 4.0f;
    const auto binWidth = width(rectangle) /
        static_cast<float>(kMusicMapBins);
    for (std::size_t lane = 0; lane < next.size(); ++lane) {
        if (FAILED(factory_->CreatePathGeometry(
                next[lane].ReleaseAndGetAddressOf()))) {
            return;
        }
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(next[lane]->Open(sink.ReleaseAndGetAddressOf()))) return;

        std::size_t index = 0;
        while (index < map.size()) {
            while (index < map.size() && map[index].observations == 0) ++index;
            if (index >= map.size()) break;
            const auto first = index;
            while (index + 1 < map.size() &&
                map[index + 1].observations != 0) {
                ++index;
            }
            const auto last = index;
            const auto center = rectangle.top +
                (static_cast<float>(lane) + 0.5f) * lanePitch;
            const auto halfFor = [&](std::size_t bin) noexcept {
                const auto normalized = std::pow(
                    std::clamp(metric(bin, lane), 0.0f, 1.0f), 1.22f);
                const auto maximum = std::max(
                    2.0f,
                    lanePitch * kMusicMapMaximumHalfLane);
                return std::lerp(1.25f, maximum, normalized);
            };
            const auto xFor = [&](std::size_t bin) noexcept {
                return rectangle.left +
                    (static_cast<float>(bin) + 0.5f) * binWidth;
            };

            sink->BeginFigure(
                D2D1::Point2F(
                    rectangle.left + static_cast<float>(first) * binWidth,
                    center - halfFor(first)),
                D2D1_FIGURE_BEGIN_FILLED);
            for (auto bin = first; bin <= last; ++bin) {
                sink->AddLine(D2D1::Point2F(
                    xFor(bin), center - halfFor(bin)));
            }
            sink->AddLine(D2D1::Point2F(
                rectangle.left + static_cast<float>(last + 1) * binWidth,
                center - halfFor(last)));
            sink->AddLine(D2D1::Point2F(
                rectangle.left + static_cast<float>(last + 1) * binWidth,
                center + halfFor(last)));
            for (auto bin = last + 1; bin > first;) {
                --bin;
                sink->AddLine(D2D1::Point2F(
                    xFor(bin), center + halfFor(bin)));
            }
            sink->AddLine(D2D1::Point2F(
                rectangle.left + static_cast<float>(first) * binWidth,
                center + halfFor(first)));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            ++index;
        }
        if (FAILED(sink->Close())) return;
    }

    musicMapRibbonGeometry_ = std::move(next);
    musicMapWaveformIdentity_ = waveform.get();
    musicMapWaveformSize_ = waveform->size();
    musicMapProfile_ = map;
    musicMapGeometryRect_ = rectangle;
}

void ListenVisualRenderer::drawChrome(
    const ListenRenderState& state,
    const D2D1_SIZE_F& size)
{
    constexpr std::array<const wchar_t*, 2> labels{
        L"OVERVIEW", L"LIVE"};
    captionRect_ = D2D1::RectF(1.0f, 1.0f,
        std::max(1.0f, size.width - 91.0f), 29.0f);
    minimizeRect_ = D2D1::RectF(
        std::max(0.0f, size.width - 90.0f), 1.0f,
        std::max(0.0f, size.width - 60.0f), 29.0f);
    maximizeRect_ = D2D1::RectF(
        std::max(0.0f, size.width - 60.0f), 1.0f,
        std::max(0.0f, size.width - 30.0f), 29.0f);
    closeRect_ = D2D1::RectF(
        std::max(0.0f, size.width - 30.0f), 1.0f,
        std::max(0.0f, size.width - 1.0f), 29.0f);
    fill(D2D1::RectF(1.0f, 1.0f, size.width - 1.0f, 29.0f), kPanel);
    drawText(L"GODENplayer LISTEN",
        D2D1::RectF(12.0f, 1.0f,
            std::max(12.0f, size.width - 100.0f), 29.0f),
        captionFormat_.Get(), kText);
    const auto hoverMinimize = state.pointerVisible &&
        contains(minimizeRect_, state.pointerX, state.pointerY);
    const auto hoverMaximize = state.pointerVisible &&
        contains(maximizeRect_, state.pointerX, state.pointerY);
    const auto hoverClose = state.pointerVisible &&
        contains(closeRect_, state.pointerX, state.pointerY);
    if (hoverMinimize) fill(minimizeRect_, kPanelAlternate);
    if (hoverMaximize) fill(maximizeRect_, kPanelAlternate);
    if (hoverClose) fill(closeRect_, D2D1::ColorF(0.48f, 0.07f, 0.15f));
    const auto drawChromeIcon = [&](const D2D1_RECT_F& bounds,
            goden::pixel::PixelIconPainter::Handle handle,
            const D2D1_COLOR_F& iconColor) {
        const auto centerX = (bounds.left + bounds.right) * 0.5f;
        const auto centerY = (bounds.top + bounds.bottom) * 0.5f;
        brush_->SetColor(iconColor);
        (void)pixelIconPainter_.drawPrepared(
            target_.Get(), handle,
            {centerX - 8.0f, centerY - 8.0f,
             centerX + 8.0f, centerY + 8.0f}, brush_.Get());
    };
    drawChromeIcon(minimizeRect_, minimizeIcon_, kMuted);
    drawChromeIcon(maximizeRect_,
        state.maximized ? restoreIcon_ : maximizeIcon_, kMuted);
    drawChromeIcon(closeRect_, closeIcon_, hoverClose ? kBright : kMuted);
    line(1.0f, 29.0f, size.width - 1.0f, 29.0f, kBorder);

    fill(D2D1::RectF(1.0f, 30.0f, size.width - 1.0f, 72.0f), kPanel);
    line(1.0f, 72.0f, size.width - 1.0f, 72.0f, kBorder);
    const auto tabWidth = size.width / 2.0f;
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int index = 0; index < 2; ++index) {
        const auto active = index == state.activeSpace;
        if (active) {
            fill(D2D1::RectF(index * tabWidth, 70.0f,
                (index + 1) * tabWidth, 72.0f), accent_);
        }
        drawText(labels[index],
            D2D1::RectF(index * tabWidth, 30.0f,
                (index + 1) * tabWidth, 70.0f),
            captionFormat_.Get(), active ? secondary_ : kMuted);
    }
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    const wchar_t* identity = L"No track playing";
    if (state.track != nullptr) {
        if (chromeIdentityKey_ != state.track->path) {
            chromeIdentityKey_ = state.track->path;
            chromeIdentityText_ =
                state.track->title + L"  \u00B7  " + state.track->artist;
            if (!state.track->album.empty()) {
                chromeIdentityText_ += L"  \u00B7  " + state.track->album;
            }
        }
        identity = chromeIdentityText_.c_str();
    }
    playPauseRect_ = D2D1::RectF(14.0f, 78.0f, 94.0f, 106.0f);
    fill(playPauseRect_, kPanelAlternate);
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    drawText(state.playback.state == PlaybackState::playing ? L"Pause" : L"Play",
        playPauseRect_, captionFormat_.Get(), accent_);
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    drawText(identity, D2D1::RectF(104.0f, 76.0f,
        std::max(104.0f, size.width - 150.0f), 99.0f),
        strongFormat_.Get(), kText);
    transportSeekRect_ = D2D1::RectF(104.0f, 99.0f,
        std::max(104.0f, size.width - 154.0f), 112.0f);
    const auto seekWidth = width(transportSeekRect_);
    line(transportSeekRect_.left, 105.0f, transportSeekRect_.right, 105.0f, kBorder, 3.0f);
    if (state.playback.durationSeconds > 0.0 && seekWidth > 0.0f) {
        const auto progress = static_cast<float>(std::clamp(
            state.playback.positionSeconds / state.playback.durationSeconds, 0.0, 1.0));
        line(transportSeekRect_.left, 105.0f,
            transportSeekRect_.left + seekWidth * progress, 105.0f, accent_, 3.0f);
    }
    wchar_t elapsed[32]{};
    wchar_t duration[32]{};
    wchar_t position[72]{};
    formatDurationText(
        state.playback.positionSeconds, elapsed, std::size(elapsed));
    formatDurationText(
        state.playback.durationSeconds, duration, std::size(duration));
    std::swprintf(position, std::size(position), L"%ls / %ls", elapsed, duration);
    numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    drawText(position, D2D1::RectF(
        std::max(0.0f, size.width - 146.0f), 76.0f,
        std::max(0.0f, size.width - 16.0f), 107.0f),
        numericFormat_.Get(), kMuted);
    numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    line(0.5f, 0.5f, size.width - 0.5f, 0.5f, kBorder);
    line(size.width - 0.5f, 0.5f,
        size.width - 0.5f, size.height - 0.5f, kBorder);
    line(size.width - 0.5f, size.height - 0.5f,
        0.5f, size.height - 0.5f, kBorder);
    line(0.5f, size.height - 0.5f, 0.5f, 0.5f, kBorder);
}

void ListenVisualRenderer::drawOverview(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    favoriteRect_ = {};
    analyzeRect_ = {};
    const auto contentWidth = width(content);
    const auto artEdge = std::clamp(
        std::min(contentWidth * 0.34f, height(content) * 0.64f), 190.0f, 390.0f);
    const auto art = D2D1::RectF(
        content.left, content.top,
        content.left + artEdge, content.top + artEdge);
    fill(art, kPanel);
    drawArtwork(art);
    line(art.left, art.top, art.right, art.top, kBorder);
    line(art.left, art.bottom, art.right, art.bottom, kBorder);

    const auto detailsLeft = art.right + 24.0f;
    if (state.track != nullptr) {
        drawText(state.track->title,
            D2D1::RectF(detailsLeft, content.top + 4.0f,
                content.right, content.top + 50.0f),
            heroFormat_.Get(), kBright);
        drawText(state.track->artist,
            D2D1::RectF(detailsLeft, content.top + 52.0f,
                content.right, content.top + 82.0f),
            titleFormat_.Get(), secondary_);
        drawText(state.track->album,
            D2D1::RectF(detailsLeft, content.top + 86.0f,
                content.right - 80.0f, content.top + 116.0f),
            strongFormat_.Get(), kText);
        strongFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        drawText(state.track->year,
            D2D1::RectF(content.right - 76.0f, content.top + 86.0f,
                content.right, content.top + 116.0f),
            strongFormat_.Get(), kBright);
        strongFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const auto format = state.presentation != nullptr
            ? state.presentation->formatLine
            : (state.track->codec.empty()
                ? std::wstring(L"Local audio") : state.track->codec);
        analyzeRect_ = D2D1::RectF(
            std::max(detailsLeft, content.right - 112.0f), content.top + 120.0f,
            content.right, content.top + 149.0f);
        auto formatRight = analyzeRect_.left - 10.0f;
        if (state.favoriteAvailable) {
            favoriteRect_ = D2D1::RectF(
                std::max(detailsLeft, analyzeRect_.left - 39.0f),
                content.top + 120.0f,
                analyzeRect_.left - 7.0f,
                content.top + 149.0f);
            formatRight = favoriteRect_.left - 8.0f;
            const auto favoriteHovered = state.pointerVisible &&
                contains(favoriteRect_, state.pointerX, state.pointerY);
            fill(favoriteRect_, favoriteHovered ? kPanelAlternate : kPanel);
            line(favoriteRect_.left, favoriteRect_.bottom,
                favoriteRect_.right, favoriteRect_.bottom, kBorder);
            drawFavoriteHeart(
                state.favorite,
                favoriteRect_,
                state.favorite ? secondary_ :
                    (favoriteHovered ? kBright : kMuted));
        }
        drawText(format,
            D2D1::RectF(detailsLeft, content.top + 122.0f,
                formatRight, content.top + 148.0f),
            bodyFormat_.Get(), kMuted);

        const auto hovered = state.pointerVisible &&
            contains(analyzeRect_, state.pointerX, state.pointerY);
        fill(analyzeRect_, hovered ? kPanelAlternate : kPanel);
        line(analyzeRect_.left, analyzeRect_.top,
            analyzeRect_.right, analyzeRect_.top,
            state.analysis.status == DeepAnalysisStatus::running
                ? kMagenta : accent_);
        line(analyzeRect_.left, analyzeRect_.bottom,
            analyzeRect_.right, analyzeRect_.bottom, kBorder);
        const wchar_t* command = L"ANALYZE";
        if (state.analysis.status == DeepAnalysisStatus::running) command = L"CANCEL";
        else if (state.analysis.status == DeepAnalysisStatus::complete) command = L"REANALYZE";
        else if (state.analysis.status == DeepAnalysisStatus::error) command = L"RETRY";
        captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        drawText(command, analyzeRect_, captionFormat_.Get(),
            hovered ? kBright : secondary_);
        captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    const auto analysisTop = content.top + 157.0f;
    std::wstring analysisText = state.presentation != nullptr
        ? state.presentation->analysisStatusText
        : std::wstring{};
    auto analysisColor = kMuted;
    switch (state.analysis.status) {
    case DeepAnalysisStatus::running:
        analysisText = analysisStageLabel(state.analysis.stage);
        analysisText += L"  ";
        analysisText += percentText(state.analysis.overallProgress);
        analysisColor = secondary_;
        break;
    case DeepAnalysisStatus::complete:
        analysisText.clear();
        break;
    case DeepAnalysisStatus::cancelled:
        break;
    case DeepAnalysisStatus::error:
        analysisText = state.analysis.error.empty()
            ? L"ANALYSIS FAILED" : state.analysis.error;
        analysisColor = kMagenta;
        break;
    default: break;
    }
    if (!analysisText.empty()) {
        drawText(analysisText,
            D2D1::RectF(detailsLeft, analysisTop,
                content.right, analysisTop + 20.0f),
            captionFormat_.Get(), analysisColor);
    }
    const auto analysisLine = analysisTop + 22.0f;
    line(detailsLeft, analysisLine, content.right, analysisLine, kBorder, 1.5f);
    if (state.analysis.status == DeepAnalysisStatus::running ||
        state.analysis.status == DeepAnalysisStatus::complete) {
        line(detailsLeft, analysisLine,
            detailsLeft + (content.right - detailsLeft) *
                state.analysis.overallProgress,
            analysisLine, state.analysis.status == DeepAnalysisStatus::complete
                ? accent_ : secondary_, 1.5f);
    }

    const auto progressTop = content.top + 191.0f;
    const auto timeline = D2D1::RectF(
        detailsLeft, progressTop - 4.0f, content.right, progressTop + 7.0f);
    fill(timeline, kPanelAlternate);
    const auto duration = std::max(0.0, state.playback.durationSeconds);
    if (duration > 0.0 && state.presentation != nullptr) {
        const auto& sections = state.presentation->sections;
        for (std::size_t index = 0; index < sections.size(); ++index) {
            const auto& section = sections[index];
            const auto left = timeline.left + width(timeline) *
                static_cast<float>(std::clamp(
                    section.startSeconds / duration, 0.0, 1.0));
            const auto right = timeline.left + width(timeline) *
                static_cast<float>(std::clamp(
                    section.endSeconds / duration, 0.0, 1.0));
            const auto sectionColor = mix(
                index % 2U == 0U ? secondary_ : accent_, kBright,
                section.energy * 0.32f);
            brush_->SetColor(sectionColor);
            target_->FillRectangle(D2D1::RectF(
                left, timeline.top, std::max(left + 1.0f, right),
                timeline.bottom), brush_.Get());
        }
        for (const auto start : state.presentation->chordStarts) {
            const auto x = timeline.left + width(timeline) *
                static_cast<float>(std::clamp(start / duration, 0.0, 1.0));
            line(x, timeline.bottom - 3.0f, x, timeline.bottom,
                kBright, 1.0f);
        }
    }
    const auto progress = state.playback.durationSeconds > 0.0
        ? static_cast<float>(std::clamp(
            state.playback.positionSeconds / state.playback.durationSeconds,
            0.0, 1.0))
        : 0.0f;
    line(detailsLeft, timeline.bottom,
        detailsLeft + (content.right - detailsLeft) * progress,
        timeline.bottom, accent_, 2.0f);

    const auto graphTop = std::max(progressTop + 32.0f, art.bottom + 18.0f);
    const auto dossierBottom = std::min(art.bottom - 8.0f, graphTop - 12.0f);
    if (state.presentation != nullptr && state.presentation->analyzed &&
        dossierBottom - progressTop >= 44.0f) {
        const auto& values = state.presentation->dossierValues;
        constexpr std::array<const wchar_t*, 12> labels{
            L"BPM", L"KEY", L"METER", L"PEAK", L"RMS", L"DYNAMIC",
            L"CREST", L"WARMTH", L"BRIGHTNESS", L"TEMPO STABILITY",
            L"STEREO CORR", L"SIDE / MID"};
        const auto columns = width(D2D1::RectF(detailsLeft, 0.0f,
            content.right, 1.0f)) >= 520.0f ? 4U : 2U;
        const auto rows = (labels.size() + columns - 1U) / columns;
        const auto tileWidth = (content.right - detailsLeft) /
            static_cast<float>(columns);
        const auto tileHeight = std::min(50.0f,
            (dossierBottom - progressTop - 10.0f) /
                static_cast<float>(rows));
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto column = index % columns;
            const auto row = index / columns;
            const auto tile = D2D1::RectF(
                detailsLeft + column * tileWidth,
                progressTop + 9.0f + row * tileHeight,
                detailsLeft + (column + 1U) * tileWidth - 4.0f,
                progressTop + 9.0f + (row + 1U) * tileHeight - 3.0f);
            fill(tile, index % 2U == 0U ? kPanel : kPanelAlternate);
            drawText(labels[index],
                D2D1::RectF(tile.left + 6.0f, tile.top + 2.0f,
                    tile.right - 5.0f, tile.top + 18.0f),
                captionFormat_.Get(), kMuted);
            drawText(values[index],
                D2D1::RectF(tile.left + 6.0f, tile.top + 17.0f,
                    tile.right - 5.0f, tile.bottom),
                numericFormat_.Get(), kText);
        }
    }

    const bool splitAlbumContext = contentWidth > 900.0f;
    const auto available = std::max(90.0f, content.bottom - graphTop);
    const auto stackedRailHeight = !splitAlbumContext && state.track != nullptr
        ? std::clamp(available * 0.32f, 84.0f, 150.0f) : 0.0f;
    const auto graphAvailable = std::max(
        84.0f, available - stackedRailHeight -
            (stackedRailHeight > 0.0f ? 10.0f : 0.0f));
    const auto graphHeight = std::max(
        18.0f, (graphAvailable - 20.0f) / 3.0f);
    const auto graphRight = splitAlbumContext
        ? content.left + contentWidth * 0.70f - 6.0f : content.right;
    if (state.model != nullptr && state.model->history) {
        const auto& history = *state.model->history;
        const std::array<const wchar_t*, 3> labels{L"ENERGY", L"SPACE", L"TEXTURE"};
        const std::array<const std::array<float, kEvolutionSamples>*, 3> values{
            &history.energyHistory,
            &history.widthHistory,
            &history.brightnessHistory};
        const std::array<D2D1_COLOR_F, 3> colors{accent_, kMagenta, kGreen};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto top = graphTop + index * (graphHeight + 10.0f);
            const auto rectangle = D2D1::RectF(
                content.left, top, graphRight, top + graphHeight);
            fill(rectangle, index % 2 == 0 ? kPanel : kPanelAlternate);
            drawText(labels[index],
                D2D1::RectF(rectangle.left + 8.0f, rectangle.top,
                    rectangle.left + 84.0f, rectangle.bottom),
                captionFormat_.Get(), kMuted);
            drawEvolution(*values[index], history.historyHead,
                D2D1::RectF(rectangle.left + 88.0f, rectangle.top + 4.0f,
                    rectangle.right - 6.0f, rectangle.bottom - 4.0f),
                colors[index]);
        }
    } else {
        const auto* presentation = state.presentation;
        constexpr std::array<const wchar_t*, 3> labels{
            L"ENERGY", L"SPACE", L"TEXTURE"};
        const std::array<D2D1_COLOR_F, 3> colors{accent_, kMagenta, kGreen};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto top = graphTop + index * (graphHeight + 10.0f);
            const auto rectangle = D2D1::RectF(
                content.left, top, graphRight, top + graphHeight);
            fill(rectangle, index % 2 == 0 ? kPanel : kPanelAlternate);
            drawText(labels[index], D2D1::RectF(
                rectangle.left + 8.0f, rectangle.top,
                rectangle.left + 84.0f, rectangle.bottom),
                captionFormat_.Get(), kMuted);
            const auto bar = D2D1::RectF(
                rectangle.left + 88.0f,
                (rectangle.top + rectangle.bottom) * 0.5f - 3.0f,
                rectangle.right - 112.0f,
                (rectangle.top + rectangle.bottom) * 0.5f + 3.0f);
            fill(bar, kGrid);
            if (presentation != nullptr &&
                presentation->overviewValid[index]) {
                brush_->SetColor(colors[index]);
                target_->FillRectangle(D2D1::RectF(
                    bar.left, bar.top,
                    bar.left + width(bar) *
                        presentation->overviewValues[index],
                    bar.bottom), brush_.Get());
            }
            numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            drawText(presentation != nullptr
                    ? presentation->overviewValueText[index]
                    : std::wstring(L"\u2014"), D2D1::RectF(
                rectangle.right - 108.0f, rectangle.top,
                rectangle.right - 8.0f, rectangle.bottom),
                numericFormat_.Get(),
                presentation != nullptr &&
                    presentation->overviewValid[index] ? kText : kMuted);
            numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
    if (state.track != nullptr) {
        const auto rail = splitAlbumContext
            ? D2D1::RectF(
                graphRight + 12.0f, graphTop,
                content.right, content.bottom)
            : D2D1::RectF(
                content.left,
                std::min(content.bottom - 64.0f,
                    graphTop + graphAvailable + 10.0f),
                content.right, content.bottom);
        drawPanel(rail, L"ALBUM CONTEXT");
        static const std::vector<ListenAlbumRowV3> emptyAlbum;
        const auto& albumTracks = state.presentation != nullptr
            ? state.presentation->albumRows : emptyAlbum;
        const auto rowHeight = 27.0f;
        const auto maximumRows = static_cast<std::size_t>(std::max(
            0.0f, std::floor((height(rail) - 42.0f) / rowHeight)));
        for (std::size_t index = 0;
            index < std::min(maximumRows, albumTracks.size()); ++index) {
            const auto& track = albumTracks[index];
            const auto top = rail.top + 31.0f + index * rowHeight;
            const auto current = track.current;
            if (current) fill(D2D1::RectF(
                rail.left + 1.0f, top, rail.right - 1.0f,
                top + rowHeight - 1.0f), kPanelAlternate);
            wchar_t number[12]{};
            std::swprintf(number, std::size(number), L"%02d", track.track);
            drawText(number,
                D2D1::RectF(rail.left + 8.0f, top,
                    rail.left + 39.0f, top + rowHeight),
                numericFormat_.Get(), current ? accent_ : kMuted);
            drawText(track.title,
                D2D1::RectF(rail.left + 42.0f, top,
                    rail.right - 8.0f, top + rowHeight),
                bodyFormat_.Get(), current ? kBright : kText);
        }
    }
}

bool ListenVisualRenderer::prepareLiveStatic(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    const auto sameContent =
        liveStaticContent_.left == content.left &&
        liveStaticContent_.top == content.top &&
        liveStaticContent_.right == content.right &&
        liveStaticContent_.bottom == content.bottom;
    const auto mode = (state.crtDirectOrientation ? 1 : 0) |
        (state.dynamicsHold ? 2 : 0);
    const auto sampleRate = state.realtime != nullptr
        ? std::max(1U, state.realtime->sampleRate) : 48000U;
    if (liveStaticBitmap_ && sameContent &&
        liveStaticThemeGeneration_ == state.theme.generation &&
        liveStaticMode_ == mode &&
        liveStaticSampleRate_ == sampleRate &&
        liveStaticFocus_ == state.liveFocus) {
        return true;
    }
    if (!d2dContext_ || !compositionBitmap_ ||
        compositionWidth_ == 0 || compositionHeight_ == 0) {
        return false;
    }

    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target_->GetDpi(&dpiX, &dpiY);
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_IGNORE),
        dpiX,
        dpiY);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(d2dContext_->CreateBitmap(
            D2D1::SizeU(compositionWidth_, compositionHeight_),
            nullptr,
            0,
            &properties,
            bitmap.ReleaseAndGetAddressOf()))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID2D1Image> previousTarget;
    d2dContext_->GetTarget(previousTarget.ReleaseAndGetAddressOf());
    d2dContext_->SetTarget(bitmap.Get());
    target_->BeginDraw();
    target_->Clear(kBackground);
    drawLiveStatic(state, content);
    const auto result = target_->EndDraw();
    d2dContext_->SetTarget(previousTarget.Get());
    if (FAILED(result)) return false;

    liveStaticBitmap_ = std::move(bitmap);
    liveStaticContent_ = content;
    liveStaticThemeGeneration_ = state.theme.generation;
    liveStaticMode_ = mode;
    liveStaticSampleRate_ = sampleRate;
    liveStaticFocus_ = state.liveFocus;
    return true;
}

void ListenVisualRenderer::drawLive(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    drawLiveStatic(state, content);
    drawLiveDynamic(state, content);
}

void ListenVisualRenderer::drawLiveStatic(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    float layoutDpiX = 96.0f, layoutDpiY = 96.0f;
    target_->GetDpi(&layoutDpiX, &layoutDpiY);
    auto layout = computeListenLayout(content, state.liveFocus, layoutDpiX);
    liveLayout_ = layout;
    const auto& scopeRect = layout.scope.frame;
    const auto& vectorRect = layout.crt.frame;
    const auto& dynamicsRect = layout.dynamics.frame;
    if (layout.scope.visible) drawPanel(scopeRect, L"SCOPE \u00B7 COMPACT 512");
    if (layout.crt.visible) drawPanel(vectorRect, L"STEREO / CRT");
    if (layout.dynamics.visible) drawPanel(dynamicsRect, L"DYNAMICS \u00B7 8 s");
    if (layout.spectrogram.visible) drawPanel(layout.spectrogram.frame, L"SPECTROGRAM \u00B7 4 s");
    const auto drawFocus = [&](const ListenPaneLayout& pane,
            ListenLiveFocus focus) {
        if (!pane.visible) return;
        const auto centerX = (pane.focusHit.left + pane.focusHit.right) * 0.5f;
        const auto centerY = (pane.focusHit.top + pane.focusHit.bottom) * 0.5f;
        brush_->SetColor(state.liveFocus == focus ? accent_ : kMuted);
        (void)pixelIconPainter_.drawPrepared(target_.Get(),
            state.liveFocus == focus ? focusExitIcon_ : focusEnterIcon_,
            {centerX - 8.0f, centerY - 8.0f,
             centerX + 8.0f, centerY + 8.0f}, brush_.Get());
    };
    drawFocus(layout.scope, ListenLiveFocus::scope);
    drawFocus(layout.crt, ListenLiveFocus::crt);
    drawFocus(layout.dynamics, ListenLiveFocus::dynamics);
    drawFocus(layout.spectrogram, ListenLiveFocus::spectrogram);
    const auto crtModeRight = layout.crt.visible
        ? layout.crt.focusHit.left - 4.0f : vectorRect.right;
    liveModeRect_ = layout.crt.visible ? D2D1::RectF(
        std::max(vectorRect.left + 82.0f, crtModeRight - 132.0f),
        vectorRect.top + 4.0f,
        crtModeRight,
        vectorRect.top + 27.0f) : D2D1_RECT_F{};
    const auto modeMiddle =
        (liveModeRect_.left + liveModeRect_.right) * 0.5f;
    const auto fieldModeRect = D2D1::RectF(
        liveModeRect_.left, liveModeRect_.top,
        modeMiddle, liveModeRect_.bottom);
    const auto crtModeRect = D2D1::RectF(
        modeMiddle, liveModeRect_.top,
        liveModeRect_.right, liveModeRect_.bottom);
    fill(fieldModeRect,
        state.crtDirectOrientation ? kPanel : kPanelAlternate);
    fill(crtModeRect,
        state.crtDirectOrientation ? kPanelAlternate : kPanel);
    line(liveModeRect_.left, liveModeRect_.top,
        liveModeRect_.right, liveModeRect_.top, kBorder);
    line(liveModeRect_.left, liveModeRect_.bottom,
        liveModeRect_.right, liveModeRect_.bottom, kBorder);
    line(liveModeRect_.left, liveModeRect_.top,
        liveModeRect_.left, liveModeRect_.bottom, kBorder);
    line(liveModeRect_.right, liveModeRect_.top,
        liveModeRect_.right, liveModeRect_.bottom, kBorder);
    line(modeMiddle, liveModeRect_.top,
        modeMiddle, liveModeRect_.bottom, kBorder);
    drawText(L"CRT M/S", fieldModeRect,
        captionFormat_.Get(),
        state.crtDirectOrientation ? kMuted : accent_);
    drawText(L"CRT L/R", crtModeRect,
        captionFormat_.Get(),
        state.crtDirectOrientation ? accent_ : kMuted);
    liveVectorPlotRect_ = layout.crt.visible
        ? layout.crt.plot : D2D1_RECT_F{};

    const auto scope = layout.scope.plot;
    const auto scopeCenter = (scope.top + scope.bottom) * 0.5f;
    if (layout.scope.visible)
        line(scope.left, scopeCenter, scope.right, scopeCenter, kGrid);

    dynamicsHoldRect_ = layout.dynamics.visible ? D2D1::RectF(
        (std::max)(dynamicsRect.left + 70.0f,
            layout.dynamics.focusHit.left - 58.0f),
        dynamicsRect.top + 4.0f,
        layout.dynamics.focusHit.left - 4.0f,
        dynamicsRect.top + 27.0f) : D2D1_RECT_F{};
    if (layout.dynamics.visible) {
    const auto plot = layout.dynamics.plot;
    if (validListenRect(plot)) {
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    constexpr std::array<int, 4> ticks{0, -24, -48, -72};
    for (const auto decibels : ticks) {
        const auto normalized = (static_cast<float>(decibels) + 72.0f) / 78.0f;
        const auto y = plot.bottom - height(plot) * normalized;
        line(plot.left, y, plot.right, y, kGrid);
        wchar_t label[12]{};
        std::swprintf(label, std::size(label), L"%d", decibels);
        const auto labelTop = std::clamp(
            y - 8.5f, plot.top, plot.bottom - 17.0f);
        drawText(label,
            D2D1::RectF(dynamicsRect.left + 3.0f, labelTop,
                plot.left - 4.0f, labelTop + 17.0f),
            captionFormat_.Get(), kMuted);
    }
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    constexpr std::array<const wchar_t*, 5> timeLabels{
        L"-8", L"-6", L"-4", L"-2", L"0 s"};
    for (std::size_t index = 0; index < timeLabels.size(); ++index) {
        const auto amount = static_cast<float>(index) /
            static_cast<float>(timeLabels.size() - 1U);
        const auto x = plot.left + width(plot) * amount;
        line(x, plot.top, x, plot.bottom, kGrid);
        captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        drawText(timeLabels[index], D2D1::RectF(
            x - 18.0f, plot.bottom + 1.0f,
            x + 18.0f, dynamicsRect.bottom - 2.0f),
            captionFormat_.Get(), kMuted);
    }
    captionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fill(dynamicsHoldRect_, state.dynamicsHold ? kPanelAlternate : kPanel);
    drawText(state.dynamicsHold ? L"LIVE" : L"HOLD", dynamicsHoldRect_,
        captionFormat_.Get(), state.dynamicsHold ? accent_ : kMuted);
    }
    }
}

void ListenVisualRenderer::drawLiveDynamic(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    float layoutDpiX = 96.0f, layoutDpiY = 96.0f;
    target_->GetDpi(&layoutDpiX, &layoutDpiY);
    auto layout = computeListenLayout(content, state.liveFocus, layoutDpiX);
    liveLayout_ = layout;
    const auto& scopeRect = layout.scope.frame;
    const auto& vectorRect = layout.crt.frame;
    const auto crtModeRight = layout.crt.visible
        ? layout.crt.focusHit.left - 4.0f : vectorRect.right;
    liveModeRect_ = layout.crt.visible ? D2D1::RectF(
        std::max(vectorRect.left + 82.0f, crtModeRight - 132.0f),
        vectorRect.top + 4.0f,
        crtModeRight,
        vectorRect.top + 27.0f) : D2D1_RECT_F{};
    liveVectorPlotRect_ = layout.crt.visible
        ? layout.crt.plot : D2D1_RECT_F{};

    if (state.realtime == nullptr || state.realtime->sequence == 0) {
        if (layout.scope.visible) {
            drawText(state.playback.state == PlaybackState::paused
                    ? L"Paused \u00b7 press Space or Play to resume"
                    : state.playback.state == PlaybackState::playing
                        ? L"Waiting for live audio" : L"Play a track to see its waveform",
                D2D1::RectF(scopeRect.left + 12.0f, scopeRect.top + 32.0f,
                    scopeRect.right - 12.0f, scopeRect.bottom - 12.0f),
                bodyFormat_.Get(), kMuted);
        }
    }
    const RealtimeVisualSnapshot emptyRealtime{};
    const auto& realtime = state.realtime != nullptr
        ? *state.realtime : emptyRealtime;

    const auto scope = layout.scope.plot;
    const auto scopeCenter = (scope.top + scope.bottom) * 0.5f;
    if (layout.scope.visible && state.scopeSamples != nullptr &&
        state.scopeMinimum != nullptr && state.scopeMaximum != nullptr &&
        validListenRect(scope)) {
        const auto& samples = *state.scopeSamples;
        const auto& minimum = *state.scopeMinimum;
        const auto& maximum = *state.scopeMaximum;
        std::array<D2D1_POINT_2F, kScopeSamples> points{};
        const auto amplitude = height(scope) * 0.47f;
        for (std::size_t index = 0; index < kScopeSamples; ++index) {
            const auto x = scope.left + width(scope) * static_cast<float>(index) /
                static_cast<float>(kScopeSamples - 1U);
            points[index] = D2D1::Point2F(x,
                scopeCenter - std::clamp(samples[index], -1.0f, 1.0f) * amplitude);
            auto envelope = mix(kBackground, accent_, 0.78f);
            envelope.a = 0.12f;
            line(x,
                scopeCenter - std::clamp(maximum[index], -1.0f, 1.0f) * amplitude,
                x,
                scopeCenter - std::clamp(minimum[index], -1.0f, 1.0f) * amplitude,
                envelope, 1.0f);
        }
        if (!swapChain_ || softwareRasterizer_) {
            auto wideHalo = accent_; wideHalo.a = 0.055f;
            auto nearHalo = mix(accent_, kBright, 0.20f); nearHalo.a = 0.12f;
            polyline(points, wideHalo, 8.0f);
            polyline(points, nearHalo, 3.5f);
        }
        polyline(points, accent_, 1.25f);
    }

    wchar_t vectorStatus[80]{};
    if (realtime.sampleRate != 0) {
        std::swprintf(vectorStatus, std::size(vectorStatus),
            state.crtDirectOrientation ? L"direct L/R  /  %u Hz" : L"rotated M/S  /  %u Hz",
            realtime.sampleRate);
    } else {
        std::swprintf(vectorStatus, std::size(vectorStatus), L"%ls \u00b7 %ls",
            state.crtDirectOrientation ? L"direct L/R" : L"rotated M/S",
            state.playback.state == PlaybackState::paused ? L"paused" : L"no live audio");
    }
    if (layout.crt.visible) drawText(vectorStatus,
        D2D1::RectF(vectorRect.left + 10.0f, vectorRect.bottom - 27.0f,
            vectorRect.right - 10.0f, vectorRect.bottom - 7.0f),
        numericFormat_.Get(), kMuted);

    if (layout.dynamics.visible) {
        const auto plot = layout.dynamics.plot;
        const auto* dynamics = state.model && state.model->dynamics
            ? state.model->dynamics.get() : nullptr;
        if (validListenRect(plot) && dynamics && dynamics->valid &&
                dynamics->count != 0) {
            const auto dbY = [&](double value) noexcept {
                const auto normalized = static_cast<float>(std::clamp(
                    (value + 72.0) / 78.0, 0.0, 1.0));
                return plot.bottom - height(plot) * normalized;
            };
            const auto count = dynamics->count;
            const auto geometryChanged =
                dynamicsGeometryRevision_ != dynamics->revision ||
                dynamicsGeometryCount_ != count ||
                dynamicsGeometryRect_.left != plot.left ||
                dynamicsGeometryRect_.top != plot.top ||
                dynamicsGeometryRect_.right != plot.right ||
                dynamicsGeometryRect_.bottom != plot.bottom;
            if (geometryChanged) {
                for (std::size_t index = 0; index < count; ++index) {
                    const auto& bin = dynamics->bins[index];
                    dynamicsMeasured_[index] = bin.measured();
                    const auto peakDb = bin.samplePeak > 0.0
                        ? 20.0 * std::log10(bin.samplePeak) : -72.0;
                    const auto power = bin.scalarCount != 0
                        ? bin.sumSquares / static_cast<double>(bin.scalarCount)
                        : 0.0;
                    const auto rmsDb = power > 0.0
                        ? 10.0 * std::log10(power) : -72.0;
                    float age = static_cast<float>(count - 1U - index) /
                        40.0f;
                    if (dynamics->qpcFrequency != 0 &&
                        dynamics->sourceQpc >= bin.endQpc && bin.endQpc != 0) {
                        age = static_cast<float>(
                            static_cast<double>(dynamics->sourceQpc - bin.endQpc) /
                            static_cast<double>(dynamics->qpcFrequency));
                    }
                    const auto x = plot.right - width(plot) *
                        std::clamp(age / 8.0f, 0.0f, 1.0f);
                    dynamicsPeakPoints_[index] = D2D1::Point2F(x, dbY(peakDb));
                    dynamicsRmsPoints_[index] = D2D1::Point2F(x, dbY(rmsDb));
                }
                dynamicsGeometryRevision_ = dynamics->revision;
                dynamicsGeometryRect_ = plot;
                dynamicsGeometryCount_ = count;
            }
            target_->PushAxisAlignedClip(
                plot, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (std::size_t index = 1; index < count; ++index) {
                if (!dynamicsMeasured_[index - 1U] ||
                        !dynamicsMeasured_[index]) continue;
                if (dynamics->bins[index - 1U].segmentId !=
                        dynamics->bins[index].segmentId) continue;
                if (dynamics->bins[index - 1U].endFrame !=
                        dynamics->bins[index].firstFrame) continue;
                auto band = accent_;
                band.a = 0.10f;
                const auto left = dynamicsRmsPoints_[index - 1U].x;
                const auto right = dynamicsRmsPoints_[index].x;
                fill(D2D1::RectF(left,
                    (std::min)(dynamicsPeakPoints_[index].y,
                        dynamicsRmsPoints_[index].y),
                    right, plot.bottom), band);
            }
            auto peakColor = kBright;
            peakColor.a = 0.86f;
            for (std::size_t index = 1; index < count; ++index) {
                if (!dynamicsMeasured_[index - 1U] ||
                        !dynamicsMeasured_[index]) continue;
                if (dynamics->bins[index - 1U].segmentId !=
                        dynamics->bins[index].segmentId) continue;
                if (dynamics->bins[index - 1U].endFrame !=
                        dynamics->bins[index].firstFrame) continue;
                line(dynamicsRmsPoints_[index - 1U].x,
                    dynamicsRmsPoints_[index - 1U].y,
                    dynamicsRmsPoints_[index].x,
                    dynamicsRmsPoints_[index].y, accent_, 1.5f);
                line(dynamicsPeakPoints_[index - 1U].x,
                    dynamicsPeakPoints_[index - 1U].y,
                    dynamicsPeakPoints_[index].x,
                    dynamicsPeakPoints_[index].y, peakColor, 0.85f);
            }
            target_->PopAxisAlignedClip();

            const auto now = std::chrono::steady_clock::now();
            if (dynamicsBadgeUpdated_.time_since_epoch().count() == 0 ||
                    now - dynamicsBadgeUpdated_ >= std::chrono::milliseconds(100)) {
                const auto reading = readDynamicsTail(*dynamics);
                wchar_t values[128]{};
                if (reading.complete && reading.peak > 0.0 &&
                        reading.power > 0.0) {
                    const auto peakDb = 20.0 * std::log10(reading.peak);
                    const auto rmsDb = 10.0 * std::log10(reading.power);
                    const auto crest = rmsDb >= -60.0
                        ? (std::max)(0.0, peakDb - rmsDb) : -1.0;
                    if (crest >= 0.0)
                        std::swprintf(values, std::size(values),
                            L"PEAK %.1f  \u00b7  RMS %.1f  \u00b7  CREST %.1f dB",
                            peakDb, rmsDb, crest);
                    else
                        std::swprintf(values, std::size(values),
                            L"PEAK %.1f  \u00b7  RMS %.1f  \u00b7  CREST --",
                            peakDb, rmsDb);
                } else {
                    std::swprintf(values, std::size(values),
                        L"PEAK --  \u00b7  RMS --  \u00b7  CREST --");
                }
                dynamicsBadgeText_ = values;
                dynamicsBadgeUpdated_ = now;
            }
            drawText(dynamicsBadgeText_, D2D1::RectF(
                plot.left + 4.0f, plot.top + 3.0f,
                plot.right - 4.0f, plot.top + 23.0f),
                numericFormat_.Get(), kBright);

            if (state.pointerVisible &&
                    contains(plot, state.pointerX, state.pointerY)) {
                const auto amount = std::clamp(
                    (state.pointerX - plot.left) /
                        (std::max)(1.0f, width(plot)), 0.0f, 1.0f);
                const auto span = dynamics->qpcFrequency * 8U;
                const auto age = static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(span) * (1.0 - amount)));
                const auto targetQpc = dynamics->sourceQpc > age
                    ? dynamics->sourceQpc - age : 0U;
                std::size_t lo = 0, hi = count;
                while (lo < hi) {
                    const auto mid = lo + (hi - lo) / 2U;
                    if (dynamics->bins[mid].endQpc <= targetQpc) lo = mid + 1U;
                    else hi = mid;
                }
                const DynamicsBin* hovered = lo < count &&
                        dynamics->bins[lo].startQpc <= targetQpc &&
                        targetQpc < dynamics->bins[lo].endQpc
                    ? &dynamics->bins[lo] : nullptr;
                {
                        const auto* bin = hovered;
                        if (!bin || !bin->measured()) {
                            wchar_t hover[96]{};
                            std::swprintf(hover, std::size(hover),
                                L"%.2f s  \u00b7  UNKNOWN  \u00b7  coverage %.0f%%",
                                -8.0 + 8.0 * amount,
                                bin ? bin->coverage() * 100.0 : 0.0);
                            line(state.pointerX, plot.top, state.pointerX,
                                plot.bottom, kBright, 0.75f);
                            drawText(hover, D2D1::RectF(
                                plot.left + 6.0f, plot.bottom - 22.0f,
                                plot.right - 6.0f, plot.bottom - 3.0f),
                                numericFormat_.Get(), kBright);
                        } else {
                        const auto localPower = bin->scalarCount != 0
                            ? bin->sumSquares /
                                static_cast<double>(bin->scalarCount) : 0.0;
                        wchar_t hover[96]{};
                        std::swprintf(hover, std::size(hover),
                            L"%.2f s  \u00b7  peak %.1f  \u00b7  RMS %.1f dBFS",
                            -8.0 + 8.0 * amount,
                            bin->samplePeak > 0.0
                                ? 20.0 * std::log10(bin->samplePeak) : -72.0,
                            localPower > 0.0
                                ? 10.0 * std::log10(localPower) : -72.0);
                        line(state.pointerX, plot.top, state.pointerX,
                            plot.bottom, kBright, 0.75f);
                        drawText(hover, D2D1::RectF(
                            plot.left + 6.0f, plot.bottom - 22.0f,
                            plot.right - 6.0f, plot.bottom - 3.0f),
                            numericFormat_.Get(), kBright);
                        }
                }
            }
        } else if (validListenRect(plot)) {
            drawText(state.playback.state == PlaybackState::paused ? L"Paused \u00b7 resume to measure dynamics"
                : L"Play a track to measure dynamics", plot,
                bodyFormat_.Get(), kMuted);
        }
    }

    const auto spectrogram = layout.spectrogram.plot;
    if (layout.spectrogram.visible && spectrogramBitmap_ &&
        validListenRect(spectrogram)) {
        const auto split = static_cast<float>(spectrogramHead_) /
            static_cast<float>(kSpectrogramColumns);
        const auto destinationSplit = spectrogram.left +
            width(spectrogram) * (1.0f - split);
        if (spectrogramHead_ == 0) {
            target_->DrawBitmap(spectrogramBitmap_.Get(), spectrogram,
                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            target_->DrawBitmap(
                spectrogramBitmap_.Get(),
                D2D1::RectF(
                    spectrogram.left, spectrogram.top,
                    destinationSplit, spectrogram.bottom),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                D2D1::RectF(
                    static_cast<float>(spectrogramHead_), 0.0f,
                    static_cast<float>(kSpectrogramColumns),
                    static_cast<float>(kSpectrogramBins)));
            target_->DrawBitmap(
                spectrogramBitmap_.Get(),
                D2D1::RectF(
                    destinationSplit, spectrogram.top,
                    spectrogram.right, spectrogram.bottom),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                D2D1::RectF(
                    0.0f, 0.0f,
                    static_cast<float>(spectrogramHead_),
                    static_cast<float>(kSpectrogramBins)));
        }
    }
}

void ListenVisualRenderer::drawMusicMap(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    const auto panel = content;
    drawPanel(panel, L"MUSIC MAP");
    drawText(
        L"FULL-TRACK STRUCTURE",
        D2D1::RectF(
            panel.left + 12.0f, panel.top + 27.0f,
            panel.right - 12.0f, panel.top + 50.0f),
        numericFormat_.Get(), kMuted);

    const auto labelsWidth = 104.0f;
    musicMapRect_ = D2D1::RectF(
        panel.left + labelsWidth, panel.top + 58.0f,
        panel.right - 18.0f, panel.bottom - 72.0f);
    if (musicMapRect_.bottom <= musicMapRect_.top + 40.0f) return;

    constexpr std::array<const wchar_t*, 4> labels{
        L"LOUDNESS", L"PEAK", L"DYNAMICS", L"CHANGE"};
    const std::array<D2D1_COLOR_F, 4> colors{
        accent_, secondary_, kMagenta, kGreen};
    const auto laneHeight = height(musicMapRect_) / 4.0f;
    fill(musicMapRect_, kPanel);
    for (std::size_t lane = 0; lane < labels.size(); ++lane) {
        const auto top = musicMapRect_.top + lane * laneHeight;
        if ((lane & 1U) != 0U) {
            auto alternate = kPanelAlternate;
            alternate.a = 0.42f;
            fill(D2D1::RectF(
                musicMapRect_.left, top,
                musicMapRect_.right, top + laneHeight), alternate);
        }
        if (lane > 0) {
            line(musicMapRect_.left, top,
                musicMapRect_.right, top, kGrid, 1.0f);
        }
        const auto center = top + laneHeight * 0.5f;
        auto centerLine = kGrid;
        centerLine.a = 0.62f;
        line(musicMapRect_.left, center,
            musicMapRect_.right, center, centerLine, 0.55f);
        drawText(labels[lane],
            D2D1::RectF(panel.left + 10.0f, top,
                musicMapRect_.left - 8.0f, top + laneHeight - 2.0f),
            captionFormat_.Get(), kMuted);
    }

    const auto duration = state.analysis.media.durationSeconds > 0.0
        ? state.analysis.media.durationSeconds
        : state.playback.durationSeconds;
    for (std::size_t tick = 0; tick <= 4U; ++tick) {
        const auto amount = static_cast<float>(tick) / 4.0f;
        const auto x = musicMapRect_.left + width(musicMapRect_) * amount;
        if (tick > 0 && tick < 4U) {
            line(x, musicMapRect_.top, x, musicMapRect_.bottom, kGrid, 0.7f);
        }
        numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        drawText(
            durationText(duration * amount),
            D2D1::RectF(
                x - 36.0f, musicMapRect_.bottom + 5.0f,
                x + 36.0f, musicMapRect_.bottom + 27.0f),
            numericFormat_.Get(), kMuted);
        numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    prepareMusicMapRibbons(state.analysis, musicMapRect_);
    for (std::size_t lane = 0; lane < colors.size(); ++lane) {
        if (!musicMapRibbonGeometry_[lane]) continue;
        auto ribbon = mix(kPanelAlternate, colors[lane], 0.72f);
        ribbon.a = 0.58f;
        brush_->SetColor(ribbon);
        target_->FillGeometry(musicMapRibbonGeometry_[lane].Get(), brush_.Get());
        auto edge = mix(kMuted, colors[lane], 0.78f);
        edge.a = 0.92f;
        brush_->SetColor(edge);
        target_->DrawGeometry(
            musicMapRibbonGeometry_[lane].Get(), brush_.Get(), 0.9f);
    }

    if (duration > 0.0) {
        const auto progress = static_cast<float>(std::clamp(
            state.playback.positionSeconds / duration, 0.0, 1.0));
        const auto x = musicMapRect_.left + width(musicMapRect_) * progress;
        line(x, musicMapRect_.top - 5.0f, x, musicMapRect_.bottom + 5.0f,
            kBright, 1.35f);
    }

    if (state.pointerVisible &&
        contains(musicMapRect_, state.pointerX, state.pointerY)) {
        line(state.pointerX, musicMapRect_.top,
            state.pointerX, musicMapRect_.bottom, secondary_, 1.0f);
        const auto amount = std::clamp(
            (state.pointerX - musicMapRect_.left) /
                std::max(1.0f, width(musicMapRect_)),
            0.0f, 1.0f);
        if (musicMapWaveformIdentity_ != nullptr) {
            const auto index = std::min<std::size_t>(
                kMusicMapBins - 1U,
                static_cast<std::size_t>(amount * kMusicMapBins));
            const auto& bin = musicMapProfile_[index];
            wchar_t details[192]{};
            std::swprintf(
                details,
                std::size(details),
                L"%s   L %02d%%   P %02d%%   D %02d%%   \u0394 %02d%%",
                durationText(duration * amount).c_str(),
                static_cast<int>(std::lround(bin.energy * 100.0f)),
                static_cast<int>(std::lround(bin.width * 100.0f)),
                static_cast<int>(std::lround(bin.brightness * 100.0f)),
                static_cast<int>(std::lround(bin.pulse * 100.0f)));
            numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            drawText(
                details,
                D2D1::RectF(
                    panel.left + 300.0f, panel.top + 27.0f,
                    panel.right - 12.0f, panel.top + 50.0f),
                numericFormat_.Get(), secondary_);
            numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    std::wstring status;
    if (state.analysis.status == DeepAnalysisStatus::running) {
        status = L"Building full-track map  \u00b7  " +
            std::to_wstring(static_cast<int>(std::lround(
                state.analysis.overallProgress * 100.0f))) + L"%  \u00b7  " +
            analysisStageLabel(state.analysis.stage);
    } else if (state.analysis.status == DeepAnalysisStatus::error) {
        status = state.analysis.error.empty()
            ? L"Music map unavailable. Retry analysis."
            : state.analysis.error;
    } else if (!state.analysis.waveform || state.analysis.waveform->empty()) {
        status = L"Analyze to build the music map";
    }
    if (!status.empty()) {
        drawText(status,
            D2D1::RectF(panel.left + 12.0f, panel.bottom - 31.0f,
                panel.right - 12.0f, panel.bottom - 7.0f),
            numericFormat_.Get(), kMuted);
    }
}

void ListenVisualRenderer::drawSignalPortrait(
    const ListenRenderState& state,
    const D2D1_RECT_F& content)
{
    drawPanel(content, L"SIGNAL PORTRAIT");
    const auto* presentation = state.presentation;
    const bool analyzed = presentation != nullptr && presentation->analyzed;
    if (state.track == nullptr || !analyzed) {
        const auto prompt = state.track == nullptr
            ? L"Play a track to view its portrait"
            : state.analysis.status == DeepAnalysisStatus::running
                ? L"Building track portrait"
                : L"Analyze to create a stable portrait";
        drawText(prompt,
            D2D1::RectF(
                content.left + 24.0f,
                content.top + 48.0f,
                content.right - 24.0f,
                content.top + 82.0f),
            strongFormat_.Get(), kMuted);
        return;
    }
    const std::array<bool, 8> emptyValid{};
    const std::array<float, 8> neutralValues{
        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    const auto& valid = presentation != nullptr
        ? presentation->portraitValid : emptyValid;
    const auto& values = presentation != nullptr
        ? presentation->portraitValues : neutralValues;
    const std::array<const wchar_t*, 8> labels{
        L"DYNAMICS", L"BRIGHTNESS", L"WARMTH", L"DENSITY",
        L"ROUGHNESS", L"COMPLEXITY", L"RHYTHM VAR.", L"TONAL STAB."};
    const auto portraitArea = D2D1::RectF(
        content.left + 24.0f, content.top + 38.0f,
        content.left + width(content) * 0.68f, content.bottom - 28.0f);
    const auto center = D2D1::Point2F(
        (portraitArea.left + portraitArea.right) * 0.5f,
        (portraitArea.top + portraitArea.bottom) * 0.5f);
    const auto radius = std::min(width(portraitArea), height(portraitArea)) * 0.31f;
    const auto pointAt = [&](std::size_t axis, float amount) noexcept {
        const auto angle = -std::numbers::pi_v<float> * 0.5f +
            static_cast<float>(axis) *
                (2.0f * std::numbers::pi_v<float> / 8.0f);
        return D2D1::Point2F(
            center.x + std::cos(angle) * radius * amount,
            center.y + std::sin(angle) * radius * amount);
    };
    for (const auto ring : {0.25f, 0.50f, 0.75f, 1.0f}) {
        std::array<D2D1_POINT_2F, 8> points{};
        for (std::size_t axis = 0; axis < points.size(); ++axis) {
            points[axis] = pointAt(axis, ring);
        }
        polyline(points, kGrid, ring == 1.0f ? 1.1f : 0.8f, true);
    }
    for (std::size_t axis = 0; axis < values.size(); ++axis) {
        const auto edge = pointAt(axis, 1.0f);
        line(center.x, center.y, edge.x, edge.y, kGrid);
    }
    std::array<D2D1_POINT_2F, 8> portrait{};
    for (std::size_t axis = 0; axis < portrait.size(); ++axis) {
        // Preserve the fixed physical value while reserving a calm inner
        // radius. A valid zero no longer collapses through the centre and
        // makes the fingerprint look like corrupt geometry.
        const auto radialAmount = valid[axis]
            ? 0.20f + 0.78f * std::pow(values[axis], 0.72f)
            : 0.55f;
        portrait[axis] = pointAt(axis, radialAmount);
        const auto labelPoint = pointAt(axis, 1.23f);
        drawText(labels[axis],
            D2D1::RectF(labelPoint.x - 52.0f, labelPoint.y - 9.0f,
                labelPoint.x + 52.0f, labelPoint.y + 10.0f),
            captionFormat_.Get(), analyzed && valid[axis] ? kMuted : kGrid);
    }
    const auto sameRadar = portraitRadarGeometry_ &&
        portraitRadarIdentity_ == presentation &&
        std::abs(portraitRadarCenter_.x - center.x) <= 0.1f &&
        std::abs(portraitRadarCenter_.y - center.y) <= 0.1f &&
        std::abs(portraitRadarRadius_ - radius) <= 0.1f;
    if (!sameRadar && factory_) {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory_->CreatePathGeometry(
                geometry.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(geometry->Open(sink.ReleaseAndGetAddressOf()))) {
            sink->BeginFigure(portrait.front(), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(
                portrait.data() + 1,
                static_cast<UINT32>(portrait.size() - 1U));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) {
                portraitRadarGeometry_ = std::move(geometry);
                portraitRadarIdentity_ = presentation;
                portraitRadarCenter_ = center;
                portraitRadarRadius_ = radius;
            }
        }
    }
    if (portraitRadarGeometry_ && target_ && brush_) {
        brush_->SetColor(mix(kBackground, accent_, 0.66f));
        brush_->SetOpacity(0.14f);
        target_->FillGeometry(portraitRadarGeometry_.Get(), brush_.Get());
        brush_->SetOpacity(1.0f);
    }
    polyline(portrait,
        analyzed ? secondary_ : mix(kMuted, kGrid, 0.5f),
        analyzed ? 2.0f : 1.0f, true);

    const auto metricsLeft = content.left + width(content) * 0.72f;
    const auto metricsRight = content.right - 22.0f;
    const auto firstTop = content.top + 54.0f;
    const auto spacing = std::clamp(
        (height(content) - 106.0f) / 8.0f, 35.0f, 58.0f);
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto top = firstTop + index * spacing;
        drawText(labels[index],
            D2D1::RectF(metricsLeft, top,
                metricsRight - 50.0f, top + 24.0f),
            captionFormat_.Get(), kMuted);
        numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        wchar_t valueText[24]{};
        if (!analyzed || !valid[index]) {
            std::swprintf(valueText, std::size(valueText), L"\u2014");
        } else {
            wcsncpy_s(
                valueText,
                presentation->portraitValueText[index].c_str(),
                _TRUNCATE);
        }
        drawText(valueText,
            D2D1::RectF(metricsRight - 48.0f, top,
                metricsRight, top + 24.0f),
            numericFormat_.Get(), kText);
        numericFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const auto barTop = top + 22.0f;
        fill(D2D1::RectF(metricsLeft, barTop, metricsRight, barTop + 2.0f), kGrid);
        if (analyzed && valid[index]) fill(D2D1::RectF(metricsLeft, barTop,
            metricsLeft + (metricsRight - metricsLeft) * values[index],
            barTop + 2.0f), index % 2 == 0 ? accent_ : secondary_);
    }

}

void ListenVisualRenderer::drawPanel(
    const D2D1_RECT_F& rectangle,
    const wchar_t* title)
{
    fill(rectangle, kPanel);
    line(rectangle.left, rectangle.top,
        rectangle.right, rectangle.top, kBorder);
    line(rectangle.left, rectangle.bottom,
        rectangle.right, rectangle.bottom, kBorder);
    line(rectangle.left, rectangle.top,
        rectangle.left, rectangle.bottom, kBorder);
    line(rectangle.right, rectangle.top,
        rectangle.right, rectangle.bottom, kBorder);
    drawText(title,
        D2D1::RectF(rectangle.left + 10.0f, rectangle.top + 5.0f,
            rectangle.right - 10.0f, rectangle.top + 28.0f),
        captionFormat_.Get(), kText);
}

void ListenVisualRenderer::drawText(
    const std::wstring& value,
    const D2D1_RECT_F& rectangle,
    IDWriteTextFormat* format,
    const D2D1_COLOR_F& textColor,
    D2D1_DRAW_TEXT_OPTIONS options)
{
    if (value.empty()) return;
    brush_->SetColor(textColor);
    target_->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()),
        format, rectangle, brush_.Get(), options);
}

void ListenVisualRenderer::drawText(
    const wchar_t* value,
    const D2D1_RECT_F& rectangle,
    IDWriteTextFormat* format,
    const D2D1_COLOR_F& textColor,
    D2D1_DRAW_TEXT_OPTIONS options)
{
    if (value == nullptr || *value == L'\0') return;
    brush_->SetColor(textColor);
    target_->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)),
        format, rectangle, brush_.Get(), options);
}

void ListenVisualRenderer::fill(
    const D2D1_RECT_F& rectangle,
    const D2D1_COLOR_F& fillColor)
{
    brush_->SetColor(fillColor);
    target_->FillRectangle(rectangle, brush_.Get());
}

void ListenVisualRenderer::line(
    float x1,
    float y1,
    float x2,
    float y2,
    const D2D1_COLOR_F& lineColor,
    float thickness)
{
    brush_->SetColor(lineColor);
    target_->DrawLine(
        D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
        brush_.Get(), thickness);
}

void ListenVisualRenderer::polyline(
    std::span<const D2D1_POINT_2F> points,
    const D2D1_COLOR_F& lineColor,
    float thickness,
    bool closed)
{
    if (points.size() < 2 || !target_ || !brush_) return;
    brush_->SetColor(lineColor);
    for (std::size_t index = 1; index < points.size(); ++index) {
        target_->DrawLine(
            points[index - 1U], points[index], brush_.Get(), thickness);
    }
    if (closed) {
        target_->DrawLine(
            points.back(), points.front(), brush_.Get(), thickness);
    }
}

void ListenVisualRenderer::drawArtwork(const D2D1_RECT_F& rectangle)
{
    if (!artworkTransition_.draw(target_.Get(), rectangle, motionMode())) {
        const auto center = D2D1::Point2F(
            (rectangle.left + rectangle.right) * 0.5f,
            (rectangle.top + rectangle.bottom) * 0.5f);
        const auto radius = std::min(width(rectangle), height(rectangle)) * 0.13f;
        line(center.x - radius, center.y - radius,
            center.x + radius, center.y + radius, kMuted, 1.0f);
        line(center.x + radius, center.y - radius,
            center.x - radius, center.y + radius, kMuted, 1.0f);
    }
}

void ListenVisualRenderer::drawFavoriteHeart(
    bool filled,
    const D2D1_RECT_F& rectangle,
    const D2D1_COLOR_F& heartColor)
{
    if (!target_ || !brush_) return;
    const auto availableWidth = width(rectangle);
    const auto availableHeight = height(rectangle);
    constexpr float size = 16.0f;
    const auto x = rectangle.left + (availableWidth - size) * 0.5f;
    const auto y = rectangle.top + (availableHeight - size) * 0.5f;
    brush_->SetColor(heartColor);
    (void)pixelIconPainter_.drawPrepared(
        target_.Get(),
        filled ? favoriteFilledIcon_ : favoriteOutlineIcon_,
        {x, y, x + size, y + size}, brush_.Get());
}

void ListenVisualRenderer::drawEvolution(
    const std::array<float, kEvolutionSamples>& values,
    std::size_t head,
    const D2D1_RECT_F& rectangle,
    const D2D1_COLOR_F& graphColor)
{
    if (width(rectangle) <= 1.0f || height(rectangle) <= 1.0f) return;
    D2D1_POINT_2F previous{rectangle.left, rectangle.bottom};
    for (std::size_t index = 0; index < kEvolutionSamples; ++index) {
        const auto source = (head + index) % kEvolutionSamples;
        const auto amount = static_cast<float>(index) /
            static_cast<float>(kEvolutionSamples - 1U);
        const auto point = D2D1::Point2F(
            rectangle.left + width(rectangle) * amount,
            rectangle.bottom - std::clamp(values[source], 0.0f, 1.0f) *
                height(rectangle));
        if (index > 0) {
            line(previous.x, previous.y, point.x, point.y, graphColor, 0.9f);
        }
        previous = point;
    }
}

D2D1_COLOR_F ListenVisualRenderer::color(
    const RgbColor& value,
    float alpha) noexcept
{
    return D2D1::ColorF(
        std::clamp(value.red, 0.0f, 1.0f),
        std::clamp(value.green, 0.0f, 1.0f),
        std::clamp(value.blue, 0.0f, 1.0f),
        std::clamp(alpha, 0.0f, 1.0f));
}

bool ListenVisualRenderer::contains(
    const D2D1_RECT_F& rectangle,
    float x,
    float y) noexcept
{
    return x >= rectangle.left && x <= rectangle.right &&
        y >= rectangle.top && y <= rectangle.bottom;
}

} // namespace nekorox::component
