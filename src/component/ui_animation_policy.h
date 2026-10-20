#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace nekorox::component {

// Pigment sampling is independent of display-paced audio and input.
inline constexpr double kThemeSampleHz = 30.0;
struct ThemeSampleStep {
    std::uint64_t bucket = 0;
    float progress = 1.0f;
    bool active = false;
};
[[nodiscard]] inline ThemeSampleStep themeSampleStep(
    double now, double start, double duration) noexcept
{
    if (!(duration > 0.0) || !std::isfinite(duration) ||
        !std::isfinite(now) || !std::isfinite(start)) return {};
    const auto elapsed = std::max(0.0, now - start);
    if (elapsed >= duration - 1.0e-9) return {
        static_cast<std::uint64_t>(std::ceil(duration * kThemeSampleHz)), 1.0f, false};
    const auto bucket = static_cast<std::uint64_t>(std::floor(elapsed * kThemeSampleHz));
    return {bucket, static_cast<float>((static_cast<double>(bucket) / kThemeSampleHz) / duration), true};
}
// Absolute phase keeps slower surfaces on the same samples as fast audio
// surfaces instead of drifting by a render duration on every frame.
[[nodiscard]] inline double nextThemeSampleSeconds(
    double now, double start, double duration) noexcept
{
    const auto step = themeSampleStep(now, start, duration);
    if (!step.active) return 0.0;
    const auto boundary = start + std::min(duration,
        static_cast<double>(step.bucket + 1U) / kThemeSampleHz);
    // Subtracting a large QPC origin can put an exact boundary in the prior
    // bucket. Wake just after it rather than requesting the same instant.
    return std::max(boundary,
        std::nextafter(now, std::numeric_limits<double>::infinity()));
}
[[nodiscard]] inline std::uint64_t cappedPresentationDeadline(
    std::uint64_t consumed, std::uint64_t frequency,
    std::uint32_t maximumMilliHz) noexcept
{
    if (!consumed || !maximumMilliHz) return 0;
    return consumed + (frequency * 1000U + maximumMilliHz - 1U) / maximumMilliHz;
}
[[nodiscard]] inline std::uint64_t displayPresentationDeadline(
    std::uint64_t consumed, std::uint64_t frequency,
    std::uint32_t maximumMilliHz, std::uint64_t displayPeriod) noexcept
{
    if (!consumed) return 0;
    return std::max(consumed + std::max<std::uint64_t>(1, displayPeriod),
        cappedPresentationDeadline(consumed, frequency, maximumMilliHz));
}
} // namespace nekorox::component
