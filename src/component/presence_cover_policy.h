#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace nekorox::component {

enum class PresenceCoverProvider : std::uint32_t {
    unconfigured = 0,
    catbox = 1,
    privateRelay = 2,
};

struct PresenceCoverConfiguration {
    PresenceCoverProvider provider = PresenceCoverProvider::unconfigured;
    bool disclosureAccepted = false;
    std::string relayEndpoint;
    // Decrypted only in the current process. Persistence uses Windows DPAPI.
    std::string relayCredential;

    [[nodiscard]] bool allowsCovers() const noexcept
    {
        if (!disclosureAccepted) return false;
        return provider == PresenceCoverProvider::catbox ||
            (provider == PresenceCoverProvider::privateRelay &&
                !relayEndpoint.empty() && !relayCredential.empty());
    }
};

inline std::atomic<std::shared_ptr<const PresenceCoverConfiguration>>
    presenceCoverConfigurationState{std::make_shared<const PresenceCoverConfiguration>()};

[[nodiscard]] inline auto presenceCoverConfiguration() noexcept
{
    return presenceCoverConfigurationState.load(std::memory_order_acquire);
}

inline void publishPresenceCoverConfiguration(PresenceCoverConfiguration value)
{
    presenceCoverConfigurationState.store(
        std::make_shared<const PresenceCoverConfiguration>(std::move(value)),
        std::memory_order_release);
}

} // namespace nekorox::component
