#pragma once

#include "c11/c11_change_inbox.h"
#include "sdk_compat.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nekorox::component {

struct LibraryTrack {
    metadb_handle_ptr handle;
    std::wstring genre;
    std::wstring artist;
    std::wstring albumArtist;
    std::vector<std::wstring> albumArtists;
    std::wstring album;
    std::wstring title;
    std::wstring year;
    std::wstring codec;
    std::wstring releaseId;
    std::wstring path;
    std::wstring searchFolded;
    int disc = 0;
    int track = 0;
    int rating = 0;
    int playCount = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    double durationSeconds = 0.0;
    // Immutable host metadata identities, not an additional copy of file tags.
    // Display-provider updates invalidate separately (ratings/play counts).
    metadb_info_container::ptr descriptionInfo;
    metadb_info_container::ptr descriptionBrowse;
};

struct LibrarySnapshot {
    std::uint64_t generation = 0;
    std::uint64_t layoutRevision = 0;
    std::shared_ptr<const std::vector<LibraryTrack>> tracks;
    std::unordered_map<std::string, std::size_t> handleIndex;
};

class LibraryModel final {
public:
    struct Diagnostics {
        std::uint64_t fullRebuilds = 0;
        std::uint64_t deltaRebuilds = 0;
        std::uint64_t describedTracks = 0;
        std::uint64_t publicationDeepCopyBytes = 0;
        std::uint64_t indexedNowPlayingHits = 0;
        std::uint64_t nowPlayingLinearScans = 0;
        std::uint64_t fullJobs = 0;
        std::uint64_t deltaJobs = 0;
        std::uint64_t coalescedHandles = 0;
        std::uint64_t sorts = 0;
        std::uint64_t indexBuiltOnWorker = 0;
        std::uint64_t publicationUs = 0;
        std::uint64_t vectorCloneCount = 0;
        std::uint64_t cloneRecords = 0;
        std::uint64_t requestsRejectedByLifetime = 0;
    };
    static LibraryModel& instance();

    void start();
    void stop();
    void invalidate();
    void invalidateModified(metadb_handle_list_cref handles);
    void invalidateTrack(const metadb_handle_ptr& handle);
    const std::vector<LibraryTrack>& tracks();
    [[nodiscard]] std::shared_ptr<const LibrarySnapshot> sharedSnapshot();
    LibraryTrack describeTrack(const metadb_handle_ptr& handle);
    std::uint64_t generation() const noexcept
    { return generation_.load(std::memory_order_acquire); }
    [[nodiscard]] Diagnostics diagnostics() const noexcept;
    // Expensive opt-in QA only; compares cached labels with current host fields.
    [[nodiscard]] std::int64_t diagnosticStaleDescriptions() noexcept;
    [[nodiscard]] std::uint64_t diagnosticTitleformatCalls() const noexcept;
    void recordIndexedNowPlayingHit() noexcept;
    void recordNowPlayingLinearScan() noexcept;

private:
    using ChangeInbox = goden::c11::ChangeInbox<
        std::string, metadb_handle_ptr>;
    struct ObserverBundle;
    struct RefreshRequest {
        std::uint64_t supersessionEpoch = 0;
        ChangeInbox::Job job;
        std::shared_ptr<const std::vector<titleformat_object::ptr>> scripts;
        std::shared_ptr<const LibrarySnapshot> base;
        std::unordered_set<std::string> forceDescriptions;
        bool forceAllDescriptions = false;
    };

    LibraryModel() = default;
    ~LibraryModel();
    void requestRefresh();
    void scheduleRefresh();
    void workerMain() noexcept;
    void compileScripts();
    void invalidateMembership();
    void scheduleChanged(metadb_handle_list_cref handles, bool forceDescription);
    std::wstring format(
        const metadb_handle_ptr& handle, std::size_t field) const;

    std::atomic<bool> dirty_{true};
    std::atomic<bool> refreshPending_{false};
    std::atomic<std::uint64_t> supersessionEpoch_{0};
    bool refreshScheduled_ = false; // main thread only
    std::uint64_t refreshLifetime_ = 0;
    std::atomic<std::uint64_t> generation_{0};
    std::shared_ptr<const std::vector<LibraryTrack>> tracksView_;
    std::atomic<std::shared_ptr<const LibrarySnapshot>> sharedSnapshot_;
    std::shared_ptr<const std::vector<titleformat_object::ptr>> scripts_;
    ChangeInbox changeInbox_;
    std::unordered_set<std::string> forceDescriptions_; // main thread only
    bool forceAllDescriptions_ = false;
    std::unique_ptr<ObserverBundle> observers_;
    std::thread worker_;
    std::mutex workerMutex_;
    std::condition_variable workerChanged_;
    std::optional<RefreshRequest> pendingRefresh_;
    bool stopping_ = false;
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> fullRebuilds_{0};
    std::atomic<std::uint64_t> deltaRebuilds_{0};
    std::atomic<std::uint64_t> describedTracks_{0};
    std::atomic<std::uint64_t> indexedNowPlayingHits_{0};
    std::atomic<std::uint64_t> nowPlayingLinearScans_{0};
    std::atomic<std::uint64_t> fullJobs_{0};
    std::atomic<std::uint64_t> deltaJobs_{0};
    std::atomic<std::uint64_t> coalescedHandles_{0};
    std::atomic<std::uint64_t> sorts_{0};
    std::atomic<std::uint64_t> indexBuiltOnWorker_{0};
    std::atomic<std::uint64_t> publicationUs_{0};
    std::atomic<std::uint64_t> publicationDeepCopyBytes_{0};
    std::atomic<std::uint64_t> vectorCloneCount_{0};
    std::atomic<std::uint64_t> cloneRecords_{0};
    std::atomic<std::uint64_t> requestsRejectedByLifetime_{0};
};

} // namespace nekorox::component
