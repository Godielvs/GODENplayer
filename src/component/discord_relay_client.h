#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace nekorox::component {

class DiscordHttpIo;

enum class DiscordRelayStatus : std::uint8_t {
    disabled = 0,
    connecting,
    connected,
    degraded,
};

enum class DiscordRelayDelivery : std::uint8_t {
    none = 0,
    exact,
    fallback,
};

struct DiscordRelayCover {
    std::string assetId;
    std::int64_t expiresAtMilliseconds = 0;
    std::uint64_t connectionGeneration = 0;
    std::string url;
    std::string error;
    [[nodiscard]] bool succeeded() const noexcept { return !url.empty(); }
};

struct DiscordRelayTrace {
    DiscordRelayStatus status = DiscordRelayStatus::disabled;
    DiscordRelayDelivery delivery = DiscordRelayDelivery::none;
    std::uint64_t connectionGeneration = 0;
    std::uint64_t deliveryGeneration = 0;
    std::uint64_t deliveredAtMilliseconds = 0;
    std::uint32_t deliveryMilliseconds = 0;
    std::string assetId;
    std::string error;
};

// Persistent, installation-scoped client for the ephemeral cover relay.
// Images never leave this process until Discord's media proxy asks for the
// signed URL. The relay stores no image bytes in Durable Object storage.
class DiscordRelayClient final {
public:
    DiscordRelayClient();
    ~DiscordRelayClient();
    DiscordRelayClient(const DiscordRelayClient&) = delete;
    DiscordRelayClient& operator=(const DiscordRelayClient&) = delete;

    [[nodiscard]] static std::string configuredEndpoint();
    [[nodiscard]] static bool configured();
    [[nodiscard]] static bool validEndpoint(const std::string& endpoint);

    void setStateChangedCallback(std::function<void()> callback);
    void start();
    // UI-safe policy revocation. Only the worker closes its async handles.
    void disable() noexcept;
    [[nodiscard]] std::uint64_t admissionTicket() const noexcept;
    // Joins only during background cleanup or final service shutdown.
    void stop() noexcept;
    void clear() noexcept;
    DiscordRelayCover registerCover(
        std::vector<std::uint8_t> png,
        std::stop_token stopToken,
        std::uint64_t admissionTicket);
    [[nodiscard]] DiscordRelayTrace trace() const;

private:
    struct Asset {
        std::vector<std::uint8_t> png;
        std::chrono::steady_clock::time_point inserted{};
    };

    using AsyncIo = DiscordHttpIo;
    void launchWorkerLocked();
    void revokeLocked() noexcept;
    [[nodiscard]] bool admittedLocked(std::uint64_t ticket) const noexcept;
    void workerMain(std::stop_token stopToken);
    bool createSession(std::stop_token stopToken, std::uint64_t ticket,
        const std::string& endpoint);
    bool connectSocket(std::stop_token stopToken, std::uint64_t ticket);
    void receiveSocket(AsyncIo& socket, std::stop_token stopToken,
        std::uint64_t ticket);
    void setError(std::uint64_t ticket, std::string value);

    std::function<void()> stateChanged_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::map<std::string, Asset> assets_;
    std::jthread worker_;
    std::stop_source connectionStop_;
    std::uint64_t admissionGeneration_ = 0;
    std::string endpoint_;
    std::string sessionId_;
    std::string secret_;
    std::string socketUrl_;
    std::string coverBaseUrl_;
    std::int64_t expiresAtMilliseconds_ = 0;
    DiscordRelayTrace trace_{};
    bool started_ = false;
    bool workerFinished_ = true;
    bool syncDraining_ = false;
};

} // namespace nekorox::component
