#include "../src/component/album_booklet_model.h"

#include <chrono>
#include <cmath>
#include <stdexcept>

namespace {
void requireBooklet(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

void runWorkpackBookletTests()
{
    using namespace std::chrono_literals;
    using nekorox::component::AlbumBookletModel;
    const auto start = AlbumBookletModel::Clock::time_point{};
    AlbumBookletModel model;
    model.setOpen(true, start, 320ms);
    const auto halfway = model.sample(start + 160ms);
    requireBooklet(halfway > 0.45f && halfway < 0.55f,
        "WP15 open transition does not use smooth endpoints");
    model.setOpen(false, start + 160ms, 320ms);
    const auto reversed = model.sample(start + 240ms);
    requireBooklet(reversed < halfway && reversed > 0.0f,
        "WP15 reverse transition restarted from the wrong endpoint");
    model.setOpen(true, start + 240ms, 320ms);
    requireBooklet(model.sample(start + 241ms) >= reversed - 0.01f,
        "WP15 reopen jumped away from the presented rectangle");

    const auto wide = nekorox::component::computeAlbumBookletLayout(
        1440.0f, 820.0f, {true, true, true,
            goden::c11::ArtworkAlignment::center});
    requireBooklet(!wide.narrow && wide.albumDetails.right <= wide.artwork.left &&
        wide.writtenDetails.left >= wide.artwork.right,
        "WP15 wide booklet columns overlap the artwork");
    const auto narrow = nekorox::component::computeAlbumBookletLayout(
        640.0f, 700.0f, {true, true, true,
            goden::c11::ArtworkAlignment::center});
    const auto detailTop = narrow.albumDetails.right > narrow.albumDetails.left
        ? narrow.albumDetails.top : narrow.writtenDetails.top;
    requireBooklet(narrow.narrow && narrow.artwork.bottom <= detailTop,
        "WP15 narrow booklet did not put details below artwork");
}
