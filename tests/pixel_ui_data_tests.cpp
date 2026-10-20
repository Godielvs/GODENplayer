#include "pixel_ui/goden_pixel_assets.generated.hpp"
#include "pixel_ui/pixel_placement.hpp"
#include "pixel_ui/ui_glyph_map.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

void pixelRequire(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

} // namespace

void runPixelUiDataTests()
{
    using namespace goden::pixel;

    constexpr std::array uiGlyphs{
        nekorox::component::UiGlyph::stop,
        nekorox::component::UiGlyph::previous,
        nekorox::component::UiGlyph::play,
        nekorox::component::UiGlyph::pause,
        nekorox::component::UiGlyph::next,
        nekorox::component::UiGlyph::repeat,
        nekorox::component::UiGlyph::shuffle,
        nekorox::component::UiGlyph::keepPlaying,
        nekorox::component::UiGlyph::volumeMuted,
        nekorox::component::UiGlyph::volumeLow,
        nekorox::component::UiGlyph::volumeMedium,
        nekorox::component::UiGlyph::volumeHigh,
        nekorox::component::UiGlyph::windowMinimize,
        nekorox::component::UiGlyph::windowMaximize,
        nekorox::component::UiGlyph::windowRestore,
        nekorox::component::UiGlyph::windowClose,
        nekorox::component::UiGlyph::listen,
        nekorox::component::UiGlyph::nowPlaying,
        nekorox::component::UiGlyph::queue,
        nekorox::component::UiGlyph::favoriteOutline,
        nekorox::component::UiGlyph::favoriteFilled,
        nekorox::component::UiGlyph::collapse,
        nekorox::component::UiGlyph::expand,
        nekorox::component::UiGlyph::menu,
        nekorox::component::UiGlyph::menuCheck,
        nekorox::component::UiGlyph::menuSubmenu,
    };
    static_assert(uiGlyphs.size() == 26);
    for (const auto glyph : uiGlyphs) {
        pixelRequire(nekorox::component::pixelIconFor(glyph) != IconId::Count,
            "UiGlyph has no Pixel UI mapping");
    }

    pixelRequire(static_cast<std::size_t>(IconId::Count) == 73,
        "pixel icon inventory changed");
    pixelRequire(icon(IconId::Count) == nullptr,
        "invalid pixel icon enum was accepted");

    for (const auto& glyph : kViews) {
        pixelRequire(glyph.width > 0 && glyph.height > 0 && glyph.count > 0,
            "invalid pixel glyph view");
        std::vector<unsigned char> cells(glyph.width * glyph.height);
        for (std::uint16_t index = 0; index < glyph.count; ++index) {
            const auto& rectangle = glyph.rectangles[index];
            pixelRequire(rectangle.x + rectangle.width <= glyph.width &&
                    rectangle.y + rectangle.height <= glyph.height,
                "pixel rectangle escaped its master");
            pixelRequire(rectangle.role == 1 || rectangle.role == 2,
                "invalid pixel ink role");
            const auto rectangleBottom = static_cast<std::uint16_t>(
                rectangle.y + rectangle.height);
            const auto rectangleRight = static_cast<std::uint16_t>(
                rectangle.x + rectangle.width);
            for (std::uint16_t y = rectangle.y;
                    y < rectangleBottom; ++y) {
                for (std::uint16_t x = rectangle.x;
                        x < rectangleRight; ++x) {
                    const auto cell = y * glyph.width + x;
                    pixelRequire(cells[cell] == 0,
                        "overlapping pixel rectangles");
                    cells[cell] = rectangle.role;
                }
            }
        }

        for (double dpi : {96.0, 120.0, 144.0, 192.0, 240.0, 288.0}) {
            const auto placement = place(glyph.width, glyph.height,
                {7.25, -3.0, 7.25 + glyph.width, -3.0 + glyph.height},
                dpi, dpi);
            pixelRequire(placement.valid, "valid pixel placement rejected");
            pixelRequire(placement.width ==
                    static_cast<int>(std::lround(glyph.width * dpi / 96.0)),
                "incorrect DPI-scaled pixel width");
            pixelRequire(placement.height ==
                    static_cast<int>(std::lround(glyph.height * dpi / 96.0)),
                "incorrect DPI-scaled pixel height");
            pixelRequire(edge(0, glyph.width, placement.width) == 0 &&
                    edge(glyph.width, glyph.width, placement.width) ==
                        placement.width,
                "pixel edge endpoints drifted");
            for (std::uint16_t x = 0; x < glyph.width; ++x) {
                pixelRequire(edge(x + 1, glyph.width, placement.width) >
                        edge(x, glyph.width, placement.width),
                    "DPI transform lost a horizontal master cell");
            }
            for (std::uint16_t y = 0; y < glyph.height; ++y) {
                pixelRequire(edge(y + 1, glyph.height, placement.height) >
                        edge(y, glyph.height, placement.height),
                    "DPI transform lost a vertical master cell");
            }
        }
    }

    pixelRequire(!place(24, 24, {0, 0, 16, 16}, 96, 96).valid,
        "pixel downsample was silently accepted");
    pixelRequire(!place(24, 24, {0, 0, 24, 24}, 0, 96).valid,
        "zero pixel DPI was accepted");
    pixelRequire(!place(24, 24, {0, 0, 24, 24}, 96, 96,
            {1, 0.2, 0, 1, 0, 0}).valid,
        "sheared pixel placement was accepted");
    pixelRequire(!place(24, 24, {0, 0, 24, 24}, 96, 96,
            {-1, 0, 0, 1, 0, 0}).valid,
        "negative pixel scale was accepted");
    pixelRequire(findIcon("keep_playing", 16) != nullptr,
        "Keep Playing pixel glyph is missing");
    pixelRequire(findIcon("does-not-exist") == nullptr,
        "unknown pixel glyph did not fail closed");
    pixelRequire(icon(IconId::MenuCheck, 12)->width == 12,
        "micro pixel master is missing");
    pixelRequire(icon(IconId::KeepPlaying, 12)->width == 16,
        "pixel master fallback changed");
}
