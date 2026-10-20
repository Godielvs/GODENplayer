#pragma once

#include <algorithm>
#include <cmath>

namespace nekorox {

// Equal-power audition crossfade with a sample-domain ceiling. The ceiling
// prevents correlated tails from creating a transient above both inputs.
template <typename Sample>
[[nodiscard]] inline Sample scrubCrossfadeSample(
    Sample previous,
    Sample next,
    double phase) noexcept
{
    phase = std::clamp(phase, 0.0, 1.0);
    constexpr double halfPi = 1.57079632679489661923;
    const auto previousGain = static_cast<Sample>(std::cos(phase * halfPi));
    const auto nextGain = static_cast<Sample>(std::sin(phase * halfPi));
    const auto ceiling = std::max(std::abs(previous), std::abs(next));
    return std::clamp(
        previous * previousGain + next * nextGain,
        -ceiling,
        ceiling);
}

} // namespace nekorox
