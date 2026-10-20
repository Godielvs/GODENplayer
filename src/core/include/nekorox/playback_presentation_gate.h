#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace nekorox {

// Presents playback cursors only when their caller-defined raster step changes.
// Repainting the same step cannot improve motion and wastes a full panel draw;
// semantic and input-driven invalidations bypass this gate at the panel level.
class PlaybackPresentationGate final {
public:
    static constexpr std::int64_t unpresentedStep =
        (std::numeric_limits<std::int64_t>::min)();

    void reset() noexcept
    {
        lastStep_ = unpresentedStep;
    }

    [[nodiscard]] bool shouldPresent(
        std::int64_t step,
        double nowSeconds,
        std::uint32_t refreshMilliHz) noexcept
    {
        (void)nowSeconds;
        (void)refreshMilliHz;
        return step != lastStep_;
    }

    void markPresented(
        std::int64_t step,
        double nowSeconds,
        std::uint32_t refreshMilliHz) noexcept
    {
        lastStep_ = step;
        (void)nowSeconds;
        (void)refreshMilliHz;
    }

private:
    std::int64_t lastStep_ = unpresentedStep;
};

} // namespace nekorox
