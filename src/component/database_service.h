#pragma once

#include "nekorox/database.h"
#include "nekorox/contracts.h"
#include "nekorox/packet_cost.h"

#include "sdk_compat.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nekorox::component {

enum class DatabaseHealthState : std::uint8_t {
    unavailable,
    opening,
    ready,
    transientError,
    incompatibleSchema,
};

enum class DatabaseCommandState : std::uint8_t {
    none,
    accepted,
    persisted,
    failed,
};

struct HistorySnapshot {
    std::uint64_t generation = 0;
    bool available = false;
    bool readAvailable = false;
    bool writerAvailable = false;
    std::string sessionId;
    std::string error;
    DatabaseHealthState health = DatabaseHealthState::unavailable;
    unsigned retryAttempt = 0;
    std::size_t queuedEvents = 0;
    std::vector<HistoryEventRecord> events;
};

struct AnalysisCachePayload {
    bool found = false;
    std::filesystem::path trackPath;
    AnalysisMediaInfoV1 media{};
    std::shared_ptr<const std::vector<AnalysisWaveformBinV1>> waveform;
    std::string resultJson;
    std::string error;
};

using AnalysisCacheCallback = std::function<void(AnalysisCachePayload)>;

struct PacketCostCachePayload {
    bool found = false;
    std::filesystem::path trackPath;
    // Identity observed by the cache worker, including the file fingerprint.
    std::string sourceTrackId;
    std::shared_ptr<const PacketCostResult> cost;
    std::string error;
};

using PacketCostCacheCallback = std::function<void(PacketCostCachePayload)>;
struct TrackIdentityBatchPayload {
    std::vector<TrackIdentityResolution> identities;
    // A failed batch has no partial identities. Missing revisions are a
    // successful result with found=false, distinct from database failure.
    std::string error;
};
using TrackIdentityBatchCallback = std::function<void(
    TrackIdentityBatchPayload)>;

struct TrackRevisionRebind {
    std::string previousRevisionTrackId;
    std::filesystem::path currentPath;
    std::uint32_t subsong = 0;
    // Verified file workers may freeze the exact committed revision. Avoid
    // identifying a different edit that happens while this request is queued.
    std::optional<PersistedTrack> frozenRevision;
};
using TrackRevisionRebindCallback = std::function<void(bool, std::string)>;

struct LyricsCachePayload {
    bool found = false;
    PersistedLyricsDocumentV2 document;
    PersistedLyricsBindingV2 binding;
    PersistedLyricsRejectionV2 rejection;
    std::string error;
};
using LyricsCacheCallback = std::function<void(LyricsCachePayload)>;

struct TrackUserStateSnapshot {
    std::uint64_t generation = 0;
    bool available = false;
    bool stale = false;
    std::string entityId;
    std::string canonicalTrackId;
    std::string revisionTrackId;
    std::string trackId;
    bool favorite = false;
    std::string note;
    std::string error;
    std::uint64_t commandId = 0;
    DatabaseCommandState commandState = DatabaseCommandState::none;
};

struct PersonaSnapshot {
    std::uint64_t generation = 0;
    bool available = false;
    std::string artistAlias;
    std::string aliasKey;
    PersistedPersona persona;
    std::string error;
};

class DatabaseService final {
public:
    static DatabaseService& instance();

    [[nodiscard]] bool start();
    void stop();
    void retry();

    void onNewTrack(const metadb_handle_ptr& track);
    void onPause(bool paused);
    void onSeek(double seconds);
    void onStop(play_control::t_stop_reason reason);

    void loadAnalysis(
        std::filesystem::path trackPath,
        AnalysisCacheCallback callback);
    void storeAnalysis(
        std::filesystem::path trackPath,
        std::string expectedTrackId,
        AnalysisMediaInfoV1 media,
        std::shared_ptr<const std::vector<AnalysisWaveformBinV1>> waveform,
        std::string resultJson);
    // Latest-only read for the single PacketCostController: replaces any older
    // pending read without invoking its callback. Already running reads finish.
    void loadPacketCost(
        std::filesystem::path trackPath,
        PacketCostCacheCallback callback);
    void storePacketCost(
        std::filesystem::path trackPath,
        std::string expectedTrackId,
        std::shared_ptr<const PacketCostResult> cost);
    // Non-null callbacks complete once on the main thread, including rejection.
    // Empty IDs retain their index as unresolved placeholders, without a query.
    void resolveTrackIdentities(
        std::vector<std::string> revisionTrackIds,
        TrackIdentityBatchCallback callback);
    void rebindTagRevisions(
        std::vector<TrackRevisionRebind> revisions,
        TrackRevisionRebindCallback callback);
    void loadLyrics(
        std::filesystem::path trackPath,
        PersistedLyricsBindingV2 binding,
        LyricsCacheCallback callback);
    void storeLyrics(
        std::filesystem::path trackPath,
        PersistedLyricsDocumentV2 document,
        PersistedLyricsBindingV2 binding);
    void storeLyricsRejection(
        std::filesystem::path trackPath,
        PersistedLyricsRejectionV2 rejection);

    std::shared_ptr<const HistorySnapshot> historySnapshot() const;
    std::shared_ptr<const TrackUserStateSnapshot> userStateSnapshot() const;
    std::shared_ptr<const PersonaSnapshot> personaSnapshot() const;
    bool toggleCurrentFavorite();
    bool setCurrentNote(
        std::string note,
        const std::string& expectedTrackId = {});
    bool saveCurrentPersona(
        PersistedPersona persona,
        const std::string& expectedAliasKey = {});

private:
    struct TrackSeed {
        std::filesystem::path nativePath;
        std::string sourcePath;
        std::string artist;
        std::string album;
        std::string title;
        std::uint32_t subsong = 0;
        std::int64_t durationMs = 0;
    };

    struct AnalysisTask;
    struct UserStateTask;
    struct PersonaTask;
    struct IdentityBatchTask;
    struct RevisionRebindTask;
    struct LyricsTask;

    // Shared by all events admitted for one track. Identity is resolved once,
    // on the writer, and survives retries or eviction of its start event.
    struct PlaybackContext {
        std::unique_ptr<TrackSeed> seed;
        std::optional<PersistedTrack> track;
        bool resolutionAttempted = false;
        bool initialStatePublished = false;
    };

    struct PendingEvent {
        std::string eventUid;
        PlayEventType type = PlayEventType::start;
        std::int64_t utcMs = 0;
        std::int64_t positionMs = 0;
        std::int64_t durationMs = 0;
        std::int32_t playlistIndex = -1;
        std::int32_t itemIndex = -1;
        std::shared_ptr<PlaybackContext> playbackContext;
        bool playbackAdmitted = false;
        bool playbackProcessed = false;
        std::unique_ptr<AnalysisTask> analysis;
        std::unique_ptr<UserStateTask> userState;
        std::unique_ptr<PersonaTask> persona;
        std::unique_ptr<IdentityBatchTask> identities;
        std::unique_ptr<RevisionRebindTask> revisionRebind;
        std::unique_ptr<LyricsTask> lyrics;
        std::chrono::steady_clock::time_point readyAt{};
        bool deferred = false;
        bool retryDatabase = false;
        unsigned writeRetryAttempt = 0;
    };

    struct AnalysisTask {
        enum class Kind : std::uint8_t {
            loadDeep,
            storeDeep,
            loadPacketCost,
            storePacketCost,
        };
        Kind kind = Kind::loadDeep;
        std::filesystem::path trackPath;
        AnalysisMediaInfoV1 media{};
        std::shared_ptr<const std::vector<AnalysisWaveformBinV1>> waveform;
        std::string resultJson;
        AnalysisCacheCallback callback;
        std::shared_ptr<const PacketCostResult> packetCost;
        std::string expectedPacketCostTrackId;
        std::string expectedDeepAnalysisTrackId;
        PacketCostCacheCallback packetCostCallback;
    };

    struct UserStateTask {
        enum class Kind : std::uint8_t {
            setFavorite,
            setNote,
        };
        Kind kind = Kind::setFavorite;
        std::string expectedTrackId;
        bool favorite = false;
        std::string note;
        std::uint64_t commandId = 0;
    };

    struct PersonaTask {
        std::string expectedAliasKey;
        PersistedPersona persona;
    };

    struct IdentityBatchTask {
        std::vector<std::string> revisionTrackIds;
        TrackIdentityBatchCallback callback;
    };

    struct RevisionRebindTask {
        std::vector<TrackRevisionRebind> revisions;
        TrackRevisionRebindCallback callback;
    };

    struct LyricsTask {
        enum class Kind : std::uint8_t { load, store, reject };
        Kind kind = Kind::load;
        std::filesystem::path trackPath;
        PersistedLyricsDocumentV2 document;
        PersistedLyricsBindingV2 binding;
        PersistedLyricsRejectionV2 rejection;
        LyricsCacheCallback callback;
    };

    DatabaseService() = default;
    // On rejection, leave event owned by the caller for failure publication.
    bool enqueue(PendingEvent&& event);
    std::size_t pendingPlaybackWriteCount();
    PendingEvent makeEvent(PlayEventType type) const;
    std::unique_ptr<TrackSeed> makeTrackSeed(const metadb_handle_ptr& track) const;
    void workerMain(std::filesystem::path databasePath, std::string sessionId);
    void publishReady(const std::string& sessionId);
    void publishError(std::string error,
        DatabaseHealthState health = DatabaseHealthState::transientError,
        unsigned retryAttempt = 0);
    void publishEvent(const PersistedPlayEvent& event, const PersistedTrack& track);
    void publishAnalysisResult(
        AnalysisCacheCallback callback,
        AnalysisCachePayload payload);
    void publishPacketCostResult(
        PacketCostCacheCallback callback,
        PacketCostCachePayload payload);
    void publishUserState(
        PersistedTrackUserState state,
        std::string error = {},
        std::uint64_t commandId = 0,
        const std::shared_ptr<PlaybackContext>& expectedPlayback = {});
    void publishUserCommandAccepted(std::uint64_t commandId);
    void publishUserStateLoading();
    void publishPersona(
        std::string artistAlias,
        std::string aliasKey,
        PersistedPersona persona,
        std::string error = {},
        const std::shared_ptr<PlaybackContext>& expectedPlayback = {});
    void publishPersonaLoading();

    mutable std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<PendingEvent> queue_;
    // Failed playback writes are retained in memory only.  Manual Retry
    // replays the bounded spool with the original immutable event UID.
    std::deque<PendingEvent> writeSpool_;
    std::shared_ptr<PlaybackContext> admittedPlayback_;
    std::thread worker_;
    bool stopping_ = false;
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> nextCommandId_{1};
    mutable std::atomic<std::uint64_t> nextEventUid_{1};

    mutable std::mutex snapshotMutex_;
    std::shared_ptr<const HistorySnapshot> snapshot_ =
        std::make_shared<HistorySnapshot>();

    mutable std::mutex userStateMutex_;
    std::shared_ptr<const TrackUserStateSnapshot> userStateSnapshot_ =
        std::make_shared<TrackUserStateSnapshot>();

    mutable std::mutex personaMutex_;
    std::shared_ptr<const PersonaSnapshot> personaSnapshot_ =
        std::make_shared<PersonaSnapshot>();
};

} // namespace nekorox::component
