#include "../src/component/scrub_preview_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
void requireAudition(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

void runWorkpackAuditionTests()
{
    constexpr std::uint32_t rate = 48000;
    constexpr std::size_t frames = 8640;
    float maximum = 0.0f;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto gain = nekorox::component::scrubPreviewEnvelope(
            frame, frames, rate);
        requireAudition(std::isfinite(gain) && gain >= 0.0f &&
                gain <= 0.501188f,
            "WP17 preview envelope exceeded its -6 dB bound");
        maximum = std::max(maximum, gain);
        const std::array<float, 2> antiPhase{gain, -gain};
        requireAudition(std::abs(antiPhase[0] + antiPhase[1]) < 1e-7f,
            "WP17 envelope generated energy in an anti-phase fixture");
    }
    requireAudition(nekorox::component::scrubPreviewEnvelope(
            frames, frames, rate) == 0.0f,
        "WP17 envelope leaked past the grain boundary");
    requireAudition(maximum > 0.50118f &&
            nekorox::component::scrubPreviewEnvelope(0, frames, rate) < 0.00001f &&
            nekorox::component::scrubPreviewEnvelope(frames - 1, frames, rate) <
                0.00001f,
        "WP17 envelope endpoints or dry attenuation are incorrect");
}
