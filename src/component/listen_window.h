#pragma once

#include "art_cache.h"
#include "crt_xy_surface.h"
#include "frame_clock.h"
#include "library_model.h"
#include "listen_lifecycle.h"
#include "listen_presentation_model.h"
#include "listen_visual_renderer.h"
#include "presentation_revision.h"
#include "realtime_analysis_hub.h"
#include "scrub_coordinator.h"
#include "sdk_compat.h"
#include "window_monitor_identity.h"

#include "nekorox/contracts.h"
#include "nekorox/crt_xy_model.h"
#include "nekorox/realtime_visual_model.h"

#include <cstdint>
#include <atomic>
#include <limits>
#include <memory>

namespace nekorox::component {

class ListenWindow final {
public:
    static ListenWindow& instance();
    void show();
    void close();
    void requestRefresh();
    void notifyArtworkChanged();
    void notifyPlaybackTrackTransition(const metadb_handle_ptr& nextTrack);
    bool isOpen() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool occludes(HWND target) const noexcept;

private:
    ListenWindow();
    ~ListenWindow();

    static LRESULT CALLBACK windowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    static void CALLBACK analysisDwellWake(
        PTP_CALLBACK_INSTANCE,
        void* context,
        PTP_TIMER) noexcept;
    LRESULT handleMessage(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    void paint();
    ListenRenderState renderState() const;
    void tick();
    void refreshTrack(bool force = false);
    void refreshArtwork();
    void refreshPresentationModel();
    void updateInterpolatedSnapshot();
    void syncFrameClock();
    void syncRealtimeDemand();
    void syncCrtSurface();
    [[nodiscard]] ListenLiveLayout currentLiveLayout() const noexcept;
    void setLiveFocus(ListenLiveFocus focus);
    void setDynamicsHold(bool hold);
    void updateVisibilityLifecycle(bool visible);
    void syncPlaybackLifecycle();
    void resetRealtimePresentation();
    void scheduleAnalysisDwell();
    void cancelAnalysisDwell(bool cancelRunning);
    void handleAnalysisDwellTimer();
    void scheduleMusicMapCursor();
    void setCrtDirectOrientation(bool direct);
    bool dispatchCommand(const ListenCommandV1& command);
    [[nodiscard]] double normalizedSeekPoint(float x) const noexcept;
    void startOrCancelAnalysis();
    std::filesystem::path nativeTrackPath() const;
    D2D1_POINT_2F pointInDip(LPARAM lParam) const noexcept;

    HWND window_ = nullptr;
    WindowMonitorIdentity monitorIdentity_;
    void refreshMonitorIdentity(HWND window) noexcept;
    mutable HWND occlusionTarget_ = nullptr;
    mutable bool targetOccluded_ = false;
    ArtCache artCache_;
    ListenVisualRenderer renderer_;
    metadb_handle_ptr track_;
    std::wstring artworkIdentity_;
    LibraryTrack trackInfo_;
    bool hasTrackInfo_ = false;
    bool trackTransitionPending_ = false;
    std::shared_ptr<const ArtPixels> artwork_;
    std::shared_ptr<const ListenPresentationModelV3> presentationModel_;
    const std::string* presentationAnalysisIdentity_ = nullptr;
    std::uint64_t presentationLibraryGeneration_ = 0;
    std::uint64_t presentationRequestId_ = 0;
    DeepAnalysisStatus presentationStatus_ = DeepAnalysisStatus::idle;
    AnalysisStage presentationStage_ = AnalysisStage::queued;
    int presentationProgressPercent_ = -1;
    const ArtPixels* themedArtwork_ = nullptr;

    int activeSpace_ = 0;
    ListenLiveFocus liveFocus_ = ListenLiveFocus::none;
    bool crtDirectOrientation_ = true;
    bool pointerVisible_ = false;
    bool trackingPointer_ = false;
    bool seeking_ = false;
    std::uint64_t scrubSession_ = 0;
    bool scrubDragging_ = false;
    float scrubPointerDownX_ = 0.0f;
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;

    std::uint64_t frameSequence_ = 0;
    RealtimeFrameV1 frame_{};
    std::shared_ptr<const RealtimeAnalysisSnapshot> analysisSnapshot_;
    std::uint64_t analysisGeneration_ = 0;
    CompactScopePresenter scopePresentation_;
    std::uint64_t scopePresentationRevision_ = 0;
    bool dynamicsHold_ = false;
    LARGE_INTEGER scopePresentationQpc_{};
    PlaybackState presentedPlaybackState_ = PlaybackState::stopped;
    WindowFrameClock frameClock_;
    PresentationRevision presentationRevision_;
    std::uint64_t preparationRetryQpc_ = 0;
    std::uint64_t preparationAttempts_ = 0;
    std::uint64_t preparationFailures_ = 0;
    ListenLifecycleState lifecycle_;
    PTP_TIMER analysisDwellWake_ = nullptr;
    std::atomic<HWND> analysisDwellWindow_{nullptr};
    bool realtimeConsumerActive_ = false;
    CrtXyModel crtXyModel_;
    CrtXySurface crtXySurface_;
    std::uint64_t crtStreamGeneration_ = 0;
    std::uint64_t crtExpectedFrameIndex_ = 0;
    std::uint32_t crtSampleRate_ = 0;
    bool hasCrtStream_ = false;
    bool crtTailPending_ = false;
    RealtimeVisualSnapshot previousSnapshot_{};
    RealtimeVisualSnapshot currentSnapshot_{};
    RealtimeVisualSnapshot displaySnapshot_{};
    bool hasVisualSnapshot_ = false;
    bool hasPreviousSnapshot_ = false;
    std::uint64_t snapshotArrivalQpc_ = 0;
    std::uint64_t previousAudioQpc_ = 0;
    double qpcFrequency_ = 1.0;
    double framePeriodTicks_ = 1.0;
    std::int64_t lastPlaybackCursorStep_ =
        (std::numeric_limits<std::int64_t>::min)();
};

} // namespace nekorox::component
