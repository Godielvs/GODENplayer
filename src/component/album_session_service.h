#pragma once

#include "library_model.h"
#include "latest_worker.h"
#include "nekorox/album_session.h"

#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nekorox::component {

struct AlbumSessionTrack {
    metadb_handle_ptr handle;
    std::wstring title;
    std::wstring artist;
    std::string revisionTrackId;
    std::string entityId;
    int disc = 0;
    int track = 0;
    std::uint32_t subsong = 0;
};

enum class AlbumSessionState : std::uint8_t {
    empty,
    loading,
    ready,
    error,
};

enum class AlbumSessionSourceKind : std::uint8_t {
    none,
    library,
    temporary,
    external,
    remote,
};

struct AlbumSessionSnapshot {
    std::uint64_t requestId = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t revision = 0;
    std::uint64_t sourceRevision = 0;
    std::uint64_t sourceLayoutRevision = 0;
    AlbumSessionState state = AlbumSessionState::empty;
    AlbumSessionSourceKind sourceKind = AlbumSessionSourceKind::none;
    // Retained while resolution is pending so repeated activation of the
    // already selected album is idempotent instead of restarting Loading.
    metadb_handle_ptr anchor;
    std::wstring albumKey;
    std::wstring stableArtworkAnchor;
    std::wstring error;
    std::wstring album;
    std::wstring albumArtist;
    std::wstring year;
    std::vector<AlbumSessionTrack> tracks;
    std::size_t currentInSession = static_cast<std::size_t>(-1);
    std::optional<std::size_t> actualPlayingIndex;
    bool outsideSession = false;
    bool queueExternal = false;
    bool playlistLeaseValid = false;
    bool ownedPlaylistValid = false;
    bool transportOrderLease = false;
    bool compositionExternallyModified = false;
    std::uint64_t compositionRevision = 0;
    nekorox::AlbumSequencePolicy policy;
    std::vector<std::size_t> permutation;
};

class AlbumSessionService final {
public:
    static AlbumSessionService& instance();

    void start();
    void stop();
    [[nodiscard]] std::shared_ptr<const AlbumSessionSnapshot> snapshot() const;
    [[nodiscard]] bool enterForTrack(const metadb_handle_ptr& anchor);
    [[nodiscard]] bool enterFromHostContext();
    [[nodiscard]] bool retry();
    void toggleFromHostContext();
    void close();
    [[nodiscard]] bool playTrack(std::size_t index);
    [[nodiscard]] bool playPreviousLocal();
    [[nodiscard]] bool playNextLocal();
    void setRepeatMode(nekorox::AlbumRepeatMode mode);
    void setShuffle(bool enabled);
    [[nodiscard]] bool onPlaybackStop(play_control::t_stop_reason reason);
    void onNewTrack(const metadb_handle_ptr& track);
    // Main-thread reconciliation after the database learns the playing revision.
    void refreshPlayingIdentity();
    void cancelContinuation() noexcept { ++transportIntent_; }
    void onQueueChanged();
    void validatePlaylistLease();
    void revokePlaylistLease();
    void markOwnedPlaylistModified(std::size_t playlist);
    void onLibrarySnapshotPublished(
        std::shared_ptr<const LibrarySnapshot> library);

private:
    struct Observer;
    AlbumSessionService();
    ~AlbumSessionService();
    void publish(std::shared_ptr<AlbumSessionSnapshot> next);
    [[nodiscard]] std::size_t ownedPlaylist() const noexcept;
    struct ResolveRequest {
        std::uint64_t requestId = 0;
        std::uint64_t resolutionTicket = 0;
        metadb_handle_ptr anchor;
        std::shared_ptr<const LibrarySnapshot> library;
        metadb_handle_list temporaryItems;
        std::vector<LibraryTrack> temporaryTracks;
        std::uint64_t temporaryGeneration = 0;
        std::chrono::steady_clock::time_point libraryDeadline{};
        bool expectedLibrary = false;
        bool libraryDeadlineExpired = false;
    };
    void workerMain() noexcept;
    void enqueue(ResolveRequest request);
    void publishResolved(
        std::uint64_t requestId, std::uint64_t resolutionTicket,
        std::shared_ptr<AlbumSessionSnapshot> resolved);
    void adoptRevisionIds(
        const std::shared_ptr<const AlbumSessionSnapshot>& identitySource,
        std::uint64_t requestId, std::vector<std::string> ids);
    [[nodiscard]] bool ensurePreparedPlaylist(
        const AlbumSessionSnapshot& state,
        std::size_t& playlist);

    std::atomic<std::shared_ptr<const AlbumSessionSnapshot>> snapshot_;
    std::unique_ptr<Observer> observer_;
    std::thread worker_;
    std::mutex workerMutex_;
    std::condition_variable workerChanged_;
    std::optional<ResolveRequest> pendingRequest_;
    std::optional<ResolveRequest> waitingLibraryRequest_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> requestId_{0};
    std::atomic<std::uint64_t> resolutionTicket_{0};
    std::uint64_t transportIntent_ = 0; // main thread only
    std::unique_ptr<LatestWorker> identityWorker_ = std::make_unique<LatestWorker>();
    bool identityStopped_ = false;
    nekorox::AlbumPlaylistPreparationLease preparationLease_;
    bool preparingPlaylist_ = false;
};

} // namespace nekorox::component
