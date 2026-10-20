#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nekorox {

inline constexpr std::uint32_t kGlyphFlowVersionV1 = 1;
inline constexpr double kGlyphFlowDurationMillisecondsV1 = 420.0;
inline constexpr std::uint32_t kGlyphFlowMaximumDimensionV1 = 64;
inline constexpr std::size_t kGlyphFlowMaximumCellCountV1 = 4096;
inline constexpr std::size_t kGlyphFlowNoOffendingIndexV1 =
    static_cast<std::size_t>(-1);

enum class GlyphFlowMotionModeV1 : std::uint8_t {
    Normal = 0,
    Reduced = 1,
    Off = 2,
};

enum class GlyphFlowErrorV1 : std::uint8_t {
    None = 0,
    UnsupportedVersion,
    InvalidDimensions,
    TooManyCells,
    FieldSizeMismatch,
    NonFiniteLuminance,
    LuminanceOutOfRange,
    InvalidGlyphCount,
    AllocationFailed,
    NotConfigured,
    InvalidMotionMode,
    NonFiniteElapsedTime,
    OutputBufferTooSmall,
};

struct GlyphFlowInputV1 {
    std::uint32_t version = kGlyphFlowVersionV1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const float> luminance;
    std::uint64_t artworkGeneration = 0;
    std::uint64_t stableSeed = 0;
    std::uint32_t glyphCount = 0;
};

struct GlyphFlowValidationV1 {
    std::uint32_t version = kGlyphFlowVersionV1;
    GlyphFlowErrorV1 error = GlyphFlowErrorV1::None;
    std::size_t offendingIndex = kGlyphFlowNoOffendingIndexV1;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return error == GlyphFlowErrorV1::None;
    }
};

struct GlyphFlowCellV1 {
    std::uint32_t column = 0;
    std::uint32_t row = 0;
    std::uint32_t glyphIndex = 0;
    float sourceLuminance = 0.0f;
    float opacity = 0.0f;

    // The renderer may scale this vector by at most one DIP in Normal mode.
    // Its Euclidean magnitude never exceeds 1.0.
    float offsetXNormalized = 0.0f;
    float offsetYNormalized = 0.0f;
};

struct GlyphFlowSampleV1 {
    std::uint32_t version = kGlyphFlowVersionV1;
    GlyphFlowErrorV1 error = GlyphFlowErrorV1::None;
    std::size_t requiredCellCount = 0;
    std::size_t writtenCellCount = 0;
    double elapsedMilliseconds = 0.0;
    float progress = 0.0f;
    bool complete = false;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return error == GlyphFlowErrorV1::None;
    }
};

[[nodiscard]] GlyphFlowValidationV1 validateGlyphFlowInputV1(
    const GlyphFlowInputV1& input) noexcept;

class GlyphFlowV1 final {
public:
    [[nodiscard]] GlyphFlowValidationV1 configure(
        const GlyphFlowInputV1& input) noexcept;

    void clear() noexcept;

    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] std::size_t cellCount() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::uint64_t artworkGeneration() const noexcept;
    [[nodiscard]] std::uint64_t stableSeed() const noexcept;

    // elapsedMilliseconds is supplied by the caller; this class owns no clock
    // or timer. output must contain at least cellCount() entries.
    [[nodiscard]] GlyphFlowSampleV1 sample(
        GlyphFlowMotionModeV1 mode,
        double elapsedMilliseconds,
        std::span<GlyphFlowCellV1> output) const noexcept;

private:
    struct CellPlanV1 {
        std::uint32_t column = 0;
        std::uint32_t row = 0;
        std::uint32_t glyphIndex = 0;
        std::uint16_t luminance = 0;
        std::uint16_t delay = 0;
        std::int16_t directionX = 0;
        std::int16_t directionY = 0;
        std::uint16_t amplitude = 0;
    };

    std::vector<CellPlanV1> cells_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t artworkGeneration_ = 0;
    std::uint64_t stableSeed_ = 0;
    bool configured_ = false;
};

using ArtworkTransitionV1 = GlyphFlowV1;

} // namespace nekorox
