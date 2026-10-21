#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nekorox::component {

enum class PresenceCoverProvider : std::uint32_t {
    unconfigured = 0,
    catbox = 1,
    privateRelay = 2,
    uguu = 3,
    imgbb = 4,
    catalogue = 5,
    automatic = 6,
};

constexpr std::uint32_t coverProviderBit(PresenceCoverProvider provider) noexcept {
    return static_cast<unsigned>(provider) < 32U ? 1U << static_cast<unsigned>(provider) : 0U;
}
constexpr auto kPublicCoverProviders = coverProviderBit(PresenceCoverProvider::catbox) |
    coverProviderBit(PresenceCoverProvider::uguu) | coverProviderBit(PresenceCoverProvider::imgbb) |
    coverProviderBit(PresenceCoverProvider::catalogue);

constexpr auto kRecommendedCoverProviders = coverProviderBit(PresenceCoverProvider::uguu) |
    coverProviderBit(PresenceCoverProvider::catbox) | coverProviderBit(PresenceCoverProvider::catalogue);
constexpr std::array kAutomaticCoverOrder{PresenceCoverProvider::uguu,
    PresenceCoverProvider::catbox, PresenceCoverProvider::imgbb, PresenceCoverProvider::catalogue};

struct InstallerCoverChoice {
    PresenceCoverProvider provider;
    std::uint32_t automaticProviders;
    bool accepted;
};

// New installer consent is versioned. Legacy markers keep their narrower scope.
inline std::optional<InstallerCoverChoice> parseInstallerCoverChoice(std::string_view choice) {
    if (choice == "automatic-v1-accepted" || choice == "automatic-v1-disabled")
        return InstallerCoverChoice{PresenceCoverProvider::automatic, kRecommendedCoverProviders,
            choice == "automatic-v1-accepted"};
    if (choice == "catbox-accepted" || choice == "catbox-disabled")
        return InstallerCoverChoice{PresenceCoverProvider::catbox, coverProviderBit(PresenceCoverProvider::catbox),
            choice == "catbox-accepted"};
    return std::nullopt;
}

struct PresenceCoverConfiguration {
    PresenceCoverProvider provider = PresenceCoverProvider::unconfigured;
    bool disclosureAccepted = false;
    std::string relayEndpoint;
    // Decrypted only in the current process. Persistence uses Windows DPAPI.
    std::string relayCredential;
    // Existing Catbox consent never enables another host. This mask is used
    // only after the user explicitly chooses/configures Automatic.
    std::uint32_t automaticProviders = coverProviderBit(PresenceCoverProvider::catbox);
    std::string imgbbKey;

    [[nodiscard]] bool allowsProvider(PresenceCoverProvider candidate) const noexcept {
        if (!disclosureAccepted) return false;
        if (provider != candidate && (provider != PresenceCoverProvider::automatic ||
                !(automaticProviders & coverProviderBit(candidate)))) return false;
        if (candidate == PresenceCoverProvider::privateRelay)
            return provider == candidate && !relayEndpoint.empty() && !relayCredential.empty();
        if (candidate == PresenceCoverProvider::imgbb) return !imgbbKey.empty();
        return candidate == PresenceCoverProvider::catbox || candidate == PresenceCoverProvider::uguu ||
            candidate == PresenceCoverProvider::catalogue;
    }

    [[nodiscard]] bool allowsCovers() const noexcept
    {
        if (provider != PresenceCoverProvider::automatic) return allowsProvider(provider);
        return allowsProvider(PresenceCoverProvider::catbox) || allowsProvider(PresenceCoverProvider::uguu) ||
            allowsProvider(PresenceCoverProvider::imgbb) || allowsProvider(PresenceCoverProvider::catalogue);
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
