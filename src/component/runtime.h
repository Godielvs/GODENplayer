#pragma once

#include "nekorox/contracts.h"
#include "nekorox/continuity.h"
#include "nekorox/realtime_ring.h"

#include "sdk_compat.h"
#include "visual_capture_recovery.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace nekorox::component {

enum class RealtimeConsumer : std::uint32_t {
    listen = 1U << 0U,
    compactMonitor = 1U << 1U,
    albumAmbient = 1U << 2U,
};

class RuntimePlaylistObserver;

class Runtime final {
public:
    static Runtime& instance();

    void start();
    void stop();
    void setRealtimeConsumerActive(RealtimeConsumer consumer, bool active);
    [[nodiscard]] std::uint64_t discardRealtimeFrames(
        RealtimeConsumer consumer) noexcept;
    void pumpVisualisation();
    struct VisualPumpCounters {
        std::uint64_t calls;
        std::uint64_t readFailures;
        std::uint64_t catchupResets;
        std::uint64_t publishRejected;
        std::uint64_t clockTransientFailures;
        std::uint64_t authoritativeDiscontinuities;
        std::uint64_t discontinuityCommitted;
        std::uint64_t softCatchups;
        std::uint64_t clockBackwardResets;
        std::uint64_t lastClockBackwardUs;
        std::uint64_t lastDiscontinuityReason;
        std::uint64_t clockJitterIgnored;
        std::uint64_t lastClockJitterUs;
    };
    VisualPumpCounters visualPumpCounters()const noexcept {return {
        visualPumpCalls_.load(),visualReadFailures_.load(),visualCatchupResets_.load(),visualPublishRejected_.load(),
        clockTransientFailures_.load(),authoritativeDiscontinuities_.load(),discontinuityCommitted_.load(),visualSoftCatchups_.load(),
        visualClockBackwardResets_.load(),visualLastClockBackwardUs_.load(),lastDiscontinuityReason_.load(),
        visualClockJitterIgnored_.load(),visualLastClockJitterUs_.load()};}
    bool consumeLatestFrame(
        RealtimeConsumer consumer,
        RealtimeFrameV1& frame,
        std::uint64_t& sequence,
        std::uint64_t* skipped = nullptr) noexcept;
    bool consumeNextFrame(
        RealtimeConsumer consumer,
        RealtimeFrameV1& frame,
        std::uint64_t& sequence,
        std::uint64_t* skipped = nullptr) noexcept;
    [[nodiscard]] std::uint64_t latestRealtimeSequence() const noexcept;
    [[nodiscard]] bool dawPreviewActive() const noexcept;
    void notifyDawPreviewChanged();

    PlaybackSnapshotV2 playbackSnapshot() const;
    [[nodiscard]] PlaybackPositionSnapshotV1
        playbackPositionSnapshot() const noexcept;
    // Smooth presentation clock derived from the last foobar callback. This
    // never drives playback; it only lets every visual surface sample the same
    // QPC-extrapolated position between the SDK's comparatively sparse ticks.
    [[nodiscard]] double presentationPlaybackPosition() const noexcept;
    [[nodiscard]] std::uint64_t streamGeneration() const noexcept;
    [[nodiscard]] std::uint64_t playbackSerial() const noexcept
    {
        return playbackSerial_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t recordingIdentityHash() const noexcept
    {
        return recordingIdentityHash_.load(std::memory_order_acquire);
    }
    metadb_handle_ptr currentTrackHandle() const;
    std::string nowPlayingText() const;
    void markRealtimeDiscontinuity(
        DiscontinuityReason reason = DiscontinuityReason::explicitReset) noexcept;
    void admitRealtimePlayback(const metadb_handle_ptr& track) noexcept;
    void notifyQueueChanged();
    void notifyPlaybackChanged(bool preserveDiscordActivity = false);
    void notifyPlaybackNewTrack(const metadb_handle_ptr& track);
    void notifyPlaybackPauseChanged(bool paused);
    bool recoverVisualisationSource() noexcept;
    std::uint64_t visualSourceRecoveryCount() const noexcept {
        return visualSourceRecoveries_.load(std::memory_order_relaxed);
    }
    void simulateVisualSourceStallForQa() noexcept {qaVisualSourceStalled_=true;}
    void notifyPlaybackMetadataChanged(const metadb_handle_ptr& track = {});
    void refreshPresentationState();
    void updatePlaybackVolume(float volumeDb) noexcept;
    void updatePlaybackPosition(
        double seconds,
        bool seek,
        std::uint64_t trackIdentityHash);
    void updatePlaybackPositionFromCallback(double seconds);

private:
    Runtime();
    ~Runtime();
    void startVisualisation();
    void stopVisualisation();
    void publishAudio(
        const audio_chunk& chunk,
        std::uint64_t chunkEndQpc) noexcept;
    void refreshNowPlayingText(const metadb_handle_ptr& trackHint = {});
    void populatePresentationState(PlaybackSnapshotV2& snapshot) const;
    void publishPlaybackSnapshot(PlaybackSnapshotV2 snapshot);

    mutable std::mutex stateMutex_;
    std::atomic<std::shared_ptr<const PlaybackSnapshotV2>> playbackSnapshot_;
    metadb_handle_ptr currentTrack_;
    std::string nowPlayingText_ = "Stopped";
    std::atomic<double> playbackSeconds_{-1.0};
    std::atomic<std::uint64_t> playbackPositionTrackHash_{0};
    std::atomic<std::uint64_t> playbackPositionQpc_{0};
    std::atomic<std::uint64_t> playbackPositionRevision_{0};
    std::atomic<std::uint64_t> playbackSeekGeneration_{0};
    std::atomic<float> playbackVolumeDb_{-100.0f};
    std::atomic<bool> playbackMuted_{false};
    std::atomic<bool> started_{false};
    std::array<std::uint32_t, 3> realtimeConsumerCounts_{};
    std::atomic<std::uint64_t> visualPumpCalls_{0},visualReadFailures_{0},visualCatchupResets_{0},visualPublishRejected_{0};
    std::atomic<std::uint64_t> clockTransientFailures_{0};
    std::atomic<std::uint64_t> authoritativeDiscontinuities_{0};
    std::atomic<std::uint64_t> discontinuityCommitted_{0};
    std::atomic<std::uint64_t> visualSoftCatchups_{0};
    std::atomic<std::uint64_t> visualClockBackwardResets_{0},visualLastClockBackwardUs_{0};
    std::atomic<std::uint32_t> lastDiscontinuityReason_{0};
    std::atomic<std::uint64_t> visualClockJitterIgnored_{0},visualLastClockJitterUs_{0};
    visualisation_stream_v2::ptr visualisationStream_;
    playback_control::ptr visualisationControl_;
    // Main-thread scratch; reset logical contents before each SDK read and
    // release retained capacity with the source. Never shared with readers.
    std::optional<audio_chunk_fast_impl> visualisationChunk_;
    double visualisationTime_ = 0.0;
    double visualisationLastAbsoluteTime_ = 0.0;
    bool visualisationTimeValid_ = false;
    bool visualisationAuthoritativeRebasePending_ = false;
    VisualCaptureRecovery visualisationRecovery_;
    std::atomic<std::uint64_t> visualSourceRecoveries_{0};
    std::uint64_t lastVisualSourceRecoveryQpc_=0;
    bool qaVisualSourceStalled_=false;
    double visualisationSkippedSeconds_ = 0;
    std::uint32_t visualisationSampleRate_ = 0, visualisationChannels_ = 0;
    std::uint64_t dawVisualCursor_ = 0;
    std::uint64_t dawVisualGeneration_ = 0;
    audio_chunk_impl dawAudioChunk_;
    std::atomic<std::uint64_t> streamGeneration_{1};
    std::atomic<std::uint64_t> playbackSerial_{0};
    std::atomic<std::uint64_t> recordingIdentityHash_{0};
    std::atomic<std::uint64_t> nextFrameIndex_{0};
    std::atomic<std::uint64_t> queueRevision_{0};
    DiscontinuityLatch pendingDiscontinuity_{};
    std::atomic<bool> presentationRefreshQueued_{false};
    std::array<float, kRealtimeMaxSamples> conversionBuffer_{};
    // Single publisher: only Runtime::publishAudio, reached serially from the
    // foobar visualisation pump, may write this bus. Consumers are multicast
    // cursors and never reclaim storage from one another.
    RealtimeFrameBus<32> realtimeFrameBus_;
    std::unique_ptr<RuntimePlaylistObserver> playlistObserver_;
};

void postListenRefresh();
void postListenAnalysisRefresh();
// Uses an explicit desired pause state and latches rapid input until the
// backend callback confirms it, rather than toggling an occasionally stale
// foobar state.
void requestPlaybackPauseToggle() noexcept;
void requestScrubLeasePause(bool paused) noexcept;

} // namespace nekorox::component
