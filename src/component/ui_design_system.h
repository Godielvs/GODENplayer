#pragma once

#include "ui_preferences.h"

#include <dwrite.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nekorox::component {

struct UiColor {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

struct OklchColor {
    float lightness = 0.0f;
    float chroma = 0.0f;
    float hueDegrees = 0.0f;
    float alpha = 1.0f;
};

enum class ColorInterpolation : std::uint8_t {
    linearLight = 0,
    oklch = 1,
};

enum class UiColorToken : std::uint8_t {
    canvas = 0,
    surface,
    surfaceRaised,
    surfaceSelected,
    separator,
    separatorStrong,
    text,
    textMuted,
    textBright,
    accentPrimary,
    accentSecondary,
    accentSubtle,
    focus,
    danger,
    count,
};

enum class UiFontRole : std::uint8_t {
    caption = 0,
    body,
    bodyStrong,
    title,
    artist,
    album,
    year,
    information,
    technical,
    time,
    count,
};

enum class UiGlyph : std::uint8_t {
    stop = 0,
    previous,
    play,
    pause,
    next,
    repeat,
    shuffle,
    keepPlaying,
    volumeMuted,
    volumeLow,
    volumeMedium,
    volumeHigh,
    windowMinimize,
    windowMaximize,
    windowRestore,
    windowClose,
    listen,
    nowPlaying,
    queue,
    favoriteOutline,
    favoriteFilled,
    collapse,
    expand,
    menu,
    menuCheck,
    menuSubmenu,
};

enum class MotionToken : std::uint8_t {
    hover = 0,
    press,
    selection,
    tab,
    drawer,
    artwork,
};

struct UiPalette {
    std::array<UiColor, static_cast<std::size_t>(UiColorToken::count)> colors{};

    [[nodiscard]] constexpr UiColor operator[](UiColorToken token) const noexcept
    {
        return colors[static_cast<std::size_t>(token)];
    }
};

struct UiMetrics {
    float separatorDip = 1.0f;
    float selectionBarDip = 2.0f;
    float rowDip = 20.0f;
    // Even the compact album card must keep cover art large enough to remain
    // identifiable.  Forty DIPs left only a 32-DIP image, which looked like a
    // list icon rather than an album cover on narrow-but-tall Library views.
    float albumHeaderCompactDip = 56.0f;
    float albumHeaderDip = 80.0f;
    float horizontalPaddingDip = 8.0f;
    float verticalPaddingDip = 4.0f;
    float compactGapDip = 4.0f;
    float standardGapDip = 8.0f;
    float glyphDip = 16.0f;
    float controlDip = 24.0f;
    float hoverTravelDip = 1.0f;
};

struct UiPixelRect {
    long x = 0;
    long y = 0;
    long width = 0;
    long height = 0;
};

struct PlayerShellLayout {
    UiPixelRect navigation;
    UiPixelRect workspace;
    UiPixelRect transport;
    UiPixelRect waveSeek;
    UiPixelRect spectrum;
    UiPixelRect caption;
    UiPixelRect cover;
};

enum class UiFixedBand : std::uint8_t {
    header = 0,
    transport,
    waveSeek,
    trackCaption,
};

[[nodiscard]] constexpr unsigned fixedBandHeightDip(
    UiFixedBand band) noexcept
{
    switch (band) {
    case UiFixedBand::header: return 70U;
    case UiFixedBand::transport: return 32U;
    case UiFixedBand::waveSeek: return 36U;
    case UiFixedBand::trackCaption: return 22U;
    }
    return 0U;
}

// Nearest-integer DIP conversion for positive band dimensions. Keeping this
// pure lets layout serialization and Win32 enforcement share one contract.
[[nodiscard]] constexpr unsigned fixedBandHeightPixels(
    UiFixedBand band,
    unsigned dpi) noexcept
{
    const auto safeDpi = dpi == 0U ? 96U : dpi;
    return (fixedBandHeightDip(band) * safeDpi + 48U) / 96U;
}

[[nodiscard]] inline PlayerShellLayout computePlayerShellLayout(
    long widthPixels,
    long heightPixels,
    unsigned dpi,
    bool albumMode,
    int spectrumHeightDip = 152,
    bool albumTheater = false) noexcept
{
    const auto width = std::max(0L, widthPixels);
    const auto height = std::max(0L, heightPixels);
    const auto safeDpi = dpi == 0U ? 96U : dpi;
    const auto px = [safeDpi](long dip) {
        return (dip * static_cast<long>(safeDpi) + 48L) / 96L;
    };
    // Windows can constrain a window below its requested minimum (small work
    // area, high DPI, display removal). Never place footer children beyond the
    // actual client. Normal-size layouts retain exactly the existing metrics.
    auto remaining = height;
    const auto fit = [&remaining](long wanted) {
        const auto allocated = std::min(remaining, wanted);
        remaining -= allocated;
        return allocated;
    };
    const auto nav = fit(px(36));
    const auto transportHeight = fit(px(32));
    const auto waveHeight = fit(px(36));
    const auto theater = albumMode && albumTheater;
    const auto captionHeight = fit(theater ? 0L : px(22));
    const auto clientHeightDip = height * 96.0f /
        static_cast<float>(safeDpi);
    const auto maximumSpectrumDip = std::max(96, std::min(360,
        static_cast<int>(clientHeightDip) - 286));
    const auto spectrumDip = std::clamp(
        spectrumHeightDip, 96, maximumSpectrumDip);
    const auto spectrumHeight = fit(theater ? 0L : px(spectrumDip));
    const auto bottomHeight = waveHeight + spectrumHeight + captionHeight;
    const auto contentHeight = std::max(
        0L, height - nav - transportHeight - bottomHeight);
    const auto transportTop = nav + contentHeight;
    const auto bottomTop = transportTop + transportHeight;
    const auto coverWidth = albumMode ? 0L : std::min(width, px(220));
    const auto visualWidth = std::max(0L, width - coverWidth);
    return {
        {0, 0, width, nav},
        {0, nav, width, contentHeight},
        {0, transportTop, width, transportHeight},
        {0, bottomTop, visualWidth, waveHeight},
        {0, bottomTop + waveHeight, visualWidth, spectrumHeight},
        {0, bottomTop + waveHeight + spectrumHeight,
            visualWidth, captionHeight},
        {visualWidth, bottomTop, coverWidth, bottomHeight},
    };
}

[[nodiscard]] constexpr float normalizedGlyphStrokeDip(
    float extentDip) noexcept
{
    return extentDip > 0.0f ? extentDip * (1.6f / 24.0f) : 0.0f;
}

struct UiFontDescriptor {
    std::array<std::wstring_view, 4> familyFallbacks{};
    std::size_t familyCount = 0;
    float sizeDip = 12.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
};

class UiDesignSystemV1 final {
public:
    static constexpr std::uint32_t version = 1;

    [[nodiscard]] static const UiPalette& nekoPalette() noexcept;
    [[nodiscard]] static const UiMetrics& metrics() noexcept;
    [[nodiscard]] static UiColor color(UiColorToken token) noexcept;
    [[nodiscard]] static const UiFontDescriptor& font(UiFontRole role) noexcept;
    [[nodiscard]] static std::wstring_view resolveFontFamily(
        IDWriteFactory* factory,
        const UiFontDescriptor& descriptor) noexcept;
    [[nodiscard]] static std::uint32_t durationMs(
        MotionToken token,
        MotionMode mode) noexcept;
};

[[nodiscard]] float interpolateScalar(
    float from,
    float to,
    float progress) noexcept;
[[nodiscard]] UiColor interpolateLinearColor(
    UiColor from,
    UiColor to,
    float progress) noexcept;
[[nodiscard]] UiColor interpolateOklchColor(
    UiColor from,
    UiColor to,
    float progress) noexcept;
[[nodiscard]] UiColor interpolateColor(
    UiColor from,
    UiColor to,
    float progress,
    ColorInterpolation interpolation) noexcept;
[[nodiscard]] OklchColor toOklch(UiColor color) noexcept;
[[nodiscard]] UiColor fromOklch(OklchColor color) noexcept;

} // namespace nekorox::component
