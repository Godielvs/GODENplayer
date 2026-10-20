#pragma once

#include <algorithm>
#include <cmath>

namespace nekorox {

// Post-DSP PCM is allowed to exceed full scale.  Displays whose geometry is
// physically bounded still need a deterministic projection, otherwise a large
// EQ boost pins the trace outside its grid.  This is deliberately a visual
// transfer only: normal samples stay untouched and no audio is written back.
inline float boundedVisualSample(float sample) noexcept
{
    if (!std::isfinite(sample)) return 0.0f;
    constexpr float knee = 0.92f;
    constexpr float range = 1.0f - knee;
    const auto magnitude = std::abs(sample);
    if (magnitude <= knee) return sample;
    const auto excess = magnitude - knee;
    const auto mapped = knee + excess / (1.0f + excess / range);
    return std::copysign(std::min(mapped, 0.9995f), sample);
}

} // namespace nekorox
