#include "runtime.h"

#include "database_service.h"
#include "album_session_service.h"
#include "art_cache.h"
#include "daw_visual_bridge.h"
#include "discord_presence_service.h"
#include "file_open_service.h"
#include "keep_playing_service.h"
#include "listen_window.h"
#include "lyrics_service.h"
#include "master_dsp_engine.h"
#include "media_file_operation_service.h"
#include "packet_cost_controller.h"
#include "performance_telemetry.h"
#include "realtime_analysis_hub.h"
#include "rollout_flags.h"
#include "scrub_coordinator.h"
#include "tag_edit_service.h"
#include "telemetry_span.h"
#include "text_util.h"
#include "theme_service.h"
#include "titleformat_cache.h"
#include "ui_invalidation_bus.h"
#include "workspace_notifications.h"
#include "nekorox/pause_intent.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace nekorox::component {
namespace {

constexpr double kVisualisationBacklogSeconds = 0.5;
constexpr double kVisualisationSafetyLagSeconds = 0.012;
constexpr double kVisualisationInitialWindowSeconds = 0.020;
constexpr double kVisualisationRequestSeconds = 0.010;
constexpr double kVisualisationMaximumCatchUpSeconds = 0.4;
constexpr std::size_t kVisualisationMaximumRequestsPerPump = 4;
PauseIntent gPauseIntent;
bool gPauseReconciliationQueued = false;
std::atomic<int> gScrubPauseExpected{-1};

std::uint64_t qpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t qpcFrequency() noexcept
{
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
            ? static_cast<std::uint64_t>(value.QuadPart)
            : 1ULL;
    }();
    return frequency;
}

std::uint64_t pauseIntentFreshnessTicks() noexcept
{
    return qpcFrequency() * 5U / 4U;
}

void publishCommittedPause(bool paused);

class PauseReconciliation : public main_thread_callback {
public:
    void callback_run() override
    {
        gPauseReconciliationQueued = false;
        if (core_api::is_shutting_down()) return;
        const auto desired = gPauseIntent.takeReconciliation(
            qpcNow(), pauseIntentFreshnessTicks());
        if (!desired) return;
        TelemetrySpan span(TelemetrySurface::transport, 17U, *desired);
        const auto control = playback_control::get();
        if (!control->is_playing()) {
            gPauseIntent.cancel();
            return;
        }
        if (control->is_paused() == *desired) {
            // Read outside the callback: the backend can already have reached
            // the requested state while delivering its preceding edge. A
            // redundant pause call need not emit a second callback.
            gPauseIntent.cancel();
            publishCommittedPause(*desired);
        } else {
            control->pause(*desired);
        }
    }
};

bool reconcilePauseCallback(bool paused) noexcept
{
    TelemetrySpan span(TelemetrySurface::transport, 13U, paused);
    const auto action = gPauseIntent.observe(
        paused, qpcNow(), pauseIntentFreshnessTicks());
    if (action == PauseIntent::CallbackAction::commit) return true;
    if (action == PauseIntent::CallbackAction::reconcile &&
            !gPauseReconciliationQueued) {
        try {
            gPauseReconciliationQueued = true;
            main_thread_callback_manager::get()->add_callback(
                new service_impl_t<PauseReconciliation>);
        } catch (...) {
            gPauseReconciliationQueued = false;
            gPauseIntent.cancel();
            return true;
        }
    }
    return false;
}

template <std::size_t Size>
void copyUtf8(std::array<char, Size>& target, const char* source) noexcept
{
    target.fill('\0');
    if (source == nullptr || Size == 0) return;
    const auto length = (std::min)(std::strlen(source), Size - 1U);
    std::memcpy(target.data(), source, length);
}

std::uint64_t fnv1a64(const char* text) noexcept
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset;
    if (text == nullptr) return hash;
    for (const auto* cursor = reinterpret_cast<const unsigned char*>(text);
         *cursor != 0; ++cursor) {
        hash ^= *cursor;
        hash *= prime;
    }
    return hash;
}

void formatPlaybackField(
    service_ptr_t<playback_control> control,
    const char* expression,
    std::array<char, kPlaybackMetadataBytes>& output)
{
    const auto script = cachedTitleFormat(expression);
    pfc::string8_fast value;
    if (control->playback_format_title(
            nullptr, value, script, nullptr,
            playback_control::display_level_all) && !value.is_empty() &&
            std::strcmp(value.c_str(), "?") != 0) {
        // Dynamic metadata may legitimately be absent for local, untagged
        // files. Never let that empty refinement erase the stable handle
        // fallback (filename / Unknown artist).
        copyUtf8(output, value.c_str());
    }
}

void formatTrackField(
    const metadb_handle_ptr& track,
    const char* expression,
    std::array<char, kPlaybackMetadataBytes>& output)
{
    if (!track.is_valid()) return;
    const auto script = cachedTitleFormat(expression);
    pfc::string8_fast value;
    if (script.is_valid() &&
        track->format_title(nullptr, value, script, nullptr)) {
        copyUtf8(output, value.c_str());
    }
}

std::uint64_t trackIdentityHash(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return 0;
    return fnv1a64(track->get_path()) ^
        (static_cast<std::uint64_t>(track->get_subsong_index()) *
            0x9e3779b97f4a7c15ULL);
}

void populateTrackIdentity(
    const metadb_handle_ptr& track,
    PlaybackSnapshotV2& snapshot)
{
    if (!track.is_valid()) return;
    copyUtf8(snapshot.path, track->get_path());
    snapshot.subsong = static_cast<std::uint32_t>(
        track->get_subsong_index());
    snapshot.trackIdentityHash = trackIdentityHash(track);
    formatTrackField(track, "$if2(%title%,$filename(%path%))", snapshot.title);
    formatTrackField(track, "$if2(%artist%,Unknown artist)", snapshot.artist);
    formatTrackField(track, "%album%", snapshot.album);
}

std::filesystem::path nativeTrackPath(const metadb_handle_ptr& track)
{
    if (!track.is_valid()) return {};
    try {
        const auto native = filesystem::g_get_native_path(track->get_path());
        return utf8ToWide(native.c_str());
    } catch (...) {
        return {};
    }
}

template <typename Callback>
void runPlaybackSideEffect(const char* label, Callback&& callback) noexcept
{
    try {
        std::forward<Callback>(callback)();
    } catch (const std::exception& exception) {
        console::printf(
            "GODENplayer Core playback observer: %s failed: %s",
            label,
            exception.what());
    } catch (...) {
        console::printf(
            "GODENplayer Core playback observer: %s failed with an unknown error",
            label);
    }
}

void publishCommittedPause(bool paused)
{
    auto& runtime = Runtime::instance();
    if (runtime.playbackSnapshot().state ==
            (paused ? PlaybackState::paused : PlaybackState::playing)) return;
    runtime.markRealtimeDiscontinuity(DiscontinuityReason::pauseResume);
    runtime.notifyPlaybackPauseChanged(paused);
    runPlaybackSideEffect("history", [&] {
        TelemetrySpan historySpan(TelemetrySurface::transport, 15U, paused);
        DatabaseService::instance().onPause(paused);
    });
}

class PlaybackObserver : public play_callback_static {
public:
    unsigned get_flags() override
    {
        return flag_on_playback_all | flag_on_volume_change;
    }

    void on_playback_starting(
        play_control::t_track_command command, bool) override
    {
        gPauseIntent.cancel();
        gScrubPauseExpected.store(-1, std::memory_order_release);
        ScrubCoordinator::instance().abandon(false);
        AlbumSessionService::instance().cancelContinuation();
        KeepPlayingService::instance().onPlaybackStarting(command);
        Runtime::instance().markRealtimeDiscontinuity(
            DiscontinuityReason::sourceStart);
    }
    void on_playback_new_track(metadb_handle_ptr track) override
    {
        if (track.is_valid()) {
            try {
                const auto infoContainer = track->get_info_ref();
                if (infoContainer.is_valid()) {
                    const auto& info = infoContainer->info();
                    const auto rate = info.info_get_int("samplerate");
                    const auto channels = info.info_get_int("channels");
                    if (rate > 0 && channels > 0)
                        goden_audio::prewarmMasterDspPlayer(
                            static_cast<std::uint32_t>(rate),
                            static_cast<std::uint32_t>(channels));
                }
            } catch (...) {
            }
        }
        auto& runtime = Runtime::instance();
        runtime.admitRealtimePlayback(track);
        KeepPlayingService::instance().onNewTrack(track);
        AlbumSessionService::instance().onNewTrack(track);
        ListenWindow::instance().notifyPlaybackTrackTransition(track);
        runtime.markRealtimeDiscontinuity(DiscontinuityReason::trackChange);
        runtime.notifyPlaybackNewTrack(track);
        LyricsService::instance().onNewTrack(track);
        notifyWorkspacePlaybackSettled();
        notifyTemporaryAlbumPlaybackChanged();
        TagEditService::instance().resumeDeferred();
        MediaFileOperationService::instance().onPlaybackChanged(track);

        const auto path = nativeTrackPath(track);
        runPlaybackSideEffect("history", [&] {
            DatabaseService::instance().onNewTrack(track);
        });
        runPlaybackSideEffect("packet cost", [&] {
            PacketCostController::instance().onTrackChanged(path);
        });
    }
    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        gPauseIntent.cancel();
        // The SDK forbids transport mutation from playback callbacks. The
        // stopped/replaced transport no longer needs a preview pause restored.
        gScrubPauseExpected.store(-1, std::memory_order_release);
        ScrubCoordinator::instance().abandon(false);
        const auto albumHandled =
            AlbumSessionService::instance().onPlaybackStop(reason);
        if (!albumHandled) KeepPlayingService::instance().onPlaybackStop(reason);
        auto& runtime = Runtime::instance();
        runtime.markRealtimeDiscontinuity(DiscontinuityReason::explicitReset);
        // stop_reason_starting_another is a preparatory edge, not a stable
        // playback state. Publishing it makes every surface briefly revert to
        // Stopped/old metadata before on_playback_new_track arrives.
        if (reason != play_control::stop_reason_starting_another &&
            reason != play_control::stop_reason_shutting_down) {
            TagEditService::instance().resumeDeferred();
            MediaFileOperationService::instance().onPlaybackChanged({});
        }
        if (reason != play_control::stop_reason_starting_another) {
            ListenWindow::instance().notifyPlaybackTrackTransition({});
            runtime.notifyPlaybackChanged();
            LyricsService::instance().onStop();
        }

        runPlaybackSideEffect("history", [&] {
            DatabaseService::instance().onStop(reason);
        });
        if (reason != play_control::stop_reason_starting_another) {
            runPlaybackSideEffect("packet cost", [] {
                PacketCostController::instance().onTrackChanged({});
            });
        }
    }
    void on_playback_seek(double seconds) override
    {
        ScrubCoordinator::instance().notifyBackendSeek(seconds);
        auto& runtime = Runtime::instance();
        runtime.markRealtimeDiscontinuity(DiscontinuityReason::seek);
        // Position-only seeks must not recompile titleformat metadata or rebuild
        // track-level presentation state for every drag callback.
        runtime.updatePlaybackPosition(
            seconds,
            true,
            runtime.playbackSnapshot().trackIdentityHash);
        DiscordPresenceService::instance().publishCurrent(true);
        runPlaybackSideEffect("history", [&] {
            DatabaseService::instance().onSeek(seconds);
        });
    }
    void on_playback_pause(bool paused) override
    {
        TelemetrySpan span(TelemetrySurface::transport, 12U, paused);
        const auto scrubExpected = gScrubPauseExpected.load(
            std::memory_order_acquire);
        if (scrubExpected >= 0 && paused == (scrubExpected != 0)) {
            gScrubPauseExpected.store(-1, std::memory_order_release);
            return;
        }
        if (!reconcilePauseCallback(paused)) return;
        publishCommittedPause(paused);
    }
    void on_playback_edited(metadb_handle_ptr track) override
    {
        // Retire old decodes once, before any surface can request replacements.
        // Later visual notifications must not cancel another surface's new job.
        ArtCache::invalidateTrack(track);
        ListenWindow::instance().notifyArtworkChanged();
        notifyWorkspaceArtworkChanged();
        notifyNativePanelsPlaybackChanged(true);
        Runtime::instance().notifyPlaybackMetadataChanged(track);
        LyricsService::instance().onMetadataChanged(track);
    }
    void on_playback_dynamic_info(const file_info&) override
    {
        Runtime::instance().notifyPlaybackMetadataChanged();
    }
    void on_playback_dynamic_info_track(const file_info&) override
    {
        Runtime::instance().notifyPlaybackMetadataChanged();
        LyricsService::instance().onMetadataChanged();
    }
    void on_playback_time(double seconds) override
    {
        Runtime::instance().updatePlaybackPositionFromCallback(seconds);
    }
    void on_volume_change(float volumeDb) override
    {
        Runtime::instance().updatePlaybackVolume(volumeDb);
    }
};

FB2K_SERVICE_FACTORY(PlaybackObserver);

} // namespace

void requestPlaybackPauseToggle() noexcept
{
    TelemetrySpan span(TelemetrySurface::transport, 10U);
    ScrubCoordinator::instance().abandon();
    const auto control = playback_control::get();
    if (!control->is_playing()) {
        gPauseIntent.cancel();
        control->play_or_pause();
        return;
    }
    const auto desired = gPauseIntent.request(
        control->is_paused(), qpcNow(), pauseIntentFreshnessTicks());
    TelemetrySpan controlSpan(TelemetrySurface::transport, 11U, desired);
    control->pause(desired);
}

void requestScrubLeasePause(bool paused) noexcept
{
    gScrubPauseExpected.store(paused ? 1 : 0, std::memory_order_release);
    playback_control::get()->pause(paused);
}

class RuntimePlaylistObserver final : public playlist_callback_impl_base {
public:
    RuntimePlaylistObserver()
        : playlist_callback_impl_base(
            playlist_callback::flag_on_playback_order_changed)
    {
    }

    void on_playback_order_changed(t_size) override
    {
        Runtime::instance().refreshPresentationState();
    }
};

Runtime::Runtime()
{
    auto initial = std::make_shared<PlaybackSnapshotV2>();
    initial->sizeBytes = sizeof(PlaybackSnapshotV2);
    playbackSnapshot_.store(std::move(initial), std::memory_order_release);
}

Runtime::~Runtime() = default;

Runtime& Runtime::instance()
{
    static Runtime runtime;
    return runtime;
}

void Runtime::start()
{
    if (started_.exchange(true)) return;
    // Create the named dormant bridge before third-party visualizers enter
    // their event-driven sleep, so DAW Mode can wake every panel on demand.
    (void)DawVisualBridge::instance();
    playlistObserver_ = std::make_unique<RuntimePlaylistObserver>();
    notifyPlaybackChanged();
}

void Runtime::stop()
{
    if (!started_.exchange(false)) return;
    RealtimeAnalysisHub::instance().stop();
    presentationRefreshQueued_.store(false, std::memory_order_release);
    playlistObserver_.reset();
    realtimeConsumerCounts_.fill(0);
    stopVisualisation();
    (void)discardRealtimeFrames(RealtimeConsumer::listen);
    (void)discardRealtimeFrames(RealtimeConsumer::compactMonitor);
    (void)discardRealtimeFrames(RealtimeConsumer::albumAmbient);
    conversionBuffer_.fill(0.0f);
    playbackPositionTrackHash_.store(0, std::memory_order_release);
    playbackPositionQpc_.store(0, std::memory_order_release);
}

void Runtime::setRealtimeConsumerActive(
    RealtimeConsumer consumer,
    bool active)
{
    const auto index = [&]() -> std::optional<std::size_t> {
        switch (consumer) {
        case RealtimeConsumer::listen: return 0U;
        case RealtimeConsumer::compactMonitor: return 1U;
        case RealtimeConsumer::albumAmbient: return 2U;
        }
        return std::nullopt;
    }();
    if (!index) return;
    const auto previousConsumerCount = realtimeConsumerCounts_[*index];
    const auto previousTotal = realtimeConsumerCounts_[0] +
        realtimeConsumerCounts_[1] + realtimeConsumerCounts_[2];
    auto& count = realtimeConsumerCounts_[*index];
    if (active) {
        if (count != std::numeric_limits<std::uint32_t>::max()) ++count;
    } else if (count != 0) {
        --count;
    }

    const auto nextTotal = realtimeConsumerCounts_[0] +
        realtimeConsumerCounts_[1] + realtimeConsumerCounts_[2];
    // A new visual owner joining an already continuous PCM source starts at
    // the current cursor. It must not reset other consumers' histories.
    if (previousTotal == 0 && nextTotal != 0) {
        markRealtimeDiscontinuity(DiscontinuityReason::sourceStart);
    } else if (previousConsumerCount != 0 && count == 0) {
        (void)discardRealtimeFrames(consumer);
    }
    if (previousTotal == 0 && nextTotal != 0) {
        startVisualisation();
    } else if (previousTotal != 0 && nextTotal == 0) {
        stopVisualisation();
        conversionBuffer_.fill(0.0f);
    }
}

std::uint64_t Runtime::discardRealtimeFrames(
    RealtimeConsumer consumer) noexcept
{
    (void)consumer;
    // Multicast frames are immutable and not reclaimed by readers. Returning
    // the current cursor lets a consumer discard its own backlog without
    // erasing frames that another panel has not observed yet.
    return realtimeFrameBus_.publishedSequence();
}

void Runtime::pumpVisualisation()
{
    if (realtimeConsumerCounts_[0] + realtimeConsumerCounts_[1] +
            realtimeConsumerCounts_[2] == 0) {
        return;
    }
    auto& daw = DawVisualBridge::instance();
    if (daw.active()) {
        DawVisualChunk visual;
        if (daw.readLatest(dawVisualCursor_, visual)) {
            if (visual.discontinuity ||
                    visual.generation != dawVisualGeneration_) {
                dawVisualGeneration_ = visual.generation;
                markRealtimeDiscontinuity(DiscontinuityReason::streamChange);
            }
            const auto sampleCount = static_cast<std::size_t>(
                visual.frameCount) * visual.channels;
            dawAudioChunk_.set_data_size(sampleCount);
            auto* destination = dawAudioChunk_.get_data();
            for (std::size_t index = 0; index < sampleCount; ++index) {
                destination[index] = static_cast<audio_sample>(
                    visual.samples[index]);
            }
            dawAudioChunk_.set_sample_count(visual.frameCount);
            dawAudioChunk_.set_srate(visual.sampleRate);
            dawAudioChunk_.set_channels(visual.channels);
            playbackSeconds_.store(
                static_cast<double>(visual.positionFrames) /
                    visual.sampleRate,
                std::memory_order_relaxed);
            if (const auto semantic = playbackSnapshot_.load(
                    std::memory_order_acquire)) {
                playbackPositionTrackHash_.store(
                    semantic->trackIdentityHash,
                    std::memory_order_relaxed);
            }
            publishAudio(dawAudioChunk_, qpcNow());
        }
        // Never mix the paused normal playback stream into DAW preview.
        return;
    }
    dawVisualCursor_ = 0;
    dawVisualGeneration_ = 0;
    if (!visualisationStream_.is_valid()) {
        startVisualisation();
        if (!visualisationStream_.is_valid()) return;
    }
    if (!visualisationControl_.is_valid()) {
        visualisationControl_ = playback_control::get();
    }
    // Cache the core service, never its mutable playback state.
    if (!visualisationControl_->is_playing() ||
            visualisationControl_->is_paused()) return;
    if (qaVisualSourceStalled_) return; // Only set by the opt-in native QA hook.

    double currentTime = 0.0;
    if (!visualisationStream_->get_absolute_time(currentTime) ||
        !std::isfinite(currentTime) || currentTime < 0.0) {
        // A missing clock observation is unknown/stale, not an authoritative
        // boundary. Preserve cursor, identity and adaptation state so a
        // transient SDK failure cannot manufacture a reset on recovery.
        clockTransientFailures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (visualisationTimeValid_ &&
        std::abs(currentTime - visualisationLastAbsoluteTime_) < 1e-7) {
        return;
    }

    const auto targetTime =
        std::max(0.0, currentTime - kVisualisationSafetyLagSeconds);
    const auto backward = visualisationTimeValid_ &&
        currentTime + 1e-6 < visualisationLastAbsoluteTime_;
    if (backward && visualisationLastAbsoluteTime_ - currentTime <=
            kVisualisationRequestSeconds + 1e-7) {
        // The SDK playback-clock estimate can retreat by a few milliseconds
        // between observations without any seek or missing PCM. Keep the
        // highest observed time and the unread cursor; wait for fresh source
        // time instead of replaying samples and resetting musical adaptation.
        // Explicit seeks/track/stream changes already invalidate this clock.
        ++visualClockJitterIgnored_;
        visualLastClockJitterUs_.store(static_cast<std::uint64_t>(
            (visualisationLastAbsoluteTime_ - currentTime) * 1000000.0),
            std::memory_order_relaxed);
        return;
    }
    const auto reset = !visualisationTimeValid_ || backward;
    if (reset) {
        if (backward) {
            ++visualClockBackwardResets_;
            visualLastClockBackwardUs_.store(static_cast<std::uint64_t>(
                (visualisationLastAbsoluteTime_ - currentTime) * 1000000.0),
                std::memory_order_relaxed);
            markRealtimeDiscontinuity(DiscontinuityReason::clockBackward);
        } else if (!visualisationAuthoritativeRebasePending_) {
            markRealtimeDiscontinuity(DiscontinuityReason::sourceStart);
        }
        visualisationTime_ =
            std::max(0.0,
                targetTime - kVisualisationInitialWindowSeconds);
        visualisationTimeValid_ = true;
        visualisationAuthoritativeRebasePending_ = false;
    }
    visualisationLastAbsoluteTime_ = currentTime;

    if (visualisationRecovery_.takeRebase()) {
        const auto freshStart = std::max(0.0,
            targetTime - kVisualisationInitialWindowSeconds);
        if (freshStart > visualisationTime_) {
            // Defer the gap commit until a real chunk confirms its format.
            visualisationSkippedSeconds_ += freshStart - visualisationTime_;
            visualisationTime_ = freshStart;
        }
    }

    ++visualPumpCalls_;
    for (std::size_t requestIndex = 0;
         requestIndex < kVisualisationMaximumRequestsPerPump;
         ++requestIndex) {
        const auto pendingSeconds = targetTime - visualisationTime_;
        if (pendingSeconds <= 1e-7) break;
        if (pendingSeconds < kVisualisationRequestSeconds &&
            visualisationTimeValid_) {
            break;
        }

        const auto requestSeconds =
            std::min(pendingSeconds, kVisualisationRequestSeconds);
        if (!visualisationChunk_) visualisationChunk_.emplace();
        auto& chunk = *visualisationChunk_;
        chunk.reset();
        if (!visualisationStream_->get_chunk_absolute(
                chunk,
                visualisationTime_,
                requestSeconds) ||
            chunk.is_empty()) {
            ++visualReadFailures_;
            if (pendingSeconds > kVisualisationMaximumCatchUpSeconds) {
                ++visualCatchupResets_;
                markRealtimeDiscontinuity(DiscontinuityReason::backlogLoss);
                visualisationTime_ = std::max(
                    0.0,
                    targetTime - kVisualisationInitialWindowSeconds);
                visualisationTimeValid_ = true;
                visualisationAuthoritativeRebasePending_ = false;
                visualisationLastAbsoluteTime_ = currentTime;
            }
            break;
        }

        const auto duration = chunk.get_duration();
        if (!std::isfinite(duration) || duration <= 0.0) break;
        const auto rate = chunk.get_sample_rate();
        const auto channels = chunk.get_channels();
        if (visualisationSkippedSeconds_ > 0.0) {
            const auto skipped = visualisationSkippedSeconds_;
            const auto cursor = nextFrameIndex_.load(std::memory_order_relaxed);
            const bool bounded = std::isfinite(skipped) && skipped <= .350 &&
                rate >= 8000 && rate <= 192000 &&
                channels != 0 && channels <= kRealtimeMaxChannels;
            const auto count = bounded
                ? std::uint64_t(std::llround(skipped * rate)) : 0;
            if (bounded &&
                    rate == visualisationSampleRate_ &&
                    channels == visualisationChannels_ && cursor != 0 &&
                    count <= std::numeric_limits<std::uint64_t>::max() - cursor) {
                // This is missing source time, not a new track or a new stream.
                // Keep the forward index gap so Analyzer can retain adaptation
                // and Dynamics can record unknown time. Never fabricate PCM.
                nextFrameIndex_.fetch_add(count, std::memory_order_relaxed);
                pendingDiscontinuity_.mark(DiscontinuityReason::backlogLoss,
                    realtimeFrameBus_.publishedSequence() + 1U, qpcNow());
                ++visualSoftCatchups_;
                ++authoritativeDiscontinuities_;
            } else {
                ++visualCatchupResets_;
                markRealtimeDiscontinuity(DiscontinuityReason::backlogLoss);
                visualisationTimeValid_ = true;
                visualisationAuthoritativeRebasePending_ = false;
                visualisationLastAbsoluteTime_ = currentTime;
            }
            visualisationSkippedSeconds_ = 0;
        }
        const auto chunkEndTime = visualisationTime_ + duration;
        const auto secondsBehind =
            std::max(0.0, currentTime - chunkEndTime);
        const auto ticksBehind = static_cast<std::uint64_t>(
            secondsBehind * static_cast<double>(qpcFrequency()));
        const auto now = qpcNow();
        publishAudio(
            chunk,
            ticksBehind < now ? now - ticksBehind : 0);
        visualisationTime_ = chunkEndTime;
        visualisationSampleRate_ = rate;
        visualisationChannels_ = channels;
    }
    visualisationRecovery_.observe(currentTime,
        std::max(0.0, currentTime - visualisationTime_));
}

bool Runtime::consumeLatestFrame(
    RealtimeConsumer consumer,
    RealtimeFrameV1& frame,
    std::uint64_t& sequence,
    std::uint64_t* skipped) noexcept
{
    (void)consumer;
    return realtimeFrameBus_.consumeLatest(frame, sequence, skipped);
}

bool Runtime::consumeNextFrame(
    RealtimeConsumer consumer,
    RealtimeFrameV1& frame,
    std::uint64_t& sequence,
    std::uint64_t* skipped) noexcept
{
    (void)consumer;
    return realtimeFrameBus_.consumeNext(frame, sequence, skipped);
}

std::uint64_t Runtime::latestRealtimeSequence() const noexcept
{
    return realtimeFrameBus_.publishedSequence();
}

bool Runtime::dawPreviewActive() const noexcept
{
    return DawVisualBridge::instance().active();
}

void Runtime::notifyDawPreviewChanged()
{
    dawVisualCursor_ = 0;
    dawVisualGeneration_ = 0;
    markRealtimeDiscontinuity(DiscontinuityReason::streamChange);
    postListenRefresh();
    notifyNativePanelsPlaybackChanged(true);
}

PlaybackSnapshotV2 Runtime::playbackSnapshot() const
{
    const auto snapshot = playbackSnapshot_.load(std::memory_order_acquire);
    if (snapshot) {
        auto result = *snapshot;
        result.volumeDb = playbackVolumeDb_.load(std::memory_order_acquire);
        if (playbackMuted_.load(std::memory_order_acquire))
            result.flags |= kPlaybackFlagMuted;
        else
            result.flags &= ~kPlaybackFlagMuted;
        const auto positionTrackHash = playbackPositionTrackHash_.load(
            std::memory_order_acquire);
        const auto position = playbackSeconds_.load(std::memory_order_acquire);
        if (positionTrackHash != 0 &&
            positionTrackHash == result.trackIdentityHash &&
            std::isfinite(position) && position >= 0.0) {
            result.positionSeconds = position;
        }
        return result;
    }
    PlaybackSnapshotV2 result;
    result.sizeBytes = sizeof(result);
    return result;
}

PlaybackPositionSnapshotV1 Runtime::playbackPositionSnapshot() const noexcept
{
    PlaybackPositionSnapshotV1 result;
    result.positionRevision = playbackPositionRevision_.load(
        std::memory_order_acquire);
    result.seekGeneration = playbackSeekGeneration_.load(
        std::memory_order_acquire);
    const auto semantic = playbackSnapshot_.load(std::memory_order_acquire);
    const auto positionTrackHash = playbackPositionTrackHash_.load(
        std::memory_order_acquire);
    if (!semantic || positionTrackHash == 0 ||
        positionTrackHash != semantic->trackIdentityHash) {
        result.positionSeconds = semantic
            ? std::max(0.0, semantic->positionSeconds) : 0.0;
        result.qpcTicks = 0;
        return result;
    }
    result.positionSeconds = std::max(
        0.0, playbackSeconds_.load(std::memory_order_acquire));
    result.qpcTicks = playbackPositionQpc_.load(std::memory_order_acquire);
    return result;
}

double Runtime::presentationPlaybackPosition() const noexcept
{
    const auto position = playbackPositionSnapshot();
    auto presented = position.positionSeconds;
    const auto semantic = playbackSnapshot_.load(std::memory_order_acquire);
    if (!semantic || semantic->state != PlaybackState::playing ||
            position.qpcTicks == 0) {
        return presented;
    }

    const auto now = qpcNow();
    if (now > position.qpcTicks) {
        // Callback cadence is normally about one second. A generous clamp
        // avoids a stale clock running indefinitely if the backend stalls.
        const auto elapsed = std::min(
            static_cast<double>(now - position.qpcTicks) /
                static_cast<double>(qpcFrequency()),
            1.5);
        presented += elapsed;
    }
    if (std::isfinite(semantic->durationSeconds) &&
            semantic->durationSeconds > 0.0) {
        presented = std::min(presented, semantic->durationSeconds);
    }
    return std::max(0.0, presented);
}

std::uint64_t Runtime::streamGeneration() const noexcept
{
    return streamGeneration_.load(std::memory_order_acquire);
}

metadb_handle_ptr Runtime::currentTrackHandle() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return currentTrack_;
}

void Runtime::populatePresentationState(PlaybackSnapshotV2& result) const
{
    const auto control = playback_control::get();
    result.volumeDb = control->get_volume();
    if (control->is_muted()) result.flags |= kPlaybackFlagMuted;
    else result.flags &= ~kPlaybackFlagMuted;

    const auto playlists = playlist_manager::get();
    const auto order = playlists->playback_order_get_active();
    result.playbackOrder = order == pfc_infinite
        ? 0U
        : static_cast<std::uint32_t>(
            std::min<t_size>(
                order,
                (std::numeric_limits<std::uint32_t>::max)()));
    result.queueRevision =
        queueRevision_.load(std::memory_order_acquire);
    result.artworkIdentityHash =
        ThemeService::instance().contractSnapshot().artworkIdentityHash;

    const auto userState =
        DatabaseService::instance().userStateSnapshot();
    result.flags &= ~(
        kPlaybackFlagFavorite |
        kPlaybackFlagFavoriteAvailable);
    if (userState && userState->available) {
        result.flags |= kPlaybackFlagFavoriteAvailable;
        if (userState->favorite) {
            result.flags |= kPlaybackFlagFavorite;
        }
    }
}

void Runtime::publishPlaybackSnapshot(PlaybackSnapshotV2 snapshot)
{
    playbackVolumeDb_.store(snapshot.volumeDb, std::memory_order_release);
    playbackMuted_.store(
        (snapshot.flags & kPlaybackFlagMuted) != 0,
        std::memory_order_release);
    playbackSnapshot_.store(
        std::make_shared<const PlaybackSnapshotV2>(std::move(snapshot)),
        std::memory_order_release);
}

void Runtime::updatePlaybackVolume(float volumeDb) noexcept
{
    playbackVolumeDb_.store(volumeDb, std::memory_order_release);
    try {
        playbackMuted_.store(
            playback_control::get()->is_muted(), std::memory_order_release);
    } catch (...) {
    }
    auto& bus = UiInvalidationBus::instance();
    bus.publish(UiInvalidationSurface::nativePanels, UiDirty::volume);
    bus.publish(UiInvalidationSurface::workspace, UiDirty::volume);
    bus.publish(UiInvalidationSurface::waveSeek, UiDirty::volume);
    bus.publish(UiInvalidationSurface::listen, UiDirty::volume);
}

std::string Runtime::nowPlayingText() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return nowPlayingText_;
}

void Runtime::markRealtimeDiscontinuity(DiscontinuityReason reason) noexcept
{
    lastDiscontinuityReason_.store(reasonMask(reason), std::memory_order_relaxed);
    authoritativeDiscontinuities_.fetch_add(1, std::memory_order_relaxed);
    visualisationTimeValid_ = false;
    visualisationAuthoritativeRebasePending_ = true;
    visualisationRecovery_.reset();
    visualisationSkippedSeconds_ = 0;
    visualisationSampleRate_ = visualisationChannels_ = 0;
    nextFrameIndex_.store(0, std::memory_order_relaxed);
    streamGeneration_.fetch_add(1, std::memory_order_acq_rel);
    pendingDiscontinuity_.mark(reason,
        realtimeFrameBus_.publishedSequence() + 1U, qpcNow());
}

void Runtime::admitRealtimePlayback(const metadb_handle_ptr& track) noexcept
{
    if (!track.is_valid()) return;
    try {
        // Admission only stamps path/subsong identity onto the PCM session.
        // The following presentation update owns titleformat and metadata.
        recordingIdentityHash_.store(
            trackIdentityHash(track), std::memory_order_release);
        playbackSerial_.fetch_add(1, std::memory_order_acq_rel);
    } catch (...) {
        // Never let identity extraction stamp an ambiguous session onto PCM.
    }
}

void Runtime::notifyQueueChanged()
{
    queueRevision_.fetch_add(1, std::memory_order_acq_rel);
    refreshPresentationState();
}

void Runtime::refreshPresentationState()
{
    if (!started_.load(std::memory_order_acquire)) {
        return;
    }

    if (!core_api::is_main_thread()) {
        if (core_api::is_shutting_down()) return;
        bool expected = false;
        if (presentationRefreshQueued_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            fb2k::inMainThread([] {
                auto& runtime = Runtime::instance();
                runtime.presentationRefreshQueued_.store(
                    false, std::memory_order_release);
                runtime.refreshPresentationState();
            });
        }
        return;
    }

    presentationRefreshQueued_.store(false, std::memory_order_release);
    if (core_api::is_shutting_down()) return;

    auto next = playbackSnapshot();
    ++next.generation;
    populatePresentationState(next);
    publishPlaybackSnapshot(std::move(next));
    postListenRefresh();
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
}

void Runtime::notifyPlaybackChanged(bool preserveDiscordActivity)
{
    if (!started_.load(std::memory_order_acquire) || core_api::is_shutting_down()) {
        return;
    }

    auto control = playback_control::get();
    PlaybackSnapshotV2 next;
    next.sizeBytes = sizeof(next);
    next.generation = playbackSnapshot().generation + 1;
    if (control->is_playing()) {
        next.state = control->is_paused()
            ? PlaybackState::paused
            : PlaybackState::playing;
        next.positionSeconds = control->playback_get_position();
        next.durationSeconds = control->playback_get_length();
        metadb_handle_ptr current;
        if (control->get_now_playing(current) && current.is_valid()) {
            populateTrackIdentity(current, next);
            const auto admittedIdentity = recordingIdentityHash_.load(
                std::memory_order_acquire);
            if (playbackSerial_.load(std::memory_order_acquire) == 0 ||
                    admittedIdentity != next.trackIdentityHash) {
                recordingIdentityHash_.store(
                    next.trackIdentityHash, std::memory_order_release);
                playbackSerial_.fetch_add(1, std::memory_order_acq_rel);
            }
            // Dynamic stream metadata belongs to playback_format_title. It can
            // refine the stable handle metadata once the backend has settled.
            formatPlaybackField(control, "%title%", next.title);
            formatPlaybackField(control, "%artist%", next.artist);
            formatPlaybackField(control, "%album%", next.album);
            std::lock_guard<std::mutex> lock(stateMutex_);
            currentTrack_ = current;
        }
    } else {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentTrack_.release();
    }
    updatePlaybackPosition(
        next.positionSeconds, false, next.trackIdentityHash);

    auto playlists = playlist_manager::get();
    t_size playlist = std::numeric_limits<t_size>::max();
    t_size item = std::numeric_limits<t_size>::max();
    if (playlists->get_playing_item_location(&playlist, &item)) {
        next.playlistIndex = static_cast<std::uint32_t>(playlist);
        next.playlistItemIndex = static_cast<std::uint32_t>(item);
    }
    populatePresentationState(next);
    publishPlaybackSnapshot(std::move(next));
    if (!preserveDiscordActivity) {
        DiscordPresenceService::instance().publishCurrent();
    }
    refreshNowPlayingText();
    postListenRefresh();
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
}

void Runtime::notifyPlaybackNewTrack(const metadb_handle_ptr& track)
{
    if (!started_.load(std::memory_order_acquire) ||
        core_api::is_shutting_down()) {
        return;
    }
    if (!track.is_valid()) {
        notifyPlaybackChanged();
        return;
    }

    // on_playback_new_track's handle is authoritative. Querying
    // get_now_playing() during this callback can still return the previous
    // handle on some decoders, which made title/art/pause state visibly jump
    // backward for one or more frames.
    const auto control = playback_control::get();
    PlaybackSnapshotV2 next;
    next.sizeBytes = sizeof(next);
    next.generation = playbackSnapshot().generation + 1U;
    next.state = control->is_paused()
        ? PlaybackState::paused : PlaybackState::playing;
    metadb_handle_ptr backend;
    const bool backendMatches = control->get_now_playing(backend) &&
        backend == track;
    // A decoder can still expose the preceding track's clock during this
    // callback. Start the authoritative new track at zero until the backend
    // handle agrees; on_playback_time will then advance it normally.
    next.positionSeconds = backendMatches
        ? std::max(0.0, control->playback_get_position()) : 0.0;
    next.durationSeconds = std::max(0.0, track->get_length());
    if (backendMatches) {
        next.durationSeconds = std::max(
            next.durationSeconds,
            std::max(0.0, control->playback_get_length()));
    }
    populateTrackIdentity(track, next);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentTrack_ = track;
    }
    updatePlaybackPosition(
        next.positionSeconds, false, next.trackIdentityHash);

    const auto playlists = playlist_manager::get();
    t_size playlist = std::numeric_limits<t_size>::max();
    t_size item = std::numeric_limits<t_size>::max();
    if (playlists->get_playing_item_location(&playlist, &item)) {
        next.playlistIndex = static_cast<std::uint32_t>(playlist);
        next.playlistItemIndex = static_cast<std::uint32_t>(item);
    }
    populatePresentationState(next);
    publishPlaybackSnapshot(std::move(next));
    DiscordPresenceService::instance().publishCurrent();
    refreshNowPlayingText(track);
    postListenRefresh();
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
}

void Runtime::notifyPlaybackPauseChanged(bool paused)
{
    TelemetrySpan span(TelemetrySurface::transport, 14U, paused);
    if (!started_.load(std::memory_order_acquire) ||
        core_api::is_shutting_down()) {
        return;
    }
    auto next = playbackSnapshot();
    if (next.trackIdentityHash == 0) {
        notifyPlaybackChanged();
        return;
    }

    // The callback argument is the committed state. Reading is_paused()
    // again inside the callback can briefly expose its preceding value.
    next.state = paused ? PlaybackState::paused : PlaybackState::playing;
    const auto control = playback_control::get();
    next.positionSeconds = std::max(0.0, control->playback_get_position());
    const auto duration = control->playback_get_length();
    if (std::isfinite(duration) && duration > 0.0) {
        next.durationSeconds = duration;
    }
    ++next.generation;
    populatePresentationState(next);
    updatePlaybackPosition(
        next.positionSeconds, false, next.trackIdentityHash);
    publishPlaybackSnapshot(std::move(next));
    DiscordPresenceService::instance().publishCurrent();
    postListenRefresh();
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
}

void Runtime::notifyPlaybackMetadataChanged(const metadb_handle_ptr& track)
{
    if (!started_.load(std::memory_order_acquire) ||
        core_api::is_shutting_down()) {
        return;
    }

    auto next = playbackSnapshot();
    if (next.state == PlaybackState::stopped ||
        next.trackIdentityHash == 0) {
        notifyPlaybackChanged();
        return;
    }

    auto authoritative = currentTrackHandle();
    // Metadata callbacks may arrive late after a track transition. Only let a
    // hinted handle enrich the currently committed track; never let it replace
    // that track with an older callback payload.
    if (track.is_valid() &&
        (!authoritative.is_valid() || track == authoritative)) {
        authoritative = track;
    }
    if (authoritative.is_valid()) {
        populateTrackIdentity(authoritative, next);
        const auto handleDuration = authoritative->get_length();
        if (std::isfinite(handleDuration) && handleDuration > 0.0) {
            next.durationSeconds = handleDuration;
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentTrack_ = authoritative;
    }

    // Dynamic title information is safe to consume only after the backend's
    // handle agrees with the callback-authoritative handle. Otherwise an old
    // decoder edge can repaint the preceding title over the new track.
    const auto control = playback_control::get();
    metadb_handle_ptr backend;
    const bool backendMatches = authoritative.is_valid() &&
        control->get_now_playing(backend) && backend == authoritative;
    if (backendMatches) {
        formatPlaybackField(control, "%title%", next.title);
        formatPlaybackField(control, "%artist%", next.artist);
        formatPlaybackField(control, "%album%", next.album);
        const auto position = control->playback_get_position();
        const auto duration = control->playback_get_length();
        if (std::isfinite(position) && position >= 0.0) {
            next.positionSeconds = position;
        }
        if (std::isfinite(duration) && duration > 0.0) {
            next.durationSeconds = duration;
        }
    }

    ++next.generation;
    updatePlaybackPosition(
        next.positionSeconds, false, next.trackIdentityHash);
    populatePresentationState(next);
    publishPlaybackSnapshot(std::move(next));
    DiscordPresenceService::instance().publishCurrent();
    if (backendMatches) refreshNowPlayingText();
    else refreshNowPlayingText(authoritative);
    postListenRefresh();
    notifyWorkspacePlaybackChanged();
    notifyNativePanelsPlaybackChanged();
}

void Runtime::updatePlaybackPosition(
    double seconds,
    bool seek,
    std::uint64_t trackIdentityHash)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return;
    // Publish identity before the clock. During a track transition, readers
    // holding the old semantic snapshot will reject this position rather than
    // briefly painting the preceding decoder's timestamp onto the new track.
    playbackPositionTrackHash_.store(
        trackIdentityHash, std::memory_order_release);
    playbackSeconds_.store(seconds, std::memory_order_release);
    playbackPositionQpc_.store(qpcNow(), std::memory_order_release);
    playbackPositionRevision_.fetch_add(1, std::memory_order_acq_rel);
    PerformanceTelemetry::instance().event(
        seek ? nekorox::PerformanceEventKindV2::seek
             : nekorox::PerformanceEventKindV2::callback,
        TelemetrySurface::transport,
        static_cast<std::uint64_t>(seconds * 1000000.0));
    if (!rolloutFlagEnabled(RolloutFlag::playbackPositionV1)) {
        if (seek) {
            playbackSeekGeneration_.fetch_add(1, std::memory_order_acq_rel);
        }
        const auto semantic = playbackSnapshot_.load(
            std::memory_order_acquire);
        if (semantic &&
            semantic->trackIdentityHash == trackIdentityHash) {
            auto next = *semantic;
            next.positionSeconds = seconds;
            ++next.generation;
            publishPlaybackSnapshot(std::move(next));
        }
        // Pre-V1 fallback deliberately restores semantic fan-out so an A/B
        // run can isolate the atomic clock optimization without recompiling.
        postListenRefresh();
        notifyWorkspacePlaybackChanged(true);
        notifyNativePanelsPlaybackChanged();
        return;
    }
    if (!seek) return;

    playbackSeekGeneration_.fetch_add(1, std::memory_order_acq_rel);
    // Clocks sample the atomic clock during normal playback. A committed seek
    // needs one directed wake so paused views repaint without reviving every
    // continuous timer or rebuilding semantic presentation state.
    auto& invalidation = UiInvalidationBus::instance();
    invalidation.publish(
        UiInvalidationSurface::nativePanels, UiDirty::position);
    invalidation.publish(
        UiInvalidationSurface::workspace, UiDirty::position);
    invalidation.publish(
        UiInvalidationSurface::waveSeek, UiDirty::position);
    invalidation.publish(
        UiInvalidationSurface::listen, UiDirty::position);
}

void Runtime::updatePlaybackPositionFromCallback(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0 ||
        !started_.load(std::memory_order_acquire) ||
        core_api::is_shutting_down()) {
        return;
    }

    const auto semantic = playbackSnapshot_.load(std::memory_order_acquire);
    if (!semantic || semantic->trackIdentityHash == 0 ||
        semantic->state == PlaybackState::stopped) {
        return;
    }

    // Some decoders deliver one final time callback after the next-track
    // callback. The authoritative handle is intentionally stored first in
    // notifyPlaybackNewTrack; reject any clock whose backend handle has not
    // caught up with that identity yet.
    const auto authoritative = currentTrackHandle();
    metadb_handle_ptr backend;
    if (!authoritative.is_valid() ||
        !playback_control::get()->get_now_playing(backend) ||
        backend != authoritative) {
        return;
    }

    updatePlaybackPosition(seconds, false, semantic->trackIdentityHash);
}

void Runtime::publishAudio(
    const audio_chunk& chunk,
    std::uint64_t chunkEndQpc) noexcept
{
    const auto anyConsumer = realtimeConsumerCounts_[0] != 0 ||
        realtimeConsumerCounts_[1] != 0 || realtimeConsumerCounts_[2] != 0;
    if (!anyConsumer || chunk.is_empty()) {
        return;
    }
    const auto channels = chunk.get_channels();
    const auto totalFrames = chunk.get_sample_count();
    const auto sampleRate = chunk.get_sample_rate();
    if (channels == 0 || channels > kRealtimeMaxChannels ||
        totalFrames == 0 || sampleRate == 0) {
        return;
    }
    const auto* source = chunk.get_data();
    if (source == nullptr) return;

    // Playback can already be running while this component is starting, or a
    // visualisation owner can attach between the backend transition and its
    // callback.  Do not stamp otherwise valid PCM with the sentinel identity:
    // recover the committed semantic identity on this main-thread pump.
    auto playbackSerial = playbackSerial_.load(std::memory_order_acquire);
    auto recordingIdentity = recordingIdentityHash_.load(
        std::memory_order_acquire);
    if (playbackSerial == 0 || recordingIdentity == 0) {
        try {
            auto track = currentTrackHandle();
            if (!track.is_valid()) {
                playback_control::get()->get_now_playing(track);
            }
            PlaybackSnapshotV2 identity;
            populateTrackIdentity(track, identity);
            if (identity.trackIdentityHash != 0) {
                recordingIdentity = identity.trackIdentityHash;
                recordingIdentityHash_.store(
                    recordingIdentity, std::memory_order_release);
                playbackSerial = playbackSerial_.fetch_add(
                    1, std::memory_order_acq_rel) + 1U;
            }
        } catch (...) {
            // The next visualisation pump retries after the backend has
            // committed its handle; sentinel PCM is never published.
        }
    }
    if (playbackSerial == 0 || recordingIdentity == 0) return;

    for (std::size_t frameOffset = 0; frameOffset < totalFrames;) {
        const auto frameCount = static_cast<std::uint32_t>(std::min<std::size_t>(
            kRealtimeMaxFrames,
            totalFrames - frameOffset));
        const auto sampleCount =
            static_cast<std::size_t>(channels) * frameCount;
        const auto sourceOffset =
            frameOffset * static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            conversionBuffer_[i] =
                static_cast<float>(source[sourceOffset + i]);
        }

        const auto firstFrameIndex =
            nextFrameIndex_.fetch_add(frameCount, std::memory_order_relaxed);
        const auto remainingFrames =
            totalFrames - frameOffset - frameCount;
        const auto remainingTicks = static_cast<std::uint64_t>(
            (static_cast<double>(remainingFrames) /
                static_cast<double>(sampleRate)) *
            static_cast<double>(qpcFrequency()));
        const auto frameQpc = remainingTicks < chunkEndQpc
            ? chunkEndQpc - remainingTicks
            : 0;
        auto flags = kRealtimeFrameFlagPcm;
        const auto pendingDiscontinuity = pendingDiscontinuity_.pending();
        if (pendingDiscontinuity) {
            flags |= kRealtimeFrameFlagDiscontinuity;
        }
        const auto published = realtimeFrameBus_.publish(
            conversionBuffer_.data(),
            sampleCount,
            sampleRate,
            static_cast<std::uint16_t>(channels),
            frameCount,
            frameQpc,
            playbackSeconds_.load(std::memory_order_relaxed),
            flags,
            streamGeneration_.load(std::memory_order_acquire),
            firstFrameIndex,
            playbackSerial,
            recordingIdentity,
            pendingDiscontinuity.reasonMask);
        if (!published) {
            ++visualPublishRejected_;
            pendingDiscontinuity_.rejected();
        } else {
            pendingDiscontinuity_.commit(pendingDiscontinuity);
            if (pendingDiscontinuity)
                discontinuityCommitted_.fetch_add(1, std::memory_order_relaxed);
        }
        if (published && RealtimeAnalysisHub::instance().hasActiveFeatures()) {
            RealtimeAnalysisHub::instance().notifyFrame();
        }
        frameOffset += frameCount;
    }
}

void Runtime::startVisualisation()
{
    if (visualisationStream_.is_valid()) return;
    try {
        visualisation_manager::get()->create_stream(
            visualisationStream_,
            visualisation_manager::KStreamFlagNewFFT);
        visualisationStream_->request_backlog(kVisualisationBacklogSeconds);
        visualisationStream_->set_channel_mode(
            visualisation_stream_v2::channel_mode_default);
        visualisationTimeValid_ = false;
    } catch (const std::exception& exception) {
        visualisationStream_.release();
        console::printf(
            "GODENplayer Core visualisation source failed: %s",
            exception.what());
    } catch (...) {
        visualisationStream_.release();
        console::print(
            "GODENplayer Core visualisation source failed with an unknown error");
    }
}

bool Runtime::recoverVisualisationSource() noexcept
{
    const auto now=qpcNow();
    if(lastVisualSourceRecoveryQpc_ && now>=lastVisualSourceRecoveryQpc_ &&
        now-lastVisualSourceRecoveryQpc_<qpcFrequency()*2U)return false;
    lastVisualSourceRecoveryQpc_=now;
    // Presentation thread only. Owners/demand and GPU scenes remain intact;
    // the next pump acquires a new native SDK source and admits real PCM.
    stopVisualisation();
    qaVisualSourceStalled_=false;
    markRealtimeDiscontinuity(DiscontinuityReason::sourceStart);
    visualSourceRecoveries_.fetch_add(1,std::memory_order_relaxed);
    return true;
}

void Runtime::stopVisualisation()
{
    TelemetrySpan span(TelemetrySurface::transport, 16U);
    visualisationStream_.release();
    visualisationChunk_.reset();
    visualisationControl_.release();
    visualisationTime_ = 0.0;
    visualisationLastAbsoluteTime_ = 0.0;
    visualisationTimeValid_ = false;
}

void Runtime::refreshNowPlayingText(const metadb_handle_ptr& trackHint)
{
    pfc::string8_fast text;
    auto control = playback_control::get();
    const auto script = cachedTitleFormat(
        "[%tracknumber%. ]%title%[ '('%length%')']  $char(183)  %artist%  $char(183)  %album%");
    const auto formatted = trackHint.is_valid()
        ? trackHint->format_title(nullptr, text, script, nullptr)
        : control->playback_format_title(
            nullptr,
            text,
            script,
            nullptr,
            playback_control::display_level_all);
    if (!formatted) {
        text = control->is_playing() ? "Opening..." : "Stopped";
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    nowPlayingText_ = text.c_str();
}

void postListenRefresh()
{
    ListenWindow::instance().requestRefresh();
}

void postListenAnalysisRefresh()
{
    UiInvalidationBus::instance().publish(
        UiInvalidationSurface::listen,
        UiDirty::analysis);
}

} // namespace nekorox::component
