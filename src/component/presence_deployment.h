#pragma once

#include <string_view>

namespace nekorox::component::presence_deployment {

// Filled by the release deployment step after the Discord application and
// Cloudflare Worker have been created. Empty values deliberately keep the
// current compatible IPC/application and legacy cover path operational.
inline constexpr std::string_view applicationId{"1538647960269627472"};
inline constexpr std::string_view relayEndpoint{}; // Private endpoints live only in local profiles.
inline constexpr std::string_view fallbackAsset{"goden_default"};
inline constexpr std::string_view playingAsset{"goden_playing"};
inline constexpr std::string_view dawAsset{"goden_daw"};

struct StaticAssets {
    std::string_view fallback;
    std::string_view playing;
    std::string_view daw;
    bool declared = false;
};

[[nodiscard]] constexpr StaticAssets assetsForApplication(
    std::string_view candidateApplicationId) noexcept
{
    if (!applicationId.empty() && candidateApplicationId == applicationId) {
        return {fallbackAsset, playingAsset, dawAsset, true};
    }
    // A custom Discord application owns an independent asset namespace.
    // Never send GODENplayer keys until that application has its own manifest.
    return {};
}

} // namespace nekorox::component::presence_deployment
