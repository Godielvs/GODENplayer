#pragma once

#include "latest_worker.h"
#include "lrclib_provider.h"
#include "lyrics_local_provider.h"

#include "nekorox/lyrics_document.h"

#include "sdk_compat.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace nekorox::component {

class LyricsService final {
public:
    static LyricsService& instance();

    [[nodiscard]] bool start();
    void stop() noexcept;
    void shutdownAndDrain() noexcept { stop(); }
    void onNewTrack(const metadb_handle_ptr& track);
    void onMetadataChanged(const metadb_handle_ptr& track = {});
    void onStop() noexcept;

    [[nodiscard]] std::shared_ptr<const LyricsSnapshot> snapshot() const noexcept;
    void subscribe(HWND window, UINT message);
    void unsubscribe(HWND window) noexcept;

    [[nodiscard]] bool onlineEnabled() const noexcept;
    void setOnlineEnabled(bool enabled);
    [[nodiscard]] bool offlineMode() const noexcept;
    void setOfflineMode(bool offline);
    [[nodiscard]] std::wstring statusText() const;
    [[nodiscard]] bool markCurrentWrongLyrics();
    [[nodiscard]] bool showCurrentWithoutSync();
    void retryCurrent();

private:
    LyricsService();
    ~LyricsService();
    LyricsService(const LyricsService&) = delete;
    LyricsService& operator=(const LyricsService&) = delete;

    struct Subscriber { HWND window = nullptr; UINT message = 0; };
    struct NegativeEntry {
        LyricsState state = LyricsState::noReliableMatch;
        std::chrono::steady_clock::time_point expires{};
        unsigned validMisses = 0;
    };

    [[nodiscard]] LyricsRequestContext capture(
        const metadb_handle_ptr& track,
        std::uint64_t generation) const;
    void beginRequest(const metadb_handle_ptr& track,
        LyricsRequestReason reason);
    [[nodiscard]] bool isCurrent(const RequestToken& request) const noexcept;
    [[nodiscard]] bool promoteResolvedRequest(
        const LyricsRequestContext& request);
    void resolveMediaRevision(LyricsRequestContext request,
        metadb_info_container::ptr info, std::stop_token stop);
    [[nodiscard]] LyricsProviderResult lookupLocal(
        const LyricsRequestContext& request,
        metadb_info_container::ptr info, std::stop_token stop);
    void runRequest(LyricsRequestContext request,
        metadb_info_container::ptr info, std::stop_token stop);
    void publish(LyricsSnapshot next);
    void publishAdmission(const LyricsRequestContext& request,
        LyricsState state);
    void publishState(const LyricsRequestContext& request, LyricsState state,
        std::shared_ptr<const LyricsDocumentV2> document = {},
        bool staleCache = false,
        LyricsSourceRank sourceRank = LyricsSourceRank::none,
        std::uint64_t sourceRevision = 0);
    [[nodiscard]] std::shared_ptr<const LyricsDocumentV2> resolve(
        const LyricsRequestContext& request,
        const std::vector<LyricsCandidateV2>& candidates) const;
    void requestCached(const LyricsRequestContext& request);
    void persist(const LyricsRequestContext& request,
        const LyricsCandidateV2& candidate,
        const LyricsDocumentV2& document);
    void refreshCurrent();
    [[nodiscard]] std::string rememberedRejection(
        const LyricsRequestContext& request,
        std::string_view documentId) const;
    void rememberRejection(
        const LyricsRequestContext& request,
        std::string documentId,
        std::string reason);
    void persistRejection(
        const LyricsSnapshot& snapshot,
        std::string reason);

    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::uint64_t> presentationRevision_{0};
    std::atomic<std::uint64_t> cacheSatisfiedGeneration_{0};
    std::atomic<std::shared_ptr<const LyricsSnapshot>> snapshot_;
    LatestWorker worker_;
    LyricsLocalProvider local_;
    LrclibProvider remote_;
    mutable std::mutex subscriberMutex_;
    std::vector<Subscriber> subscribers_;
    mutable std::mutex negativeMutex_;
    std::map<std::string, NegativeEntry> negative_;
    mutable std::mutex rejectionMutex_;
    std::map<std::string, std::string> rejections_;
};

} // namespace nekorox::component
