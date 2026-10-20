#pragma once
#include "ambient_texture_scheduler.h"
#include "collection/collection_motion.h"
#include "collection/c9_light_features.h"
#include "c11/c11_note_lights.h"
#include "ui_preferences.h"
#include "nekorox/ambient_features.h"
#include "nekorox/flow_signal.h"
#include "visual_capture_recovery.h"
#include <array>
#include <cstdint>

namespace nekorox::component {
enum class AlbumAmbientState : std::uint8_t {
    offStatic=0, waitingSource, active, decay, bookletFrozen, hidden, staticFallback,
};
struct AlbumAmbientPose {
    collection::LightBands lights{};
    collection::LightBands alloyBroad{};
    goden::c11::NoteLights alloyNotes{};
    std::array<float, kAmbientBandCount> drive{};
    std::array<float, kAmbientBandCount> transient{};
    std::array<float, kAmbientBandCount> phase{};
    std::array<float, kAmbientBandCount> absoluteEnergy{};
    std::array<float, kAmbientBandCount> relativeFast{1.0f,1.0f,1.0f};
    std::array<float, kAmbientBandCount> relativeSlow{1.0f,1.0f,1.0f};
    std::array<float, kAmbientBandCount> growth{};
    std::array<float, kAmbientBandCount> attack{};
    float availability=0.0f, gate=0.0f, sinePhase=0.0f, cosinePhase=1.0f, gain=0.0f;
    float alloyFreshness=0.0f;
    std::uint64_t publicationRevision=0;
};
struct AlbumAmbientProgress {
    std::uint64_t desiredRevision=0, producedRevision=0, acceptedRevision=0;
    std::uint64_t nextTextureQpc=0, lastTextureProductionQpc=0;
    std::uint64_t attempts=0, acceptedPresents=0, rejectedPresents=0;
    std::uint64_t deferTransitions=0;
    std::uint32_t deferReasonMask=0;
    bool deferred=false;
};
class AlbumAmbientController final {
public:
    AlbumAmbientController() noexcept;
    ~AlbumAmbientController();
    AlbumAmbientController(const AlbumAmbientController&) = delete;
    AlbumAmbientController& operator=(const AlbumAmbientController&) = delete;
    void setUserPaused(bool paused,std::uint64_t nowQpc) noexcept;
    void sync(const AlbumAmbientEligibility&, std::uint64_t stream, double displayHz,
        std::uint64_t nowQpc) noexcept;
    bool pump(std::uint64_t nowQpc) noexcept;
    [[nodiscard]] AlbumAmbientPose sample(std::uint64_t nowQpc) noexcept;
    [[nodiscard]] bool textureDue(std::uint64_t nowQpc) const noexcept;
    void deferTexture(std::uint64_t nowQpc, std::uint32_t reasonMask) noexcept;
    void resumeTexture(std::uint64_t nowQpc) noexcept;
    void observeTexturePolicy(std::uint32_t reasonMask) noexcept;
    void texturePresented(std::uint64_t nowQpc, bool produced, bool accepted,
        std::uint64_t capturedRevision=0) noexcept;
    void renderUnavailable(std::uint64_t nowQpc) noexcept;
    void retryRendering() noexcept;
    // A missing presentation surface suspends this owner's work without
    // clearing a shader failure or changing the user's transport state.
    void suspendPresentation(std::uint64_t nowQpc) noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool wantsPump() const noexcept {return leaseActive_ || physicalMotion_;}
    [[nodiscard]] bool animationActive(std::uint64_t) const noexcept;
    [[nodiscard]] std::uint64_t nextPumpQpc(std::uint64_t nowQpc) const noexcept;
    [[nodiscard]] std::uint64_t nextTextureQpc() const noexcept {return textureSchedule_.deadlineQpc();}
    [[nodiscard]] AlbumAmbientState state() const noexcept {return state_;}
    [[nodiscard]] std::uint64_t visualRevision() const noexcept {return visualRevision_;}
    [[nodiscard]] std::uint64_t presentedRevision() const noexcept {return presentedRevision_;}
    [[nodiscard]] bool physicalMotion() const noexcept {return physicalMotion_;}
    [[nodiscard]] bool lastIngestAccepted() const noexcept {return lastIngestAccepted_;}
    [[nodiscard]] AlbumAmbientProgress progress() const noexcept;
    [[nodiscard]] const collection::LightBands& lightDiagnostics() const noexcept {
        return lastScheduledPose_.lights;
    }
private:
    void acquire(std::uint64_t startSequence) noexcept;
    void release(std::uint64_t nowQpc, AlbumAmbientState) noexcept;
    bool reconcilePose(std::uint64_t nowQpc) noexcept;
    void publishDiagnostics(AlbumAmbientFallback) noexcept;
    [[nodiscard]] double qpcSeconds(std::uint64_t ticks) const noexcept;
    std::uint64_t lifetimeToken_=0, qpcFrequency_=1, expectedStreamGeneration_=0;
    std::uint64_t lastPublicationRevision_=0, sourceFreshnessQpc_=0;
    std::uint64_t visualRevision_=1, presentedRevision_=0;
    std::uint64_t producedRevision_=0, lastTextureProductionQpc_=0;
    std::uint64_t textureAttempts_=0, acceptedPresents_=0, rejectedPresents_=0;
    std::uint64_t deferTransitions_=0;
    std::uint32_t deferReasonMask_=0;
    std::uint64_t unavailableStreamGeneration_=0;
    bool userPaused_=false,pauseTail_=false;
    bool spectralLights_=false,alloyV2Notes_=false;
    double pauseStarted_=0;
    std::array<float,3> pausePhaseOffset_{};
    AlbumAmbientPose pausePose_{};
    collection::MotionRate pauseRate_;
    FlowBandFeatures flowFeatures_{};
    FlowSignal flowSignal_{};
    FlowPose flowPose_{};
    AlbumAmbientPose lastScheduledPose_{};
    bool playbackActive_=false, muted_=false, leaseActive_=false;
    bool sourceStale_=false, physicalMotion_=false, lastIngestAccepted_=false;
    bool renderUnavailable_=false, visible_=false, tailAllowed_=false;
    AlbumAmbientMode mode_=AlbumAmbientMode::reactive;
    AlbumAmbientState state_=AlbumAmbientState::offStatic;
    AlbumAmbientDiagnostics diagnostics_{};
    AmbientTextureScheduler textureSchedule_{};
    VisualSourceRecovery sourceRecovery_{};
};
} // namespace nekorox::component
