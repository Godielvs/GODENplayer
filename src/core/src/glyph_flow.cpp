#include "nekorox/glyph_flow.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace nekorox {
namespace {

constexpr std::uint32_t kMaximumGlyphCount = 65536;
constexpr double kMaximumStaggerMilliseconds = 126.0;
constexpr double kUnsigned16Scale = 65535.0;
constexpr double kSigned15Scale = 32767.0;

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t cellHash(
    std::uint64_t stableSeed,
    std::uint64_t artworkGeneration,
    std::size_t index,
    std::uint16_t luminance) noexcept
{
    auto value = stableSeed ^ 0xd6e8feb86659fd93ULL;
    value ^= splitMix64(artworkGeneration ^ 0xa0761d6478bd642fULL);
    value ^= splitMix64(
        static_cast<std::uint64_t>(index) * 0xe7037ed1a0b428dbULL);
    value ^= static_cast<std::uint64_t>(luminance) << 17U;
    return splitMix64(value);
}

std::uint16_t quantizeUnit(float value) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<double>(value) * kUnsigned16Scale + 0.5);
}

std::uint32_t chooseGlyph(
    std::uint16_t luminance,
    std::uint64_t hash,
    std::uint32_t glyphCount) noexcept
{
    const auto scaled = static_cast<std::uint64_t>(luminance) * glyphCount;
    const auto base = std::min<std::uint32_t>(
        glyphCount - 1U, static_cast<std::uint32_t>(scaled >> 16U));
    const auto spread = std::max<std::uint32_t>(1U, glyphCount / 12U);
    const auto range = spread * 2U + 1U;
    const auto jitter = static_cast<std::int64_t>(hash % range) -
        static_cast<std::int64_t>(spread);
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(base) + jitter,
        0,
        static_cast<std::int64_t>(glyphCount - 1U)));
}

std::int16_t quantizeDirection(std::int32_t value, std::int32_t denominator)
    noexcept
{
    if (denominator <= 0) return 0;
    const auto numerator =
        static_cast<std::int64_t>(value) * static_cast<std::int64_t>(32767);
    const auto rounded = numerator >= 0
        ? (numerator + denominator / 2) / denominator
        : (numerator - denominator / 2) / denominator;
    return static_cast<std::int16_t>(
        std::clamp<std::int64_t>(rounded, -32767, 32767));
}

float smootherStep(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value *
        (value * (value * 6.0f - 15.0f) + 10.0f);
}

bool validMotionMode(GlyphFlowMotionModeV1 mode) noexcept
{
    switch (mode) {
    case GlyphFlowMotionModeV1::Normal:
    case GlyphFlowMotionModeV1::Reduced:
    case GlyphFlowMotionModeV1::Off:
        return true;
    }
    return false;
}

} // namespace

GlyphFlowValidationV1 validateGlyphFlowInputV1(
    const GlyphFlowInputV1& input) noexcept
{
    GlyphFlowValidationV1 result;
    if (input.version != kGlyphFlowVersionV1) {
        result.error = GlyphFlowErrorV1::UnsupportedVersion;
        return result;
    }
    if (input.width == 0 || input.height == 0 ||
        input.width > kGlyphFlowMaximumDimensionV1 ||
        input.height > kGlyphFlowMaximumDimensionV1) {
        result.error = GlyphFlowErrorV1::InvalidDimensions;
        return result;
    }

    const auto cellCount =
        static_cast<std::size_t>(input.width) * input.height;
    if (cellCount > kGlyphFlowMaximumCellCountV1) {
        result.error = GlyphFlowErrorV1::TooManyCells;
        return result;
    }
    if (input.luminance.size() != cellCount) {
        result.error = GlyphFlowErrorV1::FieldSizeMismatch;
        return result;
    }
    if (input.glyphCount == 0 || input.glyphCount > kMaximumGlyphCount) {
        result.error = GlyphFlowErrorV1::InvalidGlyphCount;
        return result;
    }

    for (std::size_t index = 0; index < input.luminance.size(); ++index) {
        const auto value = input.luminance[index];
        if (!std::isfinite(value)) {
            result.error = GlyphFlowErrorV1::NonFiniteLuminance;
            result.offendingIndex = index;
            return result;
        }
        if (value < 0.0f || value > 1.0f) {
            result.error = GlyphFlowErrorV1::LuminanceOutOfRange;
            result.offendingIndex = index;
            return result;
        }
    }
    return result;
}

GlyphFlowValidationV1 GlyphFlowV1::configure(
    const GlyphFlowInputV1& input) noexcept
{
    auto validation = validateGlyphFlowInputV1(input);
    if (!validation.ok()) return validation;

    try {
        std::vector<std::uint16_t> luminance(input.luminance.size());
        for (std::size_t index = 0; index < input.luminance.size(); ++index) {
            luminance[index] = quantizeUnit(input.luminance[index]);
        }

        std::size_t brightestIndex = 0;
        for (std::size_t index = 1; index < luminance.size(); ++index) {
            if (luminance[index] > luminance[brightestIndex]) {
                brightestIndex = index;
            }
        }
        const auto brightestColumn =
            static_cast<std::uint32_t>(brightestIndex % input.width);
        const auto brightestRow =
            static_cast<std::uint32_t>(brightestIndex / input.width);
        const auto maximumDistance =
            std::max<std::uint32_t>(1U, input.width + input.height - 2U);

        std::vector<CellPlanV1> plans;
        plans.reserve(luminance.size());
        for (std::uint32_t row = 0; row < input.height; ++row) {
            for (std::uint32_t column = 0; column < input.width; ++column) {
                const auto index =
                    static_cast<std::size_t>(row) * input.width + column;
                const auto value = luminance[index];
                const auto hash = cellHash(
                    input.stableSeed,
                    input.artworkGeneration,
                    index,
                    value);

                const auto left = luminance[
                    static_cast<std::size_t>(row) * input.width +
                    (column == 0 ? column : column - 1U)];
                const auto right = luminance[
                    static_cast<std::size_t>(row) * input.width +
                    std::min(column + 1U, input.width - 1U)];
                const auto above = luminance[
                    static_cast<std::size_t>(row == 0 ? row : row - 1U) *
                    input.width + column];
                const auto below = luminance[
                    static_cast<std::size_t>(
                        std::min(row + 1U, input.height - 1U)) *
                    input.width + column];

                const auto jitterX =
                    static_cast<std::int32_t>((hash >> 16U) & 0x3fffULL) -
                    8192;
                const auto jitterY =
                    static_cast<std::int32_t>((hash >> 34U) & 0x3fffULL) -
                    8192;
                auto directionX =
                    static_cast<std::int32_t>(right) -
                    static_cast<std::int32_t>(left) + jitterX;
                auto directionY =
                    static_cast<std::int32_t>(below) -
                    static_cast<std::int32_t>(above) + jitterY;
                if (directionX == 0 && directionY == 0) {
                    static constexpr std::int32_t directions[8][2] = {
                        {1, 0}, {1, 1}, {0, 1}, {-1, 1},
                        {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
                    };
                    const auto directionIndex =
                        static_cast<std::size_t>((hash >> 48U) & 7ULL);
                    directionX = directions[directionIndex][0];
                    directionY = directions[directionIndex][1];
                }
                const auto directionDenominator =
                    std::abs(directionX) + std::abs(directionY);

                const auto columnDistance =
                    column > brightestColumn
                    ? column - brightestColumn
                    : brightestColumn - column;
                const auto rowDistance =
                    row > brightestRow ? row - brightestRow : brightestRow - row;
                const auto distance = columnDistance + rowDistance;
                const auto distanceUnit = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(distance) * 65535ULL) /
                    maximumDistance);
                const auto inverseLuminance =
                    65535U - static_cast<std::uint32_t>(value);
                const auto randomUnit =
                    static_cast<std::uint32_t>((hash >> 48U) & 0xffffULL);
                const auto delayUnit = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(distanceUnit) * 36ULL +
                        static_cast<std::uint64_t>(inverseLuminance) * 34ULL +
                        static_cast<std::uint64_t>(randomUnit) * 30ULL) /
                    100ULL);
                const auto amplitude = static_cast<std::uint16_t>(
                    24576U + static_cast<std::uint32_t>(
                        (hash >> 8U) % (65536U - 24576U)));

                plans.push_back({
                    column,
                    row,
                    chooseGlyph(value, hash, input.glyphCount),
                    value,
                    static_cast<std::uint16_t>(delayUnit),
                    quantizeDirection(directionX, directionDenominator),
                    quantizeDirection(directionY, directionDenominator),
                    amplitude,
                });
            }
        }

        cells_.swap(plans);
        width_ = input.width;
        height_ = input.height;
        artworkGeneration_ = input.artworkGeneration;
        stableSeed_ = input.stableSeed;
        configured_ = true;
    } catch (const std::bad_alloc&) {
        validation.error = GlyphFlowErrorV1::AllocationFailed;
    } catch (...) {
        validation.error = GlyphFlowErrorV1::AllocationFailed;
    }
    return validation;
}

void GlyphFlowV1::clear() noexcept
{
    cells_.clear();
    width_ = 0;
    height_ = 0;
    artworkGeneration_ = 0;
    stableSeed_ = 0;
    configured_ = false;
}

bool GlyphFlowV1::configured() const noexcept
{
    return configured_;
}

std::size_t GlyphFlowV1::cellCount() const noexcept
{
    return cells_.size();
}

std::uint32_t GlyphFlowV1::width() const noexcept
{
    return width_;
}

std::uint32_t GlyphFlowV1::height() const noexcept
{
    return height_;
}

std::uint64_t GlyphFlowV1::artworkGeneration() const noexcept
{
    return artworkGeneration_;
}

std::uint64_t GlyphFlowV1::stableSeed() const noexcept
{
    return stableSeed_;
}

GlyphFlowSampleV1 GlyphFlowV1::sample(
    GlyphFlowMotionModeV1 mode,
    double elapsedMilliseconds,
    std::span<GlyphFlowCellV1> output) const noexcept
{
    GlyphFlowSampleV1 result;
    result.requiredCellCount = cells_.size();
    if (!configured_) {
        result.error = GlyphFlowErrorV1::NotConfigured;
        return result;
    }
    if (!validMotionMode(mode)) {
        result.error = GlyphFlowErrorV1::InvalidMotionMode;
        return result;
    }
    if (!std::isfinite(elapsedMilliseconds)) {
        result.error = GlyphFlowErrorV1::NonFiniteElapsedTime;
        return result;
    }
    if (output.size() < cells_.size()) {
        result.error = GlyphFlowErrorV1::OutputBufferTooSmall;
        return result;
    }

    const auto off = mode == GlyphFlowMotionModeV1::Off;
    result.elapsedMilliseconds = off
        ? kGlyphFlowDurationMillisecondsV1
        : std::clamp(
            elapsedMilliseconds, 0.0, kGlyphFlowDurationMillisecondsV1);
    result.progress = off
        ? 1.0f
        : static_cast<float>(
            result.elapsedMilliseconds / kGlyphFlowDurationMillisecondsV1);
    result.complete =
        off || result.elapsedMilliseconds >= kGlyphFlowDurationMillisecondsV1;

    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const auto& plan = cells_[index];
        const auto delayMilliseconds =
            (static_cast<double>(plan.delay) / kUnsigned16Scale) *
            kMaximumStaggerMilliseconds;
        const auto localDuration = std::max(
            1.0, kGlyphFlowDurationMillisecondsV1 - delayMilliseconds);
        const auto localProgress = off
            ? 1.0f
            : static_cast<float>(std::clamp(
                (result.elapsedMilliseconds - delayMilliseconds) /
                    localDuration,
                0.0,
                1.0));
        const auto eased = smootherStep(localProgress);

        auto& cell = output[index];
        cell.column = plan.column;
        cell.row = plan.row;
        cell.glyphIndex = plan.glyphIndex;
        cell.sourceLuminance =
            static_cast<float>(plan.luminance / kUnsigned16Scale);
        cell.opacity = eased;
        cell.offsetXNormalized = 0.0f;
        cell.offsetYNormalized = 0.0f;
        if (mode == GlyphFlowMotionModeV1::Normal && !result.complete) {
            const auto amplitude =
                static_cast<float>(plan.amplitude / kUnsigned16Scale);
            const auto remaining = 1.0f - eased;
            cell.offsetXNormalized =
                static_cast<float>(plan.directionX / kSigned15Scale) *
                amplitude * remaining;
            cell.offsetYNormalized =
                static_cast<float>(plan.directionY / kSigned15Scale) *
                amplitude * remaining;
        }
    }
    result.writtenCellCount = cells_.size();
    return result;
}

} // namespace nekorox
