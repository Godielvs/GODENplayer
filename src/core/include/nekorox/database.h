#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

struct sqlite3;

namespace nekorox {

inline constexpr int kDatabaseSchemaVersion = 6;
inline constexpr std::uint64_t kAnalysisCacheLimitBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;

enum class PlayEventType : std::int32_t {
    start = 1,
    pause = 2,
    resume = 3,
    seek = 4,
    skip = 5,
    finish = 6,
    stop = 7,
};

const char* playEventTypeName(PlayEventType type) noexcept;

struct PersistedTrack {
    std::string trackId;
    std::string path;
    std::string partialFingerprint;
    std::string artist;
    std::string album;
    std::string title;
    std::uint32_t subsong = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t modifiedTicks = 0;
    std::int64_t durationMs = 0;
    std::int64_t lastSeenUtcMs = 0;
};

struct PersistedPlayEvent {
    std::string sessionId;
    std::string trackId;
    PlayEventType type = PlayEventType::start;
    std::int64_t utcMs = 0;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::int32_t playlistIndex = -1;
    std::int32_t itemIndex = -1;
    std::string eventUid;
};

struct HistoryEventRecord {
    std::int64_t id = 0;
    std::string sessionId;
    std::string trackId;
    PlayEventType type = PlayEventType::start;
    std::int64_t utcMs = 0;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::string artist;
    std::string album;
    std::string title;
    std::string eventUid;
    std::string path;
    std::uint32_t subsong = 0;
};

struct PersistedTrackUserState {
    bool found = false;
    bool stateAvailable = true;
    std::string entityId;
    std::string canonicalTrackId;
    std::string currentRevisionTrackId;
    // Compatibility alias for currentRevisionTrackId.
    std::string trackId;
    bool favorite = false;
    std::string note;
    std::int64_t favoriteUpdatedUtcMs = 0;
    std::int64_t noteUpdatedUtcMs = 0;
};

struct TrackIdentityResolution {
    bool found = false;
    std::string entityId;
    std::string canonicalTrackId;
    std::string revisionTrackId;
};

struct KnownRevisionBinding {
    PersistedTrack revision;
    std::string entityId;
};

struct PersistedPersonaAlias {
    std::string display;
    std::string key;
};

struct PersistedPersonaRelationship {
    std::string targetPersonaId;
    std::string targetDisplayName;
    std::string targetAliasKey;
    std::string relation;
    std::string note;
};

struct PersistedPersona {
    bool found = false;
    std::string personaId;
    std::string displayName;
    std::string profile;
    std::vector<PersistedPersonaAlias> aliases;
    std::vector<PersistedPersonaRelationship> relationships;
    std::int64_t updatedUtcMs = 0;
};

struct PersistedArtAsset {
    std::string assetId;
    std::string personaId;
    std::string role;
    std::string path;
    std::string contentHash;
    bool selectedByUser = false;
    std::int64_t updatedUtcMs = 0;
};

struct PersistedAnalysis {
    bool found = false;
    std::string trackId;
    std::string analyzerVersion;
    std::string state = "complete";
    std::string manifestJson = "{}";
    std::vector<std::byte> resultJson;
    std::vector<std::byte> waveform100ms;
    std::int64_t updatedUtcMs = 0;
    std::int64_t lastAccessUtcMs = 0;
};

struct PersistedAnalysisLayer {
    std::string name;
    std::int64_t resolutionMs = 0;
    std::vector<std::byte> data;
};

struct PersistedAnalysisBundle {
    bool found = false;
    std::string trackId;
    std::string analyzerVersion;
    std::string state = "complete";
    std::string manifestJson = "{}";
    std::vector<PersistedAnalysisLayer> layers;
    std::int64_t updatedUtcMs = 0;
    std::int64_t lastAccessUtcMs = 0;
};

struct PersistedLyricsDocumentV2 {
    bool found = false;
    std::string documentId;
    std::string provider;
    std::string providerRecordId;
    std::string providerRevision;
    std::string title;
    std::string artist;
    std::string album;
    std::int64_t durationMs = 0;
    std::string plainTextUtf8;
    std::string syncedTextUtf8;
    bool instrumental = false;
    std::string textHash;
    std::string timingHash;
    std::int32_t timingStatus = 0;
    std::string reasonCodes;
    std::string sourceDisplayName;
    std::string attribution;
    std::string sourceUrl;
    std::int64_t fetchedUtcMs = 0;
    std::int64_t expiryUtcMs = 0;
    std::uint32_t policyVersion = 0;
};

struct PersistedLyricsBindingV2 {
    std::string entityId;
    std::uint32_t subsong = 0;
    std::string mediaRevision;
    std::uint64_t metadataRevision = 0;
    std::string documentId;
    std::int32_t state = 0;
    std::int32_t timingStatus = 0;
    std::string reasonCodes;
    std::int64_t updatedUtcMs = 0;
};

struct PersistedLyricsRejectionV2 {
    bool found = false;
    std::string entityId;
    std::uint32_t subsong = 0;
    std::string mediaRevision;
    std::string documentId;
    std::string textHash;
    std::string timingHash;
    std::string reason;
    std::int64_t rejectedUtcMs = 0;
};

// Own one instance from one thread. The component wraps this class in its
// background writer so foobar callbacks never execute SQLite work.
class NekoDatabase final {
public:
    NekoDatabase() = default;
    ~NekoDatabase();

    NekoDatabase(const NekoDatabase&) = delete;
    NekoDatabase& operator=(const NekoDatabase&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);
    void close() noexcept;
    bool isOpen() const noexcept { return db_ != nullptr; }
    int schemaVersion() const noexcept { return schemaVersion_; }

    bool beginSession(
        const std::string& sessionId,
        std::int64_t startedUtcMs,
        const std::string& appVersion,
        std::string& error);
    bool endSession(
        const std::string& sessionId,
        std::int64_t endedUtcMs,
        std::string& error);
    bool recordPlayEvent(
        const PersistedTrack& track,
        const PersistedPlayEvent& event,
        std::string& error,
        bool* inserted = nullptr);
    std::vector<HistoryEventRecord> readSessionEvents(
        const std::string& sessionId,
        std::size_t limit,
        std::string& error);
    bool setFavorite(
        const PersistedTrack& track,
        bool favorite,
        std::int64_t updatedUtcMs,
        std::string& error);
    bool setNote(
        const PersistedTrack& track,
        const std::string& note,
        std::int64_t updatedUtcMs,
        std::string& error);
    bool readTrackUserState(
        const std::string& trackId,
        PersistedTrackUserState& state,
        std::string& error);
    bool resolveTrackIdentity(
        const std::string& revisionTrackId,
        TrackIdentityResolution& identity,
        std::string& error);
    bool bindKnownRevision(
        const PersistedTrack& revision,
        const std::string& entityId,
        std::string& error);
    // Resolve predecessor identities before submitting the batch. All bindings
    // and user-state reconciliation commit together, or none do.
    bool bindKnownRevisions(
        std::span<const KnownRevisionBinding> bindings,
        std::string& error);
    bool upsertPersona(
        const PersistedPersona& persona,
        std::string& error);
    bool readPersonaByAliasKey(
        const std::string& aliasKey,
        PersistedPersona& persona,
        std::string& error);
    bool readPersonaById(
        const std::string& personaId,
        PersistedPersona& persona,
        std::string& error);
    bool upsertArtAsset(
        const PersistedArtAsset& asset,
        std::string& error);
    bool deleteUnselectedArtAssetsWithPathPrefix(
        const std::string& pathPrefix,
        std::string& error);
    bool readArtAssetsForPersona(
        const std::string& personaId,
        std::vector<PersistedArtAsset>& assets,
        std::string& error);
    bool setSetting(
        const std::string& key,
        const std::string& value,
        std::int64_t updatedUtcMs,
        std::string& error);
    bool readSetting(
        const std::string& key,
        std::string& value,
        bool& found,
        std::string& error);
    bool storeAnalysis(
        const PersistedTrack& track,
        const PersistedAnalysis& analysis,
        std::uint64_t cacheLimitBytes,
        std::string& error);
    bool readAnalysis(
        const std::string& trackId,
        const std::string& analyzerVersion,
        PersistedAnalysis& analysis,
        std::string& error);
    bool storeAnalysisBundle(
        const PersistedTrack& track,
        const PersistedAnalysisBundle& bundle,
        std::uint64_t cacheLimitBytes,
        std::string& error);
    bool readAnalysisBundle(
        const std::string& trackId,
        const std::string& analyzerVersion,
        PersistedAnalysisBundle& bundle,
        std::string& error);
    bool removeAnalysisBundle(
        const std::string& trackId,
        const std::string& analyzerVersion,
        std::string& error);
    bool storeLyricsDocumentAndBinding(
        const PersistedLyricsDocumentV2& document,
        const PersistedLyricsBindingV2& binding,
        std::string& error);
    bool readLyricsForBinding(
        const std::string& entityId,
        std::uint32_t subsong,
        const std::string& mediaRevision,
        std::uint64_t metadataRevision,
        PersistedLyricsDocumentV2& document,
        PersistedLyricsBindingV2& binding,
        std::string& error);
    bool storeLyricsRejection(
        const PersistedLyricsRejectionV2& rejection,
        std::string& error);
    bool readLyricsRejection(
        const std::string& entityId,
        std::uint32_t subsong,
        const std::string& mediaRevision,
        const std::string& documentId,
        PersistedLyricsRejectionV2& rejection,
        std::string& error);
    std::uint64_t analysisCacheBytes(std::string& error);
    bool pruneAnalysisCache(
        std::uint64_t cacheLimitBytes,
        const std::string& protectedTrackId,
        const std::string& protectedAnalyzerVersion,
        std::string& error);

private:
    bool bindKnownRevisionInTransaction(
        const PersistedTrack& revision,
        const std::string& entityId,
        std::string& error);
    bool migrate(std::string& error);
    bool execute(const char* sql, std::string& error);

    sqlite3* db_ = nullptr;
    int schemaVersion_ = 0;
    std::filesystem::path path_;
};

} // namespace nekorox
