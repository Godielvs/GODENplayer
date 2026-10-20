#pragma once

#include "art_cache.h"
#include "analysis_controller.h"
#include "artwork_transition_renderer.h"
#include "glyph_flow_renderer.h"
#include "library_model.h"
#include "listen_presentation_model.h"
#include "listen_layout.h"
#include "pixel_ui/pixel_icon_painter.hpp"
#include "realtime_analysis_hub.h"
#include "scope_light_pass.h"
#include "theme_service.h"
#include "render_surface_contracts.h"
#include "monitor_text_rendering.h"

#include "nekorox/contracts.h"
#include "nekorox/realtime_visual_model.h"

#include "sdk_compat.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace nekorox::component {

struct ListenBgraOverlay {
    RECT pixelRect{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::uint8_t> bgra;
};

struct ListenRenderState {
    int activeSpace = 0;
    bool crtDirectOrientation = false;
    ListenLiveFocus liveFocus = ListenLiveFocus::none;
    PlaybackSnapshotV2 playback{};
    const LibraryTrack* track = nullptr;
    // Borrowed from ListenWindow for this synchronous presentation/capture.
    std::wstring_view artworkIdentity;
    const RealtimeVisualSnapshot* realtime = nullptr;
    const std::array<float, kScopeSamples>* scopeSamples = nullptr;
    const std::array<float, kScopeSamples>* scopeMinimum = nullptr;
    const std::array<float, kScopeSamples>* scopeMaximum = nullptr;
    std::uint64_t scopeVisualRevision = 0;
    const RealtimeAnalysisSnapshot* model = nullptr;
    bool dynamicsHold = false;
    const ListenPresentationModelV3* presentation = nullptr;
    std::shared_ptr<const ArtPixels> artwork;
    ThemeState theme{};
    DeepAnalysisSnapshot analysis{};
    bool favoriteAvailable = false;
    bool favorite = false;
    bool maximized = false;
    bool pointerVisible = false;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
};

class ListenVisualRenderer final {
public:
    [[nodiscard]] bool prepareSurface(HWND window)
    {
        return ensureResources(window);
    }
    // The owner exposes this only through the doubly gated disposable-profile
    // QA message. It retires real graphics resources on the next preparation.
    void queuePreparationFaultsForQa(std::uint32_t count) noexcept
    { qaPreparationFailuresRemaining_ = count; }
    std::uint32_t qaPreparationFailuresRemaining() const noexcept
    { return qaPreparationFailuresRemaining_; }
    std::uint64_t qaPreparationFaultCount() const noexcept
    { return qaPreparationFaultCount_; }
    [[nodiscard]] HANDLE readinessHandle() const noexcept
    {
        return frameLatencyHandle_;
    }
    [[nodiscard]] SurfaceBindingKey bindingKey() const noexcept
    {
        return {surfaceLifetime_, swapchainEpoch_, frameLatencyHandle_};
    }
    [[nodiscard]] SurfaceBackend surfaceBackend() const noexcept
    {
        return softwareRasterizer_
            ? SurfaceBackend::warpDxgi : SurfaceBackend::hardwareDxgi;
    }
    FrameResult draw(HWND window, const ListenRenderState& state);
    bool drawToDc(
        HDC deviceContext,
        const RECT& pixelBounds,
        float dpi,
        const ListenRenderState& state);
    bool captureBmp(
        const std::filesystem::path& path,
        std::uint32_t pixelWidth,
        std::uint32_t pixelHeight,
        float dpi,
        const ListenRenderState& state,
        const ListenBgraOverlay* overlay = nullptr);
    void resize(std::uint32_t width, std::uint32_t height);
    void discard();
    void displayEnvironmentChanged() noexcept;
    void invalidateArtwork();
    void clearLiveHistory() noexcept;
    void clearArtwork() noexcept { artworkTransition_.clear(); }
    [[nodiscard]] bool artworkTransitionActive() const noexcept
    {
        return artworkTransition_.active(motionMode());
    }
    [[nodiscard]] bool artworkReady() const noexcept
    {
        return artworkTransition_.ready();
    }
    [[nodiscard]] bool glyphTransitionActive() const noexcept
    {
        return glyphOverlayPresented_ ||
            GlyphFlowService::instance().active(motionMode());
    }

    D2D1_RECT_F musicMapRect() const noexcept { return musicMapRect_; }
    D2D1_RECT_F playPauseRect() const noexcept { return playPauseRect_; }
    D2D1_RECT_F transportSeekRect() const noexcept { return transportSeekRect_; }
    D2D1_RECT_F analyzeRect() const noexcept { return analyzeRect_; }
    D2D1_RECT_F favoriteRect() const noexcept { return favoriteRect_; }
    D2D1_RECT_F captionRect() const noexcept { return captionRect_; }
    D2D1_RECT_F minimizeRect() const noexcept { return minimizeRect_; }
    D2D1_RECT_F maximizeRect() const noexcept { return maximizeRect_; }
    D2D1_RECT_F closeRect() const noexcept { return closeRect_; }
    D2D1_RECT_F liveModeRect() const noexcept { return liveModeRect_; }
    D2D1_RECT_F dynamicsHoldRect() const noexcept { return dynamicsHoldRect_; }
    D2D1_RECT_F liveVectorPlotRect() const noexcept
    {
        return liveVectorPlotRect_;
    }
    ListenLiveLayout liveLayout() const noexcept { return liveLayout_; }

private:
    std::uint32_t qaPreparationFailuresRemaining_ = 0;
    std::uint64_t qaPreparationFaultCount_ = 0;
    bool ensureDeviceIndependentResources();
    void ensurePixelIcons();
    bool ensureResources(HWND window);
    bool createCompositionResources(HWND window);
    bool createCompositionSwapChain(
        std::uint32_t width,
        std::uint32_t height);
    bool bindCompositionBackBuffer(float dpi);
    FrameResult drawFrame(const ListenRenderState& state);
    void discardTargetResources();
    void prepareArtwork(
        std::wstring_view identity,
        const std::shared_ptr<const ArtPixels>& pixels);
    void prepareSpectrogram(const RealtimeAnalysisSnapshot& model);
    void prepareMusicMapRibbons(
        const DeepAnalysisSnapshot& analysis,
        const D2D1_RECT_F& rectangle);

    void drawChrome(const ListenRenderState& state, const D2D1_SIZE_F& size);
    void drawOverview(const ListenRenderState& state, const D2D1_RECT_F& content);
    void drawLive(const ListenRenderState& state, const D2D1_RECT_F& content);
    void drawLiveStatic(const ListenRenderState& state, const D2D1_RECT_F& content);
    void drawLiveDynamic(const ListenRenderState& state, const D2D1_RECT_F& content);
    bool prepareLiveStatic(const ListenRenderState& state, const D2D1_RECT_F& content);
    void drawMusicMap(const ListenRenderState& state, const D2D1_RECT_F& content);
    void drawSignalPortrait(const ListenRenderState& state, const D2D1_RECT_F& content);

    void drawPanel(const D2D1_RECT_F& rectangle, const wchar_t* title);
    void drawText(
        const std::wstring& value,
        const D2D1_RECT_F& rectangle,
        IDWriteTextFormat* format,
        const D2D1_COLOR_F& color,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_CLIP);
    void drawText(
        const wchar_t* value,
        const D2D1_RECT_F& rectangle,
        IDWriteTextFormat* format,
        const D2D1_COLOR_F& color,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_CLIP);
    void fill(const D2D1_RECT_F& rectangle, const D2D1_COLOR_F& color);
    void line(
        float x1,
        float y1,
        float x2,
        float y2,
        const D2D1_COLOR_F& color,
        float thickness = 1.0f);
    void polyline(
        std::span<const D2D1_POINT_2F> points,
        const D2D1_COLOR_F& color,
        float thickness = 1.0f,
        bool closed = false);
    void drawArtwork(const D2D1_RECT_F& rectangle);
    void drawFavoriteHeart(
        bool filled,
        const D2D1_RECT_F& rectangle,
        const D2D1_COLOR_F& color);
    void drawEvolution(
        const std::array<float, kEvolutionSamples>& values,
        std::size_t head,
        const D2D1_RECT_F& rectangle,
        const D2D1_COLOR_F& color);

    static D2D1_COLOR_F color(const RgbColor& value, float alpha = 1.0f) noexcept;
    static bool contains(const D2D1_RECT_F& rectangle, float x, float y) noexcept;

    HWND window_ = nullptr;
    HMONITOR renderMonitor_ = nullptr;
    MonitorTextRendering textRendering_;
    bool adapterCheckRequired_ = true;
    std::uint64_t renderRecoveryEpoch_ = 0;
    std::uint32_t compositionWidth_ = 0;
    std::uint32_t compositionHeight_ = 0;
    bool softwareRasterizer_ = false;
    HANDLE frameLatencyHandle_ = nullptr;
    std::uint64_t surfaceLifetime_ = 0;
    std::uint64_t swapchainEpoch_ = 0;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> compositionBitmap_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> captionFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> strongFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> heroFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> numericFormat_;
    ArtworkTransitionRenderer artworkTransition_;
    ScopeLightPass scopeLightPass_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> spectrogramBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> liveStaticBitmap_;
    goden::pixel::PixelIconPainter pixelIconPainter_;
    goden::pixel::PixelIconPainter::Handle favoriteOutlineIcon_{};
    goden::pixel::PixelIconPainter::Handle favoriteFilledIcon_{};
    goden::pixel::PixelIconPainter::Handle minimizeIcon_{};
    goden::pixel::PixelIconPainter::Handle maximizeIcon_{};
    goden::pixel::PixelIconPainter::Handle restoreIcon_{};
    goden::pixel::PixelIconPainter::Handle closeIcon_{};
    goden::pixel::PixelIconPainter::Handle focusEnterIcon_{};
    goden::pixel::PixelIconPainter::Handle focusExitIcon_{};
    float pixelIconDpi_ = 0.0f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> portraitRadarGeometry_;
    std::array<Microsoft::WRL::ComPtr<ID2D1PathGeometry>, 4>
        musicMapRibbonGeometry_{};
    GlyphFlowRenderer glyphFlowRenderer_;
    bool glyphOverlayPresented_ = false;
    const ArtPixels* glyphArtworkIdentity_ = nullptr;
    const ListenPresentationModelV3* portraitRadarIdentity_ = nullptr;
    D2D1_POINT_2F portraitRadarCenter_{};
    float portraitRadarRadius_ = 0.0f;
    std::wstring glyphArtworkKey_;
    std::wstring chromeIdentityKey_;
    std::wstring chromeIdentityText_;
    std::uint64_t spectrogramRevision_ = 0;
    std::uint64_t spectrogramEpoch_ = 0;
    std::uint64_t dynamicsGeometryRevision_ = 0;
    D2D1_RECT_F dynamicsGeometryRect_{};
    std::size_t dynamicsGeometryCount_ = 0;
    std::array<D2D1_POINT_2F, DynamicsSnapshot::capacity>
        dynamicsPeakPoints_{};
    std::array<D2D1_POINT_2F, DynamicsSnapshot::capacity>
        dynamicsRmsPoints_{};
    std::array<bool, DynamicsSnapshot::capacity> dynamicsMeasured_{};
    std::wstring dynamicsBadgeText_ =
        L"PEAK --  \u00b7  RMS --  \u00b7  CREST --";
    std::chrono::steady_clock::time_point dynamicsBadgeUpdated_{};
    std::uint64_t liveStaticThemeGeneration_ = 0;
    int liveStaticMode_ = -1;
    std::uint32_t liveStaticSampleRate_ = 0;
    ListenLiveFocus liveStaticFocus_ = ListenLiveFocus::none;
    D2D1_RECT_F liveStaticContent_{};
    std::size_t spectrogramHead_ = 0;
    const std::vector<AnalysisWaveformBinV1>* musicMapWaveformIdentity_ = nullptr;
    std::size_t musicMapWaveformSize_ = 0;
    MusicMapProfile musicMapProfile_{};
    D2D1_RECT_F musicMapGeometryRect_{};
    std::array<std::uint32_t, kSpectrogramColumns * kSpectrogramBins>
        spectrogramPixels_{};
    std::array<std::uint32_t, kSpectrogramBins>
        spectrogramColumnPixels_{};
    D2D1_RECT_F musicMapRect_{};
    D2D1_RECT_F playPauseRect_{};
    D2D1_RECT_F transportSeekRect_{};
    D2D1_RECT_F analyzeRect_{};
    D2D1_RECT_F favoriteRect_{};
    D2D1_RECT_F captionRect_{};
    D2D1_RECT_F minimizeRect_{};
    D2D1_RECT_F maximizeRect_{};
    D2D1_RECT_F closeRect_{};
    D2D1_RECT_F liveModeRect_{};
    D2D1_RECT_F dynamicsHoldRect_{};
    D2D1_RECT_F liveVectorPlotRect_{};
    ListenLiveLayout liveLayout_{};
    D2D1_COLOR_F accent_{0.0f, 0.847f, 0.816f, 1.0f};
    D2D1_COLOR_F secondary_{0.66f, 0.91f, 0.90f, 1.0f};
};

} // namespace nekorox::component
