#include "../src/component/ui_design_system.h"

#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

void runWorkpackShellTests()
{
    using namespace nekorox::component;

    struct Case {
        unsigned dpi;
        unsigned header;
        unsigned transport;
        unsigned waveSeek;
        unsigned caption;
    };
    constexpr Case cases[] = {
        {96U, 70U, 32U, 36U, 22U},
        {144U, 105U, 48U, 54U, 33U},
        {192U, 140U, 64U, 72U, 44U},
    };
    for (const auto& value : cases) {
        require(fixedBandHeightPixels(UiFixedBand::header, value.dpi) ==
                value.header,
            "header fixed-band DPI conversion drifted");
        require(fixedBandHeightPixels(UiFixedBand::transport, value.dpi) ==
                value.transport,
            "transport fixed-band DPI conversion drifted");
        require(fixedBandHeightPixels(UiFixedBand::waveSeek, value.dpi) ==
                value.waveSeek,
            "WaveSeek fixed-band DPI conversion drifted");
        require(fixedBandHeightPixels(UiFixedBand::trackCaption, value.dpi) ==
                value.caption,
            "caption fixed-band DPI conversion drifted");
    }
    require(fixedBandHeightPixels(UiFixedBand::transport, 0U) == 32U,
        "zero-DPI fallback is not 96 DPI");
    require(normalizedGlyphStrokeDip(24.0f) > 1.599f &&
            normalizedGlyphStrokeDip(24.0f) < 1.601f,
        "normalized 24-DIP glyph stroke is not 1.6 DIP");
    require(normalizedGlyphStrokeDip(12.0f) > 0.799f &&
            normalizedGlyphStrokeDip(12.0f) < 0.801f,
        "glyph stroke does not scale optically with its normalized geometry");
}
