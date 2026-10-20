#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace nekorox {

// Presentation cadence is deliberately separate from visual analysis. These
// helpers preserve fractional display modes (for example 59.94/119.88 Hz) and
// never let pressure mode defeat a surface-specific lower refresh cap.
[[nodiscard]] constexpr std::uint64_t presentationPeriodTicks(
    std::uint64_t qpcFrequency,
    std::uint32_t refreshNumerator,
    std::uint32_t refreshDenominator) noexcept
{
    const auto frequency = (std::max)(std::uint64_t{1}, qpcFrequency);
    const auto numerator = (std::max)(std::uint32_t{1}, refreshNumerator);
    const auto denominator = (std::max)(std::uint32_t{1}, refreshDenominator);
    const auto value = static_cast<long double>(frequency) * denominator /
        static_cast<long double>(numerator);
    return (std::max)(std::uint64_t{1},
        static_cast<std::uint64_t>(value + 0.5L));
}

[[nodiscard]] constexpr std::uint64_t presentationPeriodTicksFromMilliHz(
    std::uint64_t qpcFrequency,
    std::uint32_t refreshMilliHz) noexcept
{
    return presentationPeriodTicks(
        qpcFrequency,
        (std::max)(std::uint32_t{1}, refreshMilliHz),
        1000U);
}

[[nodiscard]] constexpr std::uint32_t pressureRefreshMilliHz(
    std::uint32_t nativeRefreshMilliHz) noexcept
{
    const auto native = (std::max)(std::uint32_t{1}, nativeRefreshMilliHz);
    // Environmental surfaces may fall to half refresh, but never below 60 Hz
    // and never above the native rate of a 59.94 Hz display.
    return (std::min)(native, (std::max)(60000U, native / 2U));
}

[[nodiscard]] constexpr std::uint64_t pressurePresentationPeriodTicks(
    std::uint64_t nominalPeriodTicks,
    std::uint64_t qpcFrequency,
    std::uint32_t nativeRefreshMilliHz) noexcept
{
    return (std::max)(
        (std::max)(std::uint64_t{1}, nominalPeriodTicks),
        presentationPeriodTicksFromMilliHz(
            qpcFrequency,
            pressureRefreshMilliHz(nativeRefreshMilliHz)));
}

} // namespace nekorox
