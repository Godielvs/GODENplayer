#include "nekorox/artwork_resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>

namespace nekorox {
namespace {

struct LinearPixel {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;
};

struct LinearImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<LinearPixel> pixels;
};

struct Weight {
    std::uint32_t index = 0;
    float value = 0.0f;
};

using WeightTable = std::vector<std::vector<Weight>>;

float srgbToLinear(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

LinearPixel addWeighted(
    LinearPixel output,
    const LinearPixel& input,
    float weight) noexcept
{
    output.red += input.red * weight;
    output.green += input.green * weight;
    output.blue += input.blue * weight;
    output.alpha += input.alpha * weight;
    return output;
}

LinearPixel blend(
    const LinearPixel& left,
    const LinearPixel& right,
    float rightAmount) noexcept
{
    const auto leftAmount = 1.0f - rightAmount;
    return {
        left.red * leftAmount + right.red * rightAmount,
        left.green * leftAmount + right.green * rightAmount,
        left.blue * leftAmount + right.blue * rightAmount,
        left.alpha * leftAmount + right.alpha * rightAmount,
    };
}

LinearImage decodeLinearPremultiplied(
    std::span<const std::uint8_t> source,
    std::uint32_t width,
    std::uint32_t height,const std::function<void()>& check)
{
    static const auto srgbByteToLinear = [] {
        std::array<float, 256> table{};
        for (std::size_t index = 0; index < table.size(); ++index) {
            table[index] = srgbToLinear(
                static_cast<float>(index) / 255.0f);
        }
        return table;
    }();
    LinearImage result;
    result.width = width;
    result.height = height;
    result.pixels.resize(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    constexpr float byteScale = 1.0f / 255.0f;
    for (std::size_t index = 0; index < result.pixels.size(); ++index) {
        if(check&&(index%4096U)==0)check();
        const auto at = index * 4U;
        const auto alphaByte = source[at + 3U];
        const auto alpha = static_cast<float>(alphaByte) * byteScale;
        if (alpha <= 0.0f) continue;
        const auto straight = [&, alpha](std::size_t channel) {
            return std::clamp(
                static_cast<float>(source[at + channel]) * byteScale / alpha,
                0.0f,
                1.0f);
        };
        auto& pixel = result.pixels[index];
        if (alphaByte == 255U) {
            pixel.blue = srgbByteToLinear[source[at]];
            pixel.green = srgbByteToLinear[source[at + 1U]];
            pixel.red = srgbByteToLinear[source[at + 2U]];
        } else {
            pixel.blue = srgbToLinear(straight(0U)) * alpha;
            pixel.green = srgbToLinear(straight(1U)) * alpha;
            pixel.red = srgbToLinear(straight(2U)) * alpha;
        }
        pixel.alpha = alpha;
    }
    return result;
}

WeightTable areaWeights(std::uint32_t source, std::uint32_t target)
{
    WeightTable table(target);
    const auto scale = static_cast<double>(source) /
        static_cast<double>(std::max<std::uint32_t>(1, target));
    for (std::uint32_t output = 0; output < target; ++output) {
        const auto start = static_cast<double>(output) * scale;
        const auto end = static_cast<double>(output + 1U) * scale;
        const auto first = static_cast<std::int64_t>(std::floor(start));
        const auto last = static_cast<std::int64_t>(std::ceil(end)) - 1;
        auto& weights = table[output];
        weights.reserve(static_cast<std::size_t>(std::max<std::int64_t>(
            1, last - first + 1)));
        for (auto input = first; input <= last; ++input) {
            const auto overlap = std::max(
                0.0,
                std::min(end, static_cast<double>(input + 1)) -
                    std::max(start, static_cast<double>(input)));
            if (overlap <= 0.0) continue;
            weights.push_back({
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                    input, 0, static_cast<std::int64_t>(source) - 1)),
                static_cast<float>(overlap / std::max(scale, 1e-12)),
            });
        }
    }
    return table;
}

float sinc(double value) noexcept
{
    if (std::abs(value) < 1e-8) return 1.0f;
    const auto angle = std::numbers::pi * value;
    return static_cast<float>(std::sin(angle) / angle);
}

WeightTable lanczos2Weights(std::uint32_t source, std::uint32_t target)
{
    WeightTable table(target);
    const auto scale = static_cast<double>(source) /
        static_cast<double>(std::max<std::uint32_t>(1, target));
    const auto filterScale = std::max(1.0, scale);
    const auto radius = 2.0 * filterScale;
    for (std::uint32_t output = 0; output < target; ++output) {
        const auto center = (static_cast<double>(output) + 0.5) * scale - 0.5;
        const auto first = static_cast<std::int64_t>(
            std::ceil(center - radius));
        const auto last = static_cast<std::int64_t>(
            std::floor(center + radius));
        auto& weights = table[output];
        weights.reserve(static_cast<std::size_t>(std::max<std::int64_t>(
            1, last - first + 1)));
        double total = 0.0;
        for (auto input = first; input <= last; ++input) {
            const auto distance = (center - static_cast<double>(input)) /
                filterScale;
            if (std::abs(distance) >= 2.0) continue;
            const auto weight = static_cast<double>(
                sinc(distance) * sinc(distance / 2.0));
            if (std::abs(weight) < 1e-8) continue;
            weights.push_back({
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                    input, 0, static_cast<std::int64_t>(source) - 1)),
                static_cast<float>(weight),
            });
            total += weight;
        }
        if (std::abs(total) <= 1e-12) {
            weights = {{
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::llround(center)),
                    0,
                    static_cast<std::int64_t>(source) - 1)),
                1.0f,
            }};
            continue;
        }
        for (auto& weight : weights) {
            weight.value = static_cast<float>(
                static_cast<double>(weight.value) / total);
        }
    }
    return table;
}

LinearImage resampleWithWeights(
    const LinearImage& source,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    const WeightTable& horizontal,
    const WeightTable& vertical,const std::function<void()>& check)
{
    LinearImage intermediate;
    intermediate.width = targetWidth;
    intermediate.height = source.height;
    intermediate.pixels.resize(
        static_cast<std::size_t>(targetWidth) * source.height);
    for (std::uint32_t y = 0; y < source.height; ++y) {
        if(check&&(y%16U)==0)check();
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            LinearPixel output;
            for (const auto& weight : horizontal[x]) {
                output = addWeighted(
                    output,
                    source.pixels[static_cast<std::size_t>(y) * source.width +
                        weight.index],
                    weight.value);
            }
            intermediate.pixels[
                static_cast<std::size_t>(y) * targetWidth + x] = output;
        }
    }

    LinearImage result;
    result.width = targetWidth;
    result.height = targetHeight;
    result.pixels.resize(
        static_cast<std::size_t>(targetWidth) * targetHeight);
    for (std::uint32_t y = 0; y < targetHeight; ++y) {
        if(check&&(y%16U)==0)check();
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            LinearPixel output;
            for (const auto& weight : vertical[y]) {
                output = addWeighted(
                    output,
                    intermediate.pixels[
                        static_cast<std::size_t>(weight.index) * targetWidth + x],
                    weight.value);
            }
            result.pixels[static_cast<std::size_t>(y) * targetWidth + x] =
                output;
        }
    }
    return result;
}

LinearImage areaResample(
    const LinearImage& source,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,const std::function<void()>& check)
{
    if (source.width == targetWidth && source.height == targetHeight) {
        return source;
    }
    return resampleWithWeights(
        source,
        targetWidth,
        targetHeight,
        areaWeights(source.width, targetWidth),
        areaWeights(source.height, targetHeight),check);
}

LinearImage lanczos2Resample(
    const LinearImage& source,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,const std::function<void()>& check)
{
    if (source.width == targetWidth && source.height == targetHeight) {
        return source;
    }
    return resampleWithWeights(
        source,
        targetWidth,
        targetHeight,
        lanczos2Weights(source.width, targetWidth),
        lanczos2Weights(source.height, targetHeight),check);
}

std::uint8_t byteFromUnit(float value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(
        std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f),
        0L,
        255L));
}

ArtworkBgraImageV1 encodeSrgbPbgra(const LinearImage& source,const std::function<void()>& check)
{
    ArtworkBgraImageV1 result;
    result.width = source.width;
    result.height = source.height;
    result.bgra.resize(source.pixels.size() * 4U);
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        if(check&&(index%4096U)==0)check();
        const auto& input = source.pixels[index];
        const auto alpha = std::clamp(input.alpha, 0.0f, 1.0f);
        const auto channel = [alpha](float premultiplied) {
            if (alpha <= 1e-7f) return 0.0f;
            const auto straight = std::clamp(
                premultiplied / alpha, 0.0f, 1.0f);
            return linearToSrgb(straight) * alpha;
        };
        const auto at = index * 4U;
        result.bgra[at] = byteFromUnit(channel(input.blue));
        result.bgra[at + 1U] = byteFromUnit(channel(input.green));
        result.bgra[at + 2U] = byteFromUnit(channel(input.red));
        result.bgra[at + 3U] = byteFromUnit(alpha);
    }
    return result;
}

bool validDimensions(
    std::span<const std::uint8_t> source,
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    if (width == 0 || height == 0) return false;
    const auto pixelCount = static_cast<std::uint64_t>(width) * height;
    return pixelCount <= std::numeric_limits<std::size_t>::max() / 4U &&
        source.size() >= static_cast<std::size_t>(pixelCount * 4U);
}

} // namespace

std::uint32_t quantizeArtworkPhysicalEdgeV1(
    std::uint32_t requestedEdge) noexcept
{
    requestedEdge = std::max<std::uint32_t>(1, requestedEdge);
    const auto step = requestedEdge <= 96U
        ? 4U
        : requestedEdge <= 256U
            ? 8U
            : 16U;
    const auto rounded = (static_cast<std::uint64_t>(requestedEdge) +
        step - 1U) / step * step;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        rounded,
        std::numeric_limits<std::uint32_t>::max()));
}

ArtworkBgraImageV1 resampleArtworkPbgraV1(
    std::span<const std::uint8_t> source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,const std::function<void()>& check)
{
    if (!validDimensions(source, sourceWidth, sourceHeight) ||
        targetWidth == 0 || targetHeight == 0) {
        return {};
    }
    targetWidth = std::min(targetWidth, sourceWidth);
    targetHeight = std::min(targetHeight, sourceHeight);
    if(check)check();
    auto current = decodeLinearPremultiplied(
        source, sourceWidth, sourceHeight,check);

    // Tiny headers benefit from an exact area prefilter: it suppresses
    // halftone shimmer and moire before the final reconstruction. At cover
    // sizes, however, that extra low-pass pass visibly softens line art and
    // typography. Lanczos already widens its footprint for minification, so
    // medium/large variants are reconstructed directly from the master.
    constexpr std::uint32_t kTinyArtworkEdge = 96U;
    const auto tinyTarget = std::max(targetWidth, targetHeight) <=
        kTinyArtworkEdge;
    if (tinyTarget &&
        (static_cast<std::uint64_t>(current.width) >
                static_cast<std::uint64_t>(targetWidth) * 2U ||
            static_cast<std::uint64_t>(current.height) >
                static_cast<std::uint64_t>(targetHeight) * 2U)) {
        const auto nextWidth = std::min(
            current.width,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(targetWidth) * 2U,
                std::numeric_limits<std::uint32_t>::max())));
        const auto nextHeight = std::min(
            current.height,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(targetHeight) * 2U,
                std::numeric_limits<std::uint32_t>::max())));
        current = areaResample(current, nextWidth, nextHeight,check);
    }

    const auto area = areaResample(current, targetWidth, targetHeight,check);
    auto detailed = lanczos2Resample(current, targetWidth, targetHeight,check);
    constexpr float kAreaBlend = 0.14f;
    for (std::size_t index = 0; index < detailed.pixels.size(); ++index) {
        if(check&&(index%4096U)==0)check();
        detailed.pixels[index] = blend(
            detailed.pixels[index], area.pixels[index], kAreaBlend);
        auto& pixel = detailed.pixels[index];
        pixel.alpha = std::clamp(pixel.alpha, 0.0f, 1.0f);
        pixel.red = std::clamp(pixel.red, 0.0f, pixel.alpha);
        pixel.green = std::clamp(pixel.green, 0.0f, pixel.alpha);
        pixel.blue = std::clamp(pixel.blue, 0.0f, pixel.alpha);
    }
    return encodeSrgbPbgra(detailed,check);
}

} // namespace nekorox
