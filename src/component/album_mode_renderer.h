#pragma once
#include "curated/curated_renderer.h"
#include "collection/visual_mux.h"

#include "album_mode_view.h"
#include "a4/a4_pipeline.h"
#include "a4/a4_track_extras.h"
#include "a4/a4_artwork_gate.h"
#include "album_booklet_model.h"
#include "album_ambient_controller.h"
#include "album_ambient_effect.h"
#include "album_session_service.h"
#include "art_cache.h"
#include "artwork_transition_renderer.h"
#include "local_content_cache.h"
#include "panel_d2d.h"
#include "text_layout_cache.h"
#include "async_text_view.h"

#include "nekorox/lyrics_document.h"
#include "nekorox/lyrics_match_policy.h"
#include "nekorox/scroll_controller.h"

#include <chrono>
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <d2d1_1.h>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include <wrl/client.h>

namespace nekorox::component {

struct LyricsHitToken {
    std::uint64_t presentationRevision = 0;
    std::uint64_t gestureRevision = 0;
    std::size_t cueIndex = 0;
    double seekSeconds = 0.0;
};

class AlbumModeRenderer final {
public:
    LRESULT collectionQa(unsigned field,LPARAM value) noexcept {return curated_.qa(field,value);}
    bool curatedSelected() const noexcept;
    void setCuratedActivity(bool active) noexcept;
    void setCuratedPlaying(bool playing) noexcept {curatedPlaying_=playing;}
    void setCuratedDefer(bool value) noexcept {curatedDeferred_=value;}
    void suspendCurated() noexcept;
    bool curatedAnimationActive() const noexcept;
    bool curatedFrameDue() const noexcept;
    std::uint64_t curatedDeadlineQpc() const noexcept;
    const curated::Diagnostics& curatedDiagnostics() const noexcept {return curated_.diagnostics();}

    [[nodiscard]] bool prepareSurface(HWND window)
    {
        return render_.prepare(window);
    }
    void queueSurfaceFaultsForQa(std::uint32_t count) noexcept { render_.queuePreparationFaultsForQa(count); }
    [[nodiscard]] std::uint64_t surfaceAttemptsForQa() const noexcept { return render_.qaPreparationAttempts(); }
    [[nodiscard]] std::uint64_t surfaceFaultsForQa() const noexcept { return render_.qaPreparationFaults(); }
    [[nodiscard]] std::uint32_t surfaceFaultsRemainingForQa() const noexcept { return render_.qaPreparationRemaining(); }
    [[nodiscard]] HANDLE readinessHandle() const noexcept
    {
        return render_.readinessHandle();
    }
    [[nodiscard]] SurfaceBindingKey bindingKey() const noexcept
    {
        return render_.bindingKey();
    }
    [[nodiscard]] SurfaceBackend surfaceBackend() const noexcept
    {
        return render_.backend();
    }
    [[nodiscard]] FrameResult lastFrameResult() const noexcept
    {
        return render_.lastFrameResult();
    }
    FrameOutcome paintNavigation(HWND window, HDC dc, bool albumActive,
        float heightDip = 36.0f);
    FrameOutcome paint(HWND window, HDC dc,
        const std::shared_ptr<const AlbumSessionSnapshot>& session,
        const std::shared_ptr<const LocalTrackContent>& content,
        const std::shared_ptr<const LyricsSnapshot>& lyrics,
        const std::shared_ptr<const ArtPixels>& artwork,
        const std::wstring& artworkIdentity,
        double playbackSeconds,
        float trackScrollDip,
        AlbumPanelKind compactSelection,
        AlbumPanelVisibility visibility,
        float bookletProgress,
        std::wstring_view personalNote,
        std::uint64_t personalNoteRevision,
        const AlbumAmbientPose& ambientPose,
        bool rebuildAmbientTexture,
        float topInsetDip = 0.0f,
        float bottomInsetDip = 0.0f,
        ArtCacheStatus artworkStatus = ArtCacheStatus::loading);
    void resize();
    void setFocusRect(D2D1_RECT_F rect) noexcept {focusRect_=rect;}
    void setFocusedTrack(std::optional<std::size_t> row) noexcept {focusedTrack_=row;}
    void releaseGraphics() noexcept;
    void retryVisualStyle() noexcept;
    bool setOpticsPointer(float x,float y) noexcept;
    D2D1_RECT_F visualMenuRect(const AlbumModeLayout&) const noexcept;
    std::optional<a4::RatingHit> ratingHitAt(float x,float y) const noexcept {return trackExtras_.hit(x,y);}
    bool validateRatingHit(const a4::RatingHit& hit) const noexcept {return trackExtras_.validate(hit);}
    bool applyRatingHit(const a4::RatingHit& hit,HWND owner) {return trackExtras_.apply(hit,owner);}
    bool capAlloyForDiagnostics(unsigned tier) noexcept { return alloy_.diagnosticCap(tier); }
    [[nodiscard]] const a4::Pipeline::Diagnostics& alloyDiagnostics() const noexcept
    {
        return alloy_.diagnostics();
    }
    [[nodiscard]] bool alloySceneReady() const noexcept { return alloySceneReady_; }

    unsigned ambientRetryMilliseconds() const noexcept {
        const auto now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return alloySelected_?alloy_.retryMilliseconds(now):0;
    }
    void reset();
    void setActionStatus(std::wstring text) {
        actionStatus_=std::move(text);actionStatusUntil_=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    }
    void noteManualLyricsInteraction() noexcept;
    void scrollLyrics(float deltaDip) noexcept;
    void scrollBooklet(float deltaDip) noexcept;
    void suspendTextPreparation() noexcept {
        textDemand_ = surfaceTextDemand_ = false; bookletText_.cancel(); plainLyricsText_.cancel();
    }
    void setTextDemand(bool value) noexcept {
        if (!value) { textDemand_ = false; bookletText_.cancel(); } else textDemand_ = true;
    }
    void setSurfaceTextDemand(bool value) noexcept {
        surfaceTextDemand_ = value;
        if (!value) suspendTextPreparation();
    }
    [[nodiscard]] AsyncTextStats bookletTextStats() const noexcept { return bookletText_.stats(); }
    [[nodiscard]] AsyncTextStats plainTextStats() const noexcept { return plainLyricsText_.stats(); }
    [[nodiscard]] std::size_t liveTextLayouts() const noexcept { return textBudget_->live.load(); }
    [[nodiscard]] std::size_t peakTextLayouts() const noexcept { return textBudget_->peak.load(); }
    [[nodiscard]] float bookletMaximumScroll() const noexcept { return bookletMaxScrollDip_; }
    [[nodiscard]] float bookletScrollPosition() const noexcept { return bookletScrollDip_; }
    void followLyrics() noexcept;
    void suspendLyricsScroll() noexcept
    {
        lyricScroll_.setPosition(lyricScroll_.position());
        lyricScrollDip_ = static_cast<float>(lyricScroll_.position());
        ++lyricsGestureRevision_;
    }
    void noteLyricsSeekIntent(const LyricsHitToken& token) noexcept;
    [[nodiscard]] std::optional<LyricsHitToken> lyricsHitAt(
        const AlbumModeLayout& layout, float xDip, float yDip) const noexcept;
    [[nodiscard]] bool validateLyricsHit(
        const LyricsHitToken& token) const noexcept;
    [[nodiscard]] bool animationActive() const noexcept;
    // Read-only diagnostic view of the last applied eight-region light state.
    [[nodiscard]] const collection::LightBands& appliedLights() const noexcept
    {
        return curated_.appliedLights();
    }
    // Album lyrics scroll telemetry. These are read-only views of the single
    // controller the lyrics column owns; TRACKS keeps its own controller in the
    // player shell, so the two lists never share state.
    [[nodiscard]] double lyricScrollPosition() const noexcept
    {
        return lyricScroll_.position();
    }
    [[nodiscard]] double lyricScrollTarget() const noexcept
    {
        return lyricScroll_.target();
    }
    [[nodiscard]] double lyricScrollVelocity() const noexcept
    {
        return lyricScroll_.velocity();
    }
    [[nodiscard]] bool lyricScrollActive() const noexcept
    {
        return lyricScroll_.active();
    }
    [[nodiscard]] double lyricMaximumScroll() const noexcept
    {
        return lyricMaximumScrollDip_;
    }
    // QA-only: returns the lyric list to a settled top offset so a scripted
    // wheel schedule begins from a known state. Never reached by user input.
    void resetLyricScroll() noexcept
    {
        lyricScroll_.setPosition(0.0);
        lyricScrollDip_ = 0.0f;
        ++lyricsGestureRevision_;
    }
    // QA-only: suspends the follow-the-playing-cue repositioning so a scripted
    // schedule measures its own deltas instead of competing with playback.
    void holdLyricsFollow() noexcept
    {
        manualLyricsUntil_ = std::chrono::steady_clock::now() +
            std::chrono::hours(1);
    }
    [[nodiscard]] bool lastAmbientTextureProduced() const noexcept
    {
        return ambientTextureProduced_;
    }
    [[nodiscard]] bool ambientFallbackRequired() const noexcept
    {
        if(curatedSelected_)return curated_.failed();
        return ambientBudgetFallback_ || (alloySelected_ && !alloySceneReady_ &&
            (FAILED(alloy_.diagnostics().lastHr)||alloy_.diagnostics().latched));
    }
    [[nodiscard]] bool artworkTransitionActive() const noexcept
    {
        return artworkTransition_.active(motionMode());
    }
    [[nodiscard]] double artworkIntervalP95Milliseconds() const noexcept
    { return artworkTransition_.acceptedIntervalP95Milliseconds(); }
    [[nodiscard]] const ArtworkTransitionDiagnostics&
        artworkTransitionDiagnostics() const noexcept
    {
        return artworkTransition_.diagnostics();
    }
    [[nodiscard]] std::optional<std::uint32_t> nextLyricsWakeMilliseconds(
        double playbackSeconds) const noexcept;

private:
    void drawNavigation(const D2D1_SIZE_F& size, float heightDip,
        bool albumActive);
    void ensureFormats();
    void onResourceEpochChanged(std::uint64_t epoch) noexcept;
    void ensureArtwork(
        const std::shared_ptr<const ArtPixels>& artwork,
        std::wstring_view identity,
        float dpi, ArtCacheStatus status);
    void ensureLyrics(const std::shared_ptr<const LyricsSnapshot>& lyrics);
    void ensureLyricsLayout(float textWidth);
    bool prepareAmbientEffect(
        Microsoft::WRL::ComPtr<ID2D1Effect>& effect);
    enum class AmbientBuildResult : std::uint8_t {
        notRequested,
        produced,
        effectUnavailable,
        constantsRejected,
        drawFailed,
    };
    AmbientBuildResult buildAmbientTexture(ID2D1Effect* effect,
        const ArtPalette& palette, const AlbumAmbientPose& pose,
        const D2D1_RECT_F& lyricsRect, const D2D1_RECT_F& tracksRect,
        std::uint64_t artworkHash, const D2D1_RECT_F& target,
        Microsoft::WRL::ComPtr<ID2D1Bitmap1>& output);
    void collectAmbientGpuTiming() noexcept;
    std::size_t beginAmbientGpuTiming() noexcept;
    void endAmbientGpuTiming(std::size_t slot) noexcept;
    void resetAmbientGpuTiming() noexcept;
    void publishAmbientRenderDiagnostics() const noexcept;
    bool prepareBookletBackdrop(float topInsetDip,
        const D2D1_RECT_F& coverRect);
    void drawBookletBackdrop(float topInsetDip,
        const D2D1_RECT_F& contentRect, float opacity);
    void drawArtwork(ID2D1Bitmap* bitmap, const D2D1_RECT_F& target,
        float opacity = 1.0f) const;
    void drawTracklist(const AlbumModeLayout& layout,
        const AlbumSessionSnapshot& session, float scrollDip);
    void drawLyrics(const AlbumModeLayout& layout, double playbackSeconds);
    void preparePlainLyrics(HWND window, const AlbumModeLayout& layout);
    void drawPlainLyrics(const AlbumModeLayout& layout);

    PanelD2D render_;
    D2D1_RECT_F focusRect_{};
    std::optional<std::size_t> focusedTrack_;
    collection::VisualMux curated_;
    bool curatedSelected_=false,curatedActive_=false,curatedDeferred_=false,curatedPlaying_=false;
    a4::Pipeline alloy_;
    a4::TrackExtras trackExtras_;
    a4::ArtworkGate artworkGate_;
    std::shared_ptr<const ArtPixels> displayedArtwork_;
    std::wstring artworkRequestIdentity_;
    std::uint64_t artworkRequestSerial_=0,artworkVisualRevision_=0;
    float artworkOpacity_=0;
    bool artworkVisible_=true;
    bool alloySceneReady_=false,alloySelected_=false;
    bool initialUiPresented_=false,graphicsStaging_=false,lyricsStaging_=false,lyricsRetry_=false;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> lyricBrush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> lyricActive_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> lyricNeighbour_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> lyricPlain_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bookletTitle_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bookletBody_;
    Microsoft::WRL::ComPtr<ID2D1Effect> ambientEffect_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> ambientTexture_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> previousAmbientTexture_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bookletBackdrop_;
    Microsoft::WRL::ComPtr<ID2D1Effect> bookletBackdropBlur_;
    ArtPalette ambientPalette_{};
    ArtPalette previousAmbientPalette_{};
    ArtworkTransitionRenderer artworkTransition_;
    std::wstring artworkIdentity_;
    std::uint64_t artworkHash_ = 0;
    std::uint64_t previousArtworkHash_ = 0;
    std::uint32_t artworkWidth_ = 0;
    std::uint32_t artworkHeight_ = 0;
    std::uintptr_t artworkTarget_ = 0;
    std::uint64_t observedResourceEpoch_ = 0;
    std::chrono::steady_clock::time_point artworkChangedAt_{};
    std::uint64_t lyricsPresentationRevision_ = 0;
    std::uint64_t lyricsRevision_ = 0;
    std::shared_ptr<const ::nekorox::LyricsDocumentV2> lyrics_;
    std::vector<std::wstring> lyricLines_;
    struct PreparedLyricsLine {
        std::shared_ptr<const RetainedTextDocument> normal;
        float top = 0.0f;
        float height = 0.0f;
    };
    struct LyricVisualState {
        float value = 0.0f;
        float from = 0.0f;
        bool targetActive = false;
        std::chrono::steady_clock::time_point started{};
    };
    std::vector<PreparedLyricsLine> preparedLyrics_;
    mutable std::vector<LyricVisualState> lyricVisuals_;
    std::uint64_t preparedLyricsRevision_ = 0;
    float preparedLyricsWidth_ = 0.0f;
    float preparedLyricsHeight_ = 0.0f;
    std::chrono::steady_clock::time_point manualLyricsUntil_{};
    float lyricScrollDip_ = 0.0f;
    float lyricMaximumScrollDip_ = 0.0f;
    nekorox::ScrollController lyricScroll_{};
    std::optional<std::size_t> pendingCueIndex_;
    std::chrono::steady_clock::time_point pendingCueStarted_{};
    mutable std::uint64_t lyricsGestureRevision_ = 1;
    std::shared_ptr<TextLayoutBudget> textBudget_ = std::make_shared<TextLayoutBudget>();
    // Reserve room for background scratch and both old/new visible text frames.
    TextLayoutCache textLayouts_{textBudget_, 2048U};
    AsyncTextView bookletText_{textBudget_};
    AsyncTextView plainLyricsText_{textBudget_};
    std::shared_ptr<const AsyncTextFrame> bookletFrame_;
    std::shared_ptr<const AsyncTextFrame> plainLyricsFrame_;
    std::shared_ptr<const AsyncTextBatch> plainLyricsBatch_;
    RecordingKey lyricsRecording_;
    std::uint64_t plainLyricsOwner_ = 0;
    float pendingPlainWheel_ = 0.f;
    bool textDemand_ = false, surfaceTextDemand_ = false;
    std::uint64_t writtenTextRevision_ = 0;
    std::shared_ptr<const std::wstring> writtenText_;
    std::uint64_t writtenOwner_ = 0;
    std::wstring actionStatus_;
    std::chrono::steady_clock::time_point actionStatusUntil_{};
    float bookletScrollDip_ = 0.0f;
    float bookletMaxScrollDip_ = 0.0f;
    bool bookletOpen_ = false;
    struct AmbientTimingSlot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> begin;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };
    std::array<AmbientTimingSlot, 6> ambientTiming_{};
    Microsoft::WRL::ComPtr<ID3D11Device> ambientTimingDevice_;
    std::array<double, 120> ambientGpuSamplesUs_{};
    std::size_t ambientGpuSampleCount_ = 0;
    std::size_t ambientGpuSampleCursor_ = 0;
    std::size_t ambientGpuSamplesSinceWindow_ = 0;
    std::size_t nextAmbientTimingSlot_ = 0;
    unsigned overBudgetWindows_ = 0;
    bool ambientDegraded_ = false;
    bool ambientBudgetFallback_ = false;
    bool ambientTextureProduced_ = false;
    double ambientGpuLatestUs_ = 0.0;
    double ambientGpuP95Us_ = 0.0;
};

} // namespace nekorox::component
