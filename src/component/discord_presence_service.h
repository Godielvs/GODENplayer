#pragma once

#include "discord_cover_service.h"
#include "presence_cover_policy.h"
#include "nekorox/contracts.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace nekorox::component {

enum class DiscordPresenceStatus : std::uint8_t {
    disabled = 0,
    connecting,
    connected,
    discordUnavailable,
    applicationRejected,
};

enum class DiscordCoverStatus : std::uint8_t {
    disabled = 0,
    idle,
    resolving,
    ready,
    fallback,
};

struct DiscordPresenceSettings {
    bool enabled = false;
    bool showPaused = false;
    bool temporaryCovers = true;
    std::string applicationId;
    PresenceCoverProvider coverProvider = PresenceCoverProvider::unconfigured;
    std::string relayEndpoint;
    bool relayCredentialSaved = false;
    bool coverDisclosureAccepted = false;
    std::uint32_t automaticProviders = coverProviderBit(PresenceCoverProvider::catbox);
    bool imgbbKeySaved = false;
};

// Ephemeral DAW activity. It is deliberately owned by the Presence service
// rather than persisted in the playback snapshot: the foobar transport stays
// paused while the independent WinMM preview is audible.
struct DiscordDawPresence {
    bool active = false;
    PlaybackState state = PlaybackState::stopped;
    std::uint32_t trackCount = 0;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    std::string focus;
};

struct PresenceUpdateV2 {
    std::uint64_t sessionGeneration = 0;
    std::uint64_t presenceRevision = 0;
    std::uint64_t coverEpoch = 0;
    std::uint64_t trackKey = 0;
    std::uint64_t artContentGeneration = 0;
    std::uint64_t seekGeneration = 0;
    PlaybackState state = PlaybackState::stopped;
    bool enabled = false;
    bool started = false;
    bool showPaused = false;
    bool temporaryCovers = false;
    bool allowCover = false;
    bool urgentClear = false;
};

struct PresenceUpdateV3 {
    PresenceUpdateV2 base{};
    std::uint64_t trackGeneration = 0;
    std::uint64_t deliveryRevision = 0;
    std::string coverToken;
};

struct PresenceDeliveryTraceV3 {
    std::uint64_t trackGeneration = 0;
    std::uint64_t activityCreatedMilliseconds = 0;
    std::uint64_t activitySentMilliseconds = 0;
    std::uint64_t activityAcknowledgedMilliseconds = 0;
    std::uint64_t mediaRequestedMilliseconds = 0;
    std::uint64_t coverDeliveredMilliseconds = 0;
    DiscordRelayDelivery delivery = DiscordRelayDelivery::none;
    std::string coverToken;
    std::string error;
};

struct DiscordTrackPresence {
    PresenceUpdateV2 update{};
    PresenceUpdateV3 updateV3{};
    PlaybackSnapshotV2 playback{};
    std::string codec;
    std::string bitrate;
    std::string sampleRate;
    std::string bitDepth;
    std::string channels;
    std::string albumArtist;
    std::string musicBrainzRecordingId;
    std::string musicBrainzReleaseId;
    std::string coverUrl;
    std::string coverToken;
    std::int64_t relayExpiresAtMilliseconds = 0;
    std::uint64_t relayConnectionGeneration = 0;
    metadb_handle_ptr trackHandle;
    bool dawMode = false;
    std::uint32_t dawTrackCount = 0;
    // True for ordinary local files whose duration is expected to become
    // available. Live/non-seekable streams legitimately have no end time.
    bool finiteLocalFile = false;
    bool localFileFallback = false;
    std::string fileName;
};

class DiscordPresenceService final {
public:
    static DiscordPresenceService& instance();

    void start();
    void stop();
    // Must be called on foobar2000's main thread. Metadata is copied before
    // the event is handed to the below-normal-priority IPC worker.
    void publishCurrent(bool force = false);
    void publishDawPreview(DiscordDawPresence presence);
    void clearDawPreview();

    [[nodiscard]] DiscordPresenceSettings settings() const;
    void setEnabled(bool enabled);
    void setShowPaused(bool showPaused);
    void setTemporaryCovers(bool enabled);
    bool setApplicationId(std::string applicationId);
    bool setCoverConfiguration(PresenceCoverProvider provider, std::string endpoint,
        std::string credential, bool disclosureAccepted,
        std::uint32_t automaticProviders = coverProviderBit(PresenceCoverProvider::catbox),
        std::string imgbbKey = {});
    // Uses the saved provider/permission; never enables uploads implicitly.
    bool retryCurrentCover(bool verifyCachedImage = false);

    [[nodiscard]] DiscordPresenceStatus status() const noexcept;
    [[nodiscard]] DiscordCoverStatus coverStatus() const noexcept;
    [[nodiscard]] bool externalPublisherConflict() const noexcept;
    [[nodiscard]] std::wstring statusText() const;
    [[nodiscard]] PresenceDeliveryTraceV3 deliveryTrace() const;

private:
    DiscordPresenceService();
    ~DiscordPresenceService();
    DiscordPresenceService(const DiscordPresenceService&) = delete;
    DiscordPresenceService& operator=(const DiscordPresenceService&) = delete;

    DiscordTrackPresence captureCurrent() const;
    void launchWorker();
    void launchWorkersLocked() noexcept;
    void stopWorker();
    void stopWorkerAsync(std::uint64_t clearRevision);
    void drainWorkers() noexcept;
    void workerMain(std::stop_token stopToken);
    void coverWorkerMain(std::stop_token stopToken);
    void setStatus(DiscordPresenceStatus status) noexcept;
    void enqueuePrivacyClear();
    void refreshExternalPublisherConflict() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::condition_variable drainChanged_;
    DiscordTrackPresence pending_{};
    std::uint64_t pendingRevision_ = 0;
    std::uint64_t sentRevision_ = 0;
    std::uint64_t sessionGeneration_ = 0;
    std::uint64_t coverEpoch_ = 0;
    std::uint64_t lastCoverTrackKey_ = 0;
    bool lastCoverPolicy_ = false;
    bool started_ = false;
    bool drainPending_ = false;
    // Reserved before workers are admitted: opt-out never allocates a drain
    // job or falls back to joining on the UI when a shared queue is full.
    void* drainWork_ = nullptr;
    std::jthread drainingRpc_;
    std::jthread drainingCover_;
    std::uint64_t drainingClearRevision_ = 0;
    std::jthread worker_;
    std::jthread coverWorker_;
    DiscordCoverService coverService_;
    DiscordDawPresence dawPresence_{};
    std::atomic<DiscordPresenceStatus> status_{
        DiscordPresenceStatus::disabled};
    std::atomic<DiscordCoverStatus> coverStatus_{
        DiscordCoverStatus::disabled};
    std::atomic<DiscordCoverFailure> coverFailure_{DiscordCoverFailure::none};
    std::atomic<bool> coverRetryPending_{false};
    std::atomic<PresenceCoverProvider> deliveredCoverProvider_{PresenceCoverProvider::unconfigured};
    std::atomic<bool> rpcReady_{false};
    std::atomic<bool> externalPublisherConflict_{false};
    std::atomic<std::uint64_t> staleCoverDiscards_{0};
    PresenceDeliveryTraceV3 deliveryTrace_{};
};

} // namespace nekorox::component
