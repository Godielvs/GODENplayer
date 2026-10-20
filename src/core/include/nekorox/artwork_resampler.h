#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include <functional>

namespace nekorox {

// Immutable BGRA8 premultiplied raster returned by the artwork variant
// pipeline. The transfer function is sRGB; filtering itself is performed in
// linear light with linear-premultiplied alpha.
struct ArtworkBgraImageV1 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra;
};

// Coalesces continuously resized destinations into close physical-pixel
// variants. Residual presentation scaling remains small while resize gestures
// cannot enqueue one background resample for every mouse pixel.
[[nodiscard]] std::uint32_t quantizeArtworkPhysicalEdgeV1(
    std::uint32_t requestedEdge) noexcept;

// Downscales WIC-style 32bppPBGRA. Large reductions are prefiltered in stages;
// the final pass blends Lanczos2 with an exact area result to retain line-art
// detail without exposing strong negative-lobe ringing.
[[nodiscard]] ArtworkBgraImageV1 resampleArtworkPbgraV1(
    std::span<const std::uint8_t> source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    const std::function<void()>& cancellationCheck = {});

} // namespace nekorox
