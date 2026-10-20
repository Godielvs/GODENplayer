#include "ui_design_system.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nekorox::component {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kChromaEpsilon = 0.0001f;

constexpr UiColor rgb(
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue,
    std::uint8_t alpha = 255) noexcept
{
    constexpr auto scale = 1.0f / 255.0f;
    return {
        red * scale,
        green * scale,
        blue * scale,
        alpha * scale,
    };
}

constexpr UiPalette kNekoPalette{{
    rgb(0x08, 0x09, 0x09),
    rgb(0x0c, 0x0d, 0x0d),
    rgb(0x10, 0x11, 0x12),
    rgb(0x18, 0x1d, 0x1d),
    rgb(0x23, 0x29, 0x2a),
    rgb(0x35, 0x3e, 0x40),
    rgb(0xa8, 0xb6, 0xbc),
    rgb(0x76, 0x84, 0x86),
    rgb(0xe0, 0xe9, 0xea),
    rgb(0x00, 0xd8, 0xd0),
    rgb(0xa8, 0xe9, 0xe5),
    rgb(0x00, 0x80, 0x7c, 0x5c),
    rgb(0x67, 0xea, 0xdf),
    rgb(0xe0, 0x63, 0x8b),
}};

constexpr UiMetrics kMetrics{};

constexpr std::array<std::wstring_view, 4> kTextFamilies{
    L"Segoe UI Variable Text",
    L"Segoe UI",
    L"Arial",
    L"",
};
constexpr std::array<std::wstring_view, 4> kDisplayFamilies{
    L"Segoe UI Variable Display",
    L"Segoe UI Semibold",
    L"Segoe UI",
    L"Arial",
};
constexpr std::array<std::wstring_view, 4> kMonoFamilies{
    L"Cascadia Mono",
    L"Consolas",
    L"Courier New",
    L"",
};

constexpr UiFontDescriptor descriptor(
    std::array<std::wstring_view, 4> families,
    std::size_t familyCount,
    float sizeDip,
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) noexcept
{
    return {
        families,
        familyCount,
        sizeDip,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
    };
}

constexpr std::array<UiFontDescriptor,
    static_cast<std::size_t>(UiFontRole::count)> kFonts{{
    descriptor(kTextFamilies, 3, 10.0f),
    descriptor(kTextFamilies, 3, 12.0f),
    descriptor(kTextFamilies, 3, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
    descriptor(kDisplayFamilies, 4, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
    descriptor(kDisplayFamilies, 4, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
    descriptor(kTextFamilies, 3, 15.0f),
    descriptor(kDisplayFamilies, 4, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
    descriptor(kTextFamilies, 3, 11.0f),
    descriptor(kMonoFamilies, 3, 11.0f),
    descriptor(kMonoFamilies, 3, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
}};

constexpr std::array<std::uint32_t, 6> kNormalDurations{
    90,
    70,
    140,
    180,
    220,
    620,
};
constexpr std::array<std::uint32_t, 6> kReducedDurations{
    50,
    40,
    70,
    80,
    100,
    120,
};

float clampUnit(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

float srgbToLinear(float value) noexcept
{
    value = clampUnit(value);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float value) noexcept
{
    value = clampUnit(value);
    return value <= 0.0031308f
        ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float normalizedHue(float degrees) noexcept
{
    degrees = std::fmod(degrees, 360.0f);
    return degrees < 0.0f ? degrees + 360.0f : degrees;
}

} // namespace

const UiPalette& UiDesignSystemV1::nekoPalette() noexcept
{
    return kNekoPalette;
}

const UiMetrics& UiDesignSystemV1::metrics() noexcept
{
    return kMetrics;
}

UiColor UiDesignSystemV1::color(UiColorToken token) noexcept
{
    const auto index = static_cast<std::size_t>(token);
    return index < kNekoPalette.colors.size()
        ? kNekoPalette.colors[index]
        : kNekoPalette[UiColorToken::text];
}

const UiFontDescriptor& UiDesignSystemV1::font(UiFontRole role) noexcept
{
    const auto index = static_cast<std::size_t>(role);
    return index < kFonts.size()
        ? kFonts[index]
        : kFonts[static_cast<std::size_t>(UiFontRole::body)];
}

std::wstring_view UiDesignSystemV1::resolveFontFamily(
    IDWriteFactory* factory,
    const UiFontDescriptor& descriptor) noexcept
{
    if (descriptor.familyCount == 0) return {};
    const auto fallback = descriptor.familyFallbacks[0];
    if (factory == nullptr) return fallback;

    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collection)) ||
        collection == nullptr) {
        return fallback;
    }

    std::wstring_view resolved = fallback;
    const auto count = std::min(
        descriptor.familyCount,
        descriptor.familyFallbacks.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto family = descriptor.familyFallbacks[index];
        if (family.empty()) continue;
        UINT32 familyIndex = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(
                family.data(), &familyIndex, &exists)) &&
            exists) {
            resolved = family;
            break;
        }
    }
    collection->Release();
    return resolved;
}

std::uint32_t UiDesignSystemV1::durationMs(
    MotionToken token,
    MotionMode mode) noexcept
{
    const auto index = static_cast<std::size_t>(token);
    if (index >= kNormalDurations.size() || mode == MotionMode::off) {
        return 0;
    }
    return mode == MotionMode::reduced
        ? kReducedDurations[index]
        : kNormalDurations[index];
}

float interpolateScalar(float from, float to, float progress) noexcept
{
    return from + (to - from) * clampUnit(progress);
}

UiColor interpolateLinearColor(
    UiColor from,
    UiColor to,
    float progress) noexcept
{
    progress = clampUnit(progress);
    return {
        linearToSrgb(interpolateScalar(
            srgbToLinear(from.red), srgbToLinear(to.red), progress)),
        linearToSrgb(interpolateScalar(
            srgbToLinear(from.green), srgbToLinear(to.green), progress)),
        linearToSrgb(interpolateScalar(
            srgbToLinear(from.blue), srgbToLinear(to.blue), progress)),
        interpolateScalar(from.alpha, to.alpha, progress),
    };
}

OklchColor toOklch(UiColor color) noexcept
{
    const auto red = srgbToLinear(color.red);
    const auto green = srgbToLinear(color.green);
    const auto blue = srgbToLinear(color.blue);
    const auto l = std::cbrt(
        0.4122214708f * red + 0.5363325363f * green +
        0.0514459929f * blue);
    const auto m = std::cbrt(
        0.2119034982f * red + 0.6806995451f * green +
        0.1073969566f * blue);
    const auto s = std::cbrt(
        0.0883024619f * red + 0.2817188376f * green +
        0.6299787005f * blue);
    const auto lightness =
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    const auto a =
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    const auto b =
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    return {
        lightness,
        std::hypot(a, b),
        normalizedHue(std::atan2(b, a) * 180.0f / kPi),
        clampUnit(color.alpha),
    };
}

UiColor fromOklch(OklchColor color) noexcept
{
    const auto hue = normalizedHue(color.hueDegrees) * kPi / 180.0f;
    const auto a = std::cos(hue) * std::max(0.0f, color.chroma);
    const auto b = std::sin(hue) * std::max(0.0f, color.chroma);
    const auto l_ =
        color.lightness + 0.3963377774f * a + 0.2158037573f * b;
    const auto m_ =
        color.lightness - 0.1055613458f * a - 0.0638541728f * b;
    const auto s_ =
        color.lightness - 0.0894841775f * a - 1.2914855480f * b;
    const auto l = l_ * l_ * l_;
    const auto m = m_ * m_ * m_;
    const auto s = s_ * s_ * s_;
    return {
        linearToSrgb(
            4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
        linearToSrgb(
            -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
        linearToSrgb(
            -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s),
        clampUnit(color.alpha),
    };
}

UiColor interpolateOklchColor(
    UiColor from,
    UiColor to,
    float progress) noexcept
{
    progress = clampUnit(progress);
    auto left = toOklch(from);
    auto right = toOklch(to);
    if (left.chroma < kChromaEpsilon) left.hueDegrees = right.hueDegrees;
    if (right.chroma < kChromaEpsilon) right.hueDegrees = left.hueDegrees;

    auto hueDelta = normalizedHue(right.hueDegrees - left.hueDegrees);
    if (hueDelta > 180.0f) hueDelta -= 360.0f;
    return fromOklch({
        interpolateScalar(left.lightness, right.lightness, progress),
        interpolateScalar(left.chroma, right.chroma, progress),
        normalizedHue(left.hueDegrees + hueDelta * progress),
        interpolateScalar(left.alpha, right.alpha, progress),
    });
}

UiColor interpolateColor(
    UiColor from,
    UiColor to,
    float progress,
    ColorInterpolation interpolation) noexcept
{
    return interpolation == ColorInterpolation::oklch
        ? interpolateOklchColor(from, to, progress)
        : interpolateLinearColor(from, to, progress);
}

} // namespace nekorox::component
