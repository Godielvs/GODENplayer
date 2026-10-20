#include "../src/component/ui_design_system.h"

#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void verifyContained(const nekorox::component::UiPixelRect& value,
    long width, long height)
{
    require(value.x >= 0 && value.y >= 0 && value.width >= 0 &&
        value.height >= 0, "shell layout produced negative geometry");
    require(value.x + value.width <= width &&
        value.y + value.height <= height,
        "shell layout escaped its client rectangle");
}

} // namespace

void runCorrectiveR2ShellIntegrationTests()
{
    using namespace nekorox::component;
    constexpr unsigned dpis[]{96U, 120U, 144U, 168U, 192U, 240U, 288U, 384U};
    for (const auto dpi : dpis) {
        for (long resize = 0; resize < 100; ++resize) {
            const auto width = 640L + resize * 13L;
            const auto height = 720L + (resize % 31L) * 19L;
            const auto normal = computePlayerShellLayout(
                width, height, dpi, false);
            const auto album = computePlayerShellLayout(
                width, height, dpi, true);
            for (const auto* rectangle : {&normal.navigation,
                    &normal.workspace, &normal.transport, &normal.waveSeek,
                    &normal.spectrum, &normal.caption, &normal.cover,
                    &album.navigation, &album.workspace, &album.transport,
                    &album.waveSeek, &album.spectrum, &album.caption,
                    &album.cover}) {
                verifyContained(*rectangle, width, height);
            }
            require(album.cover.width == 0,
                "Album mode retained an artwork rail");
            require(album.waveSeek.width == width &&
                    album.spectrum.width == width &&
                    album.caption.width == width,
                "Album footer did not reach the right edge");
            require(normal.cover.width + normal.waveSeek.width == width,
                "Library footer width is not conserved");
            require(album.transport.y ==
                    album.navigation.height + album.workspace.height,
                "fixed shell bands are not contiguous");
            require(album.waveSeek.y ==
                    album.transport.y + album.transport.height,
                "WaveSeek is not directly below transport");
            require(album.caption.y + album.caption.height == height,
                "caption does not terminate at the client edge");
            const auto theater=computePlayerShellLayout(width,height,dpi,true,152,true);
            for(const auto* rectangle:{&theater.workspace,&theater.transport,&theater.waveSeek,
                    &theater.spectrum,&theater.caption,&theater.cover})
                verifyContained(*rectangle,width,height);
            require(theater.spectrum.height==0&&theater.caption.height==0&&
                theater.waveSeek.y+theater.waveSeek.height==height,
                "Theater mode lost waveform/footer bounds");
            require(theater.workspace.height-album.workspace.height==
                album.spectrum.height+album.caption.height,
                "Theater mode did not reclaim the complete spectrum/caption");
            const auto libraryWithTheater=computePlayerShellLayout(width,height,dpi,false,152,true);
            require(libraryWithTheater.transport.y==normal.transport.y&&
                libraryWithTheater.spectrum.height==normal.spectrum.height&&
                libraryWithTheater.cover.width==normal.cover.width,
                "Album Theater preference leaked into Library");
        }

        const auto compactDeck = computePlayerShellLayout(
            1600L * static_cast<long>(dpi) / 96L,
            1000L * static_cast<long>(dpi) / 96L, dpi, false, 96);
        const auto expandedDeck = computePlayerShellLayout(
            1600L * static_cast<long>(dpi) / 96L,
            1000L * static_cast<long>(dpi) / 96L, dpi, false, 360);
        require(compactDeck.transport.height == expandedDeck.transport.height &&
                compactDeck.waveSeek.height == expandedDeck.waveSeek.height &&
                compactDeck.caption.height == expandedDeck.caption.height,
            "deck resize changed a fixed-height child band");
        require(expandedDeck.spectrum.height > compactDeck.spectrum.height &&
                expandedDeck.transport.y < compactDeck.transport.y,
            "deck resize did not move the whole lower group upward");
        require(expandedDeck.caption.y + expandedDeck.caption.height ==
                compactDeck.caption.y + compactDeck.caption.height,
            "deck resize detached the lower group from the client bottom");

        // Physical pixels, including work areas too small for the desired
        // minimum at high DPI. This is geometry coverage, not monitor testing.
        for (const auto dimensions : {std::array<long,2>{0,0}, {320,200},
                {640,480}, {1280,720}, {1080,1920}, {3440,1440}, {7680,2160}}) {
            const auto width=dimensions[0], height=dimensions[1];
            for (const auto albumMode : {false,true})
                for (const auto theaterMode : {false,true}) {
                    const auto layout=computePlayerShellLayout(width,height,dpi,albumMode,360,theaterMode);
                    for (const auto* rectangle : {&layout.navigation,&layout.workspace,
                            &layout.transport,&layout.waveSeek,&layout.spectrum,&layout.caption,&layout.cover})
                        verifyContained(*rectangle,width,height);
                    require(layout.caption.y+layout.caption.height==height,
                        "COMPAT small/high-DPI footer escaped client bottom");
                    require(layout.waveSeek.width+layout.cover.width==width,
                        "COMPAT small/high-DPI footer lost width");
                }
        }
    }
}
