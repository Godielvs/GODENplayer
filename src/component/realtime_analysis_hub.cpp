#include "realtime_analysis_hub.h"
#include "collection/collection_bridge.h"

#include "performance_telemetry.h"
#include "runtime.h"

#include <algorithm>
#include <exception>
#include <span>

namespace nekorox::component {

RealtimeAnalysisHub::RealtimeAnalysisHub()
{
    // Construct before the worker: Hub shutdown joins before Bridge destruction.
    (void)collection::Bridge::instance();
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (auto& slot : snapshotPool_) {
        slot = std::make_shared<RealtimeAnalysisSnapshot>();
        PerformanceTelemetry::instance().allocation(
            TelemetrySurface::listen);
    }
    for (auto& slot : historyPool_) {
        slot = std::make_shared<RealtimeHistorySnapshot>();
        PerformanceTelemetry::instance().allocation(
            TelemetrySurface::listen);
    }
    for (auto& slot : ambientPool_) {
        slot = std::make_shared<AmbientFeaturesV1>();
        PerformanceTelemetry::instance().allocation(
            TelemetrySurface::artwork);
    }
    for (auto& slot : dynamicsPool_) {
        slot = std::make_shared<DynamicsSnapshot>();
        PerformanceTelemetry::instance().allocation(
            TelemetrySurface::listen);
    }
}

RealtimeAnalysisHub::~RealtimeAnalysisHub()
{
    stop();
    if (wakeEvent_ != nullptr) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
}

RealtimeAnalysisHub& RealtimeAnalysisHub::instance()
{
    static RealtimeAnalysisHub hub;
    return hub;
}

void RealtimeAnalysisHub::activate(
    double durationSeconds,
    std::uint64_t startSequence)
{
    (void)setOwnerActive(AnalysisOwner::listen, 1, true,
        durationSeconds, startSequence);
}

void RealtimeAnalysisHub::reset(
    double durationSeconds,
    std::uint64_t startSequence)
{
    requestedDuration_.store(
        std::max(0.0, durationSeconds), std::memory_order_release);
    requestedSequence_.store(startSequence, std::memory_order_release);
    resetGeneration_.fetch_add(1, std::memory_order_acq_rel);
    snapshot_.store(nullptr, std::memory_order_release);
    notifyFrame();
}

void RealtimeAnalysisHub::deactivate() noexcept
{
    (void)setOwnerActive(AnalysisOwner::listen, 1, false);
}

bool RealtimeAnalysisHub::setOwnerActive(AnalysisOwner owner,
    std::uint64_t lifetimeToken, bool active, double durationSeconds,
    std::uint64_t startSequence) noexcept
{
    const auto features = owner == AnalysisOwner::albumAmbient
        ? kAnalysisFeatureAlbumBands
        : owner == AnalysisOwner::header
            ? kAnalysisFeatureCompactScope
            : kAnalysisFeatureListenVisual;
    return active
        ? setDemand(owner, lifetimeToken, features,
            durationSeconds, startSequence)
        : (removeDemand(owner, lifetimeToken), true);
}

bool RealtimeAnalysisHub::setDemand(AnalysisOwner owner,
    std::uint64_t lifetimeToken, std::uint32_t featureMask,
    double durationSeconds, std::uint64_t startSequence) noexcept
{
    if (lifetimeToken == 0) return false;
    featureMask &= kAnalysisKnownFeatures;
    if (featureMask == 0) {
        removeDemand(owner, lifetimeToken);
        return true;
    }
    std::uint32_t nextMask = 0;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ownersMutex_);
        OwnerLease* match = nullptr;
        OwnerLease* empty = nullptr;
        for (auto& lease : owners_) {
            if (lease.active && lease.owner == owner &&
                    lease.lifetimeToken == lifetimeToken) {
                match = &lease;
                break;
            }
            if (!lease.active && empty == nullptr) empty = &lease;
        }
        if (match == nullptr) {
            if (empty == nullptr) return false;
            empty->owner = owner;
            empty->lifetimeToken = lifetimeToken;
            empty->features = featureMask;
            empty->active = true;
            changed = true;
        } else if (match->features != featureMask) {
            match->features = featureMask;
            changed = true;
        }
        for (const auto& lease : owners_) {
            if (!lease.active) continue;
            nextMask |= lease.features;
        }
    }
    if (!changed) return true;
    const auto previousMask = activeFeatureMask_.exchange(
        nextMask, std::memory_order_acq_rel);
    active_.store(nextMask != 0, std::memory_order_release);
    if ((nextMask & kAnalysisFeatureSpectralHistory) != 0 &&
            (previousMask & kAnalysisFeatureSpectralHistory) == 0) {
        requestedDuration_.store(
            std::max(0.0, durationSeconds), std::memory_order_release);
        if (previousMask == 0) {
            requestedSequence_.store(startSequence, std::memory_order_release);
            resetGeneration_.fetch_add(1, std::memory_order_acq_rel);
        } else {
            // Another consumer already owns the multicast cursor. Starting
            // spectral history must not reset that cursor or its compact scope.
            listenResetGeneration_.fetch_add(1, std::memory_order_acq_rel);
        }
        snapshot_.store(nullptr, std::memory_order_release);
    }
    if ((nextMask & kAnalysisFeatureAlbumBands) != 0 &&
            (previousMask & kAnalysisFeatureAlbumBands) == 0) {
        ambientResetGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }
    // A Dynamics lease is a view concern. Re-entering focus/HOLD resumes at a
    // segment boundary supplied by the next PCM frame; it must never erase the
    // single persistent history owner.
    if (previousMask == 0 && nextMask != 0 &&
            (nextMask & kAnalysisFeatureSpectralHistory) == 0) {
        requestedSequence_.store(startSequence, std::memory_order_release);
        resetGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }
    if ((nextMask & kAnalysisFeatureAlbumBands) == 0) {
        ambientSnapshot_.store(nullptr, std::memory_order_release);
    }
    if ((nextMask & kAnalysisFeatureListenVisual) == 0) {
        snapshot_.store(nullptr, std::memory_order_release);
    }
    if (nextMask != 0 && !ensureWorker()) {
        activeFeatureMask_.store(previousMask, std::memory_order_release);
        active_.store(previousMask != 0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(ownersMutex_);
        for (auto& lease : owners_) {
            if (lease.owner == owner && lease.lifetimeToken == lifetimeToken) {
                lease.active = false;
                lease.features = 0;
                break;
            }
        }
        return false;
    }
    notifyFrame();
    return true;
}

void RealtimeAnalysisHub::removeDemand(AnalysisOwner owner,
    std::uint64_t lifetimeToken) noexcept
{
    if (lifetimeToken == 0) return;
    std::uint32_t nextMask = 0;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ownersMutex_);
        for (auto& lease : owners_) {
            if (lease.active && lease.owner == owner &&
                    lease.lifetimeToken == lifetimeToken) {
                lease.active = false;
                lease.features = 0;
                changed = true;
                break;
            }
        }
        for (const auto& lease : owners_)
            if (lease.active) nextMask |= lease.features;
    }
    if (!changed) return;
    activeFeatureMask_.store(nextMask, std::memory_order_release);
    active_.store(nextMask != 0, std::memory_order_release);
    if ((nextMask & kAnalysisFeatureAlbumBands) == 0)
        ambientSnapshot_.store(nullptr, std::memory_order_release);
    // Retain the last valid Dynamics publication while its feature is hidden.
    // A slow UI/pool exhaustion must not manufacture an empty intermediate.
    if ((nextMask & (kAnalysisFeatureCompactScope |
            kAnalysisFeatureSpectralHistory | kAnalysisFeatureDynamics)) == 0)
        snapshot_.store(nullptr, std::memory_order_release);
    notifyFrame();
}

bool RealtimeAnalysisHub::hasActiveFeatures() const noexcept
{
    return activeFeatureMask_.load(std::memory_order_acquire) != 0;
}

std::uint32_t RealtimeAnalysisHub::activeFeatureMask() const noexcept
{
    return activeFeatureMask_.load(std::memory_order_acquire);
}

void RealtimeAnalysisHub::notifyFrame() noexcept
{
    if (wakeEvent_ != nullptr) SetEvent(wakeEvent_);
}

void RealtimeAnalysisHub::configureScope(std::uint32_t windowMilliseconds,
    LargeScopeChannelMode mode, float gain) noexcept
{
    requestedScopeWindowMs_.store(windowMilliseconds, std::memory_order_release);
    requestedScopeMode_.store(mode, std::memory_order_release);
    requestedScopeGain_.store(gain, std::memory_order_release);
    notifyFrame();
}

ScopeParameters RealtimeAnalysisHub::scopeParameters() const noexcept
{
    return {
        requestedScopeWindowMs_.load(std::memory_order_acquire),
        requestedScopeMode_.load(std::memory_order_acquire),
        requestedScopeGain_.load(std::memory_order_acquire),
    };
}

void RealtimeAnalysisHub::stop() noexcept
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (!worker_.joinable()) return;
        stopping_.store(true, std::memory_order_release);
        active_.store(false, std::memory_order_release);
        notifyFrame();
        worker = std::move(worker_);
    }
    worker.join();
    stopping_.store(false, std::memory_order_release);
    snapshot_.store(nullptr, std::memory_order_release);
    ambientSnapshot_.store(nullptr, std::memory_order_release);
    currentDynamics_.store(nullptr, std::memory_order_release);
    activeFeatureMask_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> ownerLock(ownersMutex_);
        for (auto& lease : owners_) lease.active = false;
    }
}

std::shared_ptr<const RealtimeAnalysisSnapshot>
RealtimeAnalysisHub::snapshot() const noexcept
{
    if ((activeFeatureMask_.load(std::memory_order_acquire) &
            kAnalysisFeatureListenVisual) == 0) return {};
    const auto result = snapshot_.load(std::memory_order_acquire);
    // A reset may arrive while the worker is copying a publication. Its old
    // store must not revive a snapshot cleared by that newer request.
    if (result && (result->resetGeneration !=
            resetGeneration_.load(std::memory_order_acquire) ||
            result->listenResetGeneration !=
            listenResetGeneration_.load(std::memory_order_acquire))) return {};
    return result;
}

std::uint64_t RealtimeAnalysisHub::processedFrames() const noexcept
{
    return processedFrames_.load(std::memory_order_relaxed);
}

std::uint64_t RealtimeAnalysisHub::droppedPublications() const noexcept
{
    return droppedPublications_.load(std::memory_order_relaxed);
}

std::shared_ptr<const AmbientFeaturesV1>
RealtimeAnalysisHub::ambientSnapshot() const noexcept
{
    return ambientSnapshot_.load(std::memory_order_acquire);
}

std::uint64_t RealtimeAnalysisHub::ambientProcessedWindows() const noexcept
{
    return ambientProcessedWindows_.load(std::memory_order_relaxed);
}

bool RealtimeAnalysisHub::ensureWorker()
{
    std::lock_guard<std::mutex> lock(workerMutex_);
    if (worker_.joinable()) return true;
    if (wakeEvent_ == nullptr || stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    try {
        worker_ = std::thread(&RealtimeAnalysisHub::workerMain, this);
        return true;
    } catch (...) {
        return false;
    }
}

void RealtimeAnalysisHub::workerMain() noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    RealtimeFrameV1 frame{};
    while (!stopping_.load(std::memory_order_acquire)) {
        WaitForSingleObject(wakeEvent_, INFINITE);
        if (stopping_.load(std::memory_order_acquire)) break;
        const auto featureMask = activeFeatureMask_.load(
            std::memory_order_acquire);
        if (featureMask == 0) continue;

        const auto resetGeneration =
            resetGeneration_.load(std::memory_order_acquire);
        if (resetGeneration != appliedResetGeneration_) {
            model_.reset(requestedDuration_.load(std::memory_order_acquire));
            ++historyEpoch_;
            compactScopeModel_.reset();
            currentHistory_.reset();
            cursor_ = requestedSequence_.load(std::memory_order_acquire);
            ambientModel_.reset();
            appliedResetGeneration_ = resetGeneration;
        }
        const auto ambientResetGeneration =
            ambientResetGeneration_.load(std::memory_order_acquire);
        if (ambientResetGeneration != appliedAmbientResetGeneration_) {
            ambientModel_.reset();
            appliedAmbientResetGeneration_ = ambientResetGeneration;
        }
        const auto listenResetGeneration =
            listenResetGeneration_.load(std::memory_order_acquire);
        if (listenResetGeneration != appliedListenResetGeneration_) {
            model_.reset(requestedDuration_.load(std::memory_order_acquire));
            ++historyEpoch_;
            currentHistory_.reset();
            appliedListenResetGeneration_ = listenResetGeneration;
        }
        const auto dynamicsResetGeneration =
            dynamicsResetGeneration_.load(std::memory_order_acquire);
        if (dynamicsResetGeneration != appliedDynamicsResetGeneration_) {
            appliedDynamicsResetGeneration_ = dynamicsResetGeneration;
        }

        for (std::size_t drained = 0; drained < 64; ++drained) {
            std::uint64_t skipped = 0;
            if (!Runtime::instance().consumeNextFrame(
                    RealtimeConsumer::listen,
                    frame,
                    cursor_,
                    &skipped)) {
                break;
            }
            const auto discontinuity = skipped != 0 ||
                (frame.flags & kRealtimeFrameFlagDiscontinuity) != 0;
            if (discontinuity) {
                auto reasons = frame.discontinuityReasonMask;
                if (skipped != 0)
                    reasons |= reasonMask(DiscontinuityReason::publicationGap);
                if (reasons == 0)
                    reasons = reasonMask(DiscontinuityReason::explicitReset);
                pendingSnapshotDiscontinuity_.mark(
                    reasons, frame.sequence, frame.qpcTicks);
            }
            const auto currentMask = activeFeatureMask_.load(
                std::memory_order_acquire);
            if ((currentMask & kAnalysisFeatureSpectralHistory) != 0) {
                model_.configureLargeScope(
                    requestedScopeWindowMs_.load(std::memory_order_acquire),
                    requestedScopeMode_.load(std::memory_order_acquire),
                    requestedScopeGain_.load(std::memory_order_acquire));
            }
            bool publish = false;
            if ((currentMask & kAnalysisFeatureSpectralHistory) != 0 &&
                    model_.ingest(frame, false)) {
                processedFrames_.fetch_add(1, std::memory_order_relaxed);
                PerformanceTelemetry::instance().analysis(
                    TelemetrySurface::listen);
                publish = true;
            }
            if ((currentMask & kAnalysisFeatureCompactScope) != 0 &&
                    compactScopeModel_.ingest(frame)) {
                publish = true;
            }
            if ((currentMask & kAnalysisFeatureDynamics) != 0 &&
                    dynamicsModel_.ingest(frame, discontinuity)) {
                publishDynamics(dynamicsModel_.snapshot(
                    frame.sequence, frame.qpcTicks));
                publish = true;
            }
            if ((currentMask & kAnalysisFeatureAlbumBands) != 0) {
                if (!ambientModel_.configured() || discontinuity ||
                    ambientModel_.snapshot().streamGeneration !=
                        frame.streamGeneration ||
                    ambientModel_.snapshot().playbackSerial !=
                        frame.playbackSerial ||
                    ambientModel_.snapshot().recordingIdentityHash !=
                        frame.recordingIdentityHash ||
                    ambientModel_.snapshot().sampleRate != frame.sampleRate ||
                    ambientModel_.snapshot().channels != frame.channels) {
                    (void)ambientModel_.configure(frame.sampleRate,
                        frame.channels, frame.streamGeneration,
                        frame.firstFrameIndex, frame.playbackSerial,
                        frame.recordingIdentityHash);
                }
                const auto previousWindows = ambientModel_.completedWindows();
                if (ambientModel_.ingest(std::span<const float>(
                            frame.samples.data(), frame.sampleCount),
                        frame.firstFrameIndex, frame.streamGeneration,
                        frame.sequence, frame.qpcTicks) &&
                    ambientModel_.completedWindows() != previousWindows) {
                    ambientProcessedWindows_.fetch_add(
                        ambientModel_.completedWindows() - previousWindows,
                        std::memory_order_relaxed);
                    publishAmbient(ambientModel_.snapshot());
                }
            }
            if ((currentMask & (kAnalysisFeatureCollection |
                    kAnalysisFeatureAlloyV2Notes)) != 0)
                collection::Bridge::instance().ingest(frame, discontinuity,
                    (currentMask & kAnalysisFeatureAlloyV2Notes) != 0);
            if (publish) {
                const auto pending = pendingSnapshotDiscontinuity_.pending();
                if (publishSnapshot(frame, pending)) {
                    pendingSnapshotDiscontinuity_.commit(pending);
                } else {
                    pendingSnapshotDiscontinuity_.rejected();
                }
            }
        }
    }
}

void RealtimeAnalysisHub::publishAmbient(
    const AmbientFeaturesV1& features)
{
    std::shared_ptr<AmbientFeaturesV1> next;
    for (std::size_t offset = 0; offset < ambientPool_.size(); ++offset) {
        const auto index =
            (ambientPoolCursor_ + offset) % ambientPool_.size();
        auto& candidate = ambientPool_[index];
        if (candidate.use_count() != 1) continue;
        next = candidate;
        ambientPoolCursor_ = (index + 1) % ambientPool_.size();
        break;
    }
    if (!next) {
        droppedPublications_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    *next = features;
    ambientSnapshot_.store(
        std::static_pointer_cast<const AmbientFeaturesV1>(next),
        std::memory_order_release);
}

bool RealtimeAnalysisHub::publishSnapshot(
    const RealtimeFrameV1& frame,
    const PendingDiscontinuity& discontinuity)
{
    std::shared_ptr<RealtimeAnalysisSnapshot> next;
    for (std::size_t offset = 0; offset < snapshotPool_.size(); ++offset) {
        const auto index =
            (snapshotPoolCursor_ + offset) % snapshotPool_.size();
        auto& candidate = snapshotPool_[index];
        if (candidate.use_count() != 1) continue;
        next = candidate;
        snapshotPoolCursor_ = (index + 1) % snapshotPool_.size();
        break;
    }
    if (!next) {
        droppedPublications_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!currentHistory_ ||
        currentHistory_->spectrogramRevision != model_.spectrogramRevision()) {
        publishHistory();
    }
    next->generation = ++publishedGeneration_;
    next->resetGeneration = appliedResetGeneration_;
    next->listenResetGeneration = appliedListenResetGeneration_;
    next->sourceSequence = frame.sequence;
    next->sourceQpc = frame.qpcTicks;
    next->streamGeneration = frame.streamGeneration;
    next->discontinuity = static_cast<bool>(discontinuity);
    next->discontinuityReasonMask = discontinuity.reasonMask;
    next->discontinuityFirstSequence = discontinuity.firstSequence;
    next->discontinuityRejectedPublications =
        discontinuity.rejectedAttempts;
    next->featureMask = activeFeatureMask_.load(std::memory_order_acquire);
    next->visual = model_.snapshot();
    next->scope = compactScopeRenderState(compactScopeModel_);
    next->dynamics = currentDynamics_.load(std::memory_order_acquire);
    next->history = currentHistory_;
    snapshot_.store(
        std::static_pointer_cast<const RealtimeAnalysisSnapshot>(next),
        std::memory_order_release);
    return true;
}

void RealtimeAnalysisHub::publishDynamics(
    const DynamicsSnapshot& dynamics)
{
    std::shared_ptr<DynamicsSnapshot> next;
    for (std::size_t offset = 0; offset < dynamicsPool_.size(); ++offset) {
        const auto index =
            (dynamicsPoolCursor_ + offset) % dynamicsPool_.size();
        auto& candidate = dynamicsPool_[index];
        if (candidate.use_count() != 1) continue;
        next = candidate;
        dynamicsPoolCursor_ = (index + 1U) % dynamicsPool_.size();
        break;
    }
    if (!next) {
        droppedPublications_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    *next = dynamics;
    currentDynamics_.store(
        std::static_pointer_cast<const DynamicsSnapshot>(next),
        std::memory_order_release);
}

void RealtimeAnalysisHub::publishHistory()
{
    std::shared_ptr<RealtimeHistorySnapshot> next;
    for (std::size_t offset = 0; offset < historyPool_.size(); ++offset) {
        const auto index =
            (historyPoolCursor_ + offset) % historyPool_.size();
        auto& candidate = historyPool_[index];
        if (candidate.use_count() != 1) continue;
        next = candidate;
        historyPoolCursor_ = (index + 1) % historyPool_.size();
        break;
    }
    if (!next) {
        droppedPublications_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    next->spectrogram = model_.spectrogram();
    next->epoch = historyEpoch_;
    next->spectrogramHead = model_.spectrogramHead();
    next->spectrogramRevision = model_.spectrogramRevision();
    next->musicMap = model_.musicMap();
    next->energyHistory = model_.energyHistory();
    next->widthHistory = model_.widthHistory();
    next->brightnessHistory = model_.brightnessHistory();
    next->historyHead = model_.historyHead();
    next->durationSeconds = model_.durationSeconds();
    currentHistory_ = std::static_pointer_cast<const RealtimeHistorySnapshot>(next);
}

} // namespace nekorox::component
