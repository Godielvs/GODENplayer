#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace nekorox::component {

inline constexpr std::size_t kArtworkCpuBudgetBytes =
    128U * 1024U * 1024U;
inline constexpr std::size_t kArtworkTemporaryBudgetBytes =
    64U * 1024U * 1024U;
inline constexpr std::size_t kArtworkRendererGpuBudgetBytes =
    96U * 1024U * 1024U;
inline constexpr std::size_t kArtworkMaximumPendingRequests = 16U;
inline constexpr std::size_t kArtworkMaximumEntries = 256U;

[[nodiscard]] constexpr bool artworkBudgetCanAdd(
    std::size_t current,
    std::size_t additional,
    std::size_t limit) noexcept
{
    return additional <= limit && current <= limit - additional;
}

[[nodiscard]] constexpr bool artworkPriorityDisplaces(
    std::uint8_t queuedWorst,
    std::uint8_t incoming) noexcept
{
    return incoming < queuedWorst;
}

[[nodiscard]] constexpr bool artworkDecodeFitsBudget(std::size_t encoded,
    std::size_t decoded, std::size_t palette) noexcept
{
    auto remaining=kArtworkTemporaryBudgetBytes;
    const std::size_t stages[]{encoded,decoded,decoded,palette};
    for(const auto bytes:stages){
        if(bytes>remaining)return false;
        remaining-=bytes;
    }
    return true;
}

[[nodiscard]] constexpr std::size_t artworkBgraBytes(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (width == 0 || height == 0 ||
        static_cast<std::size_t>(width) > maximum / 4U / height) {
        return 0;
    }
    return static_cast<std::size_t>(width) * height * 4U;
}

} // namespace nekorox::component
