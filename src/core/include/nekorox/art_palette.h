#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>

namespace nekorox {

struct RgbColor {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

struct ArtPalette {
    bool valid = false;
    RgbColor primary{0.0f, 0.847f, 0.816f};
    RgbColor secondary{0.66f, 0.91f, 0.90f};
    RgbColor highlight{0.84f, 0.94f, 0.95f};
    // Flow A3 representatives are stored in linear RGB and are deliberately
    // independent from the accessibility-adjusted chrome accents above.
    bool flowValid = false;
    bool flowMonochrome = false;
    RgbColor flowLow{0.16f, 0.16f, 0.16f};
    RgbColor flowMid{0.09f, 0.09f, 0.09f};
    RgbColor flowHigh{0.24f, 0.24f, 0.24f};
    RgbColor flowBase{0.006f, 0.006f, 0.006f};
    // Cover pigments for the visual collection: dominant, related secondary,
    // accent and dark base, in linear RGB. Independent of UI/Flow adjustments.
    bool materialValid = false;
    bool materialMonochrome = true;
    std::array<RgbColor,4> material{
        RgbColor{.16f,.16f,.16f},RgbColor{.09f,.09f,.09f},
        RgbColor{.24f,.24f,.24f},RgbColor{.006f,.006f,.006f}};
};

ArtPalette extractArtPalette(
    std::span<const std::uint8_t> premultipliedBgra,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride);

double contrastRatio(RgbColor foreground, RgbColor background) noexcept;
RgbColor interpolateOklch(
    RgbColor from,
    RgbColor to,
    float progress) noexcept;

} // namespace nekorox
