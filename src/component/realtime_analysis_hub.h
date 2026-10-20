#pragma once

#include "nekorox/ambient_features.h"
#include "nekorox/continuity.h"
#include "nekorox/dynamics_history.h"
#include "nekorox/realtime_visual_model.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace nekorox::component {

struct RealtimeHistorySnapshot {
    std::uint64_t epoch = 0;
    std::array<std::array<float, kSpectrogramBins>, kSpectrogramColumns>
        spectrogram{};
    std::size_t spectrogramHead = 0;
    std::uint64_t spectrogramRevision = 0;
    std::array<MusicMapBin, kMusicMapBins> musicMap{};
    std::array<float, kEvolutionSamples> energyHistory{};
    std::array<float, kEvolutionSamples> widthHistory{};
    std::array<float, kEvolutionSamples> brightnessHistory{};
    std::size_t historyHead = 0;
    double durationSeconds = 0.0;
};

struct RealtimeAnalysisSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t resetGeneration = 0;
    std::uint64_t listenResetGeneration = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t sourceQpc = 0;
    std::uint64_t streamGeneration = 0;
    bool discontinuity = false;
    std::uint32_t discontinuityReasonMask = 0;
    std::uint64_t discontinuityFirstSequence = 0;
    std::uint64_t discontinuityRejectedPublications = 0;
    std::uint32_t featureMask = 0;
    RealtimeVisualSnapshot visual{};
    CompactScopeRenderState scope{};
    std::shared_ptr<const DynamicsSnapshot> dynamics;
    // Large, slowly changing history is versioned independently. Fast scope
    // and vectorscope publications therefore never copy the whole rolling
    // spectrogram merely to expose a newer PCM timestamp.
    std::shared_ptr<const RealtimeHistorySnapshot> history;
};

struct ScopeParameters {
    std::uint32_t windowMilliseconds = 30;
    LargeScopeChannelMode mode = LargeScopeChannelMode::mid;
    float gain = 1.0f;
};

enum class AnalysisOwner : std::uint8_t {
    listen = 0,
    albumAmbient = 1,
    header = 2,
};

inline constexpr std::uint32_t kAnalysisFeatureCompactScope = 1U << 0U;
inline constexpr std::uint32_t kAnalysisFeatureSpectralHistory = 1U << 1U;
inline constexpr std::uint32_t kAnalysisFeatureDynamics = 1U << 2U;
inline constexpr std::uint32_t kAnalysisFeatureAlbumBands = 1U << 3U;
inline constexpr std::uint32_t kAnalysisFeatureCollection = 1U << 4U;
inline constexpr std::uint32_t kAnalysisFeatureAlloyV2Notes = 1U << 5U;
inline constexpr std::uint32_t kAnalysisFeatureListenVisual =
    kAnalysisFeatureCompactScope | kAnalysisFeatureSpectralHistory |
    kAnalysisFeatureDynamics;
inline constexpr std::uint32_t kAnalysisKnownFeatures =
    kAnalysisFeatureListenVisual | kAnalysisFeatureAlbumBands |
    kAnalysisFeatureCollection | kAnalysisFeatureAlloyV2Notes;

// One below-normal analysis worker consumes the multicast PCM bus and publishes
// immutable model snapshots. UI surfaces only interpolate/present these values.
class RealtimeAnalysisHub final {
public:
    static RealtimeAnalysisHub& instance();

    void activate(double durationSeconds, std::uint64_t startSequence);
    void reset(double durationSeconds, std::uint64_t startSequence);
    void deactivate() noexcept;
    bool setOwnerActive(AnalysisOwner owner, std::uint64_t lifetimeToken,
        bool active, double durationSeconds = 0.0,
        std::uint64_t startSequence = 0) noexcept;
    bool setDemand(AnalysisOwner owner, std::uint64_t lifetimeToken,
        std::uint32_t featureMask, double durationSeconds = 0.0,
        std::uint64_t startSequence = 0) noexcept;
    void removeDemand(AnalysisOwner owner,
        std::uint64_t lifetimeToken) noexcept;
    [[nodiscard]] bool hasActiveFeatures() const noexcept;
    [[nodiscard]] std::uint32_t activeFeatureMask() const noexcept;
    void notifyFrame() noexcept;
    void configureScope(std::uint32_t windowMilliseconds,
        LargeScopeChannelMode mode, float gain = 1.0f) noexcept;
    [[nodiscard]] ScopeParameters scopeParameters() const noexcept;
    void stop() noexcept;

    [[nodiscard]] std::shared_ptr<const RealtimeAnalysisSnapshot>
        snapshot() const noexcept;
    [[nodiscard]] std::uint64_t processedFrames() const noexcept;
    [[nodiscard]] std::uint64_t droppedPublications() const noexcept;
    [[nodiscard]] std::shared_ptr<const AmbientFeaturesV1>
        ambientSnapshot() const noexcept;
    [[nodiscard]] std::uint64_t ambientProcessedWindows() const noexcept;

private:
    RealtimeAnalysisHub();
    ~RealtimeAnalysisHub();
    RealtimeAnalysisHub(const RealtimeAnalysisHub&) = delete;
    RealtimeAnalysisHub& operator=(const RealtimeAnalysisHub&) = delete;

    bool ensureWorker();
    void workerMain() noexcept;
    [[nodiscard]] bool publishSnapshot(
        const RealtimeFrameV1& frame,
        const PendingDiscontinuity& discontinuity);
    void publishHistory();
    void publishAmbient(const AmbientFeaturesV1& features);
    void publishDynamics(const DynamicsSnapshot& dynamics);

    struct OwnerLease {
        AnalysisOwner owner = AnalysisOwner::listen;
        std::uint64_t lifetimeToken = 0;
        std::uint32_t features = 0;
        bool active = false;
    };

    mutable std::mutex workerMutex_;
    std::thread worker_;
    HANDLE wakeEvent_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> active_{false};
    std::array<OwnerLease, 16> owners_{};
    std::mutex ownersMutex_;
    std::atomic<std::uint32_t> activeFeatureMask_{0};
    std::atomic<double> requestedDuration_{0.0};
    std::atomic<std::uint64_t> requestedSequence_{0};
    std::atomic<std::uint64_t> resetGeneration_{0};
    std::atomic<std::uint64_t> listenResetGeneration_{0};
    std::atomic<std::uint64_t> ambientResetGeneration_{0};
    std::atomic<std::uint64_t> dynamicsResetGeneration_{0};
    std::atomic<std::uint64_t> processedFrames_{0};
    std::atomic<std::uint64_t> droppedPublications_{0};
    std::atomic<std::uint64_t> ambientProcessedWindows_{0};
    std::atomic<std::uint32_t> requestedScopeWindowMs_{30};
    std::atomic<LargeScopeChannelMode> requestedScopeMode_{
        LargeScopeChannelMode::mid};
    std::atomic<float> requestedScopeGain_{1.0f};
    std::atomic<std::shared_ptr<const RealtimeAnalysisSnapshot>> snapshot_;
    // A fixed pool avoids allocating/copy-owning a large analysis object at
    // 240 Hz. A slot is only reused once the atomic latest pointer and every
    // UI reader have released it; slow readers therefore cause a dropped
    // intermediate publication instead of blocking realtime analysis.
    std::array<std::shared_ptr<RealtimeAnalysisSnapshot>, 4> snapshotPool_{};
    std::array<std::shared_ptr<RealtimeHistorySnapshot>, 8> historyPool_{};
    std::array<std::shared_ptr<AmbientFeaturesV1>, 4> ambientPool_{};
    std::array<std::shared_ptr<DynamicsSnapshot>, 4> dynamicsPool_{};
    std::size_t snapshotPoolCursor_ = 0;
    std::size_t historyPoolCursor_ = 0;
    std::size_t ambientPoolCursor_ = 0;
    std::size_t dynamicsPoolCursor_ = 0;
    std::shared_ptr<const RealtimeHistorySnapshot> currentHistory_;
    RealtimeVisualModel model_;
    CompactScopeModel compactScopeModel_;
    DynamicsHistory dynamicsModel_;
    AmbientFeaturesModel ambientModel_;
    std::atomic<std::shared_ptr<const AmbientFeaturesV1>> ambientSnapshot_;
    std::atomic<std::shared_ptr<const DynamicsSnapshot>> currentDynamics_;
    std::uint64_t cursor_ = 0;
    std::uint64_t appliedResetGeneration_ = 0;
    std::uint64_t appliedListenResetGeneration_ = 0;
    std::uint64_t appliedAmbientResetGeneration_ = 0;
    std::uint64_t appliedDynamicsResetGeneration_ = 0;
    DiscontinuityLatch pendingSnapshotDiscontinuity_{};
    std::uint64_t publishedGeneration_ = 0;
    std::uint64_t historyEpoch_ = 0;
};

} // namespace nekorox::component
