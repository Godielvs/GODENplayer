#pragma once

#include "art_cache.h"
#include "presence_cover_policy.h"
#include <array>
#include "discord_relay_client.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace nekorox::component {

enum class DiscordCoverFailure : std::uint8_t {
    none = 0, upload, origin, delivery, noArtwork, recoveryLimit,
};

struct DiscordCoverMetadata {
    std::string artist, album, releaseId;
};

struct DiscordCoverResult {
    enum class Source : std::uint8_t {
        none = 0,
        cache,
        upload,
        relay,
        catalogue,
    };

    std::uint64_t contentHash = 0;
    std::size_t encodedBytes = 0;
    std::uint32_t artworkMilliseconds = 0;
    std::uint32_t encodeMilliseconds = 0;
    std::uint32_t uploadMilliseconds = 0;
    std::uint32_t verificationMilliseconds = 0;
    Source source = Source::none;
    PresenceCoverProvider provider = PresenceCoverProvider::unconfigured;
    std::int64_t expiresAtMilliseconds = 0;
    std::uint64_t connectionGeneration = 0;
    std::string url;
    std::string token;
    std::string error;
    DiscordCoverFailure failure = DiscordCoverFailure::none;
    // A confirmed missing/broken origin has a bounded propagation grace.
    // This hint schedules a later GET; it does not authorize another POST.
    std::uint32_t retryAfterMilliseconds = 0;
    // Internal worker handoff: a confirmed broken origin needs different PNG
    // bytes to avoid Catbox returning that same deduplicated object.
    bool retryWithNewEncoding = false;

    [[nodiscard]] bool succeeded() const noexcept { return !url.empty(); }
};

// One instance is owned by the Presence cover worker. It resolves the exact
// local art already used by Goden, creates a metadata-free square PNG and
// uploads it only when Discord Presence has explicitly been enabled.
class DiscordCoverService final {
public:
    DiscordCoverService();
    void setRelayStateChangedCallback(std::function<void()> callback) {
        relay_.setStateChangedCallback(std::move(callback));
    }
    void startRelay();
    void disableRelay() noexcept;
    [[nodiscard]] std::uint64_t admissionTicket() const noexcept {
        return cancellationGeneration_.load(std::memory_order_acquire);
    }
    DiscordCoverResult resolve(
        const metadb_handle_ptr& track,
        std::uint64_t trackKey,
        std::stop_token stopToken,
        std::optional<std::uint64_t> admission = std::nullopt,
        const DiscordCoverMetadata& metadata = {});
    void requestVerification() noexcept { forceVerification_.store(true); }
    void cancel() noexcept;
    void clear() noexcept;
    [[nodiscard]] DiscordRelayTrace relayTrace() const;

private:
    struct CachedUrl {
        std::string url;
        std::chrono::system_clock::time_point expires{};
        std::chrono::system_clock::time_point lastUsed{};
        // 0 is an older cache entry with no encoding record. Subsequent
        // encodings advance monotonically; never cycle through broken bytes.
        unsigned pngEncoding = 0;
        // SHA-256 of the exact delivery URL and complete validated PNG. Only
        // created after origin verification; an upload acknowledgement is not proof.
        std::string deliveryProof;
    };

    struct ProviderCache {
        std::map<std::uint64_t, CachedUrl> entries;
        std::set<std::uint64_t> verified;
        std::filesystem::path path;
        bool loaded = false;
    };
    // Only the cover worker swaps these contexts. UI cancellation touches
    // atomics/stop sources, never these maps or the active provider.
    void exchangeCache(ProviderCache& context) noexcept;
    DiscordCoverResult resolveForProvider(const metadb_handle_ptr& track,
        std::uint64_t trackKey, std::stop_token stopToken, std::uint64_t admission,
        PresenceCoverProvider provider, bool cacheOnly);
    DiscordCoverResult resolveCatalogue(const DiscordCoverMetadata& metadata,
        std::stop_token stopToken, std::uint64_t admission);

    struct PreparedCover {
        std::uint64_t trackKey = 0;
        std::uint64_t contentHash = 0;
        std::vector<std::uint8_t> png;
        std::string rejectedOrigin;
        unsigned pngEncoding = 1;
    };

    DiscordCoverResult upload(
        std::uint64_t contentHash,
        const std::vector<std::uint8_t>& png,
        std::stop_token stopToken,
        std::uint64_t cancellationGeneration,
        unsigned pngEncoding = 1);
    DiscordCoverResult publishPrepared(
        std::uint64_t contentHash,
        const std::vector<std::uint8_t>& png,
        std::stop_token stopToken,
        std::uint64_t cancellationGeneration,
        unsigned pngEncoding = 1);
    [[nodiscard]] bool verifyRemotePng(
        const std::string& url,
        bool qaOverride,
        std::stop_token stopToken,
        std::uint64_t cancellationGeneration,
        unsigned* httpStatus = nullptr,
        bool* brokenImage = nullptr,
        std::chrono::steady_clock::time_point totalDeadline = std::chrono::steady_clock::time_point::max(),
        std::string* deliveryProof = nullptr, std::string* jsonBody = nullptr);
    [[nodiscard]] bool verifyRemotePngOnce(
        const std::string& url, bool qaOverride, std::stop_token stopToken,
        std::uint64_t cancellationGeneration, unsigned* httpStatus,
        bool* transportFailed,
        bool* brokenImage, std::chrono::steady_clock::time_point totalDeadline,
        std::string* deliveryProof, std::string* jsonBody);
    enum class CachedCoverState { unavailable, readable, repairPending, replaceOrigin };
    [[nodiscard]] CachedCoverState checkCachedCover(
        CachedUrl& cached, const std::string& delivery, bool qaOverride,
        std::stop_token stopToken, std::uint64_t cancellationGeneration,
        std::uint32_t* retryAfterMilliseconds = nullptr);
    [[nodiscard]] bool cancelled(
        std::stop_token stopToken,
        std::uint64_t cancellationGeneration) const noexcept;
    void pruneCache();
    void loadCache();
    void saveCache() const;

    std::map<std::uint64_t, CachedUrl> cache_;
    std::set<std::uint64_t> verifiedDelivery_;
    std::filesystem::path cachePath_;
    bool cacheLoaded_ = false;
    bool preferAutomaticProxy_ = false;
    PreparedCover prepared_{};
    std::array<ProviderCache, 2> providerCaches_{};
    PresenceCoverProvider activeProvider_ = PresenceCoverProvider::catbox;
    bool cacheOnly_ = false;
    std::atomic<bool> forceVerification_{false};
    std::map<PresenceCoverProvider, std::chrono::steady_clock::time_point> hostRetryAt_;
    struct CatalogueEntry { std::string url; std::chrono::system_clock::time_point expires; };
    std::map<std::string, CatalogueEntry> catalogueCache_;
    std::chrono::steady_clock::time_point lastCatalogueQuery_{};
    std::atomic<std::uint64_t> cancellationGeneration_{0};
    std::mutex artworkAbortMutex_;
    std::shared_ptr<abort_callback_impl> activeArtworkAbort_;
    // Protected with artworkAbortMutex_; cancellation never touches handles.
    std::stop_source activeNetworkStop_{std::nostopstate};
    DiscordRelayClient relay_;
};

} // namespace nekorox::component
