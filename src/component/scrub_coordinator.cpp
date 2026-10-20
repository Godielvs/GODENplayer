#include "qa_hooks.h"
#include "scrub_coordinator.h"

#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
#include "sdk_compat.h"
#include "runtime.h"
#include "scrub_preview_service.h"
#else
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace nekorox::component {
namespace {

std::atomic<bool> g_audioSmoothingActive{false};
std::atomic<std::uint64_t> g_audioSmoothingGeneration{0};

double rawQpcSeconds() noexcept
{
#if defined(NEKOROX_SCRUB_QA_STANDALONE)
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
        Clock::now().time_since_epoch()).count();
#else
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart > 0
        ? static_cast<double>(counter.QuadPart) /
            static_cast<double>(frequency.QuadPart)
        : 0.0;
#endif
}

#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
class SdkScrubPlaybackAdapter final : public ScrubPlaybackAdapter {
public:
    bool isPlaying() const noexcept override
    {
        return playback_control::get()->is_playing();
    }
    bool isPaused() const noexcept override
    {
        return playback_control::get()->is_paused();
    }
    bool canSeek() const noexcept override
    {
        return playback_control::get()->playback_can_seek();
    }
    double durationSeconds() const noexcept override
    {
        return playback_control::get()->playback_get_length();
    }
    double positionSeconds() const noexcept override
    {
        return playback_control::get()->playback_get_position();
    }
    void seek(double seconds) noexcept override
    {
        playback_control::get()->playback_seek(seconds);
    }
    void pause(bool paused) noexcept override
    {
        playback_control::get()->pause(paused);
    }
};
#else
class SdkScrubPlaybackAdapter final : public ScrubPlaybackAdapter {
public:
    bool isPlaying() const noexcept override { return false; }
    bool isPaused() const noexcept override { return false; }
    bool canSeek() const noexcept override { return false; }
    double durationSeconds() const noexcept override { return 0.0; }
    double positionSeconds() const noexcept override { return 0.0; }
    void seek(double) noexcept override {}
    void pause(bool) noexcept override {}
};
#endif

class QpcScrubClock final : public ScrubClock {
public:
    double nowSeconds() const noexcept override { return rawQpcSeconds(); }
};

SdkScrubPlaybackAdapter g_sdkPlayback;
QpcScrubClock g_qpcClock;
std::atomic<ScrubPlaybackAdapter*> g_playbackAdapter{&g_sdkPlayback};
std::atomic<ScrubClock*> g_clock{&g_qpcClock};

ScrubPlaybackAdapter& playbackAdapter() noexcept
{
    return *g_playbackAdapter.load(std::memory_order_acquire);
}

double scrubNow() noexcept
{
    return g_clock.load(std::memory_order_acquire)->nowSeconds();
}

bool qaHooksAllowed() noexcept
{
    return qa::hooksAllowed();
}

double clampUnit(double value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

ScrubCoordinator& ScrubCoordinator::instance()
{
    static ScrubCoordinator value;
    return value;
}

ScrubCoordinator::SessionToken ScrubCoordinator::begin(double normalizedPosition, bool auditionRequested)
{
    auto& control = playbackAdapter();
    if (!control.isPlaying() || !control.canSeek()) return false;
    const auto duration = control.durationSeconds();
    if (!(duration > 0.0)) return false;

    if (sessionActive_ || commitPending_) abandon();
    sessionActive_ = true;
    originallyPlaying_ = control.isPlaying();
    originallyPaused_ = control.isPaused();
    auditionRequested_ = auditionRequested;
    auditionPausedMain_ = false;
    lastPreviewCursor_ = 0;
    previewAcceptedAtLease_ = 0;
    lastPreviewProgressSeconds_ = scrubNow();
    durationSeconds_ = duration;
    visualNormalized_ = clampUnit(normalizedPosition);
    ++generation_;

    publishedDurationSeconds_.store(durationSeconds_, std::memory_order_relaxed);
    publishedVisualSeconds_.store(
        visualNormalized_ * durationSeconds_, std::memory_order_relaxed);
    publishedAudibleSeconds_.store(
        control.positionSeconds(), std::memory_order_relaxed);
    publishedAuditioning_.store(false, std::memory_order_relaxed);
    publishedCommitFailed_.store(false, std::memory_order_relaxed);
    publishedAuditionUnavailable_.store(false, std::memory_order_relaxed);
    publishedState_.store(
        ScrubSnapshot::State::selecting, std::memory_order_release);
    publishedGeneration_.store(generation_, std::memory_order_release);
    publishedActive_.store(true, std::memory_order_release);
#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
    if (auditionRequested_) {
        (void)ScrubPreviewService::instance().start(
            visualNormalized_ * durationSeconds_);
    }
#else
    (void)auditionRequested;
#endif
    return generation_;
}

void ScrubCoordinator::update(SessionToken session, double normalizedPosition)
{
    if (!sessionActive_ || session == 0 || session != generation_) return;
    visualNormalized_ = clampUnit(normalizedPosition);
    publishedVisualSeconds_.store(
        visualNormalized_ * durationSeconds_, std::memory_order_release);
#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
    if (auditionRequested_) {
        ScrubPreviewService::instance().update(
            visualNormalized_ * durationSeconds_);
    }
#endif
}

void ScrubCoordinator::pump()
{
    const auto now = scrubNow();
    if (commitPending_) {
        pumpCommit(now);
        return;
    }
    // Normal selection is deliberately silent and event-driven. Optional
    // Alt-preview owns a separate output stream and pauses the main transport
    // only after that stream has accepted its first grain.
#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
    if (sessionActive_ && auditionRequested_) {
        const auto preview = ScrubPreviewService::instance().snapshot();
        const auto ready = preview.state == ScrubPreviewState::ready;
        const auto unavailable =
            preview.state == ScrubPreviewState::silentSelection ||
            preview.state == ScrubPreviewState::failed;
        if (preview.submittedCursor != lastPreviewCursor_) {
            lastPreviewCursor_ = preview.submittedCursor;
            lastPreviewProgressSeconds_ = now;
        }
        const auto viable = ready && preview.acceptedBlocks >= 2U &&
            now - lastPreviewProgressSeconds_ <= 0.150;
        publishedAuditioning_.store(viable, std::memory_order_release);
        publishedAuditionUnavailable_.store(
            unavailable, std::memory_order_release);
        if (viable && !originallyPaused_ && !auditionPausedMain_) {
            requestScrubLeasePause(true);
            auditionPausedMain_ = true;
            previewAcceptedAtLease_ = preview.acceptedBlocks;
        } else if (auditionPausedMain_ &&
                (!viable || preview.acceptedBlocks == previewAcceptedAtLease_) &&
                now - lastPreviewProgressSeconds_ > 0.150) {
            requestScrubLeasePause(false);
            auditionPausedMain_ = false;
            publishedAuditionUnavailable_.store(true,
                std::memory_order_release);
        }
    }
#endif
}

void ScrubCoordinator::end(SessionToken session, double normalizedPosition)
{
    if (!sessionActive_ || session == 0 || session != generation_) return;
    const auto finalNormalized = clampUnit(normalizedPosition);
#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
    if (auditionRequested_) ScrubPreviewService::instance().cancel();
    if (auditionPausedMain_) requestScrubLeasePause(false);
#endif
    auditionRequested_ = false;
    auditionPausedMain_ = false;
    sessionActive_ = false;
    publishedVisualSeconds_.store(
        finalNormalized * durationSeconds_,
        std::memory_order_release);
    publishedAuditioning_.store(false, std::memory_order_release);
    publishedAuditionUnavailable_.store(false, std::memory_order_release);

    // Keep the visual target latched until foobar confirms the exact final
    // seek. A preview seek or a transient capture loss must never win after
    // the pointer has been released.
    const auto now = scrubNow();
    commitPending_ = true;
    commitTargetSeconds_ = finalNormalized * durationSeconds_;
    commitStartedSeconds_ = now;
    commitLastSeekSeconds_ = now;
    commitAttempts_ = 1;
    publishedState_.store(
        ScrubSnapshot::State::commitPending, std::memory_order_release);
    publishedActive_.store(true, std::memory_order_release);

    auto& control = playbackAdapter();
    if (control.canSeek() && durationSeconds_ > 0.0) {
        g_audioSmoothingGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_audioSmoothingActive.store(true, std::memory_order_release);
        control.seek(commitTargetSeconds_);
    } else {
        completeCommit(true);
    }
}

void ScrubCoordinator::commitCurrent(SessionToken session)
{
    if (!sessionActive_ || session == 0 || session != generation_) return;
    const auto normalized = durationSeconds_ > 0.0
        ? publishedVisualSeconds_.load(std::memory_order_acquire) /
            durationSeconds_
        : visualNormalized_;
    end(session, normalized);
}

void ScrubCoordinator::cancel(SessionToken session)
{
    if (session == 0 || session != generation_ ||
        (!sessionActive_ && !commitPending_)) return;
    abandon();
}

bool ScrubCoordinator::owns(SessionToken session) const noexcept
{
    return session != 0 && publishedActive_.load(std::memory_order_acquire) &&
        publishedGeneration_.load(std::memory_order_acquire) == session;
}

void ScrubCoordinator::abandon(bool restoreAuditionPause) noexcept
{
#if !defined(NEKOROX_SCRUB_QA_STANDALONE)
    if (auditionRequested_) ScrubPreviewService::instance().cancel();
    if (auditionPausedMain_ && restoreAuditionPause) requestScrubLeasePause(false);
#else
    (void)restoreAuditionPause;
#endif
    auditionRequested_ = false;
    auditionPausedMain_ = false;
    sessionActive_ = false;
    commitPending_ = false;
    commitAttempts_ = 0;
    g_audioSmoothingActive.store(false, std::memory_order_release);
    publishedAuditioning_.store(false, std::memory_order_release);
    publishedAuditionUnavailable_.store(false, std::memory_order_release);
    publishedState_.store(
        ScrubSnapshot::State::idle, std::memory_order_release);
    publishedActive_.store(false, std::memory_order_release);
}

void ScrubCoordinator::notifyBackendSeek(double seconds) noexcept
{
    if (!commitPending_ || !std::isfinite(seconds)) return;
    if (std::abs(seconds - commitTargetSeconds_) <= 0.075) {
        completeCommit();
    }
}

void ScrubCoordinator::pumpCommit(double now)
{
    if (!commitPending_) return;
    auto& control = playbackAdapter();
    const auto backend = control.positionSeconds();
    if (std::isfinite(backend)) {
        publishedAudibleSeconds_.store(backend, std::memory_order_release);
    }
    const auto elapsed = std::max(0.0, now - commitStartedSeconds_);
    const auto expected = commitTargetSeconds_ +
        (originallyPaused_ ? 0.0 : elapsed);
    if (std::isfinite(backend) &&
        (std::abs(backend - commitTargetSeconds_) <= 0.20 ||
         std::abs(backend - expected) <= 0.20)) {
        completeCommit();
        return;
    }

    if (now - commitLastSeekSeconds_ >= 0.75 && commitAttempts_ < 2U &&
        control.canSeek()) {
        commitLastSeekSeconds_ = now;
        ++commitAttempts_;
        control.seek(commitTargetSeconds_);
        return;
    }
    if (elapsed >= 1.0) {
        publishedVisualSeconds_.store(backend, std::memory_order_release);
        publishedAudibleSeconds_.store(backend, std::memory_order_release);
        completeCommit(true);
    }
}

void ScrubCoordinator::completeCommit(bool failed) noexcept
{
    commitPending_ = false;
    commitAttempts_ = 0;
    g_audioSmoothingActive.store(false, std::memory_order_release);
    publishedCommitFailed_.store(failed, std::memory_order_release);
    publishedState_.store(
        ScrubSnapshot::State::idle, std::memory_order_release);
    publishedActive_.store(false, std::memory_order_release);
}

ScrubSnapshot ScrubCoordinator::snapshot() const noexcept
{
    ScrubSnapshot result;
    result.active = publishedActive_.load(std::memory_order_acquire);
    result.auditioning = publishedAuditioning_.load(std::memory_order_acquire);
    result.commitFailed = publishedCommitFailed_.load(std::memory_order_acquire);
    result.auditionUnavailable = publishedAuditionUnavailable_.load(
        std::memory_order_acquire);
    result.state = publishedState_.load(std::memory_order_acquire);
    result.visualSeconds = publishedVisualSeconds_.load(std::memory_order_relaxed);
    result.audibleSeconds = publishedAudibleSeconds_.load(std::memory_order_relaxed);
    result.durationSeconds = publishedDurationSeconds_.load(std::memory_order_relaxed);
    result.generation = publishedGeneration_.load(std::memory_order_relaxed);
    return result;
}

double ScrubCoordinator::effectivePosition(double backendSeconds) const noexcept
{
    return publishedActive_.load(std::memory_order_acquire)
        ? publishedVisualSeconds_.load(std::memory_order_relaxed)
        : backendSeconds;
}

bool ScrubCoordinator::active() const noexcept
{
    return publishedActive_.load(std::memory_order_acquire);
}

bool ScrubCoordinator::audioSmoothingActive() noexcept
{
    return g_audioSmoothingActive.load(std::memory_order_acquire);
}

std::uint64_t ScrubCoordinator::audioSmoothingGeneration() noexcept
{
    return g_audioSmoothingGeneration.load(std::memory_order_acquire);
}

long ScrubCoordinator::installQaHooks(
    ScrubPlaybackAdapter* playback,
    ScrubClock* clock) noexcept
{
    if (!qaHooksAllowed()) return E_ACCESSDENIED;
    if (playback == nullptr || clock == nullptr) return E_INVALIDARG;
    abandon();
    g_playbackAdapter.store(playback, std::memory_order_release);
    g_clock.store(clock, std::memory_order_release);
    return S_OK;
}

void ScrubCoordinator::resetQaHooks() noexcept
{
    abandon();
    g_playbackAdapter.store(&g_sdkPlayback, std::memory_order_release);
    g_clock.store(&g_qpcClock, std::memory_order_release);
}

} // namespace nekorox::component
